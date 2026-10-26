// A new random-map game (docs/randommap-spec.md section 1, docs/sim-implementation.md "Random games"): the skirmish
// setup of the original's GameInfo -> Setup::build_game -> a Game in the same shape Game::load leaves, so the AI, the
// scripts, save / load and the renderer work on it unchanged.
//
// Pieces: RandomGameSetup (what the setup screen / --random produce), new_random_game (the whole build), the text form
// of a setup ("random:..." — a save records a random game as setup + seed in the scenario-path slot and regenerates the
// game on load, sim/save.cpp), and the Scenario snapshot of the finished game that the renderer draws from (terrain
// synthesized from the generated world, the objects as scenario records).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/scenario.hpp"
#include "sim/anims.hpp"
#include "sim/game.hpp"
#include "sim/mapgen.hpp"
#include "sim/rules.hpp"

namespace ron::sim {

// One seat of the setup (GameInfo.player[i]: tribe, team, human / AI, diff, color).
struct NationSetup {
	int tribe = -1;       // 0..23 (rules tribes), -1 random
	int team = -1;        // -1 none, 0..3 team 1..4
	bool human = false;
	int difficulty = 2;   // 0 Easiest .. 5 Toughest
	int color = -1;       // team colour slot 0..7, -1 = the seat number
};

// The skirmish setup. Field meanings are GameInfo's (docs/replay-spec.md section 3): indexes into the rules.xml category
// lists named in the comments. (The name GameSetup is taken by the rules loader's GameInfo subset.)
struct RandomGameSetup {
	int map_style = mapgen::kStyleOldWorld;  // mapstyles: 0 Random, 1 Random Land, 2 Random Sea, 3..5 the secret pickers, 6..22
	int map_size = 3;                        // mapsizes: 0 Tiny .. 6 Big Huge
	uint32_t seed = 1;                       // GameInfo.seed (the front end draws one when the player leaves it on "random")
	int num_nations = 2;                     // 2..8, seats 0..num_nations-1
	std::array<NationSetup, 8> nation{};
	int starting_town = 1;                   // startingtowns: 0 Nomad, 1 City Center Only, 2 Small Town, 3 Large Town
	int starting_resources = 1;              // startingresources: 0 Low, 1 Standard, 2 x2 ... 12 Random
	int starting_technology = 0;             // startingtechs: 0..7 age, 8 All Technologies, 9 Random Age
	int ending_technology = 7;               // endingtechs: 0 Classical .. 7 Early Info (the engine's 0..7 span)
	int victory = 0;                         // victories: 0 Standard ... 8 Economic, 9 Tech Race
	int elimination = 0;                     // eliminations: 0 Conquest, 1 Capital, 2 Sudden Death Capital, 3 Sudden Death
	int score_goal = 0, time_limit = 0, wonderwin = 0, popwin = 0, econwin = 0;  // the victory option lists
	int pop_limit = 5;                       // poplimits: 0 50 .. 5 200
	int tech_cost = 3;                       // techcosts: 3 Normal
	int rush_rules = 0;                      // rushrules
	int reveal_map = 1;                      // revealmaps: 0 No Ally LOS, 1 Normal, 2 Explored, 3 All Visible
	int game_rules = 0;                      // gamerules: 0 Standard
	int game_speed = 1;                      // GameInfo.game_speed (presentation; not read by the simulation)
	bool randomize_starts = false;           // map->randomize_starts: shuffle the start locations
	bool no_tribe_powers = false;            // GameInfo.flags & 4
	bool ctw = false;                        // Conquer the World battle (semaphore byte2 & 2: sim/ctw.hpp builds these); text key ctw=1 when set
	// The conquest map input of a Conquer the World battle (mapgen::ConquestInput, encoded by conquest_input_string): the square of the
	// two countries' silhouettes, the starts and the extra opponents. Empty for every other game. new_random_game generates the
	// battle's conquest map (styles 24 / 25) from it when no ConquestScope is open, so a save of a battle (which stores this setup
	// text) regenerates the same map. Text key conq=...
	std::string conquest;
	// A Conquer the World battle's style scripts (BattleDef::runtime_script / setup_script, game-relative paths): new_random_game compiles
	// them after the base scripts, as ConquestGame::build_battle does (without running the setup script), so a loaded battle save has
	// the same compiled script files as the battle it was taken from. Text keys btrt=... / btsu=...
	std::string battle_runtime, battle_setup;
};

// Seats 0..7 default: seat 0 human with a random tribe, the others AI.
RandomGameSetup default_random_setup(int num_nations = 2);

// The text form of mapgen::ConquestInput: a '.'-separated record (version, flags, node ids, supporters, countries, tileset) and the
// land bitmap as alternating run lengths (water first). No ';' or '=' inside, so it nests in the setup text. parse returns false on
// anything malformed.
std::string conquest_input_string(const mapgen::ConquestInput& in);
bool parse_conquest_input(const std::string& text, mapgen::ConquestInput& out);

// The setup text of the Conquer the World battle most recently built in this process (ctw_battle.cpp sets it, with the conquest
// input inside when the map is a conquest map). The ronmap battle keeps the placeholder scenario path "ctw:battle"; scenario_rel_path
// (save.cpp) swaps it for this text, so the save's scenario slot can regenerate the map.
void set_battle_setup_text(const std::string& text);
std::string battle_setup_text();

// A Conquer the World battle on a custom map (a .scx of the campaign) has no random setup to regenerate: its scenario slot is
// "ctwmap:" + the map's path + the style's scripts (";btrt=" runtime, ";btsu=" setup, percent-encoded). Loading it parses the
// map, attaches the scripts (compiled in build_battle's order, the setup script not run: its effects are in the save) and
// restores the state. `map` is game-relative with '/' separators when it lies in the install (scenario_rel_path).
struct CtwMapBattle {
	struct Player {  // what build_battle wrote into the scenario's player records before the load (the AI scripts it loads depend on them)
		int who = 0, tribe = 0, is_human = 0, difficulty = 0;
		std::string name;
	};
	std::string map, runtime, setup;
	std::vector<Player> players;
};
std::string ctw_map_battle_string(const CtwMapBattle& b);
bool parse_ctw_map_battle(const std::string& text, CtwMapBattle& out);
inline bool is_ctw_map_scenario(const std::string& scenario) { return scenario.compare(0, 7, "ctwmap:") == 0; }

// The text form (the scenario-path slot of a save): "random:" + key=value pairs separated by ';'. Round-trips every
// field. parse returns false on a malformed string.
std::string random_setup_string(const RandomGameSetup& s);
bool parse_random_setup(const std::string& text, RandomGameSetup& out);
inline bool is_random_scenario(const std::string& scenario) { return scenario.compare(0, 7, "random:") == 0; }

// The `--random STYLE,SIZE,SEED,NATIONS[,TRIBES...]` option grammar of the apps (STYLE: a MapStyleIndex or a style name,
// NATIONS 2..8, TRIBES a comma list of tribe indexes for seats 0.., "r" or -1 for random). Returns "" or an error text.
std::string parse_random_option(const Rules& rules, const std::string& spec, RandomGameSetup& out);

// The cosmetic land doobers (bushes and rocks) of a finished world, hash-placed from `seed` on open land (what a random game's terrain
// gets); `doobers` is the count the generator reported, at most 1200 are placed. Used by the recordings of the original game as well.
void place_land_doobers(const World& w, uint32_t seed, int doobers, Terrain& t);

// What World::analyze_map @6b58b0 leaves in the World besides land_size / sea_map / the regions' flags 4 and 8 (the original's unmodelled World
// members: the checksum shadow gets them from here; docs/original-save.md section 6.21).
struct WorldAnalysis {
	int32_t player_reg = 0, resource_reg = 0, forest_size = 0, mountain_size = 0, rock_size = 0, total_metal = 0, total_oil = 0, goodies = 0,
	        land_resources = 0, sea_resources = 0;
};

// Everything the renderer needs next to the Game (all optional output of new_random_game).
struct RandomExtras {
	mapgen::Result result;  // the generator's result (starts, rivers, mountains, empire list, draw counts)
	Scenario scenario;      // terrain + objects as scenario records (resolve_battlefield / the terrain mesh read it)
	RandomGameSetup setup;  // the setup as resolved (random tribes, random age ...)
	WorldAnalysis analysis; // World::analyze_map's counts
	std::string name;       // "Random Map: Great Lakes, 60x60, seed 123"
	std::string error;      // set when new_random_game returned null
	// INPUT (the only one): the original-save shadow the checksum provider needs (sim/original_save.hpp, a fresh-state template save with its
	// objects rebound to the new game by the caller). new_random_game sets it as Game::original before the empires are built, so the start
	// towns' Terrain::object_placed land runs as in the original, and gives the shadow the generated map's tileset.
	std::shared_ptr<OriginalShadow> original;
};

// Setup::build_game for a random map plus the Game::run pieces Game::load runs (rules, leaders, starting resources, scripts).
// The Game keeps pointers into `files` and `rules` (as after Game::load); `anims` may be null. Returns null (and
// extras->error) when the generator fails. Deterministic: the same setup gives byte-identical saves.
std::unique_ptr<Game> new_random_game(const GameFiles& files, const Rules& rules, const AnimSource* anims,
                                      const RandomGameSetup& setup, RandomExtras* extras = nullptr);

}  // namespace ron::sim
