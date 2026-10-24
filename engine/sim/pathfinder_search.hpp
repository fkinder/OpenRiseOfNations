// The time-sliced search containers of the PathFinder (pathfinder.cpp): PathNode and SavedSearch, split out of the .cpp so
// that the save / load code (sim/save.cpp) can walk them. Nothing else should include this header.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <utility>

#include "sim/pathfinder.hpp"

namespace ron::sim {

// PathNode (0x24 bytes in the original).
struct PathNode {
	int32_t x = 0, y = 0;
	int32_t length = 0, estimate = 0, value = 0, timeout = 0;
	uint32_t metric = 0;  // cell index (the BRTree keys are unsigned)
	uint8_t transport = 0, building = 0;
	PathNode* parent = nullptr;
};

// The five trees of a search (docs/movement-spec.md section 7.4).
// - open: Tree<PathNode*, int> keyed by value. ordered_insert @4796f0 descends left on key <= node.key and
//   remove_current @479770 keeps the in-order sequence, so the open list is "lowest value first, newest first among
//   equal values": the map key is (value, -insertion sequence).
// - refs (BRTree metric -> open tree node), closed (BRTree metric -> node), valid (BRTree key -> int): unique keys,
//   behaviour depends only on the key sets, so maps.
// - blocks: the collision-block snapshot read by collide_here(pathing).
// Nodes live in an arena owned by the search; recycling has no observable effect.
struct SavedSearch {
	using OpenKey = std::pair<int32_t, int64_t>;
	std::deque<PathNode> nodes;
	std::map<OpenKey, PathNode*> open;
	std::map<uint32_t, OpenKey> refs;
	std::map<uint32_t, PathNode*> closed;
	std::map<uint32_t, int32_t> valid;
	BlockSnapshot blocks;
	int64_t seq = 0;

	PathNode* new_node(const PathNode& n) {
		nodes.push_back(n);
		return &nodes.back();
	}
	void insert_open(PathNode* n) {
		const OpenKey k{n->value, -(++seq)};
		open[k] = n;
		refs[n->metric] = k;  // ordered_insert overwrites an existing key
	}
	// PathFinder::first_open_node @687970: removes the leftmost node and its ref.
	PathNode* first_open_node() {
		const auto it = open.begin();
		PathNode* n = it->second;
		const auto r = refs.find(n->metric);
		if (r != refs.end() && r->second == it->first) refs.erase(r);
		open.erase(it);
		return n;
	}
	// PathFinderData::find_node_open @6882b0.
	PathNode* find_open(uint32_t metric, OpenKey* key) const {
		const auto r = refs.find(metric);
		if (r == refs.end()) return nullptr;
		*key = r->second;
		return open.at(r->second);
	}
	void clear() {
		open.clear();
		refs.clear();
		closed.clear();
		valid.clear();
		blocks.clear();
		nodes.clear();
	}
};

}  // namespace ron::sim
