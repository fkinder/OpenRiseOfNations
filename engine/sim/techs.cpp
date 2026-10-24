// The leader's tech state and availability predicates, Leader::init's tech part, Leader::gain_tech and the stat
// recalculation of Leaders::process_all (docs/production-spec.md section 7, 6.10, economy-spec 7.1).
#include <algorithm>

#include "sim/combat.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"
#include "sim/rules_internal.hpp"
#include "sim/transport.hpp"

namespace ron::sim {

namespace {

const Constants& C(const Game& g) { return g.rules->constants; }
int32_t cst(const Game& g, const char* name) {
	const int off = constant_offset(name);
	return off < 0 ? -1 : g.rules->constants.at(uint32_t(off));  // a constant missing from rules.xml reads -1
}
Leader& L(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
const Leader& L(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }
bool valid_who(int32_t who) { return who >= 0 && who < kNumLeaders; }
bool is_t(const Game& g, int32_t t, int32_t x, bool strict = false) {
	return t >= 0 && t < kNumTypes && type_is(g.type(t), x, strict);
}

}  // namespace

int32_t num_preq(int32_t t) {
	if (is_tech_type(t) || is_build_type(t)) return 3;
	if (is_unit_type(t) || is_good_type(t) || is_bonus_type(t)) return 2;
	return 1;
}

// ---------------------------------------------------------------- get_preq, has_tech, has_preq

int32_t get_preq(const Game& g, int32_t t, int32_t i, int32_t who) {
	if (t < 0 || t >= kNumTypes) return TYPE_NONE;
	const Type& o = g.type(t);
	const GameSetup s = g.info.setup();
	if (i == 1 && who >= 0 && who < 8) {
		// The Iroquois government rule (7.9.3): a gov's age preq in slot 1 moves one age earlier (dropped for Classical).
		const int32_t p = o.preq[1];
		if (p >= 0 && is_age_type(p) && is_gov_type(t) && has_tribe_bonus(g, who, 0x12) &&
		    cst(g, "iroquois_govs_early") != 0) {
			return is_age_type(p - 1) ? p - 1 : TYPE_NONE;  // the start / end remap only touches epochs
		}
	}
	return detail::get_preq(*g.rules, o, i, s, kEndGaia);
}

bool has_rare(const Game& g, int32_t who, int32_t good) {
	if (!valid_who(who) || good < 6 || good >= 6 + 64) return false;
	const int32_t b = good - 6;
	const Leader& l = L(g, who);
	return ((l.rare[size_t(b >> 3)] >> (b & 7)) & 1) || ((l.rare_conquest[size_t(b >> 3)] >> (b & 7)) & 1);
}

namespace {

int32_t special_preq(const Game& g, int32_t who, int32_t t, int32_t* p) {
	const int32_t q = *p;
	if (q < 0) return 0;
	if (uint32_t(t - 0x1b7) < 8 && has_tribe_bonus(g, who, 6) && cst(g, "roman_fort_early") != 0 && !is_age_type(q))
		return 1;
	if (is_unit_type(t) && has_tribe_bonus(g, who, 3) && !is_t(g, t, 0xe3, true) && !is_t(g, t, 0xbb, true) &&
	    is_epoch_type(q) && g.type(q).cat == 0)
		return 1;
	if (is_wonder_type(t) && is_age_type(q) && has_tribe_bonus(g, who, 7) && cst(g, "egyptian_wonders_early") != 0)
		*p = q == 0x220 ? -1 : q - 1;
	if (((t > 0x263 && t < 0x267) || (t > 0x260 && t < 0x264) || uint32_t(t - 0x26c) < 3) &&
	    cst(g, "german_industry_early") != 0 && has_tribe_bonus(g, who, 0xc) && is_epoch_type(q) &&
	    g.type(q).cat == 3) {
		if (q > 0x227) {
			*p = q - 1;
			return 0;
		}
		*p = -1;
	}
	return 0;
}

}  // namespace

bool has_tech(const Game& g, int32_t who, int32_t t) {
	if (t == TYPE_NONE) return true;
	if (t == TYPE_ANY_BUILDING) return false;
	if (t < 50) return true;
	if (t >= kNumTypes || !valid_who(who)) return false;
	if (is_unit_type(t) && !has_preq(g, who, t)) return false;
	if (is_build_type(t)) return has_preq(g, who, t);
	return L(g, who).has_tech_bit(t);
}

// LeaderData::has_preq @6db810 (7.9.2). Replaces the simplified fog version.
bool has_preq(const Game& g, int32_t who, int32_t type) {
	if (!valid_who(who) || type < 0 || type >= kNumTypes) return false;
	const int32_t t = type;
	auto tb = [&](int32_t n) { return has_tribe_bonus(g, who, n); };
	bool finals_check = false;
	switch (t) {
	case ptype::kMarket:
		if (tb(4)) return true;
		break;
	case ptype::kKnowledge:
		if (tb(5) && cst(g, "greek_knowledge_early") != 0) return true;
		break;
	case ptype::kUniversity:
		if (tb(5) && cst(g, "greek_university_early") != 0) return true;
		break;
	case ptype::kTower:
	case ptype::kFortX:
		if (tb(6) && cst(g, "roman_fort_early") != 0) return true;
		finals_check = true;
		break;
	case ptype::kGranary:
		if (tb(7) && cst(g, "egyptian_granary_early") != 0) return true;
		if (tb(0xc) && cst(g, "german_buildings_early") != 0) return true;
		break;
	case ptype::kLumberMill:
		if (tb(10) && cst(g, "french_lumbermill_early") != 0) return true;
		if (tb(0xc) && cst(g, "german_buildings_early") != 0) return true;
		break;
	case ptype::kRefinery:
		if (tb(0xd) && has_preq(g, who, ptype::kOilWell)) return true;
		break;
	case ptype::kMetal:
	case ptype::kMine:
	case ptype::kSmelter: {
		const bool ger = tb(0xc);
		if (ger && cst(g, "german_metal_early") != 0) return true;
		if (t != ptype::kSmelter) {
			finals_check = true;
			break;
		}
		if (ger && cst(g, "german_buildings_early") != 0) return true;
		break;
	}
	case ptype::kTemple:
		if (tb(0x10) && cst(g, "korean_temple_upgrades") != 0) return true;
		break;
	default:
		finals_check = true;
		break;
	}
	if (finals_check && uint32_t(t - ptype::kFinals) < 4) {
		return has_tech(g, who, ptype::kInformationAge) && has_tech(g, who, ptype::kComputerization) &&
		       has_tech(g, who, ptype::kGlobalization) && has_tech(g, who, ptype::kInternationalLaw) &&
		       has_tech(g, who, ptype::kSelectiveService);
	}
	const int32_t n = num_preq(t);
	for (int32_t i = 0; i < n; i++) {
		int32_t p = get_preq(g, t, i, who);
		if (p < 0) {
			if (!has_tech(g, who, p)) return false;
			continue;
		}
		if (special_preq(g, who, t, &p)) continue;
		if (p < 0) {
			if (!has_tech(g, who, p)) return false;
			continue;
		}
		if (!is_bonus_type(p)) {
			if (!has_tech(g, who, p)) return false;
			continue;
		}
		const int32_t q = get_preq(g, p, i, who);
		if (q < 0 || !is_bonus_type(q)) {
			if (get_govs_taken(g, who) < 2) return false;
			const int32_t p0 = g.type(p).preq[0];
			if ((p0 == 0x271 || p0 == 0x272) && !has_tech(g, who, 627) && !has_tech(g, who, 628))
				return false;
		} else if (get_govs_taken(g, who) < 3) {
			return false;
		}
		if (p == ptype::kBuySell && tb(4)) continue;
		if (!has_preq(g, who, p)) return false;
	}
	if (is_age_type(t) && L(g, who).epochs < techs_per_age(g, who, t)) return false;
	if (!is_gov_type(t) || t < 0x271) return true;
	if (has_tech(g, who, t - 2)) return true;
	return has_tech(g, who, ((t - ptype::kGovBase) ^ 1) + 621);
}

int32_t get_govs_taken(const Game& g, int32_t who) {
	int32_t n = 0;
	for (int32_t t = ptype::kGovBase; t < 629; t++) n += has_tech(g, who, t) ? 1 : 0;
	return n;
}

int32_t starting_age(const Game& g, int32_t who) {
	(void)who;
	const GameInfo& i = g.info;
	if (i.game_rules == 8 && i.team[size_t(std::clamp(who, 0, 7))] == 0) {
		const int32_t a = std::min<int32_t>(i.starting_technology, 7), b = std::min<int32_t>(i.starting_technology2, 7);
		return std::min<int32_t>(a + b, i.ending_technology);
	}
	return std::min<int32_t>(i.starting_technology, 7);
}

bool all_techs(const Game& g, int32_t who) {
	const GameInfo& i = g.info;
	if (i.game_rules == 8 && i.team[size_t(std::clamp(who, 0, 7))] == 0)
		return i.starting_technology == 8 || i.starting_technology2 == 8;
	return i.starting_technology == 8;
}

int32_t techs_per_age(const Game& g, int32_t who, int32_t t) {
	const GameInfo& i = g.info;
	const int32_t s = i.game_rules == 8 ? i.starting_technology : starting_age(g, who);
	const int32_t a = g.type(t).age;
	if (s == 0 && i.ending_technology > 6) return a * 4 + 2;
	const int32_t n = i.ending_technology - s + 1;
	if (n == 0) return 0;
	const int32_t r = (a - s + 1) * (28 / n) - 2;
	if (s == 0 && a == 0 && r == 1) return 2;
	return r;
}

int32_t compute_epoch(const Game& g, int32_t who, int32_t cat) {
	int32_t base = cat == 1 ? ptype::kCivic : cat == 2 ? ptype::kBarter : cat == 3 ? ptype::kWrittenWord : ptype::kMilitaryBase;
	int32_t n = 0;
	for (int32_t t = base; t < base + 7; t++) {
		if (!has_tech(g, who, t)) return n;
		n++;
	}
	return n;
}

int32_t tribe_can_type(const Game& g, int32_t who, int32_t t) {
	if (!valid_who(who) || t < 0 || t >= kNumTypes) return 0;
	const Leader& l = L(g, who);
	const Type& ty = g.type(t);
	if (is_unit_type(t)) {
		const bool barb = l.tribe >= 0 && size_t(l.tribe) < g.rules->tribes.size() &&
		                  g.rules->tribes[size_t(l.tribe)].barbarian != 0;
		if (((g.info.flags & 8) || barb) && (ty.unit_flags & 0x1000000)) return 0;
	}
	return ((uint32_t(ty.tribe_mask) >> (uint32_t(l.tribe) & 31)) & 1) ? 4 : 0;
}

namespace {
// Type vslot +0x68 type_graft(who): UnitTypeData::type_graft @61d790 for units 50..401, else the type itself.
int32_t type_graft(const Game& g, int32_t t, int32_t who) {
	if (t >= 50 && t < 402) {
		const int32_t tribe = L(g, who).tribe;
		if (tribe >= 0 && size_t(tribe) < g.rules->tribes.size()) return g.rules->tribes[size_t(tribe)].graft[size_t(t - 50)];
	}
	return t;
}
}  // namespace

int32_t get_graft(const Game& g, int32_t who, int32_t t) {
	if (t < 0) return t;
	if (tribe_can_type(g, who, t)) return t;
	return type_graft(g, t, who);
}

namespace {
// LeaderData::check_predecessor @6dbcb0.
bool check_predecessor(const Game& g, int32_t who, int32_t t) {
	while (t >= 0 && type_avail(g, who, t, 0) < 3) {
		if ((uint32_t(g.type(t).tribe_mask) >> (uint32_t(L(g, who).tribe) & 31)) & 1) return false;
		t = get_graft(g, who, g.type(t).from);
	}
	return true;
}
}  // namespace

int32_t type_eligible(const Game& g, int32_t who, int32_t t, int32_t check) {
	if (tribe_can_type(g, who, t) != 4) return 0;
	const Leader& l = L(g, who);
	if (t == ptype::kTown) {
		int32_t n = l.num_buildings[1];
		const int32_t to = g.type(ptype::kTown).to;
		if (to >= 0) n += get_buildings(g, who, to, l.num_buildings[size_t(to - kBuilds)]);
		if (n == 0 && get_queued(g, who, ptype::kTown, l.num_queued[ptype::kTown]) == 0) return 4;
	}
	const int32_t np = num_preq(t);
	for (int32_t i = 0; i < np; i++) {
		const int32_t p = get_preq(g, t, i, who);
		if (p < -1) return 0;
		if (p >= 0 && !is_plain_tech_type(p) && !is_epoch_type(p) && g.info.ending_technology < g.type(p).age) return 0;
	}
	const Type& ty = g.type(t);
	if (is_good_type(t)) {
		if (!check) return 4;
		return has_tech(g, who, ty.obs) ? 0 : 4;
	}
	if (is_unit_type(t)) {
		if ((t == ptype::kNuke || t == ptype::kIcbm) && g.info.game_rules == 11) return 0;
		if (check) {
			if (has_tech(g, who, ty.obs)) return 0;
			if (Leader::bit_of(l.obs_flags, t)) return 0;
			int32_t j = get_graft(g, who, ty.jump);
			while (j >= 0) {
				if (has_preq(g, who, j)) {
					if (l.has_tech_bit(j)) return 0;
					if ((g.type(j).unit_flags & 0x200) && !l.has_tech_bit(t) && !(ty.unit_flags & 0x80)) return 0;
				}
				j = g.type(j).jump;
				if (j < 0) break;
				if (!tribe_can_type(g, who, j)) j = type_graft(g, j, who);
			}
		}
		for (int32_t k = 0; k < 3; k++) {
			const int32_t p = ty.preq[size_t(k)];
			if (p >= 0 && is_age_type(p) && g.info.ending_technology < p - 0x21f) return 0;
		}
		if (ty.unit_flags & 0x200) return 4;
		int32_t f = ty.from;
		if (f >= 0 && !tribe_can_type(g, who, f)) f = type_graft(g, f, who);
		return check_predecessor(g, who, f) ? 4 : 0;
	}
	if (is_build_type(t)) {
		if (!check) return 4;
		if (has_tech(g, who, ty.obs)) return 0;
		return Leader::bit_of(l.obs_flags, t) ? 0 : 4;
	}
	if (!is_tech_type(t)) return 4;
	if ((ty.leader_off >> (uint32_t(who) & 31)) & 1) return 0;
	if (is_final_type(t) && (g.info.game_rules == 11 || (g.info.flags & 0x80000000u))) return 0;
	if (is_age_type(t) && g.info.ending_technology < ty.age + 1) return 0;
	if (is_gov_type(t)) {
		const int32_t partner = ((t - ptype::kGovBase) ^ 1) + ptype::kGovBase;
		if (has_tech(g, who, partner) || has_tech_queued(g, who, partner)) return 0;
	}
	return 4;
}

int32_t type_avail(const Game& g, int32_t who, int32_t t, int32_t check) {
	if (!has_preq(g, who, t)) return 0;
	const int32_t e = type_eligible(g, who, t, check);
	if (e != 4) return e;
	const Leader& l = L(g, who);
	if (is_unit_type(t)) {
		if (is_gov_hero_type(t) && (l.flags2 & 0x1000)) return 0;
		if (!l.has_tech_bit(t)) return 2;
		if (!is_t(g, t, ptype::kMachineGun)) return 4;
		for (int32_t base : {ptype::kRifleman, ptype::kInfantry, ptype::kMechInfantry}) {
			const int32_t gr = tribe_can_type(g, who, base) ? base : type_graft(g, base, who);
			if (l.has_tech_bit(gr)) return 4;
		}
		return 0;
	}
	if (!is_build_type(t)) return (is_gov_type(t) && (l.flags2 & 0x800)) ? 0 : 4;
	if (is_wonder_type(t) && g.wonders[size_t(t - 526)]) return 0;  // BuildTypeData::already_built @63ce10
	const Type& ty = g.type(t);
	if (ty.from >= 0 && (ty.build_flags & 4) && !l.has_tech_bit(t)) return 2;
	if (t == ptype::kSenate) {
		// The capital / existing-senate rules of @6e33a0 (find_capital, CityData::count_buildings(SENATE, 0, 1)).
		if (l.num_queued[ptype::kSenate] != 0) return 0;
		int32_t c = -1, w = -1;
		find_capital(g, who, &c, &w, -1, -1);
		if (c < 0) return 0;
		const int32_t c0 = c, w0 = w;
		if (w != who) {
			find_capital(g, who, &c, &w, c, w);
			if (c < 0 || w < 0 || w != who) return 0;
			if (city_count_buildings(g, w, c, ptype::kSenate, 0, 1) != 0) return 0;
			if (!(g.cities[size_t(w)][size_t(c)].city_flags & cityflag::kOriginalCapital)) return 0;
		}
		// (the decompile reads cities[who][c0] here even when c0 was another leader's capital)
		if (w0 == who && city_count_buildings(g, who, c0, ptype::kSenate, 0, 1) != 0) return 0;
		find_capital(g, who, &c, &w, c0, w0);
		if (c >= 0) {
			if (w == who) {
				if (city_count_buildings(g, w, c, ptype::kSenate, 0, 1) != 0) return 0;
			} else if (w0 == who && (g.cities[size_t(who)][size_t(c0)].city_flags & cityflag::kOriginalCapital)) {
				return 0;
			}
		}
	}
	return 4;
}

int32_t current_upgrade(const Game& g, int32_t who, int32_t t) {
	if (t < 0) return t;
	for (int guard = 0; guard < 1000; guard++) {
		if (is_unit_type(t) || is_build_type(t)) {
			int32_t j = g.type(t).jump;
			if (j >= 0) {
				if (!tribe_can_type(g, who, j)) j = type_graft(g, j, who);
				if (j >= 0 && has_preq(g, who, j) && L(g, who).has_tech_bit(j)) {
					t = j;
					continue;
				}
			}
		}
		int32_t u = g.type(t).upgrade;
		if (u < 0) return t;
		if (!tribe_can_type(g, who, u)) u = type_graft(g, u, who);
		if (u < 0) return t;
		if (type_avail(g, who, u, 1) < 3) {
			const int32_t v = current_upgrade(g, who, u);
			if (v < 0 || v == u) return t;
			return v;
		}
		t = u;
	}
	return t;
}

bool researching(const Game& g, int32_t who, int32_t t, int32_t bld, bool only_bld, bool exact) {
	if (is_build_type(t) && !(g.type(t).build_flags & 4)) only_bld = true;
	const ObjectList& OL = g.objects[size_t(who)];
	for (int32_t o = kBuildBase; o < OL.build_mark; o++) {
		const Build* b = g.objects[size_t(who)].builds[size_t(o - kBuildBase)].get();
		if (!b || !(b->flags & objflag::kActive)) continue;
		if (only_bld && o != bld) continue;
		for (int32_t i = 0; i < b->queued; i++) {
			const int32_t v = b->build_queue.get_queue(i);
			if (v == t) return true;
			if (!exact && is_unit_type(v) && is_unit_type(t) && !L(g, who).has_tech_bit(v) &&
			    !L(g, who).has_tech_bit(t) && is_t(g, t, v))
				return true;
		}
	}
	return false;
}

bool has_tech_queued(const Game& g, int32_t who, int32_t t) {
	if (t == TYPE_NONE || t == TYPE_ANY_BUILDING || !is_tech_type(t)) return false;
	if (L(g, who).has_tech_bit(t)) return false;
	const ObjectList& OL = g.objects[size_t(who)];
	for (int32_t o = kBuildBase; o < OL.build_mark; o++) {
		const Build* b = OL.builds[size_t(o - kBuildBase)].get();
		if (!b || !(b->flags & objflag::kActive)) continue;
		if (!type_is(*b->ptype, g.type(t).where)) continue;
		if (build_count_queue(g, *b, 1, t) != 0) return true;  // vslot 0x190 (1, t) = count_queue(TYPE, t) 🟡
	}
	return false;
}

int32_t get_first_library(const Game& g, int32_t who) {
	const ObjectList& OL = g.objects[size_t(who)];
	for (int32_t o = kBuildBase; o < OL.build_mark; o++) {
		const Build* b = OL.builds[size_t(o - kBuildBase)].get();
		if (!b || !b->alive() || !(b->flags & objflag::kActive) || b->city < 0) continue;
		if (is_unassimilated(g, *b)) continue;
		if (type_is(*b->ptype, ptype::kLibrary)) return o;
	}
	return -1;
}

int32_t get_building_cities(const Game& g, int32_t who) {
	int32_t n = 0;
	const Leader& l = L(g, who);
	for (int32_t c = 0; c < l.city_mark && size_t(c) < g.cities[size_t(who)].size(); c++) {
		const City& ct = g.cities[size_t(who)][size_t(c)];
		if (ct.valid() && ct.race == who && city_count_buildings(g, who, c, ptype::kLibrary, 0, 1) != 0) n++;
	}
	return n;
}

int32_t get_buildings(const Game& g, int32_t who, int32_t t, int32_t n) {
	const int32_t to = g.type(t).to;
	if (to >= 0 && to >= kBuilds && to < kEndBuilds && to != t)
		n += get_buildings(g, who, to, L(g, who).num_buildings[size_t(to - kBuilds)]);
	return n;
}

int32_t get_queued(const Game& g, int32_t who, int32_t t, int32_t base) {
	if (!is_unit_type(t)) return base;
	for (int32_t i = 0; i < 0x326; i++) {
		if (i == t) continue;
		if (get_graft(g, who, g.type(i).from) == t) base += get_queued(g, who, i, L(g, who).num_queued[size_t(i)]);
	}
	return base;
}

int32_t get_units(const Game& g, int32_t who, int32_t t, int32_t p) {
	int32_t n = (t >= kUnits && t < kEndUnits) ? L(g, who).num_units[size_t(t - kUnits)] : 0;
	if (p == 0) {
		for (int32_t i = kUnits; i < kEndUnits; i++) {
			if (i != t && get_graft(g, who, g.type(i).from) == t) n += get_units(g, who, i, 0);
		}
	}
	return n;
}

int32_t get_gov_hero(const Game& g, int32_t who) {
	const int32_t gov = L(g, who).gov;
	if (L(g, who).flags2 & 0x1000) return -1;
	for (int32_t u = kUnits; u < kGaia; u++) {
		if (!(g.type(u).unit_flags & 0x4000000)) continue;
		for (int32_t i = 0; i < 3; i++) {
			if (get_preq(g, u, i, -1) == gov) return u;
		}
	}
	return -1;
}

int32_t get_radius(const Game& g, int32_t who, int32_t type) {
	const int32_t t = g.type(type).type;
	const int32_t lvl = t == ptype::kTown ? 2 : (t == ptype::kMetropolis || t == ptype::kForbiddenCity) ? 3 : 1;
	int32_t r = (lvl - 1) * cst(g, "city_center_pop_radius");
	if (has_tribe_bonus(g, who, 0x15)) r += cst(g, "indians_city_radius");
	r += cst(g, "city_center_radius");
	return std::min(r, 64);
}

int32_t get_city_limit(const Game& g, int32_t who) {
	int32_t civic = L(g, who).epoch[1];
	if (L(g, who).tribe >= 0 && has_tribe_bonus(g, who, 3) && civic != 0) civic += cst(g, "bantu_city_limit");
	int32_t lim = civic + 1;
	if (has_wonder_prod(g, who, ptype::kPyramids)) lim += cst(g, "pyramids_city_limit");
	return lim;
}

int32_t get_total_cities(const Game& g, int32_t who) {
	// The loop over FROM-replaced types is guarded by types[414]->is_unit_type() and never runs.
	int32_t n = L(g, who).city_mine + L(g, who).num_queued[ptype::kVillage];
	if (has_wonder_prod(g, who, ptype::kForbiddenCity)) n--;
	return n;
}

int32_t get_wonders(const Game& g, int32_t who) {
	int32_t n = 0;
	const auto& W = g.wonder_records[size_t(who)];
	for (int32_t i = 0; i < L(g, who).wonder_mark && size_t(i) < W.size(); i++) n += (W[size_t(i)].wonder_flags & 1) ? 1 : 0;
	return n;
}

int32_t get_unbuilt_wonders(const Game& g, int32_t who) {
	int32_t n = 0;
	for (const Unbuilt& u : g.unbuilt_wonders[size_t(who)]) n += u.o >= 0 ? 1 : 0;
	return n;
}

namespace {
int32_t count_bonus(const Game& g, int32_t who, int32_t first) {
	int32_t n = 0;
	for (int32_t b = first; b < first + 3; b++) n += has_preq(g, who, b) ? 1 : 0;  // has_bonus (BUY_SELL rule n/a)
	return n;
}
}  // namespace
int32_t get_building_speed_upgrade(const Game& g, int32_t who) { return count_bonus(g, who, 754); }
int32_t get_troops_speed_upgrade(const Game& g, int32_t who) { return count_bonus(g, who, 742); }
int32_t get_ships_speed_upgrade(const Game& g, int32_t who) { return count_bonus(g, who, 748); }
int32_t get_vehicle_speed_upgrade(const Game& g, int32_t who) { return count_bonus(g, who, 760); }
int32_t get_building_hp_upgrade(const Game& g, int32_t who) { return count_bonus(g, who, 757); }

bool check_population(const Game& g, int32_t who, int32_t t) {
	return L(g, who).pop_cap < g.type(t).control_cost + L(g, who).control;
}

int32_t get_caravan_limit(const Game& g, int32_t who, int32_t p) {
	const Leader& l = L(g, who);
	int32_t v = l.epoch[2] + 1;
	if (has_wonder_prod(g, who, ptype::kColossus)) v += cst(g, "colossus_caravan");
	if (has_wonder_prod(g, who, ptype::kTajMahal)) v += cst(g, "taj_caravan");
	if ((l.rare[0] & 0x20) || (l.rare_conquest[0] & 0x20)) v += cst(g, "silk_caravan");
	if (has_tribe_bonus(g, who, 4)) v += cst(g, "nubian_caravan_limit");
	v = std::min(v, 99);
	if (p != 0) {
		int32_t n = l.city_num;
		if (has_preq(g, who, kBonuses)) {
			for (int32_t i = 0; i < 8; i++) {
				const Leader& o = L(g, i);
				if (!(o.flags & leaderflag::kValid) || i == who) continue;
				if (who == i || (l.diplos(i) == 2 && o.diplos(who) == 2)) n += o.city_num;
			}
		}
		n = ((n - 1) * n) / 2;
		v = std::min(v, n);
	}
	return v;
}

// ---------------------------------------------------------------- bookkeeping

void track_unit_type(Game& g, int32_t who, int32_t t, int32_t d, int32_t o) {
	if (!valid_who(who) || t < kUnits || t >= kEndUnits) return;
	Leader& l = L(g, who);
	l.num_units[size_t(t - kUnits)] = int16_t(l.num_units[size_t(t - kUnits)] + d);
	const Type& ty = g.type(t);
	if (ty.attack != 0) {
		switch (ty.where) {
		case ptype::kBarracks: l.barracks_units += d; l.combat_units += d; break;
		case ptype::kStable: l.stable_units += d; l.combat_units += d; break;
		case ptype::kSiegeFactory: l.factory_units += d; break;
		case ptype::kDock: l.dock_units += d; break;
		default:
			if (ty.domain == 2) l.air_units += d;
			break;
		}
	}
	if (t == ptype::kPeasants || t == ptype::kPeasantsKorean) l.peasants += d;         // TypeData::is_peasant
	else if (t == ptype::kScholars || t == ptype::kScholarsKorean) l.scholars += d;    // TypeData::is_scholar
	else if (ty.role & 0x10) l.scouts += d;
	if (o >= 0 && is_t(g, t, ptype::kMilitia)) {
		// UnitData +0x54 made_from (Unit::rare in the port; set by cast_to_arms): militia made from scholars are counted.
		const Unit* mu = g.unit(who, o);
		if (mu && mu->rare > 0 && mu->rare < kNumTypes && is_t(g, mu->rare, ptype::kScholars)) l.scholar_militia += d;
		return;
	}
	if (is_t(g, t, 0x4b /*SPECIALFORCES*/)) {
		if (l.num_units[size_t(t - kUnits)] == 0) l.flags &= ~0x20000u;
		else l.flags |= 0x20000u;
	}
}

void track_queued(Game& g, int32_t who, int32_t t, int32_t d) {
	if (t < 0 || !valid_who(who)) return;
	Leader& l = L(g, who);
	if (l.num_queued[size_t(t)] != 0 || d >= 0) l.num_queued[size_t(t)] = uint16_t(l.num_queued[size_t(t)] + d);
	if (!is_unit_type(t) || g.type(t).attack == 0) return;
	auto bump = [&](int32_t& c) {
		if (c != 0 || d >= 0) c += d;
	};
	const Type& ty = g.type(t);
	switch (ty.where) {
	case ptype::kBarracks: bump(l.barracks_queued); bump(l.combat_queued); break;
	case ptype::kStable: bump(l.stable_queued); bump(l.combat_queued); break;
	case ptype::kSiegeFactory: bump(l.factory_queued); break;
	case ptype::kDock: bump(l.dock_queued); break;
	default:
		if (ty.domain == 2) bump(l.air_queued);
		break;
	}
}

void bucket_add(Game& g, int32_t who, int32_t good, int32_t amount) {
	if (!valid_who(who) || good < 0 || good >= 6) return;
	L(g, who).bucket[size_t(good)] += amount;
}

void calc_pop_cap(Game& g, int32_t who) {
	Leader& l = L(g, who);
	l.misery = 0;
	// Scripted rules / CTW: ScenarioData::pop_cap[who] (set_population_cap) overrides the computed cap.
	const bool scen_cap = (g.scripted_rules || g.ctw) && who >= 0 && who < 8 && g.scen.pop_cap[size_t(who)] >= 0;
	const CategoryList* pl = g.rules->category_list("pop_limits");
	const int32_t limit = pl && !pl->items.empty()
	                          ? pl->items[size_t(std::min<int32_t>(g.info.pop_limit, int32_t(pl->items.size()) - 1))].data[0]
	                          : 200;
	const uint32_t pc = uint32_t(constant_offset("pop_cap"));
	if (scen_cap) {
		l.pop_cap = g.scen.pop_cap[size_t(who)];
	} else if (l.flags & leaderflag::kIgnorePopcap) {
		l.pop_cap = limit;
	} else {
		const int32_t e = std::clamp(l.epoch[0], 0, 7);
		const int32_t base = C(g).at(pc + uint32_t(e) * 4);
		l.pop_cap = base;
		int32_t L7 = limit;
		const int32_t cap7 = C(g).at(pc + 7 * 4);
		if (cap7 < L7) {
			if (e == 7) {
				l.pop_cap = L7;
			} else {
				const int32_t d = L7 - cap7;
				if (d < 100) {
					if (d > 0x31 && e > 5) l.pop_cap = (e - 5) * 0x19 + base;
				} else if (e > 3) {
					l.pop_cap = (e - 3) * 0x19 + base;
				}
			}
		}
		// Cities' pop caps (CityData::pop_cap: village_pop x level; 0 in the shipped rules). The original tests
		// wall_mark[who] != 0 (always true once anything was allocated in the wall range; here: never allocated).
		if (has_tribe_bonus(g, who, 3)) {
			l.pop_cap = ((cst(g, "bantu_pop_cap") + 100) * l.pop_cap) / 100;
			L7 = ((cst(g, "bantu_final_pop_cap") + 100) * L7) / 100;
		}
		l.pop_cap = std::min(l.pop_cap, L7);
		if (has_preq(g, who, 695 /*VIRTUAL_REALITY_BONUS*/)) l.pop_cap = L7;
	}
	if ((l.rare[2] & 8) || (l.rare_conquest[2] & 8)) l.pop_cap = ((cst(g, "peacocks_pop") + 100) * l.pop_cap) / 100;
	if (has_wonder_prod(g, who, ptype::kColossus)) l.pop_cap += cst(g, "colossus_pop_cap");
}

void reset_obs_flags(Game& g, int32_t who) {
	Leader& l = L(g, who);
	l.obs_flags.fill(0);
	for (int32_t b = kBuilds; b < kEndBuilds; b++) {
		const Type& t = g.type(b);
		if ((t.build_flags & 4) && has_preq(g, who, b) && t.from >= 0) Leader::set_bit(l.obs_flags, t.from, true);
	}
	for (int32_t u0 = kUnits; u0 < kEndUnits; u0++) {
		if (!l.has_tech_bit(u0)) continue;
		for (int32_t u = kUnits; u < kEndUnits; u++) {
			if (get_graft(g, who, g.type(u).jump) == u0) Leader::set_bit(l.obs_flags, u, true);
		}
		const int32_t f = get_graft(g, who, g.type(u0).from);
		if (f >= 0) Leader::set_bit(l.obs_flags, f, true);
	}
}

void check_transport(Game& g, int32_t who) {
	Leader& l = L(g, who);
	if (l.flags2 & 0x20) return;
	const bool tb = has_preq(g, who, 686 /*TRANSPORT_BONUS*/);
	const int32_t want = tb ? 3 : 0;
	if (tb) {
		int32_t n = l.num_buildings[ptype::kDock - kBuilds];
		const int32_t to = g.type(ptype::kDock).to;
		if (to >= 0) n += get_buildings(g, who, to, l.num_buildings[size_t(to - kBuilds)]);
		if (n != 0) {
			const int32_t have = (l.flags & 0x100) ? 3 : (l.flags & 0x200) ? 2 : int32_t((l.flags >> 10) & 1);
			if (have == want) return;
			l.flags |= 0x100 | 0x200 | 0x400;
			const ObjectList& OL = g.objects[size_t(who)];
			for (int32_t o = 0; o < OL.unit_mark; o++) {
				Unit* u = g.unit(who, o);
				if (!u || !u->alive()) continue;
				// UnitData::can_ever_transport (land units, and the barges), then the scout option, then transport_type <= want.
				if (!unit_can_ever_transport(g, *u)) continue;
				if (!(l.options_flags & 2) && unit_is(g, *u, 0x45)) continue;
				if (unit_transport_type(g, *u) <= want) u->unit_masks |= umask::kCanTransport;
			}
			return;
		}
	}
	if (l.flags & 0x700) {
		l.flags &= ~0x700u;
		const ObjectList& OL = g.objects[size_t(who)];
		for (int32_t o = 0; o < OL.unit_mark; o++) {
			Unit* u = g.unit(who, o);
			if (u && u->alive()) u->unit_masks &= ~umask::kCanTransport;
		}
	}
}

// ---------------------------------------------------------------- Leader::init (tech part)

void leader_init_techs(Game& g, int32_t who) {
	Leader& l = L(g, who);
	const int32_t start = starting_age(g, who);
	l.age = start;
	l.discovered = 0;
	l.epochs = 0;
	l.tech.fill(0);
	l.tech_at_start.fill(0);
	const bool all = all_techs(g, who);
	for (int32_t x = kTechs; x < kEndTechs; x++) {
		if (!(all || g.type(x).age < start) || is_final_type(x) || is_gov_type(x)) continue;
		if (!all) {
			if (is_epoch_type(x)) {
				if (g.info.game_rules != 8 || g.info.team[size_t(who & 7)] != 0 || start < g.type(x).age) continue;
			} else if (!is_age_type(x)) {
				bool skip = false;
				for (int32_t i = 0; i < num_preq(x); i++) {
					const int32_t p = get_preq(g, x, i, who);
					if (p < -1 || (p >= 0 && is_epoch_type(p))) skip = true;
				}
				if (skip) continue;
			}
		}
		l.set_tech_bit(x, true);
		Leader::set_bit(l.tech_at_start, x, true);
		if (!is_age_type(x)) {
			if (is_epoch_type(x)) l.epochs++;
			else l.discovered++;
		}
	}
	for (int32_t c = 0; c < 4; c++) l.epoch[size_t(c)] = compute_epoch(g, who, c);
	l.science = l.epoch[3];
	for (int32_t b = kBuilds; b < kEndBuilds; b++) {
		if (type_avail(g, who, b, 1) != 0) l.set_tech_bit(b, true);
	}
	for (int32_t u = kUnits; u < kGaia; u++) {
		if (!has_preq(g, who, u) || !tribe_can_type(g, who, u)) continue;
		bool skip = false;
		if (!all) {
			const int32_t f = get_graft(g, who, g.type(u).from);
			if (f >= 0) {
				for (int32_t i = 0; i < num_preq(f); i++) {
					const int32_t p = get_preq(g, f, i, who);
					if (p >= 0 && is_age_type(p)) {
						const int32_t want = (g.info.game_rules == 11 || g.info.game_rules == 4) ? start : start - 1;
						if (g.type(p).age == want) {
							skip = true;
							break;
						}
					}
				}
			}
		}
		if (skip) continue;
		l.set_tech_bit(u, true);
		Leader::set_bit(l.tech_at_start, u, true);
	}
	for (int32_t s = kSpells; s < kEndSpells; s++) {
		if (type_avail(g, who, s, 1) == 0) continue;
		const Type& st = g.type(s);
		if (st.preq[0] != -1 || !(st.spell_flags & 1)) l.set_tech_bit(s, true);
	}
	reset_obs_flags(g, who);
	if (!g.ctw) {
		auto free_epoch = [&](int32_t bonus, const char* name, int32_t cat) {
			if (!has_tribe_bonus(g, who, bonus) || cst(g, name) == 0) return;
			const int32_t lvl = l.epoch[size_t(cat)];
			if (lvl >= 7) return;
			const int32_t base = cat == 1 ? ptype::kCivic : cat == 2 ? ptype::kBarter : cat == 3 ? ptype::kWrittenWord : ptype::kMilitaryBase;
			gain_tech(g, who, base + lvl, 0, 0, false, true);
		};
		free_epoch(0x16, "dutch_free_commerce", 2);
		free_epoch(0xd, "russian_free_civic", 1);
		free_epoch(0, "aztec_free_military", 0);
		free_epoch(6, "roman_free_military", 0);
		free_epoch(0x14, "americans_free_science", 3);
		if (has_tribe_bonus(g, who, 0x17) && cst(g, "persians_despotism") != 0) gain_tech(g, who, ptype::kGovBase, 0, 0, false, true);
	}
	if (has_preq(g, who, ptype::kExploreMapBonus)) l.flags |= leaderflag::kExploreMap;
	if (has_preq(g, who, ptype::kRevealEnemyBonus) || has_wonder_prod(g, who, ptype::kSpaceProgram)) l.flags |= leaderflag::kRevealEnemy;
	if (has_preq(g, who, ptype::kActivePingBonus)) l.flags |= leaderflag::kActivePing;
	l.pop_cap = 0;
	calc_pop_cap(g, who);
}

// ---------------------------------------------------------------- gain_tech

namespace {

// Game::war_allowed @594670 (rush rules). TODO(frame-spec): the decompile's DL operand of the frame branch.
bool war_allowed(const Game& g) {
	if (g.info.rush_rules == 0) return true;
	int32_t cur = 0;
	for (int32_t i = 0; i < 8; i++) {
		if (L(g, i).flags & leaderflag::kValid) cur = std::max(cur, L(g, i).age);
	}
	return cur >= g.info.rush_rules;
}

// The unit-line match of 7.2.1: old == graft(T.from), or old's (grafted) jump chain reaches t.
bool line_match(const Game& g, int32_t who, int32_t old, int32_t t) {
	if (old == get_graft(g, who, g.type(t).from)) return true;
	if (!is_unit_type(old)) return false;
	int32_t j = get_graft(g, who, g.type(old).jump);
	for (int guard = 0; j >= 0 && j != t && guard < 1000; guard++) j = get_graft(g, who, g.type(j).jump);
	return j == t;
}

// Pattern P(range[, cond]) of 7.1.2.
template <typename Cond>
void free_loop_p(Game& g, int32_t who, int32_t t, int32_t lo, int32_t hi, Cond cond) {
	for (int32_t x = lo; x < hi; x++) {
		if (!cond(x) || !has_preq(g, who, x)) continue;
		for (int32_t i = 0; i < num_preq(x); i++) {
			if (get_preq(g, x, i, who) == t) {
				if (type_eligible(g, who, x, 1)) gain_tech(g, who, x, 0, 0, false, true);
				break;
			}
		}
	}
}
void free_loop_p(Game& g, int32_t who, int32_t t, int32_t lo, int32_t hi) {
	free_loop_p(g, who, t, lo, hi, [](int32_t) { return true; });
}
// Pattern U(range).
void free_loop_u(Game& g, int32_t who, int32_t t, int32_t lo, int32_t hi) {
	for (int32_t u = lo; u < hi; u++) {
		int32_t q;
		if (get_preq(g, u, 0, who) == t) q = get_preq(g, u, 1, who);
		else if (get_preq(g, u, 1, who) == t) q = get_preq(g, u, 0, who);
		else continue;
		if (has_tech(g, who, q) && type_eligible(g, who, u, 1)) gain_tech(g, who, u, 0, 0, false, true);
	}
}

}  // namespace

void gain_tech(Game& g, int32_t who, int32_t t, int32_t x, int32_t y, bool notify, bool upgrade_existing) {
	if (!valid_who(who) || t < 0 || t >= kNumTypes) return;
	Leader& l = L(g, who);
	const Type& T = g.type(t);
	// 1-2.
	l.flags |= leaderflag::kNewTech;
	if (is_gov_type(t) && g.frame == 0) l.gov_hero_frame = 1;
	// 3. Science refund of the other techs queued at the first library.
	if (is_epoch_type(t) && T.cat == 3 && g.frame != 0) {
		const int32_t lib = get_first_library(g, who);
		if (lib >= 0) {
			Build& B = *g.build(who, lib);
			for (int32_t q = 0; q < B.queued; q++) {
				const int32_t v = B.build_queue.get_queue(q);
				if (v != t && is_tech_type(v)) {
					// Build::refund_cost @620490 (s = science epochs before this tech).
					const int32_t s = l.epoch[3], a = g.type(v).age, D = C(g).at(uint32_t(constant_offset("tech_science_discount")));
					QueueItem& it = B.build_queue.queue[size_t(q)];
					for (int32_t k = 0; k < 3; k++) {
						if (it.good[size_t(k)] < 0) continue;
						const int32_t base = (int32_t(it.cost[size_t(k)]) * 100) / (100 - D * (s - a));
						const int32_t nw = base - ((s - a + 1) * D * base) / 100;
						l.bucket[size_t(it.good[size_t(k)])] += it.cost[size_t(k)] - nw;
						it.cost[size_t(k)] = int16_t(nw);
					}
				}
			}
		}
	}
	// 4. Rush peace end test.
	bool rush_end = false;
	if (is_age_type(t) && g.info.rush_rules != 0 && t - 543 == g.info.rush_rules) rush_end = !war_allowed(g);
	// 5. Fighter-bomber bookkeeping.
	int32_t old_fb = -1;
	if (is_t(g, t, ptype::kFighterBomber)) {
		old_fb = current_upgrade(g, who, ptype::kFighterBomber);
	} else if (is_t(g, t, get_graft(g, who, ptype::kJetFighter), true)) {
		gain_tech(g, who, ptype::kJetFighterBomber, x, y, false, true);
		old_fb = t;
	}
	// 6. Counters.
	if (!has_tech(g, who, t)) {
		GameEvent ev;  // the tool log (ui-port 11): tech researched (9) / age advanced (10)
		ev.frame = g.frame, ev.who = who, ev.value = t, ev.other = notify ? 1 : 0, ev.x = x, ev.y = y;
		if (is_age_type(t)) {
			l.age++;
			l.age_stamp[size_t(t - kAges)] = g.frame;
			ev.kind = 10, ev.value = l.age;
			ev.o = t;
		} else if (is_epoch_type(t)) {
			l.epochs++;
			l.epoch[size_t(T.cat & 3)]++;
			l.science = l.epoch[3];
			ev.kind = 9;
		} else {
			l.discovered++;
			ev.kind = 9;
		}
		g.events.push_back(ev);
	}
	// 7. CTW civic threshold (TODO(ctw): conquest wonders are not loaded; the branch runs only under ctw).
	if (g.ctw && is_epoch_type(t) && T.cat == 1 && g.frame != 0 && l.epoch[1] == cst(g, "ctw_wonder_min_civics")) {
		if (has_wonder_prod(g, who, ptype::kSpaceProgram)) l.flags |= leaderflag::kRevealEnemy;
	}
	// 8-9.
	std::array<bool, 6> had{};
	for (int32_t k = 0; k < 6; k++) had[size_t(k)] = has_preq(g, who, k);
	l.set_tech_bit(t, true);
	// 11.
	l.flags |= leaderflag::kRecompute;
	// 12. Newly discovered basic goods pay their starting amount.
	if (!g.editor) {
		for (int32_t k = 0; k < 6; k++) {
			if (had[size_t(k)] || g.type(k).preq[0] != t) continue;
			int32_t amt = g.starting[size_t(k)];
			if (g.info.game_rules == 8 && g.info.team[size_t(who & 7)] == 0) amt = (g.info.starting_resources2 + 1) * amt;
			else if (g.ctw && (l.flags2 & 0x80)) amt *= cst(g, "ctw_nomad_starting_res_x");
			bucket_add(g, who, k, amt);
			if (g.info.starting_resources == 8) l.bucket[size_t(k)] = 99999;
		}
		// Goods made obsolete by t (stock data: none) would be sold down to 100: TODO(economy) Leader::action_sell.
		if (t == ptype::kClassicalAge && has_tribe_bonus(g, who, 5) && cst(g, "greek_delay_knowledge") != 0) {
			int32_t amt = g.starting[3];
			if (g.info.game_rules == 8 && g.info.team[size_t(who & 7)] == 0) amt = (g.info.starting_resources2 + 1) * amt;
			else if (g.ctw && (l.flags2 & 0x80)) amt *= cst(g, "ctw_nomad_starting_res_x");
			bucket_add(g, who, 3, amt);
		}
	}
	// 14.
	if (is_epoch_type(t) && T.cat == 1 && l.epoch[1] == cst(g, "ctw_wonder_min_civics")) {
		calc_pop_cap(g, who);
		// TODO(combat-spec 10 step 7): calc_anti_attrition, calc_attrition.
		regions_fix_all_borders(g);
	}
	// 15-18. Fog bonuses.
	if (g.type(ptype::kAllyLos).preq[0] == t) {
		for (int32_t p = 0; p < 10; p++) {
			if (p != who && (L(g, p).flags & leaderflag::kValid) && is_ally(g, who, p)) l.ally_mask = uint8_t(l.ally_mask | (1u << (p & 31)));
		}
	}
	if (g.type(ptype::kExploreMapBonus).preq[0] == t) l.flags |= leaderflag::kExploreMap;
	if (get_preq(g, ptype::kRevealEnemyBonus, 0, who) == t) l.flags |= leaderflag::kRevealEnemy;
	if (get_preq(g, ptype::kActivePingBonus, 0, who) == t) l.flags |= leaderflag::kActivePing;
	// 19. World Government: assimilate own captured cities. TODO(victory): Leader::defeat of capital-less leaders.
	if (get_preq(g, ptype::kGlobalGovernmentBonus, 0, who) == t) {
		for (int32_t c = 0; c < l.city_mark && size_t(c) < g.cities[size_t(who)].size(); c++) {
			City& ct = g.cities[size_t(who)][size_t(c)];
			if (ct.valid() && ct.race != who) city_assimilate(g, who, c);
		}
	}
	// 20.
	if (get_preq(g, ptype::kArmageddonBonus, 0, who) == t) g.armageddon = std::max(0, g.armageddon - 2);
	// 21. GRANARY2..5.
	for (int32_t b = 699; b <= 702; b++) {
		if (get_preq(g, b, 0, who) == t) calc_pop_cap(g, who);
	}
	// 22.
	check_transport(g, who);
	// 23. Buildings' max_age.
	if (is_age_type(t)) {
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			Build* b = g.build(who, o);
			if (b && b->alive()) b->max_age = uint8_t(std::max<int32_t>(b->max_age, l.age));
		}
	}
	// 25. American free bombers at the Modern Age.
	if (t == ptype::kModernAge && has_tribe_bonus(g, who, 0x14)) {
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			Build* b = g.build(who, o);
			if (!b || !b->alive() || !(b->flags & objflag::kActive) || !type_is(*b->ptype, ptype::kAirbase)) continue;
			for (int32_t k = cst(g, "americans_free_bomber"); k > 0; k--) {
				if (!check_population(g, who, ptype::kBomber))
					build_train(g, *b, current_upgrade(g, who, get_graft(g, who, ptype::kBomber)));
			}
		}
	}
	// 26. Unit branch (7.2.1).
	if (is_unit_type(t)) {
		if (is_t(g, t, ptype::kMilitia)) l.flags |= leaderflag::kCalcUnitStats;
		if (upgrade_existing) {
			for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
				Unit* u = g.unit(who, o);
				if (!u || !u->alive()) continue;
				if (is_t(g, t, ptype::kFighterBomber) && unit_is(g, *u, ptype::kCarrier)) {
					track_queued(g, who, old_fb, -u->num_queued);
					track_queued(g, who, t, u->num_queued);
					continue;
				}
				const int32_t old = u->type;
				if (!line_match(g, who, old, t)) continue;
				if (T.uber_size < g.type(old).uber_size) {
					if (T.uber_size == 1) {
						if (u->o_up >= 0) {  // not the captain
							object_die(g, *u, 0, -1, 0.0f);
							continue;
						}
						// UnitData::total_damage(1, 0): the chain's summed damage (TODO: verify the uber walk).
						int32_t td = 0;
						for (Unit* m = u; m; m = m->o_down >= 0 ? g.unit(who, m->o_down) : nullptr) {
							td += m->damage;
							if (m->o_down == m->o) break;
						}
						u->damage = td;
					}
				}
				unit_set_type(g, *u, t, 0);
			}
			for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
				Build* b = g.build(who, o);
				if (!b || !b->alive()) continue;
				for (int32_t q = 0; q < b->queued; q++) {
					const int32_t v = b->build_queue.get_queue(q);
					if (!is_unit_type(v) || !line_match(g, who, v, t)) continue;
					track_queued(g, who, v, -1);
					b->build_queue.set_queue(q, t, nullptr, true);
					track_queued(g, who, t, 1);
				}
			}
		}
		if (!(T.obj_masks & 0x4000000)) {
			for (int32_t u = kUnits; u < kEndUnits; u++) {
				if (u == t || !line_match(g, who, u, t)) continue;
				l.set_tech_bit(u, true);
				Leader::set_bit(l.obs_flags, u, true);
			}
		}
		return;
	}
	// 27. Building branch (7.2.2): global building upgrades.
	if (is_build_type(t)) {
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			Build* b = g.build(who, o);
			if (!b || !b->alive() || b->ptype->upgrade != t) continue;
			Leader::set_bit(l.obs_flags, b->type, true);
			wall_set_type(g, *b, t, 0);
		}
		if (T.from >= 0) Leader::set_bit(l.obs_flags, T.from, true);
	}
	// 28.
	if (!is_tech_type(t)) return;
	// 29.
	if (is_epoch_type(t)) {
		l.misery = 0;
		if (T.cat == 0) calc_pop_cap(g, who);
	}
	// 31. Governments.
	if (is_gov_type(t)) {
		l.gov = t;
		if (is_t(g, t, ptype::kGovCapitalism) && g.frame != 0) bucket_add(g, who, 5, cst(g, "capitalism_oil_gift"));
	}
	// 32-33. TODO(victory): tech-race victory.
	// 35. GRANARY2..5 (first match).
	for (int32_t b = 699; b <= 702; b++) {
		if (get_preq(g, b, 0, who) == t) {
			calc_pop_cap(g, who);
			break;
		}
	}
	// 36. Borders.
	bool borders = false;
	for (int32_t b = 712; b <= 714 && !borders; b++) borders = get_preq(g, b, 0, who) == t;
	for (int32_t b = 721; b <= 723 && !borders; b++) borders = get_preq(g, b, 0, who) == t;
	if (borders || (is_epoch_type(t) && T.cat == 1) || (has_tribe_bonus(g, who, 0xd) && is_age_type(t))) regions_fix_all_borders(g);
	// 37. Civic epochs grow every city's radius.
	if (is_epoch_type(t) && T.cat == 1) {
		for (int32_t c = 0; c < l.city_mark && size_t(c) < g.cities[size_t(who)].size(); c++) {
			City& ct = g.cities[size_t(who)][size_t(c)];
			if (!ct.valid()) continue;
			Build* cb = g.build(who, ct.o);
			if (cb) wall_mask_city(g, *cb, true, city_get_radius(g, who, c));
			city_find_buildings(g, who, c);
		}
	}
	// 38. Global building upgrades and city upgrades.
	for (int32_t b = kBuilds; b < kEndBuilds; b++) {
		if (!has_preq(g, who, b)) continue;
		for (int32_t i = 0; i < num_preq(b); i++) {
			if (get_preq(g, b, i, who) != t) continue;
			if (is_t(g, b, ptype::kTown)) {
				for (int32_t c = 0; c < l.city_mark && size_t(c) < g.cities[size_t(who)].size(); c++) {
					if (g.cities[size_t(who)][size_t(c)].valid()) city_check_upgrade(g, who, c);
				}
			}
			if ((g.type(b).build_flags & 4) && type_eligible(g, who, b, 1)) gain_tech(g, who, b, 0, 0, false, true);
			break;
		}
	}
	// 39. Units that need no research.
	for (int32_t u = kUnits; u < kEndUnits; u++) {
		if (get_preq(g, u, 0, who) != t && get_preq(g, u, 1, who) != t) continue;
		if (!has_preq(g, who, u) || has_tech(g, who, u) || !(g.type(u).unit_flags & 0x80)) continue;
		if (g.type(u).obj_masks & 0x4000000) continue;
		if (type_eligible(g, who, u, 1)) gain_tech(g, who, u, 0, 0, false, true);
	}
	// 40. Free-upgrade loops (table 7.1.2), in this order.
	auto tb = [&](int32_t n) { return has_tribe_bonus(g, who, n); };
	if (tb(0xe) && cst(g, "chinese_herbal_lore") != 0) free_loop_p(g, who, t, 603, 606);
	if (has_wonder_prod(g, who, ptype::kRedFort) && cst(g, "red_fort_fortification") != 0) free_loop_p(g, who, t, 593, 596);
	if (has_wonder_prod(g, who, ptype::kRedFort) && cst(g, "red_fort_tactics") != 0) free_loop_p(g, who, t, 583, 586);
	if (tb(0x13) && cst(g, "lakota_cav_upgrades") != 0) {
		free_loop_u(g, who, t, 187, 209);
		free_loop_u(g, who, t, 216, 224);
	}
	if (tb(0x12) && cst(g, "iroquois_scout_upgrades") != 0) free_loop_u(g, who, t, 69, 82);
	if (tb(0x15) && cst(g, "indians_elephant_upgrades") != 0) free_loop_u(g, who, t, 261, 265);
	if (tb(0x10) && cst(g, "korean_militia_upgrades") != 0) free_loop_u(g, who, t, 66, 69);
	if (tb(0x10) && cst(g, "korean_temple_upgrades") != 0) free_loop_p(g, who, t, 586, 589);
	if (tb(0x10) && cst(g, "korean_temple_tax_upgrades") != 0) free_loop_p(g, who, t, 589, 593);
	if (tb(0x17) && cst(g, "persians_taxation") != 0) free_loop_p(g, who, t, 589, 593);
	if (tb(0x11) && cst(g, "mongol_free_forage") != 0) free_loop_p(g, who, t, 606, 609);
	if (tb(0xd) && cst(g, "russian_attrition_upgrades") != 0) free_loop_p(g, who, t, 596, 600);
	if (tb(7) && cst(g, "egyptian_granary_upgrades") != 0) free_loop_p(g, who, t, 612, 615);
	if (tb(6) && cst(g, "roman_fort_upgrades") != 0)
		free_loop_p(g, who, t, kTechs, kEndTechs, [&](int32_t v) { return g.type(v).where == ptype::kFortX && !has_tech(g, who, v); });
	if (has_wonder_prod(g, who, ptype::kHangingGardens) && cst(g, "hanging_gardens_upgrades") != 0) {
		free_loop_p(g, who, t, 609, 612);
		free_loop_p(g, who, t, 620, 623);
		free_loop_p(g, who, t, 612, 615);
	}
	if (tb(10) && cst(g, "french_lumbermill_upgrades") != 0) free_loop_p(g, who, t, 609, 612);
	if (tb(0xe) && cst(g, "chinese_knowledge_upgrades") != 0) free_loop_p(g, who, t, 615, 620);
	if (tb(0xc) && cst(g, "german_metal_upgrades") != 0) free_loop_p(g, who, t, 620, 623);
	if (tb(0xc) && cst(g, "german_industry_upgrades") != 0) {
		free_loop_p(g, who, t, 620, 623);
		free_loop_p(g, who, t, 609, 612);
		free_loop_p(g, who, t, 612, 615);
	}
	if (tb(0xc) && cst(g, "german_heavy_infantry") != 0)
		free_loop_p(g, who, t, kUnits, kEndUnits, [&](int32_t v) {
			return g.type(v).where == ptype::kBarracks && (is_t(g, v, 153) || is_t(g, v, ptype::kHoplites));
		});
	if (tb(0xc) && cst(g, "german_light_cavalry") != 0)  // absent from rules.xml: -1, enabled
		free_loop_p(g, who, t, kUnits, kEndUnits, [&](int32_t v) { return g.type(v).where == ptype::kBarracks && is_t(g, v, ptype::kHorse); });
	if (tb(0xb) && cst(g, "british_archer_upgrades") != 0)
		free_loop_p(g, who, t, kUnits, kEndUnits, [&](int32_t v) { return g.type(v).where == ptype::kBarracks && is_t(g, v, ptype::kBowmen); });
	if (tb(9) && cst(g, "spanish_scout_upgrades") != 0)
		free_loop_p(g, who, t, kUnits, kEndUnits, [&](int32_t v) { return is_t(g, v, ptype::kScout); });
	if (tb(8) && cst(g, "turk_free_siege_upgrades") != 0)
		free_loop_p(g, who, t, kUnits, kEndUnits, [&](int32_t v) { return is_t(g, v, 265); });
	if (has_wonder_prod(g, who, ptype::kLiberty) && cst(g, "liberty_free_upgrades") != 0) {
		for (int32_t u = kUnits; u < kEndUnits; u++) {
			if (!has_preq(g, who, u)) continue;
			const Type& ut = g.type(u);
			if (!(ut.domain == 0 || ut.where == ptype::kAirbase)) continue;
			for (int32_t i = 0; i < num_preq(u); i++) {
				int32_t p = get_preq(g, u, i, who);
				if (p < 0) continue;
				if (is_unit_type(p)) p = get_graft(g, who, p);
				if (p == get_graft(g, who, t)) {
					if (type_eligible(g, who, u, 0)) gain_tech(g, who, u, 0, 0, false, true);
					break;
				}
			}
		}
	}
	if (cst(g, "colosseum_fort_upgrades") != 0 && has_wonder_prod(g, who, ptype::kColosseum)) free_loop_p(g, who, t, 593, 596);
	if (has_wonder_prod(g, who, ptype::kTikal) && cst(g, "tikal_temple_upgrades") != 0) free_loop_p(g, who, t, 586, 589);
	// 41.
	l.flags |= leaderflag::kCalcUnitStats | leaderflag::kCalcWallStats;
	// 42. Rush peace ends: every pair of non-neutral leaders that is neither allied nor at war goes to war.
	if (rush_end && g.info.team_style != 0 && g.info.team_style != 8 && g.info.team_style != 11) {
		for (int32_t p = 0; p < 8; p++) {
			if (!(L(g, p).flags & leaderflag::kValid)) continue;
			L(g, p).attrition_stamp3 = L(g, p).attrition_stamp3;  // +0x1f4 attrition_stamp = 0: TODO(state)
			for (int32_t q = 0; q < 8; q++) {
				if (q == p || !(L(g, q).flags & leaderflag::kValid)) continue;
				if (!is_ally(g, p, q) && !is_enemy(g, p, q)) L(g, p).diplo[size_t(q)] = 0;  // TODO: Leader::set_diplo side effects
			}
		}
	}
	// 43. Age: every unit re-picks its graphics pieces and re-places its guys (teleport) at its own position.
	if (is_age_type(t)) {
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			Unit* u = g.unit(who, o);
			if (!u || !u->alive()) continue;
			unit_update_gpiece(g, *u);
			// TODO(garrison): the original also re-places garrisoned units; only units on the map are moved here.
			if (u->on_map()) unit_set_new_location(g, *u, u->x, u->y, true, true);
		}
		// Wall::update_gpiece for every valid building (Leader::gain_tech @6dcb60; the max_age raised in step 23 picks the new age art).
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			Build* b = g.build(who, o);
			if (b && (b->flags & objflag::kValid)) build_update_gpiece(g, *b);
		}
	}
}

// ---------------------------------------------------------------- stat recalculation (7.7)

void calc_wall_stats(Game& g, int32_t who) {
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		Build* b = g.build(who, o);
		if (!b || !b->alive()) continue;
		if (!(b->flags & objflag::kActive)) b->constr_time = int32_t(update_construct_time(g, who, b->type));
		wall_update_hits(g, *b, false);
		build_update_los(g, *b);
	}
}

void calc_unit_stats(Game& g, int32_t who) {
	leader_calc_attrition(g, who);
	leader_calc_anti_attrition(g, who);
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		Unit* u = g.unit(who, o);
		if (!u || !u->alive()) continue;
		unit_update_los(g, *u);
		if (u->o_up < 0) {
			unit_update_hits(g, *u);  // Unit::update_hits(0) @60e930
			update_speed(g, *u);
			unit_update_armor(g, *u);  // Unit::update_armor @6054c0
		}
	}
}

void leaders_process_stats(Game& g) {
	for (int32_t who = 0; who < 8; who++) {
		Leader& l = g.leaders[size_t(who)];
		if (!(l.flags & leaderflag::kActive)) continue;
		if (l.flags & leaderflag::kCalcWallStats) {
			l.flags &= ~leaderflag::kCalcWallStats;
			calc_wall_stats(g, who);
		}
		if (l.flags & leaderflag::kCalcUnitStats) {
			l.flags &= ~leaderflag::kCalcUnitStats;
			calc_unit_stats(g, who);
		}
	}
}

}  // namespace ron::sim
