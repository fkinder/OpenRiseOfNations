// Per-frame unit movement (docs/movement-spec.md sections 3-5, 9.1, 10): Unit::do_move (the move-order handler),
// find_path (the straight-line check with building detours), go_around_building, move_step (one frame of motion),
// unit speeds, and the unit-collision layer (CollCheck queries, detect / resolve unit collision).
#pragma once

#include <cstdint>

#include "sim/objects.hpp"
#include "sim/pathfinder.hpp"

namespace ron::sim {

class Game;

// ---- movement.cpp
// Unit::do_move @5f7b30 (MOVE_TO, FLEE_TO). `speed` >= 0 replaces step 10's speed (the KEEP formation's members).
int32_t do_move(Game& g, Unit& u, Order& m, int32_t speed = -1);
int32_t do_attack_to(Game& g, Unit& u, Order& m);      // Unit::do_attack_to @5f2320 (combat scan stubbed)
int32_t do_explore_to(Game& g, Unit& u, Order& m);     // Unit::do_explore_to @5f24a0 (goody scan stubbed)
int32_t do_flee_to(Game& g, Unit& u, Order& m);        // Unit::do_flee_to @5f2480
int32_t find_path(Game& g, Unit& u, int32_t dx, int32_t dy);  // Unit::find_path @5fb910: 0 clear, 1 blocked, 2 nested
// Unit::go_around_building @5fc350. Returns 0 on failure.
int32_t go_around_building(Game& g, Unit& u, int32_t x, int32_t y, uint32_t angle, int32_t step);
int32_t move_step(Game& g, Unit& u, Order& m, int32_t speed);  // Unit::move_step @5faf30

// Speeds (world units per frame).
void update_speed(Game& g, Unit& u);                    // Unit::update_speed @6055c0 (myspeed, copied down the chain)
int32_t unit_speed(const Game& g, const Unit& u);       // UnitData::speed @60aae0
int32_t get_speed(const Game& g, const Unit& u, int32_t x, int32_t y, bool nocap);  // UnitData::get_speed @608720

// ---- collision.cpp (docs/movement-spec.md section 10)
// CollCheck::collide_here @682540: the first set bit of other units' footprints under unit `u`'s footprint of
// radius r at ucoord (ux, uy) (own footprint excluded), returned in (*hx, *hy). With `pathing`, blocks come from
// / are copied into the current search's snapshot (g.pathfinder.blocklist).
int32_t collide_here(Game& g, const Unit& u, int32_t ux, int32_t uy, int32_t r, int32_t* hx, int32_t* hy, bool pathing);
// CollCheck::move_unit @682ad0: update a guy footprint of radius r from ucoord old to new.
void coll_move_unit(Game& g, int32_t old_ux, int32_t old_uy, int32_t new_ux, int32_t new_uy, int32_t r);
// UnitData::will_be_corner @609fa0, is_corner @60a040, is_here @60a0c0.
int32_t will_be_corner(const Game& g, const Unit& u, int32_t hx, int32_t hy, int32_t ux, int32_t uy);
int32_t is_corner(const Game& g, const Unit& u, int32_t hx, int32_t hy);
bool is_here(const Game& g, const Unit& u, int32_t hx, int32_t hy);
// Unit::detect_unit_collision @617060.
int32_t detect_unit_collision(Game& g, Unit& u, int32_t x, int32_t y, bool test_only, bool allow_push, int32_t p5,
                              bool pathing, bool no_push);
void resolve_unit_collision(Game& g, Unit& u, int32_t x, int32_t y);  // Unit::resolve_unit_collision @5f9d30
int32_t resolve_block(Game& g, Unit& u);                                // Unit::resolve_block @5fccc0
// Unit::detect_boat_collision @5fa8b0 (pushers). Returns 1 when everything was pushed.
int32_t detect_boat_collision(Game& g, Unit& u, int32_t x, int32_t y, bool mutual);

}  // namespace ron::sim
