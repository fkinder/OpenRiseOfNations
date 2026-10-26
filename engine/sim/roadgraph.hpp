// The road graph of the original (Roads / RoadsOut / TerrainOut road data), the part of it that changes simulation state:
// the road *candidates* (RoadElementCandidate, 16 bytes per tcoord with a road, kept per wcoord in CoordInfo.roads_in_wcoord and
// saved by Terrain::walk_roads) with their arm bits, and the three things the graph does to the tile grid:
//   - Roads::set_diags @88e9d0 lays a corner road tile next to a road tile whose diagonal neighbour is a road (a diagonal step of a
//     road that has no orthogonal link; the horizontal neighbour is the one laid),
//   - Roads::clear_roads / clear_support @88fef0 / @88e3f0 remove such corner tiles again when the road that needed them goes,
//   - Roads::scan_and_kill_stray_roads @8956a0 (every frame, world.size / 500 wcoords) removes road bits without a consistent
//     candidate and dead-end stubs.
// What is NOT ported: the presentation half (mark_white_lines / mark_yellow_lines / mark_intersections / fixup_lines /
// delete_straglers / find_piece / RoadsPieces::find_map choose the art piece from the low 24 bits of a candidate's flags and set its
// element_num / rotation); those bits are carried over from a loaded save and otherwise stay 0, element_num only marks the
// diagonal-piece candidates (the element "C4" of Data/roads.xml). See docs/sim-implementation.md, "Fidelity log, tenth pass".
#pragma once

#include <cstdint>

namespace ron::sim {

class World;

// World::set_road_at(.., on, quiet = false) calls these after the tile grid changed (Roads::road_added @8954d0 / road_cleared @8955d0:
// both queue a modification and run Roads::add_roads @88f4b0, which processes the queue, the changed lists and set_diags).
void roads_road_added(World& w, int32_t tx, int32_t ty);
void roads_road_cleared(World& w, int32_t tx, int32_t ty);

// Roads::scan_and_kill_stray_roads @8956a0 for one frame (Game::do_frame step 21).
void roads_scan_and_kill_stray(World& w);

// TerrainOut::caravan_step @875700 (from Unit::do_move on a caravan's road point): the tile's candidate and its eight neighbours that
// have a free reference (ref_b != 0) move one reference (ref_a++, ref_b--) and record the player's ally mask in their byte 14.
void roads_caravan_step(World& w, int32_t tx, int32_t ty, uint8_t ally_mask);

}  // namespace ron::sim
