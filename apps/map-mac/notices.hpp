// Game messages and notices the simulation causes (docs/ui-spec.md 6.7, docs/ui-port.md section 11): MessageWin::add_message lines,
// AnimNotice banners and minimap pings for city captures and losses, capital loss, attack warnings, completed buildings and
// wonders, finished research, new ages and diplomacy changes. The simulation logs only some of these as GameEvents (city and
// building captures, capital loss, defeat, victory); the rest is read from the per-frame view (own objects losing hit points,
// a building going active, a queued tech leaving the queue, a leader's age, the diplomacy table). The texts are the install's
// translated_strings.xml entries (the decompiled call sites address them by offset).
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/strings.hpp"
#include "sim/game.hpp"
#include "sim/view.hpp"

namespace notices {

struct Note {
	enum Kind { Message, Notice } kind = Message;
	std::string text;
	uint32_t argb = 0xFFB4B4B4;  // MessageWin's default LIGHT_GREY (180, 180, 180)
	int ticks = 0;               // message duration in 66 ms ticks, 0 = message_length * 45
	std::string image;           // Notice: the IFACEMAINBASE piece (TEX_ALERTS)
	bool located = false;
	float x = 0, y = 0;          // where it happened (minimap ping, Space jumps there)
	int ping = 0;                // minimap ping style: 0 default, 1 attack, 2 wonder, 3 citylost, 4 citywon
	std::string sound;           // soundglobals.xml key (S_CITY_BEING_ATTACKED, ...)
};

struct Env {
	const ron::sim::Game* game = nullptr;
	const ron::sim::GameView* view = nullptr;
	int me = 0;
	std::function<std::string(int type)> type_name;  // display name of a type
	std::function<std::string(int who)> nation;      // nation name of a leader
	std::function<std::string(int who)> leader;      // the leader's name (the nation when it has none)
};

class Notifier {
public:
	void init(const ron::StringTables* tables) { tables_ = tables; }
	// Forgets the previous view (a new game or a loaded save): the next update only records.
	void reset(const ron::sim::Game& g);
	// After a Game::do_frame: appends what happened since the last call, for the seat `env.me`.
	void update(const Env& env, std::vector<Note>& out);

private:
	struct Obj {
		int32_t hp = 0, type = 0;
		bool active = false, build = false, city = false, started = false;
		int32_t q_type = -1, q_percent = 0;
	};
	std::string loc(int index, const char* expect, const char* fallback) const;
	static std::string parse(std::string text, const std::vector<std::string>& args);
	std::string loc_or(int index, const char* expect, const char* fallback, const std::vector<std::string>& args) const {
		return parse(loc(index, expect, fallback), args);
	}

	const ron::StringTables* tables_ = nullptr;
	size_t events_seen_ = 0;
	bool have_prev_ = false;
	std::unordered_map<int64_t, Obj> prev_;
	std::unordered_map<int64_t, int> last_alert_;  // per object: the frame of its last attack warning
	int last_unit_alert_ = -100000, last_notice_ = -100000;
	std::vector<int> prev_age_;
	std::vector<int> prev_rel_;  // relation of leader pairs: 0 war, 1 peace, 2 allied
};

}  // namespace notices
