/*
 * Copyright 2016-2019 Arm Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "spirv_cfg.hpp"
#include "spirv_cross.hpp"
#include <algorithm>
#include <assert.h>

using namespace std;

namespace SPIRV_CROSS_NAMESPACE
{
CFG::CFG(Compiler &compiler_, const SPIRFunction &func_)
    : compiler(compiler_)
    , func(func_)
{
	build_post_order_visit_order();
	build_immediate_dominators();
}

uint32_t CFG::find_common_dominator(uint32_t a, uint32_t b) const
{
	while (a != b)
	{
		if (get_visit_order(a) < get_visit_order(b))
			a = get_immediate_dominator(a);
		else
			b = get_immediate_dominator(b);
	}
	return a;
}

void CFG::build_immediate_dominators()
{
	// Traverse the post-order in reverse and build up the immediate dominator tree.
	immediate_dominators.clear();
	immediate_dominators[func.entry_block] = func.entry_block;

	for (auto i = post_order.size(); i; i--)
	{
		uint32_t block = post_order[i - 1];
		auto &pred = preceding_edges[block];
		if (pred.empty()) // This is for the entry block, but we've already set up the dominators.
			continue;

		for (auto &edge : pred)
		{
			if (immediate_dominators[block])
			{
				assert(immediate_dominators[edge]);
				immediate_dominators[block] = find_common_dominator(block, edge);
			}
			else
				immediate_dominators[block] = edge;
		}
	}
}

bool CFG::is_back_edge(uint32_t to) const
{
	// We have a back edge if the visit order is set with the temporary magic value 0.
	// Crossing edges will have already been recorded with a visit order.
	auto itr = visit_order.find(to);
	assert(itr != end(visit_order));
	return itr->second.get() == 0;
}

bool CFG::post_order_visit(uint32_t block_id)
{
	// If we have already branched to this block (back edge), stop recursion.
	// If our branches are back-edges, we do not record them.
	// We have to record crossing edges however.
	if (visit_order[block_id].get() >= 0)
		return !is_back_edge(block_id);

	// Block back-edges from recursively revisiting ourselves.
	visit_order[block_id].get() = 0;

	// First visit our branch targets.
	auto &block = compiler.get<SPIRBlock>(block_id);
	switch (block.terminator)
	{
	case SPIRBlock::Direct:
		if (post_order_visit(block.next_block))
			add_branch(block_id, block.next_block);
		break;

	case SPIRBlock::Select:
		if (post_order_visit(block.true_block))
			add_branch(block_id, block.true_block);
		if (post_order_visit(block.false_block))
			add_branch(block_id, block.false_block);
		break;

	case SPIRBlock::MultiSelect:
		for (auto &target : block.cases)
		{
			if (post_order_visit(target.block))
				add_branch(block_id, target.block);
		}
		if (block.default_block && post_order_visit(block.default_block))
			add_branch(block_id, block.default_block);
		break;

	default:
		break;
	}

	// If this is a loop header, add an implied branch to the merge target.
	// This is needed to avoid annoying cases with do { ... } while(false) loops often generated by inliners.
	// To the CFG, this is linear control flow, but we risk picking the do/while scope as our dominating block.
	// This makes sure that if we are accessing a variable outside the do/while, we choose the loop header as dominator.
	if (block.merge == SPIRBlock::MergeLoop)
		add_branch(block_id, block.merge_block);

	// Then visit ourselves. Start counting at one, to let 0 be a magic value for testing back vs. crossing edges.
	visit_order[block_id].get() = ++visit_count;
	post_order.push_back(block_id);
	return true;
}

void CFG::build_post_order_visit_order()
{
	uint32_t block = func.entry_block;
	visit_count = 0;
	visit_order.clear();
	post_order.clear();
	post_order_visit(block);
}

void CFG::add_branch(uint32_t from, uint32_t to)
{
	const auto add_unique = [](SmallVector<uint32_t> &l, uint32_t value) {
		auto itr = find(begin(l), end(l), value);
		if (itr == end(l))
			l.push_back(value);
	};
	add_unique(preceding_edges[to], from);
	add_unique(succeeding_edges[from], to);
}

uint32_t CFG::find_loop_dominator(uint32_t block_id) const
{
	while (block_id != 0)
	{
		auto itr = preceding_edges.find(block_id);
		if (itr == end(preceding_edges))
			return 0;
		if (itr->second.empty())
			return 0;

		uint32_t pred_block_id = 0;
		bool ignore_loop_header = false;

		// If we are a merge block, go directly to the header block.
		// Only consider a loop dominator if we are branching from inside a block to a loop header.
		// NOTE: In the CFG we forced an edge from header to merge block always to support variable scopes properly.
		for (auto &pred : itr->second)
		{
			auto &pred_block = compiler.get<SPIRBlock>(pred);
			if (pred_block.merge == SPIRBlock::MergeLoop && pred_block.merge_block == block_id)
			{
				pred_block_id = pred;
				ignore_loop_header = true;
				break;
			}
			else if (pred_block.merge == SPIRBlock::MergeSelection && pred_block.next_block == block_id)
			{
				pred_block_id = pred;
				break;
			}
		}

		// No merge block means we can just pick any edge. Loop headers dominate the inner loop, so any path we
		// take will lead there.
		if (!pred_block_id)
			pred_block_id = itr->second.front();

		block_id = pred_block_id;

		if (!ignore_loop_header && block_id)
		{
			auto &block = compiler.get<SPIRBlock>(block_id);
			if (block.merge == SPIRBlock::MergeLoop)
				return block_id;
		}
	}

	return block_id;
}

DominatorBuilder::DominatorBuilder(const CFG &cfg_)
    : cfg(cfg_)
{
}

void DominatorBuilder::add_block(uint32_t block)
{
	if (!cfg.get_immediate_dominator(block))
	{
		// Unreachable block via the CFG, we will never emit this code anyways.
		return;
	}

	if (!dominator)
	{
		dominator = block;
		return;
	}

	if (block != dominator)
		dominator = cfg.find_common_dominator(block, dominator);
}

void DominatorBuilder::lift_continue_block_dominator()
{
	// It is possible for a continue block to be the dominator of a variable is only accessed inside the while block of a do-while loop.
	// We cannot safely declare variables inside a continue block, so move any variable declared
	// in a continue block to the entry block to simplify.
	// It makes very little sense for a continue block to ever be a dominator, so fall back to the simplest
	// solution.

	if (!dominator)
		return;

	auto &block = cfg.get_compiler().get<SPIRBlock>(dominator);
	auto post_order = cfg.get_visit_order(dominator);

	// If we are branching to a block with a higher post-order traversal index (continue blocks), we have a problem
	// since we cannot create sensible GLSL code for this, fallback to entry block.
	bool back_edge_dominator = false;
	switch (block.terminator)
	{
	case SPIRBlock::Direct:
		if (cfg.get_visit_order(block.next_block) > post_order)
			back_edge_dominator = true;
		break;

	case SPIRBlock::Select:
		if (cfg.get_visit_order(block.true_block) > post_order)
			back_edge_dominator = true;
		if (cfg.get_visit_order(block.false_block) > post_order)
			back_edge_dominator = true;
		break;

	case SPIRBlock::MultiSelect:
		for (auto &target : block.cases)
		{
			if (cfg.get_visit_order(target.block) > post_order)
				back_edge_dominator = true;
		}
		if (block.default_block && cfg.get_visit_order(block.default_block) > post_order)
			back_edge_dominator = true;
		break;

	default:
		break;
	}

	if (back_edge_dominator)
		dominator = cfg.get_function().entry_block;
}
} // namespace SPIRV_CROSS_NAMESPACE
