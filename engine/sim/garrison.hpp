// Garrisons (docs/garrison-spec.md): the container links (Object::insert_inside / remove_from_inside / get_inside),
// counting what is inside an object (num_inside, count_inside), who may garrison where (UnitTypeData::can_garrison),
// the GARRISON order and its handler, the GARRISON / EJECTALL group actions and Object::eject_contents / kill_contents.
//
// Transports (units carried by units: sim/transport.hpp) share the container links and the unit-source branch of
// eject_contents; aircraft on airbases / carriers and missiles in silos are TODO(air).
#pragma once

#include <cstdint>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;
struct Group;

// ---------------------------------------------------------------- the container links (garrison-spec 1)

// ObjectData::get_inside @651a80 (units only): the outermost container (o; its leader in *who_out) of a contained unit,
// following inside_up while the container is a unit; -1 on the map.
int32_t unit_get_inside(const Game& g, const Unit& u, int32_t* who_out);
// Object::insert_inside(o, who) @647e90: the unit leaves the map and is linked behind the last object inside
// (o, who); a captain that is not a decoy updates the leader's "inside" counters when the container is a building.
void unit_insert_inside(Game& g, Unit& u, int32_t o, int32_t who);
// Object::remove_from_inside @6480f0: unlinks the unit (its own chain stays with the container) and puts it back on
// the map at its stored position.
void unit_remove_from_inside(Game& g, Unit& u);

// ---------------------------------------------------------------- counting (garrison-spec 2)

// ObjectData::count_inside's CountIndex (the PDB enum; garrison-spec 2). The kinds not listed (SIEGE 4, CASTERS 5,
// DECOYS 6, TANKS 7, MISSILES 8, CAN_GATHER 15, GATHER_POINT 23, CAN_GARRISON_AT 24, CAN_AIR_GARRISON 25, GARRISON 27,
// GARRISON_WITH_TYPE 28, TYPE_INSIDE 29, CAN_BE_SCHOLARS 31) have no caller here yet.
enum CountInside : int32_t {
	kCountWorkers = 0, kCountPeasants = 1, kCountAttack = 2, kCountMilitary = 3, kCountDomain = 9, kCountOnMap = 10,
	kCountGatherPeasants = 11, kCountGatherWorkers = 12, kCountGatherScholars = 13, kCountBuildPeasants = 14,
	kCountStrictType = 16, kCountType = 17, kCountTypeSameWho = 18, kCountNonDecoyType = 19, kCountCategory = 20,
	kCountRegion = 21, kCountTrade = 22, kCountGarrisonArrows = 26, kCountWhoInside = 30
};

// ObjectData::num_inside(p) @646d50: 0 unless the object is on the map; over the captains inside: p != 0 the number
// of chains, p == 0 the population (control_cost, 1 for a government hero, 0 for a decoy).
int32_t num_inside(const Game& g, const Object& ob, int32_t p);
// ObjectData::count_inside(kind, t) @64dda0 over the chain inside `ob` (valid objects only).
int32_t count_inside(const Game& g, const Object& ob, int32_t kind, int32_t t);
// ObjectData::can_carry(domain) @646c40: DomainIndex 2 (air) = airbase / carrier / missile silo (strict is); 0 (ground)
// also any unit whose type carries (carry != 0, not a carrier).
bool can_carry_domain(const Game& g, const Object& ob, int32_t domain);
// UnitData::control_cost for the population tests: 0 for a decoy.
int32_t unit_control_cost(const Unit& u);

// ---------------------------------------------------------------- eligibility (garrison-spec 3)

// UnitTypeData::can_garrison(bt) @61d7d0 (bt = the building's TypeIndex, -1 = any building).
bool type_can_garrison(const Game& g, const Type& unit_type, int32_t bt);
// UnitData::can_garrison(o, who) @6099a0: the target objects[who][o] is a valid, active building the type fits.
bool unit_can_garrison(const Game& g, const Unit& u, int32_t o, int32_t who);
// Unit::find_garrison_build(city, who) @605040: the nearest building of the city with room that the unit can enter, or -1.
int32_t find_garrison_build(const Game& g, const Unit& u, int32_t city, int32_t who);
// Object::adjacent_to(ox, whom) @651f40 (non-"e" ships compare with boat_garrison_max_distance).
bool object_adjacent_to(const Game& g, const Object& s, int32_t ox, int32_t whom);

// ---------------------------------------------------------------- the order (garrison-spec 4)

// Unit::add_garrison_order(ox, whom, find, q, explicit) @5e4080.
Order* add_garrison_order(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t find, QueuePos q, bool expl);
// Unit::kill_garrison_order(p) @5e2bd0: the captain and its live o_down members drop a current GARRISON action.
void kill_garrison_order(Game& g, Unit& u, int32_t p);
// Unit::do_garrison(order) @5e6b80, the handler of the current GARRISON order.
int32_t do_garrison(Game& g, Unit& u, Order& o);

// ---------------------------------------------------------------- the group actions (garrison-spec 5)

// Group::action_garrison(ox, whom, q, find) @700490 (process_garrison).
void group_action_garrison(Game& g, Group& gr, int32_t ox, int32_t whom, QueuePos q, int32_t find);
// Group::action_eject_all(back_to_work, who, eject_o, eject_who) @710b40 (process_eject_all).
void group_action_eject_all(Game& g, Group& gr, int32_t back_to_work, int32_t who, int32_t eject_o, int32_t eject_who);

// ---------------------------------------------------------------- ejection (garrison-spec 6)

// Object::eject_contents(kill_if_stuck, type, carry_orders, no_orders) @64cd20. A building called with type < 0 outside
// the editor only raises EJECTING (Build::process_ejection lets one chain out per frame).
void eject_contents(Game& g, Object& ob, int32_t kill_if_stuck, int32_t type, int32_t carry_orders, int32_t no_orders);
// Object::kill_contents @645550: every contained unit comes out and dies with its chain.
void kill_contents(Game& g, Object& ob);
// Build::process_ejection @6201e0: one chain comes out (come_out(1) on the captain of the first contained unit); the
// flag clears when the building is empty. A dead building keeps its slot held (hold_frames + 1) while it empties.
void build_process_ejection(Game& g, Build& b);

// ---------------------------------------------------------------- the frame (garrison-spec 7)

// The garrisoned branch of Unit::process @610bc0 (frame-spec 9 step 6).
void unit_process_inside(Game& g, Unit& u);

}  // namespace ron::sim
