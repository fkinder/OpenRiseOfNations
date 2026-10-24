// The game's BigHugeScript run time (docs/script-spec.md section 6): one bhs::Runtime per game (RunTimeEnv
// `script_run_time`), the script state of ScenarioData the builtins use (timers, find counters), and the three call
// points of the simulation:
//   - the scenario script, every frame at Game::do_frame step 5 (frame-spec 3), no arguments;
//   - the general powers script (scripted rules / Conquer the World), every frame with frame > 0, right after;
//   - the AI production scripts (ai/scripts/<name>.bhs), state 1 of Leader::production_ai (plan_strategy).
// The builtins are registered in engine/sim/script_funcs.cpp (environment, the AI production set and the scenario
// queries that the simulation supports); every other builtin is a logged stub returning its error value.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/scenario.hpp"
#include "script/bhs_vm.hpp"
#include "sim/script_ui.hpp"

namespace ron::sim {

class Game;
namespace ctw {
class ConquestGame;
}  // namespace ctw

// ScriptTimers (ScenarioData::timers @a049e0..): at most 100 named timers, each an expiry tick (Game::tick, seconds).
struct ScriptTimers {
	struct Timer {
		bhs::U16 name;
		int32_t expiry = 0;
	};
	std::vector<Timer> list;  // LinkList ordered by expiry (ordered_insert)
	int add(const bhs::U16& name, int32_t expiry);  // replaces a timer of the same name; 1, or -1 when full
	int check(const bhs::U16& name, int32_t now);   // -1 none, 1 (and removed) when now >= expiry, else 0
	int remove(const bhs::U16& name);               // 1 removed, -1 none
};

struct ScriptLoadReport {
	std::string scenario_script, general_powers_script;
	std::vector<std::string> production_scripts;  // file paths compiled for the AI leaders
	std::vector<std::string> errors;               // compile errors (the script is then not run)
};

class ScriptHost {
public:
	explicit ScriptHost(Game& g);
	~ScriptHost();
	ScriptHost(const ScriptHost&) = delete;
	ScriptHost& operator=(const ScriptHost&) = delete;

	Game& game;
	bhs::Runtime rt;
	const GameFiles* files = nullptr;

	// GameInfo.scenario_script and ScenarioData::general_powers_script: the names run every frame ("" = none).
	bhs::U16 scenario_script, general_powers_script;

	// ScenarioData script state (saved with ScenarioData, state-walk.md; save/load: TODO).
	ScriptTimers timers;
	std::array<int32_t, 31> find_counters{};  // ScenarioData::find_counters (the find_* builtins' resume slots)

	// Presentation state and the seat-local input of the builtins (sim/script_ui.hpp).
	ScriptPresentation pres;
	ScriptInput input;
	// GameInfo.scenario_dir: the scenario's game-relative directory with a trailing backslash (play_sound paths).
	std::string scenario_dir;
	// ScenarioData::sound (the scenario sound channel of play_sound / play_objective_sound): the file, the frame it
	// started and its length in ms from the WAV header (-1 unknown); `active` until stop_sound or a new sound.
	struct Sound {
		std::string name, path;
		int32_t start_frame = -1, ms = -1;
		bool active = false;
	} sound;
	// sound_finished's answer for the current sound (ScenarioData::sound vfunc +0x4c == 0): the renderer's report when
	// attached, else the WAV-length model at 15 frames per second.
	bool sound_done() const;
	// play_sound @a02f50 without the audio: resolves <scenario_dir><file>, records the sound, posts a kSound event.
	// Returns 1 when the file exists, -1 otherwise (the original's Sound open failure).
	int32_t play_sound(const bhs::U16& file);

	// The player-profile XML files of open_file / file_* (ScenarioData::xml, xml_root, xml_curr_tag, xml_parent_tag,
	// xml_elements, xml_curr_entry, xml_curr_index). Kept in memory for the session (the original reads and writes
	// <app directory>/<file> in the player profile; nothing is written to disk here).
	struct XmlNode {
		std::string name;
		std::vector<std::pair<bhs::U16, bhs::U16>> attribs;
		std::vector<std::shared_ptr<XmlNode>> children;
	};
	std::map<std::string, std::shared_ptr<XmlNode>> xml_files;
	// ConquestGame::stored_ints / stored_strs (NamedArray, exact-name lookup): get_/set_ctw_int, get_/set_ctw_str,
	// clear_ctw_val. They outlive a battle in the original (the campaign keeps them); here they live as long as the
	// host. TODO(ctw): move to the conquest state once the strategic layer is ported.
	std::map<bhs::U16, int32_t> ctw_ints;
	std::map<bhs::U16, bhs::U16> ctw_strs;
	// The Conquer the World campaign these scripts run for (sim/ctw.hpp): the strategic-layer hooks and the battle's
	// ctw_* builtins read / write it (script_funcs_ctw.cpp). Null for a battle started on its own (the stored values above).
	ctw::ConquestGame* ctw = nullptr;
	std::shared_ptr<XmlNode> xml_root, xml_curr, xml_parent;
	std::vector<std::shared_ptr<XmlNode>> xml_elements;
	bhs::U16 xml_curr_entry;
	int32_t xml_curr_index = 0;

	// Statistics for the tools.
	int64_t runs = 0, run_errors = 0;
	std::map<std::string, int> run_failures;  // "<script>: status" -> count
	std::vector<std::string> messages;         // run-time errors / script prints (capped)
	std::map<std::string, int> todo_calls;     // unported game functions reached from ported builtins
	bool verbose = false;

	// Compiles a .bhs file (game-relative or absolute path) and loads its images, unless that file is loaded already
	// (Compiler::compile skips unchanged files). Returns false with `err` on a compile error.
	bool compile(const std::string& path, std::string* err);
	// run_script(name, args) with the statistics; returns the Runtime status.
	int run(const bhs::U16& name, const std::vector<bhs::VP>& args);

	// Game::do_frame step 5-6: the scenario script, then the general powers script when frame > 0.
	void run_frame_scripts();

	// The unported builtins called so far (name -> calls), for reports.
	std::vector<std::pair<std::string, int>> unported() const;
};

// Setup::build_game's script part and Leaders::init_production_script: creates g.scripts and compiles
//   - the scenario script when the scenario asks for one (scenario_flags & 1 LOAD_SCRIPT): <scenario dir>/<base>.bhs,
//     run as the script named <base> (GameInfo::get_full_script_path; an unnamed `scenario { }` takes the file name);
//   - scenario/scriptlibrary/general_powers.bhs when g.scripted_rules or g.ctw;
//   - ai/scripts/<name>.bhs for every VALID leader 0..7 that is not a human seat without co-op AI and has a production
//     script name (Leader.prod_script).
// `scenario_path` is the scenario file's game-relative or absolute path. Call after Game::load.
ScriptLoadReport script_host_load(Game& g, const GameFiles& files, const std::string& scenario_path, const Scenario& s);

// Leader::production_ai / Leader::init's AI part: sim/ai.hpp.

// Registers the builtins (script_funcs.cpp).
void install_script_funcs(ScriptHost& h);

}  // namespace ron::sim
