// Conquer the World screens of the front end (docs/ui-port.md section 10): the campaign list and set-up, the strategic map
// (the campaign's art with the countries tinted by owner, link lines, army markers), the turn UI (move / attack, cards,
// diplomacy, news, end turn, save / load) and the hand-off to a battle in the game window.
//
// The campaign lives in a Session that outlives the front end: the front end is rebuilt around a battle (the menu's large
// textures are freed while a game runs) and the session returns the player to the map with the outcome applied.
#pragma once

#import <Metal/Metal.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/iface_layout.hpp"
#include "sim/ctw.hpp"
#include "sim/rules.hpp"
#include "ui.hpp"

namespace ctwui {

// One campaign in play. `game` is the strategic state; the rest is what the front end and the host share around a battle.
struct Session {
	std::shared_ptr<ron::GameFiles> files;
	std::shared_ptr<ron::sim::Rules> rules;
	ron::sim::ctw::ConquestGame game;
	// A battle waiting to be played (set by the map screen's Play button; consumed by the host).
	bool battle_pending = false;
	bool battle_is_defense = false;  // the AI attacked the player (resolve_player_defense afterwards) or the player attacked
	ron::sim::ctw::ConquestGame::BattleDef battle;
	// After a battle: the line the map shows (outcome) and whether the AI round must continue.
	std::string last_result;
	bool resume_ai = false;
	// The map screen's own state survives too (selection, camera): opaque to the host.
	std::shared_ptr<void> ui_state;
	std::string save_dir;  // where manual saves go
	std::string autosave_path() const;

	// Applies a finished battle: process_results / resolve_player_defense, then the autosave. `outcome`: 1 won, 0 lost, 2 retreated.
	void battle_finished(int outcome);
};

// Creates a session for a new campaign (setup of the campaign state; seed 0 = from the clock). Returns null with `err`.
std::shared_ptr<Session> new_session(const std::string& game_dir, const ron::sim::ctw::ConquestSetup& setup, std::string* err);
// Loads a saved campaign file. Returns null with `err`.
std::shared_ptr<Session> load_session(const std::string& game_dir, const std::string& path, std::string* err);
// The default save directory (~/Library/Application Support/OpenRiseOfNations/Conquer the World).
std::string default_save_dir();
// *.ctw files of the save directory, newest first.
std::vector<std::string> list_saves(const std::string& dir);

// What the screens need from the front end: drawing primitives with its skin, and the way out.
struct Bridge {
	ron::metal::UiRenderer* ui = nullptr;
	int ox = 0, oy = 0;          // the 1024 x 768 design canvas's origin in the window (UI pixels)
	int ui_w = 0, ui_h = 0;      // the window in UI pixels
	std::function<void(const ron::IfaceRect&, const std::string&, int state, int font)> button;  // window-space rect; state 0 normal, 1 hot, 2 pressed, 3 disabled
	std::function<void(const ron::IfaceRect&, uint32_t fill)> box;                              // thin-bordered dark box
	std::function<void(const ron::IfaceRect&)> frame;                                           // the bronze frame
	std::function<void(const ron::IfaceRect&, bool selected, bool hot)> row;                    // a list row highlight
	std::function<void(const std::string&)> banner;                                             // the title ribbon (design canvas)
	std::function<void()> click;
	id<MTLDevice> device = nil;
	id<MTLCommandQueue> queue = nil;
};

enum class Action { None, Back, PlayBattle, Quit };

class Screen {
public:
	Screen(std::shared_ptr<ron::GameFiles> files, std::string game_dir);
	~Screen();
	// The session (null until a campaign is started or loaded); `map` selects the map screen.
	void set_session(std::shared_ptr<Session> s);
	std::shared_ptr<Session> session() const;
	bool has_session() const;

	// "ctw" (campaign list / set-up) or "ctw-map". Sub-screens of the map for snapshots: "news", "cards", "diplomacy", "attack", "menu".
	void open(bool map_screen, const std::string& sub = std::string());
	bool on_map() const;
	// Frame: draws into `b.ui` (the caller has begun the frame and drawn the backdrop).
	void draw(Bridge& b);
	// Input in UI pixels of the window (top-left origin).
	void mouse_move(Bridge& b, float x, float y);
	void mouse_down(Bridge& b, float x, float y, int clicks, bool right);
	void mouse_drag(Bridge& b, float x, float y);
	void mouse_up(Bridge& b, float x, float y);
	void scroll(Bridge& b, float dy, float x, float y);
	void key(Bridge& b, const std::string& chars, int keycode, bool shift);
	// What the front end should do next (consumed by taking it).
	Action take_action();
	// Headless: sets the camera / selection used by snapshots.
	void select_node(int n);
	void select_country(const std::string& id);
	void set_zoom(float z);

	struct Impl;  // public name so the draw helpers of ctw_ui.mm can take it; the type is defined there

private:
	std::unique_ptr<Impl> p_;
};

}  // namespace ctwui
