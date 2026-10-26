// Cities, forts and wonder records (docs/production-spec.md section 5): city types, radius and CITY bits, membership,
// founding (Cities::init_city / City::init), upgrades, capital lookup, closing, territory triggers, the wonder
// ownership test.
#include <algorithm>
#include <cmath>

#include "sim/aircraft.hpp"
#include "sim/guys.hpp"
#include "sim/heroes.hpp"
#include "sim/roads.hpp"
#include "sim/ai.hpp"
#include "sim/capture.hpp"
#include "sim/combat.hpp"
#include "sim/fog.hpp"
#include "sim/production_internal.hpp"
#include "sim/territory.hpp"

namespace ron::sim {

void repath(Game& g, Unit& u);  // Unit::repath @5e29b0 (sim/orders.hpp; cities.cpp stays free of that header's other declarations)

namespace {

Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }
const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }

// even_circle_init @006816d0: rings r = 1..64 of the half-tile lattice (coordinates +-1..+-(r+1), zero excluded) whose
// round-half-down single-precision distance is r, x outer, y inner; positive coordinates are shifted down by one.
struct EvenCircles {
	std::vector<int8_t> x, y;
	std::array<int32_t, 65> radius{};
};
const EvenCircles& even_circles() {
	static const EvenCircles c = [] {
		EvenCircles e;
		int32_t n = 0;
		for (int32_t r = 1; r <= 64; r++) {
			for (int32_t a = -r - 1; a <= r + 1; a++) {
				for (int32_t b = -r - 1; b <= r + 1; b++) {
					if (a == 0 || b == 0) continue;
					const float f = std::sqrt(float(b * b + a * a));
					int32_t d = int32_t(f);
					if (f - float(d) > 0.5f) d++;
					if (d != r) continue;
					e.x.push_back(int8_t(a < 1 ? a : a - 1));
					e.y.push_back(int8_t(b < 1 ? b : b - 1));
					n++;
					if (n > 0x3248) {
						for (int32_t k = r; k <= 64; k++) e.radius[size_t(k)] = n;
						return e;
					}
				}
			}
			e.radius[size_t(r)] = n;
		}
		e.radius[0] = 0;
		return e;
	}();
	return c;
}

// vector_dist on tcoord offsets (movement-spec 1).
int32_t tdist(int32_t ax, int32_t ay) {
	uint32_t hi = uint32_t(std::abs(ax)), lo = uint32_t(std::abs(ay));
	if (hi < lo) std::swap(hi, lo);
	if (hi == 0) return 0;
	if (lo >= 60000) return int32_t((lo + 2 * hi) >> 1);
	return int32_t(lo * lo / (2 * hi) + hi);
}

Build* city_building(Game& g, int32_t who, int32_t c) {
	if (c < 0 || size_t(c) >= g.cities[size_t(who)].size()) return nullptr;
	return g.build(who, g.cities[size_t(who)][size_t(c)].o);
}

}  // namespace

// ---------------------------------------------------------------- types, levels, radius

int32_t city_get_level(int32_t type) {
	return type == ptype::kTown ? 2 : (type == ptype::kMetropolis || type == ptype::kForbiddenCity) ? 3 : 1;
}
int32_t city_get_pop_value(int32_t type) {
	const int32_t l = city_get_level(type);
	return l == 1 ? 1 : l == 2 ? 3 : 5;
}
int32_t city_get_radius(const Game& g, int32_t who, int32_t c) {
	const City& ct = g.cities[size_t(who)][size_t(c)];
	const Build* b = g.objects[size_t(who)].builds[size_t(ct.o - kBuildBase)].get();
	return std::min(get_radius(g, who, b ? b->type : ptype::kVillage), 64);
}

void wall_mask_city(Game& g, Build& b, bool add, int32_t radius) {
	if (!(b.flags & objflag::kCity)) return;
	const EvenCircles& e = even_circles();
	World& w = g.world;
	const int32_t cx = tcoord_of(b.x), cy = tcoord_of(b.y);
	const int32_t n = e.radius[size_t(std::clamp(radius, 0, 64))];
	for (int32_t i = 0; i < n; i++) {
		const int32_t x = e.x[size_t(i)] + cx, y = e.y[size_t(i)] + cy;
		if (!w.in_t(x, y)) continue;
		w.set_mask_bit(x, y, tmask::kCity, add);
	}
}

void wall_mask_me(Game& g, Build& b, bool add, Regen regen) {
	int32_t radius = 0;
	if (b.flags & objflag::kCity) radius = b.city < 0 ? get_radius(g, b.who, b.type) : city_get_radius(g, b.who, b.city);
	mask_me(g.world, *b.ptype, b.x, b.y, add, regen, b.city >= 0);
	place_roads_regen(g, b, add, regen);  // the REGEN_TOTAL / REGEN_FORCE tail of BuildType::place_roads
	if (b.flags & objflag::kCity) wall_mask_city(g, b, add, radius);
}

// ---------------------------------------------------------------- wonders

int32_t has_wonder_bits(const Game& g, int32_t who, int32_t type) {
	if (who < 0 || who >= kNumLeaders || !is_wonder_type(type)) return 0;
	const Leader& l = LD(g, who);
	int32_t v = 0;
	// TODO(ctw): conquest_wonders bit (type - 526) with epoch[1] >= ctw_wonder_min_civics -> 2.
	const auto& W = g.wonder_records[size_t(who)];
	for (int32_t i = 0; i < l.wonder_mark && size_t(i) < W.size(); i++) {
		const Wonder& wr = W[size_t(i)];
		if (!(wr.wonder_flags & 1)) continue;
		const Build* b = g.objects[size_t(who)].builds[size_t(wr.o - kBuildBase)].get();
		if (!b || b->type != type) continue;
		if (type == ptype::kRedFort || b->city >= 0) return v | 1;
	}
	return v;
}
bool has_wonder_prod(const Game& g, int32_t who, int32_t type) { return has_wonder_bits(g, who, type) != 0; }

// ---------------------------------------------------------------- capital

void find_capital(const Game& g, int32_t who, int32_t* c, int32_t* w, int32_t skip_c, int32_t skip_w) {
	const Leader& l = LD(g, who);
	const auto& cl = g.cities[size_t(who)];
	for (int32_t i = 0; i < l.city_mark && size_t(i) < cl.size(); i++) {
		const City& ct = cl[size_t(i)];
		if ((ct.city_flags & cityflag::kValid) && (ct.city_flags & cityflag::kCapital) && (i != skip_c || who != skip_w)) {
			*c = i;
			*w = who;
			return;
		}
	}
	for (int32_t p = 0; p < 8; p++) {
		if (p == who) continue;
		if ((l.flags & 3) != 3) continue;  // (the original tests this leader's own VALID|ACTIVE)
		const auto& pl = g.cities[size_t(p)];
		for (int32_t i = 0; i < LD(g, p).city_mark && size_t(i) < pl.size(); i++) {
			const City& ct = pl[size_t(i)];
			if ((ct.city_flags & 1) && (ct.was_capital_flags & (1u << (who & 31))) && (i != skip_c || p != skip_w)) {
				*c = i;
				*w = p;
				return;
			}
		}
	}
	*c = -1;
	*w = who;
}

// ---------------------------------------------------------------- city queries

int32_t city_count_buildings(const Game& g, int32_t who, int32_t c, int32_t type, int32_t exact, int32_t active_only) {
	if (c < 0 || size_t(c) >= g.cities[size_t(who)].size()) return 0;
	int32_t o = g.cities[size_t(who)][size_t(c)].o, n = 0;
	for (int guard = 0; o >= 0 && guard < 1000; guard++) {
		const Build* b = g.objects[size_t(who)].builds[size_t(o - kBuildBase)].get();
		if (!b) break;
		o = b->city_down;
		if (!b->alive()) continue;
		if (active_only && !(b->flags & objflag::kActive)) continue;
		if (exact ? b->type != type : !type_is(*b->ptype, type)) continue;
		n++;
	}
	return n;
}

int32_t city_num_wonders(const Game& g, int32_t who, int32_t c, int32_t p) {
	if (c < 0 || size_t(c) >= g.cities[size_t(who)].size()) return 0;
	int32_t o = g.cities[size_t(who)][size_t(c)].o, n = 0;
	for (int guard = 0; o >= 0 && guard < 1000; guard++) {
		const Build* b = g.objects[size_t(who)].builds[size_t(o - kBuildBase)].get();
		if (!b) break;
		o = b->city_down;
		if (!is_wonder_type(b->type) || type_is(*b->ptype, ptype::kRedFort)) continue;
		if (p == 0 || !(b->flags & objflag::kCity)) n++;
	}
	return n;
}

namespace {

// CityData::enough_kinds(n) @736540: distinct active building types in the city list (the city counts).
bool city_enough_kinds(const Game& g, int32_t who, int32_t c, int32_t n) {
	n = std::min(n, 24);
	std::vector<int32_t> kinds;
	int32_t o = g.cities[size_t(who)][size_t(c)].o;
	for (int guard = 0; o >= 0 && guard < 1000; guard++) {
		const Build* b = g.objects[size_t(who)].builds[size_t(o - kBuildBase)].get();
		if (!b) break;
		o = b->city_down;
		if (!b->alive() || !(b->flags & objflag::kActive)) continue;
		if (std::find(kinds.begin(), kinds.end(), b->type) != kinds.end()) continue;
		kinds.push_back(b->type);
		if (int32_t(kinds.size()) >= n) return true;
	}
	return false;
}

int32_t city_upgrades_to(int32_t type) {  // CityData::upgrades_to @7364f0
	if (type == ptype::kVillage) return ptype::kTown;
	if (type == ptype::kMetropolis || type == ptype::kForbiddenCity) return -1;
	return ptype::kMetropolis;
}

bool city_can_upgrade(const Game& g, int32_t who, int32_t c, int32_t t) {  // CityData::can_upgrade @7383c0
	if (t == ptype::kVillage) return true;
	if (t == ptype::kTown) return city_enough_kinds(g, who, c, K("city_buildings") + 1);
	return city_enough_kinds(g, who, c, K("metro_buildings") + 1);
}

bool city_ready_to_upgrade(const Game& g, int32_t who, int32_t c) {  // CityData::ready_to_upgrade @736480
	const Build* b = g.objects[size_t(who)].builds[size_t(g.cities[size_t(who)][size_t(c)].o - kBuildBase)].get();
	if (!b || b->type == ptype::kMetropolis || b->type == ptype::kForbiddenCity) return false;
	const int32_t t = city_upgrades_to(b->type);
	return t >= 0 && type_avail(g, who, t, 1) != 0 && city_can_upgrade(g, who, c, t);
}

// City::generate_name @735c90: the city-name counter update (the chosen string is file-only). TODO: the number of
// names in the tribe's <CITIES> list (counters beyond it fall back to "<tribe> City N" without the owner fix-up).
void generate_name_counter(Game& g, int32_t who, const Build& cb) {
	if (type_is(*cb.ptype, ptype::kForbiddenCity)) return;
	if (g.ctw && !(LD(g, who).tribe >= 0 && size_t(LD(g, who).tribe) < g.rules->tribes.size() &&
	               g.rules->tribes[size_t(LD(g, who).tribe)].barbarian != 0))
		return;  // conquest names
	const int32_t tribe = LD(g, who).tribe;
	int32_t best = -1, mx = -1;
	for (int32_t i = 0; i < 8; i++) {
		if (LD(g, i).tribe != tribe) continue;
		if (best < 0) best = i;
		if (mx < LD(g, i).city_name) {
			best = i;
			mx = LD(g, i).city_name;
		}
	}
	const int32_t old = LD(g, who).city_name;
	const int32_t target = mx > 0 ? best : who;
	const int32_t n = ++LD(g, target).city_name;
	if ((n < 2 || old == 0) && LD(g, who).city_name == 0) LD(g, who).city_name = 1;
}

void city_fix_world_vals(Game& g, int32_t who, const City& ct) {
	const int32_t wx = wcoord_of(ct.x), wy = wcoord_of(ct.y);
	const Build* b = g.objects[size_t(who)].builds[size_t(ct.o - kBuildBase)].get();
	const int32_t r = std::min(get_radius(g, who, b->type), 64);
	const int32_t rw = std::min((r + 3) / 4, 0x3d);
	const Circles& C = circles();
	World& w = g.world;
	for (int32_t k = 0; k < C.radius[size_t(rw + 3)]; k++) {
		const int32_t x = C.x[size_t(k)] + wx, y = C.y[size_t(k)] + wy;
		if (!w.in_w(x, y)) continue;
		uint8_t& v = w.w(x, y).val;
		v = k < C.radius[size_t(rw)] ? uint8_t(v >> 2) : uint8_t(v >> 1);
	}
}

}  // namespace

// Cities::init_city(who, o, loading, capital) @7352c0 -> City::init @737050. Returns the city index.
int32_t cities_init_city(Game& g, int32_t who, int32_t o, bool loading, bool capital) {
	Leader& l = LD(g, who);
	auto& cl = g.cities[size_t(who)];
	int32_t idx = -1;
	for (int32_t i = 0; i < l.city_mark && size_t(i) < cl.size(); i++) {
		if (!cl[size_t(i)].valid()) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		if (size_t(l.city_mark) < cl.size()) {
			idx = l.city_mark;
		} else {
			cl.emplace_back();
			idx = int32_t(cl.size()) - 1;
		}
	}
	l.city_mark = std::max(l.city_mark, idx + 1);
	City& ct = cl[size_t(idx)];
	const Build& b = *g.build(who, o);
	ct = City();
	ct.city = int16_t(idx);
	ct.o = int16_t(o);
	ct.who = int8_t(who);
	ct.pop = 1;
	const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
	ct.reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : 0;
	ct.x = b.x;
	ct.y = b.y;
	ct.city_flags = cityflag::kValid;
	const int32_t stamp = loading ? g.frame : 0;
	ct.attack_stamp = ct.raid_stamp = ct.capture_stamp = ct.assimilation_timer = ct.reduce_stamp = stamp;
	ct.conquest_node = -1;
	ct.race = 0;
	ct.founder = -1;
	const int32_t pv = city_get_pop_value(b.type);
	l.pop += pv;
	g.world_pop += pv;
	if (ct.reg < 64) {
		l.reg_pop[size_t(ct.reg)] = int16_t(l.reg_pop[size_t(ct.reg)] + pv);
		l.reg_cities[size_t(ct.reg)]++;
	}
	calc_pop_cap(g, who);
	for (int32_t r = 0; r < 64; r++) g.regions.r[size_t(r)].borders = 0;
	if (capital) ct.city_flags |= cityflag::kCapital | cityflag::kOriginalCapital;
	if (!loading) generate_name_counter(g, who, b);
	ct.vans.clear();
	city_fix_world_vals(g, who, ct);
	return idx;
}

// Cities::close_city @733310: invalidate, lower city_mark.
void cities_close_city(Game& g, int32_t who, int32_t c) {
	auto& cl = g.cities[size_t(who)];
	if (c < 0 || size_t(c) >= cl.size()) return;
	cl[size_t(c)].city_flags &= uint16_t(~cityflag::kValid);
	Leader& l = LD(g, who);
	while (l.city_mark > 0 && !cl[size_t(l.city_mark - 1)].valid()) l.city_mark--;
}

// ---------------------------------------------------------------- find_city_at, get_town

int32_t find_city_at(const Game& g, int32_t x, int32_t y, int32_t who, int32_t type, int32_t skip_check) {
	if (who < 0 || who >= 8 || !(LD(g, who).flags & leaderflag::kValid)) return -1;
	const World& w = g.world;
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	if (!w.in_t(tx, ty)) return -1;
	if (!skip_check) {
		// WorldData::is_city_at @6b3b80: CITY bit and the territory owner none / who / an ally.
		if (!(w.t(tx, ty) & tmask::kCity)) return -1;
		const int32_t own = w.w(tx >> 2, ty >> 2).who;
		if (!(own == -1 || own == who || (own >= 0 && is_ally(g, who, own)))) return -1;
	}
	int32_t best = -1, find_dist = 99999999;
	const auto& cl = g.cities[size_t(who)];
	for (int32_t i = 0; i < LD(g, who).city_mark && size_t(i) < cl.size(); i++) {
		const City& ct = cl[size_t(i)];
		if (!ct.valid()) continue;
		int32_t d = tdist(tcoord_of(ct.y) - ty, tcoord_of(ct.x) - tx);
		if (d > city_get_radius(g, who, i)) continue;
		if (type >= 0 && (g.type(type).build_flags & 0x200) && city_count_buildings(g, who, i, type, 0, 0) != 0) d += 100;
		if (d <= find_dist) {
			find_dist = d;
			best = i;
		}
	}
	return best;
}

int32_t get_town(const Game& g, int32_t type, int32_t tx, int32_t ty, int32_t who) {
	const Type& bt = g.type(type);
	const World& w = g.world;
	const int32_t cx = tx * 192 + 96, cy = ty * 192 + 96;
	const Footprint f = footprint(bt, cx, cy);
	for (int32_t x = f.x0; x < f.x0 + f.w; x++) {
		for (int32_t y = f.y0; y < f.y0 + f.h; y++) {
			if (!w.in_t(x, y)) continue;
			if (!(w.t(x, y) & tmask::kCity)) return -1;
		}
	}
	int32_t c;
	if (!(bt.build_flags & 0x10) && (bt.build_flags & 0x800)) {
		// ObjectsData::find_town_at @658190: only cities that are(TOWN). TODO(spec 5.2): exact tie rules (unused by
		// the stock data, which has no 'l' building).
		c = find_city_at(g, cx, cy, who, type, 0);
		if (c >= 0) {
			const Build* cb = g.objects[size_t(who)].builds[size_t(g.cities[size_t(who)][size_t(c)].o - kBuildBase)].get();
			if (!cb || !type_is(*cb->ptype, ptype::kTown)) c = -1;
		}
	} else {
		c = find_city_at(g, cx, cy, who, type, 0);
	}
	return c < 0 ? -1 : g.cities[size_t(who)][size_t(c)].o;
}

// ---------------------------------------------------------------- membership

void build_add_to_city(Game& g, Build& b, int32_t c) {
	auto& cl = g.cities[size_t(b.who)];
	if (c < 0 || size_t(c) >= cl.size() || !cl[size_t(c)].valid()) return;
	City& ct = cl[size_t(c)];
	b.city = int16_t(c);
	if (!(b.flags & objflag::kCity)) {
		Build* t = g.build(b.who, ct.o);
		for (int guard = 0; t && t->city_down >= 0 && guard < 1000; guard++) t = g.build(b.who, t->city_down);
		if (t && t != &b) t->city_down = b.o;
		b.city_down = -1;
	}
	const bool active = (b.flags & objflag::kActive) != 0;
	if (active && !(b.ptype->build_flags & 0x10)) wall_increment_stats(g, b);
	if (active) {
		if (type_is(*b.ptype, ptype::kTemple)) ct.city_flags |= cityflag::kHasTemple;
		else if (type_is(*b.ptype, ptype::kLumberMill)) ct.city_flags |= cityflag::kHasLumber;
		else if (type_is(*b.ptype, ptype::kGranary)) ct.city_flags |= cityflag::kHasGranary;
		else if (type_is(*b.ptype, ptype::kMarket)) ct.city_flags |= cityflag::kHasMarket;
	}
}

void build_remove_from_city(Game& g, Build& b) {
	if ((b.flags & objflag::kCity) || b.city < 0) return;
	auto& cl = g.cities[size_t(b.who)];
	const int32_t c = b.city;
	if (size_t(c) < cl.size()) {
		City& ct = cl[size_t(c)];
		Build* p = g.build(b.who, ct.o);
		for (int guard = 0; p && guard < 1000; guard++) {
			if (p->city_down == b.o) {
				p->city_down = b.city_down;
				break;
			}
			p = p->city_down >= 0 ? g.build(b.who, p->city_down) : nullptr;
		}
		// City::regen_roads(city): MAKE_ROAD on every building of the list.
		for (Build* q = g.build(b.who, ct.o); q; q = q->city_down >= 0 ? g.build(b.who, q->city_down) : nullptr) {
			q->build_masks |= bmask::kMakeRoad;
			if (q->city_down == q->o) break;
		}
		auto clear_if = [&](int32_t t, uint16_t f) {
			if (type_is(*b.ptype, t) && city_count_buildings(g, b.who, c, t, 0, 1) == 0) ct.city_flags &= uint16_t(~f);
		};
		clear_if(ptype::kTemple, cityflag::kHasTemple);
		clear_if(ptype::kLumberMill, cityflag::kHasLumber);
		clear_if(ptype::kGranary, cityflag::kHasGranary);
		clear_if(ptype::kMarket, cityflag::kHasMarket);
	}
	b.city = -1;
	b.city_down = -1;
	if ((b.flags & objflag::kActive) && !(b.ptype->build_flags & 0x10)) wall_decrement_stats(g, b);
}

void build_find_city(Game& g, Build& b) {
	if (b.flags & objflag::kCity) return;
	if (b.city >= 0) build_remove_from_city(g, b);
	const int32_t co = get_town(g, b.type, tcoord_of(b.x), tcoord_of(b.y), b.who);
	if (co < 0) return;
	const Build* cb = g.build(b.who, co);
	if (cb) build_add_to_city(g, b, cb->city);
}

void city_regen_roads(Game& g, int32_t who, int32_t c) {
	Build* q = city_building(g, who, c);
	for (int guard = 0; q && guard < 1000; guard++) {
		q->build_masks |= bmask::kMakeRoad;
		q = q->city_down >= 0 ? g.build(who, q->city_down) : nullptr;
	}
}

void city_find_buildings(Game& g, int32_t who, int32_t c) {
	City& ct = g.cities[size_t(who)][size_t(c)];
	Build* cb = g.build(who, ct.o);
	if (!cb) return;
	const int32_t r = std::min(get_radius(g, who, cb->type), 64);
	for (int32_t w = 0; w < 8; w++) {
		if (!(LD(g, w).flags & leaderflag::kValid)) continue;
		for (int32_t o = kBuildBase; o < g.objects[size_t(w)].build_mark; o++) {
			Build* b = g.build(w, o);
			if (!b || !b->alive() || (b->flags & objflag::kCity) || b->city >= 0) continue;
			if (w != who && (b->ptype->build_flags & 0x10)) continue;
			const int32_t d = tdist(tcoord_of(b->x) - tcoord_of(cb->x), tcoord_of(b->y) - tcoord_of(cb->y));
			if (d > r) continue;
			if (w == who) {
				if (type_is(*b->ptype, ptype::kLibrary)) build_new_library(g, *b);
				build_add_to_city(g, *b, c);
			} else {
				// A foreign orphan inside the radius changes sides (Build::swap_team, capture-spec 4.4); a failed copy
				// destroys it and ends the walk without check_upgrade.
				const int32_t n = build_swap_team(g, *b, who);
				if (n < 0) {
					object_die(g, *b, 0, -1, 0.0f);
					return;
				}
				Build& nb = *g.build(who, n);
				build_activate_full(g, nb, false, true, false);  // activate(0, 1, 0)
				build_close(g, *b, 0);
				wall_mask_me(g, nb, true, Regen::Total);
			}
		}
	}
	city_check_upgrade(g, who, c);
}

void city_check_upgrade(Game& g, int32_t who, int32_t c) {
	if (!city_ready_to_upgrade(g, who, c)) return;
	City& ct = g.cities[size_t(who)][size_t(c)];
	Build* cb = g.build(who, ct.o);
	const int32_t old = city_get_pop_value(cb->type);
	wall_set_type(g, *cb, city_upgrades_to(cb->type), 0);
	wall_mask_me(g, *cb, true, Regen::None);
	LD(g, who).flags |= leaderflag::kCalcWallStats;
	city_find_buildings(g, who, c);
	const int32_t d = city_get_pop_value(cb->type) - old;
	LD(g, who).pop += d;
	g.world_pop += d;
	const int32_t wx = wcoord_of(cb->x), wy = wcoord_of(cb->y);
	const int32_t reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : 64;
	if (reg < 64) LD(g, who).reg_pop[size_t(reg)] = int16_t(LD(g, who).reg_pop[size_t(reg)] + d);
	calc_pop_cap(g, who);
	regions_fix_all_borders(g);
}

void city_assimilate(Game& g, int32_t who, int32_t c) {
	City& ct = g.cities[size_t(who)][size_t(c)];
	ct.city_flags &= uint16_t(~cityflag::kGotCaptured);
	if (ct.race == who) return;
	ct.race = int8_t(who);
	const int32_t wx = wcoord_of(ct.x), wy = wcoord_of(ct.y);
	if (g.world.in_w(wx, wy) && g.world.w(wx, wy).region < 64) regions_fix_all_borders(g);
	Build* cb = g.build(who, ct.o);
	// The Chinese quirk (5.4): with chinese_large_cities the city object becomes a TOWN (a Major City is downgraded).
	if (cb && has_tribe_bonus(g, who, 14) && K("chinese_large_cities") != 0) wall_set_type(g, *cb, ptype::kTown, 0);
	LD(g, who).flags |= leaderflag::kRecompute;
	for (Build* b = cb ? (cb->city_down >= 0 ? g.build(who, cb->city_down) : nullptr) : nullptr; b;
	     b = b->city_down >= 0 ? g.build(who, b->city_down) : nullptr) {
		if ((b->flags & objflag::kActive) && type_is(*b->ptype, ptype::kLibrary)) {
			b->flags &= uint8_t(~objflag::kActive);
			build_new_library(g, *b);
			b->flags |= objflag::kActive;
		}
		// An active SENATE respawns the government hero as in Build::activate (City::assimilate @738e90).
		if ((b->flags & objflag::kActive) && type_is(*b->ptype, ptype::kSenate)) build_try_train_gov_hero(g, *b);
	}
}

// City::close @737550 (attacker = the capturing / destroying leader, -1).
void city_close(Game& g, int32_t who, int32_t c, int32_t attacker) {
	City& ct = g.cities[size_t(who)][size_t(c)];
	Leader& l = LD(g, who);
	Build* cb = g.build(who, ct.o);
	if (cb && (cb->flags & objflag::kCity)) {
		l.city_num--;
		g.world_cities--;
		l.city_mine--;
		l.lost_city_stamp = g.frame;
	}
	const int32_t pv = cb ? city_get_pop_value(cb->type) : 1;
	l.pop -= pv;
	g.world_pop -= pv;
	const int32_t wx = wcoord_of(ct.x), wy = wcoord_of(ct.y);
	const int32_t reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : 64;
	if (reg < 64) {
		l.reg_pop[size_t(reg)] = int16_t(l.reg_pop[size_t(reg)] - pv);
		l.reg_cities[size_t(reg)]--;
	}
	armies_city_closed(g, who, c, ct.o);
	ct.city_flags &= uint16_t(~cityflag::kValid);
	const bool was_capital = (ct.city_flags & cityflag::kCapital) != 0, own = ct.race == ct.who;
	leader_lost_a_city(g, who, was_capital, own, attacker);  // (may defeat the leader: capture-spec 6)
	int32_t o = g.cities[size_t(who)][size_t(c)].o;
	for (int guard = 0; o >= 0 && guard < 1000; guard++) {
		Build* b = g.build(who, o);
		if (!b) break;
		b->city = -1;
		if (type_is(*b->ptype, ptype::kLibrary) && b->queued) {
			// The library's research moves to the first library (overwriting its queue) or is refunded (7.10).
			const int32_t lib = get_first_library(g, who);
			Build* fl = lib >= 0 ? g.build(who, lib) : nullptr;
			if (!fl) {
				build_clean_queue(g, *b, true);
			} else {
				for (int32_t i = 0; i < b->queued && i < b->build_queue.queue_size && i < fl->build_queue.queue_size; i++)
					fl->build_queue.queue[size_t(i)] = b->build_queue.queue[size_t(i)];
				fl->queued = b->queued;
				b->queued = 0;
			}
		}
		const int32_t next = b->city_down;
		if (!(b->ptype->build_flags & 0x10)) wall_decrement_stats(g, *b);
		b->city_down = -1;
		if (b->alive() && !(b->flags & objflag::kCity)) build_find_city(g, *b);
		o = next;
	}
	calc_pop_cap(g, who);
	g.cities[size_t(who)][size_t(c)].o = -1;
	// City::close @737550 (end): every caravan route of the city whose record is in use and whose unit is alive repaths; the list is freed.
	for (const std::array<int16_t, 2>& link : g.cities[size_t(who)][size_t(c)].vans) {
		const int32_t cw = link[1], ci = link[0];
		if (cw < 0 || cw >= 8 || ci < 0 || size_t(ci) >= g.caravans[size_t(cw)].size()) continue;
		const CaravanRec& r = g.caravans[size_t(cw)][size_t(ci)];
		if (!(r.caravan_flags & 1)) continue;
		Unit* cu = g.unit(cw, r.o);
		if (cu && cu->alive()) repath(g, *cu);
	}
	g.cities[size_t(who)][size_t(c)].vans.clear();
}

// ---------------------------------------------------------------- territory

void regions_fix_all_borders(Game& g) {
	for (int32_t r = 0; r < 64; r++) g.regions.r[size_t(r)].borders = 0;
}

void check_borders(Game& g) {
	// GameDaemon::check_borders @732060: every land region whose cursor is behind gets compute_reg_territory (at most
	// 256 wcoords per frame over all regions, sim/territory.hpp); when the last region finishes, the leaders' totals.
	g.check_borders_budget = 0;
	int32_t any = 0;
	for (int32_t r = 0; r < 64; r++) {
		const Region& rg = g.regions.r[size_t(r)];
		if (rg.size == 0 || rg.borders >= rg.size) continue;
		any |= compute_reg_territory(g, r, 0);
		if (g.daemon.busy == 0) g.daemon.busy = 1;
	}
	if (!(any & 2)) return;
	if (!(any & 1)) {
		// (fog.flags |= 2: the fog renderer's refresh request; presentation.)
		if (g.daemon.busy == 0) g.daemon.busy = 1;
	}
	for (int32_t r = 0; r < 64; r++) {
		const Region& rg = g.regions.r[size_t(r)];
		if (rg.size != 0 && rg.borders < rg.size) return;
	}
	for (int32_t p = 0; p < 8; p++) {
		Leader& L = g.leaders[size_t(p)];
		if (!(L.flags & leaderflag::kValid)) continue;
		L.territory = 0;
		for (int32_t r = 0; r < 64; r++) {
			if (g.regions.r[size_t(r)].size != 0) L.territory += int32_t(L.reg_terr[size_t(r)]);
		}
		L.territory_high = std::max(L.territory_high, L.territory);
	}
}

// ---------------------------------------------------------------- forts and wonders

// Forts::init_fort @73ed70 -> Fort::init @73eac0.
int32_t forts_init_fort(Game& g, int32_t who, int32_t o) {
	Leader& l = LD(g, who);
	auto& fl = g.forts[size_t(who)];
	int32_t idx = -1;
	for (int32_t i = 0; i < l.fort_mark && size_t(i) < fl.size(); i++) {
		if (!(fl[size_t(i)].fort_flags & 1)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		if (size_t(l.fort_mark) < fl.size()) idx = l.fort_mark;
		else {
			fl.emplace_back();
			idx = int32_t(fl.size()) - 1;
		}
	}
	l.fort_mark = std::max(l.fort_mark, idx + 1);
	Fort& f = fl[size_t(idx)];
	const Build& b = *g.build(who, o);
	f.fort = int16_t(idx);
	f.o = int16_t(o);
	f.who = int8_t(who);
	f.fort_flags = 1;
	l.forts_built++;
	const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
	f.reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : -1;
	if (f.reg >= 0 && f.reg < 64) l.reg_forts[size_t(f.reg)]++;
	if (f.reg >= 0) regions_fix_all_borders(g);
	return idx;
}

void forts_close_fort(Game& g, int32_t who, int32_t idx) {
	auto& fl = g.forts[size_t(who)];
	if (idx < 0 || size_t(idx) >= fl.size()) return;
	Fort& f = fl[size_t(idx)];
	if (f.reg >= 0) regions_fix_all_borders(g);
	const Build* b = g.build(who, f.o);
	if (b && b->alive() && f.reg >= 0 && f.reg < 64) LD(g, who).reg_forts[size_t(f.reg)]--;
	f.fort_flags = 0;
	Leader& l = LD(g, who);
	while (l.fort_mark > 0 && !(fl[size_t(l.fort_mark - 1)].fort_flags & 1)) l.fort_mark--;
}

// Docks::init_dock @740fc0 -> Dock::init @740a80 (Build::activate, any dock-family building). The record takes the first free slot below the leader's
// dock_mark, else the allocated record at the mark, else a new one; `reg_docks[region]` counts the dock; and a seagull (gaia GULLBIRD, type 404) is
// created a tile and a half up-left of the building: its facing is one of seven directions drawn from game_random, and it strafes the dock.
int32_t docks_init_dock(Game& g, int32_t who, int32_t o) {
	constexpr int32_t kGullBird = 404;
	Leader& l = LD(g, who);
	auto& dl = g.docks[size_t(who)];
	int32_t idx = -1;
	for (int32_t i = 0; i < l.dock_mark && size_t(i) < dl.size(); i++) {
		if (!(dl[size_t(i)].dock_flags & 1)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		if (size_t(l.dock_mark) < dl.size()) idx = l.dock_mark;
		else {
			dl.emplace_back();
			idx = int32_t(dl.size()) - 1;
		}
	}
	l.dock_mark = std::max(l.dock_mark, idx + 1);
	Dock& d = dl[size_t(idx)];
	const Build* b = g.build(who, o);
	if (!b) return idx;
	d.dock = int16_t(idx);
	d.who = int8_t(who);
	d.o = int16_t(o);
	d.dock_flags = 1;
	const int32_t wx = wcoord_of(b->x), wy = wcoord_of(b->y);
	d.reg = g.world.in_w(wx, wy) ? g.world.w(wx, wy).region : 0;
	if (d.reg >= 0 && d.reg < 64) l.reg_docks[size_t(d.reg)]++;
	const int32_t gull = init_unit(g, kGaiaWho, kGullBird, b->x - 0xc0, b->y - 0xc0, -1, -1, -1);
	if (gull >= 0) {
		const uint32_t r = uint32_t(g.rnd_draw(7));
		if (Unit* gu = g.unit(kGaiaWho, gull)) {
			unit_set_angle(g, *gu, r * 0xaaaaaaau - 0x40000000u, false);
			add_strafe_order(g, *gu, o, who, -1, -1, 1, kQueueNew, false);
		}
	}
	d.gull_o = int16_t(gull);
	return idx;
}

// Docks::close_dock @740f50 -> Dock::close @7409f0 (Build::close of a dock-family building).
void docks_close_dock(Game& g, int32_t who, int32_t idx) {
	auto& dl = g.docks[size_t(who)];
	if (idx < 0 || size_t(idx) >= dl.size()) return;
	Dock& d = dl[size_t(idx)];
	const Build* b = d.o >= 0 ? g.build(who, d.o) : nullptr;
	if (b && b->alive() && d.reg >= 0 && d.reg < 64) LD(g, who).reg_docks[size_t(d.reg)]--;
	d.dock_flags &= uint8_t(~1);
	d.o = -1;
	d.reg = 0;
	const int32_t gull = d.gull_o;
	d.who = int8_t(-1);
	if (gull >= 0) {
		if (Unit* gu = g.unit(kGaiaWho, gull)) {
			if (gu->alive()) unit_close(g, *gu, 0, -1, 0.0f);
		}
	}
	d.gull_o = -1;
	Leader& l = LD(g, who);
	while (l.dock_mark > 0 && size_t(l.dock_mark) <= dl.size() && !(dl[size_t(l.dock_mark - 1)].dock_flags & 1)) l.dock_mark--;
}

// Wonders::init_wonder @73c860 (Wonder::get_timer @73c660 for the timer).
int32_t wonders_init_wonder(Game& g, int32_t who, int32_t o) {
	Leader& l = LD(g, who);
	auto& wl = g.wonder_records[size_t(who)];
	int32_t idx = -1;
	for (int32_t i = 0; i < l.wonder_mark && size_t(i) < wl.size(); i++) {
		if (!(wl[size_t(i)].wonder_flags & 1)) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		if (size_t(l.wonder_mark) < wl.size()) idx = l.wonder_mark;
		else {
			wl.emplace_back();
			idx = int32_t(wl.size()) - 1;
		}
	}
	l.wonder_mark = std::max(l.wonder_mark, idx + 1);
	Wonder& wr = wl[size_t(idx)];
	const Build& b = *g.build(who, o);
	wr.wonder = int16_t(idx);
	wr.who = int8_t(who);
	wr.stamp = g.frame;
	wr.o = int16_t(o);
	wr.wonder_flags = 1;
	const int32_t xs = g.world.xs;
	const int32_t wt0 = K("wonder_timer"), wa0 = K("wonder_age");
	const int32_t wt = wt0 >= 1 ? std::max(1, (35 + xs * wt0) / 70) : 0;
	const int32_t wa = wa0 >= 1 ? std::max(1, (35 + xs * wa0) / 70) : 0;
	wr.timer = wt + (542 - b.type) * wa;
	g.wonders[size_t(b.type - 526)] |= 1;
	l.wonders_held = std::max(l.wonders_held, get_wonders(g, who));
	return idx;
}

void wonders_close_wonder(Game& g, int32_t who, int32_t idx) {
	auto& wl = g.wonder_records[size_t(who)];
	if (idx < 0 || size_t(idx) >= wl.size()) return;
	wl[size_t(idx)].wonder_flags = 0;
	Leader& l = LD(g, who);
	while (l.wonder_mark > 0 && !(wl[size_t(l.wonder_mark - 1)].wonder_flags & 1)) l.wonder_mark--;
}

// UnbuiltCities::remove_unbuilt_city @73b810 (forts and wonders alike): the first entry with this o is overwritten by
// the last entry and the count drops (the list order changes).
void unbuilt_remove(std::vector<Unbuilt>& v, int32_t o, int32_t who) {
	(void)who;
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i].o == o) {
			v[i] = v.back();
			v.pop_back();
			return;
		}
	}
}

}  // namespace ron::sim
