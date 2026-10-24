// The ground halves of PATROL and ATTACK_GROUND (docs/orders-spec.md 3, docs/aircraft-spec.md 3 / 8; decompiles in
// re/decomp/aircraft/ and re/decomp/gaps/): Group::action_patrol's ground branch, Unit::add_patrol_order (GROUP_PATROL),
// Unit::update_patrol_order, Unit::do_patrol, Group::redo_patrol_order, Group::action_attack_ground's ground branch,
// UnitTypeData::can_attack_ground, Unit::add_attack_ground_order and Unit::do_attack_ground.
//
// A GROUP_PATROL order keeps its waypoints in `wp` (world units, cell centres: ucoord * 0x30 + 0x18), the current one in
// `wp_index`, and the GroupOrder fields of the original in `oxx` / `whose` (the group's leader), `id` (the command id) and
// `form_id` (the member's list index). An ATTACK_GROUND order keeps its point in `x` / `y`, the AttackGroundOrder accuracy
// (+0xc) in `tolerance` and attack_unit (+0x10) in `attempts` (fields no other ground order uses; the save walks them).
#include "sim/orders.hpp"

#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/combat_internal.hpp"
#include "sim/compass.hpp"
#include "sim/game.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/placement.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace combat_detail;

namespace {

constexpr int32_t kTypeCarrier = 0x15f, kTypeSiegeWeapon = 0x7b;
constexpr uint32_t kMissileMask = 0x8000000u;

// LeaderData::is_peace(a, b) @6e1200 (the form aircraft_cmd.cpp uses).
bool is_peace_between(const Game& g, int32_t a, int32_t b) {
	const int32_t dab = g.leaders[size_t(a)].diplos(b), dba = g.leaders[size_t(b)].diplos(a);
	return !(a == b || dab == 0 || dba == 0 || (dab == 2 && dba == 2));
}

// GroupData::is_on_map @70c450 (groups.cpp keeps its twin file-local).
bool group_on_map(Game& g, const Group& gr) {
	if (gr.num == 0) return false;
	if (gr.buildings) return true;
	for (int32_t i = 0; i < gr.num; i++) {
		const Unit* u = g.unit(gr.who, gr.list[size_t(i)]);
		if (u && u->alive() && u->o_up < 0 && u->on_map()) return true;
	}
	return false;
}

int32_t owner_of_point(const Game& g, int32_t x, int32_t y) { return g.world.w(x / 768, y / 768).who; }

}  // namespace

// ---------------------------------------------------------------- UnitTypeData::can_attack_ground @61db20

bool type_can_attack_ground(const Game& g, const Type& t) {
	if (!war_allowed(g)) return false;  // the rush-rules prelude (age below the rule, or minutes not yet passed)
	if (t.max_range == 0) return false;
	if (t.unit_flags & 0x102000u) return false;  // FIREWORK-style: fireship 0x2000, submarine 0x100000
	if (uint32_t(t.obj_masks) & (objmask::kExplosive | objmask::kSiege | objmask::kNaval | objmask::kBombard)) return true;
	return type_is(t, kTypeSiegeWeapon, false);
}

// ---------------------------------------------------------------- PATROL (GROUP_PATROL)

Order* update_patrol_order(Unit& u, int32_t id) {
	// Unit::update_patrol_order @5e35e0: the first GROUP_PATROL order (id -1) or the one whose command id matches.
	for (Order& o : u.orders)
		if (o.type == kGroupPatrol && (id == -1 || o.id == id)) return &o;
	return nullptr;
}

Order* add_group_patrol_order(Game& g, Unit& u, int32_t ux0, int32_t uy0, int32_t ux1, int32_t uy1, int32_t id,
                              int32_t index, int32_t leader_o, int32_t leader_who, QueuePos q) {
	// Unit::add_patrol_order @5e4560: two waypoints (cell units to world cell centres), waypoint 0, explicit.
	Order p;
	p.type = kGroupPatrol;
	p.flags = orderflag::kExplicit;
	p.oxx = leader_o;
	p.whose = leader_who;
	p.id = id;
	p.form_id = index;
	p.wp.clear();
	p.wp.push_back({ux0 * 0x30 + 0x18, uy0 * 0x30 + 0x18});
	p.wp.push_back({ux1 * 0x30 + 0x18, uy1 * 0x30 + 0x18});
	p.wp_index = 0;
	return add_order(g, u, p, q);
}

void do_patrol(Game& g, Unit& u, Order& P) {
	// Unit::do_patrol @5f1910.
	if (u.group < 0) {
		// Alone: walk to the next waypoint with an ATTACK_TO move in front (the patrol order stays behind it).
		int32_t i = P.wp_index + 1;
		if (i >= int32_t(P.wp.size())) i = 0;
		P.wp_index = i;
		const int32_t wx = P.wp[size_t(i)][0], wy = P.wp[size_t(i)][1];
		const uint32_t a = find_angle(wx - u.x, wy - u.y);
		add_move_facing_order(g, u, ucoord_of(wx), ucoord_of(wy), a, 2, false, kQueueFirst, false, -1, -1, -1, false);
	} else if (P.oxx == u.o && P.whose == u.who) {
		// The group's leader: advance, then the whole group moves there (ATTACK_TO, QUEUE_FIRST: the insert pattern re-adds the
		// patrol orders behind the move through Group::redo_patrol_order with this waypoint index).
		int32_t i = P.wp_index + 1;
		if (i >= int32_t(P.wp.size())) i = 0;
		P.wp_index = i;
		const int32_t wx = P.wp[size_t(i)][0], wy = P.wp[size_t(i)][1];
		group_action_move_to(g, g.groups.g[size_t(u.group)], wx, wy, kQueueFirst, false, 0, kAttackTo, 0, -1, -1, 0);
	} else {
		unit_set_anim(g, u, kCharDefault, 0, 1);  // a follower waits for the leader's group move
	}
	if (u.inside_down >= 0 && type_is(*u.ptype, kTypeCarrier, true)) {
		// A carrier on patrol scrambles its wing every time.
		Group tmp;
		group_clear(tmp, -1);
		tmp.stamp = 0;
		group_add(g, tmp, u.o, u.who, 0, 0);
		group_action_scramble(g, tmp);
	}
}

void group_action_ground_patrol(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q) {
	// Group::action_patrol @7030c0, the branch for groups without a plane.
	const int32_t li = group_find_leader(g, gr);
	if (li < 0) return;
	const int32_t lo = gr.list[size_t(li)];
	Unit* L = g.unit(gr.who, lo);
	if (!L) return;
	int32_t sx = L->x, sy = L->y;
	bool leader_patrols = false;
	if (q == kQueueLast) {
		if (group_get_loc_to(g, gr, &sx, &sy) != 0) return;
		leader_patrols = update_patrol_order(*L, -1) != nullptr;
	} else {
		if (group_get_loc(g, gr, &sx, &sy) != 0) return;
	}
	const int32_t id_base = (gr.id + g.frame * 10) * 100 + gr.order_num;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive()) continue;
		if (!m->on_map()) {
			// A helicopter inside a container is sent to the point.
			if (m->ptype->unit_flags & utflag::kHelicopter) add_move_order(g, *m, x, y, 1, false, kQueueNew, true, -1, -1);
			continue;
		}
		if (unit_is_plane(*m) || cast_is_busy(*m)) continue;
		Order* mp = leader_patrols ? update_patrol_order(*m, -1) : nullptr;
		if (mp) {
			mp->wp.push_back({x, y});  // another waypoint at the end of the patrol
		} else {
			add_group_patrol_order(g, *m, ucoord_of(sx), ucoord_of(sy), ucoord_of(x), ucoord_of(y), id_base, i, lo, gr.who, q);
		}
	}
	gr.order_num++;
}

void group_redo_patrol_order(Game& g, Group& gr, const Order& S) {
	// Group::redo_patrol_order @706d90: the QUEUE_FIRST insert pattern gives every member its patrol order back, with the
	// saved waypoints and the waypoint reached.
	group_normalize(g, gr);
	if (S.wp.size() < 2) return;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map() || unit_is_plane(*m) || (uint32_t(m->ptype->obj_masks) & kMissileMask)) continue;
		add_group_patrol_order(g, *m, ucoord_of(S.wp[0][0]), ucoord_of(S.wp[0][1]), ucoord_of(S.wp[1][0]),
		                       ucoord_of(S.wp[1][1]), S.id, S.form_id, S.oxx, S.whose, kQueueLast);
		if (Order* np = update_patrol_order(*m, S.id)) {
			for (size_t k = 2; k < S.wp.size(); k++) np->wp.push_back(S.wp[k]);
			np->wp_index = S.wp_index;
		}
	}
}

// ---------------------------------------------------------------- ATTACK_GROUND

void add_attack_ground_order(Game& g, Unit& u, int32_t x, int32_t y, QueuePos q, bool expl, int32_t attack_unit,
                             int32_t accuracy) {
	// Unit::add_attack_ground_order @5e4290.
	Order a;
	a.type = kAttackGround;
	a.x = x;
	a.y = y;
	a.attempts = attack_unit;
	a.tolerance = accuracy;
	if (expl) a.flags |= orderflag::kExplicit;
	add_order(g, u, a, q);
}

void do_attack_ground(Game& g, Unit& u, Order& A) {
	// Unit::do_attack_ground @5f1410.
	const int32_t X = A.x, Y = A.y;
	if (!type_can_attack_ground(g, *u.ptype)) {
		kill_current_order(g, u, false);
		return;
	}
	const int32_t attack_unit = A.attempts;
	if (attack_unit == 0) {
		const int32_t owner = owner_of_point(g, X, Y);
		if (owner >= 0 && is_peace_between(g, u.who, owner)) {
			kill_current_order(g, u, false);
			return;
		}
	}
	uint32_t angle = find_angle(X - u.x, Y - u.y);
	if (u.ptype->unit_flags & utflag::kAttackSideways) {
		const uint32_t a1 = angle + 0xc0000000u, a2 = angle + 0x40000000u;
		angle = abs_angle(u.angle - a1) < abs_angle(u.angle - a2) ? a1 : a2;
	}
	if (attack_unit == 0 && !is_in_range_point(g, u, X, Y, u.x, u.y)) {
		// Out of range: walk to the nearest free spot from which the point is in range (a MOVE_TO in front).
		const int32_t d = vector_dist(X - u.x, Y - u.y) - u.ptype->big_radius;
		const int32_t minr = obj_min_range(g, u), maxr = obj_max_range(g, u);
		int32_t ring;
		if (d < minr * 192 - 6) ring = minr * 192 + 0x90;
		else if (maxr * 192 - 6 < d) ring = maxr * 192 - 0x30;
		else ring = d;
		int32_t sx = 0, sy = 0;
		if (find_nearby_spot(g, *u.ptype, X, Y, &sx, &sy, ring, -1, 0, angle + 0x80000000u, 3, u.o, u.who, 0, 0, -1, 0, -1) == 0 &&
		    is_in_range_point(g, u, X, Y, sx, sy)) {
			add_move_facing_order(g, u, ucoord_of(sx), ucoord_of(sy), angle, 1, false, kQueueFirst, false, -1, -1, -1, false);
			return;
		}
		// (message: the point cannot be reached, for the local player)
		kill_current_order(g, u, false);
		return;
	}
	if (u.recharging != 0) {
		if (A.flags & orderflag::kFlag80) return;  // already fired for this order
		if (!u.guys.empty() && u.guys[0].cur_anim >= 0 && u.guys[0].cur_anim <= 3) return;
		unit_set_anim(g, u, kCharDefault, 1, 1);
		return;
	}
	if (A.attempts > 0) {
		if (A.attempts == 1) {
			kill_current_order(g, u, false);
			work(g, u);  // vtable +0x188
			return;
		}
		A.attempts = 1;
	}
	set_attack(g, u, -1, -1);
	if ((u.ptype->unit_flags2 & utflag2::kPacker) && (u.unit_masks & umask::kPacked)) {
		add_cast_order(g, u, -1, -1, -1, -1, 0x28c, kQueueFirst, false);  // unpack first
		return;
	}
	u.unit_masks |= 0x11000u;
	if (angle != u.angle) unit_set_angle(g, u, angle, false);
	A.flags |= orderflag::kFlag80;
	if (u.ptype->unit_flags & utflag::kRockingAttack) {
		const uint32_t d = find_angle(X - u.x, Y - u.y) - angle;
		unit_set_anim(g, u, int32_t(d) >= 0 ? kCharAttack2 : kCharAttack1, 0, 0);
	} else {
		unit_set_anim(g, u, kCharAttack1, 0, 1);
	}
	if (!g.fire_proj.empty() && g.fire_proj[size_t(u.type)] >= 0) fire_ammo(g, u, -1, -1);
	u.recharging = uint8_t(unit_recharge(g, u, nullptr) + 1);
}

void group_action_ground_attack_ground(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q) {
	// Group::action_attack_ground @704520, the branch for a group of ground units: every member that can shoot at the
	// ground (and is not busy with another thing) gets the order.
	if (!group_on_map(g, gr)) return;
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map() || unit_is_plane(*m)) continue;
		if (!type_can_attack_ground(g, *m->ptype)) continue;
		add_attack_ground_order(g, *m, x, y, q, true, 0, 0);
	}
}

}  // namespace ron::sim
