// Aircraft (docs/aircraft-spec.md): planes, helicopters, airbases, carriers and missile silos. The air orders (STRAFE,
// AIR_PATROL, AIR_ATTACK_GROUND) and their handlers, the flight model (Unit::do_air_physics with the banking and pitch
// state of the guy), fuel and landing, the container rules (can_carry, home_base, num_aircraft_here / limit), the
// launch machinery (Object::do_launch, attempt_launch, Build::do_missile_launch) and the air commands (FLIGHT,
// AIR_PATROL, AIR_ATTACK_GROUND, SCRAMBLE, RECALL).
//
// Flight state that is simulation state (and so walked and digested): the altitude reroll and the edge-turn draws, the
// guy's z / bank / pitch floats, `mana_burn` (fuel used), the launch timer and launch list of the containers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;
struct Group;

// TypeIndex values of the air families (aircraft-spec 1).
namespace airtype {
constexpr int32_t kBiplane = 0x11f, kJetFighter = 0x127, kBomber = 0x130, kFighterBomber = 0x134, kHelicopter = 0x136,
                  kNukeLine = 0x13b, kCarrier = 0x15f, kAirbase = 0x1bf, kSilo = 0x208, kAirDefense = 0x20b,
                  kForbiddenCity = 0x213;
}

// FRAMES_BETWEEN_LAUNCHES (exe data @c06248 = int 15).
constexpr int32_t kFramesBetweenLaunches = 15;

bool order_is_air(OrderType t);  // UnitOrder::is_air: STRAFE, AIR_PATROL, AIR_ATTACK_GROUND

// RON_AIR_TRACE=1 prints launches, landings, crashes and rebases on stderr (developer aid; no effect on the simulation).
bool air_trace();

// ---------------------------------------------------------------- containers (aircraft-spec 2)

// ObjectData::can_carry(AIR) @646c40 (strict is: airbase, carrier, missile silo).
bool can_carry_air(const Object& ob);
// ObjectData::can_carry(TypeIndex) @645e00 for a unit type: may a container of this kind hold it.
bool object_can_carry_type(const Game& g, const Object& container, int32_t type);
// The air-cargo branch of ObjectData::can_carry(o, who) @6483c0: container `c`, cargo = the plane / missile (o, who).
bool air_can_carry(const Game& g, const Object& c, int32_t o, int32_t who);
// ObjectData::num_aircraft_limit @6454a0 / num_aircraft_here(count_queue) @645330.
int32_t num_aircraft_limit(const Game& g, const Object& ob);
int32_t num_aircraft_here(const Game& g, const Object& ob, int32_t count_queue);
// UnitData::home_base(&who) @609dc0: the container o (its leader in *who_out), or -1.
int32_t unit_home_base(const Game& g, const Unit& u, int32_t* who_out);
// UnitData::mana_left @609a30 (fuel left; 0 when the type has none).
int32_t unit_mana_left(const Game& g, const Unit& u);
// UnitData::has_repeat_air @46cec0 (units) / WallData (buildings): REPEAT_AIR.
bool has_repeat_air(const Object& ob);
// UnitData::is_flying_low @60a140 / is_flying_high @60a310 (the AA rolls of Ammo::init, valid_target_const).
bool unit_is_flying_low(const Game& g, const Unit& u);
bool unit_is_flying_high(const Game& g, const Unit& u);

// ---------------------------------------------------------------- orders (aircraft-spec 3)

// Unit::add_strafe_order(ox, who, home_o, home_who, mandatory, q, explicit) @5e48c0: the argument names follow the stores
// (the 5th argument is the AttackOrder `mandatory` byte, the 7th the UnitOrder flag 4).
void add_strafe_order(Game& g, Unit& u, int32_t ox, int32_t who, int32_t home_o, int32_t home_who, int32_t mandatory,
                      QueuePos q, bool expl);
// Unit::add_air_attack_ground_order(x, y, home_o, home_who, q, explicit) @5e41c0.
void add_air_attack_ground_order(Game& g, Unit& u, int32_t x, int32_t y, int32_t home_o, int32_t home_who, QueuePos q,
                                 bool expl);
// Unit::add_air_patrol_order(x, y, home_o, home_who, explicit) @5e4350 (always replaces the order list; helicopters
// get an EXPLORE_TO-style move instead).
void add_air_patrol_order(Game& g, Unit& u, int32_t x, int32_t y, int32_t home_o, int32_t home_who, bool expl);
// Unit::add_spec_anim_order(type, data1, data2, q) @5e4160 (SPECIAL_ANIM; the landing order is type 0).
Order* add_spec_anim_order(Game& g, Unit& u, int32_t type, int32_t data1, int32_t data2);

// ---------------------------------------------------------------- the flight (aircraft-spec 4-6)

// Unit::do_air_physics(order, x, y) @5e86d0: 0 when the plane landed, died or the order ended.
int32_t do_air_physics(Game& g, Unit& u, Order& ord, int32_t x, int32_t y);
// Unit::check_fuel(order, &x, &y, &z) @5e9be0: 1 when the plane crashed.
int32_t check_fuel(Game& g, Unit& u, Order& ord, int32_t* x, int32_t* y, int32_t* z);
// Unit::land_plane @5e9950.
void land_plane(Game& g, Unit& u);
// Unit::do_strafe @5eab00, do_air_patrol @5ea620, do_air_attack_ground @5ea420 (the do_job handlers).
void do_strafe(Game& g, Unit& u, Order& ord);
void do_air_patrol(Game& g, Unit& u, Order& ord);
void do_air_attack_ground(Game& g, Unit& u, Order& ord);
// Unit::do_spec_anim @5e5880 (SPECIAL_ANIM: the landing seats the plane, the take-off from an airbase places it).
void do_spec_anim(Game& g, Unit& u, Order& ord);
// Unit::find_new_air_target / find_new_bomber_target @5ebc70 / @5eb960: the target (o, *who_out) or -1.
int32_t find_new_air_target(Game& g, Unit& u, int32_t x, int32_t y, int32_t type_o, int32_t type_who, int32_t* who_out);
int32_t find_new_bomber_target(Game& g, Unit& u, int32_t x, int32_t y, int32_t type_o, int32_t* who_out);

// The air block of Unit::process @610bc0 (fuel, recharge in a base, healing in a base).
void unit_process_air(Game& g, Unit& u);

// ---------------------------------------------------------------- launching (aircraft-spec 8)

// Object::do_launch @64f3b0 (airbases and silos from Build::process, carriers from Unit::work).
void do_launch(Game& g, Object& container);
// Object::attempt_launch(x, y) @643a10 (from Object::do_damage: the attacker scrambles a Biplane-line fighter).
void attempt_launch(Game& g, Object& container, int32_t x, int32_t y);
// Build::do_missile_launch @622670.
void build_do_missile_launch(Game& g, Build& silo);
// The type-5 (PLANERELEASE) events of Guy::execute_events: the carrier's launch counter (Unit +0x5c, trench_angle) and
// GraphicEvents::execute_game_events @8e48e0. Called for guy 0 of a carrier.
void carrier_launch_events(Game& g, Unit& carrier, Guy& gy);

// Build::train @62f9b0, the aircraft-housing branch (obj_masks & 0x200): the airbase's gather points become patrols,
// strafes or a rebase of the new plane. Returns 0 to go on to the common tail, 1 to return the unit's o at once (a missile
// refused by the target's MISSILE_DEFENSE_BONUS), 2 when the unit could not leave and died (Build::train returns -1).
int32_t build_train_air(Game& g, Build& b, Unit& u);
// Build::train's new-carrier wing: Unit::action_unqueue(1) (a no-op: the carrier queue is empty) then
// num_aircraft_limit() x init_unit(current FIGHTERBOMBER) + go_inside.
void carrier_new_wing(Game& g, Unit& carrier);

// ---------------------------------------------------------------- commands (aircraft-spec 8)

// Group::action_flight(ox, whom, order, shift, ctrl, alt) @6fb260 (FLIGHT): `order` 1 MOVE_TO (rebase onto an airbase /
// carrier) or 10 ATTACK; a group of buildings goes to action_launch_flight @6fbfb0 (one plane per container, or all with
// `shift`; `ctrl` / `alt` pick bombers / fighters out of airbases).
void group_action_flight(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t order, int32_t shift, int32_t ctrl, int32_t alt);
void group_action_launch_flight(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t order, int32_t shift, int32_t ctrl,
                                int32_t alt);
// Group::action_air_patrol(x, y, q) @7029d0 and action_patrol @7030c0 (PATROL, air half), action_launch_patrol @703580
// (LAUNCH_PATROL: planes out of airbases and carriers).
void group_action_air_patrol(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q);
void group_action_patrol(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q);
void group_action_launch_patrol(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q, int32_t shift, int32_t ctrl, int32_t alt);
// Group::action_air_attack_ground(x, y, q) @703d80 and action_attack_ground @704520 (ATTACK_GROUND, air half).
void group_action_air_attack_ground(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q);
void group_action_attack_ground(Game& g, Group& gr, int32_t x, int32_t y, QueuePos q);
// Group::action_scramble @7111c0 (SCRAMBLE), Group::action_recall @6fa7e0 (RECALL; planes: action_return @6fad40).
void group_action_scramble(Game& g, Group& gr);
void group_action_recall(Game& g, Group& gr);
// Unit::action_unqueue(1) @5e1f20 (UNQUEUE of a unit: one queued plane off a carrier, with the counters and the refund).
void unit_action_unqueue(Game& g, Unit& u);
// Group::action_spell, aircraft branch: BUILDAIR (queue the current FIGHTERBOMBER on the group's carriers); the carrier's
// production step of Unit::process @611158 (called from unit_process).
void group_action_buildair(Game& g, Group& gr);
void unit_process_air_queue(Game& g, Unit& u);
// Unit::close of a carrier @60ee50: the patrols based on it get absolute waypoints.
void carrier_closed(Game& g, Unit& carrier);
// Build::clear_gather / add_gather_point on an airbase (aircraft-spec 8): its planes go home / re-issue their patrols.
void airbase_gather_cleared(Game& g, Build& b);
void airbase_gather_added(Game& g, Build& b);

// ---------------------------------------------------------------- harness

// One line per aircraft or container for ron-sim --list / --air-every: position, fuel, base, air order, launch state.
std::vector<std::string> air_describe(const Game& g, int32_t who);

}  // namespace ron::sim
