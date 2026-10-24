// HUD panels added in the second pass (docs/ui-port.md sections 6 and 12): the rare-goods column, the production progress
// buttons, the library grid (IFaceLibrary), the garrison grid (IFaceGarrison) and the market's buy / sell buttons
// (IFaceMarket::on_button_redraw). Decompiles: re/decomp/ui_panels.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "assets/xml.hpp"
#include "hud.hpp"
#include "uistr.hpp"

using ron::IfaceBorder;
using ron::IfacePiece;
using ron::IfaceRect;
using ron::metal::UiRenderer;
using ron::metal::UiType;
using ron::metal::argb;
using ron::metal::rgb;

namespace hud {

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000, kRed = rgb(255, 0, 0);

IfaceRect rect_in(const ron::XmlElement& win, const char* tag, const std::string& name, int pw, int ph, int nth = 0) {
	IfaceRect r;
	ron::iface_find_rect(win, tag, name, pw, ph, r, nth);
	return r;
}

// IFaceButton icon cell (ui-spec 6.4): (tex_x, tex_y) when both >= 0, else 62 * col, 40 * row; 61 x 39 (30 x 19 small).
IfacePiece icon_cell(const std::string& tex_id, int col, int row, int tx, int ty, bool small) {
	IfacePiece p;
	p.texture = tex_id;
	if (tx >= 0 && ty >= 0) p.left = tx, p.top = ty;
	else p.left = (small ? 31 : 62) * col, p.top = (small ? 20 : 40) * row;
	p.width = small ? 30 : 61;
	p.height = small ? 19 : 39;
	return p;
}

}  // namespace

// ---------------------------------------------------------------- rare goods

// IFaceMainBase::draw_special_resources @0080ef80: the rare goods owned, one icon each (the TEX2 rectangle of the good in
// resourcerules.xml) at half alpha, in columns of eight from x = 10, y = the resource panel's bottom + 20, 4 px apart, the
// count (when more than one) in digit set 0 at the icon's bottom right.
void Hud::draw_rares(UiRenderer& ui, const State& s) {
	rare_cols_ = 0;
	rare_rects_.clear();
	if (s.rares.empty() || !opened_) return;
	ui.next_window();
	int x = 10;
	const int base = res_win_.b + 20;
	int y = base, n = 0;
	for (const State::Rare& r : s.rares) {
		if (r.good < 0 || size_t(r.good) >= rare_icons_.size() || rare_icons_[size_t(r.good)].tex_id.empty()) continue;
		const RareIcon& ic = rare_icons_[size_t(r.good)];
		IfacePiece p;
		p.texture = ic.tex_id == "TEX_SPECIAL_RESOURCES" ? std::string("TEX_GOOD_ICONS") : ic.tex_id;
		p.left = ic.x, p.top = ic.y, p.width = ic.w, p.height = ic.h;
		ui.add(p, float(x), float(y), 0, 0, argb(0x80, 255, 255, 255), UiType::Alpha);
		if (r.count > 1) {
			const std::string t = std::to_string(r.count);
			ui.write_digits(t, float(x + ic.w - ui.digits_width(t, 0) - 1), float(y + ic.h - ui.digits_height(0) - 1), kWhite, 0);
		}
		rare_rects_.push_back({IfaceRect{x, x + ic.w, y, y + ic.h}, r.good});
		y += ic.h + 4;
		if (++n % 8 == 0) {
			x += ic.w + 4;
			y = base;
		}
	}
	rare_cols_ = (n + 7) / 8;
	ui.next_layer();
}

// ---------------------------------------------------------------- progress buttons

// setup_progress_buttons @0080db10 (only with pref bit 0x40): one 61 x 39 button per queued unit or tech of the viewer's
// buildings, in columns down the right edge from the bottom of the timers, 6 px apart; the first column holds six, the
// next ones seven (the decompile counts before it tests); a new column goes to the left. percent_done = job counter /
// train time, the outline the owner's colour.
IfaceRect Hud::progress_rect(int i, int top) const {
	const int w = 61, h = 39;
	int col = 0, row = 0, cnt = 0;
	for (int k = 0; k <= i; k++) {
		if (++cnt > 6) cnt = 0, col++, row = 0;
		if (k == i) break;
		row++;
	}
	const int x = ui_w_ - w - 12 - col * (w + 6), y = top + row * (h + 6);
	return {x, x + w, y, y + h};
}

int Hud::progress_index_at(int x, int y, const State& s) const {
	if (!progress_buttons_) return -1;
	for (size_t i = 0; i < s.progress.size(); i++) {
		if (progress_rect(int(i), timers_bottom_).contains(x, y)) return int(i);
	}
	return -1;
}

void Hud::draw_progress(UiRenderer& ui, const State& s) {
	progress_rects_.clear();
	if (!progress_buttons_ || s.progress.empty() || !opened_) return;
	ui.next_window();
	for (size_t i = 0; i < s.progress.size(); i++) {
		const ProgressItem& p = s.progress[i];
		const IfaceRect r = progress_rect(int(i), timers_bottom_);
		progress_rects_.push_back(r);
		const bool hover = r.contains(hover_x_, hover_y_);
		draw_icon_button(ui, p.tex_id, p.tex_col, p.tex_row, p.tex_x, p.tex_y, r, hover ? 0xa4 : 0x80, false);
		ui.next_layer();
		ui.fill(float(r.l), float(r.t), float(r.w() * std::clamp(p.percent, 0, 100) / 100), float(r.h()), argb(0x80, 0, 255, 0), UiType::ColorBlend);
		ui.frame(float(r.l - 1), float(r.t - 1), float(r.w() + 2), float(r.h() + 2), rgb(p.rgb[0], p.rgb[1], p.rgb[2]));
		ui.next_layer();
	}
}

// ---------------------------------------------------------------- market buttons

// IFaceMarket::on_button_redraw @008131e0: an OPTION_BUY / OPTION_SELL button of the Market shows the good's icon
// (interface.txt #IFACEMARKET BASE_GOOD_TEX, TEX_SPECIAL_RESOURCES 62,160 61 x 39, one cell right per good; ALPHA, GREY when
// disabled), the price in digit set 1 centred (WHITE, RED when disabled) and the trend arrow of Game::delta_flux (up / neutral /
// down, two thirds size). The arrow's position is not in the decompile: centred at the button's bottom.
void Hud::draw_market_button(UiRenderer& ui, const Option& o, const IfaceRect& r, int factor) {
	const bool disabled = !o.enabled;
	IfacePiece base = ui.interface_txt().piece("IFACEMARKET", "BASE_GOOD_TEX");
	if (!base.valid()) {
		base.left = 62, base.top = 160, base.width = 61, base.height = 39;
	}
	base.texture = "TEX_GOOD_ICONS";
	base.left += (base.width + 1) * std::max(0, o.good);
	(void)factor;
	ui.add(base, float(r.l), float(r.t), float(r.w()), float(r.h()), disabled ? rgb(160, 160, 160) : kWhite, UiType::Alpha);
	ui.next_layer();
	const std::string t = std::to_string(o.price);
	ui.write_digits(t, float(r.l + (r.w() - ui.digits_width(t, 1)) / 2), float(r.t + (r.h() - ui.digits_height(1)) / 2), disabled ? kRed : kWhite, 1);
	const char* arrow = o.trend < 0 ? "ARROW_DOWN_TEX" : o.trend > 0 ? "ARROW_UP_TEX" : "NEUTRAL_TEX";
	IfacePiece a = ui.interface_txt().piece("IFACEMARKET", arrow);
	if (a.valid()) {
		const int aw = a.width * 2 / 3, ah = std::max(2, a.height * 2 / 3);
		ui.add(a, float(r.l + (r.w() - aw) / 2), float(r.b - ah - 3), float(aw), float(ah));
	}
	if (!o.label.empty()) {  // the row head ("Sell:" / "Buy:") over the first button of the row
		const int f = ui.font(10, ron::kFontBold);
		ui.text(f, o.label, float(r.l + 3), float(r.t + 2), kWhite, kBlack);
	}
}

// ---------------------------------------------------------------- library

namespace {
const char* kLineTech[4] = {"tech0", "tech1", "tech2", "tech3"};
}

int Hud::library_cell_at(int x, int y, const State& s, int* cat_out, int* col_out) const {
	if (!s.library.active || !lib_el_) return -1;
	const ron::XmlElement& e = *lib_el_;
	const IfaceRect w = library_rect_;
	const int pw = w.w(), ph = w.h();
	for (int c = 0; c < 7; c++) {
		if (rect_in(e, "BUTTON", "age", pw, ph, c).offset(w.l, w.t).contains(x, y)) {
			if (cat_out) *cat_out = -1;
			if (col_out) *col_out = c;
			return s.library.age[c].type;
		}
		for (int k = 0; k < 4; k++) {
			if (rect_in(e, "BUTTON", kLineTech[k], pw, ph, c).offset(w.l, w.t).contains(x, y)) {
				if (cat_out) *cat_out = k;
				if (col_out) *col_out = c;
				return s.library.tech[k][c].type;
			}
		}
	}
	return -1;
}

// IFaceLibrary::on_redraw @008071d0 / on_button_redraw @00807b00 / draw_button_underlays @00806960: the civ-skinned `back`
// (TEX_LIBRARY_BACK, 319 x 168) painted with the grid, an age button (36 x 36) over `age_underlay` per column, the four lines'
// level buttons (36 x 28) under it, the category buttons down the first column; a researched cell shows the `*_overlay1`
// frame, an unreachable one is dimmed, and a research in progress fills `age_progress` / `tech_progress` up the cell.
void Hud::draw_library(UiRenderer& ui, const State& s) {
	if (!lib_el_ || !s.library.active) return;
	const ron::XmlElement& e = *lib_el_;
	library_rect_ = IfaceRect{left_.l + (left_.w() - 319) / 2, left_.l + (left_.w() - 319) / 2 + 319, left_.t, left_.t + 168};
	if (s.library.final_view) return draw_library_final(ui, s);
	const IfaceRect w = library_rect_;
	const int pw = w.w(), ph = w.h();
	ui.add(ron::iface_find_piece(e, "back"), float(w.l), float(w.t), 0, 0, kWhite, UiType::Normal);
	ui.next_layer();
	const IfacePiece under = ron::iface_find_piece(e, "age_underlay");
	auto cell = [&](const LibCell& c, const IfaceRect& rc, bool age) {
		if (c.type < 0) return;
		const bool hover = rc.contains(hover_x_, hover_y_);
		int factor = c.owned ? 0x80 : c.enabled ? (hover ? 0xa4 : 0x80) : 0x40;
		if (!c.tex_id.empty()) {
			const IfacePiece p = icon_cell(c.tex_id, c.tex_col, c.tex_row, c.tex_x, c.tex_y, false);
			ui.add(p, float(rc.l), float(rc.t), float(rc.w()), float(rc.h()), rgb(factor, factor, factor), UiType::Highlight);
		}
		ui.next_layer();
		const IfacePiece ov = ron::iface_find_piece(e, age ? (c.owned ? "age_overlay1" : "age_overlay0") : (c.owned ? "cat_overlay1" : "cat_overlay0"));
		if (ov.valid()) ui.add(ov, float(rc.l), float(rc.t), float(rc.w()), float(rc.h()), kWhite, UiType::Alpha);
		if (c.percent >= 0) {
			const IfacePiece pb = ron::iface_find_piece(e, age ? "age_progress" : "tech_progress");
			const int fill = pb.height * std::clamp(c.percent, 0, 100) / 100;
			ui.fill(float(rc.l), float(rc.t), float(pb.width), float(rc.h()), argb(0x90, 0, 0, 0), UiType::ColorBlend);
			ui.add_sub(pb, 0, float(pb.height - fill), float(pb.width), float(fill), float(rc.l), float(rc.b - fill), float(pb.width), float(fill));
		}
		ui.next_layer();
	};
	for (int c = 0; c < 7; c++) {
		const IfaceRect ar = rect_in(e, "BUTTON", "age", pw, ph, c).offset(w.l, w.t);
		if (under.valid()) ui.add(under, float(ar.l - 5), float(ar.t - 1));
		cell(s.library.age[c], ar, true);
		for (int k = 0; k < 4; k++) cell(s.library.tech[k][c], rect_in(e, "BUTTON", kLineTech[k], pw, ph, c).offset(w.l, w.t), false);
	}
	ui.next_layer();
	// The category column (cat_age / cat 0..3): a lit frame under the mouse while the line can research, and the progress of the
	// next tech filling the cell in the line's colour (update_visibility: percent_done, get_tech_color).
	static const uint32_t line_col[4] = {rgb(255, 40, 40), rgb(0, 255, 255), rgb(0, 255, 0), rgb(255, 255, 0)};
	for (int i = 0; i < 5; i++) {
		const MiniLibButton& m = i == 0 ? s.minilib.age : s.minilib.cat[i - 1];
		if (!m.valid) continue;
		const IfaceRect rc = i == 0 ? rect_in(e, "BUTTON", "cat_age", pw, ph).offset(w.l, w.t) : rect_in(e, "BUTTON", "cat", pw, ph, i - 1).offset(w.l, w.t);
		if (m.percent >= 0) {
			const int h = rc.h() * std::clamp(m.percent, 0, 100) / 100;
			ui.fill(float(rc.l), float(rc.b - h), float(rc.w()), float(h), ((i == 0 ? rgb(255, 255, 255) : line_col[i - 1]) & 0x00FFFFFF) | 0x70000000, UiType::ColorBlend);
		} else if (m.enabled && rc.contains(hover_x_, hover_y_)) {
			ui.frame(float(rc.l), float(rc.t), float(rc.w()), float(rc.h()), rgb(255, 255, 255));
		}
	}
	ui.next_layer();
}

// ---------------------------------------------------------------- garrison

// The window right of the selection panel (IFaceMain::position_windows): from the panel's right edge to the display button.
IfaceRect Hud::garrison_rect(bool air) const {
	if (!opened_) {
		// Streamlined (IFaceMain::position_windows): the window sits at x = 25 of the right slot, level with the selection window. The
		// air panel is 50 px of buttons in the narrow layout (the computed width is under 101), the plain garrison grid keeps the 294 of
		// its window.
		const int l = right_.l + 25 - 4, r = air ? l + 58 : std::min(right_.r - 2, l + 294);
		return {l, r, right_.t, right_.b};
	}
	const int l = sel_win_.r + 2, r = std::max(l + 40, display_btn_.l - 4);
	return {l, r, right_.t, right_.b};
}

// IFaceGarrison::update_visibility @00806170: 50 x 30 cells (2 px apart) filled column-major; with more units than cells they
// shrink by k (the smallest with k^2 cells >= count) and use the small icons. Units of one type and owner merge into a counted
// button; with several owners each button is outlined in its owner's colour (on_button_redraw @00805cc0).
int Hud::garrison_index_at(int x, int y, const State& s, IfaceRect* cell_out) const {
	if (!s.garrison.active) return -1;
	const IfaceRect g = garrison_rect();
	const IfaceRect area{g.l + 4, g.r - 4, g.t + 4, g.b - 4};
	const int cw = 50, chh = 30, sp = 2;
	const int nx = std::max(1, (area.w() + sp) / (cw + sp)), ny = std::max(1, (area.h() + sp) / (chh + sp));
	const int count = int(s.garrison.units.size());
	int k = 1;
	while (count > k * k * nx * ny) k++;
	const int w = cw / k, h = chh / k;
	for (int i = 0; i < count; i++) {
		const int col = i / (ny * k), row = i % (ny * k);
		const IfaceRect rc{area.l + col * (w + sp / k), area.l + col * (w + sp / k) + w, area.t + row * (h + sp / k), area.t + row * (h + sp / k) + h};
		if (rc.r > area.r) break;
		if (rc.contains(x, y)) {
			if (cell_out) *cell_out = rc;
			return i;
		}
	}
	return -1;
}

void Hud::draw_garrison(UiRenderer& ui, const State& s) {
	if (!s.garrison.active) return;
	const ron::XmlElement* ge = layout_->find("IFACEGARRISON");
	const IfaceRect g = garrison_rect();
	ui.next_window();
	if (ge) {
		ui.tile(ron::iface_find_piece(*sel_el_, "back"), g, kWhite, UiType::Normal);
		ui.next_layer();
		const ron::IfaceBorder bd = ron::iface_find_border(*ge, "back");
		ui.border(bd, g);
		ui.next_layer();
	}
	const IfaceRect area{g.l + 4, g.r - 4, g.t + 4, g.b - 4};
	const int cw = 50, chh = 30, sp = 2;
	const int nx = std::max(1, (area.w() + sp) / (cw + sp)), ny = std::max(1, (area.h() + sp) / (chh + sp));
	const int count = int(s.garrison.units.size());
	int k = 1;
	while (count > k * k * nx * ny) k++;
	const bool small = k > 1;
	for (int i = 0; i < count; i++) {
		IfaceRect rc;
		const int w = cw / k, h = chh / k;
		const int col = i / (ny * k), row = i % (ny * k);
		rc = IfaceRect{area.l + col * (w + sp / k), area.l + col * (w + sp / k) + w, area.t + row * (h + sp / k), area.t + row * (h + sp / k) + h};
		if (rc.r > area.r) break;
		const GarrisonUnit& u = s.garrison.units[size_t(i)];
		if (s.garrison.mixed_owners) ui.fill(float(rc.l - 1), float(rc.t - 1), float(rc.w() + 2), float(rc.h() + 2), rgb(u.rgb[0], u.rgb[1], u.rgb[2]), UiType::Normal);
		ui.next_layer();
		const bool hover = rc.contains(hover_x_, hover_y_);
		draw_icon_button(ui, u.tex_id, u.tex_col, u.tex_row, u.tex_x, u.tex_y, rc, !u.own ? 0x40 : hover ? 0xa4 : 0x80, small);
		ui.next_layer();
		if (u.count > 1) {
			const std::string t = std::to_string(u.count);
			ui.write_digits(t, float(rc.r - ui.digits_width(t, 1) - 2), float(rc.b - ui.digits_height(1) - 2), kWhite, 1);
		}
		ui.next_layer();
	}
}

}  // namespace hud
