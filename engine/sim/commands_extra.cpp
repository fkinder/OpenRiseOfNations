// The console / turn-control verbs of CommandPackage and their issuers in CommandManager (docs/replay-spec.md section 9; decompiles in
// re/decomp/gaps/): HOTKEY (control groups), COME_OUT, SPEED_SET / SPEED_UP / SPEED_DOWN, PAUSE, CHAT, RENAME_CITY and the nine CHEAT_*.
// None of them is read by the simulation proper (sim/control.hpp), except COME_OUT, RENAME_CITY and the cheats, which change units, city
// names, stockpiles and tech bits the way the original's actions do.
#include "sim/commands.hpp"

#include <algorithm>
#include <cstring>

#include "sim/ai.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"
#include "sim/rules_xml.hpp"
#include "sim/script_funcs_internal.hpp"
#include "sim/search.hpp"

namespace ron::sim {

namespace {

constexpr int32_t kTypeScholars = 0x34, kTypeScholarsKorean = 0x35, kTypeUniversity = 0x1a4, kTechBits = 0x326;
constexpr size_t kMaxChatLines = 64;

bool valid_who(int32_t w) { return w >= 0 && w < kNumLeaders; }

// TurnControl::speed_set(0, n, 0) @9567d0 as the three process_speed_* call it (the multiplayer speed lock first).
void turn_speed_set(Game& g, int32_t n) {
	if (n < 0 || n > 4 || g.ctl.speed == n) return;
	if (g.multiplayer && (g.info.flags & 0x100)) return;  // GameInfo.flags & 0x100: speed locked in a multiplayer game
	g.ctl.speed = n;
	g.ctl.target_frame_time = kTurnTimings[n];  // TurnControl::set_new_target_time
	g.ctl.speed_changes++;
}

// HotKeyGroups::copy_group @715120 into a control group: owner, buildings flag, the members in order; the frame stamps it.
void hotkey_assign(Game& g, HotKeyGroup& hg, const Group& src) {
	hg.used = src.num > 0 ? 1 : 0;
	hg.who = src.who;
	hg.buildings = src.buildings;
	hg.stamp = g.frame;
	hg.list.assign(src.list.begin(), src.list.begin() + std::min<int32_t>(src.num, kGroupMax));
	hg.bookmark = 0;
}

void cheat_warning(Game& g, int32_t who) {
	if (valid_who(who)) g.ctl.accum_cheated[size_t(who)]++;  // Game::action_cheat_warning: the cheater is remembered (message / sound: UI)
}

std::string utf8_of(const std::u16string& s) { return to_utf8(s); }

}  // namespace

void hotkey_groups_kill(Game& g, int32_t who, int32_t o) {
	if (who < 0 || who >= 9) return;
	for (int n = 0; n < kHotKeyGroupsPerLeader; n++) {
		HotKeyGroup& hg = g.ctl.hotkeys[size_t(who * kHotKeyGroupsPerLeader + n)];
		if (!hg.used || hg.bookmark) continue;
		auto it = std::find(hg.list.begin(), hg.list.end(), int16_t(o));
		if (it == hg.list.end()) continue;
		hg.list.erase(it);
		if (hg.list.empty()) hg.used = 0;
	}
}

// Unit::action_come_out @5e20b0: the unit drops its orders and leaves the building / carrier it is in. A scholar leaving a University
// passes its animation state along the chain of the scholars still inside (the leader's flag 0x2000000 marks it).
void unit_action_come_out(Game& g, Unit& u) {
	clear_orders(g, u);
	int32_t cw = -1;
	const int32_t c = unit_get_inside(g, u, &cw);
	if (c >= 0 && (u.type == kTypeScholars || u.type == kTypeScholarsKorean) && cw == u.who) {
		const Object* C = g.object(cw, c);
		if (C && !C->is_unit && type_is(*C->ptype, kTypeUniversity, false)) {
			g.leaders[size_t(u.who)].flags |= 0x2000000u;
			if (!u.guys.empty()) {
				int32_t anim = u.guys[0].cur_anim;
				uint32_t time = u.guys[0].cur_time;
				const Unit* cur = &u;
				for (int32_t guard = 0; cur->inside_down >= 0 && guard < kUnitLimit; guard++) {
					Unit* n = g.unit(cur->inside_down_who, cur->inside_down);
					if (!n) break;
					if ((n->type == kTypeScholars || n->type == kTypeScholarsKorean) && !n->guys.empty()) {
						const int32_t a = n->guys[0].cur_anim;
						const uint32_t t = n->guys[0].cur_time;
						n->guys[0].cur_time = time;
						n->guys[0].cur_anim = int8_t(anim);
						anim = a;
						time = t;
					}
					cur = n;
				}
			}
		}
	}
	unit_come_out(g, u, 0);
}

// ---------------------------------------------------------------- the executor

bool command_process_extra(Game& g, CommandPackage& pk, const Command& c, int32_t play, int32_t play_who) {
	switch (c.verb) {
	case Verb::Hotkey: {  // process_hotkey @9474d0
		if (c.hk_group < 0 || c.hk_group >= kHotKeyGroupCount) return true;
		HotKeyGroup& hg = g.ctl.hotkeys[size_t(c.hk_group)];
		if (c.hk_clear == 0) {
			if (pk.group >= 0) hotkey_assign(g, hg, g.groups.g[size_t(pk.group)]);
		} else {
			hg = HotKeyGroup();
			hg.who = int8_t(c.hk_group / kHotKeyGroupsPerLeader);
			if (c.hk_valid != 0) {
				hg.used = 1;
				hg.bookmark = 1;
				hg.x = c.hk_x;
				hg.y = c.hk_y;
				hg.zoom = c.hk_zoom;
			}
		}
		return true;
	}
	case Verb::ComeOut: {  // process_come_out @9465d0: a live unit with the matching uid
		Object* ob = valid_who(c.who) && c.ox >= 0 && c.ox < kUnitLimit ? g.object(c.who, c.ox) : nullptr;
		if (ob && ob->alive() && ob->is_unit && ob->uid == c.uid) unit_action_come_out(g, static_cast<Unit&>(*ob));
		return true;
	}
	case Verb::SpeedUp:  // process_speed_up @9461a0: only below Fast
		if (g.ctl.speed < 3) turn_speed_set(g, g.ctl.speed + 1);
		return true;
	case Verb::SpeedDown:  // process_speed_down @946290
		if (g.ctl.speed != 0) turn_speed_set(g, g.ctl.speed - 1);
		return true;
	case Verb::SpeedSet:  // process_speed_set @946380
		turn_speed_set(g, c.speed);
		return true;
	case Verb::Pause: {  // process_pause @944160 -> TurnControl::process_pause(play, state)
		const bool want = c.set != 0;
		if (g.ctl.paused == want) return true;
		g.ctl.paused = want;
		if (want) g.ctl.pauses[size_t(play & 7)]++;
		return true;
	}
	case Verb::Chat: {  // process_chat @9454f0: the line reaches every leader in the recipient mask
		(void)play_who;
		ControlState::ChatLine line;
		line.frame = g.frame;
		line.from = play_who;
		line.bits = uint32_t(c.chat_bits);
		line.text = utf8_of(c.text);
		const int32_t me = g.local_play;
		const bool for_me = c.chat_bits == -1 || (valid_who(me) && ((uint32_t(c.chat_bits) >> me) & 1u));
		if (for_me && g.scripts && valid_who(me) && (g.leaders[size_t(me)].flags & leaderflag::kValid)) {
			ScriptEvent& e = sf::push_event(g, ScriptEvent::kChat);  // LeaderOut::display_chat
			e.text = line.text;
			e.who = play_who;
		}
		g.ctl.chat.push_back(std::move(line));
		if (g.ctl.chat.size() > kMaxChatLines) g.ctl.chat.erase(g.ctl.chat.begin());
		return true;
	}
	case Verb::RenameCity: {  // process_rename_city @944090: a city building (object flag 0x20) takes the name
		Object* ob = valid_who(c.who) && c.ox >= kBuildBase && c.ox < kBuildLimit ? g.object(c.who, c.ox) : nullptr;
		if (ob && ob->alive() && !ob->is_unit && (ob->flags & objflag::kCity)) {
			const Build& b = static_cast<const Build&>(*ob);
			if (b.city >= 0 && size_t(b.city) < g.cities[size_t(c.who)].size()) g.cities[size_t(c.who)][size_t(b.city)].name = utf8_of(c.text.substr(0, c.text.find(u'\0')));
		}
		return true;
	}
	case Verb::CheatViewAll:  // Game::action_cheat_view_all @592cd0: toggles the view-all bit (semaphore byte0 & 2)
		g.ctl.view_all = !g.ctl.view_all;
		cheat_warning(g, c.who);
		return true;
	case Verb::CheatGiveTechs:  // @593180: every tech bit set (806 of them), no gain_tech effects
		if (valid_who(c.who)) {
			Leader& l = g.leaders[size_t(c.who)];
			for (int32_t t = 0; t < kTechBits && t < kNumTypes; t++) l.tech[size_t(t >> 3)] |= uint8_t(1u << (t & 7));
			cheat_warning(g, c.who);
		}
		return true;
	case Verb::CheatZeroTechs:  // @593120
		if (valid_who(c.who)) {
			Leader& l = g.leaders[size_t(c.who)];
			for (int32_t t = 0; t < kTechBits && t < kNumTypes; t++) l.tech[size_t(t >> 3)] &= uint8_t(~(1u << (t & 7)));
			cheat_warning(g, c.who);
		}
		return true;
	case Verb::CheatIncreaseBuckets:  // @592ff0: +1000 of each of the six goods
		if (valid_who(c.who)) {
			for (int32_t& v : g.leaders[size_t(c.who)].bucket) v += 1000;
			cheat_warning(g, c.who);
		}
		return true;
	case Verb::CheatZeroBuckets:  // @592e70
		if (valid_who(c.who)) {
			for (int32_t& v : g.leaders[size_t(c.who)].bucket) v = 0;
			cheat_warning(g, c.who);
		}
		return true;
	case Verb::CheatInitUnit: {  // @592d90: one unit for `who`, or one near the point for every valid leader
		if (c.type < kUnits || c.type >= kEndUnits) return true;
		if (c.who >= 0) {
			if (valid_who(c.who)) {
				init_unit(g, c.who, c.type, c.x, c.y, -1, -1, -1);
				cheat_warning(g, c.who);
			}
			return true;
		}
		for (int32_t w = 0; w < kNumLeaders; w++) {
			if (!(g.leaders[size_t(w)].flags & leaderflag::kValid)) continue;
			int32_t nx = 0, ny = 0;
			if (find_nearby_spot(g, g.type(c.type), c.x, c.y, &nx, &ny, 0, 0xc00, 0, 0x55555555, kFilterNotMe, -1, -1, 0, 0, -1, 0, -1) == 0)
				init_unit(g, w, c.type, nx, ny, -1, -1, -1);
		}
		return true;
	}
	case Verb::CheatAiSpeedIncrease:  // @593100: one step, at most 10
		g.ai_speed = std::min(g.ai_speed + 1, 10);
		return true;
	case Verb::CheatAiSpeedNormal:  // @5930e0: single player only
		if (!g.multiplayer) g.ai_speed = 1;
		return true;
	case Verb::CheatAiToggle:  // @5930c0: single player only
		if (!g.multiplayer) g.ai_off = !g.ai_off;
		return true;
	default:
		return false;
	}
}

// ---------------------------------------------------------------- the issuers

bool CommandManager::issue_hotkey(const Game& g, const std::vector<int32_t>& objs, int32_t n, bool clear) {
	if (n < 0 || n >= kHotKeyGroupsPerLeader || play_who < 0 || play_who >= 9) return false;
	Command c;
	c.verb = Verb::Hotkey;
	c.hk_group = play_who * kHotKeyGroupsPerLeader + n;
	c.hk_x = c.hk_y = -1.0f;
	if (clear || objs.empty()) {
		c.hk_clear = 1;  // CommandManager::issue_hotkey(null group): erase
	} else if (!issue_group(g, objs)) {
		return false;
	}
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_hotkey_bookmark(int32_t n, float x, float y, int32_t zoom) {
	if (n < 0 || n >= kHotKeyGroupsPerLeader || play_who < 0 || play_who >= 9) return false;
	Command c;
	c.verb = Verb::Hotkey;
	c.hk_group = play_who * kHotKeyGroupsPerLeader + n;
	c.hk_clear = 1;
	c.hk_valid = 1;
	c.hk_x = x;
	c.hk_y = y;
	c.hk_zoom = zoom;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_come_out(const Game& g, int32_t who, int32_t o) {
	const Object* ob = valid_who(who) && o >= 0 ? g.object(who, o) : nullptr;
	if (!ob || !ob->alive() || !ob->is_unit) return false;
	Command c;
	c.verb = Verb::ComeOut;
	c.who = who;
	c.ox = o;
	c.uid = ob->uid;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_speed_set(int32_t speed) {
	if (speed < 0 || speed > 4) return false;
	Command c;
	c.verb = Verb::SpeedSet;
	c.speed = speed;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_speed_up() {
	Command c;
	c.verb = Verb::SpeedUp;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_speed_down() {
	Command c;
	c.verb = Verb::SpeedDown;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_chat(const std::u16string& text, int32_t bits, int32_t taunt, int32_t taunt_num) {
	size_t a = 0, b = text.size();
	while (a < b && text[a] == u' ') a++;  // trim_left / trim_right (' ')
	while (b > a && text[b - 1] == u' ') b--;
	if (a == b) return false;
	Command c;
	c.verb = Verb::Chat;
	c.text = text.substr(a, std::min<size_t>(b - a, 255));
	c.chat_bits = bits;
	c.chat_taunt = taunt;
	c.chat_taunt_num = taunt_num;
	local_.push_back(std::move(c));
	return true;
}

bool CommandManager::issue_rename_city(const Game& g, int32_t o, const std::u16string& name) {
	const Object* ob = g.object(play_who, o);
	if (!ob || !ob->alive() || ob->is_unit || !(ob->flags & objflag::kCity)) return false;
	Command c;
	c.verb = Verb::RenameCity;
	c.who = play_who;
	c.ox = o;
	c.text = name.substr(0, 20);  // the wire pads it with zeros to 22 units
	local_.push_back(std::move(c));
	return true;
}

bool CommandManager::issue_pause(bool state) {
	Command c;
	c.verb = Verb::Pause;
	c.set = state ? 1 : 0;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_cheat(Verb v, int32_t who) {
	switch (v) {
	case Verb::CheatViewAll: case Verb::CheatGiveTechs: case Verb::CheatZeroTechs: case Verb::CheatIncreaseBuckets:
	case Verb::CheatZeroBuckets: case Verb::CheatAiSpeedIncrease: case Verb::CheatAiSpeedNormal: case Verb::CheatAiToggle:
		break;
	default:
		return false;
	}
	Command c;
	c.verb = v;
	c.who = who < 0 ? play_who : who;
	local_.push_back(c);
	return true;
}

bool CommandManager::issue_cheat_init_unit(int32_t who, int32_t type, int32_t x, int32_t y) {
	Command c;
	c.verb = Verb::CheatInitUnit;
	c.who = who;
	c.type = type;
	c.x = x;
	c.y = y;
	local_.push_back(c);
	return true;
}

}  // namespace ron::sim
