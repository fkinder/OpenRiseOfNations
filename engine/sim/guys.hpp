// Guys (guy.obj; docs/movement-spec.md section 9): the figures of a unit — their slots around the unit, turning,
// per-frame motion, collision footprints (through CollCheck::move_unit), the animation clock — plus the unit-side
// calls that drive them (Unit::set_new_location, Unit::set_angle, Unit::set_anim) and terrain z.
//
// Integration contract (see guys.cpp for details):
// - Unit::init: for each guy i in [0, squad_size + crew_size): type, o, who set first (new guys cleared like
//   Guy::clear @5db590: x = y = -0x600, ...), then guy_init_real(keep = 0) (ONE game_random draw), then guy_num = i,
//   variation = 0; guy_mark = squad_size; then unit_set_new_location(x, y, true, true).
//   Unit::set_type re-inits with keep = 1.
// - Unit::process: guy_process for guys [0, guy_mark) and [squad_size, guys.size()) after Unit::work.
// - Objects::inc_time: unit_inc_time then unit_execute_events per alive unit.
#pragma once

#include <cstdint>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;

// ---- terrain z (TerrainOut; single-precision float, truncated with cvttss2si)
// TerrainOut::find_tcoord_z @8544a0 on a TCoord: water tiles 0; else trunc((H[tx,ty+1] + H[tx+1,ty]) * 0.5f);
// flag 1 turns a negative result into 0.
int32_t find_tcoord_z(const Game& g, int32_t tx, int32_t ty, int32_t flag);
// TerrainOut::find_tcoord_z @866710: the same at the tile of world coords (x, y).
int32_t find_tcoord_z_at(const Game& g, int32_t x, int32_t y, int32_t flag);
// TerrainOut::find_data_z @866560: bilinear-on-triangles interpolation at world coords; flag 1: water or negative -> 0.
int32_t find_data_z(const Game& g, int32_t x, int32_t y, int32_t flag);

// UnitAnimCat @af4370: the category of a UnitAnim (0 DEFAULT, 8 WALK, 10, 12 ATTACK2, 15 DEATH, others themselves).
int32_t unit_anim_cat(int32_t anim);

// ---- unit side
// Unit::set_new_location @5f8d20. Returns 0 when the unit did not move (boat transform etc.), else 1.
int32_t unit_set_new_location(Game& g, Unit& u, int32_t x, int32_t y, bool p3, bool p4);
// Unit::set_angle @605400 (the original's unused middle argument is dropped).
void unit_set_angle(Game& g, Unit& u, uint32_t a, bool now);
// Unit::set_anim @616f40 (all guys [0, guy_mark) and the crew). Callers in the original pass (anim, 0, 1).
void unit_set_anim(Game& g, Unit& u, int32_t anim, int32_t p2 = 0, int32_t p3 = 1);
// The guy slot layout of set_new_location (each guy's des_x/des_y around the unit); teleport = p3, set_angle = p4.
void unit_place_guys(Game& g, Unit& u, bool teleport, bool set_angle);
// Unit::inc_time @610b40 and Unit::execute_events @60edc0.
void unit_inc_time(Game& g, Unit& u);
void unit_execute_events(Game& g, Unit& u);

// ---- graphics pieces (docs/anim-spec.md section 2; the piece keys the animation data of g.anims)
// GraphicPieces::get_unit_gpiece @90c030 (immediate load, no age offset): unit type, owner, o (odd o: female),
// crew = the guy's slot, packed = UNIT_PACKED. -1 when g.anims is null.
int32_t unit_gpiece(const Game& g, int32_t type, int32_t who, int32_t o, int32_t crew, bool packed);
void guy_update_gpiece(Game& g, Unit& u, Guy& gy);  // Guy::update_gpiece @5d8530 (sets gy.gpiece)
void unit_update_gpiece(Game& g, Unit& u);          // Unit::update_gpiece @5e2920 (guys [0, guy_mark) and the crew)

// ---- Guy (the Unit passed is the guy's owner, units[gy.who][gy.o])
void guy_init_real(Game& g, Unit& u, Guy& gy, int32_t keep);                       // Guy::init_real @5db6b0
uint32_t guy_turn_speed(const Game& g, const Unit& u, const Guy& gy, bool full);  // GuyData::turn_speed @5de340
int32_t guy_get_speed(const Game& g, const Unit& u, const Guy& gy);                // GuyData::get_speed @5de410
void guy_set_angle(Game& g, Unit& u, Guy& gy, uint32_t a, bool now);               // Guy::set_angle @5d9010
// Guy::do_turn @5d97a0 (p3 is unused by the original).
void guy_do_turn(Game& g, Unit& u, Guy& gy, uint32_t target, uint32_t new_angle, int32_t p3, bool anim);
// Guy::turn_towards @5d9720: returns the remaining angle (p2 is unused by the original).
uint32_t guy_turn_towards(Game& g, Unit& u, Guy& gy, uint32_t target, int32_t p2, bool anim);
// Guy::turn_angles @5d98c0 (detect_boat_collision): the heading after one turn step towards `target` into *out,
// returns the remainder; `full` is turn_speed's argument, `half` halves the rate.
uint32_t guy_turn_angles(const Game& g, const Unit& u, const Guy& gy, uint32_t target, uint32_t* out, bool full,
                         bool half);
void guy_set_new_location(Game& g, Unit& u, Guy& gy, int32_t x, int32_t y, bool now);  // @5d86f0
void guy_set_new_z(Guy& gy, int32_t z, bool now);                                        // Guy::set_new_z @5d86c0
void guy_update_z(Game& g, Unit& u, Guy& gy);                                            // Guy::update_z @5d9950
void guy_move(Game& g, Unit& u, Guy& gy);                                   // Guy::move @5d9240
void guy_process(Game& g, Unit& u, Guy& gy);                                // Guy::process @5e0230
// Guy::set_anim @5da300. Callers pass p2 = 0 except the group idle (p2 = 1 skips the "same category, still
// running" early-out); p3 = 1 lets DEFAULT pick idle variants and ATTACK2 pick attack variants.
void guy_set_anim(Game& g, Unit& u, Guy& gy, int32_t anim, int32_t p2 = 0, int32_t p3 = 1);

// Diagnostics only (apps/sav RON_FISH_TIMELINE): called right after the idle-variant draw of Guy::set_anim.
inline void (*g_idle_pick_hook)(Game&, Unit&) = nullptr;
void guy_inc_time(Game& g, Unit& u, Guy& gy);                               // Guy::inc_time @5d9e10

}  // namespace ron::sim
