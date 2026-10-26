#include "options_ui.hpp"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "keymap.hpp"
#include "uistr.hpp"

using ron::IfaceBorder;
using ron::IfacePiece;
using ron::IfaceRect;
using ron::metal::UiRenderer;
using ron::metal::UiType;
using ron::metal::argb;
using ron::metal::rgb;

namespace optui {

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000;
const uint32_t kGrey = rgb(150, 150, 150), kLight = rgb(212, 212, 212);
constexpr int kPaneX = 42, kPaneY = 42;  // gamepane / hotkeyspane ... (42,42) in the window
constexpr int kRowH = 16;

std::string tr(const char* s) { return uistr::tr(s); }

std::string upper_key(std::string s) {  // "Auto Explore" -> "AUTO_EXPLORE"
	std::string o;
	for (char c : s) {
		if (std::isalnum(static_cast<unsigned char>(c))) o += char(std::toupper(static_cast<unsigned char>(c)));
		else if (c == ' ' || c == '-' || c == '_') o += '_';
	}
	return o;
}

std::string prettify(const std::string& option) {  // OPTION_CYCLE_PEASANT -> Cycle Peasant
	if (option == "OPTION_ALLAIR") return "Select All Aircraft";
	if (option == "OPTION_ALL_CURRENT") return "Select All of Current Types";
	std::string s = option.rfind("OPTION_", 0) == 0 ? option.substr(7) : option;
	std::string o;
	bool up = true;
	for (char c : s) {
		if (c == '_') {
			o += ' ';
			up = true;
		} else {
			o += up ? char(std::toupper(static_cast<unsigned char>(c))) : char(std::tolower(static_cast<unsigned char>(c)));
			up = false;
		}
	}
	return o;
}

// What a widget is bound to.
enum class Kind { Area, Slider, Check, Combo, Text };

struct Widget {
	Kind kind = Kind::Text;
	IfaceRect r;  // window-relative
	std::string title;
	int lo = 0, hi = 1;
	std::string left_label, right_label;
	std::function<int()> get;
	std::function<void(int)> set;
	std::vector<std::string> items;  // combo
	bool sound_preview = false;
	bool enabled = true;
};

struct CmdRow {
	size_t index;  // keymap entry
	std::string label;
};

}  // namespace

struct Page::Impl {
	const ron::GameFiles& files;
	const ron::IfaceLayout& iface;
	std::unique_ptr<ron::IfaceLayout> win;
	const ron::XmlElement* root = nullptr;
	Host host;
	bool is_open = false;
	int pane = 0;
	prefs::Prefs w, saved;
	int ox = 0, oy = 0;      // the last draw's origin
	int hx = -1, hy = -1;    // hover, window-relative
	int pressed = -1;        // pressed button (tab, ok / reset / cancel, hotkeys buttons)
	int drag_widget = -1;    // slider being dragged
	bool drag_scroll = false;
	int drag_scroll_list = 0, drag_off = 0;
	int open_combo = -1;     // widget index of the open drop-down

	// pieces
	IfacePiece check[2], check_small[2], bleft, bmid, bright, tab[2][3], large[3], area_top, caret, border_l, border_m, border_r;
	IfacePiece combo[4], vbar, hbar;
	IfaceBorder frame, area_partial, area_full;
	IfaceRect main_r, button_border_r, ok_r, reset_r, cancel_r, first_tab, first_tab_text;
	IfacePiece scroll_up, scroll_down, scroll_back[3], scroll_slider[3];

	std::vector<Widget> widgets;

	// Hotkeys pane
	std::vector<std::string> hk;           // working key of each keymap entry
	std::vector<std::string> cmd_label;    // label of each keymap entry
	std::vector<CmdRow> rows;              // the filtered list
	int filter = 0;
	int sel_row = -1;                      // index into rows
	int list_top = 0, assigned_top = 0;
	std::string cand;                      // the key in the edit box
	bool capturing = false;
	std::string hk_note;
	double note_until = 0;
	std::map<std::string, std::string> option_names;  // OPTION_X -> options.xml NAME

	Impl(const ron::GameFiles& f, const ron::IfaceLayout& i) : files(f), iface(i) {}

	// ---------------------------------------------------------------- setup
	bool init() {
		try {
			win = std::make_unique<ron::IfaceLayout>(files.read_text("Data/optionswin.xml"));
		} catch (...) {
			return false;
		}
		root = win->find("optionswin");
		if (!root) return false;
		const ron::XmlElement& e = *root;
		check[0] = ron::iface_find_piece(e, "check_off"), check[1] = ron::iface_find_piece(e, "check_on");
		check_small[0] = ron::iface_find_piece(e, "check_off_small"), check_small[1] = ron::iface_find_piece(e, "check_on_small");
		bleft = ron::iface_find_piece(e, "button_left"), bmid = ron::iface_find_piece(e, "button_mid"), bright = ron::iface_find_piece(e, "button_right");
		for (int s = 0; s < 2; s++) {
			tab[s][0] = ron::iface_find_piece(e, "tab_left_" + std::to_string(s));
			tab[s][1] = ron::iface_find_piece(e, "tab_mid_" + std::to_string(s));
			tab[s][2] = ron::iface_find_piece(e, "tab_right_" + std::to_string(s));
		}
		large[0] = ron::iface_find_piece(e, "large_button_left"), large[1] = ron::iface_find_piece(e, "large_button_mid"),
		large[2] = ron::iface_find_piece(e, "large_button_right");
		area_top = ron::iface_find_piece(e, "area_top");
		border_l = ron::iface_find_piece(e, "button_border_left"), border_m = ron::iface_find_piece(e, "button_border_mid"),
		border_r = ron::iface_find_piece(e, "button_border_right");
		frame = ron::iface_find_border(e, "border"), area_partial = ron::iface_find_border(e, "area_partial"),
		area_full = ron::iface_find_border(e, "area_full");
		const ron::XmlElement* hot = win->find("hotkeyspane");
		if (hot) {
			const char* names[4] = {"combo_left", "combo_mid", "combo_right", "combo_arrow"};
			for (int k = 0; k < 4; k++) combo[k] = ron::iface_find_piece(*hot, names[k]);
			vbar = ron::iface_find_piece(*hot, "vert_bar"), hbar = ron::iface_find_piece(*hot, "horiz_bar");
		}
		if (const ron::XmlElement* sl = win->find("game_speed")) caret = ron::iface_find_piece(*sl, "caret");
		if (const ron::XmlElement* sc = iface.find("SCROLLCLASS")) {
			scroll_up = ron::iface_find_piece(*sc, "button_up"), scroll_down = ron::iface_find_piece(*sc, "button_down");
			for (int k = 0; k < 3; k++) {
				scroll_back[k] = ron::iface_find_piece(*sc, "back_vert", k);
				scroll_slider[k] = ron::iface_find_piece(*sc, "slider_vert", k);
			}
		}
		auto rect = [&](const char* tag, const char* name) {
			IfaceRect r;
			ron::iface_find_rect(e, tag, name, kW(), kH(), r);
			return r;
		};
		main_r = rect("IFACERECT", "main"), first_tab = rect("IFACERECT", "first_tab"), first_tab_text = rect("IFACERECT", "first_tab_text");
		button_border_r = rect("IFACERECT", "button_border");
		ok_r = rect("BUTTON", "ok"), reset_r = rect("BUTTON", "reset"), cancel_r = rect("BUTTON", "cancel");
		// options.xml: the command names of the Hotkeys pane
		try {
			const ron::XmlElement doc = ron::parse_xml(files.read_text("Data/options.xml"));
			ron::for_each_element(doc, "OPTION", [&](const ron::XmlElement& o) {
				const ron::XmlElement* en = o.child("ENGLISH_NAME");
				const ron::XmlElement* nm = o.child("NAME");
				if (!en || !nm) return;
				option_names.emplace("OPTION_" + upper_key(en->get("#text")), nm->get("#text"));
			});
		} catch (...) {
		}
		return true;
	}
	static int kW() { return Page::kW; }
	static int kH() { return Page::kH; }

	// ---------------------------------------------------------------- the working copy
	void load_working() {
		w = prefs::get();
		if (host.live) host.live(w);
		saved = w;
		const size_t n = keymap::count();
		hk.assign(n, {});
		for (size_t i = 0; i < n; i++) hk[i] = keymap::keys_of(i);
		cmd_label.assign(n, {});
		for (size_t i = 0; i < n; i++) cmd_label[i] = label_of(i);
		filter = 0, sel_row = -1, list_top = assigned_top = 0, cand.clear(), capturing = false, hk_note.clear();
		rebuild_rows();
	}

	std::string label_of(size_t i) const {
		const keymap::Entry& e = keymap::entry(i);
		auto it = option_names.find(e.option);
		std::string base = it != option_names.end() && !it->second.empty() ? it->second : prettify(e.option);
		if (e.param >= 50 && host.type_name) {
			const std::string t = host.type_name(e.param);
			if (!t.empty()) base += ": " + t;
		} else if (e.param >= 0 && std::strcmp(e.option, "OPTION_TOGGLE_HELP") == 0) {
			static const char* names[4] = {"", "Help Detail 1", "Help Detail 2", "Help Detail 3"};
			base = names[std::clamp(e.param, 0, 3)];
		} else if (e.param >= 0 && std::strcmp(e.option, "OPTION_STANCE") == 0) {
			static const char* names[6] = {"Aggressive", "Defensive", "Stand Ground", "Raid", "Raze", "Hold Fire"};
			base += std::string(": ") + tr(names[std::clamp(e.param, 0, 5)]);
		} else if (e.param >= 0 && std::strcmp(e.option, "OPTION_FORM") == 0) {
			base += ": " + std::to_string(e.param + 1);
		} else if (e.param >= 0 && (std::strcmp(e.option, "OPTION_BUY") == 0 || std::strcmp(e.option, "OPTION_SELL") == 0) && host.type_name) {
			const std::string t = host.type_name(e.param);
			if (!t.empty()) base += ": " + t;
		}
		if (e.mode == 1) base += " (" + tr("Build Mode") + ")";
		return base;
	}

	bool in_filter(size_t i) const {
		const keymap::Entry& e = keymap::entry(i);
		switch (filter) {
			case 0: return true;
			case 1: return e.mode == 0 && e.ctx == -1;                                    // Commands
			case 2: return e.mode == 1;                                                   // Build Mode
			case 3: return e.ctx == -2 || e.ctx == -4 || (e.ctx >= 415 && e.ctx < 629);  // Building
			case 4: return e.ctx == -3 || e.ctx == -5 || e.ctx == -6 || (e.ctx >= 50 && e.ctx < 414);  // Unit
			case 5: return e.ctx == 414;                                                  // City
			default: return true;
		}
	}

	void rebuild_rows() {
		const int keep = sel_row >= 0 && sel_row < int(rows.size()) ? int(rows[size_t(sel_row)].index) : -1;
		rows.clear();
		for (size_t i = 0; i < keymap::count(); i++) {
			if (in_filter(i)) rows.push_back({i, cmd_label[i]});
		}
		sel_row = -1;
		for (size_t r = 0; r < rows.size(); r++) {
			if (int(rows[r].index) == keep) sel_row = int(r);
		}
		list_top = std::clamp(list_top, 0, std::max(0, int(rows.size()) - 22));
	}

	static std::string key_text(const std::string& k) { return k.empty() ? "<" + tr("None") + ">" : k; }

	void select_row(int r) {
		sel_row = r;
		cand = r >= 0 ? hk[rows[size_t(r)].index] : std::string();
		capturing = false;
		assigned_top = 0;
	}

	void commit() {
		// OK: the page's values become the preferences and take effect.
		prefs::Prefs& g = prefs::get();
		std::map<std::string, std::string> over;
		for (size_t i = 0; i < keymap::count(); i++) {
			if (hk[i] != keymap::entry(i).keys) over[keymap::signature(i)] = hk[i];
		}
		w.hotkeys = over;
		const std::set<std::string> present = g.present;
		g = w;
		g.present = present;
		prefs::save();
		keymap::apply_overrides(g.hotkeys);
		if (host.apply) host.apply(g);
	}

	void set_pane(int pn) {
		pane = std::clamp(pn, 0, int(kPaneCount) - 1);
		capturing = false;
		build_pane();
	}

	// ---------------------------------------------------------------- panes
	Widget* add(Widget wd) {
		widgets.push_back(std::move(wd));
		return &widgets.back();
	}
	static IfaceRect P(int x, int y, int w_, int h) { return {kPaneX + x, kPaneX + x + w_, kPaneY + y, kPaneY + y + h}; }

	void slider(int ax, int ay, int aw, int ah, const char* title, int sx, int sy, int sw, int lo, int hi, std::function<int()> get,
	            std::function<void(int)> set, const char* ll, const char* rl, bool preview = false) {
		Widget a;
		a.kind = Kind::Area, a.r = P(ax, ay, aw, ah), a.title = tr(title);
		add(a);
		Widget s;
		s.kind = Kind::Slider, s.r = P(ax + sx, ay + sy, sw, 28), s.lo = lo, s.hi = hi, s.get = std::move(get), s.set = std::move(set);
		s.left_label = ll ? tr(ll) : "", s.right_label = rl ? tr(rl) : "", s.sound_preview = preview;
		add(s);
	}
	void check_box(int x, int y, int w_, const char* title, std::function<int()> get, std::function<void(int)> set, int h = 22) {
		Widget c;
		c.kind = Kind::Check, c.r = P(x, y, w_, h), c.title = tr(title), c.get = std::move(get), c.set = std::move(set);
		add(c);
	}

	void build_pane() {
		widgets.clear();
		open_combo = -1;
		switch (pane) {
			case kGame: {
				// optionswin.xml GAMEPANE: Auto Citizen (240 x 106 at 9,10), its delay (9,128), Building Stances (9,186), the switches at
				// (260,17) and (530,17) (24 px apart; the ones with no feature behind them are left out), the four sliders at x = 360.
				{
					Widget a;
					a.kind = Kind::Area, a.r = P(9, 10, 240, 106), a.title = tr("Auto Citizen");
					add(a);
					static const int ids[4] = {3, 2, 0, 1};  // the XML's order: Idle, Build, Gather, Build and Gather (the stance each sets)
					static const char* names[4] = {"Idle", "Build", "Gather", "Build and Gather"};
					for (int k = 0; k < 4; k++) {
						const int id = ids[k];
						check_box(9 + 24, 10 + 20 + k * 19, 158, names[k], [this, id] { return int(w.auto_citizen == id); },
						          [this, id](int v) { if (v) w.auto_citizen = id; }, 13);
					}
				}
				slider(9, 128, 240, 44, "Auto Citizen Delay", 79, 11, 71, 0, 5, [this] { return w.auto_citizen_delay; }, [this](int v) { w.auto_citizen_delay = v; },
				       "Short", "Long");
				{
					Widget a;
					a.kind = Kind::Area, a.r = P(9, 186, 240, 140), a.title = tr("Building Stances");
					add(a);
					static const char* names[6] = {"Aggressive", "Defensive", "Stand Ground", "Raid", "Raze", "Hold Fire"};
					for (int k = 0; k < 6; k++) {
						check_box(9 + 24, 186 + 20 + k * 19, 158, names[k], [this, k] { return int(w.building_stance == k); },
						          [this, k](int v) { if (v) w.building_stance = k; }, 13);
					}
				}
				int row = 0;
				check_box(260, 17 + 24 * row++, 270, "Show Advanced Options", [this] { return int(w.advanced_menu); }, [this](int v) { w.advanced_menu = v != 0; });
				check_box(260, 17 + 24 * row++, 270, "Scouts Auto Transport", [this] { return int(w.auto_transport); }, [this](int v) { w.auto_transport = v != 0; });
				check_box(260, 17 + 24 * row++, 270, "Show Research Progress Bars", [this] { return int(w.progress_bars); }, [this](int v) { w.progress_bars = v != 0; });
				row = 0;
				check_box(530, 17 + 24 * row++, 270, "Siege units deploy when idle", [this] { return int(w.auto_deploy); }, [this](int v) { w.auto_deploy = v != 0; });
				check_box(530, 17 + 24 * row++, 270, "Record games", [this] { return int(w.record_games); }, [this](int v) { w.record_games = v != 0; });
				slider(360, 180, 300, 44, "Game Speed", 100, 11, 96, 0, 3, [this] { return std::min(w.game_speed, 3); }, [this](int v) { w.game_speed = v; },
				       "Very Slow", "Fast");
				slider(360, 238, 300, 44, "Difficulty Level", 100, 11, 96, 0, 5, [this] { return w.difficulty; }, [this](int v) { w.difficulty = v; }, "Easiest",
				       "Toughest");
				slider(360, 296, 300, 44, "Help Message Detail", 100, 11, 96, 0, 3, [this] { return w.help_detail; }, [this](int v) { w.help_detail = v; }, "None",
				       "High");
				slider(360, 354, 300, 44, "Message Duration", 100, 11, 96, 0, 10, [this] { return w.message_length; }, [this](int v) { w.message_length = v; },
				       "Short", "Long");
				break;
			}
			case kMouse: {
				slider(301, 80, 285, 63, "Map Scroll Speed", 90, 18, 100, 0, 12, [this] { return w.scroll_speed; }, [this](int v) { w.scroll_speed = v; }, "Slow",
				       "Fast");
				check_box(301, 155, 346, "Drag-Select Prefer Military", [this] { return int(w.prefer_military); }, [this](int v) { w.prefer_military = v != 0; });
				break;
			}
			case kGraphics: {
				Widget c;
				c.kind = Kind::Combo, c.r = P(300, 40, 220, 22), c.items = {tr("Windowed"), tr("Full Screen")};
				c.get = [this] { return int(w.fullscreen); }, c.set = [this](int v) { w.fullscreen = v != 0; };
				add(c);
				check_box(300, 74, 346, "Streamlined Interface", [this] { return int(w.streamlined); }, [this](int v) { w.streamlined = v != 0; });
				Widget t;
				t.kind = Kind::Text, t.r = P(300, 112, 346, 22), t.title = tr("Resolution: ") + (host.resolution ? host.resolution() : std::string("-"));
				add(t);
				break;
			}
			case kSound: {
				slider(257, 20, 292, 76, "FX Volume", 110, 25, 71, 0, 255, [this] { return w.fx_volume; }, [this](int v) { w.fx_volume = v; }, nullptr, nullptr, true);
				slider(257, 120, 292, 76, "Music Volume", 110, 25, 71, 0, 255, [this] { return w.music_volume; }, [this](int v) { w.music_volume = v; }, nullptr,
				       nullptr, true);
				check_box(257, 230, 346, "Allow In-Game Music", [this] { return int(w.music_in_game); }, [this](int v) { w.music_in_game = v != 0; });
				check_box(257, 254, 346, "Allow Main Menu Music", [this] { return int(w.music_in_menu); }, [this](int v) { w.music_in_menu = v != 0; });
				break;
			}
			case kPlayer: {
				Widget a;
				a.kind = Kind::Area, a.r = P(540, 35, 240, 70), a.title = tr("Language");
				add(a);
				Widget c;
				c.kind = Kind::Combo, c.r = P(585, 60, 160, 21), c.items = {"English", "Deutsch", "Espa\xC3\xB1ol", "Fran\xC3\xA7" "ais", "Italiano"};
				c.get = [this] {
					static const char* codes[5] = {"en", "de", "es", "fr", "it"};
					for (int k = 0; k < 5; k++) {
						if (w.language == codes[k]) return k;
					}
					return 0;
				};
				c.set = [this](int v) {
					static const char* codes[5] = {"en", "de", "es", "fr", "it"};
					w.language = codes[std::clamp(v, 0, 4)];
				};
				add(c);
				break;
			}
			default: break;
		}
	}

	// ---------------------------------------------------------------- drawing helpers
	IfaceRect abs(const IfaceRect& r) const { return r.offset(ox, oy); }

	void expander3(UiRenderer& ui, const IfacePiece p[3], const IfaceRect& r, int row_shift) {
		IfacePiece q[3] = {p[0], p[1], p[2]};
		for (IfacePiece& x : q) x.top += row_shift * (x.height + 1);
		if (q[0].valid() && q[1].valid() && q[2].valid()) ui.expander(q[0], q[1], q[2], r);
		else ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), argb(255, 70, 70, 70), UiType::Normal);
	}

	// A large button of the OK row / the small `button` of the Hotkeys pane (OptionsWin::on_button_redraw / button_redraw): the piece row below
	// is the idle state, the piece row itself the hot (hover / pressed) one, the third the disabled one; the label shifts 2,2 while pressed.
	void button(UiRenderer& ui, const IfaceRect& rel, const std::string& label, int id, bool is_large, bool enabled = true) {
		const IfaceRect r = abs(rel);
		const bool hover = enabled && rel.contains(hx, hy), down = pressed == id && hover;
		const int row = !enabled ? 2 : (down || hover) ? 0 : 1;
		const IfacePiece sm[3] = {bleft, bmid, bright};
		if (is_large) expander3(ui, large, r, row);
		else expander3(ui, sm, r, row);
		ui.next_layer();
		const int f = is_large ? ui.font(16, -1) : ui.font(12, 1);
		ui.text_centered(f, label, down ? r.offset(2, 2) : r, enabled ? kWhite : kGrey);
		ui.next_layer();
	}

	void area(UiRenderer& ui, const IfaceRect& rel, const std::string& title) {
		const IfaceRect r = abs(rel);
		ui.border(title.empty() ? area_full : area_partial, r);
		ui.next_layer();
		if (title.empty()) return;
		// OptionsWin::draw_area: the title is written on the top edge's left end, the edge continues after it.
		const int f = ui.font(14, 1);
		const ron::FontMetrics* m = ui.font_metrics(f);
		const int fh = m ? m->font_height : 16;
		const int ty = r.t - fh / 2 - 1;
		ui.text(f, title, float(r.l + 8), float(ty), kWhite);
		const int tw = ui.text_width(f, title);
		if (area_top.valid()) ui.tile(area_top, {r.l + 8 + tw + 5, r.r - 2, r.t, r.t + area_top.height});
		ui.next_layer();
	}

	void check_draw(UiRenderer& ui, const Widget& c) {
		const IfaceRect r = abs(c.r);
		const bool on = c.get && c.get() != 0, hover = c.enabled && c.r.contains(hx, hy);
		IfacePiece p = check[on ? 1 : 0];
		if (c.r.h() < p.height) p = check_small[on ? 1 : 0];
		if (hover || !c.enabled) p.top += p.height + 1;  // the next row of the sheet: the lit / disabled box
		if (p.valid()) ui.add(p, float(r.l), float(r.t + (r.h() - p.height) / 2));
		ui.next_layer();
		const int f = ui.font(12, -1);
		const ron::FontMetrics* m = ui.font_metrics(f);
		const int fh = m ? m->font_height : 14;
		ui.text(f, c.title, float(r.l + p.width + 8), float(r.t + (r.h() - fh) / 2), c.enabled ? kWhite : kGrey);
		ui.next_layer();
	}

	int slider_x(const Widget& s, int value) const {  // the caret's left edge (window-relative)
		const int span = std::max(1, s.r.w() - caret.width);
		return s.r.l + span * (value - s.lo) / std::max(1, s.hi - s.lo);
	}
	int slider_value_at(const Widget& s, int x) const {
		const int span = std::max(1, s.r.w() - caret.width);
		const int v = s.lo + int(std::lround(double(x - s.r.l - caret.width / 2) * (s.hi - s.lo) / span));
		return std::clamp(v, s.lo, s.hi);
	}

	void slider_draw(UiRenderer& ui, const Widget& s) {
		const IfaceRect r = abs(s.r);
		const int cy = (r.t + r.b) / 2;
		// OptionsWinSlider::on_redraw: a grey double-line track at the middle, tick marks, the caret on top.
		ui.fill(float(r.l), float(cy - 2), float(r.w()), 4, rgb(128, 128, 128), UiType::Normal);
		ui.fill(float(r.l + 1), float(cy - 1), float(r.w() - 2), 2, rgb(0x3f, 0x3f, 0x3f), UiType::Normal);
		ui.next_layer();
		const int range = s.hi - s.lo;
		const int ticks = range <= 12 ? range : 5;
		for (int k = 1; k < ticks; k++) {
			const int x = r.l + (r.w() - 1) * k / ticks;
			ui.fill(float(x), float(cy + 3), 1, 3, rgb(0xc2, 0xc2, 0xc2), UiType::Normal);
		}
		ui.fill(float(r.l), float(cy + 3), 1, 4, rgb(0xc2, 0xc2, 0xc2), UiType::Normal);
		ui.fill(float(r.r - 1), float(cy + 3), 1, 4, rgb(0xc2, 0xc2, 0xc2), UiType::Normal);
		ui.next_layer();
		const int v = s.get ? s.get() : s.lo;
		const int cx = ox + slider_x(s, v);
		if (caret.valid()) ui.add(caret, float(cx), float(r.t));
		ui.next_layer();
		const int f = ui.font(12, -1);
		const ron::FontMetrics* m = ui.font_metrics(f);
		const int fh = m ? m->font_height : 14;
		if (!s.left_label.empty()) ui.text(f, s.left_label, float(r.l - 10 - ui.text_width(f, s.left_label)), float(cy - fh / 2), kWhite);
		if (!s.right_label.empty()) ui.text(f, s.right_label, float(r.r + 10), float(cy - fh / 2), kWhite);
		ui.next_layer();
	}

	// The GameSpy-style combo box: white field with the arrow button; the open list drops below.
	void combo_draw(UiRenderer& ui, const IfaceRect& rel, const std::string& text, bool open) {
		const IfaceRect D = abs(rel);
		if (combo[0].valid()) {
			const IfaceRect field{D.l, D.r - combo[3].width + 2, D.t, D.b};
			ui.expander(combo[0], combo[1], combo[2], field);
			ui.add(combo[3], float(D.r - combo[3].width), float(D.t));
		} else {
			ui.fill(float(D.l), float(D.t), float(D.w()), float(D.h()), rgb(240, 240, 240), UiType::Normal);
		}
		ui.next_layer();
		const int f = ui.font(12, 1);
		const ron::FontMetrics* m = ui.font_metrics(f);
		const int fh = m ? m->font_height : 14;
		ui.text(f, text, float(D.l + 7), float(D.t + (D.h() - fh) / 2), kBlack);
		ui.next_layer();
		(void)open;
	}

	void drop_draw(UiRenderer& ui, const IfaceRect& rel, const std::vector<std::string>& items, int cur) {
		const int ih = 20;
		const IfaceRect list{rel.l, rel.r - 2, rel.b + 1, rel.b + 1 + int(items.size()) * ih + 6};
		const IfaceRect L = abs(list);
		ui.next_window();
		ui.fill(float(L.l), float(L.t), float(L.w()), float(L.h()), rgb(244, 244, 240), UiType::Normal);
		ui.next_layer();
		ui.frame(float(L.l), float(L.t), float(L.w()), float(L.h()), rgb(60, 60, 60));
		const int f = ui.font(12, 1);
		const ron::FontMetrics* m = ui.font_metrics(f);
		const int fh = m ? m->font_height : 14;
		for (size_t k = 0; k < items.size(); k++) {
			const IfaceRect it{L.l + 3, L.r - 3, L.t + 3 + int(k) * ih, L.t + 3 + int(k + 1) * ih};
			const bool hot = IfaceRect{list.l + 3, list.r - 3, list.t + 3 + int(k) * ih, list.t + 3 + int(k + 1) * ih}.contains(hx, hy) || (hx < 0 && int(k) == cur);
			if (hot) ui.fill(float(it.l), float(it.t), float(it.w()), float(it.h()), rgb(53, 47, 132), UiType::Normal);
			ui.next_layer();
			ui.text(f, items[k], float(it.l + 5), float(it.t + (ih - fh) / 2), hot ? kWhite : kBlack);
		}
	}
	IfaceRect drop_item_rect(const IfaceRect& rel, size_t k) const {
		const int ih = 20;
		return {rel.l + 3, rel.r - 5, rel.b + 1 + 3 + int(k) * ih, rel.b + 1 + 3 + int(k + 1) * ih};
	}

	// ---------------------------------------------------------------- the hotkeys pane
	// OptionsWinHotkeys: left_title "Commands" over the command list (368 x 356), a filter combo, the "Hotkey:" edit box with Assign / Remove,
	// the "Hotkey Mappings" list of the commands bound to the key in the edit box, the horizontal and vertical bars.
	IfaceRect hk_rect(const char* tag, const char* name) const {
		IfaceRect r;
		if (const ron::XmlElement* e = win->find("hotkeyspane")) ron::iface_find_rect(*e, tag, name, 796, 426, r);
		return r.offset(kPaneX - 1, kPaneY);
	}
	IfaceRect hk_list() const { return hk_rect("LISTBOX2", "key"); }
	IfaceRect hk_assigned() const {  // the XML's list is 315 px tall; the lower 44 px carry the notes
		IfaceRect r = hk_rect("LISTBOX2", "assigned");
		r.b -= 44;
		return r;
	}
	IfaceRect hk_edit() const { return hk_rect("HOTKEYEDITBOX", "edit"); }
	IfaceRect hk_groups() const { return hk_rect("COMBOBOX", "groups"); }
	IfaceRect hk_assign() const { return hk_rect("BUTTON", "assign"); }
	IfaceRect hk_remove() const { return hk_rect("BUTTON", "remove"); }

	std::vector<size_t> assigned_entries() const {
		std::vector<size_t> out;
		if (cand.empty()) return out;
		for (size_t i = 0; i < keymap::count(); i++) {
			if (hk[i] == cand) out.push_back(i);
		}
		return out;
	}

	void scrollbar_draw(UiRenderer& ui, const IfaceRect& rel_list, int total, int visible, int top) {
		const IfaceRect L = abs(rel_list);
		const IfaceRect s{L.r + 1, L.r + 17, L.t, L.b};
		if (scroll_back[0].valid()) ui.vexpander(scroll_back[0], scroll_back[1], scroll_back[2], s);
		ui.next_layer();
		const IfaceRect tr_ = thumb_rect(rel_list, total, visible, top).offset(ox, oy);
		if (scroll_slider[0].valid() && total > visible) ui.vexpander(scroll_slider[0], scroll_slider[1], scroll_slider[2], tr_);
		if (scroll_up.valid()) ui.add(scroll_up, float(s.l), float(s.t));
		if (scroll_down.valid()) ui.add(scroll_down, float(s.l), float(s.b - scroll_down.height));
		ui.next_layer();
	}
	IfaceRect thumb_rect(const IfaceRect& rel_list, int total, int visible, int top) const {
		const int t0 = rel_list.t + 17, t1 = rel_list.b - 17, th = t1 - t0;
		if (total <= visible) return {rel_list.r + 1, rel_list.r + 17, t0, t0};
		const int len = std::max(24, th * visible / total);
		const int y = t0 + (th - len) * top / std::max(1, total - visible);
		return {rel_list.r + 1, rel_list.r + 17, y, y + len};
	}

	void list_rows_draw(UiRenderer& ui, const IfaceRect& rel_list, int top, int total, const std::function<std::string(int)>& left,
	                    const std::function<std::string(int)>& right, int selected, bool hover_rows) {
		const IfaceRect L = abs(rel_list);
		const int rows_vis = rel_list.h() / kRowH;
		const int f = ui.font(12, -1);
		const ron::FontMetrics* m = ui.font_metrics(f);
		const int fh = m ? m->font_height : 14;
		for (int r = 0; r < rows_vis && top + r < total; r++) {
			const int i = top + r;
			const IfaceRect row{L.l, L.r, L.t + r * kRowH, L.t + (r + 1) * kRowH};
			const bool hov = hover_rows && IfaceRect{rel_list.l, rel_list.r, rel_list.t + r * kRowH, rel_list.t + (r + 1) * kRowH}.contains(hx, hy);
			if (i == selected) ui.fill(float(row.l), float(row.t), float(row.w()), float(row.h()), argb(170, 150, 105, 40), UiType::ColorBlend);
			else if (hov) ui.fill(float(row.l), float(row.t), float(row.w()), float(row.h()), argb(90, 150, 130, 80), UiType::ColorBlend);
			ui.next_layer();
			ui.text(f, left(i), float(row.l + 6), float(row.t + (kRowH - fh) / 2), kWhite);
			const std::string rt = right ? right(i) : std::string();
			if (!rt.empty()) ui.text(f, rt, float(row.r - 8 - ui.text_width(f, rt)), float(row.t + (kRowH - fh) / 2), rgb(255, 224, 140));
			ui.next_layer();
		}
	}

	void hotkeys_draw(UiRenderer& ui, double now) {
		const int f14 = ui.font(14, 1), f12b = ui.font(12, 1);
		const ron::FontMetrics* m14 = ui.font_metrics(f14);
		const int fh14 = m14 ? m14->font_height : 16;
		auto title = [&](const char* tag, const char* name, const std::string& text) {
			IfaceRect r;
			if (const ron::XmlElement* e = win->find("hotkeyspane")) ron::iface_find_rect(*e, tag, name, 796, 426, r);
			r = abs(r.offset(kPaneX, kPaneY));
			ui.text_centered(f14, text, {r.l, r.r, r.t + (r.h() - fh14) / 2 - 4, r.t + (r.h() + fh14) / 2 - 4}, kWhite);
		};
		title("IFACERECT", "left_title", tr("Commands"));
		title("IFACERECT", "right_title", tr("Hotkey Mappings"));
		ui.next_layer();
		// the bars
		IfaceRect hb = hk_rect("IFACERECT", "horiz_bar");
		if (hbar.valid()) ui.tile(hbar, abs(hb));
		IfaceRect vb = hk_rect("IFACERECT", "vert_bar");
		if (vbar.valid()) ui.tile(vbar, abs(vb));
		ui.next_layer();
		// filter combo with its label
		const IfaceRect g = hk_groups();
		const std::string flt = tr("Filter: ");
		ui.text(f12b, flt, float(ox + g.l - 4 - ui.text_width(f12b, flt)), float(oy + g.t + 4), kLight);
		static const char* fnames[6] = {"All", "Commands", "Build Mode", "Building", "Unit", "City"};
		combo_draw(ui, g, tr(fnames[filter]), open_combo == 100);
		// the command list: label and key
		const IfaceRect L = hk_list();
		list_rows_draw(ui, L, list_top, int(rows.size()), [&](int i) { return rows[size_t(i)].label; },
		               [&](int i) { return key_text(hk[rows[size_t(i)].index]); }, sel_row, true);
		scrollbar_draw(ui, L, int(rows.size()), L.h() / kRowH, list_top);
		// the "Hotkey:" edit box
		const IfaceRect E = hk_edit();
		const std::string hot = tr("Hotkey: ");
		ui.text(f12b, hot, float(ox + E.l - 6 - ui.text_width(f12b, hot)), float(oy + E.t + 4), kLight);
		const IfaceRect EA = abs(E);
		ui.fill(float(EA.l), float(EA.t), float(EA.w()), float(EA.h()), capturing ? rgb(255, 255, 210) : rgb(240, 240, 240), UiType::Normal);
		ui.next_layer();
		ui.frame(float(EA.l), float(EA.t), float(EA.w()), float(EA.h()), capturing ? rgb(200, 140, 20) : rgb(60, 60, 60));
		const bool blink = int(now * 2) % 2 == 0;
		ui.text(f12b, capturing ? (blink ? tr("Press a key...") : "") : key_text(cand), float(EA.l + 7), float(EA.t + 4), kBlack);
		ui.next_layer();
		button(ui, hk_assign(), tr("Assign"), 40, false, sel_row >= 0 && cand != hk[rows[size_t(sel_row)].index]);
		button(ui, hk_remove(), tr("Remove"), 41, false, sel_row >= 0 && !hk[rows[size_t(sel_row)].index].empty());
		// the commands bound to the key in the edit box
		const IfaceRect A = hk_assigned();
		const std::vector<size_t> as = assigned_entries();
		list_rows_draw(ui, A, assigned_top, int(as.size()), [&](int i) { return cmd_label[as[size_t(i)]]; }, nullptr, -1, false);
		scrollbar_draw(ui, A, int(as.size()), A.h() / kRowH, assigned_top);
		// the notes under the lists
		const int fs = ui.font(12, -1);
		std::string note = tr("System keys may not be reassigned.");
		uint32_t nc = kGrey;
		if (!hk_note.empty() && now < note_until) note = hk_note, nc = rgb(255, 160, 120);
		ui.text(fs, note, float(ox + A.l), float(oy + A.b + 8), nc);
		if (sel_row >= 0 && !cand.empty() && as.size() > 1) {
			const std::string w2 = tr("Several commands share this key.");
			ui.text(fs, w2, float(ox + A.l), float(oy + A.b + 24), rgb(255, 200, 120));
		}
		ui.next_layer();
	}

	// ---------------------------------------------------------------- draw
	void draw(UiRenderer& ui, int x0, int y0, double now) {
		ox = x0, oy = y0;
		ui.next_window();
		const IfaceRect inner = abs(main_r);
		ui.fill(float(inner.l), float(inner.t), float(inner.w()), float(inner.h()), argb(214, 13, 11, 34), UiType::ColorBlend);
		ui.next_layer();
		ui.border(frame, abs({0, kW(), 0, 509}));
		ui.next_layer();
		// the button row: left and right end pieces and the middle tiled between them
		const IfaceRect br = abs(button_border_r);
		if (border_l.valid()) ui.add(border_l, float(br.l), float(br.t));
		if (border_r.valid()) ui.add(border_r, float(br.r - border_r.width), float(br.t));
		if (border_m.valid()) ui.tile(border_m, {br.l + border_l.width, br.r - border_r.width, br.t, br.t + border_m.height});
		ui.next_layer();
		button(ui, ok_r, tr("OK"), 10, true);
		button(ui, reset_r, tr("Reset Defaults"), 11, true);
		button(ui, cancel_r, tr("Cancel"), 12, true);
		draw_tabs(ui);
		// the pane
		ui.next_window();
		if (pane == kHotkeys) {
			hotkeys_draw(ui, now);
		} else if (pane == kPlayer) {
			draw_widgets(ui);
			const int f = ui.font(14, -1);
			const std::string note = tr("The new language is used the next time the game starts.");
			const IfaceRect r = abs(P(60, 140, 700, 24));
			ui.text(f, note, float(r.l), float(r.t), kLight);
			const std::string n2 = tr("Profiles and player names are not part of this port.");
			ui.text(f, n2, float(r.l), float(r.t + 22), kGrey);
			ui.next_layer();
		} else {
			draw_widgets(ui);
		}
		if (open_combo >= 0) {
			const IfaceRect rel = open_combo == 100 ? hk_groups() : widgets[size_t(open_combo)].r;
			static const char* fn[6] = {"All", "Commands", "Build Mode", "Building", "Unit", "City"};
			std::vector<std::string> items;
			int cur = 0;
			if (open_combo == 100) {
				for (int k = 0; k < 6; k++) items.push_back(tr(fn[k]));
				cur = filter;
			} else {
				items = widgets[size_t(open_combo)].items;
				cur = widgets[size_t(open_combo)].get();
			}
			drop_draw(ui, rel, items, cur);
		}
	}

	void draw_widgets(UiRenderer& ui) {
		for (const Widget& wd : widgets) {
			switch (wd.kind) {
				case Kind::Area: area(ui, wd.r, wd.title); break;
				case Kind::Slider: slider_draw(ui, wd); break;
				case Kind::Check: check_draw(ui, wd); break;
				case Kind::Combo: combo_draw(ui, wd.r, wd.items[size_t(std::clamp(wd.get(), 0, int(wd.items.size()) - 1))], false); break;
				case Kind::Text: {
					const IfaceRect r = abs(wd.r);
					const int f = ui.font(12, -1);
					ui.text(f, wd.title, float(r.l), float(r.t + 4), kLight);
					break;
				}
			}
		}
		ui.next_layer();
	}

	void draw_tabs(UiRenderer& ui) {
		ui.next_window();
		const int f = ui.font(14, -1);
		const ron::FontMetrics* m = ui.font_metrics(f);
		const int fh = m ? m->font_height : 16;
		static const char* names[kPaneCount] = {"Game", "Hotkeys", "Mouse", "Graphics", "Sound", "Player"};
		for (int i = 0; i < kPaneCount; i++) {
			const IfaceRect t = abs(first_tab.offset(first_tab.w() * i, 0));
			const IfacePiece* piece = tab[i == pane ? 1 : 0];
			if (piece[0].valid() && piece[1].valid() && piece[2].valid()) ui.expander(piece[0], piece[1], piece[2], t);
			ui.next_layer();
			const IfaceRect tt = abs(first_tab_text.offset(first_tab_text.w() * i, 0));
			ui.text_centered(f, tr(names[i]), {tt.l, tt.r, tt.t + (tt.h() - fh) / 2, tt.t + (tt.h() + fh) / 2}, kWhite);
			ui.next_layer();
		}
	}

	// ---------------------------------------------------------------- input
	void play(const char* key) {
		if (host.sound) host.sound(key);
	}

	void finish(bool ok) {
		if (ok) {
			commit();
		} else if (host.preview) {
			host.preview(prefs::get());
		}
		play("OPTIONS_CLOSE");
		is_open = false;
		capturing = false;
		if (host.closed) host.closed();
	}

	void reset_current() {
		prefs::reset_pane(w, pane);
		if (pane == kHotkeys) {
			for (size_t i = 0; i < keymap::count(); i++) hk[i] = keymap::entry(i).keys;
			select_row(sel_row);
		}
		if (host.preview && pane == kSound) host.preview(w);
	}

	int widget_at(int x, int y) const {
		for (size_t i = 0; i < widgets.size(); i++) {
			const Widget& wd = widgets[i];
			if ((wd.kind == Kind::Slider || wd.kind == Kind::Check || wd.kind == Kind::Combo) && wd.r.contains(x, y)) return int(i);
		}
		return -1;
	}

	bool mouse_move(int x, int y) {
		hx = x - ox, hy = y - oy;
		if (drag_widget >= 0) {
			Widget& s = widgets[size_t(drag_widget)];
			s.set(slider_value_at(s, hx));
			if (s.sound_preview && host.preview) host.preview(w);
		}
		if (drag_scroll) scroll_drag(hy);
		return is_open;
	}

	void scroll_drag(int ly) {
		const bool left = drag_scroll_list == 0;
		const IfaceRect L = left ? hk_list() : hk_assigned();
		const int total = left ? int(rows.size()) : int(assigned_entries().size());
		const int vis = L.h() / kRowH;
		if (total <= vis) return;
		const int t0 = L.t + 17, th = L.b - 17 - t0;
		const IfaceRect tr_ = thumb_rect(L, total, vis, left ? list_top : assigned_top);
		const int len = tr_.h();
		const int pos = std::clamp(ly - drag_off - t0, 0, std::max(1, th - len));
		const int top = pos * (total - vis) / std::max(1, th - len);
		(left ? list_top : assigned_top) = std::clamp(top, 0, total - vis);
	}

	bool hit_scrollbar(const IfaceRect& L, int total, int vis, int top, int x, int y, int list_id, int* new_top) {
		const IfaceRect s{L.r + 1, L.r + 17, L.t, L.b};
		if (!s.contains(x, y)) return false;
		if (y < L.t + 16) *new_top = std::max(0, top - 1);
		else if (y >= L.b - 16) *new_top = std::min(std::max(0, total - vis), top + 1);
		else {
			const IfaceRect t = thumb_rect(L, total, vis, top);
			if (t.contains(x, y)) {
				drag_scroll = true, drag_scroll_list = list_id, drag_off = y - t.t;
				*new_top = top;
			} else {
				*new_top = std::clamp(top + (y < t.t ? -vis : vis), 0, std::max(0, total - vis));
			}
		}
		return true;
	}

	bool mouse_down(int x, int y) {
		hx = x - ox, hy = y - oy;
		const int lx = hx, ly = hy;
		if (open_combo >= 0) {
			const bool hk_combo = open_combo == 100;
			const IfaceRect rel = hk_combo ? hk_groups() : widgets[size_t(open_combo)].r;
			const size_t n = hk_combo ? 6 : widgets[size_t(open_combo)].items.size();
			for (size_t k = 0; k < n; k++) {
				if (drop_item_rect(rel, k).contains(lx, ly)) {
					if (hk_combo) {
						filter = int(k);
						rebuild_rows();
						list_top = 0;
					} else {
						widgets[size_t(open_combo)].set(int(k));
					}
					play("OPTIONS_BUTTON");
					break;
				}
			}
			open_combo = -1;
			return true;
		}
		for (int i = 0; i < kPaneCount; i++) {
			if (first_tab.offset(first_tab.w() * i, 0).contains(lx, ly)) {
				if (i != pane) {
					set_pane(i);
					play("OPTIONS_TAB");
				}
				return true;
			}
		}
		pressed = -1;
		if (ok_r.contains(lx, ly)) pressed = 10;
		else if (reset_r.contains(lx, ly)) pressed = 11;
		else if (cancel_r.contains(lx, ly)) pressed = 12;
		if (pressed >= 0) return true;
		if (pane == kHotkeys) return hotkeys_down(lx, ly);
		const int wi = widget_at(lx, ly);
		if (wi < 0) return true;
		Widget& wd = widgets[size_t(wi)];
		if (wd.kind == Kind::Slider) {
			drag_widget = wi;
			wd.set(slider_value_at(wd, lx));
			if (wd.sound_preview && host.preview) host.preview(w);
		} else if (wd.kind == Kind::Check) {
			wd.set(wd.get() ? 0 : 1);
			play("OPTIONS_BUTTON");
		} else if (wd.kind == Kind::Combo) {
			open_combo = wi;
		}
		return true;
	}

	bool hotkeys_down(int lx, int ly) {
		const IfaceRect L = hk_list(), A = hk_assigned(), E = hk_edit(), G = hk_groups();
		if (G.contains(lx, ly)) {
			open_combo = 100;
			return true;
		}
		int nt = 0;
		if (hit_scrollbar(L, int(rows.size()), L.h() / kRowH, list_top, lx, ly, 0, &nt)) return list_top = nt, true;
		const int na = int(assigned_entries().size());
		if (hit_scrollbar(A, na, A.h() / kRowH, assigned_top, lx, ly, 1, &nt)) return assigned_top = nt, true;
		if (L.contains(lx, ly)) {
			const int r = list_top + (ly - L.t) / kRowH;
			if (r >= 0 && r < int(rows.size())) {
				select_row(r);
				play("OPTIONS_BUTTON");
			}
			return true;
		}
		if (E.contains(lx, ly)) {
			if (sel_row >= 0) capturing = !capturing;
			return true;
		}
		if (hk_assign().contains(lx, ly)) pressed = 40;
		else if (hk_remove().contains(lx, ly)) pressed = 41;
		return true;
	}

	bool mouse_up(int x, int y) {
		hx = x - ox, hy = y - oy;
		const int was = pressed;
		pressed = -1;
		drag_widget = -1;
		drag_scroll = false;
		if (was < 0) return is_open;
		const bool over = was == 10 ? ok_r.contains(hx, hy) : was == 11 ? reset_r.contains(hx, hy) : was == 12 ? cancel_r.contains(hx, hy)
		                  : was == 40 ? hk_assign().contains(hx, hy) : hk_remove().contains(hx, hy);
		if (!over) return true;
		play("OPTIONS_BUTTON");
		if (was == 10) finish(true);
		else if (was == 12) finish(false);
		else if (was == 11) reset_current();
		else if (was == 40) assign_current();
		else if (was == 41) remove_current();
		return true;
	}

	void assign_current() {
		if (sel_row < 0) return;
		hk[rows[size_t(sel_row)].index] = cand;
		capturing = false;
	}
	void remove_current() {
		if (sel_row < 0) return;
		hk[rows[size_t(sel_row)].index].clear();
		cand.clear();
		capturing = false;
	}

	bool wheel(double dy, int x, int y) {
		hx = x - ox, hy = y - oy;
		if (pane != kHotkeys) return is_open;
		const IfaceRect L = hk_list(), A = hk_assigned();
		const int step = dy > 0 ? -3 : 3;
		if (IfaceRect{L.l, L.r + 18, L.t, L.b}.contains(hx, hy)) {
			list_top = std::clamp(list_top + step, 0, std::max(0, int(rows.size()) - L.h() / kRowH));
		} else if (IfaceRect{A.l, A.r + 18, A.t, A.b}.contains(hx, hy)) {
			assigned_top = std::clamp(assigned_top + step, 0, std::max(0, int(assigned_entries().size()) - A.h() / kRowH));
		}
		return is_open;
	}

	bool key(unsigned short code, unsigned ch, bool ctrl, bool alt, bool shift, double now) {
		if (capturing) {
			if (code == 53) {  // Esc leaves the edit box
				capturing = false;
				cand = sel_row >= 0 ? hk[rows[size_t(sel_row)].index] : std::string();
				return true;
			}
			const std::string k = keymap::key_string(code, ctrl, alt, shift);
			if (k.empty()) return true;  // a modifier on its own
			if (k == "F10" || k == "Esc") {
				hk_note = tr("System keys may not be reassigned."), note_until = now + 3;
				return true;
			}
			cand = k;
			capturing = false;
			assigned_top = 0;
			return true;
		}
		if (open_combo >= 0) {
			if (code == 53) open_combo = -1;
			return true;
		}
		if (code == 53) {  // Esc: Cancel
			finish(false);
			return true;
		}
		if (code == 36 || code == 76) {  // Return: OK
			finish(true);
			return true;
		}
		if (code == 48) {  // Tab / Shift+Tab: the next / previous pane
			set_pane((pane + (shift ? kPaneCount - 1 : 1)) % kPaneCount);
			play("OPTIONS_TAB");
			return true;
		}
		if (pane == kHotkeys && !rows.empty()) {
			if (ch == 0xF700 || ch == 0xF701) {  // Up / Down arrows move the selection
				select_row(std::clamp(sel_row < 0 ? 0 : sel_row + (ch == 0xF700 ? -1 : 1), 0, int(rows.size()) - 1));
				const int vis = hk_list().h() / kRowH;
				if (sel_row < list_top) list_top = sel_row;
				if (sel_row >= list_top + vis) list_top = sel_row - vis + 1;
				return true;
			}
		}
		return true;
	}
};

// ---------------------------------------------------------------- the public face

Page::Page(const ron::GameFiles& files, const ron::IfaceLayout& iface) : p_(new Impl(files, iface)) {
	if (!p_->init()) p_->root = nullptr;
}
Page::~Page() = default;
bool Page::ready() const { return p_->root != nullptr; }
bool Page::is_open() const { return p_->is_open; }
int Page::pane() const { return p_->pane; }

void Page::open(const Host& host, int pane) {
	if (!ready()) return;
	p_->host = host;
	p_->load_working();
	p_->is_open = true;
	p_->pressed = p_->drag_widget = -1;
	p_->drag_scroll = false;
	set_pane(pane);
	p_->play("OPTIONS_OPEN");
}

void Page::close() { p_->is_open = false; }

void Page::set_pane(int pane) { p_->set_pane(pane); }

void Page::draw(ron::metal::UiRenderer& ui, int ox, int oy) {
	if (!p_->is_open) return;
	static double t0 = -1;
	(void)t0;
	p_->draw(ui, ox, oy, double(CFAbsoluteTimeGetCurrent()));
}

bool Page::mouse_move(int x, int y) { return p_->mouse_move(x, y); }
bool Page::mouse_down(int x, int y) { return p_->is_open && p_->mouse_down(x, y); }
bool Page::mouse_drag(int x, int y) { return p_->mouse_move(x, y); }
bool Page::mouse_up(int x, int y) { return p_->is_open && p_->mouse_up(x, y); }
bool Page::wheel(double dy, int x, int y) { return p_->is_open && p_->wheel(dy, x, y); }
bool Page::key(unsigned short code, unsigned ch, bool ctrl, bool alt, bool shift) {
	return p_->is_open && p_->key(code, ch, ctrl, alt, shift, double(CFAbsoluteTimeGetCurrent()));
}

void Page::test_select_hotkey(const std::string& option_name) {
	for (size_t r = 0; r < p_->rows.size(); r++) {
		if (option_name == keymap::entry(p_->rows[r].index).option) {
			p_->select_row(int(r));
			p_->list_top = std::clamp(int(r) - 2, 0, std::max(0, int(p_->rows.size()) - 22));
			return;
		}
	}
}

void Page::test_open_combo(int index) {
	if (index == 100) p_->open_combo = 100;
	else if (index >= 0 && size_t(index) < p_->widgets.size()) p_->open_combo = index;
	else p_->open_combo = -1;
}

std::string Page::state_text() const {
	char b[256];
	std::snprintf(b, sizeof b, "pane=%d speed=%d diff=%d help=%d scroll=%d fx=%d music=%d streamlined=%d", p_->pane, p_->w.game_speed, p_->w.difficulty,
	              p_->w.help_detail, p_->w.scroll_speed, p_->w.fx_volume, p_->w.music_volume, int(p_->w.streamlined));
	return b;
}

}  // namespace optui
