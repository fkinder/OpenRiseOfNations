// Production (docs/production-spec.md): the leader's tech and availability predicates (techs.cpp), costs and times
// (costs.cpp), the building data model, creation and placement rules (build.cpp, build_place.cpp), construction,
// completion and per-frame building processing (construct.cpp), cities (cities.cpp), training queues, spawning and
// rally points (queues.cpp), research (gain_tech in techs.cpp), repair and deletion (construct.cpp).
//
// Conventions as the rest of engine/sim: int32 C arithmetic, every game_random draw where the original draws
// (production-spec section 10), loops in the original's order. Functions are named after the original methods;
// `who` is the leader, `o` an object slot.
#pragma once

#include <array>
#include <cstdint>

#include "sim/objects.hpp"
#include "sim/placement.hpp"

namespace ron::sim {

class Game;
struct Leader;
struct City;
struct Group;

// TypeIndex values production tests (re/types/typeindex.tsv).
namespace ptype {
constexpr int32_t kFood = 0, kTimber = 1, kWealth = 2, kKnowledge = 3, kMetal = 4, kOil = 5;
constexpr int32_t kPeasants = 50, kPeasantsKorean = 51, kScholars = 52, kScholarsKorean = 53, kGeneral = 54, kSpy = 58,
                  kCara = 59, kMerchant = 61, kMerchantDutch = 62, kMilitia = 66, kMinuteman = 67, kPartisan = 68,
                  kScout = 69, kSlingers = 82, kRifleman = 100, kInfantry = 102, kMechInfantry = 104,
                  kMachineGun = 123, kHoplites = 132, kBowmen = 170, kHorse = 209, kAaGun = 281, kJetFighter = 295,
                  kBomber = 304, kFighterBomber = 308, kJetFighterBomber = 309, kHelicopter = 310, kNuke = 315,
                  kIcbm = 316, kTransportBarge = 320, kBark = 323, kFireRaft = 334, kCarrier = 351, kThePresident = 355,
                  kCharles = 369, kFurTrapper = 400;
constexpr int32_t kVillage = 414, kTown = 415, kMetropolis = 416, kFarm = 417, kWoodcutter = 418, kMine = 419,
                  kUniversity = 420, kOilWell = 421, kOilPlatform = 422, kGranary = 423, kLumberMill = 424,
                  kSmelter = 425, kRefinery = 426, kBarracks = 427, kStable = 428, kAutoPlant = 429,
                  kSiegeFactory = 430, kFactory = 431, kDock = 432, kAnchorage = 433, kShipyard = 434, kLibrary = 435,
                  kMarket = 436, kTemple = 437, kSenate = 438, kTower = 439, kFortX = 443, kAirbase = 447,
                  kMissileSilo = 520, kLookout = 521, kAirDefense = 523;
constexpr int32_t kPyramids = 526, kColossus = 527, kHangingGardens = 528, kTerraCotta = 529, kColosseum = 530,
                  kForbiddenCity = 531, kTikal = 532, kPorcelain = 533, kRedFort = 534, kAngkorWat = 535,
                  kVersailles = 536, kLiberty = 537, kKremlin = 538, kTajMahal = 539, kEiffel = 540,
                  kSupercollider = 541, kSpaceProgram = 542;
constexpr int32_t kClassicalAge = 544, kMedievalAge = 545, kModernAge = 549, kInformationAge = 550,
                  kWrittenWord = 551, kComputerization = 557, kBarter = 558, kGlobalization = 564, kCivic = 565,
                  kInternationalLaw = 571, kMilitaryBase = 572, kSelectiveService = 578, kFinals = 579,
                  kGovBase = 623, kGovCapitalism = 628;
constexpr int32_t kDepopulate = 646, kRaze = 667, kDisband = 666, kCivilianSpell = 659;
constexpr int32_t kAllyLos = 688, kExploreMapBonus = 690, kActivePingBonus = 691, kRevealEnemyBonus = 692,
                  kColonizeBonus = 687, kBuySell = 685, kGlobalGovernmentBonus = 697, kArmageddonBonus = 694,
                  kInstantUnitBonus = 696;
}  // namespace ptype

// TypeData range predicates (TypeData::is_*_type @4705b0.. 470870; vtable slots 2..17 of every Type).
inline bool is_good_type(int32_t t) { return t < 50; }
inline bool is_unit_type(int32_t t) { return t >= 50 && t < 414; }
inline bool is_build_type(int32_t t) { return t >= 414 && t < 543; }
inline bool is_wonder_type(int32_t t) { return t >= 526 && t < 543; }
inline bool is_tech_type(int32_t t) { return t >= 544 && t < 629; }
inline bool is_age_type(int32_t t) { return t >= 544 && t < 551; }
inline bool is_epoch_type(int32_t t) { return t >= 551 && t < 579; }
inline bool is_final_type(int32_t t) { return t >= 579 && t < 583; }
inline bool is_plain_tech_type(int32_t t) { return t >= 579 && t < 629; }  // tech, not age, not epoch
inline bool is_gov_type(int32_t t) { return t >= 623 && t < 629; }
inline bool is_gov_hero_type(int32_t t) { return t >= 352 && t < 358; }
inline bool is_spell_type(int32_t t) { return t >= 629 && t < 684; }
inline bool is_bonus_type(int32_t t) { return t >= 684 && t < 806; }
// TypeData::num_preq (vtable slot 1): techs and buildings 3, units, goods and bonuses 2, everything else 1.
int32_t num_preq(int32_t t);

// ---------------------------------------------------------------- techs.cpp: leader predicates (production-spec 7.9)

// TypeData::get_preq(o, i, who) @668700 with the live GameInfo, plus the Iroquois early-government rule (7.9.3).
int32_t get_preq(const Game& g, int32_t t, int32_t i, int32_t who);
bool has_tech(const Game& g, int32_t who, int32_t t);          // LeaderData::has_tech @6e0c80
bool has_rare(const Game& g, int32_t who, int32_t good);       // LeaderData::has_rare @6e0770
int32_t tribe_can_type(const Game& g, int32_t who, int32_t t);  // LeaderData::tribe_can_type @6d9380: 4 or 0
int32_t get_graft(const Game& g, int32_t who, int32_t t);       // LeaderData::get_graft @6e30f0
int32_t type_eligible(const Game& g, int32_t who, int32_t t, int32_t check);  // @6dbd10
int32_t type_avail(const Game& g, int32_t who, int32_t t, int32_t check);     // @6e33a0: 0, 2 (research) or 4
int32_t current_upgrade(const Game& g, int32_t who, int32_t t);  // LeaderData::current_upgrade @6e3140
int32_t starting_age(const Game& g, int32_t who);                // @6d7320
bool all_techs(const Game& g, int32_t who);                      // @6d7370
int32_t techs_per_age(const Game& g, int32_t who, int32_t t);    // @6d7280
int32_t compute_epoch(const Game& g, int32_t who, int32_t cat);  // @6d6f80
int32_t get_govs_taken(const Game& g, int32_t who);              // @6d69f0
// LeaderData::researching(t, bld, only_bld, exact) @6db510.
bool researching(const Game& g, int32_t who, int32_t t, int32_t bld, bool only_bld, bool exact);
bool has_tech_queued(const Game& g, int32_t who, int32_t t);     // @6d73b0
int32_t get_first_library(const Game& g, int32_t who);           // @6db6c0
int32_t get_building_cities(const Game& g, int32_t who);         // @6e06f0
int32_t get_buildings(const Game& g, int32_t who, int32_t t, int32_t n);  // @6e0680 (adds the `to` chain)
int32_t get_queued(const Game& g, int32_t who, int32_t t, int32_t base);  // @6e0d50
int32_t get_gov_hero(const Game& g, int32_t who);                // @6e0600
int32_t get_radius(const Game& g, int32_t who, int32_t type);    // LeaderData::get_radius @6db790 (tcoords)
int32_t get_city_limit(const Game& g, int32_t who);              // @6d6130
int32_t get_total_cities(const Game& g, int32_t who);            // @6d6060
int32_t get_wonders(const Game& g, int32_t who);                 // @6db680: live own wonder records
int32_t get_unbuilt_wonders(const Game& g, int32_t who);         // @6da290
// The speed-upgrade counters (count of the 3-entry bonus family held, 7.11).
int32_t get_building_speed_upgrade(const Game& g, int32_t who);  // BUILDINGS_FASTER_1..3 (754..756)
int32_t get_troops_speed_upgrade(const Game& g, int32_t who);    // 742..744
int32_t get_ships_speed_upgrade(const Game& g, int32_t who);     // 748..750
int32_t get_vehicle_speed_upgrade(const Game& g, int32_t who);   // 760..762
int32_t get_building_hp_upgrade(const Game& g, int32_t who);     // 757..759
bool check_population(const Game& g, int32_t who, int32_t t);    // LeaderData::check_population @6e1330
int32_t get_caravan_limit(const Game& g, int32_t who, int32_t p);  // @6dca50 (TODO: bonuses)
int32_t get_units(const Game& g, int32_t who, int32_t t, int32_t p);  // LeaderData::get_units @6e07d0
// Leader bookkeeping (production-spec 6.10).
void track_unit_type(Game& g, int32_t who, int32_t t, int32_t d, int32_t o);  // Leader::track_unit_type @6e0dd0
void track_queued(Game& g, int32_t who, int32_t t, int32_t d);               // Leader::track_queued @6e0f30
void calc_pop_cap(Game& g, int32_t who);                                     // Leader::calc_pop_cap @6dc490
void bucket_add(Game& g, int32_t who, int32_t good, int32_t amount);         // LeaderData::bucket_add @43ed10

// Leader::init's tech part @6e3930 (7.9.7) and the tribe free techs; called by Game::load per scenario leader.
void leader_init_techs(Game& g, int32_t who);
void reset_obs_flags(Game& g, int32_t who);  // Leader::reset_obs_flags @6e32a0
// Leader::gain_tech(t, x, y, notify, upgrade_existing) @6dcb60 (the 44 ordered steps of 7.1.1, as far as the
// simulation has the systems they touch; see techs.cpp).
void gain_tech(Game& g, int32_t who, int32_t t, int32_t x, int32_t y, bool notify, bool upgrade_existing);
void check_transport(Game& g, int32_t who);  // Leader::check_transport @6bc5f0
// Leaders::process_all's stat recalculation (7.7): CALC_WALL_STATS then CALC_UNIT_STATS.
void calc_wall_stats(Game& g, int32_t who);  // Leader::calc_wall_stats @6cf7c0
void calc_unit_stats(Game& g, int32_t who);  // Leader::calc_unit_stats @6cf970
void leaders_process_stats(Game& g);

// LeaderFlagIndex bits production adds to sim/objects.hpp's leaderflag::.
namespace leaderflag {
constexpr uint32_t kKremlinSpy = 0x10000000, kNewTech = 0x1000000, kCalcUnitStats = 0x4000000,
                   kCalcWallStats = 0x8000000, kAssimilateImmed = 0x200000, kIgnorePopcap = 0x100000;
}

// ---------------------------------------------------------------- costs.cpp (production-spec 2)

// TypeData::get_cost(r, who, o, loc, refund, convert, type_override) @664090.
int32_t get_cost(const Game& g, int32_t t, int32_t r, int32_t who, int32_t o, int32_t loc, int32_t refund,
                 int32_t convert, int32_t type_override);
// TypeData::can_pay_cost(who, loc, o, total) @667570: the largest quotient (0 = cannot pay; 10 = free).
int32_t can_pay_cost(const Game& g, int32_t t, int32_t who, int32_t loc, int32_t o, int32_t total);
// Type::pay_cost(who, o, loc, from_escrow, out[6]) @6681f0 (out may be null).
void pay_cost(Game& g, int32_t t, int32_t who, int32_t o, int32_t loc, int32_t from_escrow, int32_t* out);
// Type::unpay_cost(who, o, loc) @6682f0 (recomputed price with refund = 1).
void unpay_cost(Game& g, int32_t t, int32_t who, int32_t o, int32_t loc);
int32_t sum_cost(const Game& g, int32_t t, int32_t who, int32_t o, int32_t refund);  // Type::sum_cost @668080
int32_t get_support_count(const Game& g, int32_t who, int32_t t);                     // @6da110
int32_t calc_science_discount(const Game& g, int32_t who, int32_t t, int32_t v);      // @6da630
int32_t cost_factor_dow(const Game& g, int32_t who, int32_t target, int32_t t);       // @6da590
uint32_t type_time(const Game& g, int32_t t, int32_t who);           // TypeData::time @663f20 (1/100 frame)
uint32_t type_research_time(const Game& g, int32_t t, int32_t who);  // TypeData / UnitTypeData::research_time
// ObjectData::train_time(t) @6508c0 for the producer object (o, who) (o < 0: no producer object).
int32_t train_time(const Game& g, int32_t o, int32_t who, int32_t t);
// Wall::update_construct_time @63d560 (the value for type t and leader who) and BuildData::construct_time @62d5c0.
uint32_t update_construct_time(const Game& g, int32_t who, int32_t t);
int32_t construct_time(const Game& g, const Build& b, int32_t all);
// The producer test of get_cost / train_time: BuildData::is_unassimilated @62d470.
bool is_unassimilated(const Game& g, const Build& b);

// ---------------------------------------------------------------- build.cpp: buildings (production-spec 3.3, 4, 5)

// Objects::init_build(who, type, x, y, preplaced, forced) @65d190 -> Build::init @629740. Returns o or < 0.
int32_t init_build_site(Game& g, int32_t who, int32_t type, int32_t x, int32_t y, bool preplaced, int32_t forced);
// Build::activate(loading, msg, count) @623e20.
void build_activate_full(Game& g, Build& b, bool loading, bool msg, bool count);
// Build::start(msg) @6273a0 -> Wall::start @63e810.
void build_start(Game& g, Build& b, bool msg);
// Wall::update_hits(full) @63f0d0: myhits and construct_hits; returns hits.
int32_t wall_update_hits(Game& g, Build& b, bool full);
// GraphicPieces::get_build_gpiece @90bce0 (the tribe of `who`, building age `age`) and Wall::update_gpiece @643070 (+0x58 from the founder's tribe
// and the building's max_age).
int32_t build_gpiece(const Game& g, int32_t type, int32_t who, int32_t age);
void build_update_gpiece(Game& g, Build& b);
// Wall::increment_stats @643270 / decrement_stats @642da0.
void wall_increment_stats(Game& g, Build& b);
void wall_decrement_stats(Game& g, Build& b);
// Build::find_gather_tiles @623350 (RNG shuffle) and BuildTypeData::max_gatherers @63c430.
void find_gather_tiles(Game& g, Build& b);
int32_t max_gatherers(Game& g, int32_t type, int32_t o, int32_t who, int32_t tx, int32_t ty);
// Object::disband(full) @6455c0 for a building (no RNG; refunds).
void build_disband(Game& g, Build& b, bool full);
// Build::close (death or disband) @628980: queue refund, stats, city / fort / wonder records.
void build_close_production(Game& g, Build& b, int32_t dt);
// BuildTypeData predicates.
bool is_military_trainer(const Game& g, int32_t t);   // @63bcf0
bool is_training_building(const Game& g, int32_t t);  // @639de0
int32_t basic_type(const Game& g, int32_t t);         // @639970
int32_t build_stance_type(const Game& g, int32_t t);  // BuildTypeData::get_stance_type @6396c0
int32_t get_garrison_limit(const Game& g, int32_t t, int32_t who);  // @633f50
bool is_gather_type(const Type& t);                   // build_flags & 0x40
bool is_flat_gather(const Type& t);                   // build_flags & 0x10000000

// ---------------------------------------------------------------- build_place.cpp (production-spec 3.5-3.8)

// BlockIndex codes of blocked_site.
namespace block {
constexpr int32_t kNone = 0, kBuilding = 1, kMountain = 2, kRock = 3, kRuins = 4, kNoOil = 5, kForest = 6, kRare = 7,
                  kNoResources = 9, kNoForest = 10, kForestTaken = 11, kNoMountain = 12, kMountainTaken = 13,
                  kWater = 14, kLand = 15, kDockLand = 16, kDockWater = 17, kDockTerr = 18, kCityDistance = 20,
                  kFortDistance = 21, kFortCityDistance = 22, kTerritory = 23, kEnemyTerritory = 24,
                  kEnemyTerritory2 = 25, kNeutralTerritory = 26, kColonize = 28, kPortCity = 29, kPortFort = 30,
                  kOutsideRadius = 31, kOutsideTown = 32, kRazingTown = 33, kOffMap = 34, kUnseen = 35, kSeen = 36,
                  kOne = 39, kOneOther = 40, kFarm = 41, kWonder = 42, kNeedWall = 43, kRiver = 44, kLostCity = 45,
                  kCantTransport = 46;
}
// BuildTypeData::blocked_site(cx, cy, who, o_self, *gather) @636a50.
int32_t blocked_site(Game& g, int32_t type, int32_t cx, int32_t cy, int32_t who, int32_t o_self, int32_t* gather);
// BuildTypeData::snap_center(x, y, who) @636190 with the dock / mine / woodcutter search when who >= 0.
void snap_center_who(Game& g, int32_t type, int32_t x, int32_t y, int32_t* ox, int32_t* oy, int32_t who);
// BuildTypeData::get_town(tx, ty, who) @639b90: the city object o whose radius covers the footprint, or -1.
int32_t get_town(const Game& g, int32_t type, int32_t tx, int32_t ty, int32_t who);
// ObjectsData::find_city_at(x, y, who, type, skip_check) @65b870: city index or -1.
int32_t find_city_at(const Game& g, int32_t x, int32_t y, int32_t who, int32_t type, int32_t skip_check);

// ObjectsData::find_building_placed_at(tx, ty, owner (-1 any), o_self, who_self) @658c80: a building (any state) of
// leaders 0..7 whose footprint covers the tcoord, searched over the 9 wcoord object lists around it (only when the
// tcoord is BUILDING or STARTED). *who_out = objects.find_who.
int32_t find_building_placed_at(const Game& g, int32_t tx, int32_t ty, int32_t owner, int32_t o_self, int32_t who_self,
                                int32_t* who_out);

// ---------------------------------------------------------------- construct.cpp (production-spec 4, 8)

// Group::action_build @707510 (the BUILD command), Group::action_swarm_around @70fbe0.
void group_action_build(Game& g, Group& gr, int32_t x, int32_t y, int32_t type, QueuePos q);
void group_action_swarm_around(Game& g, Group& gr, int32_t ox, int32_t whom, QueuePos q, int32_t order, bool expl);
// Unit::add_build_order @5e5210 / add_repair_order @5e4ff0.
Order* add_build_order(Game& g, Unit& u, int32_t ox, int32_t whom, QueuePos q, bool expl);
Order* add_repair_order(Game& g, Unit& u, int32_t ox, int32_t whom, QueuePos q, bool expl);
// Unit::do_build @5eebf0 (BUILD_AT handler) and Unit::do_repair @5ee420 (REPAIR handler); return do_job's value.
int32_t do_build(Game& g, Unit& u, Order& o);
int32_t do_repair(Game& g, Unit& u, Order& o);
// Wall::do_construct(amount) @6434d0: 1 when this call completed the building.
int32_t do_construct(Game& g, Build& b, int32_t amount);
// Build::process @61edf0 (with Wall::process @640450) and Wall::inc_time @63fb60.
void build_process(Game& g, Build& b);
void wall_inc_time(Game& g, Build& b);
// Group::action_disband @70e260 (DISBAND command).
void group_action_disband(Game& g, Group& gr, int32_t all);

// ---------------------------------------------------------------- cities.cpp (production-spec 5)

int32_t city_get_level(int32_t type);       // CityData::get_level @739340
int32_t city_get_pop_value(int32_t type);   // CityData::get_pop_value @738450
int32_t city_get_radius(const Game& g, int32_t who, int32_t city);  // CityData::get_radius @738410
void build_find_city(Game& g, Build& b);    // Build::find_city @628180
void build_add_to_city(Game& g, Build& b, int32_t c);  // Build::add_to_city @622380
void build_remove_from_city(Game& g, Build& b);        // Build::remove_from_city @622030
void city_find_buildings(Game& g, int32_t who, int32_t c);  // City::find_buildings @7384c0
void city_check_upgrade(Game& g, int32_t who, int32_t c);   // City::check_upgrade @738b20
int32_t city_count_buildings(const Game& g, int32_t who, int32_t c, int32_t type, int32_t p3, int32_t p4);
int32_t city_num_wonders(const Game& g, int32_t who, int32_t c, int32_t p);
void regions_fix_all_borders(Game& g);      // Regions::fix_all_borders @67f7d0 (resets Region.borders 0..63)
// GameDaemon::check_borders @732060 (frame step 11): drives World::compute_reg_territory @6b0bb0 (sim/territory.hpp).
void check_borders(Game& g);
bool has_wonder_prod(const Game& g, int32_t who, int32_t type);  // LeaderData::has_wonder @6ebc10 != 0
int32_t has_wonder_bits(const Game& g, int32_t who, int32_t type);  // 1 own wonder in a city, 2 CTW conquest wonder
// LeaderData::find_capital(&c, &w, skip_c, skip_w) @6eb930.
void find_capital(const Game& g, int32_t who, int32_t* c, int32_t* w, int32_t skip_c, int32_t skip_w);
void city_assimilate(Game& g, int32_t who, int32_t c);  // City::assimilate @738e90
void city_close(Game& g, int32_t who, int32_t c, int32_t attacker);  // City::close @737550 (attacker or -1)
// Wall::mask_city(add, radius) @63e310: CITY bits on the first even_circle_radius[r] points around the centre tcoord.
void wall_mask_city(Game& g, Build& b, bool add, int32_t radius);
// Wall::mask_me(add, regen) @642fc0: BuildType::mask_me plus the CITY bits (Wall::mask_city) of city buildings.
void wall_mask_me(Game& g, Build& b, bool add, Regen regen);

// ---------------------------------------------------------------- queues.cpp (production-spec 6)

// Group::action_queue_up(t, num) @6fdbb0 (QUEUE_UP), Build::queue_up(t, use_escrow) @620f40 (0 = queued).
void group_action_queue_up(Game& g, Group& gr, int32_t t, int32_t num);
int32_t build_queue_up(Game& g, Build& b, int32_t t, int32_t use_escrow);
int32_t build_can_make(const Game& g, const Build& b, int32_t t, int32_t p);  // BuildData::can_make @62db10
bool build_could_queue(Game& g, const Build& b, int32_t t);                  // BuildData::could_queue @62da50
bool queue_here(const Game& g, int32_t bt, int32_t t);                        // BuildTypeData::queue_here @639d10
void build_do_queue(Game& g, Build& b, int32_t slot);                         // Build::do_queue @61e410
int32_t build_finished(Game& g, Build& b, int32_t t);                         // Build::finished @628490
int32_t build_train(Game& g, Build& b, int32_t t);                            // Build::train @62f9b0
int32_t unit_come_out(Game& g, Unit& u, int32_t param);                       // Unit::come_out @617c10
void build_unqueue(Game& g, Build& b, int32_t i, bool cancel);               // Build::unqueue @6207c0
void build_action_unqueue(Game& g, Build& b, int32_t i);                      // Build::action_unqueue @620280
void build_clean_queue(Game& g, Build& b, bool refund);                       // Build::clean_queue @620b60
int32_t build_count_queue(const Game& g, const Build& b, int32_t kind, int32_t t);  // BuildData::count_queue
void build_add_gather_point(Game& g, Build& b, int32_t x, int32_t y, int32_t action, QueuePos q);
void build_clear_gather(Game& g, Build& b);
bool build_gather_inside(const Build& b);  // BuildData::gather_inside @46f180
void group_action_gather_point(Game& g, Group& gr, int32_t x, int32_t y, int32_t action, int32_t add_to_end);
void group_action_buildmask(Game& g, Group& gr, int32_t mask, int32_t set);
void build_new_library(Game& g, Build& b);  // Build::new_library @627fe0
// Unit::set_type(T, mode) @612fa0 / Wall::set_type @640da0 (mode: 0 NORMAL, 1 INIT, 2 LOAD).
void unit_set_type(Game& g, Unit& u, int32_t t, int32_t mode);
void wall_set_type(Game& g, Build& b, int32_t t, int32_t mode);
// Unit::go_inside(o, who) @61a2e0 (the captain and its o_down chain leave the map into the container).
void unit_go_inside(Game& g, Unit& u, int32_t o, int32_t who);

}  // namespace ron::sim
