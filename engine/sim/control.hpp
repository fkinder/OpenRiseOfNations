// Console / TurnControl state that player commands write (docs/replay-spec.md section 9, 10; sim-implementation.md "Orders and
// commands, second pass"): the game speed and pause of TurnControl, the control groups of HotKeyGroups, the cheat counters. None of
// it feeds the simulation (units, objects and the random stream never read it), so it is neither in the checksums nor in the native
// save (the original saves the hotkey groups: docs/state-spec.md; the port's save leaves them out, see the sim-implementation.md gap).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ron::sim {

// TurnControl::timings @afc4a4: milliseconds per simulation frame by game speed (Very Slow, Slow, Normal, Fast, Hyper Fast).
constexpr int32_t kTurnTimings[5] = {200, 125, 67, 50, 1};

// One control group (HotKeyGroup, 0x9fc bytes: a Group plus the camera bookmark x / y / zoom at +0x9d0 and the flag +0x9d8). The port
// keeps what a control group is used for: the owner, the member object indices (units or buildings) and the bookmark.
struct HotKeyGroup {
	int32_t used = 0;            // a Group copy (num > 0) or a bookmark is stored
	int8_t who = 0, buildings = 0;
	int32_t stamp = 0;           // the frame it was assigned (Group +0x14)
	std::vector<int16_t> list;   // member object indices, in group order
	int32_t bookmark = 0;        // +0x9d8: a camera bookmark instead of units
	float x = 0.0f, y = 0.0f;    // +0x9d0 / +0x9d4
	int32_t zoom = 0;            // +0x9dc
};

constexpr int kHotKeyGroupsPerLeader = 18, kHotKeyGroupCount = 162;  // hotkey_groups: 9 leaders x 18 (digits 0-9, F1-F8)

struct ControlState {
	// TurnControl: speed (0..4) and the frame time it sets (set_new_target_time), the pause bit (semaphore byte0 & 1) and the pauses
	// each seat used. `speed_changes` / `last_speed_by` let a front end announce a change once.
	int32_t speed = 2, target_frame_time = 67;
	bool paused = false;
	std::array<int32_t, 8> pauses{};
	int32_t speed_changes = 0;
	// HotKeyGroups: index = leader * 18 + n.
	std::array<HotKeyGroup, kHotKeyGroupCount> hotkeys{};
	// Game::action_cheat_*: info.player[who].accum_cheated, and the "view all" toggle (semaphore byte0 & 2).
	std::array<int32_t, 10> accum_cheated{};
	bool view_all = false;
	// The newest chat lines that reached the console (CHAT): sender leader, recipient mask, frame, text (UTF-8).
	struct ChatLine {
		int32_t frame = 0, from = -1;
		uint32_t bits = 0xffffffffu;
		std::string text;
	};
	std::vector<ChatLine> chat;  // newest last, at most 64
};

class Game;
// HotKeyGroups::kill @714e30 (from Unit::close / Build::close): the object leaves every control group of its leader.
void hotkey_groups_kill(Game& g, int32_t who, int32_t o);

}  // namespace ron::sim
