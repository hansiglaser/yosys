/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"
#include <string.h>
#include <dirent.h>
#include <readline/readline.h>

using RTLIL::id2cstr;

#undef CLUSTER_CELLS_AND_PORTBOXES

struct ShowWorker
{
	CellTypes ct;

	std::vector<std::string> dot_escape_store;
	std::map<RTLIL::IdString, int> dot_id2num_store;
	std::map<RTLIL::IdString, int> autonames;
	int single_idx_count;

	struct net_conn { std::set<std::string> in, out; int bits; std::string color; };
	std::map<std::string, net_conn> net_conn_map;

	FILE *f;
	RTLIL::Design *design;
	RTLIL::Module *module;
	uint32_t currentColor;
	bool genWidthLabels;
	bool stretchIO;
	bool enumerateIds;
	bool abbreviateIds;
	bool notitle;
	int page_counter;

	const std::vector<std::pair<std::string, RTLIL::Selection>> &color_selections;
	const std::vector<std::pair<std::string, RTLIL::Selection>> &label_selections;

	uint32_t xorshift32(uint32_t x) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		return x;
	}

	std::string nextColor()
	{
		if (currentColor == 0)
			return "color=\"black\"";
		return stringf("colorscheme=\"dark28\", color=\"%d\", fontcolor=\"%d\"", currentColor%8+1);
	}

	std::string nextColor(std::string presetColor)
	{
		if (presetColor.empty())
			return nextColor();
		return presetColor;
	}

	std::string nextColor(RTLIL::SigSpec sig, std::string defaultColor)
	{
		sig.sort_and_unify();
		for (auto &c : sig.chunks) {
			if (c.wire != NULL)
				for (auto &s : color_selections)
					if (s.second.selected_members.count(module->name) > 0 && s.second.selected_members.at(module->name).count(c.wire->name) > 0)
						return stringf("color=\"%s\"", s.first.c_str());
		}
		return defaultColor;
	}

	std::string nextColor(RTLIL::SigSig &conn, std::string defaultColor)
	{
		return nextColor(conn.first, nextColor(conn.second, defaultColor));
	}

	std::string nextColor(RTLIL::SigSpec &sig)
	{
		return nextColor(sig, nextColor());
	}

	std::string nextColor(RTLIL::SigSig &conn)
	{
		return nextColor(conn, nextColor());
	}

	std::string widthLabel(int bits)
	{
		if (bits <= 1)
			return "label=\"\"";
		if (!genWidthLabels)
			return "style=\"setlinewidth(3)\", label=\"\"";
		return stringf("style=\"setlinewidth(3)\", label=\"<%d>\"", bits);
	}

	const char *findColor(std::string member_name)
	{
		for (auto &s : color_selections)
			if (s.second.selected_member(module->name, member_name)) {
				dot_escape_store.push_back(stringf(", color=\"%s\"", s.first.c_str()));
				return dot_escape_store.back().c_str();
			}
		return "";
	}

	const char *findLabel(std::string member_name)
	{
		for (auto &s : label_selections)
			if (s.second.selected_member(module->name, RTLIL::escape_id(member_name)))
				return escape(s.first);
		return escape(member_name, true);
	}

	const char *escape(std::string id, bool is_name = false)
	{
		if (id.size() == 0)
			return "";

		if (id[0] == '$' && is_name) {
			if (enumerateIds) {
				if (autonames.count(id) == 0) {
					autonames[id] = autonames.size() + 1;
					log("Generated short name for internal identifier: _%d_ -> %s\n", autonames[id], id.c_str());
				}
				id = stringf("_%d_", autonames[id]);
			} else if (abbreviateIds) {
				const char *p = id.c_str();
				const char *q = strrchr(p, '$');
				id = std::string(q);
			}
		}

		if (id[0] == '\\')
			id = id.substr(1);

		std::string str;
		for (char ch : id) {
			if (ch == '\\' || ch == '"')
				str += "\\";
			str += ch;
		}

		dot_escape_store.push_back(str);
		return dot_escape_store.back().c_str();
	}

	int id2num(RTLIL::IdString id)
	{
		if (dot_id2num_store.count(id) > 0)
			return dot_id2num_store[id];
		return dot_id2num_store[id] = dot_id2num_store.size() + 1;
	}

	std::string gen_signode_simple(RTLIL::SigSpec sig, bool range_check = true)
	{
		sig.optimize();

		if (sig.chunks.size() == 0) {
			fprintf(f, "v%d [ label=\"\" ];\n", single_idx_count);
			return stringf("v%d", single_idx_count++);
		}

		if (sig.chunks.size() == 1) {
			RTLIL::SigChunk &c = sig.chunks[0];
			if (c.wire != NULL && design->selected_member(module->name, c.wire->name)) {
				if (!range_check || c.wire->width == c.width)
						return stringf("n%d", id2num(c.wire->name));
			} else {
				fprintf(f, "v%d [ label=\"%s\" ];\n", single_idx_count, findLabel(log_signal(c)));
				return stringf("v%d", single_idx_count++);
			}
		}

		return std::string();
	}

	std::string gen_portbox(std::string port, RTLIL::SigSpec sig, bool driver, std::string *node = NULL)
	{
		std::string code;
		std::string net = gen_signode_simple(sig);
		if (net.empty())
		{
			std::string label_string;
			sig.optimize();
			int pos = sig.width-1;
			int idx = single_idx_count++;
			for (int i = int(sig.chunks.size())-1; i >= 0; i--) {
				RTLIL::SigChunk &c = sig.chunks[i];
				net = gen_signode_simple(c, false);
				assert(!net.empty());
				if (driver) {
					label_string += stringf("<s%d> %d:%d - %d:%d |", i, pos, pos-c.width+1, c.offset+c.width-1, c.offset);
					net_conn_map[net].in.insert(stringf("x%d:s%d", idx, i));
					net_conn_map[net].bits = c.width;
					net_conn_map[net].color = nextColor(c, net_conn_map[net].color);
				} else {
					label_string += stringf("<s%d> %d:%d - %d:%d |", i, c.offset+c.width-1, c.offset, pos, pos-c.width+1);
					net_conn_map[net].out.insert(stringf("x%d:s%d", idx, i));
					net_conn_map[net].bits = c.width;
					net_conn_map[net].color = nextColor(c, net_conn_map[net].color);
				}
				pos -= c.width;
			}
			if (label_string[label_string.size()-1] == '|')
				label_string = label_string.substr(0, label_string.size()-1);
			code += stringf("x%d [ shape=record, style=rounded, label=\"%s\" ];\n", idx, label_string.c_str());
			if (!port.empty()) {
				currentColor = xorshift32(currentColor);
				if (driver)
					code += stringf("%s:e -> x%d:w [arrowhead=odiamond, arrowtail=odiamond, dir=both, %s, %s];\n", port.c_str(), idx, nextColor(sig).c_str(), widthLabel(sig.width).c_str());
				else
					code += stringf("x%d:e -> %s:w [arrowhead=odiamond, arrowtail=odiamond, dir=both, %s, %s];\n", idx, port.c_str(), nextColor(sig).c_str(), widthLabel(sig.width).c_str());
			}
			if (node != NULL)
				*node = stringf("x%d", idx);
		}
		else
		{
			if (!port.empty()) {
				if (driver)
					net_conn_map[net].in.insert(port);
				else
					net_conn_map[net].out.insert(port);
				net_conn_map[net].bits = sig.width;
				net_conn_map[net].color = nextColor(sig, net_conn_map[net].color);
			}
			if (node != NULL)
				*node = net;
		}
		return code;
	}

	void collect_proc_signals(std::vector<RTLIL::SigSpec> &obj, std::set<RTLIL::SigSpec> &signals)
	{
		for (auto &it : obj)
			if (!it.is_fully_const())
				signals.insert(it);
	}

	void collect_proc_signals(std::vector<RTLIL::SigSig> &obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		for (auto &it : obj) {
			output_signals.insert(it.first);
			if (!it.second.is_fully_const())
				input_signals.insert(it.second);
		}
	}

	void collect_proc_signals(RTLIL::CaseRule *obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		collect_proc_signals(obj->compare, input_signals);
		collect_proc_signals(obj->actions, input_signals, output_signals);
		for (auto it : obj->switches)
			collect_proc_signals(it, input_signals, output_signals);
	}

	void collect_proc_signals(RTLIL::SwitchRule *obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		input_signals.insert(obj->signal);
		for (auto it : obj->cases)
			collect_proc_signals(it, input_signals, output_signals);
	}

	void collect_proc_signals(RTLIL::SyncRule *obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		input_signals.insert(obj->signal);
		collect_proc_signals(obj->actions, input_signals, output_signals);
	}

	void collect_proc_signals(RTLIL::Process *obj, std::set<RTLIL::SigSpec> &input_signals, std::set<RTLIL::SigSpec> &output_signals)
	{
		collect_proc_signals(&obj->root_case, input_signals, output_signals);
		for (auto it : obj->syncs)
			collect_proc_signals(it, input_signals, output_signals);
	}

	void handle_module()
	{
		single_idx_count = 0;
		dot_escape_store.clear();
		dot_id2num_store.clear();
		net_conn_map.clear();

		fprintf(f, "digraph \"%s\" {\n", escape(module->name));
		if (!notitle)
			fprintf(f, "label=\"%s\";\n", escape(module->name));
		fprintf(f, "rankdir=\"LR\";\n");
		fprintf(f, "remincross=true;\n");

		std::set<std::string> all_sources, all_sinks;

		std::map<std::string, std::string> wires_on_demand;
		for (auto &it : module->wires) {
			if (!design->selected_member(module->name, it.first))
				continue;
			const char *shape = "diamond";
			if (it.second->port_input || it.second->port_output)
				shape = "octagon";
			if (it.first[0] == '\\') {
				fprintf(f, "n%d [ shape=%s, label=\"%s\", %s, fontcolor=\"black\" ];\n",
						id2num(it.first), shape, findLabel(it.first),
						nextColor(RTLIL::SigSpec(it.second), "color=\"black\"").c_str());
				if (it.second->port_input)
					all_sources.insert(stringf("n%d", id2num(it.first)));
				else if (it.second->port_output)
					all_sinks.insert(stringf("n%d", id2num(it.first)));
			} else {
				wires_on_demand[stringf("n%d", id2num(it.first))] = it.first;
			}
		}

		if (stretchIO)
		{
			fprintf(f, "{ rank=\"source\";");
			for (auto n : all_sources)
				fprintf(f, " %s;", n.c_str());
			fprintf(f, "}\n");

			fprintf(f, "{ rank=\"sink\";");
			for (auto n : all_sinks)
				fprintf(f, " %s;", n.c_str());
			fprintf(f, "}\n");
		}

		for (auto &it : module->cells)
		{
			if (!design->selected_member(module->name, it.first))
				continue;

			std::vector<RTLIL::IdString> in_ports, out_ports;

			for (auto &conn : it.second->connections) {
				if (!ct.cell_output(it.second->type, conn.first))
					in_ports.push_back(conn.first);
				else
					out_ports.push_back(conn.first);
			}

			std::string label_string = "{{";

			for (auto &p : in_ports)
				label_string += stringf("<p%d> %s|", id2num(p), escape(p));
			if (label_string[label_string.size()-1] == '|')
				label_string = label_string.substr(0, label_string.size()-1);

			label_string += stringf("}|%s\\n%s|{", findLabel(it.first), escape(it.second->type));

			for (auto &p : out_ports)
				label_string += stringf("<p%d> %s|", id2num(p), escape(p));
			if (label_string[label_string.size()-1] == '|')
				label_string = label_string.substr(0, label_string.size()-1);

			label_string += "}}";

			std::string code;
			for (auto &conn : it.second->connections) {
				code += gen_portbox(stringf("c%d:p%d", id2num(it.first), id2num(conn.first)),
						conn.second, ct.cell_output(it.second->type, conn.first));
			}

#ifdef CLUSTER_CELLS_AND_PORTBOXES
			if (!code.empty())
				fprintf(f, "subgraph cluster_c%d {\nc%d [ shape=record, label=\"%s\"%s ];\n%s}\n",
						id2num(it.first), id2num(it.first), label_string.c_str(), findColor(it.first), code.c_str());
			else
#endif
				fprintf(f, "c%d [ shape=record, label=\"%s\"%s ];\n%s",
						id2num(it.first), label_string.c_str(), findColor(it.first), code.c_str());
		}

		for (auto &it : module->processes)
		{
			RTLIL::Process *proc = it.second;

			if (!design->selected_member(module->name, proc->name))
				continue;

			std::set<RTLIL::SigSpec> input_signals, output_signals;
			collect_proc_signals(proc, input_signals, output_signals);

			int pidx = single_idx_count++;
			input_signals.erase(RTLIL::SigSpec());
			output_signals.erase(RTLIL::SigSpec());

			for (auto &sig : input_signals) {
				std::string code, node;
				code += gen_portbox("", sig, false, &node);
				fprintf(f, "%s", code.c_str());
				net_conn_map[node].out.insert(stringf("p%d", pidx));
				net_conn_map[node].bits = sig.width;
				net_conn_map[node].color = nextColor(sig, net_conn_map[node].color);
			}

			for (auto &sig : output_signals) {
				std::string code, node;
				code += gen_portbox("", sig, true, &node);
				fprintf(f, "%s", code.c_str());
				net_conn_map[node].in.insert(stringf("p%d", pidx));
				net_conn_map[node].bits = sig.width;
				net_conn_map[node].color = nextColor(sig, net_conn_map[node].color);
			}

			std::string proc_src = RTLIL::unescape_id(proc->name);
			if (proc->attributes.count("\\src") > 0)
				proc_src = proc->attributes.at("\\src").decode_string();
			fprintf(f, "p%d [shape=box, style=rounded, label=\"PROC %s\\n%s\"];\n", pidx, findLabel(proc->name), proc_src.c_str());
		}

		for (auto &conn : module->connections)
		{
			bool found_lhs_wire = false;
			for (auto &c : conn.first.chunks) {
				if (c.wire == NULL || design->selected_member(module->name, c.wire->name))
					found_lhs_wire = true;
			}
			bool found_rhs_wire = false;
			for (auto &c : conn.second.chunks) {
				if (c.wire == NULL || design->selected_member(module->name, c.wire->name))
					found_rhs_wire = true;
			}
			if (!found_lhs_wire || !found_rhs_wire)
				continue;

			std::string code, left_node, right_node;
			code += gen_portbox("", conn.second, false, &left_node);
			code += gen_portbox("", conn.first, true, &right_node);
			fprintf(f, "%s", code.c_str());

			if (left_node[0] == 'x' && right_node[0] == 'x') {
				currentColor = xorshift32(currentColor);
			fprintf(f, "%s:e -> %s:w [arrowhead=odiamond, arrowtail=odiamond, dir=both, %s, %s];\n", left_node.c_str(), right_node.c_str(), nextColor(conn).c_str(), widthLabel(conn.first.width).c_str());
		} else {
				net_conn_map[right_node].bits = conn.first.width;
				net_conn_map[right_node].color = nextColor(conn, net_conn_map[right_node].color);
				net_conn_map[left_node].bits = conn.first.width;
				net_conn_map[left_node].color = nextColor(conn, net_conn_map[left_node].color);
				if (left_node[0] == 'x') {
					net_conn_map[right_node].in.insert(left_node);
				} else if (right_node[0] == 'x') {
					net_conn_map[left_node].out.insert(right_node);
				} else {
					net_conn_map[right_node].in.insert(stringf("x%d:e", single_idx_count));
					net_conn_map[left_node].out.insert(stringf("x%d:w", single_idx_count));
					fprintf(f, "x%d [shape=box, style=rounded, label=\"BUF\"];\n", single_idx_count++);
				}
			}
		}

		for (auto &it : net_conn_map)
		{
			currentColor = xorshift32(currentColor);
			if (wires_on_demand.count(it.first) > 0) {
				if (it.second.in.size() == 1 && it.second.out.size() > 1 && it.second.in.begin()->substr(0, 1) == "p")
					it.second.out.erase(*it.second.in.begin());
				if (it.second.in.size() == 1 && it.second.out.size() == 1) {
					std::string from = *it.second.in.begin(), to = *it.second.out.begin();
					if (from != to || from.substr(0, 1) != "p")
						fprintf(f, "%s:e -> %s:w [%s, %s];\n", from.c_str(), to.c_str(), nextColor(it.second.color).c_str(), widthLabel(it.second.bits).c_str());
					continue;
				}
				if (it.second.in.size() == 0 || it.second.out.size() == 0)
					fprintf(f, "%s [ shape=diamond, label=\"%s\" ];\n", it.first.c_str(), findLabel(wires_on_demand[it.first]));
				else
					fprintf(f, "%s [ shape=point ];\n", it.first.c_str());
			}
			for (auto &it2 : it.second.in)
				fprintf(f, "%s:e -> %s:w [%s, %s];\n", it2.c_str(), it.first.c_str(), nextColor(it.second.color).c_str(), widthLabel(it.second.bits).c_str());
			for (auto &it2 : it.second.out)
				fprintf(f, "%s:e -> %s:w [%s, %s];\n", it.first.c_str(), it2.c_str(), nextColor(it.second.color).c_str(), widthLabel(it.second.bits).c_str());
		}

		fprintf(f, "}\n");
	}

	ShowWorker(FILE *f, RTLIL::Design *design, std::vector<RTLIL::Design*> &libs, uint32_t colorSeed,
			bool genWidthLabels, bool stretchIO, bool enumerateIds, bool abbreviateIds, bool notitle,
			const std::vector<std::pair<std::string, RTLIL::Selection>> &color_selections,
			const std::vector<std::pair<std::string, RTLIL::Selection>> &label_selections) :
			f(f), design(design), currentColor(colorSeed), genWidthLabels(genWidthLabels),
			stretchIO(stretchIO), enumerateIds(enumerateIds), abbreviateIds(abbreviateIds),
			notitle(notitle), color_selections(color_selections), label_selections(label_selections)
	{
		ct.setup_internals();
		ct.setup_internals_mem();
		ct.setup_stdcells();
		ct.setup_stdcells_mem();
		ct.setup_design(design);

		for (auto lib : libs)
			ct.setup_design(lib);

		design->optimize();
		page_counter = 0;
		for (auto &mod_it : design->modules)
		{
			module = mod_it.second;
			if (!design->selected_module(module->name))
				continue;
			if (design->selected_whole_module(module->name)) {
				if (module->get_bool_attribute("\\blackbox")) {
					log("Skipping blackbox module %s.\n", id2cstr(module->name));
					continue;
				} else
				if (module->cells.empty() && module->connections.empty() && module->processes.empty()) {
					log("Skipping empty module %s.\n", id2cstr(module->name));
					continue;
				} else
					log("Dumping module %s to page %d.\n", id2cstr(module->name), ++page_counter);
			} else
				log("Dumping selected parts of module %s to page %d.\n", id2cstr(module->name), ++page_counter);
			handle_module();
		}
	}
};

struct ShowPass : public Pass {
	ShowPass() : Pass("show", "generate schematics using graphviz") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    show [options] [selection]\n");
		log("\n");
		log("Create a graphviz DOT file for the selected part of the design and compile it\n");
		log("to a graphics file (usually SVG or PostScript).\n");
		log("\n");
		log("    -viewer <viewer>\n");
		log("        Run the specified command with the graphics file as parameter.\n");
		log("\n");
		log("    -format <format>\n");
		log("        Generate a graphics file in the specified format.\n");
		log("        Usually <format> is 'svg' or 'ps'.\n");
		log("\n");
		log("    -lib <verilog_or_ilang_file>\n");
		log("        Use the specified library file for determining whether cell ports are\n");
		log("        inputs or outputs. This option can be used multiple times to specify\n");
		log("        more than one library.\n");
		log("\n");
		log("    -prefix <prefix>\n");
		log("        generate <prefix>.* instead of ~/.yosys_show.*\n");
		log("\n");
		log("    -color <color> <object>\n");
		log("        assign the specified color to the specified object. The object can be\n");
		log("        a single selection wildcard expressions or a saved set of objects in\n");
		log("        the @<name> syntax (see \"help select\" for details).\n");
		log("\n");
		log("    -label <text> <object>\n");
		log("        assign the specified label text to the specified object. The object can\n");
		log("        be a single selection wildcard expressions or a saved set of objects in\n");
		log("        the @<name> syntax (see \"help select\" for details).\n");
		log("\n");
		log("    -colors <seed>\n");
		log("        Randomly assign colors to the wires. The integer argument is the seed\n");
		log("        for the random number generator. Change the seed value if the colored\n");
		log("        graph still is ambigous. A seed of zero deactivates the coloring.\n");
		log("\n");
		log("    -width\n");
		log("        annotate busses with a label indicating the width of the bus.\n");
		log("\n");
		log("    -stretch\n");
		log("        stretch the graph so all inputs are on the left side and all outputs\n");
		log("        (including inout ports) are on the right side.\n");
		log("\n");
		log("    -pause\n");
		log("        wait for the use to press enter to before returning\n");
		log("\n");
		log("    -enum\n");
		log("        enumerate objects with internal ($-prefixed) names\n");
		log("\n");
		log("    -long\n");
		log("        do not abbeviate objects with internal ($-prefixed) names\n");
		log("\n");
		log("    -notitle\n");
		log("        do not add the module name as graph title to the dot file\n");
		log("\n");
		log("When no <format> is specified, SVG is used. When no <format> and <viewer> is\n");
		log("specified, 'yosys-svgviewer' is used to display the schematic.\n");
		log("\n");
		log("The generated output files are '~/.yosys_show.dot' and '~/.yosys_show.<format>',\n");
		log("unless another prefix is specified using -prefix <prefix>.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design)
	{
		log_header("Generating Graphviz representation of design.\n");
		log_push();

		std::vector<std::pair<std::string, RTLIL::Selection>> color_selections;
		std::vector<std::pair<std::string, RTLIL::Selection>> label_selections;

		std::string format;
		std::string viewer_exe;
		std::string prefix = stringf("%s/.yosys_show", getenv("HOME") ? getenv("HOME") : ".");
		std::vector<std::string> libfiles;
		std::vector<RTLIL::Design*> libs;
		uint32_t colorSeed = 0;
		bool flag_width = false;
		bool flag_stretch = false;
		bool flag_pause = false;
		bool flag_enum = false;
		bool flag_abbeviate = true;
		bool flag_notitle = false;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			std::string arg = args[argidx];
			if (arg == "-viewer" && argidx+1 < args.size()) {
				viewer_exe = args[++argidx];
				continue;
			}
			if (arg == "-lib" && argidx+1 < args.size()) {
				libfiles.push_back(args[++argidx]);
				continue;
			}
			if (arg == "-prefix" && argidx+1 < args.size()) {
				prefix = args[++argidx];
				continue;
			}
			if (arg == "-color" && argidx+2 < args.size()) {
				std::pair<std::string, RTLIL::Selection> data;
				data.first = args[++argidx], argidx++;
				handle_extra_select_args(this, args, argidx, argidx+1, design);
				data.second = design->selection_stack.back();
				design->selection_stack.pop_back();
				color_selections.push_back(data);
				continue;
			}
			if (arg == "-label" && argidx+2 < args.size()) {
				std::pair<std::string, RTLIL::Selection> data;
				data.first = args[++argidx], argidx++;
				handle_extra_select_args(this, args, argidx, argidx+1, design);
				data.second = design->selection_stack.back();
				design->selection_stack.pop_back();
				label_selections.push_back(data);
				continue;
			}
			if (arg == "-colors" && argidx+1 < args.size()) {
				colorSeed = atoi(args[++argidx].c_str());
				continue;
			}
			if (arg == "-format" && argidx+1 < args.size()) {
				format = args[++argidx];
				continue;
			}
			if (arg == "-width") {
				flag_width= true;
				continue;
			}
			if (arg == "-stretch") {
				flag_stretch= true;
				continue;
			}
			if (arg == "-pause") {
				flag_pause= true;
				continue;
			}
			if (arg == "-enum") {
				flag_enum = true;
				flag_abbeviate = false;
				continue;
			}
			if (arg == "-long") {
				flag_enum = false;
				flag_abbeviate = false;
				continue;
			}
			if (arg == "-notitle") {
				flag_notitle = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		if (format != "ps") {
			int modcount = 0;
			for (auto &mod_it : design->modules) {
				if (mod_it.second->get_bool_attribute("\\blackbox"))
					continue;
				if (mod_it.second->cells.empty() && mod_it.second->connections.empty())
					continue;
				if (design->selected_module(mod_it.first))
					modcount++;
			}
			if (modcount > 1)
				log_cmd_error("For formats different than 'ps' only one module must be selected.\n");
		}

		for (auto filename : libfiles) {
			FILE *f = fopen(filename.c_str(), "rt");
			if (f == NULL)
				log_error("Can't open lib file `%s'.\n", filename.c_str());
			RTLIL::Design *lib = new RTLIL::Design;
			Frontend::frontend_call(lib, f, filename, (filename.size() > 3 && filename.substr(filename.size()-3) == ".il") ? "ilang" : "verilog");
			libs.push_back(lib);
			fclose(f);
		}

		if (libs.size() > 0)
			log_header("Continuing show pass.\n");

		std::string dot_file = stringf("%s.dot", prefix.c_str());
		std::string out_file = stringf("%s.%s", prefix.c_str(), format.empty() ? "svg" : format.c_str());

		log("Writing dot description to `%s'.\n", dot_file.c_str());
		FILE *f = fopen(dot_file.c_str(), "w");
		if (f == NULL) {
			for (auto lib : libs)
				delete lib;
			log_cmd_error("Can't open dot file `%s' for writing.\n", dot_file.c_str());
		}
		ShowWorker worker(f, design, libs, colorSeed, flag_width, flag_stretch, flag_enum, flag_abbeviate, flag_notitle, color_selections, label_selections);
		fclose(f);

		for (auto lib : libs)
			delete lib;

		if (worker.page_counter == 0)
			log_cmd_error("Nothing there to show.\n");

		if (format != "dot") {
			std::string cmd = stringf("dot -T%s -o '%s' '%s'", format.empty() ? "svg" : format.c_str(), out_file.c_str(), dot_file.c_str());
			log("Exec: %s\n", cmd.c_str());
			if (system(cmd.c_str()) != 0)
				log_cmd_error("Shell command failed!\n");
		}

		if (!viewer_exe.empty()) {
			std::string cmd = stringf("%s '%s' &", viewer_exe.c_str(), out_file.c_str());
			log("Exec: %s\n", cmd.c_str());
			if (system(cmd.c_str()) != 0)
				log_cmd_error("Shell command failed!\n");
		} else
		if (format.empty()) {
			std::string svgviewer = proc_self_dirname() + "yosys-svgviewer";
			std::string cmd = stringf("fuser -s '%s' || '%s' '%s' &", out_file.c_str(), svgviewer.c_str(), out_file.c_str());
			log("Exec: %s\n", cmd.c_str());
			if (system(cmd.c_str()) != 0)
				log_cmd_error("Shell command failed!\n");
		}

		if (flag_pause) {
			char *input = NULL;
			while ((input = readline("Press ENTER to continue (or type 'shell' to open a shell)> ")) != NULL) {
				if (input[strspn(input, " \t\r\n")] == 0)
					break;
				char *p = input + strspn(input, " \t\r\n");
				if (!strcmp(p, "shell")) {
					Pass::call(design, "shell");
					break;
				}
			}
		}

		log_pop();
	}
} ShowPass;
 
