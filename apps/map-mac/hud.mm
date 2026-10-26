#include "hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "assets/strings.hpp"
#include "assets/xml.hpp"
#include "uistr.hpp"

using ron::IfaceBorder;
using ron::IfacePiece;
using ron::IfaceRect;
using ron::metal::UiRenderer;
using ron::metal::UiType;
using ron::metal::argb;
using ron::metal::argb;
using ron::metal::rgb;

namespace hud {

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000;
constexpr uint32_t kGrey = rgb(160, 160, 160), kRed = rgb(255, 0, 0), kYellow = rgb(255, 255, 0), kGreen = rgb(0, 255, 0);
constexpr uint32_t kPlate = rgb(50, 50, 50);  // IFaceResources::draw_back's normal colour

IfaceRect rect_in(const ron::XmlElement& win, const char* tag, const std::string& name, int pw, int ph, int nth = 0) {
	return cached_iface_rect(win, tag, name, pw, ph, nth);
}

IfaceRect unite(const IfaceRect& a, const IfaceRect& b) {
	return {std::min(a.l, b.l), std::max(a.r, b.r), std::min(a.t, b.t), std::max(a.b, b.b)};
}

// IFaceButton icon cell (section 6.4): (tex_x, tex_y) when both >= 0, else 62 * col, 40 * row; 61 x 39.
IfacePiece icon_piece(const std::string& tex_id, int col, int row, int tx, int ty, bool small) {
	IfacePiece p;
	p.texture = tex_id;
	if (tx >= 0 && ty >= 0) p.left = tx, p.top = ty;
	else p.left = (small ? 31 : 62) * col, p.top = (small ? 20 : 40) * row;
	p.width = small ? 30 : 61;
	p.height = small ? 19 : 39;
	return p;
}

std::string clock_text(int frames) {
	// say_timer: H:MM:SS of game time (15 simulation frames per second at normal speed).
	const int s = frames / 15;
	char buf[32];
	std::snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	return buf;
}

}  // namespace

bool Hud::init(const ron::GameFiles& files, const ron::IfaceLayout& layout, std::string& error) {
	layout_ = &layout;
	if (const char* e = std::getenv("RON_ADVANCED")) advanced_ = std::atoi(e) != 0;  // snapshots: the advanced card rows
	if (const char* e = std::getenv("RON_GROUP_TYPES")) group_by_type_ = std::atoi(e) != 0;
	if (const char* e = std::getenv("RON_PREFER_MILITARY")) prefer_military_ = std::atoi(e) != 0;
	base_el_ = layout.find("IFACEMAINBASE");
	res_el_ = layout.find("IFACERESOURCES");
	sel_el_ = layout.find("IFACESELECTED");
	opt_el_ = layout.find("IFACEOPTIONS");
	mini_el_ = layout.find("IFACEMINIMAP");
	rm_el_ = layout.find("IFACERENDERMANAGER");
	btn_el_ = layout.find("IFACEBUTTONCLASS");
	ml_el_ = layout.find("IFACEMINILIBRARY");
	lib_el_ = layout.find("IFACELIBRARY");
	if (!base_el_ || !res_el_ || !sel_el_ || !opt_el_ || !mini_el_ || !rm_el_ || !btn_el_) {
		error = "interface.xml lacks a HUD window";
		return false;
	}
	// icon_texs.xml: the 17 x 17 inline icons of text (#ICONn, 1-based); the wonder clock's line uses #ICON46.
	try {
		int n = 0;
		const ron::XmlElement idoc = ron::parse_xml(files.read_text("Data/icon_texs.xml"));
		ron::for_each_element(idoc, "IFACETEX", [&](const ron::XmlElement& e) {
			if (++n == 46) wonder_icon_ = ron::iface_piece(e);
		});
	} catch (const std::exception&) {
	}
	// options.xml: one OPTION per OptionIndex, with its command-card icon.
	const ron::XmlElement doc = ron::parse_xml(ron::read_localised(files, "options.xml", uistr::language()));
	ron::for_each_element(doc, "OPTION", [&](const ron::XmlElement& e) {
		OptionInfo o;
		o.tex_id = e.get("tex_id");
		o.col = e.get_int("tex_col");
		o.row = e.get_int("tex_row");
		if (const ron::XmlElement* n = e.child("NAME")) o.name = n->get("#text");
		if (const ron::XmlElement* n = e.child("ENGLISH_NAME")) o.english = n->get("#text");
		if (const ron::XmlElement* n = e.child("DESC")) o.desc = n->get("#text");
		option_info_.push_back(o);
	});
	// resourcerules.xml: the rare-goods column icons (TEX2_ID / X / Y / XS / YS) by TypeIndex.
	{
		const ron::XmlElement rdoc = ron::parse_xml(files.read_text("Data/resourcerules.xml"));
		int index = 0;
		ron::for_each_element(rdoc, "RESOURCE", [&](const ron::XmlElement& e) {
			RareIcon ic;
			const std::string id = e.get("TEX2_ID");
			if (const ron::XmlElement* c = e.child("TEX2_ID")) ic.tex_id = c->get("#text");
			if (const ron::XmlElement* c = e.child("TEX2_X")) ic.x = std::atoi(c->get("#text").c_str());
			if (const ron::XmlElement* c = e.child("TEX2_Y")) ic.y = std::atoi(c->get("#text").c_str());
			if (const ron::XmlElement* c = e.child("TEX2_XS")) ic.w = std::atoi(c->get("#text").c_str());
			if (const ron::XmlElement* c = e.child("TEX2_YS")) ic.h = std::atoi(c->get("#text").c_str());
			(void)id;
			if (ic.tex_id == "CRAP") ic.tex_id.clear();
			rare_icons_.push_back(ic);
			index++;
		});
	}
	return true;
}

void Hud::set_opened(bool on) {
	if (on == opened_) return;
	opened_ = on;
	ui_w_ = -1;  // relayout on the next layout() / draw()
}

void Hud::set_panel_flags(int f) {
	f &= 7;
	if (f == res_flags_) return;
	res_flags_ = f;
	ui_w_ = -1;
}

// OPTION_TOGGLE_IFACE_MARKET / _RESOURCES / _INFRA: market and infrastructure only count as open with the resources
// ((flags & 3) == 3, (flags & 6) == 6); collapsing the resources hides both side toggles.
void Hud::toggle_panel(int bit) { set_panel_flags(res_flags_ ^ (1 << bit)); }

void Hud::layout(int w, int h) {
	if (w == ui_w_ && h == ui_h_) return;
	ui_w_ = w, ui_h_ = h;
	base_ = ron::resolve_iface_rect(*base_el_, w, h);
	const int bw = base_.w(), bh = base_.h();
	left_ = rect_in(*base_el_, "IFACERECT", "left_window", bw, bh).offset(base_.l, base_.t);
	mid_ = rect_in(*base_el_, "IFACERECT", "mid_window", bw, bh).offset(base_.l, base_.t);
	right_ = rect_in(*base_el_, "IFACERECT", "right_window", bw, bh).offset(base_.l, base_.t);
	if (!opened_) {
		// IFaceMainBase::update_visibility, streamlined: the minimap moves to the right end (against the display button) and the
		// right slot runs from the left slot's right edge to the minimap (right_window_rect.r - left.r - mid width: 362 px at
		// 1024, 618 at 1280; the selection window's `main_streamlined` rect is 626 wide).
		const IfaceRect disp = rect_in(*base_el_, "BUTTON", "display", bw, bh).offset(base_.l, base_.t);
		mid_ = mid_.offset(disp.l - mid_.r, 0);
		right_ = {left_.r, std::max(left_.r + 1, mid_.l), right_.t, right_.b};
	}
	bar_buttons_.clear();
	for (const char* n : {"pause", "advanced_options", "ping", "diplo", "chat", "stats"}) {
		bar_buttons_.push_back(rect_in(*base_el_, "BUTTON", n, bw, bh).offset(base_.l, base_.t));
	}
	display_btn_ = rect_in(*base_el_, "BUTTON", "display", bw, bh).offset(base_.l, base_.t);
	library_rect_ = IfaceRect{left_.l + (left_.w() - 319) / 2, left_.l + (left_.w() - 319) / 2 + 319, left_.t, left_.t + 168};
	// Help and menu buttons sit on the top strip (screen coordinates, resolved against the HUD width).
	bar_buttons_.push_back(rect_in(*base_el_, "IFACERECT", "help_button", bw, bh));
	bar_buttons_.push_back(rect_in(*base_el_, "IFACERECT", "menu_button", bw, bh));
	minimap_win_ = ron::resolve_iface_rect(*mini_el_, mid_.w(), mid_.h()).offset(mid_.l, mid_.t);
	map_ = rect_in(*mini_el_, "IFACERECT", "map", minimap_win_.w(), minimap_win_.h()).offset(minimap_win_.l, minimap_win_.t);
	sel_win_ = ron::resolve_iface_rect(*sel_el_, right_.w(), right_.h()).offset(right_.l, right_.t);
	opt_win_ = ron::resolve_iface_rect(*opt_el_, left_.w(), left_.h()).offset(left_.l, left_.t);
	for (int x = 0; x < 5; x++) {
		for (int y = 0; y < 4; y++) {
			card_[x][y] = rect_in(*opt_el_, "BUTTON", "butt_" + std::to_string(x) + std::to_string(y), opt_win_.w(), opt_win_.h())
			                  .offset(opt_win_.l, opt_win_.t);
		}
	}
	// IFaceResources::adjust_rects @00819a90 with six goods (four market goods).
	const ron::XmlElement& r = *res_el_;
	const IfaceRect title = rect_in(r, "IFACERECT", "market_title_rect", w, h);
	const IfaceRect sell = rect_in(r, "IFACERECT", "market_sell_rect", w, h);
	IfaceRect m[2] = {rect_in(r, "IFACERECT", "market_rect0", w, h), rect_in(r, "IFACERECT", "market_rect1", w, h)};
	IfaceRect rs[2] = {rect_in(r, "IFACERECT", "resource_rect0", w, h), rect_in(r, "IFACERECT", "resource_rect1", w, h)};
	IfaceRect in[2] = {rect_in(r, "IFACERECT", "infra_rect0", w, h), rect_in(r, "IFACERECT", "infra_rect1", w, h)};
	const int head = m[0].t + 12 + title.h();
	const int pitch = sell.h() + 3;
	rs[1].b = pitch * 6 - 3 + head;
	m[0].b = m[1].b = pitch * 4 - 3 + head;
	in[0].b = in[1].b = rs[1].b;
	const int f = res_flags_;
	for (IfaceRect& x : rs) {
		const int d = (f & 2) ? m[(f & 3) == 3].r - x.l - 1 : -x.l;
		x.l += d, x.r += d;
	}
	for (IfaceRect& x : in) {
		const int d = rs[(f >> 1) & 1].r - x.l - 1;
		x.l += d, x.r += d;
	}
	market_rect_ = m[(f & 3) == 3];
	resource_rect_ = rs[(f >> 1) & 1];
	infra_rect_ = in[(f & 6) == 6];
	res_win_ = (f & 2) ? unite(unite(market_rect_, resource_rect_), infra_rect_) : resource_rect_;
	if (ml_el_) minilib_win_ = ron::resolve_iface_rect(*ml_el_, w, h);
}

std::vector<std::pair<std::string, IfaceRect>> Hud::named_rects() const {
	std::vector<std::pair<std::string, IfaceRect>> out;
	const int bw = base_.w(), bh = base_.h();
	for (const ron::XmlElement& c : base_el_->children) {
		if (c.name != "IFACERECT" || c.get("name").empty()) continue;
		std::string n = ron::to_lower(c.get("name"));
		// IFACERECTs inside POSITIONING groups (the streamlined mode) are skipped: the first match is the opened mode's.
		bool seen = false;
		for (const auto& kv : out) seen = seen || kv.first == n;
		if (seen) continue;
		IfaceRect r = rect_in(*base_el_, "IFACERECT", c.get("name"), bw, bh);
		// The top-strip rectangles are drawn at the top of the screen (see layout); the rest sit in the bar.
		if (n != "help_button" && n != "menu_button" && n != "cap_area" && n != "pop_cap" && n != "city_cap") r = r.offset(base_.l, base_.t);
		out.push_back({n, r});
	}
	out.push_back({"minimap", map_});
	for (int x = 0; x < 5; x++) {
		for (int y = 0; y < 4; y++) out.push_back({"butt_" + std::to_string(x) + std::to_string(y), card_[x][y]});
	}
	return out;
}

int Hud::option_index(const std::string& english_name) const {
	const std::string want = ron::to_lower(english_name);
	for (size_t i = 0; i < option_info_.size(); i++) {
		if (ron::to_lower(option_info_[i].english) == want) return int(i);
	}
	return -1;
}

ron::IfacePiece Hud::base_piece(const std::string& name) const { return ron::iface_find_piece(*base_el_, name); }

bool Hud::captures(int x, int y) const {
	for (const IfaceRect* r : {&left_, &mid_, &right_, &display_btn_, &res_win_}) {
		// Streamlined: the left and right slots exist only while the selection needs them.
		if (!opened_ && (r == &left_ || r == &right_) && !has_sel_) continue;
		if (r->contains(x, y)) return true;
	}
	for (size_t i = 0; i < bar_buttons_.size(); i++) {
		if ((opened_ || i >= 6) && bar_buttons_[i].contains(x, y)) return true;
	}
	for (const IfaceRect& r : progress_rects_) {
		if (r.contains(x, y)) return true;
	}
	if (minilib_open_ ? minilib_win_.contains(x, y) : toggle_rects_[3].contains(x, y)) return true;
	const IfaceRect cap = rect_in(*base_el_, "IFACERECT", "cap_area", base_.w(), base_.h());
	if (cap.contains(x, y)) return true;
	const int cx = (ui_w_ - 420) / 2;
	return x >= cx && x < cx + 420 && y >= 0 && y < 25;  // the caption plate (tribe / age spots)
}

// ---------------------------------------------------------------- minimap mapping

void Hud::world_to_minimap(float wx, float wy, const State& s, float& x, float& y) const {
	const float u = (wx - s.world_w / 2) / s.world_w, v = (wy - s.world_h / 2) / s.world_h;
	x = float(map_.l) + float(map_.w()) / 2 * (1 + 1.003f * (u - v));
	y = float(map_.t) + float(map_.h()) / 2 * (1 + 1.003f * (u + v));
}

bool Hud::minimap_to_world(int x, int y, const State& s, float& wx, float& wy) const {
	const float a = (float(x - map_.l) / (float(map_.w()) / 2) - 1) / 1.003f;  // u - v
	const float b = (float(y - map_.t) / (float(map_.h()) / 2) - 1) / 1.003f;  // u + v
	const float u = (a + b) / 2, v = (b - a) / 2;
	wx = std::clamp((u + 0.5f) * s.world_w, 0.0f, s.world_w - 1);
	wy = std::clamp((v + 0.5f) * s.world_h, 0.0f, s.world_h - 1);
	return map_.contains(x, y);
}

// ---------------------------------------------------------------- drawing helpers

void Hud::hdivider(UiRenderer& ui, int x, int y, int w, bool thick) {
	// draw_horiz_divider @0051cf80: the bar tiled (NORMAL), then the end tees.
	const ron::InterfaceTxt& t = ui.interface_txt();
	const IfacePiece bar = t.piece("IFACERENDERMANAGER", thick ? "THICK_HORIZ_BAR" : "THIN_HORIZ_BAR");
	ui.tile(bar, {x, x + w, y, y + bar.height}, kWhite, UiType::Normal);
	const IfacePiece l = t.piece("IFACERENDERMANAGER", thick ? "THICK_TEE[3]" : "THIN_TEE[3]");
	const IfacePiece r = t.piece("IFACERENDERMANAGER", thick ? "THICK_TEE[1]" : "THIN_TEE[1]");
	ui.add(l, float(x - l.width), float(y + (bar.height - l.height) / 2));
	ui.add(r, float(x + w), float(y + (bar.height - r.height) / 2));
}

void Hud::vdivider(UiRenderer& ui, int x, int y, int h, bool thick) {
	const ron::InterfaceTxt& t = ui.interface_txt();
	const IfacePiece bar = t.piece("IFACERENDERMANAGER", thick ? "THICK_VERT_BAR" : "THIN_VERT_BAR");
	ui.tile(bar, {x, x + bar.width, y, y + h}, kWhite, UiType::Normal);
	const IfacePiece top = t.piece("IFACERENDERMANAGER", thick ? "THICK_TEE[0]" : "THIN_TEE[0]");
	const IfacePiece bot = t.piece("IFACERENDERMANAGER", thick ? "THICK_TEE[2]" : "THIN_TEE[2]");
	ui.add(top, float(x + (bar.width - top.width) / 2), float(y - top.height));
	ui.add(bot, float(x + (bar.width - bot.width) / 2), float(y + h));
}

// IFaceButton::on_redraw @007fc730 for an icon button: the cell stretched with HIGHLIGHT (x2) in grey `factor`, then the
// frame overlay (def_overlay_texs0, ALPHA, same colour).
void Hud::draw_icon_button(UiRenderer& ui, const std::string& tex_id, int col, int row, int tx, int ty, const IfaceRect& r, int factor,
                           bool small) {
	const uint32_t c = rgb(factor, factor, factor);
	if (!tex_id.empty()) ui.add(icon_piece(tex_id, col, row, tx, ty, small), float(r.l), float(r.t), float(r.w()), float(r.h()), c, UiType::Highlight);
	if (!small) ui.add(ron::iface_find_piece(*btn_el_, "def_overlay_texs0"), float(r.l), float(r.t), float(r.w()), float(r.h()), c);
}

// The object button's health bar: inset 3 px left, right and bottom, 5 px tall (2 for small icons); black background,
// fill by ObjectData::health_level (>= 90 % green, 25..90 % yellow, < 25 % red).
void Hud::draw_health(UiRenderer& ui, const IfaceRect& r, int hp, int max_hp, bool small) {
	if (max_hp <= 0) return;
	const int h = small ? 2 : 5;
	const float x = float(r.l + 3), y = float(r.b - 3 - h), w = float(r.w() - 6);
	ui.fill(x, y, w, float(h), kBlack);
	const int pct = hp * 100 / max_hp;
	const uint32_t c = pct >= 90 ? kGreen : pct >= 25 ? kYellow : kRed;
	const float fw = (w - 2) * std::clamp(float(hp) / float(max_hp), 0.0f, 1.0f);
	if (small) ui.fill(x, y, w * float(hp) / float(max_hp), float(h), c);
	else ui.fill(x + 1, y + 1, fw, float(h - 2), c);
}

// ---------------------------------------------------------------- panels

void Hud::draw(UiRenderer& ui, const State& s) {
	layout(ui.width(), ui.height());
	if (!icons_aliased_) {
		// interface.xml defines TEX_SPECIAL_RESOURCES twice (the global iface_specialresource.tga, then the main bar's
		// resource_icon.tga for the rare-goods buttons) and the layout keeps the last; the market buttons' goods and the rare
		// column's TEX2 rectangles are in the first file.
		ui.add_texload("TEX_GOOD_ICONS", "art/iface_specialresource.tga");
		icons_aliased_ = true;
	}
	ui.depth = 0;
	has_sel_ = !s.selection.empty();
	draw_bar(ui, s);
	ui.next_window();
	draw_minimap(ui, s);
	ui.next_window();
	draw_selected(ui, s);
	ui.next_window();
	if (s.library.active) draw_library(ui, s);
	else draw_options(ui, s);
	ui.next_window();
	draw_garrison(ui, s);
	draw_air_garrison(ui, s);
	ui.next_window();
	draw_top_strip(ui, s);
	ui.next_window();
	draw_resources(ui, s);
	draw_minilib(ui, s);
	draw_timers(ui, s);
	draw_rares(ui, s);
	draw_progress(ui, s);
	draw_chat(ui, s);
}

void Hud::chat_backspace() {
	while (!chat_text_.empty()) {
		const bool cont = (static_cast<unsigned char>(chat_text_.back()) & 0xC0) == 0x80;
		chat_text_.pop_back();
		if (!cont) break;
	}
}

// The chat line: "Say to <everybody|allies|enemies>: text_" in a dark box just above the interface, white with a black shadow.
void Hud::draw_chat(UiRenderer& ui, const State& s) {
	if (chat_mode_ < 0) return;
	static const char* const to[3] = {"everybody", "allies", "enemies"};
	const int f = ui.font_tag("clock_size");
	const ron::FontMetrics* m = ui.font_metrics(f);
	const float h = (m ? float(m->font_height) : 14.0f) + 8.0f;
	const float w = float(std::min(ui_w_ - 20, 640));
	const float x = 10.0f, y = std::max(4.0f, float(base_.t) - h - 8.0f);
	ui.fill(x - 1, y - 1, w + 2, h + 2, kBlack, UiType::Normal);
	ui.fill(x, y, w, h, 0xFF1C1C1C, UiType::Normal);
	const std::string line = std::string("Say to ") + to[std::clamp(chat_mode_, 0, 2)] + ": " + chat_text_ + ((s.graphic_tick & 8) ? "" : "_");
	ui.text(f, line, x + 4, y + 4, kWhite, kBlack);
}

// IFaceMainBase::on_redraw @00811600 and on_window_redraw @00811b60. Opened: the full bar. Streamlined: the bar only over the
// left slot, no clock or bar buttons, the slots only with a selection, the minimap at the right end.
void Hud::draw_bar(UiRenderer& ui, const State& s) {
	const ron::XmlElement& b = *base_el_;
	const int bw = base_.w(), bh = base_.h();
	const bool card_visible = !s.selection.empty();
	// Slot backgrounds: the left slot only when none of its panels shows, the right slot always (opened).
	const IfacePiece back = ron::iface_find_piece(b, "back");
	if (opened_) {
		if (!card_visible) {
			ui.tile(back, left_, kWhite, UiType::Normal);
			ui.border(ron::iface_find_border(b, "back"), left_);
		}
		ui.tile(back, right_, kWhite, UiType::Normal);
	}
	ui.next_layer();
	// The bar across the top 34 px (the whole width opened, the left slot when streamlined), then the clock.
	if (opened_) ui.tile(ron::iface_find_piece(b, "bar"), {base_.l, base_.r, base_.t, base_.t + 34});
	else if (card_visible) ui.tile(ron::iface_find_piece(b, "bar"), {base_.l, left_.r, base_.t, base_.t + 34});
	ui.next_layer();
	if (opened_) {
		const IfaceRect timer = rect_in(b, "IFACERECT", "timer_area2", bw, bh).offset(base_.l, base_.t);
		ui.add(ron::iface_find_piece(b, "time_back"), float(timer.l), float(timer.t), float(timer.w()), float(timer.h()), kWhite, UiType::Normal);
		// draw_button_row_area: "<speed>: H:MM:SS", font clock_size, white with a 1,1 black shadow; blinks off while paused.
		const int f = ui.font_tag("clock_size");
		if (!(s.paused && (s.graphic_tick & 8)))
			ui.text_centered(f, (s.paused ? (s.paused_name.empty() ? std::string("Paused") : s.paused_name)
			                              : (s.speed_name.empty() ? std::string("Normal") : s.speed_name)) + ": " + clock_text(s.frame),
			                 timer, kWhite, kBlack);
	}
	// Frame edges (tile_random): opened: left edge, bottom and right edges. Streamlined: the left edge and a bottom edge under
	// the left slot, and the end cap at its right edge, while the slot shows.
	if (opened_) {
		ui.tile(ron::iface_find_piece(b, "left_tile"), rect_in(b, "IFACERECT", "left_tile", bw, bh).offset(base_.l, base_.t));
		for (int i = 0; i < 2; i++) {
			ui.tile(ron::iface_find_piece(b, "right_tile", i), rect_in(b, "IFACERECT", "right_tile", bw, bh, i).offset(base_.l, base_.t));
		}
	} else if (card_visible) {
		ui.tile(ron::iface_find_piece(b, "left_tile"), rect_in(b, "IFACERECT", "left_tile", bw, bh).offset(base_.l, base_.t));
		ui.tile(ron::iface_find_piece(b, "left_tile_only"), rect_in(b, "IFACERECT", "left_tile_only", bw, bh).offset(base_.l, base_.t));
		const IfacePiece cap = ron::iface_find_piece(b, "streamlined_end_cap");
		ui.add(cap, float(right_.r - cap.width), float(base_.b - cap.height));
	}
	// Bar buttons: their tex at native size (opened only).
	const char* names[6] = {"pause", "advanced_options", "ping", "diplo", "chat", "stats"};
	for (int i = 0; i < 6 && opened_; i++) {
		const ron::XmlElement* e = ron::iface_child(b, "BUTTON", names[i]);
		if (!e) continue;
		const ron::XmlElement* t = ron::iface_child(*e, "IFACETEX", "tex");
		if (t) ui.add(ron::iface_piece(*t), float(bar_buttons_[size_t(i)].l), float(bar_buttons_[size_t(i)].t));
	}
	// The display (collapse) button: a vertical 3-slice with three arrows in the middle (on_button_redraw @00811020).
	ui.vexpander(ron::iface_find_piece(b, "collapse_button_up0"), ron::iface_find_piece(b, "collapse_button_up1"),
	             ron::iface_find_piece(b, "collapse_button_up2"), display_btn_);
	const IfacePiece arrow = ron::iface_find_piece(b, opened_ ? "collapse_button_arrow_right" : "collapse_button_arrow_left");
	const int ay = display_btn_.t + (display_btn_.h() - 3 * arrow.height - 4) / 2;
	for (int k = 0; k < 3; k++) {
		ui.add(arrow, float(display_btn_.l + (display_btn_.w() - arrow.width) / 2), float(ay + k * (arrow.height + 2)));
	}
	ui.next_layer();
}

// The top-of-screen strip: caption, cap area (pop cap, city cap), help and menu buttons.
void Hud::draw_top_strip(UiRenderer& ui, const State& s) {
	const ron::XmlElement& b = *base_el_;
	const int bw = base_.w(), bh = base_.h();
	// draw_caption @0080f360: 105 + 10 x 21 + 105 = 420 px, centred; nation and age, 14 px, (212,212,212).
	const int cx = (ui_w_ - 420) / 2;
	const IfacePiece mid = ron::iface_find_piece(b, "caption_ends_mid");
	ui.add(ron::iface_find_piece(b, "caption_ends_left"), float(cx), 0);
	for (int k = 0; k < 10; k++) ui.add(mid, float(cx + 105 + k * 21), 0);
	ui.add(ron::iface_find_piece(b, "caption_ends_right"), float(cx + 315), 0);
	ui.next_layer();
	ui.text_centered(ui.font(14), s.caption, {cx, cx + 420, -2, 23}, rgb(212, 212, 212));
	// draw_cap_area @00810370: border3 around the cap area and around help + menu (inset -4).
	const IfaceRect cap = rect_in(b, "IFACERECT", "cap_area", bw, bh);
	const IfaceRect help = rect_in(b, "IFACERECT", "help_button", bw, bh), menu = rect_in(b, "IFACERECT", "menu_button", bw, bh);
	const IfaceBorder border3 = ron::iface_find_border(*rm_el_, "border3");
	ui.border(border3, cap);
	const IfaceRect hm = unite(help, menu);
	ui.border(border3, {hm.l - 4, hm.r + 4, hm.t - 4, hm.b + 4});
	ui.next_layer();
	ui.add(ron::iface_find_piece(b, "help_button"), float(help.l), float(help.t), float(help.w()), float(help.h()));
	ui.add(ron::iface_find_piece(b, "menu_button"), float(menu.l), float(menu.t), float(menu.w()), float(menu.h()));
	// draw_pop_cap @0080ff90 / draw_city_cap @0080fbe0: plate, icon scaled to the rect height, "a/b" in digit set 0.
	auto plate = [&](const IfaceRect& r, uint32_t c) {
		ui.expander(ron::iface_find_piece(*res_el_, "resource_item_tex0"), ron::iface_find_piece(*res_el_, "resource_item_tex1"),
		            ron::iface_find_piece(*res_el_, "resource_item_tex2"), r, c, UiType::Highlight);
	};
	auto capbox = [&](const char* name, const std::string& text, bool red) {
		const IfaceRect r = rect_in(b, "IFACERECT", name, bw, bh);
		plate(r, red ? kRed : kPlate);
		ui.next_layer();
		const IfacePiece icon = ron::iface_find_piece(b, name);
		const int iw = icon.height > 0 ? icon.width * r.h() / icon.height : 0;
		ui.add(icon, float(r.l), float(r.t), float(iw), float(r.h()), kWhite, UiType::Normal);
		const int tw = ui.digits_width(text, 0);
		ui.write_digits(text, float(r.l + iw + (r.w() - iw - tw) / 2), float(r.t + (r.h() - ui.digits_height(0)) / 2), kWhite, 0);
		ui.next_layer();
	};
	const bool pop_limit = s.res.live && s.res.pop_cap > 0 && s.res.pop >= s.res.pop_cap;
	capbox("pop_cap", std::to_string(s.res.pop) + "/" + std::to_string(s.res.pop_cap), pop_limit && (s.graphic_tick & 8));
	capbox("city_cap", std::to_string(s.res.cities) + "/" + std::to_string(s.res.city_limit), false);
}

// IFaceResources::on_redraw @0081b3b0: market | resources | infrastructure.
void Hud::draw_resources(UiRenderer& ui, const State& s) {
	const ron::XmlElement& r = *res_el_;
	const int w = ui_w_, h = ui_h_;
	IfaceBorder bd;
	for (int i = 0; i < 4; i++) {
		bd.slot[i] = ron::iface_find_piece(r, "corner_tex" + std::to_string(i));
		bd.slot[4 + i] = ron::iface_find_piece(r, "side_tex" + std::to_string(i));
	}
	auto plate = [&](const IfaceRect& rc, uint32_t c) {
		ui.expander(ron::iface_find_piece(r, "resource_item_tex0"), ron::iface_find_piece(r, "resource_item_tex1"),
		            ron::iface_find_piece(r, "resource_item_tex2"), rc, c, UiType::Highlight);
	};
	// A vertical toggle (on_button_redraw @0081af70): top / tiled middle / bottom, arrows down the middle.
	auto vtoggle = [&](const IfaceRect& rc, bool open) {
		const IfacePiece top = ron::iface_find_piece(r, "top_vert_button_tex0"), bot = ron::iface_find_piece(r, "bottom_vert_button_tex0");
		ui.add(top, float(rc.l), float(rc.t));
		ui.tile(ron::iface_find_piece(r, "mid_vert_button_tex0"), {rc.l, rc.l + top.width, rc.t + top.height, rc.b - bot.height}, kWhite,
		        UiType::Normal);
		ui.add(bot, float(rc.l), float(rc.b - bot.height));
		const IfacePiece a = ron::iface_find_piece(r, open ? "left_arrow_tex" : "right_arrow_tex");
		const int ay = rc.t + (rc.h() - 3 * a.height - 4) / 2;
		for (int k = 0; k < 3; k++) ui.add(a, float(rc.l + (rc.w() - a.width) / 2), float(ay + k * (a.height + 2)));
	};
	// Market (collapsed: border and toggle; expanded: the buy / sell prices). Collapsing the resources hides both side sections.
	toggle_rects_[0] = toggle_rects_[2] = IfaceRect{0, 0, 0, 0};
	if (res_flags_ & 2) {
		ui.border(bd, {market_rect_.l + 1, market_rect_.r - 1, market_rect_.t + 1, market_rect_.b - 1});
		const bool open = (res_flags_ & 3) == 3;
		IfaceRect b0 = rect_in(r, "IFACERECT", open ? "market_button_rect1" : "market_button_rect0", w, h);
		b0.b = market_rect_.h() - b0.t;
		toggle_rects_[0] = b0.offset(market_rect_.l, market_rect_.t);
		vtoggle(toggle_rects_[0], open);
		ui.next_layer();
		if (open) draw_market(ui, s);
	}
	ui.next_layer();
	// Resources (draw_resource_area @00818cc0).
	const IfaceRect a = resource_rect_;
	ui.border(bd, {a.l + 1, a.r - 1, a.t + 1, a.b - 1});
	const IfaceRect btn = rect_in(r, "IFACERECT", "resource_button_rect1", w, h);
	vdivider(ui, a.l + btn.r, a.t + btn.t, btn.h(), true);
	const IfaceRect caprc = rect_in(r, "IFACERECT", "resource_cap_rect", w, h).offset(a.l, a.t);
	ui.next_layer();
	{
		// draw_cap @00819370: plate, cap icon at height - 2, "+N" in green (N = min resource_cap / 16); the box blinks red
		// with black text while any good is over the cap.
		bool over = false;
		for (int v : s.res.over_cap) over = over || v != 0;
		const bool flash = over && (s.graphic_tick & 8);
		plate(caprc, flash ? kRed : kPlate);
		ui.next_layer();
		const IfacePiece icon = ron::iface_find_piece(r, "cap_tex");
		const int ih = caprc.h() - 2, iw = icon.width * ih / std::max(1, icon.height);
		ui.add(icon, float(caprc.l + 1), float(caprc.t + 1), float(iw), float(ih));
		const std::string t = "+" + std::to_string(s.res.commerce_cap);
		ui.write_digits(t, float(caprc.l + 1 + iw + (caprc.w() - 1 - iw - ui.digits_width(t, 0)) / 2),
		                float(caprc.t + (caprc.h() - ui.digits_height(0)) / 2), flash ? kBlack : kGreen, 0);
	}
	// The toggle button with its arrow.
	{
		const IfaceRect b = btn.offset(a.l, a.t);
		toggle_rects_[1] = b;
		ui.add(ron::iface_find_piece(r, "resource_button_tex"), float(b.l), float(b.t));
		const IfacePiece arrow = ron::iface_find_piece(r, (res_flags_ & 2) ? "up_arrow_tex" : "down_arrow_tex");
		ui.add(arrow, float(b.l + (b.w() - arrow.width) / 2), float(b.t + (b.h() - arrow.height) / 2));
	}
	if (res_flags_ & 2) {
		const IfaceRect u = unite(btn, rect_in(r, "IFACERECT", "resource_cap_rect", w, h));
		hdivider(ui, a.l + u.l, a.t + u.b, u.w(), true);
		IfaceRect row = rect_in(r, "IFACERECT", "resource_item_rect", w, h).offset(a.l, a.t);
		static const int order[6] = {0, 1, 4, 5, 2, 3};  // good_order: food, timber, metal, oil, wealth, knowledge
		const IfacePiece first = ron::iface_find_piece(r, "first_good_tex");
		for (int i = 0; i < 6; i++) {
			const int good = order[i];
			// draw_resource @00818840: plate (YELLOW with black text, blinking, while over the cap), icon (first_good_tex + 25 *
			// good) at x + 1, stockpile at x + 29 in digit set 0, then "/" and "+rate" in grey.
			const bool flashing = ((s.flash_mask >> good) & 1) && s.graphic_tick - s.flash_tick < 8 && s.graphic_tick >= s.flash_tick;
			const bool capped = flashing || (s.res.over_cap[size_t(good)] != 0 && (s.graphic_tick & 8));
			ui.next_layer();
			plate(row, flashing ? kRed : capped ? kYellow : kPlate);
			ui.next_layer();
			IfacePiece icon = first;
			icon.left += (first.width + 4) * good;
			ui.add(icon, float(row.l + 1), float(row.t + (row.h() - icon.height) / 2));
			const int dy = row.t + (icon.height - ui.digits_height(0)) / 2 - 1;
			int x = row.l + 1 + icon.width + 7;
			x += ui.write_digits(std::to_string(s.res.stock[size_t(good)]), float(x), float(dy), capped ? kBlack : kWhite, 0);
			x += ui.write_digits("/", float(x), float(dy), capped ? kBlack : kGrey, 0);
			const int rate = s.res.rate[size_t(good)];
			ui.write_digits(rate > 0 ? "+" + std::to_string(rate) : std::to_string(rate), float(x), float(dy), capped ? kBlack : kGrey, 0);
			if (i < 5) hdivider(ui, row.l, row.b, row.w(), false);
			row = row.offset(0, row.h() + 3);
		}
	}
	ui.next_layer();
	// Infrastructure (collapsed: border and toggle; expanded: the workers of each good).
	if (res_flags_ & 2) {
		ui.border(bd, {infra_rect_.l + 1, infra_rect_.r - 1, infra_rect_.t + 1, infra_rect_.b - 1});
		const bool open = (res_flags_ & 6) == 6;
		IfaceRect b0 = rect_in(r, "IFACERECT", open ? "infra_button_rect1" : "infra_button_rect0", w, h);
		b0.b = infra_rect_.h() - b0.t;
		toggle_rects_[2] = b0.offset(infra_rect_.l, infra_rect_.t);
		vtoggle(toggle_rects_[2], open);
		ui.next_layer();
		if (open) draw_infra(ui, s);
	}
	ui.next_layer();
}

// IFaceMiniMap::on_redraw @008152e0 (the frame) and WorldMap::on_redraw @008c81f0 (background, fog, blips, view box).
void Hud::draw_minimap(UiRenderer& ui, const State& s) {
	ui.add(ron::iface_find_piece(*mini_el_, "back"), float(minimap_win_.l), float(minimap_win_.t));
	ui.next_window();
	float px[4], py[4];
	const float cw[4] = {0, s.world_w, s.world_w, 0}, ch[4] = {0, 0, s.world_h, s.world_h};
	for (int i = 0; i < 4; i++) world_to_minimap(cw[i], ch[i], s, px[i], py[i]);
	const float xy[8] = {px[0], py[0], px[1], py[1], px[2], py[2], px[3], py[3]};
	if (s.has_minimap_bg) {
		const float uv[8] = {0, 0, 256, 0, 256, 256, 0, 256};
		ui.add_quad("MINIMAP_BG", xy, uv, kWhite, UiType::Normal);
	} else {
		const float uv[8] = {};
		ui.add_quad("", xy, uv, rgb(40, 60, 40), UiType::Normal);
	}
	ui.next_layer();
	if (s.has_territory) {
		// WorldMap::render_borders: the border colours at fog resolution, alpha 0x50 inside and 0xC8 on the edges, under the fog.
		if (const ron::metal::UiTexture* tt = ui.texture("MINIMAP_TERR")) {
			const float uv[8] = {0, 0, float(tt->w), 0, float(tt->w), float(tt->h), 0, float(tt->h)};
			ui.add_quad("MINIMAP_TERR", xy, uv, kWhite, UiType::Alpha);
			ui.next_layer();
		}
	}
	if (s.fog && s.fog_xs > 0) {
		// The fog texture multiplies the map (fog-spec 9.4); texel centres at fog-cell centres.
		const float fx = float(s.fog_xs), fy = float(s.fog_ys);
		const float uv[8] = {0, 0, fx, 0, fx, fy, 0, fy};
		ui.add_quad("MINIMAP_FOG", xy, uv, kWhite, UiType::Multiply);
		ui.next_layer();
	}
	// Blips (add_blip @008ce500): a black 1-pixel frame around an s x s square; buildings first, then units.
	for (int pass = 0; pass < 2; pass++) {
		for (const Blip& b : s.blips) {
			if (b.building != (pass == 0)) continue;
			float x, y;
			world_to_minimap(b.x, b.y, s, x, y);
			const int half = b.size / 2;
			const float bx = std::floor(x) - float(half), by = std::floor(y) - float(half);
			ui.fill(bx - 1, by - 1, float(b.size + 2), float(b.size + 2), kBlack, UiType::Normal);
			ui.fill(bx, by, float(b.size), float(b.size), rgb(b.rgb[0], b.rgb[1], b.rgb[2]), UiType::Normal);
		}
	}
	ui.next_layer();
	// Pings (WorldMap::render_ping @008ca6d0, ui-spec 6.6): each PHASE of the ping's style that is active at p = (8 - t mod 8) / 8
	// draws its 64 x 64 alpha mask (half-size 16 px x the usescale term, alpha a triangle wave over the phase, rotated
	// (1 - p) * 360 * rotate_speed degrees); a player's PING also gets a square of half-size 16 * p in its colour over a black
	// shadow. Both last for the ping's lifetime.
	for (const Ping& pg : s.pings) {
		if (s.graphic_tick >= pg.end_tick) continue;
		float x, y;
		world_to_minimap(pg.x, pg.y, s, x, y);
		const int t = s.graphic_tick - pg.start_tick;
		const float p = float(8 - (t % 8)) / 8.0f;  // 1 .. 1/8
		const uint32_t col = rgb(pg.rgb[0], pg.rgb[1], pg.rgb[2]);
		if (pg.flash && size_t(pg.style) < s.ping_styles.size()) {
			const PingStyle& st = s.ping_styles[size_t(pg.style)];
			for (const PingPhase& ph : st.phases) {
				if (ph.tex_id.empty() || p > ph.start + 1e-4f || p < ph.end - 1e-4f) continue;
				const float q = ph.start > ph.end ? (p - ph.end) / (ph.start - ph.end) : 1.0f;
				const float alpha = ph.usealpha ? (q > 0.5f ? 2 * (1 - q) : 2 * q) : 1.0f;
				const float hs = 16.0f * (ph.usescale ? 1.0f + ph.scaleby * ph.scalespeed * (1.0f - p) : 1.0f);
				const float ang = st.rotates ? (1.0f - p) * 2 * float(M_PI) * st.rotate_speed : 0.0f;
				const float c = std::cos(ang) * hs, sn = std::sin(ang) * hs;
				const float xy[8] = {x - c + sn, y - sn - c, x + c + sn, y + sn - c, x + c - sn, y + sn + c, x - c - sn, y - sn + c};
				const float uv[8] = {0, 0, 64, 0, 64, 64, 0, 64};
				const uint32_t base = ph.color ? ph.color : col;
				ui.add_quad(ph.tex_id, xy, uv, argb(int(std::clamp(alpha, 0.0f, 1.0f) * 255), int((base >> 16) & 255), int((base >> 8) & 255), int(base & 255)),
				            UiType::ColorAlphaMod);
			}
		}
		if (pg.square) {
			const float h = std::max(1.0f, std::round(16.0f * p * 0.5f));
			ui.fill(std::floor(x - h) + 1, std::floor(y - h) + 1, 2 * h, 2 * h, kBlack, UiType::Normal);
			ui.fill(std::floor(x - h), std::floor(y - h), 2 * h, 2 * h, col, UiType::Normal);
		}
	}
	ui.next_layer();
	// View rect (draw_view_rect @008c7f70): an axis-aligned white box between the main view's projected corners, kept
	// 4 px inside the window.
	{
		float x0, y0, x1, y1;
		world_to_minimap(s.view_corners[0], s.view_corners[1], s, x0, y0);
		world_to_minimap(s.view_corners[2], s.view_corners[3], s, x1, y1);
		const float l = std::clamp(std::min(x0, x1), float(map_.l + 4), float(map_.r - 4));
		const float r = std::clamp(std::max(x0, x1), float(map_.l + 4), float(map_.r - 4));
		const float t = std::clamp(std::min(y0, y1), float(map_.t + 4), float(map_.b - 4));
		const float b = std::clamp(std::max(y0, y1), float(map_.t + 4), float(map_.b - 4));
		if (r - l >= 2 && b - t >= 2) ui.frame(std::round(l), std::round(t), std::round(r - l), std::round(b - t), kWhite);
	}
	ui.next_window();
	for (const char* n : {"cycle_military", "cycle_general", "objectives", "cycle_peasant"}) {
		const ron::XmlElement* e = ron::iface_child(*mini_el_, "BUTTON", n);
		const ron::XmlElement* t = e ? ron::iface_child(*e, "IFACETEX", "tex") : nullptr;
		if (!t) continue;
		const IfaceRect rc = ron::resolve_iface_rect(*e, minimap_win_.w(), minimap_win_.h()).offset(minimap_win_.l, minimap_win_.t);
		ui.add(ron::iface_piece(*t), float(rc.l), float(rc.t));
	}
}

// IFaceSelected::update_selected @00823790: 60 x 35 cells 2 px apart (nx = (W + 2) / 62, ny = (H + 2) / 37), column-major; more objects
// than cells shrink the cells by k (the smallest k with k^2 * nx * ny >= count, small icons) or, with the "group by type"
// preference (0x20000), several types and room for one full-size button per type, give one counted button per type in type
// order (units 50.., buildings 414..) instead.
std::vector<Hud::MultiButton> Hud::multi_layout(const State& s, bool* small_out) const {
	std::vector<MultiButton> out;
	if (small_out) *small_out = false;
	const int count = int(s.selection.size());
	if (count < 2) return out;
	const IfaceRect win{right_.l + 4, right_.r - 4, right_.t + 4, right_.b - 4};
	const int nx = std::max(1, (win.w() + 2) / 62), ny = std::max(1, (win.h() + 2) / 37);
	int k = 1;
	while (count > k * k * nx * ny) k++;
	if (k > 1 && group_by_type_) {
		std::vector<std::pair<int, bool>> types;  // type, building (a unit and a building never share a TypeIndex)
		std::vector<int> counts;
		for (const Object& o : s.selection) {
			size_t i = 0;
			while (i < types.size() && types[i].first != o.type) i++;
			if (i == types.size()) types.push_back({o.type, o.building}), counts.push_back(0);
			counts[i]++;
		}
		std::vector<size_t> order(types.size());
		for (size_t i = 0; i < order.size(); i++) order[i] = i;
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return types[a].first < types[b].first; });
		if (types.size() != 1 && int(types.size()) <= nx * ny) {
			for (size_t n = 0; n < order.size(); n++) {
				const int col = int(n) / ny, row = int(n) % ny;
				MultiButton b;
				b.r = IfaceRect{win.l + col * 62, win.l + col * 62 + 60, win.t + row * 37, win.t + row * 37 + 35};
				b.type = types[order[n]].first, b.count = counts[order[n]], b.building = types[order[n]].second;
				for (int i = 0; i < count; i++) {
					if (s.selection[size_t(i)].type == b.type) {
						b.index = i;  // the first member's icon
						break;
					}
				}
				out.push_back(b);
			}
			return out;
		}
	}
	const int cw = 62 / k, chh = 37 / k;
	if (small_out) *small_out = k > 1;
	for (int i = 0; i < count; i++) {
		const int col = i / (ny * k), row = i % (ny * k);
		MultiButton b;
		b.r = IfaceRect{win.l + col * cw, win.l + col * cw + cw - 2, win.t + row * chh, win.t + row * chh + chh - 2};
		if (b.r.r > win.r) break;
		b.index = i, b.type = -1;
		out.push_back(b);
	}
	return out;
}

// -1: none (or a type group: see multi_button_at); else the selection index of the individual button under (x, y).
int Hud::multi_index_at(int x, int y, const State& s) const {
	for (const MultiButton& b : multi_layout(s)) {
		if (b.r.contains(x, y) && b.type < 0) return b.index;
	}
	return -1;
}

int Hud::multi_type_at(int x, int y, const State& s) const {
	for (const MultiButton& b : multi_layout(s)) {
		if (b.r.contains(x, y) && b.type >= 0) return b.type;
	}
	return -1;
}

// IFaceSelected::on_redraw @00826420 / update_selected @00823790.
void Hud::draw_selected(UiRenderer& ui, const State& s) {
	if (s.selection.empty()) return;
	const ron::XmlElement& e = *sel_el_;
	const int pw = sel_win_.w(), ph = sel_win_.h();
	if (s.selection.size() > 1) {
		// Multi-selection (multi_layout): individual buttons with a health bar, or counted buttons per type.
		bool small = false;
		for (const MultiButton& b : multi_layout(s, &small)) {
			const Object& o = s.selection[size_t(b.index)];
			const bool hover = b.r.contains(hover_x_, hover_y_);
			draw_icon_button(ui, o.tex_id, o.tex_col, o.tex_row, o.tex_x, o.tex_y, b.r, hover ? 0xa4 : 0x80, small);
			ui.next_layer();
			if (b.type >= 0) {
				const std::string t = std::to_string(b.count);
				ui.write_digits(t, float(b.r.r - ui.digits_width(t, 1) - 3), float(b.r.b - ui.digits_height(1) - 3), kWhite, 1);
			} else {
				draw_health(ui, b.r, o.hp, o.max_hp, small);
			}
			ui.next_layer();
		}
		return;
	}
	const Object& o = s.selection.front();
	const int ox = sel_win_.l, oy = sel_win_.t;
	if (!opened_) {
		// Streamlined: only progress and queue (update_progress / update_queue with the *_streamlined rects); other objects get
		// just their portrait and name.
		const uint32_t team = rgb(o.rgb[0], o.rgb[1], o.rgb[2]);
		if (o.building && (o.progress >= 0 || !o.queue.empty())) {
			const int pct = o.progress >= 0 ? o.progress : o.queue.front().percent;
			const std::string label = o.progress >= 0 ? uistr::tr("Construction") + " " + std::to_string(pct) + "%"
			                                          : o.queue.front().name + " " + std::to_string(pct) + "%";
			const IfaceRect bar = rect_in(e, "IFACERECT", "progress_bar_streamlined", pw, ph).offset(ox, oy);
			const IfacePiece p0 = ron::iface_find_piece(e, "progress_bar0"), p1 = ron::iface_find_piece(e, "progress_bar1");
			const int done = bar.w() * pct / 100;
			ui.add_sub(p0, 0, 0, float(done), float(p0.height), float(bar.l), float(bar.t), float(done), float(bar.h()));
			ui.add_sub(p1, float(done), 0, float(bar.w() - done), float(p1.height), float(bar.l + done), float(bar.t), float(bar.w() - done), float(bar.h()));
			const IfaceRect txt = rect_in(e, "IFACERECT", "progress_bar_text_streamlined", pw, ph).offset(ox, oy);
			const int f = ui.font(12, ron::kFontBold);
			if (const ron::FontMetrics* m = ui.font_metrics(f)) ui.text(f, label, float(txt.l), float(txt.b - m->font_height), 0xFFCFCFCF, kBlack);
			for (size_t i = 0; i < o.queue.size() && i <= 10; i++) {
				const IfaceRect qb = rect_in(e, "BUTTON", "queue_streamlined", pw, ph, int(i)).offset(ox, oy);
				const QueueItem& q = o.queue[i];
				draw_icon_button(ui, q.tex_id, q.tex_col, q.tex_row, q.tex_x, q.tex_y, qb, qb.contains(hover_x_, hover_y_) ? 0xa4 : 0x80, false);
				ui.next_layer();
				if (i == 0) ui.fill(float(qb.l), float(qb.t), float(qb.w() * q.percent / 100), float(qb.h()), argb(0x80, 0, 255, 0), UiType::ColorBlend);
			}
			const char* inf_name = o.repeat ? "infinite_queue_on_streamlined" : "infinite_queue_off_streamlined";
			const ron::XmlElement* ib = ron::iface_child(e, "BUTTON", inf_name);
			const ron::XmlElement* it = ib ? ron::iface_child(*ib, "IFACETEX", "tex") : nullptr;
			if (it) {
				const IfaceRect inf = rect_in(e, "BUTTON", inf_name, pw, ph).offset(ox, oy);
				ui.add(ron::iface_piece(*it), float(inf.l), float(inf.t));
			}
		} else if (!s.air.active && !s.garrison.active) {  // (the air / garrison panel takes the slot's left side: IFaceMain::position_windows)
			const IfaceRect obj = rect_in(e, "BUTTON", "object", pw, ph).offset(ox, oy + 14);
			ui.fill(float(obj.l), float(obj.t - 14), float(obj.w()), 14, team, UiType::Normal);
			ui.next_layer();
			ui.text_centered(ui.font(12, ron::kFontBold), o.name, {obj.l, obj.r, obj.t - 14, obj.t}, kWhite, kBlack);
			draw_icon_button(ui, o.tex_id, o.tex_col, o.tex_row, o.tex_x, o.tex_y, obj, 0x80, false);
			ui.next_layer();
			draw_health(ui, obj, o.hp, o.max_hp, false);
		}
		return;
	}
	// Frame: back tiled, misc_border borders, misc_tile rules, misc joints.
	const IfaceRect main = rect_in(e, "IFACERECT", "main", pw, ph).offset(ox, oy);
	ui.tile(ron::iface_find_piece(e, "back"), main, kWhite, UiType::Normal);
	ui.next_layer();
	const IfaceBorder mb = ron::iface_find_border(e, "misc_border");
	for (int i = 0; i < 2; i++) ui.border(mb, rect_in(e, "IFACERECT", "misc_border", pw, ph, i).offset(ox, oy));
	ui.next_layer();
	for (int i = 0; i < 3; i++) ui.tile(ron::iface_find_piece(e, "misc_tile", i), rect_in(e, "IFACERECT", "misc_tile", pw, ph, i).offset(ox, oy));
	ui.next_layer();
	for (int i = 0; i < 5; i++) {
		const IfaceRect rc = rect_in(e, "IFACERECT", "misc", pw, ph, i).offset(ox, oy);
		ui.add(ron::iface_find_piece(e, "misc", i), float(rc.l), float(rc.t), float(rc.w()), float(rc.h()));
	}
	ui.next_layer();
	const uint32_t team = rgb(o.rgb[0], o.rgb[1], o.rgb[2]);
	// Name line: the team colour bar with the name, 12 B white, 1,1 black shadow, centred.
	const IfaceRect name = rect_in(e, "IFACERECT", "name", pw, ph).offset(ox, oy);
	ui.fill(float(name.l), float(name.t), float(name.w()), float(name.h()), team, UiType::Normal);
	// Owner plate behind the portrait, modulated by the team colour.
	const IfaceRect plate = rect_in(e, "IFACERECT", "player_color", pw, ph).offset(ox, oy);
	ui.add(ron::iface_find_piece(e, "player_color"), float(plate.l), float(plate.t), float(plate.w()), float(plate.h()), team);
	ui.next_layer();
	ui.text_centered(ui.font(12, ron::kFontBold), o.name, name, kWhite, kBlack);
	// Portrait (object button) with its health bar.
	const IfaceRect obj = rect_in(e, "BUTTON", "object", pw, ph).offset(ox, oy);
	draw_icon_button(ui, o.tex_id, o.tex_col, o.tex_row, o.tex_x, o.tex_y, obj, 0x80, false);
	ui.next_layer();
	draw_health(ui, obj, o.hp, o.max_hp, false);
	// Hit points "cur/max" (10 px, white, centred).
	if (o.max_hp > 0) {
		const IfaceRect hp = rect_in(e, "IFACERECT", "hitpoints", pw, ph).offset(ox, oy);
		ui.text_centered(ui.font(10), std::to_string(o.hp) + "/" + std::to_string(o.max_hp), hp, kWhite);
	}
	// Production (update_progress @00824d20, update_queue @00824200): construction or the item in production on the
	// progress bar (progress_bar0 over the done part, progress_bar1 for the rest), its text above, the current item's
	// button, then the rest of the queue in 30x32 buttons; the in-progress item gets a translucent green overlay.
	if (o.building && (o.progress >= 0 || !o.queue.empty())) {
		const int pct = o.progress >= 0 ? o.progress : o.queue.front().percent;
		const std::string label = o.progress >= 0 ? uistr::tr("Construction") + " " + std::to_string(pct) + "%"
		                                          : o.queue.front().name + " " + std::to_string(pct) + "%";
		const IfaceRect bar = rect_in(e, "IFACERECT", "progress_bar", pw, ph).offset(ox, oy);
		const IfacePiece p0 = ron::iface_find_piece(e, "progress_bar0"), p1 = ron::iface_find_piece(e, "progress_bar1");
		const int done = bar.w() * pct / 100;
		ui.add_sub(p0, 0, 0, float(done), float(p0.height), float(bar.l), float(bar.t), float(done), float(bar.h()));
		ui.add_sub(p1, float(done), 0, float(bar.w() - done), float(p1.height), float(bar.l + done), float(bar.t), float(bar.w() - done),
		           float(bar.h()));
		const IfaceRect txt = rect_in(e, "IFACERECT", "progress_bar_text", pw, ph).offset(ox, oy);
		const int f = ui.font(12, ron::kFontBold);
		if (const ron::FontMetrics* m = ui.font_metrics(f)) ui.text(f, label, float(txt.l), float(txt.b - m->font_height), 0xFFCFCFCF, kBlack);
		if (!o.queue.empty()) {
			const IfaceRect big = rect_in(e, "BUTTON", "queue", pw, ph, 0).offset(ox, oy);
			const QueueItem& q0 = o.queue.front();
			draw_icon_button(ui, q0.tex_id, q0.tex_col, q0.tex_row, q0.tex_x, q0.tex_y, big, big.contains(hover_x_, hover_y_) ? 0xa4 : 0x80, false);
			ui.next_layer();
			ui.fill(float(big.l), float(big.t), float(big.w() * q0.percent / 100), float(big.h()), argb(0x80, 0, 255, 0), UiType::ColorBlend);
			for (size_t i = 1; i < o.queue.size() && i <= 10; i++) {
				const IfaceRect qb = rect_in(e, "BUTTON", "queue", pw, ph, int(i)).offset(ox, oy);
				const QueueItem& q = o.queue[i];
				draw_icon_button(ui, q.tex_id, q.tex_col, q.tex_row, q.tex_x, q.tex_y, qb, qb.contains(hover_x_, hover_y_) ? 0xa4 : 0x80, false);
			}
			ui.next_layer();
		}
		const char* inf_name = o.repeat ? "infinite_queue_on" : "infinite_queue_off";
		const ron::XmlElement* ib = ron::iface_child(e, "BUTTON", inf_name);
		const ron::XmlElement* it = ib ? ron::iface_child(*ib, "IFACETEX", "tex") : nullptr;
		if (it) {
			const IfaceRect inf = rect_in(e, "BUTTON", inf_name, pw, ph).offset(ox, oy);
			ui.add(ron::iface_piece(*it), float(inf.l), float(inf.t));
		}
	}
	// Stats (draw_stat @008214d0): cells in the fixed order attack, armor, range; left column right-aligned plate with
	// the icon at its left, right column with the icon at its right. Values in a 10 px font, white.
	struct Stat {
		const char* icon;
		int value;
	};
	std::vector<Stat> stats;
	if (o.attack >= 0) stats.push_back({"stat_attack", o.attack});
	if (o.armor >= 0) stats.push_back({"stat_armor", o.armor});
	if (o.range >= 0) stats.push_back({"stat_range", o.range});
	for (size_t i = 0; i < stats.size() && i < 8; i++) {
		const int col = int(i % 2), row = int(i / 2);
		const IfaceRect cell = rect_in(e, "IFACERECT", "stat_short", pw, ph, col * 4 + row).offset(ox, oy);
		const IfacePiece bg = ron::iface_find_piece(e, col == 0 ? "stat_left" : "stat_right");
		const IfacePiece icon = ron::iface_find_piece(e, stats[i].icon);
		const int bx = col == 0 ? cell.r - bg.width : cell.l;
		ui.add(bg, float(bx), float(cell.t));
		ui.next_layer();
		ui.add(icon, float(col == 0 ? cell.l : cell.r - icon.width), float(cell.t - 3));
		ui.text_centered(ui.font(10), std::to_string(stats[i].value), {bx, bx + bg.width, cell.t, cell.b}, kWhite);
		ui.next_layer();
	}
}

bool Hud::infinite_hit(int x, int y, const State& s) const {
	if (s.selection.size() != 1 || !s.selection.front().building || !s.selection.front().own || !opened_) return false;
	const Object& o = s.selection.front();
	if (o.queue.empty()) return false;
	const char* inf_name = o.repeat ? "infinite_queue_on" : "infinite_queue_off";
	return rect_in(*sel_el_, "BUTTON", inf_name, sel_win_.w(), sel_win_.h()).offset(sel_win_.l, sel_win_.t).contains(x, y);
}

int Hud::queue_index_at(int x, int y, const State& s) const {
	if (s.selection.size() != 1 || !s.selection.front().building) return -1;
	const int pw = sel_win_.w(), ph = sel_win_.h();
	for (size_t i = 0; i < s.selection.front().queue.size() && i <= 10; i++) {
		if (rect_in(*sel_el_, "BUTTON", opened_ ? "queue" : "queue_streamlined", pw, ph, int(i)).offset(sel_win_.l, sel_win_.t).contains(x, y)) return int(i);
	}
	return -1;
}

int Hud::card_slot_at(int x, int y) const {
	for (int gx = 0; gx < 5; gx++) {
		for (int gy = 0; gy < 4; gy++) {
			if (card_[gx][gy].contains(x, y)) return gx * 4 + gy;
		}
	}
	return -1;
}

// IFaceOptions::on_redraw @00815a50 and the option buttons (IFaceButton::on_redraw).
void Hud::draw_options(UiRenderer& ui, const State& s) {
	if (s.selection.empty()) return;
	const ron::XmlElement& e = *opt_el_;
	const int pw = opt_win_.w(), ph = opt_win_.h();
	for (int i = 0; i < 8; i++) {
		ui.tile(ron::iface_find_piece(e, "misc_tile", i), rect_in(e, "IFACERECT", "misc_tile", pw, ph, i).offset(opt_win_.l, opt_win_.t));
	}
	ui.next_layer();
	for (int i = 0; i < 15; i++) {
		const IfaceRect rc = rect_in(e, "IFACERECT", "misc", pw, ph, i).offset(opt_win_.l, opt_win_.t);
		ui.add(ron::iface_find_piece(e, "misc", i), float(rc.l), float(rc.t), float(rc.w()), float(rc.h()));
	}
	ui.next_layer();
	const Option* slot[5][4] = {};
	for (const Option& o : s.options) {
		if (o.grid_x >= 0 && o.grid_x < 5 && o.grid_y >= 0 && o.grid_y < 4 && !slot[o.grid_x][o.grid_y]) slot[o.grid_x][o.grid_y] = &o;
	}
	const IfacePiece blank = ron::iface_find_piece(e, "blank_button");
	int hl_x = -1, hl_y = -1;
	for (int x = 0; x < 5; x++) {
		for (int y = 0; y < 4; y++) {
			const IfaceRect& rc = card_[x][y];
			const Option* o = slot[x][y];
			if (!o) {
				ui.add(blank, float(rc.l), float(rc.t), float(rc.w()), float(rc.h()), kWhite, UiType::Normal);
				continue;
			}
			const bool hover = rc.contains(hover_x_, hover_y_);
			const bool held = pressed_slot_ == x * 4 + y && hover;
			int factor = !o->enabled ? 0x40 : held ? 0x80 : (hover || o->latched) ? 0xa4 : 0x80;
			std::string tex = o->tex_id;
			int col = o->tex_col, row = o->tex_row;
			if (tex.empty() && o->option >= 0 && size_t(o->option) < option_info_.size()) {
				tex = option_info_[size_t(o->option)].tex_id;
				col = option_info_[size_t(o->option)].col;
				row = option_info_[size_t(o->option)].row;
			}
			IfaceRect r = rc;
			if (held || !o->enabled) r = r.offset(0, 0);
			if (o->price >= 0) {
				draw_market_button(ui, *o, r, factor);
				continue;
			}
			int tx = o->tex_x, ty = o->tex_y;
			if (o->variant >= 0 && !o->variant_list.empty()) {
				// STANCE / FORM: the piece of the group's current state (IFACEOPTIONS combat / worker / caster / packer / form).
				const IfacePiece vp = ron::iface_find_piece(*opt_el_, o->variant_list, o->variant);
				if (vp.valid()) tex = vp.texture, tx = vp.left, ty = vp.top;
			}
			draw_icon_button(ui, tex, col, row, tx, ty, r, factor, false);
			if (o->orange) {  // a mixed / switched-on state (ORANGE)
				ui.next_layer();
				ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), argb(0x58, 255, 160, 0), UiType::ColorBlend);
			}
			if (!o->label.empty()) {
				const int lf = ui.font(10, ron::kFontBold);
				ui.next_layer();
				ui.text_centered(lf, o->label, r, kWhite, kBlack);
			}
			if (o->option == kOptResearch) {
				const IfacePiece badge = ron::iface_find_piece(*btn_el_, "research_icon");
				ui.add(badge, float(rc.r - 19), float(rc.b - 25));
			}
			// IFaceButton::on_redraw: plain option buttons match hilite_option, unit / building / tech buttons their object.
			const bool by_option = o->object < 0 && o->option >= 0 && o->option == s.hilite_option;
			const bool by_object = o->object >= 0 && o->object == s.hilite_object;
			if (s.highlight && (by_option || by_object)) hl_x = (rc.l + rc.r) / 2, hl_y = (rc.t + rc.b) / 2;
		}
	}
	ui.next_layer();
	if (!s.card_message.empty()) {
		// IFaceMarket::on_redraw: the message wrapped and centred over the lower rows of the card.
		const IfaceRect area{card_[0][2].l, card_[4][3].r, card_[0][2].t, card_[0][3].b};
		const int f = ui.font(14, ron::kFontBold);
		const ron::FontMetrics* fm = ui.font_metrics(f);
		std::vector<std::string> lines;
		size_t pos = 0;
		while (pos <= s.card_message.size()) {
			size_t nl = s.card_message.find('\n', pos);
			if (nl == std::string::npos) nl = s.card_message.size();
			lines.push_back(s.card_message.substr(pos, nl - pos));
			pos = nl + 1;
		}
		const int lh = fm ? fm->line_height : 16;
		int y = area.t + (area.h() - lh * int(lines.size())) / 2;
		ui.fill(float(area.l), float(area.t), float(area.w()), float(area.h()), argb(0xA0, 0, 0, 0), UiType::ColorBlend);
		ui.next_layer();
		for (const std::string& l : lines) {
			ui.text_centered(f, l, {area.l, area.r, y, y + lh}, kWhite, kBlack);
			y += lh;
		}
		ui.next_layer();
	}
	if (hl_x >= 0 && !s.highlight_tex.empty()) {
		// GraphicPieces::draw_highlight_circle @00902e40: the BUTTONHIGHLIGHT texture centred on the button, half extents
		// = half the texture size, turning 20 degrees per graphic tick, WHITE.
		const ron::metal::UiTexture* t = ui.texture(s.highlight_tex);
		if (t) {
			const float a = float((s.graphic_tick % 360) * 20 % 360) * float(M_PI) / 180.0f;
			const float hx = float(t->w / 2), hy = float(t->h / 2);
			const float ax = std::cos(a) * hx, ay = std::sin(a) * hy;                          // first axis
			const float bx = std::cos(a + float(M_PI) / 2) * hx, by = std::sin(a + float(M_PI) / 2) * hy;  // second
			const float cx = float(hl_x), cy = float(hl_y);
			const float xy[8] = {cx - ax - bx, cy - ay - by, cx + ax - bx, cy + ay - by, cx + ax + bx, cy + ay + by, cx - ax + bx, cy - ay + by};
			const float uv[8] = {0, 0, float(t->w), 0, float(t->w), float(t->h), 0, float(t->h)};
			ui.add_quad(s.highlight_tex, xy, uv, kWhite, UiType::Alpha);
			ui.next_layer();
		}
	}
}

// ---------------------------------------------------------------- input

Action Hud::mouse_down(int x, int y, bool right, const State& s) {
	Action a;
	hover_x_ = x, hover_y_ = y;
	if (!captures(x, y)) return a;
	a.kind = ActionKind::Consumed;
	float wx, wy;
	if (map_.contains(x, y) && minimap_to_world(x, y, s, wx, wy)) {
		// WorldMap::on_left_down: press / drag moves the camera; right release orders at that point.
		a.kind = right ? ActionKind::None : ActionKind::MinimapLook;
		if (right) a.kind = ActionKind::Consumed;
		a.wx = wx, a.wy = wy;
		minimap_drag_ = !right;
		return a;
	}
	pressed_market_ = -1;
	if ((res_flags_ & 3) == 3 && !right) {
		for (int side = 0; side < 2; side++) {
			for (int i = 0; i < 4; i++) {
				if (market_buttons_[side][i].contains(x, y)) pressed_market_ = side * 4 + i;
			}
		}
	}
	if (!s.selection.empty()) {
		const int slot = card_slot_at(x, y);
		if (slot >= 0) pressed_slot_ = slot;
	}
	return a;
}

Action Hud::mouse_drag(int x, int y, const State& s) {
	Action a;
	hover_x_ = x, hover_y_ = y;
	if (minimap_drag_) {
		minimap_to_world(x, y, s, a.wx, a.wy);
		a.kind = ActionKind::MinimapLook;
	}
	return a;
}

Action Hud::mouse_up(int x, int y, bool right, const State& s) {
	Action a;
	hover_x_ = x, hover_y_ = y;
	const bool was_drag = minimap_drag_;
	minimap_drag_ = false;
	if (was_drag) return {ActionKind::Consumed};
	if (!captures(x, y)) {
		pressed_slot_ = -1;
		return a;
	}
	a.kind = ActionKind::Consumed;
	if (!right) {
		// Toggles (OPTION_TOGGLE_IFACE_*), the display button, the mini library tab, market prices, bar and minimap buttons.
		if (res_flags_ & 2) {
			if (toggle_rects_[0].contains(x, y)) return toggle_panel(0), a;
			if (toggle_rects_[2].contains(x, y)) return toggle_panel(2), a;
		}
		if (toggle_rects_[1].contains(x, y)) return toggle_panel(1), a;
		if (toggle_rects_[3].contains(x, y)) return toggle_minilib(), a;
		if (display_btn_.contains(x, y)) return set_opened(!opened_), a;
		if (minilib_open_) {
			for (int i = 0; i < 5; i++) {
				if (minilib_buttons_[i].contains(x, y)) {
					a.kind = ActionKind::MiniLib, a.index = i;  // 0 the age, 1..4 military, civic, commerce, science
					return a;
				}
			}
		}
		if (pressed_market_ >= 0) {
			const int side = pressed_market_ / 4, i = pressed_market_ % 4;
			pressed_market_ = -1;
			if (market_buttons_[side][i].contains(x, y)) {
				static const int goods[4] = {0, 1, 4, 5};
				if (s.res.can_trade) a.kind = side == 0 ? ActionKind::Buy : ActionKind::Sell, a.object = goods[i];
				return a;
			}
		}
		for (size_t i = 0; i < bar_buttons_.size(); i++) {
			if ((opened_ || i >= 6) && bar_buttons_[i].contains(x, y)) {
				a.kind = ActionKind::BarButton, a.index = int(i);
				return a;
			}
		}
		int mi = 0;
		for (const char* n : {"cycle_military", "cycle_general", "objectives", "cycle_peasant"}) {
			const ron::XmlElement* e = ron::iface_child(*mini_el_, "BUTTON", n);
			if (e && ron::resolve_iface_rect(*e, minimap_win_.w(), minimap_win_.h()).offset(minimap_win_.l, minimap_win_.t).contains(x, y)) {
				a.kind = ActionKind::MinimapButton, a.index = mi;
				return a;
			}
			mi++;
		}
	}
	float wx, wy;
	if (right && map_.contains(x, y) && minimap_to_world(x, y, s, wx, wy)) {
		a.kind = ActionKind::MinimapOrder;
		a.wx = wx, a.wy = wy;
		return a;
	}
	if (!right && s.library.active) {
		if (s.library.final_view) {
			const int k = final_button_at(x, y, s);
			if (k >= 0 && s.library.fin[k].type >= 0) {
				a.kind = ActionKind::Library, a.object = s.library.fin[k].type, a.index = k;
				return a;
			}
			return a;
		}
		int which = -1;
		if (cat_button_at(x, y, &which) >= 0) {
			a.kind = ActionKind::MiniLib, a.index = which;  // the line's next tech / the next age, as the mini library
			return a;
		}
		int cat = -1, col = -1;
		const int type = library_cell_at(x, y, s, &cat, &col);
		if (type >= 0) {
			a.kind = ActionKind::Library, a.object = type, a.index = cat;
			return a;
		}
	}
	if (!right) {
		const int ar = air_cell_at(x, y, s);
		if (ar >= 0) {
			if (s.air.own) a.kind = ActionKind::AirSelect, a.index = ar;
			return a;
		}
	}
	{
		const int gi = garrison_index_at(x, y, s);
		if (gi >= 0) {
			a.kind = ActionKind::GarrisonEject, a.index = gi, a.all = right;
			return a;
		}
		const int pi = right ? -1 : progress_index_at(x, y, s);
		if (pi >= 0) {
			a.kind = ActionKind::Progress, a.index = pi;
			return a;
		}
	}
	const int slot = card_slot_at(x, y);
	if (!right && slot >= 0 && slot == pressed_slot_ && !s.selection.empty()) {
		for (const Option& o : s.options) {
			if (o.grid_x * 4 + o.grid_y == slot && o.enabled) {
				a.kind = ActionKind::Option;
				a.option = o.option;
				a.object = o.object;
				break;
			}
		}
	}
	pressed_slot_ = -1;
	const int idx = multi_index_at(x, y, s);
	if (!right && idx >= 0) {
		a.kind = ActionKind::SelectIndex;
		a.index = idx;
	}
	const int tg = multi_type_at(x, y, s);
	if (!right && tg >= 0) {
		a.kind = ActionKind::SelectType;
		a.object = tg;
	}
	const int q = queue_index_at(x, y, s);
	if (!right && q >= 0) {
		a.kind = ActionKind::QueueItem;
		a.index = q;
	}
	if (!right && infinite_hit(x, y, s)) a.kind = ActionKind::Infinite;
	return a;
}

}  // namespace hud
