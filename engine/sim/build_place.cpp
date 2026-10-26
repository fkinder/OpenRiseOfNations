// Placement rules (docs/production-spec.md sections 3.5-3.8, from the disassembly): BuildTypeData::blocked_site /
// blocked_tcoord / blocked_location and their territory and adjacency helpers, the snap with the dock / mine /
// woodcutter search, ObjectsData::find_building_placed_at.
#include <algorithm>

#include "sim/compass.hpp"
#include "sim/fog.hpp"
#include "sim/production_internal.hpp"

namespace ron::sim {

using prod::is_t;
using prod::tb;

namespace {

const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }

bool mutual(const Game& g, int32_t a, int32_t b) {
	if (a < 0 || b < 0 || a >= kNumLeaders || b >= kNumLeaders) return false;
	return LD(g, a).diplos(b) == 2 && LD(g, b).diplos(a) == 2;
}
bool friendly(const Game& g, int32_t owner, int32_t who) { return owner == who || mutual(g, who, owner); }

uint32_t vdist(int32_t ax, int32_t ay) {  // vector_dist @46cff0 (unsigned)
	uint32_t hi = uint32_t(std::abs(ax)), lo = uint32_t(std::abs(ay));
	if (hi < lo) std::swap(hi, lo);
	if (hi == 0) return 0;
	if (lo >= 60000) return (lo + 2 * hi) >> 1;
	return lo * lo / (2 * hi) + hi;
}

int8_t owner_at(const Game& g, int32_t tx, int32_t ty) { return g.world.w(tx >> 2, ty >> 2).who; }

// non_friendly_territory @6389c0.
int32_t non_friendly_territory(const Game& g, const Footprint& f, int32_t who) {
	int32_t r = 0;
	for (int32_t x = f.x0; x < f.x0 + f.w; x++) {
		for (int32_t y = f.y0; y < f.y0 + f.h; y++) {
			if (!g.world.in_t(x, y) || g.world.is_tocean(x, y)) continue;
			const int32_t own = owner_at(g, x, y);
			if (friendly(g, own, who)) continue;
			if (own < -1) r = 2;
			else if (own == -1) r = std::max(r, 1);
			else if (LD(g, who).diplos(own) == 0 || LD(g, own).diplos(who) == 0) return 3;
			else r = 2;
		}
	}
	return r;
}

// in_enemy_territory @638b40.
bool in_enemy_territory(const Game& g, const Footprint& f, int32_t who) {
	for (int32_t x = f.x0; x < f.x0 + f.w; x++) {
		for (int32_t y = f.y0; y < f.y0 + f.h; y++) {
			if (!g.world.in_t(x, y)) continue;
			const int32_t own = owner_at(g, x, y);
			if (own < 0 || own == who || mutual(g, who, own)) continue;
			if (LD(g, who).city_num != 0) return true;
			const int32_t reg = g.world.w(x >> 2, y >> 2).region;
			if (reg < 64 && (LD(g, who).reg_cities[size_t(reg)] != 0 || LD(g, who).reg_forts[size_t(reg)] != 0)) return true;
			if (is_really_seen(g, x >> 1, y >> 1, own)) return true;
		}
	}
	return false;
}

int32_t count_water(const Game& g, const Footprint& f) {
	int32_t n = 0;
	for (int32_t x = f.x0; x < f.x0 + f.w; x++)
		for (int32_t y = f.y0; y < f.y0 + f.h; y++)
			if (g.world.in_t(x, y) && g.world.is_tocean(x, y)) n++;
	return n;
}

// check_land_adjacent @638ed0: the ring around the footprint without the corners.
int32_t check_land_adjacent(const Game& g, const Footprint& f, int32_t who) {
	bool land = false;
	for (int32_t x = f.x0 - 1; x <= f.x0 + f.w; x++) {
		for (int32_t y = f.y0 - 1; y <= f.y0 + f.h; y++) {
			const bool inside = x >= f.x0 && x < f.x0 + f.w && y >= f.y0 && y < f.y0 + f.h;
			const bool corner = (x == f.x0 - 1 || x == f.x0 + f.w) && (y == f.y0 - 1 || y == f.y0 + f.h);
			if (inside || corner || !g.world.in_t(x, y)) continue;
			if (g.world.is_tocean(x, y) || g.world.is_blocked_at(x, y)) continue;
			land = true;
			const int32_t own = owner_at(g, x, y);
			if (who < 0 || friendly(g, own, who) || (own == -1 && tb(g, who, 0x13))) return 0;
		}
	}
	return land ? 1 : 2;
}

// count_trees_adjacent @639090 / count_mountains_adjacent @639180.
int32_t count_adjacent(const Game& g, const Footprint& f, int32_t d, bool trees) {
	int32_t n = 0;
	for (int32_t x = f.x0 - d; x < f.x0 + f.w + d; x++) {
		for (int32_t y = f.y0 - d; y < f.y0 + f.h + d; y++) {
			const bool inside = x >= f.x0 && x < f.x0 + f.w && y >= f.y0 && y < f.y0 + f.h;
			const bool corner = (x == f.x0 - d || x == f.x0 + f.w + d - 1) && (y == f.y0 - d || y == f.y0 + f.h + d - 1);
			if (inside || corner || !g.world.in_t(x, y)) continue;
			const uint16_t m = g.world.t(x, y);
			if (trees ? (m & 0x30) == 0x30 : (m & 3) == 2) n++;
		}
	}
	return n;
}

bool site_started(const Game& g, int32_t w, int32_t o) {
	const Build* b = g.objects[size_t(w)].builds[size_t(o - kBuildBase)].get();
	return b && (b->flags & objflag::kStarted);
}

}  // namespace

int32_t find_building_placed_at(const Game& g, int32_t tx, int32_t ty, int32_t owner, int32_t o_self, int32_t who_self,
                                int32_t* who_out) {
	if (who_out) *who_out = -1;
	const World& w = g.world;
	if (!w.in_t(tx, ty)) return -1;
	const uint16_t m = w.t(tx, ty);
	if ((m & 3) != 3 && !(m & tmask::kStarted)) return -1;
	for (int32_t k = 0; k < 9; k++) {
		const int32_t wx = move_x[k] + (tx >> 2), wy = move_y[k] + (ty >> 2);
		if (!w.in_w(wx, wy)) continue;
		int32_t o = w.w(wx, wy).down, ow = w.w(wx, wy).down_who;
		while (o >= 0) {
			const Object* ob = g.object(ow, o);
			if (!ob) break;
			const int32_t no = ob->down, nw = ob->down_who;
			if (ow < 8 && (o != o_self || ow != who_self) && (owner < 0 || owner == ow) && !ob->is_unit && ob->alive()) {
				const Footprint f = footprint(*ob->ptype, ob->x, ob->y);
				if (f.x0 <= tx && tx < f.x0 + f.w && f.y0 <= ty && ty < f.y0 + f.h) {
					if (who_out) *who_out = ow;
					return o;
				}
			}
			o = no;
			ow = nw;
		}
	}
	return -1;
}

// ---------------------------------------------------------------- blocked_tcoord (3.6)

namespace {

int32_t blocked_tcoord(Game& g, int32_t type, int32_t x, int32_t y, int32_t who, int32_t o_self) {
	const World& w = g.world;
	const Type& bt = g.type(type);
	if (!w.in_t(x, y)) return block::kOffMap;
	const bool editor = g.editor;
	const bool seen = who < 0 || editor || was_seen(g, x >> 1, y >> 1, who);
	const bool really = seen && (who < 0 || editor || was_really_seen(g, x >> 1, y >> 1, who));
	auto C = [&](int32_t code) { return seen ? code : block::kSeen; };
	auto R = [&](int32_t code) { return really ? code : (seen ? block::kUnseen : block::kSeen); };
	const uint16_t m = w.t(x, y);
	const WData& W = w.w(x >> 2, y >> 2);
	if (W.flags & 0x8000) return R(block::kRuins);
	if ((m & 3) == 3) return R(block::kBuilding);
	if (!type_is(bt, ptype::kDock)) {
		if (bt.domain == 1) {
			if (who >= 0 && !(LD(g, who).flags & (leaderflag::kCanTransportCiv | leaderflag::kCanTransportMil |
			                                      leaderflag::kCanTransportSct)) && !editor)
				return C(block::kCantTransport);  // LeaderData::can_transport == 0
			if ((m & 0x30) != 0x20) return C(block::kLand);
		} else if (bt.domain == 0) {
			if ((m & 0x30) == 0x20) return C(block::kWater);
		}
	}
	if (m & tmask::kBlocked) {
		if ((m & 0x30) == 0x30) return C(block::kForest);
		if ((m & 3) == 2 || (W.flags & 0x10)) return C(block::kMountain);
		return R(block::kRare);
	}
	if (m & tmask::kResource) return R(block::kRare);
	if (!type_is(bt, ptype::kOilWell) && !type_is(bt, ptype::kOilPlatform)) {
		if (W.flags & 8) return C(block::kRock);
	} else if (!(W.flags & 0x800)) {
		return C(block::kNoOil);
	}
	if ((m & tmask::kStarted) && find_building_placed_at(g, x, y, who, o_self, who, nullptr) >= 0) return R(block::kBuilding);
	if (!(bt.build_flags & 0x10) && !(m & tmask::kCity)) return C(block::kOutsideRadius);
	const bool city = is_city_type(bt), fort = type_is(bt, ptype::kFortX);
	if ((city || fort) && who >= 0 && !tb(g, who, 0x13) && !(fort && tb(g, who, 0x16) && K("dutch_fort_placement") != 0)) {
		if (W.who < 0) {
			const int32_t rg = w.get_tregion(x, y);
			if (LD(g, who).reg_cities[size_t(rg & 63)] != 0 || LD(g, who).reg_forts[size_t(rg & 63)] != 0)
				return fort ? block::kNeutralTerritory : block::kTerritory;
			for (int pass = 0; pass < 2; pass++) {
				for (const Unbuilt& u : (pass == 0 ? g.unbuilt_cities : g.unbuilt_forts)[size_t(who)]) {
					if (u.o < 0 || u.o == o_self) continue;
					const Build* ub = g.build(who, u.o);
					if (!ub) continue;
					if (w.get_tregion(tcoord_of(ub->x), tcoord_of(ub->y)) == rg)
						return seen ? (fort ? block::kNeutralTerritory : block::kTerritory) : block::kSeen;
				}
			}
		}
	}
	if (bt.build_flags & 0x400) return C(block::kNeedWall);
	if (m & tmask::kRiverB) return C(block::kRiver);
	if (is_gather_type(bt) && is_flat_gather(bt)) {
		// LandData::get_amount(get_land(x, y, 1), get_good()) == 0 -> NO_RESOURCES.
		const int32_t land = w.gather_land_tile(x, y);  // get_land(TCoord, 1) @6b4c70
		static const int32_t kGood[] = {0, 1, 4, 3, 5, 5};
		const int32_t gd = type >= ptype::kFarm && type <= ptype::kOilPlatform ? kGood[type - ptype::kFarm] : -1;
		int32_t amount = 0;
		if (land >= 0 && size_t(land) < g.rules->lands.size()) {
			for (const LandMake& lm : g.rules->lands[size_t(land)])
				if (lm.good == gd) amount += lm.num;
		}
		if (amount == 0) return C(block::kNoResources);
	}
	return block::kNone;
}

bool masked_in_editor(int32_t r) {
	return r == 32 || r == 31 || r == 36 || r == 24 || r == 20 || r == 23 || r == 26 || r == 27 || r == 46 || r == 45;
}

// ---------------------------------------------------------------- blocked_location (3.7)

int32_t blocked_location(Game& g, int32_t type, int32_t cx, int32_t cy, int32_t x0, int32_t y0, int32_t who,
                         int32_t o_self, int32_t* gather) {
	const World& w = g.world;
	const Type& bt = g.type(type);
	const Footprint f{x0, y0, bt.x_size, bt.y_size};
	const int32_t tcx = tcoord_of(cx), tcy = tcoord_of(cy);
	const bool city = is_city_type(bt), fort = type_is(bt, ptype::kFortX);
	bool coast_ok = false;
	if (who >= 0) {
		const Leader& l = LD(g, who);
		const int32_t rg = w.in_t(tcx, tcy) ? w.get_tregion(tcx, tcy) : 0;
		if (!city && !fort) {
			if (!(bt.build_flags & 0x20) && type != ptype::kDock && !g.editor) {
				const int32_t nf = non_friendly_territory(g, f, who);
				if (nf >= 3) return block::kEnemyTerritory;
				if (nf == 2) return block::kNeutralTerritory;
				if (nf == 1 && !tb(g, who, 0x13)) return block::kNeutralTerritory;
			}
		} else {
			if (in_enemy_territory(g, f, who)) return l.city_num != 0 ? block::kEnemyTerritory : block::kEnemyTerritory2;
			const int32_t fcnc = K("first_city_near_coast");
			if (l.reg_forts[size_t(rg & 63)] == 0 && l.reg_cities[size_t(rg & 63)] == 0 && l.city_num != 0 && fcnc != 0) {
				if (!has_preq(g, who, ptype::kColonizeBonus)) return block::kColonize;
				const Circles& Ci = circles();
				const int32_t wx0 = wcoord_of(cx), wy0 = wcoord_of(cy);
				const int32_t n = Ci.radius[size_t(std::clamp(fcnc / 4, 0, 64))];
				for (int32_t k = 0; k < n; k++) {
					const int32_t wx = Ci.x[size_t(k)] + wx0, wy = Ci.y[size_t(k)] + wy0;
					if (!w.in_w(wx, wy) || !w.is_ocean(wx, wy)) continue;
					const int32_t r = w.w(wx, wy).region;
					if (r >= 0 && r < Regions::kNum && g.regions.r[size_t(r)].size >= 25) {
						coast_ok = true;
						break;
					}
				}
				if (!coast_ok) return city ? block::kPortCity : block::kPortFort;
			}
		}
		if (city) {
			for (int32_t L = 0; L < 8; L++) {
				if (!(LD(g, L).flags & leaderflag::kValid)) continue;
				int32_t sp = K("city_spacing");
				const int32_t rs = (rg >= 0 && rg < Regions::kNum) ? g.regions.r[size_t(rg)].size : 0;
				if (rg < 64 && l.reg_terr[size_t(rg & 63)] >= (rs * 9) / 10) sp -= K("relax_city_spacing");
				for (const Unbuilt& u : g.unbuilt_cities[size_t(L)]) {
					if (u.o < 0) continue;
					const bool counted = (L == who && u.o != o_self) || site_started(g, L, u.o);
					if (!counted) continue;
					const Build* ub = g.build(L, u.o);
					if (!ub) continue;
					if (w.w(wcoord_of(ub->x), wcoord_of(ub->y)).region != rg) continue;
					if (int32_t(vdist(tcoord_of(ub->x) - tcx, tcoord_of(ub->y) - tcy)) <= sp) return block::kCityDistance;
				}
				if (L == who || LD(g, L).reg_cities[size_t(rg & 63)] != 0) {
					const auto& cl = g.cities[size_t(L)];
					for (int32_t c = 0; c < LD(g, L).city_mark && size_t(c) < cl.size(); c++) {
						const City& ct = cl[size_t(c)];
						if (!ct.valid() || ct.reg != rg) continue;
						if (int32_t(vdist(tcoord_of(ct.x) - tcx, tcoord_of(ct.y) - tcy)) <= sp) return block::kCityDistance;
					}
				}
			}
		}
		if (fort) {
			const int32_t rg2 = w.w(wcoord_of(cx), wcoord_of(cy)).region;
			for (int32_t L = 0; L < 8; L++) {
				if (!(LD(g, L).flags & leaderflag::kValid)) continue;
				if (L != who) {
					const int32_t sp2 = mutual(g, who, L) ? K("fort_spacing") : K("fort_to_enemy_city_spacing");
					for (const Unbuilt& u : g.unbuilt_cities[size_t(L)]) {
						if (u.o < 0 || !site_started(g, L, u.o)) continue;
						const Build* ub = g.build(L, u.o);
						if (!ub || w.w(wcoord_of(ub->x), wcoord_of(ub->y)).region != rg2) continue;
						if (int32_t(vdist(tcoord_of(ub->x) - tcx, tcoord_of(ub->y) - tcy)) <= sp2) return block::kFortCityDistance;
					}
					if (LD(g, L).reg_cities[size_t(rg2 & 63)] != 0) {
						const auto& cl = g.cities[size_t(L)];
						for (int32_t c = 0; c < LD(g, L).city_mark && size_t(c) < cl.size(); c++) {
							const City& ct = cl[size_t(c)];
							if (!ct.valid() || ct.reg != rg2) continue;
							if (int32_t(vdist(tcoord_of(ct.x) - tcx, tcoord_of(ct.y) - tcy)) <= sp2) return block::kFortCityDistance;
						}
					}
				}
				const int32_t fs = K("fort_spacing");
				const int32_t code = L == who ? block::kFortDistance : block::kFortCityDistance;
				for (const Unbuilt& u : g.unbuilt_forts[size_t(L)]) {
					if (u.o < 0) continue;
					const bool counted = (L == who && u.o != o_self) || site_started(g, L, u.o);
					if (!counted) continue;
					const Build* ub = g.build(L, u.o);
					if (!ub || w.w(wcoord_of(ub->x), wcoord_of(ub->y)).region != rg2) continue;
					if (int32_t(vdist(tcoord_of(ub->x) - tcx, tcoord_of(ub->y) - tcy)) <= fs) return code;
				}
				if (L == who || LD(g, L).reg_forts[size_t(rg2 & 63)] != 0) {
					const auto& fl = g.forts[size_t(L)];
					for (int32_t i = 0; i < LD(g, L).fort_mark && size_t(i) < fl.size(); i++) {
						const Fort& ft = fl[size_t(i)];
						if (!(ft.fort_flags & 1)) continue;
						const Build* fb = g.build(L, ft.o);
						if (!fb) continue;
						if (int32_t(vdist(tcoord_of(fb->x) - tcx, tcoord_of(fb->y) - tcy)) <= fs) return code;
					}
				}
			}
		}
		if (!(bt.build_flags & 0x10)) {
			const int32_t town = get_town(g, type, tcx, tcy, who);
			if (town < 0) return (bt.build_flags & 0x800) ? block::kOutsideTown : block::kOutsideRadius;
			const Build* tbld = g.build(who, town);
			for (int32_t i = 0; i < tbld->queued; i++) {
				const int32_t q = tbld->build_queue.get_queue(i);
				if (q == ptype::kDisband || q == ptype::kRaze) return block::kRazingTown;
			}
			const int32_t tc = tbld->city;
			if ((bt.build_flags & 0x200) && city_count_buildings(g, who, tc, type, 0, 0) != 0) {
				const auto& cl = g.cities[size_t(who)];
				for (int32_t c = 0; c < l.city_mark && size_t(c) < cl.size(); c++) {
					if (c == tc || !cl[size_t(c)].valid()) continue;
					const Build* cb = g.build(who, cl[size_t(c)].o);
					const int32_t r = std::min(64, get_radius(g, who, cb ? cb->type : ptype::kVillage));
					if (int32_t(vdist(tcoord_of(cl[size_t(c)].x) - tcx, tcoord_of(cl[size_t(c)].y) - tcy)) <= r &&
					    city_count_buildings(g, who, c, type, 0, 0) == 0)
						return block::kOneOther;
				}
				return block::kOne;
			}
			if (type == ptype::kFarm) {
				// CityData::get_farm_limit = LeaderData::get_farm_limit + (level - 1) * farms_per_city_level.
				int32_t lim = tb(g, who, 7) ? K("egyptian_farms_per_city_base") : K("farms_per_city_base");
				if (has_wonder_prod(g, who, ptype::kTajMahal)) lim += K("taj_farms");
				if (has_wonder_prod(g, who, ptype::kKremlin)) lim += K("kremlin_farms");
				if (has_rare(g, who, 39)) lim += K("olive_farms");  // OLIVEOIL
				lim += (city_get_level(tbld->type) - 1) * K("farms_per_city_level");
				if (city_count_buildings(g, who, tc, ptype::kFarm, 0, 0) >= lim) return block::kFarm;
			}
			if (is_wonder_type(type) && !type_is(bt, ptype::kRedFort) &&
			    city_num_wonders(g, who, tc, 1) > (tb(g, who, 7) ? 1 : 0))
				return block::kWonder;
		}
	}
	// F. Water and adjacency (also for who < 0).
	const int32_t area = bt.x_size * bt.y_size;
	if (type_is(bt, ptype::kDock)) {
		const int32_t nw = count_water(g, f);
		if (nw < (area * 3) / 4) {
			if (nw == 0) return block::kDockWater;
			if (check_land_adjacent(g, f, who) != 0 && !g.editor) return block::kDockTerr;
			return block::kDockWater;
		}
		const int32_t cla = check_land_adjacent(g, f, who);
		if (cla == 1 && !g.editor) return block::kDockTerr;
		if (cla == 2) return block::kDockLand;
	} else if (bt.domain == 1) {
		if (count_water(g, f) < area) return block::kLand;
	} else if (bt.domain == 0) {
		if (count_water(g, f) != 0) return block::kWater;
	}
	if (city || fort) {
		const int32_t d = coast_ok ? 1 : 2;
		if (count_adjacent(g, f, d, false) != 0) return block::kMountain;
		if (count_adjacent(g, f, d, true) != 0) return block::kForest;
	}
	if ((type_is(bt, ptype::kOilWell) || type_is(bt, ptype::kOilPlatform)) && who >= 0 &&
	    !was_seen(g, cx / 384, cy / 384, who) && !g.editor)
		return block::kNoOil;
	if (is_gather_type(bt) && !is_flat_gather(bt)) {
		// BuildTypeData::calc_gather preview (economy-spec 3.4): the slot count with the site's own list.
		const int32_t amount = max_gatherers(g, type, (o_self >= 0 && who >= 0) ? o_self : -1, who, x0, y0);
		if (gather) *gather = std::max(amount, 0);
		if (amount == 0) return type_is(bt, ptype::kMine) ? block::kNoMountain : type_is(bt, ptype::kWoodcutter) ? block::kNoForest : block::kNoResources;
		if (amount < 0) return type_is(bt, ptype::kWoodcutter) ? block::kForestTaken : block::kMountainTaken;
	}
	return block::kNone;
}

}  // namespace

int32_t blocked_site(Game& g, int32_t type, int32_t cx, int32_t cy, int32_t who, int32_t o_self, int32_t* gather) {
	const Type& bt = g.type(type);
	const int32_t sx = tcoord_of(cx) * 192 + ((bt.x_size & 1) ? 96 : 0);
	const int32_t sy = tcoord_of(cy) * 192 + ((bt.y_size & 1) ? 96 : 0);
	const int32_t x0 = tcoord_of(sx) - (bt.x_size >> 1), y0 = tcoord_of(sy) - (bt.y_size >> 1);
	if (is_city_type(bt) && who >= 0) {
		for (int32_t L = 0; L < 8; L++) {
			if (!(LD(g, L).flags & leaderflag::kValid)) continue;
			if (L == who || mutual(g, who, L)) {
				if ((LD(g, who).flags & leaderflag::kHuman) && LD(g, L).lost_city_stamp > g.frame - 75 && !g.editor)
					return block::kLostCity;
			}
		}
	}
	int32_t best = 0, unseen = 0;
	for (int32_t x = x0; x < x0 + bt.x_size; x++) {
		for (int32_t y = y0; y < y0 + bt.y_size; y++) {
			const int32_t r = blocked_tcoord(g, type, x, y, who, o_self);
			if (r == block::kOffMap) return block::kOffMap;
			if (r != 0 && (best == 0 || r == block::kTerritory)) {
				if (!g.editor || !masked_in_editor(r)) best = r;
			}
			if (who >= 0 && !g.editor && !was_seen(g, x >> 1, y >> 1, who)) unseen++;
		}
	}
	if (best == block::kSeen) return block::kSeen;
	if (unseen > (bt.x_size * bt.y_size) / 2 && !type_is(bt, ptype::kDock)) return block::kSeen;
	if (best == block::kWater) return block::kWater;
	const int32_t loc = blocked_location(g, type, cx, cy, x0, y0, who, o_self, gather);
	if (loc == 0 || (g.editor && masked_in_editor(loc))) return best;
	return loc;
}

void snap_center_who(Game& g, int32_t type, int32_t x, int32_t y, int32_t* ox, int32_t* oy, int32_t who) {
	const Type& bt = g.type(type);
	const World& w = g.world;
	if (type_is(bt, ptype::kOilWell) || type_is(bt, ptype::kOilPlatform)) {
		const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
		if (w.in_w(wx, wy) && (w.w(wx, wy).flags & wflag::kOil)) {
			x = wcoord_centre(wx);
			y = wcoord_centre(wy);
		} else {
			for (int32_t k = 1; k <= 8; k++) {
				const int32_t nx = wx + move_x[k], ny = wy + move_y[k];
				if (!w.in_w(nx, ny) || !(w.w(nx, ny).flags & wflag::kOil)) continue;
				if (who >= 0 && !was_really_seen(g, nx * 2, ny * 2, who)) continue;  // TODO: the fog-cell argument
				x = wcoord_centre(nx);
				y = wcoord_centre(ny);
				break;
			}
		}
	}
	if (type_is(bt, ptype::kDock) || (who >= 0 && (type_is(bt, ptype::kMine) || type_is(bt, ptype::kWoodcutter)))) {
		const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
		if (blocked_site(g, type, tx * 192 + 96, ty * 192 + 96, who, -1, nullptr) != 0) {
			for (int32_t k = 1; k <= 80; k++) {
				const int32_t nx = tx + move_x[k], ny = ty + move_y[k];
				if (ny < 0 || ny >= w.tile_ys) continue;  // (only the candidate's y is bounds-checked)
				if (blocked_site(g, type, nx * 192 + 96, ny * 192 + 96, who, -1, nullptr) == 0) {
					x = nx * 192 + 96;
					y = ny * 192 + 96;
					break;
				}
			}
		}
	}
	*ox = tcoord_of(x) * 192 + ((bt.x_size & 1) ? 96 : 0);
	*oy = tcoord_of(y) * 192 + ((bt.y_size & 1) ? 96 : 0);
}

}  // namespace ron::sim
