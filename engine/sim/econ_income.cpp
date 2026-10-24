// The income side of the economy (docs/economy-spec.md sections 2, 3, 5.7, 5.8): starting stockpiles, the leader's
// economy reset, Leader::gather -> calc_gather -> calc_resource_caps -> do_gather, the per-building rates of
// BuildTypeData::calc_gather (flat / university / mine / radius paths), cities' contributions, leader-wide
// multipliers and the income of collected rares. No game_random draws except Game::init_starting_resources.
#include <algorithm>

#include "sim/economy.hpp"
#include "sim/production_internal.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using prod::is_t;
using prod::tb;

// build.cpp (gather tiles and deposits)
int32_t gather_radius(const Game& g, const Type& t);
int32_t mountains_find_nearest(const Game& g, int32_t x, int32_t y, int32_t region, int32_t* dist);
int32_t mountain_gather_size(const Game& g, const PlacedRange& m, int32_t who, const Build* list);
bool owner_ok(const Game& g, int32_t owner, int32_t who);
// econ_trade.cpp
int32_t leader_collect_units(Game& g, int32_t who, int32_t* out);

namespace {

Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }

// A constants array element (the K macro reads element 0).
int32_t KA(const Game& g, const char* name, int32_t i) {
	const int off = constant_offset(name);
	if (off < 0) return 0;
	return g.rules->constants.at(uint32_t(off + 4 * i));
}
int32_t pct(int32_t x, int32_t p) { return ((p + 100) * x) / 100; }
int32_t trunc256(int32_t x) { return x / 256; }
int32_t trunc16(int32_t x) { return x / 16; }

bool rare_bit(const std::array<uint8_t, 8>& m, int32_t i) { return i >= 0 && i < 64 && ((m[size_t(i >> 3)] >> (i & 7)) & 1); }
void set_rare_bit(std::array<uint8_t, 8>& m, int32_t i) {
	if (i >= 0 && i < 64) m[size_t(i >> 3)] = uint8_t(m[size_t(i >> 3)] | (1u << (i & 7)));
}

// tdist(dx, dy): the octagonal vector_dist of TCoord differences (vector_dist(TCoord, TCoord, TCoord, TCoord) @46d060).
int32_t tdist(int32_t ax, int32_t ay) { return vector_dist(iabs(ax), iabs(ay)); }

// WorldData::get_land(TCoord, 1) @6b4c70: the gather land of one tile (flat path).
int32_t tile_gather_land(const World& w, int32_t tx, int32_t ty) {
	const WData& d = w.w(tx >> 2, ty >> 2);
	const uint16_t f = d.flags;
	const uint16_t m = w.t(tx, ty);
	if (f & wflag::kCoast) return (m & tmask::kMedium) == tmask::kWater ? land::kOcean : land::kLand;
	if (f & wflag::kForest) return land::kForest;
	if ((m & tmask::kLowField) == tmask::kMountain) return land::kMountains;
	if (f & wflag::kOil) return land::kOil;
	if (f & wflag::kRock) return land::kRocks;
	return d.land;
}

// WorldData::has_gather_access(t, who, p4, 0) @6b4e50 (the GATHER_EDGE variant used by total_gather_access and the
// woodcutter / miner tile choice).
bool has_gather_access_who(const Game& g, int32_t tx, int32_t ty, int32_t who, int32_t p4) {
	const World& w = g.world;
	if (!w.in_t(tx, ty)) return false;
	if (!(w.t(tx, ty) & tmask::kGatherEdge)) return false;
	if (!(p4 != 0 || who < 0)) {
		const int32_t own = w.w(tx >> 2, ty >> 2).who;
		if (!(own < 0 || own == who || is_ally(g, who, own))) return false;
	}
	const bool check_owner = who >= 0 && p4 != 0;
	for (int k = 1; k <= 4; k++) {
		const int32_t x = tx + orthog_x[k], y = ty + orthog_y[k];
		if (!w.in_t(x, y)) continue;
		if (check_owner) {
			const int32_t own = w.w(x >> 2, y >> 2).who;
			if (own >= 0 && own != who && !is_ally(g, who, own)) continue;
		}
		const uint16_t n = w.t(x, y);
		if ((n & tmask::kMedium) != tmask::kWater && !(n & tmask::kBlocked)) return true;
	}
	return false;
}

}  // namespace

bool econ_has_gather_access(const Game& g, int32_t tx, int32_t ty, int32_t who, int32_t p4) {
	return has_gather_access_who(g, tx, ty, who, p4);
}

namespace {

// BuildTypeData::total_gather_access(list, cx, cy, who) @636500.
int32_t total_gather_access_who(const Game& g, const std::vector<std::array<int32_t, 2>>& list, int32_t cx, int32_t cy,
                                int32_t who) {
	int32_t n = 0;
	for (const auto& t : list) {
		if (!has_gather_access_who(g, t[0], t[1], who, 0)) continue;
		const int32_t ax = iabs(cx - t[0] * 0xc0 - 0x60), ay = iabs(cy - t[1] * 0xc0 - 0x60);
		if (ay + ax > 0x240 || ax > 0x23f || ay > 0x23f) n++;
	}
	return n;
}

// CityData::enhancer_amount(k, 100) @738360.
int32_t enhancer_amount(const City& c, int32_t k) {
	int32_t b = 0;
	switch (k) {
	case 0: b = c.granary; break;
	case 1: b = c.lumber_mill; break;
	case 4: b = c.smelter; break;
	case 5: b = c.refinery; break;
	default: b = 0; break;
	}
	return ((b + 100) * 100) / 100;
}

// The bonus multiply of every path: out and per through the 8.8 multiplier when it is not 1.0.
void apply_bonus(int32_t& out, int32_t& per, int32_t b) {
	if (b != 0x100) {
		out = trunc256(out * b);
		per = trunc256(per * b);
	}
}

}  // namespace

// ---------------------------------------------------------------- small getters

int32_t building_good(int32_t t) {
	switch (t) {
	case ptype::kFarm: return 0;
	case ptype::kWoodcutter: return 1;
	case ptype::kMine: return 4;
	case ptype::kUniversity: return 3;
	case ptype::kOilWell: case ptype::kOilPlatform: return 5;
	default: return -1;
	}
}
int32_t best_gather_type(int32_t good) {
	switch (good) {
	case 0: return ptype::kFarm;
	case 1: return ptype::kWoodcutter;
	case 3: return ptype::kUniversity;
	case 4: return ptype::kMine;
	case 5: return ptype::kOilWell;
	default: return -1;
	}
}
static int32_t level_of(const Game& g, int32_t who, int32_t first, int32_t n, int32_t base) {
	for (int32_t i = n - 1; i >= 0; i--)
		if (has_preq(g, who, first + i)) return base + i + 1;
	return base;
}
int32_t get_granary(const Game& g, int32_t who) { return level_of(g, who, etype::kGranary2, 4, 1); }
int32_t get_lumbermill(const Game& g, int32_t who) { return level_of(g, who, etype::kLumbermill2, 3, 1); }
int32_t get_smelter(const Game& g, int32_t who) { return level_of(g, who, etype::kSmelter2, 3, 1); }
int32_t get_university(const Game& g, int32_t who) { return level_of(g, who, etype::kUniversity2, 5, 1); }
int32_t get_taxation(const Game& g, int32_t who) { return level_of(g, who, etype::kTax1, 4, 0); }
int32_t get_merchants_level(const Game& g, int32_t who) { return level_of(g, who, etype::kMerchants1, 4, 0); }
int32_t get_fishermen(const Game& g, int32_t who) { return level_of(g, who, etype::kFishermen1, 3, 0); }

// ---------------------------------------------------------------- starting stockpiles (5.7)

void init_starting_resources(Game& g) {
	// Game::init_starting_resources @58a500, from Game::init_rules_and_teams (Game::run) before Leaders::init; the
	// draws precede Setup::build_game's reseed (game_random := GameInfo.seed), so they never reach the stream that
	// the simulation sees, but the values they pick stay.
	if (g.info.starting_resources == 12) g.info.starting_resources = uint8_t(g.rnd(12));  // S1
	const int32_t setting = g.info.starting_resources;
	const CategoryList* cl = g.rules->category_list("starting_resources");
	if (!cl) cl = g.rules->category_list("startingresources");
	for (int32_t k = 0; k < 6; k++) {
		int32_t base = KA(g, "starting_goods", k);
		if (setting == 7 && k != 3) base = KA(g, "starting_goods", 0);
		int32_t lo = 0, hi = 0;
		if (cl && setting >= 0 && size_t(setting) < cl->items.size()) {
			lo = cl->items[size_t(setting)].data[0];
			hi = cl->items[size_t(setting)].data[1];
		}
		if (lo == 0) {
			g.starting[size_t(k)] = base / 2;
		} else {
			const int32_t span = hi - lo;
			const int32_t r = (span != 0 && span * base - 1 > 0) ? g.rnd(span * base) : 0;  // S2
			g.starting[size_t(k)] = lo * base + r;
		}
		if (setting == 8) g.starting[size_t(k)] = 99999;
	}
}

void leader_init_economy(Game& g, int32_t who, bool with_tribe) {
	// Leader::init @6e3930: LeaderDataEncrypt::init and the economy arrays (every call), then for scenario leaders the
	// AI savings rate and the starting stockpile.
	Leader& l = LD(g, who);
	const int32_t cap0 = K("commerce_cap") << 4;
	for (int32_t k = 0; k < 6; k++) {
		l.bucket[size_t(k)] = 0;
		l.leftover[size_t(k)] = 0;
		l.escrow[size_t(k)] = 0;
		l.escrow_rate[size_t(k)] = 0;
		l.tributes[size_t(k)] = 0;
		l.resources[size_t(k)] = 0;
		l.support[size_t(k)] = 0;
		l.income[size_t(k)] = 0;
		l.resource_cap[size_t(k)] = cap0;
		l.over_cap[size_t(k)] = 0;
		l.bonus[size_t(k)] = 0;
		l.rate[size_t(k)] = 0;
		l.base_rate[size_t(k)] = 0;
		l.collected[size_t(k)] = 0;
		l.bonus_cap[size_t(k)] = 0;
	}
	l.resource_cap[6] = cap0;
	l.bonus_cap[6] = 0;
	l.gather_stamp = 0;
	l.rare.fill(0);
	l.rare_owned.fill(0);
	l.rare_conquest.fill(0);
	if (!with_tribe) return;
	// escrow_rate 40 when the leader already has more than two cities (city_num + village_num: active city buildings
	// at the time of the call; a scenario's objects are created later, so 0 there).
	int32_t cities = 0;
	for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
		const Build* b = g.build(who, o);
		if (b && b->alive() && (b->flags & objflag::kActive) && (b->flags & objflag::kCity)) cities++;
	}
	if (cities > 2) l.escrow_rate.fill(40);
	// TODO(AI, economy-spec 5.7): random_personality and the coin flip @6e4eb6 for leaders that are not human seats
	// (draws game_random during the scenario load; the AI is not specced yet).
	for (int32_t k = 0; k < 6; k++) {
		l.escrow[size_t(k)] = 0;
		if (!type_avail(g, who, k, 1)) continue;
		int32_t amt = g.starting[size_t(k)];
		if (g.info.game_rules == 8 && g.info.team[size_t(who & 7)] == 0) amt = (g.info.starting_resources2 + 1) * amt;
		else if (g.ctw && (l.flags2 & 0x80)) amt = amt * K("ctw_nomad_starting_res_x");
		bucket_add(g, who, k, amt);
		if (k == 3) {
			if (tb(g, who, 5) && l.age == 0 && K("greek_delay_knowledge") != 0) l.bucket[3] = 0;
		} else if (k == 0 && tb(g, who, 0x17)) {
			l.bucket[0] = pct(l.bucket[0], K("persians_bonus_food"));
		}
		if (g.info.starting_resources == 8) l.bucket[size_t(k)] = 99999;
	}
}

// ---------------------------------------------------------------- rares (5.8)

void calc_rare(const Game& g, int32_t who, int32_t r, int32_t* out, bool in_territory) {
	// LeaderData::calc_rare @6e08d0
	std::array<int32_t, 6> mult{100, 100, 100, 100, 100, 100};
	const int32_t M = KA(g, "merchants_bonus", get_merchants_level(g, who));
	for (int32_t k = 0; k < 6; k++) out[k] = 0;
	const Type& gt = g.type(r);
	for (int32_t i = 0; i < 2; i++) {
		const uint32_t t = uint32_t(gt.bonus_type[size_t(i)]);
		if (t >= 6) continue;
		out[t] += gt.bonus_num[size_t(i)] * 16;
		if (in_territory || ((r == etype::kFish || r == etype::kWhales) && t != 0)) mult[t] = M;
	}
	int32_t F = 0, B = 0;
	if (r == etype::kFish || r == etype::kWhales) {
		F = KA(g, "fishermen_bonus", get_fishermen(g, who));
		if (tb(g, who, 0xf)) B = K("japanese_fishing_boats");
	} else if (in_territory) {
		if (has_wonder(g, who, ptype::kPorcelain)) B = K("porcelain_rare");
		if (tb(g, who, 4)) B += K("nubian_rare");
	}
	if (F + B != 0 || mult[0] > 100) out[0] = ((F + B + mult[0]) * out[0]) / 100;
	for (int32_t k = 1; k < 6; k++)
		if (B != 0 || mult[size_t(k)] > 100) out[k] = ((mult[size_t(k)] + B) * out[k]) / 100;
}

// ---------------------------------------------------------------- leader-wide multipliers (3.8)

void calc_resource_bonuses(const Game& g, int32_t who, int32_t* out) {
	// LeaderData::calc_resource_bonuses @6db030
	if (tb(g, who, 0xd)) out[5] = pct(out[5], K("russian_oil"));
	if (has_wonder(g, who, ptype::kPyramids)) out[0] = pct(out[0], K("pyramids_food"));
	if (has_wonder(g, who, ptype::kColossus)) out[2] = pct(out[2], K("colossus_wealth"));
	if (has_wonder(g, who, ptype::kHangingGardens)) out[3] += K("hanging_gardens_knowledge") * 16;
	if (has_wonder(g, who, ptype::kAngkorWat)) out[4] = pct(out[4], K("angkor_metal"));
	if (has_wonder(g, who, ptype::kTajMahal)) out[2] = pct(out[2], K("taj_wealth"));
	if (has_wonder(g, who, ptype::kEiffel)) out[5] = pct(out[5], K("eiffel_oil"));
	if (has_wonder(g, who, ptype::kTikal)) out[1] = pct(out[1], K("tikal_timber"));
	if (has_preq(g, who, etype::kVirtualReality)) {
		for (int32_t k = 0; k < 6; k++)
			if (k != 3) out[k] = pct(out[k], K("global_prosperity"));
	}
	if (g.ctw) {
		const Leader& l = LD(g, who);
		for (int32_t k = 0; k < 6; k++) {
			const int32_t n = l.num_ctw_rate_bonuses[size_t(k)];
			if (n != 0) out[k] = int32_t((uint32_t(n) * uint32_t(K("ctw_prod_rate_bonus")) * uint32_t(out[k]))) / 100 + out[k];
		}
	}
}

// ---------------------------------------------------------------- BuildTypeData::calc_gather formulas (3.4)

int32_t mine_slots(const Game& g, int32_t size, int32_t total, int32_t claimable) {
	int32_t c = size < K("mtn_tiny_size")     ? K("mtn_tiny_gather")
	            : size < K("mtn_small_size")  ? K("mtn_small_gather")
	            : size < K("mtn_medium_size") ? K("mtn_medium_gather")
	            : size < K("mtn_large_size")  ? K("mtn_large_gather")
	                                          : K("mtn_huge_gather");
	if (total != 0) c = (c * claimable) / total;
	if (claimable != 0 && c < 1) c = 1;
	return c;
}

// (A) flat: FARM, OILWELL, OILPLATFORM. in.raw = the land makes under the footprint (rivers doubled).
GatherCalc gather_flat(const Game& g, int32_t type, const GatherFormulaIn& in) {
	GatherCalc r;
	if (!in.active && !in.assume) {
		r.max = 1;
		r.per = 0;
		return r;
	}
	r.max = 1;  // max_flat_gatherers
	r.per = 0;
	const int32_t good = building_good(type);
	const int32_t enh = in.enh + 100;
	for (int32_t k = 0; k < 6; k++) {
		if (!in.active || k != good) continue;
		const int32_t rate = k == 5 ? K("oil_rate") : K("peasant_rate");
		r.per = std::max(0, trunc256(in.raw[size_t(k)] * rate));
		if (enh != 100) r.per = (r.per * enh) / 100;
		if (k == 0 && in.tribe == 15) r.per = pct(r.per, K("japanese_fishing_boats"));
		r.out[size_t(k)] += r.per * in.gatherers;
		apply_bonus(r.out[size_t(k)], r.per, in.bonus256[size_t(k)]);
	}
	if (type_is(g.type(type), ptype::kFarm) && in.tribe == 7 && K("egyptian_farm_wealth") > 0 && in.gatherers != 0)
		r.out[2] += K("egyptian_farm_wealth") * 16;
	return r;
}

// (B) university: 7 slots, the scholar rate of the university level.
GatherCalc gather_university(const Game& g, const GatherFormulaIn& in) {
	GatherCalc r;
	if (!in.active && !in.assume) {
		r.max = 1;
		return r;
	}
	r.max = 7;  // max_knowledge_gatherers
	const int32_t n = std::min(r.max, in.gatherers);
	r.per = std::max(0, trunc256(KA(g, "scholar_rate", in.univ_level - 1) * 16));
	// (the enhancer of knowledge is always 100)
	r.out[3] += r.per * n;
	apply_bonus(r.out[3], r.per, in.bonus256[3]);
	return r;
}

// (C) mine: in.slots = the deposit's gather_size (-1 when claimed elsewhere, 0 none).
GatherCalc gather_mine(const Game& g, const GatherFormulaIn& in) {
	GatherCalc r;
	if (!in.active && !in.assume) {
		r.max = 1;
		return r;
	}
	r.max = in.slots;
	if (r.max != 0) {
		if (in.tribe == 12) r.max += K("german_miners");
		if (in.taj) r.max += K("taj_farms");
		if (in.kremlin) r.max += K("kremlin_farms");
	}
	r.per = trunc256(K("peasant_rate")) << 4;
	const int32_t enh = in.enh + 100;
	if (enh != 100) r.per = (r.per * enh) / 100;
	const int32_t n = std::min(std::max(r.max, 0), in.gatherers);
	if (in.tribe == 2 && K("inca_wealth_per_miner") > 0) r.out[2] += n * K("inca_wealth_per_miner") * 16;
	r.out[4] += r.per * n;
	apply_bonus(r.out[4], r.per, in.bonus256[4]);
	return r;
}

// (D) radius: in.raw = the W-cell scans (16 x num per counted W-cell), in.iroquois_tiles added to food for the
// Iroquois, in.rivers, in.access = total_gather_access (not doubled), in.blocked.
GatherCalc gather_radius_path(const Game& g, int32_t type, const GatherFormulaIn& in) {
	GatherCalc r;
	if (!in.active && !in.assume) {
		r.max = 1;
		return r;
	}
	const int32_t good = building_good(type);
	std::array<int32_t, 6> raw = in.raw;
	if (in.tribe == 18) raw[0] += in.iroquois_tiles;
	int32_t gatherers = in.gatherers;
	r.max = 1;
	r.per = 0;
	for (int32_t k = 0; k < 6; k++) {
		if (!in.active) continue;
		if (k != good && !(K("iroquois_food") != 0 && k == 0 && in.tribe == 18)) continue;
		raw[size_t(k)] = trunc16(raw[size_t(k)] * in.rivers) + raw[size_t(k)];
		r.max = trunc16(raw[size_t(k)] + 8);
		if (r.max == 0 && in.blocked) {
			r.max = -1;
			break;
		}
		if (r.max != 0 && k == 1) {
			if (in.tribe == 10) r.max += K("french_woodies");
			if (in.taj) r.max += K("taj_farms");
			if (in.kremlin) r.max += K("kremlin_farms");
		}
		if (k == 0 && in.tribe == 18 && good == 1) r.per = r.per + K("iroquois_food") * 16;
		else r.per = trunc256(K("peasant_rate")) << 4;
		const int32_t enh = k == good ? in.enh + 100 : 100;
		if (enh != 100) r.per = (r.per * enh) / 100;
		if (k == 5) {
			r.per = r.per * r.max;
			if (r.max > 1) r.max = 1;
		}
		int32_t n = std::min(r.max, gatherers);
		const int32_t acc = in.access * 2;
		n = std::min(n, acc);
		gatherers = n;
		r.max = std::min(r.max, acc);
		int32_t v = n * r.per;
		apply_bonus(v, r.per, in.bonus256[size_t(k)]);
		r.out[size_t(k)] += v;
	}
	return r;
}

// ---------------------------------------------------------------- BuildTypeData::calc_gather over the world

GatherCalc build_type_calc_gather(Game& g, int32_t type, bool assume, int32_t city, int32_t who, bool apply_bonuses,
                                  int32_t tx, int32_t ty, bool active, int32_t gatherers, const Build* list) {
	const Type& bt = g.type(type);
	const World& w = g.world;
	GatherFormulaIn in;
	in.active = active;
	in.assume = assume;
	in.gatherers = gatherers;
	const int32_t good = building_good(type);
	if (city >= 0 && who >= 0 && who < 8 && size_t(city) < g.cities[size_t(who)].size())
		in.enh = enhancer_amount(g.cities[size_t(who)][size_t(city)], good < 0 ? 2 : good) - 100;
	if (who >= 0 && who < kNumLeaders && LD(g, who).tribe >= 0 && size_t(LD(g, who).tribe) < g.rules->tribes.size()) {
		// has_tribe_bonus answers by the tribe's TribeIndex; the formulas compare in.tribe with it.
		for (int32_t n : {2, 7, 10, 12, 15, 18})
			if (tb(g, who, n)) in.tribe = n;
	}
	if (who >= 0) {
		in.taj = has_wonder(g, who, ptype::kTajMahal);
		in.kremlin = has_wonder(g, who, ptype::kKremlin);
	}
	if (who >= 0 && apply_bonuses) calc_resource_bonuses(g, who, in.bonus256.data());
	if (!active && !assume) {
		GatherCalc r;
		r.max = 1;
		return r;
	}
	if (bt.build_flags & 0x10000000) {
		// (A) flat: every footprint tile (valid, owner none / own / allied): the land's makes, doubled on rivers.
		for (int32_t x = tx; x < tx + bt.x_size; x++) {
			for (int32_t y = ty; y < ty + bt.y_size; y++) {
				if (!w.in_t(x, y)) continue;
				const int32_t own = w.w(x >> 2, y >> 2).who;
				if (own >= 0 && own != who && !is_ally(g, who, own)) continue;
				const int32_t ld = tile_gather_land(w, x, y);
				const bool river = w.is_river(x, y);
				if (ld < 0 || size_t(ld) >= g.rules->lands.size()) continue;
				const auto& L = g.rules->lands[size_t(ld)];
				for (size_t m = 0; m < 4 && m < L.size(); m++) {
					if (L[m].good < 0) continue;
					in.raw[size_t(L[m].good)] += river ? L[m].num * K("river_resource_value") : L[m].num;
				}
			}
		}
		return gather_flat(g, type, in);
	}
	if (type_is(bt, ptype::kUniversity)) {
		in.univ_level = who >= 0 ? get_university(g, who) : 1;
		return gather_university(g, in);
	}
	const int32_t cx = (bt.x_size + tx * 2) * 0x60, cy = (bt.y_size + ty * 2) * 0x60;  // corner_tile: the centre
	if (type_is(bt, ptype::kMine)) {
		// (C) the deposit: the stored one, else the nearest range / cliff of the centre's W-cell region.
		const int32_t R = gather_radius(g, bt) * 0xc0;
		int32_t mtn, cliff, dm = 0, dc = 0;
		if (!list || (list->mtn < 0 && list->cliff < 0)) {
			const int32_t wx = wcoord_of(cx), wy = wcoord_of(cy);
			const int32_t region = w.in_w(wx, wy) ? w.w(wx, wy).region : -1;
			mtn = mountains_find_nearest(g, cx, cy, region, &dm);
			cliff = -1;  // CliffsData::find_nearest: the stock maps have no cliffs (world-spec 3.4). TODO(cliffs).
			dc = 0;
		} else {
			mtn = list->mtn;
			cliff = list->cliff;
		}
		if (cliff >= 0 && mtn >= 0 && dc < dm) mtn = -1;
		in.slots = 0;
		if (mtn < 0) {
			// TODO(cliffs): CliffsData::gather_size and the claimed-tile test over the cliff's tiles.
		} else if (R < dm) {
			mtn = -1;
		} else {
			const PlacedRange& m = g.mountains[size_t(mtn)];
			bool claimed = false;
			for (const auto& t : m.tiles) {
				const int32_t x = t[0] + m.wx * 4, y = t[1] + m.wy * 4;
				if (!w.in_t(x, y)) continue;
				if (w.is_tree_at(x, y) || !w.is_mountain_at(x, y)) continue;
				const int32_t own = w.w(x >> 2, y >> 2).who;
				if (own >= 0 && own != who && !is_ally(g, who, own)) continue;
				if (!list && w.is_gathered_from(x, y)) {
					claimed = true;
					break;
				}
			}
			in.slots = claimed ? -1 : mountain_gather_size(g, m, who, list);
		}
		return gather_mine(g, in);
	}
	// (D) radius (woodcutters): W-cells of the circle around the centre's W-cell, within gather_radius tiles of the
	// centre tile (W-cell centre tiles, WCoord -> TCoord = 4w + 2).
	const int32_t R = gather_radius(g, bt);
	const int32_t wx0 = wcoord_of(cx), wy0 = wcoord_of(cy);
	const int32_t tcx = tcoord_of(cx), tcy = tcoord_of(cy);
	std::vector<std::array<int32_t, 2>> scratch;  // the static MiningList `local` of previews
	const Circles& C = circles();
	const int32_t rw = std::clamp((R + 3) / 4, 0, 64);
	for (int32_t i = 0; i < C.radius[size_t(rw)]; i++) {
		const int32_t wx = C.x[size_t(i)] + wx0, wy = C.y[size_t(i)] + wy0;
		if (!w.in_w(wx, wy)) continue;
		if (tdist(tcx - (4 * wx + 2), tcy - (4 * wy + 2)) > R) continue;
		const int32_t own = w.w(wx, wy).who;
		if (own >= 0 && own != who && !is_ally(g, who, own)) continue;
		const int32_t ctx = 4 * wx + 2, cty = 4 * wy + 2;
		bool scan = false;
		if (list) {
			for (const auto& t : list->gather_from)
				if (t[0] == ctx && t[1] == cty) {
					scan = true;
					break;
				}
		} else if (!w.is_gathered_from(ctx, cty)) {
			if (type_is(bt, ptype::kWoodcutter)) {
				for (int32_t j = 0; j < 16; j++) {
					const int32_t x = 4 * wx + (j & 3), y = 4 * wy + (j >> 2);  // WCoord::traverse_x / traverse_y
					if (w.is_tree_at(x, y)) scratch.push_back({x, y});
				}
				scan = w.is_tree_at(ctx, cty);
			} else {
				if (w.w(wx, wy).flags & wflag::kForest) continue;
				for (int32_t j = 0; j < 16; j++) {
					const int32_t x = 4 * wx + (j & 3), y = 4 * wy + (j >> 2);
					if (w.is_mountain_at(x, y)) scratch.push_back({x, y});
				}
				scan = w.is_mountain_at(ctx, cty);
			}
		} else {
			in.blocked = true;
		}
		if (!scan) continue;
		const int32_t ld = w.gather_land(wx, wy);
		if (ld < 0 || size_t(ld) >= g.rules->lands.size()) continue;
		const auto& L = g.rules->lands[size_t(ld)];
		int32_t counted = 0;
		for (size_t m = 0; m < 4 && m < L.size(); m++) {
			const int32_t gd = L[m].good;
			if (gd < 0 || gd != good || L[m].num == 0) continue;
			in.raw[size_t(gd)] += 16 * L[m].num;
			counted += 16;
		}
		if (who >= 0 && tb(g, who, 0x12)) in.raw[0] += counted;  // (the Iroquois term is applied here, not in in.tribe)
	}
	in.iroquois_tiles = 0;
	for (int32_t x = tx; x < tx + bt.x_size; x++)
		for (int32_t y = ty; y < ty + bt.y_size; y++)
			if (w.in_t(x, y) && w.is_river(x, y)) in.rivers++;
	in.access = total_gather_access_who(g, list ? list->gather_from : scratch, cx, cy, who);
	return gather_radius_path(g, type, in);
}

GatherCalc build_calc_gather(Game& g, Build& b, bool assume, bool assume_active, bool apply_bonuses) {
	// BuildData::calc_gather(out, &max, &per, p4, p5, p6) @62d360
	const Footprint f = footprint(*b.ptype, b.x, b.y);
	const bool active = assume_active ? true : (b.flags & objflag::kActive) != 0;
	// (scholars inside a university are counted by num_gatherers; the `inside` argument is unused by the formula)
	const int32_t n = build_num_gatherers(g, b, true, true);
	GatherCalc r = build_type_calc_gather(g, b.type, assume, b.city, b.who, apply_bonuses, f.x0, f.y0, active, n, &b);
	if (r.max < 0) r.max = 0;
	return r;
}

// BuildTypeData::max_gatherers @63c430 (production's entry point, sim/production.hpp).
int32_t max_gatherers(Game& g, int32_t type, int32_t o, int32_t who, int32_t tx, int32_t ty) {
	const Type& bt = g.type(type);
	if (bt.build_flags & 0x10000000) return 1;
	if (o < 0 || who < 0) {
		GatherCalc r = build_type_calc_gather(g, type, false, -1, who, false, tx, ty, true, 1, nullptr);
		return std::max(r.max, 0);
	}
	const Build* b = g.build(who, o);
	int32_t city = -1;
	if (b && b->alive() && (b->flags & objflag::kActive)) city = b->city;
	GatherCalc r = build_type_calc_gather(g, type, false, city, who, false, tx, ty, true, 1, b);
	return std::max(r.max, 0);
}

// ---------------------------------------------------------------- cities (3.3)

void city_income_tail(const Game& g, const CityTailIn& in, int32_t* out) {
	// The tail of LeaderData::calc_city_resources @6d5530 after the buildings: Forbidden City and CEO multipliers,
	// the base gather, Roman / German extras, CityData::get_taxes @737b50 and get_literacy @737c00.
	if (in.forbidden)
		for (int32_t k = 0; k < 6; k++) out[k] = pct(out[k], K("forbidden_city_gather"));
	if (in.ceo)
		for (int32_t k = 0; k < 6; k++) out[k] = pct(out[k], K("theceo_production_bonus"));
	for (int32_t k = 0; k < 6; k++) {
		const int32_t base = KA(g, "city_gather", k);
		if (base == 0) continue;
		const int32_t fb = K("forbidden_city_base_gather");
		out[k] += 16 * ((in.forbidden_is && fb != 0) ? fb : base);
	}
	if (K("roman_city_gather") != 0 && in.roman) out[2] += K("roman_city_gather") * 16;
	if (K("german_city_gather") != 0 && in.german) {
		out[0] += K("german_city_gather") * 16;
		out[1] += K("german_city_gather") * 16;
		if (in.metal_avail) out[4] += K("german_city_gather") * 16;
	}
	int32_t taxes = K("village_taxes") + in.num_buildings * K("building_taxes");
	if (in.market) {
		int32_t m = K("market_taxes");
		if (in.porcelain) m = pct(m, K("porcelain_market"));
		taxes += m;
	}
	if (in.temple) taxes += K("temple_taxes");
	out[2] += taxes * 16;
	int32_t lit = K("village_literacy");
	if (in.university) lit += K("university_literacy");
	if (in.library) lit += K("library_literacy");
	out[3] += lit * 16;
}

void leader_income_tail(const Game& g, const LeaderTailIn& in, int32_t* out) {
	// The tail of Leader::calc_gather @6ceee0 after the unit loop (refineries, capitalism, the Inca wealth rule, the
	// conquest rares' sum, coffee, territory taxes and the Mongol food), before calc_resource_bonuses.
	out[5] = ((in.refineries * K("refinery_bonus") + 100) * out[5]) / 100;
	if (in.capitalism) out[5] += K("capitalism_oil_prod") * 16;
	if (K("inca_wealth_per_miner") < 0 && in.inca) out[2] += out[4];
	for (int32_t k = 0; k < 6; k++) out[k] += in.conquest[size_t(k)];
	if (in.coffee)
		for (int32_t k = 0; k < 6; k++) out[k] = pct(out[k], K("coffee_income_bonus"));
	if (in.total_land != 0) {
		int32_t tax = KA(g, "territory_taxes", in.taxation);
		if (in.british) tax = pct(tax, K("british_taxation"));
		if (in.ctw_missionaries) tax = pct(tax, K("ctw_missionaries_bonus"));
		out[2] += (in.territory * tax * 16) / in.total_land;
		if (in.mongol && K("mongol_nomadic_food") != 0)
			out[0] += ((in.num_nations * in.territory * 800) / in.total_land) / K("mongol_nomadic_food");
	}
}


namespace {

// LeaderData::calc_city_resources(out, x, y, city) @6d5530 for a city index (the city < 0 form has no callers on the
// income path).
void calc_city_resources(Game& g, int32_t who, int32_t c, int32_t* out) {
	for (int32_t k = 0; k < 6; k++) out[k] = 0;
	const City& ct = g.cities[size_t(who)][size_t(c)];
	out[2] += ct.trade_val;
	for (int32_t o = ct.o, guard = 0; o >= 0 && guard < 1000; guard++) {
		Build* b = g.build(who, o);
		if (!b) break;
		const int32_t next = b->city_down;
		if (b->alive() && is_gather_type(*b->ptype) && !type_is(*b->ptype, ptype::kOilWell) &&
		    !type_is(*b->ptype, ptype::kOilPlatform) && b->city == c) {
			const GatherCalc r = build_calc_gather(g, *b, true, false, false);
			for (int32_t k = 0; k < 6; k++) out[k] += r.out[size_t(k)];
		}
		o = next;
	}
	const Build* centre = g.build(who, ct.o);
	const bool forbidden = centre && centre->type == ptype::kForbiddenCity;
	CityTailIn in;
	in.forbidden = forbidden;
	in.forbidden_is = centre && type_is(*centre->ptype, ptype::kForbiddenCity);
	const Leader& l = LD(g, who);
	if (l.num_units[size_t(etype::kTheCeo - kUnits)] != 0) {
		// The first live hero (Heroes list, slot order) that is a map unit and is(THECEO): its city decides.
		for (int32_t i = 0; i < l.num_heroes && size_t(i) < l.heroes.size(); i++) {
			if (!(l.heroes[size_t(i)].hero_flags & 1)) continue;
			const Unit* u = g.unit(who, l.heroes[size_t(i)].o);
			if (!u || !u->alive() || !u->on_map() || !(u->ptype->unit_flags2 & 0x20)) continue;
			if (!type_is(*u->ptype, etype::kTheCeo, true)) continue;
			in.ceo = find_city_at(g, u->x, u->y, who, -1, 0) == c;
			break;
		}
	}
	in.roman = tb(g, who, 6);
	in.german = tb(g, who, 0xc);
	in.metal_avail = type_avail(g, who, 4, 1) != 0;
	// CityData::num_buildings @738190: live active buildings of the chain (the centre included).
	for (int32_t o = ct.o, guard = 0; o >= 0 && guard < 1000; guard++) {
		const Build* b = g.build(who, o);
		if (!b) break;
		o = b->city_down;
		if (b->alive() && (b->flags & objflag::kActive)) in.num_buildings++;
	}
	in.market = city_count_buildings(g, who, c, ptype::kMarket, 0, 1) != 0;
	in.porcelain = has_wonder(g, who, ptype::kPorcelain);
	in.temple = city_count_buildings(g, who, c, ptype::kTemple, 0, 1) != 0;
	in.university = city_count_buildings(g, who, c, ptype::kUniversity, 0, 1) != 0;
	in.library = city_count_buildings(g, who, c, ptype::kLibrary, 0, 1) != 0;
	city_income_tail(g, in, out);
}

}  // namespace

void city_calc_gather(Game& g, int32_t who, int32_t c, int32_t* out) {
	// City::calc_gather @737c60: the enhancer bytes first (the decompile shows the tables shifted by one array).
	City& ct = g.cities[size_t(who)][size_t(c)];
	ct.granary = (ct.city_flags & cityflag::kHasGranary) ? uint8_t(KA(g, "granary_bonus", get_granary(g, who) - 1)) : 0;
	ct.lumber_mill = (ct.city_flags & cityflag::kHasLumber) ? uint8_t(KA(g, "lumbermill_bonus", get_lumbermill(g, who) - 1)) : 0;
	ct.smelter = city_count_buildings(g, who, c, ptype::kSmelter, 0, 1) != 0
	                 ? uint8_t(KA(g, "smelter_bonus", get_smelter(g, who) - 1))
	                 : 0;
	ct.refinery = 0;  // (the refinery count is evaluated and discarded)
	calc_city_resources(g, who, c, out);
}

// ---------------------------------------------------------------- the income pipeline (2, 3)

void leader_calc_gather(Game& g, int32_t who, int32_t* out) {
	// Leader::calc_gather @6ceee0
	Leader& l = LD(g, who);
	const int32_t frame = g.frame;
	if (!(l.flags & leaderflag::kRecompute)) {
		if (frame < l.gather_stamp + 300) return;
		if ((frame + who * 8) % 256 != 0) return;
	} else if (frame != 0 && (who + frame) % 8 != 0) {
		return;
	}
	l.rare_owned.fill(0);
	l.known_rares = 0;
	for (int32_t r = 0; r < 64; r++) l.known_rares += l.reg_known_rares[size_t(r)];
	l.rares_collected.fill(0);
	if (has_wonder(g, who, ptype::kPorcelain)) {
		for (size_t i = 0; i < g.goods.size(); i++) {
			const GoodObj& gd = g.goods[i];
			if (!gd.alive() || !good_is_seen(g, gd, who) || !type_avail(g, who, gd.type, 1)) continue;
			const int32_t wx = wcoord_of(gd.x), wy = wcoord_of(gd.y);
			if (!g.world.in_w(wx, wy) || g.world.w(wx, wy).who != who) continue;
			set_rare_bit(l.rare_owned, gd.type - 6);
		}
	}
	for (int32_t k = 0; k < 6; k++) {
		out[k] = KA(g, "basic_gather", k) << 4;
		l.bonus[size_t(k)] = 0;
	}
	if (tb(g, who, 0x13)) {
		// TODO(garrisons): the garrisoned scout / stable / peasant counters (+0xa28..+0xa30) are 0.
		out[0] += (l.stable_units + l.scouts + l.peasants) * K("lakota_food") * 16;
	}
	if (tb(g, who, 0x14)) {
		const int32_t a = l.barracks_units * K("americans_barracks_gather") * 16;  // (- barracks garrison: TODO)
		if (type_avail(g, who, 0, 1)) out[0] += a;
		if (type_avail(g, who, 1, 1)) out[1] += a;
		if (type_avail(g, who, 4, 1)) out[4] += a;
		if (type_avail(g, who, 2, 1)) out[2] += a;
	}
	if (!g.scenario_mode || g.buildings_gather) {
		int32_t tmp[6];
		for (int32_t c = 0; c < l.city_mark && size_t(c) < g.cities[size_t(who)].size(); c++) {
			if (!(g.cities[size_t(who)][size_t(c)].city_flags & cityflag::kValid)) continue;
			city_calc_gather(g, who, c, tmp);
			for (int32_t k = 0; k < 6; k++) out[k] += tmp[k];
		}
		for (size_t s = 0; s < l.oil_wells.size(); s++) {
			const int32_t o = l.oil_wells[s];
			if (o < 0) continue;
			Build* b = g.build(who, o);
			if (!b) continue;
			const GatherCalc r = build_calc_gather(g, *b, true, false, false);
			for (int32_t k = 0; k < 6; k++) out[k] += r.out[size_t(k)];
		}
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			Build* b = g.build(who, o);
			if (!b || !b->alive() || b->city >= 0) continue;
			if (b->build_masks & bmask::kNeutralized) continue;  // vslot +0x180 is_neutralized
			if (b->type != ptype::kWoodcutter && b->type != ptype::kMine) continue;
			const GatherCalc r = build_calc_gather(g, *b, true, false, false);
			for (int32_t k = 0; k < 6; k++) out[k] += r.out[size_t(k)];
		}
		int32_t u6[6] = {0, 0, 0, 0, 0, 0};
		leader_collect_units(g, who, u6);  // idle fishermen / merchants (Unit::do_gather(out, ...), economy-spec 3.7)
		for (int32_t k = 0; k < 6; k++) out[k] += u6[k];
	}
	LeaderTailIn ti;
	ti.refineries = get_buildings(g, who, ptype::kRefinery, l.num_buildings[size_t(ptype::kRefinery - kBuilds)]);
	ti.capitalism = has_preq(g, who, etype::kCapitalism1);
	ti.inca = tb(g, who, 2);
	// Conquest rares (CTW only): rare_conquest is empty outside Conquer the World, so the loop never runs.
	bool any_conquest = false;
	for (uint8_t b : l.rare_conquest) any_conquest |= b != 0;
	if (any_conquest) {
		bool wellington = false;
		for (int32_t p = 0; p < 8; p++)
			if (p != who && (g.leaders[size_t(p)].flags & leaderflag::kValid) &&
			    g.leaders[size_t(p)].num_units[size_t(etype::kWellington - kUnits)] != 0)
				wellington = true;
		if (!wellington) {
			for (int32_t i = 0; i < 44; i++) {
				if (!rare_bit(l.rare_conquest, i)) continue;
				int32_t tmp[6];
				l.rares_collected[size_t(i)]++;
				calc_rare(g, who, i + 6, tmp, false);
				for (int32_t k = 0; k < 6; k++) ti.conquest[size_t(k)] += tmp[k];
			}
		}
	}
	// (the rare-change bookkeeping between the conquest rares and coffee does not touch out)
	std::array<uint8_t, 8> nw{};
	for (size_t i = 0; i < 8; i++) nw[i] = uint8_t(l.rare_owned[i] | l.rare_conquest[i]);
	if (nw != l.rare) {
		if (((l.rare[2] >> 7) & 1) != ((nw[2] >> 7) & 1)) regions_fix_all_borders(g);  // GEMS (bit 23)
		l.rare = nw;
		l.flags |= leaderflag::kCalcUnitStats | leaderflag::kCalcWallStats;
		calc_pop_cap(g, who);
	}
	ti.coffee = has_rare(g, who, etype::kCoffee);
	ti.total_land = g.world_total_land;
	if (ti.total_land != 0) {
		ti.taxation = get_taxation(g, who);
		ti.british = tb(g, who, 0xb);
		ti.ctw_missionaries = g.ctw && l.num_bonus_cards[32] != 0;
		ti.territory = l.territory;
		ti.mongol = tb(g, who, 0x11);
		ti.num_nations = g.info.num_nations;
	}
	leader_income_tail(g, ti, out);
	calc_resource_bonuses(g, who, out);
	l.flags &= ~leaderflag::kRecompute;
	l.gather_stamp = g.frame;
	for (int32_t k = 0; k < 6; k++) {
		if (type_avail(g, who, k, 1) || !has_preq(g, who, k)) continue;
		const Type& gt = g.type(k);
		const int32_t to = gt.obsolete_production_good;
		if (to >= 0 && to < 6) out[to] += trunc256(out[k] * gt.obsolete_production_rate);
		out[k] = 0;
	}
}

void leader_calc_resource_caps(Game& g, int32_t who) {
	// Leader::calc_resource_caps @6ce900
	Leader& l = LD(g, who);
	const int32_t commerce = std::clamp(l.epoch[2], 0, 7);
	for (int32_t k = 0; k < 7; k++) {
		int32_t cap;
		if (k == 3) {
			cap = 999;
		} else {
			cap = KA(g, "commerce_cap", commerce);
			if (tb(g, who, 0xb)) cap = pct(cap, K("british_commerce"));
			if (k == 2) {
				if (K("inca_wealth_cap") != 0 && tb(g, who, 2)) cap = pct(cap, K("inca_wealth_cap"));
			} else if (k == 1) {
				if (K("french_timber_commerce") != 0 && tb(g, who, 10)) cap = pct(cap, K("french_timber_commerce"));
			} else if (k == 0 && K("egyptian_food_commerce") != 0 && tb(g, who, 7)) {
				cap = pct(cap, K("egyptian_food_commerce"));
			}
			if (((l.rare[2] >> 6) & 1) || ((l.rare_conquest[2] >> 6) & 1)) cap = pct(cap, K("diamonds_commerce"));
			if ((k == 0 || k == 2) && has_wonder(g, who, ptype::kPyramids)) cap += K("pyramids_commerce");
			bool other = true;
			if (k == 1 || k == 2) {
				if (has_wonder(g, who, ptype::kColossus)) cap += K("colossus_commerce");
				if (k == 2) {
					if (has_wonder(g, who, ptype::kTajMahal)) cap += K("taj_wealth_commerce");
					other = false;
				}
			}
			if (other) {
				if (k == 5) {
					if (has_wonder(g, who, ptype::kEiffel)) cap += K("eiffel_oil_commerce");
					if (has_wonder(g, who, ptype::kKremlin)) cap += K("kremlin_commerce");
				} else if (k < 6) {
					if (has_wonder(g, who, ptype::kKremlin)) cap += K("kremlin_commerce");
					if (k == 1) {
						if (has_wonder(g, who, ptype::kTikal)) cap += K("tikal_timber_commerce");
					} else if (k == 4 && has_wonder(g, who, ptype::kAngkorWat)) {
						cap += K("angkor_metal_commerce");
					}
				}
			}
			if (has_preq(g, who, etype::kRepublic1 + 2)) cap += K("republic_commerce_bonus3");
			else if (has_preq(g, who, etype::kRepublic1 + 1)) cap += K("republic_commerce_bonus2");
			else if (has_preq(g, who, etype::kRepublic1)) cap += K("republic_commerce_bonus");
			cap += l.bonus_cap[size_t(k)];
			cap = std::clamp(cap, 0, 999);
		}
		if (has_preq(g, who, etype::kVirtualReality)) cap = 999;
		l.resource_cap[size_t(k)] = cap << 4;
	}
}

int32_t get_gather_handicap(const Game& g, int32_t who) {
	// LeaderData::get_gather_handicap @6d66a0
	const Leader& l = LD(g, who);
	if (l.flags & leaderflag::kHuman) {
		if (!g.multiplayer) return 0;
		// LeaderData::get_handicap in multiplayer: the lobby handicap's category data[0].
		const CategoryList* cl = g.rules->category_list("handicaps");
		if (!cl || l.handicap < 0 || size_t(l.handicap) >= cl->items.size()) return 0;
		return cl->items[size_t(l.handicap)].data[0];
	}
	int32_t d;
	if (g.multiplayer) {
		d = l.difficulty;
	} else if (((g.scripted_rules) && !g.ctw) && l.difficulty >= 0) {
		d = l.difficulty;
	} else {
		d = g.info.difficulty;
	}
	switch (d) {
	case 0: return -35;
	case 1: return -15;
	case 2: return -7;
	case 4: return 25;
	case 5: return 50;
	default: return 0;
	}
}

int32_t gather_frame(const Game& g, GatherFrameState& st, int32_t k, int32_t r, int32_t cap, int32_t h,
                     int32_t dutch_base, bool dutch, int32_t escrow_rate, bool ai) {
	// The arithmetic of Leader::do_gather @6ce450 for one good with a non-negative rate r.
	if (r < 0) {
		st.income = r;
		st.over_cap = 0;
		return 0;
	}
	if (cap < r) {
		st.over_cap = (cap > 0x3e6f ? 1 : 0) + 1;
		r = cap;
	} else {
		st.over_cap = 0;
	}
	if (k != 3 && dutch) {
		const int32_t excess = st.bucket - dutch_base;
		int32_t r2 = r;
		if (excess > 0) {
			r2 = r + ((K("dutch_interest") * excess) / 100) * 16;
			const int32_t lim = K("dutch_interest_cap") * 16 + cap;
			if (lim < r2) r2 = lim;
		}
		r = r2 < 0x3e70 ? r2 : 0x3e70;
	}
	st.income = r;
	if (h != 0) r = ((h + 100) * r) / 100;
	const int32_t tc = g.info.tech_cost;
	if (k == 3 && tc > 1 && tc > 4) r = tc < 7 ? (r * 3) / 4 : r / 2;
	if ((g.info.flags & 2) || g.info.game_rules == 9) r = (r * 3) / 2;
	if (g.ai_speed > 1) r = r * g.ai_speed;
	const int32_t D = K("gather_rate") * 16;
	int32_t q = r / D;
	st.leftover += r % D;
	while (st.leftover >= D) {
		q++;
		st.leftover -= D;
	}
	st.bucket += q;
	if (ai) {
		const int32_t D2 = K("gather_rate") * 0x640;
		const int32_t num = escrow_rate * r;
		int32_t e = num / D2;
		const int32_t m = num % D2;
		if (m != 0) {
			const int32_t period = std::max(2, (D2 + m / 2) / m);
			if (g.frame % period == 0) e++;
		}
		st.escrow += e;
	}
	return q;
}

void leader_do_gather(Game& g, int32_t who) {
	// Leader::do_gather @6ce450
	Leader& l = LD(g, who);
	for (int32_t k = 0; k < 6; k++) {
		if (!type_avail(g, who, k, 1)) continue;
		if (g.info.starting_resources == 8) {
			l.bucket[size_t(k)] = 99999;
			l.escrow[size_t(k)] = 0;
			continue;
		}
		const int32_t r = l.resources[size_t(k)] - l.support[size_t(k)] + l.base_rate[size_t(k)];
		// (the "income capped" console message when over_cap was 0 is presentation)
		const bool dutch = k != 3 && tb(g, who, 0x16);
		int32_t base = 0;
		if (dutch) {
			if (g.info.game_rules == 8 && g.info.team[size_t(who & 7)] == 0) base = (g.info.starting_resources2 + 1) * g.starting[size_t(k)];
			else if (g.ctw && (l.flags2 & 0x80)) base = g.starting[size_t(k)] * K("ctw_nomad_starting_res_x");
			else base = g.starting[size_t(k)];
		}
		GatherFrameState st;
		st.bucket = l.bucket[size_t(k)];
		st.leftover = l.leftover[size_t(k)];
		st.over_cap = l.over_cap[size_t(k)];
		st.income = l.income[size_t(k)];
		st.escrow = l.escrow[size_t(k)];
		const bool ai = (l.flags & 0xc) != leaderflag::kHuman;
		const int32_t q = gather_frame(g, st, k, r, l.resource_cap[size_t(k)], r < 0 ? 0 : get_gather_handicap(g, who), base,
		                               dutch, l.escrow_rate[size_t(k)], ai);
		l.bucket[size_t(k)] = st.bucket;
		l.leftover[size_t(k)] = st.leftover;
		l.over_cap[size_t(k)] = st.over_cap;
		l.income[size_t(k)] = st.income;
		l.escrow[size_t(k)] = st.escrow;
		if (r >= 0) l.collected[size_t(k)] += q;
	}
}

void leader_gather(Game& g, int32_t who) {
	// Leader::gather @6ce280
	Leader& l = LD(g, who);
	int32_t tmp[6];
	for (int32_t k = 0; k < 6; k++) tmp[k] = l.resources[size_t(k)];
	leader_calc_gather(g, who, tmp);
	for (int32_t k = 0; k < 6; k++) l.resources[size_t(k)] = tmp[k];
	std::array<uint8_t, 8> nw{};
	for (size_t i = 0; i < 8; i++) nw[i] = uint8_t(l.rare_owned[i] | l.rare_conquest[i]);
	if (nw != l.rare) {
		l.rare = nw;
		l.flags |= leaderflag::kCalcUnitStats | leaderflag::kCalcWallStats;
	}
	l.support.fill(0);
	leader_calc_resource_caps(g, who);
	leader_do_gather(g, who);
}

// ---------------------------------------------------------------- views

LeaderEconView leader_econ_view(const Game& g, int32_t who) {
	LeaderEconView v;
	if (who < 0 || who >= 8) return v;
	const Leader& l = LD(g, who);
	for (int32_t k = 0; k < 6; k++) {
		v.rate[size_t(k)] = l.resources[size_t(k)];
		v.income[size_t(k)] = l.income[size_t(k)];
		v.cap[size_t(k)] = l.resource_cap[size_t(k)];
		v.over_cap[size_t(k)] = l.over_cap[size_t(k)];
		calc_market_prices(g, who, k, &v.buy[size_t(k)], &v.sell[size_t(k)]);
	}
	v.can_trade = can_buy_sell(g, who);
	return v;
}

}  // namespace ron::sim
