// Control groups through the command layer (replay-spec verb 34 HOTKEY: i32 group, i32 clear, i32 valid, f32 x, f32 y, i32 zoom;
// HotKeyGroups::copy_group). CommandManager (sim/commands.*) has no HOTKEY verb yet, so the record lives here: `issue` sends the
// selection as a GROUP command through CommandManager::issue_group (what the original sends before it) and queues the HOTKEY
// record, `process` applies the queued records in issue order when the turn is processed (CommandManager::process_turn). Not yet
// in the recorded / replayed / networked command stream: that needs the verb in CommandPackage (the gap is listed in
// docs/ui-port.md 12.5).
#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "sim/commands.hpp"

namespace hotkeys {

using ObjectList = std::vector<std::pair<int32_t, int32_t>>;  // (leader, object)

// The payload of HOTKEY (25 bytes): group 0..9; clear drops it; valid = the group holds the selection, else it is a camera
// bookmark (x, y, zoom).
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
	// Queues `c` for the turn. For a valid group the selected units are first sent as GROUP (issue_group); `units` / `builds`
	// are what the group will hold. Returns false for an invalid group number.
	bool issue(ron::sim::CommandManager& cm, const ron::sim::Game& g, const Command& c, const ObjectList& units, const ObjectList& builds);
	// Applies the queued commands (CommandPackage::process for verb 34).
	void process();
	const Slot& slot(int play, int group) const { return slots_[size_t(play & 7)][size_t(group % 10)]; }
	Slot& mut_slot(int play, int group) { return slots_[size_t(play & 7)][size_t(group % 10)]; }
	void clear() {
		slots_ = {};
		pending_.clear();
		log_.clear();
	}
	// Everything issued, in order (the HOTKEY records a replay would carry).
	const std::vector<Command>& log() const { return log_; }

private:
	struct Pending {
		Command c;
		ObjectList units, builds;
	};
	std::array<std::array<Slot, 10>, 8> slots_{};
	std::vector<Pending> pending_;
	std::vector<Command> log_;
};

}  // namespace hotkeys
