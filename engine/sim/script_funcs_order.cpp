// The order builtins of ScenarioFuncSet (docs/script-spec.md 7.2 "orders"): one-unit orders (a one-member Group
// pushed with force, then Group::action_*) and the three group families: (int who) = the default group
// "_Unnamed<who>", (string name) = a ScenarioData named group, (UnitGroup g) = the script struct, which
// ScenarioFuncSet::validate_param @9e3410 turns into a pushed Group ("active_unit_group"). Orders are applied
// directly, as the original does (no command queue); ScenarioData::ignore_orders, which the bodies force to 0 around
// the action, is not modelled (groups.cpp TODO).
#include "script/bhs_stdlib.hpp"
#include "sim/aircraft.hpp"
#include "sim/heroes.hpp"
#include "sim/spells.hpp"
#include "sim/combat.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"
#include "sim/script_funcs_internal.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {
namespace sf {

namespace {

U16 unnamed(int32_t who) { return U16(u"_Unnamed") + bhs::itow_text(who); }

bool is_plane_unit(const Unit& u) { return u.ptype->domain == kDomainAir && !(u.ptype->unit_flags & utflag::kHelicopter); }

// Unit::set_group(n, 0) @605220: from the captain down the o_down chain while valid.
void set_group(Game& g, int32_t w, int32_t o, int32_t n) {
	Unit* u = unit_at(g, w, get_captain(g, w, o));
	for (int guard = 0; u && guard < kUnitLimit; guard++) {
		u->group = int16_t(n);
		if (u->o_down < 0) return;
		u = unit_at(g, w, u->o_down);
		if (!u || !(u->flags & objflag::kValid)) return;
	}
}

// ScenarioFuncSet::create_game_group @9e1dc0 / @9e1f40: the members' orders are dropped (planes excepted), the
// valid live units < 2000 added, and the group pushed (force). Returns the pushed Group (null when `who` is not live).
Group* create_game_group(Game& g, int32_t who, const std::vector<int32_t>& ids) {
	if (!live_leader(g, who)) return nullptr;
	Group tmp;
	group_clear(tmp, -1);
	tmp.id = -1;
	for (int32_t o : ids) {
		if (!live_leader(g, who) || uint32_t(o) >= uint32_t(kUnitLimit)) continue;
		Unit* u = unit_at(g, who, o);
		if (!u || !(u->flags & objflag::kValid)) continue;
		if (!is_plane_unit(*u)) clear_orders(g, *u);
		group_add(g, tmp, o, who, 0, 0);
	}
	const int32_t slot = groups_push_group(g, who, tmp, 1);
	return slot >= 0 ? &g.groups.g[size_t(slot)] : nullptr;
}

// The UnitGroup struct {int nation; int[] units} of an argument.
bool unit_group_arg(NativeCall& c, size_t k, int32_t* who, std::vector<int32_t>* ids) {
	bhs::Value* s = k < c.args.size() ? c.args[k].get() : nullptr;
	if (!s || s->elems.size() < 2 || !s->elems[0] || !s->elems[1]) return false;
	*who = bhs::value_int(s->elems[0].get()) - 1;
	ids->clear();
	for (const VP& v : s->elems[1]->elems) ids->push_back(v ? bhs::value_int(v.get()) : -1);
	return true;
}

// Unit::dbg_jump_to_action @602ea0: puts the unit where its current action leads (garrison / gather targets, the end
// of a chain of move-family orders), as Group::dbg_jump_to_action @707a00 does for every valid on-map member in single
// player only.
void unit_dbg_jump(Game& g, Unit& u) {
	const Order* a = get_action(u);
	if (a && a->type == kGarrison) {
		const int32_t ox = a->ox, whom = a->whom;
		unit_insert_inside(g, u, ox, whom);
		repath(g, u);
		kill_current_order(g, u, false);
		return;
	}
	if (a && a->type == kGather) {
		const Object* t = g.object(a->whom, a->ox);
		if (t) unit_set_new_location(g, u, t->x, t->y, true, true);
		return;
	}
	const OrderType ot = u.order_type();
	if (!(ot == kMoveTo || ot == kAttackTo || ot == kExploreTo || ot == kFleeTo || ot == kChangeForm || ot == kGroupMove ||
	      ot == kGroupAttackTo))
		return;
	int32_t x = u.x, y = u.y;  // (uninitialised in the original when the loop takes nothing; it always takes one here)
	while (!u.orders.empty()) {
		const Order& f = u.orders.front();
		const OrderType t = f.type;
		if (t != kMoveTo && t != kAttackTo && t != kExploreTo && t != kFleeTo && t != kChangeForm && t != kGroupMove &&
		    t != kGroupAttackTo)
			break;
		x = f.x;
		y = f.y;
		unit_set_angle(g, u, f.angle, true);
		kill_current_order(g, u, false);
	}
	if (unit_invalid_loc(g, u, tile_of(x), tile_of(y), false, false, false, false, false)) {
		int32_t ox = x, oy = y;
		if (find_nearby_spot(g, *u.ptype, x, y, &ox, &oy, 0xc0, 0x600, 0, 0x55555555u, kFilterNotMe, u.o, u.who, 0, 0, -1,
		                     0, -1)) {
			unit_set_new_location(g, u, u.x, u.y, true, true);
			return;
		}
		x = ox;
		y = oy;
	}
	unit_set_new_location(g, u, x, y, true, true);
}

void group_dbg_jump(Game& g, Group& gr) {
	if (mp(g)) return;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* u = unit_at(g, gr.who, gr.list[size_t(i)]);
		if (u && (u->flags & objflag::kValid) && u->on_map()) unit_dbg_jump(g, *u);
	}
}

}  // namespace

// Exposed for add_unit (script_funcs_mutate.cpp).
void group_dbg_jump_to_action(Game& g, Group& gr) { group_dbg_jump(g, gr); }

void install_order_funcs(ScriptHost& h) {
	Game& g = h.game;
	bhs::Runtime& rt = h.rt;
	auto def = [&rt](const char* name, Fn f, int overload = 0) { rt.set_native(name, std::move(f), overload); };

	// ---- the three group families. `act` gets the pushed Group and returns the builtin's value.
	using Act = std::function<VP(NativeCall&, Group&)>;
	auto families = [&g, def](const char* name, Act act) {
		def(name, [&g, act](NativeCall& c) {  // (int who): default_group(who) -> the named body
			const int32_t gi = g.scen.find_group(unnamed(c.arg_int(0)));
			if (gi < 0) return make_int(-1);
			ScenarioData::Group& sg = g.scen.groups[size_t(gi)];
			Group* gr = create_game_group(g, sg.who, sg.ids);
			return gr ? act(c, *gr) : make_int(-1);
		}, 0);
		def(name, [&g, act](NativeCall& c) {  // (string name)
			const int32_t gi = g.scen.find_group(c.arg_str(0));
			if (gi < 0) return make_int(-1);
			ScenarioData::Group& sg = g.scen.groups[size_t(gi)];
			Group* gr = create_game_group(g, sg.who, sg.ids);
			return gr ? act(c, *gr) : make_int(-1);
		}, 1);
		def(name, [&g, act](NativeCall& c) {  // (UnitGroup g): validate_param, REJECT -> the error value
			int32_t who;
			std::vector<int32_t> ids;
			if (!unit_group_arg(c, 0, &who, &ids)) return make_int(-1);
			Group* gr = create_game_group(g, who, ids);
			return gr ? act(c, *gr) : make_int(-1);
		}, 2);
	};
	auto move_family = [&g](int32_t orders) {  // group_move_order @9faf90 / group_attack_to_order @9fb140
		return [&g, orders](NativeCall& c, Group& gr) {
			const int32_t x = c.arg_int(1), y = c.arg_int(2);
			if (!tile_ok(g, x, y)) return make_int(-1);
			if (gr.num != 0)
				group_action_move_to(g, gr, tile_centre(x), tile_centre(y), kQueueNew, false, 0, orders, 1, -1, -1, 0);
			return make_int(gr.num);
		};
	};
	families("group_move_order", move_family(kMoveTo));
	families("group_attack_to_order", move_family(kAttackTo));
	families("group_stance_order", [&g](NativeCall& c, Group& gr) {  // @9fb560
		const int32_t s = stance_index(g, c.arg_str(1));
		if (s < 0) return make_int(-1);
		if (gr.num != 0) group_action_stance(g, gr, s);
		return make_int(gr.num);
	});
	families("group_jump_move", [&g](NativeCall& c, Group& gr) {  // @9fb2d0: move with angle degrees_to_angle(1)
		const int32_t x = c.arg_int(1), y = c.arg_int(2);
		if (!tile_ok(g, x, y)) return make_int(-1);
		if (gr.num != 0) {
			const uint32_t ang = uint32_t((uint64_t(1) << 32) / 360u);  // degrees_to_angle(1)
			group_action_move_to(g, gr, tile_centre(x), tile_centre(y), kQueueNew, true, ang, kMoveTo, 1, -1, -1, 0);
			group_dbg_jump(g, gr);
		}
		return make_int(gr.num);
	});
	families("group_attack_order", [&g](NativeCall& c, Group& gr) {  // @9fb400
		const int32_t tw = c.arg_int(1) - 1, to = c.arg_int(2);
		if (!valid_object(g, tw, to)) return make_int(-1);
		if (gr.num != 0) {
			if (Build* tb = build_at(g, tw, to)) tb->ever_seen = uint8_t(tb->ever_seen | (1u << (gr.who & 31)));
			group_action_attack(g, gr, to, tw, 1, kQueueNew, 0);
		}
		return make_int(gr.num);
	});
	families("group_patrol_order", [&g, &h](NativeCall& c, Group& gr) {  // @9fb3f0
		const int32_t x = c.arg_int(1), y = c.arg_int(2);
		if (!tile_ok(g, x, y)) return make_int(-1);
		if (gr.num != 0) h.todo_calls["group_patrol_order: Group::action_patrol"]++;  // TODO(orders): PATROL
		return make_int(gr.num);
	});

	// ---- one-unit orders
	auto one = [&g](int32_t w, int32_t o) -> Group& {
		set_group(g, w, o, -1);
		return push_one(g, w, o);
	};
	def("unit_attack_order", [&g, one](NativeCall& c) {  // @9f79e0
		const int32_t w = c.arg_int(0) - 1, tw = c.arg_int(2) - 1, to = c.arg_int(3);
		int32_t o = c.arg_int(1);
		if (!active_unit_slot(g, w, o) || !valid_object(g, tw, to)) return make_int(-1);
		o = get_captain(g, w, o);
		Unit* u = unit_at(g, w, o);
		if (!is_plane_unit(*u)) clear_orders(g, *u);
		Group& gr = one(w, o);
		if (Build* tb = build_at(g, tw, to)) tb->ever_seen = uint8_t(tb->ever_seen | (1u << w));
		if (is_plane_unit(*u) || (uint32_t(u->ptype->obj_masks) & 0x8000000u)) {
			group_action_flight(g, gr, to, tw, kAttack, 0, 0, 0);
			return make_int(1);
		}
		group_action_attack(g, gr, to, tw, 1, kQueueNew, 0);
		return make_int(1);
	});
	auto move_one = [&g, one](NativeCall& c, int32_t orders, bool captain_of_slot) {
		// unit_flee_order @9f7650 (active slot, captain), unit_waypoint_order @9f7330 (same), unit_attack_to_order @9f7c40
		// (a valid unit itself): clear_orders, set_group(-1), QUEUE_LAST move_near.
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		const int32_t x = c.arg_int(2), y = c.arg_int(3);
		if (captain_of_slot ? !active_unit_slot(g, w, o) : !valid_unit(g, w, o)) return make_int(-1);
		if (!tile_ok(g, x, y)) return make_int(-1);
		if (captain_of_slot) o = get_captain(g, w, o);
		clear_orders(g, *unit_at(g, w, o));
		Group& gr = one(w, o);
		group_action_move_near(g, gr, tile_centre(x), tile_centre(y), 0, kQueueLast, false, 0, orders, 1, -1, -1, 0);
		return make_int(1);
	};
	def("unit_flee_order", [move_one](NativeCall& c) { return move_one(c, kFleeTo, true); });
	def("unit_waypoint_order", [move_one](NativeCall& c) { return move_one(c, kMoveTo, true); });
	def("unit_attack_to_order", [move_one](NativeCall& c) { return move_one(c, kAttackTo, false); });
	auto ground_one = [&g, &h, one](NativeCall& c, const char* what) {
		// unit_attack_ground_order @9f7db0 / unit_patrol_order @9f7fd0: a valid unit, the tile, orders dropped (not
		// planes), set_group(-1), pushed; then Group::action_attack_ground / action_patrol (or the air variants).
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		const int32_t x = c.arg_int(2), y = c.arg_int(3);
		Unit* u = valid_unit(g, w, o);
		if (!u || x < 0 || y < 0 || x >= g.world.tile_xs || y >= g.world.tile_ys) return make_int(-1);
		if (!is_plane_unit(*u)) clear_orders(g, *u);
		one(w, o);
		h.todo_calls[what]++;  // TODO(orders): ATTACK_GROUND / PATROL actions (and the air variants) are not ported
		return make_int(1);
	};
	def("unit_attack_ground_order", [ground_one](NativeCall& c) {
		return ground_one(c, "unit_attack_ground_order: Group::action_attack_ground");
	});
	def("unit_patrol_order", [ground_one](NativeCall& c) { return ground_one(c, "unit_patrol_order: Group::action_patrol"); });
	def("unit_clear_orders", [&g](NativeCall& c) {  // @9f77e0
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!active_unit_slot(g, w, o) || !valid_unit(g, w, o)) return make_int(-1);
		set_group(g, w, o, -1);
		Unit* u = unit_at(g, w, o);
		clear_orders(g, *u);
		if (is_plane_unit(*u)) add_strafe_order(g, *u, -1, -1, -1, -1, 1, kQueueNew, false);  // back to its nearest base
		return make_int(1);
	});
	def("unit_stance_order", [&g](NativeCall& c) {  // @9f9e50 (o < 0: every captain of the leader)
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		const int32_t s = stance_index(g, c.arg_str(2));
		if (o < 0 && live_leader(g, w) && s >= 0) {
			int32_t n = 0;
			Group tmp;
			group_clear(tmp, -1);
			tmp.id = -1;
			for (int32_t k = 0; k < g.objects[size_t(w)].unit_mark; k++) {
				const Unit* u = unit_at(g, w, k);
				if (!u || u->o_up >= 0) continue;
				// The same local group accumulates every captain (pushed and stanced each time).
				group_add(g, tmp, k, w, 0, 0);
				const int32_t slot = groups_push_group(g, w, tmp, 1);
				group_action_stance(g, g.groups.g[size_t(slot)], s);
				n++;
			}
			return make_int(n);
		}
		if (!active_unit_slot(g, w, o) || s < 0) return make_int(-1);
		o = get_captain(g, w, o);
		Unit* u = unit_at(g, w, o);
		clear_orders(g, *u);
		u->group = -1;
		Group& gr = push_one(g, w, o);
		group_action_stance(g, gr, s);
		return make_int(1);
	});
	// ScenarioFuncSet::cast_spell @9e2140 (general_forced_march @9f8bc0 / general_entrench @9f8ac0): a valid unit
	// of a valid leader, a spell type the leader has and SpellTypeData::is_castable == 3, then Unit::add_cast_order.
	// (the wrappers pass ox = whom = x = y = -1; the craft and the unit travel in registers the decompile lost: the
	// craft is the wrapper's own (0x27c / 0x27d), the unit its second script argument. ❓ heroes-spec 4)
	auto cast = [&g](NativeCall& c, int32_t spell) {
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w) || !valid_unit(g, w, c.arg_int(1))) return make_int(-1);
		return make_int(scenario_cast_spell(g, w, c.arg_int(1), -1, -1, -1, -1, spell));
	};
	def("general_forced_march", [cast](NativeCall& c) { return cast(c, 0x27c); });
	def("general_entrench", [cast](NativeCall& c) { return cast(c, 0x27d); });
	def("unit_stop_effect", [&g](NativeCall& c) {  // @9f7930: a hero's running spell effects stop
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!active_unit_slot(g, w, o) || !valid_unit(g, w, o)) return make_int(-1);
		const Unit* u = unit_at(g, w, o);
		if (!(u->ptype->unit_flags2 & utflag2::kHero)) return make_int(0);  // UnitData::is_hero (+0x2b8 & 0x20)
		Leader& L = g.leaders[size_t(w)];
		if (u->cara >= 0 && size_t(u->cara) < L.heroes.size()) caster_process_spells(g, L.heroes[size_t(u->cara)], o, w, true);
		return make_int(1);
	});
	def("air_unit_transfer_order", [&g](NativeCall& c) {  // @9f8720
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1), bo = c.arg_int(2);
		if (!valid_unit(g, w, o) || !valid_build(g, w, bo)) return make_int(-1);
		const Object* B = g.object(w, bo);
		if (!can_carry_air(*B)) return make_int(-1);
		if (num_aircraft_here(g, *B, 0) >= num_aircraft_limit(g, *B)) return make_int(0);
		Unit* u = unit_at(g, w, o);
		set_group(g, w, o, -1);
		clear_orders(g, *u);
		add_strafe_order(g, *u, -1, -1, -1, -1, 1, kQueueNew, false);
		Group tmp;
		group_clear(tmp, -1);
		tmp.id = -1;
		group_add(g, tmp, o, w, 0, 0);
		const int32_t slot = groups_push_group(g, w, tmp, 1);
		group_action_flight(g, g.groups.g[size_t(slot)], bo, w, kMoveTo, 0, 0, 0);  // (ignore_orders is off for the call)
		return make_int(1);
	});
	// create_missile_and_target_build @9f4fd0 is in script_funcs_mutate.cpp (it needs add_unit).
	def("get_conquest_invasion_dir", [&g, &h](NativeCall&) {  // @9e5ef0: CTW only
		if (!g.ctw) return make_int(-1);
		h.todo_calls["get_conquest_invasion_dir: ConquestGame"]++;  // TODO(ctw): the strategic map
		return make_int(-1);
	});

	auto garrison = [&g, one](int32_t w, int32_t o, int32_t bw, int32_t bo) {  // unit_garrison_order @9f8380
		if (!valid_unit(g, w, o) || !valid_build(g, bw, bo) || !live_leader(g, w) || !live_leader(g, bw)) return -1;
		// A group of the one unit, GARRISON with QUEUE_NEW and find 0; the builtin returns -1 (garrison-spec 5).
		Group& gr = one(w, o);
		group_action_garrison(g, gr, bo, bw, kQueueNew, 0);
		return -1;
	};
	def("unit_garrison_order", [garrison](NativeCall& c) {
		return make_int(garrison(c.arg_int(0) - 1, c.arg_int(1), c.arg_int(2) - 1, c.arg_int(3)));
	});
	def("unit_garrison_order", [garrison](NativeCall& c) {  // @4ce730 (the unit's own leader)
		return make_int(garrison(c.arg_int(0) - 1, c.arg_int(1), c.arg_int(0) - 1, c.arg_int(2)));
	}, 1);
	// group_garrison_order: (int who; int building_o) / (int who; int building_who; int building_o) on the default
	// group "_Unnamed<who>", (string name; int who; int object_o), (UnitGroup g; int who; int object_o). The
	// building's leader is the group's own for the first form.
	auto group_garrison = [&g](Group& gr, int32_t bw, int32_t bo) {
		if (!valid_build(g, bw, bo)) return make_int(-1);
		if (gr.num != 0) group_action_garrison(g, gr, bo, bw, kQueueNew, 0);
		return make_int(-1);
	};
	def("group_garrison_order", [&g, group_garrison](NativeCall& c) {  // (int who; int building_o)
		const int32_t gi = g.scen.find_group(unnamed(c.arg_int(0)));
		if (gi < 0) return make_int(-1);
		ScenarioData::Group& sg = g.scen.groups[size_t(gi)];
		Group* gr = create_game_group(g, sg.who, sg.ids);
		return gr ? group_garrison(*gr, sg.who, c.arg_int(1)) : make_int(-1);
	}, 0);
	def("group_garrison_order", [&g, group_garrison](NativeCall& c) {  // (int who; int building_who; int building_o)
		const int32_t gi = g.scen.find_group(unnamed(c.arg_int(0)));
		if (gi < 0) return make_int(-1);
		ScenarioData::Group& sg = g.scen.groups[size_t(gi)];
		Group* gr = create_game_group(g, sg.who, sg.ids);
		return gr ? group_garrison(*gr, c.arg_int(1) - 1, c.arg_int(2)) : make_int(-1);
	}, 1);
	def("group_garrison_order", [&g, group_garrison](NativeCall& c) {  // (string group_name; int who; int object_o)
		const int32_t gi = g.scen.find_group(c.arg_str(0));
		if (gi < 0) return make_int(-1);
		ScenarioData::Group& sg = g.scen.groups[size_t(gi)];
		Group* gr = create_game_group(g, sg.who, sg.ids);
		return gr ? group_garrison(*gr, c.arg_int(1) - 1, c.arg_int(2)) : make_int(-1);
	}, 2);
	def("group_garrison_order", [&g, group_garrison](NativeCall& c) {  // (UnitGroup g; int who; int object_o)
		int32_t who;
		std::vector<int32_t> ids;
		if (!unit_group_arg(c, 0, &who, &ids)) return make_int(-1);
		Group* gr = create_game_group(g, who, ids);
		return gr ? group_garrison(*gr, c.arg_int(1) - 1, c.arg_int(2)) : make_int(-1);
	}, 3);
	def("unit_eject_order", [&g, &h](NativeCall& c) {  // @9f84e0: a unit inside a unit issues COME_OUT (a command)
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		const Unit* u = valid_unit(g, w, o);
		if (!live_leader(g, w) || !u || u->inside_up < 0) return make_int(-1);
		const Object* in = object_at(g, u->inside_up_who, u->inside_up);
		if (!in || in->is_unit) return make_int(-1);
		h.todo_calls["unit_eject_order: UnitOut::issue_come_out"]++;  // TODO(commands): the COME_OUT command
		return make_int(1);
	});
	auto ctw_link = [&g](NativeCall&) {  // get_attack_to @9e6100 / get_attack_from: CTW only ("" otherwise)
		if (!g.ctw) return make_string(U16());
		return make_string(U16());  // TODO(ctw): ConquestGame links
	};
	def("get_attack_to", ctw_link);
	def("get_attack_from", ctw_link);

	def("general_is_forced_marching", [&g](NativeCall& c) {  // @9f8d50: unit_masks & MARCH_RUNNING (raw)
		const Unit* u = valid_unit(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!u) return make_int(-1);
		return make_int(int32_t(u->unit_masks & umask::kMarchRunning));
	});
}

}  // namespace sf
}  // namespace ron::sim
