// The scenario builtins that change simulation state directly (docs/script-spec.md 7.3 category c): rules tables,
// unit creation and removal, techs, stockpiles, victory / defeat, object health, ownership, named groups and the
// ScenarioData switches. Each body follows its ScenarioFuncSet decompile / disassembly (re/decomp/script_funcs,
// re/decomp/scen_builtins; gitignored), with every game_random draw where the original draws (kill_unit*).
#include <cmath>

#include "script/bhs_stdlib.hpp"
#include "sim/ai.hpp"
#include "sim/aircraft.hpp"
#include "sim/capture.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"
#include "sim/script_funcs_internal.hpp"
#include "sim/search.hpp"
#include "sim/territory.hpp"
#include "sim/transport.hpp"

namespace ron::sim {
namespace sf {

namespace {

U16 unnamed(int32_t who) { return U16(u"_Unnamed") + bhs::itow_text(who); }  // default_group @4ce990

// The range of rules types the "every type whose is-list contains t" loops of the type builtins walk: units
// 50..401 for a unit type, else buildings 414..542.
void for_line(Game& g, int32_t t, bool all_types, const std::function<void(Type&)>& f) {
	Rules& R = g.mut_rules();
	int32_t lo = 0, hi = kNumTypes;
	if (!all_types) {
		if (is_unit_type(t)) lo = kUnits, hi = kEndUnits;
		else lo = kBuilds, hi = kEndBuilds;
	}
	for (int32_t k = lo; k < hi; k++) {
		if (type_is(R.types[size_t(k)], t)) f(R.types[size_t(k)]);
	}
}

// TechType::is_ultimate_preq(t) @66c410 on tech `self`: `self` is reached through t's (effective) prerequisites.
// The original re-derives each slot's GameInfo-dependent prerequisite inline; TypeData::get_preq (production-spec
// 7.9.1, techs.cpp) performs the same derivation.
bool is_ultimate_preq(const Game& g, int32_t self, int32_t t, int depth = 0) {
	if (depth > 64 || t < 0 || t >= kNumTypes) return false;
	const int32_t n = num_preq(t);
	for (int32_t i = 0; i < n; i++) {
		const int32_t p = get_preq(g, t, i, -1);
		if (p == TYPE_ANY_BUILDING) return false;
		if (p >= 0 && (p == self || is_ultimate_preq(g, self, p, depth + 1))) return true;
	}
	return false;
}

// The prerequisite walk of gain_tech @9ee1a0 / gain_upgrade @9eeb90: every non-age tech 551..628 that is an ultimate
// prerequisite of t and not held is gained first (Leader::gain_tech(p, -1, -1, 0, 0)).
void gain_preqs(Game& g, int32_t w, int32_t t) {
	for (int32_t p = kAges; p < kEndTechs; p++) {
		if (is_age_type(p)) continue;
		if (is_ultimate_preq(g, p, t) && !has_tech(g, w, p)) gain_tech(g, w, p, -1, -1, false, false);
	}
}

// The tech-category names of gain_tech / lose_tech (rules tech_cats keys): the next (or current) level of the
// category's epoch line (MILITARY_BASE 572, CIVIC 565, BARTER 558, WRITTEN_WORD 551 + the line's epoch).
int32_t category_tech(const Game& g, int32_t w, const U16& name, int32_t delta, bool* found) {
	*found = false;
	const CategoryList* cl = g.rules->category_list("tech_cats");
	if (!cl) return -1;
	static const int32_t base[4] = {0x23c, 0x235, 0x22e, 0x227};
	for (size_t k = 0; k < cl->items.size() && k < 4; k++) {
		if (!bhs::ieq(cl->items[k].key, name)) continue;
		*found = true;
		const int32_t e = g.leaders[size_t(w)].epoch[k];
		if (e > 7) return -2;
		return base[k] + e + delta;
	}
	return -1;
}

// Leader::fix_tech_flags @6d2480: borders, the ally-LOS mask, the explore / reveal / ping flags from the bonus
// types' granting techs, then check_transport.
void fix_tech_flags(Game& g, int32_t w) {
	regions_fix_all_borders(g);
	Leader& l = g.leaders[size_t(w)];
	auto bonus_tech = [&g](int32_t b) { return g.type(b).preq[0]; };
	l.ally_mask = uint8_t(1u << w);
	if (has_tech(g, w, bonus_tech(ptype::kAllyLos))) {
		for (int32_t k = 0; k < 8; k++) {
			if (k != w && (g.leaders[size_t(k)].flags & leaderflag::kValid) && is_ally(g, w, k))
				l.ally_mask = uint8_t(l.ally_mask | (1u << k));
		}
	}
	if (has_tech(g, w, bonus_tech(ptype::kExploreMapBonus))) l.flags |= leaderflag::kExploreMap;
	else l.flags &= ~leaderflag::kExploreMap;
	if (has_tech(g, w, bonus_tech(ptype::kRevealEnemyBonus)) || has_wonder_prod(g, w, ptype::kSpaceProgram))
		l.flags |= leaderflag::kRevealEnemy;
	else l.flags &= ~leaderflag::kRevealEnemy;
	if (has_tech(g, w, bonus_tech(ptype::kActivePingBonus))) l.flags |= leaderflag::kActivePing;
	else l.flags &= ~leaderflag::kActivePing;
	check_transport(g, w);
}

// Leader::lose_tech(t) @6d2850.
void leader_lose_tech(Game& g, int32_t w, int32_t t, int depth = 0) {
	Leader& l = g.leaders[size_t(w)];
	if (depth > 128) return;
	if (!is_unit_type(t)) {
		if (is_tech_type(t)) {
			l.set_tech_bit(t, false);
			for (int32_t T = kAges; T < kEndTechs; T++) {
				if (!l.has_tech_bit(T)) continue;
				if (!is_age_type(T) || !is_age_type(t)) {
					if (is_ultimate_preq(g, t, T)) leader_lose_tech(g, w, T, depth + 1);
				} else if (g.type(t).age <= T - kAges) {
					leader_lose_tech(g, w, T, depth + 1);
				}
			}
			fix_tech_flags(g, w);
			if (!is_age_type(t)) {
				if (!is_epoch_type(t)) {
					l.discovered--;
				} else {
					l.epochs--;
					const int32_t cat = g.type(t).cat;
					if (cat >= 0 && cat < 4) l.epoch[size_t(cat)] = compute_epoch(g, w, cat);
				}
			} else {
				int32_t ages = 0;
				for (int32_t a = kAges; a < kEndAges; a++) ages += has_tech(g, w, a) ? 1 : 0;
				l.age = ages;
				for (int32_t o = kBuildBase; o < g.objects[size_t(w)].build_mark; o++) {
					Build* b = build_at(g, w, o);
					if (b && (b->flags & objflag::kValid)) b->max_age = uint8_t(ages);
				}
			}
		}
	} else {
		if (has_tech(g, w, t)) l.discovered--;
		l.set_tech_bit(t, false);
	}
	reset_obs_flags(g, w);
	l.flags |= 0xc000000;  // CALC_UNIT_STATS | CALC_WALL_STATS
}

// Leader::set_age(n) @6d25a0.
void leader_set_age(Game& g, int32_t w, int32_t n) {
	n = std::clamp(n, 0, 7);
	const int32_t top = kAges + n;
	for (int32_t t = ptype::kInformationAge; t >= top; t--) {
		if (has_tech(g, w, t)) leader_lose_tech(g, w, t);
	}
	for (int32_t t = kAges; t < top; t++) {
		if (!has_tech(g, w, t)) gain_tech(g, w, t, 0, 0, false, true);
	}
	for (int32_t t = kAges; t < kEndTechs; t++) {
		if (!is_age_type(t) && has_tech(g, w, t) && !has_preq(g, w, t)) leader_lose_tech(g, w, t);
	}
	for (int32_t t = kUnits; t < kEndUnits; t++) {
		if (g.leaders[size_t(w)].has_tech_bit(t) && !has_preq(g, w, t)) leader_lose_tech(g, w, t);
	}
	for (int32_t t = kBuilds; t < kEndBuilds; t++) {
		if (g.leaders[size_t(w)].has_tech_bit(t) && !has_preq(g, w, t)) leader_lose_tech(g, w, t);
	}
	reset_obs_flags(g, w);
	calc_unit_stats(g, w);
	calc_wall_stats(g, w);
}



}  // namespace

void install_mutation_funcs(ScriptHost& h) {
	Game& g = h.game;
	bhs::Runtime& rt = h.rt;
	auto def = [&rt](const char* name, Fn f, int overload = 0) { rt.set_native(name, std::move(f), overload); };

	// ---- rules tables (types.list)
	def("set_type_build_time", [&g](NativeCall& c) {  // @9ea7d0: job_time = secs / 100 (min 1) on every match
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0) return make_int(-1);
		const int32_t v = c.arg_int(1) / 100;
		for_line(g, t, true, [v](Type& T) { T.job_time = uint32_t(v == 0 ? 1 : v); });
		return make_int(c.arg_int(1));
	});
	def("set_type_job_time", [&g](NativeCall& c) {  // @9ea880: job_time = 1 below 200, else secs / 100
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0) return make_int(-1);
		const int32_t v = c.arg_int(1) < 200 ? 1 : c.arg_int(1) / 100;
		for_line(g, t, true, [v](Type& T) { T.job_time = uint32_t(v); });
		return make_int(c.arg_int(1));
	});
	def("rename_type", [&g](NativeCall& c) {  // @9ea6c0: the display name (+0x74) of every match
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0) return make_int(-1);
		const U16 nn = c.arg_str(1);
		for_line(g, t, true, [&nn](Type& T) { T.display_name = nn; });
		return make_int(1);
	});
	def("disable_tech", [&g](NativeCall& c) {  // @9ea2b0: TechTypeData leader_off |= 1 << (who - 1)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || is_unit_type(t) || is_build_type(t) || !is_tech_type(t)) return make_int(-1);
		if (uint32_t(w) < 8) {
			Type& T = g.mut_rules().types[size_t(t)];
			T.leader_off = uint8_t(T.leader_off | (1u << w));
		}
		return make_int(t);
	});
	def("disable_type", [&g](NativeCall& c) {  // @9ea130: preq[0] = -2, tribe mask 0, modified
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0 || (!is_unit_type(t) && !is_build_type(t))) return make_int(-1);
		for_line(g, t, false, [](Type& T) {
			T.preq[0] = -2;
			T.tribe_mask = 0;
			T.modified = 1;
		});
		return make_int(t);
	});
	def("enable_type", [&g](NativeCall& c) {  // @9ea3e0: ObjectType::restore from the backup (the loaded tables)
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0 || (!is_unit_type(t) && !is_build_type(t))) return make_int(-1);
		const Rules* base = g.base_rules;
		for_line(g, t, false, [base](Type& T) {
			if (base) T = base->types[size_t(T.type)];
		});
		return make_int(t);
	});
	auto tribe_type = [&g](NativeCall& c, bool enable) {  // disable_type_by_tribe @a006a0 / enable_type_by_tribe @a009a0
		const int32_t t = type_index(g, c.arg_str(0));
		const int32_t tr = tribe_index(g, c.arg_str(1));
		if (tr < 0 || t < 0) return -1;
		if (!is_unit_type(t) && !is_build_type(t)) return -1;
		Type& T = g.mut_rules().types[size_t(t)];
		if (enable) T.tribe_mask |= int32_t(1u << (tr & 31));
		else T.tribe_mask &= ~int32_t(1u << (tr & 31));
		T.modified = 1;
		for (int32_t w = 0; w < 8; w++) {
			Leader& l = g.leaders[size_t(w)];
			if (!(l.flags & leaderflag::kValid) || l.tribe != tr) continue;
			// disable: the leader's tech bit (+0x6c18) of the type; enable: its obsolete bit (+0x6d00).
			if (enable) Leader::set_bit(l.obs_flags, t, false);
			else l.set_tech_bit(t, false);
		}
		return t;
	};
	def("disable_type_by_tribe", [tribe_type](NativeCall& c) { return make_int(tribe_type(c, false)); });
	def("enable_type_by_tribe", [tribe_type](NativeCall& c) { return make_int(tribe_type(c, true)); });
	def("enable_type_by_tribe", [&g, tribe_type](NativeCall& c) {  // @a00ca0: + where / grid column / row for units
		const int32_t t = tribe_type(c, true);
		if (t < 0) return make_int(-1);
		if (is_build_type(t)) return make_int(1);
		const int32_t b = type_index(g, c.arg_str(2));
		if (b < 0 || !is_build_type(b)) return make_int(-1);
		Type& T = g.mut_rules().types[size_t(t)];
		T.where = b;
		T.grid_x = int8_t(c.arg_int(4));
		T.grid_y = int8_t(c.arg_int(3));
		return make_int(1);
	}, 1);
	def("set_object_type_max_health", [&g](NativeCall& c) {  // @9f5da0: hits = n on the line, then stats
		const int32_t t = type_index(g, c.arg_str(0));
		const int32_t n = c.arg_int(1);
		if (t < 0 || n < 1 || (!is_unit_type(t) && !is_build_type(t))) return make_int(-1);
		for_line(g, t, false, [n](Type& T) {
			T.hits = n;
			T.modified = 1;
		});
		for (int32_t w = 0; w < 8; w++) {
			if (!live_leader(g, w)) continue;
			calc_wall_stats(g, w);
			calc_unit_stats(g, w);
		}
		return make_int(t);
	});

	// set_object_type_attack @9f6170 / _max_range @9f6330 / _armor (ObjectTypeData +0x1e8 / +0x1fc / +0x214) and
	// set_unit_type_speed @9f66b0 (UnitTypeData +0x2c0 moves, units only): the same shape as set_object_type_max_health.
	auto set_line_field = [&g](NativeCall& c, int32_t Type::*field, bool units_only) {
		const int32_t t = type_index(g, c.arg_str(0));
		const int32_t n = c.arg_int(1);
		if (t < 0 || n < 1) return make_int(-1);
		if (units_only ? !is_unit_type(t) : (!is_unit_type(t) && !is_build_type(t))) return make_int(-1);
		for_line(g, t, false, [n, field](Type& T) {
			T.*field = n;
			T.modified = 1;
		});
		for (int32_t w = 0; w < 8; w++) {
			if (!live_leader(g, w)) continue;
			calc_wall_stats(g, w);
			calc_unit_stats(g, w);
		}
		return make_int(t);
	};
	def("set_object_type_attack", [set_line_field](NativeCall& c) { return set_line_field(c, &Type::attack, false); });
	def("set_object_type_max_range",
	    [set_line_field](NativeCall& c) { return set_line_field(c, &Type::max_range, false); });
	def("set_object_type_armor", [set_line_field](NativeCall& c) { return set_line_field(c, &Type::armor, false); });
	def("set_unit_type_speed", [set_line_field](NativeCall& c) { return set_line_field(c, &Type::moves, true); });

	// ---- techs
	def("gain_tech", [&g](NativeCall& c) {  // @9ee1a0
		const int32_t w = c.arg_int(0) - 1;
		const U16 name = c.arg_str(1);
		int32_t t = type_index(g, name);
		if (t >= 0) {
			// Governments in Conquer the World: refused while another valid non-allied leader holds that card (+0x6911).
			if (is_gov_type(t) && g.ctw) {
				for (int32_t k = 0; k < 8; k++) {
					if (k != w && valid_leader(g, k) && !is_ally(g, k, w)) {
						// TODO(ctw): LeaderData +0x6911 (the government card) is not kept; never refused here.
					}
				}
			}
		} else {
			bool found = false;
			t = category_tech(g, w, name, 0, &found);
			if (t == -2) return make_int(-1);
		}
		if (t < 0 || !live_leader(g, w) || !is_tech_type(t)) return make_int(-1);
		gain_preqs(g, w, t);
		gain_tech(g, w, t, 0, 0, false, true);
		return make_int(1);
	});
	def("gain_upgrade", [&g](NativeCall& c) {  // @9eeb90
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t0 = type_index(g, c.arg_str(1));
		if (t0 < 0 || !live_leader(g, w)) return make_int(-1);
		const int32_t t = get_graft(g, w, t0);
		if (!tribe_can_type(g, w, t)) return make_int(-1);
		if (!is_unit_type(t)) gain_preqs(g, w, t);
		gain_tech(g, w, t, -1, -1, false, true);
		calc_unit_stats(g, w);
		calc_wall_stats(g, w);
		compute_all_territory(g);
		return make_int(has_tech(g, w, t) ? 1 : 0);
	});

	// ---- stockpiles (LeaderDataEncrypt goods)
	auto good = [&g](NativeCall& c, int32_t* w, int32_t* k) {
		*w = c.arg_int(0) - 1;
		*k = type_index(g, c.arg_str(1));
		return valid_leader(g, *w) && uint32_t(*k) < 6;
	};
	def("give_good", [&g, good](NativeCall& c) {  // @9fb590 (no clamp)
		int32_t w, k;
		if (!good(c, &w, &k)) return make_int(-1);
		g.leaders[size_t(w)].bucket[size_t(k)] += c.arg_int(2);
		return make_int(1);
	});
	def("take_good", [&g, good](NativeCall& c) {  // @9fb630 (clamped at 0)
		int32_t w, k;
		if (!good(c, &w, &k)) return make_int(-1);
		int32_t& b = g.leaders[size_t(w)].bucket[size_t(k)];
		b -= c.arg_int(2);
		if (b < 0) b = 0;
		return make_int(1);
	});
	def("set_good", [&g, good](NativeCall& c) {  // @9fb6f0
		int32_t w, k;
		if (!good(c, &w, &k) || c.arg_int(2) < 0) return make_int(-1);
		g.leaders[size_t(w)].bucket[size_t(k)] = c.arg_int(2);
		return make_int(1);
	});
	def("set_base_rate", [&g](NativeCall& c) {  // @9fbb80: base_rate = n << 4
		const int32_t w = c.arg_int(0) - 1;
		const int32_t k = type_index(g, c.arg_str(1));
		if (!live_leader(g, w) || uint32_t(k) >= 6) return make_int(-1);
		g.leaders[size_t(w)].base_rate[size_t(k)] = c.arg_int(2) << 4;
		return make_int(1);
	});
	def("set_population_cap", [&g](NativeCall& c) {  // @9e8ef0
		const int32_t w = c.arg_int(0) - 1, n = c.arg_int(1);
		if (n < 0 || !live_leader(g, w)) return make_int(-1);
		g.scen.pop_cap[size_t(w)] = n;
		calc_pop_cap(g, w);
		return make_int(n);
	});

	// ---- leaders
	def("victory", [&g](NativeCall& c) {  // @9f3fa0
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		leader_victory(g, w, 0, 0);
		game_check_victory(g);
		return make_int(1);
	});
	def("defeat", [&g](NativeCall& c) {  // @9f3f50
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		leader_defeat(g, w, 9, -1, 0);
		return make_int(1);
	});
	def("disable_city_defeat", [&g](NativeCall& c) {  // @9ffc80: leader_flags2 |= 1
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.leaders[size_t(w)].flags2 |= leaderflag2::kNoCityDefeat;
		return make_int(1);
	});
	def("set_leader_name", [&g](NativeCall& c) {  // @9e5900: GameInfo.player[get_player()].name
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.leaders[size_t(w)].name = bhs::to_utf8(c.arg_str(1));
		return make_int(1);
	});
	def("declare_war", [&g](NativeCall& c) {  // @9fbe80: Leader::action_declare(whom, 0, 1, 0); chat_status both ways
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		leader_action_declare(g, a, b, 0, 1, 0);
		g.leaders[size_t(a)].chat_status[size_t(b)] = 1;
		g.leaders[size_t(b)].chat_status[size_t(a)] = 1;
		return make_int(1);
	});

	// ---- units
	// ScenarioFuncSet::add_unit @9e2220 (create_unit, create_unit_upgrade with the current upgrade, create_unit_in_group
	// without clearing): returns the last init_unit result (-1 when the last creation failed), -1 on bad arguments.
	auto add_unit = [&g](int32_t player, int32_t x, int32_t y, int32_t type, int32_t n, bool append) {
		const int32_t w = player - 1;
		if (!live_leader(g, w) || uint32_t(n) > 2000u) return -1;
		if (type < 0 || !is_unit_type(type)) return -1;  // TypeData::is_unit_type (50..413)
		if (!g.world.in_w(x >> 2, y >> 2)) return -1;  // WorldData::valid
		const int32_t cx = tile_centre(x), cy = tile_centre(y);
		const int32_t t = get_graft(g, w, type);
		Group grp;
		group_clear(grp, -1);
		grp.id = -1;
		// clear_group(&who) with the leader index (the default group of player who - 1), add_to_group(&who + 1, o):
		// the original clears one group and fills the next (faithful).
		if (!append) {
			const int32_t gi = g.scen.find_group(unnamed(w));
			if (gi >= 0) g.scen.groups.erase(g.scen.groups.begin() + gi);
		}
		if (type_is(g.type(t), ptype::kTransportBarge)) return -1;
		const Type& T = g.type(t);
		const bool ocean = World::is_ocean(g.world.w(x >> 2, y >> 2));
		const int32_t dom = T.domain;
		int32_t last = -1;
		auto add_named = [&g, w](int32_t o) {  // add_to_group(name "_Unnamed<who>", who, o) @9fa120
			const U16 name = unnamed(w + 1);
			int32_t gi = g.scen.find_group(name);
			if (gi < 0) {
				g.scen.groups.push_back(ScenarioData::Group{name, w, {}, 0});
				gi = int32_t(g.scen.groups.size()) - 1;
			}
			const Unit* u = valid_unit(g, w, o);
			if (!u) return;
			auto& ids = g.scen.groups[size_t(gi)].ids;
			if (std::find(ids.begin(), ids.end(), o) == ids.end()) ids.push_back(o);
		};
		const bool normal = (dom == 0 && !ocean) || (dom == 1 && ocean) || dom == 2;
		if (!normal) {
			if (dom != 0) return last;  // ships on land: nothing
			const Leader& L = g.leaders[size_t(w)];
			if (!(L.flags & (leaderflag::kCanTransportCiv | leaderflag::kCanTransportMil | leaderflag::kCanTransportSct)))
				return last;
			for (int32_t k = 0; k < n; k++) {
				last = init_unit(g, w, t, cx, cy);
				if (last >= 0) group_add(g, grp, last, w, 0, 0);
			}
			if (group_get_num(g, grp) == 0) return last;
			groups_push_group(g, w, grp, 0);
			group_action_transport(g, grp, 1);  // the units board a new transport (on the local group, as the original)
			for (int32_t i = 0; i < grp.num; i++) {
				int32_t o = grp.list[size_t(i)];
				const Unit* u = unit_at(g, w, o);
				if (u && u->inside_up >= 0) o = u->inside_up;  // +0x82: the transport
				add_named(o);
			}
			group_action_form(g, grp, 0, 0, kQueueNew, 0);
			group_dbg_jump_to_action(g, grp);
			return last;
		}
		for (int32_t k = 0; k < n; k++) {
			int32_t ox = cx, oy = cy;
			find_nearby_spot(g, T, cx, cy, &ox, &oy, 0, 0xc00, 0, 0x55555555u, kFilterNotMe, -1, -1, 0, 0, -1, 0, -1);
			last = init_unit(g, w, t, ox, oy);
			if (last < 0) continue;
			group_add(g, grp, last, w, 0, 0);
			add_named(last);
			if (T.domain == kDomainAir && !(T.unit_flags & utflag::kHelicopter)) {
				// A plane flies at once: a no-target STRAFE (nearest base) and the guy at terrain z + 1000.
				if (Unit* u = unit_at(g, w, last)) {
					clear_orders(g, *u);
					add_strafe_order(g, *u, -1, -1, -1, -1, 1, kQueueNew, false);
					Guy& gy = u->guys[0];
					gy.z = gy.last_z = find_tcoord_z_at(g, u->x, u->y, 0) + 1000;
				}
			}
			if (type_is(T, airtype::kCarrier, false)) {
				if (Unit* u = unit_at(g, w, last)) carrier_new_wing(g, *u);  // action_unqueue(1) + limit x FIGHTERBOMBER
			}
		}
		if (group_get_num(g, grp) == 0) return last;
		if (T.domain == kDomainAir) return last;
		groups_push_group(g, w, grp, 0);
		group_action_form(g, grp, 0, 0, kQueueNew, 0);
		group_dbg_jump_to_action(g, grp);
		return last;
	};
	def("create_unit", [&g, add_unit](NativeCall& c) {  // @9f4c50
		return make_int(add_unit(c.arg_int(0), c.arg_int(1), c.arg_int(2), type_index(g, c.arg_str(3)), c.arg_int(4), false));
	});
	def("create_missile_and_target_build", [&g, add_unit](NativeCall& c) {  // @9f4fd0
		const int32_t w = c.arg_int(0) - 1, tw = c.arg_int(4) - 1;
		if (!live_leader(g, w) || !live_leader(g, tw)) return make_int(-1);
		const int32_t t = type_index(g, c.arg_str(3));
		if (t < 0 || !is_unit_type(t) || !(uint32_t(g.type(t).obj_masks) & 0x8000000u)) return make_int(-1);  // is_missile
		if (!valid_build(g, tw, c.arg_int(5))) return make_int(-1);
		if (!g.world.in_w(c.arg_int(1) >> 2, c.arg_int(2) >> 2)) return make_int(-1);
		// add_unit(missile, 1) (indexed with the new unit's o), a mandatory STRAFE at the building (a missile turns it into
		// an AIR_ATTACK_GROUND at the building's position) and one Unit::process (vslot 0x9c).
		const int32_t o = add_unit(c.arg_int(0), c.arg_int(1), c.arg_int(2), t, 1, false);
		if (Unit* u = unit_at(g, w, o)) {
			add_strafe_order(g, *u, c.arg_int(5), tw, -1, -1, 1, kQueueNew, false);
			unit_process(g, *u);
		}
		return make_int(1);
	});
	auto upgraded = [&g](NativeCall& c) {
		const int32_t t = type_index(g, c.arg_str(3));
		const int32_t w = c.arg_int(0) - 1;
		if (t < 0 || !valid_leader(g, w)) return -1;
		return current_upgrade(g, w, t);
	};
	def("create_unit_upgrade", [add_unit, upgraded](NativeCall& c) {  // @a030e0
		const int32_t t = upgraded(c);
		if (t < 0) return make_int(-1);
		return make_int(add_unit(c.arg_int(0), c.arg_int(1), c.arg_int(2), t, c.arg_int(4), false));
	});
	def("create_unit_in_group", [add_unit, upgraded](NativeCall& c) {  // @9f4c80 (current upgrade, appends)
		const int32_t t = upgraded(c);
		if (t < 0) return make_int(-1);
		return make_int(add_unit(c.arg_int(0), c.arg_int(1), c.arg_int(2), t, c.arg_int(4), true));
	});
	def("kill_unit_type", [&g](NativeCall& c) {  // @9f6d70: every live unit slot of the type ("" = all), r % 3 + 1
		const int32_t w = c.arg_int(0) - 1;
		int32_t t = -1;
		if (!c.arg_str(1).empty()) {
			t = type_index(g, c.arg_str(1));
			if (t < 0) return make_int(-1);
		}
		if (!valid_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int32_t o = 0; o < g.objects[size_t(w)].unit_mark; o++) {
			Unit* u = unit_at(g, w, o);
			if (!u || !(u->flags & objflag::kValid)) continue;
			if (t >= 1 ? !type_is(*u->ptype, t) : t >= 0) continue;
			const int32_t r = g.rng.get(0, 0xffff);
			object_die(g, *u, r % 3 + 1, -1, 0.f);
			n++;
		}
		return make_int(n);
	});
	def("kill_unit", [&g](NativeCall& c) {  // @9f6b20: the whole squad, one draw per member
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		if (!active_unit_slot(g, w, o)) return make_int(-1);
		for (int guard = 0; guard < kUnitLimit; guard++) {
			o = get_captain(g, w, o);
			const int32_t r = g.rng.get(0, 0xffff);
			if (Unit* u = unit_at(g, w, o)) object_die(g, *u, r % 3 + 1, -1, 0.f);
			const Unit* v = unit_at(g, w, o);
			if (!live_leader(g, w) || !v || !((v->flags & objflag::kValid) || v->o_up >= 0)) break;
		}
		return make_int(1);
	});
	def("kill_unit_anim", [&g](NativeCall& c) {  // @9f6c70: death type anim - 1, no draw
		const int32_t w = c.arg_int(0) - 1, anim = c.arg_int(2);
		int32_t o = c.arg_int(1);
		if (!active_unit_slot(g, w, o) || uint32_t(anim - 1) >= 4u) return make_int(-1);
		for (int guard = 0; guard < kUnitLimit; guard++) {
			o = get_captain(g, w, o);
			if (Unit* u = unit_at(g, w, o)) object_die(g, *u, anim - 1, -1, 0.f);
			if (!active_unit_slot(g, w, o)) break;
		}
		return make_int(1);
	});

	// ---- objects
	def("set_object_health", [&g](NativeCall& c) {  // @9f5aa0
		const int32_t w = c.arg_int(0) - 1, pct = c.arg_int(2);
		int32_t o = c.arg_int(1);
		if (!valid_object(g, w, o) || pct < 0 || pct > 100) return make_int(-1);
		o = captain_of(g, w, o);
		Object* ob = object_at(g, w, o);
		const int32_t hits = obj_hits(g, *ob);
		const int32_t d = obj_hits(g, *ob) - int32_t((float(pct) / 100.0f) * float(hits));
		if (d < ob->damage) {
			if (ob->is_unit) unit_repair_damage(g, *static_cast<Unit*>(ob), ob->damage - d, false, true);
			else {
				ob->damage = std::min(ob->damage, hits);
				ob->damage_frac = 0;
				ob->damage = std::max(0, ob->damage - (ob->damage - d));
			}
			return make_int(1);
		}
		take_damage(g, *ob, d - ob->damage, 0, 0, -1, true, 0x55555555u, -1, -1, false);
		return make_int(1);
	});
	def("heal_object", [&g](NativeCall& c) {  // @9f5cb0: repair_damage(pct% of hits)
		const int32_t w = c.arg_int(0) - 1, pct = c.arg_int(2);
		int32_t o = c.arg_int(1);
		if (!valid_object(g, w, o) || pct < 0 || float(pct) > 100.0f) return make_int(-1);
		o = captain_of(g, w, o);
		Object* ob = object_at(g, w, o);
		const int32_t amount = int32_t((float(pct) / 100.0f) * float(obj_hits(g, *ob)));
		if (ob->is_unit) {
			unit_repair_damage(g, *static_cast<Unit*>(ob), amount, false, false);
		} else {  // Build::repair_damage -> Object::repair_damage
			const int32_t h0 = obj_hits(g, *ob);
			if (h0 < ob->damage) ob->damage = h0;
			ob->damage_frac = 0;
			ob->damage = ob->damage <= amount ? 0 : ob->damage - amount;
		}
		return make_int(1);
	});
	def("damage_object", [&g](NativeCall& c) {  // @9f5bd0: take_damage(pct% of hits)
		const int32_t w = c.arg_int(0) - 1, pct = c.arg_int(2);
		int32_t o = c.arg_int(1);
		if (!valid_object(g, w, o) || pct < 0 || pct > 100) return make_int(-1);
		o = captain_of(g, w, o);
		Object* ob = object_at(g, w, o);
		const int32_t hp = int32_t((float(pct) / 100.0f) * float(obj_hits(g, *ob)));
		take_damage(g, *ob, hp, 0, 0, -1, true, 0x55555555u, -1, -1, false);
		return make_int(1);
	});
	def("toggle_infinite_queue", [&g](NativeCall& c) {  // @9f45e0: BuildData::can_infinite, BUILDMASK_INFINITE
		const int32_t w = c.arg_int(0) - 1, o = c.arg_int(1);
		Build* b = valid_build(g, w, o);
		if (!b || !is_training_building(g, b->type)) return make_int(-1);
		bool can = false;  // BuildData::can_infinite @62d4d0: a queued item the leader may make
		for (int32_t i = 0; i < int32_t(uint8_t(b->queued)) && !can; i++) {
			const int32_t t = b->build_queue.get_queue(i);
			can = t >= 0 && is_unit_type(t) && g.leaders[size_t(w)].has_tech_bit(t);
		}
		if (!can) return make_int(-1);
		group_action_buildmask(g, push_one(g, w, o), bmask::kInfinite, c.arg_int(2));
		return make_int(1);
	});
	def("force_transport_ability", [&g](NativeCall& c) {  // @9ffb00
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		Leader& l = g.leaders[size_t(w)];
		l.flags |= 0x700;  // CAN_TRANSPORT_CIV | _MIL | _SCT
		l.flags2 |= 0x20;
		for (int32_t o = 0; o < g.objects[size_t(w)].unit_mark; o++) {
			Unit* u = unit_at(g, w, o);
			if (!u || !(u->flags & objflag::kValid)) continue;
			// UnitData::can_ever_transport @46f290: land units, and ships that carry and are not carriers.
			const Type& T = *u->ptype;
			const bool ever = T.domain == 0 || (T.carry != 0 && !type_is(T, ptype::kCarrier));
			if (ever) u->unit_masks |= umask::kCanTransport;
		}
		return make_int(1);
	});
	def("field_battle_enable", [&g](NativeCall&) {  // @9e66c0: semaphore byte2 |= 0x10
		g.field_battle = true;
		return make_int(1);
	});
	def("gain_next_age", [&g](NativeCall& c) {  // @9ee4a0: Leader::set_age(ages + 1)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		const int32_t a = g.leaders[size_t(w)].age + 1;
		if (a >= 8) return make_int(-1);
		leader_set_age(g, w, a);
		return make_int(1);
	});
	def("lose_current_age", [&g](NativeCall& c) {  // @9ee6a0: Leader::set_age(ages - 1) (the original tests the age against 0x220, never equal)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		leader_set_age(g, w, g.leaders[size_t(w)].age - 1);
		return make_int(1);
	});
	def("lose_upgrade", [&g](NativeCall& c) {  // @9eed00: lose_tech(type) then the stat and territory recomputation
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(1));
		if (t < 0 || !live_leader(g, w) || !has_tech(g, w, t)) return make_int(-1);
		leader_lose_tech(g, w, t);
		calc_unit_stats(g, w);
		calc_wall_stats(g, w);
		compute_all_territory(g);
		return make_int(has_tech(g, w, t) ? 0 : 1);
	});
	def("lose_tech", [&g](NativeCall& c) {  // @9ee500: a tech, or a category name (the current level - 1)
		const int32_t w = c.arg_int(0) - 1;
		const U16 name = c.arg_str(1);
		int32_t t = type_index(g, name);
		if (t < 0) {
			bool found = false;
			t = category_tech(g, w, name, -1, &found);
			if (t == -2) return make_int(-1);
		}
		if (t < 0 || !live_leader(g, w) || !is_tech_type(t)) return make_int(-1);
		if (!has_tech(g, w, t)) return make_int(-1);
		leader_lose_tech(g, w, t);
		return make_int(1);
	});
	def("switch_teams", [&g](NativeCall& c) {  // @9f46b0
		const int32_t to = c.arg_int(0) - 1, from = c.arg_int(1) - 1;
		int32_t o = c.arg_int(2);
		if (!live_leader(g, to) || !valid_object(g, from, o) || to == from) return make_int(-1);
		Object* ob = object_at(g, from, o);
		if (!ob->is_unit) {
			Build& b = *static_cast<Build*>(ob);
			if (!(b.flags & objflag::kActive)) {  // an unfinished building is destroyed (Wall::die(0, -1, 0))
				object_die(g, b, 0, -1, 0.f);
				return make_int(-1);
			}
			if (b.flags & objflag::kCity) {
				const int32_t ni = cities_capture_city(g, to, b.city, from);
				if (ni < 0 || size_t(ni) >= g.cities[size_t(to)].size()) return make_int(-1);
				return make_int(g.cities[size_t(to)][size_t(ni)].o);
			}
			const int32_t no = build_swap_team(g, b, to);
			if (no >= 0) {
				if (Build* nb = build_at(g, to, no)) build_activate_full(g, *nb, true, true, false);
				build_close(g, b, 0);
				if (Build* nb = build_at(g, to, no)) wall_mask_me(g, *nb, true, Regen::Total);
			}
			return make_int(no);
		}
		o = get_captain(g, from, o);
		Unit* u = unit_at(g, from, o);
		return make_int(u ? unit_swap_team(g, *u, to) : -1);
	});
	def("train_unit_at", [&g](NativeCall& c) {  // @9f4190: queue n of the grafted current upgrade (no cost check)
		const int32_t w = c.arg_int(0) - 1, n = c.arg_int(1), o = c.arg_int(3);
		if (n < 1 || n > 2000 || !live_leader(g, w)) return make_int(-1);
		const int32_t t0 = type_index(g, c.arg_str(2));
		if (t0 < 0) return make_int(-1);
		const int32_t t = get_graft(g, w, current_upgrade(g, w, t0));
		Build* b = valid_build(g, w, o);
		if (!b || !build_could_queue(g, *b, t)) return make_int(-1);
		group_action_queue_up(g, push_one(g, w, o), t, n);
		return make_int(o);
	});

	// ---- named groups (ScenarioData::scenario_groups)
	def("add_to_group", [&g](NativeCall& c) {  // @4ce5f0 (int who, int o): "_Unnamed<who>"
		const int32_t player = c.arg_int(0), o = c.arg_int(1);
		const U16 name = unnamed(player);
		int32_t gi = g.scen.find_group(name);
		if (gi < 0) {
			g.scen.groups.push_back(ScenarioData::Group{name, player - 1, {}, 0});
			gi = int32_t(g.scen.groups.size()) - 1;
		}
		if (!valid_unit(g, player - 1, o)) return make_int(-1);
		auto& ids = g.scen.groups[size_t(gi)].ids;
		if (std::find(ids.begin(), ids.end(), o) != ids.end()) return make_int(0);
		ids.push_back(o);
		return make_int(1);
	});
	def("add_to_group", [&g](NativeCall& c) {  // @9fa120 (string name, int who, int o)
		const U16 name = c.arg_str(0);
		const int32_t w = c.arg_int(1) - 1, o = c.arg_int(2);
		int32_t gi = g.scen.find_group(name);
		if (gi < 0) {
			g.scen.groups.push_back(ScenarioData::Group{name, w, {}, 0});
			gi = int32_t(g.scen.groups.size()) - 1;
		}
		if (!valid_unit(g, w, o)) return make_int(-1);
		auto& ids = g.scen.groups[size_t(gi)].ids;
		if (std::find(ids.begin(), ids.end(), o) != ids.end()) return make_int(0);
		ids.push_back(o);
		return make_int(1);
	}, 1);
	// remove_from_group @4ce560 (int who, int o: "_Unnamed<who>") / @9fa2a0 (name, who, o): the group must belong to
	// that leader; SimpleArray<int>::remove drops the first match and keeps the order. 1, or -1.
	auto remove_named = [&g](const U16& name, int32_t w, int32_t o) {
		const int32_t gi = g.scen.find_group(name);
		if (gi < 0 || g.scen.groups[size_t(gi)].who != w) return make_int(-1);
		auto& ids = g.scen.groups[size_t(gi)].ids;
		auto it = std::find(ids.begin(), ids.end(), o);
		if (it != ids.end()) ids.erase(it);
		return make_int(1);
	};
	def("remove_from_group", [remove_named](NativeCall& c) {
		return remove_named(unnamed(c.arg_int(0)), c.arg_int(0) - 1, c.arg_int(1));
	});
	def("remove_from_group", [remove_named](NativeCall& c) {
		return remove_named(c.arg_str(0), c.arg_int(1) - 1, c.arg_int(2));
	}, 1);
	auto clear_named = [&g](const U16& name) {  // clear_group @4ce470 / @9fa350
		const int32_t gi = g.scen.find_group(name);
		if (gi < 0) return make_int(-1);
		g.scen.groups.erase(g.scen.groups.begin() + gi);
		return make_int(1);
	};
	def("clear_group", [clear_named](NativeCall& c) { return clear_named(unnamed(c.arg_int(0))); });
	def("clear_group", [clear_named](NativeCall& c) { return clear_named(c.arg_str(0)); }, 1);

	// ---- fog of war (World::set_seen with detect, fog cell = tile >> 1)
	def("set_explored", [&g](NativeCall& c) {  // @9e44b0: (who, x, y, r); who 0 = every valid leader 0..7
		const int32_t w = c.arg_int(0) - 1;
		const int32_t x = c.arg_int(1), y = c.arg_int(2), r = c.arg_int(3);
		const int32_t xs = g.world.tile_xs, ys = g.world.tile_ys;
		if (x < 0 || y < 0 || x >= xs || y >= ys || r < 0) return make_int(-1);
		if (w >= 0 && !live_leader(g, w)) return make_int(-1);
		const int32_t x0 = std::max(0, x - r), y0 = std::max(0, y - r);
		const int32_t x1 = std::min(xs - 1, x + r), y1 = std::min(ys - 1, y + r);
		for (int32_t tx = x0; tx <= x1; tx++) {
			for (int32_t ty = y0; ty < y1; ty++) {  // (the last row is skipped: `<` against the clamped bound)
				// The squared distance is compared with the radius itself (the sqrtf result is dropped).
				const float dy = float(ty - y), dx = float(tx - x);
				if (!(dy * dy + dx * dx < float(r))) continue;
				if (w < 0) {
					for (int32_t k = 0; k < 8; k++) {
						if (g.leaders[size_t(k)].flags & leaderflag::kValid) fog_set_seen(g.world, tx >> 1, ty >> 1, k, true);
					}
				} else {
					fog_set_seen(g.world, tx >> 1, ty >> 1, w, true);
				}
			}
		}
		return make_int(1);
	});
	auto reveal_flag = [&g](NativeCall& c, bool on) {  // set_explored(who) @9e46f0 / show_all_map_* @9e4a30
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		Leader& l = g.leaders[size_t(w)];
		if (on) l.flags |= leaderflag::kRevealEnemy;
		else l.flags &= ~leaderflag::kRevealEnemy;
		g.world.fog_changes++;  // Camera::outdate, fog.flags |= 2 (redraw)
		return make_int(1);
	};
	def("set_explored", [reveal_flag](NativeCall& c) { return reveal_flag(c, true); }, 1);
	def("show_all_map_enable", [reveal_flag](NativeCall& c) { return reveal_flag(c, true); });
	def("show_all_map_disable", [reveal_flag](NativeCall& c) { return reveal_flag(c, false); });
	def("set_explored_show_buildings", [&g](NativeCall& c) {  // @a03300
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.leaders[size_t(w)].flags |= leaderflag::kExploreMap;
		for (int32_t k = 0; k < 8; k++) {
			if (!(g.leaders[size_t(k)].flags & leaderflag::kValid)) continue;
			for (int32_t o = kBuildBase; o < g.objects[size_t(k)].build_mark; o++) {
				Build* b = build_at(g, k, o);
				if (!b || !(b->flags & objflag::kValid)) continue;
				b->ever_seen = uint8_t(b->ever_seen | (1u << w));
				b->ever_seen_completed = uint8_t(b->ever_seen_completed | (1u << w));
			}
		}
		update_all_seen(g);
		return make_int(1);
	});
	def("add_reveal_point", [&g](NativeCall& c) {  // @9e4750
		const int32_t w = c.arg_int(0) - 1;
		const int32_t x = c.arg_int(1), y = c.arg_int(2), r = c.arg_int(3);
		if (!live_leader(g, w) || !tile_ok(g, x, y) || r <= 0) return make_int(-1);
		g.reveal_points[size_t(w)].push_back(Game::RevealPoint{x, y, r});
		return make_int(1);
	});
	def("clear_reveal_points", [&g](NativeCall& c) {  // @9e4810
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.reveal_points[size_t(w)].clear();
		return make_int(1);
	});
	def("set_seen", [&g](NativeCall& c) {  // @9e4860: the building's footprint + 2 tiles
		const int32_t w = c.arg_int(0) - 1;
		Build* b = valid_build(g, c.arg_int(1) - 1, c.arg_int(2));
		if (!b || !valid_leader(g, w)) return make_int(-1);
		const int32_t tx = tile_of(b->x), ty = tile_of(b->y);
		const int32_t hx = b->ptype->x_size >> 1, hy = b->ptype->y_size >> 1;
		for (int32_t x = tx - hx - 2; x <= tx + 2 + hx; x++) {
			for (int32_t y = ty - hy - 2; y < ty + hy + 2; y++) {
				if (tile_ok(g, x, y)) fog_set_seen(g.world, x >> 1, y >> 1, w, true);
			}
		}
		return make_int(1);
	});

	// ---- AI switches (leader_flags2: 2 unit AI, 4 production AI, 8 combat AI, 0x10 city AI; UNIT_AI_OFF per unit)
	auto l2 = [&g](NativeCall& c, uint32_t bit, bool on) {  // @9ff5e0..9ff8e0
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		if (on) g.leaders[size_t(w)].flags2 |= bit;
		else g.leaders[size_t(w)].flags2 &= ~bit;
		return make_int(1);
	};
	def("disable_all_unit_ai", [l2](NativeCall& c) { return l2(c, 2, true); });
	def("enable_all_unit_ai", [l2](NativeCall& c) { return l2(c, 2, false); });
	def("disable_production_ai", [l2](NativeCall& c) { return l2(c, 4, true); });
	def("enable_production_ai", [l2](NativeCall& c) { return l2(c, 4, false); });
	def("disable_combat_ai", [l2](NativeCall& c) { return l2(c, 8, true); });
	def("enable_combat_ai", [l2](NativeCall& c) { return l2(c, 8, false); });
	def("disable_city_ai", [&g](NativeCall& c) {  // @9ffbf0: live, not human
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w) || (g.leaders[size_t(w)].flags & leaderflag::kHuman)) return make_int(-1);
		g.leaders[size_t(w)].flags2 |= 0x10;
		return make_int(1);
	});
	auto unit_ai = [&g](NativeCall& c, bool off) {  // disable_unit_ai @9ffa10 / enable_unit_ai @9ff920
		const int32_t w = c.arg_int(0) - 1;
		int32_t o = c.arg_int(1);
		if (active_unit_slot(g, w, o)) {
			o = get_captain(g, w, o);
			for (int guard = 0; o >= 0 && guard < kUnitLimit; guard++) {
				Unit* u = unit_at(g, w, o);
				if (!u) break;
				if (off) u->unit_masks |= umask::kAiOff;
				else u->unit_masks &= ~umask::kAiOff;
				o = u->o_down;
			}
			return make_int(1);
		}
		if (valid_leader(g, w) && o < 0) {
			if (off) g.leaders[size_t(w)].flags2 |= 2;
			else g.leaders[size_t(w)].flags2 &= ~2u;
			return make_int(1);
		}
		return make_int(-1);
	};
	def("disable_unit_ai", [unit_ai](NativeCall& c) { return unit_ai(c, true); });
	def("enable_unit_ai", [unit_ai](NativeCall& c) { return unit_ai(c, false); });
	def("set_auto_peasant_level", [&g](NativeCall& c) {  // @9ff620: the local leader's citizens and worker buildings
		if (mp(g)) return make_int(-1);
		const int32_t lv = c.arg_int(0);
		if (lv < 0 || lv >= 4) return make_int(-1);
		const int32_t w = g.local_play;
		if (uint32_t(w) >= 8) return make_int(-1);
		Leader& l = g.leaders[size_t(w)];
		if (lv != l.opt_peasants) {
			for (int32_t o = 0; o < g.objects[size_t(w)].unit_mark; o++) {
				Unit* u = unit_at(g, w, o);
				if (!u || !(u->flags & objflag::kValid) || (u->type != 50 && u->type != 51)) continue;
				// Unit::set_stance(lv, 0): the stance of the whole chain from the captain.
				Unit* q = unit_at(g, w, get_captain(g, w, o));
				for (int guard = 0; q && guard < 64; guard++) {
					q->stance = int8_t(lv);
					if (q->o_down < 0) break;
					q = unit_at(g, w, q->o_down);
					if (q && !(q->flags & objflag::kValid)) break;
				}
			}
			for (int32_t o = kBuildBase; o < g.objects[size_t(w)].build_mark; o++) {
				Build* b = build_at(g, w, o);
				if (b && (b->flags & objflag::kValid) && build_stance_type(g, b->type) == 1) b->stance = int8_t(lv);
			}
		}
		l.opt_peasants = lv;  // leader_options[who] +4
		l.worker_stance = int8_t(lv);
		return make_int(1);
	});

	// ---- cities, nations, diplomacy
	def("city_assimilate", [&g](NativeCall& c) {  // @9ef3c0: a captured city assimilates at once
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		const int32_t ci = city_index(g, w, c.arg_str(1));
		if (ci < 0 || !(g.cities[size_t(w)][size_t(ci)].city_flags & cityflag::kGotCaptured)) return make_int(0);
		city_assimilate(g, w, ci);
		return make_int(1);
	});
	def("set_nation_name", [&g](NativeCall& c) {  // @9ed230: the tribe whose (English) name matches
		const U16 from = c.arg_str(0), to = c.arg_str(1);
		if (from.empty() || to.empty()) return make_int(-1);
		for (Tribe& t : g.mut_rules().tribes) {
			if (!bhs::ieq(t.eng_name, from)) continue;
			if (t.old_name.empty()) t.old_name = t.name;
			t.name = to;
			return make_int(1);
		}
		return make_int(-1);
	});
	auto dset = [&g](NativeCall& c, uint8_t v) {  // diplomacy_set_wait @9fc570 (1) / _reject @9fc5f0 (3)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.scen_diplomacy_setting[size_t(w)] = v;
		return make_int(1);
	};
	def("diplomacy_set_wait", [dset](NativeCall& c) { return dset(c, 1); });
	def("diplomacy_set_reject", [dset](NativeCall& c) { return dset(c, 3); });
	def("diplomacy_block_war", [&g](NativeCall& c) {  // @9fc630 (read by the UI's declare-war option)
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		g.scen.war_blocked[size_t(a)][size_t(b)] = 1;
		return make_int(1);
	});
	def("accept_offer", [&g](NativeCall& c) {  // @9fc410: Leader::action_respond(asker, 1)
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b) || a == b) return make_int(-1);
		action_respond(g, a, b, 1);
		return make_int(1);
	});
	def("reject_offer", [&g](NativeCall& c) {  // @9fc490: respond 0 when dip[asker].agree == 1, else 2
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b) || a == b) return make_int(-1);
		action_respond(g, a, b, g.leaders[size_t(a)].dip[size_t(b)].agree == 1 ? 0 : 2);
		return make_int(1);
	});
	def("make_counter_offer", [&g](NativeCall& c) {  // @9fc850: clear_agree both ways and a chat line
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		const int32_t k = type_index(g, c.arg_str(2));
		if (!live_leader(g, a) || !live_leader(g, b) || a == b || uint32_t(k) >= 6) return make_int(-1);
		clear_agree(g, a, b);
		clear_agree(g, b, a);
		if (b == g.local_play) {  // Leader::chat_to_local
			ScriptEvent& e = push_event(g, ScriptEvent::kChat);
			e.text = bhs::to_utf8(c.arg_str(4)) + " " + std::to_string(c.arg_int(3)) + " " +
			         bhs::to_utf8(g.type(k).display_name);
			e.who = a;
		}
		return make_int(1);
	});
	def("make_peace", [&g](NativeCall& c) {  // @9fbf20: Leader::set_diplo(whom, 1)
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		leader_set_diplo(g, a, b, 1);
		return make_int(1);
	});
	def("set_time_limit", [&g](NativeCall& c) {  // @9e5c40: victory 4 only; time_limit setting 8 (custom)
		if (g.info.victory != 4 || c.arg_int(0) < 0) return make_int(-1);
		g.scen.custom_time_limit = c.arg_int(0);
		g.info.time_limit = 8;
		return make_int(1);
	});
	def("remove_infiltrated", [&g](NativeCall& c) {  // @9f9260 (who, o, spy's leader)
		const int32_t w = c.arg_int(0) - 1, p = c.arg_int(2) - 1;
		Object* ob = valid_object(g, w, c.arg_int(1));
		if (!live_leader(g, w) || !ob || !live_leader(g, p)) return make_int(-1);
		// The byte becomes the boolean "p's bit was clear" (`x = (x & bit) == 0`, the original's expression).
		ob->infiltrated = int8_t((uint8_t(ob->infiltrated) & (1u << p)) == 0 ? 1 : 0);
		return make_int(1);
	});
	def("remove_infiltrated", [&g](NativeCall& c) {  // @9f9300
		Object* ob = valid_object(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!ob) return make_int(-1);
		ob->infiltrated = 0;
		return make_int(1);
	}, 1);
	def("switch_building_type", [&g](NativeCall& c) {  // @9f4a00: every building of a type changes hands
		const int32_t to = c.arg_int(0) - 1, from = c.arg_int(1) - 1;
		if (!live_leader(g, to) || !valid_leader(g, from) || to == from) return make_int(-1);
		int32_t t = -1;
		if (!c.arg_str(2).empty()) {
			t = type_index(g, c.arg_str(2));
			if (t < 0) return make_int(-1);
		}
		int32_t n = 0;
		for (int32_t o = kBuildBase; o < g.objects[size_t(from)].build_mark; o++) {
			Build* b = build_at(g, from, o);
			if (!b || !(b->flags & objflag::kValid)) continue;
			if (!(b->flags & objflag::kActive)) {
				if (t >= 0) continue;
				// An unfinished building is closed (vfunc +0x150). The original then also runs the hand-over below on the
				// closed object (a copy of a dead building for `to`); the port stops here (deviation).
				build_close(g, *b, 0);
				continue;
			}
			if (t > 0 && !type_is(*b->ptype, t)) continue;
			if (!(b->flags & objflag::kCity)) {
				build_swap_team(g, *b, to);
				build_close(g, *b, 0);
				n++;
			} else {
				cities_capture_city(g, to, b->city, from);
				n++;
			}
		}
		return make_int(n);
	});

	// ---- buildings
	// create_building @9f4d30: a finished building of the type at the tile (blocked_site 0, OUTSIDE_TOWN or
	// OUTSIDE_RADIUS accepted), started and activated (with its free units / bonuses), territory when it is a city.
	auto create_building = [&g](int32_t w, int32_t x, int32_t y, int32_t t) {
		if (t < 0 || !is_build_type(t) || !live_leader(g, w)) return -1;
		const int32_t cx = tile_centre(x), cy = tile_centre(y);
		if (!tile_ok(g, x, y)) return -1;  // WorldData::is_valid
		const int32_t blk = blocked_site(g, t, cx, cy, -1, -1, nullptr);
		if (blk != 0 && blk != block::kOutsideTown && blk != block::kOutsideRadius) return -1;
		const int32_t o = init_build_site(g, w, t, cx, cy, false, -1);
		if (o >= 0) {
			if (Build* b = build_at(g, w, o)) {
				build_start(g, *b, true);
				build_activate_full(g, *b, false, true, true);
			}
		}
		if (type_is(g.type(t), kBuilds)) compute_all_territory(g);
		calc_unit_stats(g, w);
		calc_wall_stats(g, w);
		return o;
	};
	def("create_building", [&g, create_building](NativeCall& c) {
		return make_int(create_building(c.arg_int(0) - 1, c.arg_int(1), c.arg_int(2), type_index(g, c.arg_str(3))));
	});
	def("create_building_upgrade", [&g, create_building](NativeCall& c) {  // @9f4f00
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t = type_index(g, c.arg_str(3));
		if (t < 0 || !valid_leader(g, w) || !is_build_type(t)) return make_int(-1);
		return make_int(create_building(w, c.arg_int(1), c.arg_int(2), current_upgrade(g, w, t)));
	});
	def("create_building_near", [&g, create_building](NativeCall& c) {  // @a03160: the first free circle point
		const int32_t w = c.arg_int(0) - 1;
		const int32_t t0 = type_index(g, c.arg_str(3));
		if (t0 < 0 || !valid_leader(g, w) || !is_build_type(t0)) return make_int(-1);
		const int32_t t = current_upgrade(g, w, t0);
		const int32_t r = std::clamp(c.arg_int(4), 0, 0x40);
		const Circles& C = circles();
		for (int32_t i = 0; i < C.radius[size_t(r)]; i++) {
			const int32_t x = c.arg_int(1) + C.x[size_t(i)], y = c.arg_int(2) + C.y[size_t(i)];
			if (!tile_ok(g, x, y)) continue;
			if (blocked_site(g, t, tile_centre(x), tile_centre(y), -1, -1, nullptr) != 0) continue;
			const int32_t o = create_building(w, x, y, t);
			if (o >= 0) return make_int(o);
		}
		return make_int(-1);
	});
	def("destroy_building_type", [&g](NativeCall& c) {  // @9f7030: every (finished, of the type) building closed
		const int32_t w = c.arg_int(0) - 1;
		int32_t t = -1;
		if (!c.arg_str(1).empty()) {
			t = type_index(g, c.arg_str(1));
			if (t < 0) return make_int(-1);
		}
		if (!valid_leader(g, w)) return make_int(-1);
		int32_t n = 0;
		for (int32_t o = kBuildBase; o < g.objects[size_t(w)].build_mark; o++) {
			Build* b = build_at(g, w, o);
			if (!b || !(b->flags & objflag::kValid)) continue;
			bool kill = false;
			if (t < 1) kill = t < 0;  // "" = all; a type index 0 matches nothing
			else kill = (b->flags & objflag::kActive) && type_is(*b->ptype, t);
			if (!kill) continue;
			build_close(g, *b, 0);
			n++;
		}
		return make_int(n);
	});
	def("clear_build_queue", [&g](NativeCall& c) {  // @9f27e0: Build::clean_queue(refund)
		Build* b = valid_build(g, c.arg_int(0) - 1, c.arg_int(1));
		if (!b) return make_int(-1);
		build_clean_queue(g, *b, c.arg_int(2) > 0);
		return make_int(1);
	});
	def("set_capital", [&g](NativeCall& c) {  // @a00220
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		auto& list = g.cities[size_t(w)];
		const U16 name = c.arg_str(1);
		int32_t ci = -1;
		for (size_t k = 0; k < list.size() && ci < 0; k++) {
			if (list[k].valid() && bhs::ieq(bhs::to_u16(list[k].name), name)) ci = int32_t(k);
		}
		if (ci < 0) return make_int(-1);
		const bool tb23 = has_tribe_bonus(g, w, 0x17);
		for (City& ct : list) {
			if (!ct.valid()) continue;
			if (!tb23) {
				if (ct.city_flags & cityflag::kCapital) ct.city_flags = uint16_t(ct.city_flags & 0xffef);
			} else if ((ct.city_flags & 0x4010) == 0x4010) {
				ct.city_flags = uint16_t(ct.city_flags & 0xbfef);
			}
		}
		City& C = list[size_t(ci)];
		C.city_flags = uint16_t(C.city_flags | 0x4010);
		C.founder = int8_t(w);
		const Build* b = build_at(g, w, C.o);
		if (b && g.world.in_w(wcoord_of(b->x), wcoord_of(b->y)) && g.world.w(wcoord_of(b->x), wcoord_of(b->y)).region >= 0)
			regions_fix_all_borders(g);
		return make_int(1);
	});

	// ---- rules tables (continued)
	def("set_type_line_of_sight", [&g](NativeCall& c) {  // @a00550: los (clamped 0..64) on the line, then stats
		const int32_t los = std::clamp(c.arg_int(1), 0, 0x40);
		const int32_t t = type_index(g, c.arg_str(0));
		if (t < 0) return make_int(-1);
		for_line(g, t, false, [los](Type& T) {
			T.los = los;
			T.modified = 1;
		});
		for (int32_t w = 0; w < 8; w++) {
			if (!live_leader(g, w)) continue;
			calc_wall_stats(g, w);
			calc_unit_stats(g, w);
		}
		return make_int(1);
	});
	auto set_preq_n = [&g](NativeCall& c, int n) {  // set_preq @a01180 / @a01230 / @a01370
		int32_t t[4];
		for (int k = 0; k <= n; k++) t[k] = type_index(g, c.arg_str(size_t(k)));
		for (int k = 0; k <= n; k++) {
			if (t[k] < 0) return make_int(-1);
		}
		Type& T = g.mut_rules().types[size_t(t[0])];
		if (n == 1) {
			T.preq[0] = t[1];
			return make_int(1);
		}
		if (n == 2) {  // an epoch prerequisite goes first
			int32_t a = t[1], b = t[2];
			if (is_tech_type(b) && is_epoch_type(b)) std::swap(a, b);
			T.preq[0] = a;
			T.preq[1] = b;
			return make_int(1);
		}
		int32_t p1 = t[1], p2 = t[2], p3 = t[3];
		if (is_tech_type(p3) && is_epoch_type(p3)) std::swap(p2, p3);  // (preq3 <-> preq2)
		if (is_tech_type(p2) && is_epoch_type(p2)) std::swap(p1, p2);  // (preq2 <-> preq1)
		T.preq[0] = p1;
		T.preq[1] = p2;
		T.preq[2] = p3;
		return make_int(1);
	};
	def("set_preq", [set_preq_n](NativeCall& c) { return set_preq_n(c, 1); });
	def("set_preq", [set_preq_n](NativeCall& c) { return set_preq_n(c, 2); }, 1);
	def("set_preq", [set_preq_n](NativeCall& c) { return set_preq_n(c, 3); }, 2);

	// ---- leaders (continued)
	def("set_leader_difficulty", [&g](NativeCall& c) {  // @9e5350: LeaderData +0x50 (1..6)
		const int32_t w = c.arg_int(0) - 1, d = c.arg_int(1);
		if (d < 1 || d > 6 || !live_leader(g, w)) return make_int(-1);
		g.leaders[size_t(w)].difficulty = d;
		return make_int(1);
	});
	auto attrition = [&g](NativeCall& c, int32_t v) {  // disable / enable_building_attrition @a029c0 / @a02980
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.leaders[size_t(w)].disable_building_attrition = v;
		return make_int(1);
	};
	def("disable_building_attrition", [attrition](NativeCall& c) { return attrition(c, 1); });
	def("enable_building_attrition", [attrition](NativeCall& c) { return attrition(c, 0); });
	def("set_neutral_attrition", [&g](NativeCall& c) {  // @a02a00: LeaderData +0x800 (0..1000)
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.leaders[size_t(w)].neutral_attrition = std::clamp(c.arg_int(1), 0, 1000);
		return make_int(1);
	});
	def("set_bonus_cap", [&g](NativeCall& c) {  // @9fbd60: bonus_cap[good] = n (always returns -1)
		const int32_t w = c.arg_int(0) - 1;
		const int32_t k = type_index(g, c.arg_str(1));
		if (live_leader(g, w) && k >= 0 && k < 0x32) {
			if (k < 7) g.leaders[size_t(w)].bonus_cap[size_t(k)] = c.arg_int(2);
			// (k >= 7 writes past bonus_cap into the following leader fields in the original: not reproduced)
		}
		return make_int(-1);
	});
	def("make_alliance", [&g](NativeCall& c) {  // @9fbf90: set_diplo(2), chat_status 0 both ways
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b)) return make_int(-1);
		leader_set_diplo(g, a, b, 2);
		g.leaders[size_t(a)].chat_status[size_t(b)] = 0;
		g.leaders[size_t(b)].chat_status[size_t(a)] = 0;
		return make_int(1);
	});
	def("offer_peace", [&g](NativeCall& c) {  // @9fc1f0: Leader::action_treaty(whom, 1)
		const int32_t a = c.arg_int(0) - 1, b = c.arg_int(1) - 1;
		if (!live_leader(g, a) || !live_leader(g, b) || a == b) return make_int(-1);
		leader_action_treaty(g, a, b, 1);
		return make_int(1);
	});
	def("add_visibility", [&g](NativeCall& c) {  // @9fc710: ScenarioData::ally_mask and the leader's ally mask
		const int32_t w = c.arg_int(0) - 1, t = c.arg_int(1) - 1;
		if (!live_leader(g, w) || !live_leader(g, t)) return make_int(-1);
		g.scen_ally_mask[size_t(w)] = uint8_t(g.scen_ally_mask[size_t(w)] | (1u << t));
		g.leaders[size_t(w)].ally_mask = uint8_t(g.leaders[size_t(w)].ally_mask | (1u << t));
		update_all_seen(g);
		return make_int(1);
	});
	def("diplomacy_set_auto", [&g](NativeCall& c) {  // @9fc530
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.scen_diplomacy_setting[size_t(w)] = 0;
		return make_int(1);
	});
	def("unit_ignore_orders", [&g](NativeCall& c) {  // @a01750: ScenarioData::objects_ignoring_orders
		const int32_t w = c.arg_int(0) - 1;
		if (!live_leader(g, w)) return make_int(-1);
		g.scen.ignore_orders = 1;
		g.scen.objects_ignoring_orders[size_t(w)].push_back(c.arg_int(1));
		return make_int(1);  // the group actions kill the listed objects from their group (the ignore_orders prelude)
	});
	def("is_in_group", [&g](NativeCall& c) {  // @4ce4f0 (who, o): "_Unnamed<who>"
		const int32_t gi = g.scen.find_group(unnamed(c.arg_int(0)));
		if (gi < 0) return make_int(-1);
		const auto& gr = g.scen.groups[size_t(gi)];
		if (gr.who != c.arg_int(0) - 1) return make_int(-1);
		return make_int(std::find(gr.ids.begin(), gr.ids.end(), c.arg_int(1)) != gr.ids.end() ? 1 : 0);
	});
	def("is_in_group", [&g](NativeCall& c) {  // @9fa2e0 (name, who, o)
		const int32_t gi = g.scen.find_group(c.arg_str(0));
		if (gi < 0) return make_int(-1);
		const auto& gr = g.scen.groups[size_t(gi)];
		if (gr.who != c.arg_int(1) - 1) return make_int(-1);
		return make_int(std::find(gr.ids.begin(), gr.ids.end(), c.arg_int(2)) != gr.ids.end() ? 1 : 0);
	}, 1);

	// ---- ScenarioData switches (scripted rules / CTW only, else -1; @9e5500..9e57b0)
	auto flag_fn = [&g](bool* f, bool v, bool recompute) {
		return [&g, f, v, recompute](NativeCall&) {
			if (!scripted(g)) return make_int(-1);
			if (recompute) {  // buildings_gather_*: every leader recomputes its gather rates
				for (int32_t w = 0; w < 8; w++) g.leaders[size_t(w)].flags |= leaderflag::kRecompute;
			}
			*f = v;
			return make_int(1);
		};
	};
	def("buildings_gather_disable", flag_fn(&g.buildings_gather, false, true));
	def("buildings_gather_enable", flag_fn(&g.buildings_gather, true, true));
	def("building_unit_bonus_disable", flag_fn(&g.building_unit_bonus, false, false));
	def("building_unit_bonus_enable", flag_fn(&g.building_unit_bonus, true, false));
	def("building_resource_bonus_disable", flag_fn(&g.building_resource_bonus, false, false));
	def("building_resource_bonus_enable", flag_fn(&g.building_resource_bonus, true, false));
	def("unit_costs_disable", flag_fn(&g.scen.units_free, true, false));
	def("unit_costs_enable", flag_fn(&g.scen.units_free, false, false));
	def("building_costs_disable", flag_fn(&g.scen.buildings_free, true, false));
	def("building_costs_enable", flag_fn(&g.scen.buildings_free, false, false));
	def("tech_costs_disable", flag_fn(&g.scen.techs_free, true, false));
	def("tech_costs_enable", flag_fn(&g.scen.techs_free, false, false));
	// enable / disable_take_attrition @a02880 / @a028c0 (LeaderData +0x7fc) and enable / disable_give_attrition
	// @a02900 / @a02940 (+0x7f8): the flag only (Leader::calc_attrition / calc_anti_attrition read it at the next
	// LEADER_CALC_UNIT_STATS). Always -1.
	auto att_flag = [&g](NativeCall& c, int32_t Leader::*field, int32_t v) {
		const int32_t w = c.arg_int(0) - 1;
		if (live_leader(g, w)) g.leaders[size_t(w)].*field = v;
		return make_int(-1);
	};
	def("enable_take_attrition", [att_flag](NativeCall& c) { return att_flag(c, &Leader::take_att_disabled, 0); });
	def("disable_take_attrition", [att_flag](NativeCall& c) { return att_flag(c, &Leader::take_att_disabled, 1); });
	def("enable_give_attrition", [att_flag](NativeCall& c) { return att_flag(c, &Leader::give_att_disabled, 0); });
	def("disable_give_attrition", [att_flag](NativeCall& c) { return att_flag(c, &Leader::give_att_disabled, 1); });
	def("set_attrition_free_point", [&g](NativeCall& c) {  // @a02aa0: tcoords, radius in tiles; 1 or -1
		const int32_t w = c.arg_int(0) - 1;
		const int32_t x = c.arg_int(1), y = c.arg_int(2), r = c.arg_int(3);
		if (!live_leader(g, w) || !tile_ok(g, x, y) || r < 1) return make_int(-1);
		g.attrition_free_points[size_t(w)].push_back(Game::RevealPoint{x, y, r});
		return make_int(1);
	});
	def("clear_attrition_free_points", [&g](NativeCall& c) {  // @a02b60: always -1
		const int32_t w = c.arg_int(0) - 1;
		if (live_leader(g, w)) g.attrition_free_points[size_t(w)].clear();
		return make_int(-1);
	});
	def("set_age_diff_penalty", [&g](NativeCall& c) {  // @a01520: Constants one_age_down .. five_ages_down
		// The balance table was built from these at load (Balance::type_damage), so like the original this only changes
		// the constant.
		static constexpr uint32_t off[5] = {Constants::kOneAgeDown, Constants::kTwoAgesDown, Constants::kThreeAgesDown,
		                                    Constants::kFourAgesDown, Constants::kFiveAgesDown};
		const int32_t d = std::clamp(c.arg_int(0), 1, 5);
		g.mut_rules().constants.v[off[d - 1] / 4] = c.arg_int(1);
		return make_int(1);
	});
	def("change_to_human", [&g](NativeCall& c) {  // @9e5c80: HUMAN flag, live units lose AI control
		const int32_t w = c.arg_int(0) - 1;
		if (!valid_leader(g, w)) return make_int(-1);
		Leader& l = g.leaders[size_t(w)];
		if (l.flags & leaderflag::kHuman) return make_int(0);
		l.flags |= leaderflag::kHuman;
		for (int32_t o = 0; o < g.objects[size_t(w)].unit_mark; o++) {
			Unit* u = g.unit(w, o);
			if (u && u->alive()) u->unit_masks &= ~umask::kAiControl;
		}
		return make_int(1);
	});
}

}  // namespace sf
}  // namespace ron::sim
