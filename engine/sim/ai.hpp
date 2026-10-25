// The computer opponent (docs/ai-spec.md): Leader::init's AI part and the personality draws, the strategy census
// (Leader::plan_strategy), sites, the production state machine (Leader::production_ai) and its steps, the make list
// and purchases, AI building placement, armies (Armies / Army), the per-unit AI and the diplomacy AI.
//
// Modules: ai_setup.cpp (setup, helpers, the ObjectsData searches the AI uses), ai_strategy.cpp (plan_strategy,
// check_orphaned_buildings, compute_sites), ai_production.cpp (production_ai and its steps, make list, market AI,
// make_this and the produce_* functions), ai_create.cpp (create_units, create_buildings), ai_place.cpp
// (produce_building), ai_army.cpp (armies), ai_unit.cpp (per-unit AI), ai_diplo.cpp (Leader::diplomacy).
//
// Conventions as the rest of engine/sim: int32 C arithmetic (truncating division), every game_random draw where the
// original draws (ai-spec 13), loops in the original's order. The AI calls the simulation directly (Group::action_*,
// Build::queue_up, Objects::init_build), never through commands.
#pragma once

#include <cstdint>

#include "sim/ai_types.hpp"

namespace ron::sim {

class Game;
struct Unit;
struct Group;

// ---------------------------------------------------------------- ai_setup.cpp

// Leader::init's AI part @6e3930 (after the economy, before the scenario's child chunks): prod_script = "", and for
// AI seats prod_script_run = 1, random_personality, the lobby player-flag overrides, the script choice (with the coin
// when rush == 0), then the sites / make list / lists reset.
void leader_init_ai(Game& g, int32_t who);
void random_personality(Game& g, int32_t who);  // Leader::random_personality @6cfd00 (ai-spec 2.3)

// LeaderData::get_diff @6ec000 (ai-spec 0).
int32_t get_diff(const Game& g, int32_t who);
bool is_ai_seat(const Game& g, int32_t who);  // (leader_flags & 0xc) != 4
bool is_rusher(const Game& g, int32_t who);   // LeaderData::is_rusher @6d61f0 (player flag 0x1000)
bool is_boomer(const Game& g, int32_t who);   // LeaderData::is_boomer @6d6190 (player flag 0x2000)
bool mutual_ally_ai(const Game& g, int32_t a, int32_t b);  // diplos 2 both ways (or a == b)
int32_t get_team_terr(const Game& g, int32_t who);   // LeaderData::get_team_terr @6d62e0
int32_t get_mod_resource_cap(const Game& g, int32_t who, int32_t good);  // @6d65b0
int32_t get_lowest_epoch(const Game& g, int32_t who);   // @6d6ed0
int32_t get_highest_epoch(const Game& g, int32_t who);  // @6d6f30
int32_t get_farm_limit(const Game& g, int32_t who);     // @6db270
int32_t get_target(const Game& g, int32_t who);         // LeaderData::get_target @6da000
bool is_tribute_period(const Game& g, int32_t a, int32_t b);  // LeaderData::is_tribute_period @6d5b90
int32_t leaders_max_age(const Game& g);                 // Leaders::max_age @6b8370
int32_t leaders_max_enemy_age(const Game& g, int32_t who);  // @6b8300
int32_t leaders_max_human_age(const Game& g);           // @6b8470
int32_t leaders_max_human_cities(const Game& g);        // @6b8580
int32_t leaders_best_human_age_stamp(const Game& g, int32_t age_type);  // @6b81e0
int32_t queued_units(const Game& g, int32_t who);       // Leader::queued_units @6ce000
bool war_allowed(const Game& g);                        // Game::war_allowed @594670
int32_t get_armageddon(const Game& g);                  // Game::get_armageddon @594020
int32_t get_nukes(const Game& g, int32_t who);          // LeaderData::get_nukes
int32_t wonder_winning(const Game& g);                  // Game::wonder_winning @5948a0
int32_t get_wonder_net(const Game& g, int32_t who);     // LeaderData::get_wonder_net @6ebb10
int32_t get_team_wonder_value(const Game& g, int32_t who);   // @6da990
int32_t get_enemy_wonder_value(const Game& g, int32_t who);  // @6da8f0
int32_t get_enemy_unbuilt_wonder_value(const Game& g, int32_t who);  // @6d5e40
int32_t city_level(const Game& g, int32_t who, int32_t c);   // CityData::get_level of the city building
int32_t city_num_buildings(const Game& g, int32_t who, int32_t c);  // CityData::num_buildings @738190
int32_t city_get_building(const Game& g, int32_t who, int32_t c, int32_t t);  // CityData::get_building @739480
// City::count_gather_slots @737dc0: gs[g] = Σ gather_max, fs[g] = Σ (gather_max − gatherers); returns Σ gs except
// knowledge. gs / fs may be null.
int32_t city_count_gather_slots(const Game& g, int32_t who, int32_t c, int32_t* gs, int32_t* fs);
bool city_could_queue(Game& g, int32_t who, int32_t c, int32_t t);  // City::could_queue @737fa0

// The ObjectsData searches (Objects.obj; Search::valid_search / valid_filter). They write g.find_dist / g.find_who.
// SearchIndexBH values and the FilterIndex values the AI passes.
namespace search {
constexpr int32_t kAll = 0, kFriendly = 1, kPeace = 2, kEnemy = 3, kAllied = 4, kNonAllied = 5;  // SearchIndexBH
}
namespace filter {  // FilterIndex
constexpr int32_t kAll = 0, kType = 1, kBaseType = 2, kNotMe = 3, kNotUnderAttack = 8, kCombat = 9, kTarget = 11,
                  kNotBusy = 12, kDomain = 13, kNotDomain = 14;
}
bool ai_has_market(const Game& g, int32_t who);  // LeaderData::has_market @6d5410
// ObjectsData::find_city @65ba90 (any leader passing the search relation; 0x200 = same region).
int32_t objects_find_city(const Game& g, int32_t x, int32_t y, int32_t search, int32_t who, int32_t flags,
                          int32_t filter, int32_t f1, int32_t f2);
// ObjectsData::find_unit @65ca80 (radius < 0: no limit; flags 0x200 same region, 0x10000 captains only, 0x20000
// units inside something too).
int32_t objects_find_unit(const Game& g, int32_t x, int32_t y, int32_t search, int32_t who, int32_t radius,
                          int32_t flags, int32_t filter, int32_t f1, int32_t f2, int32_t filter2, int32_t f3,
                          int32_t f4);
// ObjectsData::find_building @65d260.
int32_t objects_find_building(const Game& g, int32_t x, int32_t y, int32_t search, int32_t who, int32_t radius,
                              int32_t flags, int32_t filter, int32_t f1, int32_t f2);

// ---------------------------------------------------------------- ai_strategy.cpp

// Leaders::strategy_all @6ed430 (frame step 10): check_explore, plan_strategy, compute_score, diplomacy.
void leaders_strategy_all(Game& g);
void leader_plan_strategy(Game& g, int32_t who);        // Leader::plan_strategy @6b9620 (ai-spec 3, 4)
void check_orphaned_buildings(Game& g, int32_t who);    // @6c9f20 (ai-spec 4.6)
void compute_sites(Game& g, int32_t who, int32_t force);  // @6cc950 (ai-spec 5.1)
void compute_site_stats(Game& g, int32_t who, int32_t wx, int32_t wy, int32_t city, int32_t o, int32_t reg,
                        int32_t* val, int32_t* dist, int32_t scan, int32_t* bx, int32_t* by);  // @6cd040
void leader_compute_score(Game& g, int32_t who, int32_t p);  // Leader::compute_score @6ec560

// ---------------------------------------------------------------- ai_production.cpp

void leader_production_ai(Game& g, int32_t who);   // Leader::production_ai @6c1960 (ai-spec 3.1)
void production_ai_setup(Game& g, int32_t who);    // @6c83e0
void market_speculation(Game& g, int32_t who);     // @6c8110
int32_t get_need(const Game& g, int32_t who, int32_t good);  // Leader::get_need @6cdc80
void found_cities(Game& g, int32_t who);           // @6c7a60
void research_techs(Game& g, int32_t who);         // @6c6ba0
void upgrade_units(Game& g, int32_t who);          // @6c6430
int32_t unit_prod_value(const Game& g, int32_t who, int32_t t);  // @6cc580
int32_t check_income(Game& g, int32_t who, int32_t t, int32_t k, int32_t o, int32_t total, int32_t city,
                     int32_t need, int32_t* n);    // Leader::check_income @6cc800
void make_list_clear(Game& g, int32_t who);        // MakeList::clear @6c9db0
void make_me(Game& g, int32_t who, int32_t t, int32_t val, int32_t escrow, int32_t cat, int32_t city, int32_t up,
             int32_t num, int32_t wx, int32_t wy);  // MakeList::make_me @6c9be0
bool can_pay(Game& g, int32_t who, int32_t i);     // Leader::can_pay @6c9b90
int32_t make_stuff(Game& g, int32_t who);          // @6c8af0: 1 when item 0 was bought
void use_market(Game& g, int32_t who);             // @6c91c0
int32_t make_this(Game& g, int32_t who, int32_t i);  // @6c94f0: 0 on success
int32_t produce_tech(Game& g, int32_t who, int32_t t, int32_t escrow);               // @6ca980
int32_t produce_upgrade(Game& g, int32_t who, int32_t t, int32_t city, int32_t escrow);  // @6cb5d0
int32_t produce_spell(Game& g, int32_t who, int32_t t, int32_t city, int32_t escrow);    // @6ca720
int32_t produce_unit(Game& g, int32_t who, int32_t t, int32_t city, int32_t num, int32_t escrow);  // @6cb9e0
int32_t produce_city(Game& g, int32_t who, int32_t t, int32_t wx, int32_t wy, int32_t escrow);    // @6cb120

// ---------------------------------------------------------------- ai_create.cpp

void create_units(Game& g, int32_t who);      // @6c40a0 (ai-spec 6.5)
void create_buildings(Game& g, int32_t who);  // @6c1be0 (ai-spec 6.6)

// ---------------------------------------------------------------- ai_place.cpp

// Leader::produce_building(t, city_o, escrow) @6e1400 (ai-spec 7): 0 on success.
int32_t produce_building(Game& g, int32_t who, int32_t t, int32_t city_o, int32_t escrow);
int32_t find_friends(const Game& g, int32_t t, int32_t x, int32_t y, int32_t cidx, int32_t who);  // @639270

// ---------------------------------------------------------------- ai_army.cpp

void armies_init(Game& g);                          // Armies::init @6f3c10 (16 slots per player)
int32_t armies_init_army(Game& g, int32_t who, int32_t city);  // @6f36a0
int32_t armies_init_navy(Game& g, int32_t who, int32_t city, int32_t reg);  // @6f31b0
int32_t armies_find_army(Game& g, int32_t who, int32_t x, int32_t y, int32_t maxd, int32_t u);  // @6f34f0
void armies_process_all(Game& g);                   // Armies::process_all @6f3b00 (frame step 12)
void armies_emergency(Game& g, int32_t who);        // @6f3250
void armies_diplo_change(Game& g, int32_t who);     // @6f30f0
int32_t unit_add_to_army(Game& g, Unit& u);         // Unit::add_to_army @5f7740
void army_add_unit(Game& g, int32_t who, int32_t army, int32_t o);  // Army::add_unit @6f9f40
int32_t object_get_army(const Game& g, const Unit& u);  // Object::get_army @649d70
void army_charge(Game& g, Army& a, int32_t o, int32_t w);  // Army::charge @6f9a90
void army_stop(Game& g, Army& a);                     // Army::stop @6f9180
void armies_leader_defeated(Game& g, int32_t who);    // Armies::leader_defeated @6f2f90
void armies_update_city(Game& g, int32_t old_o, int32_t old_w, int32_t new_o, int32_t new_w);  // @6f2d70
void armies_city_closed(Game& g, int32_t who, int32_t city, int32_t city_o);  // City::close's army part
// Search::valid_filter @67dbb0 for object (cw, co) (the cases the AI and the capture code pass; others pass).
bool search_valid_filter(const Game& g, int32_t cw, int32_t co, int32_t f1, int32_t f2, int32_t filter);
// Search::valid_search @67daa0: does leader `cand` pass the SearchIndexBH relation `s` to `who` (aircraft target scans).
bool search_valid_search(const Game& g, int32_t cand, int32_t s, int32_t who);
void calc_danger(Game& g);  // GameDaemon::calc_danger @732d10 (World.danger, every 200 frames)

// ---------------------------------------------------------------- ai_unit.cpp

// The AI branches of Unit::think after the economy part (ai-spec 12.6 steps 12-16).
void think_ai(Game& g, Unit& u, bool tick);
int32_t think_scout(Game& g, Unit& u, int32_t region_mode);  // Unit::think_scout @5f6010
int32_t ai_think_spellcaster(Game& g, Unit& u);              // Unit::think_spellcaster @5f27a0 (use_generals / use_spies / use_scouts)
void explore_goody(Game& g, Unit& u);  // Unit::explore_goody @5f9780 (G1 draws)
int32_t ai_find_goody_box(Game& g, Unit& u);  // Unit::find_goody_box @5f2540

// ---------------------------------------------------------------- ai_diplo.cpp

void leader_diplomacy(Game& g, int32_t who);  // Leader::diplomacy @6bc950 (ai-spec 10.1)
bool is_neutral(const Game& g, int32_t who);                        // LeaderData::is_neutral @6ebae0
bool is_team_p(const Game& g, int32_t a, int32_t b, int32_t p);     // LeaderData::is_team(b, p) @6ebd30 on a
int32_t num_allies(const Game& g, int32_t who);                     // LeaderData::num_allies @6d6250
int32_t num_team_members(const Game& g, int32_t who, int32_t p);    // LeaderData::num_team_members @6ebf90
// The diplomacy actions (ai-spec 10.1.9; action_offer / action_respond / clear_agree are in economy.hpp).
void leader_set_diplo(Game& g, int32_t who, int32_t t, int32_t d);  // Leader::set_diplo @6ec6a0
// Leader::action_declare(t, d, free, force) @6dab50: 0 done (or already at war), 1 refused.
int32_t leader_action_declare(Game& g, int32_t who, int32_t t, int32_t d, int32_t free, int32_t force);
void leader_consider_tribute(Game& g, int32_t who, int32_t from, int32_t amount, int32_t good);  // @6d1360
void leader_action_clear_all(Game& g, int32_t who, int32_t b);                  // @6d15e0
void leader_action_treaty(Game& g, int32_t who, int32_t b, int32_t t);         // @6d1570
void leader_action_attack(Game& g, int32_t who, int32_t b, int32_t k, int32_t v);  // @6d14e0

}  // namespace ron::sim
