// Save / load of the simulation (docs/save-format.md): the engine's own native format, not the original game's .sav.
//
// A save holds the scenario path (relative to the game folder) and the load seed, and then every piece of simulation
// state. Loading runs the normal Game::load(files, scenario, rules, seed) first, which rebuilds everything derived from
// the install's files (rules, art, terrain heights, regions, mountains), and then overwrites all state from the file, so
// a game saved at frame F and continued M frames gives bit for bit the digests of the loaded save continued M frames.
//
// Errors throw std::runtime_error whose message names the section ("save: section 'WRLD': ...").
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "sim/anims.hpp"
#include "sim/game.hpp"

namespace ron::sim {

constexpr uint32_t kSaveVersion = 16;  // 16: Ammo::has_path / spline and Game::spline_pool (the missile ammo's B-spline, hashed in `ammo`, and the Recycler<Spline> pool whose allocations survive); 15: Ammo::crash_piece (the wreck keeps its own plane piece), City::vans_cap (the allocation of the Array<CaravanLink>, hashed with the header of a non-empty array); 14: DeathObj::turret_angles / node_flags (the corpse keeps the dying guy's pivot angles and the node flags 0xffff, both hashed); 13: Region::scouted / scout_mark (the ferry search; Unit::think_scout marks a region scouted); 12: Game::docks (the dock records) and Leader::dock_mark / reg_docks (Docks::init_dock; gull units circle the docks); 11: CaravanRec::road / making_road / reset_road and the suspended road search (PathNode::z_val; caravan roads, economy-spec 6.4); 10: section NUKE (nuclear explosions in progress, sunk tiles, embargo stamps); 9: World::roads (the road candidates and the Roads sweep cursor); 8: Leader combat-statistics (rates, 15-second sums, frame_battle; Leader::process_event_frame); 7: Build::gpiece (WallData +0x58, the building's art piece); 6: Leader::flock_stamp (Objects::add_flock's 225-frame limit); 5: section HGHT (the terraformed height grid); 4: Leader::capital_stamp / ally_stamp (do_damage warning stamps)

// What the header of a save says (read_save_info) and what the loader found out (load_game).
struct SaveInfo {
	uint32_t version = 0;
	std::string scenario;  // as given to save_game: game-relative, or absolute for a scenario outside the install
	uint32_t seed = 0;     // the Game::load seed
	int32_t frame = 0, tick = 0;
	bool has_scripts = false;    // a ScriptHost was attached (script_host_load had run)
	bool has_own_rules = false;  // a scenario script rewrote the rules tables (Game::mut_rules)
	uint64_t rules_hash = 0;     // FNV-1a of the loaded rules tables the game started from
	bool has_battle_campaign = false;  // header flag 1: a Conquer the World battle that embeds its campaign (section CTWB)
	// Filled by load_game: errors script_host_load reported (the original run saw the same ones).
	std::vector<std::string> script_errors;
};

// The scenario file `file` (as the apps hold it: a path inside the install or an absolute one) as a save stores it:
// relative to the game folder with '/' separators when it lies inside, else unchanged.
std::string scenario_rel_path(const GameFiles& files, const std::string& file);

// Writes g to `path` (atomically: a temporary file next to it, then a rename). A path ending in ".gz" is gzipped; the
// loader reads both. scenario_rel / seed are what Game::load was run with.
void save_game(const Game& g, const std::string& scenario_rel, uint32_t seed, const std::string& path);
// The same bytes in memory (the round-trip self-check compares two of them).
std::vector<uint8_t> save_game_bytes(const Game& g, const std::string& scenario_rel, uint32_t seed);

SaveInfo read_save_info(const std::string& path);
SaveInfo read_save_info(const std::vector<uint8_t>& bytes);

// Compares two saves (save_game_bytes) section by section: "" when identical, else the first difference ("section
// 'LEAD' differs at payload byte 4120 of 27984" ...). Used by the round-trip self-check and for debugging.
std::string describe_save_difference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);

// The same for two live games, naming the field where the first differing byte of the saved state lies ("section 'OBJS'
// differs at payload byte 10196 of 56124 (in v.damage < unit slot 12 < leader 1 object list)").
std::string describe_game_difference(const Game& a, const Game& b, const std::string& scenario_rel, uint32_t seed);

// Builds the saved game: parses the scenario, runs Game::load (and script_host_load when the save has scripts) and
// restores the state. `rules` is the loader's rules (load_rules + start_game(GameSetup())); it and `files` must outlive
// the returned Game, as for Game::load. `anims` is the art source (may be null, as in the apps without art). Throws when
// the file is corrupt, was written by another format version, belongs to different rules tables, or its scenario is
// missing.
std::unique_ptr<Game> load_game(const GameFiles& files, const Rules& rules, const AnimSource* anims, const std::string& path,
                                SaveInfo* info = nullptr);
std::unique_ptr<Game> load_game_bytes(const GameFiles& files, const Rules& rules, const AnimSource* anims,
                                      const std::vector<uint8_t>& bytes, SaveInfo* info = nullptr);

}  // namespace ron::sim
