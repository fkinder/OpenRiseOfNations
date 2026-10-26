// The order list and the per-frame order machine (docs/orders-spec.md sections 3-6).
// The original's OrderList is a circular list whose execution order is head_node->prev first; here it is a deque in
// execution order (front() = current order). QUEUE_FIRST is push_front, every other add is push_back.
#include "sim/orders.hpp"
#include <cstdio>
#include <cstdlib>
#include "sim/animals.hpp"
#include "sim/production.hpp"

#include "sim/aircraft.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/spells.hpp"
#include "sim/transport.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- predicates (UnitOrder virtuals)

bool order_is_move(OrderType t) {
	return t == kMoveTo || t == kAttackTo || t == kExploreTo || t == kFleeTo || t == kChangeForm || t == kGroupMove ||
	       t == kGroupAttackTo;
}
bool order_is_targeted(OrderType t) {
	switch (t) {
	case kBuildAt: case kGather: case kBoardShip: case kAwaitBoard: case kAttack: case kFollow: case kGuard:
	case kRepair: case kCastSpell: case kTradeRoute: case kStrafe: case kGroupAttack: case kGarrison:
		return true;
	default:
		return false;
	}
}
bool order_is_attack(OrderType t) {
	return t == kAttack || t == kStrafe || t == kGroupAttack || t == kAttackGround || t == kAirAttackGround;
}
bool order_is_group(OrderType t) { return t == kGroupMove || t == kGroupAttack || t == kGroupAttackTo || t == kGroupPatrol; }

// ---------------------------------------------------------------- adding

void clear_orders(Game& g, Unit& u) {
	u.unit_masks &= ~umask::kMultimove;
	u.path.clear();
	close_orders(g, u, false);
	clear_partial_path(u);
	update_action(g, u);
}

Order* add_order(Game& g, Unit& u, const Order& o, QueuePos q) {
	if (q == kQueueNew) clear_orders(g, u);
	Order* r;
	if (q == kQueueFirst) {
		// orderlist.add then head_node = head_node->next: the new node becomes the front.
		clear_partial_path(u);
		u.orders.push_front(o);
		r = &u.orders.front();
	} else {
		u.orders.push_back(o);
		r = &u.orders.back();
	}
	update_action(g, u);
	return r;
}

Order* add_move_facing_order(Game& g, Unit& u, int32_t ux, int32_t uy, uint32_t angle, int32_t kind, bool pathed,
                             QueuePos q, bool expl, int32_t facing, int32_t orig_x, int32_t orig_y, bool disembark) {
	// @5e55c0. QUEUE_LAST plain moves of explorers (role & 0x10) become EXPLORE_TO with UNIT_MULTIMOVE.
	bool readd_attack = false;
	int32_t att_ox = -1, att_whom = -1;
	if (q != kQueueNew) {
		if (q == kQueueLast && kind == 1 && (u.ptype->role & 0x10)) {
			u.unit_masks |= umask::kMultimove;
			kind = 3;
		}
	} else {
		// Attack-move while attacking a valid target: the attack is re-added in front afterwards.
		if (kind == 2 && u.order_type() == kAttack) {
			att_ox = u.front()->ox;
			att_whom = u.front()->whom;
			if (att_ox >= 0 && att_whom >= 0) {
				const Object* t = g.object(att_whom, att_ox);
				// An air target only when the unit can hit air (ObjMask ANTI_AIR).
				if (t && t->ptype && (t->ptype->domain != 2 || (uint32_t(u.ptype->obj_masks) & 0x80000000u)))
					readd_attack = true;
			}
		}
		clear_orders(g, u);
	}
	Order m;
	m.type = kind == 2 ? kAttackTo : kind == 3 ? kExploreTo : kind == 4 ? kFleeTo : kMoveTo;
	// MoveOrder::clear @4889c0, then the fields add_move_facing_order writes.
	m.x = ux * 48 + 24;
	m.y = uy * 48 + 24;
	m.angle = angle;
	m.dest = 0;
	m.dest_x = m.x;
	m.dest_y = m.y;
	m.last_x = -1;
	m.last_y = -1;
	m.coll_x = m.coll_y = 0;
	m.tolerance = 0;
	m.attempts = 0;
	m.flags = uint8_t((pathed ? orderflag::kPathed : 0) | (expl ? orderflag::kExplicit : 0) |
	                  (disembark ? orderflag::kDisembark : 0));
	m.off_x = int16_t(m.x - (m.x / 0x300) * 0x300);
	m.off_y = int16_t(m.y - (m.y / 0x300) * 0x300);
	m.orig_x = orig_x;
	m.orig_y = orig_y;
	m.facing = facing;
	m.pause = m.retry = m.timer = 0;
	Order* r;
	if (q == kQueueFirst) {
		clear_partial_path(u);
		u.orders.push_front(m);
		r = &u.orders.front();
	} else {
		u.orders.push_back(m);
		r = &u.orders.back();
	}
	if (readd_attack) {
		update_action(g, u);
		add_attack_order(g, u, att_ox, att_whom, kQueueFirst, 0, false);  // (deque: r stays valid)
	}
	update_action(g, u);
	return r;
}

Order* add_move_order(Game& g, Unit& u, int32_t x, int32_t y, int32_t kind, bool pathed, QueuePos q, bool expl,
                      int32_t orig_x, int32_t orig_y) {
	// @616ed0: facing angle from the unit to the target, facing = -1.
	const uint32_t a = find_angle(x - u.x, y - u.y);
	return add_move_facing_order(g, u, ucoord_of(x), ucoord_of(y), a, kind, pathed, q, expl, -1, orig_x, orig_y, false);
}

void add_think_order(Game& g, Unit& u) {
	Order t;
	t.type = kThink;
	t.flags = orderflag::kExplicit;  // always set on THINK
	add_order(g, u, t, kQueueFirst);
}

// ---------------------------------------------------------------- finishing

void kill_current_path(Game& g, Unit& u) {
	(void)g;
	// @5e31d0: pop up to and including the first final (flag 1) entry.
	if (u.path.empty()) return;
	while (!u.path.empty()) {
		const bool fin = (u.path.back().flags & pathflag::kFinal) != 0;
		u.path.pop_back();
		if (fin) break;
	}
	clear_partial_path(u);
}

void kill_current_order(Game& g, Unit& u, bool no_side_effects) {
	u.unit_masks &= ~umask::kCasting;
	if (u.orders.empty()) {
		clear_partial_path(u);
		update_action(g, u);
		return;
	}
	Order& cur = u.orders.front();
	if (order_is_move(cur.type)) {
		if (cur.facing >= 0 && !no_side_effects) {
			// TODO(orders-spec 4.3): reversing() inverts the facing bit; reversing is not traced (taken as 0).
			if (cur.facing != 0) u.unit_masks |= umask::kFacing;
			else u.unit_masks &= ~umask::kFacing;
			if (u.group >= 0) {
				Group& gr = g.groups.g[size_t(u.group)];
				int32_t lead = -1;
				if (gr.num >= 1) {
					const int32_t li = gr.buildings ? 0 : group_find_leader(g, gr);
					if (li >= 0) lead = gr.list[size_t(li)];
				}
				if (lead == u.o) gr.facing = int8_t(cur.facing);
			}
		}
	} else if (cur.type == kGather) {
		gather_order_killed(g, u, cur);  // LEADER_RECOMPUTE, Build::remove_gatherer, UNIT_WITH_STUFF, doobers
	} else if (cur.type == kTradeRoute) {
		end_trade_route(g, u);
	}
	// CAST_SPELL side effects (spells-spec 7): a paid order gives its cost back (unpay_cast_costs; a no-op for TRANSPORT:
	// empty cost, mana 0), and a merchant killing a pack cast refreshes its gpiece afterwards (scene.recalc_builds).
	bool merchant_packing = false;
	if (cur.type == kCastSpell) {
		if (std::getenv("RON_SPELL_TRACE")) std::fprintf(stderr, "[spell] f%d %d:%d CAST_SPELL 0x%x order removed (paid %d, spell_time %d)\n", g.frame, u.who, u.o, unsigned(cur.spell), cur.paid, int(u.spell_time));
		if (cur.paid != 0 && !no_side_effects && !g.scenario_loading) unpay_cast_costs(g, cur.spell, u.o, u.who);
		merchant_packing = unit_is_merchant(u) && cast_is_packing(u);
	}
	// SPECIAL_ANIM: TODO (the landing-order bookkeeping lives in aircraft.cpp).
	const bool pathed = order_is_pathed(cur);
	u.orders.pop_front();
	if (pathed) kill_current_path(g, u);
	clear_partial_path(u);
	update_action(g, u);
	if (merchant_packing && !g.scenario_loading) unit_update_gpiece(g, u);
}

void close_orders(Game& g, Unit& u, bool x) {
	while (!u.orders.empty()) kill_current_order(g, u, x);
}

void repath(Game& g, Unit& u) {
	// @5e29b0: drop the move-family orders at the front (CHANGE_FORM only without its explicit bit). A GROUP_MOVE of
	// a grouped unit goes through Group::kill_group_move.
	for (;;) {
		if (u.orders.empty()) return;
		const Order& f = u.orders.front();
		const OrderType t = f.type;
		const bool movefam = t == kMoveTo || t == kAttackTo || t == kExploreTo || t == kFleeTo || t == kChangeForm ||
		                     t == kGroupMove || t == kGroupAttackTo;
		if (!movefam) return;
		if (t == kChangeForm && (f.flags & orderflag::kExplicit)) return;
		if (t == kGroupMove && u.group >= 0) {
			const size_t before = u.orders.size();
			group_kill_group_move(g, g.groups.g[size_t(u.group)], f.id);
			if (u.orders.size() == before) kill_current_order(g, u, false);  // guard against a non-member
			continue;
		}
		kill_current_order(g, u, false);
	}
}

void kill_current_action(Game& g, Unit& u) {
	repath(g, u);
	kill_current_order(g, u, false);
}

Order* update_order(Unit& u) { return u.front(); }

namespace {

// The walk shared by update_action (writes orders_x/orders_y/dest_angle) and get_action.
template <class U, class O>
O* walk_action(U& u, int32_t* ox, int32_t* oy, uint32_t* da) {
	*ox = u.x;
	*oy = u.y;
	*da = u.angle;
	if (u.orders.empty()) return nullptr;
	const size_t last = u.orders.size() - 1;
	size_t i = 0;
	auto rec = [&](const Order& o) {
		*ox = o.x;
		*oy = o.y;
		*da = o.angle;
	};
	if (!(i == last && !order_is_move(u.orders[0].type))) {
		while (((order_is_move(u.orders[i].type) && !(u.orders[i].flags & orderflag::kExplicit)) ||
		        u.orders[i].type == kChangeForm) &&
		       i != last) {
			if (order_is_move(u.orders[i].type)) rec(u.orders[i]);
			i++;
		}
		if (order_is_move(u.orders[i].type) && (i == last || (u.orders[i].flags & orderflag::kExplicit))) rec(u.orders[i]);
	}
	O& o = u.orders[i];
	if ((order_is_move(o.type) && !(o.flags & orderflag::kExplicit)) || o.type == kChangeForm) return nullptr;
	return &o;
}

}  // namespace

Order* update_action(Game& g, Unit& u) {
	(void)g;
	int32_t ox, oy;
	uint32_t da;
	Order* r = walk_action<Unit, Order>(u, &ox, &oy, &da);
	u.orders_x = ox;
	u.orders_y = oy;
	u.dest_angle = da;
	return r;
}

const Order* get_action(const Unit& u) {
	int32_t ox, oy;
	uint32_t da;
	return walk_action<const Unit, const Order>(u, &ox, &oy, &da);
}

OrderType action_type(const Unit& u) {
	const Order* a = get_action(u);
	return a ? a->type : kOrderNone;
}

// ---------------------------------------------------------------- the order machine

void work(Game& g, Unit& u) {
	// @60d180
	Order* cur = u.front();
	OrderType type = cur ? cur->type : kOrderNone;
	if ((g.frame + u.o) % 32 == 0) {
		if (!(u.flags & objflag::kAttacking)) u.visible = 0;
		u.unit_masks &= ~umask::kInCombat;
	}
	if (type != kAttack) {
		u.flags &= uint8_t(~objflag::kAttacking);
		if ((u.unit_masks & umask::kAutoExplore) && cur && (cur->flags & orderflag::kExplicit) && type != kExploreTo)
			u.unit_masks &= ~umask::kAutoExplore;
		// A melee unit still recovering from its last blow waits unless its order is group level.
		if (u.recharging && u.ptype->max_range == 0 && !(u.ptype->unit_flags & 0x400)) {
			if (!cur || !(cur->flags & orderflag::kExplicit)) return;
		}
	}
	// The caravan part of Unit::work: a trade route lives only while its TRADE_ROUTE order is the action; the road search runs a slice.
	caravan_work(g, u);
	if (order_is_move(type)) {
		if (type == kMoveTo && (u.unit_masks & umask::kMultimove)) {
			// Re-issue the front move as EXPLORE_TO in front.
			const Order m = u.orders.front();
			u.orders.pop_front();
			add_move_facing_order(g, u, ucoord_of(m.x), ucoord_of(m.y), m.angle, 3, order_is_pathed(m), kQueueFirst,
			                      (m.flags & orderflag::kExplicit) != 0, m.facing, -1, -1, false);
			cur = update_order(u);
			type = cur ? cur->type : kOrderNone;
		}
		const bool packer = (u.ptype->unit_flags2 & 4) != 0;
		if (!packer || (u.unit_masks & umask::kPacked)) {
			if ((g.frame + u.o) % 16 == 0) {
				Order* act = update_action(g, u);
				if (act && order_is_targeted(act->type) && act->type != kAwaitBoard) {
					bool replanned = false;
					if (act->type == kGuard) {
						if ((g.frame + u.o) % 64 == 0) {
							repath(g, u);
							replanned = true;
						}
					} else if (cur && cur->type != kChangeForm &&
					           (!order_is_group(cur->type) || (cur->oxx == u.o && cur->whose == u.who))) {
						if (check_target_path(g, u, *act)) replanned = true;  // combat-spec 3.7
					}
					if (replanned) {
						cur = update_order(u);
						type = cur ? cur->type : kOrderNone;
					}
				}
			}
		} else {
			// An unpacked packer packs up before it moves (orders-spec 5, @60d180): a lone explicit move that ends within the
			// unit's block radius is rewritten to end where the unit stands; a move to its own 48-cell is simply finished.
			bool done = false;
			if (cur && (cur->flags & orderflag::kExplicit) && u.orders.size() == 1 &&
			    vector_dist(cur->x - u.x, cur->y - u.y) < u.ptype->block_radius) {
				cur->x = u.x;
				cur->y = u.y;
				cur->dest_x = u.x;
				cur->dest_y = u.y;
				cur->last_x = u.x;
				cur->last_y = u.y;
				cur->tolerance = 0;
				if (order_is_pathed(*cur) && !u.path.empty()) {
					while (!u.path.empty() && !(u.path.back().flags & pathflag::kFinal)) u.path.pop_back();
					if (!u.path.empty()) {
						PathData p = u.path.back();
						u.path.pop_back();
						p.x = u.x;
						p.y = u.y;
						path_push(u, p);
					}
				}
				done = true;
			}
			if (!done && cur) {
				if (ucoord_of(u.x) == ucoord_of(cur->x) && ucoord_of(u.y) == ucoord_of(cur->y)) {
					kill_current_order(g, u, false);
					return;
				}
				add_cast_order(g, u, -1, -1, -1, -1, 0x28b, kQueueFirst, false);
				cur = update_order(u);
				type = cur ? cur->type : kOrderNone;
			}
		}
	}
	// LAB_0060d733 (both the move branch and the others reach it): an idle unit with an order stops idling.
	if (type != kOrderNone && (u.flags & objflag::kIdling)) {
		const int32_t t = u.type;
		if (t == 0x3d || t == 0x3e || t == 400 || unit_is(g, u, 0x13d)) g.leaders[size_t(u.who)].flags |= leaderflag::kRecompute;
		u.flags &= uint8_t(~objflag::kIdling);
	}
	if (u.openlist && cur && !order_is_move(cur->type)) clear_partial_path(u);
	Order* act = update_action(g, u);
	if (!act || !order_is_targeted(act->type)) {
		if (type != kCastSpell) u.spell_time = 0;
	} else {
		if (type != kCastSpell && act->type != kCastSpell && u.who < 8) u.spell_time = 0;
		if (act->ox >= 0 && act->whom >= 0) {
			if (act->type == kAttack) {
				set_in_danger(g, u, false);
				Object* to = g.object(act->whom, act->ox);
				if (to && to->is_unit) set_in_danger(g, *static_cast<Unit*>(to), false);
			}
			const Object* t = g.object(act->whom, act->ox);
			if (!t || t->uid != act->uid) {
				if (act->type == kStrafe) {
					act->ox = act->whom = -1;  // the strafe has no target: it flies home
					act->returning = 1;
				} else if (order_is_attack(act->type)) {
					if (act == update_order(u)) {
						act->ox = act->whom = -1;  // the attack handler retargets next frame
						return;
					}
				} else {
					repath(g, u);
					kill_current_order(g, u, false);
					return;
				}
			}
			// TODO(orders-spec 5): TRADE_ROUTE second endpoint check.
		}
	}
	// A unit that carries aircraft (a carrier; airbases and silos launch from Build::process) lets one plane out.
	if (u.inside_down >= 0 && can_carry_air(u)) do_launch(g, u);
	if (u.safe) u.safe--;
	if (type != kOrderNone) {
		u.idle = 0;
		const uint32_t uf = u.ptype->unit_flags;
		if (!(uf & 0x4000) && (uf & 0x40000) && !(u.unit_masks & umask::kUnderCover) &&
		    !(u.unit_masks2 & 0x8000))
			u.unit_masks |= umask::kBlowCover;
	}
	if (g.frame - 4 < u.collide_frame) {
		const bool pusher = u.ptype->domain == 1 || (u.ptype->unit_flags2 & 0x40) || (u.ptype->unit_flags2 & 0x20);
		// TODO: the siege-type (vfunc +0x10c) pusher test.
		if (pusher && !order_is_move(type)) detect_boat_collision(g, u, u.x, u.y, false);
	}
	do_job(g, u, type, cur);
	// TODO(orders-spec 5): helicopter separation.
	u.unit_masks &= ~umask::kZoc;
}

int32_t do_job(Game& g, Unit& u, OrderType t, Order* o) {
	switch (t) {
	case kOrderNone:
		if (is_animal_who(u.who)) animal_do_idle(g, u);  // vslot +0x184: Animal::do_idle @5d7460 (sim/animals.cpp)
		else do_idle(g, u);
		return 0;
	case kMoveTo:
		return do_move(g, u, *o);
	case kFleeTo:
		return do_flee_to(g, u, *o);
	case kAttackTo:
		return do_attack_to(g, u, *o);
	case kAttack:
		return do_attack(g, u, *o);
	case kGroupAttack:
		return do_group_attack(g, u, *o);
	case kExploreTo:
		return do_explore_to(g, u, *o);
	case kGroupMove:
		return do_group_move(g, u, *o);
	case kGroupAttackTo: {
		// Unit::do_group_attack_to @5e74e0: the group move, then the scan while the order is still current.
		const int32_t r = do_group_move(g, u, *o);
		if (!u.orders.empty() && &u.orders.front() == o) do_group_attack_to_scan(g, u, *o);
		return r;
	}
	case kThink:
		do_think_order(g, u);  // Unit::do_think_order @5e5bf0 (econ_gather.cpp)
		return 0;
	case kGather:
		return do_gather(g, u, *o);  // Unit::do_gather @5ef2a0 (economy-spec 4.2)
	case kTradeRoute:
		return do_trade(g, u, *o);  // Unit::do_trade @5ed270 (economy-spec 6.3)
	case kBuildAt:
		return do_build(g, u, *o);  // Unit::do_build @5eebf0 (production-spec 4.1)
	case kRepair:
		return do_repair(g, u, *o);  // Unit::do_repair @5ee420 (4.8)
	case kGarrison:
		return do_garrison(g, u, *o);  // Unit::do_garrison @5e6b80 (garrison-spec 4)
	case kBoardShip:
		return do_board(g, u, *o);  // Unit::do_board @5ed1f0 (transport-spec 8)
	case kAwaitBoard:
		return do_await_board(g, u, *o);  // Unit::do_await_board @5ed040
	case kCastSpell:
		return do_cast(g, u, *o);  // Unit::do_cast @5ebfe0 (transport-spec 4)
	case kFollow:
		do_follow(g, u, *o);  // Unit::do_follow @5e65d0 (orders_guard.cpp)
		return 0;
	case kGuard:
		do_guard(g, u, *o);  // Unit::do_guard @5e5c70 (orders_guard.cpp)
		return 0;
	case kGroupPatrol:
		do_patrol(g, u, *o);  // Unit::do_patrol @5f1910 (orders_patrol.cpp)
		return 0;
	case kAttackGround:
		do_attack_ground(g, u, *o);  // Unit::do_attack_ground @5f1410 (orders_patrol.cpp)
		return 0;
	case kSpecialAnim:
		do_spec_anim(g, u, *o);  // Unit::do_spec_anim @5e5880 (aircraft-spec 6): landing, take-off, the walk-out
		return 0;
	case kStrafe:
		do_strafe(g, u, *o);  // Unit::do_strafe @5eab00 (aircraft-spec 5)
		return 0;
	case kAirPatrol:
		if (is_animal_who(u.who)) animal_think_bird(g, u, *o);  // vslot +0x180 (a no-op for ordinary units)
		do_air_patrol(g, u, *o);  // Unit::do_air_patrol @5ea620
		return 0;
	case kAirAttackGround:
		do_air_attack_ground(g, u, *o);  // Unit::do_air_attack_ground @5ea420
		return 0;
	case kChangeForm:
		// TODO(orders-spec 6): do_form_change. Not created by the commands implemented so far.
		kill_current_order(g, u, false);
		return 0;
	default:
		// TODO(orders-spec 6): patrol, ... are not simulated yet; such orders are never created by the implemented commands.
		return 0;
	}
}

void do_idle(Game& g, Unit& u) {
	// @60dcd0
	const bool march = (u.unit_masks2 & 0x8000) != 0;
	if (march) u.unit_masks2 &= ~0x8000u;
	// TODO: heroes keep an ATTACK2 pose (UnitAnimCat of guy 0's animation).
	unit_set_anim(g, u, kCharDefault);
	u.collide = 0;
	check_idle(g, u);
	think(g, u);
	if (march) u.unit_masks2 |= 0x8000u;
}

void check_idle(Game& g, Unit& u) {
	// @6032c0
	const uint8_t idle = u.idle;
	if (idle == 0xff) {
		u.idle = 1;
	} else if (idle <= 1 || (u.o + g.frame) % 16 == 0) {
		u.idle = uint8_t(idle + 1);
		if (u.idle == 4 && (u.unit_masks & umask::kEntrenched)) unit_set_angle(g, u, u.trench_angle, false);
	}
	// TODO(orders-spec 6.1): marines entrench, cloak particles (UI).
	if (!(u.flags & objflag::kIdling)) {
		u.flags |= objflag::kIdling | objflag::kNewThink;
		const int32_t t = u.type;
		if (unit_is(g, u, 0x13d) || t == 0x3d || t == 0x3e || t == 400) g.leaders[size_t(u.who)].flags |= leaderflag::kRecompute;
	}
}

void think(Game& g, Unit& u) {
	// Unit::think @5f6e40: the combat parts (combat_target.cpp, combat-spec 2.2). TODO(orders-spec 6.1): auto-gather,
	// scouts, AI-controlled economy behaviour (several branches draw game_random).
	think_combat(g, u);
}

}  // namespace ron::sim
