// Combat (docs/combat-spec.md): stat getters and damage (combat_damage.cpp), death and corpses (combat_death.cpp),
// range, validity and target acquisition (combat_target.cpp), attack orders and approach positions
// (combat_orders.cpp), fight, projectiles, animation-event firing and building fire (combat_fight.cpp).
//
// Conventions as the rest of engine/sim: the function comment names the original method and address; int32
// arithmetic with C truncating division; every game_random draw sits where the original draws (combat-spec 8).
// Objects are addressed as (o, who) like the original; `Object&` arguments are the acting object ("this").
#pragma once

#include <cstdint>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;
struct Group;

// ---------------------------------------------------------------- flags and enums (PDB names)

// ObjMaskType (ObjectTypeData.obj_masks)
namespace objmask {
constexpr uint32_t kArmored = 0x1, kBombard = 0x2, kCivilian = 0x4, kMusketInf = 0x8, kElephant = 0x10, kFoot = 0x20,
                   kGun = 0x40, kHeavyInf = 0x80, kModernInf = 0x100, kFootArcher = 0x400, kMounted = 0x1000,
                   kNaval = 0x2000, kHorseArcher = 0x4000, kLightInf = 0x10000, kArchery = 0x20000,
                   kSiege = 0x40000, kWarMachine = 0x80000, kVehicle = 0x200000, kMelee = 0x400000,
                   kExplosive = 0x800000, kDetect = 0x2000000, kMissile = 0x8000000, kAir = 0x10000000,
                   kAntiAir = 0x80000000u;
}
// UnitTypeData.unit_flags: rules letters a.. = bits 0.. (rules-spec section 5)
namespace utflag {
constexpr uint32_t kTransport = 0x10, kHelicopter = 0x20, kAttackSideways = 0x40, kMeleeAndRanged = 0x400,
                   kFireship = 0x2000, kCloak = 0x4000, kBroadside = 0x10000, kSiege = 0x20000,
                   kStealth = 0x40000, kTank = 0x80000, kSubmarine = 0x100000, kFireWhileMoving = 0x200000,
                   kStrafing = 0x400000, kBlocksWhileDead = 0x800000, kRockingAttack = 0x2000000;
}
// UnitTypeData.unit_flags2
namespace utflag2 {
constexpr uint32_t kRetargets = 0x1, kCaster = 0x2, kPacker = 0x4, kCaravan = 0x8, kSpecial = 0x10, kHero = 0x20,
                   kSupply = 0x40;
}
// UnitTypeData.role
namespace role {
constexpr int32_t kScout = 0x10, kRanged = 0x400, kCombat = 0x10000;
}
// CombatStanceIndex
enum CombatStance : int32_t { kAggressive = 0, kDefensive = 1, kStandGround = 2, kRaid = 3, kRaze = 4, kHoldFire = 5 };
// DeathIndex
enum DeathType : int32_t { kDeathNone = 0, kDeathStabbed = 1, kDeathShot = 2, kDeathSploded = 3, kDeathScorch = 4 };
// DomainIndex
constexpr int32_t kDomainGround = 0, kDomainSea = 1, kDomainAir = 2;

// Load-time combat data: UnitTypeData.fire_proj from the art (GraphicEvents::init_unit_events @8e2520,
// combat-spec 4.2), the ammo pool (Objects::init: 200 free slots) and the corpse slots. Called by Game::load.
void init_combat_data(Game& g);

// ---------------------------------------------------------------- stat getters (combat-spec 5.1)
// The leader-dependent terms (nation powers, generals, heroes, rare goods, wonders, techs) are 0 here: TODO(combat-
// spec 10 step 8) once leaders, heroes and the economy exist.

// The raw type max_range (+0x1fc) as the original reads it, including the temporary MELEE_AND_RANGED swap.
int32_t raw_max_range(const Game& g, const Type& t);
int32_t obj_attack(const Game& g, const Object& ob);     // vslot 0x120: UnitData::attack @6103c0 / BuildData @62e610
int32_t obj_armor(const Game& g, const Object& ob);      // vslot 0x124: UnitData::armor @610160 (myarmor)
int32_t obj_hits(const Game& g, const Object& ob);       // vslot 0x11c: myhits / BuildData::hits @62e740
int32_t obj_max_range(const Game& g, const Object& ob);  // vslot 0x130: UnitData @60fe90 / BuildData @62e470 (tiles)
int32_t obj_min_range(const Game& g, const Object& ob);  // vslot 0x12c (tiles)
int32_t obj_hits_left(const Game& g, const Object& ob);  // ObjectData::hits_left @6535c0
// UnitData::recharge @60fdf0 (sup = the supply that keeps the unit supplied, -1).
int32_t unit_recharge(const Game& g, const Unit& u, int32_t* sup);
// get_combat_stance: UnitData @610a30 / BuildData @62e0f0 (STAND_GROUND for types without a COMBAT stance type).
int32_t get_combat_stance(const Game& g, const Object& ob);
int32_t unit_stance_type(const Type& t);  // UnitTypeData::get_stance_type @61d350
// Captain chain: UnitData::get_captain @610ab0 (self for buildings), curr_uber_size @60a760.
int32_t obj_captain(const Game& g, const Object& ob);
int32_t unit_curr_uber_size(const Game& g, const Unit& u, bool from_here = false);
bool unit_is_moving(const Unit& u);  // vslot 0xd8: the front order is_move
bool is_build_active(const Object& ob);  // Build && flags & ACTIVE
bool type_is_siege(const Type& t);   // UnitTypeData::is_siege (unit_flags & 0x20000)
bool type_is_tank(const Type& t);    // UnitTypeData::is_tank (unit_flags & 0x80000)
bool unit_is_plane(const Unit& u);   // UnitData::is_plane: domain AIR and not a helicopter
bool obj_is_wonder(const Object& ob);  // BuildData::is_wonder (wonder types)

// ---------------------------------------------------------------- damage (combat-spec 5.2-5.5)

// The situation of one get_damage call: the getter values and every state-dependent test, so that the pure core
// below can be checked against re/tools/combat-ref.mjs. Defaults: two units on open ground, nothing special.
struct DamageSituation {
	int32_t attack = 0, armor = 0;  // attacker vslot 0x120, target vslot 0x124
	bool target_packed = false, target_moving = false;
	bool target_active = true;                    // building target finished
	bool target_in_attacker_territory = false;    // citizen/militia vs building: W-cell owner == attacker
	bool target_severe_attrition = false, team_style2_not_target = false;
	bool splash = false, target_in_river = false, target_decoy = false;
	bool target_in_unfriendly_territory = false, scenario_no_border_penalty = false;
	bool has_angles = false;  // flank test runs only for unit vs unit (angles always given by get_damage)
	uint32_t target_angle = 0, attack_angle = 0;
	bool overkill = false;  // the overkill condition held (second hitter within overkill_frames)
	bool target_on_rocky = false;
	int32_t height_diff = 0;  // attacker z - target z when the height bonus applies (> 0), else 0
	bool target_entrenched = false, antipater = false;
	uint32_t trench_angle = 0;
	int32_t building_shot = -1;  // attacker building: BuildData::get_shot
	bool wellington = false, cossack = false, recapture_own_city = false;
	int32_t japanese_ages = -1;  // >= 0: Japanese barracks unit with this many ages
};
// ObjectData::get_damage @644130 on resolved inputs. Returns the damage in hit points; *death = DeathIndex.
int32_t get_damage_core(const Rules& r, const Type& att, const Type& def, const DamageSituation& s, int32_t* death);
// Object::do_damage's scaling of one call into (hit points, sixteenths) (mult 0x100 = 1.0; ammo: from an Ammo).
struct HitScale {
	int32_t hp = 0, frac = 0;
};
HitScale scale_damage(const Type& att, bool att_is_unit, int32_t hp, int32_t mult, bool ammo, bool zoc = false);
// flanking with the frontal cone test (0 frontal, 1 rear, 2 side) and the entrenchment sector (combat-spec 5.2.1).
int32_t flank_level(uint32_t target_angle, uint32_t attack_angle);
int32_t entrench_sector(uint32_t trench_angle, uint32_t attack_angle);

// ObjectData::get_damage @644130 on live objects (attacker `att`, target objects[whom][ox]).
int32_t get_damage(Game& g, const Object& att, int32_t ox, int32_t whom, uint32_t angle, bool splash, bool overkill,
                   int32_t* death);
// Object::do_damage @64a480. ammo = Ammo slot (-1 melee/direct), mult 8.8, splash, no_react (collateral).
void do_damage(Game& g, Object& att, int32_t ox, int32_t whom, uint32_t angle, int32_t guys, int32_t ammo,
               int32_t mult, bool splash, bool no_react);
// Object::take_damage @652020: 0 survived, 1 died, 2 a decoy died.
int32_t take_damage(Game& g, Object& tgt, int32_t hp, int32_t frac, int32_t death, int32_t ammo, bool quiet,
                    uint32_t angle, int32_t att_o, int32_t att_who, bool splash);
// UnitTypeData::get_kill_value @61d3d0 / TypeData::get_kill_value @663a00 (victim owner `who`).
int32_t get_kill_value(const Game& g, int32_t type, int32_t who);
// TypeData::get_score_value(0) @6673b0.
int32_t get_score_value(const Game& g, int32_t type);
// Type::sum_rules_cost @6680e0 (compare_target).
int32_t sum_rules_cost(const Game& g, int32_t type);

// ---------------------------------------------------------------- death (combat-spec 6)

// Object::die @647080 (Unit::die @60eda0 / Wall::die @63f6c0): close, then hold the slot while own ammo flies.
void object_die(Game& g, Object& ob, int32_t death, int32_t gpiece, float deg);
// Unit::close @60ee50 with Object::close @647160 (death animation draws, uber-chain relink, corpses).
void unit_close(Game& g, Unit& u, int32_t death, int32_t gpiece, float deg);
// Building death (Build::close / Wall::close / Object::close; queues, cities and contents: TODO).
void build_close(Game& g, Build& b, int32_t death);
// Object::close @647160 (the alive part: leave the world, lower the slot marks, hold the slot; flags = 0).
void object_close(Game& g, Object& ob, int32_t death);
// Objects::kill_guy @659410 and Objects::add_death @653b60.
void kill_guy(Game& g, int32_t who, int32_t o, int32_t guy, int32_t anim, int32_t gpiece, float deg);
int32_t add_death(Game& g, int32_t who, int32_t o, int32_t guy, int32_t anim, int32_t gpiece, float deg);
// DeathObj::inc_time @8d5240 for every valid corpse (Objects::inc_time, after the ammo pass).
void deaths_inc_time(Game& g);

// ---------------------------------------------------------------- range and validity (combat-spec 2.9-2.11, 3.6)

// ObjectData::attack_dist @6488f0 (shooter placed at (x, y)) and @64c880 (own position).
int32_t attack_dist(const Game& g, const Object& s, int32_t ox, int32_t whom, int32_t x, int32_t y);
int32_t attack_dist(const Game& g, const Object& s, int32_t ox, int32_t whom);
// ObjectData::is_in_range @6486b0 (from (x, y); slack adds 144) and @648d70 (own position).
bool is_in_range(const Game& g, const Object& s, int32_t ox, int32_t whom, int32_t x, int32_t y, int32_t slack,
                 int32_t* dist = nullptr);
bool is_in_range(const Game& g, const Object& s, int32_t ox, int32_t whom, int32_t slack = 0);
// ObjectData::is_in_range @64e4a0 (a ground point from (sx, sy)).
bool is_in_range_point(const Game& g, const Object& s, int32_t tx, int32_t ty, int32_t sx, int32_t sy);
// Fog: ObjectData::is_seen vslot 0x48 (UnitData::is_seen @607a60 / BuildData::is_seen @62e1a0) for player `who`.
bool obj_is_seen(const Game& g, const Object& t, int32_t who, int32_t p2 = 0);
// ObjectData::valid_target_const @6472c0 and Object::valid_target @648ba0 (*capturable may be null).
bool valid_target_const(const Game& g, const Object& s, int32_t ox, int32_t whom);
bool valid_target(Game& g, Object& s, int32_t ox, int32_t whom, int32_t* capturable = nullptr);
// BuildData::check_capture_eligible @62d1d0 and Build::check_capture @6276a0 (sim/capture.cpp; 1 = captured).
bool check_capture_eligible(const Game& g, const Object& t);
int32_t check_capture(Game& g, Object& t, int32_t o, int32_t who);

// ---------------------------------------------------------------- acquisition (combat-spec 2)

int32_t compare_target(Game& g, Object& s, int32_t ox, int32_t whom, bool unchecked, bool easy);  // @64e5c0
bool poor_target(Game& g, Object& s, int32_t ox, int32_t whom);                                // @64a270
// Object::check_target @649e00 (dist may be null).
bool check_target(Game& g, Object& s, int32_t ox, int32_t whom, bool on_duty, int32_t* dist, bool guarding,
                  bool use_poor, bool facing);
// Object::find_nearby_target @648da0 -> ox (or -1), *whom.
int32_t find_nearby_target(Game& g, Object& s, int32_t r, int32_t* whom, bool act, bool facing, int32_t filter);
// Unit::find_melee_target @5ff9c0 (whom may be null).
int32_t find_melee_target(Game& g, Unit& u, int32_t r, int32_t* whom, bool facing, bool act, int32_t filter);
bool on_duty(const Unit& u);                                            // Unit::on_duty @5fff70
int32_t think_attack(Game& g, Unit& u);                                 // Unit::think_attack @5f5a80
void think_combat(Game& g, Unit& u);                                    // the combat part of Unit::think @5f6e40
int32_t find_new_target(Game& g, Unit& u, int32_t* whom, bool stand_ground);  // Unit::find_new_target @5ff6a0
void target_opportunity(Game& g, Unit& u, int32_t ox, int32_t whom, bool from_group);  // Unit::target_opportunity @5fffc0
void group_target_opportunity(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t member_o, bool from_unit);  // @7107d0

// ---------------------------------------------------------------- attack orders (combat-spec 3)

// Unit::add_attack_order @5e5410.
Order* add_attack_order(Game& g, Unit& u, int32_t ox, int32_t whom, QueuePos q, int32_t mandatory, bool expl);
// UnitData::find_def_pos @608560.
bool find_def_pos(const Game& g, const Unit& u, int32_t* x, int32_t* y);
const Order* get_activity(const Unit& u);  // UnitData::get_activity @608370
int32_t do_attack(Game& g, Unit& u, Order& a);          // Unit::do_attack @5f1b80
int32_t check_target_path(Game& g, Unit& u, Order& act);  // Unit::check_target_path @5e22d0
// Unit::find_attack_pos @601280 (from (fx, fy)) and @602e60 (own position).
int32_t find_attack_pos(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t p3, int32_t* rx, int32_t* ry, int32_t fx,
                        int32_t fy, int32_t p8);
int32_t find_attack_pos(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t p3, int32_t* rx, int32_t* ry, int32_t p8);
// Unit::change_target @5e36c0 (do_move's ranged retarget): the unit and its o_down chain swap (oox, owhom) for
// (nox, nwhom) and drop the approach moves.
void unit_change_target(Game& g, Unit& u, int32_t oox, int32_t owhom, int32_t nox, int32_t nwhom);
void do_attack_to_scan(Game& g, Unit& u, Order& m);    // the combat half of Unit::do_attack_to @5f2320
void do_attack_to_pause(Game& g, Unit& u, Order& m);   // Unit::do_attack_to_pause @5f22a0
int32_t do_group_attack(Game& g, Unit& u, Order& a);   // Unit::do_group_attack @5e75a0
void do_group_attack_to_scan(Game& g, Unit& u, Order& m);  // the combat half of do_group_attack_to @5e74e0
// Group::action_attack @712490, distribute_attack @713390, is_attacking_near @710e40.
void group_action_attack(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t mandatory, QueuePos q, int32_t ignore);
void group_distribute_attack(Game& g, Group& gr, int32_t ox, int32_t whom);
bool group_is_attacking_near(Game& g, Group& gr, int32_t x, int32_t y);
bool group_is_attacking_to(Game& g, Group& gr);  // Group::is_attacking_to (TODO: spec; see combat_orders.cpp)
bool group_member(const Game& g, const Group& gr, int32_t o, int32_t who);  // GroupData::member(o, who, 1)

// ---------------------------------------------------------------- firing (combat-spec 4)

// Unit::fight @5fd4d0 (p3 mandatory / keep target, p4 temporary, p5 cavalry-archer mode). 1 = attacked this call.
int32_t fight(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t p3, int32_t p4, int32_t p5);
void cavarch_fight(Game& g, Unit& u);                 // Unit::cavarch_fight @5ff4b0
int32_t set_attack(Game& g, Unit& u, int32_t ox, int32_t whom);  // Unit::set_attack @5fce70
void set_attacking(Game& g, Unit& u, int32_t whom);   // Unit::set_attacking @5ff5b0
void set_in_danger(Game& g, Unit& u, bool flag);       // Unit::set_in_danger @5fcfb0
void fire_ammo(Game& g, Object& s, int32_t ox, int32_t whom);  // Object::fire_ammo @64c8b0
// The GameDataPackage that add_ammo / Ammo::init receive.
struct AmmoPackage {
	int32_t x = 0, y = 0, z = 0;
	uint32_t angle = 0;  // shooter angle, or the release node for event shots
	int32_t who = -1, o = -1, whom = -1, ox = -1;
	int32_t gpiece = -1;  // ammo piece
};
int32_t add_ammo(Game& g, const AmmoPackage& pkg);   // Objects::add_ammo @658b10
void ammo_inc_time_all(Game& g);                      // the Ammo pass of Objects::inc_time
void guy_execute_events(Game& g, Unit& u, Guy& gy);  // Guy::execute_events @5d99c0 (release events only)
void build_process_attack(Game& g, Build& b);         // the attack part of Build::process @61edf0
int32_t build_do_attack(Game& g, Build& b);           // Build::do_attack @6228f0
int32_t get_garrison_arrows(const Game& g, const Build& b);  // BuildData::get_garrison_arrows @62e3d0
int32_t get_shot(const Game& g, const Object& b);     // BuildData::get_shot @62dd90

// ---------------------------------------------------------------- healing, supply, attrition (combat-spec 7; attrition.cpp)
void process_healing(Game& g, Unit& u);                    // Unit::process_healing @5e0670
// Unit::repair_damage(hp, show, conquest) @60de10: heals the chain from u down and regenerates dead members.
void unit_repair_damage(Game& g, Unit& u, int32_t hp, bool show, bool conquest);
int32_t process_attrition(Game& g, Unit& u);              // Unit::process_attrition @5e11a0 (sets u.attrition)
bool process_supply(Game& g, Unit& u);                     // Unit::process_supply @5e0560: supplied this tick
int32_t suffer_attrition(Game& g, Unit& u, bool show);     // Unit::suffer_attrition @5e1a10: take_damage's result
int32_t unit_get_attrition(const Game& g, const Unit& u, int32_t owner);  // UnitData::get_attrition @608fd0
bool unit_in_supply(const Game& g, const Unit& u, int32_t* sup);         // UnitData::in_supply @609ef0
bool unit_has_damage(const Game& g, const Unit& u, bool from_here);      // UnitData::has_damage @609b50
void leader_calc_attrition(Game& g, int32_t who);          // Leader::calc_attrition @6cdea0
void leader_calc_anti_attrition(Game& g, int32_t who);     // Leader::calc_anti_attrition @6cdcc0
int32_t get_heal_level(const Game& g, int32_t who);        // LeaderData::get_heal_level @6e12e0: 0..3
int32_t get_supply_upgrade(const Game& g, int32_t who);    // LeaderData::get_supply_upgrade @6e0880: 0..3
// Supplies (economy-spec 7.2): init_supply @73ad40 / close_supply @73acc0 / find_supply @73aba0 (first live on-map
// supply unit of `who` within its radius of (x, y), or -1).
int32_t supplies_init_supply(Game& g, int32_t who, int32_t o);
void supplies_close_supply(Game& g, int32_t who, int32_t s);
int32_t supplies_find_supply(const Game& g, int32_t x, int32_t y, int32_t who);

}  // namespace ron::sim
