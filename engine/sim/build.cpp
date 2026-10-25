// Buildings (docs/production-spec.md sections 3.3, 4.4-4.6, 4.9, 5.3, 5.13, 8.3): Objects::init_build -> Build::init ->
// Wall::init, Build::start, Build::activate, hit points, the leader's building counters, gather tiles, disbanding and
// closing.
#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/compass.hpp"
#include "sim/heroes.hpp"
#include "sim/roads.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/production_internal.hpp"

namespace ron::sim {

constexpr int32_t kSupplyWagon = 63, kHorseArchers = 187, kCatapult = 265, kBiplane = 287, kFishermen = 317, kTrireme = 340;

// cities.cpp
int32_t cities_init_city(Game& g, int32_t who, int32_t o, bool loading, bool capital);
void cities_close_city(Game& g, int32_t who, int32_t c);
void city_regen_roads(Game& g, int32_t who, int32_t c);
void city_close(Game& g, int32_t who, int32_t c, int32_t attacker);
int32_t forts_init_fort(Game& g, int32_t who, int32_t o);
void forts_close_fort(Game& g, int32_t who, int32_t idx);
int32_t wonders_init_wonder(Game& g, int32_t who, int32_t o);
void wonders_close_wonder(Game& g, int32_t who, int32_t idx);
void unbuilt_remove(std::vector<Unbuilt>& v, int32_t o, int32_t who);

using prod::is_t;
using prod::tb;

namespace {

[[maybe_unused]] Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
[[maybe_unused]] const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }

int32_t tdist(int32_t ax, int32_t ay) {
	uint32_t hi = uint32_t(std::abs(ax)), lo = uint32_t(std::abs(ay));
	if (hi < lo) std::swap(hi, lo);
	if (hi == 0) return 0;
	if (lo >= 60000) return int32_t((lo + 2 * hi) >> 1);
	return int32_t(lo * lo / (2 * hi) + hi);
}

}  // namespace

// ---------------------------------------------------------------- type predicates

bool is_gather_type(const Type& t) { return (t.build_flags & 0x40) != 0; }
bool is_flat_gather(const Type& t) { return (t.build_flags & 0x10000000) != 0; }

int32_t basic_type(const Game& g, int32_t t) {
	for (int guard = 0; guard < 64; guard++) {
		const int32_t f = g.type(t).from;
		if (f < 0) return t;
		t = f;
	}
	return t;
}
bool is_military_trainer(const Game& g, int32_t t) { return (g.type(basic_type(g, t)).build_flags & 0x40000000) != 0; }
bool is_training_building(const Game& g, int32_t t) { return (g.type(basic_type(g, t)).build_flags & 0x80000000u) != 0; }

int32_t build_stance_type(const Game& g, int32_t t) {
	if (!is_training_building(g, t)) return kStanceNone;
	if (is_t(g, t, ptype::kVillage)) return kStanceWorker;
	if (is_t(g, t, ptype::kFortX)) return kStanceCaster;
	const int32_t ty = g.type(t).type;
	if (ty == 0x1ae || ty == 0x1af) return kStancePacker;
	if (ty == 0x1b4 || ty == 0x1a4 || ty == 0x1bf) return kStanceNone;
	return ty != 0x208 ? kStanceCombat : kStanceNone;  // (ty != 0x208) - 1 in StanceTypes terms: 0 combat / -1
}

int32_t get_garrison_limit(const Game& g, int32_t t, int32_t who) {
	const Type& ty = g.type(t);
	int32_t n = ty.garrison_max;
	auto lvl = [&]() {
		if (has_preq(g, who, 729)) return 4;
		if (has_preq(g, who, 728)) return 3;
		return has_preq(g, who, 727) ? 2 : 1;
	};
	if (is_t(g, t, ptype::kTower)) n += (lvl() - 1) * K("tower_garrison_upgrade");
	if (is_t(g, t, ptype::kFortX)) return (lvl() - 1) * K("fort_garrison_upgrade") + n;
	return n;
}

// ---------------------------------------------------------------- counters

void wall_increment_stats(Game& g, Build& b) {
	Leader& l = LD(g, b.who);
	if (!(b.flags & objflag::kActive)) {
		if (b.type >= 0) l.num_queued[size_t(b.type)]++;
		return;
	}
	l.num_buildings[size_t(b.type - kBuilds)]++;
	const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
	const int32_t reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : 64;
	if (reg < 64) l.reg_buildings[size_t(reg)][size_t(b.type - kBuilds)]++;
	if (is_military_trainer(g, b.type)) l.mil_trainers.push_back(b.o);  // the AI's trainer list (+0x6e50)
}

void wall_decrement_stats(Game& g, Build& b) {
	Leader& l = LD(g, b.who);
	if (!(b.flags & objflag::kActive)) {
		if (b.type >= 0 && l.num_queued[size_t(b.type)] != 0) l.num_queued[size_t(b.type)]--;
		return;
	}
	l.num_buildings[size_t(b.type - kBuilds)]--;
	const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
	const int32_t reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : 64;
	if (reg < 64) l.reg_buildings[size_t(reg)][size_t(b.type - kBuilds)]--;
	if (is_military_trainer(g, b.type)) {  // SimpleArray<int>::remove: the first match, order kept
		auto it = std::find(l.mil_trainers.begin(), l.mil_trainers.end(), int32_t(b.o));
		if (it != l.mil_trainers.end()) l.mil_trainers.erase(it);
	}
}

// GraphicPieces::get_build_gpiece @90bce0 (load 1, param_5 1: the first piece that exists is returned). The candidates, each from the
// building's age (clamped to 0..7) down to age 0: the tribe's BUILD_CONTINENT style, its BACKUP_BUILD_CONTINENT style when that differs, the
// DEFAULT style; none: age 0 of the DEFAULT style. A piece exists when building_graphics.xml has its <BUILD name="GRAPH-STYLE-AGEn"> and the
// art files are there (AnimSource::build_piece_exists); without art data nothing does.
int32_t build_gpiece(const Game& g, int32_t type, int32_t who, int32_t age) {
	using namespace gpiece;
	const int32_t t = type - kFirstBuildType;
	age = std::clamp(age, 0, 7);
	if (g.anims && who >= 0 && who < kNumLeaders) {
		const Leader& l = g.leaders[size_t(who)];
		const Tribe* tr = size_t(l.tribe) < g.rules->tribes.size() ? &g.rules->tribes[size_t(l.tribe)] : nullptr;
		std::vector<int32_t> styles;
		if (tr) {
			styles.push_back(tr->build_continent);
			if (tr->backup_build_continent != tr->build_continent) styles.push_back(tr->backup_build_continent);
		}
		styles.push_back(0);
		for (const int32_t style : styles) {
			if (style < 0 || style > 8) continue;
			for (int32_t a = age; a >= 0; a--)
				if (g.anims->build_piece_exists(type, style, a)) return kFirstBuild + kBuildPerStyle * style + kBuildPerAge * a + t;
		}
	}
	return kFirstBuild + t;
}

void build_update_gpiece(Game& g, Build& b) {
	if (!b.ptype) return;
	b.gpiece = build_gpiece(g, b.type, b.founder, b.max_age);
}

void wall_set_type(Game& g, Build& b, int32_t t, int32_t mode) {
	if (mode == 0) wall_decrement_stats(g, b);
	b.type = t;  // SubObject::set_type
	b.ptype = &g.type(t);
	build_update_gpiece(g, b);  // Wall::update_gpiece
	if (mode != 2) wall_increment_stats(g, b);
}

// ---------------------------------------------------------------- hit points

int32_t wall_update_hits(Game& g, Build& b, bool full) {
	const int32_t who = b.who;
	int32_t h = b.ptype->hits;  // Object::update_hits
	if (tb(g, who, 1)) h = ((K("maya_building_hp") + 100) * h) / 100;
	if (tb(g, who, 6) && (is_t(g, b.type, ptype::kFortX) || is_t(g, b.type, ptype::kTower)) && !is_wonder_type(b.type))
		h = ((K("roman_fort_hp") + 100) * h) / 100;
	h = ((K("building_hp_upgrade") * get_building_hp_upgrade(g, who) + 100) * h) / 100;
	if (has_wonder_prod(g, who, ptype::kTajMahal)) h = ((K("taj_building_hp") + 100) * h) / 100;
	if (has_wonder_prod(g, who, ptype::kRedFort) && is_t(g, b.type, ptype::kFortX) && !is_wonder_type(b.type))
		h = ((K("red_fort_fort_hps") + 100) * h) / 100;
	if (is_t(g, b.type, ptype::kMarket) && tb(g, who, 4) && K("nubian_hit_points") != 0)
		h = ((K("nubian_hit_points") + 100) * h) / 100;
	if (!(b.flags & objflag::kCity)) {
		if ((b.flags & objflag::kActive) && b.city >= 0 && !is_t(g, b.type, ptype::kFortX) &&
		    !is_t(g, b.type, ptype::kTower) && !is_t(g, b.type, 0x209)) {
			// (the decompile reads the city level of the building's city: senate_hp_bonus per level above 1)
			const Build* cb = g.build(who, g.cities[size_t(who)][size_t(b.city)].o);
			const int32_t lvl = cb ? city_get_level(cb->type) : 1;
			h += (K("senate_hp_bonus") * (lvl - 1) * h) / 100;
		}
	} else if ((b.flags & objflag::kActive) && b.city >= 0 &&
	           (g.cities[size_t(who)][size_t(b.city)].city_flags & cityflag::kHasTemple)) {
		int32_t lvl = has_preq(g, who, 717) ? 4 : has_preq(g, who, 716) ? 3 : has_preq(g, who, 715) ? 2 : 1;
		int32_t add = g.rules->constants.at(uint32_t(constant_offset("temple_upgrade_hp")) + uint32_t(lvl - 1) * 4);
		if (has_wonder_prod(g, who, ptype::kTikal)) add = ((K("tikal_temple_hp") + 100) * add + 99) / 100;
		h = ((add + 100) * h) / 100;
		// Missionaries (card 32, Wall::update_hits @63f0d0): the temple effect on the wall hit points, + ctw_missionaries_bonus %.
		if (g.ctw && g.leaders[size_t(who)].num_bonus_cards[32] != 0) h = ((K("ctw_missionaries_bonus") + 100) * h) / 100;
	}
	b.myhits = h;
	int32_t ch = h;
	if (!(b.flags & objflag::kActive)) {
		const uint32_t jc = uint32_t(b.job_counter), ct = uint32_t(construct_time(g, b, 0));
		if (jc < ct) {
			uint32_t a = jc >> 5;
			if (a == 0) a = 1;
			uint32_t c = ct >> 5;
			if (c == 0) c = 1;
			if (!is_wonder_type(b.type)) {
				ch = int32_t(a * uint32_t(h)) / int32_t(c);
				if (ch < 1) ch = 1;
			} else {
				int32_t v = int32_t(uint32_t(h / 2) * a) / int32_t(c);
				if (v < 1) v = 1;
				ch = (h + 1) / 2 + v;
			}
		}
	}
	b.construct_hits = ch;
	if (b.construct_hits <= b.damage && b.inside_down >= 0 && !can_carry_domain(g, b, kDomainAir))
		eject_contents(g, b, 1, -1, 0, 1);
	return full ? b.myhits : b.construct_hits;
}

// ---------------------------------------------------------------- gather tiles (8.3, economy-spec 3.6)

// (gather_radius, owner_ok, mountains_find_nearest, mountain_gather_size are shared with econ_income.cpp)

// BuildTypeData::gather_radius @63bc60 (tiles).
int32_t gather_radius(const Game& g, const Type& t) {
	if (!is_gather_type(t) || is_flat_gather(t)) return 0;
	if (type_is(t, ptype::kUniversity)) return 0;
	if (type_is(t, ptype::kWoodcutter)) return K("woodcutter_radius");
	return K("mine_radius");
}

bool owner_ok(const Game& g, int32_t owner, int32_t who) {
	return owner < 0 || owner == who || (who >= 0 && is_ally(g, who, owner));
}

// MountainsData::find_nearest @89cd30: the placed mountain (index) one of whose solid wcoords is nearest to (x, y)
// (octagonal distance to the wcoord centre), restricted to `region` (tested at the range's first solid wcoord).
int32_t mountains_find_nearest(const Game& g, int32_t x, int32_t y, int32_t region, int32_t* dist) {
	int32_t best = -1;
	uint32_t bd = 0xffffffffu;
	for (size_t i = 0; i < g.mountains.size(); i++) {
		const PlacedRange& m = g.mountains[i];
		if (m.solid.empty()) continue;
		if (region >= 0) {
			const int32_t wx = m.solid[0][0] + m.wx, wy = m.solid[0][1] + m.wy;
			const int32_t r = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : -999;
			if (r != region) continue;
		}
		for (const auto& s : m.solid) {
			const int32_t dx = std::abs(x - ((s[0] + m.wx) * 0x300 + 0x180));
			const int32_t dy = std::abs(y - ((s[1] + m.wy) * 0x300 + 0x180));
			uint32_t d;
			uint32_t ax = uint32_t(dx), ay = uint32_t(dy);
			if (int32_t(ay) < int32_t(ax)) d = ax == 0 ? 0 : ay < 60000 ? ay * ay / (ax * 2) + ax : (ay + ax * 2) >> 1;
			else d = ay == 0 ? 0 : ax < 60000 ? ax * ax / (ay * 2) + ay : (ax + ay * 2) >> 1;
			if (int32_t(d) < int32_t(bd) || best < 0) {
				best = int32_t(i);
				bd = d;
			}
		}
	}
	if (dist) *dist = int32_t(bd);
	return best;
}

// MountainRangeData::gather_size(wx, wy, who, list) @89d170.
int32_t mountain_gather_size(const Game& g, const PlacedRange& m, int32_t who, const Build* list) {
	if (m.solid.empty()) return 0;
	int32_t total = 0, claim = 0;
	const World& w = g.world;
	for (const auto& s : m.solid) {
		const int32_t wx = s[0] + m.wx, wy = s[1] + m.wy;
		if (!w.in_w(wx, wy)) continue;
		if ((w.w(wx, wy).flags & 0x20) || (w.t(4 * wx + 2, 4 * wy + 2) & 3) != 2) continue;
		total++;
		if (!owner_ok(g, w.w(wx, wy).who, who)) continue;
		if (list) {
			bool found = false;
			for (const auto& t : list->gather_from) found |= t[0] == wx * 4 + 2 && t[1] == wy * 4 + 2;
			if (!found) continue;
		}
		claim++;
	}
	const int32_t n = m.tcoord_count;
	int32_t cls = n < K("mtn_tiny_size")     ? K("mtn_tiny_gather")
	              : n < K("mtn_small_size")  ? K("mtn_small_gather")
	              : n < K("mtn_medium_size") ? K("mtn_medium_gather")
	              : n < K("mtn_large_size")  ? K("mtn_large_gather")
	                                         : K("mtn_huge_gather");
	if (total != 0) cls = (cls * claim) / total;
	if (claim != 0 && cls < 1) cls = 1;
	return cls;
}

// BuildTypeData::find_gather_tcoords(x, y, list, who) @63bdc0 (from the disassembly).
void find_gather_tcoords(Game& g, const Type& bt, int32_t x, int32_t y, Build& b, int32_t who) {
	int32_t kind;
	if (type_is(bt, ptype::kWoodcutter)) kind = 1;
	else if (type_is(bt, ptype::kMine)) kind = 4;
	else return;
	World& w = g.world;
	const int32_t wx0 = wcoord_of(x), wy0 = wcoord_of(y);
	const int32_t R = gather_radius(g, bt);
	if (type_is(bt, ptype::kMine)) {
		const int32_t Rc = R * 192;
		int32_t mtn = b.mtn, cliff = b.cliff, dm = 0, dc = 0;
		if (mtn < 0 && cliff < 0) {
			const int32_t region = w.in_w(wx0, wy0) ? w.w(wx0, wy0).region : -1;
			mtn = mountains_find_nearest(g, x, y, region, &dm);
			cliff = -1;  // CliffsData::find_nearest: the stock data has no cliffs (world-spec 3.4). TODO(cliffs).
			dc = -1;
		}
		bool use_cliff = cliff >= 0 && (mtn < 0 || dm > dc);
		if (!use_cliff) {
			if (mtn < 0) return;
			if (dm > Rc) return;
			b.mtn = int8_t(mtn);
			const PlacedRange& m = g.mountains[size_t(mtn)];
			for (const auto& t : m.tiles) {
				const int32_t tx = t[0] + m.wx * 4, ty = t[1] + m.wy * 4;
				if (!w.in_t(tx, ty)) continue;
				const uint16_t mk = w.t(tx, ty);
				if ((mk & 0x30) == 0x30) continue;
				if (!owner_ok(g, w.w(tx >> 2, ty >> 2).who, who)) continue;
				if (mk & tmask::kGathered) continue;
				b.gather_from.push_back({tx, ty});
			}
		} else {
			if (dc > Rc) return;
			b.cliff = int8_t(cliff);  // TODO(cliffs): the cliff's tile list.
		}
		return;
	}
	const int32_t rw = (R + 3) / 4;
	const Circles& C = circles();
	const int32_t tcx = tcoord_of(x), tcy = tcoord_of(y);
	for (int32_t k = 0; k < C.radius[size_t(std::clamp(rw, 0, 64))]; k++) {
		const int32_t wx = C.x[size_t(k)] + wx0, wy = C.y[size_t(k)] + wy0;
		if (!w.in_w(wx, wy)) continue;
		if (tdist(std::abs(4 * wx - tcx + 2), std::abs(4 * wy - tcy + 2)) > R) continue;
		if (!owner_ok(g, w.w(wx, wy).who, who)) continue;
		if (w.t(4 * wx + 2, 4 * wy + 2) & tmask::kGathered) continue;
		if (kind == 4 && (w.w(wx, wy).flags & 0x20)) continue;
		for (int32_t i = 0; i < 16; i++) {
			const int32_t tx = (i & 3) + 4 * wx, ty = (i >> 2) + 4 * wy;
			const uint16_t mk = w.t(tx, ty);
			if (kind == 1 ? (mk & 0x30) != 0x30 : (mk & 3) != 2) continue;
			if (mk & tmask::kGathered) continue;
			b.gather_from.push_back({tx, ty});
		}
	}
}

void find_gather_tiles(Game& g, Build& b) {
	const Type& bt = *b.ptype;
	if (!is_gather_type(bt) || is_flat_gather(bt) || type_is(bt, ptype::kUniversity)) return;
	const int32_t old = int32_t(b.gather_from.size());
	find_gather_tcoords(g, bt, b.x, b.y, b, b.who);
	for (const auto& t : b.gather_from) g.world.set_mask_bit(t[0], t[1], tmask::kGathered, true);
	const int32_t n = int32_t(b.gather_from.size());
	if (old < n) {
		for (int32_t k = 4 * n; k > 0; k--) {
			const int32_t i = n - 1 < 1 ? 0 : int32_t(g.rng.get(0, 0xffff) % uint32_t(n));
			const std::array<int32_t, 2> t = b.gather_from[size_t(i)];
			// Array<TCoordData>::remove: the first entry equal to t, then append.
			for (size_t j = 0; j < b.gather_from.size(); j++) {
				if (b.gather_from[j] == t) {
					b.gather_from.erase(b.gather_from.begin() + std::ptrdiff_t(j));
					break;
				}
			}
			b.gather_from.push_back(t);
		}
	}
	const Footprint f = footprint(bt, b.x, b.y);
	b.gather_max = int8_t(max_gatherers(g, b.type, b.o, b.who, f.x0, f.y0));
}

// ---------------------------------------------------------------- creation (3.3)

// TerrainOut::terraform_for_building @875210 (the render-side normals / tangents / visibility calls left out): flatten the height grid
// under a new building. Averages the heights of the vertices from (tx - 1, ty - 1) to (tx + tw + 1, ty + th + 1) (the far corner row and
// column included, in the original's summing order; nothing changes when any sampled height is not above 0), then writes the average on the
// vertices of each wcoord that is land (WData.land 0) and not next to a mountain / cliff tile, and the mean of the old height and the average
// on the outer ring of the rectangle. Wcoords with a good on a resource tile and open-water wcoords (land 1 / 2 without the 0x100 flag) are
// left alone. (The same routine as TerrainInit::terraform_for_building in the generator, which places the rocky patches under oil.)
void terraform_for_building(Game& g, int32_t tx, int32_t ty, int32_t tw, int32_t th) {
	World& w = g.world;
	Heights& H = g.heights;
	if (H.h.empty()) return;
	const int32_t side = 4 * w.xs;  // the original compares both coordinates with 4 * xs - 1
	if (tx == 0 || ty == 0 || tx == side - 1 || ty == side - 1) return;
	const int32_t x0 = std::max(tx - 1, 0), y0 = std::max(ty - 1, 0);
	int32_t y1 = th + ty + 2, x1 = tw + tx + 2;
	if (w.ys * 4 <= y1) y1 = w.ys * 4 - 1;
	if (w.xs * 4 <= x1) x1 = w.xs * 4 - 1;
	auto h = [&](int32_t x, int32_t y) -> float& { return H.h[size_t(y) * size_t(H.vxs) + size_t(x)]; };
	float sum = 0.0f;
	int32_t count = 0;
	for (int32_t y = y0; y < y1; y++) {
		const float v = h(x1, y);
		sum = sum + v;
		if (v <= 0.0f) return;
		count++;
	}
	for (int32_t x = x0; x < x1; x++) {
		const float v = h(x, y1);
		sum = sum + v;
		if (v <= 0.0f) return;
		count++;
	}
	for (int32_t y = y0; y < y1; y++) {
		for (int32_t x = x0; x < x1; x++) {
			const float v = h(x, y);
			sum = sum + v;
			if (v <= 0.0f) return;
			count++;
		}
	}
	const float avg = sum / float(count);
	for (int32_t y = y0; y < y1; y++) {
		for (int32_t x = x0; x < x1; x++) {
			const int32_t wx = x >> 2, wy = y >> 2;
			const WData& d = w.w(wx, wy);
			if ((d.flags & 0x100) == 0 && (d.land == 1 || d.land == 2)) continue;  // open water
			bool next_to_mountain = false;
			for (int32_t k = 0; k < 9 && !next_to_mountain; k++) {
				const int32_t nx = x + move_x[k], ny = y + move_y[k];
				if (nx < 0 || ny < 0 || nx >= w.tile_xs || ny >= w.tile_ys) continue;
				const uint16_t m = w.t(nx, ny) & 3;
				if (m == 2 || m == 1) next_to_mountain = true;
			}
			if (next_to_mountain) continue;
			if (d.land != 0) continue;
			if ((w.t(x, y) & tmask::kResource) && find_good_at_wcoord(g, wx, wy) >= 0) continue;
			float nv = avg;
			if (y == y0 || y == y1 - 1 || x == x0 || x == x1 - 1) nv = (h(x, y) + avg) * 0.5f;
			h(x, y) = nv;
		}
	}
}

// Wall::init's last loop: Unit::update_z @606590 on every live domain-0 unit of the first `cells` wcoords of the spiral around (x, y):
// the z of the unit from the (changed) heights, and of each of its guys (0 for ships). The loop asks vtable +0x8 (is_active) first: the
// slot is Window::get_button (returns 0) in the Build vtable, so buildings in the cell lists are skipped.
void update_z_around(Game& g, int32_t x, int32_t y, int32_t cells) {
	World& w = g.world;
	const int32_t cwx = wcoord_of(x), cwy = wcoord_of(y);
	for (int32_t k = 0; k < cells; k++) {
		const int32_t wx = cwx + move_x[k], wy = cwy + move_y[k];
		if (!w.in_w(wx, wy)) continue;
		int32_t o = w.w(wx, wy).down, who = w.w(wx, wy).down_who;
		for (int32_t guard = 0; o >= 0 && guard < 100000; guard++) {
			Object* ob = g.object(who, o);
			if (!ob) break;
			const int32_t no = ob->down, nw = ob->down_who;
			if (ob->is_unit && ob->alive() && ob->ptype && ob->ptype->domain == 0) {
				ob->z = g.world.in_t(tcoord_of(ob->x), tcoord_of(ob->y)) ? find_tcoord_z(g, tcoord_of(ob->x), tcoord_of(ob->y), 1) : 0;
				Unit& u = static_cast<Unit&>(*ob);
				for (Guy& gy : u.guys) guy_update_z(g, u, gy);
			}
			o = no;
			who = nw;
		}
	}
}

int32_t init_build_site(Game& g, int32_t who, int32_t type, int32_t x, int32_t y, bool preplaced, int32_t forced) {
	const int32_t o = find_free(g, who, kBuildBase, kBuildLimit, forced);
	if (o < 0) return o;
	int32_t sx = x, sy = y;
	snap_center_who(g, type, x, y, &sx, &sy, g.scenario_loading ? -1 : who);
	Build& b = *g.build(who, o);
	Leader& l = LD(g, who);
	// ---- Build::init (prologue)
	b.founder = int8_t(who);
	b.max_age = uint8_t(l.age);
	b.wonder = -1;
	b.fort = -1;
	b.orig_type = type;
	b.healing = 0;
	// ---- Wall::init -> Object::init -> SubObject::init (set_type INIT -> increment_stats on the inactive site)
	b.is_unit = false;
	b.who = int8_t(who);
	b.o = int16_t(o);
	b.flags = objflag::kValid;
	b.type = type;
	b.ptype = &g.type(type);
	build_update_gpiece(g, b);  // SubObject::init -> set_type(INIT) -> Wall::update_gpiece (founder and max_age are set above)
	if (is_city_type(*b.ptype)) b.flags |= objflag::kCity;
	b.city = -1;  // (Build.city is reset later by Build::init; increment_stats on an inactive site ignores it)
	wall_increment_stats(g, b);
	b.x = sx;
	b.y = sy;
	b.z = g.world.in_t(tcoord_of(sx), tcoord_of(sy)) ? find_tcoord_z(g, tcoord_of(sx), tcoord_of(sy), 1) : 0;
	b.damage = 0;
	b.damage_frac = 0;
	b.inside_down = -1;
	b.inside_down_who = int8_t(who);
	b.near_o = b.near_who = -1;
	b.targeted = 0;
	b.visible = 0;
	b.infiltrated = 0;
	b.uid = g.objects[size_t(who)].obj_ctr++;
	b.hold_frames = 0;
	if (b.ptype->obj_masks & 0x2000000) b.flags |= objflag::kDetector;
	b.up = b.down = -1;
	add_to_world(g, b);
	b.flags &= uint8_t(~(objflag::kStarted | objflag::kActive));
	b.build_masks = 0;
	b.ever_seen = 0;
	b.ever_seen_completed = 0;
	b.job_counter = b.job_counter_2 = 0;
	b.helpers = 0;
	b.demolition = 0;
	b.frame_started = -1;
	if (!g.scenario_loading) start_me(g.world, *b.ptype, b.x, b.y);  // (scenario grids come from the world loader)
	l.flags |= leaderflag::kRecompute;
	b.constr_time = int32_t(update_construct_time(g, who, type));
	wall_update_hits(g, b, false);
	build_update_los(g, b);
	check_ever_seen(g, b, false);
	// Wall::init @63e9b0, the rest: not preplaced -> tile_corner, TerrainOut::terraform_for_building (heights; not for a FARM),
	// Terrain::refresh_good_z (goods and items take their z from the heights when they are emitted),
	// Farms::recalc_heights (presentation: the farm's corner heights for drawing), SubObject::set_new_location (z); then, whatever the
	// flag, Unit::update_z on every domain-0 object in the 81 (a city) / 25 wcoords around the building.
	if (!preplaced) {
		if (!type_is(*b.ptype, ptype::kFarm)) {
			const Footprint f = footprint(*b.ptype, b.x, b.y);
			terraform_for_building(g, f.x0, f.y0, b.ptype->x_size, b.ptype->y_size);
		}
		b.z = g.world.in_t(tcoord_of(b.x), tcoord_of(b.y)) ? find_tcoord_z(g, tcoord_of(b.x), tcoord_of(b.y), 1) : 0;
		update_z_around(g, b.x, b.y, is_city_type(*b.ptype) ? 0x51 : 0x19);
	}
	// ---- Build::init (continued)
	build_clear_gather(g, b);
	if (!preplaced) l.buildings_built++;
	if (b.ptype->attack != 0) l.defense++;
	b.infiltrate = 0;
	b.recharging = 0;
	b.attack_whom = -1;
	b.queued = 0;
	b.gather_down = -1;
	b.gather_max = 0;
	b.city = -1;
	b.city_down = -1;
	b.attack_ox = -1;
	switch (build_stance_type(g, type)) {
	case kStanceCombat: b.stance = int8_t(l.opt_buildings); break;  // leader_options +0xc
	case kStanceWorker:
		b.stance = (!(l.flags & leaderflag::kHuman) && g.info.starting_resources == 8) ? 2 : int8_t(l.opt_peasants);  // leader_options +4
		break;
	case kStanceCaster: b.stance = 1; break;
	case kStancePacker: b.stance = int8_t(!((l.options_flags >> 3) & 1)); break;
	default: b.stance = 0; break;
	}
	if (!(b.flags & objflag::kCity)) {
		build_find_city(g, b);
	} else {
		g.unbuilt_cities[size_t(who)].push_back({int16_t(o), int8_t(who)});
		if (!(l.flags & leaderflag::kHuman) && l.city_mark == 0) {
			// The AI's first city: every live non-citizen, non-special unit moves next to the site.
			Group tmp;
			group_clear(tmp, -1);
			for (int32_t u = 0; u < g.objects[size_t(who)].unit_mark; u++) {
				const Unit* un = g.unit(who, u);
				if (!un || !un->alive()) continue;
				if (un->type >= 50 && un->type <= 53) continue;
				if (un->ptype->unit_flags2 & 0x10) continue;
				group_add(g, tmp, u, who, 0, 0);
			}
			if (tmp.num != 0) {
				const int32_t s = groups_push_group(g, who, tmp, 1);
				if (s >= 0) {
					int32_t mx = b.x + 0x900, my = b.y + 0x900;
					uint32_t ang = 0x60000000u;
					if (mx < 0 || my < 0 || mx >= g.world.xs * 768 || my >= g.world.ys * 768) {
						mx = b.x - 0x900;
						my = b.y - 0x900;
						ang = 0xe0000000u;
					}
					group_action_move_near(g, g.groups.g[size_t(s)], mx, my, 0, kQueueNew, true, ang, kAttackTo, 0, -1, -1, 0);
				}
			}
		}
	}
	if (type_is(*b.ptype, ptype::kFortX)) g.unbuilt_forts[size_t(who)].push_back({int16_t(o), int8_t(who)});
	if (is_wonder_type(type)) g.unbuilt_wonders[size_t(who)].push_back({int16_t(o), int8_t(who)});
	const int32_t qn = (is_military_trainer(g, type) || is_training_building(g, type)) ? 20
	                   : (b.ptype->build_flags & 0x8000000)                        ? 10
	                                                                               : 2;
	b.build_queue.init(qn);
	// FARM and not preplaced: Farms::add (farms-spec 2.1; the pen / ambience draws) gives the farm slot (+0x78).
	if (type_is(*b.ptype, ptype::kFarm) && !preplaced) b.fort = int16_t(farms_add(g, b));
	b.mtn = -1;
	b.gather_from.clear();
	if (is_gather_type(*b.ptype)) {
		if (!is_flat_gather(*b.ptype) && !type_is(*b.ptype, ptype::kUniversity)) {
			b.mtn = -1;
			find_gather_tiles(g, b);
		} else {
			const Footprint f = footprint(*b.ptype, b.x, b.y);
			b.gather_max = int8_t(max_gatherers(g, type, o, who, f.x0, f.y0));
		}
	}
	// ObjectData::can_carry(AIR): buildings holding aircraft (airbases and missile silos).
	if (can_carry_air(b)) b.build_masks |= bmask::kHoldsAir | bmask::kRepeatAir;
	return o;
}

// ---------------------------------------------------------------- starting (4.4)

// Wall::start_me(0) @63d790: per footprint tcoord, STARTED is cleared when STARTED2 is not set; with STARTED2 both
// are cleared only when no other building covers the tcoord.
void wall_start_me_off(Game& g, Build& b) {
	const Footprint f = footprint(*b.ptype, b.x, b.y);
	for (int32_t x = f.x0; x < f.x0 + f.w; x++) {
		for (int32_t y = f.y0; y < f.y0 + f.h; y++) {
			if (!g.world.in_t(x, y)) continue;
			uint16_t& m = g.world.t(x, y);
			if (!(m & tmask::kStarted2)) {
				m &= uint16_t(~tmask::kStarted);
			} else if (find_building_placed_at(g, x, y, -1, b.o, b.who, nullptr) < 0) {
				m &= uint16_t(~(tmask::kStarted | tmask::kStarted2));
			}
		}
	}
}

// GameEvent 11 (wonder started) / 12 (wonder completed): the tool log the HUD's messages and banners read (ui-port 11.2).
static void wonder_event(Game& g, const Build& b, int32_t kind) {
	GameEvent e;
	e.frame = g.frame, e.kind = kind, e.who = b.who, e.o = b.o, e.value = b.type, e.x = b.x, e.y = b.y;
	g.events.push_back(e);
}

void build_start(Game& g, Build& b, bool msg) {
	(void)msg;
	b.flags |= objflag::kStarted;
	b.frame_started = g.frame;
	// Wall::kill_competing_buildings @63d3a0.
	if (!g.scenario_loading) {
		const Footprint f = footprint(*b.ptype, b.x, b.y);
		for (int32_t x = f.x0; x < f.x0 + f.w; x++) {
			for (int32_t y = f.y0; y < f.y0 + f.h; y++) {
				if (!g.world.in_t(x, y) || !(g.world.t(x, y) & tmask::kStarted2)) continue;
				for (int guard = 0; guard < 100; guard++) {
					int32_t sw = -1;
					const int32_t s = find_building_placed_at(g, x, y, -1, b.o, b.who, &sw);
					if (s < 0) break;
					Build* sb = g.build(sw, s);
					if (sb->flags & objflag::kStarted) break;
					build_disband(g, *sb, true);
				}
			}
		}
	}
	// Terrain::object_placed (presentation). mask_me(1, REGEN_FORCE), the fog marks and mark_behind_tiles: the world
	// loader applied them already for scenario buildings; the CITY bits always here.
	if (!g.scenario_loading) {
		std::pair<Game*, Build*> ctx{&g, &b};
		wall_start(g.world, *b.ptype, b.x, b.y, b.who,
		           [](void* c) {
			           auto* p = static_cast<std::pair<Game*, Build*>*>(c);
			           place_roads_regen(*p->first, *p->second, true, Regen::Force);  // mask_me(1, REGEN_FORCE): the road to the city
		           },
		           &ctx, b.city >= 0);  // the ring of BuildType::place_roads needs Build +0x72 >= 0
	}
	if (b.flags & objflag::kCity) wall_mask_city(g, b, true, b.city < 0 ? get_radius(g, b.who, b.type) : city_get_radius(g, b.who, b.city));
	check_ever_seen(g, b, false);
	// Build::start: wonders become visible to everyone (update_local_seen).
	if (is_wonder_type(b.type)) object_update_local_seen(g, b);
	if (is_wonder_type(b.type) && !g.scenario_loading) wonder_event(g, b, 11);
}

// ---------------------------------------------------------------- activation (4.6, 5.3)

// Build::activate @623e20, the free units of a new high-water mark (production-spec 4.6 step 8, from the decompile and
// the asm of the Aztec count). Every unit comes through Build::train (Guy::init_real draws, come_out). "Room" = the
// base type's pop cost fits under pop_cap; check_population(base) == 0 = not capped. Lines resolve through the tribe
// graft and, for most, current_upgrade. The game's patch version is taken as >= 4 (the Extended Edition).
void give_free_units(Game& g, Build& b) {
	const int32_t who = b.who;
	Leader& l = LD(g, who);
	const int32_t t = b.type;
	auto line = [&](int32_t base, bool upgrade) {
		const int32_t u = get_graft(g, who, base);
		return upgrade ? current_upgrade(g, who, u) : u;
	};
	auto room = [&](int32_t base) { return g.type(base).control_cost + l.control <= l.pop_cap; };
	auto capped = [&](int32_t base) { return check_population(g, who, base) != 0; };
	// level = min(ages, military epoch) (LeaderDataEncrypt +0xdc / +0xe8)
	const int32_t level = std::min(l.age, l.epoch[0]);
	auto by_age = [&](int32_t per, int32_t mx, int32_t mult) {
		int32_t n = per * mult;
		if (n < 0 || mx < 0) return 0;
		return n > mx ? mx : n;
	};
	if (t == ptype::kMarket) {
		if (tb(g, who, 4) && K("nubian_free_caravan") != 0 && !capped(ptype::kCara)) build_train(g, b, ptype::kCara);
	} else if (t == ptype::kBarracks) {
		if (tb(g, who, 0x12)) {
			for (int32_t k = K("iroquois_free_scout"); k > 0; k--)
				if (room(ptype::kScout)) build_train(g, b, line(ptype::kScout, true));
		}
		if (tb(g, who, 6)) {
			int32_t n = K("roman_barracks_legion");
			const int32_t mx = K("roman_max_legion");
			if (mx != 0) {
				const int32_t c = level >= K("roman_age_for_3_legions")   ? 3
				                  : level >= K("roman_age_for_2_legions") ? 2
				                  : level >= K("roman_age_for_1_legion")  ? 1
				                                                          : 0;
				n = by_age(K("roman_barracks_legion"), mx, c);
			}
			for (; n > 0; n--)
				if (room(ptype::kHoplites)) build_train(g, b, line(ptype::kHoplites, true));
		}
		if (tb(g, who, 0xb)) {
			const int32_t c = level >= K("british_age_for_3_archers")   ? 3
			                  : level >= K("british_age_for_2_archers") ? 2
			                  : level >= K("british_age_for_1_archer")  ? 1
			                                                            : 0;
			for (int32_t n = c; n > 0; n--)
				if (room(ptype::kBowmen)) build_train(g, b, line(ptype::kBowmen, true));
		}
		if (tb(g, who, 0)) {
			int32_t n = K("aztec_barracks_light");
			const int32_t mx = K("aztec_max_light");
			if (mx != 0) {
				const int32_t c = level == 0 ? 1 : (uint32_t(level - 1) > 1u ? 3 : 2);
				n = by_age(K("aztec_barracks_light"), mx, c);
			}
			for (; n > 0; n--)
				if (room(ptype::kSlingers)) build_train(g, b, line(ptype::kSlingers, true));
		}
	} else if (t == ptype::kSiegeFactory || t == ptype::kFactory) {
		if (tb(g, who, 8)) {
			for (int32_t k = K("turk_free_siege"); k > 0; k--)
				if (!capped(kCatapult)) build_train(g, b, line(kCatapult, true));
		}
		if (tb(g, who, 10) && K("french_free_supply") != 0 && !capped(kSupplyWagon)) build_train(g, b, kSupplyWagon);
	} else if (t == ptype::kDock || t == ptype::kAnchorage || t == ptype::kShipyard) {
		if (tb(g, who, 9) && K("spanish_free_trireme") != 0 && l.age < 5) {
			for (int32_t k = K("spanish_free_trireme"); k > 0; k--)
				if (!capped(kTrireme)) build_train(g, b, line(kTrireme, true));
		}
		if (tb(g, who, 0x16)) {
			for (int32_t k = K("dutch_free_light_ship"); k > 0; k--)
				if (!capped(ptype::kBark)) build_train(g, b, line(ptype::kBark, true));
		}
		if (tb(g, who, 0xb)) {
			for (int32_t k = K("british_free_fishermen"); k > 0; k--)
				if (!capped(kFishermen)) build_train(g, b, line(kFishermen, true));
		}
	} else if (t == ptype::kUniversity) {
		if (tb(g, who, 0x14)) {
			for (int32_t k = K("americans_free_scholar"); k > 0; k--)
				if (!capped(ptype::kScholars)) build_train(g, b, line(ptype::kScholars, false));
		}
	} else if (!type_is(*b.ptype, ptype::kFortX)) {
		if (t == ptype::kAirbase) {
			if (tb(g, who, 0xc)) {
				for (int32_t k = K("german_free_fighter"); k > 0; k--)
					if (!capped(kBiplane)) build_train(g, b, line(kBiplane, true));
			}
			if (tb(g, who, 0x14) && has_tech(g, who, ptype::kModernAge)) {
				for (int32_t k = K("americans_free_bomber"); k > 0; k--)
					if (!capped(ptype::kBomber)) build_train(g, b, line(ptype::kBomber, true));
			}
		} else if (!(b.flags & objflag::kCity)) {
			if ((t == ptype::kStable || t == ptype::kAutoPlant) && tb(g, who, 0x11)) {
				const int32_t e = l.epoch[0];
				int32_t n = e < 2 ? K("mongol_start_cavalry") : K("mongol_free_cavalry");
				if (e > 2 && n < K("mongol_three_mil_cavalry")) n = K("mongol_three_mil_cavalry");
				const int32_t u = line(kHorseArchers, true);
				if (u >= 0)
					for (; n > 0; n--)
						if (!capped(kHorseArchers)) build_train(g, b, u);
			}
		} else if (tb(g, who, 0x10)) {
			const int32_t i = std::clamp(l.city_num - 1, 0, 7);
			const int off = constant_offset("korean_citizens");
			for (int32_t k = off < 0 ? 0 : g.rules->constants.at(uint32_t(off + 4 * i)); k > 0; k--) {
				const int32_t u = line(ptype::kPeasants, false);
				if (!capped(u)) build_train(g, b, u);
			}
		}
	} else if (tb(g, who, 10) && K("french_free_general") != 0 && !capped(ptype::kGeneral)) {
		build_train(g, b, ptype::kGeneral);
	}
}

// Build::do_bonus(good, amount) @627140: LeaderData::bucket_add of the completion bonus (the German tribe bonus scales it; the
// floating text is presentation).
static void build_do_bonus(Game& g, const Build& b, int32_t good, int32_t amount) {
	if (tb(g, b.who, 0xc)) amount = ((K("german_completion_bonus") + 100) * amount) / 100;
	LD(g, b.who).bucket[size_t(good)] += amount;
}

// The guard of Build::activate's two do_bonus calls: not the first frame, not a load, `count`, and (is_scenario implies
// building_resource_bonus).
static bool completion_bonus_allowed(const Game& g, bool loading, bool count) {
	return g.frame != 0 && !loading && count && (!g.scenario_mode || g.building_resource_bonus);
}

void build_activate_full(Game& g, Build& b, bool loading, bool msg, bool count) {
	const int32_t who = b.who;
	Leader& l = LD(g, who);
	if (type_is(*b.ptype, ptype::kLibrary)) build_new_library(g, b);
	b.recharging = 0;
	if (b.flags & objflag::kCity) {
		l.city_num++;
		if (tb(g, who, 14) && !loading && K("chinese_large_cities") != 0 && !type_is(*b.ptype, ptype::kForbiddenCity)) {
			wall_set_type(g, b, ptype::kTown, 0);
			wall_mask_me(g, b, true, Regen::None);
			l.flags |= leaderflag::kCalcWallStats;
		}
		l.city_num--;
	}
	if (!loading && count) l.last_building_finished[size_t(b.type - kBuilds)] = b.o;
	// ---- Wall::activate @63e4b0
	b.ever_seen_completed = uint8_t(b.ever_seen_completed | l.ally_mask);
	if (!(b.flags & objflag::kStarted)) build_start(g, b, msg);
	b.flags |= objflag::kActive;
	b.build_masks |= bmask::kActivated;
	if (is_wonder_type(b.type) && !loading && !g.scenario_loading) wonder_event(g, b, 12);
	b.job_counter = b.job_counter_2 = 0;
	if (type_is(*b.ptype, ptype::kAirDefense)) b.job_counter = 0x40000000;
	if ((b.ptype->build_flags & 0x10) || b.city >= 0) wall_increment_stats(g, b);
	if (l.num_queued[size_t(b.type)] != 0) l.num_queued[size_t(b.type)]--;
	l.flags |= leaderflag::kRecompute | leaderflag::kCalcWallStats;
	// (plain walls stamp here; buildings at the end) Doober::remove_cover_doober: presentation.
	// ---- Build::activate (continued)
	if (!(b.flags & objflag::kCity)) {
		const int32_t t = b.type;
		const bool in_city = b.city >= 0;
		City* ct = in_city ? &g.cities[size_t(who)][size_t(b.city)] : nullptr;
		if (is_t(g, t, ptype::kGranary)) {
			if (ct) ct->city_flags |= cityflag::kHasGranary;  // (the enhancer byte: City::calc_gather)
		} else if (is_t(g, t, ptype::kLumberMill)) {
			l.flags |= leaderflag::kRecompute;
			if (ct) ct->city_flags |= cityflag::kHasLumber;  // (the enhancer byte: City::calc_gather)
		} else if (is_t(g, t, ptype::kSmelter) || is_t(g, t, ptype::kRefinery)) {
			l.flags |= leaderflag::kRecompute;
		} else if (is_t(g, t, ptype::kTemple)) {
			if (ct) {
				const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
				if (g.world.in_w(wx, wy) && g.world.w(wx, wy).region >= 0) regions_fix_all_borders(g);
				ct->city_flags |= cityflag::kHasTemple;
			}
		} else if (is_t(g, t, ptype::kMarket)) {
			if (ct) ct->city_flags |= cityflag::kHasMarket;
		} else if (is_t(g, t, ptype::kSenate)) {
			// The government hero spawn (Build::activate @623e20): train(get_gov_hero) when none is alive and
			// gov_hero_frame <= frame.
			if (!loading) build_try_train_gov_hero(g, b);
			if (in_city && !loading) {
				l.senates_built++;
				int32_t c = -1, w = -1;
				if (ct->race == who) {
					find_capital(g, who, &c, &w, -1, -1);
					if (w == who && c >= 0 && city_count_buildings(g, who, c, ptype::kSenate, 0, 1) == 0) {
						for (int32_t i = 0; i < l.city_mark && size_t(i) < g.cities[size_t(who)].size(); i++) {
							City& ci = g.cities[size_t(who)][size_t(i)];
							if (!ci.valid()) continue;
							if (!tb(g, who, 0x17)) {
								if (ci.city_flags & cityflag::kCapital) ci.city_flags &= uint16_t(~cityflag::kCapital);
							} else if ((ci.city_flags & 0x4010) == 0x10) {
								ci.city_flags &= uint16_t(~cityflag::kCapital);
							}
						}
						ct->city_flags |= cityflag::kCapital;
						ct->founder = int8_t(who);
						const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
						if (g.world.in_w(wx, wy) && g.world.w(wx, wy).region >= 0) regions_fix_all_borders(g);
					}
				}
			}
		} else {
			l.flags |= leaderflag::kRecompute;
		}
	} else {
		// ---- founding (5.3)
		unbuilt_remove(g.unbuilt_cities[size_t(who)], b.o, who);
		bool first;
		bool second_cap = false;
		if (g.editor) first = l.city_num == 0;
		else first = l.city_num == 0 && !(l.flags2 & 1);
		if (!first && tb(g, who, 0x17)) {
			int32_t c = -1, w = -1;
			find_capital(g, who, &c, &w, -1, -1);
			if (c >= 0 && w >= 0 && w == who) {
				int32_t c2 = -1, w2 = -1;
				find_capital(g, who, &c2, &w2, c, w);
				if (c2 < 0) second_cap = !(l.flags2 & 1);
			}
		}
		const int32_t idx = cities_init_city(g, who, b.o, loading, first);
		b.city = int16_t(idx);
		city_find_buildings(g, who, idx);
		if (count) l.cities_built++;
		City& ct = g.cities[size_t(who)][size_t(idx)];
		if (!loading) {
			ct.race = int8_t(who);
			ct.founder = int8_t(who);
		}
		l.city_num++;
		g.world_cities++;
		l.city_mine++;
		if (tb(g, who, 9)) l.flags |= leaderflag::kExploreMap;
		if (first) ct.city_flags |= cityflag::kCapital | cityflag::kOriginalCapital;
		else if (second_cap) ct.city_flags |= cityflag::kCapital;
		calc_pop_cap(g, who);
	}
	if (b.city >= 0) city_regen_roads(g, who, b.city);
	if (type_is(*b.ptype, ptype::kFortX)) {
		b.fort = int16_t(forts_init_fort(g, who, b.o));
		unbuilt_remove(g.unbuilt_forts[size_t(who)], b.o, who);
	} else if (type_is(*b.ptype, ptype::kDock)) {
		// Docks::init_dock: the dock record (+0x78). TODO(docks): dock records (transport, fishing) are not kept.
	}
	if (type_is(*b.ptype, ptype::kDock) || type_is(*b.ptype, ptype::kMarket) || type_is(*b.ptype, ptype::kTemple)) {
		l.gather_slots[2]++;
		if (l.gather_slots_high[2] < l.gather_slots[2]) {
			l.gather_slots_high[2] = l.gather_slots[2];
			if (completion_bonus_allowed(g, loading, count)) build_do_bonus(g, b, 2, 0x1e);  // Build::activate @624b42
		}
	}
	// Free units on a new high-water mark (4.6 step 8).
	{
		const int32_t to = b.ptype->to;
		const int32_t n = l.num_buildings[size_t(b.type - kBuilds)] +
		                  (to >= 0 ? get_buildings(g, who, to, l.num_buildings[size_t(to - kBuilds)]) : 0);
		const int32_t basic = basic_type(g, b.type);
		if (l.high_buildings[size_t(basic - kBuilds)] < n && !loading) {
			l.high_buildings[size_t(basic - kBuilds)] = int16_t(n);
			if (type_is(*b.ptype, ptype::kKremlin)) l.flags |= leaderflag::kKremlinSpy;
			if (count && (!g.scenario_mode || g.building_unit_bonus)) give_free_units(g, b);
		}
	}
	// Gather buildings: gather slots; farms get their pen animals (Farms::add_animals, economy-spec / farms-spec 6);
	// oil wells and platforms their OilWell record (economy-spec 7.4). The completion bonuses
	// (Build::do_bonus) are paid on a new high-water mark of the slots.
	if (is_gather_type(*b.ptype)) {
		static const int32_t kGood[] = {0, 1, 4, 3, 5, 5};
		const int32_t t = b.type;
		if (t >= ptype::kFarm && t <= ptype::kOilPlatform) {
			const int32_t gd = kGood[t - ptype::kFarm];
			l.gather_slots[size_t(gd)] += b.gather_max;
			if (l.gather_slots_high[size_t(gd)] < l.gather_slots[size_t(gd)]) {
				const int32_t delta = l.gather_slots[size_t(gd)] - l.gather_slots_high[size_t(gd)];
				l.gather_slots_high[size_t(gd)] = l.gather_slots[size_t(gd)];
				if (completion_bonus_allowed(g, loading, count)) {  // Build::activate @6259b5: the bonus per good
					switch (gd) {
					case 0: build_do_bonus(g, b, 0, K("food_bonus_for_farm")); break;
					case 1: build_do_bonus(g, b, 1, K("timber_bonus_per_wood_slot") * delta); break;
					case 3: build_do_bonus(g, b, 3, K("knowledge_bonus_for_university")); break;
					case 4: build_do_bonus(g, b, 4, K("metal_bonus_per_mine_slot") * delta); break;
					case 5: build_do_bonus(g, b, 5, K("oil_bonus_for_well")); break;
					default: break;
					}
				}
			}
			if (gd == 0) farms_add_animals(g, b.fort);
		}
	}
	if (type_is(*b.ptype, ptype::kOilWell) || type_is(*b.ptype, ptype::kOilPlatform)) b.fort = int16_t(oil_wells_init(g, b));
	if (b.ptype->attack != 0) l.defense++;
	// Wonders (5.13).
	if (is_wonder_type(b.type)) {
		unbuilt_remove(g.unbuilt_wonders[size_t(who)], b.o, who);
		l.wonders_built++;
		b.wonder = int16_t(wonders_init_wonder(g, who, b.o));
		for (int32_t w = 0; w < 8; w++) {
			if (!(LD(g, w).flags & leaderflag::kValid)) continue;
			for (int32_t o = kBuildBase; o < g.objects[size_t(w)].build_mark; o++) {
				Build* ob = g.build(w, o);
				if (!ob || !ob->alive() || (ob->flags & objflag::kActive) || ob->type != b.type) continue;
				if (ob == &b) continue;
				build_disband(g, *ob, true);
			}
		}
		if (b.type == ptype::kTerraCotta) b.recharging = int16_t(K("terra_cotta_free") * 15);
		if (b.type == ptype::kKremlin) {
			b.recharging = int16_t(loading ? K("kremlin_spy") : 1);
			l.flags |= leaderflag::kKremlinSpy;  // TODO: unless an own SPY with unit_masks 0x400 exists
		}
		if (b.type == ptype::kColossus) calc_pop_cap(g, who);
		if (b.type == ptype::kColosseum || b.type == ptype::kEiffel || b.type == ptype::kTikal) {
			const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
			if (g.world.in_w(wx, wy) && g.world.w(wx, wy).region >= 0) regions_fix_all_borders(g);
		}
		if (b.type == ptype::kSpaceProgram) l.flags |= leaderflag::kRevealEnemy;
		// TODO(spec 5.13.4): the free techs of Hanging Gardens / Colosseum / Liberty / Red Fort / Tikal and the
		// Forbidden City capital clock.
		l.flags |= leaderflag::kCalcUnitStats;
	}
	l.flags |= leaderflag::kCalcWallStats;
	if (type_is(*b.ptype, ptype::kDock)) check_transport(g, who);
	wall_update_hits(g, b, false);
	build_update_los(g, b);
	if (b.city >= 0 && !(b.flags & objflag::kCity)) city_check_upgrade(g, who, b.city);
	object_update_seen(g, b, false);
}

// ---------------------------------------------------------------- deletion (4.9)

void build_disband(Game& g, Build& b, bool full) {
	const int32_t who = b.who;
	if ((b.flags & objflag::kValid) && (b.flags & objflag::kActive) && who < 8)
		g.scen.last_razed[size_t(who)] = b.o;  // Object::disband: ScenarioData::last_razed
	// (An active gather building's gatherers drop their orders through Unit::do_gather once it is gone.)
	bool fullp = full;
	if (tb(g, who, 0x13) && K("lakota_raze_price") != 0) fullp = true;
	bool refund = false;
	int32_t ct = 0, done = 0;
	if (!fullp) {
		if (!(b.flags & objflag::kActive) && uint32_t(b.job_counter) < type_time(g, b.type, who)) {
			refund = true;
			ct = construct_time(g, b, 0);
			done = b.job_counter_2;
		}
	} else {
		refund = true;
	}
	const int32_t t = b.type, orig = b.orig_type;
	if (b.flags & objflag::kStarted) object_die(g, b, 1, -1, 0.0f);
	else {
		build_close(g, b, 0);
	}
	if (refund) {
		for (int32_t k = 0; k < 6; k++) {
			if (type_avail(g, who, k, 1) == 0) continue;
			if (!fullp) {
				if (ct != 0 && done < ct) {
					const int32_t c = get_cost(g, t, k, who, -1, -1, 0, 1, orig);
					g.leaders[size_t(who)].bucket[size_t(k)] += int32_t((float(c) * float(ct - done)) / float(ct));
				}
			} else {
				g.leaders[size_t(who)].bucket[size_t(k)] += get_cost(g, t, k, who, -1, -1, 0, 1, -1);
			}
		}
	}
}

// ---------------------------------------------------------------- closing (Build::close / Wall::close)

void build_close_production(Game& g, Build& b, int32_t dt) {
	const int32_t who = b.who;
	Leader& l = LD(g, who);
	// Scripted rules / CTW: ScenarioData::builds_destroyed[who][type] (building_type_destroyed).
	if ((g.scripted_rules || g.ctw) && who < 8 && b.type >= kBuilds && b.type < kEndBuilds)
		g.scen.builds_destroyed[size_t(who)][size_t(b.type - kBuilds)]++;
	if (b.ptype->attack != 0) l.defense--;
	const bool was_valid = (b.flags & objflag::kValid) != 0;
	const bool was_active = (b.flags & objflag::kActive) != 0;
	const bool was_city = (b.flags & objflag::kCity) != 0;
	if (was_valid) {
		if (!type_is(*b.ptype, ptype::kLibrary)) {
			build_clean_queue(g, b, !g.editor);
		}
		// TODO(spec 7.10): a closing first library hands its items to the next library (or refunds them).
		l.flags |= leaderflag::kRecompute;
		if (was_active) {
			if ((b.ptype->build_flags & 0x10) || b.city >= 0) wall_decrement_stats(g, b);
			if (is_gather_type(*b.ptype)) {
				static const int32_t kGood[] = {0, 1, 4, 3, 5, 5};
				if (b.type >= ptype::kFarm && b.type <= ptype::kOilPlatform)
					l.gather_slots[size_t(kGood[b.type - ptype::kFarm])] -= b.gather_max;
			}
			if (type_is(*b.ptype, ptype::kDock) || type_is(*b.ptype, ptype::kMarket) || type_is(*b.ptype, ptype::kTemple))
				l.gather_slots[2]--;
		}
		if (dt != 5) {
			const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
			if (g.world.in_w(wx, wy)) g.world.w(wx, wy).flags |= wflag::kDeadBuild;
		}
	}
	b.gather.clear();
	// ---- Wall::close @63f6e0
	const bool started = (b.flags & objflag::kStarted) != 0;
	if (started) mark_behind_tiles(g.world, *b.ptype, b.x, b.y, false);
	if (!was_active) {
		if (l.num_queued[size_t(b.type)] != 0) l.num_queued[size_t(b.type)]--;
	}
	if (!was_active) {
		// (Doober::remove_cover_doober: presentation)
	}
	wall_start_me_off(g, b);
	if (started && dt != 5) wall_mask_me(g, b, false, Regen::Simple);
	for (const auto& t : b.gather_from) g.world.set_mask_bit(t[0], t[1], tmask::kGathered, false);
	b.gather_from.clear();
	object_close(g, b, dt);
	if (was_city) b.flags |= objflag::kCity;
	// Build::close: the farm record, the oil-well record, and a farm's pen animals (gaia units that belong to it).
	if (type_is(*b.ptype, ptype::kFarm) && b.fort != -1) {
		farms_remove(g, b.fort);
		for (int32_t o = 0; o < g.objects[kGaiaWho].unit_mark; o++) {
			Unit* a = g.unit(kGaiaWho, o);
			if (a && a->alive() && a->doober < 0 && a->ptype->domain != 2 && a->animal_ox == b.o && a->animal_whom == who)
				unit_die(g, *a, 0);
		}
	}
	if ((type_is(*b.ptype, ptype::kOilWell) || type_is(*b.ptype, ptype::kOilPlatform)) && b.fort >= 0) {
		oil_wells_close(g, who, b.fort);
		b.fort = -1;
	}
	// ---- the city / fort / wonder records
	if (was_valid) {
		if (!was_city) {
			build_remove_from_city(g, b);
		} else {
			if (!was_active) unbuilt_remove(g.unbuilt_cities[size_t(who)], b.o, who);
			if (b.city >= 0) {
				city_close(g, who, b.city, -1);
				cities_close_city(g, who, b.city);
			}
			regions_fix_all_borders(g);
			for (int32_t w = 0; w < 8; w++) {
				if (!(LD(g, w).flags & leaderflag::kValid)) continue;
				for (int32_t o = kBuildBase; o < g.objects[size_t(w)].build_mark; o++) {
					Build* cb = g.build(w, o);
					if (cb && cb->alive() && (cb->flags & objflag::kActive) && (cb->flags & objflag::kCity))
						wall_mask_me(g, *cb, true, Regen::None);
				}
			}
			calc_pop_cap(g, who);
		}
		if (type_is(*b.ptype, ptype::kFortX)) {
			if (b.fort >= 0) forts_close_fort(g, who, b.fort);
			if (!was_active) unbuilt_remove(g.unbuilt_forts[size_t(who)], b.o, who);
		}
		if (type_is(*b.ptype, ptype::kTemple)) regions_fix_all_borders(g);
		if (is_wonder_type(b.type)) {
			if (b.wonder >= 0) {
				wonders_close_wonder(g, who, b.wonder);
				l.flags |= leaderflag::kCalcUnitStats;
				if (type_is(*b.ptype, ptype::kColossus)) calc_pop_cap(g, who);
				if (type_is(*b.ptype, ptype::kColosseum) || type_is(*b.ptype, ptype::kEiffel) ||
				    type_is(*b.ptype, ptype::kTikal))
					regions_fix_all_borders(g);
				b.wonder = -1;
			}
			if (!was_active) {
				unbuilt_remove(g.unbuilt_wonders[size_t(who)], b.o, who);
			} else if (type_is(*b.ptype, ptype::kSpaceProgram) && !has_preq(g, who, ptype::kRevealEnemyBonus)) {
				l.flags &= ~leaderflag::kRevealEnemy;
				update_all_seen(g);
			}
		}
		l.flags |= leaderflag::kCalcWallStats;
	}
	b.build_queue.close();
	b.queued = 0;
	b.city = -1;
}

}  // namespace ron::sim
