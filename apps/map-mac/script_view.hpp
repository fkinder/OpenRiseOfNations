// The renderer side of the scenario scripts (docs/ui-port.md "Script UI"): ronmap's consumer of
// sim::script_presentation (engine/sim/script_ui.hpp). It keeps the MessageWin / AnimNotice / Scene-ping state the
// original's presentation calls build up (messages, chat, feedback / instructions / scenario message, text bubbles,
// notices, pings, objective check boxes, popups, the goal dialog, the game-over line) and draws it through the UI layer
// with the original's fonts, plates, positions, colours and durations (ui-spec 6.6, 6.7; the MessageWin decompiles in
// re/decomp/ui_hud, ScreenArrowSpline / TextBubble / Scene::add_ping in re/decomp/scriptui_render). It never touches the
// simulation: sounds, music, camera moves, selection clears and pauses go back to main.mm through callbacks.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/iface_layout.hpp"
#include "hud.hpp"
#include "notices.hpp"
#include "sim/script_ui.hpp"
#include "ui.hpp"

namespace ron::sim {
class Game;
}

namespace scriptview {

// What the view needs from the renderer each frame.
struct Env {
	int ui_w = 0, ui_h = 0;
	int tick = 0;        // Game::graphic_tick: 66 ms wall-clock ticks (durations, blinks)
	double ms = 0;       // wall clock in ms (AnimNotice uses timeGetTime)
	int local = 0;       // the console leader
	const ron::sim::Game* game = nullptr;
	const hud::Hud* hud = nullptr;
	bool hud_visible = true;
	bool game_over_line = true;  // the "Victory!" / "Defeat!" line (hidden while the end-game window is up)
	bool score_window = false;  // the StatWin (docs/ui-port.md section 17.4) is part of this game's interface
	// World point (x, y; ground height added by the callee) -> UI pixels; false when it is behind the camera.
	std::function<bool(float wx, float wy, float& x, float& y)> project;
};

struct Callbacks {
	std::function<void(const std::string& abs_path)> play_voice;  // ScenarioData::sound
	std::function<void()> stop_voice;
	std::function<void(const std::string& mood)> music_mood;
	std::function<void(const std::string& abs_path)> play_effect;  // SoundGlobal one-shots (S_PING ...)
	std::function<void(int kind, int x, int y, int zoom)> camera;   // CameraRequest kinds
	std::function<void()> clear_selection;
	std::function<void(int who, int o)> select_object;  // select_object: the console's selection becomes that one object
	std::function<void(bool on)> pause;
	std::function<void(int option)> highlight_option;  // highlight_option(name): the OptionIndex, -1 unknown
	std::function<void(const std::string& file)> screenshot;
};

// The Options page's Message Duration (player_profile.prefs.message_length 0..10, default 5): the messages stay message_length * 45 ticks and
// the banners message_length * 400 + 500 ms.
void set_message_length(int n);

class View {
public:
	bool init(const ron::GameFiles& files, const ron::IfaceLayout& layout, std::string& error);

	// Consumes the events posted since the last call (ScriptEvent::seq cursor).
	void consume(const ron::sim::ScriptPresentation& p, const Env& env, const Callbacks& cb);
	// A line in the message window from the interface itself (save / load feedback), in the standard colour and duration.
	void say(const std::string& text, const Env& env);
	// Forgets everything shown for the previous game (messages, notices, bubbles, pings, objective marks, popups, the
	// game-over line) after a save is loaded, and makes the event cursor skip the queue the loaded game carries so its
	// old sounds and messages do not play again. The current instruction line is kept from the saved queue.
	void reset(const ron::sim::ScriptPresentation* p);
	// A message / banner the simulation caused (notices.hpp), with its minimap ping and sound.
	void post(const notices::Note& n, const Env& env, const Callbacks& cb);
	// Space (OPTION_NEXT_MESSAGE): the location of the next located message, newest first; false when there is none.
	bool next_event(float& x, float& y);
	// The minimap ping styles (default, attack, wonder, citylost, citywon, waypoint, resource) with their art paths.
	std::vector<hud::PingStyle>& ping_styles() { return ping_styles_; }
	// SoundGlobal::play's lookup: soundglobals.xml KEY (without the S_) -> a random entry's soundfiles.xml file (absolute path, "" if unknown).
	std::string sound_file(const std::string& key);
	// Game over from the simulation's event log (victory / defeat of the local leader): the AnimNotice, YOU_WIN /
	// YOU_LOSE and the game-over line.
	void game_over(bool won, const std::string& message, const Env& env, const Callbacks& cb);

	// Screen layers drawn after the HUD (MessageWin::on_redraw, AnimNotice::process_all, ScreenArrowSpline::draw_arrows,
	// the modal windows). `p` may be null (a scenario without scripts: only game-over and pings).
	void draw(ron::metal::UiRenderer& ui, const ron::sim::ScriptPresentation* p, const Env& env);

	// MessageWin::scenario_message_rect of the last draw (get_objectives_*): all zero when no objectives are shown.
	ron::sim::ScriptRect objectives_rect() const { return objectives_rect_; }
	const std::vector<hud::Ping>& pings() const { return pings_; }
	struct Announce {  // MessageWin::add_event's shrinking ANNOUNCE_CIRCLE (8 ticks) on the terrain
		float x = 0, y = 0;
		int tick = 0;
	};
	const std::vector<Announce>& announces() const { return announces_; }

	// Modal windows. The popup is answered by main (a nested event loop while the script waits in BasePop::exec).
	struct Popup {
		std::string text;
		std::vector<std::string> buttons;  // "OK" when the script gave none
		int hover = -1, pressed = -1;
	};
	bool popup_open() const { return popup_open_; }
	void open_popup(const ron::sim::ScriptEvent& e);
	void close_popup() { popup_open_ = false; }
	Popup& popup() { return popup_; }
	int popup_button_at(int x, int y) const;  // after a draw
	bool goal_dialog_open() const { return goals_open_; }
	void close_goal_dialog() { goals_open_ = false; }
	bool goal_dialog_close_at(int x, int y) const;
	bool modal() const { return popup_open_ || goals_open_; }
	bool game_is_over() const { return over_; }
	// The banners of the game (AnimNotice list) are dropped when the end-game window opens (EndGameWin::exec cleans them).
	void clear_notices() { notices_.clear(); }
	// ui_hide_score: Window::hide(stat_win).
	bool score_hidden() const { return score_hidden_; }
	void show_score() { score_hidden_ = false; }  // the Alt+Shift+1 key after a ui_hide_score

private:
	struct Message {
		std::string text, prefix;  // prefix: chat "Name (Nation): " in the sender's text colour
		uint32_t color = 0xFFB4B4B4, prefix_color = 0xFFFFFFFF;
		int start = 0, end = 0;
	};
	struct Bubble {
		std::string text;
		float x = 0, y = 0;
		int start = 0;
		int color_leader = 0;
	};
	struct Notice {
		std::string image, text;
		double start_ms = 0;
	};
	struct ObjectiveMark {
		std::string id, text;
		int frame = 0, done_tick = -1;
	};
	void add_message(const std::string& text, uint32_t color, int ticks, const Env& env, const std::string& prefix = {},
	                 uint32_t prefix_color = 0);
	std::vector<std::string> wrap(ron::metal::UiRenderer& ui, int font, const std::string& text, int width) const;
	void scroll_plate(ron::metal::UiRenderer& ui, ron::IfaceRect r, uint32_t tint) const;
	int draw_objectives(ron::metal::UiRenderer& ui, const ron::sim::ScriptPresentation& p, const Env& env, int x, int y);
	void draw_messages(ron::metal::UiRenderer& ui, const Env& env, int x, int y);
	void draw_top_centre(ron::metal::UiRenderer& ui, const ron::sim::ScriptPresentation* p, const Env& env);
	void draw_bubbles(ron::metal::UiRenderer& ui, const ron::sim::ScriptPresentation* p, const Env& env);
	void draw_notices(ron::metal::UiRenderer& ui, const Env& env);
	void draw_arrows(ron::metal::UiRenderer& ui, const ron::sim::ScriptPresentation& p, const Env& env);
	void draw_popup(ron::metal::UiRenderer& ui, const Env& env);
	void draw_goal_dialog(ron::metal::UiRenderer& ui, const Env& env);
	void draw_game_over(ron::metal::UiRenderer& ui, const Env& env);
	void draw_score(ron::metal::UiRenderer& ui, const Env& env);
	uint32_t neon(const Env& env, int who) const;
	uint32_t text_color(const Env& env, int who) const;

	const ron::GameFiles* files_ = nullptr;
	const ron::IfaceLayout* layout_ = nullptr;
	const ron::XmlElement *rm_el_ = nullptr, *msg_el_ = nullptr, *popup_el_ = nullptr;
	std::string arrow_tex_;
	std::vector<hud::PingStyle> ping_styles_;
	std::vector<std::pair<float, float>> events_;  // located MessageWin events (add_event), oldest first
	int next_event_ = -1;
	int arrow_head_px_ = 18;
	uint64_t last_seq_ = 0;
	std::vector<Message> messages_;
	std::string instructions_, feedback_;
	int feedback_start_ = -1000000;
	int scen_msg_start_ = -1;  // graphic tick print_msg was seen (MessageWin::scenario_clock)
	uint64_t scen_msg_seq_ = 0;
	std::vector<Bubble> bubbles_;
	std::vector<Notice> notices_;
	std::vector<hud::Ping> pings_;
	std::vector<Announce> announces_;
	std::vector<ObjectiveMark> marks_;
	ron::sim::ScriptRect objectives_rect_{0, 0, 0, 0};
	bool popup_open_ = false;
	Popup popup_;
	std::vector<ron::IfaceRect> popup_buttons_;
	bool goals_open_ = false;
	ron::IfaceRect goals_ok_;
	bool over_ = false, won_ = false;
	bool score_hidden_ = false;
	std::string over_text_;
	std::vector<std::pair<std::string, std::vector<std::string>>> globals_;  // soundglobals KEY -> sound tags
	std::vector<std::pair<std::string, std::string>> sound_files_;           // soundfiles tag -> file
	unsigned rng_ = 1;
};

}  // namespace scriptview
