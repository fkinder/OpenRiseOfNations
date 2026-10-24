#include "hotkeys.hpp"

#include "sim/game.hpp"

namespace hotkeys {

bool Groups::issue(ron::sim::CommandManager& cm, const ron::sim::Game& g, const Command& c, const ObjectList& units, const ObjectList& builds) {
	if (c.group < 0 || c.group >= kNumSlots) return false;
	attach(g);
	bool ok;
	if (c.clear && !c.valid) {
		ok = cm.issue_hotkey(g, {}, c.group, true);  // Ctrl+Shift+N: erase
	} else if (c.clear || (!c.valid && units.empty() && builds.empty())) {
		ok = cm.issue_hotkey_bookmark(c.group, c.x, c.y, c.zoom);  // an empty selection bookmarks the camera
	} else {
		std::vector<int32_t> objs;  // GROUP first (CommandManager::issue_group): the captains the group will hold, as slots of the local seat
		for (const auto& [who, o] : units.empty() ? builds : units) {
			if (who == cm.play_who) objs.push_back(o);
		}
		ok = !objs.empty() && cm.issue_hotkey(g, objs, c.group, false);
	}
	if (ok) log_.push_back(c);
	return ok;
}

Slot Groups::slot(int play, int group) const {
	Slot s;
	if (!game_ || play < 0 || play >= 9 || group < 0 || group >= kNumSlots) return s;
	const ron::sim::HotKeyGroup& h = group < ron::sim::kHotKeyGroupsPerLeader
	                                     ? game_->ctl.hotkeys[size_t(play * ron::sim::kHotKeyGroupsPerLeader + group)]
	                                     : game_->ctl.named[size_t(play * ron::sim::kNamedGroupsPerLeader + (group - ron::sim::kHotKeyGroupsPerLeader))];
	if (!h.used) return s;
	s.used = true;
	if (h.bookmark) {
		s.bookmark = true, s.x = h.x, s.y = h.y, s.zoom = h.zoom;
		return s;
	}
	ObjectList& dst = h.buildings ? s.builds : s.units;
	for (int16_t o : h.list) dst.push_back({int32_t(h.who), int32_t(o)});
	return s;
}

}  // namespace hotkeys
