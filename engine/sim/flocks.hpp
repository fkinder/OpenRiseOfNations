// Bird flocks (Objects::add_flock @65c0e0, docs/aircraft-spec.md section 13): the gaia FLOCKBIRDs that burst out of a forest
// when a group walks onto it (Unit::do_group_move) or out of a fort / temple / town the first time a siege weapon hits it
// (Object::take_damage). Each bird flies an AIR_ATTACK_GROUND chain of four scattered waypoints and dies on landing at the last.
#pragma once

#include <cstdint>

namespace ron::sim {

class Game;

// Objects::add_flock(tx, ty, who, n): `n` flock birds start at tile (tx, ty) (TCoords). A leader `who` >= 0 that is in the game
// gets at most one flock per 225 frames (LeaderData::flock_stamp, +0x7b8); gaia (-1) always does. Draws game_random: two for the
// first waypoint, two for the second, one for the circle index, one for the first heading, then per bird one heading step (after
// init_unit's own draws) and two per waypoint.
void add_flock(Game& g, int32_t tx, int32_t ty, int32_t who, int32_t n);

// The type of the flock bird (TypeIndex FLOCKBIRD; the wild BIRD is 402).
constexpr int32_t kFlockBirdType = 403;

}  // namespace ron::sim
