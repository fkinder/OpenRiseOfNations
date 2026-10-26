// Game messages and notices the simulation causes (docs/ui-spec.md 6.7, docs/ui-port.md section 11): MessageWin::add_message lines,
// AnimNotice banners and minimap pings for city captures and losses, capital loss, attack warnings, completed buildings and
// wonders, finished research, new ages, tribute and diplomacy changes. All of it comes from the simulation's event log
// (sim/game.hpp GameEvent kinds 1-26: 17-26 are the victory clocks and modes, docs/ui-port.md section 17), whose attack warnings are written by Object::do_damage's own stamps and throttles;
// only the "building completed" line is still read from the per-frame view (a building going active). The texts are the
// install's translated_strings.xml entries (the decompiled call sites address them by offset).
#pragma once

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/strings.hpp"
#include "sim/game.hpp"
#include "sim/view.hpp"

namespace notices {

struct Note {
	enum Kind { Message, Notice, Flash } kind = Message;  // Flash: the resource panel's red flash (`goods` bit k)
	int goods = 0;
	std::string text;
	uint32_t argb = 0xFFB4B4B4;  // MessageWin's default LIGHT_GREY (180, 180, 180)
	int ticks = 0;               // message duration in 66 ms ticks, 0 = message_length * 45
	std::string image;           // Notice: the IFACEMAINBASE piece (TEX_ALERTS)
	bool located = false;
	float x = 0, y = 0;          // where it happened (minimap ping, Space jumps there)
	int ping = 0;                // minimap ping style: 0 default, 1 attack, 2 wonder, 3 citylost, 4 citywon
	std::string sound;           // soundglobals.xml key (S_CITY_BEING_ATTACKED, ...)
};

// String::parse: "$NAMEn" is replaced by argument n (the digits end the placeholder).
std::string fill(std::string text, const std::vector<std::string>& args);
// The same for a template that mixes text and numbers: String::parse has overloads (String, String, ..., int, int, ...), and
// the digit of a placeholder counts within its kind: "$NUMBERn" takes numbers[n], every other placeholder strings[n]
// ("$PRESIDENTRICHARDNIXON0 now has $NUMBER0 net wonder points!" with ("Rome") and (3)).
std::string fill_typed(std::string text, const std::vector<std::string>& strings, const std::vector<int>& numbers);

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
		bool active = false, build = false;
		int32_t type = 0;
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
	std::array<std::array<int32_t, 6>, 8> prev_buckets_{};  // every leader's stockpile as the last update saw it (the tribute lines of Leader::defeat)
	bool have_buckets_ = false;
	int wonder_stop_frame_ = -1;                   // the victory-clock stop sound plays once per pass (frame)
	std::unordered_map<int64_t, int> last_alert_;  // per city: the frame of its last attack warning (the banner needs 400 quiet frames)
};

}  // namespace notices
