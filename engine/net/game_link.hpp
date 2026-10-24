// The binding of the lockstep layer (net/lockstep.hpp) to the engine's Game: SimHooks over sim::Game, and the conversion of
// the lobby's key/value settings to the engine's random-game setup (docs/multiplayer-spec.md sections 2.1, 8).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "net/lobby.hpp"
#include "net/lockstep.hpp"
#include "sim/commands.hpp"
#include "sim/game.hpp"
#include "sim/newgame.hpp"

namespace ron::net {

// Which commands the instance executes. The verbs the engine has a Command for are `all_engine`; `sav_compatible` leaves out the
// aircraft verbs (ATTACK_GROUND, PATROL, LAUNCH_PATROL, FLIGHT, RECALL, SCRAMBLE) and the console verbs (HOTKEY, COME_OUT, SPEED_*,
// CHEAT_*, CHAT, RENAME_CITY, PAUSE: sim/commands_extra.cpp) that apps/sav/main.cpp's --replay does not feed, so that
// `ron-net replay` gives the numbers of `ron-sav --replay` (`--all-verbs` runs them all).
struct ExecPolicy {
	std::array<bool, 128> allow{};
	bool execute = true;
	// RESIGN / QUIT / UNGRACEFUL_PLAYER_DROP act on the engine (Leader::defeat with reason 6 / 7, the AI takes the leader over for
	// REPLACE_WITH_AI). Off in `sav_compatible` (ron-sav does not run them).
	bool player_exit = false;
	static ExecPolicy all_engine();
	static ExecPolicy sav_compatible();
};

// How the 16 CHECK_SUMS values are computed.
enum class ChecksumMode {
	Original,  // a Game loaded from an original save: the 15 DataWalk parts (sim/datawalk_engine.hpp), as the original sends them
	Native,    // any Game: the CRC-32s of the sections of the engine's own save (sim/save.hpp) stand for the parts
};

class GameHooks : public SimHooks {
public:
	// `seat_who[slot]` = the leader (Player.who) of the seat. For Native checksums give the save's scenario text and seed.
	GameHooks(sim::Game& g, const std::array<int, 8>& seat_who, ChecksumMode mode, ExecPolicy policy = ExecPolicy::all_engine(),
	          const std::string& scenario_rel = std::string(), uint32_t seed = 0);

	int32_t frame() const override { return g_->frame; }
	void step_frame() override;
	bool checksums(uint32_t out[16]) override;
	void begin_package(int slot, uint32_t stamp) override;
	void execute(int slot, const uint8_t* cmd, size_t len) override;
	// OOS recovery: the native save of the game is the snapshot. `restore` hands the bytes to `on_restore`, which builds the Game
	// (sim::load_game_bytes) and calls `rebind` with it; without a callback the hooks cannot restore.
	bool game_over() const override { return g_->game_over; }   // TURN_DATA then carries bit 7
	bool snapshot(Bytes& out) override;
	bool restore(const Bytes& snap, std::string& err) override;
	std::function<bool(const Bytes& snap, std::string& err)> on_restore;
	// The application's frame wrapper (script input before, script output after); runs around Game::do_frame.
	std::function<void()> before_frame, after_frame;

	sim::Game& game() { return *g_; }
	void rebind(sim::Game& g) { g_ = &g; }
	// The scenario text and seed a native save records (they change when a recovery loads a snapshot of another game).
	void set_save_identity(const std::string& scenario_rel, uint32_t seed) { scenario_ = scenario_rel, seed_ = seed; }
	const std::string& scenario() const { return scenario_; }
	uint32_t seed() const { return seed_; }
	// What the instance did with the commands it was given.
	uint64_t commands_run = 0;
	std::map<int, uint64_t> unsupported;  // opcode -> count (verbs the engine cannot run, or the policy leaves out)
	int32_t first_unsupported_frame = -1;
	int first_unsupported_op = -1;
	std::string last_error;

private:
	sim::Game* g_;
	std::array<int, 8> who_;
	ChecksumMode mode_;
	ExecPolicy policy_;
	std::string scenario_;
	uint32_t seed_;
	sim::CommandPackage package_;
	void player_exit(int op, const uint8_t* cmd, size_t len, int sender);
};

// The lobby keys <-> RandomGameSetup (multiplayer-spec 2.1 names: map_style map_size game_seed teamstyle gamerules gamespeed
// pop_limit tech_cost rush_rules reveal_map victory elimination score_goal time_limit wonderwin popwin econwin starting_town
// starting_resources starting_technology ending_technology, and per slot n: slot_type_n tribe_n team_n diff_n; num_nations).
void setup_to_lobby(const sim::RandomGameSetup& s, Lobby& lobby);
bool lobby_to_setup(const Lobby& lobby, sim::RandomGameSetup& out, std::string& error);

// What the room adds to a game that the RandomGameSetup / scenario does not carry (multiplayer-spec 8.6): the seat plan START fixed, the
// players' names and the handicaps, all by game slot. Read from the lobby table at START, so every peer holds the same values.
struct MpSeats {
	SeatPlan plan;
	std::array<std::string, 8> name;
	std::array<int, 8> handicap{};
};
MpSeats lobby_mp_seats(const Lobby& lobby);
// Makes the Game a multiplayer game on this machine (Game::multiplayer = the semaphore bit the lobby game has: handicaps, the AI's own
// difficulty, the multiplayer capture rules), gives the human seats their player names and handicaps. `who_of[slot]` = the leader of a game
// slot. Called by every peer before the barrier, identically.
void apply_mp_seats(sim::Game& g, const std::array<int, 8>& who_of, const MpSeats& seats);

}  // namespace ron::net
