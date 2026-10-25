// Helpers shared by the builtin implementation files (script_funcs*.cpp): the ScenarioFuncSet validity checks
// (valid_who, active_who, valid_object_o, active_unit_slot, ...), type / city / category lookups and the multiplayer
// guard. `w` is always a leader index (the scripts' player number minus one).
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "script/bhs_vm.hpp"
#include "sim/game.hpp"
#include "sim/objects.hpp"
#include "sim/script_host.hpp"
#include "sim/script_ui.hpp"

namespace ron::sim::sf {

using bhs::make_float;
using bhs::make_int;
using bhs::make_string;
using bhs::NativeCall;
using bhs::U16;
using bhs::VP;
using Fn = bhs::Native;

// ScenarioFuncSet::valid_who @9e33c0 (VALID) / active_who @9e33e0 (VALID and ACTIVE), on a leader index.
inline bool valid_leader(const Game& g, int32_t w) {
	return uint32_t(w) < 8 && (g.leaders[size_t(w)].flags & leaderflag::kValid);
}
inline bool live_leader(const Game& g, int32_t w) { return uint32_t(w) < 8 && (g.leaders[size_t(w)].flags & 3) == 3; }
// semaphore byte0 & 4.
inline bool mp(const Game& g) { return g.multiplayer; }
// semaphore byte1 & 0x10 or byte2 & 2 (Game::is_scenario).
inline bool scripted(const Game& g) { return g.scripted_rules || g.ctw; }

inline Unit* unit_at(Game& g, int32_t w, int32_t o) {
	return (uint32_t(w) < uint32_t(kNumLeaders) && o >= 0 && o < kUnitLimit) ? g.unit(w, o) : nullptr;
}
inline Build* build_at(Game& g, int32_t w, int32_t o) {
	return (uint32_t(w) < uint32_t(kNumLeaders) && o >= kBuildBase && o < kBuildLimit) ? g.build(w, o) : nullptr;
}
inline Object* object_at(Game& g, int32_t w, int32_t o) {
	return (uint32_t(w) < uint32_t(kNumLeaders) && o >= 0 && o < kBuildLimit) ? g.object(w, o) : nullptr;
}
inline bool captain(const Unit& u) { return u.o_up < 0; }
// UnitData::get_captain (+0xe4): up the o_up chain.
inline int32_t get_captain(Game& g, int32_t w, int32_t o) {
	for (int guard = 0; guard < kUnitLimit; guard++) {
		const Unit* u = unit_at(g, w, o);
		if (!u || u->o_up < 0) return o;
		o = u->o_up;
	}
	return o;
}
// ObjectData +0x18 is_unit then get_captain: buildings keep their o.
inline int32_t captain_of(Game& g, int32_t w, int32_t o) { return o < kUnitLimit ? get_captain(g, w, o) : o; }

// ScenarioFuncSet::valid_object_o @9e32a0: live leader, o < 3000, slot allocated and VALID.
inline Object* valid_object(Game& g, int32_t w, int32_t o) {
	if (!live_leader(g, w)) return nullptr;
	Object* ob = object_at(g, w, o);
	return ob && (ob->flags & objflag::kValid) ? ob : nullptr;
}
// valid_object_slot @9e3240 / valid_unit_slot @9e31e0: the slot exists (allocated).
inline bool valid_object_slot(Game& g, int32_t w, int32_t o) { return live_leader(g, w) && object_at(g, w, o); }
inline bool valid_unit_slot(Game& g, int32_t w, int32_t o) { return live_leader(g, w) && unit_at(g, w, o); }
// active_unit_slot @9e3160 / valid_unit_o @9e3360 / valid_build_o @9e3300.
inline bool active_unit_slot(Game& g, int32_t w, int32_t o) {
	if (!live_leader(g, w)) return false;
	const Unit* u = unit_at(g, w, o);
	return u && ((u->flags & objflag::kValid) || u->o_up >= 0);
}
inline Unit* valid_unit(Game& g, int32_t w, int32_t o) {
	if (!live_leader(g, w)) return nullptr;
	Unit* u = unit_at(g, w, o);
	return u && (u->flags & objflag::kValid) ? u : nullptr;
}
inline Build* valid_build(Game& g, int32_t w, int32_t o) {
	if (!live_leader(g, w)) return nullptr;
	Build* b = build_at(g, w, o);
	return b && (b->flags & objflag::kValid) ? b : nullptr;
}

// Type lookups: ScenarioFuncSet::get_type_index @a03480 and the inlined loops (TypeData name +0x60, the first of the
// 806 types, case-insensitive; "" or unknown: -1).
int32_t type_index(const Game& g, const U16& name);
// The same search on the type's type name (+0xb0, get_type_index with the alternate flag set by the *_with_type_name builtins).
int32_t type_index_alt(const Game& g, const U16& name);
// get_unit_cat @9e3060: index in rules unit_cats by name (case-insensitive), -1.
int32_t unit_cat_index(const Game& g, const U16& name);
// get_stance @9e2c40: the stance index in stances_combat, then worker, caster, packer (first list that has it), -1.
int32_t stance_index(const Game& g, const U16& name);
// Tribes::find: tribe index by name (case-insensitive), -1.
int32_t tribe_index(const Game& g, const U16& name);

// get_city_index @9e2ba0: leader w's valid city (0 .. city_mark) with that name, -1.
int32_t city_index(const Game& g, int32_t w, const U16& name);

// Tile helpers (world tile size checks of the bodies; tile centre t * 192 + 96).
inline bool tile_ok(const Game& g, int32_t x, int32_t y) {
	return x >= 0 && y >= 0 && x < g.world.tile_xs && y < g.world.tile_ys;
}
inline int32_t tile_centre(int32_t t) { return t * 192 + 96; }
inline int32_t tile_of(int32_t c) { return (c >> 6) / 3; }  // div_3_table[c >> 6]

// Unit counts by type (LeaderData +0x56fe + 2t: num_units for 50..401; 402..413 read into num_queued).
inline int32_t unit_count(const Leader& l, int32_t t) {
	if (t < kEndUnits) return l.num_units[size_t(t - kUnits)];
	return l.num_queued[size_t(t - kEndUnits)];
}

// The presentation state and its event queue.
ScriptPresentation& pres(Game& g);
ScriptInput& input(Game& g);
ScriptEvent& push_event(Game& g, ScriptEvent::Kind k);

// The one-object group of the *_order / train / research builtins: Group::clear(-1), add, Groups::push_group(force).
Group& push_one(Game& g, int32_t w, int32_t o);

// Group::dbg_jump_to_action @707a00 (single player: the members jump to where their orders lead).
void group_dbg_jump_to_action(Game& g, Group& gr);

// Installers (one per file).
void install_query_funcs(ScriptHost& h);
void install_mutation_funcs(ScriptHost& h);
void install_ui_funcs(ScriptHost& h);
void install_order_funcs(ScriptHost& h);
void install_ctw_funcs(ScriptHost& h);
void install_more_funcs(ScriptHost& h);

}  // namespace ron::sim::sf
