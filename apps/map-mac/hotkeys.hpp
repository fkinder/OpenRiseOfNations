// Control groups through the command layer (replay-spec verb 34 HOTKEY: i32 group, i32 clear, i32 valid, f32 x, f32 y, i32 zoom;
// HotKeyGroups::copy_group). The verb lives in sim/commands_extra.cpp: `issue` sends the selection as GROUP and then the HOTKEY
// command through CommandManager (issue_hotkey / issue_hotkey_bookmark), so a control group is recorded, replayed and networked like
// every other order; the group table is Game::ctl.hotkeys (sim/control.hpp), 18 groups per leader (digits 0-9 -> 0..9, F1-F8 ->
// 10..17; 18..25 the named army groups of Game::ctl.named, docs/ui-port.md section 14). `slot` reads the table back for the HUD (recall,
// centre, add to the selection).
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "sim/commands.hpp"

namespace hotkeys {

using ObjectList = std::vector<std::pair<int32_t, int32_t>>;  // (leader, object)

// The payload of HOTKEY: group 0..25 of `play`; clear drops it; valid = the verb stores a camera bookmark (x, y, zoom) instead of the
// selection (clear == 1 and valid == 1), as CommandManager::issue_hotkey sends it.
constexpr int kNumSlots = 26;  // 18 of the original's table + 8 named army groups

struct Command {
	int play = 0;
	int group = 0, clear = 0, valid = 0;
	float x = 0, y = 0;
	int zoom = 0;
};

struct Slot {
	ObjectList units, builds;
	bool bookmark = false, used = false;
	float x = 0, y = 0;
	int zoom = 0;
};

class Groups {
public:
	// Makes the game whose table `slot` reads (call when a game starts and whenever it is replaced).
	void attach(const ron::sim::Game& g) { game_ = &g; }
	// Queues the command in `cm` (nothing is executed here: the caller runs CommandManager::process_turn, or the multiplayer sink hands
	// it to the session). Assign (clear == 0, valid == 1): the units (else the buildings) of the local leader become the group; a
	// bookmark (clear == 0, valid == 0, no selection) stores the camera x / y / zoom; clear erases. False for a bad group number.
	bool issue(ron::sim::CommandManager& cm, const ron::sim::Game& g, const Command& c, const ObjectList& units, const ObjectList& builds);
	// Kept for the callers that applied the queued records themselves: the verb now runs in CommandPackage, so there is nothing left.
	void process() {}
	// The group as the simulation holds it (members are not checked for life: the caller filters).
	Slot slot(int play, int group) const;
	Slot& mut_slot(int, int) { return scratch_; }  // a throw-away copy: the table is the simulation's
	void clear() { log_.clear(), game_ = nullptr; }
	// Everything issued, in order (the HOTKEY records a replay would carry).
	const std::vector<Command>& log() const { return log_; }

private:
	const ron::sim::Game* game_ = nullptr;
	Slot scratch_;
	std::vector<Command> log_;
};

}  // namespace hotkeys
