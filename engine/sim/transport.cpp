// Transports (docs/transport-spec.md): the predicates of who may be carried, SpellType::cast_transport, Unit::same_damage,
// the BOARD_SHIP / AWAIT_BOARD orders with Unit::check_meet_ship, and the TRANSPORT / SET_TRANSPORT / BOARD_SHIP group
// actions. The cast order itself is queued by Unit::set_new_location (guys.cpp) and run by Unit::do_cast (spells.cpp);
// the landing is Object::eject_contents (garrison.cpp).
#include "sim/transport.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "sim/aircraft.hpp"
#include "sim/combat_internal.hpp"
#include "sim/compass.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/production_internal.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace combat_detail;

namespace {

constexpr int32_t kTypeScout = 0x45, kTypeCaravan = 0x3b, kTypeCarrier = 0x15f, kTypeMarines = 0x77,
                  kTypeMerchantFleet = 318;

// UnitData::total_damage(0, null) @609c00 (the same walk as script_funcs_query.cpp's, which is private to that file):
// the damage of the whole uber chain from the captain, the members already dead counted at full hits, plus the
// sixteenths.
int32_t total_damage(Game& g, Unit& start, int32_t from_member, int32_t* frac_out) {
	Unit* u = &start;
	if (!from_member && u->o_up >= 0) {
		Unit* cap = g.unit(u->who, u->o_up);
		if (cap) return total_damage(g, *cap, 0, nullptr);
	}
	int32_t frac = 0;
	int32_t d = u->damage;
	if (u->o_down >= 0) {
		Unit* n = g.unit(u->who, u->o_down);
		if (n && (n->flags & objflag::kValid)) d += total_damage(g, *n, 1, &frac);
	}
	if (u->o_up >= 0) {
		if (frac_out) *frac_out = int32_t(u->damage_frac) + frac;
		return d;
	}
	const int32_t hits = obj_hits(g, *u);
	const int32_t uber = u->ptype->uber_size;
	const int32_t cur = unit_curr_uber_size(g, *u, true);
	if (uber != 0) d += ((uber - cur) * hits) / uber;
	const uint32_t f = uint32_t(frac + int32_t(u->damage_frac));
	if (int32_t(f) > 0xf) return d + int32_t(f >> 4);
	return d;
}

int32_t region_of_tile_at(const Game& g, int32_t x, int32_t y) {
	return g.world.get_tregion(tcoord_of(x), tcoord_of(y));
}

// UnitData::is_busy @60a370: the front order is a pack / unpack cast, or the unit is entering / leaving a building.
bool is_busy(const Unit& u) { return cast_is_busy(u); }

}  // namespace

bool transport_trace() {
	static const bool on = std::getenv("RON_TRANSPORT_TRACE") != nullptr;
	return on;
}

// ---------------------------------------------------------------- who may be transported (2)

int32_t leader_can_transport(const Game& g, int32_t who) {
	// @6e0c60
	const uint32_t f = g.leaders[size_t(who)].flags;
	if (f & leaderflag::kCanTransportCiv) return kTransportCivilian;
	if (f & leaderflag::kCanTransportMil) return kTransportMilitary;
	return int32_t((f >> 10) & 1);
}

int32_t unit_transport_type(const Game& g, const Unit& u) {
	// @46f790
	if (unit_is(g, u, kTypeScout)) return kTransportScout;
	const int32_t t = u.type;
	if (t == 0x32 || t == 0x33 || t == 0x34 || t == 0x35) return kTransportCivilian;
	if (unit_is(g, u, kTypeCaravan) || t == 0x3d || t == 0x3e || t == 0x190) return kTransportCivilian;
	return kTransportMilitary;
}

bool unit_can_ever_transport(const Game& g, const Unit& u) {
	// @46f290. ❓ The spec (2.3) has the last test inverted; the disassembly returns 1 for a unit that is NOT a carrier.
	if (u.ptype->domain == 0) return true;
	if (u.ptype->carry == 0) return false;
	return !unit_is(g, u, kTypeCarrier);
}

bool object_is_cargo(const Game& g, const Object& ob) {
	// @653600. Buildings use BuildTypeData::is_cargo (never carried here).
	(void)g;
	if (!ob.is_unit) return false;
	if (!static_cast<const Unit&>(ob).on_map()) return false;
	return ob.type >= 0x32 && ob.type < 0x19e && ob.ptype->domain == 0;
}

int32_t cargo_inside(const Game& g, const Object& ob) {
	// @64dcd0
	if (ob.is_unit && !static_cast<const Unit&>(ob).on_map()) return 0;
	int32_t n = 0;
	int32_t o = ob.inside_down, w = ob.inside_down_who;
	for (int guard = 0; o >= 0 && guard < 4 * kUnitLimit; guard++) {
		const Object* c = g.object(w, o);
		if (!c) break;
		if (c->is_unit && c->alive() && static_cast<const Unit&>(*c).o_up < 0) n += c->ptype->carry_size;
		o = c->inside_down;
		w = c->inside_down_who;
	}
	return n;
}

bool can_carry_unit(const Game& g, const Object& carrier, int32_t o, int32_t who) {
	// @6483c0, this = carrier
	if (o == carrier.o && who == carrier.who) return false;
	if (who != carrier.who) return false;
	const Object* cargo = g.object(who, o);
	if (!cargo || !cargo->ptype) return false;
	if (cargo->ptype->domain != kDomainAir) {
		if (cargo->ptype->domain == kDomainSea) return false;
		if (!cargo->is_unit || !carrier.is_unit) return false;
		if (type_is(*carrier.ptype, kTypeCarrier)) return false;
		return cargo_inside(g, carrier) + cargo->ptype->carry_size < carrier.ptype->carry;
	}
	// Aircraft cargo: airbases, carriers and missile silos (sim/aircraft.cpp).
	return air_can_carry(g, carrier, o, who);
}

bool unit_is_caravan(const Unit& u) { return u.ptype && (u.ptype->unit_flags2 & utflag2::kCaravan) != 0; }

// ---------------------------------------------------------------- same_damage (7.2)

void same_damage(Game& g, Unit& self, int32_t src_o, int32_t src_who) {
	// @5f9400. `L` is the last live member of the chain (local_8 in the decompile).
	Unit* L = &self;
	if (self.o_down >= 0) {
		Unit* cur = &self;
		for (;;) {
			Unit* n = g.unit(cur->who, cur->o_down);
			// An invalid follower ends the walk with L still the first unit (local_8 is only updated when the loop
			// runs to the end of the chain). ❓ as read from the decompile.
			if (!n || !(n->flags & objflag::kValid)) break;
			cur = n;
			if (cur->o_down < 0) {
				L = cur;
				break;
			}
		}
	}
	const int32_t per = obj_hits(g, *L) / L->ptype->uber_size;  // hit points per member
	int32_t r = (total_damage(g, *L, 0, nullptr) << 8) / obj_hits(g, *L);
	Unit* src = g.unit(src_who, src_o);
	if (!src) return;
	const int32_t r_src = (total_damage(g, *src, 0, nullptr) << 8) / obj_hits(g, *src);
	if (r < r_src) r = r_src;
	const int32_t h = obj_hits(g, *L);
	int32_t extra = ((h * r + (((h * r) >> 31) & 0xff)) >> 8) - total_damage(g, *L, 0, nullptr);
	if (extra < 0) extra = 0;
	if (extra == 0) return;
	int32_t o = L->o;
	const int32_t who = L->who;
	for (;;) {
		if (o < 0) return;
		Unit* u = g.unit(who, o);
		if (!u || !(u->flags & objflag::kValid)) return;
		const int32_t next = u->o_down;
		if (next < 0) {  // the last member takes what is left, but never dies of it
			const int32_t cap = (obj_hits(g, *u) - total_damage(g, *u, 0, nullptr)) - 1;
			u->damage = extra < cap ? extra : cap;
			return;
		}
		if (extra < per) {
			u->damage = extra;
			return;
		}
		object_die(g, *u, 0, -1, 0.0f);
		extra -= per;
		if (extra == 0) return;
		o = next;
	}
}

// ---------------------------------------------------------------- cast_transport (5)

void cast_transport(Game& g, int32_t o, int32_t who) {
	// @670db0. `U` is re-fetched after init_unit (the unit arrays may move).
	Unit* U = g.unit(who, o);
	if (!U || !U->alive() || !U->on_map()) return;
	if (U->ptype->domain == kDomainSea || U->ptype->domain == kDomainAir) return;
	int32_t B;
	if (!unit_is_caravan(*U)) {
		B = current_upgrade(g, who, ptype::kTransportBarge);
		if (B < 0) B = ptype::kTransportBarge;
	} else {
		B = current_upgrade(g, who, kTypeMerchantFleet);
		if (B < 0) B = kTypeMerchantFleet;
	}
	int32_t sx = 0, sy = 0;
	if (find_nearby_spot(g, g.type(B), U->x, U->y, &sx, &sy, 0, K("unit_board_distance"), 0, 0x55555555, kFilterNotMe, -1, -1, 0, 0,
	                     -1, 0, -1) != 0) {
		// (console: S_NOT_NEAR_OCEAN_FOR_TRANSPORT) the cast order is killed by do_cast on its next first frame.
		return;
	}
	const int32_t b = init_unit(g, who, B, U->x, U->y, -1, -1, -1);  // created ON the unit's position
	if (b < 0) return;
	U = g.unit(who, o);
	Unit* Bg = g.unit(who, b);
	// (sound S_TRANSPORTLOAD when the unit is on screen and seen by the console)
	if (transport_trace())
		std::fprintf(stderr, "[transport] f%d cast_transport L%d unit %d (type %d at %d,%d) -> barge %d (type %d) spot %d,%d\n", g.frame,
		             who, o, U->type, U->x, U->y, b, B, sx, sy);
	if (unit_is(g, *U, kTypeMarines)) {
		Bg->unit_masks2 |= umask2::kMarineTransport;
		unit_update_hits(g, *Bg);
		update_speed(g, *Bg);
	}
	same_damage(g, *Bg, o, who);
	unit_set_new_location(g, *Bg, sx, sy, true, true);  // teleport to the found spot
	unit_set_angle(g, *Bg, U->angle, true);
	if (unit_is_caravan(*U)) {
		Bg->cara = U->cara;
		if (U->cara >= 0 && size_t(U->cara) < g.caravans[size_t(who)].size()) g.caravans[size_t(who)][size_t(U->cara)].o = int16_t(b);
		if (U->unit_masks & 0x200) Bg->unit_masks |= 0x200;  // kTradeLoaded
		else Bg->unit_masks &= ~0x200u;
	}
	while (!U->orders.empty()) {  // every order moves, front first
		Bg->orders.push_back(U->orders.front());
		U->orders.pop_front();
	}
	kill_current_order(g, *Bg, false);  // kills the CAST order itself (it was first)
	for (const PathData& p : U->path) path_push(*Bg, p);  // the original inverts U.path and pops it into Bg.path
	U->path.clear();
	Bg->unit_masks |= umask::kCanTransport;
	if (!Bg->path.empty()) {  // the next point loses flag 4 when it is in the barge's region
		PathData p = Bg->path.back();
		Bg->path.pop_back();
		// ❓ the decompile reuses the registers of the spot (sx, sy); the barge now stands there.
		if (region_of_tile_at(g, p.x, p.y) == region_of_tile_at(g, sx, sy)) p.flags &= ~pathflag::kTransport;
		path_push(*Bg, p);
	}
	if (transport_trace())
		std::fprintf(stderr, "[transport] f%d barge %d takes %zu orders (front %d), path %zu, damage %d/%d (unit %d/%d)\n", g.frame, b,
		             Bg->orders.size(), Bg->orders.empty() ? 0 : int(Bg->orders.front().type), Bg->path.size(), Bg->damage,
		             obj_hits(g, *Bg), U->damage, obj_hits(g, *U));
	clear_orders(g, *U);
	// (console selection: the unit out, the barge in; Object::replace_hotunit: presentation)
	unit_go_inside(g, *U, b, who);  // the unit and its o_down chain enter the barge
}

// ---------------------------------------------------------------- the boarding orders (8)

Order* add_board_order(Game& g, Unit& u, int32_t o, int32_t who, QueuePos q) {
	// @5e4d10: the QUEUE_NEW preamble (add_order's), no other queue handling.
	Order c;
	c.type = kBoardShip;
	c.ox = o;
	c.whom = who;
	const Object* t = (o < 0 || who < 0) ? nullptr : g.object(who, o);
	c.uid = t ? t->uid : uint16_t(0xffff);
	c.flags = orderflag::kExplicit;
	if (q == kQueueNew) clear_orders(g, u);
	u.orders.push_back(c);
	Order* r = &u.orders.back();
	update_action(g, u);
	return r;
}

Order* add_await_board_order(Game& g, Unit& u, int32_t o, int32_t who, bool expl) {
	// @5e4c80: no queue handling at all.
	Order c;
	c.type = kAwaitBoard;
	c.ox = o;
	c.whom = who;
	const Object* t = (o < 0 || who < 0) ? nullptr : g.object(who, o);
	c.uid = t ? t->uid : uint16_t(0xffff);
	c.flags = expl ? uint8_t(orderflag::kExplicit) : uint8_t(0);
	u.orders.push_back(c);
	Order* r = &u.orders.back();
	update_action(g, u);
	return r;
}

int32_t do_board(Game& g, Unit& u, Order& o) {
	// @5ed1f0
	const int32_t so = o.ox, swho = o.whom;
	unit_set_anim(g, u, kCharDefault, 0, 1);
	if (check_meet_ship(g, u, so, swho) == 0) {
		kill_current_order(g, u, false);
		const Unit* S = g.unit(swho, so);
		if (S && can_carry_unit(g, *S, u.o, u.who)) unit_go_inside(g, u, so, swho);
	}
	return 0;
}

int32_t do_await_board(Game& g, Unit& u, Order& o) {
	// @5ed040: the ship waits for (o.ox, o.whom) to come aboard.
	const int32_t ox = o.ox, whom = o.whom;
	unit_set_anim(g, u, kCharDefault, 0, 1);
	if (ox != u.o || whom != u.who) {
		if (whom == u.who) {
			Unit* T = g.unit(whom, ox);
			if (T && T->alive() && T->on_map() && can_carry_unit(g, u, ox, whom)) {
				const Order* act = get_action(*T);
				if (act && act->type == kBoardShip) {
					const Order* b = update_action(g, *T);
					if (b->ox == u.o) {
						if (b->whom == u.who) return 0;  // still coming: keep waiting
						kill_current_order(g, u, false);
						return 0;
					}
					kill_current_order(g, u, false);
					return 0;
				}
			}
		}
		Unit* T = g.unit(whom, ox);
		if (T && T->alive() && T->on_map()) {
			const Order* act = get_action(*T);
			if (act && act->type == kBoardShip) {
				const Order* b = update_action(g, *T);
				if (b->ox == u.o && b->whom == u.who) {
					repath(g, *T);
					kill_current_order(g, *T, false);
				}
			}
		}
	}
	kill_current_order(g, u, false);
	return 0;
}

namespace {

// Region::coast_here(region, x, y) @681020 with `me` as the region record: the direction (1..8, the move_x spiral) from the
// wcoord to a neighbour whose tregion is the other side of the land / sea pair, else 0.
int32_t coast_here(const Game& g, int32_t me, int32_t other, int32_t wx, int32_t wy) {
	if (!g.regions.is_coast(me, other)) return 0;
	const int32_t r = g.world.w(wx, wy).region;
	int32_t p1 = other;
	if (r != me) {
		if (r != other) return 0;
		p1 = me;
	}
	for (int32_t k = 1; k < 9; k++) {
		const int32_t nx = wx + move_x[k], ny = wy + move_y[k];
		if (nx < 0 || ny < 0 || nx >= g.world.xs || ny >= g.world.ys) continue;
		if (g.world.get_tregion(nx * 4 + 2, ny * 4 + 2) == p1) return k;
	}
	return 0;
}

// The hand-built MOVE_TO of check_meet_ship: a plain unpathed, non-explicit move to the ucoord centre of (x, y), put in
// front of the unit's orders (the original adds the node by hand: QUEUE_FIRST).
void push_meet_move(Game& g, Unit& u, int32_t x, int32_t y) {
	const uint32_t angle = find_angle(x - u.x, y - u.y);
	add_move_facing_order(g, u, ucoord_of(x), ucoord_of(y), angle, 1, false, kQueueFirst, false, -1, -1, -1, false);
}

}  // namespace

int32_t check_meet_ship(Game& g, Unit& me, int32_t so, int32_t swho) {
	// @604550. 0 = met, 1 = keep walking (a MOVE_TO was queued in front). ❓ the decompile loses most register moves; the
	// coordinate cases below follow the disassembly (re/decomp/transport_dis).
	Unit* S = g.unit(swho, so);
	if (!S) return 0;
	if (!can_carry_unit(g, *S, me.o, me.who)) return 0;
	const int32_t d = vector_dist(iabs(me.x - S->x), iabs(me.y - S->y));
	if (d <= K("unit_board_distance")) return 0;
	int32_t mx, my;    // where I walk
	int32_t sxt, syt;  // where the ship walks
	if (d >= 0x900) {
		// Far: the ship comes to a wcoord next to MINE (the last orthogonal neighbour it can stand on); I walk to the midpoint.
		int32_t bx = wcoord_of(me.x), by = wcoord_of(me.y);
		const int32_t wx = bx, wy = by;
		for (int32_t k = 1; k < 5; k++) {
			const int32_t cx = wx + orthog_x[k], cy = wy + orthog_y[k];
			if (unit_invalid_loc(g, *S, cx * 4 + 2, cy * 4 + 2, false, false, false, false, false) == 0) {
				bx = cx;
				by = cy;
			}
		}
		sxt = bx * 0x300 + 0x180;
		syt = by * 0x300 + 0x180;
		mx = (S->x + me.x) / 2;
		my = (S->y + me.y) / 2;
	} else if (d < 0xc0) {
		mx = sxt = (S->x + me.x) / 2;
		my = syt = (S->y + me.y) / 2;
	} else {
		const int32_t rme = region_of_tile_at(g, me.x, me.y);
		const int32_t rship = region_of_tile_at(g, S->x, S->y);
		if (!g.regions.is_coast(rme, rship)) {
			kill_current_order(g, me, false);
			return 1;
		}
		const int32_t midx = (S->x + me.x) / 2, midy = (S->y + me.y) / 2;
		mx = sxt = midx;
		my = syt = midy;
		const int32_t ttx = tcoord_of(midx), tty = tcoord_of(midy);
		if (unit_invalid_loc(g, me, ttx, tty, false, false, false, false, false) != 0 &&
		    unit_invalid_loc(g, *S, ttx, tty, false, false, false, false, false) != 0) {
			// Neither of us can stand on the midpoint: look for a coast cell of my region facing the ship's region, around
			// the midpoint's wcoord, where I (A) and the ship (B) have a free tile on each side of the shore.
			const int32_t cwx = wcoord_of(midx), cwy = wcoord_of(midy);
			int32_t k = 0;
			for (; k < 0x51; k++) {
				const int32_t cx = move_x[k] + cwx, cy = move_y[k] + cwy;
				if (!g.world.in_w(cx, cy)) continue;
				if (g.world.w(cx, cy).region != rme) continue;
				const int32_t dir = coast_here(g, rme, rship, cx, cy);
				if (dir == 0) continue;
				int32_t ax = cx * 4, ay = cy * 4, bx = ax, by = ay;
				if (dir - 3 < 4 && dir >= 3) {
					ax += 3;
					bx += 4;
				}
				if (dir >= 5) {
					ay += 3;
					by += 4;
				}
				if (dir == 1 || dir >= 6) bx--;
				if (dir <= 3 || dir == 8) by--;
				bool found = false;
				if (dir & 1) {
					found = unit_invalid_loc(g, me, ax, ay, false, false, false, false, false) == 0 &&
					        unit_invalid_loc(g, *S, bx, by, false, false, false, false, false) == 0;
				} else {
					int32_t dxs = (dir & 2) != 0 ? 1 : 0, dys = (dir & 2) == 0 ? 1 : 0;
					if (dir > 5) dxs = -dxs, dys = -dys;
					for (int32_t i = 0; i < 4; i++) {
						if (unit_invalid_loc(g, me, ax, ay, false, false, false, false, false) == 0 &&
						    unit_invalid_loc(g, *S, bx, by, false, false, false, false, false) == 0) {
							found = true;
							break;
						}
						ax += dxs;
						ay += dys;
						bx += dxs;
						by += dys;
					}
				}
				if (found) {
					mx = ax * 0xc0 + 0x60;
					my = ay * 0xc0 + 0x60;
					sxt = bx * 0xc0 + 0x60;
					syt = by * 0xc0 + 0x60;
					break;
				}
			}
			if (k == 0x51) {  // no such cell
				kill_current_order(g, me, false);
				return 1;
			}
		}
	}
	const Order* sact = get_action(*S);
	if (swho == me.who && (!sact || sact->type == kOrderNone)) {
		// The ship is idle: it walks to its point and waits for me.
		push_meet_move(g, me, mx, my);
		S = g.unit(swho, so);
		clear_orders(g, *S);
		push_meet_move(g, *S, sxt, syt);
		add_await_board_order(g, *S, me.o, me.who, false);
		return 1;
	}
	push_meet_move(g, me, mx, my);
	if (swho == me.who && S->order_type() == kAwaitBoard) {
		const Order* a = S->front();
		if (a->ox == me.o && a->whom == me.who) add_move_order(g, *S, sxt, syt, 1, false, kQueueFirst, false, -1, -1);
	}
	return 1;
}

// ---------------------------------------------------------------- the group actions (2.9, 8.1, 9)

void group_action_set_transport(Game& g, Group& gr, int32_t flag) {
	// @7024b0.
	if (g.scen.ignore_orders != 0 && uint32_t(gr.who) < 8) for (size_t k = 0; k < g.scen.objects_ignoring_orders[size_t(gr.who)].size(); k++) if (g.scen.objects_ignoring_orders[size_t(gr.who)][k] >= 0) group_kill(g, gr, g.scen.objects_ignoring_orders[size_t(gr.who)][k], gr.who, 0, 0);  // ScenarioData::ignore_orders prelude
	gr.disband = 0;  // action_begin
	if (gr.buildings != 0) return;
	const int32_t level = leader_can_transport(g, gr.who);
	const int32_t on = level != 0 ? flag : 0;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !unit_can_ever_transport(g, *m)) continue;
		if (on == 0) m->unit_masks &= ~umask::kCanTransport;
		else m->unit_masks |= umask::kCanTransport;
	}
}

void group_action_transport(Game& g, Group& gr, int32_t force) {
	// @702620.
	if (g.scen.ignore_orders != 0 && uint32_t(gr.who) < 8) for (size_t k = 0; k < g.scen.objects_ignoring_orders[size_t(gr.who)].size(); k++) if (g.scen.objects_ignoring_orders[size_t(gr.who)][k] >= 0) group_kill(g, gr, g.scen.objects_ignoring_orders[size_t(gr.who)][k], gr.who, 0, 0);  // ScenarioData::ignore_orders prelude
	gr.disband = 0;  // action_begin
	if (gr.buildings != 0) return;
	gr.form = -1;  // group +0x10
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* M = g.unit(gr.who, gr.list[size_t(i)]);
		if (!M || !M->alive() || !M->on_map() || M->o_up >= 0) continue;
		if (M->ptype->domain == kDomainSea || M->ptype->domain == kDomainAir || is_busy(*M)) continue;
		int32_t B;
		if (!unit_is_caravan(*M)) {
			B = current_upgrade(g, gr.who, ptype::kTransportBarge);
			if (B < 0) B = ptype::kTransportBarge;
		} else {
			B = current_upgrade(g, gr.who, kTypeMerchantFleet);
			if (B < 0) B = kTypeMerchantFleet;
		}
		const int32_t mo = M->o;
		const int32_t b = init_unit(g, gr.who, B, M->x, M->y, -1, -1, -1);
		if (b < 0) continue;
		M = g.unit(gr.who, mo);
		Unit* Bg = g.unit(gr.who, b);
		int32_t sx = 0, sy = 0;
		if (find_nearby_spot(g, *Bg->ptype, M->x, M->y, &sx, &sy, 0, K("unit_board_distance"), 0, 0x55555555, kFilterNotMe, b, gr.who, 0, 0,
		                     -1, 0, -1) == 0 ||
		    force != 0) {
			unit_set_new_location(g, *Bg, sx, sy, true, true);
			clear_orders(g, *M);
			unit_go_inside(g, *M, b, gr.who);
		} else {
			object_die(g, *Bg, 0, -1, 0.0f);
		}
	}
}

void group_action_board_ship(Game& g, Group& gr, int32_t ship_o, QueuePos q) {
	// @700010.
	if (g.scen.ignore_orders != 0 && uint32_t(gr.who) < 8) for (size_t k = 0; k < g.scen.objects_ignoring_orders[size_t(gr.who)].size(); k++) if (g.scen.objects_ignoring_orders[size_t(gr.who)][k] >= 0) group_kill(g, gr, g.scen.objects_ignoring_orders[size_t(gr.who)][k], gr.who, 0, 0);  // ScenarioData::ignore_orders prelude
	gr.disband = 0;  // action_begin
	if (q == kQueueFirst) {
		// ❓ the decompile runs finish_insert on a temporary group holding the ship; it is run on the group here like the
		// other actions.
		std::vector<Order> saved = group_set_up_insert(g, gr);
		group_action_halt(g, gr, 0);
		group_action_board_ship(g, gr, ship_o, kQueueNew);
		group_finish_insert(g, gr, saved);
		return;
	}
	gr.form = -1;  // group +0x10
	Unit* S = ship_o >= 0 ? g.unit(gr.who, ship_o) : nullptr;
	if (!S) return;
	const int32_t rs = region_of_tile_at(g, S->x, S->y);
	int32_t full = 0, count = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map() || m->ptype->domain == kDomainAir || is_busy(*m)) continue;
		if (!g.regions.is_coast(region_of_tile_at(g, m->x, m->y), rs)) continue;
		if (!can_carry_unit(g, *S, m->o, gr.who)) {
			full++;
			continue;
		}
		add_board_order(g, *m, ship_o, gr.who, q == kQueueLast ? kQueueLast : kQueueNew);
		if (count == 0) clear_orders(g, *S);
		count++;
		add_await_board_order(g, *S, m->o, gr.who, true);
	}
	(void)full;  // (console: "ship full" feedback)
}

}  // namespace ron::sim
