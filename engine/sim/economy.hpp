// The economy (docs/economy-spec.md): starting stockpiles, the income pipeline (Leader::gather -> calc_gather ->
// calc_resource_caps -> do_gather), per-building gather rates, gatherer lists, the GATHER order and its handlers,
// auto-gather, fishermen / merchants / rares, markets (buy / sell), tribute, caravans and trade.
//
// Modules: econ_income.cpp (stockpiles, rates, caps, income, cities, rares), econ_gather.cpp (gatherer lists, GATHER
// orders, Unit::do_gather / do_non_flat_gather, auto-gather, farms), econ_market.cpp (prices, buy / sell, tribute),
// econ_trade.cpp (fishermen, merchants, caravans, trade routes).
//
// Conventions as the rest of engine/sim: int32 C arithmetic, every game_random draw where the original draws
// (economy-spec section 8), loops in the original's order. Rates are x16 per gather_rate (450) frames.
#pragma once

#include <array>
#include <cstdint>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;
struct Group;
struct Leader;

// TypeIndex values the economy tests (re/types/typeindex.tsv).
namespace etype {
constexpr int32_t kFish = 6, kSilk = 11, kSpice = 12, kAmber = 17, kPeacocks = 25, kDiamonds = 28, kGems = 29,
                  kWhales = 31, kCoffee = 44;
constexpr int32_t kFishermen = 317, kTheCeo = 357, kWellington = 368, kFurTrapper = 400, kFarmPig = 405,
                  kFarmChicken = 406;
constexpr int32_t kForeignTrade = 684, kBuySell = 685, kVirtualReality = 695;
constexpr int32_t kGranary2 = 699, kFishermen1 = 703, kLumbermill2 = 706, kSmelter2 = 709, kUniversity2 = 737,
                  kSupplyWagons1 = 763, kTax1 = 779, kMerchants1 = 783, kRepublic1 = 797, kCapitalism1 = 805;
constexpr int32_t kCivilianSpell = 659;  // SpellTypeData 0xa4c / 4: citizens' "civilian" spell (is_castable)
}  // namespace etype

// UnitMask bits of the gathering code (economy-spec 0).
namespace umask_econ {
constexpr uint32_t kBadFish = 0x20, kHasWood = 0x8000000, kGettingWood = 0x10000000, kHasMetal = 0x20000000,
                   kGettingMetal = 0x40000000, kWithStuff = 0x78000000;
}

// UnitAnim values of the gathering code (economy-spec 4.8).
namespace eanim {
constexpr int32_t kChopWood = 25, kDumpWood = 27, kMineOre = 29, kDumpOre = 31, kSow = 35, kReap = 36, kFarm = 37;
}

// ---------------------------------------------------------------- econ_income.cpp

// Game::init_starting_resources @58a500 (draws S1 / S2 when the setting is random / variable). Writes Game.starting
// (and info.starting_resources for the Random setting).
void init_starting_resources(Game& g);
// Leader::init @6e3930, the economy part: every slot (tribe < 0) resets the LeaderDataEncrypt arrays (caps to
// commerce_cap[0]); a scenario leader (tribe >= 0) then gets escrow_rate 40 (more than two cities), and per available
// good bucket_add(starting amount), the Greek / Persian rules and the Infinite setting.
void leader_init_economy(Game& g, int32_t who, bool with_tribe);

// Leader::gather @6ce280 (once per frame per ACTIVE leader from Leaders::process_all).
void leader_gather(Game& g, int32_t who);
// Leader::calc_gather @6ceee0 (throttled; recomputes `out` only on its cadence; economy-spec 3.1, 3.2).
void leader_calc_gather(Game& g, int32_t who, int32_t* out);
void leader_calc_resource_caps(Game& g, int32_t who);  // Leader::calc_resource_caps @6ce900
void leader_do_gather(Game& g, int32_t who);           // Leader::do_gather @6ce450
int32_t get_gather_handicap(const Game& g, int32_t who);  // LeaderData::get_gather_handicap @6d66a0
// LeaderData::calc_resource_bonuses @6db030 (leader-wide multipliers; also on 8.8 multiplier arrays).
void calc_resource_bonuses(const Game& g, int32_t who, int32_t* out);
// LeaderData::calc_rare(r, out, in_territory) @6e08d0: the income of one collected rare.
void calc_rare(const Game& g, int32_t who, int32_t r, int32_t* out, bool in_territory);
// LeaderData::calc_city_resources @6d5530 via City::calc_gather @737c60 (refreshes the city's enhancer bytes).
void city_calc_gather(Game& g, int32_t who, int32_t c, int32_t* out);
// The tech level getters (has_preq of the upgrade families).
int32_t get_granary(const Game& g, int32_t who);     // @6db340: 1..5
int32_t get_lumbermill(const Game& g, int32_t who);  // @6db3a0: 1..4
int32_t get_smelter(const Game& g, int32_t who);     // @6db3f0: 1..4
int32_t get_university(const Game& g, int32_t who);  // @6db1f0: 1..6
int32_t get_taxation(const Game& g, int32_t who);    // @6d6e20: 0..4
int32_t get_merchants_level(const Game& g, int32_t who);  // @6d6dc0: 0..4
int32_t get_fishermen(const Game& g, int32_t who);   // @6d6e80: 0..3
int32_t building_good(int32_t build_type);           // BuildTypeData::get_good @63bd50 (-1 for non-gatherers)
int32_t best_gather_type(int32_t good);              // GoodTypeData::best_gather_type @670230 (-1 none)

// BuildTypeData::calc_gather @639e40 over the live world, and BuildData::calc_gather @62d360 for a building.
struct GatherCalc {
	std::array<int32_t, 6> out{};
	int32_t max = 0, per = 0;
};
// `list` = the building whose gather_from (MiningList) is used, or null (placement preview).
GatherCalc build_type_calc_gather(Game& g, int32_t type, bool assume, int32_t city, int32_t who, bool apply_bonuses,
                                  int32_t tx, int32_t ty, bool active, int32_t gatherers, const Build* list);
GatherCalc build_calc_gather(Game& g, Build& b, bool assume, bool assume_active, bool apply_bonuses);

// The formula layer of BuildTypeData::calc_gather (the four paths after the map scans), shared with ron-econ-check
// (== node re/tools/gather-ref.mjs --examples). Inputs mirror gather-ref.mjs: raw land counts, rivers, access, slots,
// the city enhancer byte, the owner's tribe and wonders.
struct GatherFormulaIn {
	bool active = true, assume = false;
	int32_t gatherers = 0;
	int32_t enh = 0;          // city enhancer byte of the building's good (0 outside a city)
	int32_t tribe = -1;       // has_tribe_bonus answers by TribeIndex (-1: none)
	bool taj = false, kremlin = false;
	std::array<int32_t, 6> bonus256{256, 256, 256, 256, 256, 256};
	// flat: raw land makes under the footprint (rivers already doubled); radius: raw[] from the W-cell scans, the
	// river tiles of the footprint and 2x access; mine: slots (gather_size, or -1 when claimed elsewhere).
	std::array<int32_t, 6> raw{};
	int32_t rivers = 0, access = 0, iroquois_tiles = 0, slots = 0, univ_level = 1;
	bool blocked = false;
};
GatherCalc gather_flat(const Game& g, int32_t type, const GatherFormulaIn& in);
GatherCalc gather_radius_path(const Game& g, int32_t type, const GatherFormulaIn& in);
GatherCalc gather_mine(const Game& g, const GatherFormulaIn& in);
GatherCalc gather_university(const Game& g, const GatherFormulaIn& in);
int32_t mine_slots(const Game& g, int32_t size, int32_t total, int32_t claimable);  // gather_size's slot formula

// The city tail of LeaderData::calc_city_resources (after the buildings' sum and trade_val, which `out` holds) and
// the leader tail of Leader::calc_gather (after the unit loop, before calc_resource_bonuses), as pure functions.
struct CityTailIn {
	bool forbidden = false, forbidden_is = false, ceo = false, roman = false, german = false, metal_avail = false;
	int32_t num_buildings = 0;
	bool market = false, porcelain = false, temple = false, university = false, library = false;
};
void city_income_tail(const Game& g, const CityTailIn& in, int32_t* out);
struct LeaderTailIn {
	int32_t refineries = 0;
	bool capitalism = false, inca = false, coffee = false, british = false, ctw_missionaries = false, mongol = false;
	std::array<int32_t, 6> conquest{};
	int32_t total_land = 0, territory = 0, taxation = 0, num_nations = 1;
};
void leader_income_tail(const Game& g, const LeaderTailIn& in, int32_t* out);

// The per-frame stockpile update of one good (the arithmetic of do_gather after the rate is known), shared with
// ron-econ-check: returns the whole resources paid.
struct GatherFrameState {
	int32_t bucket = 0, leftover = 0, over_cap = 0, income = 0, escrow = 0;
};
int32_t gather_frame(const Game& g, GatherFrameState& st, int32_t k, int32_t rate, int32_t cap, int32_t handicap,
                     int32_t dutch_base, bool dutch, int32_t escrow_rate, bool ai);

// ---------------------------------------------------------------- econ_gather.cpp

// Gatherer lists (economy-spec 3.5).
void build_add_gatherer(Game& g, Build& b, int32_t o, int32_t who);   // Build::add_gatherer @62f640
void build_remove_gatherer(Game& g, Build& b, int32_t o);             // Build::remove_gatherer @62f8d0
int32_t build_check_gatherers(Game& g, Build& b);                     // Build::check_gatherers @62f710
bool build_all_gathering(Game& g, Build& b);                          // Build::all_gathering @62f570
int32_t build_num_gatherers(const Game& g, const Build& b, bool been_there, bool no_decoys);  // @630450
bool build_is_gathered_by(const Game& g, const Build& b, int32_t o);  // BuildData::is_gathered_by @62f520
bool unit_is_gathering_at(const Game& g, const Unit& u, int32_t o, int32_t who, bool been_there);  // @608880
bool unit_is_gathering(const Game& g, const Unit& u);                 // UnitData::is_gathering @6089b0
void build_update_max_gatherers(Game& g, Build& b);                   // Build::update_max_gatherers @623310
void build_verify_gather_tiles(Game& g, Build& b);                    // Build::verify_gather_tiles @623570

// Unit::add_gather_order(ox, q, explicit) @61a5c0.
void add_gather_order(Game& g, Unit& u, int32_t ox, QueuePos q, bool expl);
// The GATHER side effects of Unit::kill_current_order (orders-spec 4.3): remove from the building's list, flags.
void gather_order_killed(Game& g, Unit& u, const Order& o);
// Unit::do_gather(order) @5ef2a0 (the GATHER handler) and do_non_flat_gather @5f0170.
int32_t do_gather(Game& g, Unit& u, Order& o);
void do_non_flat_gather(Game& g, Unit& u, Order& o);
// Unit::think_peasant(force) @5f5760 and Unit::find_gather_spot(range) @5f5170 (1 = an order was given).
int32_t think_peasant(Game& g, Unit& u, bool force);
int32_t find_gather_spot(Game& g, Unit& u, int32_t range);
// Unit::do_think_order @5e5bf0.
void do_think_order(Game& g, Unit& u);
// The rest of Unit::think @5f6e40 after the combat part (LAB_005f7179): think_peasant, think_caravan, the human rare
// collectors' re-check, AI fishermen (G8) and merchants. TODO(AI): spellcasters, scouts, carriers, armies.
void think_economy(Game& g, Unit& u);
// Group::action_gather(ox, q) @700b90 (GATHER) and Group::action_city_gather(t, q) @701780 (CITY_GATHER).
void group_action_gather(Game& g, Group& gr, int32_t ox, QueuePos q);
void group_action_city_gather(Game& g, Group& gr, int32_t t, QueuePos q);
// GroupData::count @711720 for the kinds the economy uses (CountIndex).
int32_t group_count(const Game& g, const Group& gr, int32_t kind, int32_t p2);

// Farms (docs/farms-spec.md 2-3.3): Farms::add (the 1-in-4 pen draw), remove, inc_time (draws every frame per
// farm), grow, snip, get_farm_type, add_animals.
int32_t farms_add(Game& g, Build& b);
void farms_remove(Game& g, int32_t farm);
void farms_inc_time(Game& g);
void farms_grow(Game& g, int32_t farm, int32_t x, int32_t y);
void farms_snip(Game& g, int32_t farm, int32_t x, int32_t y);
int32_t farms_get_farm_type(const Game& g, int32_t farm);
void farms_add_animals(Game& g, int32_t farm);

// Oil wells (OilWells, economy-spec 7.4): Build::activate / Build::close of OILWELL / OILPLATFORM.
int32_t oil_wells_init(Game& g, Build& b);
// Doober::remove_hold_doobers for a worker's held doober (unit +0x86).
void remove_hold_doober(Game& g, Unit& u);
void oil_wells_close(Game& g, int32_t who, int32_t slot);

// ---------------------------------------------------------------- econ_market.cpp

// LeaderData::calc_market_prices(g, &buy, &sell) @6dc2a0 (wealth per lot of 100).
void calc_market_prices(const Game& g, int32_t who, int32_t good, int32_t* buy, int32_t* sell);
bool can_buy_sell(const Game& g, int32_t who);  // LeaderData::can_buy_sell @6d53e0 && has_market @6d5410
bool do_buy(Game& g, int32_t who, int32_t good);   // Leader::do_buy @6cfbd0
bool do_sell(Game& g, int32_t who, int32_t good);  // Leader::do_sell @6cfc60
void action_buy(Game& g, int32_t who, int32_t good, int32_t flags);   // process_buy @946a20 -> action_buy
void action_sell(Game& g, int32_t who, int32_t good, int32_t flags);  // process_sell @9468a0
// Tribute (economy-spec 5.6): action_offer (TRIBUTE / DEMAND_TRIBUTE), clear_tributes, action_respond (ACCEPT /
// REJECT; resource paths), scale_tribute.
void action_offer(Game& g, int32_t who, int32_t whom, int32_t good, int32_t amount);
void action_clear_tributes(Game& g, int32_t who, int32_t whom);
void action_respond(Game& g, int32_t who, int32_t whom, int32_t mode);
void clear_agree(Game& g, int32_t a, int32_t b);
int32_t scale_tribute(const Game& g, int32_t who, int32_t amount);

// ---------------------------------------------------------------- econ_trade.cpp

// Fishermen, merchants, rares (economy-spec 3.7, 4.7).
bool unit_is_merchant(const Unit& u);       // UnitData::is_merchant @46d370 (61, 62 strict? and 400)
bool unit_is_rare_collector(const Game& g, const Unit& u);  // @46fae0
// Unit::do_gather(out, owned, collected, check_only, ...) @5fce20 -> UnitData::calc_gather @609180: 1 when the unit
// collects something (out filled).
// px / py (-1: the unit's position) and p8 (-1: = check_only) are calc_gather's position override and "count unpacking
// neighbours" flag (good_merchant_spot, spells-spec 5).
int32_t unit_collect(Game& g, Unit& u, int32_t* out, std::array<uint8_t, 8>* owned, int32_t* collected, bool check_only,
                     int32_t px = -1, int32_t py = -1, int32_t p8 = -1);
int32_t think_fish(Game& g, Unit& u);       // Unit::think_fish @5f4c60 (G8)
int32_t think_merchant(Game& g, Unit& u);   // Unit::think_merchant @5f4740
int32_t unpack_merchant(Game& g, Unit& u, int32_t mode);  // Unit::unpack_merchant @6038e0
// Caravans (economy-spec 6).
int32_t caravans_init(Game& g, int32_t who, int32_t o);  // Caravans::init_caravan @73e1f0
void caravans_close(Game& g, int32_t who, int32_t slot); // Caravans::close_caravan @73e350
int32_t trade_value(const Game& g, int32_t ca, int32_t pa, int32_t cb, int32_t pb);  // Caravan::trade_value @73d9d0
void city_compute_trade(Game& g, int32_t who, int32_t c);   // City::compute_trade @739640
void city_new_caravan(Game& g, int32_t who, int32_t c, int32_t pl, int32_t pc);  // City::new_caravan @739750
int32_t city_get_empty_trade_routes(const Game& g, int32_t who, int32_t c, int32_t p, int32_t pc);  // @7395c0
void add_trade_order(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t oxx, int32_t whose, QueuePos q, bool expl);
int32_t do_trade(Game& g, Unit& u, Order& o);     // Unit::do_trade @5ed270
int32_t think_caravan(Game& g, Unit& u, bool force);  // Unit::think_caravan @5f5650
void end_trade_route(Game& g, Unit& u);          // Unit::end_trade_route @5e3bd0
void group_action_trade(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t oxx, int32_t whose, QueuePos q);

// ---------------------------------------------------------------- views (sim/view.hpp)

// The HUD numbers of a leader: gather rates (x16 per gather_rate frames, as the original's HUD divides), caps,
// market prices per good.
struct LeaderEconView {
	std::array<int32_t, 6> rate{}, income{}, cap{}, over_cap{}, buy{}, sell{};
	bool can_trade = false;
};
LeaderEconView leader_econ_view(const Game& g, int32_t who);

}  // namespace ron::sim
