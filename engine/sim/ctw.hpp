// Conquer the World (docs/ctw-spec.md): the campaign data model loaded from the install (conquest/CTW_*_Map_01.xml,
// Data/conquestrules.xml), the ConquestGame state (spec section 2.1), the strategic round loop, armies, tribute,
// diplomacy, bonus cards, the link -> battle hand-off, outcomes and the campaign save.
//
// The strategic layer is NOT lockstep: it runs on one machine between battles with its own Random (the campaign's
// `random`, spec section 0). Only the battle (ctw_battle.cpp -> new_random_game / Game::load) must stay bit-exact.
//
// Files: ctw_data.cpp (XML loaders, card table), ctw.cpp (state, setup, round loop, armies, income), ctw_diplo.cpp
// (stances, costs, bribes), ctw_cards.cpp (the 38 bonus cards), ctw_ai.cpp (AI turns, AI-vs-AI), ctw_battle.cpp
// (style pick, GameInfo fill, battle build, outcomes), ctw_script.cpp (the strategic script hooks), ctw_save.cpp
// (the campaign save), script_funcs_ctw.cpp (the ctw_* builtins).
//
// Marks: "?" = unknown in the original (the spec's unknowns); such rules are replaced by the spec's known rules or a
// documented heuristic and carry a "(?)" comment.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "sim/newgame.hpp"
#include "sim/random.hpp"
#include "sim/rules.hpp"

namespace ron::sim {
class Game;
class ScriptHost;
}  // namespace ron::sim

namespace ron::sim::ctw {

constexpr int kSlots = 24;  // the 24 base tribes: ConquestLeader / stance matrices / piece lists are indexed by slot
constexpr int kNumCardTypes = 38;
constexpr int kMaxDifficulty = 5;
constexpr int kUsa = 20, kUssr = 13;  // the Cold War superpowers' slots (americans, russians)

enum Stance : int { kWar = 0, kPeace = 1, kAlly = 2, kVassal = 3 };
enum NodeFlag : int { kFortified = 1, kCapital = 2, kSupplyDepot = 4, kVisible = 8, kPriority = 0x10 };
// ConquestStyle categories (conquest_game_cats): the index of the style group.
enum StyleCat : int { kOffensive = 0, kDefensive = 1, kBarbarian = 2, kCustom = 3, kEspionageUSA = 4, kEspionageUSSR = 5 };
// conquest_map_styles.
enum MapStyle : int { kMsStandard = 0, kMsNoCity, kMsPlayerCity, kMsEnemyCity, kMsDefender, kMsAttacker, kMsAttackerDefense,
                      kMsDefenderAttack, kMsCustom };
// game_end codes (spec section 6).
enum GameEnd : int { kRunning = 0, kVictoryAllied = 1, kPlayerEliminated = 2, kTimeOver = 3, kArmageddon = 4,
                     kVictorySome = 5 };

// The card types of conquest_bonus_cards (spec section 4), index = ConquestBonusCard.type.
enum CardType : int {
	kOath = 0, kWonderCard, kSabotage, kEureka, kEconomicBoom, kMercenaries, kTreachery, kGreatThinker, kMerchantGuild,
	kLogistics, kPartisans, kCulturalDominance, kTradeEmbargo, kPropaganda, kSkillBonus, kCommandeerTransport, kBrigands,
	kSpyNetwork, kElephantArmy, kParmenio, kAntipater, kPtolemy, kMemnon, kSpitamenes, kPorus, kTraitors, kColonyBlockade,
	kPoliticalDissidents, kSpyAcademy, kContinentalSystem, kLetterOfMarque, kWarpath, kMissionaries, kPopulationBoom,
	kFouche, kTalleyrand, kSchwarzenberg, kPaoli
};

// ---------------------------------------------------------------- rules (Data/conquestrules.xml)

struct CardTypeDef {
	std::string name, key, desc;
	std::array<int, 6> data{};  // strategic, tactical, in standard deck, may be placed on the map, may stack, tribute value
	bool strategic() const { return data[0] != 0; }
	bool tactical() const { return data[1] != 0; }
	bool in_deck() const { return data[2] != 0; }
	bool placeable() const { return data[3] != 0; }
	bool can_stack() const { return data[4] != 0; }
	int value() const { return data[5]; }
	int variants = 1;  // Oath 24 (per tribe), Eureka 5 (tech lines + all), Economic Boom 5 (goods 0..4); else 1
};

struct StyleDef {
	int category = kOffensive;
	std::map<std::string, std::string> attr;  // every attribute of <STYLE>, name lower-cased (title, setup, runtime, ...)
	std::string title, id, setup, runtime, bonus, custom_map, objective, description, victory_text, defeat_text;
	int map_style = kMsStandard;
	int probability = 100, min_players = 2, min_victory = 0, max_victory = 0, min_age = 0, reveal_buildings = 0;
	std::array<int, 6> add_opponents{};
	bool land_only = false, water_only = false, opp_team_together = false, is_default = false, no_rare = false,
	     no_hero = false, not_first_time = false, enemy_override = false;
	std::string enemy_override_tribe;
	int player_start_x = -1, player_start_y = -1, ai_start_x = -1, ai_start_y = -1;
	int times_played = 0;  // ConquestStyle.times_played (saved)
	int num(const char* k, int def = 0) const;
	const std::string& str(const char* k) const;
};

struct ArmyUnit {
	std::string type;
	int num = 1, min_diff = 0;
};
struct BuildItem {
	std::string type, alt;
	int num = 1;
};
using CityBuild = std::vector<BuildItem>;  // one <CITY> of a <LEVEL>

struct ConquestRules {
	std::array<CardTypeDef, kNumCardTypes> cards;
	std::vector<std::string> stances, attitude_names, map_style_names, game_cat_names, rankings;
	std::vector<std::pair<int, int>> attitude_bands;  // [lo, hi) per name
	std::vector<StyleDef> styles;                     // the default CONQUESTSTYLES (used when a campaign has none)
	std::vector<ArmyUnit> army;
	int min_arrival_time = 2, max_arrival_time = 4;
	std::array<int, 6> min_no_attack{{4, 4, 3, 3, 2, 2}};
	int default_build_diff = 3;
	std::array<std::array<std::vector<CityBuild>, 8>, 6> build_out;  // [difficulty][territory level] -> cities
	std::array<std::array<int, 8>, 6> resource_bonus{};
};

// ---------------------------------------------------------------- campaign files (conquest/CTW_*_Map_01.xml)

struct LinkDef {
	std::string to;
	bool sea = false;
	std::string from;  // <LINK from=...> (Cold War): the one-way origin override
};

struct CountryDef {
	std::string name, id, continent, tileset, owner, wonder, rare, card, tribute;  // attribute texts (random / names)
	int value = 0, size = 0, red = 0, green = 0, blue = 0, nukes = 0;
	bool capital = false, supply_depot = false, priority_target = false;
	int left = 0, right = 0, top = 0, bottom = 0, x = 0, y = 0;
	std::vector<LinkDef> links;
};

struct ColonyDef {
	std::string name, id, owner, wonder;
	int value = 0;
	std::string tribute;
	std::vector<std::string> rares;  // goods names
};

struct ContinentDef {
	std::string name, id, barbarian_file, image;
};

struct TribeDef {
	std::string file, key, unique_unit, display_name;
	int skill = -1;  // 1..100, -1 = random
	bool no_eliminating = false, no_propose_ally = false, present = false;
	int base_armies = 1;
	std::vector<std::string> cards;
	std::vector<std::string> leaders, cities;
};

struct CampaignDef {
	std::string file;  // "CTW_Alexander_Map_01"
	std::string name, desc, intro, victory_conditions;
	std::map<std::string, std::string> root;  // every ROOT attribute
	int starting_age = 0, ending_age = 7, starting_round = 0, turns_per_age = 2, tex_width = 0, tex_height = 0;
	std::string hero_unit;
	bool allow_vassals = false, allow_continue = false, allow_wrap = false, strategic_nukes = false, ally_move = false,
	     free_move = false, no_gov_hero = false, territory_income = false, no_oath_cards = false, no_patriot = false,
	     allow_random_nations = false, allow_random_bonuses = false, client_state = false;
	std::string battle_script, post_turn_script, diplo_response_script, diplo_offer_script, links_script,
	    initialization_script, strategy_script, post_ai_script;
	std::vector<ContinentDef> continents;
	std::vector<CountryDef> countries;
	std::vector<ColonyDef> colonies;
	std::vector<std::string> deck;  // <BONUSCARDDECK name>
	std::array<TribeDef, kSlots> tribes;
	std::vector<std::string> valid_nations;
	std::vector<StyleDef> styles;  // campaign CONQUESTSTYLES; empty -> ConquestRules::styles
	std::string map_art, pick_map;  // conquest/RenderMaps/RENDER_<file>.png / conquest/<file>.png (UI)
	int length() const { return (ending_age - starting_age + 1) * turns_per_age; }
	int country(const std::string& id) const;  // case-insensitive id or name, -1
	int tribe_by_key(const std::string& key) const;
};

// The campaign files present in <game>/conquest, in the order of the front end's list (file stems, no extension).
std::vector<std::string> list_campaigns(const GameFiles& files);
// Throws std::runtime_error on a missing / malformed file.
std::shared_ptr<const CampaignDef> load_campaign(const GameFiles& files, const Rules& rules, const std::string& stem);
std::shared_ptr<const ConquestRules> load_conquest_rules(const GameFiles& files);
// Checks a loaded campaign (every link and owner resolves, topology). Returns a list of problems (empty = ok).
std::vector<std::string> validate_campaign(const CampaignDef& c);

// ---------------------------------------------------------------- state (ConquestGame, spec section 2.1)

struct Card {
	int type = -1;     // CardType
	int variant = 0;   // tribe slot (Oath), tech line (Eureka), good (Economic Boom)
	bool valid() const { return type >= 0; }
	bool operator==(const Card& o) const { return type == o.type && variant == o.variant; }
};

struct Node {  // ConquestNode
	int owner = -1, founder = -1, level = 0, turns_held = 0, wonder = -1, rare = -1, tribute = 0, flags = 0, value = 0, nukes = 0,
	    tech_cat = 0, continent = -1;
	std::string tileset;  // "" = dirty
	Card card;
	std::string display_name;                // set_territory_display_name; empty = the country name
	std::vector<int> links;                  // adjacent node indexes (both directions merged)
	std::vector<uint8_t> link_sea;           // parallel to links
	bool take_tribute = true;                // the node's one-time tribute has not been paid out yet
	bool nuked = false;                      // ConquestNode +0x3c: destroyed by a nuclear strike
};

struct Colony {
	std::string id, name;
	std::vector<int> rares;  // goods type indexes
	int wonder = -1, value = 0, tribute = 0, owner = -1;
};

struct Leader {  // ConquestLeader
	int capital = -1, capital2 = -1, nodes_controlled = 0, supply_depots = 0, tribute = 0, strength = 0, attack_link = -1,
	    inherent_skill = 0, num_cards_purchased = 0, vassal_of = -1, colony_blockade = 0, base_armies = 1, eliminated_by = -1;
	std::array<int, 6> base_rate{};
	std::array<int, kSlots> stances{}, stance_time{}, attitude_base{}, no_attack_time{}, bribe_paid{}, nodes_won_from{};
	std::array<int, kNumCardTypes> bonus_card_types_played{};
	bool active = false, valid = false, no_eliminating = false, no_propose_ally = false, no_negotiation = false,
	     reject_bribe = false, gunpowder_off = false, rares_off = false;
	std::string name;  // leader name (generate_name)
	std::vector<int> rare_resources, wonders, continents, new_nodes, no_attack, no_declare_war;
	std::vector<Card> cards, selected;  // hand, cards chosen for the next battle
	int fought_mask = 0, met_mask = 0;
	int prev_node_attacked = -1;  // ConquestLeader +0x40: the army node of this nation's last attack on the player (plan_strategy)
	int num_overruns = 0;         // ConquestLeader +0x3c
};

struct Piece {  // ConquestPiece: one army
	int node = -1;
	bool moved = false;
};

struct News {
	int round = 0;
	std::string text;
};

struct Reinforcement {  // ReinforcementArmy (setup_reinforcements @789d70)
	int time = 0;            // arrival tick (seconds of game time); -1 = the Terra Cotta Army's: when the civic line reaches CTW_WONDER_MIN_CIVICS
	int side = 0;            // the battle seat that receives it (0 the player, 1 the other main nation)
	int node = -1;           // origin node (-1: the wonder's)
	int loc_x = 0, loc_y = 0;    // where it appears (battle W cells, on the map edge)
	int dest_x = 0, dest_y = 0;  // where it marches to (ATTACK_TO)
	bool placed = false;
};

// One attackable link of the mover (find_available_links / process_link).
struct LinkChoice {
	int from = -1, to = -1, attacker = -1, defender = -1;  // nodes; leaders' slots (defender -1 = barbarian)
	bool sea = false;
	std::vector<int> att_supporters, def_supporters;  // slots
	int declare_war_cost = 0;                          // > 0: the mover must declare war first (cost in tribute)
	bool needs_war = false;
	bool overrun = false;                              // plan_strategy found an overrun (resolved without a battle)
};

struct Overrun {
	int node = -1, slot = -1;
};

struct StyleChoice {
	int category = kOffensive, index = -1;  // into styles()
	std::string custom_id;                  // battle_script override (CUSTOM group id)
	std::string info_text, goal1, goal2;
};

// What a finished battle reports.
struct BattleOutcome {
	bool played = true;   // kind 0
	bool won = false;     // player side won
	int game_ticks = 0;
};

// The result of ai_step().
struct AiStep {
	enum Kind { kProgress, kPlayerAttacked, kNukeStrike, kDone } kind = kProgress;  // kNukeStrike: ask the player (nuke_pending), finish_nuclear_exchange, ai_step again
	LinkChoice battle;  // kPlayerAttacked: the AI's attack on a player node (play it, then resolve_player_defense)
};

struct ConquestSetup {
	std::string campaign;  // file stem
	int difficulty = 2;
	int player = -1;       // slot, -1 = the campaign's VALID_NATION (random among them)
	bool random_nations = false, random_bonuses = false;
	bool vary_battle_seed = false;  // see ConquestGame::vary_battle_seed
	uint32_t seed = 1;     // "Seed (0 for random)": 0 is replaced by a clock-derived value by the front end
};

// ConquestDiploOffer (scenario/scriptlibrary/game_structs.bhs): the deal of the diplomacy window. tribe = the offerer,
// tribe2 = the nation that answers. offer_type: "Exchange", "Peace", "Ally", "Vassal".
struct DiploOffer {
	std::string tribe, tribe2, offer_type, response, id;
	int accepted = 0, counter_offer = 0, tribe_trib = 0, tribe2_trib = 0, disable_counter_offer = 0;
	std::vector<std::string> tribe_terr, tribe2_terr, tribe_col, tribe2_col, tribe_cards, tribe2_cards;
};

class ScriptRuntime;  // ctw_script.cpp: the campaign's own script host

class ConquestGame {
public:
	ConquestGame();
	~ConquestGame();
	ConquestGame(const ConquestGame&) = delete;
	ConquestGame& operator=(const ConquestGame&) = delete;

	// ---- data (not saved: rebuilt from the files on load)
	const GameFiles* files = nullptr;
	const sim::Rules* rules = nullptr;
	std::shared_ptr<const CampaignDef> def;
	std::shared_ptr<const ConquestRules> crules;
	std::vector<StyleDef> style_table;  // styles in play; times_played saved

	// ---- saved scalars (ConquestGame raw block)
	int player_tribe = -1, difficulty = 2, cur_round = 0, num_turns = 0, turns_skipped = 0, cur_age = 0, starting_age = 0,
	    ending_age = 0, scen_per_age = 2, turns_no_attack = 0, game_end = 0, return_to_node = -1, defcon_level = 0,
	    nuke_launch_turns = 0, talleyrand_tribe = -1, talleyrand_state = 0, fouche_tribe = -1, style_index1 = -1,
	    style_index2 = -1, cur_espionage_level = 0;
	uint32_t seed = 1;
	// The original builds every battle's map with conquest_game.seed (build_game copies it into game_random once per battle: two
	// battles with the same style and size share a map). false = that; true = a seed derived from (seed, link, round), so two
	// battles of one campaign get different maps (an option of this port).
	bool vary_battle_seed = false;
	bool random_nations = false, random_bonuses = false, has_fought_barbarian = false, free_move_turn = false,
	     need_defensive_bonus = false, piece_moves_disabled = false, attacked_this_round = false;
	// Cold War (ctw_cold.cpp): a strike in preparation (saved: the AI launched; the UI asks the player whether to retaliate)
	bool nuke_pending = false, finished_espionage = false;
	int nuke_p1 = -1, nuke_p2 = -1;
	std::vector<int> tribe_nukes, tribe2_nukes, tribe_nuke_targets, tribe2_nuke_targets;  // launch nodes / aimed nodes (-1 none) of both sides
	int tribe_must_attack = -1;  // ConquestGame +0x48: a nation that declared war on the player and must attack him (do_diplomacy)
	int ai_phase = 0;  // 0: the AI tribe at valid_tribes[ai_index] has not started its turn, 1: planning (saved so a round can resume)
	std::string map_name, map_id, player_name = "Player";
	Random random;  // the campaign stream; use rget() (inclusive)
	std::array<Leader, kSlots> leaders;
	std::vector<Node> nodes;
	std::vector<Colony> colonies;
	std::array<std::vector<Piece>, kSlots> pieces;
	std::vector<std::string> deck;  // the campaign's deck as card names (make_card_list source, saved)
	std::vector<Card> pool;         // the shuffled draw pile (saved)
	std::vector<News> news;
	std::vector<int> valid_tribes;  // the AI move order of the round in progress
	std::vector<Overrun> overrun_armies;
	std::vector<int> continents_captured;
	std::vector<Reinforcement> reinforcements;
	std::map<std::string, int32_t> stored_ints;
	std::map<std::string, std::string> stored_strs;
	std::vector<std::string> sub_goals;
	std::vector<int> allied_punks;
	std::map<std::string, int> deals;  // stored_diplo_deals (NamedArray<int>): deal id -> 1 accepted / 0 declined; absent = never answered (-1 to the scripts)
	// script-driven presentation / state (saved): the custom end screen, the nuke / depot switches, no-negotiation text
	std::string end_title, end_text, end_image, end_button, no_negotiate_text;
	int end_victory = -1;
	bool last_battle_victory = false, nuke_building_disabled = false, supply_depots_hidden = false, espionage_disabled = false;

	// ---- not saved (recomputed)
	std::vector<LinkChoice> available_links;
	LinkChoice cur_link;          // the link of the battle in preparation / in progress
	StyleChoice cur_style;
	int ai_index = 0;             // position in valid_tribes
	bool ai_in_progress = false;
	std::vector<std::string> log;  // debug / headless trace (not saved)
	mutable std::array<std::array<int, 2>, 8> start_locs{{{-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}};  // battle seats' start tiles (get_starting_loc_x / y)
	int center_camera = -1;        // ctw_center_camera: a node the UI should look at (consumed by the UI)
	// ---- the battle in progress (not saved)
	std::array<int, 8> battle_slot{{-1, -1, -1, -1, -1, -1, -1, -1}};  // campaign nation of each battle seat
	std::array<bool, 8> battle_human{};
	std::array<uint8_t, 8> reinf_arrived{};  // ScenarioData::reinforcements_arrived: read once by ctw_reinforcements_arrived
	std::vector<bool> reinf_att_side;        // per reinforcement: the army belongs to the attacker's nation
	int reinforce_mark = 0;                  // reinforcements placed so far
	std::vector<std::string> battle_notices; // "Reinforcements arrive from X": drained by the UI
	sim::Game* battle_game = nullptr;        // the battle built by build_battle (valid until it is destroyed)
	std::vector<std::pair<int, std::u16string>> saved_names;  // rules tribe names replaced by the campaign's during a battle
	int battle_bonus = 0;                    // Skill Bonus amount of the battle just won (run_bonus_script)
	std::vector<std::string> messages;  // popup_dialog / notice texts of the script hooks, drained by the UI
	std::vector<DiploOffer> proposals;   // AI offers to the player (diplo_offer hook)

	// ---------------------------------------------------------------- setup (ctw.cpp)
	// Builds a new campaign (spec 2.2). Returns "" or an error text.
	std::string setup(const GameFiles& f, const sim::Rules& r, const ConquestSetup& s);
	// Re-attaches the data files to a loaded state (load calls this).
	std::string attach(const GameFiles& f, const sim::Rules& r, const std::string& campaign);

	int rget(int lo, int hi) { return random.get(lo, hi + 1); }  // inclusive, as ConquestGame's Random::get is
	int length() const { return def ? def->length() : 0; }
	int rounds_left() const { return std::max(0, length() - cur_round); }
	const std::string& tribe_name(int slot) const;
	std::string node_name(int n) const;
	int eff(int owner) const;  // vassal -> master
	int node_eff_owner(int n) const { return eff(nodes[size_t(n)].owner); }
	bool is_player(int slot) const { return slot == player_tribe; }
	int find_node(const std::string& id_or_name) const;  // case-insensitive, -1
	int slot_of(const std::string& tribe) const;          // "Greeks" / "greeks" / display name, -1
	bool has_army(int node) const;                        // <= 1 army per node
	Piece* army_at(int node);
	int army_count(int slot) const { return int(pieces[size_t(slot)].size()); }
	std::vector<int> owned_nodes(int slot, bool with_vassals = false) const;
	int num_nodes(int slot, bool with_vassals) const;
	std::vector<int> active_slots() const;  // valid && active
	int node_strength(int n) const { return nodes[size_t(n)].level; }

	// ---------------------------------------------------------------- the round (ctw.cpp)
	int army_cap(int slot) const;
	// place_new_armies: tops the slot up to its cap (adds on empty owned nodes, removes the first when over). Returns news text or "".
	std::string place_new_armies(int slot);
	std::vector<LinkChoice> find_available_links(int slot);  // links the mover can attack along (script links hook included)
	std::vector<int> move_targets(int node);                 // friendly nodes an army on `node` may move to this round
	int can_drop_army(int from, int to, int slot) const;     // 0 ok, else the spec's negative codes
	bool move_army(int from, int to, std::string* why = nullptr);  // the mover's own move (player or script)
	void end_turn_player();                // spec 2.3 end_turn: skip bonus, tribute, flags (does not start the AI)
	void begin_player_round();             // clears the moved flags, recomputes the player's links
	void refresh_links() { available_links = find_available_links(player_tribe); }
	void assign_supply(int slot);          // supply_depots count
	void begin_ai_round();                 // valid_tribes shuffle
	AiStep ai_step();                      // plays AI tribes until one attacks the player or all are done
	void finish_round();                   // assign_bonuses, adjust attitudes, advance_round, links for the player
	void advance_round();
	void check_income(int slot);
	void assign_bonuses();
	void clear_moved();
	void add_news(const std::string& text);
	void transfer_to(int node, int new_owner, bool collect, std::string* text = nullptr);  // ConquestNode::transfer_to
	void defeat(int slot, int by);                                                          // ConquestLeader::defeat
	void check_allied_victory();
	void check_continents(int slot);
	void set_node_level(int n, int level);
	int count_continent_nodes(int cont, int slot) const;
	bool player_alive() const { return leaders[size_t(player_tribe)].active; }
	void give_tribute(int slot, int amount) { leaders[size_t(slot)].tribute = std::max(0, leaders[size_t(slot)].tribute + amount); }

	// ---------------------------------------------------------------- diplomacy (ctw_diplo.cpp)
	int stance(int a, int b) const { return a == b ? kAlly : leaders[size_t(a)].stances[size_t(b)]; }
	void set_stance_pair(int a, int b, int st);
	void set_war(int a, int b);
	void set_peace(int a, int b);
	void set_ally(int a, int b);
	void set_vassal(int master, int vassal);
	int get_attitude(int a, int b) const;     // a's attitude toward b (spec section 3)
	const std::string& attitude_name(int a, int b) const;
	int war_cost(int a, int b) const;
	int peace_cost(int a, int b) const;
	int alliance_cost(int a, int b) const;
	int territory_cost(int buyer, int node) const;
	int vassal_cost(int master, int vassal) const;
	int card_cost(int slot) const;
	int build_cost(int node) const;           // a fortification
	int nuke_cost(int slot) const;
	int num_allies(int slot) const;
	bool can_declare_war(int a, int b, std::string* why = nullptr) const;
	bool buy_war(int a, int b, std::string* why = nullptr);
	bool buy_peace(int a, int b, std::string* why = nullptr);
	bool buy_ally(int a, int b, std::string* why = nullptr);
	bool buy_vassal(int master, int vassal, std::string* why = nullptr);
	bool buy_territory(int buyer, int node, std::string* why = nullptr);
	bool buy_fortification(int slot, int node, std::string* why = nullptr);
	bool demand_tribute(int from, int to, std::string* why = nullptr);  // `from` demands half the purse of `to`
	bool offer_bribe(int payer, int receiver, int amount, std::string* why = nullptr);
	bool available_army_at(int node) const;  // ConquestNode::has_available_armies(node, 1)
	void do_ai_diplomacy(int slot);           // ConquestLeader::do_diplomacy @79e030
	bool propose_peace(int me, int target);   // ConquestLeader::propose_peace @7a0c20 (peace -> alliance when already at peace)
	void adjust_attitudes();                  // post_adjust_attitudes
	bool diplo_accept(int offerer, int target, int offer_type);  // the response hook (script or built-in rule)

	// ---------------------------------------------------------------- cards (ctw_cards.cpp)
	std::string card_name(const Card& c) const;
	std::string card_desc(const Card& c) const;
	bool parse_card(const std::string& name, Card* out) const;
	std::vector<Card> make_card_list();          // the shuffled deck (spec 2.2 step 3)
	Card draw_card();
	void give_card(int slot, const Card& c);
	bool buy_card(int slot, std::string* why = nullptr);
	bool card_targets(int slot, const Card& c, std::vector<int>* nodes, std::vector<int>* slots) const;  // strategic targets
	bool play_strategic_card(int slot, int hand_index, int target, int target2 = -1, std::string* why = nullptr);
	bool select_tactical(int slot, int hand_index, std::string* why = nullptr);
	void clear_selected(int slot);
	int card_value(const Card& c) const;
	std::vector<std::string> find_available_wonders(int slot) const;

	// ---------------------------------------------------------------- battles (ctw_battle.cpp)
	StyleChoice pick_style(const LinkChoice& l);
	// The GameInfo-shaped description of the battle for `l` (spec 5.2) and the engine setup built from it.
	struct BattleSeat {
		int slot = -1;           // campaign nation slot, -1 barbarian
		int team = 0;
		bool human = false;
		bool barbarian = false;
		std::string name;
		int node = -1;           // the node it defends / attacks from
	};
	struct BattleDef {
		LinkChoice link;
		StyleChoice style;
		std::vector<BattleSeat> seats;
		sim::RandomGameSetup setup;  // the engine parameters (generator style/size, victory, technology ...)
		std::string custom_map;      // .scx path (game relative) when the style is a custom map
		std::string setup_script, runtime_script, bonus_script, objective, description, victory_text, defeat_text;
		int time_limit_min = 90;
		uint32_t seed = 1;
		std::string tileset;
		int defender_level = 0;
		std::array<int, 8> start_armies{};  // armies at each seat's start (start_add_armies)
		std::vector<int> side;              // team of each seat (0 player side, 1 other)
		bool defender_default = false;      // the defender's start city is built out from BUILDINGS_<diff>[level]
		int map_style = 0;                  // the style's conquest map style (MapStyle)
	};
	BattleDef make_battle(const LinkChoice& l, const StyleChoice& style);
	// Builds the Game for a battle (new_random_game or Game::load of the custom map), applies the conquest
	// parameters and the tactical cards. `extras` receives the renderer's scenario records. Returns null + error.
	// `f` and `r` are the files and rules the returned Game will keep pointers into (the caller's, e.g. the renderer's).
	std::unique_ptr<sim::Game> build_battle(const GameFiles& f, const sim::Rules& r, const BattleDef& b, const sim::AnimSource* anims,
	                                        sim::RandomExtras* extras, std::string* error) const;
	// ---- tactical cards in the battle (ctw_tactical.cpp)
	void select_ai_tactical(int slot, const LinkChoice& l);  // ConquestLeader::select_tactical_cards (the AI's picks)
	void consume_selected(int slot, bool count_played);      // the cards of a finished battle are spent
	void apply_battle_cards(sim::Game& g, const BattleDef& b) const;  // setup_scenario_bonuses / _rares and the card units
	// ConquestStyle::get_bonus_amt: runs the style's `bonus` script on the finished battle; -1 = no script / failed.
	int run_bonus_script(sim::Game& battle, const StyleDef& style, std::string* text);
	// ---- reinforcements (ctw_reinf.cpp)
	void setup_reinforcements(const LinkChoice& l, const StyleDef& style);
	void locate_reinforcements(const LinkChoice& l, int map_style, int world_w, int world_h);
	void find_reinforcement_loc(int world_w, int world_h, int a, int b, int n, int map_style, bool att, Reinforcement* out) const;
	void apply_tribe_names(sim::Rules& r, const BattleDef& b);    // the HUD shows the campaign's nation names in a battle
	void restore_tribe_names(sim::Rules& r);
	void place_reinforcement(sim::Game& g, Reinforcement& r);
	void place_reinforcements(sim::Game& g);  // every frame of a battle (Game::do_frame step 6)
	int invasion_dir() const;                 // get_conquest_invasion_dir
	float map_scale() const;
	float map_width() const;
	void country_ws(int node, float* x, float* y) const;  // the country's position on the strategic map (starting_coords_ws)
	// §6 outcome kinds: 0 the battle was played, 1 the attacker wins without a battle, 2 a forced loss.
	void process_results(int kind, bool player_won, const LinkChoice& l, const StyleChoice& style);
	// An AI attack on the player's node was resolved (after the battle, or automatically): continues the round.
	void resolve_player_defense(bool player_won);
	bool resolve_ai_vs_ai(const LinkChoice& l);  // one automatic attack; returns whether the attacker won
	bool check_overrun(const LinkChoice& l) const;
	bool check_overrun_nodes(int attacker_node, int defended_node, int forced_defender, bool* potential) const;
	bool check_potential_overrun(int me, int target) const;
	void resolve_overrun(const LinkChoice& l);
	int num_adjacent(int me, int j) const;         // ConquestLeader::get_num_adjacent
	bool share_neighbor(int me, int j) const;
	int nodes_controlled(int slot, bool with_vassals) const;  // ConquestLeader::get_nodes_controlled

	// ---------------------------------------------------------------- Cold War (ctw_cold.cpp)
	bool nuke_tribes(int* player, int* other) const;  // get_nuke_tribes: the two superpowers, false without strategic nukes
	int num_nukes(int slot) const;
	int build_nuke(int node, bool pay);               // ctw_build_nuke: 1 built, 0 refused, -1 bad node
	void shall_we_play_a_game(int p1, int p2);
	bool ai_launch_nukes();                           // ctw_ai_launch_nukes
	void finish_nuclear_exchange(bool retaliate);     // applies the aimed missiles (the_only_move_is_not_to_play)
	int espionage_category() const;
	int espionage_missions() const;
	bool can_espionage(std::string* why = nullptr) const;
	bool begin_espionage(StyleChoice* out, std::string* why = nullptr);
	BattleDef make_espionage_battle(const StyleChoice& style);
	void process_espionage_results(bool won, const StyleChoice& style);

	// ---------------------------------------------------------------- AI (ctw_ai.cpp)
	int plan_strategy(int slot, LinkChoice* attack);  // 0 nothing to do, 1 an overrun (attack->overrun), 2 an attack to fight / resolve
	void play_ai_cards(int slot);
	int ai_army_target(int slot, const std::vector<LinkChoice>& options) const;  // best option index or -1

	// ---------------------------------------------------------------- save (ctw_save.cpp)
	std::vector<uint8_t> save_bytes() const;
	// Replaces this state with the saved one; the data files are re-read from `f`. Returns "" or an error text.
	std::string load_bytes(const GameFiles& f, const sim::Rules& r, const std::vector<uint8_t>& bytes);
	bool save_file(const std::string& path, std::string* err = nullptr) const;
	std::string load_file(const GameFiles& f, const sim::Rules& r, const std::string& path);
	std::string save_name() const;  // "<map_name> - <player nation>"
	std::vector<uint8_t> digest_bytes() const { return save_bytes(); }

	// ---------------------------------------------------------------- scripts (ctw_script.cpp)
	std::unique_ptr<ScriptRuntime> scripts;
	ScriptRuntime& script_runtime();                     // created on first use (loads the campaign's hooks)
	void scripts_drop();                                 // forgets the script runtime (re-created on demand)
	// The static variables of the campaign's script files (they outlive a run: `static int x = ...` counters) as a blob
	// for the save, and back. Restoring compiles the hook files first.
	std::vector<uint8_t> script_statics();
	void restore_script_statics(const std::vector<uint8_t>& blob);
	void run_init_script();
	void run_post_turn();                                // post_turn_script_file (turn)
	void run_post_ai();
	bool run_links_script(int slot);                     // true: use only the links the script added
	bool run_strategy_script(int slot);                  // true: the script played the AI's turn
	std::string run_battle_override(const LinkChoice& l, StyleChoice* style);  // returns the CUSTOM id or ""
	// Runs the campaign's diplo_response script on `offer` (the AI's answer: accepted / response / counter_offer). False
	// when no script answered; the caller then applies the built-in rule (diplo_accept).
	bool run_diplo_response(DiploOffer* offer);
	// The campaign's diplo_offer hook: the AI nation `slot` may make the player an offer (story offers of the campaigns). True when
	// the script filled `offer` with one.
	bool run_diplo_offer(int slot, DiploOffer* offer);
	// Applies an accepted deal (tribute, territories, colonies, cards, and the Peace / Ally / Vassal stance). "" or a reason.
	std::string apply_offer(const DiploOffer& offer);
	// The player's answer to an offer of an AI nation (the diplomacy window / the host): applies it when accepted and records the
	// deal id in `deals` (ctw_deal_accepted reads it). "" or the reason the deal could not be made.
	std::string answer_proposal(const DiploOffer& offer, bool accept);
	std::string tribe_key(int slot) const;  // the TRIBE KEY (lower-case) the scripts use, "" for barbarians
	LinkChoice pending_script_attack;                    // ctw_ai_set_attack
	std::vector<std::pair<int, int>> script_links;       // links added by ctw_add_link during the links hook (node pairs)
	std::map<int, std::set<int>> script_no_attack;       // set_no_attack by slot (also mirrored into Leader::no_attack)

	// the node-level names the scripts use
	std::string territory_owner_name(int node, bool ignore_vassal) const;
};

// find_angle / angle_to_degrees (ctw_reinf.cpp): the compass degrees of a strategic-map delta (x right, y down).
int angle_degrees_of(int dx, int dy);

// Constants of the CTW_* table (rules.xml CONSTANTS) by lower-case name; 0 when unknown.
int K(const sim::Rules& r, const char* name);
// Utility: case-insensitive equality.
bool ieq(const std::string& a, const std::string& b);
std::string lower(std::string s);

}  // namespace ron::sim::ctw
