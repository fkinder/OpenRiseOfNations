// Costs and times (docs/production-spec.md section 2): TypeData::get_cost @664090 and its wrappers, ObjectData::train_time
// @6508c0, TypeData::time / research_time, Wall::update_construct_time @63d560, BuildData::construct_time @62d5c0.
// Checked against re/tools/production-ref.mjs --examples by ron-prod-check.
#include <algorithm>

#include "sim/heroes.hpp"
#include "sim/fog.hpp"
#include "sim/production_internal.hpp"

namespace ron::sim {

using prod::is_t;
using prod::pct;
using prod::tb;

namespace {

const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }

bool has_spell(const Game& g, int32_t who, int32_t t) {
	// LeaderData::has_spell @6e0bc0: the spell's tech bit (TODO: confirm the decompile's extra conditions).
	return who >= 0 && who < kNumLeaders && is_spell_type(t) && LD(g, who).has_tech_bit(t);
}

bool teams_locked(const Game& g) { return g.info.team_style != 0 && g.info.team_style != 8 && g.info.team_style != 11; }

bool mutual(const Game& g, int32_t a, int32_t b) {
	return a == b || (LD(g, a).diplos(b) == 2 && LD(g, b).diplos(a) == 2);
}

int32_t team_wonders(const Game& g, int32_t who) {
	int32_t n = 0;
	for (int32_t i = 0; i < 8; i++) {
		if ((LD(g, i).flags & leaderflag::kValid) && mutual(g, who, i)) n += get_wonders(g, i);
	}
	return n;
}
int32_t team_unbuilt_wonders(const Game& g, int32_t who) {
	int32_t n = 0;
	for (int32_t i = 0; i < 8; i++) {
		if ((LD(g, i).flags & leaderflag::kValid) && mutual(g, who, i)) n += get_unbuilt_wonders(g, i);
	}
	return n;
}
// LeaderData::has_unbuilt_wonder @6da470: has the wonder, or an own unbuilt site of it.
bool has_unbuilt_wonder(const Game& g, int32_t who, int32_t t) {
	if (has_wonder_prod(g, who, t)) return true;
	for (const Unbuilt& u : g.unbuilt_wonders[size_t(who)]) {
		if (u.o < 0) continue;
		const Build* b = g.objects[size_t(who)].builds[size_t(u.o - kBuildBase)].get();
		if (b && b->type == t) return true;
	}
	return false;
}
bool team_has_unbuilt_wonder(const Game& g, int32_t who, int32_t t) {
	for (int32_t i = 0; i < 8; i++) {
		if (!(LD(g, i).flags & leaderflag::kValid)) continue;
		if (i != who && !(LD(g, who).diplos(i) == 2 && LD(g, i).diplos(who) == 2)) continue;
		if (t >= 0 && has_unbuilt_wonder(g, i, t)) return true;
	}
	return false;
}

int32_t span(const Game& g) { return g.info.ending_technology - g.info.starting_technology + 1; }

// ---------------------------------------------------------------- 2.1.2 units
// Returns false for the "return 0" (free) cases.
bool unit_cost(const Game& g, int32_t t, int32_t rr, int32_t who, int32_t& v) {
	const Type& o = g.type(t);
	const Leader& l = LD(g, who);
	if (has_rare(g, who, 41) && ((o.unit_flags2 & 0x10) || t == ptype::kMerchant || t == ptype::kMerchantDutch ||
	                             t == ptype::kFurTrapper))
		return false;
	if ((g.scripted_rules || g.ctw) && g.scen.units_free) return false;  // ScenarioData::units_free
	const bool bit = l.has_tech_bit(t);
	if (bit) {
		for (int32_t q = kUnits; q < 402; q++) {
			if (l.num_queued[size_t(q)] == 0) continue;
			bool hit = get_graft(g, who, g.type(q).from) == t;
			if (!hit) {
				int32_t j = get_graft(g, who, o.jump);
				for (int guard = 0; j >= 0 && guard < 1000; guard++) {
					if (j == q) {
						hit = true;
						break;
					}
					j = get_graft(g, who, g.type(j).jump);
				}
			}
			if (hit) v += std::max(0, g.type(q).costs[size_t(rr)] - o.costs[size_t(rr)]);
		}
	}
	v *= K("unit_cost_factor");
	const int32_t ramp_base = v;
	const int32_t where = o.where;
	if (where == ptype::kBarracks && has_rare(g, who, 13)) v = (v * 9) / 10;
	if (tb(g, who, 17) && where >= 0 && is_t(g, where, ptype::kStable)) v = pct(K("mongol_stable_cost"), v);
	if (tb(g, who, 10)) {
		if (where == ptype::kSiegeFactory || where == ptype::kFactory) v = pct(K("french_siege_cost"), v);
		else if (is_t(g, t, ptype::kGeneral)) v = pct(K("french_special_cost"), v);
	}
	if (tb(g, who, 15)) {
		if (where == ptype::kBarracks) {
			const int32_t k = K("japanese_barracks_cost");
			if (k < 0) v = ((std::min(l.age, l.epoch[0]) * k + 100) * v) / 100;
			else v = pct(k, v);
		} else if (where == ptype::kDock || where == ptype::kAnchorage || where == ptype::kShipyard) {
			v = pct(K("japanese_ship_cost"), v);
		}
	}
	if (tb(g, who, 12) && is_t(g, t, ptype::kFireRaft)) v = pct(K("german_submarine_cost"), v);
	if (tb(g, who, 20)) {
		if (where == ptype::kAirbase) v = pct(K("americans_aircraft"), v);
		if (is_t(g, t, ptype::kCarrier)) v = pct(K("americans_aircraft"), v);
		if (is_t(g, t, ptype::kFighterBomber)) v = pct(K("americans_aircraft"), v);
	}
	if (tb(g, who, 11) && is_t(g, t, ptype::kAaGun)) v = pct(K("british_aa_cost"), v);
	if (tb(g, who, 4) && (is_t(g, t, ptype::kCara) || t == ptype::kMerchant || t == ptype::kMerchantDutch ||
	                      t == ptype::kFurTrapper))
		v = pct(K("nubian_merchants"), v);
	if (tb(g, who, 13) && is_t(g, t, ptype::kSpy)) v = pct(K("russian_spy_cost"), v);
	if (tb(g, who, 6) && is_t(g, t, ptype::kHoplites)) v = pct(K("roman_legion_cheap"), v);
	if (where == ptype::kBarracks || where == ptype::kStable || where == ptype::kAutoPlant) {
		v = pct(K("terra_cotta_cost"), v);  // no wonder test (production-spec 2.1.2)
		if (has_wonder_prod(g, who, ptype::kAngkorWat)) v = pct(K("angkor_ships"), v);
	}
	if (where == ptype::kStable || where == ptype::kAutoPlant) {
		if (has_rare(g, who, 14)) v = pct(K("horses_stable_cost"), v);
		if (has_rare(g, who, 15)) v = pct(K("rubber_autoplant_cost"), v);
	}
	if ((where == ptype::kSiegeFactory || where == ptype::kFactory) && has_rare(g, who, 16)) v = pct(K("sulphur_siege_cost"), v);
	if ((is_t(g, t, ptype::kSpy, true) || is_t(g, t, ptype::kGeneral, true)) && has_preq(g, who, 771))
		v = pct(K("spy_general_cost"), v);
	if (t == ptype::kPeasants || t == ptype::kPeasantsKorean) {
		if (has_rare(g, who, 33)) v = pct(K("wool_citizens_cost"), v);
		if (tb(g, who, 8)) v = pct(K("turk_citizen"), v);
	}
	if (o.domain == 1) {
		if (has_wonder_prod(g, who, ptype::kAngkorWat)) v = pct(K("angkor_ships"), v);
	} else if (o.domain == 2) {
		if (has_rare(g, who, 30)) v = pct(K("aluminum_air_cost"), v);
		if (has_wonder_prod(g, who, ptype::kSpaceProgram)) v = pct(K("space_air_cost"), v);
	}
	if (where == ptype::kBarracks) {
		if (has_preq(g, who, 796)) v = pct(K("despotism_military_cheaper3"), v);
		else if (has_preq(g, who, 795)) v = pct(K("despotism_military_cheaper2"), v);
		else if (has_preq(g, who, 794)) v = pct(K("despotism_military_cheaper"), v);
	}
	if (has_wonder_prod(g, who, ptype::kLiberty)) {
		if (o.obj_masks & 0x8000000) v = pct(K("liberty_missiles"), v);
		else if (is_t(g, t, ptype::kBomber)) v = pct(K("liberty_bombers"), v);
	}
	if (has_rare(g, who, 35) && is_t(g, t, ptype::kNuke)) v = pct(K("uranium_nuke_cost"), v);
	if (l.num_units[size_t(ptype::kCharles - kUnits)] != 0) v = (K("charles_unit_cost") * v) / 100;

	if (!bit && !(o.unit_flags & 0x80)) {
		// Upgrade research price.
		if (has_rare(g, who, 8)) v = pct(K("wine_unit_upgrades"), v);
		v = ((K("research_premium") * v) / 256 * o.research_premium_cost) / 256;
		if (o.special_upgrade == rr) v += o.special_upgrade_cost;
		if (t < 66 || t > 68) {
			for (int32_t q = kUnits; q < 402; q++) {
				bool hit = q == get_graft(g, who, o.from);
				if (!hit) {
					int32_t j = get_graft(g, who, g.type(q).jump);
					for (int guard = 0; j >= 0 && guard < 1000; guard++) {
						if (j == t) {
							hit = true;
							break;
						}
						j = get_graft(g, who, g.type(j).jump);
					}
				}
				if (!hit) continue;
				int32_t d = o.costs[size_t(rr)] - g.type(q).costs[size_t(rr)];
				if (g.type(q).costs[size_t(rr)] == 0) d /= 2;
				if (d <= 0) continue;
				d = std::min(d, K("unit_refit_max_cost"));
				const int32_t n = l.num_queued[size_t(q)] + l.num_units[size_t(q - kUnits)];
				v += K("unit_cost_factor") * n * d;
			}
		}
		if (o.role & 0x10000) {
			int32_t d = l.epoch[0] - o.military_level;
			if (d > 0) {
				d *= K("military_upgrade_discount");
				const int32_t sp = span(g);
				if (sp < 8) d = (sp * d + 7) >> 3;
				v = pct(d, v);
				if (v < 0) v = 0;
			}
			if (tb(g, who, 20) && o.domain == 0) v = pct(K("americans_upgrade_discount"), v);
			if (tb(g, who, 22) && o.domain == 1) v = pct(K("dutch_ship_discount"), v);
		}
		return true;
	}
	// The escalating price.
	int32_t n = get_support_count(g, who, t);
	if (t == ptype::kPeasants || t == ptype::kPeasantsKorean)
		n += get_support_count(g, who, 66) + get_support_count(g, who, 67) - l.scholar_militia + get_support_count(g, who, 68);
	else if (t == ptype::kScholars || t == ptype::kScholarsKorean)
		n += l.scholar_militia;
	if (is_t(g, t, ptype::kNuke)) n += l.nukes_used;
	if (n > 0) {
		if (o.progression & 2) n = ((n + 1) * n) / 2;
		int32_t extra = 0, cap;
		if (t == ptype::kScholars || t == ptype::kScholarsKorean) {
			cap = (K("unit_scholar_ramp_max") * ramp_base) / 100;
			for (int32_t k = 7; k < n; k += 7) extra += n - k;
		} else {
			const bool worker = t == ptype::kPeasants || t == ptype::kPeasantsKorean || t == ptype::kScholars ||
			                    t == ptype::kScholarsKorean || t == ptype::kMerchant || t == ptype::kMerchantDutch ||
			                    t == ptype::kFurTrapper || (o.unit_flags2 & 8);
			const int32_t mx = worker ? K("unit_worker_ramp_max")
			                   : (o.obj_masks & 4) ? K("unit_other_civilian_ramp_max")
			                                       : K("unit_military_ramp_max");
			cap = (mx * ramp_base) / 100;
		}
		for (int32_t i = 0; i < 2; i++) {
			if (o.support[size_t(i)] != rr) continue;
			int32_t add = o.support_cost[size_t(i)] * n + extra;
			if (cap != 0 && cap < add) add = cap;
			if (has_rare(g, who, 48)) add = pct(K("maize_ramping_bonus"), add);
			v += add;
		}
	}
	if (o.role & 0x10000) {
		const int32_t lvl = std::max(1, o.military_level);
		int32_t d = l.epoch[0] - lvl;
		if (d > 0) {
			d *= K("military_unit_discount");
			const int32_t sp = span(g);
			if (sp < 8) d = std::max(1, (sp * d + 7) >> 3);
			v = pct(d, v);
			if (v < 0) v = 0;
		}
	}
	if ((has_preq(g, who, 800) || has_preq(g, who, 801)) && where == ptype::kStable) v = pct(K("monarchy_unit_prod"), v);
	if (has_preq(g, who, 804) && (where == ptype::kSiegeFactory || where == ptype::kAirbase || where == ptype::kDock))
		v = pct(K("socialism_unit_bonus"), v);
	if (has_rare(g, who, 46) && o.domain == 1) v = pct(K("salmon_naval_cost"), v);
	return true;
}

// ---------------------------------------------------------------- 2.1.3 buildings
int32_t build_ramp_count(const Game& g, int32_t t, int32_t who, int32_t refund) {
	const Leader& l = LD(g, who);
	if (is_wonder_type(t)) {
		int32_t w = get_wonders(g, who) + get_unbuilt_wonders(g, who);
		if (refund != 0) return 0;
		int32_t n = teams_locked(g) ? team_wonders(g, who) - w + team_unbuilt_wonders(g, who) : 0;
		if (w > 3) n += w - 3;
		if (w > 6) n += w - 6;
		if (t == ptype::kSupercollider || t == ptype::kSpaceProgram) {
			if (has_unbuilt_wonder(g, who, ptype::kSupercollider) || has_unbuilt_wonder(g, who, ptype::kSpaceProgram)) {
				w += 1;
			} else if (team_has_unbuilt_wonder(g, who, ptype::kSupercollider) ||
			           team_has_unbuilt_wonder(g, who, ptype::kSpaceProgram)) {
				return n + 1 + w * 2;
			}
		}
		return n + w * 2;
	}
	if (is_city_type(g.type(t))) {
		int32_t n = 0;
		for (int32_t k = ptype::kVillage; k <= ptype::kMetropolis; k++)
			n += l.num_buildings[size_t(k - kBuilds)] + l.num_queued[size_t(k)];
		return n;
	}
	int32_t c = l.num_queued[size_t(t)] + l.num_buildings[size_t(t - kBuilds)];
	if (t == ptype::kOilPlatform) c += l.num_buildings[ptype::kOilWell - kBuilds] + l.num_queued[ptype::kOilWell];
	else if (t == ptype::kOilWell) c += l.num_buildings[ptype::kOilPlatform - kBuilds] + l.num_queued[ptype::kOilPlatform];
	int32_t n = c;
	if (t == 427 || t == 428 || t == 429 || t == 430 || t == 431 || t == 447 || t == 520) {
		const uint32_t b = uint32_t(c);
		if (b > 2) n = 2 * c - 2;
		if (b > 5) n += 2 * c - 10;
		if (b > 8) n += 3 * c - 24;
		if (b > 12) n += 4 * c - 48;
	}
	if (t >= 443 && t <= 446) {
		const uint32_t b = uint32_t(n);
		const int32_t c2 = n;
		if (b > 1) n = 2 * c2 - 1;
		if (b > 3) n += c2 - 3;
		if (b > 5) n += c2 - 5;
		if (b > 7) n += c2 - 7;
	}
	return n;
}

bool build_cost(const Game& g, int32_t t, int32_t rr, int32_t who, int32_t refund, int32_t& v) {
	const Type& o = g.type(t);
	const Leader& l = LD(g, who);
	if ((g.scripted_rules || g.ctw) && g.scen.buildings_free) return false;  // ScenarioData::buildings_free
	if (tb(g, who, 18) && is_t(g, t, ptype::kSenate) && K("iroquois_quick_senate") != 0 &&
	    l.high_buildings[ptype::kSenate - kBuilds] == 0)
		return false;
	v *= K("build_cost_factor");
	if (!l.has_tech_bit(t) && (o.build_flags & 4)) v = (K("research_premium") * v) / 256;
	const int32_t n = build_ramp_count(g, t, who, refund);
	const bool city = is_city_type(o);
	for (int32_t i = 0; i < 2; i++) {
		if (o.support[size_t(i)] != rr) continue;
		int32_t add = o.support_cost[size_t(i)] * K("build_support_factor") * n;
		if (is_wonder_type(t)) add /= 2;
		if (tb(g, who, 21) && !city) {
			if (is_wonder_type(t) || (t >= 439 && t <= 446) || (t >= 521 && t <= 525)) add = (add + 1) / 2;
			else add = 0;
		}
		v += add;
	}
	if (city && !is_wonder_type(t)) {
		if (K("bantu_city_cost") != 0 && tb(g, who, 3)) v = pct(K("bantu_city_cost"), v);
		if (has_wonder_prod(g, who, ptype::kPyramids)) v = pct(K("pyramids_city_discount"), v);
	}
	if (is_wonder_type(t)) {
		if (has_rare(g, who, 20)) v = pct(K("marble_wonder_cost"), v);
		if (tb(g, who, 7)) v = pct(K("egyptian_wonders"), v);
	}
	if (is_t(g, t, ptype::kTower) && tb(g, who, 16)) v = pct(K("korean_tower_cost"), v);
	if ((is_t(g, t, ptype::kFortX) || is_t(g, t, ptype::kTower)) && !is_wonder_type(t)) {
		if (has_wonder_prod(g, who, ptype::kColosseum)) v = pct(K("colosseum_fort_cost"), v);
		if (tb(g, who, 6)) v = pct(K("roman_fort_cost"), v);
	}
	if (is_t(g, t, ptype::kTemple) && has_wonder_prod(g, who, ptype::kTikal)) v = pct(K("tikal_temple_cost"), v);
	if (t == ptype::kMarket) {
		if (tb(g, who, 4)) v = pct(K("nubian_merchants"), v);
	} else if ((t == ptype::kLibrary || t == ptype::kUniversity) && tb(g, who, 5)) {
		v = pct(K("greek_library_cost"), v);
	}
	static const int32_t kMayaExcl[] = {427, 428, 429, 431, 430, 447, 432, 433, 434, 443, 444, 445,
	                                   446, 520, 439, 440, 441, 442, 521, 522, 523, 524, 525};
	if (tb(g, who, 1) && rr == 1 && !is_wonder_type(t) &&
	    std::find(std::begin(kMayaExcl), std::end(kMayaExcl), t) == std::end(kMayaExcl))
		v = pct(K("maya_building_timber"), v);
	if (tb(g, who, 11) && is_t(g, t, ptype::kAirDefense)) v = pct(K("british_aa_cost"), v);
	if (t == ptype::kMine) {
		if (tb(g, who, 12)) v = pct(K("german_mine_cost"), v);
	} else if (t == ptype::kFarm) {
		if (tb(g, who, 15)) v = pct(K("japanese_farms_cheaper"), v);
	} else if (t >= 427 && t <= 431 && has_wonder_prod(g, who, ptype::kColosseum)) {
		v = pct(K("colosseum_barracks_cost"), v);
	}
	if (is_t(g, t, ptype::kLookout) && has_wonder_prod(g, who, ptype::kLiberty)) v = pct(K("liberty_air_defense"), v);
	return true;
}

// ---------------------------------------------------------------- 2.1.4 techs
// The "count discovered" and "researching" helpers of the final-tech ramp.
bool tech_cost(const Game& g, int32_t t, int32_t rr, int32_t who, int32_t& v) {
	const Type& o = g.type(t);
	const Leader& l = LD(g, who);
	if ((g.scripted_rules || g.ctw) && g.scen.techs_free) return false;  // Game::is_scenario, ScenarioData::techs_free
	if (g.ctw && is_age_type(t) && v != 0) v = K("ctw_age_cost");
	v *= K("tech_cost_factor");
	if (is_gov_type(t) && tb(g, who, 20)) v = 0;
	if (has_rare(g, who, 49) && o.where == ptype::kTemple) v = pct(K("incense_temple_research"), v);
	// Greek classical / age knowledge conversions (constants 0 in the stock data).
	if (t == ptype::kClassicalAge && K("greek_classical") != 0 && tb(g, who, 5)) {
		if (rr == 0) v = pct(K("greek_classical"), v);
		else if (rr == 3) v += (K("tech_cost_factor") * o.costs[0] * K("greek_classical")) / 100;
	} else if (is_age_type(t) && K("greek_age_knowledge") != 0 && tb(g, who, 5)) {
		if (rr == 0) v = pct(K("greek_age_knowledge"), v);
		else if (rr == 3) v += (K("tech_cost_factor") * o.costs[0] * K("greek_age_knowledge")) / 100;
	}
	v = calc_science_discount(g, who, t, v);
	if (is_age_type(t)) {
		const int32_t d = rr == 3 ? K("tech_age_behind_knowledge_discount") : K("tech_age_behind_discount");
		bool done = false;
		for (int32_t i = 0; i < 8 && !done; i++) {
			const Leader& p = LD(g, i);
			if (i == who || !(p.flags & leaderflag::kValid) || is_ally(g, i, who)) continue;
			if (l.age + 1 < p.age) {
				v = 2 * d < 100 ? ((50 - d) * v * 2) / 100 : 0;
				done = true;
			}
		}
		for (int32_t i = 0; i < 8 && !done; i++) {
			const Leader& p = LD(g, i);
			if (i == who || !(p.flags & leaderflag::kValid) || is_ally(g, i, who)) continue;
			if (l.age < p.age) {
				v = d < 100 ? pct(d, v) : 0;
				done = true;
			}
		}
	}
	if (is_final_type(t)) {
		int32_t k = 0;
		for (int32_t f = ptype::kFinals; f < ptype::kFinals + 4; f++) {
			if (has_tech(g, who, f) || researching(g, who, f, -1, false, false)) k += 8;
		}
		v = (((K("ramp_final") * k) / 8 + 100) * v) / 100;
	}
	const int32_t tc = g.info.tech_cost;
	if (tc < 2) v = (2 * v) / 3;
	else if (tc == 5 || tc == 6) v = (!is_age_type(t) && rr != 3) ? (5 * v) / 4 : (3 * v) / 2;
	else if (tc >= 7) v = (!is_age_type(t) && rr != 3) ? (3 * v) / 2 : 2 * v;
	if (is_age_type(t) || is_epoch_type(t)) {
		if (tb(g, who, 5) && rr != 3) v = pct(K("greek_research_cost"), v);
		// TypeData::get_cost @664090: one division per Great Thinker card in play (CtW only).
		if (g.ctw) {
			for (int32_t i = l.num_bonus_cards[7]; i > 0; i--) v = (v * 100) / (K("ctw_great_thinker_discount") + 100);
		}
	} else {
		if (o.where == ptype::kGranary && has_rare(g, who, 23)) v = pct(K("bison_granary_cost"), v);
		if (o.where != ptype::kLibrary) {
			if (has_preq(g, who, 803)) v = pct(K("democracy_tech_bonus2"), v);
			else if (has_preq(g, who, 802)) v = pct(K("democracy_tech_bonus"), v);
		}
	}
	if ((t >= 609 && t <= 614) || (t >= 620 && t <= 622)) {
		if (has_wonder_prod(g, who, ptype::kHangingGardens) && K("hanging_gardens_industry_discount") != 0)
			v = pct(K("hanging_gardens_industry_discount"), v);
		if (tb(g, who, 12)) v = pct(K("german_industry_discount"), v);
	}
	if (t >= 589 && t <= 592 && tb(g, who, 11)) v = pct(K("british_taxation_discount"), v);
	if (rr != 3 && has_wonder_prod(g, who, ptype::kVersailles)) {
		bool ok;
		if (is_age_type(t) || is_epoch_type(t) || is_final_type(t)) ok = true;
		else if (t >= 615 && t <= 619) ok = K("versailles_affects_university_upgrades") != 0;
		else ok = K("versailles_affects_misc_upgrades") != 0;
		if (ok) v = pct(K("versailles_knowledge_costs"), v);
	}
	if (is_age_type(t) && has_rare(g, who, 18)) v = pct(K("silver_age_cost"), v);
	if (is_epoch_type(t)) {
		switch (o.cat) {
		case 0:
			if (has_rare(g, who, 21)) v = pct(K("furs_military"), v);
			if (has_preq(g, who, 796)) v = pct(K("despotism_military_cheaper3"), v);
			else if (has_preq(g, who, 795)) v = pct(K("despotism_military_cheaper2"), v);
			else if (has_preq(g, who, 794)) v = pct(K("despotism_military_cheaper"), v);
			if (tb(g, who, 8)) v = pct(K("turk_military_cheap"), v);
			break;
		case 1:
			if (has_rare(g, who, 10)) v = pct(K("dye_civic_commerce"), v);
			if (tb(g, who, 23)) v = pct(K("persians_civic_discount"), v);
			break;
		case 2:
			if (has_rare(g, who, 11)) v = pct(K("silk_commerce"), v);
			if (tb(g, who, 22)) v = pct(K("dutch_commerce_discount"), v);
			break;
		case 3:
			if (has_rare(g, who, 9)) v = pct(K("papyrus_science_military"), v);
			if (tb(g, who, 14)) v = pct(K("chinese_science_discount"), v);
			if (tb(g, who, 20)) v = pct(K("americans_science_discount"), v);
			break;
		default: break;
		}
	}
	return true;
}

}  // namespace

// ---------------------------------------------------------------- TypeData::get_cost

int32_t calc_science_discount(const Game& g, int32_t who, int32_t t, int32_t v) {
	if (!is_tech_type(t)) return v;
	int32_t a = g.type(t).age;
	if (!is_age_type(t) && !is_epoch_type(t)) a += 1;
	return v - ((LD(g, who).epoch[3] - a) * K("tech_science_discount") * v) / 100;
}

int32_t cost_factor_dow(const Game& g, int32_t who, int32_t target, int32_t t) {
	const int32_t m = (target == who) || (LD(g, who).diplos(target) == 2 && LD(g, target).diplos(who) == 2) ? 1 : 0;
	const int32_t k = m + (t >= kDeclareWar && t < kDeclareWar + 8 ? 1 : 0);
	return k * (LD(g, who).age + 1);
}

int32_t get_support_count(const Game& g, int32_t who, int32_t t) {
	if (!is_unit_type(t)) return 0;
	const Type& o = g.type(t);
	const Leader& l = LD(g, who);
	if ((o.progression & 1) && o.attack != 0) {
		if (o.where == ptype::kBarracks) return l.barracks_queued + l.barracks_units;
		if (o.where == ptype::kStable) return l.stable_queued + l.stable_units;
		if (o.where == ptype::kSiegeFactory) return l.factory_queued + l.factory_units;
		if (o.where == ptype::kDock) return l.dock_queued + l.dock_units;
		if (o.domain == 2) return l.air_queued + l.air_units;
	}
	int32_t n = l.num_queued[size_t(t)] + (t < kEndUnits ? l.num_units[size_t(t - kUnits)] : 0);
	if (t == 315 || t == 316) n += l.nukes_used;
	if (t == 313 || t == 314) n += l.missiles_used;
	// TODO(spec 2.10.2): the American free-bomber term (operand of get_buildings undecoded).
	return n;
}

int32_t get_cost(const Game& g, int32_t t, int32_t r, int32_t who, int32_t o, int32_t loc, int32_t refund,
                 int32_t convert, int32_t type_override) {
	if (t < 0 || t >= kNumTypes || r < 0 || r >= 6) return 0;
	const Type& ty = g.type(t);
	// 2.1.1 Prologue.
	if (t == 665 && who >= 0) {  // RADARSPELL
		const int32_t u = current_upgrade(g, who, ptype::kAirDefense);
		if (u >= 0) return std::max(0, get_cost(g, u, r, who, o, loc, refund, convert, -1) - 10);
	}
	int32_t rr = r, v = 0;
	bool spell_owned = false;
	if (is_spell_type(t) && has_spell(g, who, t)) {
		spell_owned = true;
		v = ty.use[size_t(r)];
		if (o >= 0) {
			const Object* ob = g.object(who, o);
			const bool city_caster = ob && ob->alive() && !ob->is_unit && (ob->flags & objflag::kActive) &&
			                         (ob->flags & objflag::kCity) && t != 647 && t != ptype::kDisband;
			if (city_caster) {
				const int32_t c = static_cast<const Build*>(ob)->city;
				if (c >= 0) v = v * -(refund != 0 ? 1 : 0);
			}
			if (t == 635 && ob && ob->is_unit && (static_cast<const Unit*>(ob)->unit_masks & 0x2000)) return 0;
			if (t == ptype::kDisband && ob && !ob->is_unit && (ob->flags & objflag::kActive))
				v *= (ob->ptype->x_size + ob->ptype->y_size) / 2;
		}
	} else {
		if ((t == 52 || t == 53) && who >= 0 && K("egyptian_scholars_food") != 0 && tb(g, who, 7)) {
			if (r == 0) rr = 2;
			else if (r == 2) rr = 0;
		}
		v = type_override == TYPE_NONE ? ty.costs[size_t(rr)] : g.type(type_override).costs[size_t(rr)];
	}
	(void)loc;
	(void)spell_owned;
	// Dispatch.
	if (is_unit_type(t)) {
		if (!unit_cost(g, t, rr, who, v)) return 0;
	} else if (is_build_type(t)) {
		if (!build_cost(g, t, rr, who, refund, v)) return 0;
	} else if (is_spell_type(t)) {
		// 2.1.5 (casting and declarations of war belong to the spells / diplomacy spec).
		if (t != ptype::kDisband) v *= K("spell_cost_factor");
		if (who >= 0) {
			if (t >= kDeclareWar && t < kDeclareWar + 8) {
				const int32_t target = t - kDeclareWar;
				v *= cost_factor_dow(g, who, target, t);
				if (!is_ally(g, who, target)) v = std::max(0, v - 10 * 0);  // TODO(diplomacy): invaders[target]
			} else if (t >= 676 && t < 684) {
				v *= cost_factor_dow(g, who, t - 676, t);
			}
		}
	} else if (is_tech_type(t)) {
		if (!tech_cost(g, t, rr, who, v)) return 0;
	} else {
		v *= K("tech_cost_factor");
	}
	// 2.1.6 Common tail.
	if (rr == 1 && has_rare(g, who, 34)) v = pct(K("coal_timber_cost"), v);
	else if (rr == 0 && has_rare(g, who, 27)) v = pct(K("sugar_food_cost"), v);
	else if (rr == 2 && has_rare(g, who, 42)) v = pct(K("gold_wealth_cost"), v);
	else if (rr == 4 && has_rare(g, who, 47)) v = pct(K("iron_metal_cost"), v);
	if (has_rare(g, who, 40)) v = pct(K("gypsum_cost"), v);
	if (o >= 0 && is_unit_type(t)) {
		const Object* ob = g.object(who, o);
		if (ob && !ob->is_unit && is_unassimilated(g, *static_cast<const Build*>(ob))) v *= 2;
	}
	if (g.wonders[15] != 0 && is_unit_type(t) && (ty.obj_masks & 0x8000000)) {
		for (int32_t i = 0; i < 8; i++) {
			if (i == who || (LD(g, i).flags & 3) != 3 || is_ally(g, who, i)) continue;
			if (has_wonder_prod(g, i, ptype::kSupercollider)) v = ((K("super_missiles") + 100) * v) / 100;
		}
	}
	if (tb(g, who, 21) && is_unit_type(t) &&
	    (is_t(g, t, 261) || is_t(g, t, 262) || is_t(g, t, 263) || is_t(g, t, 264)))
		v = pct(K("indians_elephant_cost"), v);
	// 2.1.7 Unavailable-resource conversion.
	if (convert != 0 && who >= 0) {
		for (int32_t k = 0; k < 6; k++) {
			if (type_avail(g, who, k, 1) != 0) continue;
			const Type& gd = g.type(k);
			const bool disc = has_preq(g, who, k);
			if (!disc && rr == gd.undiscovered_cost_good) {
				v += (get_cost(g, t, k, who, o, loc, refund, 1, -1) * gd.undiscovered_cost_rate) / 256;
			} else if (disc && rr == gd.obsolete_cost_good) {
				v += (get_cost(g, t, k, who, o, loc, refund, 1, -1) * gd.obsolete_cost_rate) / 256;
			}
		}
	}
	return v;
}

int32_t can_pay_cost(const Game& g, int32_t t, int32_t who, int32_t loc, int32_t o, int32_t total) {
	if (g.editor) return 10;
	// Scripted rules / CTW: ScenarioData::units_free / buildings_free / techs_free -> 10.
	if (g.scripted_rules || g.ctw) {
		if (is_unit_type(t) && g.scen.units_free) return 10;
		if (is_build_type(t) && g.scen.buildings_free) return 10;
		if (is_tech_type(t) && g.scen.techs_free) return 10;
	}
	int32_t best = -1;
	const Leader& l = LD(g, who);
	for (int32_t k = 0; k < 6; k++) {
		if (type_avail(g, who, k, 1) == 0) continue;
		const int32_t c = get_cost(g, t, k, who, o, loc, 0, 1, -1);
		if (c == 0) continue;
		const int32_t have = l.bucket[size_t(k)] - (total ? 0 : l.escrow[size_t(k)]);
		const int32_t q = have / c;
		if (q == 0) return 0;
		best = std::max(best, q);
	}
	return best < 0 ? 10 : best;
}

void pay_cost(Game& g, int32_t t, int32_t who, int32_t o, int32_t loc, int32_t from_escrow, int32_t* out) {
	Leader& l = g.leaders[size_t(who)];
	for (int32_t k = 0; k < 6; k++) {
		if (out) out[k] = 0;
		if (type_avail(g, who, k, 1) == 0) continue;
		const int32_t c = get_cost(g, t, k, who, o, loc, 0, 1, -1);
		if (out) out[k] = c;
		if (!from_escrow && l.bucket[size_t(k)] - l.escrow[size_t(k)] < c) l.escrow[size_t(k)] = 0;
		l.bucket[size_t(k)] = std::max(0, l.bucket[size_t(k)] - c);
		if (from_escrow) l.escrow[size_t(k)] = std::max(0, l.escrow[size_t(k)] - c);
	}
}

void unpay_cost(Game& g, int32_t t, int32_t who, int32_t o, int32_t loc) {
	if (g.editor) return;
	for (int32_t k = 0; k < 6; k++) {
		if (type_avail(g, who, k, 1) == 0) continue;
		g.leaders[size_t(who)].bucket[size_t(k)] += get_cost(g, t, k, who, o, loc, 1, 1, -1);
	}
}

int32_t sum_cost(const Game& g, int32_t t, int32_t who, int32_t o, int32_t refund) {
	int32_t s = 0;
	for (int32_t k = 0; k < 6; k++) {
		if (type_avail(g, who, k, 1) != 0) s += get_cost(g, t, k, who, o, -1, refund, 1, -1);
	}
	return s;
}

// ---------------------------------------------------------------- times

uint32_t type_time(const Game& g, int32_t t, int32_t who) {
	const Type& o = g.type(t);
	if (is_spell_type(t) && !has_spell(g, who, t)) return o.res_time * 100u;
	return o.job_time * 100u;
}

uint32_t type_research_time(const Game& g, int32_t t, int32_t who) {
	const Type& o = g.type(t);
	if (is_spell_type(t)) return o.res_time * 100u;
	uint32_t v = (type_time(g, t, who) * uint32_t(K("research_tick_premium"))) >> 8;
	if (is_unit_type(t)) v = uint32_t((int32_t(v) * o.research_premium_time) / 256);
	return v;
}

bool is_unassimilated(const Game& g, const Build& b) {
	// BuildData::is_unassimilated @62d470: in a city assimilated to someone else, and the building is the city or
	// lacks 'e' (0x10).
	if (b.city < 0 || b.who < 0 || b.who >= 8) return false;
	const auto& cl = g.cities[size_t(b.who)];
	if (size_t(b.city) >= cl.size()) return false;
	if (cl[size_t(b.city)].race == b.who) return false;
	return (b.flags & objflag::kCity) || !(b.ptype->build_flags & 0x10);
}

int32_t train_time(const Game& g, int32_t o, int32_t who, int32_t t) {
	const Type& ty = g.type(t);
	const Leader& l = LD(g, who);
	const Object* prod = o >= 0 ? g.object(who, o) : nullptr;
	const bool bit = l.has_tech_bit(t);
	int32_t v = int32_t(is_unit_type(t) && !bit ? type_research_time(g, t, who) : type_time(g, t, who));
	if (t == ptype::kDisband || t == ptype::kDepopulate) {
		if (prod) {
			v = train_time(g, o, who, prod->type);
			if (type_is(*prod->ptype, ptype::kVillage)) v = (v * K("disband_city_rate")) / 100;
			if (type_is(*prod->ptype, ptype::kSenate)) v = (v * K("disband_senate_rate")) / 100;
			if (prod::tb(g, who, 19)) v = (v * K("lakota_raze_speed")) / 100;
			v /= 2;
		}
	}
	if (is_t(g, t, ptype::kNuke) && has_rare(g, who, 35)) v = (v * 100) / (K("uranium_nuke_speed") + 100);
	if (is_unit_type(t) && bit) {
		v = (K("unit_rate_base") * v) / 100;
		const int32_t cap = v * 3;
		v += int32_t(l.num_units[size_t(t - kUnits)]) * int32_t(ty.job_extra_time) * K("unit_rate_progression");
		v = (v < 0 || cap < 0) ? 0 : std::min(v, cap);
		// TODO(handicap): semaphore[0] & 4 handicap mode ((200 - handicap) * v / 200).
		if (prod && has_general(g, *prod, 0, 0x163) >= 0) v = (v * 100) / (K("thepresident_unit_build_speed") + 100);
		const int32_t where = ty.where;
		if (tb(g, who, 17) && ty.attack != 0 && where >= 0 && is_t(g, where, ptype::kStable))
			v = prod::div100k(v, K("mongol_stable_speed"));
		if (tb(g, who, 15)) {
			if (where == ptype::kBarracks) {
				int32_t k = K("japanese_barracks_speed");
				if (k < 0) k = -k * std::min(l.age, l.epoch[0]);
				v = prod::div100k(v, k);
			}
			if (is_t(g, t, ptype::kCarrier)) v = prod::div100k(v, K("japanese_carrier_speed"));
		}
		if (tb(g, who, 14) && (t == 50 || t == 51 || t == ptype::kMerchant || t == ptype::kMerchantDutch ||
		                       t == ptype::kFurTrapper || (ty.unit_flags2 & 8))) {
			v = prod::div100k(v, K("chinese_citizen_creation"));
			if (K("chinese_citizen_instant") != 0) return 1;
		}
		if (tb(g, who, 11)) {
			if (ty.domain == 1) v = prod::div100k(v, K("british_ship_speed"));
			if (is_t(g, t, ptype::kBowmen)) v = prod::div100k(v, K("british_archer_speed"));
			if (is_t(g, t, ptype::kAaGun)) v = prod::div100k(v, K("british_aa_speed"));
		}
		if (tb(g, who, 10)) {
			if (where == ptype::kSiegeFactory || where == ptype::kFactory) v = prod::div100k(v, K("french_siege_speed"));
			else if (is_t(g, t, ptype::kGeneral)) v = prod::div100k(v, K("french_special_speed"));
		}
		if (tb(g, who, 12)) {
			if (ty.domain == 2) v = prod::div100k(v, K("german_air_speed"));
			if (is_t(g, t, ptype::kFireRaft)) v = prod::div100k(v, K("german_submarine_speed"));
		}
		if (tb(g, who, 6) && is_t(g, t, ptype::kHoplites)) v = prod::div100k(v, K("roman_legion_fast"));
		if ((ty.role & 0x10000) && has_preq(g, who, 789)) v = (3 * v) / 4;
		int32_t up = -1;
		if (ty.domain == 1) up = get_ships_speed_upgrade(g, who);
		else if (ty.obj_masks & (0x20 | 0x1000)) up = get_troops_speed_upgrade(g, who);
		else if (ty.obj_masks & 0x200000) up = get_vehicle_speed_upgrade(g, who);
		if (up >= 0) v = ((10 - up) * v) / 10;
		if ((where == ptype::kBarracks || where == ptype::kStable || where == ptype::kDock) && has_rare(g, who, 22))
			v = prod::div100k(v, K("cotton_creation_speed"));
		if ((is_t(g, t, ptype::kGeneral, true) || is_t(g, t, ptype::kSpy, true)) && has_preq(g, who, 770)) v /= 2;
		if ((t == 50 || t == 51) && has_rare(g, who, 33)) v = prod::div100k(v, K("wool_citizens_speed"));
		if (where == ptype::kStable) {
			if (has_preq(g, who, 801)) v = prod::div100k(v, K("monarchy_unit_prod2"));
			else if (has_preq(g, who, 800)) v = prod::div100k(v, K("monarchy_unit_prod"));
		}
		if ((where == ptype::kSiegeFactory || where == ptype::kAirbase || where == ptype::kDock) && has_preq(g, who, 804))
			v = prod::div100k(v, K("socialism_unit_bonus"));
		if (ty.domain == 0) {
			if (has_wonder_prod(g, who, ptype::kLiberty)) v = prod::div100k(v, K("liberty_units"));
		} else if (ty.domain == 1) {
			if (has_wonder_prod(g, who, ptype::kPorcelain)) v = prod::div100k(v, K("porcelain_ships"));
		} else if (ty.domain == 2) {
			if (has_wonder_prod(g, who, ptype::kSpaceProgram)) v = prod::div100k(v, K("space_air_speed"));
		}
		if ((is_t(g, t, ptype::kSpy, true) && has_wonder_prod(g, who, ptype::kKremlin)) ||
		    has_preq(g, who, ptype::kInstantUnitBonus))
			return K("accel_train") * 5 - 1;
	}
	if (is_tech_type(t)) {
		if (is_age_type(t) || is_epoch_type(t)) {
			int32_t w = 0;
			for (int32_t i = 0; i < 8; i++) {
				const Leader& p = LD(g, i);
				if (!(p.flags & leaderflag::kValid) || i == who) continue;
				const int32_t mine = is_age_type(t) ? l.age + l.ages_queued : l.age + l.epochs + l.ages_queued + l.epochs_queued;
				const int32_t other = is_age_type(t) ? p.age + p.ages_queued : p.age + p.epochs + p.ages_queued + p.epochs_queued;
				if (other > mine + (is_age_type(t) ? 1 : 3)) w += is_team(g, i, who) ? 1 : 2;
			}
			if (w != 0) v = ((2 * g.info.num_nations - w + 2) * v) / (2 * g.info.num_nations + 2);
			if (tb(g, who, 5)) v = prod::div100k(v, K("greek_research_speed"));
		} else {
			// TechType::count_discovered (vslot 0x74): players who already have it.
			int32_t k = 0;
			for (int32_t i = 0; i < 8; i++) {
				if ((LD(g, i).flags & leaderflag::kValid) && LD(g, i).has_tech_bit(t)) k++;
			}
			if (k != 0) v = ((g.info.num_players - k + 1) * v) / (g.info.num_players + 1);
		}
		if (has_wonder_prod(g, who, ptype::kSupercollider)) v = 0;
	}
	if (is_tech_type(t) || ((is_unit_type(t) || is_build_type(t)) && !bit)) {
		if (has_preq(g, who, 788)) v = (3 * v) / 4;
		if (has_wonder_prod(g, who, ptype::kAngkorWat)) v = prod::div100k(v, K("angkor_research"));
		if (has_rare(g, who, 7)) v = pct(K("relics_research_speed"), v);
		// ObjectData::train_time @6508c0: one division per Great Thinker card in play, after the relics (CtW only).
		if (g.ctw) {
			for (int32_t i = l.num_bonus_cards[7]; i > 0; i--) v = (v * 100) / (K("ctw_great_thinker_time") + 100);
		}
		// TODO: handicap.
		const int32_t tc = g.info.tech_cost;
		if (tc == 0 || tc == 2) v = (2 * v) / 3;
		else if (tc == 4 || tc == 6 || tc == 8) v = (3 * v) / 2;
		const int32_t sci = l.epoch[3];
		int32_t a = 0;
		if (is_tech_type(t)) a = ty.age;
		else if (ty.preq[0] >= 0) a = g.type(ty.preq[0]).age;
		if (a < sci) v -= ((sci - a) * K("tech_science_speedup") * v) / 100;
		if (has_wonder_prod(g, who, ptype::kSupercollider) && is_tech_type(t)) v = 0;
	}
	if (prod && !prod->is_unit && is_unassimilated(g, *static_cast<const Build*>(prod))) v = (5 * v) / 4;
	return std::max(1, v);
}

uint32_t update_construct_time(const Game& g, int32_t who, int32_t t) {
	const Type& ty = g.type(t);
	uint32_t v = type_time(g, t, who);
	const bool wonder = is_wonder_type(t);
	if (tb(g, who, 1) && !wonder) v = (v * 100u) / uint32_t(K("maya_building_speed") + 100);
	if (has_preq(g, who, 787)) v = (3u * v) >> 2;
	if (has_wonder_prod(g, who, ptype::kVersailles)) v = (v * 100u) / uint32_t(K("versailles_building_speed") + 100);
	if (has_rare(g, who, 19)) v = (v * 100u) / uint32_t(K("tobacco_building_speed") + 100);
	if (tb(g, who, 11) && is_t(g, t, ptype::kAirDefense)) v = (v * 100u) / uint32_t(K("british_aa_speed") + 100);
	const bool fort = type_is(ty, ptype::kFortX);
	if (tb(g, who, 22) && fort && !wonder) v = (v * 100u) / uint32_t(K("dutch_fort_speed") + 100);
	if (tb(g, who, 6) && fort && !wonder) v = (v * 100u) / uint32_t(K("roman_fort_speed") + 100);
	if (LD(g, who).city_num == 0) v = (uint32_t(K("capital_build_time")) * v) / 100u;
	v = (uint32_t(10 - get_building_speed_upgrade(g, who)) * v) / 10u;
	return v;
}

int32_t construct_time(const Game& g, const Build& b, int32_t all) {
	const int32_t who = b.who;
	int32_t v = b.constr_time;
	if (is_wonder_type(b.type) && tb(g, who, 20) && get_wonders(g, who) == 0 && LD(g, who).wonders_built == 0 &&
	    b.type != ptype::kSupercollider && b.type != ptype::kSpaceProgram) {
		bool other = false;
		for (int32_t i = 0; i < 8 && !other; i++) {
			if (i == who || !(LD(g, i).flags & leaderflag::kValid)) continue;
			for (const auto& ob : g.objects[size_t(i)].builds) {
				if (ob && ob->alive() && ob->type == b.type && !(ob->flags & objflag::kActive)) {
					other = true;
					break;
				}
			}
		}
		if (!other) v = 1;
	}
	if (all == 0) {
		// Hanging Gardens in the same city (hanging_gardens_build_time, 0 in the stock rules): TODO with wonder
		// cities.
		if (has_general(g, b, 0, 0x163) >= 0) v = int32_t(uint32_t(v * 100) / uint32_t(K("thepresident_building_speed") + 100));
		if (tb(g, who, 18) && is_t(g, b.type, ptype::kSenate) && K("iroquois_quick_senate") != 0 &&
		    LD(g, who).high_buildings[ptype::kSenate - kBuilds] == 0)
			v = 0;
	}
	return std::max(1, v);
}

}  // namespace ron::sim
