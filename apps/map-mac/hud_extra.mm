// HUD parts added after the first port (docs/ui-port.md section 6): the market and infrastructure sections of the resource
// panel, the mini library, the victory timers, the scroll plate and the mouse-over target.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "hud.hpp"

using ron::IfaceBorder;
using ron::IfacePiece;
using ron::IfaceRect;
using ron::metal::UiRenderer;
using ron::metal::UiType;
using ron::metal::argb;
using ron::metal::rgb;

namespace hud {

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000, kGrey = rgb(160, 160, 160);
constexpr uint32_t kPlate = rgb(50, 50, 50);

IfaceRect rect_in(const ron::XmlElement& win, const char* tag, const std::string& name, int pw, int ph, int nth = 0) {
	IfaceRect r;
	ron::iface_find_rect(win, tag, name, pw, ph, r, nth);
	return r;
}

// TeamColor::get_border slots: red, blue, pink, green, yellow, cyan, white, orange (ui-spec 6.6).
constexpr uint8_t kBorder[9][3] = {{255, 0, 0}, {0, 0, 255}, {188, 0, 250}, {0, 255, 0}, {255, 255, 0},
                                   {0, 255, 255}, {255, 255, 255}, {255, 128, 0}, {255, 255, 255}};

const int kGoodOrder[6] = {0, 1, 4, 5, 2, 3};  // IFaceMainBase::good_order: food, timber, metal, oil, wealth, knowledge
const int kMarketGoods[4] = {0, 1, 4, 5};      // num_market_resources: without wealth and knowledge

}  // namespace

// IFaceRenderManager::draw_scroll @0051cdb0: the "scroll" 9-slice (TEX_CIV) stretched over the rect, tinted.
void Hud::scroll_plate(UiRenderer& ui, IfaceRect r, uint32_t tint) const {
	IfacePiece p[9];
	for (int i = 0; i < 9; i++) p[i] = ron::iface_find_piece(*rm_el_, "scroll", i);
	if (!p[0].valid()) {
		ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), (tint & 0x00FFFFFF) | 0xA0000000);
		return;
	}
	const int lw = p[0].width, rw = p[2].width, th = p[0].height, bh = p[6].height;
	const int mw = std::max(0, r.w() - lw - rw), mh = std::max(0, r.h() - th - bh);
	const int xs[3] = {r.l, r.l + lw, r.l + lw + mw}, ws[3] = {lw, mw, rw};
	const int ys[3] = {r.t, r.t + th, r.t + th + mh}, hs[3] = {th, mh, bh};
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			if (ws[col] <= 0 || hs[row] <= 0) continue;
			ui.add(p[row * 3 + col], float(xs[col]), float(ys[row]), float(ws[col]), float(hs[row]), tint, UiType::Alpha);
		}
	}
}

// IFaceResources::draw_market_area @00818f20: the buy / sell title, then one row per market good with two price buttons
// (draw_market_button @00817ba0: a 3-slice tinted 0x006C1E for buy, 0x007B9B for sell, HIGHLIGHT, the price in digit set 1).
void Hud::draw_market(UiRenderer& ui, const State& s) {
	const ron::XmlElement& r = *res_el_;
	const int w = ui_w_, h = ui_h_;
	const IfaceRect m = market_rect_;
	auto plate = [&](const IfaceRect& rc, uint32_t c) {
		ui.expander(ron::iface_find_piece(r, "resource_item_tex0"), ron::iface_find_piece(r, "resource_item_tex1"),
		            ron::iface_find_piece(r, "resource_item_tex2"), rc, c, UiType::Highlight);
	};
	const IfaceRect title = rect_in(r, "IFACERECT", "market_title_rect", w, h).offset(m.l, m.t);
	plate(title, kPlate);
	ui.next_layer();
	const int f = ui.font(14);
	const int half = title.w() / 2;
	ui.text_centered(f, "Buy", {title.l, title.l + half, title.t, title.b}, kGrey, kBlack);
	ui.text_centered(f, "Sell", {title.l + half, title.r, title.t, title.b}, kGrey, kBlack);
	const IfaceRect buy0 = rect_in(r, "IFACERECT", "market_buy_rect", w, h), sell0 = rect_in(r, "IFACERECT", "market_sell_rect", w, h);
	const int pitch = buy0.h() + 3;
	const IfacePiece t0[3] = {ron::iface_find_piece(r, "market_button_tex00"), ron::iface_find_piece(r, "market_button_tex01"),
	                          ron::iface_find_piece(r, "market_button_tex02")};
	const IfacePiece t1[3] = {ron::iface_find_piece(r, "market_button_tex10"), ron::iface_find_piece(r, "market_button_tex11"),
	                          ron::iface_find_piece(r, "market_button_tex12")};
	hdivider(ui, buy0.l + m.l - 4, m.t + buy0.t - 3, sell0.r - buy0.l + 8, true);
	for (int i = 0; i < 4; i++) {
		const int good = kMarketGoods[i];
		for (int side = 0; side < 2; side++) {  // 0 buy, 1 sell
			const IfaceRect base = side == 0 ? buy0 : sell0;
			const IfaceRect rc = base.offset(m.l, m.t + pitch * i);
			market_buttons_[side][i] = rc;
			const bool down = pressed_market_ == side * 4 + i && rc.contains(hover_x_, hover_y_);
			const bool on = s.res.can_trade;
			uint32_t tint = side == 0 ? rgb(0x00, 0x6C, 0x1E) : rgb(0x00, 0x7B, 0x9B);
			if (!on) tint = rgb(0x00, 0x6C * 2 / 3, 0x1E * 2 / 3), tint = side == 0 ? tint : rgb(0, 0x7B * 2 / 3, 0x9B * 2 / 3);
			ui.next_layer();
			ui.expander(down ? t1[0] : t0[0], down ? t1[1] : t0[1], down ? t1[2] : t0[2], rc, tint, UiType::Highlight);
			ui.next_layer();
			const std::string price = std::to_string(side == 0 ? s.res.buy[size_t(good)] : s.res.sell[size_t(good)]);
			const int tw = ui.digits_width(price, 1), th = ui.digits_height(1);
			ui.write_digits(price, float(rc.l + (rc.w() - tw) / 2 + (down ? 1 : 0)), float(rc.t + (rc.h() - th) / 2 + (down ? 1 : 0)),
			                on ? kWhite : kGrey, 1);
		}
		if (i < 3) hdivider(ui, buy0.l + m.l, m.t + buy0.b + pitch * i + 1, sell0.r - buy0.l, false);
	}
	// The column divider between the buy and sell prices (a cross at the row dividers is the tee pieces' overlap).
	vdivider(ui, m.l + (buy0.r + sell0.l) / 2, m.t + buy0.t, pitch * 4 - 3, false);
	ui.next_layer();
}

// IFaceResources::draw_infra_area @00818270: a cell per good with its worker icon, "workers" and "/limit".
void Hud::draw_infra(UiRenderer& ui, const State& s) {
	const ron::XmlElement& r = *res_el_;
	const IfaceRect cell0 = rect_in(r, "IFACERECT", "infra_item_rect", ui_w_, ui_h_).offset(infra_rect_.l, infra_rect_.t);
	const IfacePiece first = ron::iface_find_piece(r, "first_good_cap_tex");
	IfaceRect row = cell0;
	for (int i = 0; i < 6; i++) {
		const int good = kGoodOrder[i];
		infra_cells_[i] = row;
		ui.next_layer();
		ui.expander(ron::iface_find_piece(r, "resource_item_tex0"), ron::iface_find_piece(r, "resource_item_tex1"),
		            ron::iface_find_piece(r, "resource_item_tex2"), row, kPlate, UiType::Highlight);
		ui.next_layer();
		IfacePiece icon = first;
		icon.left += (first.width + 1) * good;  // stride 20
		ui.add(icon, float(row.l), float(row.t + (row.h() - icon.height) / 2));
		const int dy = row.t + (row.h() - ui.digits_height(0)) / 2;
		int x = row.l + icon.width + 4;
		x += ui.write_digits(std::to_string(s.res.workers[size_t(good)]), float(x), float(dy), kWhite, 0);
		x += ui.write_digits("/", float(x), float(dy), kGrey, 0);
		ui.write_digits(s.res.limit[size_t(good)] > 0 ? std::to_string(s.res.limit[size_t(good)]) : "-", float(x), float(dy), kGrey, 0);
		if (i < 5) hdivider(ui, row.l, row.b, row.w(), false);
		row = row.offset(0, row.h() + 3);
	}
}

// IFaceMiniLibrary::on_redraw @00814210: the back plate, one button per library line (next research) and the age button,
// the progress bars of items in production, and a 2 x 5 tick per level reached; the tab toggles it.
void Hud::draw_minilib(UiRenderer& ui, const State& s) {
	if (!ml_el_) return;
	const ron::XmlElement& e = *ml_el_;
	const IfaceRect win = minilib_win_;
	const IfaceRect min_tab = rect_in(e, "IFACERECT", "min_button_rect", win.w(), win.h()).offset(win.l, win.t);
	const IfaceRect max_tab = rect_in(e, "IFACERECT", "max_button_rect", win.w(), win.h()).offset(win.l, win.t);
	ui.next_window();
	if (!minilib_open_) {
		ui.add(ron::iface_find_piece(e, "max_button_tex"), float(min_tab.l), float(min_tab.t));
		toggle_rects_[3] = min_tab;
		return;
	}
	ui.add(ron::iface_find_piece(e, "back"), float(win.l), float(win.t));
	ui.next_layer();
	ui.add(ron::iface_find_piece(e, "min_button_tex"), float(max_tab.l), float(max_tab.t));
	toggle_rects_[3] = max_tab;
	static const char* names[4] = {"military_button", "civic_button", "commerce_button", "science_button"};
	static const char* ticks[4] = {"military_tick", "civic_tick", "commerce_tick", "science_tick"};
	static const char* tick_rects[4] = {"military_tick_rect", "civic_tick_rect", "commerce_tick_rect", "science_tick_rect"};
	for (int i = -1; i < 4; i++) {
		const MiniLibButton& b = i < 0 ? s.minilib.age : s.minilib.cat[i];
		const IfaceRect rc = rect_in(e, "BUTTON", i < 0 ? "age_button" : names[i], win.w(), win.h()).offset(win.l, win.t);
		minilib_buttons_[i + 1] = rc;
		if (!b.valid) continue;
		IfacePiece cell;
		cell.texture = b.tex_id;
		const bool small = i >= 0;
		if (b.tex_x >= 0 && b.tex_y >= 0) cell.left = b.tex_x, cell.top = b.tex_y;
		else cell.left = 62 * b.tex_col, cell.top = 40 * b.tex_row;
		cell.width = 61, cell.height = 39;
		const int factor = !b.enabled ? 0x40 : rc.contains(hover_x_, hover_y_) ? 0xa4 : 0x80;
		if (!b.tex_id.empty()) ui.add(cell, float(rc.l), float(rc.t), float(rc.w()), float(rc.h()), rgb(factor, factor, factor), UiType::Highlight);
		(void)small;
		if (b.percent >= 0) {
			// age_progress (12 x 35) / tech_progress (12 x 28): a bar up the button's left edge, filled from the bottom.
			const IfacePiece pb = ron::iface_find_piece(e, i < 0 ? "age_progress" : "tech_progress");
			const int fill = pb.height * std::clamp(b.percent, 0, 100) / 100;
			ui.next_layer();
			ui.fill(float(rc.l), float(rc.t), float(pb.width), float(rc.h()), argb(0x90, 0, 0, 0), UiType::ColorBlend);
			ui.add_sub(pb, 0, float(pb.height - fill), float(pb.width), float(fill), float(rc.l), float(rc.b - fill), float(pb.width), float(fill));
		}
	}
	ui.next_layer();
	const IfaceRect off = rect_in(e, "IFACERECT", "tick_offset_rect", win.w(), win.h());
	for (int c = 0; c < 4; c++) {
		const IfaceRect tr = rect_in(e, "IFACERECT", tick_rects[c], win.w(), win.h()).offset(win.l, win.t);
		const IfacePiece tp = ron::iface_find_piece(e, ticks[c]);
		for (int k = 0; k < s.minilib.epoch[c] && k < 7; k++) ui.add(tp, float(tr.l + k * (tp.width + off.w() - tp.width)), float(tr.t));
	}
}

// IFaceMainBase::draw_timers @0080b060 / draw_game_timer @0080a3c0: a column at the top right from y = 45, each a 42 x 39
// icon with the 60 x 26 background (team-coloured borders for a player's timer) and M:SS in the 11 x 20 timer digits.
void Hud::draw_timers(UiRenderer& ui, const State& s) {
	if (s.timers.empty()) return;
	const ron::XmlElement& b = *base_el_;
	static const char* icons[8] = {"wonder_timer", "capital_timer", "armageddon_timer", "territory_timer",
	                               "peace_timer",  "time_limit_timer", "barbarian_timer", "musical_timer"};
	int y = 45;
	ui.next_window();
	for (const Timer& t : s.timers) {
		const IfacePiece icon = ron::iface_find_piece(b, icons[std::clamp(t.kind, 0, 7)]);
		const int x = ui_w_ - 10 - icon.width;
		ui.add(icon, float(x), float(y));
		const IfacePiece bg = ron::iface_find_piece(b, "background_timer");
		const int bx = x - bg.width, by = y + (icon.height - bg.height) / 2;
		ui.add(bg, float(bx), float(by), 0, 0, kWhite, UiType::Normal);
		ui.next_layer();
		if (t.who >= 0) {
			const uint8_t* c = kBorder[std::clamp(t.who, 0, 8)];
			const uint32_t col = rgb(c[0], c[1], c[2]);
			const IfacePiece vb = ron::iface_find_piece(b, "vert_border_timer"), hb = ron::iface_find_piece(b, "horiz_border_timer");
			ui.add(vb, float(bx), float(by), 0, 0, col);
			ui.add(vb, float(bx + bg.width - vb.width), float(by), 0, 0, col);
			ui.add(hb, float(bx + 3), float(by), 0, 0, col);
			ui.add(hb, float(bx + 3), float(by + bg.height - hb.height), 0, 0, col);
		}
		const int secs = std::max(0, t.frames) / 15, mins = std::min(99, secs / 60);
		char buf[16];
		std::snprintf(buf, sizeof buf, "%d:%02d", mins, secs % 60);
		int dx = bx + 5;
		for (const char* p = buf; *p; p++) {
			const IfacePiece d = ron::iface_find_piece(b, *p == ':' ? std::string("colon_timer") : std::string(1, *p) + "_digit_timer");
			ui.add(d, float(dx), float(by + 3), 0, 0, kWhite, UiType::ColorAlphaMod);
			dx += d.width;
		}
		if (t.count >= 0) {
			const int f = ui.font(18, ron::kFontBold);
			const std::string n = std::to_string(t.count);
			const uint8_t* c = kBorder[std::clamp(t.who < 0 ? 8 : t.who, 0, 8)];
			ui.text(f, n, float(bx - 6 - ui.text_width(f, n)), float(by + 2), rgb(c[0], c[1], c[2]), kBlack);
		}
		y += icon.height + 2;
		ui.next_layer();
	}
}

// MouseOver targets: the card button under the mouse, the resource rows and caps, the toggles, the selection panel's
// buttons and stats, the bar buttons, the minimap's buttons, the display button and the mini library.
Hover Hud::hover_at(int x, int y, const State& s) const {
	Hover h;
	// Mini library.
	if (minilib_win_.contains(x, y)) {
		if (toggle_rects_[3].contains(x, y)) return {HoverKind::MiniLibToggle, 0, 0};
		if (minilib_open_) {
			for (int i = 0; i < 5; i++) {
				if (minilib_buttons_[i].contains(x, y)) return {HoverKind::MiniLibCat, i, 0};
			}
		}
	}
	// Resource panel.
	const ron::XmlElement& r = *res_el_;
	if (toggle_rects_[0].contains(x, y)) return {HoverKind::ToggleMarket, 0, 0};
	if (toggle_rects_[1].contains(x, y)) return {HoverKind::ToggleResources, 0, 0};
	if (toggle_rects_[2].contains(x, y)) return {HoverKind::ToggleInfra, 0, 0};
	if ((res_flags_ & 3) == 3) {
		for (int side = 0; side < 2; side++) {
			for (int i = 0; i < 4; i++) {
				if (market_buttons_[side][i].contains(x, y)) return {side == 0 ? HoverKind::MarketBuy : HoverKind::MarketSell, kMarketGoods[i], 0};
			}
		}
	}
	if ((res_flags_ & 6) == 6) {
		for (int i = 0; i < 6; i++) {
			if (infra_cells_[i].contains(x, y)) return {HoverKind::Infra, kGoodOrder[i], 0};
		}
	}
	if (res_flags_ & 2) {
		const IfaceRect caprc = rect_in(r, "IFACERECT", "resource_cap_rect", ui_w_, ui_h_).offset(resource_rect_.l, resource_rect_.t);
		if (caprc.contains(x, y)) return {HoverKind::CommerceCap, 0, 0};
		IfaceRect row = rect_in(r, "IFACERECT", "resource_item_rect", ui_w_, ui_h_).offset(resource_rect_.l, resource_rect_.t);
		for (int i = 0; i < 6; i++) {
			if (row.contains(x, y)) return {HoverKind::Good, kGoodOrder[i], 0};
			row = row.offset(0, row.h() + 3);
		}
	}
	// Top strip.
	if (rect_in(*base_el_, "IFACERECT", "pop_cap", base_.w(), base_.h()).contains(x, y)) return {HoverKind::PopCap, 0, 0};
	if (rect_in(*base_el_, "IFACERECT", "city_cap", base_.w(), base_.h()).contains(x, y)) return {HoverKind::CityCap, 0, 0};
	for (size_t i = 0; i < bar_buttons_.size(); i++) {
		const bool top_strip = i >= 6;
		if ((top_strip || opened_) && bar_buttons_[i].contains(x, y)) return {HoverKind::BarButton, int(i), 0};
	}
	if (display_btn_.contains(x, y)) return {HoverKind::Display, 0, 0};
	if (opened_) {
		const IfaceRect timer = rect_in(*base_el_, "IFACERECT", "timer_area2", base_.w(), base_.h()).offset(base_.l, base_.t);
		if (timer.contains(x, y)) return {HoverKind::Clock, 0, 0};
	}
	// Minimap buttons.
	{
		int i = 0;
		for (const char* n : {"cycle_military", "cycle_general", "objectives", "cycle_peasant"}) {
			const ron::XmlElement* e = ron::iface_child(*mini_el_, "BUTTON", n);
			if (e && ron::resolve_iface_rect(*e, minimap_win_.w(), minimap_win_.h()).offset(minimap_win_.l, minimap_win_.t).contains(x, y))
				return {HoverKind::MinimapButton, i, 0};
			i++;
		}
		if (map_.contains(x, y)) return {HoverKind::Minimap, 0, 0};
	}
	// Timers column.
	{
		int ty = 45;
		for (const Timer& t : s.timers) {
			static const int w = 42 + 60, hgt = 39;
			if (x >= ui_w_ - 10 - w && x < ui_w_ - 10 && y >= ty && y < ty + hgt) return {HoverKind::Timer, t.kind, 0};
			ty += hgt + 2;
		}
	}
	if (s.selection.empty()) return h;
	// Card.
	{
		const int slot = card_slot_at(x, y);
		if (slot >= 0) {
			for (const Option& o : s.options) {
				if (o.grid_x * 4 + o.grid_y == slot) return {HoverKind::Option, o.option, o.object};
			}
			return h;
		}
	}
	// Selection panel.
	if (s.selection.size() > 1) {
		const int i = multi_index_at(x, y, s);
		if (i >= 0) return {HoverKind::SelObject, i, 0};
		return h;
	}
	const ron::XmlElement& e = *sel_el_;
	const int pw = sel_win_.w(), ph = sel_win_.h();
	const IfaceRect obj = rect_in(e, "BUTTON", "object", pw, ph).offset(sel_win_.l, sel_win_.t);
	if (obj.contains(x, y)) return {HoverKind::SelObject, 0, 0};
	const IfaceRect hp = rect_in(e, "IFACERECT", "hitpoints", pw, ph).offset(sel_win_.l, sel_win_.t);
	if (hp.contains(x, y)) return {HoverKind::SelHitPoints, 0, 0};
	const int q = queue_index_at(x, y, s);
	if (q >= 0) return {HoverKind::SelQueue, q, 0};
	{
		const Object& o = s.selection.front();
		std::vector<int> kinds;
		if (o.attack >= 0) kinds.push_back(0);
		if (o.armor >= 0) kinds.push_back(1);
		if (o.range >= 0) kinds.push_back(2);
		for (size_t i = 0; i < kinds.size() && i < 8; i++) {
			const int col = int(i % 2), row = int(i / 2);
			const IfaceRect cell = rect_in(e, "IFACERECT", "stat_short", pw, ph, col * 4 + row).offset(sel_win_.l, sel_win_.t);
			if (cell.contains(x, y)) return {HoverKind::SelStat, kinds[i], 0};
		}
	}
	return h;
}

}  // namespace hud
