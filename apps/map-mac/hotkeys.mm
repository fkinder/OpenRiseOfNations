#include "hotkeys.hpp"

#include "sim/game.hpp"

namespace hotkeys {

bool Groups::issue(ron::sim::CommandManager& cm, const ron::sim::Game& g, const Command& c, const ObjectList& units, const ObjectList& builds) {
	if (c.group < 0 || c.group > 9) return false;
	if (c.valid && !c.clear) {
		// GROUP first (CommandManager::issue_group): the units the group will hold, as object slots of the local seat.
		std::vector<int32_t> objs;
		for (const auto& [who, o] : units) {
			if (who == cm.play_who) objs.push_back(o);
		}
		if (!objs.empty()) cm.issue_group(g, objs);
	}
	pending_.push_back({c, units, builds});
	log_.push_back(c);
	return true;
}

void Groups::process() {
	for (const Pending& p : pending_) {
		Slot& s = mut_slot(p.c.play, p.c.group);
		s = Slot{};
		if (p.c.clear) continue;
		s.used = true;
		if (p.c.valid) {
			s.units = p.units, s.builds = p.builds;
		} else {
			s.bookmark = true, s.x = p.c.x, s.y = p.c.y, s.zoom = p.c.zoom;
		}
	}
	pending_.clear();
}

}  // namespace hotkeys
