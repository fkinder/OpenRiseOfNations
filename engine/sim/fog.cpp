// Fog of war and line of sight (docs/fog-spec.md); see sim/fog.hpp. Every function names the original it ports.
#include "sim/heroes.hpp"
#include "sim/fog.hpp"
#include "sim/production.hpp"

#include <algorithm>
#include <cstring>

#include "sim/game.hpp"
#include "sim/placement.hpp"
#include "sim/script_host.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"

namespace ron::sim {

namespace {

// x86 `1 << (who & 31)` truncated to a byte: players 8 and 9 contribute nothing to a fog byte.
inline uint8_t bitb(int32_t who) { return uint8_t(1u << (uint32_t(who) & 31)); }

inline size_t fi(const World& w, int32_t fx, int32_t fy) { return size_t(fy) * size_t(w.fog_xs) + size_t(fx); }
inline size_t wi_of_f(const World& w, int32_t fx, int32_t fy) { return size_t(fy >> 1) * size_t(w.xs) + size_t(fx >> 1); }
inline bool in_f(const World& w, int32_t fx, int32_t fy) { return fx >= 0 && fy >= 0 && fx < w.fog_xs && fy < w.fog_ys; }

// The constants the fog code reads (rules.xml CONSTANTS by name, docs/rules-spec.md section 12.6).
struct FogConstants {
	int32_t furs_los, dogs_scout_los, troops_upgrade_los, obsidian_archers_range, spanish_los, iroquois_los,
	    turk_siege_los, liberty_siege_range, eiffel_siege_range, spy_upgrade_los, general_upgrade_los,
	    ptolemy_los_bonus, theceo_unit_los, fort_upgrade_los, tower_fort_los, colosseum_fort_range, city_center_radius,
	    city_center_pop_radius, indians_city_radius;
};
const FogConstants& fog_offsets() {
	static const FogConstants k = [] {
		FogConstants c{};
		c.furs_los = constant_offset("furs_los");
		c.dogs_scout_los = constant_offset("dogs_scout_los");
		c.troops_upgrade_los = constant_offset("troops_upgrade_los");
		c.obsidian_archers_range = constant_offset("obsidian_archers_range");
		c.spanish_los = constant_offset("spanish_los");
		c.iroquois_los = constant_offset("iroquois_los");
		c.turk_siege_los = constant_offset("turk_siege_los");
		c.liberty_siege_range = constant_offset("liberty_siege_range");
		c.eiffel_siege_range = constant_offset("eiffel_siege_range");
		c.spy_upgrade_los = constant_offset("spy_upgrade_los");
		c.general_upgrade_los = constant_offset("general_upgrade_los");
		c.ptolemy_los_bonus = constant_offset("ptolemy_los_bonus");
		c.theceo_unit_los = constant_offset("theceo_unit_los");
		c.fort_upgrade_los = constant_offset("fort_upgrade_los");  // [lvl - 1] (the code indexes fort_upgrade_range[lvl + 4])
		c.tower_fort_los = constant_offset("tower_fort_los");      // [lvl - 1] (tower_fort_range[lvl + 3])
		c.colosseum_fort_range = constant_offset("colosseum_fort_range");
		c.city_center_radius = constant_offset("city_center_radius");
		c.city_center_pop_radius = constant_offset("city_center_pop_radius");
		c.indians_city_radius = constant_offset("indians_city_radius");
		return c;
	}();
	return k;
}
int32_t konst(const Game& g, int32_t off, int32_t index = 0) {
	return off < 0 ? 0 : g.rules->constants.at(uint32_t(off + 4 * index));
}

bool leader_valid(const Game& g, int32_t p) {
	return p >= 0 && p < kNumLeaders && (g.leaders[size_t(p)].flags & leaderflag::kValid);
}
bool mutual_ally(const Game& g, int32_t a, int32_t b) {
	return g.leaders[size_t(a)].diplos(b) == 2 && g.leaders[size_t(b)].diplos(a) == 2;
}
int32_t num_units_of(const Leader& L, int32_t type) { return L.num_units[size_t(type - kUnits)]; }
// LeaderData +0x6da4 / +0x6dcc: the rare bit of a good type (TypeIndex - 6).
bool has_rare(const Leader& L, int32_t type) {
	const int32_t b = type - 6;
	return ((L.rare[size_t(b >> 3)] | L.rare_conquest[size_t(b >> 3)]) >> (b & 7)) & 1;
}
// "Reveals all enemies": REVEAL_ENEMY or a FOUCHE (num_units[FOUCHE - 50], LeaderData +0x59e4).
bool reveals_all(const Leader& L) {
	return (L.flags & leaderflag::kRevealEnemy) || num_units_of(L, fogtype::kFouche) != 0;
}

// The upgrade counters (LeaderData::get_troops_los_upgrade @6e1110, get_spy_upgrade @6e1090, get_general_upgrade
// @6e0830): bonuses lo..hi held. (Their BUY_SELL test is folded generic code that never matches these ranges.)
int32_t count_preqs(const Game& g, int32_t who, int32_t lo, int32_t hi) {
	int32_t n = 0;
	for (int32_t t = lo; t <= hi; t++) n += has_preq(g, who, t) ? 1 : 0;
	return n;
}
// LeaderData::get_fort_los @6daf30 / get_tower_fort_los @6daf80 (tech bits 68 / 67 / 66 = TypeIndex PARTISAN /
// MINUTEMAN / MILITIA, fog-spec 11.1).
int32_t get_fort_los(const Game& g, int32_t who) {
	if (has_preq(g, who, fogtype::kFortLos4)) return 4;
	if (has_preq(g, who, fogtype::kFortLos3)) return 3;
	return has_preq(g, who, fogtype::kFortLos2) ? 2 : 1;
}
int32_t get_tower_fort_los(const Leader& L) {
	const uint8_t b = L.tech[8];
	if (b & 0x10) return 4;
	if (b & 8) return 3;
	return (b & 4) ? 2 : 1;
}

// The science epoch byte as the LOS code reads it: (epoch[3] ^ 0x63187) low byte = the science level. Read from epoch[3] itself, not from
// the `Leader::science` mirror: the original has no mirror, and a loaded game leaves the mirror 0 until the leader gains an epoch tech
// (Wall::update_los @63eeb0 reads LeaderDataEncrypt +0xf4 ^ 0x87 on every recompute).
uint8_t science_byte(const Leader& L) { return uint8_t(L.epoch[3]); }

// Type predicates (folded vtable slots, see CLAUDE.md on identical-COMDAT folding).
bool type_is_fort(const Type& t) { return t.kind == Kind::Build && type_is(t, fogtype::kFort); }  // BuildTypeData::is_fort
bool type_is_siege(const Type& t) { return (t.unit_flags & 0x20000) != 0; }                    // UnitTypeData::is_siege
bool object_is_wonder(const Object& ob) {  // BuildData::is_wonder (units and walls: folded `return 0`)
	return !ob.is_unit && ob.type >= fogtype::kWonderFirst && ob.type <= fogtype::kWonderLast;
}
bool object_started(const Object& ob) { return (ob.flags & objflag::kStarted) != 0; }
bool object_active(const Object& ob) { return (ob.flags & objflag::kActive) != 0; }
// The visible-wonder test of Object::update_seen (ObjectData::is_visible_wonder + get_build()->is_started).
bool started_visible_wonder(const Object& ob) {
	return object_is_wonder(ob) && !(ob.flags & objflag::kCity) && !type_is_fort(*ob.ptype) && object_started(ob);
}
bool is_gaia_animal(const Object& ob) { return ob.is_unit && ob.type >= kGaia && ob.type < kEndGaia; }

// UnitData::is_packing @60aa60: the current order is a CAST_SPELL of a pack spell (sim/spells.hpp).
bool unit_is_packing(const Unit& u) { return cast_is_packing(u); }
// ObjectData::has_general @646b00 (sim/heroes.hpp).
int32_t object_has_general(const Game& g, const Unit& u, int32_t mask, int32_t type) {
	return has_general(g, u, uint32_t(mask), type);
}

}  // namespace

// ---------------------------------------------------------------- primitive writers

bool fog_set_seen(World& w, int32_t fx, int32_t fy, int32_t who, bool detect) {
	const size_t i = fi(w, fx, fy);
	const uint8_t b = who < 0 ? 0xff : bitb(who);
	const uint8_t s_old = w.seen[i], s2_old = w.seen2[i];
	w.seen[i] = uint8_t(s_old | b);
	if (detect) w.seen3[i] = uint8_t(w.seen3[i] | b);
	w.seen2[i] = uint8_t(s2_old | b);
	const size_t wi = wi_of_f(w, fx, fy);
	w.wdata[wi].was_seen = uint8_t(w.wdata[wi].was_seen | b);
	w.wcoord_seen[wi] = uint8_t(w.wcoord_seen[wi] | b);
	if (w.seen2[i] != s2_old || w.seen[i] != s_old) w.fog_changes++;  // WorldMapFog::add_update
	return w.seen2[i] != s2_old;
}

bool fog_set_seen2(World& w, int32_t fx, int32_t fy, uint8_t mask, bool explored_only) {
	const size_t i = fi(w, fx, fy);
	const uint8_t s2_old = w.seen2[i], s_old = w.seen[i];
	w.seen2[i] = uint8_t(s2_old | mask);
	const size_t wi = wi_of_f(w, fx, fy);
	if (!explored_only) {
		w.seen[i] = uint8_t(s_old | mask);
		w.wcoord_seen[wi] = uint8_t(w.wcoord_seen[wi] | mask);
	}
	w.wdata[wi].was_seen = uint8_t(w.wdata[wi].was_seen | mask);
	if (w.seen2[i] == s2_old && w.seen[i] == s_old) return false;
	w.fog_changes++;
	return true;
}

void fog_set_was_seen(World& w, int32_t fx, int32_t fy, int32_t who) {
	const uint8_t b = bitb(who);
	const size_t wi = wi_of_f(w, fx, fy);
	w.wdata[wi].was_seen = uint8_t(w.wdata[wi].was_seen | b);
	const size_t i = fi(w, fx, fy);
	if ((w.seen2[i] | b) != w.seen2[i]) w.fog_changes++;
	w.seen2[i] = uint8_t(w.seen2[i] | b);
}

void fog_clear_seen(World& w) {
	std::fill(w.seen.begin(), w.seen.end(), uint8_t(0));
	std::fill(w.wcoord_seen.begin(), w.wcoord_seen.end(), uint8_t(0));
	w.fog_changes++;
}

void fog_clear_seen3(World& w) { std::fill(w.seen3.begin(), w.seen3.end(), uint8_t(0)); }

// ---------------------------------------------------------------- queries

bool is_seen(const Game& g, int32_t fx, int32_t fy, int32_t who) {
	if (who > 7 || who < 0 || g.info.reveal_map == 3) return true;
	const Leader& L = g.leaders[size_t(who)];
	if (reveals_all(L)) return true;
	const World& w = g.world;
	if (L.flags & leaderflag::kActivePing) {
		const int32_t t = w.wdata[wi_of_f(w, fx, fy)].who;
		if (t >= 0 && is_ally(g, who, t)) return true;
	}
	return (w.seen[fi(w, fx, fy)] & L.ally_mask) != 0;
}

bool is_really_seen(const Game& g, int32_t fx, int32_t fy, int32_t who) {
	if (who > 7 || who < 0) return true;
	const Leader& L = g.leaders[size_t(who)];
	if (reveals_all(L)) return true;
	const World& w = g.world;
	if (L.flags & leaderflag::kActivePing) {
		const int32_t t = w.wdata[wi_of_f(w, fx, fy)].who;
		if (t >= 0 && is_ally(g, who, t)) return true;
	}
	return (w.seen[fi(w, fx, fy)] & L.ally_mask) != 0;
}

bool was_seen(const Game& g, int32_t fx, int32_t fy, int32_t who) {
	if (who > 7 || who < 0 || g.info.reveal_map > 1) return true;
	const Leader& L = g.leaders[size_t(who)];
	if ((L.flags & leaderflag::kExploreMap) || reveals_all(L)) return true;
	const World& w = g.world;
	const WData& d = w.wdata[wi_of_f(w, fx, fy)];
	if (d.who >= 0 && is_ally(g, who, d.who) && d.region >= 0 && d.region < 64) {
		const Leader& A = g.leaders[size_t(d.who)];
		if (A.reg_cities[size_t(d.region)] != 0 || A.reg_forts[size_t(d.region)] != 0) return true;
	}
	return (w.seen2[fi(w, fx, fy)] & L.ally_mask) != 0;
}

bool was_really_seen(const Game& g, int32_t fx, int32_t fy, int32_t who) {
	if (who > 7 || who < 0 || g.info.reveal_map == 3) return true;
	const Leader& L = g.leaders[size_t(who)];
	if (reveals_all(L)) return true;
	return (g.world.seen2[fi(g.world, fx, fy)] & L.ally_mask) != 0;
}

bool is_seen_wcoord(const Game& g, int32_t wx, int32_t wy, int32_t who) {
	if (who > 7 || who < 0 || g.info.reveal_map == 3) return true;
	const Leader& L = g.leaders[size_t(who)];
	if (reveals_all(L)) return true;
	const World& w = g.world;
	if (L.flags & leaderflag::kActivePing) {
		const int32_t t = w.w(wx, wy).who;  // WorldData::get_whose
		if (t >= 0 && is_ally(g, who, t)) return true;
	}
	return (w.wcoord_seen[w.wi(wx, wy)] & L.ally_mask) != 0;
}

bool was_seen_wcoord(const Game& g, int32_t wx, int32_t wy, int32_t who) {
	if (who > 7 || who < 0 || g.info.reveal_map > 1) return true;
	const Leader& L = g.leaders[size_t(who)];
	if ((L.flags & leaderflag::kExploreMap) || reveals_all(L)) return true;
	const WData& d = g.world.w(wx, wy);
	if (d.who >= 0 && is_ally(g, who, d.who) && d.region >= 0 && d.region < 64) {
		const Leader& A = g.leaders[size_t(d.who)];
		if (A.reg_cities[size_t(d.region)] != 0 || A.reg_forts[size_t(d.region)] != 0) return true;
	}
	return (d.was_seen & L.ally_mask) != 0;
}

bool is_detected(const Game& g, int32_t fx, int32_t fy, int32_t who) {
	if (who < 0 || who >= kNumLeaders) return false;
	return (g.world.seen3[fi(g.world, fx, fy)] & g.leaders[size_t(who)].ally_mask) != 0;
}

bool is_detected_by_enemy(const Game& g, int32_t fx, int32_t fy, int32_t who) {
	if (who < 0 || who >= kNumLeaders) return false;
	return (g.world.seen3[fi(g.world, fx, fy)] & uint8_t(~g.leaders[size_t(who)].ally_mask)) != 0;
}

// ---------------------------------------------------------------- leader helpers

bool has_tribe_bonus(const Game& g, int32_t who, int32_t bonus) {
	if (who < 0 || who >= kNumLeaders) return false;
	const Leader& L = g.leaders[size_t(who)];
	if ((g.info.flags & 4) || (g.info.starting_town == 0 && L.city_num == 0) || L.tribe < 0) return false;
	if (bonus >= 0 && bonus < 32 && ((L.conquest_racial_powers[size_t(bonus >> 3)] >> (bonus & 7)) & 1)) return true;
	if (L.flags2 & 0x40) return false;
	if (size_t(L.tribe) >= g.rules->tribes.size()) return false;
	return g.rules->tribes[size_t(L.tribe)].tribe == bonus;
}

// LeaderData::has_preq / has_wonder: the production module's faithful versions (techs.cpp, cities.cpp).
bool has_wonder(const Game& g, int32_t who, int32_t type) { return has_wonder_prod(g, who, type); }

void leader_meet(Game& g, int32_t who, int32_t p) {
	// Leader::treaty_on @6e1190 (the say_meet messages are presentation).
	g.leaders[size_t(who)].treaty(p) |= 1;
	g.leaders[size_t(p)].treaty(who) |= 1;
}

void leader_new_rare(Game& g, int32_t who, int32_t good) {
	const Leader& W = g.leaders[size_t(who)];
	if ((W.flags & 0xc) == leaderflag::kHuman || good < 0) return;  // human seats without co-op AI keep no list
	const GoodObj& gd = g.goods[size_t(good)];
	for (int32_t j = 0; j < 8; j++) {
		Leader& L = g.leaders[size_t(j)];
		if (!(L.flags & leaderflag::kValid)) continue;
		if (!(j == who || mutual_ally(g, j, who))) continue;
		if ((L.flags & leaderflag::kHuman) && !(W.flags & leaderflag::kCoopSolo)) continue;
		if (std::find(L.new_rares.begin(), L.new_rares.end(), good) != L.new_rares.end()) continue;
		if (type_avail(g, j, gd.type, 1) == 0) continue;  // LeaderData::type_avail(good type, 1)
		const Type& t = g.type(gd.type);
		if (type_is(t, fogtype::kFish) || type_is(t, fogtype::kWhales)) continue;
		L.new_rares.push_back(good);
	}
}

void leader_check_explore(Game& g, int32_t who) {
	const int32_t period = 200 / g.ai_speed;
	if (g.frame != 0 && (who * 25 + g.frame + 12) % period != 0) return;
	Leader& L = g.leaders[size_t(who)];
	const World& w = g.world;
	if (has_preq(g, who, fogtype::kExploreMapBonus)) {
		L.explored = w.reg_size;
		return;
	}
	L.explored = 0;
	const uint8_t b = bitb(who);
	for (int32_t k = 0; k < w.reg_ys; k++) {
		const int32_t fy = 4 * k + 3;
		if (fy >= w.fog_ys) break;
		for (int32_t i = 0; i < w.reg_xs; i++) {
			const int32_t fx = 4 * i + 3;
			if (fx >= w.fog_xs) break;
			if (w.seen2[fi(w, fx, fy)] & b) L.explored++;
		}
	}
}

void leader_init_ally_mask(Game& g, int32_t who) {
	Leader& L = g.leaders[size_t(who)];
	L.ally_mask = bitb(who);
	if (!(has_preq(g, who, fogtype::kAllyLos) || g.info.reveal_map != 0)) return;
	for (int32_t j = 0; j < 8; j++) {
		if (j != who && is_ally(g, who, j)) L.ally_mask = uint8_t(L.ally_mask | bitb(j));
	}
}

// ---------------------------------------------------------------- LOS

int32_t unit_update_los(Game& g, Unit& u) {
	if (is_gaia_animal(u)) {  // Animal::update_los @5d7d50
		u.mylos = 0;
		return 0;
	}
	const Leader& L = g.leaders[size_t(u.who)];
	const Type& t = *u.ptype;
	const FogConstants& K = fog_offsets();
	const int8_t base = (L.flags & leaderflag::kValid) ? int8_t(t.los) : int8_t(0);
	u.mylos = base;
	if (base == 0) return 0;
	auto add = [&](int32_t v) { u.mylos = int8_t(int32_t(u.mylos) + v); };
	if (has_rare(L, fogtype::kFurs)) u.mylos = int8_t(int32_t(base) + int8_t(konst(g, K.furs_los)));
	if (has_rare(L, fogtype::kDogs) && type_is(t, fogtype::kScout)) add(int8_t(konst(g, K.dogs_scout_los)));
	if (u.type == fogtype::kPeasants || u.type == fogtype::kPeasantsKorean) {
		if (L.tech[8] & 4) add(2);
		if (L.tech[8] & 8) add(2);
		if ((L.tech[8] & 0x10) || g.info.starting_town == 0) add(2);
	}
	const uint8_t sci = science_byte(L);
	add(int32_t(sci) * int32_t(int8_t(t.science_los)));
	bool merchant_check = true;
	if (t.unit_flags2 & 4) {  // packers
		if ((u.unit_masks & umask::kPacked) || unit_is_packing(u)) {
			if (u.mylos > 3) u.mylos = 4;
			merchant_check = false;
		}
	}
	if (merchant_check && (u.type == fogtype::kMerchant || u.type == fogtype::kMerchantDutch ||
	                       u.type == fogtype::kFurTrapper)) {
		u.mylos = int8_t(sci + 4);
	}
	if (t.where == fogtype::kBarracks || t.where == fogtype::kStable || t.where == fogtype::kAutoplant) {
		const int32_t n = count_preqs(g, u.who, fogtype::kTroopsLos1, fogtype::kTroopsLos3);
		add(int32_t(int8_t(n)) * int32_t(int8_t(konst(g, K.troops_upgrade_los))));
		if ((t.obj_masks & 0x4000) || (t.obj_masks & 0x400)) {  // HORSE_ARCHER / FOOT_ARCHER
			if (has_rare(L, fogtype::kObsidian)) add(int8_t(konst(g, K.obsidian_archers_range)));
		}
	}
	if (has_tribe_bonus(g, u.who, 9) && type_is(t, fogtype::kScout)) add(int8_t(konst(g, K.spanish_los)));
	if (has_tribe_bonus(g, u.who, 18)) {
		const bool citizen = u.type == fogtype::kPeasants || u.type == fogtype::kPeasantsKorean;
		if (citizen || (!type_is(t, fogtype::kScout) && t.where == fogtype::kBarracks)) add(int8_t(konst(g, K.iroquois_los)));
	}
	if (type_is_siege(t)) {
		if (has_tribe_bonus(g, u.who, 8)) add(int8_t(konst(g, K.turk_siege_los)));
		if (has_wonder(g, u.who, fogtype::kLibertyStatue)) add(int8_t(konst(g, K.liberty_siege_range)));
		if (has_wonder(g, u.who, fogtype::kEiffelTower)) add(int8_t(konst(g, K.eiffel_siege_range)));
	}
	if (type_is(t, fogtype::kSpy)) {
		const int32_t n = count_preqs(g, u.who, fogtype::kSpiesUpgrade1, fogtype::kSpiesUpgrade3);
		add(int32_t(int8_t(n)) * int32_t(int8_t(konst(g, K.spy_upgrade_los))));
	}
	if (t.unit_flags2 & 0x20) {  // UnitData::is_hero
		const int32_t n = count_preqs(g, u.who, fogtype::kGeneralsUpgrade1, fogtype::kGeneralsUpgrade3);
		add(int32_t(int8_t(n)) * int32_t(int8_t(konst(g, K.general_upgrade_los))));
	}
	if (u.unit_masks & umask::kDecoy) u.mylos = 1;
	return u.mylos;
}

int32_t build_update_los(Game& g, Build& b) {
	const Leader& L = g.leaders[size_t(b.who)];
	const Type& t = *b.ptype;
	const FogConstants& K = fog_offsets();
	if (!object_started(b) || (!object_active(b) && !object_is_wonder(b))) {
		b.mylos = 0;
		return 0;
	}
	if (!object_active(b)) {
		b.mylos = 1;  // a started, unfinished wonder
	} else {
		const int8_t base = (L.flags & leaderflag::kValid) ? int8_t(t.los) : int8_t(0);
		b.mylos = int8_t(int32_t(science_byte(L)) * int32_t(int8_t(t.science_los)) + base);
		auto add = [&](int32_t v) { b.mylos = int8_t(int32_t(b.mylos) + v); };
		const bool fort = type_is_fort(t);
		const bool tower = !fort && type_is(t, fogtype::kTower);
		if (fort) add(int8_t(konst(g, K.fort_upgrade_los, get_fort_los(g, b.who) - 1)));
		else if (tower) add(int8_t(konst(g, K.tower_fort_los, get_tower_fort_los(L) - 1)));
		if ((fort || tower) && has_wonder(g, b.who, fogtype::kColosseum)) add(int8_t(konst(g, K.colosseum_fort_range)));
		if (has_rare(L, fogtype::kFurs)) add(int8_t(konst(g, K.furs_los)));
	}
	b.mylos = int8_t(int32_t(b.mylos) + int8_t(t.x_size / 2));
	return b.mylos;
}

int32_t object_update_los(Game& g, Object& ob) {
	return ob.is_unit ? unit_update_los(g, static_cast<Unit&>(ob)) : build_update_los(g, static_cast<Build&>(ob));
}

int32_t object_los(const Game& g, const Object& ob) {
	int32_t los = ob.mylos;
	if (!ob.is_unit) return los;  // WallData::los
	const Unit& u = static_cast<const Unit&>(ob);
	if (is_gaia_animal(u)) return los;
	const Leader& L = g.leaders[size_t(u.who)];
	const FogConstants& K = fog_offsets();
	if (num_units_of(L, fogtype::kPtolemy) != 0 && (u.ptype->role & 0x420) &&
	    object_has_general(g, u, 0, fogtype::kPtolemy) >= 0)
		los += konst(g, K.ptolemy_los_bonus);
	if (num_units_of(L, fogtype::kTheCeo) != 0 && !type_is_siege(*u.ptype) &&
	    object_has_general(g, u, 0, fogtype::kTheCeo) >= 0)
		los += konst(g, K.theceo_unit_los);
	return los;
}

// ---------------------------------------------------------------- stamping

StampRange stamp_range(int32_t los, bool move, bool projected, bool detector) {
	const Circles& C = circles();
	const Rings& R = rings();
	StampRange s;
	if (los == 0) return s;
	int32_t r = (los * 0xc0) / 0x180;
	if (r > 64) r = 64;
	if (r < 0) return s;  // negative LOS: the original reads before the tables; stamp nothing
	int32_t start = 0;
	bool ring = false;
	if (r < 4) {
		if (projected) start = move ? C.radius[0] : 0;  // circle_radius[max(r - 5, 0)] with r < 4
		else start = (move && r - 1 >= 1) ? C.radius[size_t(r - 1)] : 0;
	} else if (move) {
		if (r > 32) r = 32;
		start = R.radius[size_t(std::max(r - 1, 0))];
		ring = true;
	}
	int32_t det = 0;
	if (detector) {
		if (ring) {
			if (r > 31) r = 31;
			det = R.radius[size_t(r)];
		} else {
			det = C.radius[size_t(r)];
		}
	}
	int32_t end;
	if (!move) {
		start = 0;
		end = C.radius[size_t(r)];
	} else {
		end = ring ? R.radius[size_t(r)] : C.radius[size_t(r)];
	}
	s.r = r;
	s.ring = ring;
	s.start = start;
	s.end = end;
	s.detect_end = det;
	return s;
}

void object_update_seen(Game& g, Object& ob, bool move) {
	World& w = g.world;
	const bool vw = started_visible_wonder(ob);
	const int32_t los = object_los(g, ob);
	if (los == 0) return;
	// Centre: own fog cell, or for small-LOS ground units (not packers, not decoys) 384 units ahead.
	int32_t r0 = (los * 0xc0) / 0x180;
	if (r0 > 64) r0 = 64;
	bool projected = false;
	int32_t cx = fcoord_of(ob.x), cy = fcoord_of(ob.y);
	if (r0 < 4 && ob.is_unit) {
		const Unit& u = static_cast<const Unit&>(ob);
		if (u.ptype->domain == 0 && !(u.ptype->unit_flags2 & 4) && !(u.unit_masks & umask::kDecoy)) {
			int32_t px = 0, py = 0;
			project(ob.x, ob.y, u.angle, 0x180, &px, &py);
			cx = fcoord_of(px);
			cy = fcoord_of(py);
			projected = true;
		}
	}
	const StampRange s = stamp_range(los, move, projected, (ob.flags & objflag::kDetector) != 0);
	if (!move && (ob.visible != 0 || vw)) object_update_local_seen(g, ob);
	const int32_t r = s.r;
	const bool inside = r + cx >= 0 && r + cy >= 0 && r + cx < w.fog_xs && r + cy < w.fog_ys && cx - r >= 0 &&
	                    cy - r >= 0 && cx - r < w.fog_xs && cy - r < w.fog_ys;
	const std::vector<int8_t>& X = s.ring ? rings().x : circles().x;
	const std::vector<int8_t>& Y = s.ring ? rings().y : circles().y;
	const int32_t who = ob.who, o = ob.o;
	for (int32_t k = s.start; k < s.end; k++) {
		const int32_t fx = cx + X[size_t(k)], fy = cy + Y[size_t(k)];
		if (!inside && !in_f(w, fx, fy)) continue;
		if (fog_set_seen(w, fx, fy, who, k < s.detect_end)) reveal_fog(g, fx, fy, who, o);
		if (ob.infiltrated != 0) fog_set_seen2(w, fx, fy, uint8_t(ob.infiltrated), false);
	}
}

void object_update_local_seen(Game& g, Object& ob) {
	World& w = g.world;
	if (ob.is_unit) {
		// Unit::update_local_seen @60e410: the circle of radius type.x_size (fog cells) around the unit's fog cell.
		if (ob.visible == 0) return;
		const Circles& C = circles();
		const int32_t xs = std::clamp(ob.ptype->x_size, 0, 64);
		const int32_t cx = fcoord_of(ob.x), cy = fcoord_of(ob.y);
		for (int32_t k = 0; k < C.radius[size_t(xs)]; k++) {
			const int32_t fx = cx + C.x[size_t(k)], fy = cy + C.y[size_t(k)];
			if (in_f(w, fx, fy)) fog_set_seen2(w, fx, fy, uint8_t(ob.visible), false);
		}
		return;
	}
	// Wall::update_local_seen @63ed50: the footprint plus a one-tile border.
	const Build& b = static_cast<const Build&>(ob);
	const bool wonderish = object_is_wonder(b) && !(b.flags & objflag::kCity) && !type_is_fort(*b.ptype);
	const uint8_t mask = (wonderish && object_started(b))
	                         ? uint8_t(0xff)
	                         : uint8_t(b.ever_seen | uint8_t(b.visible) | bitb(b.who));
	const Footprint f = footprint(*b.ptype, b.x, b.y);
	for (int32_t tx = f.x0 - 1; tx <= f.x0 + b.ptype->x_size; tx++) {
		for (int32_t ty = f.y0 - 1; ty <= f.y0 + b.ptype->y_size; ty++) {
			if (!w.in_t(tx, ty)) continue;
			fog_set_seen2(w, tx >> 1, ty >> 1, mask, !wonderish);
		}
	}
}

int32_t find_good_at_wcoord(const Game& g, int32_t wx, int32_t wy) {
	const WData& d = g.world.w(wx, wy);
	int32_t o = d.down, who = d.down_who;
	for (int guard = 0; o >= 0 && guard < 100000; guard++) {
		const Object* ob = g.object(who, o);
		if (!ob) return -1;
		o = ob->down;
		who = ob->down_who;
	}
	if (o != -2 || who < 0 || size_t(who) >= g.goods.size()) return -1;
	const GoodObj& gd = g.goods[size_t(who)];
	if (!gd.alive() || gd.type == fogtype::kOil) return -1;
	return who;
}

int32_t find_goody_at_wcoord(const Game& g, int32_t wx, int32_t wy) {
	const WData& d = g.world.w(wx, wy);
	int32_t o = d.down, who = d.down_who;
	for (int guard = 0; o >= 0 && guard < 100000; guard++) {
		const Object* ob = g.object(who, o);
		if (!ob) return -1;
		o = ob->down;
		who = ob->down_who;
	}
	if (o != -3 || who < 0 || size_t(who) >= g.items.size()) return -1;
	return g.items[size_t(who)].alive() ? who : -1;
}

// Unit::get_goody_box @5f7690: a one-unit group moves (QUEUE_FIRST, EXPLORE_TO) to the goody box's wcoord centre.
void unit_get_goody_box(Game& g, Unit& u, int32_t wx, int32_t wy) {
	Group tmp;
	group_clear(tmp, -1);
	tmp.id = -1;
	tmp.stamp = 0;
	group_add(g, tmp, u.o, u.who, 0, 0);
	const int32_t gi = groups_push_group(g, u.who, tmp, 1);
	if (gi < 0) return;
	group_action_move_to(g, g.groups.g[size_t(gi)], wx * 0x300 + 0x180, wy * 0x300 + 0x180, kQueueFirst, false, 0,
	                     kExploreTo, 0, -1, -1, 0);
}

void reveal_fog(Game& g, int32_t fx, int32_t fy, int32_t who, int32_t o) {
	World& w = g.world;
	// 1. Rare resources: the tcoord (2fx + 1, 2fy + 1) has RESOURCE and its wcoord list ends in a live good.
	const int32_t tx = 2 * fx + 1, ty = 2 * fy + 1;
	if (w.in_t(tx, ty) && (w.t(tx, ty) & tmask::kResource)) {
		const int32_t gi = find_good_at_wcoord(g, tx >> 2, ty >> 2);
		if (gi >= 0 && g.goods[size_t(gi)].alive()) {
			leader_new_rare(g, who, gi);
			// (the "found a rare resource" message for the local seat is UI)
			g.goods[size_t(gi)].ever_seen = uint8_t(g.goods[size_t(gi)].ever_seen | bitb(who));
		}
	}
	// 2. Oil: the first live good (any type) in the wcoord joins L[who].oil_patches.
	const int32_t wx = fx >> 1, wy = fy >> 1;
	const WData& d = w.w(wx, wy);
	if (d.flags & wflag::kOil) {
		for (int32_t i = 0; i < g.good_mark && size_t(i) < g.goods.size(); i++) {
			const GoodObj& gd = g.goods[size_t(i)];
			if (!gd.alive() || wcoord_of(gd.x) != wx || wcoord_of(gd.y) != wy) continue;
			std::vector<int32_t>& op = g.leaders[size_t(who)].oil_patches;
			if (std::find(op.begin(), op.end(), i) == op.end()) op.push_back(i);
			break;
		}
	}
	// 3. Goody boxes (ruins): not on open ocean.
	if (!(d.flags & wflag::kGoody)) return;
	if (!World::is_ocean(d)) {
		const int32_t it = find_goody_at_wcoord(g, wx, wy);
		if (it >= 0) g.items[size_t(it)].ever_seen = uint8_t(g.items[size_t(it)].ever_seen | bitb(who));
	}
	if (o >= 0 && o < kUnitLimit) {
		Unit* u = g.unit(who, o);
		if (u && (u->unit_masks & umask::kAutoExplore)) unit_get_goody_box(g, *u, wx, wy);
	}
}

void update_seen_ally(Game& g, Object& ob) {
	if (!ob.is_unit && !object_active(ob)) return;
	World& w = g.world;
	int32_t r = object_los(g, ob);
	if (ob.flags & objflag::kCity) {
		// LeaderData::get_radius @6db790 of the city's building type, halved (signed, toward zero).
		const FogConstants& K = fog_offsets();
		const int32_t bt = ob.type;
		const int32_t size = bt == 415 ? 2 : (bt == 416 || bt == 531) ? 3 : 1;
		int32_t rad = (size - 1) * konst(g, K.city_center_pop_radius);
		if (has_tribe_bonus(g, ob.who, 0x15)) rad += konst(g, K.indians_city_radius);
		rad += konst(g, K.city_center_radius);
		if (rad > 64) rad = 64;
		r = (rad * 2) / 4;
	}
	if (r == 0) return;
	if (r > 64) r = 64;
	if (r < 0) return;  // (the original would index before circle_radius)
	const int32_t owner = ob.who;
	const Leader& O = g.leaders[size_t(owner)];
	auto shares = [&](int32_t j) {
		if (j == owner || mutual_ally(g, owner, j)) return true;
		if (!g.multiplayer && (O.flags & leaderflag::kHuman) && (g.leaders[size_t(j)].flags & leaderflag::kHuman)) return true;
		const int8_t team = g.info.team[size_t(owner)];  // LeaderData::get_team (8 when not seated)
		return g.info.game_rules == 8 && team == 0;
	};
	if (!ob.is_unit) {
		Build& b = static_cast<Build&>(ob);
		for (int32_t j = 0; j < 8; j++) {
			if (leader_valid(g, j) && shares(j)) b.ever_seen = uint8_t(b.ever_seen | bitb(j));
		}
	}
	const Circles& C = circles();
	const int32_t cx = fcoord_of(ob.x), cy = fcoord_of(ob.y);
	for (int32_t k = 0; k < C.radius[size_t(r)]; k++) {
		const int32_t fx = cx + C.x[size_t(k)], fy = cy + C.y[size_t(k)];
		if (!in_f(w, fx, fy)) continue;
		for (int32_t j = 0; j < 8; j++) {
			if (!leader_valid(g, j) || !shares(j)) continue;
			const size_t wi = wi_of_f(w, fx, fy), i = fi(w, fx, fy);
			w.wdata[wi].was_seen = uint8_t(w.wdata[wi].was_seen | bitb(j));
			if ((w.seen2[i] | bitb(j)) != w.seen2[i]) w.fog_changes++;
			w.seen2[i] = uint8_t(w.seen2[i] | bitb(j));
		}
	}
}

void update_all_seen(Game& g) {
	if (g.info.reveal_map == 3) return;
	World& w = g.world;
	g.daemon.busy = 4;
	fog_clear_seen(w);
	fog_clear_seen3(w);
	// Buildings then walls of leaders 0..7: active -> full stamp; a started wonder -> update_local_seen only.
	for (int32_t who = 0; who < 8; who++) {
		if (!leader_valid(g, who)) continue;
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			Build* b = g.build(who, o);
			if (!b || !b->alive()) continue;
			if (object_active(*b)) object_update_seen(g, *b, false);
			else if (object_is_wonder(*b) && object_started(*b)) object_update_local_seen(g, *b);
		}
		// walls [3000, wall_mark): never allocated (frame-spec 8.1)
	}
	// Units on the map of leaders 0..7.
	for (int32_t who = 0; who < 8; who++) {
		if (!leader_valid(g, who)) continue;
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			Unit* u = g.unit(who, o);
			if (u && u->alive() && u->on_map()) object_update_seen(g, *u, false);
		}
	}
	// Scripted reveal points. The original reads ScenarioData::reveal_points[0] (scenario player 1's list) for every
	// VALID leader p (docs/fog-spec.md section 5.2).
	if (g.scripted_rules || g.ctw) {
		const Circles& C = circles();
		for (int32_t p = 0; p < 8; p++) {
			if (!leader_valid(g, p)) continue;
			for (const Game::RevealPoint& pt : g.reveal_points[0]) {
				const int32_t r = std::clamp((pt.radius * 0xc0) / 0x180, 0, 64);
				for (int32_t k = 0; k < C.radius[size_t(r)]; k++) {
					const int32_t fx = C.x[size_t(k)] + (pt.tx >> 1), fy = C.y[size_t(k)] + (pt.ty >> 1);
					if (!in_f(w, fx, fy)) continue;
					if (fog_set_seen(w, fx, fy, p, false)) reveal_fog(g, fx, fy, p, -1);
				}
			}
		}
	}
	// Frame 0 only: allies start with each other's explored area.
	if (g.frame == 0) {
		uint32_t covered = 0;
		for (int32_t i = 0; i < 8; i++) {
			if (!leader_valid(g, i)) continue;
			uint32_t m = 1u << i;
			for (int32_t j = 0; j < 8; j++) {
				if (j == i) continue;
				if (mutual_ally(g, i, j) || (g.leaders[size_t(i)].ally_mask & (1u << j))) m |= 1u << j;
			}
			if ((m & covered) == m) continue;
			if (g.info.starting_resources != 0 || g.info.reveal_map != 0) {
				const uint8_t mb = uint8_t(m);
				for (uint8_t& c : w.seen2) {
					if (c & mb) c = uint8_t(c | mb);
				}
				w.fog_changes++;
			}
			covered |= m;
		}
	}
}

void check_ever_seen(Game& g, Build& b, bool force) {
	const World& w = g.world;
	const uint8_t old = b.ever_seen;
	const Leader& O = g.leaders[size_t(b.who)];
	const bool started = object_started(b);
	const uint32_t S = started ? g.everyone_mask : O.ally_mask;
	if ((b.ever_seen & S) != S || (b.ever_seen_completed & S) != S) {
		const Footprint f = footprint(*b.ptype, b.x, b.y);
		for (int32_t x = f.x0; x < f.x0 + b.ptype->x_size; x++) {
			for (int32_t y = f.y0; y < f.y0 + b.ptype->y_size; y++) {
				const int32_t fx = x >> 1, fy = y >> 1;
				if (!in_f(w, fx, fy)) continue;  // (the original does not bounds-check the footprint)
				const uint8_t s = w.seen[fi(w, fx, fy)];
				if (!started) {
					b.ever_seen = uint8_t(b.ever_seen | (s & O.ally_mask));
				} else {
					b.ever_seen = uint8_t(b.ever_seen | s);
					if (object_active(b)) b.ever_seen_completed = uint8_t(b.ever_seen_completed | s);
				}
			}
		}
	}
	if (old == b.ever_seen && !force) return;
	bool any = false;
	for (int32_t p = 0; p < 8; p++) {
		if (p == b.who || !leader_valid(g, p)) continue;
		const uint8_t am = g.leaders[size_t(p)].ally_mask;
		if (!(b.ever_seen & am) || (old & am)) continue;
		any = true;
		if (!(g.leaders[size_t(b.who)].treaty(p) & 1)) leader_meet(g, b.who, p);
	}
	if (any) object_update_local_seen(g, b);
}

// ---------------------------------------------------------------- per-object visibility

bool unit_is_cloaked(const Unit& u) {
	if ((u.unit_masks & umask::kUnderCover) || (u.ptype->unit_flags & 0x4000) || (u.unit_masks2 & 0x8000)) return true;
	if (!(u.ptype->unit_flags & 0x40000)) return false;
	return u.orders.empty();  // stealth while idle (get_order() == null)
}

bool unit_is_detected(const Game& g, const Unit& u, int32_t who) {
	if (who == u.who) return true;
	if (who < 0 || who >= kNumLeaders) return false;
	const World& w = g.world;
	const int32_t fx = fcoord_of(u.x), fy = fcoord_of(u.y);
	if (!in_f(w, fx, fy)) return false;
	return (g.leaders[size_t(who)].ally_mask & w.seen3[fi(w, fx, fy)]) != 0;
}

namespace {

bool fog_cell_of(const Game& g, const Object& ob, int32_t* fx, int32_t* fy) {
	*fx = fcoord_of(ob.x);
	*fy = fcoord_of(ob.y);
	return in_f(g.world, *fx, *fy);
}

// WallData::is_seen @642bd0.
bool wall_is_seen(const Game& g, const Build& b, int32_t who) {
	if (who == b.who) return true;
	if (who < 0 || who >= kNumLeaders) return true;
	const Leader& P = g.leaders[size_t(who)];
	if (!object_started(b) && !(P.ally_mask & bitb(b.who))) return false;
	if (g.info.reveal_map == 3 || (b.ever_seen & P.ally_mask) || reveals_all(P)) return true;
	return (b.flags & objflag::kCity) && !g.multiplayer && (P.flags & leaderflag::kHuman) &&
	       (g.leaders[size_t(b.who)].flags & leaderflag::kHuman);
}

// WallData::is_now_seen @642ca0.
bool wall_is_now_seen(const Game& g, const Build& b, int32_t who) {
	if (who == b.who) return true;
	if (who < 0 || who >= kNumLeaders) return true;
	const Leader& P = g.leaders[size_t(who)];
	if (!object_started(b) && !(P.ally_mask & bitb(b.who))) return false;
	if (g.info.reveal_map == 3) return true;
	if ((b.flags & objflag::kCity) && !g.multiplayer && (g.leaders[size_t(b.who)].flags & leaderflag::kHuman))
		return true;
	int32_t fx, fy;
	if (!fog_cell_of(g, b, &fx, &fy)) return false;
	return is_seen(g, fx, fy, who);
}

// The BuildData prefix of is_seen / is_now_seen: revealed by attacking, or by a spy of `who` inside.
int build_revealed(const Build& b, int32_t who) {
	const uint32_t bit = 1u << (uint32_t(who) & 31);
	if (bit & uint32_t(int32_t(b.visible))) return 1;
	if (who == b.who || b.infiltrated == 0) return 0;
	if (who < 0) return 1;
	return (bit & uint32_t(int32_t(b.infiltrated))) ? 1 : 0;
}

}  // namespace

bool object_is_seen(const Game& g, const Object& ob, int32_t who, bool ignore_cloak) {
	if (!ob.is_unit) {
		const Build& b = static_cast<const Build&>(ob);
		return build_revealed(b, who) || wall_is_seen(g, b, who);
	}
	const Unit& u = static_cast<const Unit&>(ob);
	int32_t fx, fy;
	if (is_gaia_animal(u)) {
		// AnimalData::is_seen @5d8160.
		if (g.info.reveal_map == 3) return true;
		if (!fog_cell_of(g, u, &fx, &fy)) return false;
		if (u.cara == -1) {
			// not a herd animal: a pen animal asks its farm, the others the fog cell
			if (who < 0 || u.animal_ox < 0) return was_seen(g, fx, fy, who);
			const Object* farm = g.object(u.animal_whom, u.animal_ox);
			return farm ? object_is_seen(g, *farm, who, false) : false;
		}
		// a herd animal is seen once its herd's good was discovered by `who`, else like the good (fish / whales: the cell
		// really seen; Spanish / Nubians: seen before; the rest: really seen)
		if (size_t(u.cara) < g.herds.size() && g.herds[size_t(u.cara)].good_o >= 0 && size_t(g.herds[size_t(u.cara)].good_o) < g.goods.size()) {
			const GoodObj& gd = g.goods[size_t(g.herds[size_t(u.cara)].good_o)];
			if (who >= 0 && who < 32 && (gd.ever_seen & (1u << uint32_t(who)))) return true;
			if (gd.type == fogtype::kFish || gd.type == fogtype::kWhales || who < 0) return was_really_seen(g, fx, fy, who);
			if (has_tribe_bonus(g, who, 9) || has_tribe_bonus(g, who, 4)) return was_seen(g, fx, fy, who);
		}
		return was_really_seen(g, fx, fy, who);
	}
	if (!ignore_cloak && unit_is_cloaked(u) && !(u.unit_masks & umask::kBlowCover) && !unit_is_detected(g, u, who))
		return false;
	if (who == u.who || g.info.reveal_map == 3) return true;
	if (!fog_cell_of(g, u, &fx, &fy)) return false;
	if (is_seen(g, fx, fy, who)) return true;
	return ((1u << (uint32_t(who) & 31)) & uint32_t(int32_t(u.visible))) != 0;
}

bool object_is_now_seen(const Game& g, const Object& ob, int32_t who) {
	if (ob.is_unit) return object_is_seen(g, ob, who, false);
	const Build& b = static_cast<const Build&>(ob);
	return build_revealed(b, who) || wall_is_now_seen(g, b, who);
}

bool good_is_seen(const Game& g, const GoodObj& gd, int32_t who) {
	if (who >= 0 && who < kNumLeaders && (gd.ever_seen & g.leaders[size_t(who)].ally_mask)) return true;
	const int32_t fx = fcoord_of(gd.x), fy = fcoord_of(gd.y);
	if (!in_f(g.world, fx, fy)) return false;
	if (gd.type != fogtype::kFish && gd.type != fogtype::kWhales && who >= 0) {
		if (gd.type == fogtype::kOil || has_tribe_bonus(g, who, 9) || has_tribe_bonus(g, who, 4))
			return was_seen(g, fx, fy, who);
	}
	return was_really_seen(g, fx, fy, who);
}

bool item_is_seen(const Game& g, const GoodObj& it, int32_t who) {
	if (who >= 0 && who < kNumLeaders && (it.ever_seen & g.leaders[size_t(who)].ally_mask)) return true;
	const int32_t fx = fcoord_of(it.x), fy = fcoord_of(it.y);
	if (!in_f(g.world, fx, fy)) return false;
	if (who >= 0 && has_tribe_bonus(g, who, 9)) {
		const int32_t wx = wcoord_of(it.x), wy = wcoord_of(it.y);
		if (g.world.in_w(wx, wy) && g.world.w(wx, wy).who == who) return was_seen(g, fx, fy, who);
	}
	return is_seen(g, fx, fy, who);
}

}  // namespace ron::sim
