// Road searches of the simulation (docs/movement-spec.md section 12, economy-spec 6.4, world-spec 6.3): the road a started or
// regenerated building paths to its city centre. PathFinder::find_road @688a40 -> astar_caravan_road @685990 (building roads:
// 4-connected, `cara` < 0) with calc_road_cost @686300 (ONE game_random draw per valid neighbour), valid_roadcoord @688740, and
// the REGEN_TOTAL / REGEN_FORCE tail of BuildType::place_roads @63c580.
#pragma once

#include <cstdint>
#include <vector>

#include "sim/objects.hpp"
#include "sim/placement.hpp"

namespace ron::sim {

class Game;

// PathFinder::find_road for a building road (unit = -1): path from building (src_o, src_pl) to (dst_o, dst_pl); the nodes (without the
// start and the goal, without tiles already BLOCKED) are pushed onto `stack` from the goal side to the start side. Returns the stack
// length, or -1 (no path within 3200 expansions, bad endpoints).
int32_t find_road(Game& g, std::vector<PathData>& stack, int32_t src_o, int32_t src_pl, int32_t dst_o, int32_t dst_pl);

// The tail of BuildType::place_roads for building `b` (after the ring of set_road_at calls): `add` with REGEN_TOTAL marks the building
// MAKE_ROAD (Build +0x60 |= 0x100, processed every 16 frames by Build::process); with REGEN_FORCE it paths a road to the city centre
// (find_road) and lays it with set_road_at. Only for non-city buildings that belong to a city (Build +0x72 >= 0) and are not 'g' types.
void place_roads_regen(Game& g, Build& b, bool add, Regen regen);

}  // namespace ron::sim
