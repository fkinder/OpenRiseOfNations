// The end-game window (EndGameWin, interface.xml ENDGAMEWIN; docs/ui-port.md section 17): what Game::process_end_game shows when
// the game is decided (Game::victory_checked): the VICTORY! / DEFEAT! / ARMAGEDDON! banner, why the game ended in the install's own
// sentences (EndGameWin::get_type_string), the game time and the viewer's score, and the four buttons (Examine Map, Game Stats,
// View Playback, Quit). Game Stats is this port's post-game score table: the original's Game Stats button opens the Steam
// achievements list, which has no counterpart here; the table lists every leader with the terms of Leader::compute_score.
//
// `make_info` reads the finished game; `Screen` draws it through the UI layer and answers the buttons. main.mm owns the modal state
// (it reuses the game menu's hover / press / pause handling, kind 2).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "assets/iface_layout.hpp"
#include "assets/strings.hpp"
#include "sim/game.hpp"
#include "ui.hpp"

namespace endgame {

struct Row {
	int who = -1;
	std::string name;
	int color = 8;  // the leader's colour slot (team colours)
	std::string age;
	int units = 0, buildings = 0, economy = 0, research = 0, territory = 0, wonders = 0, score = 0;
	int outcome = 0;  // 0 still in the game, 1 won (Leader.flags 0x20), 2 lost (0x40)
	bool local = false;
};

struct Info {
	int mode = 0;  // EndGameWin::mode: 0 victory, 1 defeat, 2 Armageddon
	std::string title, message;
	int frames = 0;  // the game clock
	int score = 0;   // the viewer's score (Leader +0x18)
	bool ctw = false;
	std::string time_label, score_label;  // "Game Time: " / "Your Score: "
	std::string quit_caption;  // "Quit"; "Continue to Strategic Map" in a Conquer the World battle
	std::string kind;  // conquest, capital, sudden death, wonder, territory, tech, score, economic, time limit, chairs, resign, allied, armageddon
	std::vector<Row> rows;
};

struct Names {
	std::function<std::string(int who)> leader;  // the leader's name (the nation when it has none)
	std::function<std::string(int age)> age;     // the display name of an age count (type 543 + n)
};

// EndGameWin::exec's decisions (mode, victory / defeat type, instant flags, team game) and get_type_string's sentence.
// `victory_message` / `defeat_message` are the scenario's (set_victory_message / set_defeat_message); they replace the sentence.
Info make_info(const ron::sim::Game& g, int console, const ron::StringTables* tables, const Names& names, const std::string& victory_message,
               const std::string& defeat_message);

class Screen {
public:
	enum class Action { None, Examine, Stats, Playback, Quit, Continue, Close };
	bool init(const ron::IfaceLayout& layout);
	void open(Info info);
	const Info& info() const { return info_; }
	bool stats_view() const { return stats_; }
	void show_stats(bool on) { stats_ = on; }
	// The buttons now on screen (the window's four, or Close in the table), their captions and what a click does.
	std::vector<std::string> items() const;
	Action action(int i) const;
	bool enabled(int i) const;
	// Draws everything and fills `rects` with the buttons' rectangles in UI pixels.
	void draw(ron::metal::UiRenderer& ui, int ui_w, int ui_h, int hover, int press, std::vector<ron::IfaceRect>& rects) const;

private:
	void draw_window(ron::metal::UiRenderer& ui, int ui_w, int ui_h, int hover, int press, std::vector<ron::IfaceRect>& rects) const;
	void draw_stats(ron::metal::UiRenderer& ui, int ui_w, int ui_h, int hover, int press, std::vector<ron::IfaceRect>& rects) const;
	void button(ron::metal::UiRenderer& ui, const ron::IfaceRect& r, const std::string& caption, int state, bool enabled, int font) const;
	void scroll_plate(ron::metal::UiRenderer& ui, ron::IfaceRect r) const;

	const ron::XmlElement* win_ = nullptr;  // ENDGAMEWIN
	const ron::XmlElement* rm_ = nullptr;   // IFACERENDERMANAGER (the scroll pieces)
	Info info_;
	bool stats_ = false;
};

}  // namespace endgame
