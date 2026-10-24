// Gaia animals: Animal::process / work / do_idle and the herds (docs/randommap-resources.md section 8, docs/fog-spec.md 6.5).
#pragma once

#include <cstdint>

namespace ron::sim {

class Game;
struct Unit;
struct Order;

// The leader slots whose objects are `Animal`s (gaia: slot 8 in the original's games, slot 9 for the ambient birds).
inline bool is_animal_who(int32_t who) { return who >= 8; }

// Animal::process @5d72c0: Animal::work, then the guys. (No Unit::process: no countdowns, healing, attrition.)
void animal_process(Game& g, Unit& u);
// Animal::work @5d7330, Animal::do_idle @5d7460.
void animal_work(Game& g, Unit& u);
void animal_do_idle(Game& g, Unit& u);
// Animal::think_bird @5d79e0 (vslot +0x180, the first call of Unit::do_air_patrol for gaia birds): every 8 frames the eagle's patrol
// point jitters inside its region, and from time to time it moves to a random cell of the region (game_random draws).
void animal_think_bird(Game& g, Unit& u, Order& patrol);
// Herds::process @741cc0 (called every frame by Objects::process_all; acts when frame & 63 == 0) and Herd::process @741760.
void herds_process(Game& g);

}  // namespace ron::sim
