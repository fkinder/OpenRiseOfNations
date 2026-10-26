// Fishermen, merchants and rares (docs/economy-spec.md 3.7, 4.7), caravans and trade routes (section 6).
#include <algorithm>

#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/production_internal.hpp"
#include "sim/roads.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"
#include "sim/transport.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using prod::tb;

namespace {

Leader& LD(Game& g, int32_t who) { return g.leaders[size_t(who)]; }

bool mutual_ally_or_self(const Game& g, int32_t a, int32_t b) { return a == b || is_ally(g, a, b); }

// ObjectTypeData::upgrade_level @661090 (UnitType vslot +0x124): how many types of the `upgrade` chain this type is.
int32_t upgrade_level(const Game& g, int32_t t) {
	int32_t lvl = 0;
	for (int32_t u = g.type(t).upgrade, guard = 0; u >= 0 && guard < 64; guard++) {
		if (!type_is(g.type(t), u)) return lvl;
		lvl++;
		u = g.type(u).upgrade;
	}
	return lvl;
}

// UnitType::gather_radius @61daa0: the rare / fish search radius in tiles (circle table index).
int32_t unit_gather_radius(const Game& g, int32_t t) {
	const bool mf = t == ptype::kMerchant || t == ptype::kMerchantDutch || t == etype::kFurTrapper ||
	                type_is(g.type(t), etype::kFishermen);
	return mf ? upgrade_level(g, t) * 4 + 4 : upgrade_level(g, t) + 2;
}

bool is_fisherman(const Game& g, const Unit& u) {
	(void)g;
	return type_is(*u.ptype, etype::kFishermen);
}

}  // namespace

bool unit_is_merchant(const Unit& u) {
	return u.type == ptype::kMerchant || u.type == ptype::kMerchantDutch || u.type == etype::kFurTrapper;
}
bool unit_is_rare_collector(const Game& g, const Unit& u) { return unit_is_merchant(u) || is_fisherman(g, u); }

// ---------------------------------------------------------------- UnitData::calc_gather (3.7)

int32_t unit_collect(Game& g, Unit& u, int32_t* out, std::array<uint8_t, 8>* owned, int32_t* collected, bool check_only,
                     int32_t px, int32_t py, int32_t p8_arg) {
	// Unit::do_gather(out, owned, collected, check_only, p5 = check_only) @5fce20 -> UnitData::calc_gather @609180
	// (callers pass p5 equal to check_only: 1 from think, 0 from Leader::calc_gather). calc_gather(.., p7 = check_only,
	// p8 = count the packed neighbours that are unpacking, x, y): good_merchant_spot @6068a0 passes (1, 1, tile corner).
	const bool p8 = p8_arg < 0 ? check_only : p8_arg != 0;
	if (px == -1) {  // (-1, -1): the unit's own position
		px = u.x;
		py = u.y;
	}
	int32_t crowded = 0;
	for (int32_t k = 0; k < 6; k++) out[k] = 0;
	u.rare = -1;
	auto finish = [&](int32_t r) {
		if (crowded) u.unit_masks |= umask_econ::kBadFish;
		else u.unit_masks &= ~umask_econ::kBadFish;
		return r;
	};
	if (unit_is_rare_collector(g, u) && (u.unit_masks & umask::kPacked) && !check_only) return finish(0);
	const int32_t R = unit_gather_radius(g, u.type);
	const int32_t tx0 = tcoord_of(px), ty0 = tcoord_of(py);
	const Circles& C = circles();
	const World& w = g.world;
	const bool merchant = unit_is_merchant(u);
	int32_t good = -1;
	bool in_territory = false;
	auto terrain_ok = [&](int32_t tx, int32_t ty) {
		const uint16_t m = w.t(tx, ty);
		return merchant ? (m & tmask::kMedium) != tmask::kWater : (m & tmask::kMedium) == tmask::kWater;
	};
	const int32_t rr = std::clamp(R, 0, 64);
	if (u.good_obj >= 0 && u.good_obj < C.radius[size_t(rr)]) {
		const int32_t tx = C.x[size_t(u.good_obj)] + tx0, ty = C.y[size_t(u.good_obj)] + ty0;
		if (w.in_t(tx, ty) && terrain_ok(tx, ty)) {
			if (w.t(tx, ty) & tmask::kResource) {
				const int32_t gi = find_good_at_wcoord(g, tx >> 2, ty >> 2);
				if (gi >= 0) {
					good = g.goods[size_t(gi)].type;
					const int32_t own = w.w(tx >> 2, ty >> 2).who;
					in_territory = own >= 0 && is_ally(g, u.who, own);
				}
			}
			if (good < 0) u.good_obj = -1;
		}
	}
	if (good < 0) {
		for (int32_t i = 0; i < C.radius[size_t(rr)]; i++) {
			const int32_t tx = C.x[size_t(i)] + tx0, ty = C.y[size_t(i)] + ty0;
			if (!w.in_t(tx, ty) || !terrain_ok(tx, ty) || !(w.t(tx, ty) & tmask::kResource)) continue;
			const int32_t gi = find_good_at_wcoord(g, tx >> 2, ty >> 2);
			if (gi < 0) continue;
			u.good_obj = int16_t(i);
			good = g.goods[size_t(gi)].type;
			const int32_t own = w.w(tx >> 2, ty >> 2).who;
			in_territory = own >= 0 && (own == u.who || (g.leaders[size_t(u.who)].diplos(own) == 2 &&
			                                             g.leaders[size_t(own)].diplos(u.who) == 2));
			break;
		}
		if (good < 0) return finish(0);
	}
	// TODO(spec): ObjectsData::find_good_at's filter arguments (who, 0, 0) are taken as "any live good".
	u.rare = good;
	if (owned) (*owned)[size_t((good - 6) >> 3)] = uint8_t((*owned)[size_t((good - 6) >> 3)] | (1u << ((good - 6) & 7)));
	if (collected) collected[good - 6]++;
	calc_rare(g, u.who, good, out, in_territory);
	// Crowding: own units of the same type within (their radius + mine) * 192, over the 5x5-ish W-cell circle.
	int32_t n = 0;
	const int32_t wx0 = wcoord_of(px), wy0 = wcoord_of(py);
	for (int32_t i = 0; i < C.radius[2]; i++) {
		const int32_t wx = C.x[size_t(i)] + wx0, wy = C.y[size_t(i)] + wy0;
		if (!w.in_w(wx, wy)) continue;
		int32_t o = w.w(wx, wy).down, ow = w.w(wx, wy).down_who;
		for (int guard = 0; o >= 0 && guard < 10000; guard++) {
			const Object* ob = g.object(ow, o);
			if (!ob) break;
			const int32_t no = ob->down, nw = ob->down_who;
			if (ow == u.who && ob->is_unit && ob->alive() && static_cast<const Unit*>(ob)->on_map() &&
			    type_is(*ob->ptype, u.type)) {
				const Unit& v = *static_cast<const Unit*>(ob);
				// Packed units are skipped (with p8: unless unpacking).
				const bool packed = (v.unit_masks & umask::kPacked) != 0;
				const bool skip = packed && !(p8 && cast_is_unpacking(v));
				if (!skip && o != u.o) {
					const int32_t d = vector_dist(iabs(v.x - px), iabs(v.y - py));
					if (d < (unit_gather_radius(g, v.type) + R) * 0xc0) n++;
				}
			}
			o = no;
			ow = nw;
		}
	}
	if (n == 0) return finish(1);
	for (int32_t k = 0; k < 6; k++) out[k] /= n + 1;
	if (check_only) return finish(0);
	crowded = 1;
	return finish(1);
}

// The unit loop of Leader::calc_gather (economy-spec 3.2): idle fishermen and merchants on the map.
int32_t leader_collect_units(Game& g, int32_t who, int32_t* out) {
	Leader& l = LD(g, who);
	int32_t tmp[6];
	for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
		Unit* u = g.unit(who, o);
		if (!u || !u->alive() || !u->on_map()) continue;  // ObjectData::is_map_unit
		if (!(is_fisherman(g, *u) || unit_is_merchant(*u)) || u->order_type() != kOrderNone) continue;
		unit_collect(g, *u, tmp, &l.rare_owned, l.rares_collected.data(), false);
		for (int32_t k = 0; k < 6; k++) out[k] += tmp[k];
	}
	return 0;
}

int32_t unpack_merchant(Game& g, Unit& u, int32_t mode) {
	// Unit::unpack_merchant @6038e0 (spells-spec 5, packers.cpp): find_merchant_spot, then the UNPACK cast and a move.
	return unpack_merchant_spot(g, u, mode);
}

int32_t think_fish(Game& g, Unit& u) {
	// Unit::think_fish @5f4c60 (AI-controlled fishermen): G8, one draw per scanned water W-cell of the region.
	if ((u.ptype->unit_flags2 & 4) && !(u.unit_masks & umask::kPacked)) {
		if ((u.o + g.frame) % 1024 != 0) return 0;
		int32_t tmp[6];
		if (unit_collect(g, u, tmp, nullptr, nullptr, true)) return 0;
	}
	const World& w = g.world;
	const int32_t region = w.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
	const int32_t uwx = wcoord_of(u.x), uwy = wcoord_of(u.y);
	int32_t best = 0, bi = 0;
	for (int32_t k = 0; k <= 0x120; k++) {
		const int32_t wx = move_x[size_t(k)] + uwx, wy = move_y[size_t(k)] + uwy;
		if (!w.in_w(wx, wy)) continue;
		const WData& d = w.w(wx, wy);
		if (d.flags & wflag::kHalfLand) continue;
		if (!(d.land == land::kOcean || d.land == land::kSandy) || d.region != region) continue;
		int32_t v;
		if (find_good_at_wcoord(g, wx, wy) < 0) {
			v = 0;
		} else {
			if (d.down >= 0 && d.down_who < 8 && d.down != u.o) continue;
			v = 1000000;
		}
		int32_t m = iabs(uwy - wy) + iabs(uwx - wx);
		if (m < 1) m = 1;
		v = v / m;
		v = v + int32_t(g.rng.get(0, 0xffff) % 60) + k;
		if (best < v) {
			best = v;
			bi = k;
		}
	}
	if (bi > 0x120) bi = int32_t(g.rng.get(0, 0xffff) % 0x79) + 0x19;  // (unreachable)
	const int32_t tx = move_x[size_t(bi)] + uwx, ty = move_y[size_t(bi)] + uwy;
	if (tx == uwx && ty == uwy) {
		// A packed packer boat (fishermen) casts UNPACK where it floats (spells-spec 5).
		if (!(u.ptype->unit_flags2 & 4) || !(u.unit_masks & umask::kPacked)) return 0;
		add_cast_order(g, u, -1, -1, -1, -1, 0x28c, kQueueFirst, false);
		return 1;
	}
	add_move_order(g, u, tx * 0x300 + 0x180, ty * 0x300 + 0x180, 1, false, kQueueNew, false, -1, -1);
	return 1;
}

int32_t think_merchant(Game& g, Unit& u) {
	// Unit::think_merchant @5f4740 (an unpacked merchant stays; a packed one tries to unpack, then goes to the best
	// rare of the leader's new_rares list).
	if (!(u.unit_masks & umask::kPacked) || unpack_merchant(g, u, 3)) return 1;
	Leader& l = LD(g, u.who);
	const World& w = g.world;
	const int32_t region = w.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
	const bool on_water = (w.t(tcoord_of(u.x), tcoord_of(u.y)) & tmask::kMedium) == tmask::kWater;
	int32_t best = -1, bi = -1, prio = 200;
	for (size_t i = 0; i < l.new_rares.size(); i++, prio -= 10) {
		const int32_t gi = l.new_rares[i];
		if (gi < 0 || size_t(gi) >= g.goods.size()) continue;
		const GoodObj& gd = g.goods[size_t(gi)];
		if (!gd.alive() || !((gd.ever_seen >> (u.who & 31)) & 1)) continue;
		const int32_t wx = wcoord_of(gd.x), wy = wcoord_of(gd.y);
		const int32_t own = w.w(wx, wy).who;
		if (own >= 0 && !is_ally(g, u.who, own)) continue;
		const bool g_water = (w.t(tcoord_of(gd.x), tcoord_of(gd.y)) & tmask::kMedium) == tmask::kWater;
		if (g_water != on_water) continue;
		const int32_t cx = wx * 0x300 + 0x180, cy = wy * 0x300 + 0x180;
		// TODO(spec): ObjectsData::find_unit / find_unit_ordered with FILTER_NOT_ME + FILTER_TYPE and the enemy
		// FILTER_COMBAT search, approximated by plain scans (distance to the unit, no radius terms).
		bool blocked = false;
		for (int32_t o = 0; o < g.objects[size_t(u.who)].unit_mark && !blocked; o++) {
			const Unit* v = g.unit(u.who, o);
			if (!v || !v->alive() || v->o == u.o || !type_is(*v->ptype, u.type)) continue;
			if (vector_dist(iabs(v->x - cx), iabs(v->y - cy)) <= 0x300) blocked = true;
			const OrderType ot = v->order_type();
			if (order_is_move(ot) && vector_dist(iabs(v->orders_x - cx), iabs(v->orders_y - cy)) <= 0x300) blocked = true;
		}
		for (int32_t p = 0; p < 8 && !blocked; p++) {
			if (!is_enemy(g, u.who, p)) continue;
			for (int32_t o = 0; o < g.objects[size_t(p)].unit_mark && !blocked; o++) {
				const Unit* v = g.unit(p, o);
				if (!v || !v->alive() || !(v->ptype->role & 1)) continue;
				if (vector_dist(iabs(v->x - cx), iabs(v->y - cy)) <= 0xc00) blocked = true;
			}
		}
		if (blocked) continue;
		int32_t s = prio;
		if (w.w(wx, wy).region == region) s += 100;
		const int32_t rc = (wy >> 1) * w.reg_xs + (wx >> 1);
		if (!w.danger.empty() && rc >= 0 && rc < w.reg_size) s -= w.danger[size_t(u.who * w.reg_size + rc)];
		s = std::max(1, s);
		if (best < s) {
			best = s;
			bi = gi;
		}
	}
	if (bi < 0) return 0;
	auto it = std::find(l.new_rares.begin(), l.new_rares.end(), bi);
	if (it != l.new_rares.end()) l.new_rares.erase(it);
	l.new_rares.push_back(bi);
	const GoodObj& gd = g.goods[size_t(bi)];
	add_move_order(g, u, gd.x, gd.y, 1, false, kQueueNew, false, -1, -1);
	return 1;
}

// ---------------------------------------------------------------- caravans (6)

int32_t caravans_init(Game& g, int32_t who, int32_t o) {
	// Caravans::init_caravan @73e1f0 + Caravan::init @73d130: the first unused record below the mark, else a new one. The record
	// keeps the allocation of its road stack (Stack.size) across uses.
	auto& v = g.caravans[size_t(who)];
	auto set_up = [&](CaravanRec& r, size_t i) {
		r.cara = int16_t(i);
		r.o = int16_t(o);
		r.who = int8_t(who);
		r.caravan_flags = 1;
		caravan_clear_temp_road(r);
		caravan_clear_road(r);
	};
	for (size_t i = 0; i < v.size(); i++) {
		if (!(v[i].caravan_flags & 1)) {
			set_up(v[i], i);
			LD(g, who).caravan_mark = std::max<int32_t>(LD(g, who).caravan_mark, int32_t(i) + 1);
			return int32_t(i);
		}
	}
	v.emplace_back();
	set_up(v.back(), v.size() - 1);
	LD(g, who).caravan_mark = int32_t(v.size());
	return int32_t(v.size()) - 1;
}

void caravans_close(Game& g, int32_t who, int32_t slot) {
	// Caravans::close_caravan @73e350 (Caravan::close): the road and the suspended search go with it.
	auto& v = g.caravans[size_t(who)];
	if (slot < 0 || size_t(slot) >= v.size()) return;
	CaravanRec& r = v[size_t(slot)];
	r.caravan_flags &= uint8_t(~1);
	r.who = -1;
	r.o = -1;
	r.city2 = r.whom = r.city3 = r.whose = -1;
	caravan_clear_temp_road(r);
	caravan_clear_road(r);
	while (!v.empty() && !(v.back().caravan_flags & 1)) v.pop_back();
	LD(g, who).caravan_mark = int32_t(v.size());
}

// ---------------------------------------------------------------- roads (economy-spec 6.4)

void caravan_clear_road(CaravanRec& r) { r.road.clear(); }  // Caravan::clear_road @73dfc0: pops everything, the allocation stays

void caravan_clear_temp_road(CaravanRec& r) {  // Caravan::clear_temp_road @73de80
	r.trees.reset();
	r.search_dir0 = r.search_gx = r.search_gy = r.search_count = 0;
	r.making_road = 0;
}

namespace {
void road_push(CaravanRec& r, const PathData& pd) {  // Stack<PathData>::push on the caravan's road (increase_size(-1) doubles)
	if (r.road_size <= int32_t(r.road.size())) r.road_size += r.road_size != 0 ? r.road_size : 1;
	r.road.push_back(pd);
}
}  // namespace

void caravans_reset_paths(Game& g) {
	// Caravans::reset_paths @73e060: every other caravan still searching starts its search over.
	for (int32_t p = 0; p < 8; p++) {
		if ((g.leaders[size_t(p)].flags & 3) != 3) continue;
		for (CaravanRec& r : g.caravans[size_t(p)]) {
			if ((r.caravan_flags & 1) && r.making_road != 0) r.reset_road = 1;
		}
	}
}

int32_t caravan_build_road(Game& g, CaravanRec& r) {
	// Caravan::build_road @73db10
	caravan_clear_road(r);
	auto city_of = [&](int32_t pl, int32_t c) -> const City* {
		if (pl < 0 || pl >= 8 || c < 0 || size_t(c) >= g.cities[size_t(pl)].size()) return nullptr;
		return &g.cities[size_t(pl)][size_t(c)];
	};
	const City* ca = r.city2 < 0 || r.city3 < 0 ? nullptr : city_of(r.whom, r.city2);
	const City* cb = ca ? city_of(r.whose, r.city3) : nullptr;
	if (!ca || !cb || !ca->valid() || !cb->valid()) {
		caravan_clear_temp_road(r);
		return 0;
	}
	int32_t res;
	bool restore = false;
	if (r.making_road != 0) {
		if (r.reset_road == 0 && r.trees) restore = true;
		else {
			caravan_clear_temp_road(r);
			r.reset_road = 0;
		}
	} else if (r.reset_road != 0 || !r.trees) {
		caravan_clear_temp_road(r);
		r.reset_road = 0;
	}
	res = find_road_caravan(g, r, ca->o, r.whom, cb->o, r.whose, restore);
	if (res < 1) {
		if (res < 0) return -1;
		r.road.clear();
		return 0;
	}
	// post-process: the road is popped from its top (the source end) into a temporary stack, laid as it goes, and pushed back
	std::vector<PathData> tmp;
	int32_t water = 0;
	while (!r.road.empty()) {
		PathData n = r.road.back();
		r.road.pop_back();
		const int32_t tx = n.x / 192, ty = n.y / 192;
		const uint16_t td = g.world.t(tx, ty);
		bool keep = true;
		if ((td & 0x30) == 0x20) {  // water: thinned
			if (water == 0 || r.road.size() < 5 || tmp.size() < 5) {
				water = 4;
				n.tol = 0x180;
			} else {
				water--;
				keep = false;
			}
		} else {
			g.world.set_road_at(tx, ty, true);
			n.flags |= 0x20;
			water = 0;
		}
		if (!keep) continue;
		if (tmp.empty() || r.road.empty()) n.flags |= 1;
		tmp.push_back(n);
	}
	while (!tmp.empty()) {
		road_push(r, tmp.back());
		tmp.pop_back();
	}
	r.reset_road = 0;
	r.making_road = 0;
	caravans_reset_paths(g);
	return 1;
}

void caravan_verify_road(Game& g, CaravanRec& r) {
	// Caravan::verify_road @73d950: a building (or a blocked tile) on the road rebuilds it.
	for (const PathData& n : r.road) {
		const uint16_t td = g.world.t(n.x / 192, n.y / 192);
		if ((td & 0x80) || (td & 3) == 3) {
			caravan_build_road(g, r);
			return;
		}
	}
}

void caravan_restart_trade_route(Game& g, CaravanRec& r) {
	// Caravan::restart_trade_route @73d070: clear the road and search again (trade_val is not recomputed).
	caravan_clear_road(r);
	caravan_clear_temp_road(r);
	r.caravan_flags &= uint8_t(~4);
	if (r.o >= 0 && r.who >= 0) {
		Unit* u = g.unit(r.who, r.o);
		if (u && u->alive()) {
			repath(g, *u);
			caravan_build_road(g, r);
		}
	}
}

void caravans_new_danger(Game& g, int32_t x, int32_t y, int32_t pl) {
	// Caravans::new_danger @73e0c0 (from Object::take_damage): every allied caravan whose road passes within 0x600 restarts.
	for (int32_t i = 0; i < 8; i++) {
		if ((g.leaders[size_t(i)].flags & 3) != 3) continue;
		if (pl != i && !(g.leaders[size_t(i)].diplos(pl) == 2 && g.leaders[size_t(pl)].diplos(i) == 2)) continue;
		for (size_t c = 0; c < g.caravans[size_t(i)].size() && int32_t(c) < LD(g, i).caravan_mark; c++) {
			CaravanRec& r = g.caravans[size_t(i)][c];
			if ((r.caravan_flags & 3) != 3) continue;
			bool restart = true;
			if (r.making_road == 0 && !r.road.empty()) {
				restart = false;
				for (const PathData& n : r.road) {
					if (vector_dist(iabs(n.x - x), iabs(n.y - y)) <= 0x600) {
						restart = true;
						break;
					}
				}
			}
			if (restart) caravan_restart_trade_route(g, r);
		}
	}
}

namespace {

// CityData::get_trade_value @7363f0
int32_t city_trade_value(const Game& g, int32_t who, int32_t c) {
	const City& ct = g.cities[size_t(who)][size_t(c)];
	int32_t nb = 0;
	for (int32_t o = ct.o, guard = 0; o >= 0 && guard < 1000; guard++) {
		const Build* b = g.build(who, o);
		if (!b) break;
		o = b->city_down;
		if (b->alive() && (b->flags & objflag::kActive)) nb++;
	}
	const Build* centre = g.build(who, ct.o);
	const int32_t t = centre ? centre->type : -1;
	if (t == ptype::kTown) return nb + 2;
	if (t == ptype::kMetropolis || t == ptype::kForbiddenCity) return nb + 4;
	return nb;
}

const CaravanRec* caravan_of(const Game& g, int32_t who, int32_t cara) {
	if (who < 0 || who >= 8 || cara < 0 || size_t(cara) >= g.caravans[size_t(who)].size()) return nullptr;
	return &g.caravans[size_t(who)][size_t(cara)];
}

}  // namespace

int32_t trade_value(const Game& g, int32_t ca, int32_t pa, int32_t cb, int32_t pb) {
	// Caravan::trade_value @73d9d0 (bonuses use pA); Caravan::distance @73d300 over the cities' W-cells.
	const City& A = g.cities[size_t(pa)][size_t(ca)];
	const City& B = g.cities[size_t(pb)][size_t(cb)];
	const int32_t D = vector_dist(iabs(wcoord_of(A.x) - wcoord_of(B.x)), iabs(wcoord_of(A.y) - wcoord_of(B.y)));
	const int32_t xs = g.world.xs;
	const int32_t d = D < xs / 4 ? 0 : D < xs / 2 ? 1 : ((xs * 4) / 5 <= D ? 1 : 0) + 2;
	int32_t v = city_trade_value(g, pa, ca) + city_trade_value(g, pb, cb);
	if (d != 0) v = ((d + 3) * v) / 3;
	if (pa != pb) v = (v * 3) / 2;
	if (tb(g, pa, 0x15)) v = ((K("indians_caravan") + 100) * v) / 100;
	if (has_rare(g, pa, etype::kSpice)) v = ((K("spice_caravan_income") + 100) * v) / 100;
	return v;
}

void city_compute_trade(Game& g, int32_t who, int32_t c) {
	// City::compute_trade @739640
	City& ct = g.cities[size_t(who)][size_t(c)];
	const int16_t old = ct.trade_val;
	ct.trade_val = 0;
	for (const auto& link : ct.vans) {
		const CaravanRec* r = caravan_of(g, link[1], link[0]);
		if (!r || !(r->caravan_flags & 4) || r->city2 < 0 || r->city3 < 0) continue;
		int32_t c1 = r->city2, p1 = r->whom;
		if (!(g.cities[size_t(p1)][size_t(c1)].city_flags & cityflag::kValid)) continue;
		if (!(g.cities[size_t(r->whose)][size_t(r->city3)].city_flags & cityflag::kValid)) continue;
		int32_t c2 = r->city3, p2 = r->whose;
		if (p1 == who) {
			std::swap(c1, c2);
			std::swap(p1, p2);
		}
		// (trade_value(param_1 = c1, param_2 = p1, ECX/EDX = the other end): "from the city's side")
		const int32_t v = trade_value(g, c2, p2, c1, p1);
		ct.trade_val = int16_t(ct.trade_val + int16_t((v * 16) / 2));
	}
	if (old != ct.trade_val) LD(g, ct.who).flags |= leaderflag::kRecompute;
}

void city_new_caravan(Game& g, int32_t who, int32_t c, int32_t pl, int32_t pc) {
	// City::new_caravan(pl, c) @739750: the first delivery from player pl's city pc pays a lump sum.
	City& ct = g.cities[size_t(who)][size_t(c)];
	const uint32_t bit = 1u << (uint32_t(pc) & 31);
	if (uint32_t(ct.traded_with[size_t(pl)]) & bit) return;
	ct.traded_with[size_t(pl)] = int32_t(uint32_t(ct.traded_with[size_t(pl)]) | bit);
	int32_t a = (LD(g, pl).epoch[2] + 1) * 10;
	if (pl != ct.who) a *= 2;
	LD(g, pl).bucket[2] += a;
}

int32_t city_get_empty_trade_routes(const Game& g, int32_t who, int32_t c, int32_t p, int32_t pc) {
	// CityData::get_empty_trade_routes(c2, p) @7395c0 on city (who, c): 1 - routes already joining (pc, p).
	const City& ct = g.cities[size_t(who)][size_t(c)];
	int32_t n = 1;
	for (const auto& link : ct.vans) {
		const CaravanRec* r = caravan_of(g, link[1], link[0]);
		if (!r || !(r->caravan_flags & 2)) continue;
		if ((r->city2 == pc && r->whom == p) || (r->city3 == pc && r->whose == p)) n--;
	}
	return n;
}

static void city_add_van(City& ct, int32_t cara, int32_t who) {
	// ArrayBase<CaravanLink>::add @46e550: a full array (length >= size) calls increase_size(increment -1) @489280: size += size, or 4 from an empty allocation.
	if (int32_t(ct.vans.size()) >= ct.vans_cap) ct.vans_cap += ct.vans_cap != 0 ? ct.vans_cap : 4;
	ct.vans.push_back({int16_t(cara), int16_t(who)});
}

void add_trade_order(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t oxx, int32_t whose, QueuePos q, bool expl) {
	// Unit::add_trade_order @5e4dc0
	if (q == kQueueNew) clear_orders(g, u);
	u.unit_masks &= ~0x200u;  // UNIT_TRADE_ROUTE
	Order o;
	o.type = kTradeRoute;
	o.ox = ox;
	o.whom = whom;
	const Object* a = (ox >= 0 && whom >= 0) ? g.object(whom, ox) : nullptr;
	o.uid = a ? a->uid : 0xffff;
	o.oxx = oxx;
	o.whose = whose;
	o.started = 0;
	o.loaded = 0;
	if (oxx >= 0 && whom >= 0) {  // (tests whom, sic)
		const Object* b = g.object(whose, oxx);
		o.uid2 = b ? b->uid : 0xffff;
	}
	if (expl) o.flags |= orderflag::kExplicit;
	u.orders.push_back(o);
	update_action(g, u);
}

void end_trade_route(Game& g, Unit& u) {
	// Unit::end_trade_route @5e3bd0: only while the game is being played; clears flags 2 | 4 and the road, then drops the van
	// links of the ORDER's two cities (not the record's: a route started nearer B swapped them) and recomputes both.
	if (g.scenario_loading) return;
	u.unit_masks &= ~0x200u;
	CaravanRec* r = (u.cara >= 0 && size_t(u.cara) < g.caravans[size_t(u.who)].size()) ? &g.caravans[size_t(u.who)][size_t(u.cara)] : nullptr;
	if (!r) return;
	r->caravan_flags &= uint8_t(~2);
	r->caravan_flags &= uint8_t(~4);
	caravan_clear_temp_road(*r);
	caravan_clear_road(*r);
	const Order* act = get_action(u);
	if (!act || act->type != kTradeRoute) return;
	Order* a = update_action(g, u);
	if (!a) return;
	a->loaded = 0;
	auto unlink = [&](int32_t o, int32_t p) {
		if (o < 0 || p < 0 || p >= 8) return;
		const Build* b = g.build(p, o);
		if (!b || !(b->flags & objflag::kActive) || b->city < 0 || size_t(b->city) >= g.cities[size_t(p)].size()) return;
		City& ct = g.cities[size_t(p)][size_t(b->city)];
		if (!ct.valid()) return;
		for (size_t i = 0; i < ct.vans.size(); i++)
			if (ct.vans[i][0] == u.cara && ct.vans[i][1] == u.who) {
				ct.vans.erase(ct.vans.begin() + std::ptrdiff_t(i));
				break;
			}
		city_compute_trade(g, p, b->city);
	};
	unlink(a->ox, a->whom);
	unlink(a->oxx, a->whose);
}

void caravan_work(Game& g, Unit& u) {
	// The caravan part of Unit::work @60d180: a route is kept only while its TRADE_ROUTE order is the action (and the current or
	// the next order); the suspended road search runs one slice per frame.
	if (u.cara < 0 || size_t(u.cara) >= g.caravans[size_t(u.who)].size() || !unit_is_caravan(u)) return;
	CaravanRec& r = g.caravans[size_t(u.who)][size_t(u.cara)];
	if (!(r.caravan_flags & 6)) return;
	const Order* cur = u.front();
	if (cur && cur->type == kCastSpell) return;
	const Order* act = get_action(u);
	if (!act || act->type != kTradeRoute) {
		end_trade_route(g, u);
		return;
	}
	if (r.making_road != 0) caravan_build_road(g, r);
	cur = u.front();
	if (!cur || cur->type != kTradeRoute) {
		// the order list's tail->prev: the order after the current one
		if (u.orders.size() < 2 || u.orders[1].type != kTradeRoute) end_trade_route(g, u);
	}
}

namespace {

// CityData::is_seen @739560: the city's centre building is seen by `by`.
bool city_is_seen(const Game& g, int32_t who, int32_t c, int32_t by) {
	const City& ct = g.cities[size_t(who)][size_t(c)];
	if (!ct.valid() || ct.o < 0 || ct.who < 0) return false;
	const Object* ob = g.object(ct.who, ct.o);
	return ob && object_is_seen(g, *ob, by, false);
}

// Unit::go_to_city @5f79c0 (-> Unit::go_to @5f7a50): a walk to the city centre unless the unit is within 0x480 of it.
void go_to_city(Game& g, Unit& u, int32_t c, int32_t pl, int32_t order) {
	const City& ct = g.cities[size_t(pl)][size_t(c)];
	const Object* ob = g.object(pl, ct.o);
	if (!ob) return;
	if (vector_dist(ob->x - u.x, ob->y - u.y) <= 0x480) return;
	int32_t x = ob->x, y = ob->y;
	if (find_nearby_spot(g, *u.ptype, x, y, &x, &y, 0x240, 0, 0, 0x55555555u, kFilterNotMe, u.o, u.who, 0, 1, -1, 0, -1) == 0) {
		Group tmp;
		group_clear(tmp, -1);
		group_add(g, tmp, u.o, u.who, 0, 0);
		const int32_t id = groups_push_group(g, u.who, tmp, 1);
		if (id >= 0) group_action_move_to(g, g.groups.g[size_t(id)], x, y, kQueueNew, false, 0, order, 0, -1, -1, 0);
	}
}

}  // namespace

int32_t think_caravan(Game& g, Unit& u, bool force) {
	// Unit::think_caravan @5f5650: the peasant threshold from leader option peasants_wait, then the nearest allied
	// city (last at the minimum distance) that has an empty route with some other city.
	if (!force) {
		int32_t T;
		if (u.unit_masks & umask::kAiControl) {
			T = 1;
		} else {
			switch (g.leaders[size_t(u.who)].opt_peasants_wait) {
			case 1: T = 7; break;
			case 2: T = 0xc; break;
			case 3: T = 0x11; break;
			case 4: T = 0x20; break;
			case 5: T = 0x3e; break;
			default: T = 2; break;
			}
		}
		if (u.idle < T) return 0;
		if (u.idle != T && (u.idle - 2) % 5 != 0) return 0;
	}
	int32_t bd = 99999999, bw = -1, bc = -1;
	for (int32_t p = 0; p < 8; p++) {
		if (!(g.leaders[size_t(p)].flags & leaderflag::kValid) || !mutual_ally_or_self(g, u.who, p)) continue;
		for (int32_t c = 0; c < g.leaders[size_t(p)].city_mark && size_t(c) < g.cities[size_t(p)].size(); c++) {
			const City& ct = g.cities[size_t(p)][size_t(c)];
			if (!ct.valid()) continue;
			const int32_t d = vector_dist(iabs(u.x - ct.x), iabs(u.y - ct.y));
			if (d > bd) continue;
			// FILTER_CAN_TRADE (Search::valid_filter entry 17, jump table @67e57c -> @67e204): the candidate is a city centre and
			// some other city, seen by the candidate's owner, has an empty route with it both ways (own, or an ally's when the
			// owner has FOREIGN_TRADE).
			bool ok = false;
			for (int32_t q = 0; q < 8 && !ok; q++) {
				if (!(g.leaders[size_t(q)].flags & leaderflag::kValid)) continue;
				if (q != p && !(is_ally(g, p, q) && has_preq(g, p, etype::kForeignTrade))) continue;
				for (int32_t e = 0; e < g.leaders[size_t(q)].city_mark && size_t(e) < g.cities[size_t(q)].size() && !ok; e++) {
					if (q == p && e == c) continue;
					if (!g.cities[size_t(q)][size_t(e)].valid()) continue;
					if (!city_is_seen(g, q, e, p)) continue;
					ok = city_get_empty_trade_routes(g, p, c, q, e) != 0 && city_get_empty_trade_routes(g, q, e, p, c) != 0;
				}
			}
			if (!ok) continue;
			bd = d;
			bw = p;
			bc = c;
		}
	}
	if (bc < 0) return 0;
	const City& ct = g.cities[size_t(bw)][size_t(bc)];
	add_trade_order(g, u, ct.o, ct.who, -1, -1, kQueueNew, false);
	return 1;
}

int32_t do_trade(Game& g, Unit& u, Order& ord) {
	// Unit::do_trade @5ed270 (economy-spec 6.3). The locals keep the original's names: the near / far swap of a route started
	// nearer B changes a_* / b_* but not the order's ox / oxx, so the arrival tests of that first frame read what the original reads.
	unit_set_anim(g, u, kCharDefault, 0, 1);
	if (u.cara < 0 || size_t(u.cara) >= g.caravans[size_t(u.who)].size()) return 0;
	CaravanRec& rec = g.caravans[size_t(u.who)][size_t(u.cara)];
	if (rec.making_road != 0) return 0;  // the sliced road search continues in Unit::work
	const int32_t pa0 = ord.whom;
	int32_t a_o = ord.ox;
	auto fail = [&]() {
		kill_current_order(g, u, false);
		if (u.orders.empty()) think_caravan(g, u, true);
		return 0;
	};
	if (a_o < 0 || pa0 < 0 || pa0 >= 8) return fail();
	Build* A = g.build(pa0, a_o);
	if (!A || A->city < 0) return fail();
	int32_t a_city = A->city, a_pl = pa0;
	if (!ord.started && size_t(a_city) < g.cities[size_t(a_pl)].size() && g.cities[size_t(a_pl)][size_t(a_city)].o != a_o) {
		ord.ox = a_o = g.cities[size_t(a_pl)][size_t(a_city)].o;
		A = g.build(a_pl, a_o);
		if (!A) return fail();
	}
	const int32_t ureg = g.world.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
	const int32_t areg = g.world.get_tregion(tcoord_of(A->x), tcoord_of(A->y));
	if (ureg != areg && !unit_can_transport(u)) return fail();
	int32_t b_o = ord.oxx, b_pl = ord.whose;
	if (b_o < 0) {
		int32_t best = -1, bc = -1, bp = u.who;
		for (int32_t p = 0; p < 8; p++) {
			if (!is_ally(g, u.who, p)) continue;
			if (u.who != p && !(has_preq(g, u.who, etype::kForeignTrade) && u.who == pa0)) continue;
			for (int32_t c = 0; c < g.leaders[size_t(p)].city_mark && size_t(c) < g.cities[size_t(p)].size(); c++) {
				if (c == a_city && pa0 == p) continue;
				const City& ct = g.cities[size_t(p)][size_t(c)];
				if (!ct.valid()) continue;
				if (!city_is_seen(g, p, c, u.who)) continue;
				if (!city_get_empty_trade_routes(g, pa0, a_city, p, c) || !city_get_empty_trade_routes(g, p, c, pa0, a_city)) continue;
				const Build* cb = g.build(p, ct.o);
				if (!cb) continue;
				if (areg != g.world.get_tregion(tcoord_of(cb->x), tcoord_of(cb->y)) && !unit_can_transport(u)) continue;
				// asm @5ed665: ECX / EDX = (a_city, pa) are the first (bonus) end; the stack holds the candidate
				const int32_t v = u.who == pa0 ? trade_value(g, a_city, pa0, c, p) << 2 : trade_value(g, c, p, a_city, pa0);
				if (best < v) {
					best = v;
					bc = c;
					bp = p;
				}
			}
		}
		if (best < 0 || bc < 0) {
			u.idle = 99;
			kill_current_order(g, u, false);
			if (u.unit_masks & umask::kAiControl) {
				const int32_t nx = a_city + 1;
				if (nx < g.leaders[size_t(pa0)].city_mark && size_t(nx) < g.cities[size_t(pa0)].size() && g.cities[size_t(pa0)][size_t(nx)].valid())
					go_to_city(g, u, nx, pa0, 1);
			}
			return 0;
		}
		b_o = g.cities[size_t(bp)][size_t(bc)].o;
		b_pl = bp;
		ord.oxx = b_o;
		ord.whose = b_pl;
		const Build* nb = g.build(b_pl, b_o);
		ord.uid2 = nb ? nb->uid : 0xffff;
	}
	Build* B = (b_pl >= 0 && b_pl < 8) ? g.build(b_pl, b_o) : nullptr;
	if (!A || !(A->flags & objflag::kActive) || !B || !(B->flags & objflag::kActive) || B->city < 0) return fail();
	int32_t b_city = B->city;
	if (size_t(a_city) >= g.cities[size_t(a_pl)].size() || size_t(b_city) >= g.cities[size_t(b_pl)].size()) return fail();
	if (!g.cities[size_t(a_pl)][size_t(a_city)].valid() || !g.cities[size_t(b_pl)][size_t(b_city)].valid()) return fail();
	const int32_t breg = g.world.get_tregion(tcoord_of(B->x), tcoord_of(B->y));
	if ((ureg != areg || areg != breg) && !unit_can_transport(u)) return fail();
	if (!ord.started) {
		if (!((b_pl == u.who && a_pl == u.who) || has_preq(g, u.who, etype::kForeignTrade))) return fail();
		if (!city_get_empty_trade_routes(g, a_pl, a_city, b_pl, b_city) || !city_get_empty_trade_routes(g, b_pl, b_city, a_pl, a_city))
			return fail();
		// Nearer B: swap the local ends but not ox / oxx (quirk kept); the distances are to the cities' x / y.
		const City& cA = g.cities[size_t(a_pl)][size_t(a_city)];
		const City& cB = g.cities[size_t(b_pl)][size_t(b_city)];
		const int32_t dA = vector_dist(iabs(cA.x - u.x), iabs(cA.y - u.y));
		const int32_t dB = vector_dist(iabs(cB.x - u.x), iabs(cB.y - u.y));
		if (dB < dA) {  // asm @5edaf7: dist(unit, B) < dist(unit, A)
			std::swap(a_city, b_city);
			std::swap(a_pl, b_pl);
		}
		city_add_van(g.cities[size_t(a_pl)][size_t(a_city)], u.cara, u.who);
		city_add_van(g.cities[size_t(b_pl)][size_t(b_city)], u.cara, u.who);
		rec.city2 = int16_t(a_city);
		rec.whom = int16_t(a_pl);
		rec.city3 = int16_t(b_city);
		rec.whose = int16_t(b_pl);
		rec.caravan_flags |= 3;
		ord.started = 1;
		city_compute_trade(g, a_pl, a_city);
		city_compute_trade(g, b_pl, b_city);
		if (caravan_build_road(g, rec) < 0) {
			const City& ac = g.cities[size_t(a_pl)][size_t(a_city)];
			add_move_order(g, u, ac.x, ac.y, 1, false, kQueueFirst, false, -1, -1);
			return 0;
		}
	}
	// LAB_005edd56: the arrival tests. a_* / b_* are the (possibly swapped) route ends; the objects are those of the order.
	int32_t target_o = a_o, target_who = a_pl, R = 0;
	if (ord.loaded) {
		const Object* t = g.object(a_pl, a_o);
		if (t && t->ptype) {
			R = std::max(t->ptype->x_size, t->ptype->y_size) * 0x60;
			if (iabs(u.x - t->x) <= R + 0x306 && iabs(u.y - t->y) <= R + 0x306) {
				ord.loaded = 0;
				u.unit_masks |= 0x200;
				if (!(u.unit_masks & umask::kDecoy)) {
					rec.caravan_flags |= 4;
					city_compute_trade(g, a_pl, a_city);
					city_compute_trade(g, b_pl, b_city);
					city_new_caravan(g, a_pl, a_city, b_pl, b_city);
				}
			}
		}
		target_o = b_o;
		target_who = b_pl;
	} else {
		const Object* t = g.object(b_pl, b_o);
		// B must be an active trade building, or the route ends
		if (!t || !t->ptype || t->is_unit || !(t->flags & objflag::kActive) || !type_is(*t->ptype, ptype::kVillage)) return fail();
		R = std::max(t->ptype->x_size, t->ptype->y_size) * 0x60;
		if (iabs(u.x - t->x) <= R + 0x306 && iabs(u.y - t->y) <= R + 0x306) {
			ord.loaded = 1;
			if (!(u.unit_masks & umask::kDecoy)) {
				const Build* tbld = g.build(b_pl, b_o);
				if (tbld && tbld->city >= 0) city_new_caravan(g, b_pl, tbld->city, a_pl, a_city);
			}
		}
	}
	// LAB_005ee01a: the movement. With a road: the end that is farther from the unit becomes the destination.
	if (!rec.road.empty()) {
		if (vector_dist(iabs(rec.road.front().x - u.x), iabs(rec.road.front().y - u.y)) <
		    vector_dist(iabs(rec.road.back().x - u.x), iabs(rec.road.back().y - u.y)))
			std::reverse(rec.road.begin(), rec.road.end());  // Stack::invert
		u.path = rec.road;                                   // Stack::operator=
		u.path_size = std::max(u.path_size, rec.road_size);
		if (u.path.size() > 1) {
			// the lane: every step within 192 of the previous one is nudged sideways (by the previous, unshifted, point)
			int32_t py = u.path[0].y, px = u.path[0].x;
			for (size_t i = 1; i < u.path.size(); i++) {
				const int32_t cy = u.path[i].y, cx = u.path[i].x;
				const int32_t dy = cy - py, dx = cx - px;
				if (iabs(dy) < 0xc1 && iabs(dx) < 0xc1) {
					u.path[i].x += dy / 3;
					const int32_t hi = int32_t((int64_t(dx) * 0x55555555) >> 32) - dx;
					u.path[i].y += (hi >> 1) - (hi >> 31);
				}
				px = cx;
				py = cy;
			}
		}
		const PathData popped = u.path.back();
		u.path.pop_back();
		const PathData& dest = u.path.empty() ? popped : u.path.front();  // (Stack::pop leaves the element in place: list[0] after the pop)
		add_move_order(g, u, dest.x, dest.y, 1, true, kQueueFirst, false, -1, -1);
		if (!u.path.empty()) {
			const PathData& top = u.path.back();
			const int32_t r_u = g.world.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
			const int32_t r_t = g.world.get_tregion(top.x / 192, top.y / 192);
			if (r_u != r_t) u.path.back().flags |= 4;  // pop + push with the region-change flag
		}
		std::reverse(rec.road.begin(), rec.road.end());  // the stored road's orientation toggles on every use
		return 0;
	}
	// no road: a spot near the building that was tested, and a move when it is close enough
	const Object* T = g.object(target_who, target_o);
	if (!T) return fail();
	int32_t sx = T->x, sy = T->y;
	find_nearby_spot(g, *u.ptype, T->x, T->y, &sx, &sy, R, R + 0xc0, 0, find_angle(u.x - T->x, u.y - T->y), kFilterNotMe,
	                 u.o, u.who, 0, 0, -1, 0, -1);
	if (vector_dist(iabs(sx - T->x), iabs(sy - T->y)) > R + 0xc6) return 0;
	add_move_order(g, u, sx, sy, 1, false, kQueueFirst, false, -1, -1);
	return 0;
}

void group_action_trade(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t oxx, int32_t whose, QueuePos q) {
	// Group::action_trade @701cc0 🟡: every caravan member of a group gets the route (QUEUE_FIRST through the insert
	// pattern is TODO; it is treated as QUEUE_NEW).
	const Object* T = g.object(whom, ox);
	if (!T || T->is_unit || !(T->flags & objflag::kActive) || !type_is(*T->ptype, ptype::kVillage)) return;
	gr.disband = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* u = g.unit(gr.who, gr.list[size_t(i)]);
		if (!u || !u->alive() || !(u->ptype->unit_flags2 & 8) || u->ptype->domain != 0) continue;
		add_trade_order(g, *u, ox, whom, oxx, whose, q == kQueueLast ? kQueueLast : kQueueNew, true);
	}
}

}  // namespace ron::sim
