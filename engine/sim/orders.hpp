// The order list and the per-frame order machine (docs/orders-spec.md sections 3-6): Unit::add_*_order with queue
// positions, kill_current_order / close_orders / clear_orders / repath, update_action, Unit::work and do_job.
#pragma once

#include <cstdint>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;
struct Group;

// Unit::add_move_facing_order @5e55c0: (ux, uy) in UCoords (the target snaps to the cell centre). kind: 1 MOVE_TO,
// 2 ATTACK_TO, 3 EXPLORE_TO, 4 FLEE_TO (anything else MOVE_TO). Returns the new order.
Order* add_move_facing_order(Game& g, Unit& u, int32_t ux, int32_t uy, uint32_t angle, int32_t kind, bool pathed,
                             QueuePos q, bool expl, int32_t facing, int32_t orig_x, int32_t orig_y, bool disembark);
// Unit::add_move_order @616ed0: Coord target -> add_move_facing_order(x/48, y/48, find_angle(...), ...).
Order* add_move_order(Game& g, Unit& u, int32_t x, int32_t y, int32_t kind, bool pathed, QueuePos q, bool expl,
                      int32_t orig_x, int32_t orig_y);
// Unit::add_think_order @5e3df0 (always QUEUE_FIRST).
void add_think_order(Game& g, Unit& u);
// The QUEUE_NEW preamble shared by every adder (and Unit::clear_orders @5e3860).
void clear_orders(Game& g, Unit& u);
// Adds `o` (already filled) at queue position q, with the QUEUE_NEW / QUEUE_FIRST handling of every add_*_order.
// Returns the stored order.
Order* add_order(Game& g, Unit& u, const Order& o, QueuePos q);

void kill_current_order(Game& g, Unit& u, bool no_side_effects = false);  // Unit::kill_current_order @5e2cb0
void close_orders(Game& g, Unit& u, bool x = false);                      // Unit::close_orders @5e37f0
void kill_current_path(Game& g, Unit& u);                                 // Unit::kill_current_path @5e31d0
void repath(Game& g, Unit& u);                                            // Unit::repath @5e29b0
void kill_current_action(Game& g, Unit& u);                               // Unit::kill_current_action @5e2bb0
// Unit::update_action @60a870: sets orders_x/orders_y/dest_angle, returns the action order (or null).
Order* update_action(Game& g, Unit& u);
// UnitData::get_action @608450 (const twin; no writes).
const Order* get_action(const Unit& u);
OrderType action_type(const Unit& u);  // UnitData::action_type
// Unit::update_order @6179d0: the current (front) order, or null.
Order* update_order(Unit& u);

// FOLLOW and GUARD (orders_guard.cpp). A GuardOrder keeps dx / dy in def_x / def_y, the guard point in x / y, idle in timer and
// retry in pause (no new Order members: the save layout is unchanged); a FollowOrder uses oxx / whose / uid2.
Order* add_follow_order(Game& g, Unit& u, int32_t ox, int32_t whom, QueuePos q);                        // Unit::add_follow_order @5e3f60
Order* add_guard_order(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t dx, int32_t dy, QueuePos q);  // Unit::add_guard_order @5e3e40
Order* update_guard_order(Game& g, Unit& u, int32_t ox, int32_t whom);                                  // Unit::update_guard_order @5e3220
void do_follow(Game& g, Unit& u, Order& f);                                                             // Unit::do_follow @5e65d0
void do_guard(Game& g, Unit& u, Order& gd);                                                             // Unit::do_guard @5e5c70

// Ground PATROL (GROUP_PATROL) and ATTACK_GROUND (orders_patrol.cpp; aircraft_cmd.cpp dispatches the commands).
bool type_can_attack_ground(const Game& g, const Type& t);  // UnitTypeData::can_attack_ground @61db20
Order* update_patrol_order(Unit& u, int32_t id);            // Unit::update_patrol_order @5e35e0 (id -1: any)
Order* add_group_patrol_order(Game& g, Unit& u, int32_t ux0, int32_t uy0, int32_t ux1, int32_t uy1, int32_t id, int32_t index,
                              int32_t leader_o, int32_t leader_who, QueuePos q);  // Unit::add_patrol_order @5e4560 (ucoords)
void do_patrol(Game& g, Unit& u, Order& p);                 // Unit::do_patrol @5f1910
void group_action_ground_patrol(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q);  // Group::action_patrol @7030c0, ground branch
void group_redo_patrol_order(Game& g, Group& gr, const Order& saved);                   // Group::redo_patrol_order @706d90
void add_attack_ground_order(Game& g, Unit& u, int32_t x, int32_t y, QueuePos q, bool expl, int32_t attack_unit, int32_t accuracy);  // @5e4290
void do_attack_ground(Game& g, Unit& u, Order& a);          // Unit::do_attack_ground @5f1410
void group_action_ground_attack_ground(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q);  // Group::action_attack_ground, ground branch

void work(Game& g, Unit& u);                           // Unit::work @60d180
int32_t do_job(Game& g, Unit& u, OrderType t, Order* o);  // Unit::do_job @617a10
void do_idle(Game& g, Unit& u);                        // Unit::do_idle @60dcd0
void check_idle(Game& g, Unit& u);                     // Unit::check_idle @6032c0
void think(Game& g, Unit& u);                          // Unit::think @5f6e40 (TODO: AI; see orders.cpp)

}  // namespace ron::sim
