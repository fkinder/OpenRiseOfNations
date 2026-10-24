// Reader of Rise of Nations replays (.rcx) and the codec of the command packages (docs/replay-spec.md sections 1-9, 14;
// docs/multiplayer-spec.md section 8). C++ twin of re/tools/rcxdump.mjs: the same container walk (gzip or plain, header,
// embedded save, random-game info, rules block, package records, the multiplayer XOR and padding, the stream located after
// an embedded save by the same scan), and the same verb table.
//
// Three layers:
//   - the verb table: names, fixed lengths, `replay_command_length` (walks a plain package without decoding it);
//   - the file: `read_replay` fills a `Replay` (header, save, random info, rules block, packages with their commands);
//   - the engine bridge: `replay_to_engine` decodes one command into the engine's `Command` (sim/commands.hpp) when the engine
//     has a verb for it, `engine_to_wire` writes one back. Verbs the engine has no Command for stay raw bytes
//     (`ReplayPackage::plain` + `ReplayCommand::at/len`).
//
// A package's `plain` bytes are what CommandPackage::process_all sees after the MP de-XOR: commands with the per-command
// padding garbage still in between (`ReplayCommand::pad`). `ReplayPackage::wire()` returns the commands back to back
// without the padding: the form the lockstep layer (engine/net) puts on the wire.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sim/commands.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- verbs (replay-spec section 9)

constexpr int kReplayVerbCount = 82;  // opcodes 0..81
namespace op {
constexpr uint8_t kGroup = 0, kMoveTo = 7, kCheckSums = 57, kChat = 68, kResign = 70, kQuit = 71, kCamera = 72, kLeaderOptions = 73,
                  kTurnData = 74, kPause = 76, kCannonTime = 77, kPlayerSpeed = 79, kUngracefulDrop = 80, kSplineVerb = 51,
                  kSpeedSet = 52, kSpeedUp = 53, kSpeedDown = 54, kHotkey = 34, kCheatViewAll = 59;
}  // namespace op

// "MOVE_TO"; nullptr for an opcode above 81.
const char* replay_verb_name(int opcode);
// Length in bytes of the command that starts at p (opcode included), 0 when `avail` is too short, the opcode is unknown or
// the variable part (GROUP count, SPLINE count, CHAT length) is out of range.
size_t replay_command_length(const uint8_t* p, size_t avail);
// True when the engine has a Command for this opcode (replay_to_engine / engine_to_wire handle it).
bool replay_has_engine_verb(int opcode);

// ---------------------------------------------------------------- file

enum ReplaySetupField : int {
	kSetTeamStyle, kSetMapStyle, kSetMapSize, kSetPlayers, kSetMaxObservers, kSetGameSpeed, kSetGameRules, kSetDifficulty,
	kSetStartingTown, kSetStartingResources, kSetStartingResources2, kSetTechCost, kSetRevealMap, kSetPopLimit, kSetRushRules,
	kSetCannonTimes, kSetStartingTechnology, kSetStartingTechnology2, kSetEndingTechnology, kSetElimination, kSetVictory,
	kSetWonderWin, kSetScoreGoal, kSetPopWin, kSetTimeLimit, kSetChairs, kSetEconWin, kSetScenarioType, kSetScriptType, kSetMods,
	kSetupFieldCount
};

struct ReplayPlayer {
	int slot = 0;
	uint16_t flags = 0;  // Player.flags; bit 0 = the slot is valid (the rest of the record exists only then)
	int32_t synced[12] = {};
	uint8_t tribe = 0, who = 0, handicap = 0, play = 0, pauses = 0, diff = 0;
	int8_t team = 0;
	std::u16string name;  // personal data: never print or copy it
	bool present() const { return (flags & 1) != 0; }
};

struct ReplayHeader {
	std::u16string version_string;
	uint32_t version = 0, seed = 0;
	int32_t checksum_deep = 0, checksum_window = 0, checksum_threshold = 0;
	uint32_t flags = 0;
	std::array<uint8_t, kSetupFieldCount> setup{};
	std::array<ReplayPlayer, 8> players{};
	uint64_t scenario_workshop_id = 0, mod_workshop_id = 0;
	std::u16string scenario_script, scenario_path, scenario_dir, mod_path;
	int32_t frame = 0, tick = 0, num_players = 0;  // Game[0x550, 0x6e4): frame, tick, num_players
	int32_t semaphore_bits = 0;
	std::vector<uint8_t> semaphore;  // byte0: 4 multiplayer, 8 recording, 0x10 playback; byte2: 2 Conquer the World
	int32_t graphic_tick = 0;
	std::u16string save_name;
	bool multiplayer = false, conquest = false;
	int setup_field(ReplaySetupField f) const { return setup[size_t(f)]; }
	int present_players() const {
		int n = 0;
		for (const ReplayPlayer& p : players) n += p.present();
		return n;
	}
};

struct ReplayRandomInfo {
	struct Entry {
		int8_t team = 0;
		int32_t leader_value = 0;  // leaders[who] + 0x0c
	};
	std::vector<Entry> entries;
	int32_t world30 = 0;
};

// One command inside a package.
struct ReplayCommand {
	uint8_t op = 0;
	uint8_t pad = 0;   // padding bytes that followed it (multiplayer, 0 or 1)
	uint16_t len = 0;  // command bytes, opcode included, padding excluded
	uint32_t at = 0;   // offset of the opcode in the package's `plain`
};

// One record (RecordGame::write_package).
struct ReplayPackage {
	int32_t frame = 0, play = 0, valid = 0;
	uint32_t stamp = 0;
	int16_t size = 0;
	uint64_t file_at = 0;
	std::vector<uint8_t> plain;  // payload after the de-XOR (multiplayer) with the padding bytes still in place
	std::vector<ReplayCommand> cmds;
	const uint8_t* data(const ReplayCommand& c) const { return plain.data() + c.at; }
	// The commands back to back without padding. `drop_automatic` leaves out CHECK_SUMS, TURN_DATA and CAMERA (the three
	// commands the lockstep layer writes itself, replay-spec section 9 "Automatic per-turn commands").
	std::vector<uint8_t> wire(bool drop_automatic = false) const;
};

struct Replay {
	std::string path;
	bool gzip = false;
	size_t decompressed_size = 0;
	ReplayHeader header;
	size_t header_end = 0;
	// Embedded save (header.save_name non-empty): `save_at` is the offset of its magic string, `save_size` the bytes up to the
	// random-game-info block. save_frame is the frame inside the save (the header's frame can differ, replay-spec section 4).
	bool has_save = false;
	size_t save_at = 0, save_size = 0;
	std::string save_magic;
	uint32_t save_version = 0;
	int32_t save_frame = -1;
	ReplayHeader save_info;  // the GameInfo at the start of the embedded save (setup, seed, players; frame / semaphore are not filled)
	bool has_save_info = false;
	ReplayRandomInfo random_info;
	bool has_rules = false;  // a rules block follows (no embedded save): the replay's own rules, replay-spec section 6
	size_t rules_at = 0, rules_size = 0;
	int rules_types = 0;
	size_t stream_at = 0;
	std::string located;  // "exact" (after the rules block) or "scan" (after an embedded save)
	std::vector<ReplayPackage> packages;
	bool truncated = false;  // the last record is cut short (a crashed game)
	size_t truncated_at = 0, truncated_have = 0, truncated_need = 0;
	std::vector<std::string> warnings;
	std::vector<uint8_t> data;  // the decompressed file

	// Bytes of the embedded save (what load_original_save takes): data[save_at, save_at + save_size).
	std::vector<uint8_t> save_bytes() const;
	// The first frame the game runs from: the embedded save's frame, else the header's.
	int32_t start_frame() const { return save_frame >= 0 ? save_frame : header.frame; }
	// Counts and totals the way `rcxdump --survey` reports them.
	size_t command_count() const;
	size_t check_sum_rows() const;
	int32_t last_frame() const { return packages.empty() ? header.frame : packages.back().frame; }
};

// Parses a replay. Returns false with `error` set when it cannot be decoded.
bool read_replay(const std::string& path, Replay& out, std::string& error);
bool read_replay_bytes(const std::vector<uint8_t>& file, Replay& out, std::string& error);

// gzip or plain to the plain bytes (a truncated gzip yields what could be inflated). `was_gzip` reports the container.
bool replay_gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool& was_gzip);

// CHECK_SUMS command (65 bytes) -> its 16 values (units .. script_run_time, all). False when `len` is not 65.
bool replay_check_sums(const uint8_t* cmd, size_t len, uint32_t out[16]);

// ---------------------------------------------------------------- engine bridge

// One command to the engine's Command. Returns false (and leaves `out` untouched) when the verb has no Command, or the bytes are
// shorter than the verb needs. The field mapping is the one apps/sav/main.cpp uses for the same verbs.
bool replay_to_engine(const uint8_t* cmd, size_t len, Command& out);
// The wire form of an engine Command appended to `out` (no padding, no obfuscation). False when the verb has none.
bool engine_to_wire(const Command& c, std::vector<uint8_t>& out);

// Little-endian append helpers for the commands the lockstep layer builds itself.
void wire_check_sums(std::vector<uint8_t>& out, const uint32_t sums[16]);
void wire_turn_data(std::vector<uint8_t>& out, uint16_t ping_time, uint16_t frame_average = 0, uint16_t wait_time = 0,
                    uint16_t game_lag = 0, uint16_t forced_loads = 0);
void wire_camera(std::vector<uint8_t>& out, uint8_t zoom, int32_t x, int32_t y);
void wire_pause(std::vector<uint8_t>& out, uint8_t state);
void wire_resign(std::vector<uint8_t>& out, int32_t play);
void wire_drop(std::vector<uint8_t>& out, uint8_t play, uint8_t option);

// The reader's package decode: a payload as recorded (obfuscated when `multiplayer`) -> `plain` (de-XORed, padding still in place) and its commands. False when it does not
// parse to the last byte.
bool replay_decode_package(const uint8_t* data, size_t n, uint32_t seed, bool multiplayer, std::vector<uint8_t>& plain, std::vector<ReplayCommand>& cmds);

// The multiplayer padding generator (CommandPackage::add_command): Random(seed).get(0, 2) per command, a fresh generator per package.
// Exposed for tests that re-obfuscate a package.
std::vector<uint8_t> replay_obfuscate(const std::vector<uint8_t>& wire, uint32_t seed);

}  // namespace ron::sim
