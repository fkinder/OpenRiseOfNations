// The simulation core (docs/frame-spec.md, docs/orders-spec.md, docs/movement-spec.md): the data model shared by every
// sim module — objects (units, buildings) with the original slot allocation, orders, the path stack, guys, leaders —
// and the Game that owns them together with the rules, the world grid, game_random and the frame counter.
//
// Conventions:
// - Coordinates are plain world units (the original stores x/y/z XORed with 0x63637; a save/checksum walk must XOR).
// - Angles are 32-bit binary angles held in uint32_t (the original's int; all arithmetic is mod 2^32).
// - Function names follow the original's methods; the address in each comment is the function the code ports.
// - Modules: objects.cpp (slots, lifecycle, Objects::process_all/inc_time, world links), orders.cpp (order list and
//   Unit::work), movement.cpp (do_move, find_path, move_step, speed), collision.cpp (CollCheck, unit collisions),
//   pathfinder.cpp (PathFinder), guys.cpp (Guy, set_new_location, set_angle, terrain z), groups.cpp (Group, Groups,
//   formations, group moves), game.cpp (load, do_frame), commands.cpp (CommandPackage equivalent), view.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/scenario.hpp"
#include "sim/ai_types.hpp"
#include "sim/anims.hpp"
#include "sim/compass.hpp"
#include "sim/fog.hpp"
#include "sim/groups.hpp"
#include "sim/objects.hpp"
#include "sim/pathfinder.hpp"
#include "sim/random.hpp"
#include "sim/regions.hpp"
#include "sim/rules.hpp"
#include "sim/scenario_data.hpp"
#include "sim/world.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- leaders

// A running general power (heroes-spec 1): ActiveSpell {t, start, frame}, `frame` = the expiry frame.
struct ActiveSpell {
	int32_t t = 0, start = 0, frame = 0;
};
// HeroData / SpecialData (a Caster, 0x30 bytes): the active spells of one hero (Leader::heroes) or one spy / commando
// (Leader::specials). `hero` is the record's own slot (+0x24, Unit.cara of its unit), `o` the unit (+0x26), hero_flags
// bit 0 = in use (+0x28), `who` +0x29.
struct Caster {
	std::vector<ActiveSpell> active_spells;
	int16_t hero = -1, o = -1;
	uint8_t hero_flags = 0;
	int8_t who = -1;
};

// The leader (player) state the movement core reads. LeaderData is ~0x6eec bytes in the original; most of it is
// economy and AI and lives in later modules.
struct Leader {
	uint32_t flags = 0, flags2 = 0;
	int32_t tribe = 0, color = 0, age = 0;
	std::string name;
	// LeaderOptions: default stances per stance type (Unit::init reads them).
	int8_t combat_stance = 0, worker_stance = 0;
	uint32_t options_flags = 0;  // leader_options +0x1c (caster/packer defaults, transport bit 2)
	int32_t units_built = 0;
	// LeaderData +0x74 diplos[8] then +0x94 treaties[8], kept contiguous: the original indexes diplos by any leader
	// slot, so diplos[8] / diplos[9] (slot 8, gaia) read treaties[0] / treaties[1]. diplos: 0 war, 1 peace, 2 allied.
	std::array<int32_t, 8> chat_status{};  // +0x54 (declare_war sets both ways)
	std::array<int32_t, 16> diplo{};
	int32_t diplos(int32_t who) const { return who >= 0 && who < 16 ? diplo[size_t(who)] : 0; }
	int32_t& treaty(int32_t who) { return diplo[size_t(8 + who)]; }
	int32_t treaty(int32_t who) const { return diplo[size_t(8 + who)]; }

	// ---- fog of war (docs/fog-spec.md sections 1.3, 6.6)
	uint8_t ally_mask = 0;  // +0x6929: players whose vision this leader shares (always includes itself)
	// +0x6c18 tech: the known-tech bitmask, indexed by TypeIndex. Filled from the scenario's tech list (Leader::gain_tech
	// and its other effects are not ported).
	std::array<uint8_t, (kNumTypes + 7) / 8> tech{};
	bool has_tech_bit(int32_t t) const { return t >= 0 && t < kNumTypes && (tech[size_t(t >> 3)] >> (t & 7)) & 1; }
	// +0x6da4 rare / +0x6dcc rare_conquest: owned rare resources, bit = TypeIndex - 6 (economy: always empty so far).
	std::array<uint8_t, 8> rare{}, rare_conquest{};
	std::array<uint8_t, 4> conquest_racial_powers{};  // CTW racial power cards (has_tribe_bonus)
	int32_t science = 0;   // LeaderDataEncrypt.epoch[3]: the science epoch (techs 551..557 held)
	int32_t city_num = 0;  // cities owned (has_tribe_bonus: a nomad without a city has no bonuses)
	// +0x125e reg_cities / +0x12de reg_forts: allied cities and forts per land region (was_seen). TODO(territory).
	std::array<int16_t, 64> reg_cities{}, reg_forts{};
	std::array<int16_t, kEndUnits - kUnits> num_units{};  // +0x56fe..: alive units per type (FOUCHE, PTOLEMY, THECEO)
	std::vector<int32_t> oil_patches;  // +0x6e88: goods found on OIL wcoords (World::reveal_fog)
	std::vector<int32_t> new_rares;    // +0x6e6c: rare goods discovered for the AI (Leader::new_rare)
	int32_t explored = 0;              // +0x9d4: Leader::check_explore's explored sample count
	// AI production (script-spec 6.4; Leader::production_ai @6c1960): the state (+0x788; non-zero: plan_strategy runs
	// production_ai instead), the script switch (+0x78c, 0 = retired), the script's `ref int step` (+0x790), the
	// personality's boom_vs_rush (+0x6dd4) and the production script name (+0x6ea4, "" = none). Set by
	// leader_init_production (Leader::init).
	int32_t production_ai = 0, prod_script_on = 0, prod_step = 1;
	std::string prod_script;
	// Combat statistics and stamps (walked LeaderData; combat-spec 5.3, 5.4, 6.4). score_combat +0x44,
	// units_killed +0x80c, units_lost +0x810, buildings_razed +0x818, buildings_lost +0x81c, frame_attacked +0xa40,
	// deaths/kills/hits_current_frame +0xa58/+0xa5a/+0xa5c, retarget_budget +0x9f4 (zeroed every frame).
	int32_t score_combat = 0, units_killed = 0, units_lost = 0, buildings_razed = 0, buildings_lost = 0;
	int32_t frame_attacked = 0, retarget_budget = 0;
	int16_t deaths_current_frame = 0, kills_current_frame = 0, hits_current_frame = 0;
	std::array<int32_t, 8> attack_stamp{}, raid_stamp{};

	// ---- production (docs/production-spec.md section 11; plain values, the original keeps the LeaderDataEncrypt
	// fields XORed). `age` above is LeaderDataEncrypt.ages (the number of age techs gained); `science` mirrors epoch[3].
	// Stockpile (economy-spec 2.1): bucket[k] (+0x00 ^ 0x8221) per good 0..5, escrow[k] (+0x468, AI savings).
	std::array<int32_t, 6> bucket{}, escrow{};
	int32_t epochs = 0, discovered = 0;       // LeaderDataEncrypt +0xe0 / +0xe4
	std::array<int32_t, 4> epoch{};          // +0xe8: library lines (0 military, 1 civic, 2 commerce, 3 science)
	std::array<uint8_t, (kNumTypes + 7) / 8> tech_at_start{}, obs_flags{};  // +0x6c80 / +0x6cf4 BitMask<806>
	std::array<int32_t, 7> age_stamp{};      // +0x8f8
	int32_t gov = -1, gov_hero_frame = -1, misery = 0;  // +0x14, +0xa48, +0x7ec
	// Heroes::lists[who] (20 preallocated records, grown by one; heroes-spec 2) and Specials::lists[who]: the records of
	// this leader, num_heroes +0x420 / num_specials +0x43c = slots in use below the high mark (sim/heroes.hpp).
	std::vector<Caster> heroes, specials;
	int32_t num_heroes = 0, num_specials = 0;
	float plunder_scale = -1.0f;  // +0x914 (scenario set_plunder_scale; < 0 = none)
	std::array<uint16_t, kNumTypes> num_queued{};           // +0x5a22: queued items per type, all buildings
	std::array<int16_t, kEndBuilds - kBuilds> num_buildings{}, high_buildings{};  // +0x555e / +0x5660
	std::array<std::array<int16_t, kEndBuilds - kBuilds>, 64> reg_buildings{};      // +0x14de [region][type - 414]
	std::array<int16_t, kEndUnits - kUnits> last_unit_finished{};  // +0x6274
	std::array<int32_t, kEndBuilds - kBuilds> last_building_finished{};  // +0x6070
	// Per-class unit counters (track_unit_type / track_queued, production-spec 6.10): barracks, stable, factory,
	// combat (barracks + stable), dock, air; and peasants, scholars, scouts, scholar_militia.
	int32_t barracks_units = 0, stable_units = 0, factory_units = 0, combat_units = 0, dock_units = 0, air_units = 0;
	int32_t barracks_queued = 0, stable_queued = 0, factory_queued = 0, combat_queued = 0, dock_queued = 0,
	        air_queued = 0;
	int32_t peasants = 0, scholars = 0, scouts = 0, scholar_militia = 0;
	// Captains garrisoned in a building (Object::insert_inside / remove_from_inside, garrison-spec 1): citizens
	// +0xa28, stable units +0xa2c, specials +0xa30, barracks units +0xa34.
	int32_t peasants_inside = 0, stable_inside = 0, special_inside = 0, barracks_inside = 0;
	uint8_t ages_queued = 0, epochs_queued = 0;  // +0x67f4 / +0x67f5
	int32_t tech_frame = 0;                        // +0x7c4
	std::array<int32_t, 4> tech_cat_frame{};       // +0x7c8
	// Population (economy-spec 7.1): pop (city statistic) +0x95c, pop_cap +0x7e4, pop_issues +0x7e8, control (pop in
	// use) +0x940, active (unit count) +0x93c, caras (caravans) +0x980.
	int32_t pop = 0, pop_cap = 0, pop_issues = 0, control = 0, active = 0, caras = 0;
	// Cities, forts, wonders (production-spec 5.0.2).
	int32_t city_mine = 0, city_mark = 0, city_name = 0, fort_mark = 0, wonder_mark = 0, lost_city_stamp = 0;
	std::array<int16_t, 64> reg_pop{};
	// Territory (production-spec 5.10, sim/territory.hpp): +0x145e reg_terr (ushort[64], wcoords owned per land region),
	// +0x4d4 reg_known_rares (int[64], zeroed with reg_terr at a region's fresh pass), +0x9d8 territory (sum of reg_terr
	// over non-empty land regions) and +0x9dc territory_high (its high-water mark).
	std::array<uint16_t, 64> reg_terr{};
	std::array<int32_t, 64> reg_known_rares{};
	int32_t territory = 0, territory_high = 0;
	std::array<uint8_t, 38> num_bonus_cards{};  // +0x68f6: CTW bonus cards held per card type ([32] missionaries)
	int32_t buildings_built = 0, cities_built = 0, wonders_built = 0, wonders_held = 0, senates_built = 0,
	        forts_built = 0, defense = 0;
	std::array<int32_t, 6> gather_slots{}, gather_slots_high{};  // +0x8a4 / +0x8d4
	int32_t attrition_stamp3 = 0, disable_building_attrition = 0, repair_stamp = 0;
	// Attrition (combat-spec 7.4, economy-spec 7.3): att +0x7f0 (Leader::calc_attrition, the attrition this leader's
	// territory gives), anti_att +0x7f4 (Leader::calc_anti_attrition, 256.0f = no resistance), give_att_disabled
	// +0x7f8 (disable_give_attrition), take_att_disabled +0x7fc (disable_take_attrition), neutral_attrition +0x800
	// (set_neutral_attrition; the attrition period in neutral territory), the warning stamps +0x1f4
	// (process_attrition) / +0x1f8 (suffer_attrition), and supply_mark +0x434 (Supplies, highest live slot + 1).
	int32_t att = 0;
	float anti_att = 256.0f;
	int32_t give_att_disabled = 0, take_att_disabled = 0, neutral_attrition = 0;
	int32_t attrition_warn_stamp = 0, attrition_hit_stamp = 0;
	int32_t supply_mark = 0;
	int32_t nukes_used = 0, nukes_launched = 0, missiles_used = 0;
	int32_t handicap = 0, difficulty = 0;  // +0x4c / +0x50 (scenario file values; difficulty = multi_diff)
	int32_t wonderwin_timer = 0;           // +0x44c: wonder-victory clock running (flag; GameDaemon::process_victory)
	// Victory / elimination clocks (docs/capture-spec.md 7): each stamp is "minus the elapsed time" while its flag is
	// clear (stepped toward 0 every timer_refresh_ratio frames by Leaders::process_all) and "start frame minus the
	// elapsed time" while it runs. lost_capital_stamp +0x414 / lost_capital_timer +0x418 (flag) /
	// lost_capital_modifier +0x41c (x256), popwin_stamp +0x440 (flag popwin_timer +0x444), wonderwin_stamp +0x448.
	int32_t lost_capital_stamp = 0, lost_capital_timer = 0, lost_capital_modifier = 0x100;
	int32_t popwin_stamp = 0, wonderwin_stamp = 0;
	// Capture / defeat bookkeeping: cities_captured +0x824, cities_lost +0x828, defeated_by +0x10 (-1 until
	// Leader::defeat_by names a conqueror), defeat_stamp +0x6924, defeat_type +0x7dc, victory_type +0x7d8.
	int32_t cities_captured = 0, cities_lost = 0, defeated_by = -1, defeat_stamp = 0, defeat_type = 0, victory_type = 0;

	// ---- economy (docs/economy-spec.md 0, 2; sim/economy.hpp). The LeaderDataEncrypt fields as plain values (the
	// original keeps each XORed with its key in RAM; saves and checksums hold plain values).
	std::array<int32_t, 6> leftover{};      // +0x18: sub-unit remainder of the per-frame income (x7200)
	std::array<int32_t, 7> resource_cap{};  // +0x30: commerce cap on the income rate, x16 (slot 6 unused)
	std::array<int32_t, 6> over_cap{};      // +0x4c: 0 under cap, 1 capped, 2 capped at the 999 ceiling
	std::array<int32_t, 6> resources{};     // +0x64: the gather rate (x16 per gather_rate frames) from calc_gather
	std::array<int32_t, 6> support{};       // +0x7c: always 0 (Leader::gather)
	std::array<int32_t, 6> income{};        // +0x94: the capped rate the HUD shows
	std::array<int32_t, 6> rate{};          // +0xac: never written by the economy (economy-spec 13.1)
	std::array<int32_t, 6> bonus{};         // +0xc4: reset by every calc_gather recompute
	std::array<int32_t, 6> escrow_rate{};   // +0x480: AI savings rate (percent of income)
	std::array<int32_t, 6> tributes{};      // +0x498: resources held while a tribute deal is pending
	std::array<int32_t, 6> base_rate{};     // +0x4b0: added to the rate (only zeroed by Leader::init)
	std::array<int32_t, 6> collected{};     // +0x874: total gathered (statistics)
	std::array<int32_t, 7> bonus_cap{};     // +0x918: commerce cap bonus per good (techs / cards)
	std::array<int32_t, 44> rares_collected{};  // +0x6d8
	int32_t known_rares = 0;                // +0x6d4: sum of reg_known_rares
	int32_t gather_stamp = 0;               // +0x7ac: frame of the last calc_gather recompute
	std::array<uint8_t, 8> rare_owned{};    // +0x6dac BitMask<44>: rares collected by merchants / Porcelain
	std::array<uint8_t, 6> num_ctw_rate_bonuses{};  // CTW production cards per good
	// LeaderOptions (LEADER_OPTIONS command; LeaderOptions::init defaults): auto-gather delay, caravan delay, buildings.
	int32_t opt_peasants = 0, opt_peasants_wait = 2, opt_buildings = 0;
	// Diplomacy deals (economy-spec 5.6): Diplomacy dip[8] (agree, any_offer, offers[6], dows[6]), tribute statistics.
	struct Deal {
		int32_t agree = 0, any_offer = 0, treaty = -1;  // Diplomacy::clear_all state
		std::array<int32_t, 6> offers{}, dows{};
		std::array<int32_t, 8> attacks{};
	};
	std::array<Deal, 8> dip{};
	int32_t tribute_sent = 0, tribute_received = 0;
	std::array<int32_t, 8> tribute_stamp{}, gift_stamp{};
	// Records of this leader (OilWells / Caravans lists, economy-spec 6.1, 7.4): slot -> object o (-1 free).
	std::vector<int16_t> oil_wells;  // OilWell.o per slot; the mark is the vector size
	int32_t caravan_mark = 0;

	// ---- the computer opponent (docs/ai-spec.md; sim/ai.hpp). Census fields shared with production / economy
	// (active, control, pop, peasants, scholars, scouts, caras, defense, reg_cities, gather_slots ...) are above.
	Personality pers;                      // +0x6dd4
	std::array<Site, 10> sites{};          // +0x6e34
	std::array<MakeObject, 11> make_list{};  // +0x6ec8
	std::vector<int32_t> mil_trainers;     // +0x6e50: active military trainers (Wall::increment_stats order)
	uint32_t site_mark = 0;                // +0x7e0
	int32_t village_num = 0, village_mine = 0;  // +0x3fc / +0x404 (only Leader::init writes them: always 0)
	int32_t score = 0;                     // +0x18 (Leader::compute_score)
	// The score parts (Leader::compute_*_score @6bc190..6bc5e0): explored +0x1c (always 0), territory +0x20, units
	// +0x24 (attack units also +0x28), buildings +0x2c, economy +0x30, pop +0x34 (always 0), unit upgrades +0x38,
	// research +0x3c, wonders +0x40 (score_combat +0x44 is the last term).
	int32_t score_explored = 0, score_territory = 0, score_units = 0, score_attack_units = 0, score_buildings = 0;
	int32_t score_economy = 0, score_pop = 0, score_upgrades = 0, score_research = 0, score_wonders = 0;
	std::array<int32_t, 6> econ{};         // +0x450: need bits per good (production_ai_setup)
	int32_t worst_good = 0, best_good = 0, shortages = 0;  // +0x4c8..
	std::array<int32_t, 64> reg_unpack_merch{};  // +0x5d4
	int32_t wonder_mod = 0, ground_mod = 0x100, air_mod = 0x100, sea_mod = 0x100, infra_mod = 0x100,
	        defense_mod = 0x100;          // +0x794..+0x7a8 (Leader::init: 0x100, wonder_mod 0)
	int32_t army_high = 0, peasant_high = 0, scholar_high = 0, caravan_high = 0, merchant_high = 0, city_high = 0,
	        village_high = 0, population_high = 0, city_pop_high = 0, resources_controlled = 0;  // +0x82c..
	std::array<int32_t, 6> filled_gather_slots{};  // +0x8bc
	int32_t combat = 0, non_siege = 0, sea_combat = 0, siege = 0, attack = 0, naval = 0, air = 0, missile = 0,
	        transports = 0, fishermen = 0, idle_fishermen = 0, merchants = 0, fighters = 0, bombers = 0, cruise = 0,
	        nuke = 0;                      // +0x944..+0x998
	int32_t wars = 0, allies = 0, active_wars = 0, active_wars_with = 0, free_peasants = 0, xport_peasants = 0,
	        gatherers = 0, home_reg = 0, attacked = 0, full_cities = 0;  // +0x9ac..+0x9d0
	int32_t effective_pop = 0, other_team_terr = 0, min_other_team_terr = 0, my_team_terr = 0;  // +0x9e0..
	int32_t attacked_by = 0, nukes_in_flight = 0, popwin_timer = 0;  // +0xa44, +0xa3c, +0x444
	std::array<uint16_t, 64> strategy{};   // +0xa68
	std::array<uint16_t, 127> reg_active{}, reg_combat{}, reg_attack{};  // +0xae8 / +0xbe6 / +0xd64
	std::array<uint16_t, 64> reg_defense{}, reg_peasants{}, reg_free_peasants{}, reg_xport_peasants{},
	    reg_gatherers{}, reg_gather_slots{}, reg_land{};
	std::array<uint16_t, 63> reg_naval{}, reg_transports{};
	std::array<uint8_t, 64> reg_attacked{}, reg_wars{}, reg_neutrals{}, reg_allies{};  // +0x67f6..
	// Diplomacy AI (ai-spec 10): agendas +0xb4, good_deeds +0xd4, hire_stamp +0x1b4, hire_who +0x1d4, blacken +0x20c,
	// aggression / strong / weak +0x210 / +0x230 / +0x250, dow +0x270, invaders +0x290, broke_alliance +0x2b0,
	// made_peace +0x2d0, got_diplo_message +0x2f0, counteroffer +0x314, tribute_demanded +0x334.
	std::array<int32_t, 8> agendas{}, good_deeds{}, hire_stamp{}, hire_who{}, aggression{}, strong{}, weak{}, dow{},
	    invaders{}, broke_alliance{}, made_peace{}, counteroffer{}, tribute_demanded{};
	int32_t blacken = 0, got_diplo_message = 0;
	int32_t goody_box_resources = 0;  // +0x86c (statistics)

	bool has_tech_raw(int32_t t) const { return has_tech_bit(t); }
	void set_tech_bit(int32_t t, bool on) {
		if (t < 0 || t >= kNumTypes) return;
		const uint8_t b = uint8_t(1u << (t & 7));
		tech[size_t(t >> 3)] = on ? uint8_t(tech[size_t(t >> 3)] | b) : uint8_t(tech[size_t(t >> 3)] & ~b);
	}
	static bool bit_of(const std::array<uint8_t, (kNumTypes + 7) / 8>& m, int32_t t) {
		return t >= 0 && t < kNumTypes && (m[size_t(t >> 3)] >> (t & 7)) & 1;
	}
	static void set_bit(std::array<uint8_t, (kNumTypes + 7) / 8>& m, int32_t t, bool on) {
		if (t < 0 || t >= kNumTypes) return;
		const uint8_t b = uint8_t(1u << (t & 7));
		m[size_t(t >> 3)] = on ? uint8_t(m[size_t(t >> 3)] | b) : uint8_t(m[size_t(t >> 3)] & ~b);
	}
};

// ---------------------------------------------------------------- cities, forts, wonders (production-spec 5.0)

// City (CityData, 0xb8). Records live in Game::cities[who], index = city number (slot allocation 5.0.3).
namespace cityflag {
constexpr uint16_t kValid = 0x1, kUnderAttack = 0x2, kAttacking = 0x4, kEverAttacked = 0x8, kCapital = 0x10,
                   kAlarm = 0x40, kHasTemple = 0x80, kGotCaptured = 0x100, kHasGranary = 0x200, kHasLumber = 0x400,
                   kHasMarket = 0x800, kVeryNearBorder = 0x1000, kCrowded = 0x2000, kOriginalCapital = 0x4000,
                   kWasOrigCapital = 0x8000;
}
struct City {
	uint16_t city_flags = 0;
	int16_t city = 0, o = -1, reg = 0;
	int32_t x = 0, y = 0;
	int32_t attack_stamp = 0, raid_stamp = 0, reduce_stamp = 0, capture_stamp = 0, assimilation_timer = 0;
	int32_t capture_strength = 0;
	std::array<int32_t, 8> traded_with{};
	int16_t scouted = 0, in_port = 0, peasant_dist = 0, trade_val = 0, conquest_node = -1;
	uint8_t granary = 0, lumber_mill = 0, smelter = 0, refinery = 0;
	uint8_t free = 0, busy = 0, gatherers = 0, pop = 0;
	int8_t who = 0, race = 0, founder = -1;
	uint8_t plundered = 0, ocean = 0, land = 9, filled = 1, bordering = 0, ocean_filled = 0, dock_tile = 0;
	uint8_t was_capital_flags = 0;
	std::array<uint8_t, 3> space{};  // +0x69: free building squares of size 2..4 (AI census)
	std::array<uint8_t, 6> ter{};
	std::vector<std::array<int16_t, 2>> vans;  // CaravanLink {cara, who} (Array, capacity 10)
	std::string name;           // file-only
	bool valid() const { return (city_flags & cityflag::kValid) != 0; }
};
struct Fort {
	int16_t fort = 0, o = -1, reg = 0;
	uint8_t fort_flags = 0;  // 1 VALID
	int8_t who = 0;
};
struct Wonder {
	int16_t wonder = 0, o = -1;
	int32_t stamp = 0, timer = 0;
	uint8_t wonder_flags = 0;  // 1 VALID
	int8_t who = 0;
};
// A placed mountain range as MountainsData keeps it (mountain_locs / mountain_types / ranges): the stamp's wcoord,
// the range's solid wcoords (offsets from it; find_nearest, gather_size) and mountain tcoords (offsets from 4 * the
// wcoord; find_gather_tcoords), and the range's tcoord count (mount_tx, gather_size's size class).
struct PlacedRange {
	int32_t wx = 0, wy = 0, range = 0;
	std::vector<std::array<int32_t, 2>> solid, tiles;
	int32_t tcoord_count = 0;
};

// UnbuiltCities / UnbuiltForts / UnbuiltWonders entries {o, who} (append order = creation order).
struct Unbuilt {
	int16_t o = -1;
	int8_t who = 0;
};

// The GameInfo (lobby) settings the simulation reads so far: teams for the starting diplomacy (Leader::init).
struct GameInfo {
	uint8_t team_style = 0;  // Game::teams_locked: locked unless 0, 8 or 11
	// rules.xml revealmaps: 0 No Ally LOS, 1 Normal, 2 Explored, 3 All Visible (3 also starts every treaty set).
	uint8_t reveal_map = 1;
	uint8_t rush_rules = 0;  // TODO(frame-spec): peace until this age (Leader::init); not applied yet
	uint8_t starting_town = 1;       // 0: nomad start (citizens' LOS bonus, has_tribe_bonus)
	uint8_t starting_resources = 0;  // the frame-0 ally share of explored areas needs this or reveal_map != 0
	uint8_t game_rules = 0;          // 8: team-0 starting cities share their explored area with everyone
	uint32_t flags = 0;              // GameInfo.flags: 4 = no tribe bonuses
	// GameInfo difficulty (LeaderData::get_diff in single player; 0 = easiest). TODO: from the lobby / scenario
	// settings; 2 (moderate) until then.
	uint8_t difficulty = 2;
	std::array<int8_t, 8> team{-1, -1, -1, -1, -1, -1, -1, -1};  // GameInfo.player[get_player(who)].team, -1 none
	// Production settings (production-spec 2, 7): the tech span TypeData::get_preq reads live, the research price /
	// speed setting (rules "techcost", 3 = standard), victory / elimination modes, pop limit index, players and
	// nations for the research speed-ups. From the scenario header when loading a scenario (Game::load).
	int32_t starting_technology = 0, starting_technology2 = 0, ending_technology = 7;
	uint8_t tech_cost = 3, victory = 0, elimination = 0, pop_limit = 5, starting_resources2 = 0;
	int32_t num_players = 2, num_nations = 2;
	// Game.num_sides (Game::init_teams @58ae70: teams with members plus team-less players; only get_armageddon reads
	// it here). TODO(teams): computed from the lobby teams; 2 until init_teams is ported.
	int32_t num_sides = 2;
	// AI settings (ai-spec 0, 14): the map style (scenario header 0xd8), the lobby player flags (GameInfo.player[p].flags:
	// 0x1000 rusher, 0x2000 boomer; none in scenarios), the wonder-victory setting index.
	uint8_t map_style = 0, wonderwin = 0;
	// Victory settings indexes into the rules lists (scenario header 0x100..0x110): score_goal (scores), time_limit
	// (time_limits; 8+ = custom), popwin (popwins), econwin (econwins); chairs (chairs, lobby only).
	uint8_t score_goal = 0, time_limit = 0, popwin = 0, econwin = 0, chairs = 0;
	std::array<uint32_t, 8> player_flags{};
	GameSetup setup() const {
		GameSetup s;
		s.starting_technology = starting_technology;
		s.starting_technology2 = starting_technology2;
		s.ending_technology = ending_technology;
		s.game_rules = game_rules;
		s.flag8 = (flags & 8) != 0;
		return s;
	}
};

constexpr int kNumLeaders = 10;
constexpr int kGaiaWho = 9;  // leader slot of gaia (animals, birds, herds)
constexpr int32_t kUnitLimit = 2000, kBuildBase = 2000, kBuildLimit = 3000;

// Objects (objects.obj): per-leader slot lists. Units use o in [0, unit_mark), buildings [2000, build_mark).
struct ObjectList {
	std::vector<std::unique_ptr<Unit>> units;    // index o
	std::vector<std::unique_ptr<Build>> builds;  // index o - 2000
	int32_t unit_mark = 0, build_mark = kBuildBase;
	uint16_t obj_ctr = 0;  // uid counter (u16, wraps)
};

// GameDaemon state used by movement.
struct GameDaemon {
	int32_t busy = 0;  // set to 4 by every fog refresh (update_all_seen), decremented every frame
	std::array<int32_t, 8> repaths{};  // per-player path-request throttle (halved every frame)
};

// The market (Game +0x564..; docs/economy-spec.md 5.1): per good (Food, Timber, Wealth, Knowledge, Metal, Oil) the
// supply/demand level and the drifting price offset. Setup::build_game's values.
struct Market {
	int32_t tick = 0;
	std::array<int32_t, 6> level{50, 50, 50, 50, 50, 50};  // market[]: the base price
	std::array<int32_t, 6> flux{15, 15, 15, 15, 15, 15};   // market_flux[]
	std::array<int32_t, 6> next_flux{15, 15, 15, 15, 15, 15};
	std::array<int32_t, 6> delta_flux{};
	std::array<int32_t, 6> flux_length{1, 1, 1, 1, 1, 1};
};

// A wheat farm or animal pen (FarmStruct, farms.obj; docs/farms-spec.md section 2, 3.3): the simulation part (growth
// cells, type). Farms::add gives a farm building its slot (Build.fort union = farm index).
struct FarmRec {
	int32_t who = -1, o = -1;
	bool valid = false;
	uint8_t farm_type = 0;  // +0xbd: 0 wheat, 1 animal pen (bit 4: ambience sound; presentation)
	std::array<std::array<float, 4>, 4> percent{};  // [x][y] growth 0..1
	std::array<std::array<uint8_t, 4>, 4> status{};  // [x][y] 0 bare, 1 growing, 2 ripe, 3 harvested
};

// A land caravan's route record (Caravan, 0x50; economy-spec 6.1). The road (Stack<PathData>) and the sliced road
// search are TODO (caravans walk without roads; economy-spec 6.4).
struct CaravanRec {
	int16_t city2 = -1, whom = -1, city3 = -1, whose = -1, cara = -1, o = -1;
	uint8_t caravan_flags = 0;  // 1 in use, 2 route assigned, 4 a round trip completed
	int8_t who = -1;
};

// Terrain heights for z (TerrainOut height grid, (4xs+1) x (4ys+1) floats).
struct Heights {
	int32_t vxs = 0, vys = 0;
	std::vector<float> h;
	float at(int32_t tx, int32_t ty) const { return h[size_t(ty) * size_t(vxs) + size_t(tx)]; }
};

// A notable simulation event for tools (ron-sim, ronmap): city captures, defeats, victories. Written by the capture /
// victory code (sim/capture.hpp) and never read by the simulation (not walked, not in any digest). kind: 1 city
// captured (who = new owner, other = old owner, o / o2 = old / new city object, value = new city index, goods = plunder
// paid to the captor, goods2 = to the victim, n_* = buildings / units handled), 2 leader defeated (who, other = the
// conqueror or -1, value = defeat type), 3 victory (who, value = victory type), 4 game over, 5 capital lost (who, other =
// holder), 6 capital recaptured (who), 7 building captured (who = new owner, other = old owner, o / o2).
struct GameEvent {
	int32_t frame = 0, kind = 0, who = -1, other = -1, o = -1, o2 = -1, value = 0;
	std::array<int32_t, 6> goods{}, goods2{};
	int32_t n_swapped = 0, n_closed = 0, n_disbanded = 0, n_units = 0;
};

// ---------------------------------------------------------------- the game

class ScriptHost;  // sim/script_host.hpp
struct OriginalShadow;  // sim/original_save.hpp

class Game {
public:
	Game();
	~Game();
	Game(const Game&) = delete;
	Game& operator=(const Game&) = delete;

	// ---- static data
	const Rules* rules = nullptr;
	// The rules tables the scenario builtins rewrite (set_type_build_time, disable_type, set_object_type_max_health,
	// ...: the original writes the global types.list, which the next game reloads). The first write copies *rules into
	// own_rules, repoints `rules` and every object's ptype at the copy, so a shared Rules stays pristine.
	std::unique_ptr<Rules> own_rules;
	const Rules* base_rules = nullptr;  // the tables as loaded (enable_type restores from them: GameAccess::typesbak)
	Rules& mut_rules();
	GameInfo info;  // set before load()
	// Art data for the guy animation clock (sim/anims.hpp; sim/anim_assets.hpp builds it from the install). Null:
	// no art (every animation absent, 3 frames long, looping approximated by category); the original cannot run
	// without it ("No Animation Packet"), so bit-exact runs need a source.
	const AnimSource* anims = nullptr;

	// ---- simulation state
	World world;
	Regions regions;
	Heights heights;
	Random rng;  // game_random
	int32_t frame = 0, tick = 0;
	uint32_t seed = 0;  // GameInfo.seed (GameAccessConst::gamec->info.seed; bird graphics pieces use it)
	std::array<Leader, kNumLeaders> leaders{};
	std::array<ObjectList, kNumLeaders> objects{};
	GameDaemon daemon;
	Market market;
	Groups groups;
	std::array<FormData, kNumForms> forms{};  // the global `forms` (Forms), scratch of Group::compute_form
	PathFinder pathfinder;
	int32_t total_units = 0;  // GameData.total_units: units of leaders 0..8 (Unit::init ++, Unit::close --)
	// Function statics of Group::action_move_near: the shared group path `grouppath` (its length is reset on every
	// call) and `cols` (a SimpleArray<int> resized to the group size and never written; see groups.cpp).
	std::vector<PathData> grouppath;
	std::vector<int32_t> group_cols;
	// Combat (combat-spec 4.7, 6.3): the Ammo pool (200 slots at start, grows), its running graph_index counter
	// (Objects +0x1f8), the 100 corpse slots, and per unit type the projectile piece decided by the art at load
	// (UnitTypeData.fire_proj: -1 = fires from animation release events (the original's 0), >= 0 = ammo piece
	// fired directly by fight; see combat.cpp init_combat_data).
	std::vector<Ammo> ammo;
	int32_t ammo_counter = 0;
	std::array<DeathObj, 100> deaths{};
	std::vector<int32_t> fire_proj;
	// The temporary ptype.max_range swap of do_attack / cavarch_fight for MELEE_AND_RANGED types (the original
	// writes the shared type; here every combat range read goes through raw_max_range(), which applies it).
	int32_t range_swap_type = -1, range_swap_value = 0;
	int32_t local_play = 0;  // console.play / the local leader (seat-local; never feeds the simulation)
	bool editor = false;     // semaphore byte1 & 8
	bool scripted_rules = false;  // semaphore byte1 & 0x10 (scenario scripts: reveal points, scripted ally masks)
	bool ctw = false;             // semaphore byte2 & 2 (Conquer the World)
	bool multiplayer = false;     // semaphore byte0 & 4
	bool field_battle = false;    // semaphore byte2 & 0x10 (field_battle_enable)
	int32_t ai_speed = 1;         // GameAccess::ai_speed (strategy cadence 200 / ai_speed)
	// The BigHugeScript run time of this game (sim/script_host.hpp): scenario, general powers and AI production scripts.
	// Null: no scripts (Game::do_frame skips the script steps); created by script_host_load.
	std::unique_ptr<ScriptHost> scripts;

	// ---- goods, items and fog state outside the grids (sim/fog.hpp; docs/fog-spec.md)
	std::vector<GoodObj> goods, items;  // global arrays in creation order (Objects::init_good / init_item)
	int32_t good_mark = 0;              // Objects +0x154: highest good index + 1
	uint32_t everyone_mask = 0;         // Game.everyone_mask: bit of every scenario leader
	struct RevealPoint {
		int32_t tx = 0, ty = 0, radius = 0;  // tcoords
	};
	std::array<uint8_t, 8> scen_ally_mask{};                  // ScenarioData::ally_mask (add_visibility)
	// ScenarioData::diplomacy_setting (Leader::diplomacy: 1 no AI diplomacy, 2 accept, 3 refuse). TODO(script): the
	// scenario natives that set it are not ported (always 0).
	std::array<uint8_t, 8> scen_diplomacy_setting{};
	std::array<std::vector<RevealPoint>, 8> reveal_points{};  // ScenarioData::reveal_points (add_reveal_point)
	// ScenarioData::attrition_free_points (set_attrition_free_point): no attrition within `radius` tiles.
	std::array<std::vector<RevealPoint>, 8> attrition_free_points{};
	// supplies.lists[who] (Supplies, economy-spec 7.2): one record per is_supply unit, reused by slot.
	struct Supply {
		int16_t o = -1;       // +0x0: this record's slot
		int16_t unit = -1;    // +0x2: the supply unit's object slot
		uint8_t flags = 0;    // +0x4: bit 0 live
		int8_t who = -1;      // +0x5
	};
	std::array<std::vector<Supply>, 8> supplies{};

	// ---- production state (docs/production-spec.md section 11)
	std::array<std::vector<City>, 8> cities{};    // cities.lists[who]
	std::array<std::vector<Fort>, 8> forts{};     // forts.lists[who]
	std::array<std::vector<Wonder>, 8> wonder_records{};  // wonders.lists[who]
	std::array<std::vector<Unbuilt>, 8> unbuilt_cities{}, unbuilt_forts{}, unbuilt_wonders{};
	std::array<int32_t, 17> wonders{};  // Game +0x618: bit 0 = wonder type 526 + i has been completed by someone
	int32_t terra_cotta_total = 0, world_pop = 0, world_cities = 0;
	std::array<int32_t, 6> starting{};  // Game +0x600: starting resources per good (Game::init_starting_resources)
	// ---- economy (sim/economy.hpp)
	std::vector<FarmRec> farms;                     // Farms (FarmStruct list; slot = Build.fort of a FARM)
	std::vector<uint8_t> hold_doobers;              // Doober hold slots in use (Doober::add_hold_doober indices)
	int32_t world_total_land = 0;                   // World +0x78 land_size (World::analyze_map: land regions' sizes)
	// ScenarioData::buildings_gather: scenario buildings and cities produce (buildings_gather_disable / _enable;
	// ScenarioFuncSet::init sets it).
	bool buildings_gather = true;
	std::array<std::vector<CaravanRec>, 8> caravans{};  // caravans.lists[who]
	int32_t queue_fail = 0;             // BuildData::queue_fail (static, walked)
	int32_t check_borders_budget = 0;   // GameDaemon.borders (per-frame territory budget counter)
	int32_t armageddon = 0;             // Game.armageddon (nukes left before Armageddon; ARMAGEDDON_BONUS -2)
	// Game over (semaphore byte0 & 0x40, set by Game::check_victory): TurnControl stops stepping the simulation, so
	// Game::do_frame returns at once. victory_checked = semaphore byte2 & 0x40. musical_chairs = Game.musical_chairs
	// (frame of the last defeat / chairs elimination). docs/capture-spec.md 7.
	bool game_over = false, victory_checked = false;
	int32_t musical_chairs = 0;
	std::vector<GameEvent> events;  // tool log (see GameEvent); not simulation state
	std::vector<PlacedRange> mountains; // MountainsData (placed ranges, scenario order)
	// ---- the computer opponent (sim/ai.hpp)
	std::array<std::array<Army, 16>, 8> armies{};  // Armies.lists[who]: 16 slots per player (Armies::init)
	int32_t armies_find_dist = 0;                  // ArmiesData::find_dist @cb4bac (walked global)
	// ObjectsData +0x1fc find_dist / +0x200 find_who: outputs of the ObjectsData::find_* searches that the AI reads
	// back (written by the sim/ai.hpp finders; the movement searches do not model them).
	mutable int32_t find_dist = 99999999, find_who = -1;
	bool ai_off = false;  // *GameAccess::ai_off (cheat toggle)
	// True while Game::load creates the scenario's objects: the world loader has already applied the buildings'
	// grid effects (start_me, mask_me, fog marks), so Build::init / Build::start skip them.
	bool scenario_loading = false;
	// Game::is_scenario @43ec70 (semaphore byte1 & 0x10 or byte2 & 2, i.e. scripted rules or Conquer the World; set by
	// Game::load). Free units and resource bonuses of Build::activate apply only outside it, or with ScenarioData's
	// building_unit_bonus / building_resource_bonus (ScenarioFuncSet::init / close set both to 1; the
	// building_*_bonus_disable builtins clear them).
	bool scenario_mode = false;
	bool building_unit_bonus = true, building_resource_bonus = true;
	// The rest of ScenarioData's script state (sim/scenario_data.hpp).
	ScenarioData scen;

	// ---- loading
	// Builds the world grid from the scenario (docs/world-spec.md section 3), the leaders, and every unit through
	// Objects::init_unit with the file's slot (game_random := seed before the objects, as ScenarioEditor::init_terrain).
	// Throws std::runtime_error on errors. `seed` = GameInfo.seed (also used for the terrain-load draws, see
	// docs/sim-implementation.md).
	void load(const GameFiles& files, const Scenario& s, const Rules& rules, uint32_t seed);

	// ---- the frame (docs/frame-spec.md section 3). Commands for this frame must have been executed already.
	void do_frame();

	// ---- objects (objects.cpp)
	Unit* unit(int32_t who, int32_t o);              // null if the slot was never allocated
	const Unit* unit(int32_t who, int32_t o) const;
	Build* build(int32_t who, int32_t o);            // o in [2000, 3000)
	const Build* build(int32_t who, int32_t o) const;
	Object* object(int32_t who, int32_t o);          // unit or building by slot
	const Object* object(int32_t who, int32_t o) const;
	Unit* alive_unit(int32_t who, int32_t o);        // null unless allocated and VALID
	const Type& type(int32_t t) const { return rules->types[size_t(t)]; }
	// rnd(n) = n <= 1 ? 0 (no draw) : game_random.get(0, 0xffff) % n  (GameAccess::rnd)
	int32_t rnd(int32_t n);
	// game_random.get(0, 0xffff) % n with a draw even for n == 1 (the movement call sites draw unconditionally).
	int32_t rnd_draw(int32_t n) { return rng.get(0, 0xffff) % n; }

	// The original bytes of a game loaded from an original-game save (sim/original_save.hpp, docs/original-save.md): the
	// decoded save with the members the engine does not model. Null for every other game; the native save ignores it.
	std::shared_ptr<OriginalShadow> original;
};

// ---------------------------------------------------------------- objects.cpp

// Objects::find_free @65ad60.
int32_t find_free(Game& g, int32_t who, int32_t base, int32_t limit, int32_t forced);
// Objects::init_unit @65e0c0: creates type.uber_size units (1 when forced) as an o_up/o_down chain. Returns the
// captain's o, or -1.
int32_t init_unit(Game& g, int32_t who, int32_t type, int32_t x, int32_t y, int32_t forced_o = -1, int32_t o_up = -1,
                  int32_t o_down = -1);
// Buildings: Objects::init_build / Build::activate are production's (sim/production.hpp: init_build_site,
// build_activate_full).
// The counted-unit test of Unit::set_type / Unit::close / Objects::init_unit (production-spec 6.8.1, 6.10).
bool unit_counted_type(const Game& g, const Unit& u);  // control_cost, FIGHTERBOMBER or a government hero
bool unit_counted(const Game& g, const Unit& u);       // ... and not a decoy
// Unit::init @612100.
void unit_init(Game& g, Unit& u, int32_t who, int32_t type, int32_t o, int32_t x, int32_t y);
// Object::die @647080 -> Unit::close @60ee50 -> Object::close @647160 (dt = death type).
void unit_die(Game& g, Unit& u, int32_t dt);
// Object::add_to_world @64d8c0 / remove_from_world @647970: link in the wcoord list (head insertion) and stamp /
// clear the CollBlock footprint of every guy [0, guy_mark).
void add_to_world(Game& g, Object& ob);
void remove_from_world(Game& g, Object& ob);
// Objects::process_all @65dce0 and Objects::inc_time @65db70.
void objects_process_all(Game& g);
void objects_inc_time(Game& g);
// Unit::process @610bc0.
void unit_process(Game& g, Unit& u);
// Types: ObjectTypeData::is (non-strict / strict) on a unit.
bool unit_is(const Game& g, const Unit& u, int32_t t, bool strict = false);
// UnitData::can_transport @0046f960
bool unit_can_transport(const Unit& u);
// UnitData::invalid_loc @607c30 for this unit (builds the passability Mover: top path flag 4 forces transp).
int32_t unit_invalid_loc(const Game& g, const Unit& u, int32_t tx, int32_t ty, bool wlevel, bool fog, bool bldg,
                         bool transp, bool p7);
// UnitData::needs_transport @609920: 0 same tile or medium, 2 land -> water, 1 water -> land.
int32_t needs_transport(const Game& g, int32_t tx1, int32_t ty1, int32_t tx2, int32_t ty2);
// The unit-type predicates the movement code tests (UnitData::is_plane, is_helicopter, ...).
bool unit_is_air(const Unit& u);         // ptype.domain == 2
bool unit_is_helicopter(const Unit& u);  // unit_flags 'f' (0x20)
bool unit_is_modern_infantry(const Game& g, const Unit& u);

// ---- game.cpp: diplomacy (fog of war queries such as was_seen are in sim/fog.hpp; territory is not simulated yet).
// LeaderData::is_ally @6edb50 (self, or both sides at 2) and LeaderData::is_enemy @6ebaa0 (not self, either side at
// 0), on the diplos table loaded from the scenario (chunk 41) over Leader::init's defaults.
bool is_ally(const Game& g, int32_t a, int32_t b);
bool is_enemy(const Game& g, int32_t a, int32_t b);
// LeaderData::is_team @6ebd30 as Leader::init calls it (frame 0): self, or the same lobby team 0..3.
bool is_team(const Game& g, int32_t a, int32_t b);

}  // namespace ron::sim
