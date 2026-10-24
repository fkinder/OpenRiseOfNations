// Fishermen, merchants and rares (docs/economy-spec.md 3.7, 4.7), caravans and trade routes (section 6).
#include <algorithm>

#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/production_internal.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"
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
	// Caravans::init_caravan @73e1f0: the first unused record below the mark, else a new one.
	auto& v = g.caravans[size_t(who)];
	CaravanRec r;
	r.caravan_flags = 1;
	r.o = int16_t(o);
	r.who = int8_t(who);
	for (size_t i = 0; i < v.size(); i++) {
		if (!(v[i].caravan_flags & 1)) {
			r.cara = int16_t(i);
			v[i] = r;
			LD(g, who).caravan_mark = std::max<int32_t>(LD(g, who).caravan_mark, int32_t(i) + 1);
			return int32_t(i);
		}
	}
	r.cara = int16_t(v.size());
	v.push_back(r);
	LD(g, who).caravan_mark = int32_t(v.size());
	return int32_t(v.size()) - 1;
}

void caravans_close(Game& g, int32_t who, int32_t slot) {
	auto& v = g.caravans[size_t(who)];
	if (slot < 0 || size_t(slot) >= v.size()) return;
	v[size_t(slot)].caravan_flags &= uint8_t(~1);
	v[size_t(slot)].who = -1;
	v[size_t(slot)].o = -1;
	while (!v.empty() && !(v.back().caravan_flags & 1)) v.pop_back();
	LD(g, who).caravan_mark = int32_t(v.size());
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
	// Unit::end_trade_route @5e3bd0 🟡: clear flags 2 | 4, drop the van links, recompute both cities.
	CaravanRec* r = (u.cara >= 0 && size_t(u.cara) < g.caravans[size_t(u.who)].size()) ? &g.caravans[size_t(u.who)][size_t(u.cara)] : nullptr;
	if (!r) return;
	const int32_t c2 = r->city2, p2 = r->whom, c3 = r->city3, p3 = r->whose;
	r->caravan_flags &= uint8_t(~6);
	auto unlink = [&](int32_t p, int32_t c) {
		if (p < 0 || c < 0 || size_t(c) >= g.cities[size_t(p)].size()) return;
		auto& v = g.cities[size_t(p)][size_t(c)].vans;
		for (size_t i = 0; i < v.size(); i++)
			if (v[i][0] == u.cara && v[i][1] == u.who) {
				v.erase(v.begin() + std::ptrdiff_t(i));
				break;
			}
		city_compute_trade(g, p, c);
	};
	unlink(p2, c2);
	unlink(p3, c3);
	// TODO(trade): Caravan::clear_road / the stored road.
}

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
			// FILTER_CAN_TRADE (valid_filter 17): another seen, alive city with an empty route both ways (own, or an
			// ally's with FOREIGN_TRADE). TODO(spec): city seen tests (CityData::is_seen) are taken as true.
			bool ok = false;
			for (int32_t q = 0; q < 8 && !ok; q++) {
				if (!(g.leaders[size_t(q)].flags & leaderflag::kValid)) continue;
				if (q != p && !(is_ally(g, p, q) && has_preq(g, p, etype::kForeignTrade))) continue;
				for (int32_t e = 0; e < g.leaders[size_t(q)].city_mark && size_t(e) < g.cities[size_t(q)].size() && !ok; e++) {
					if (q == p && e == c) continue;
					if (!g.cities[size_t(q)][size_t(e)].valid()) continue;
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
	// Unit::do_trade @5ed270 (economy-spec 6.3). TODO(trade, economy-spec 6.4): Caravan::build_road and the road
	// movement; without a road the caravan heads to the building it is not testing (the original's quirk), so a
	// route never completes until roads are ported.
	unit_set_anim(g, u, kCharDefault, 0, 1);
	if (u.cara < 0) return 0;
	const int32_t pa = ord.whom;
	int32_t a_o = ord.ox;
	auto fail = [&]() {
		kill_current_order(g, u, false);
		if (u.orders.empty()) think_caravan(g, u, true);
		return 0;
	};
	if (a_o < 0 || pa < 0 || pa >= 8) return fail();
	Build* A = g.build(pa, a_o);
	if (!A || A->city < 0) return fail();
	int32_t ca = A->city;
	if (!ord.started && g.cities[size_t(pa)][size_t(ca)].o != a_o) {
		ord.ox = a_o = g.cities[size_t(pa)][size_t(ca)].o;
		A = g.build(pa, a_o);
	}
	const int32_t ureg = g.world.get_tregion(tcoord_of(u.x), tcoord_of(u.y));
	const int32_t areg = g.world.get_tregion(tcoord_of(A->x), tcoord_of(A->y));
	if (ureg != areg && !unit_can_transport(u)) return fail();
	int32_t b_o = ord.oxx, pb = ord.whose;
	if (b_o < 0) {
		int32_t best = -1, bc = -1, bp = -1;
		for (int32_t p = 0; p < 8; p++) {
			if (!is_ally(g, u.who, p)) continue;
			if (!(u.who == p || (has_preq(g, u.who, etype::kForeignTrade) && u.who == pa))) continue;
			for (int32_t c = 0; c < g.leaders[size_t(p)].city_mark && size_t(c) < g.cities[size_t(p)].size(); c++) {
				if (c == ca && pa == p) continue;
				const City& ct = g.cities[size_t(p)][size_t(c)];
				if (!ct.valid()) continue;
				if (!city_get_empty_trade_routes(g, pa, ca, p, c) || !city_get_empty_trade_routes(g, p, c, pa, ca)) continue;
				const Build* cb = g.build(p, ct.o);
				if (!cb) continue;
				if (g.world.get_tregion(tcoord_of(cb->x), tcoord_of(cb->y)) != areg && !unit_can_transport(u)) continue;
				const int32_t v = u.who == pa ? trade_value(g, c, p, ca, pa) << 2 : trade_value(g, ca, pa, c, p);
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
			// TODO(AI): AI caravans go_to_city(a + 1, pa, 1).
			return 0;
		}
		b_o = g.cities[size_t(bp)][size_t(bc)].o;
		pb = bp;
		ord.oxx = b_o;
		ord.whose = pb;
		const Build* nb = g.build(pb, b_o);
		ord.uid2 = nb ? nb->uid : 0xffff;
	}
	Build* B = g.build(pb, b_o);
	if (!A || !(A->flags & objflag::kActive) || !B || !(B->flags & objflag::kActive) || B->city < 0) return fail();
	int32_t cb = B->city;
	if (!g.cities[size_t(pa)][size_t(ca)].valid() || !g.cities[size_t(pb)][size_t(cb)].valid()) return fail();
	if (!ord.started) {
		if (!((pb == u.who && pa == u.who) || has_preq(g, u.who, etype::kForeignTrade))) return fail();
		if (!city_get_empty_trade_routes(g, pa, ca, pb, cb) || !city_get_empty_trade_routes(g, pb, cb, pa, ca)) return fail();
		// Nearer B: swap the local ends but not ox / oxx (quirk kept).
		int32_t la = ca, lpa = pa, lb = cb, lpb = pb;
		if (vector_dist(iabs(u.x - B->x), iabs(u.y - B->y)) < vector_dist(iabs(u.x - A->x), iabs(u.y - A->y))) {
			std::swap(la, lb);
			std::swap(lpa, lpb);
		}
		g.cities[size_t(lpa)][size_t(la)].vans.push_back({u.cara, int16_t(u.who)});
		g.cities[size_t(lpb)][size_t(lb)].vans.push_back({u.cara, int16_t(u.who)});
		CaravanRec& r = g.caravans[size_t(u.who)][size_t(u.cara)];
		r.city2 = int16_t(la);
		r.whom = int16_t(lpa);
		r.city3 = int16_t(lb);
		r.whose = int16_t(lpb);
		r.caravan_flags |= 3;
		ord.started = 1;
		city_compute_trade(g, lpa, la);
		city_compute_trade(g, lpb, lb);
		// Caravan::build_road: TODO(trade) -> treated as "still searching" (< 0): walk toward A.
		const City& ac = g.cities[size_t(lpa)][size_t(la)];
		add_move_order(g, u, ac.x, ac.y, 1, false, kQueueFirst, false, -1, -1);
		return 0;
	}
	int32_t target_o = a_o, target_who = pa;
	if (ord.loaded) {
		const int32_t R = std::max(A->ptype->x_size, A->ptype->y_size) * 0x60;
		if (iabs(u.x - A->x) <= R + 0x306 && iabs(u.y - A->y) <= R + 0x306) {
			ord.loaded = 0;
			u.unit_masks |= 0x200;
			if (!(u.unit_masks & umask::kDecoy)) {
				g.caravans[size_t(u.who)][size_t(u.cara)].caravan_flags |= 4;
				city_compute_trade(g, pa, ca);
				city_compute_trade(g, pb, cb);
				city_new_caravan(g, pa, ca, pb, cb);
			}
		}
		target_o = b_o;
		target_who = pb;
	} else {
		const int32_t R = std::max(B->ptype->x_size, B->ptype->y_size) * 0x60;
		if (iabs(u.x - B->x) <= R + 0x306 && iabs(u.y - B->y) <= R + 0x306) {
			ord.loaded = 1;
			if (!(u.unit_masks & umask::kDecoy)) city_new_caravan(g, pb, cb, pa, ca);
		}
	}
	const Build* T = g.build(target_who, target_o);
	if (!T) return fail();
	// (no road) a spot near the target building, and a move when it is close enough.
	const int32_t R = std::max(T->ptype->x_size, T->ptype->y_size) * 0x60;
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
