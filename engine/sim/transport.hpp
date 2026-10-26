// Transports (docs/transport-spec.md): a land unit crosses water in a self-made Transport Barge. Who may be carried
// (the predicates of section 2), SpellType::cast_transport (the barge is created next to the unit and takes its orders
// and path), the damage hand-over (Unit::same_damage), the boarding orders (BOARD_SHIP / AWAIT_BOARD,
// Unit::check_meet_ship) and the TRANSPORT / SET_TRANSPORT / BOARD_SHIP group actions. The crossing itself lives in
// Unit::set_new_location (guys.cpp: the cast order on entering water, the ejection on leaving it) and
// Object::eject_contents (garrison.cpp: the unit-source branch).
#pragma once

#include <cstdint>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;
struct Group;

// RON_TRANSPORT_TRACE=1 prints every transport event (the cast order, the barge, the landing) on stderr.
bool transport_trace();

// ---------------------------------------------------------------- who may be transported (section 2)

// TransportType (the PDB enum): the level a leader grants and the level a unit needs.
enum TransportType : int32_t { kTransportNone = 0, kTransportScout = 1, kTransportMilitary = 2, kTransportCivilian = 3 };

// LeaderData::can_transport() @6e0c60: 3 with LEADER_CAN_TRANSPORT_CIV (0x100), 2 with _MIL (0x200), else bit 10 (scouts).
int32_t leader_can_transport(const Game& g, int32_t who);
// UnitData::transport_type() @46f790: scouts 1, citizens / scholars / caravans / merchants / fur trapper 3, the rest 2.
int32_t unit_transport_type(const Game& g, const Unit& u);
// UnitData::can_ever_transport() @46f290: a land unit; a non-land unit with a carry capacity that is not a carrier
// (the barges themselves). (The spec text has the carrier test inverted; the disassembly returns 1 when it is NOT a carrier.)
bool unit_can_ever_transport(const Game& g, const Unit& u);
// ObjectData::is_cargo() @653600 (vslot 0x110): on the map and a land unit type.
bool object_is_cargo(const Game& g, const Object& ob);
// Unit::think_civilian_transport(flag) @5f40d0 (sim/ferry.cpp): the unit's leader can carry it across water, so it walks to the coast of its region
// facing the best other land region (flag 0: a scout looking for an unscouted region; flag 1: an AI citizen looking for a region to settle).
// 1 when it gave an order, 0 when not (an auto-exploring unit with nothing to do falls back to think_scout(1)).
int32_t unit_think_ferry(Game& g, Unit& u, int32_t flag);
// ObjectData::cargo_inside() @64dcd0: the carry_size of the captains inside a carrier.
int32_t cargo_inside(const Game& g, const Object& ob);
// ObjectData::can_carry(o, who) @6483c0 with `carrier` as this: the unit (o, who) fits (same leader, land cargo, room left).
bool can_carry_unit(const Game& g, const Object& carrier, int32_t o, int32_t who);
// UnitData::is_caravan (vslot 0xd0): the type's CARAVAN flag (unit_flags2 & 8).
bool unit_is_caravan(const Unit& u);

// ---------------------------------------------------------------- the crossing (sections 5 and 7)

// SpellType::cast_transport(o, who) @670db0.
void cast_transport(Game& g, int32_t o, int32_t who);
// Unit::same_damage(o, who) @5f9400: `self` (and its o_down chain) takes the damage ratio of the unit (o, who) when that is
// higher than its own.
void same_damage(Game& g, Unit& self, int32_t o, int32_t who);

// ---------------------------------------------------------------- boarding an existing ship (section 8)

// Unit::add_board_order(o, who, q) @5e4d10 and Unit::add_await_board_order(o, who, q, expl) @5e4c80.
Order* add_board_order(Game& g, Unit& u, int32_t o, int32_t who, QueuePos q);
Order* add_await_board_order(Game& g, Unit& u, int32_t o, int32_t who, bool expl);
// Unit::do_board(order) @5ed1f0 / Unit::do_await_board(order) @5ed040: the handlers of the current orders.
int32_t do_board(Game& g, Unit& u, Order& o);
int32_t do_await_board(Game& g, Unit& u, Order& o);
// Unit::check_meet_ship(so, swho) @604550: 0 = the ship is close enough (board now), 1 = keep walking.
int32_t check_meet_ship(Game& g, Unit& u, int32_t so, int32_t swho);

// ---------------------------------------------------------------- the group actions (sections 2.9, 8.1, 9)

// Group::action_transport(force) @702620 (process_transport: force 0): the "board now" button.
void group_action_transport(Game& g, Group& gr, int32_t force);
// Group::action_set_transport(flag) @7024b0: sets / clears UNIT_CAN_TRANSPORT on the members that can ever be carried.
void group_action_set_transport(Game& g, Group& gr, int32_t flag);
// Group::action_board_ship(ship_o, q) @700010.
void group_action_board_ship(Game& g, Group& gr, int32_t ship_o, QueuePos q);

}  // namespace ron::sim
