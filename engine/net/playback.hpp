// Playing a recording (.rcx) back: the one implementation behind `ron-net replay` (the start game of a recording), `ron-net playback`,
// `ronmap --playback` and the app's Recorded Games viewer (docs/multiplayer-spec.md "Playback", docs/ui-port.md section 18).
//
// A recording is the setup of a game plus every executed command package with the frame it ran at (replay-spec sections 1-9). Three kinds:
//   Original      a recording of the original game: the embedded save of the original (sim/original_save.hpp), the game script the
//                 header names, the playback prologue of `ron-sav --replay`;
//   NativeSave    a recording of this port that starts from a native save (a game loaded from one, or the segment after a recovery);
//   NativeRandom  a recording of this port of a random game: the header's scenario_path holds the setup text, the room's names and
//                 handicaps come from the header.
//
// `Playback::make_start` builds the game at the start of the recording (for any kind; the lockstep runner of `ron-net replay` calls it once
// per human seat). The direct player then drives ONE such game: `bind` it, call `step()` (one game frame; the packages recorded at the
// frame the game is at run first, through GameHooks::execute, the same entry the lockstep session uses) until `at_end()`. Every package
// group is a turn boundary of the recorded game: before the group runs, the state's CHECK_SUMS are computed and compared with the ones the
// recording stored for that boundary (the row of the next package, replay-spec "Automatic per-turn commands"); `sync()` tells how the
// playback has agreed so far. Nothing here draws or sleeps.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "net/game_link.hpp"
#include "sim/anim_assets.hpp"
#include "sim/newgame.hpp"
#include "sim/replay.hpp"
#include "sim/rules.hpp"

namespace ron::net {

// What the Recorded Games list shows about a file, read from the header alone (no game is built).
struct PlaybackSummary {
	std::string path, file;
	bool ok = false;
	std::string error;
	bool original = false;        // recorded by the original game
	bool multiplayer = false;
	bool playable = false;        // this port can play it (not a single-player or Conquer the World recording, has a start state)
	std::string unplayable;       // why not
	int players = 0, humans = 0;  // seats in the header, human ones
	std::vector<int> tribes;      // the present seats' tribe indexes, in seat order
	std::vector<int> seat_who;    // the leader of each of those seats
	int map_style = -1, map_size = -1;
	int64_t start_frame = 0, end_frame = 0;   // the first frame the playback runs from, the frame of the last package
	bool has_check_sums = false;
	bool ctw_battle = false;      // a Conquer the World battle (the embedded save carries the campaign copy)
	bool recovery = false;        // starts from an out-of-sync recovery snapshot (start_frame > 0): the recording starts there, not at frame 0
};
bool playback_summary(const std::string& path, PlaybackSummary& out);

// How the playback agrees with the recording's CHECK_SUMS so far.
struct PlaybackSync {
	bool has_rows = false;                 // the recording carries CHECK_SUMS rows at all
	int64_t compared = 0;                  // boundaries compared
	int64_t equal = 0;                     // of them with all 15 parts equal
	int64_t first_out_of_sync = -1;        // frame of the first boundary where any part differs (-1: none yet)
	std::array<int64_t, 15> first_diff{};  // per part: frame of its first difference (-1 none)
	std::array<int64_t, 15> last_match{};  // per part: the last boundary frame it agreed at before its first difference
	int64_t last_frame = -1;               // the last boundary compared
	bool last_ok = true;                   // ... and whether it agreed
	bool in_sync() const { return first_out_of_sync < 0; }
};

// What a start game is made of. Null members: the playback's own (loaded from the install). Native kinds adopt the caller's files /
// rules / art (the app's renderer has built them already); the original kind always uses its own (the rules come from the save's setup).
struct PlaybackEnv {
	const ron::GameFiles* files = nullptr;
	const sim::Rules* rules = nullptr;
	const sim::AnimSource* art = nullptr;
};

class Playback {
public:
	enum class Kind { Original, NativeSave, NativeRandom };

	// Parses the recording. `game_dir` is the install the rules, art and scripts come from. False with `err` when it cannot be played.
	static std::unique_ptr<Playback> open(const std::string& path, const std::string& game_dir, std::string& err);

	Kind kind() const { return kind_; }
	const sim::Replay& replay() const { return rp_; }
	const std::string& path() const { return path_; }
	const ron::GameFiles& files() const { return files_; }
	// The scenario text and seed a native save of this game records ("random:..." or a scenario path); for NativeRandom the setup too.
	const std::string& scenario() const { return scenario_; }
	uint32_t seed() const { return seed_; }
	const sim::RandomGameSetup& random_setup() const { return setup_; }
	// The leader of each game slot (Player.who), as the recording's header names them.
	const std::array<int, 8>& seat_who() const { return who_; }
	bool has_check_sums() const { return !rows_.empty(); }

	// Where the recording starts and ends (frames). Valid after bind().
	int64_t start_frame() const { return start_; }
	int64_t end_frame() const { return groups_.empty() ? start_ : groups_.back().frame; }

	// Builds the game at the start of the recording, ready to bind (the prologue and the game script included). Callable any number of times.
	std::unique_ptr<sim::Game> make_start(std::string& err, const PlaybackEnv& env = PlaybackEnv(), bool with_art = true);
	// NativeRandom only: makes a fresh game built from random_setup() (by the caller, e.g. a renderer that built it already) the multiplayer game the
	// room started: the header's names and handicaps. make_start does this itself.
	void apply_header_seats(sim::Game& g);
	// Notes make_start left (the game script attached, rules adopted from the save ...), one line each, since the last call.
	std::vector<std::string> take_notes();
	bool rules_equal() const { return rules_equal_; }
	// The rules / art make_start built for a native kind when the caller adopted none (valid after the first make_start).
	const sim::Rules* own_rules() const { return own_rules_.get(); }
	const sim::AnimSource* own_art() const { return arts_.empty() ? nullptr : arts_.back().get(); }

	// ---- the direct player
	// Starts the schedule for `g` (a game from make_start, at the start). Creates the GameHooks (`policy_all_verbs`: the original kind runs
	// the verbs `ron-sav --replay` feeds only, unless this is set). The frame wrappers set before are installed.
	void bind(sim::Game& g, bool all_verbs = false);
	bool bound() const { return game_ != nullptr; }
	sim::Game& game() { return *game_; }
	GameHooks& hooks() { return *hooks_; }
	std::function<void()> before_frame, after_frame;   // the application's frame wrapper (script input before, output after)
	// Runs the packages due at the game's frame, then one game frame. False (nothing run) once the last package ran.
	bool step();
	bool at_end() const { return next_ >= groups_.size(); }
	// Steps until the game reaches `frame` (or the end). Returns the frames run.
	int64_t run_to(int64_t frame);
	int32_t frame() const { return game_ ? game_->frame : 0; }
	// Compare the boundaries' CHECK_SUMS (default on; they cost one hash of the state per turn).
	void set_checking(bool on) { checking_ = on; }
	const PlaybackSync& sync() const { return sync_; }
	// "in sync" / "out of sync at frame N" / "" (no check sums).
	std::string sync_text() const;
	uint32_t turns_run() const { return uint32_t(next_); }
	uint32_t turns_total() const { return uint32_t(groups_.size()); }
	// The command packages run so far / in all (records).
	uint64_t commands_run() const { return hooks_ ? hooks_->commands_run : 0; }

	// Everything the original's recording says about who plays (header players): name text is never exposed here.
	struct SeatInfo {
		int slot = 0, who = 0, tribe = 0, team = 0;
		bool human = false;
	};
	std::vector<SeatInfo> seats() const;

private:
	explicit Playback(const std::string& dir) : game_dir_(dir), files_(dir) {}
	struct Group {
		int64_t frame = 0;
		size_t first = 0, last = 0;   // package index range [first, last)
	};
	struct RulesCache;
	std::string path_, game_dir_;
	ron::GameFiles files_;
	sim::Replay rp_;
	Kind kind_ = Kind::Original;
	std::string scenario_;
	uint32_t seed_ = 0;
	sim::RandomGameSetup setup_;
	std::array<int, 8> who_{};
	std::vector<Group> groups_;
	std::map<int64_t, std::vector<uint32_t>> rows_;   // record frame -> the CHECK_SUMS tuple most senders agree on
	std::map<int64_t, std::vector<uint32_t>> expect_;  // boundary frame -> expected parts (filled by bind)
	std::unique_ptr<sim::Rules> own_rules_;            // native kinds without an adopted rules
	std::shared_ptr<RulesCache> cache_;
	std::vector<std::unique_ptr<sim::AssetAnimSource>> arts_;
	std::vector<std::string> notes_;
	bool rules_equal_ = false;
	sim::Game* game_ = nullptr;
	std::unique_ptr<GameHooks> hooks_;
	size_t next_ = 0;
	int64_t start_ = 0;
	bool checking_ = true;
	PlaybackSync sync_;

	void deliver();
	void compare(int64_t frame);
};

// The scenario-shaped terrain the renderer draws a loaded game from: Scenario::terrain synthesised from the game's World (tdata / wdata),
// its height field, its placed mountains and the tileset the original save names (OriginalShadow::tileset), the same record a random
// game's generator hands the renderer (sim/newgame.hpp RandomExtras::scenario). Used for the recordings of the original game, whose map
// is no scenario file and no setup text. Objects are not part of it (the renderer draws units and buildings from the simulation's view).
// False with `err` when the game has no tileset (not a game loaded from an original save).
bool playback_scenario(const sim::Game& g, const ron::GameFiles& files, Scenario& out, std::string& err);

// Runs a recording to `frames` after its start (0: to its end) with no display and prints the report `ron-net playback` and
// `ronmap --playback` share (kind, turns compared, per-part first differences). Returns 0 when it ran to where it was asked.
int playback_report(Playback& pb, sim::Game& g, int64_t frames, bool all_verbs);
// The part of the report about the check sums: how many boundaries were compared, whether they agreed, each part's first difference.
void playback_print_sync(Playback& pb);

}  // namespace ron::net
