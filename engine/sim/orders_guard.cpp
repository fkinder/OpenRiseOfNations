// FOLLOW and GUARD (docs/sim-implementation.md "GUARD / FOLLOW"): the unit side. Unit::add_follow_order @5e3f60,
// Unit::add_guard_order @5e3e40, Unit::update_guard_order @5e3220, Unit::do_follow @5e65d0 and Unit::do_guard @5e5c70,
// ported from re/decomp/fidelity2 (decompile + disassembly: the sinx / cosx / vector_dist / find_angle arguments are read from
// the asm). The group side (Group::action_follow / action_guard) is in groups.cpp, the verbs in commands.cpp.
//
// A GuardOrder keeps its members in Order fields it does not otherwise use (orders.hpp: dx / dy = def_x / def_y, the
// guard point = x / y, idle = timer, retry = pause), a FollowOrder in oxx / whose / uid2.
#include <algorithm>
#include <cstdlib>

#include "sim/combat.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

constexpr int32_t kFilterNotMeSpot = 3;  // FILTER_NOT_ME as the original pushes it
constexpr uint32_t kSpotAngle = 0x55555555u;

// vtable +8 / +0xbc of an object: valid (active) and on the map (buildings count as on the map).
bool obj_active_on_map(const Object* ob) {
	if (!ob || !ob->alive()) return false;
	return ob->is_unit ? static_cast<const Unit*>(ob)->on_map() : true;
}

}  // namespace

// ---------------------------------------------------------------- the order adders

Order* add_follow_order(Game& g, Unit& u, int32_t ox, int32_t whom, QueuePos q) {
	// @5e3f60
	if (q == kQueueNew) clear_orders(g, u);
	Order o;
	o.type = kFollow;
	o.ox = ox;
	o.whom = whom;
	const Object* t = (ox >= 0 && whom >= 0) ? g.object(whom, ox) : nullptr;
	o.uid = t ? t->uid : uint16_t(0xffff);
	// The object really followed: the first unit riding inside the target (a transport), else the target itself.
	if (!t || t->inside_down < 0) {
		o.oxx = ox;
		o.whose = whom;
		o.uid2 = o.uid;
	} else {
		o.oxx = t->inside_down;
		o.whose = t->inside_down_who;
		const Object* in = g.object(o.whose, o.oxx);
		o.uid2 = in ? in->uid : uint16_t(0xffff);
	}
	o.flags |= orderflag::kExplicit;
	return add_order(g, u, o, q == kQueueNew ? kQueueLast : q);
}

Order* add_guard_order(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t dx, int32_t dy, QueuePos q) {
	// @5e3e40
	if (q == kQueueNew) clear_orders(g, u);
	Order o;
	o.type = kGuard;
	o.ox = ox;
	o.whom = whom;
	const Object* t = (ox >= 0 && whom >= 0) ? g.object(whom, ox) : nullptr;
	if (t) {
		o.uid = t->uid;
		o.x = t->x;  // guard_x / guard_y
		o.y = t->y;
	} else {
		o.uid = 0xffff;
		o.x = u.x;
		o.y = u.y;
	}
	o.def_x = dx;
	o.def_y = dy;
	o.timer = 0;  // idle
	o.pause = 0;  // retry
	o.flags |= orderflag::kExplicit;
	return add_order(g, u, o, q == kQueueNew ? kQueueLast : q);
}

Order* update_guard_order(Game& g, Unit& u, int32_t ox, int32_t whom) {
	// @5e3220: the unit's own GUARD order on (ox, whom), or null.
	(void)g;
	for (Order& o : u.orders)
		if (o.type == kGuard && o.ox == ox && o.whom == whom) return &o;
	return nullptr;
}

// ---------------------------------------------------------------- do_follow @5e65d0

void do_follow(Game& g, Unit& u, Order& F) {
	int32_t ox = F.ox, whom = F.whom;
	if (ox >= 0) {
		// A target that is inside a carrier is followed through its container (the real target is remembered).
		const Object* T = g.object(whom, ox);
		const Unit* TU = T && T->is_unit ? static_cast<const Unit*>(T) : nullptr;
		if (!obj_active_on_map(T) && T && T->alive() && TU && TU->inside_up >= 0) {
			int32_t cwho = 0;
			const int32_t cont = unit_get_inside(g, *TU, &cwho);
			const Object* C = g.object(cwho, cont);
			if (obj_active_on_map(C)) {
				F.uid2 = F.uid;
				F.whose = whom;
				F.oxx = ox;
				F.ox = cont;
				F.whom = cwho;
				F.uid = C->uid;
				ox = cont;
				whom = cwho;
			}
		}
	}
	const Object* T = ox >= 0 ? g.object(whom, ox) : nullptr;
	if (ox >= 0 && obj_active_on_map(T)) {
		if (F.oxx != ox || F.whose != whom) {
			const Object* R = g.object(F.whose, F.oxx);
			if (R && R->uid == F.uid2) {
				// Still the remembered object: follow its captain from now on.
				const Unit* RU = g.unit(F.whose, F.oxx);
				const int32_t c = RU ? obj_captain(g, *RU) : F.oxx;
				F.oxx = c;
				const Object* CO = g.object(F.whose, c);
				F.uid2 = CO ? CO->uid : uint16_t(0xffff);
			} else {
				F.oxx = ox;
				F.whose = whom;
				F.uid2 = F.uid;
			}
		}
		if (!object_is_seen(g, *T, u.who, false)) {
			kill_current_order(g, u);
			return;
		}
		const int32_t dist = vector_dist(std::abs(T->x - u.x), std::abs(T->y - u.y));
		const Unit* TU = T->is_unit ? static_cast<const Unit*>(T) : nullptr;
		int32_t base;
		if (TU && unit_speed(g, *TU) < unit_speed(g, u)) base = object_los(g, u) * 0x60;
		else base = (object_los(g, u) * 0x300) / 5;
		if (TU && unit_is_moving(*TU)) base *= 2;
		const int32_t d1 = std::min(std::max(object_los(g, u) * 0x180 - base, 0x180), 0x600);
		if (dist <= d1 + 0xc0) {
			unit_set_anim(g, u, kCharDefault, 0, 1);
			return;
		}
		// A spot d1 from the target on the line to the unit, else d1 behind the target, else on a ring around it.
		int32_t px = 0, py = 0, sx = 0, sy = 0;
		const uint32_t toward = find_angle(u.x - T->x, u.y - T->y);
		project(T->x, T->y, toward, d1, &px, &py);
		bool found = find_nearby_spot(g, *u.ptype, px, py, &sx, &sy, 0, -1, 0, kSpotAngle, kFilterNotMeSpot, u.o, u.who, 0, 0, -1, 0, -1) == 0;
		if (!found) {
			const uint32_t tang = TU ? TU->angle : 0u;
			project(T->x, T->y, tang - 0x80000000u, d1, &px, &py);
			found = find_nearby_spot(g, *u.ptype, px, py, &sx, &sy, 0, -1, 0, kSpotAngle, kFilterNotMeSpot, u.o, u.who, 0, 0, -1, 0, -1) == 0;
			if (!found)
				found = find_nearby_spot(g, *u.ptype, T->x, T->y, &sx, &sy, d1, d1 + 0xc0, 0x30, TU ? TU->angle : 0u, kFilterNotMeSpot, u.o,
				                         u.who, 0, 0, -1, 0, -1) == 0;
			if (!found) {
				sx = T->x;
				sy = T->y;
			}
		}
		add_move_facing_order(g, u, ucoord_of(sx), ucoord_of(sy), TU ? TU->angle : 0u, 1, false, kQueueFirst, false, -1, -1, -1, false);
		if (Order* m = update_order(u)) do_move(g, u, *m);
		return;
	}
	// The target is gone (or not on the map): fall back to the remembered object, else the order ends.
	if (F.oxx == ox && F.whose == whom) {
		kill_current_order(g, u);
		return;
	}
	F.whom = F.whose;
	F.ox = F.oxx;
	F.uid = F.uid2;
	work(g, u);
}

// ---------------------------------------------------------------- do_guard @5e5c70

void do_guard(Game& g, Unit& u, Order& G) {
	const int32_t ox = G.ox, whom = G.whom;
	if (ox < 0) {
		unit_set_anim(g, u, kCharDefault, 0, 1);
		kill_current_order(g, u);
		return;
	}
	const Object* T = g.object(whom, ox);
	if (!T || !T->alive()) {
		unit_set_anim(g, u, kCharDefault, 0, 1);
		kill_current_order(g, u);
		return;
	}
	if (G.pause != 0) {  // retry: frames to wait before the next move
		G.pause--;
		return;
	}
	const Unit* TU = T->is_unit ? static_cast<const Unit*>(T) : nullptr;
	bool moving = false;
	if (obj_active_on_map(T) && TU && unit_is_moving(*TU)) {
		moving = true;
	} else {
		const int32_t v = u.o + g.frame;
		if (((v + 8) & 15) == 0) {
			find_melee_target(g, u, -1, nullptr, false, true, 0);
			return;
		}
		if ((v & 15) == 0) {
			G.timer++;  // idle
			return;
		}
	}
	if (!T->is_unit) {
		// Guarding a building: stay within 0x600 of it, else walk (as a one-unit group) to a spot around it.
		if (attack_dist(g, u, ox, whom) <= 0x600) return;
		if (u.o_up >= 0) return;  // only captains move
		const Type& tt = *T->ptype;
		int32_t sx = 0, sy = 0, cx, cy;
		if (find_nearby_spot(g, *u.ptype, T->x, T->y, &sx, &sy, (tt.x_size + tt.y_size) * 0x30, (tt.y_size + 0x20 + tt.x_size) * 0x30, 0,
		                     kSpotAngle, kFilterNotMeSpot, u.o, u.who, 0, 1, -1, 0, -1) == 0) {
			cx = sx;
			cy = sy;
		} else {
			cx = T->x;
			cy = T->y;
		}
		Group tmp;
		group_clear(tmp, -1);
		group_add(g, tmp, u.o, u.who, 0, 0);
		const int32_t id = groups_push_group(g, u.who, tmp, 1);
		if (id >= 0) group_action_move_near(g, g.groups.g[size_t(id)], cx, cy, 0, kQueueNew, false, 0, kMoveTo, 0, -1, -1, 0);
		return;
	}
	// Guarding a unit: stand at its (dx, dy) slot, turned with it.
	int32_t ddx = G.def_x;
	if (TU->unit_masks & umask::kFacing) ddx = -ddx;
	const uint32_t tang = TU->angle;
	int32_t gx = sin_a(tang, G.def_y) + TU->x + cos_a(tang, ddx);
	int32_t gy = TU->y - cos_a(tang, G.def_y) + sin_a(tang, ddx);
	int32_t ux = ucoord_of(gx), uy = ucoord_of(gy);
	if (unit_invalid_loc(g, u, ux >> 2, uy >> 2, false, false, false, false, false) != 0) {
		int32_t cx, cy;
		const World& w = g.world;
		const bool water = w.in_t(ux >> 2, uy >> 2) && (w.t(ux >> 2, uy >> 2) & 0x30) == 0x20;
		if (!(u.ptype->unit_flags & 0x10) || water) {
			int32_t sx = 0, sy = 0;
			if (find_nearby_spot(g, *u.ptype, ux * 0x30 + 0x18, uy * 0x30 + 0x18, &sx, &sy, 0xc0, 0x180, 0x60, kSpotAngle, kFilterNotMeSpot,
			                     u.o, u.who, 0, 0, -1, 0, -1) == 0) {
				cx = sx;
				cy = sy;
			} else {
				const int32_t d = vector_dist(G.def_x, G.def_y);
				if (find_nearby_spot(g, *u.ptype, TU->x, TU->y, &sx, &sy, d, d + 0x180, 0xc0, kSpotAngle, kFilterNotMeSpot, u.o, u.who, 0, 0,
				                     -1, 0, -1) == 0) {
					cx = sx;
					cy = sy;
				} else {
					cx = u.x;
					cy = u.y;
				}
			}
		} else {
			cx = ux * 0x30 + 0x18;
			cy = uy * 0x30 + 0x18;
		}
		ux = ucoord_of(cx);
		uy = ucoord_of(cy);
	}
	const int32_t spot_x = ux * 0x30 + 0x18, spot_y = uy * 0x30 + 0x18;
	G.x = spot_x;  // guard_x / guard_y
	G.y = spot_y;
	uint32_t A = moving ? TU->angle : find_angle(spot_x - TU->x, spot_y - TU->y);
	if ((u.unit_masks & umask::kAiControl) &&
	    ((u.ptype->unit_flags & 0x20000) || (u.ptype->unit_flags2 & 0x40) || (u.ptype->unit_flags2 & 0x20)))
		A = TU->angle;  // AI siege, supply and heroes copy the guarded unit's facing
	if (ucoord_of(u.x) != ux || ucoord_of(u.y) != uy) {
		G.timer = 0;  // idle
		add_move_facing_order(g, u, ux, uy, A, 2, false, kQueueFirst, false, -1, -1, -1, false);
		Order* m0 = update_order(u);
		const int32_t wd = std::abs(wcoord_of(u.y) - wcoord_of(spot_y)) + std::abs(wcoord_of(u.x) - wcoord_of(spot_x));
		if (m0) m0->timer = std::max(1, wd) * 0x1e;
		if (Order* m = update_order(u)) do_move(g, u, *m);
		if (u.order_type() == kGuard) {
			unit_set_anim(g, u, kCharDefault, 0, 1);
			if (Order* gg = update_order(u)) gg->pause = int32_t(g.rng.get(0, 0xffff) % 3) + 6;  // retry
		}
		return;
	}
	if (u.angle != A && !moving) unit_set_angle(g, u, A, false);
	G.timer++;  // idle
	if ((u.ptype->unit_flags2 & 4) && unit_packer_stance(u) == packer::kAuto && (u.unit_masks & umask::kPacked) && !cast_is_unpacking(u)) {
		const int32_t lim = unit_is(g, u, 0x7b) ? 0x1e : 0x46;
		if (G.timer >= lim) {
			add_cast_order(g, u, -1, -1, -1, -1, 0x28c, kQueueFirst, false);
			return;
		}
	}
	unit_set_anim(g, u, kCharDefault, 0, 1);
}

}  // namespace ron::sim
