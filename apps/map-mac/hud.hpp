// The in-game HUD of ronmap (docs/ui-spec.md section 6, port plan section 8): the IFACEMAINBASE bar with its civ
// skin, the top-of-screen strip, the resource panel (IFACERESOURCES), the selection panel (IFACESELECTED), the command
// card (IFACEOPTIONS), the minimap (IFACEMINIMAP + WorldMap). Positions come from interface.xml through
// engine/assets/iface_layout; drawing goes through ron::metal::UiRenderer. The HUD only reads a State snapshot that
// main.mm fills from the simulation's views; it never touches the simulation.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/files.hpp"
#include "assets/iface_layout.hpp"
#include "assets/xml.hpp"
#include "ui.hpp"

namespace hud {

// iface_find_rect with the result remembered: the HUD asked it for the same (window, tag, name, size) several hundred times a frame and each call
// re-resolved the whole layout of the window (docs/ui-port.md 16.2: 12 % of the render CPU). The interface XML is loaded once and never changes, so
// the window's address plus its child count identify the layout.
inline ron::IfaceRect cached_iface_rect(const ron::XmlElement& win, const char* tag, const std::string& name, int pw, int ph, int nth) {
	std::string key;
	key.reserve(64);
	key.append(reinterpret_cast<const char*>(&win), sizeof(void*));
	const size_t kids = win.children.size();
	key.append(reinterpret_cast<const char*>(&kids), sizeof kids);
	key.append(reinterpret_cast<const char*>(&pw), sizeof pw);
	key.append(reinterpret_cast<const char*>(&ph), sizeof ph);
	key.append(reinterpret_cast<const char*>(&nth), sizeof nth);
	key += tag;
	key += '\1';
	key += name;
	static std::unordered_map<std::string, ron::IfaceRect> cache;
	const auto it = cache.find(key);
	if (it != cache.end()) return it->second;
	ron::IfaceRect r;
	ron::iface_find_rect(win, tag, name, pw, ph, r, nth);
	cache.emplace(std::move(key), r);
	return r;
}

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
	// IFaceMarket::on_button_redraw: OPTION_BUY / OPTION_SELL buttons of a Market show the good's icon, its price in wealth and
	// the trend arrow (Game::delta_flux of the good: -1 falling, 0 steady, 1 rising). price < 0: an ordinary option.
	int price = -1, trend = 0, good = -1;
	std::string label;  // a text button's caption (ejected units, the "Sell:" / "Buy:" row heads)
	// STANCE / FORM buttons (IFaceButton::setup_button EXPLICIT): the icon is the `variant`-th piece of the IFACEBUTTONCLASS list
	// `variant_list` ("combat", "worker", "caster", "packer", "form"); `orange` marks a mixed / switched-on state (ORANGE tint).
	std::string variant_list;
	int variant = -1;
	bool orange = false;
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
// `tint` colours the digits: 1 flashes red / yellow (the time limit under 120 frames, musical chairs under 450), 2 white / yellow
// (the time limit under 300 frames), by the graphic tick as draw_timers does.
struct Timer {
	int kind = 0, frames = 0, who = -1, count = -1, tint = 0;
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

// IFaceLibrary (docs/ui-spec.md 6.8): the grid that replaces the command card for a finished Library. A cell is an age
// (types 544..550) or the level of a library line (military 572.., civic 565.., commerce 558.., science 551..).
struct LibCell {
	int type = -1;
	bool enabled = false;  // can be queued now
	bool owned = false;    // already researched
	int percent = -1;      // in production at a Library
	std::string tex_id;
	int tex_col = 0, tex_row = 0, tex_x = -1, tex_y = -1;
};
struct Library {
	bool active = false;
	LibCell age[7];
	LibCell tech[4][7];  // [line][level]
	// The final-techs ("jeopardy") view (IFaceLibrary::on_redraw: has_preq(BASE_FINALTYPES) and no rules / flag veto): four
	// 74 x 157 buttons for the final techs 579 + line, replacing the grid.
	bool final_view = false;
	LibCell fin[4];
};

// A group of garrisoned units (IFaceGarrison): the units of one type and owner inside the selected building / carrier.
struct GarrisonUnit {
	int32_t who = 0, type = 0, count = 1;
	std::vector<int32_t> objects;  // their object slots
	std::string tex_id;
	int tex_col = 0, tex_row = 0, tex_x = -1, tex_y = -1;
	uint8_t rgb[3] = {255, 255, 255};  // the owner's team colour
	bool own = false;                  // owned by the viewer (clickable)
};
// IFaceAirGarrison (ui-spec 6.3, re/decomp/ui_air): the aircraft inside the selected airbase / carrier by family (fighters,
// bombers, helicopters) and state (ready = full fuel, fuelling), with the count and the mean fuel of the cell.
struct Icon {  // a command-card / unit icon cell (tex_x / tex_y >= 0: the explicit position)
	std::string tex_id;
	int col = 0, row = 0, x = -1, y = -1;
};
struct AirCell {
	int count = 0, fuel = 100;  // fuel: mean percent of the planes in the cell
	std::vector<int32_t> objs;  // their object slots
};
struct AirGarrison {
	bool active = false;
	int32_t who = -1, o = -1;  // the container
	bool own = false;          // the buttons are enabled only for the viewer's own container
	bool helis = false;        // helicopters inside (the small layout shows one row)
	bool carrier = false;      // a carrier (fighter-bombers) rather than an airbase (biplane line)
	AirCell cell[3][2];        // [family: fighters, bombers, helicopters][0 ready, 1 fuelling]
	Icon icon[3];              // the families' unit icons (the leader's current upgrade of each line)
};
struct Garrison {
	bool active = false;
	int32_t building = -1, owner = -1;  // the container
	bool mixed_owners = false;          // units of more than one leader: buttons are outlined in their owner's colour
	std::vector<GarrisonUnit> units;
};

// One production in progress (IFaceMainBase::setup_progress_buttons): an item queued at an own building.
struct ProgressItem {
	std::string tex_id;
	int tex_col = 0, tex_row = 0, tex_x = -1, tex_y = -1;
	int percent = 0;
	int32_t who = 0, o = 0;  // the producer
	float wx = 0, wy = 0;    // its position
	uint8_t rgb[3] = {255, 255, 255};
	std::string name;
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
	// Console::flash_resource_flags / _stamp: the resource rows turn red for 8 graphic ticks after an order failed for lack of
	// a good (bit k = good k).
	int flash_mask = 0, flash_tick = -1000;
	// draw_special_resources: the rare goods owned (TypeIndex 7..49) with the count collected.
	struct Rare {
		int good = 0, count = 0;
	};
	std::vector<Rare> rares;
	Library library;
	Garrison garrison;
	AirGarrison air;
	std::vector<ProgressItem> progress;  // own productions (the buttons show with set_progress_buttons)
	std::string card_message;            // IFaceMarket's text over the card rows (a Market that cannot trade yet)
	std::string speed_name;              // the clock's game speed word ("Normal"), localised
	std::string paused_name;             // the word shown while paused
	int armageddon = -1;                 // nukes left before Armageddon (-1 none): the armageddon timer's count
};

// What the mouse is over, for the mouse-over help (MouseOver: option, spots, buttons).
enum class HoverKind { None, Option, SelObject, SelQueue, SelStat, SelHitPoints, Good, CommerceCap, PopCap, CityCap, MarketBuy,
	                   MarketSell, ToggleMarket, ToggleResources, ToggleInfra, Infra, Caption, BarButton, MinimapButton, Display,
	                   MiniLibCat, MiniLibToggle, Timer, Clock, Minimap, Rare, LibraryCell, Garrison, Progress, AirButton, AirHeader, TypeGroup,
	                   LibraryCat, LibraryFinal };
struct Hover {
	HoverKind kind = HoverKind::None;
	int a = -1, b = -1;       // Option: option, object. Good / Market / Infra: the good. BarButton: its index. SelStat: 0 attack 1 armor 2 range.
	                          // SelObject / SelQueue: the index. MinimapButton / MiniLibCat: the button index. Timer: the kind.
	bool operator==(const Hover& o) const { return kind == o.kind && a == o.a && b == o.b; }
	bool operator!=(const Hover& o) const { return !(*this == o); }
};

enum class ActionKind { None, Option, MinimapLook, MinimapOrder, SelectIndex, QueueItem, Consumed, Buy, Sell, BarButton, MinimapButton, MiniLib,
	                        Library, GarrisonEject, Progress, AirSelect, SelectType, Infinite };
struct Action {
	ActionKind kind = ActionKind::None;
	int option = -1, object = -1, index = -1;
	float wx = 0, wy = 0;
	bool all = false;  // GarrisonEject: a right click ejects every unit of the type
	// AirSelect: index = the family row (0 fighters, 1 bombers, 2 helicopters, -1 all); SelectType: object = the TypeIndex of a
	// grouped selection button (State::selection entries of that type).
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
	// Production progress buttons (profile pref 0x40, IFaceMainBase::setup_progress_buttons): off by default.
	bool progress_buttons() const { return progress_buttons_; }
	void set_progress_buttons(bool on) { progress_buttons_ = on; }
	// Columns of rare goods drawn (draw_special_resources: a column of 8): MessageWin shifts right by 50 px per column.
	int rare_columns() const { return rare_cols_; }
	// "Advanced Menu" (profile pref bit 0x2, OPTION_TOGGLE_ADVANCED): the card also offers stances, formations, patrol, guard,
	// disband, bombard / air-patrol (UnitOut::make_options "A" rows). "Group by type" (pref 0x20000): an overflowing selection
	// shows one counted button per type.
	bool advanced() const { return advanced_; }
	void set_advanced(bool on) { advanced_ = on; }
	// "Prefer Military" (a box that holds soldiers and civilians takes the soldiers; Ctrl flips it): profile preference.
	bool prefer_military() const { return prefer_military_; }
	void set_prefer_military(bool on) { prefer_military_ = on; }
	bool group_by_type() const { return group_by_type_; }
	void set_group_by_type(bool on) { group_by_type_ = on; }
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

	// The chat line (OPTION_CHAT / _ALL / _ALLIES / _ENEMIES): a one-line edit box above the bar. mode 0 everybody, 1 allies, 2 enemies.
	// The window feeds it keys; Return sends the text as a CHAT command (CommandManager::issue_chat).
	bool chat_active() const { return chat_mode_ >= 0; }
	void chat_open(int mode) { chat_mode_ = mode, chat_text_.clear(); }
	void chat_close() { chat_mode_ = -1, chat_text_.clear(); }
	int chat_mode() const { return chat_mode_; }
	const std::string& chat_text() const { return chat_text_; }
	void chat_append(const std::string& utf8) {
		if (chat_text_.size() + utf8.size() <= 250) chat_text_ += utf8;
	}
	void chat_backspace();  // one UTF-8 character

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
	void draw_rares(ron::metal::UiRenderer& ui, const State& s);
	void draw_progress(ron::metal::UiRenderer& ui, const State& s);
	void draw_library(ron::metal::UiRenderer& ui, const State& s);
	void draw_garrison(ron::metal::UiRenderer& ui, const State& s);
	void draw_air_garrison(ron::metal::UiRenderer& ui, const State& s);
	void draw_library_final(ron::metal::UiRenderer& ui, const State& s);
	// The air panel's cells: a rect per (family, column) and the header spots, resolved against the window (air_layout).
	struct AirLayout {
		bool small = false;
		ron::IfaceRect win, head[2], cell[3][2];
		int rows = 3;
	};
	AirLayout air_layout(const State& s) const;
	int air_cell_at(int x, int y, const State& s, int* col = nullptr) const;
	int air_header_at(int x, int y, const State& s) const;
	int final_button_at(int x, int y, const State& s) const;
	int cat_button_at(int x, int y, int* which) const;
	// The selection panel's buttons: index into State::selection, or (grouped) a type; see multi_layout.
	struct MultiButton {
		ron::IfaceRect r;
		int index = -1;  // the selection entry (individual buttons), -1 for a type group
		int type = -1, count = 1;
		bool building = false;
	};
	std::vector<MultiButton> multi_layout(const State& s, bool* small = nullptr) const;
	void draw_chat(ron::metal::UiRenderer& ui, const State& s);
	void draw_market_button(ron::metal::UiRenderer& ui, const Option& o, const ron::IfaceRect& r, int factor);
	ron::IfaceRect garrison_rect(bool air = false) const;
	int garrison_index_at(int x, int y, const State& s, ron::IfaceRect* cell = nullptr) const;
	int library_cell_at(int x, int y, const State& s, int* cat = nullptr, int* col = nullptr) const;
	int progress_index_at(int x, int y, const State& s) const;
	ron::IfaceRect progress_rect(int i, int timers_bottom) const;
	void draw_icon_button(ron::metal::UiRenderer& ui, const std::string& tex_id, int col, int row, int tx, int ty, const ron::IfaceRect& r,
	                      int factor, bool small);
	void draw_health(ron::metal::UiRenderer& ui, const ron::IfaceRect& r, int hp, int max_hp, bool small);
	void hdivider(ron::metal::UiRenderer& ui, int x, int y, int w, bool thick);
	void vdivider(ron::metal::UiRenderer& ui, int x, int y, int h, bool thick);
	int card_slot_at(int x, int y) const;
	int multi_index_at(int x, int y, const State& s) const;
	int multi_type_at(int x, int y, const State& s) const;
	int queue_index_at(int x, int y, const State& s) const;
	bool infinite_hit(int x, int y, const State& s) const;  // the selection panel's INFINITE toggle (OPTION_INFINITE)

	const ron::IfaceLayout* layout_ = nullptr;
	const ron::XmlElement *base_el_ = nullptr, *res_el_ = nullptr, *sel_el_ = nullptr, *opt_el_ = nullptr, *mini_el_ = nullptr,
	                      *rm_el_ = nullptr, *btn_el_ = nullptr;
	int chat_mode_ = -1;      // the chat line: -1 closed
	std::string chat_text_;
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
	bool progress_buttons_ = false, icons_aliased_ = false, advanced_ = false, group_by_type_ = true, prefer_military_ = true;
	int rare_cols_ = 0, timers_bottom_ = 45;
	struct RareIcon {
		std::string tex_id;
		int x = 0, y = 0, w = 0, h = 0;
	};
	std::vector<RareIcon> rare_icons_;  // resourcerules.xml TEX2_*, by TypeIndex
	std::vector<std::pair<ron::IfaceRect, int>> rare_rects_;  // the column's icons (screen) and their good, set by draw
	ron::IfaceRect library_rect_;
	std::vector<ron::IfaceRect> progress_rects_;  // the progress buttons drawn last (the map must not take their clicks)
	const ron::XmlElement* lib_el_ = nullptr;
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
	kOptGroup = 900,  // a port addition (docs/ui-port.md section 14): name the selected soldiers as a control group / regroup it
};

}  // namespace hud
