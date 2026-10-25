// HUD panels of the third pass (docs/ui-port.md section 13): the air garrison (IFaceAirGarrison, re/decomp/ui_air) and the
// library's final-techs view and category column (IFaceLibrary, re/decomp/ui_panels).
#include <algorithm>
#include <cmath>

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

constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000;

IfaceRect rect_in(const ron::XmlElement& win, const char* tag, const std::string& name, int pw, int ph, int nth = 0) {
	return cached_iface_rect(win, tag, name, pw, ph, nth);
}

}  // namespace

// ---------------------------------------------------------------- air garrison

// IFaceAirGarrison::update_visibility @007fb320 / on_redraw @007fad20. Wide (>= 126 px, the original switches at 101 for its own
// 60 px cells): two columns, ready (full fuel) and fuelling, under their `meter` headers (59 x 32), one 60 x 40 button per family
// (fighters, bombers, helicopters) below, 2 px apart. Narrow: the `small_meter_*` strips (50 x 15) and 50 x 30 buttons, a block
// for the ready planes over a block for the fuelling ones, the families side by side; with helicopters inside a single block.
Hud::AirLayout Hud::air_layout(const State& s) const {
	AirLayout L;
	const IfaceRect g = garrison_rect(true);
	L.win = IfaceRect{g.l + 4, g.r - 4, g.t + 1, g.b - 1};
	L.small = L.win.w() < 126;
	const int sp = 2;
	if (!L.small) {
		const int bw = 60, bh = 40, mh = 32;
		for (int c = 0; c < 2; c++) {
			const int x = L.win.l + c * (bw + sp);
			L.head[c] = IfaceRect{x, x + 59, L.win.t, L.win.t + mh};
			for (int r = 0; r < 3; r++) {
				const int y = L.win.t + mh + sp + r * (bh + sp);
				L.cell[r][c] = IfaceRect{x, x + bw, y, y + bh};
			}
		}
		L.rows = 3;
		return L;
	}
	const int bw = 50, bh = 30, mh = 15;
	const int blocks = s.air.helis ? 1 : 2;
	const int fam = s.air.helis ? 3 : 2;
	// update_visibility, draw_small: one column; per block (ready, then fuelling) the 15 px header and the families stacked
	// below it (fighters, bombers; helicopters too when there are some, which then drop the fuelling block).
	for (int b = 0; b < blocks; b++) {
		const int y0 = L.win.t + b * (mh + sp + fam * (bh + sp));
		L.head[b] = IfaceRect{L.win.l, L.win.l + bw, y0, y0 + mh};
		for (int r = 0; r < fam; r++) {
			const int y = y0 + mh + sp + r * (bh + sp);
			L.cell[r][b] = IfaceRect{L.win.l, L.win.l + bw, y, y + bh};
		}
	}
	L.rows = fam;
	return L;
}

int Hud::air_cell_at(int x, int y, const State& s, int* col) const {
	if (!s.air.active) return -1;
	const AirLayout L = air_layout(s);
	const int blocks = L.small && s.air.helis ? 1 : 2;
	for (int r = 0; r < L.rows; r++) {
		for (int c = 0; c < blocks; c++) {
			if (!L.small && r == 2 && !s.air.helis) continue;
			if (L.cell[r][c].contains(x, y)) {
				if (col) *col = c;
				return r;
			}
		}
	}
	return -1;
}

int Hud::air_header_at(int x, int y, const State& s) const {
	if (!s.air.active) return -1;
	const AirLayout L = air_layout(s);
	const int blocks = L.small && s.air.helis ? 1 : 2;
	for (int c = 0; c < blocks; c++) {
		if (L.head[c].contains(x, y)) return c;
	}
	return -1;
}

void Hud::draw_air_garrison(UiRenderer& ui, const State& s) {
	if (!s.air.active) return;
	const ron::XmlElement* ae = layout_->find("IFACEAIRGARRISON");
	const ron::XmlElement* ge = layout_->find("IFACEGARRISON");
	if (!ae) return;
	const IfaceRect g = garrison_rect(true);
	ui.next_window();
	if (ge) {
		ui.tile(ron::iface_find_piece(*sel_el_, "back"), g, kWhite, UiType::Normal);
		ui.next_layer();
		ui.border(ron::iface_find_border(*ge, "back"), g);
		ui.next_layer();
	}
	const AirLayout L = air_layout(s);
	const int blocks = L.small && s.air.helis ? 1 : 2;
	for (int c = 0; c < blocks; c++) {
		const IfacePiece m = L.small ? ron::iface_find_piece(*ae, c == 0 ? "small_meter_ready" : "small_meter_fueling")
		                             : ron::iface_find_piece(*ae, "meter", c);
		if (m.valid()) ui.add(m, float(L.head[c].l), float(L.head[c].t));
	}
	ui.next_layer();
	for (int r = 0; r < L.rows; r++) {
		for (int c = 0; c < blocks; c++) {
			if (!L.small && r == 2 && !s.air.helis) continue;
			const AirCell& cell = s.air.cell[r][c];
			const IfaceRect rc = L.cell[r][c];
			const bool hover = rc.contains(hover_x_, hover_y_);
			const Icon& ic = s.air.icon[r];
			// helicopters never fuel: with them in the small layout the one block counts every one
			const int count = L.small && s.air.helis ? s.air.cell[r][0].count + s.air.cell[r][1].count : cell.count;
			const int factor = !s.air.own || count == 0 ? 0x40 : hover ? 0xa4 : 0x80;
			draw_icon_button(ui, ic.tex_id, ic.col, ic.row, ic.x, ic.y, rc, factor, L.small);
			ui.next_layer();
			if (cell.count > 0 && c == 1 && !L.small) {  // the fuelling cell's mean fuel
				const int bh = 4, bx = rc.l + 3, bw = rc.w() - 6, by = rc.b - 3 - bh;
				const int pct = std::clamp(cell.fuel, 0, 100);
				ui.fill(float(bx), float(by), float(bw), float(bh), kBlack, UiType::Normal);
				const uint32_t col = pct >= 60 ? rgb(0, 255, 0) : pct >= 25 ? rgb(255, 255, 0) : rgb(255, 0, 0);
				ui.fill(float(bx + 1), float(by + 1), float((bw - 2) * pct / 100), float(bh - 2), col, UiType::Normal);
				ui.next_layer();
			}
			if (count > 0) {
				const std::string t = std::to_string(count);
				ui.write_digits(t, float(rc.r - ui.digits_width(t, 1) - 3), float(rc.b - ui.digits_height(1) - (c == 1 && !L.small ? 9 : 3)), kWhite, 1);
			}
			ui.next_layer();
		}
	}
}

// ---------------------------------------------------------------- library: category column, final techs

// IFaceLibrary: the column of line buttons at the left of the grid (`cat_age` 34 x 38 and `cat` id 0..3, 34 x 28) researches the
// next age / the next tech of the line, as the mini library (on_button_clicked: OPTION_RESEARCH with the button's tech); the hover
// is OPTION 0xaf ("Category of research"). `which` is the mini library's index: 0 the age, 1..4 military, civic, commerce, science.
int Hud::cat_button_at(int x, int y, int* which) const {
	if (!lib_el_) return -1;
	const IfaceRect w = library_rect_;
	if (rect_in(*lib_el_, "BUTTON", "cat_age", w.w(), w.h()).offset(w.l, w.t).contains(x, y)) {
		if (which) *which = 0;
		return 0;
	}
	for (int k = 0; k < 4; k++) {
		if (rect_in(*lib_el_, "BUTTON", "cat", w.w(), w.h(), k).offset(w.l, w.t).contains(x, y)) {
			if (which) *which = k + 1;
			return k + 1;
		}
	}
	return -1;
}

int Hud::final_button_at(int x, int y, const State& s) const {
	if (!lib_el_ || !s.library.active || !s.library.final_view) return -1;
	const IfaceRect w = library_rect_;
	for (int k = 0; k < 4; k++) {
		if (rect_in(*lib_el_, "BUTTON", "final_jep", w.w(), w.h(), k).offset(w.l, w.t).contains(x, y)) return k;
	}
	return -1;
}

// IFaceLibrary::on_redraw (jep_back0) and on_button_redraw for the `final_jep` buttons @00807b00: the back, then per final tech
// (579 + line) the button art: researched = the bright art, the star and the complete overlay; in production = the dim art (the
// next row of the sheet), the overlay and a bar filled with the progress in the line's colour (military red, civic cyan, commerce
// green, science yellow); available = the dim art and the overlay; otherwise only the hover shows the bright art.
void Hud::draw_library_final(UiRenderer& ui, const State& s) {
	if (!lib_el_) return;
	const ron::XmlElement& e = *lib_el_;
	const IfaceRect w = library_rect_;
	const int pw = w.w(), ph = w.h();
	ui.add(ron::iface_find_piece(e, "jep_back0"), float(w.l), float(w.t), 0, 0, kWhite, UiType::Normal);
	ui.next_layer();
	static const uint32_t tint[4] = {rgb(255, 40, 40), rgb(0, 255, 255), rgb(0, 255, 0), rgb(255, 255, 0)};
	for (int k = 0; k < 4; k++) {
		const LibCell& c = s.library.fin[k];
		const IfaceRect b = rect_in(e, "BUTTON", "final_jep", pw, ph, k).offset(w.l, w.t);
		const bool hover = b.contains(hover_x_, hover_y_);
		IfacePiece art = ron::iface_find_piece(e, "jep_tech_button", k);
		if (!art.valid()) continue;
		if (c.owned || c.percent > 0) {
			IfacePiece shown = art;
			if (!c.owned) shown.top += shown.height + 1;  // a tech in production shows the dim art (the next row of the sheet)
			ui.add(shown, float(b.l), float(b.t));
			ui.next_layer();
			if (c.owned) {
				const IfaceRect star = rect_in(e, "IFACERECT", "jep_star", b.w(), b.h()).offset(b.l, b.t);
				ui.add(ron::iface_find_piece(e, "jep_star"), float(star.l), float(star.t), float(star.w()), float(star.h()));
				const IfaceRect ov = rect_in(e, "IFACERECT", "jep_overlay_complete", b.w(), b.h()).offset(b.l, b.t);
				ui.add(ron::iface_find_piece(e, "jep_overlay", k), float(ov.l), float(ov.t), float(ov.w()), float(ov.h()));
			} else {
				const IfaceRect ov = rect_in(e, "IFACERECT", "jep_overlay", b.w(), b.h()).offset(b.l, b.t);
				ui.add(ron::iface_find_piece(e, "jep_overlay", k), float(ov.l), float(ov.t), float(ov.w()), float(ov.h()));
				const IfaceRect bar = rect_in(e, "IFACERECT", "jep_bar", b.w(), b.h()).offset(b.l, b.t);
				ui.add(ron::iface_find_piece(e, "jep_bar", k), float(bar.l), float(bar.t), float(bar.w()), float(bar.h()));
				const IfaceRect fill = rect_in(e, "IFACERECT", "jep_bar_fill", b.w(), b.h()).offset(b.l, b.t);
				const IfacePiece fp = ron::iface_find_piece(e, "jep_bar_fill");
				const int fw = fill.w() * std::clamp(c.percent, 0, 100) / 100;
				if (fp.valid() && fw > 0) ui.add_sub(fp, 0, 0, float(fp.width * std::clamp(c.percent, 0, 100) / 100), float(fp.height), float(fill.l), float(fill.t), float(fw), float(fill.h()), tint[k], UiType::Alpha);
			}
		} else if (c.enabled) {
			art.top += art.height + 1;  // the lit variant is the next row of the sheet
			ui.add(art, float(b.l), float(b.t), 0, 0, hover ? kWhite : rgb(0xd0, 0xd0, 0xd0), UiType::Alpha);
			ui.next_layer();
			const IfaceRect ov = rect_in(e, "IFACERECT", "jep_overlay", b.w(), b.h()).offset(b.l, b.t);
			ui.add(ron::iface_find_piece(e, "jep_overlay", k), float(ov.l), float(ov.t), float(ov.w()), float(ov.h()));
		} else if (hover) {
			ui.add(art, float(b.l), float(b.t));
		}
		ui.next_layer();
	}
}

}  // namespace hud
