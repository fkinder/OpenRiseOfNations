// The in-game HUD of ronmap (docs/ui-spec.md section 6, port plan section 8): the IFACEMAINBASE bar with its civ
// skin, the top-of-screen strip, the resource panel (IFACERESOURCES), the selection panel (IFACESELECTED), the command
// card (IFACEOPTIONS), the minimap (IFACEMINIMAP + WorldMap). Positions come from interface.xml through
// engine/assets/iface_layout; drawing goes through ron::metal::UiRenderer. The HUD only reads a State snapshot that
// main.mm fills from the simulation's views; it never touches the simulation.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "assets/iface_layout.hpp"
#include "ui.hpp"

namespace hud {

// A building's production queue entry (BuildView::queue).
struct QueueItem {
	int type = -1;
	int percent = 0;  // progress of the item in production
	std::string name, tex_id;
	int tex_col = 0, tex_row = 0, tex_x = -1, tex_y = -1;
};

// One selected object (a unit or a building).
struct Object {
	bool building = false;
	int32_t who = 0, o = 0, type = 0;
	int hp = 0, max_hp = 0;
	uint8_t rgb[3] = {255, 255, 255};  // owner's team UNIT colour
	std::string name;
	std::string tex_id;  // icon atlas (TEX_UNITS_n / TEX_BUILDINGS_n), "" for none
	int tex_col = 0, tex_row = 0, tex_x = -1, tex_y = -1;
	bool own = false;  // owned by the viewing player
	int attack = -1, armor = -1, range = -1;  // stat values to show (-1: not shown)
	// Buildings: construction progress of an unfinished site (0..100, -1 none), the production queue ([0] in progress).
	int progress = -1;
	bool building_site = false;
	std::vector<QueueItem> queue;
	bool repeat = false;
};

// Six goods in IFaceMainBase::good_order: food, timber, metal, oil, wealth, knowledge.
struct Resources {
	bool live = false;  // false until the production / economy view API exists (values are placeholders)
	std::array<int, 6> stock{}, rate{};
	int pop = 0, pop_cap = 0, cities = 0, city_limit = 0, commerce_cap = 0;
	int age = 0;
	// Market (IFaceResources::draw_market_area): prices in wealth per lot of 100 (buy / sell) by good, enabled with a market and
	// the trade right. Infrastructure (draw_infra): workers / limit by good. over_cap: 0 under, 1 capped, 2 at the ceiling.
	std::array<int, 6> buy{}, sell{}, workers{}, limit{}, over_cap{};
	bool can_trade = false;
};

// A command-card entry (Options: option, object, grid slot).
struct Option {
	int option = -1;    // OptionIndex
	int object = -1;    // a TypeIndex for TRAIN / RESEARCH, else -1
	int grid_x = -1, grid_y = -1;
	std::string tex_id;
	int tex_col = 0, tex_row = 0, tex_x = -1, tex_y = -1;
	bool enabled = true;
	bool latched = false;  // the targeting mode this option started is active
};

struct Blip {
	float x = 0, y = 0;  // world units
	uint8_t rgb[3] = {255, 255, 255};
	int size = 2;
	bool building = false;
};

// EFFECTS/MINIMAP/PING of effects_graphics.xml (WorldMap::init_texture_pings): a style is a list of PHASEs, each active while
// end_pct <= p <= start_pct of the 8-tick cycle (p = (8 - t mod 8) / 8), drawing a 64 x 64 alpha mask.
struct PingPhase {
	float start = 1, end = 0;
	uint32_t color = 0xFFFFFFFF;  // 0 = the event's colour
	bool usealpha = true, usescale = false;
	float scaleby = 0, scalespeed = 0;
	std::string file, tex_id;  // the art path and its UiRenderer id (resolved by the caller)
};
struct PingStyle {
	std::string name;
	bool rotates = false;
	float rotate_speed = 0;
	std::vector<PingPhase> phases;
};

// A minimap ping (Scene::add_ping / MessageWin::add_event, ui-spec 6.6): world point, the colour, the graphic tick it
// started; the style's phases (`flash`) and, for a player's PING, the owner-coloured square.
struct Ping {
	float x = 0, y = 0;
	uint8_t rgb[3] = {255, 255, 255};
	int start_tick = 0, end_tick = 0;
	bool flash = true, square = true;
	int style = 0;  // index into State::ping_styles
};

// A victory / elimination clock (IFaceMainBase::draw_game_timer, ui-spec 6.1): kind 0 wonder, 1 capital, 2 armageddon,
// 3 territory, 4 peace, 5 time limit, 7 musical chairs; `frames` left (900 per minute); `who` the team colour of the owner
// (-1 none); `count` the wonder points / armageddon count (-1 none).
struct Timer {
	int kind = 0, frames = 0, who = -1, count = -1;
};

// IFaceMiniLibrary: the next research of each library line, or the next age, with its progress (-1: not in production).
struct MiniLibButton {
	bool valid = false, enabled = false;
	std::string tex_id;
	int tex_col = 0, tex_row = 0, tex_x = -1, tex_y = -1;
	int percent = -1;
};
struct MiniLib {
	MiniLibButton cat[4], age;  // military, civic, commerce, science
	int epoch[4] = {0, 0, 0, 0};
};

struct State {
	float world_w = 1, world_h = 1;  // world units
	// Fog levels for the viewer (0..3) per fog cell; empty = all visible.
	const std::vector<uint8_t>* fog = nullptr;
	int fog_xs = 0, fog_ys = 0;
	std::vector<Blip> blips;
	float view_corners[4] = {0, 0, 0, 0};  // the main view's screen corners (0,0) and (w,h) on z = 0: x0, y0, x1, y1
	bool has_minimap_bg = false;           // the "MINIMAP_BG" texture is registered with the UiRenderer
	std::vector<Object> selection;
	std::vector<Option> options;
	Resources res;
	std::string caption;  // nation name and age
	int frame = 0;        // simulation frame (the clock: 900 frames per minute... 15 per second)
	bool paused = false;
	int graphic_tick = 0;  // 66 ms wall-clock ticks (blinks)
	// Scenario highlight (ScenarioData hilite_option / hilite_object, IFaceButton::on_redraw): a command-card button whose
	// option (plain option buttons) or object (train / research / build buttons) matches gets the rotating
	// BUTTONHIGHLIGHT circle (GraphicPieces::draw_highlight_circle) drawn by the HUD; `highlight_tex` is its UiRenderer id.
	bool highlight = false;
	int hilite_option = -1, hilite_object = -1;
	std::string highlight_tex;
	std::vector<Ping> pings;
	std::vector<PingStyle> ping_styles;
	bool has_territory = false;  // "MINIMAP_TERR" is registered (the border colours, fog resolution)
	std::vector<Timer> timers;
	MiniLib minilib;
	int rare_columns = 0;  // columns of rare goods owned (MessageWin shifts right by 50 px per column)
};

// What the mouse is over, for the mouse-over help (MouseOver: option, spots, buttons).
enum class HoverKind { None, Option, SelObject, SelQueue, SelStat, SelHitPoints, Good, CommerceCap, PopCap, CityCap, MarketBuy,
	                   MarketSell, ToggleMarket, ToggleResources, ToggleInfra, Infra, Caption, BarButton, MinimapButton, Display,
	                   MiniLibCat, MiniLibToggle, Timer, Clock, Minimap };
struct Hover {
	HoverKind kind = HoverKind::None;
	int a = -1, b = -1;       // Option: option, object. Good / Market / Infra: the good. BarButton: its index. SelStat: 0 attack 1 armor 2 range.
	                          // SelObject / SelQueue: the index. MinimapButton / MiniLibCat: the button index. Timer: the kind.
	bool operator==(const Hover& o) const { return kind == o.kind && a == o.a && b == o.b; }
	bool operator!=(const Hover& o) const { return !(*this == o); }
};

enum class ActionKind { None, Option, MinimapLook, MinimapOrder, SelectIndex, QueueItem, Consumed, Buy, Sell, BarButton, MinimapButton, MiniLib };
struct Action {
	ActionKind kind = ActionKind::None;
	int option = -1, object = -1, index = -1;
	float wx = 0, wy = 0;
};

class Hud {
public:
	bool init(const ron::GameFiles& files, const ron::IfaceLayout& layout, std::string& error);
	// Resolves every panel against a ui_w x ui_h screen (called by draw; call it before hit tests on resize).
	void layout(int ui_w, int ui_h);
	void draw(ron::metal::UiRenderer& ui, const State& s);

	// True if (x, y) (UI pixels) is over a HUD window that takes the mouse; map clicks there must not reach the map.
	bool captures(int x, int y) const;
	Action mouse_down(int x, int y, bool right, const State& s);
	Action mouse_drag(int x, int y, const State& s);
	Action mouse_up(int x, int y, bool right, const State& s);
	void mouse_move(int x, int y) { hover_x_ = x, hover_y_ = y; }
	// Streamlined (collapsed) mode: the display button toggles it (OPTION_TOGGLE_IFACE_MAIN). opened: the full bar.
	bool opened() const { return opened_; }
	void set_opened(bool on);
	// IFaceResources flags: bit 0 market, bit 1 resources, bit 2 infrastructure (OPTION_TOGGLE_IFACE_*).
	int panel_flags() const { return res_flags_; }
	void set_panel_flags(int f);
	void toggle_panel(int bit);
	bool minilib_open() const { return minilib_open_; }
	void toggle_minilib() { minilib_open_ = !minilib_open_; }
	// Where MessageWin's feedback / instructions line sits: top 45, or 100 with the mini library expanded.
	int feedback_top() const { return minilib_open_ ? 100 : 45; }
	// The tooltip target under (x, y) (None outside the interface) and a tick counter that restarts when it changes.
	Hover hover_at(int x, int y, const State& s) const;
	// The 9-slice scroll plate (IFaceRenderManager::draw_scroll) tinted `tint`.
	void scroll_plate(ron::metal::UiRenderer& ui, ron::IfaceRect r, uint32_t tint) const;
	int bar_top() const { return base_.t; }
	// The resource panel's bottom edge (screen), MessageWin's anchor (resource panel height + 20).
	int resource_bottom() const { return res_win_.b; }
	// IFaceMain's rectangles by lower-case name (GraphicWinXML::find_rect after client_to_screen): every IFACERECT of
	// IFACEMAINBASE (left_window, mid_window, right_window, ...) plus "minimap" (the map) and the card slots
	// ("butt_XY"). For ScenarioFuncSet::get_iface_rect (get_button_*).
	std::vector<std::pair<std::string, ron::IfaceRect>> named_rects() const;
	// option_info: the OptionIndex whose ENGLISH_NAME matches (case-insensitive), -1 none (highlight_option).
	int option_index(const std::string& english_name) const;
	// options.xml: the NAME and DESC of an OptionIndex ("" when unknown).
	std::string option_name(int option) const { return option >= 0 && size_t(option) < option_info_.size() ? option_info_[size_t(option)].name : std::string(); }
	std::string option_desc(int option) const { return option >= 0 && size_t(option) < option_info_.size() ? option_info_[size_t(option)].desc : std::string(); }
	// The piece of IFACEMAINBASE named `name` (AnimNotice icons from TEX_ALERTS), invalid when absent.
	ron::IfacePiece base_piece(const std::string& name) const;

	// World <-> minimap (UI pixels), the closed form of WorldMap::init_camera (2:1 diamond, 45 degree yaw).
	bool minimap_to_world(int x, int y, const State& s, float& wx, float& wy) const;
	void world_to_minimap(float wx, float wy, const State& s, float& x, float& y) const;

private:
	struct OptionInfo {
		std::string tex_id, name, english, desc;
		int col = 0, row = 0;
	};
	void draw_bar(ron::metal::UiRenderer& ui, const State& s);
	void draw_top_strip(ron::metal::UiRenderer& ui, const State& s);
	void draw_resources(ron::metal::UiRenderer& ui, const State& s);
	void draw_minimap(ron::metal::UiRenderer& ui, const State& s);
	void draw_selected(ron::metal::UiRenderer& ui, const State& s);
	void draw_options(ron::metal::UiRenderer& ui, const State& s);
	void draw_market(ron::metal::UiRenderer& ui, const State& s);
	void draw_infra(ron::metal::UiRenderer& ui, const State& s);
	void draw_minilib(ron::metal::UiRenderer& ui, const State& s);
	void draw_timers(ron::metal::UiRenderer& ui, const State& s);
	void draw_icon_button(ron::metal::UiRenderer& ui, const std::string& tex_id, int col, int row, int tx, int ty, const ron::IfaceRect& r,
	                      int factor, bool small);
	void draw_health(ron::metal::UiRenderer& ui, const ron::IfaceRect& r, int hp, int max_hp, bool small);
	void hdivider(ron::metal::UiRenderer& ui, int x, int y, int w, bool thick);
	void vdivider(ron::metal::UiRenderer& ui, int x, int y, int h, bool thick);
	int card_slot_at(int x, int y) const;
	int multi_index_at(int x, int y, const State& s) const;
	int queue_index_at(int x, int y, const State& s) const;

	const ron::IfaceLayout* layout_ = nullptr;
	const ron::XmlElement *base_el_ = nullptr, *res_el_ = nullptr, *sel_el_ = nullptr, *opt_el_ = nullptr, *mini_el_ = nullptr,
	                      *rm_el_ = nullptr, *btn_el_ = nullptr;
	std::vector<OptionInfo> option_info_;  // options.xml, by OptionIndex
	int ui_w_ = 0, ui_h_ = 0;
	// Screen rects (UI pixels).
	ron::IfaceRect base_, left_, mid_, right_, minimap_win_, map_, sel_win_, opt_win_, res_win_, display_btn_;
	ron::IfaceRect card_[5][4];
	std::vector<ron::IfaceRect> bar_buttons_;  // pause, advanced, ping, diplo, chat, stats (+ help, menu)
	// Resource panel rects (window space).
	int res_flags_ = 2;  // bit 0 market open, bit 1 resources open, bit 2 infra open (market and infra collapsed)
	ron::IfaceRect market_rect_, resource_rect_, infra_rect_;
	bool opened_ = true, minilib_open_ = true, has_sel_ = false;
	ron::IfaceRect minilib_win_, toggle_rects_[4], minilib_buttons_[5], market_buttons_[2][4], infra_cells_[6];  // screen rects (set by draw)
	const ron::XmlElement* ml_el_ = nullptr;
	int pressed_market_ = -1;
	int hover_x_ = -1, hover_y_ = -1;
	bool minimap_drag_ = false;
	int pressed_slot_ = -1;
};

// OptionIndex values (rise.pdb enum; re/analysis/types_ui.txt) used by the card.
enum : int {
	kOptBuild = 0, kOptBuildMilitary = 1, kOptBuildWonder = 2, kOptRepair = 3, kOptPatrol = 8, kOptMoveTo = 9, kOptAttackTo = 10,
	kOptDisband = 15, kOptStance = 17, kOptForm = 19, kOptHalt = 25, kOptGarrison = 26, kOptAlarmPeasant = 27,
	kOptBackToWork = 30, kOptEjectAll = 31, kOptAutoExplore = 33, kOptTrain = 47, kOptResearch = 48, kOptGather = 51,
	kOptCancel = 67,
};

}  // namespace hud
