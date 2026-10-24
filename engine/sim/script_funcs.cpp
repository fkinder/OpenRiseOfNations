// The builtins of the simulation's script run time (docs/script-spec.md section 7): the utility sets with game_random,
// the script environment of ScenarioFuncSet (timers, script UnitGroups, named groups) and the 55 builtins the AI
// production scripts call (ai/scripts/*.bhs), each from its ScenarioFuncSet body (re/decomp/script_funcs, gitignored).
// `who` arguments are player numbers (1..8; leader index + 1). Builtins without a body here are logged stubs (the
// Runtime counts the calls and returns the error value of the return type).
#include <cstdint>
#include <string>

#include "script/bhs_stdlib.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/groups.hpp"
#include "sim/orders.hpp"
#include "sim/pathfinder.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"
#include "sim/ai.hpp"
#include "sim/script_host.hpp"
#include "sim/script_funcs_internal.hpp"

namespace ron::sim {

using bhs::make_int;
using bhs::make_string;
using bhs::NativeCall;
using bhs::U16;
using bhs::VP;

namespace {

// ---------------------------------------------------------------- helpers

using namespace sf;

bool live_unit(const Unit& u) { return (u.flags & objflag::kValid) != 0; }
bool valid_unit_o(Game& g, int32_t w, int32_t o) { return valid_unit(g, w, o) != nullptr; }

// Goods: LeaderDataEncrypt + 4t ^ 0x8221 (the stockpile for 0..5). TODO: rare goods read other encrypted fields.
int32_t good_count(const Leader& l, int32_t t) { return t < 6 ? l.bucket[size_t(t)] : 0; }

// ScenarioFuncSet::find_unit @9e2850 (who = leader index): from slot start + 1, wrapping at unit_mark, the next
// live captain of type t (t < 0: any); mode 1 on the map, mode 2 inside something and active, 0 any.
int32_t find_unit_raw(Game& g, int32_t w, int32_t t, int32_t start, int32_t mode) {
	const int32_t mark = g.objects[size_t(w)].unit_mark;
	if (start >= mark || start < 0) start = 0;
	int32_t i = start + 1;
	for (;;) {
		const int32_t k = i < mark ? i : 0;
		const Unit* u = unit_at(g, w, k);
		if (u && (u->flags & objflag::kValid) && captain(*u) && (t < 0 || type_is(*u->ptype, t))) {
			if (mode == 0) return k;
			if (mode == 1 && live_unit(*u) && u->on_map()) return k;
			if (mode == 2 && !u->on_map() && live_unit(*u)) return k;
		}
		i = k + 1;
		if (i == start + 1) return -1;
	}
}

// ScenarioFuncSet::find_build @9e2970: from slot start + 1 (2000 when out of range), wrapping at build_mark, the next
// valid building of type t (t < 0: any); mode != 0 finished (ACTIVE), mode 0 unfinished.
int32_t find_build_raw(Game& g, int32_t w, int32_t t, int32_t start, int32_t mode) {
	const int32_t mark = g.objects[size_t(w)].build_mark;
	if (start >= mark || start < 0) start = kBuildBase;
	int32_t i = start + 1;
	for (;;) {
		if (i >= mark) i = kBuildBase;
		const Build* b = build_at(g, w, i);
		if (b && (b->flags & objflag::kValid) && (t < 0 || type_is(*b->ptype, t))) {
			const bool active = (b->flags & objflag::kActive) != 0;
			if (mode != 0 ? active : !active) return i;
		}
		i++;
		if (i == start + 1) return -1;
	}
}

// BuildData::can_queue @62da00.
bool can_queue(Game& g, const Build& b, int32_t t) {
	if (!can_pay_cost(g, t, b.who, -1, b.o, 1)) {
		g.queue_fail = 5;  // QUEUE_COST
		return false;
	}
	return build_could_queue(g, b, t);
}

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

}  // namespace

int32_t ScenarioData::find_group(const std::u16string& name) const {
	for (size_t i = 0; i < groups.size(); i++) {
		if (bhs::ieq(groups[i].name, name)) return int32_t(i);
	}
	return -1;
}

namespace sf {

int32_t type_index(const Game& g, const U16& name) {
	if (name.empty()) return -1;
	const auto& types = g.rules->types;
	for (size_t i = 0; i < types.size() && i < size_t(kNumTypes); i++) {
		if (bhs::ieq(types[i].name, name)) return int32_t(i);
	}
	return -1;
}

namespace {
int32_t category_index(const Game& g, const char* var, const U16& name) {
	const CategoryList* cl = g.rules->category_list(var);
	if (!cl) return -1;
	for (size_t i = 0; i < cl->items.size(); i++) {
		if (bhs::ieq(cl->items[i].key, name)) return int32_t(i);  // Category +0x14 (key)
	}
	return -1;
}
}  // namespace

int32_t unit_cat_index(const Game& g, const U16& name) { return category_index(g, "unit_cats", name); }

int32_t stance_index(const Game& g, const U16& name) {
	for (const char* v : {"stances_combat", "stances_worker", "stances_caster", "stances_packer"}) {
		const int32_t k = category_index(g, v, name);
		if (k >= 0) return k;
	}
	return -1;
}

int32_t tribe_index(const Game& g, const U16& name) {
	const auto& tr = g.rules->tribes;
	for (size_t i = 0; i < tr.size(); i++) {
		if (bhs::ieq(tr[i].name, name) || bhs::ieq(tr[i].eng_name, name)) return int32_t(i);
	}
	return -1;
}

int32_t city_index(const Game& g, int32_t w, const U16& name) {
	const auto& list = g.cities[size_t(w)];
	const int32_t n = std::min<int32_t>(g.leaders[size_t(w)].city_mark, int32_t(list.size()));
	for (int32_t c = 0; c < n; c++) {
		if (list[size_t(c)].valid() && bhs::ieq(bhs::to_u16(list[size_t(c)].name), name)) return c;
	}
	return -1;
}

Group& push_one(Game& g, int32_t w, int32_t o) {
	Group tmp;
	group_clear(tmp, -1);
	tmp.id = -1;
	group_add(g, tmp, o, w, 0, 0);
	const int32_t idx = groups_push_group(g, w, tmp, 1);
	return g.groups.g[size_t(idx)];
}

}  // namespace sf

void install_script_funcs(ScriptHost& h) {
	Game& g = h.game;
	bhs::Runtime& rt = h.rt;
	auto def = [&rt](const char* name, Fn f, int overload = 0) { rt.set_native(name, std::move(f), overload); };

	// ---- MathUtil / TriggerUtil / StringUtil / ArrayUtil with game_random
	bhs::StdlibHooks hooks;
	hooks.rand_get = [&g](int32_t lo, int32_t hi) { return g.rng.get(lo, hi); };
	hooks.rand_next = [&g]() { return g.rng.next(); };
	hooks.rand_set_seed = [&g](uint32_t s) { g.rng.reseed(s); };
	hooks.rand_seed = [&g]() { return g.rng.seed(); };
	hooks.debug_print = [&h](const U16& s) {
		if (h.verbose && h.messages.size() < 200) h.messages.push_back("print: " + bhs::to_utf8(s));
	};
	bhs::install_stdlib(rt, hooks);

	// ---- timers (ScenarioData::timers; seconds of Game::tick)
	def("set_timer", [&h, &g](NativeCall& c) {  // @9e3c40
		const U16 id = c.arg_str(0);
		const int32_t secs = c.arg_int(1);
		if (secs < 1 || id.empty()) return make_int(-1);
		return make_int(h.timers.add(id, g.tick + secs));
	});
	def("stop_timer", [&h](NativeCall& c) { return make_int(h.timers.remove(c.arg_str(0))); });
	def("timer_expired", [&h, &g](NativeCall& c) { return make_int(h.timers.check(c.arg_str(0), g.tick)); });

	// ---- script UnitGroup structs {int nation; int[] units} (@9fa7f0 / @9fa870 / @9fa8e0)
	auto group_units = [](NativeCall& c) -> bhs::Value* {
		bhs::Value* s = c.args.empty() ? nullptr : c.args[0].get();
		if (!s || s->elems.size() < 2 || !s->elems[1] || !s->elems[1]->is_array()) return nullptr;
		return s->elems[1].get();
	};
	def("add_to_group", [group_units](NativeCall& c) {
		bhs::Value* a = group_units(c);
		if (!a) return make_int(-1);
		const int32_t id = c.arg_int(1);
		for (size_t k = 0; k < a->elems.size(); k++) {
			if (a->elems[k] && bhs::value_int(a->elems[k].get()) == id) return make_int(int32_t(k));
		}
		a->elems.push_back(make_int(id, bhs::SC_VAR));
		return make_int(int32_t(a->elems.size()) - 1);
	}, 2);
	def("remove_from_group", [group_units](NativeCall& c) {  // ScriptArray::remove: the new length
		bhs::Value* a = group_units(c);
		if (!a) return make_int(-1);
		const int32_t id = c.arg_int(1);
		for (size_t k = 0; k < a->elems.size(); k++) {
			if (a->elems[k] && bhs::value_int(a->elems[k].get()) == id) {
				a->elems.erase(a->elems.begin() + long(k));
				return make_int(int32_t(a->elems.size()));
			}
		}
		return make_int(-1);
	}, 2);
	def("clear_group", [](NativeCall& c) {
		bhs::Value* s = c.args.empty() ? nullptr : c.args[0].get();
		if (s && s->elems.size() >= 2) {
			if (s->elems[0]) s->elems[0]->i = 0;
			if (s->elems[1]) s->elems[1]->elems.clear();
		}
		return VP();
	}, 2);

	// ---- game settings
	def("get_is_no_nation_powers", [&g](NativeCall&) { return make_int(int32_t((g.info.flags >> 2) & 1)); });
	def("get_rush_rules", [&g](NativeCall&) { return make_int(g.info.rush_rules); });
	def("is_victory_economic", [&g](NativeCall&) { return make_int(g.info.victory == 8); });
	def("is_victory_musical_chairs", [&g](NativeCall&) { return make_int(g.info.victory == 5); });
	def("is_victory_score", [&g](NativeCall&) { return make_int(g.info.victory == 3); });
	def("is_victory_tech_race", [&g](NativeCall&) { return make_int(g.info.victory == 9); });
	def("is_victory_territory", [&g](NativeCall&) { return make_int(g.info.victory == 7); });
	def("is_victory_wonder", [&g](NativeCall&) { return make_int(g.info.victory == 6); });
	def("is_conquest_scenario", [&g](NativeCall&) { return make_int(g.ctw ? 1 : 0); });
	// get_mapstyle (map_styles[world.map_style].name): TODO(world) the map style is not kept by the loader.
	def("get_mapstyle", [](NativeCall&) { return make_string(U16()); });

	// ---- common scenario queries (beyond the AI set)
	def("time_min", [&g](NativeCall&) { return make_int(g.tick / 60); });
	def("time_later_than", [&g](NativeCall& c) { return make_int(c.arg_int(0) <= g.tick / 60 ? 1 : 0); });
	def("num_players", [&g](NativeCall&) {
		int32_t n = 0;
		for (int32_t w = 0; w < 8; w++) n += (g.leaders[size_t(w)].flags & leaderflag::kValid) ? 1 : 0;
		return make_int(n);
	});
	def("get_difficulty", [&g](NativeCall&) { return make_int(g.info.difficulty + 1); });  // (CTW map: its own)
	def("is_defeated", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		return make_int((g.leaders[size_t(w)].flags & leaderflag::kDefeated) ? 1 : 0);
	});
	auto diplo = [&g](NativeCall& c) -> int32_t {  // diplos[who2] of who1, -2 when either is not live
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return -2;
		return g.leaders[size_t(a)].diplos(b);
	};
	def("have_alliance", [diplo](NativeCall& c) {
		const int32_t d = diplo(c);
		return make_int(d == -2 ? -1 : d == 2 ? 1 : 0);
	});
	def("have_peace", [diplo](NativeCall& c) {
		const int32_t d = diplo(c);
		return make_int(d == -2 ? -1 : (d == 1 || d == 2) ? 1 : 0);
	});

	// ---- leader queries
	def("population", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(live_leader(g, w) ? g.leaders[size_t(w)].control : -1);
	});
	def("age", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(live_leader(g, w) ? g.leaders[size_t(w)].age : -1);
	});
	def("get_starting_town_size", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		const Leader& l = g.leaders[size_t(w)];
		if (l.flags2 & 0x80) return make_int(0);
		if (g.scripted_rules || g.ctw) return make_int(l.city_num > 0 ? 1 : 0);  // patch version > 3
		return make_int(g.info.starting_town);
	});
	def("get_starting_resources", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		if (g.info.game_rules == 8 && g.info.team[size_t(w)] == 0) return make_int(g.info.starting_resources2);
		return make_int(g.info.starting_resources);
	});
	def("num_cities", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(valid_leader(g, w) ? g.leaders[size_t(w)].city_num : -1);
	});
	def("num_rare_resources_seen", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		return make_int(live_leader(g, w) ? int32_t(g.leaders[size_t(w)].new_rares.size()) : -1);
	});
	def("find_nation", [&g](NativeCall& c) {  // tribes[tribe].eng_name
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_string(U16());
		const int32_t tr = g.leaders[size_t(w)].tribe;
		if (tr < 0 || size_t(tr) >= g.rules->tribes.size()) return make_string(U16());
		return make_string(g.rules->tribes[size_t(tr)].eng_name);
	});
	def("get_techs_per_age", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t ages = g.leaders[size_t(w)].age;
		if (ages > 6) return make_int(0);
		return make_int(techs_per_age(g, w, ages + 544));
	});

	// ---- type counts
	auto count_body = [&g](int32_t w, int32_t t, bool queued) -> int32_t {
		if (t < 0 || uint32_t(w) >= 8 || !live_leader(g, w)) return -1;
		const Leader& l = g.leaders[size_t(w)];
		if (queued) t = get_graft(g, w, current_upgrade(g, w, t));
		if (t < 0) return -1;
		if (is_unit_type(t)) return unit_count(l, t) + (queued ? l.num_queued[size_t(t)] : 0);
		if (is_build_type(t)) return l.num_buildings[size_t(t - kBuilds)] + (queued ? l.num_queued[size_t(t)] : 0);
		if (is_good_type(t)) return good_count(l, t);
		return -1;
	};
	def("num_type", [&g, count_body](NativeCall& c) {
		return make_int(count_body(c.arg_int(0) - 1, type_index(g, c.arg_str(1)), false));
	});
	def("num_type_with_queued", [&g, count_body](NativeCall& c) {
		return make_int(count_body(c.arg_int(0) - 1, type_index(g, c.arg_str(1)), true));
	});
	def("at_least_type", [&g, count_body](NativeCall& c) {  // @9ed570 (no grafting)
		const int32_t n = count_body(c.arg_int(0) - 1, type_index(g, c.arg_str(2)), false);
		if (n == -1) return make_int(-1);
		return make_int(c.arg_int(1) <= n ? 1 : 0);
	});
	def("have_tech", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || !live_leader(g, w)) return make_int(-1);
		return make_int(has_tech(g, w, t) ? 1 : 0);
	});
	def("researching_tech", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || !valid_leader(g, w) || !is_tech_type(t)) return make_int(-1);
		return make_int(g.leaders[size_t(w)].num_queued[size_t(t)] != 0 ? 1 : 0);
	});
	def("can_pay_cost", [&g](NativeCall& c) {
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t t = type_index(g, c.arg_str(1));
		if (t > 0 && type_avail(g, w, t, 1) == 4) return make_int(can_pay_cost(g, t, w, -1, -1, 1) != 0 ? 1 : 0);
		return make_int(-1);
	});

	// ---- object searches (find_counters: 1 find_unit, 2 find_build, 4 find_idle_citizen, 0x16/0x17
	// find_build_at_city, 0x18 find_inactive_build, 0x1c train_unit_with_cost, 0x1e research_tech_with_cost)
	auto& fc = h.find_counters;
	def("find_unit", [&g, &fc](NativeCall& c) {  // @9ebe10
		const int32_t w = c.arg_int(0) - 1;
		if (fc[1] < 0) fc[1] = 0;
		int32_t t = -1;
		const U16 name = c.arg_str(1);
		if (!name.empty()) {
			t = type_index(g, name);
			if (t < 0 || !is_unit_type(t) || !live_leader(g, w)) return make_int(-1);
		}
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t o = find_unit_raw(g, w, t, fc[1], 1);
		if (o < 0) return make_int(-1);
		fc[1] = o;
		return make_int(o);
	});
	auto find_build_counter = [&g](int32_t w, int32_t t, int32_t& counter, bool active) -> int32_t {
		const int32_t start = counter;
		const int32_t o = find_build_raw(g, w, t, start, active ? 1 : 0);
		if (o < 0) return -1;
		counter = o;
		return o;
	};
	def("find_build", [&g, &fc, find_build_counter](NativeCall& c) {  // @9ec160
		const int32_t w = c.arg_int(0) - 1;
		if (fc[2] < 0) fc[2] = kBuildBase;
		int32_t t = -1;
		const U16 name = c.arg_str(1);
		if (!name.empty()) {
			t = type_index(g, name);
			if (t < 0 || !is_build_type(t) || !live_leader(g, w)) return make_int(-1);
		}
		if (!live_leader(g, w)) return make_int(-1);
		return make_int(find_build_counter(w, t, fc[2], true));
	});
	def("find_inactive_build", [&g, &fc, find_build_counter](NativeCall& c) {  // @9ece40
		const int32_t w = c.arg_int(0) - 1;
		if (fc[0x18] < 0) fc[0x18] = kBuildBase;
		int32_t t = -1;
		const U16 name = c.arg_str(1);
		if (!name.empty()) {
			t = type_index(g, name);
			if (t < 0 || !is_build_type(t) || !live_leader(g, w)) return make_int(-1);
		}
		if (!live_leader(g, w)) return make_int(-1);
		return make_int(find_build_counter(w, t, fc[0x18], false));
	});
	def("find_idle_citizen", [&g, &fc](NativeCall& c) {  // @9ead70
		const int32_t w = c.arg_int(0) - 1;
		const int32_t mark = uint32_t(w) < 8 ? g.objects[size_t(w)].unit_mark : 0;
		if (fc[4] < 0 || mark <= fc[4]) fc[4] = 0;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t t = get_graft(g, w, 50);  // PEASANTS, or the nation's replacement
		if (mark <= fc[4]) fc[4] = 0;
		int32_t i = fc[4] + 1;
		do {
			const int32_t k = i < mark ? i : 0;
			const Unit* u = unit_at(g, w, k);
			if (u && live_unit(*u) && u->on_map() && type_is(*u->ptype, t) && u->order_type() == kOrderNone &&
			    captain(*u)) {
				fc[4] = k;
				return make_int(k);
			}
			i = k + 1;
		} while (i != fc[4] + 1);
		fc[4] = 0;
		return make_int(-1);
	});
	def("find_num_idle_unit", [&g](NativeCall& c) {  // @9f3390
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (!live_leader(g, w) || t < 0 || !is_unit_type(t)) return make_int(-1);
		int32_t n = 0;
		for (int32_t k = 0; k < g.objects[size_t(w)].unit_mark; k++) {
			const Unit* u = unit_at(g, w, k);
			if (u && live_unit(*u) && u->on_map() && type_is(*u->ptype, t) && u->order_type() == kOrderNone && captain(*u))
				n++;
		}
		return make_int(n);
	});
	auto find_build_at_city = [&g, &fc](NativeCall& c) {  // @9ec7a0 (and _using_type_name @9ecaf0, the same body)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t count_inactive = c.arg_int(3);
		if (fc[0x17] < 0) fc[0x17] = -1;
		if (fc[0x16] < 0) fc[0x16] = -1;
		if (!valid_leader(g, w)) return make_int(-1);
		const U16 tn = c.arg_str(2);
		const int32_t t = tn.empty() ? -2 : type_index(g, tn);
		const int32_t ci = city_index(g, w, c.arg_str(1));
		if (t == -1 || ci < 0) return make_int(-1);
		auto matches = [&](const Build& b) {
			if (count_inactive == 0 && !((b.flags & objflag::kValid) && (b.flags & objflag::kActive))) return false;
			return t == -2 || type_is(*b.ptype, t);
		};
		const int32_t head = g.cities[size_t(w)][size_t(ci)].o;
		bool first = true;
		int32_t o = head;
		if (fc[0x17] == w && fc[0x16] >= 0) {
			const Build* r = build_at(g, w, fc[0x16]);
			if (r && (r->flags & objflag::kValid) && r->city == ci) {
				first = false;
				o = fc[0x16];
			}
		}
		for (int guard = 0; o >= 0 && guard < kBuildLimit; guard++) {  // from the resume point to the end
			const Build* b = build_at(g, w, o);
			if (!b) break;
			const int32_t next = b->city_down;
			if (matches(*b)) {
				fc[0x16] = next;
				fc[0x17] = w;
				return make_int(o);
			}
			o = next;
		}
		if (!first) {  // wrap: from the head up to the resume point
			for (int32_t n = head, guard = 0; n >= 0 && n != fc[0x16] && guard < kBuildLimit; guard++) {
				const Build* b = build_at(g, w, n);
				if (!b) break;
				const int32_t next = b->city_down;
				if (matches(*b)) {
					fc[0x16] = next;
					fc[0x17] = w;
					return make_int(n);
				}
				n = next;
			}
		}
		fc[0x16] = -1;
		return make_int(-1);
	};
	def("find_build_at_city", find_build_at_city);
	def("find_build_at_city_using_type_name", find_build_at_city);

	// ---- cities
	def("find_city_id", [&g](NativeCall& c) {  // @9ef580: every valid leader's cities, the city object's o
		const U16 name = c.arg_str(0);
		for (int32_t w = 0; w < 8; w++) {
			if (!(g.leaders[size_t(w)].flags & leaderflag::kValid)) continue;
			for (const City& ct : g.cities[size_t(w)]) {
				if (ct.valid() && bhs::ieq(bhs::to_u16(ct.name), name)) return make_int(ct.o);
			}
		}
		return make_int(-1);
	});
	def("find_city_with_num", [&g](NativeCall& c) {  // @9eff00
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_string(U16());
		const int32_t k = c.arg_int(1) - 1;
		if (k >= g.leaders[size_t(w)].city_mark || k < 0 || size_t(k) >= g.cities[size_t(w)].size())
			return make_string(U16());
		const City& ct = g.cities[size_t(w)][size_t(k)];
		return make_string(ct.valid() ? bhs::to_u16(ct.name) : U16());
	});
	auto num_city_buildings = [&g](NativeCall& c) {  // @9f0150 (and _using_type_name @9f01e0, the same body)
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		const int32_t ci = city_index(g, w, c.arg_str(1));
		const int32_t t = type_index(g, c.arg_str(2));
		if (ci < 0 || t < 0) return make_int(-1);
		return make_int(city_count_buildings(g, w, ci, t, 0, c.arg_int(3) == 0 ? 1 : 0));
	};
	def("num_city_buildings", num_city_buildings);
	def("num_city_buildings_using_type_name", num_city_buildings);
	auto was_city = [&g](NativeCall& c, bool raided) {  // @9fd3e0 / @9fd510
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t secs = c.arg_int(2);
		const int32_t ci = city_index(g, w, c.arg_str(1));
		const auto& list = g.cities[size_t(w)];
		if (ci < 0) {  // any city (a never-stamped city counts as recent when secs >= 1)
			for (const City& ct : list) {
				if (!ct.valid()) continue;
				const int32_t st = raided ? ct.raid_stamp : ct.attack_stamp;
				if ((secs < 1 && st != 0) || (g.frame - st) / 15 <= secs) return make_int(1);
			}
			return make_int(0);
		}
		const int32_t st = raided ? list[size_t(ci)].raid_stamp : list[size_t(ci)].attack_stamp;
		if (st == 0) return make_int(0);
		if (secs > 0) return make_int((g.frame - st) / 15 <= secs ? 1 : 0);
		return make_int(1);
	};
	def("was_city_raided", [was_city](NativeCall& c) { return was_city(c, true); });
	def("was_city_attacked", [was_city](NativeCall& c) { return was_city(c, false); });

	// ---- objects
	auto position = [&g](NativeCall& c, bool y) {  // @9f1360 / @9f1470: tcoords of the object or its container
		int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		if (!active_unit_slot(g, w, o) && !valid_build(g, w, o)) return make_int(-1);
		if (o < kUnitLimit) {
			o = get_captain(g, w, o);
			const Unit* u = unit_at(g, w, o);
			if (u && u->inside_up >= 0) {  // ObjectData::get_inside
				w = u->inside_up_who;
				o = u->inside_up;
			}
		}
		const Object* ob = g.object(w, o);
		if (!ob) return make_int(-1);
		const int32_t v = y ? ob->y : ob->x;
		return make_int((v >> 6) / 3);  // div_3_table[v >> 6]
	};
	def("object_position_x", [position](NativeCall& c) { return position(c, false); });
	def("object_position_y", [position](NativeCall& c) { return position(c, true); });
	def("building_started", [&g](NativeCall& c) {
		const Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		return make_int(b ? ((b->flags & objflag::kStarted) ? 1 : 0) : -1);
	});
	def("num_workers_at_building", [&g](NativeCall& c) {
		const Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b || !is_gather_type(*b->ptype)) return make_int(-1);
		return make_int(build_num_gatherers(g, *b, false, false));
	});
	def("max_workers_at_building", [&g](NativeCall& c) {
		const Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b || !is_gather_type(*b->ptype)) return make_int(-1);
		return make_int(b->gather_max);
	});
	def("num_type_queued", [&g](NativeCall& c) {  // @9f25e0
		const Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b) return make_int(-1);
		const U16 name = c.arg_str(2);
		auto entry = [&](int32_t i) {
			return i < b->build_queue.queue_size && size_t(i) < b->build_queue.queue.size() ? int32_t(b->build_queue.queue[size_t(i)].type)
			                                                                               : -1;
		};
		int32_t n = 0;
		if (name.empty() || name[0] == u' ') {
			for (int32_t i = 0; i < int32_t(uint8_t(b->queued)); i++) n += entry(i) != -1;
			return make_int(n);
		}
		const int32_t t = type_index(g, name);
		if (t < 0) return make_int(-1);
		for (int32_t i = 0; i < int32_t(uint8_t(b->queued)); i++) {
			const int32_t e = entry(i);
			if (e >= 0 && type_is(g.type(e), t)) n++;
		}
		return make_int(n);
	});

	// ---- production
	def("research_tech_with_cost", [&g, &fc](NativeCall& c) {  // @9ee700
		if (fc[0x1e] < 0) fc[0x1e] = kBuildBase;
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || !live_leader(g, w)) return make_int(-1);
		if (has_tech(g, w, t)) return make_int(1);
		const int32_t bt = current_upgrade(g, w, g.type(t).where);
		if (bt < 0) return make_int(-1);
		const int32_t o = find_build_raw(g, w, bt, fc[0x1e], 1);
		if (o >= 0) {
			Build* b = build_at(g, w, o);
			if (b && can_queue(g, *b, t)) {
				fc[0x1e] = o;
				group_action_queue_up(g, push_one(g, w, o), t, 1);
				return make_int(o);
			}
		}
		return make_int(0);
	});
	def("train_unit_with_cost", [&g, &fc](NativeCall& c) {  // @9f42d0
		if (fc[0x1c] < 0) fc[0x1c] = kBuildBase;
		const int32_t num = c.arg_int(1);
		if (num < 1 || num > 2000) return make_int(-1);
		const int32_t w = c.arg_int(0) - 1;
		int32_t t = type_index(g, c.arg_str(2));
		if (t < 0 || !live_leader(g, w)) return make_int(-1);
		t = get_graft(g, w, current_upgrade(g, w, t));
		if (t < 0) return make_int(-1);
		const int32_t bt = current_upgrade(g, w, g.type(t).where);
		if (num <= can_pay_cost(g, t, w, -1, -1, 1)) {
			const int32_t o = find_build_raw(g, w, bt, fc[0x1c], 1);
			if (o >= 0) {
				fc[0x1c] = o;
				group_action_queue_up(g, push_one(g, w, o), t, num);
				return make_int(o);
			}
		}
		return make_int(0);
	});
	def("train_unit_at_with_cost", [&g](NativeCall& c) {  // @9f4460
		const int32_t num = c.arg_int(1);
		if (num < 1 || num > 2000) return make_int(-1);
		const int32_t w = c.arg_int(0) - 1;
		int32_t t = type_index(g, c.arg_str(2));
		if (t < 0 || !live_leader(g, w)) return make_int(-1);
		t = get_graft(g, w, current_upgrade(g, w, t));
		if (t < 0) return make_int(-1);
		const int32_t o = c.arg_int(3);
		if (can_pay_cost(g, t, w, -1, -1, 1) < num) return make_int(0);
		Build* b = valid_build(g, w, o);
		if (b && build_could_queue(g, *b, t)) {
			group_action_queue_up(g, push_one(g, w, o), t, num);
			return make_int(o);
		}
		return make_int(-1);
	});
	// The placement builtins end in the AI's Leader::produce_building (ai-spec 7) and found_cities (ai-spec 6.2).
	// place_orphan_building_with_cost @9f5520 / ..._upgrade_with_cost @9f5700: the type (current_upgrade for the
	// upgrade variant) must be affordable once (loc = the building's city when it is an active city building), then
	// 1 when produce_building(t, o, 0) placed it, else 0. place_orphan_building @9f52f0 / ..._upgrade @9f5400 return
	// produce_building's own result. place_[orphan_]building* by city name use the city building (@9f5270..9f5680).
	auto place_orphan = [&g](int32_t w, const U16& type_name, int32_t o, bool upgrade, bool cost) {
		Build* b = valid_build(g, w, o);
		if (!b) return -1;
		int32_t t = type_index(g, type_name);
		if (t < 0 || !is_build_type(t)) return -1;
		if (upgrade) t = current_upgrade(g, w, t);
		if (!cost) return produce_building(g, w, t, o, 0);
		int32_t loc = -1;
		if ((b->flags & objflag::kActive) && (b->flags & objflag::kCity)) loc = b->city;
		if (!can_pay_cost(g, t, w, loc, -1, 1)) return 0;
		return produce_building(g, w, t, o, 0) == 0 ? 1 : 0;
	};
	auto by_city = [&g, place_orphan](NativeCall& c, bool upgrade, bool cost) {
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t ci = city_index(g, w, c.arg_str(2));
		if (ci < 0 || !g.cities[size_t(w)][size_t(ci)].valid()) return make_int(-1);
		return make_int(place_orphan(w, c.arg_str(1), g.cities[size_t(w)][size_t(ci)].o, upgrade, cost));
	};
	def("place_orphan_building_with_cost", [place_orphan](NativeCall& c) {
		return make_int(place_orphan(c.arg_int(0) - 1, c.arg_str(1), c.arg_int(2), false, true));
	});
	def("place_orphan_building_upgrade_with_cost", [place_orphan](NativeCall& c) {
		return make_int(place_orphan(c.arg_int(0) - 1, c.arg_str(1), c.arg_int(2), true, true));
	});
	def("place_orphan_building", [place_orphan](NativeCall& c) {
		return make_int(place_orphan(c.arg_int(0) - 1, c.arg_str(1), c.arg_int(2), false, false));
	});
	def("place_orphan_building_upgrade", [place_orphan](NativeCall& c) {
		return make_int(place_orphan(c.arg_int(0) - 1, c.arg_str(1), c.arg_int(2), true, false));
	});
	def("place_building_with_cost", [by_city](NativeCall& c) { return by_city(c, false, true); });  // @9f54a0
	def("place_building_upgrade_with_cost", [by_city](NativeCall& c) { return by_city(c, true, true); });  // @9f5680
	def("place_building", [by_city](NativeCall& c) { return by_city(c, false, false); });  // @9f5270
	def("place_building_upgrade", [by_city](NativeCall& c) { return by_city(c, true, false); });  // @9f5380
	def("place_city_with_cost", [&g](NativeCall& c) {  // @9f5860
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		if (get_city_limit(g, w) <= get_total_cities(g, w)) return make_int(-1);
		const int32_t before = get_total_cities(g, w);
		compute_sites(g, w, 0);
		make_list_clear(g, w);
		found_cities(g, w);
		make_list_clear(g, w);
		return make_int(before < get_total_cities(g, w) ? 1 : 0);
	});
	def("destroy_building", [&g](NativeCall& c) {  // @9f6fc0: Build::close(0, -1, 0)
		Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b) return make_int(-1);
		build_close(g, *b, 0);
		return make_int(1);
	});

	// ---- orders (directly on the units, like the group commands: no CommandPackage)
	def("unit_move_order", [&g](NativeCall& c) {  // @9f71a0
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		const int32_t x = c.arg_int(2), y = c.arg_int(3);
		if (!active_unit_slot(g, w, o)) return make_int(-1);
		if (x < 0 || y < 0 || x >= g.world.tile_xs || y >= g.world.tile_ys) return make_int(-1);
		o = get_captain(g, w, o);
		Unit* u = unit_at(g, w, o);
		if (!u) return make_int(-1);
		clear_orders(g, *u);  // UNIT_MULTIMOVE off, path length 0, close_orders, clear_partial_path, update_action
		set_group(g, w, o, -1);
		Group& gr = push_one(g, w, o);
		group_action_move_near(g, gr, x * 192 + 96, y * 192 + 96, 0, kQueueNew, false, 0, kMoveTo, 1, -1, -1, 0);
		return make_int(1);
	});
	def("citizen_repair_order", [&g](NativeCall& c) {  // @9f85b0
		const int32_t w = c.arg_int(0) - 1;
		const int32_t o = c.arg_int(1), bo = c.arg_int(2);
		if (!valid_unit_o(g, w, o)) return make_int(-1);
		Build* b = valid_build(g, w, bo);
		if (!b) return make_int(-1);
		Unit* u = unit_at(g, w, o);
		if (u->type != 50 && u->type != 51) return make_int(-1);
		clear_orders(g, *u);
		u->group = -1;
		Group& gr = push_one(g, w, o);
		const int32_t order = (b->flags & objflag::kActive) ? kRepair : kBuildAt;
		group_action_swarm_around(g, gr, bo, w, kQueueFirst, order, true);
		return make_int(1);
	});

	sf::install_query_funcs(h);
	sf::install_mutation_funcs(h);
	sf::install_order_funcs(h);
	sf::install_ui_funcs(h);
	sf::install_ctw_funcs(h);  // last: the campaign-aware versions replace the stand-alone ones (script_funcs_ctw.cpp)
}

}  // namespace ron::sim
