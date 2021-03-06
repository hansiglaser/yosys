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
#include "kernel/log.h"
#include <sstream>
#include <algorithm>
#include <stdlib.h>
#include <assert.h>

static void handle_memory(RTLIL::Module *module, RTLIL::Cell *memory)
{
	log("Creating $memrd and $memwr for memory `%s' in module `%s':\n",
			memory->name.c_str(), module->name.c_str());

	RTLIL::IdString mem_name = RTLIL::escape_id(memory->parameters.at("\\MEMID").decode_string());

	while (module->memories.count(mem_name) != 0)
		mem_name += stringf("_%d", RTLIL::autoidx++);

	RTLIL::Memory *mem = new RTLIL::Memory;
	mem->name = mem_name;
	mem->width = memory->parameters.at("\\WIDTH").as_int();
	mem->start_offset = memory->parameters.at("\\OFFSET").as_int();
	mem->size = memory->parameters.at("\\SIZE").as_int();
	module->memories[mem_name] = mem;

	int abits = memory->parameters.at("\\ABITS").as_int();
	int num_rd_ports = memory->parameters.at("\\RD_PORTS").as_int();
	int num_wr_ports = memory->parameters.at("\\WR_PORTS").as_int();

	for (int i = 0; i < num_rd_ports; i++)
	{
		RTLIL::Cell *cell = new RTLIL::Cell;
		cell->name = NEW_ID;
		cell->type = "$memrd";
		cell->parameters["\\MEMID"] = mem_name;
		cell->parameters["\\ABITS"] = memory->parameters.at("\\ABITS");
		cell->parameters["\\WIDTH"] = memory->parameters.at("\\WIDTH");
		cell->parameters["\\CLK_ENABLE"] = RTLIL::SigSpec(memory->parameters.at("\\RD_CLK_ENABLE")).extract(i, 1).as_const();
		cell->parameters["\\CLK_POLARITY"] = RTLIL::SigSpec(memory->parameters.at("\\RD_CLK_POLARITY")).extract(i, 1).as_const();
		cell->parameters["\\TRANSPARENT"] = RTLIL::SigSpec(memory->parameters.at("\\RD_TRANSPARENT")).extract(i, 1).as_const();
		cell->connections["\\CLK"] = memory->connections.at("\\RD_CLK").extract(i, 1);
		cell->connections["\\ADDR"] = memory->connections.at("\\RD_ADDR").extract(i*abits, abits);
		cell->connections["\\DATA"] = memory->connections.at("\\RD_DATA").extract(i*mem->width, mem->width);
		module->add(cell);
	}

	for (int i = 0; i < num_wr_ports; i++)
	{
		RTLIL::Cell *cell = new RTLIL::Cell;
		cell->name = NEW_ID;
		cell->type = "$memwr";
		cell->parameters["\\MEMID"] = mem_name;
		cell->parameters["\\ABITS"] = memory->parameters.at("\\ABITS");
		cell->parameters["\\WIDTH"] = memory->parameters.at("\\WIDTH");
		cell->parameters["\\CLK_ENABLE"] = RTLIL::SigSpec(memory->parameters.at("\\WR_CLK_ENABLE")).extract(i, 1).as_const();
		cell->parameters["\\CLK_POLARITY"] = RTLIL::SigSpec(memory->parameters.at("\\WR_CLK_POLARITY")).extract(i, 1).as_const();
		cell->parameters["\\PRIORITY"] = i;
		cell->connections["\\CLK"] = memory->connections.at("\\WR_CLK").extract(i, 1);
		cell->connections["\\EN"] = memory->connections.at("\\WR_EN").extract(i, 1);
		cell->connections["\\ADDR"] = memory->connections.at("\\WR_ADDR").extract(i*abits, abits);
		cell->connections["\\DATA"] = memory->connections.at("\\WR_DATA").extract(i*mem->width, mem->width);
		module->add(cell);
	}

	module->cells.erase(memory->name);
	delete memory;
}

static void handle_module(RTLIL::Design *design, RTLIL::Module *module)
{
	std::vector<RTLIL::IdString> memcells;
	for (auto &cell_it : module->cells)
		if (cell_it.second->type == "$mem" && design->selected(module, cell_it.second))
			memcells.push_back(cell_it.first);
	for (auto &it : memcells)
		handle_memory(module, module->cells.at(it));
}

struct MemoryUnpackPass : public Pass {
	MemoryUnpackPass() : Pass("memory_unpack", "unpack multi-port memory cells") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    memory_unpack [selection]\n");
		log("\n");
		log("This pass converts the multi-port $mem memory cells into individual $memrd and\n");
		log("$memwr cells. It is the counterpart to the memory_collect pass.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, RTLIL::Design *design) {
		log_header("Executing MEMORY_UNPACK pass (generating $memrd/$memwr cells form $mem cells).\n");
		extra_args(args, 1, design);
		for (auto &mod_it : design->modules)
			if (design->selected(mod_it.second))
				handle_module(design, mod_it.second);
	}
} MemoryUnpackPass;
 
