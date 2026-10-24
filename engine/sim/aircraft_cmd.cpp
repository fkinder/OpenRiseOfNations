// Aircraft commands (docs/aircraft-spec.md section 8; decompiles in re/decomp/aircraft, disassembly in its dis/ directory):
// FLIGHT (Group::action_flight / action_launch_flight), PATROL and LAUNCH_PATROL (action_patrol / action_air_patrol /
// action_launch_patrol), ATTACK_GROUND (action_attack_ground / action_air_attack_ground), SCRAMBLE and RECALL
// (action_scramble / action_recall / action_return), and the carrier half of UNQUEUE (Unit::action_unqueue).
//
// The console feedback of the original (the "out of fuel", "out of range", "defended", "no nukes" messages and their
// sounds) has no simulation effect and is not modelled; each place where one is printed is marked "(message)".
#include <algorithm>
#include <cstdint>

#include "sim/aircraft.hpp"
#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/combat_internal.hpp"
#include "sim/compass.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/pathfinder.hpp"
#include "sim/production.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace combat_detail;

namespace {

constexpr int32_t kTypeBiplane = 0x11f, kTypeBomber = 0x130, kTypeFighterBomber = 0x134, kTypeHelicopter = 0x136,
                  kTypeCruise = 0x139, kTypeNuke = 0x13b, kTypeAirbase = 0x1bf, kTypeSilo = 0x208;
constexpr int32_t kMissileDefenseBonusTech = 0x2b5;  // LeaderData::has_preq(MISSILE_DEFENSE_BONUS)
constexpr uint32_t kMissileMask = 0x8000000u;

bool is_missile(const Unit& u) { return (uint32_t(u.ptype->obj_masks) & kMissileMask) != 0; }
bool is_heli(const Unit& u) { return (u.ptype->unit_flags & utflag::kHelicopter) != 0; }

// UnitData::is_busy @60a370 / is_entering_or_exiting @60a6f0 (packers.cpp): a pack / unpack cast, or a SPECIAL_ANIM of type 0 / 1.
bool busy(const Unit& u) { return cast_is_busy(u); }

// GroupData::member(o, who, 1).
bool in_group(const Game& g, const Group& gr, int32_t o, int32_t who) {
	if (who != gr.who || o < 0) return false;
	const Object* ob = g.object(gr.who, o);
	if (!ob || !ob->alive()) return false;
	for (int32_t i = 0; i < gr.num; i++)
		if (gr.list[size_t(i)] == o) return true;
	return false;
}

// LeaderData::is_peace(a, b) @6e1200 (the form used by aircraft.cpp).
bool is_peace_between(const Game& g, int32_t a, int32_t b) {
	const int32_t dab = g.leaders[size_t(a)].diplos(b), dba = g.leaders[size_t(b)].diplos(a);
	return !(a == b || dab == 0 || dba == 0 || (dab == 2 && dba == 2));
}

void clamp_to_world(const Game& g, int32_t& x, int32_t& y) {
	x = std::max(x, 0);
	y = std::max(y, 0);
	if (x >= g.world.xs * 0x300) x = g.world.xs * 0x300 - 1;
	if (y >= g.world.ys * 0x300) y = g.world.ys * 0x300 - 1;
}

// The leader's unit of a group: the list entry find_leader names (units groups), the first one for buildings.
int32_t leader_index(Game& g, const Group& gr) {
	if (gr.num < 1) return -1;
	return gr.buildings ? 0 : group_find_leader(g, gr);
}

// A helicopter sent to a point instead of flying a patrol: Unit::close_orders / clear_partial_path / update_action then a
// MOVE_TO order on the centre of the ucoord cell with `angle` (explicit, not pathed, facing -1; the open-coded
// MoveOrder of Group::action_scramble @7111c0 and action_launch_patrol @703580).
void heli_move_to(Game& g, Unit& u, int32_t x, int32_t y, uint32_t angle) {
	add_move_facing_order(g, u, ucoord_of(x), ucoord_of(y), angle, 1, false, kQueueNew, true, -1, -1, -1, false);
}

void launch_list_remove(Object& c, int32_t o) {
	auto it = std::find(c.launch_list.begin(), c.launch_list.end(), o);
	if (it != c.launch_list.end()) c.launch_list.erase(it);
}

// (cost / 4 rounded towards zero, the helicopter and fighter weights.)
int32_t quarter(int32_t v) { return (v + ((v >> 31) & 3)) >> 2; }

}  // namespace

// ---------------------------------------------------------------- Unit::action_unqueue @5e1f20

void unit_action_unqueue(Game& g, Unit& u) {
	// The queued-aircraft counter of a carrier (BUILDAIR, group_action_buildair): one plane less, the clock restarts when
	// the queue empties, the leader's per-type and per-class counters (track_queued) and the price back at the current
	// cost (Type::unpay_cost(who, -1, -1), the original's refund argument is 1 from both callers).
	if (u.num_queued == 0) return;
	u.num_queued--;
	if (u.num_queued == 0) u.queue_time = 0;
	const int32_t f = current_upgrade(g, u.who, airtype::kFighterBomber);
	track_queued(g, u.who, f, -1);
	if (f >= 0) unpay_cost(g, f, u.who, -1, -1);
}

// Group::action_spell @6fe1a0, the aircraft branch (a unit type of domain 2 as the "spell"): BUILDAIR queues the leader's
// current FIGHTERBOMBER on every carrier of the group that may cast 0x295, up to 10 queued and the carrier's room. The
// first plane that cannot be paid for ends the command (a `return`, not a `continue`).
void group_action_buildair(Game& g, Group& gr) {
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* u = g.unit(gr.who, gr.list[size_t(i)]);
		if (!u || !u->alive() || !u->on_map()) continue;
		if (spell_is_castable(g, 0x295, u->o, gr.who, 0) == 0) continue;
		if (u->num_queued >= 10) continue;  // (message: queue full)
		if (num_aircraft_here(g, *u, 0) >= num_aircraft_limit(g, *u)) continue;  // (message: carrier full)
		const int32_t f = current_upgrade(g, gr.who, airtype::kFighterBomber);
		if (can_pay_cost(g, f, gr.who, -1, -1, 1) == 0) return;
		pay_cost(g, f, gr.who, -1, -1, 1, nullptr);
		u->num_queued++;
		track_queued(g, gr.who, f, 1);
	}
}

// Unit::process @611158: a carrier with planes queued (and an active leader) builds one every train_time(f) frames of
// accel_train (x ai_speed); a full population holds the clock back one step and counts a pop issue.
void unit_process_air_queue(Game& g, Unit& u) {
	if (u.num_queued == 0 || !(g.leaders[size_t(u.who)].flags & leaderflag::kActive)) return;
	const int32_t f = current_upgrade(g, u.who, airtype::kFighterBomber);
	const int32_t tt = train_time(g, u.o, u.who, f);
	int32_t acc = RON_K(g, "accel_train");
	if (g.ai_speed > 1) acc *= g.ai_speed;
	u.queue_time += acc;
	if (u.queue_time < tt) return;
	Leader& L = g.leaders[size_t(u.who)];
	if (L.pop_cap < g.type(f).control_cost + L.control) {
		L.pop_issues++;  // (the pop-limit message and sound of the local player)
		u.queue_time -= acc;
		return;
	}
	u.num_queued--;
	u.queue_time = 0;
	track_queued(g, u.who, f, -1);
	const int32_t a = init_unit(g, u.who, f, u.x, u.y, -1, -1, -1);
	if (a >= 0) unit_go_inside(g, *g.unit(u.who, a), u.o, u.who);
}

// ---------------------------------------------------------------- FLIGHT: Group::action_flight @6fb260

void group_action_flight(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t order, int32_t shift, int32_t ctrl,
                         int32_t alt) {
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	gr.disband = 0;  // action_begin
	if (ox < 0 || whom < 0) return;
	Object* T = obj_at(g, ox, whom);
	if (!T || !T->alive()) return;
	if (order == kMoveTo && !(gr.who == whom && can_carry_air(*T))) return;
	if (gr.buildings) {
		group_action_launch_flight(g, gr, ox, whom, order, shift, ctrl, alt);
		return;
	}
	if (order == kAttack) {
		// Planes fly the attack; every other member is split off to Group::action_attack / action_guard.
		Group planes, others;
		group_clear(planes, -1);
		group_clear(others, -1);
		planes.id = others.id = gr.id;
		planes.stamp = others.stamp = 0;
		for (int32_t i = 0; i < gr.num; i++) {
			const int32_t o = gr.list[size_t(i)];
			const Object* ob = g.object(gr.who, o);
			const Unit* m = g.unit(gr.who, o);
			if (!ob || !ob->alive() || !m) continue;
			group_add(g, unit_is_plane(*m) ? planes : others, o, gr.who, 0, 0);
		}
		if (others.num != 0) {
			planes.facing = others.facing = gr.facing;
			group_action_flight(g, planes, ox, whom, kAttack, 0, 0, 0);
			if (!is_ally(g, gr.who, whom)) group_action_attack(g, others, ox, whom, 1, kQueueNew, 0);
			// ❓ an ally target sends the others to Group::action_guard @6fcd30, which is not ported.
			gr.facing = others.facing;
			return;
		}
	}
	bool nuke_done = false;
	const bool nukes_only = order == kAttack && group_count(g, gr, kCountType, kTypeNuke) != 0;
	gr.form = -1;
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t mo = gr.list[size_t(i)];
		const Object* mob = g.object(gr.who, mo);
		if (!mob || !mob->alive()) continue;
		Unit* M = g.unit(gr.who, mo);
		if (!M) continue;
		if (order == kMoveTo) {
			if (is_missile(*M)) continue;
			if (!air_can_carry(g, *T, mo, gr.who)) continue;
			// A fighter-bomber is only handled by the carrier it is based on already.
			if (type_is(*M->ptype, kTypeFighterBomber, false) && unit_home_base(g, *M, nullptr) != ox) continue;
		}
		if (nukes_only && !type_is(*M->ptype, kTypeNuke, false)) continue;
		const OrderType ot = M->order_type();
		if (ot == kStrafe) {
			Order& S = M->orders.front();
			if (order == kAttack) {
				if (!is_missile(*M)) {
					S.ox = ox;
					S.whom = whom;
					S.uid = T->uid;
					S.xx = T->x;
					S.yy = T->y;
					if (unit_mana_left(g, *M) != 0) {  // else (message: out of fuel)
						S.returning = 0;
						S.mandatory = 1;
						S.flags |= orderflag::kExplicit;
					}
				}  // else (message: a missile cannot be retargeted)
			} else if (!is_missile(*M) || S.ox < 0 || S.whom < 0) {
				if (unit_mana_left(g, *M) == 0 && S.oxx >= 0 && S.whose >= 0) {
					// No fuel: the plane may not be sent to a base farther away than the one it is heading for.
					const Object* H = obj_at(g, S.oxx, S.whose);
					if (H && H->alive()) {
						const int32_t d_new = vector_dist(T->x - M->x, T->y - M->y);
						const int32_t d_old = vector_dist(H->x - M->x, H->y - M->y);
						if (d_new > d_old) continue;  // (message)
					}
				}
				S.whose = whom;
				S.ox = -1;
				S.whom = -1;
				S.oxx = ox;
				S.returning = 1;
				S.xx = S.yy = -1;
				S.mandatory = 1;
				S.flags |= orderflag::kExplicit;
			}  // else (message)
			continue;
		}
		int32_t home_o, home_who = -1;
		if (!order_is_air(ot)) {
			home_o = unit_get_inside(g, *M, &home_who);
		} else {
			if (is_missile(*M)) continue;
			home_o = M->orders.front().oxx;
			home_who = M->orders.front().whose;
		}
		if (home_o < 0 && order != kMoveTo) continue;
		if (is_missile(*M) && order == kAttack) {
			if (!valid_target(g, *M, ox, whom, nullptr)) continue;
			if (whom != gr.who && has_preq(g, whom, kMissileDefenseBonusTech)) continue;  // (message: defended)
			const Object* H = obj_at(g, home_o, home_who);
			if (!H) continue;
			const int32_t dist = vector_dist(H->x - T->x, H->y - T->y);
			if (dist > unit_mana(g, *M) * get_speed(g, *M, M->x, M->y, true)) continue;  // (message: out of range)
			if (type_is(*M->ptype, kTypeNuke, false) && !nuke_done) {
				nuke_done = true;
				if (get_units(g, gr.who, kTypeNuke, 0) == 0) continue;  // LeaderData::can_nuke (message)
			}
		} else if (order == kMoveTo) {
			int32_t w2 = -1;
			const int32_t in = unit_get_inside(g, *M, &w2);
			if (!(in == ox && w2 == whom)) add_strafe_order(g, *M, -1, -1, ox, whom, 1, kQueueNew, true);
			if (nuke_done) return;
			continue;
		}
		if (!is_ally(g, gr.who, whom) && !war_allowed(g)) {
			if (nuke_done) return;
			continue;
		}
		add_strafe_order(g, *M, ox, whom, home_o, home_who, 1, kQueueNew, true);
		if (nuke_done) return;
	}
	// (message: no war allowed yet, for an attack on a non-ally)
}

// ---------------------------------------------------------------- Group::action_launch_flight @6fbfb0

void group_action_launch_flight(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t order, int32_t shift, int32_t ctrl,
                                int32_t alt) {
	Group flight;
	group_clear(flight, -1);
	if (!gr.buildings) return;
	bool all_nukes = true, any_nuke = false, any_cruise = false;
	const bool airbases = group_count(g, gr, kCountType, kTypeAirbase) != 0;
	Object* T = obj_at(g, ox, whom);
	if (!T) return;
	if (order == kAttack) {
		for (int32_t i = 0; i < gr.num; i++) {
			const Object* B = g.object(gr.who, gr.list[size_t(i)]);
			if (!B || !B->alive()) continue;
			if (count_inside(g, *B, kCountType, kTypeNuke) == 0) {
				const int32_t cruise = count_inside(g, *B, kCountType, kTypeCruise);
				all_nukes = false;
				if (cruise != 0) any_cruise = true;
			} else {
				any_nuke = true;
			}
		}
	} else {
		all_nukes = false;
		if (type_is(*T->ptype, kTypeSilo, false) && num_inside(g, *T, 1) != 0) any_cruise = any_nuke = true;
	}
	int32_t best_o = -1, best_w = -1;
	int32_t best_cost = order == kAttack ? 9999999 : -1;
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* B = g.object(gr.who, gr.list[size_t(i)]);
		if (!B) continue;
		int32_t cur = B->inside_down, curw = B->inside_down_who;
		for (int32_t guard = 0; cur >= 0 && curw >= 0 && guard < kUnitLimit; guard++) {
			Unit* P = g.unit(curw, cur);
			if (!P) break;
			const int32_t next = P->inside_down, nextw = P->inside_down_who;
			cur = next;
			curw = nextw;
			if (P->ptype->domain != kDomainAir) continue;
			if (all_nukes && !type_is(*P->ptype, kTypeNuke, false)) continue;
			if (busy(*P)) continue;
			if (!any_nuke && !any_cruise) {
				if (airbases && (alt != 0 || ctrl != 0) &&
				    !type_is(*P->ptype, alt != 0 ? kTypeBiplane : kTypeBomber, false))
					continue;
				if (shift == 0 && P->mana_burn != 0) continue;  // (message: refuelling)
			} else if (!is_missile(*P)) {
				continue;
			}
			// The distance counts from the launching building (the member), not the plane.
			int32_t cost = vector_dist(B->x - T->x, B->y - T->y);
			bool pick = false;
			if (order == kAttack) {
				if (!war_allowed(g)) continue;  // (message: rush rules)
				if (!valid_target(g, *P, ox, whom, nullptr) && any_cruise) continue;  // (message)
				if (cost > unit_mana(g, *P) * get_speed(g, *P, P->x, P->y, true)) continue;  // (message: out of range)
				if (!any_cruise && !any_nuke) {
					bool prefer;
					if (is_ally(g, gr.who, whom)) prefer = type_is(*P->ptype, kTypeBiplane, false);
					else prefer = type_is(*P->ptype, T->is_unit ? kTypeBiplane : kTypeBomber, false);
					if (prefer) cost = quarter(cost);
					if (P->order_type() != kOrderNone) cost *= 200;
					pick = true;
				} else if (!P->orders.empty() && P->order_type() != kOrderNone) {
					continue;
				} else {
					pick = true;
				}
				if (pick) {
					if (shift == 0) {
						if (cost < best_cost) {
							best_o = P->o;
							best_w = P->who;
							best_cost = cost;
						}
					} else {
						group_add(g, flight, P->o, P->who, 0, 0);
					}
				}
			} else if (!is_missile(*P) && air_can_carry(g, *T, P->o, P->who)) {
				if (shift == 0) {
					if (best_cost < cost) {
						best_o = P->o;
						best_w = P->who;
						best_cost = cost;
					}
				} else {
					group_add(g, flight, P->o, P->who, 0, 0);
				}
			}
		}
	}
	if (best_o >= 0) group_add(g, flight, best_o, best_w, 0, 0);
	if (flight.num == 0) return;  // (message: nothing could fly)
	group_action_flight(g, flight, ox, whom, order, 0, 0, 0);
}

// ---------------------------------------------------------------- PATROL: Group::action_air_patrol @7029d0

void group_action_air_patrol(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q) {
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	Group planes, others;
	group_clear(planes, -1);
	group_clear(others, -1);
	planes.id = others.id = gr.id;
	planes.stamp = others.stamp = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t o = gr.list[size_t(i)];
		const Object* ob = g.object(gr.who, o);
		const Unit* m = g.unit(gr.who, o);
		if (!ob || !ob->alive() || !m) continue;
		group_add(g, unit_is_plane(*m) ? planes : others, o, gr.who, 0, 0);
	}
	if (others.num != 0) {
		planes.facing = others.facing = gr.facing;
		group_action_air_patrol(g, planes, x, y, q);
		group_action_move_to(g, others, x, y, q, false, 0, kMoveTo, 0, -1, -1, 0);
		gr.facing = others.facing;
		return;
	}
	gr.disband = 0;
	clamp_to_world(g, x, y);
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t o = gr.list[size_t(i)];
		Unit* M = g.unit(gr.who, o);
		if (!M || !M->alive() || !unit_is_plane(*M)) continue;
		if (busy(*M) || is_missile(*M)) continue;
		// (message: out of fuel, when mana - mana_burn <= 0 for a fixed-wing plane)
		if (!M->orders.empty() && M->order_type() == kAirPatrol && q == kQueueLast) {
			// Another waypoint on the patrol in progress, relative to its base when that is alive.
			Order& P = M->orders.front();
			int32_t wx = x, wy = y;
			if (P.oxx >= 0 && P.whose >= 0) {
				const Unit* H = g.unit(P.whose, P.oxx);  // vslot 8 is is_live_unit: only a carrier
				if (H && H->alive()) {
					wx = x - H->x;
					wy = y - H->y;
				}
			}
			P.wp.push_back({wx, wy});
			P.flags |= orderflag::kExplicit;
			continue;
		}
		int32_t home_o, home_who = -1;
		if (!M->orders.empty() && (M->order_type() == kStrafe || M->order_type() == kAirPatrol ||
		                           M->order_type() == kAirAttackGround)) {
			home_o = M->orders.front().oxx;
			home_who = M->orders.front().whose;
		} else {
			home_o = unit_get_inside(g, *M, &home_who);
		}
		if (home_o >= 0 && home_who >= 0) add_air_patrol_order(g, *M, x, y, home_o, home_who, true);
	}
}

// Group::action_patrol @7030c0, the air half: a group with a plane (leader or member, COUNT_DOMAIN 4) patrols in the air.
// The ground half (GroupPatrolOrder / Unit::add_patrol_order) is orders_patrol.cpp.
void group_action_patrol(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q) {
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	gr.disband = 0;
	clamp_to_world(g, x, y);
	const QueuePos q2 = q == kQueueFirst ? kQueueNew : q;
	if (gr.buildings) return;
	gr.form = -1;
	const int32_t li = leader_index(g, gr);
	if (li < 0) return;
	bool air = false;
	const Unit* L = g.unit(gr.who, gr.list[size_t(li)]);
	if (L && unit_is_plane(*L)) air = true;
	for (int32_t i = 0; !air && i < gr.num; i++) {
		const Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (m && m->alive() && unit_is_plane(*m)) air = true;
	}
	if (air) group_action_air_patrol(g, gr, x, y, q2);
	else group_action_ground_patrol(g, gr, x, y, q2);
}

// ---------------------------------------------------------------- LAUNCH_PATROL: Group::action_launch_patrol @703580

void group_action_launch_patrol(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q, int32_t shift, int32_t ctrl,
                                int32_t alt) {
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	bool launched = false;
	int32_t best_cost = 9999999, best_o = -1, best_w = -1;
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t bo = gr.list[size_t(i)];
		const Object* B = g.object(gr.who, bo);
		if (!B) continue;
		int32_t cur = B->inside_down, curw = B->inside_down_who;
		for (int32_t guard = 0; cur >= 0 && curw >= 0 && guard < kUnitLimit; guard++) {
			Unit* P = g.unit(curw, cur);
			if (!P) break;
			cur = P->inside_down;
			curw = P->inside_down_who;
			if (!P->alive() || P->ptype->domain != kDomainAir || busy(*P) || is_missile(*P)) continue;
			if ((alt != 0 || ctrl != 0) && !type_is(*P->ptype, alt != 0 ? kTypeBiplane : kTypeBomber, false)) continue;
			if (shift == 0 && P->mana_burn != 0) continue;  // (message: refuelling)
			int32_t cost = vector_dist(P->x - x, P->y - y);
			if (type_is(*P->ptype, kTypeBiplane, false)) cost /= 10;
			else if (type_is(*P->ptype, kTypeHelicopter, false)) cost = quarter(cost);
			if (!P->orders.empty() && P->order_type() != kOrderNone) cost *= 200;
			if (shift == 0 && cost < best_cost) {
				best_o = P->o;
				best_w = P->who;
				best_cost = cost;
			}
			if (q == kQueueLast || shift != 0) {
				launched = true;
				if (!is_heli(*P)) add_air_patrol_order(g, *P, x, y, bo, gr.who, true);
				else heli_move_to(g, *P, x, y, find_angle(x - P->x, y - P->y));
			}
		}
	}
	if (best_o < 0 && !launched) return;  // (message: nobody could launch)
	if (q == kQueueLast || shift != 0) return;
	Unit* P = g.unit(best_w, best_o);
	if (!P) return;
	int32_t cw = -1;
	const int32_t co = unit_get_inside(g, *P, &cw);
	if (!is_heli(*P)) add_air_patrol_order(g, *P, x, y, co, cw, true);
	else heli_move_to(g, *P, x, y, find_angle(x - P->x, y - P->y));
}

// ---------------------------------------------------------------- ATTACK_GROUND: Group::action_attack_ground @704520

void group_action_air_attack_ground(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q) {
	// @703d80. Planes and missiles bomb a map point; one missile per command.
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	gr.disband = 0;
	bool missile_sent = false;
	clamp_to_world(g, x, y);
	const bool nukes = group_count(g, gr, kCountType, kTypeNuke) != 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t mo = gr.list[size_t(i)];
		Unit* M = g.unit(gr.who, mo);
		if (!M || !M->alive() || !unit_is_plane(*M)) continue;
		if (is_missile(*M)) {
			// A missile already in its silo's launch list is on its way.
			int32_t cw = -1;
			const int32_t in = unit_get_inside(g, *M, &cw);
			if (in >= 0) {
				const Object* C = obj_at(g, in, cw);
				if (C && C->alive() && std::find(C->launch_list.begin(), C->launch_list.end(), mo) != C->launch_list.end())
					continue;
			}
		}
		if (busy(*M)) continue;
		if (nukes && !type_is(*M->ptype, kTypeNuke, false)) continue;
		if (missile_sent) continue;
		int32_t home_o, home_who = -1;
		const OrderType ot = M->order_type();
		if (ot == kStrafe || ot == kAirPatrol || ot == kAirAttackGround) {
			home_o = M->orders.front().oxx;
			home_who = M->orders.front().whose;
		} else {
			home_o = unit_get_inside(g, *M, &home_who);
		}
		if (home_o < 0 || home_who < 0) continue;
		if (!is_missile(*M)) {
			// (message: out of fuel, when mana_left == 0)
			add_air_attack_ground_order(g, *M, x, y, home_o, home_who, q, true);
			continue;
		}
		if (!war_allowed(g)) continue;  // (message: no war yet)
		// The owner of the target cell; over open water the first land cell of the surrounding spiral decides.
		const int32_t wx = x / 768, wy = y / 768;
		int32_t owner;
		if (g.world.is_ocean(wx, wy)) {
			owner = -1;
			for (int32_t k = 1; k < 121; k++) {
				const int32_t cx = wx + move_x[k], cy = wy + move_y[k];
				if (!g.world.in_w(cx, cy) || g.world.is_ocean(cx, cy)) continue;
				owner = g.world.w(cx, cy).who;
				if (owner >= 0) break;
			}
		} else {
			owner = g.world.w(wx, wy).who;
		}
		if (owner >= 0) {
			if (owner != gr.who && has_preq(g, owner, kMissileDefenseBonusTech)) continue;  // (message: defended)
			if (owner != gr.who && is_peace_between(g, gr.who, owner)) continue;            // (message: no war)
		}
		if (vector_dist(M->x - x, M->y - y) > unit_mana(g, *M) * get_speed(g, *M, M->x, M->y, true)) continue;  // (message)
		missile_sent = true;
		add_air_attack_ground_order(g, *M, x, y, home_o, home_who, q, true);
	}
}

// Group::action_attack_ground (ATTACK_GROUND, verb 9) @704520, the air half: planes and missiles (or the contents of
// airbases, carriers and silos) bomb the point; a group of ground units gets Unit::add_attack_ground_order on the members
// that can shoot at the ground (orders_patrol.cpp).
void group_action_attack_ground(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q) {
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	gr.disband = 0;
	if (gr.num == 0) return;
	clamp_to_world(g, x, y);
	const int32_t owner = g.world.w(x / 768, y / 768).who;
	if (owner >= 0 && owner != gr.who && is_peace_between(g, gr.who, owner)) return;  // (message: no war)
	if (gr.num < 1) return;
	const int32_t li = leader_index(g, gr);
	if (li < 0) return;
	if (!gr.buildings) {
		const Object* L = g.object(gr.who, gr.list[size_t(li)]);
		const Unit* LU = g.unit(gr.who, gr.list[size_t(li)]);
		if (!L) return;
		if (!can_carry_air(*L)) {
			if (!LU || !unit_is_plane(*LU)) {  // the ground half
				group_action_ground_attack_ground(g, gr, x, y, q);
				return;
			}
			group_action_air_attack_ground(g, gr, x, y, q);
			return;
		}
	}
	// A building group (or a carrier): the units inside take its place.
	Group inside;
	group_clear(inside, -1);
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* B = g.object(gr.who, gr.list[size_t(i)]);
		if (!B || !B->alive() || B->inside_down < 0) continue;
		int32_t cur = B->inside_down, curw = B->inside_down_who;
		for (int32_t guard = 0; cur >= 0 && curw >= 0 && guard < kUnitLimit; guard++) {
			group_add(g, inside, cur, curw, 0, 0);
			const Object* C = g.object(curw, cur);
			if (!C) break;
			cur = C->inside_down;
			curw = C->inside_down_who;
		}
	}
	group_clear(gr, -1);
	for (int32_t k = 0; k < inside.num; k++) group_add(g, gr, inside.list[size_t(k)], inside.who, 1, 1);
	group_action_air_attack_ground(g, gr, x, y, q);
}

// ---------------------------------------------------------------- SCRAMBLE: Group::action_scramble @7111c0

void group_action_scramble(Game& g, Group& gr) {
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	for (int32_t i = 0; i < gr.num; i++) {
		const Object* B = g.object(gr.who, gr.list[size_t(i)]);
		if (!B) continue;
		int32_t cur = B->inside_down, curw = B->inside_down_who;
		for (int32_t guard = 0; cur >= 0 && curw >= 0 && guard < kUnitLimit; guard++) {
			Unit* P = g.unit(curw, cur);
			if (!P) break;
			cur = P->inside_down;
			curw = P->inside_down_who;
			if (!P->alive() || P->ptype->domain != kDomainAir || busy(*P) || is_missile(*P) || P->mana_burn != 0) continue;
			int32_t hw = -1;
			const int32_t ho = unit_get_inside(g, *P, &hw);
			if (!is_heli(*P)) add_air_patrol_order(g, *P, B->x, B->y, ho, hw, true);
			else heli_move_to(g, *P, B->x, B->y, find_angle(B->x - P->x, B->y - P->y));
		}
	}
}

// ---------------------------------------------------------------- RECALL: Group::action_recall @6fa7e0 / action_return @6fad40

namespace {

// The order a plane is flying (the one behind a landing SPECIAL_ANIM), or null.
Order* flying_order(Unit& u) {
	if (u.orders.empty()) return nullptr;
	if (u.orders.front().type != kSpecialAnim) return &u.orders.front();
	return u.orders.size() > 1 ? &u.orders[1] : nullptr;
}

// Close the unit's orders the way the recall does (UNIT_MULTIMOVE off, path 0, close_orders, clear_partial_path,
// update_action) and send it to its base with a no-target STRAFE, keeping the old order's altitude and turn state.
void recall_to(Game& g, Unit& u, int32_t home_o, int32_t home_who, int32_t alt, int32_t sharp) {
	u.unit_masks &= ~umask::kMultimove;
	u.path.clear();
	close_orders(g, u, false);
	clear_partial_path(u);
	update_action(g, u);
	add_strafe_order(g, u, -1, -1, home_o, home_who, 0, kQueueNew, false);
	if (Order* n = flying_order(u); n && n->type == kStrafe) {
		n->cruising_alt = alt;
		n->sharp_turn = sharp;
	}
}

}  // namespace

// Group::action_return @6fad40: the planes of the group go home.
static void group_action_return(Game& g, Group& gr) {
	gr.disband = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		const int32_t mo = gr.list[size_t(i)];
		const Object* ob = g.object(gr.who, mo);
		Unit* M = g.unit(gr.who, mo);
		if (!ob || !ob->alive() || !M) continue;
		if (!unit_is_plane(*M)) {
			if (is_heli(*M)) {
				// A helicopter drops its orders and flies off to the nearest base.
				M->unit_masks &= ~umask::kMultimove;
				M->path.clear();
				close_orders(g, *M, false);
				clear_partial_path(*M);
				update_action(g, *M);
				if (M->on_map()) add_strafe_order(g, *M, -1, -1, -1, -1, 1, kQueueNew, false);
			}
			continue;
		}
		if (is_missile(*M)) continue;
		int32_t cw = -1;
		const int32_t in = unit_get_inside(g, *M, &cw);
		if (in < 0) {
			const Order* f = flying_order(*M);
			if (!f) continue;
			recall_to(g, *M, f->oxx, f->whose, f->cruising_alt, f->sharp_turn);
		} else {
			M->unit_masks &= ~umask::kMultimove;
			M->path.clear();
			close_orders(g, *M, false);
			clear_partial_path(*M);
			update_action(g, *M);
			int32_t hw = -1;
			const int32_t ho = unit_get_inside(g, *M, &hw);
			if (Object* C = obj_at(g, ho, hw)) launch_list_remove(*C, mo);
		}
	}
}

void group_action_recall(Game& g, Group& gr) {
	// TODO(scenarios): the ScenarioData::ignore_orders prelude.
	if (gr.num <= 0) return;
	const int32_t li = leader_index(g, gr);
	if (li >= 0) {
		const Object* L = g.object(gr.who, gr.list[size_t(li)]);
		if (L && L->ptype && L->ptype->domain == kDomainAir) {
			group_action_return(g, gr);
			return;
		}
	}
	gr.disband = 0;
	for (int32_t i = 0; i < gr.num; i++) {
		Object* ob = g.object(gr.who, gr.list[size_t(i)]);
		if (ob && ob->alive() && !ob->is_unit) build_clear_gather(g, static_cast<Build&>(*ob));
	}
	gr.form = -1;
	const int32_t n = g.objects[size_t(gr.who)].unit_mark;
	for (int32_t o = 0; o < n; o++) {
		const Object* ob = g.object(gr.who, o);
		Unit* M = g.unit(gr.who, o);
		if (!ob || !ob->alive() || !M) continue;
		if (!unit_is_plane(*M) || is_missile(*M)) continue;
		int32_t cw = -1;
		const int32_t in = unit_get_inside(g, *M, &cw);
		if (in < 0 || cw < 0) {
			Order* f = flying_order(*M);
			if (!f) continue;
			const int32_t ho = f->oxx, hw = f->whose;
			if (!in_group(g, gr, ho, hw)) continue;
			recall_to(g, *M, ho, hw, f->cruising_alt, f->sharp_turn);
		} else if (in_group(g, gr, in, cw)) {
			clear_orders(g, *M);
			if (Object* C = obj_at(g, in, cw)) launch_list_remove(*C, o);
		}
	}
}

}  // namespace ron::sim
