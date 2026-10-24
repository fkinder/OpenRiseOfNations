// The query builtins of ScenarioFuncSet the stock scenarios and tutorials poll (docs/script-spec.md 7.3 category a):
// counts, object state, proximity, cities, diplomacy and the scenario tallies. Each body follows its decompile
// (re/decomp/script_funcs, disassembly in re/decomp/scen_builtins/asm; gitignored). `w` is the leader index.
#include <cmath>

#include "script/bhs_stdlib.hpp"
#include "sim/aircraft.hpp"
#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/compass.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/orders.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"
#include "sim/script_funcs_internal.hpp"
#include "sim/trig.hpp"

namespace ron::sim {
namespace sf {

namespace {

// UnitData::total_damage(0, null) @609c00: the damage of the whole uber chain (from the captain), the members already
// dead counted at full hits, plus the sixteenths.
int32_t total_damage(Game& g, Unit& start, int32_t from_member, int32_t* frac_out) {
	Unit* u = &start;
	if (!from_member && u->o_up >= 0) {
		Unit* cap = unit_at(g, u->who, u->o_up);
		if (cap) return total_damage(g, *cap, 0, nullptr);
	}
	int32_t frac = 0;
	int32_t d = u->damage;
	if (u->o_down >= 0) {
		Unit* n = unit_at(g, u->who, u->o_down);
		if (n && (n->flags & objflag::kValid)) d += total_damage(g, *n, 1, &frac);
	}
	if (u->o_up >= 0) {
		if (frac_out) *frac_out = int32_t(u->damage_frac) + frac;
		return d;
	}
	const int32_t hits = obj_hits(g, *u);
	const int32_t uber = u->ptype->uber_size;
	const int32_t cur = unit_curr_uber_size(g, *u, true);
	if (uber != 0) d += ((uber - cur) * hits) / uber;
	const uint32_t f = uint32_t(frac + int32_t(u->damage_frac));
	if (int32_t(f) > 0xf) return d + int32_t(f >> 4);
	return d;
}

// ObjectData::num_inside(p) @646d50: the carried objects (inside_down chain); p != 0 counts each captain once, p == 0
// counts heroes as 1 and other units by their control cost (decoys 0).
int32_t num_inside(Game& g, const Object& ob, int32_t p) {
	if (ob.is_unit && !static_cast<const Unit&>(ob).on_map()) return 0;  // vfunc +0xbc is_on_map
	int32_t n = 0;
	int32_t co = ob.inside_down, cw = ob.inside_down_who;
	for (int guard = 0; co >= 0 && guard < 4 * kUnitLimit; guard++) {
		Object* c = object_at(g, cw, co);
		if (!c) break;
		const bool cap = !c->is_unit || static_cast<Unit*>(c)->o_up < 0;
		if (cap) {
			if (p != 0) {
				n++;
			} else if (c->is_unit) {
				const Unit& u = *static_cast<Unit*>(c);
				if (u.ptype->unit_flags2 & 0x4000000u) n++;  // is_gov_hero test of the decompile (+0x2b4 & 0x4000000)
				else n += (u.unit_masks & umask::kDecoy) ? 0 : u.ptype->control_cost;
			}
		}
		const int32_t nco = c->inside_down, ncw = c->inside_down_who;
		co = nco;
		cw = ncw;
	}
	return n;
}

// ScenarioFuncSet::find_objects @9e2a80 with Objects::find_units @65a620 / find_builds @65a120: the leader's objects
// near (x, y) (world corner of the tile, as the callers pass t * 192), radius dist * 192, of type t (< 0: any).
// Units first, then buildings, appended to `out` in the searches' order. Returns the count.
int32_t find_objects(Game& g, int32_t who, int32_t x, int32_t y, int32_t t, int32_t dist, std::vector<int32_t>& out) {
	out.clear();
	const int32_t r = (dist > 0 ? 0xc0 : 0x60) * dist;
	const int32_t filter = t >= 0 ? 1 : 0;  // FILTER_TYPE
	const int32_t ft = t >= 0 ? t : 0;
	const Circles& C = circles();
	const World& w = g.world;
	auto pass_unit = [&](int32_t cw, int32_t co, const Object& ob) {
		if (cw != who || !ob.is_unit || !(ob.flags & objflag::kValid)) return false;  // SEARCH_FRIENDLY, +8 valid
		if (!static_cast<const Unit&>(ob).on_map()) return false;                       // +0xbc
		return filter == 0 || search_valid_filter(g, cw, co, ft, 0, filter);
	};
	auto pass_build = [&](int32_t cw, int32_t co, const Object& ob) {
		if (cw != who || ob.is_unit || !(ob.flags & objflag::kValid)) return false;  // +0x10 valid building
		return filter == 0 || search_valid_filter(g, cw, co, ft, 0, filter);
	};
	auto walk_circle = [&](int32_t k, bool units) {
		const int32_t wx = (x >> 8) / 3, wy = (y >> 8) / 3;  // div_3_table[c >> 8]: the wcoord
		for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
			const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
			if (!w.in_w(cx, cy)) continue;
			const WData& d = w.w(cx, cy);
			int32_t co = d.down, cw = d.down_who;
			while (co >= 0) {
				const Object* ob = g.object(cw, co);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				if (cw < 8 && (units ? pass_unit(cw, co, *ob) : pass_build(cw, co, *ob))) out.push_back(co);
				co = no;
				cw = nw;
			}
		}
	};
	// Objects::find_units (param_11 = 1: leaders 0..7).
	const int32_t k = (r + 0x2ff) / 0x300;
	if (r >= 0 && k <= 64 && C.radius[size_t(k)] <= g.total_units) {
		walk_circle(k, true);
	} else if (live_leader(g, who) || valid_leader(g, who)) {
		const auto& list = g.objects[size_t(who)];
		for (int32_t o = 0; o < list.unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !pass_unit(who, o, *u)) continue;
			const int32_t d = vector_dist(u->x - x, u->y - y);
			const int32_t br = u->ptype->domain == 1 ? u->ptype->push_size : u->ptype->big_radius;
			if (r < 0 || d - br <= r) out.push_back(o);
		}
	}
	// Objects::find_builds (num_def_builds = 200).
	if (r >= 0 && k <= 64 && C.radius[size_t(k)] <= 200) {
		walk_circle(k, false);
	} else if (valid_leader(g, who)) {
		const auto& list = g.objects[size_t(who)];
		for (int32_t o = kBuildBase; o < list.build_mark; o++) {
			const Build* b = g.build(who, o);
			if (!b || !pass_build(who, o, *b)) continue;
			int32_t dx = b->x - x, dy = b->y - y;
			dx = dx < 0 ? -dx : dx;
			dy = dy < 0 ? -dy : dy;
			const int32_t ex = dx - b->ptype->x_size * 192, ey = dy - b->ptype->y_size * 192;
			dx = ex < 0 ? 0 : ex;
			dy = ey < 0 ? 0 : ey;
			if (r < 0 || vector_dist(dx, dy) <= r) out.push_back(o);
		}
	}
	return int32_t(out.size());
}

// The military / unit-category searches of find_military @9eb5a0 & co: from counter + 1 (0 when out of range),
// wrapping at unit_mark, the next live on-map captain passing `pred`; the counter is reset to 0 on a miss.
template <class P>
int32_t find_unit_counter(Game& g, int32_t w, int32_t& counter, P pred) {
	const int32_t mark = g.objects[size_t(w)].unit_mark;
	if (counter < 0 || mark <= counter) counter = 0;
	const int32_t start = counter;
	int32_t i = start + 1;
	for (;;) {
		const int32_t k = i < mark ? i : 0;
		const Unit* u = unit_at(g, w, k);
		if (u && (u->flags & objflag::kValid) && u->on_map() && pred(*u) && u->o_up < 0) {
			counter = k;
			return k;
		}
		i = k + 1;
		if (i == start + 1) {
			counter = 0;
			return -1;
		}
	}
}

bool is_military(const Unit& u) { return (u.ptype->role & role::kCombat) != 0; }  // UnitTypeData +0x2c8 & 0x10000

}  // namespace

void install_query_funcs(ScriptHost& h) {
	Game& g = h.game;
	bhs::Runtime& rt = h.rt;
	auto& fc = h.find_counters;
	auto def = [&rt](const char* name, Fn f, int overload = 0) { rt.set_native(name, std::move(f), overload); };

	// ---- leader counts
	def("num_military_units", [&g](NativeCall& c) {  // @9e9ef0: units whose category is not 5 (civilian)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int32_t t = kUnits; t < kUnits + 0x160; t++) {
			if (g.type(t).cat != 5) n += g.leaders[size_t(w)].num_units[size_t(t - kUnits)];
		}
		return make_int(n);
	});
	def("num_unit_category", [&g](NativeCall& c) {  // @9e9f70 ("" = every unit)
		const int32_t w = c.arg_int(0) - 1;
		const U16 name = c.arg_str(1);
		const int32_t cat = unit_cat_index(g, name);
		if (!valid_leader(g, w) || (cat < 0 && !name.empty())) return make_int(-1);
		int32_t n = 0;
		for (int32_t t = kUnits; t < kUnits + 0x160; t++) {
			if (g.type(t).cat == cat || name.empty()) n += g.leaders[size_t(w)].num_units[size_t(t - kUnits)];
		}
		return make_int(n);
	});
	def("num_units", [&g](NativeCall& c) {  // @9e9e70: every unit type count
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int32_t t = kUnits; t < kUnits + 0x160; t++) n += g.leaders[size_t(w)].num_units[size_t(t - kUnits)];
		return make_int(n);
	});
	def("num_buildings", [&g](NativeCall& c) {  // @9e9bf0: the 129 building counts
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int32_t k = 0; k < 129; k++) n += uint16_t(g.leaders[size_t(w)].num_buildings[size_t(k)]);
		return make_int(n);
	});
	def("num_units_killed", [&g](NativeCall& c) {  // @9e98e0 (LeaderData +0x80c)
		const int32_t w = c.arg_int(0) - 1;
		return make_int(valid_leader(g, w) ? g.leaders[size_t(w)].units_killed : -1);
	});
	def("num_units_lost", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(valid_leader(g, w) ? g.leaders[size_t(w)].units_lost : -1);
	});
	def("get_territory", [&g](NativeCall& c) {  // @9e8fe0: territory * 100 / world land size
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w) || g.world_total_land == 0) return make_int(-1);
		return make_int(g.leaders[size_t(w)].territory * 100 / g.world_total_land);
	});
	def("population_cap", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(live_leader(g, w) ? g.leaders[size_t(w)].pop_cap : -1);
	});
	def("time_sec", [&g](NativeCall&) { return make_int(g.tick); });
	def("time", [&g](NativeCall&) { return make_int(g.tick / 60); });
	def("world_x_size", [&g](NativeCall&) { return make_int(g.world.tile_xs); });
	def("world_y_size", [&g](NativeCall&) { return make_int(g.world.tile_ys); });
	def("get_map_size", [&g](NativeCall&) { return make_int(g.world.size << 2); });
	def("type_build_time", [&g](NativeCall& c) {  // @9ea760: TypeData::time(-1)
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0) return make_int(-1);
		return make_int(int32_t(type_time(g, t, -1)));
	});
	def("num_type_upgrade", [&g](NativeCall& c) {  // the current line member's count
		const int32_t w = c.arg_int(0) - 1;
		int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || !live_leader(g, w)) return make_int(-1);
		t = get_graft(g, w, current_upgrade(g, w, t));  // @9e94a0
		if (t < 0) return make_int(-1);
		const Leader& l = g.leaders[size_t(w)];
		if (is_unit_type(t)) return make_int(uint16_t(unit_count(l, t)));
		if (is_build_type(t)) return make_int(uint16_t(l.num_buildings[size_t(t - kBuilds)]));
		if (is_good_type(t)) return make_int(t < 6 ? l.bucket[size_t(t)] : 0);
		return make_int(-1);
	});

	// ---- objects
	def("object_health", [&g](NativeCall& c) {  // @9f5910: (int)((float)left / (float)hits * 100)
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		if (!valid_object(g, w, o)) return make_int(-1);
		o = captain_of(g, w, o);
		Object* ob = object_at(g, w, o);
		if (!ob) return make_int(-1);
		const int32_t hits = obj_hits(g, *ob);
		int32_t left;
		if (ob->is_unit) left = ob->myhits - total_damage(g, *static_cast<Unit*>(ob), 0, nullptr);
		else left = obj_hits_left(g, *ob);
		return make_int(int32_t((float(left) / float(hits)) * 100.0f));
	});
	def("building_destroyed", [&g](NativeCall& c) {  // @9f1d80: slot allocated and no longer VALID
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!live_leader(g, w)) return make_int(-1);
		const Object* ob = object_at(g, w, o);
		if (!ob) return make_int(-1);
		return make_int((ob->flags & objflag::kValid) ? 0 : 1);
	});
	def("unit_killed", [&g](NativeCall& c) {  // @9f29c0: not VALID and a captain
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!live_leader(g, w)) return make_int(-1);
		const Unit* u = unit_at(g, w, o);
		if (!u) return make_int(-1);
		return make_int(!(u->flags & objflag::kValid) && u->o_up < 0 ? 1 : 0);
	});
	def("unit_type_killed", [&g](NativeCall& c) {  // @9f2a40: read and clear the tally
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (!valid_leader(g, w) || t <= 0) return make_int(-1);
		if (t < kUnits || t >= kUnits + 352) return make_int(0);  // the original reads outside the unit rows
		int16_t& v = g.scen.units_killed[size_t(w)][size_t(t - kUnits)];
		const int32_t n = v;
		v = 0;
		return make_int(n);
	});
	def("unit_any_killed", [&g](NativeCall& c) {  // @9f2af0: every tally of the leader, cleared
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int16_t& v : g.scen.units_killed[size_t(w)]) {
			n += v;
			v = 0;
		}
		return make_int(n);
	});
	def("building_type_destroyed", [&g](NativeCall& c) {  // @9f1df0
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (!valid_leader(g, w) || t <= 0) return make_int(-1);
		if (t < kBuilds || t >= kBuilds + 129) return make_int(0);
		int16_t& v = g.scen.builds_destroyed[size_t(w)][size_t(t - kBuilds)];
		const int32_t n = v;
		v = 0;
		return make_int(n);
	});
	def("get_last_razed_building", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(valid_leader(g, w) ? g.scen.last_razed[size_t(w)] : -1);
	});
	def("get_last_unit_built", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(valid_leader(g, w) ? g.scen.last_unit_built_o[size_t(w)] : -1);
	});
	def("is_idle", [&g](NativeCall& c) {  // @9f9370: no current order and a captain
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!live_leader(g, w)) return make_int(-1);
		if (!valid_object(g, w, o)) return make_int(-1);
		const Unit* u = unit_at(g, w, o);
		if (!u) return make_int(0);
		return make_int(u->order_type() == kOrderNone && u->o_up < 0 ? 1 : 0);
	});
	auto order_test = [&g](NativeCall& c, auto pred, bool need_busy) {  // has_*_order @9f9490..
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!live_leader(g, w)) return make_int(-1);
		if (!valid_object(g, w, o)) return make_int(-1);
		const Unit* u = unit_at(g, w, o);
		if (!u) return make_int(0);
		if (need_busy && u->orders.empty()) return make_int(0);  // UnitData::is_idle
		const Order* f = u->front();                              // UnitData::get_order
		return make_int(f && pred(*f) ? 1 : 0);
	};
	def("has_gather_order", [order_test](NativeCall& c) { return order_test(c, [](const Order& o) { return o.type == kGather; }, true); });
	def("has_build_order", [order_test](NativeCall& c) { return order_test(c, [](const Order& o) { return o.type == kBuildAt; }, true); });
	def("has_repair_order", [order_test](NativeCall& c) { return order_test(c, [](const Order& o) { return o.type == kRepair; }, true); });
	def("has_move_order", [order_test](NativeCall& c) { return order_test(c, [](const Order& o) { return order_is_move(o.type); }, true); });
	def("has_attack_order", [order_test](NativeCall& c) { return order_test(c, [](const Order& o) { return order_is_attack(o.type); }, false); });
	def("has_target_order", [order_test](NativeCall& c) { return order_test(c, [](const Order& o) { return order_is_targeted(o.type); }, false); });
	def("has_patrol_order", [order_test](NativeCall& c) {  // UnitOrder::is_patrol: AIR_PATROL, GROUP_PATROL
		return order_test(c, [](const Order& o) { return o.type == kAirPatrol || o.type == kGroupPatrol; }, false);
	});
	def("has_cast_order", [&g](NativeCall& c) {  // @9f98d0: the current action is CAST_SPELL
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!live_leader(g, w)) return make_int(-1);
		if (!valid_object(g, w, o)) return make_int(-1);
		const Unit* u = unit_at(g, w, o);
		if (!u || u->orders.empty()) return make_int(0);
		const Order* a = get_action(*u);
		return make_int(a && a->type == kCastSpell ? 1 : 0);
	});
	def("is_entrenched", [&g](NativeCall& c) {  // @9f8c00: unit_masks & ENTRENCHED (the raw bit)
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		const Unit* u = valid_unit(g, w, o);
		if (!u) return make_int(-1);
		return make_int(int32_t(u->unit_masks & umask::kEntrenched));
	});
	def("is_decoy", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		const Unit* u = valid_unit(g, w, o);
		if (!u) return make_int(-1);
		return make_int((u->unit_masks & umask::kDecoy) ? 1 : 0);
	});
	def("is_on_transport", [&g](NativeCall& c) {  // @9f3060: 1 inside a ship, else -1 (planes -1)
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		const Unit* u = valid_unit(g, w, o);
		if (!u || unit_is_plane(*u)) return make_int(-1);
		if (u->inside_up < 0) return make_int(-1);
		const Object* in = object_at(g, u->inside_up_who, u->inside_up);  // ObjectData::in_a_ship
		return make_int(in && in->is_unit && in->ptype->domain == kDomainSea ? 1 : -1);
	});
	def("is_rally_set", [&g](NativeCall& c) {  // @9f1fa0: the building's gather point list is not empty
		const Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b) return make_int(-1);
		return make_int(b->gather.empty() ? 0 : 1);
	});
	auto rally = [&g](NativeCall& c, bool y) {  // get_rally_x @9f20c0 / _y @9f21a0: the first gather point
		const Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b || b->gather.empty()) return make_int(-1);
		int32_t v = y ? b->gather.front().y : b->gather.front().x;
		if (v < 0) v = y ? b->y : b->x;  // an object target: the building's own position
		return make_int(tile_of(v));
	};
	def("get_rally_x", [rally](NativeCall& c) { return rally(c, false); });
	def("get_rally_y", [rally](NativeCall& c) { return rally(c, true); });
	def("num_garrisoned_in", [&g](NativeCall& c) {  // @9f2280
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		Object* ob = valid_object(g, w, o);
		if (!ob) return make_int(-1);
		if (valid_build(g, w, o) && can_carry_air(*ob)) return make_int(num_aircraft_here(g, *ob, 0));  // airbase, silo, carrier
		return make_int(num_inside(g, *ob, 1));
	});
	def("building_full", [&g](NativeCall& c) {  // @9f2360: garrison limit <= num_inside(0)
		Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b) return make_int(-1);
		if (can_carry_air(*b)) return make_int(num_aircraft_limit(g, *b) <= num_aircraft_here(g, *b, 0) ? 1 : 0);
		const int32_t lim = get_garrison_limit(g, b->type, b->who);
		return make_int(lim <= num_inside(g, *b, 0) ? 1 : 0);
	});
	def("object_visible", [&g](NativeCall& c) {  // @9f0a60: is_seen(who) of the building / the captain
		const int32_t w = c.arg_int(0) - 1, whose = c.arg_int(1) - 1;
		int32_t o = c.arg_int(2);
		if (!valid_object(g, whose, o) || !live_leader(g, w)) return make_int(-1);
		o = captain_of(g, whose, o);
		const Object* ob = object_at(g, whose, o);
		return make_int(ob && object_is_seen(g, *ob, w, false) ? 1 : 0);
	});
	def("is_seen", [&g](NativeCall& c) {  // @9e49b0
		const int32_t w = c.arg_int(0) - 1;
		Object* ob = valid_object(g, c.arg_int(1) - 1, c.arg_int(2));
		if (!live_leader(g, w) || !ob) return make_int(-1);
		return make_int(object_is_seen(g, *ob, w, false) ? 1 : 0);
	});
	def("territory_object", [&g](NativeCall& c) {  // @9e4f50: the territory owner + 1 of the object's wcoord
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		if (!active_unit_slot(g, w, o) && !valid_build(g, w, o)) return make_int(-1);
		o = captain_of(g, w, o);
		const Object* ob = object_at(g, w, o);
		if (!ob) return make_int(-1);
		const int32_t tx = tile_of(ob->x), ty = tile_of(ob->y);
		if (!tile_ok(g, tx, ty)) return make_int(-1);
		return make_int(g.world.w(tx >> 2, ty >> 2).who + 1);
	});
	auto infiltrated = [&g](NativeCall& c, int32_t p) {  // @9f9180 / @9f91f0: ObjectData::is_infiltrated(p)
		const Object* ob = valid_object(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!ob) return make_int(-1);
		const uint8_t m = uint8_t(ob->infiltrated);
		return make_int(p != ob->who && m != 0 && (p < 0 || (m & (1u << (p & 31)))) ? 1 : 0);
	};
	def("is_infiltrated", [infiltrated](NativeCall& c) { return infiltrated(c, -1); });
	def("is_infiltrated_by", [infiltrated](NativeCall& c) { return infiltrated(c, c.arg_int(2) - 1); });

	// ---- proximity (tile coordinates)
	def("object_near", [&g](NativeCall& c) {  // @9f0c30: vector_dist(object tile - (x, y)) < r
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		const int32_t r = c.arg_int(4);
		if (!valid_object(g, w, o) || r <= 0) return make_int(-1);
		o = captain_of(g, w, o);
		const Object* ob = object_at(g, w, o);
		const int32_t dx = tile_of(ob->x) - c.arg_int(2), dy = tile_of(ob->y) - c.arg_int(3);
		return make_int(vector_dist(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) < r ? 1 : 0);
	});
	def("object_near_build", [&g](NativeCall& c) {  // @9f0d10: minus (x_size + y_size) / 2 of the building
		const int32_t w = c.arg_int(0) - 1, bw = c.arg_int(2) - 1;
		int32_t o = c.arg_int(1);
		const int32_t bo = c.arg_int(3), r = c.arg_int(4);
		if (!valid_object(g, w, o)) return make_int(-1);
		Build* b = valid_build(g, bw, bo);
		if (!b || r <= 0) return make_int(-1);
		o = captain_of(g, w, o);
		const Object* ob = object_at(g, w, o);
		const int32_t dx = tile_of(b->x) - tile_of(ob->x), dy = tile_of(b->y) - tile_of(ob->y);
		const int32_t d = vector_dist(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
		return make_int(d - ((b->ptype->y_size + b->ptype->x_size) >> 1) < r ? 1 : 0);
	});
	auto near_common = [&g](int32_t w, int32_t num, int32_t x, int32_t y, int32_t t, int32_t r) {
		// any_object_near @9f11b0 / object_type_near @9f0f10 / any_object_near_build @9f1280: ScenarioData's
		// involved objects = the leader's objects found; count when at least `num`, else cleared and 0.
		ScenarioData& sd = g.scen;
		sd.involved_objects.clear();
		sd.involved_who = w;
		const int32_t n = find_objects(g, w, x, y, t, r, sd.involved_objects);
		if (n != 0 && num <= int32_t(sd.involved_objects.size())) return n;
		sd.involved_objects.clear();
		return 0;
	};
	def("any_object_near", [&g, near_common](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		g.scen.involved_objects.clear();
		if (!live_leader(g, w) || c.arg_int(4) <= 0) return make_int(-1);
		return make_int(near_common(w, c.arg_int(1), c.arg_int(2) * 0xc0, c.arg_int(3) * 0xc0, -1, c.arg_int(4)));
	});
	def("object_type_near", [&g, near_common](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		g.scen.involved_objects.clear();
		if (t < 0 || !live_leader(g, w) || c.arg_int(5) <= 0) return make_int(-1);
		return make_int(near_common(w, c.arg_int(2), c.arg_int(3) * 0xc0, c.arg_int(4) * 0xc0, t, c.arg_int(5)));
	});
	def("any_object_near_build", [&g, near_common](NativeCall& c) {  // around the building's position (world)
		const int32_t w = c.arg_int(0) - 1;
		g.scen.involved_objects.clear();
		if (!live_leader(g, w)) return make_int(-1);
		Build* b = valid_build(g, c.arg_int(2) - 1, c.arg_int(3));
		if (!b || c.arg_int(4) <= 0) return make_int(-1);
		return make_int(near_common(w, c.arg_int(1), b->x, b->y, -1, c.arg_int(4)));
	});
	def("object_type_near_build", [&g, near_common](NativeCall& c) {  // (who, type, num, who_build, build_o, r)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		g.scen.involved_objects.clear();
		if (t < 0 || !live_leader(g, w)) return make_int(-1);
		Build* b = valid_build(g, c.arg_int(3) - 1, c.arg_int(4));
		if (!b || c.arg_int(5) <= 0) return make_int(-1);
		return make_int(near_common(w, c.arg_int(2), b->x, b->y, t, c.arg_int(5)));
	});

	// ---- searches (find_counters)
	def("find_military", [&g, &fc](NativeCall& c) {  // @9eb5a0 (counter 9)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].unit_mark : 0;
		if (fc[9] < 0 || mark <= fc[9]) fc[9] = 0;
		if (!live_leader(g, w)) return make_int(-1);
		return make_int(find_unit_counter(g, w, fc[9], is_military));
	});
	def("find_military_category", [&g, &fc](NativeCall& c) {  // @9eb990 (counter 1, shared with find_unit)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].unit_mark : 0;
		if (fc[1] < 0 || mark <= fc[1]) fc[1] = 0;
		const int32_t cat = unit_cat_index(g, c.arg_str(1));
		if (!live_leader(g, w) || cat < 0) return make_int(-1);
		return make_int(find_unit_counter(g, w, fc[1], [cat](const Unit& u) { return is_military(u) && u.ptype->cat == cat; }));
	});
	// find_all_unit @9ebfb0 (counter 0x11, ScenarioFuncSet::find_unit mode 0: any live captain) and find_inside_unit
	// @9ebee0 (counter 0x10, mode 2: carried by something).
	auto find_mode = [&g](NativeCall& c, int32_t& counter, int32_t mode) {
		const int32_t w = c.arg_int(0) - 1;
		if (counter < 0) counter = 0;
		int32_t t = -1;
		const U16 name = c.arg_str(1);
		if (!name.empty()) {
			t = type_index(g, name);
			if (t < 0 || !is_unit_type(t) || !live_leader(g, w)) return make_int(-1);
		}
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t mark = g.objects[size_t(w)].unit_mark;
		int32_t start = counter;
		if (start >= mark || start < 0) start = 0;
		int32_t i = start + 1;
		for (;;) {
			const int32_t k = i < mark ? i : 0;
			const Unit* u = unit_at(g, w, k);
			if (u && (u->flags & objflag::kValid) && u->o_up < 0 && (t < 0 || type_is(*u->ptype, t)) &&
			    (mode == 0 || !u->on_map())) {
				counter = k;
				return make_int(k);
			}
			i = k + 1;
			if (i == start + 1) return make_int(-1);
		}
	};
	def("find_all_unit", [find_mode, &fc](NativeCall& c) { return find_mode(c, fc[0x11], 0); });
	def("find_inside_unit", [find_mode, &fc](NativeCall& c) { return find_mode(c, fc[0x10], 2); });
	def("find_capital", [&g, &fc](NativeCall& c) {  // @9ecfe0 (counter 0xe over buildings)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].build_mark : 0;
		if (fc[0xe] < 0 || mark <= fc[0xe]) fc[0xe] = kBuildBase;
		if (!live_leader(g, w)) return make_string(U16());
		const int32_t start = fc[0xe] + 1;
		int32_t i = start;
		do {
			if (i < kBuildBase || mark <= i) i = kBuildBase;
			const Build* b = build_at(g, w, i);
			if (b && (b->flags & objflag::kValid) && (b->flags & objflag::kActive) && (b->flags & objflag::kCity) &&
			    b->city >= 0 && size_t(b->city) < g.cities[size_t(w)].size()) {
				const City& ct = g.cities[size_t(w)][size_t(b->city)];
				if (ct.city_flags & cityflag::kCapital) {
					fc[0xe] = i;
					return make_string(bhs::to_u16(ct.name));
				}
			}
			i++;
		} while (i != start);
		return make_string(U16());
	});
	def("find_city_owner", [&g](NativeCall& c) {  // @9ef790: owner + 1 of the named city (any valid leader)
		const U16 name = c.arg_str(0);
		for (int32_t w = 0; w < 8; w++) {
			if (!valid_leader(g, w)) continue;
			for (const City& ct : g.cities[size_t(w)]) {
				if (ct.valid() && bhs::ieq(bhs::to_u16(ct.name), name)) return make_int(ct.who + 1);
			}
		}
		return make_int(-1);
	});
	def("find_city_name", [&g, &fc](NativeCall& c) {  // @9efbd0: round robin over the cities (counter 0xc)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.leaders[size_t(w)].city_mark : 0;
		if (fc[0xc] < 0 || mark <= fc[0xc]) fc[0xc] = 0;
		if (!valid_leader(g, w) || mark == 0) return make_string(U16());
		const auto& list = g.cities[size_t(w)];
		const int32_t stop = fc[0xc] + 1;
		int32_t i = stop;
		for (;;) {
			fc[0xc] = i < mark ? i : 0;
			if (size_t(fc[0xc]) < list.size() && list[size_t(fc[0xc])].valid())
				return make_string(bhs::to_u16(list[size_t(fc[0xc])].name));
			i = fc[0xc] + 1;
			if (i == stop) {
				fc[0xc] = 0;
				return make_string(U16());
			}
		}
	});

	def("find_all_military", [&g, &fc](NativeCall& c) {  // @9eb860 (counter 0xb; any valid military captain)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].unit_mark : 0;
		if (fc[0xb] < 0 || mark <= fc[0xb]) fc[0xb] = 0;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t start = fc[0xb];
		int32_t i = start + 1;
		for (;;) {
			const int32_t k = i < mark ? i : 0;
			const Unit* u = unit_at(g, w, k);
			if (u && (u->flags & objflag::kValid) && is_military(*u) && u->o_up < 0) {
				fc[0xb] = k;
				return make_int(k);
			}
			i = k + 1;
			if (i == start + 1) {
				fc[0xb] = 0;
				return make_int(-1);
			}
		}
	});
	def("find_all_military_category", [&g, &fc](NativeCall& c) {  // @9ebcb0 (counter 3; inside units too)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].unit_mark : 0;
		if (fc[3] < 0 || mark <= fc[3]) fc[3] = 0;
		const int32_t cat = unit_cat_index(g, c.arg_str(1));
		if (!live_leader(g, w) || cat < 0) return make_int(-1);
		const int32_t start = fc[3];
		int32_t i = start + 1;
		for (;;) {
			const int32_t k = i < mark ? i : 0;
			const Unit* u = unit_at(g, w, k);
			if (u && (u->flags & objflag::kValid) && is_military(*u) && u->ptype->cat == cat && u->o_up < 0) {
				fc[3] = k;
				return make_int(k);
			}
			i = k + 1;
			if (i == start + 1) {
				fc[3] = 0;
				return make_int(-1);
			}
		}
	});
	def("find_idle_military", [&g, &fc](NativeCall& c) {  // @9eb150 (counter 6): on the map, no order, captain
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].unit_mark : 0;
		if (fc[6] < 0 || mark <= fc[6]) fc[6] = 0;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t start = fc[6];
		int32_t i = start + 1;
		do {
			const int32_t k = i < mark ? i : 0;
			const Unit* u = unit_at(g, w, k);
			if (u && (u->flags & objflag::kValid) && u->on_map() && is_military(*u) && u->orders.empty() && u->o_up < 0) {
				fc[6] = k;
				return make_int(k);
			}
			i = k + 1;
		} while (i != start + 1);
		fc[6] = 0;
		return make_int(-1);
	});
	def("find_military_build", [&g, &fc](NativeCall& c) {  // @9ec4c0 (counter 0x14)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].build_mark : 0;
		if (fc[0x14] < 0 || mark <= fc[0x14]) fc[0x14] = kBuildBase;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t start = fc[0x14];
		int32_t i = start + 1;
		do {
			if (mark <= i) i = kBuildBase;
			const Build* b = build_at(g, w, i);
			if (b && (b->flags & objflag::kValid)) {
				const Type& T = *b->ptype;
				// build_flags 0x2000 or 0x10, not a city type, not a gather building (0x40).
				if ((T.build_flags & 0x2010u) && !type_is(T, kBuilds) && !(T.build_flags & 0x40u)) {
					fc[0x14] = i;
					return make_int(i);
				}
			}
			i++;
		} while (i != start + 1);
		return make_int(-1);
	});
	def("num_military_buildings", [&g](NativeCall& c) {  // @9e9c60: building counts of category 1
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int32_t k = 0; k < 129; k++) {
			if (g.type(kBuilds + k).cat == 1) n += uint16_t(g.leaders[size_t(w)].num_buildings[size_t(k)]);
		}
		return make_int(n);
	});
	def("find_current_upgrade", [&g](NativeCall& c) {  // @9eaae0
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w) || c.arg_str(1).empty()) return make_string(U16());
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || (!is_unit_type(t) && !is_build_type(t))) return make_string(U16());
		const int32_t u = current_upgrade(g, w, t);
		if (u < 0) return make_string(U16());
		return make_string(g.type(u).name);
	});
	def("is_merchant_unpacked", [&g](NativeCall& c) {  // @9f2e90: merchants / fur trapper, not PACKED, rare >= 0
		const Unit* u = valid_unit(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!u) return make_int(-1);
		if (u->type != 0x3d && u->type != 0x3e && u->type != 400) return make_int(-1);
		return make_int(!(u->unit_masks & umask::kPacked) && u->rare >= 0 ? 1 : 0);
	});
	def("num_caravan_routes", [&g](NativeCall& c) {  // @9f3240: caravan records with a completed round trip
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		const auto& cv = g.caravans[size_t(w)];
		for (int32_t k = 0; k < g.leaders[size_t(w)].caras && size_t(k) < cv.size(); k++) n += (cv[size_t(k)].caravan_flags & 4) ? 1 : 0;
		return make_int(n);
	});
	def("get_spell_name", [&g](NativeCall& c) {  // @9f9bc0: the CAST_SPELL order's spell name
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_object(g, w, c.arg_int(1))) return make_string(U16());
		const Unit* u = unit_at(g, w, c.arg_int(1));
		if (!u || u->orders.empty() || u->orders.front().type != kCastSpell) return make_string(U16());
		return make_string(U16());  // TODO(spells): CastSpellOrder's spell type (+0x20) is not kept
	});
	def("get_ending_age", [&g](NativeCall&) { return make_int(g.info.ending_technology + 1); });
	def("get_time_limit", [&g](NativeCall&) {  // @9e5bf0
		if (g.info.victory != 4) return make_int(-1);
		if (g.info.time_limit < 8) {
			const CategoryList* cl = g.rules->category_list("time_limits");
			if (cl && size_t(g.info.time_limit) < cl->items.size()) return make_int(cl->items[g.info.time_limit].data[0]);
			return make_int(-1);
		}
		if (scripted(g)) return make_int(g.scen.custom_time_limit == 0 ? 300 : g.scen.custom_time_limit);
		return make_int(-1);
	});

	// ---- map and world (tile coordinates)
	def("map_is_land", [&g](NativeCall& c) {  // @9e4d90: tile medium != water
		const int32_t x = c.arg_int(0), y = c.arg_int(1);
		if (!tile_ok(g, x, y)) return make_int(-1);
		return make_int((g.world.t(x, y) & tmask::kMedium) != tmask::kWater ? 1 : 0);
	});
	def("map_is_passable", [&g](NativeCall& c) {  // @9e4de0: the wcoord has no mountain / forest / cliff
		const int32_t x = c.arg_int(0), y = c.arg_int(1);
		if (!tile_ok(g, x, y)) return make_int(-1);
		return make_int((g.world.w(x >> 2, y >> 2).flags & 0x70) == 0 ? 1 : 0);
	});
	def("map_is_buildable", [&g](NativeCall& c) {  // @9e4e50
		const int32_t x = c.arg_int(0), y = c.arg_int(1);
		if (!tile_ok(g, x, y)) return make_int(-1);
		const WData& d = g.world.w(x >> 2, y >> 2);
		if ((d.flags & 0x30) == 0 && !World::is_ocean(d) && (d.flags & 0x48) == 0) return make_int(1);
		return make_int(0);
	});
	def("territory_owner", [&g](NativeCall& c) {  // @9e4f00: WData.who + 1
		const int32_t x = c.arg_int(0), y = c.arg_int(1);
		if (!tile_ok(g, x, y)) return make_int(-1);
		return make_int(g.world.w(x >> 2, y >> 2).who + 1);
	});
	def("get_starting_age", [&g](NativeCall&) { return make_int(g.info.starting_technology + 1); });
	def("time_earlier_than", [&g](NativeCall& c) { return make_int(g.tick / 60 < c.arg_int(0) ? 1 : 0); });
	def("score", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(live_leader(g, w) ? g.leaders[size_t(w)].score : -1);
	});
	def("get_base_rate", [&g](NativeCall& c) {  // @9fbc20: base_rate / 16 (toward zero)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t k = type_index(g, c.arg_str(1));
		if (!live_leader(g, w) || uint32_t(k) >= 6) return make_int(-1);
		return make_int(g.leaders[size_t(w)].base_rate[size_t(k)] / 16);
	});
	def("gather_rate", [&g](NativeCall& c) {  // @9e90b0: the displayed income / 16
		const int32_t w = c.arg_int(0) - 1;
		const int32_t k = type_index(g, c.arg_str(1));
		if (!live_leader(g, w) || k < 0 || !is_good_type(k)) return make_int(-1);
		if (k >= 6) return make_int(0);  // (the original reads past the six goods)
		return make_int(g.leaders[size_t(w)].income[size_t(k)] / 16);
	});
	def("object_max_health", [&g](NativeCall& c) {  // @9f5f60: hits(0) of the object
		Object* ob = valid_object(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!ob) return make_int(-1);
		return make_int(obj_hits(g, *ob));
	});
	def("unit_category", [&g](NativeCall& c) {  // @9f3190: the unit_cats key of the unit's category
		const Unit* u = valid_unit(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!u) return make_string(U16());
		const CategoryList* cl = g.rules->category_list("unit_cats");
		const int32_t cat = u->ptype->cat;
		if (!cl || cat < 0 || size_t(cat) >= cl->items.size()) return make_string(U16());
		return make_string(cl->items[size_t(cat)].key);
	});
	def("get_leader_name", [&g](NativeCall& c) {  // @9e5850: LeaderData::get_name
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_string(U16());
		return make_string(bhs::to_u16(g.leaders[size_t(w)].name));
	});
	def("get_last_type_built", [&g](NativeCall& c) {  // @9e9910: read and reset the last finished object of the type
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || !valid_leader(g, w)) return make_int(-1);
		if (!is_unit_type(t) && !is_build_type(t)) return make_int(-1);
		Leader& l = g.leaders[size_t(w)];
		if (is_unit_type(t)) {
			if (t >= kEndUnits) return make_int(-1);
			const int32_t v = l.last_unit_finished[size_t(t - kUnits)];
			l.last_unit_finished[size_t(t - kUnits)] = -1;
			return make_int(v);
		}
		const int32_t v = l.last_building_finished[size_t(t - kBuilds)];
		l.last_building_finished[size_t(t - kBuilds)] = -1;
		return make_int(v);
	});
	def("is_garrisoned", [&g](NativeCall& c) {  // @9f2f20: carried by a building
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_object(g, w, c.arg_int(1))) return make_int(-1);
		const Unit* u = unit_at(g, w, c.arg_int(1));
		if (!u || u->inside_up < 0) return make_int(0);
		const Object* in = object_at(g, u->inside_up_who, u->inside_up);
		return make_int(in && !in->is_unit ? 1 : 0);
	});
	def("is_offering_peace", [&g](NativeCall& c) {  // @9fcb90: dip[askee].treaty == 1
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b) || a == b) return make_int(-1);
		return make_int(g.leaders[size_t(a)].dip[size_t(b)].treaty == 1 ? 1 : 0);
	});
	def("find_unit_upgrade", [&g, &fc](NativeCall& c) {  // @9ec080 (counter 0x12, the current upgrade, on the map)
		const int32_t w = c.arg_int(0) - 1;
		if (fc[0x12] < 0) fc[0x12] = 0;
		if (c.arg_str(1).empty()) return make_int(-1);
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || !is_unit_type(t) || !live_leader(g, w)) return make_int(-1);
		const int32_t u = current_upgrade(g, w, t);
		const int32_t mark = g.objects[size_t(w)].unit_mark;
		int32_t start = fc[0x12];
		if (start >= mark || start < 0) start = 0;
		int32_t i = start + 1;
		for (;;) {
			const int32_t k = i < mark ? i : 0;
			const Unit* v = unit_at(g, w, k);
			if (v && (v->flags & objflag::kValid) && v->o_up < 0 && (u < 0 || type_is(*v->ptype, u)) && v->on_map()) {
				fc[0x12] = k;
				return make_int(k);
			}
			i = k + 1;
			if (i == start + 1) return make_int(-1);
		}
	});
	def("city_assimilated_by", [&g](NativeCall& c) {  // @9ef250
		const int32_t d = c.arg_int(0) - 1, a = c.arg_int(1) - 1;
		if (!valid_leader(g, d) || !valid_leader(g, a)) return make_int(-1);
		const U16 name = c.arg_str(2);
		const auto& list = g.cities[size_t(a)];
		if (name.empty()) {
			for (const City& ct : list) {
				if (ct.valid() && ct.founder == d && !(ct.city_flags & cityflag::kGotCaptured)) return make_int(1);
			}
			return make_int(0);
		}
		// (The body looks the name up twice; a city found the first time answers 0.)
		const int32_t ci = city_index(g, a, name);
		if (ci < 0) return make_int(-1);
		return make_int(0);
	});

	// ---- cities
	def("city_captured", [&g](NativeCall& c) {  // @9eee50
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		const U16 name = c.arg_str(1);
		ScenarioData& sd = g.scen;
		if (!name.empty()) {
			if (city_index(g, w, name) >= 0) return make_int(0);
			sd.city_lost &= uint8_t(~(1u << w));
			return make_int(1);
		}
		if (sd.city_lost & (1u << w)) {
			sd.city_lost &= uint8_t(~(1u << w));
			return make_int(1);
		}
		// The first other live leader decides: one of its cities founded by / of the race of w.
		for (int32_t k = 0; k < 8; k++) {
			if ((g.leaders[size_t(k)].flags & 3) != 3 || k == w) continue;
			for (const City& ct : g.cities[size_t(k)]) {
				if (ct.valid() && (ct.race == w || ct.founder == w)) return make_int(1);
			}
			return make_int(0);
		}
		return make_int(-1);
	});
	def("city_captured_by", [&g](NativeCall& c) {  // @9ef040
		const int32_t def_w = c.arg_int(0) - 1, att = c.arg_int(1) - 1;
		if (!valid_leader(g, def_w) || !valid_leader(g, att)) return make_int(-1);
		const U16 name = c.arg_str(2);
		const auto& list = g.cities[size_t(att)];
		auto captured = [](const City& ct) { return (ct.city_flags & 0x101) == 0x101; };  // VALID | GOT_CAPTURED
		if (name.empty()) {
			for (const City& ct : list) {
				if (captured(ct) && (ct.who != ct.founder || ct.who != ct.race) && (ct.founder == def_w || ct.race == def_w))
					return make_int(1);
			}
			return make_int(0);
		}
		const int32_t ci = city_index(g, att, name);
		if (ci < 0) return make_int(-1);
		const City& ct = list[size_t(ci)];
		return make_int(captured(ct) && (ct.race == def_w || ct.founder == def_w) ? 1 : 0);
	});
	def("city_captured_by", [&g, &fc](NativeCall& c) {  // @9ef160: round robin (counter 0xd) over the attacker's cities
		const int32_t att = c.arg_int(1) - 1, def_w = c.arg_int(0) - 1;
		if (uint32_t(att) >= 8) return make_string(U16());
		const auto& list = g.cities[size_t(att)];
		const int32_t n = int32_t(list.size());
		if (n == 0) return make_string(U16());
		int32_t cur = fc[0xd];
		if (n <= cur || cur < 0) cur = 0;
		const int32_t stop = cur;
		for (;;) {
			const int32_t k = cur + 1 < n ? cur + 1 : 0;
			const City& ct = list[size_t(k)];
			if ((ct.city_flags & 0x101) == 0x101 && (ct.race == def_w || ct.founder == def_w)) {
				fc[0xd] = k;
				return make_string(bhs::to_u16(ct.name));
			}
			cur = k;
			if (k == stop) return make_string(U16());
		}
	}, 1);
	def("city_alarm_sounded", [&g](NativeCall& c) {  // @9ef430 (ALARM 0x40)
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		const U16 name = c.arg_str(1);
		if (name.empty()) {
			for (const City& ct : g.cities[size_t(w)]) {
				if ((ct.city_flags & 0x41) == 0x41) return make_int(1);
			}
			return make_int(0);
		}
		const int32_t ci = city_index(g, w, name);
		if (ci < 0) return make_int(-1);
		return make_int((g.cities[size_t(w)][size_t(ci)].city_flags >> 6) & 1);
	});
	def("city_being_assimilated", [&g](NativeCall& c) {  // @9ef360: the GOT_CAPTURED bit (0x100 raw)
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		const int32_t ci = city_index(g, w, c.arg_str(1));
		if (ci < 0) return make_int(0);
		return make_int(g.cities[size_t(w)][size_t(ci)].city_flags & 0x100);
	});

	// ---- diplomacy
	def("have_fought", [&g](NativeCall& c) {  // @9fd0b0: treaties bit 2 (fought)
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		return make_int((g.leaders[size_t(a)].treaty(b) & 2) ? 1 : 0);
	});
	def("have_war", [&g](NativeCall& c) {
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		return make_int(g.leaders[size_t(a)].diplos(b) == 0 ? 1 : 0);
	});
	def("is_offering_alliance", [&g](NativeCall& c) {  // @9fcc00: dip[askee].treaty == 2
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b) || a == b) return make_int(-1);
		return make_int(g.leaders[size_t(a)].dip[size_t(b)].treaty == 2 ? 1 : 0);
	});
	def("amount_offered", [&g](NativeCall& c) {  // @9fcc70: dip[whom].offers[good]
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		const int32_t t = type_index(g, c.arg_str(2));
		if (!live_leader(g, a) || !live_leader(g, b) || a == b || uint32_t(t) >= 6) return make_int(-1);
		return make_int(g.leaders[size_t(a)].dip[size_t(b)].offers[size_t(t)]);
	});
	def("offer_made", [&g](NativeCall& c) {  // @9fca20: any_proposals(askee, asker) and dip[askee].agree == 1
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		const Leader::Deal& d = g.leaders[size_t(b)].dip[size_t(a)];
		bool any = d.treaty != -1;
		for (int32_t k = 0; k < 6 && !any; k++) any = d.offers[size_t(k)] != 0;
		for (int32_t k = 0; k < 8 && !any; k++) any = d.attacks[size_t(k)] != 0;
		return make_int(any && g.leaders[size_t(a)].dip[size_t(b)].agree == 1 ? 1 : 0);
	});
	def("have_contact", [&g](NativeCall& c) {  // @9fcd60: treaties[whom] bit 0 (both leaders live)
		const int32_t w = c.arg_int(0) - 1, ww = c.arg_int(1) - 1;
		if (!live_leader(g, w) || !live_leader(g, ww)) return make_int(-1);
		return make_int(g.leaders[size_t(w)].treaty(ww) & 1);
	});
	def("find_sell_price", [&g](NativeCall& c) {  // @9fb9e0: a resource (type < 6) with BUY_SELL and a market
		const int32_t w = c.arg_int(0) - 1;
		const U16 name = c.arg_str(1);
		const int32_t k = name.empty() ? -1 : type_index(g, name);
		if (!live_leader(g, w) || uint32_t(k) >= 6) return make_int(-1);
		if (!has_tribe_bonus(g, w, 4) && !has_preq(g, w, etype::kBuySell)) return make_int(-1);
		if (!can_buy_sell(g, w)) return make_int(-1);  // has_market (can_buy_sell = the bonus test above && has_market)
		int32_t buy = 0, sell = 0;
		calc_market_prices(g, w, k, &buy, &sell);
		return make_int(sell);
	});
	def("city_id_captured", [&g](NativeCall& c) {  // @9eefb0: the city_lost bit, consumed once the build is gone
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		if (!valid_leader(g, w)) return make_int(-1);
		if (!(g.scen.city_lost & (1u << w))) return make_int(0);
		if (valid_build(g, w, o)) return make_int(0);
		g.scen.city_lost &= ~(1u << w);
		return make_int(1);
	});
	// ConquestGame reads. A battle started on its own (no campaign) has an uninitialized ConquestGame: cur_round and
	// cur_age 0, map_id "", the tribute / times-played lookups -1. TODO(ctw): the strategic layer.
	def("get_turn", [](NativeCall&) { return make_int(0); });              // @9e67c0 conquest_game->cur_round
	def("get_age", [](NativeCall&) { return make_int(0); });               // @9e67d0 conquest_game->cur_age
	def("get_ctw_campaign", [](NativeCall&) { return make_string(U16()); });  // @9e82d0 "" unless initialized
	def("get_num_tribute", [](NativeCall&) { return make_int(-1); });      // @9e7430 -1 unless initialized
	def("get_num_times_played", [](NativeCall&) { return make_int(-1); });  // ConquestStyles lookup, -1 without one
	def("get_ctw_int", [&h](NativeCall& c) {  // @9e6350: 0 when absent
		auto it = h.ctw_ints.find(c.arg_str(0));
		return make_int(it == h.ctw_ints.end() ? 0 : it->second);
	});
	def("set_ctw_int", [&h](NativeCall& c) {  // @9e6230: set or add
		h.ctw_ints[c.arg_str(0)] = c.arg_int(1);
		return VP();
	});
	def("get_ctw_str", [&h](NativeCall& c) {  // @9e6380: "" when absent
		auto it = h.ctw_strs.find(c.arg_str(0));
		return make_string(it == h.ctw_strs.end() ? U16() : it->second);
	});
	def("set_ctw_str", [&h](NativeCall& c) {  // @9e6280
		h.ctw_strs[c.arg_str(0)] = c.arg_str(1);
		return VP();
	});
	def("clear_ctw_val", [&h](NativeCall& c) {  // @9e6430: both arrays
		const U16 id = c.arg_str(0);
		h.ctw_ints.erase(id);
		h.ctw_strs.erase(id);
		return VP();
	});
}

}  // namespace sf
}  // namespace ron::sim
