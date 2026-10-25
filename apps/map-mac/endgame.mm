#include "endgame.hpp"

#include <algorithm>
#include <cstdio>

#include "assets/xml.hpp"
#include "sim/ai.hpp"
#include "sim/victory.hpp"
#include "uistr.hpp"

using ron::IfaceBorder;
using ron::IfacePiece;
using ron::IfaceRect;
using ron::metal::argb;
using ron::metal::rgb;
using ron::metal::UiRenderer;
using ron::metal::UiType;

namespace endgame {

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000;
constexpr int kWinW = 248, kWinH = 308;       // ENDGAMEWIN
constexpr int kScrollPadX = 8, kScrollPadY = 4;  // IFaceRenderManager scroll_pad_* (ui-spec 6.5)
// TeamColors text slots (red, blue, pink, green, yellow, cyan, white, orange, 8 = white), as script_view.mm.
constexpr uint8_t kText[9][3] = {{255, 100, 100}, {70, 100, 255}, {188, 0, 250}, {0, 236, 33},  {255, 239, 55},
                                 {0, 255, 255},   {255, 255, 255}, {255, 182, 6}, {255, 255, 255}};

// One of translated_strings.xml (positional in every language), the English text when the tables are missing.
std::string str(const ron::StringTables* t, size_t index, const char* fallback) {
	if (t && index < t->translated().size() && !t->translated()[index].empty()) return t->translated()[index];
	return fallback;
}

std::string clock_text(int frames) {  // say_timer: H:MM:SS of game time (15 frames a second)
	const int s = frames / 15;
	char buf[32];
	std::snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
	return buf;
}

// The sentence EndGameWin::get_type_string picks (decompile re/decomp/endgame, strings by loc_str_array offset / 20).
struct Facts {
	int mode = 0;
	int victory_type = 0, defeat_type = 0;
	bool instant_victory = false, instant_defeat = false, team_game = false;
	int num_players = 2, num_teams = 2;
	int team_style = 0;
	bool ctw = false;
};

std::string sentence(const ron::StringTables* t, const Facts& f) {
	auto S = [&](size_t i, const char* fb) { return str(t, i, fb); };
	if (f.mode == 0) {
		if (!f.team_game) {
			switch (f.victory_type) {
				case 0:
					switch (f.defeat_type) {
						case 0: return S(1160, "You won by conquest!");
						case 1:
						case 2: return f.instant_defeat ? S(1161, "You won a World Government (Capital) victory!") : S(1162, "You won a capital victory!");
						case 3: return S(1163, "You won a sudden death victory!");
						case 4: return S(1164, "You had the highest score at the final time check!");
						case 6: return f.num_players == 2 ? S(1165, "Your opponent gave up!") : S(1121, "All of your opponents gave up!");
						case 7:
							return f.num_players == 2 ? S(1166, "Your opponent disconnected or was dropped.") : S(1167, "All of your opponents were disconnected or gave up.");
						default: return S(1168, "You won!");
					}
				case 1: return f.instant_victory ? S(1169, "You won a World Government (Wonder) victory!") : S(1170, "You won a wonder victory!");
				case 2: return f.instant_victory ? S(1171, "You won a World Government (Territory) victory!") : S(1172, "You won a territory victory!");
				case 3: return S(1173, "You won a technology victory!");
				case 4: return S(1174, "You won a score victory!");
				case 5: return S(1175, "You won an economic victory!");
				case 6: return f.team_style != 3 ? S(1176, "You had the high score at the time limit!") : S(1131, "You held out until the time limit!");
				default: return {};
			}
		}
		switch (f.victory_type) {
			case 0:
				switch (f.defeat_type) {
					case 0: return S(1116, "Your team won by conquest!");
					case 1:
					case 2: return f.instant_defeat ? S(1117, "Your team won a World Government (Capital) victory!") : S(1118, "Your team won a capital victory!");
					case 3: return S(1119, "Your team won a sudden death victory!");
					case 4: return S(1120, "Your team had the highest score at the final time check!");
					case 6: return S(1121, "All of your opponents gave up!");
					case 7: return S(1122, "All of your opponents gave up or disconnected!");
					default: return S(1123, "Your team won!");
				}
			case 1: return f.instant_victory ? S(1124, "Your team won a World Government (Wonder) victory!") : S(1125, "Your team won a wonder victory!");
			case 2: return f.instant_victory ? S(1126, "Your team won a World Government (Territory) victory!") : S(1127, "Your team won a territory victory!");
			case 3: return S(1128, "Your team won a technology victory!");
			case 4: return S(1129, "Your team won a score victory!");
			case 5: return S(1130, "Your team won an economic victory!");
			case 6: return f.team_style != 3 ? S(1132, "Your team had the high score at the time limit!") : S(1131, "You held out until the time limit!");
			default: return {};
		}
	}
	if (f.mode == 1) {
		if (f.ctw) return f.defeat_type == 10 ? S(1141, "Your leader was killed!") : S(1133, "You have lost the battle.");
		if (!f.team_game) {
			switch (f.victory_type) {
				case 0:
					switch (f.defeat_type) {
						case 0: return S(1134, "Your nation has been conquered!");
						case 1:
						case 2: return f.instant_defeat ? S(1135, "You lost your capital to World Government!") : S(1136, "You lost your capital!");
						case 3: return S(1137, "You lost a city in sudden death!");
						case 4: return S(1177, "You had the lowest score at the time check!");
						case 6:
						case 7: return S(1139, "You resigned or quit!");
						case 9: return S(1178, "You failed to achieve your objectives!");
						default: return S(1142, "You have lost!");
					}
				case 1: return f.instant_victory ? S(1179, "You lost to a World Government (Wonder) victory.") : S(1180, "You lost to a wonder victory.");
				case 2: return f.instant_victory ? S(1181, "You lost to a World Government (Territory) victory.") : S(1182, "You lost to a territory victory.");
				case 3: return S(1183, "You lost to a technology victory.");
				case 4: return S(1184, "You lost to a score victory.");
				case 5: return S(1185, "You lost to an economic victory.");
				case 6: return f.team_style != 3 ? S(1186, "You did not have the high score at the time limit.") : S(1157, "You did not conquer the defending nation within the time limit.");
				default: return {};
			}
		}
		switch (f.victory_type) {
			case 0:
				switch (f.defeat_type) {
					case 0: return S(1134, "Your nation has been conquered!");
					case 1:
					case 2: return f.instant_defeat ? S(1135, "You lost your capital to World Government!") : S(1136, "You lost your capital!");
					case 3: return S(1137, "You lost a city in sudden death!");
					case 4: return S(1138, "Your team had the lowest score at the time check!");
					case 6:
					case 7: return S(1139, "You resigned or quit!");
					case 9: return S(1140, "Your team failed to achieve its objectives!");
					case 10: return S(1141, "Your leader was killed!");
					default: return S(1142, "You have lost!");
				}
			case 1:
				if (f.num_teams < 3) return f.instant_victory ? S(1145, "The other team won a World Government (Wonder) victory.") : S(1146, "The other team won a wonder victory.");
				return f.instant_victory ? S(1143, "Another team won a World Government (Wonder) victory.") : S(1144, "Another team won a wonder victory.");
			case 2:
				if (f.num_teams < 3)
					return f.instant_victory ? S(1149, "The other team won a World Government (Territory) victory.") : S(1150, "The other team won a territory victory.");
				return f.instant_victory ? S(1147, "Another team won a World Government (Territory) victory.") : S(1148, "Another team won a territory victory.");
			case 3: return f.num_teams < 3 ? S(1152, "The other team won a technology victory.") : S(1151, "Another team won a technology victory.");
			case 4: return f.num_teams < 3 ? S(1154, "The other team won a score victory.") : S(1153, "Another team won a score victory.");
			case 5: return f.num_teams < 3 ? S(1156, "The other team won an economic victory.") : S(1155, "Another team won an economic victory.");
			case 6: return f.team_style != 3 ? S(1158, "Your team did not have the high score at the time limit.") : S(1157, "You did not conquer the defending nation within the time limit.");
			default: return {};
		}
	}
	return S(1159, "All players lose!");
}

const char* kind_of(const Facts& f) {
	if (f.mode == 2) return "armageddon";
	switch (f.victory_type) {
		case 1: return "wonder";
		case 2: return "territory";
		case 3: return "tech";
		case 4: return "score";
		case 5: return "economic";
		case 6: return "time limit";
		default: break;
	}
	switch (f.defeat_type) {
		case 1:
		case 2: return "capital";
		case 3: return "sudden death";
		case 4: return "chairs";
		case 6:
		case 7: return "resign";
		case 5: return "allied";
		default: return "conquest";
	}
}

}  // namespace

Info make_info(const ron::sim::Game& g, int console, const ron::StringTables* tables, const Names& names, const std::string& victory_message,
               const std::string& defeat_message) {
	namespace lf = ron::sim::leaderflag;
	Info info;
	info.frames = g.frame;
	info.ctw = g.ctw;
	info.time_label = str(tables, 1193, "Game Time: ");
	info.score_label = str(tables, 1194, "Your Score: ");
	info.quit_caption = g.ctw ? str(tables, 1114, "Continue to Strategic Map") : uistr::tr("Quit");
	console = std::clamp(console, 0, 7);
	const ron::sim::Leader& me = g.leaders[size_t(console)];
	info.score = me.score;
	Facts f;
	f.num_players = g.info.num_players;
	f.num_teams = ron::sim::game_team_counts(g).num_teams;
	f.team_style = g.info.team_style;
	f.ctw = g.ctw;
	// EndGameWin::exec: Armageddon when the count reached the limit, else victory when the viewer is not defeated.
	if (!(g.armageddon < ron::sim::get_armageddon(g))) {
		f.mode = 2;
		f.team_game = false;
	} else if (!(me.flags & lf::kDefeated)) {
		f.mode = 0;
		f.victory_type = me.victory_type;
		f.instant_victory = (me.flags2 & ron::sim::leaderflag2::kVictoryFlag) != 0;
		f.team_game = ron::sim::num_team_members(g, console, 0) > 1;
		int last = -1;
		bool inst = false;
		f.defeat_type = -1;
		for (int i = 0; i < 8; i++) {
			const ron::sim::Leader& L = g.leaders[size_t(i)];
			if ((L.flags & (lf::kValid | lf::kDefeated)) != (lf::kValid | lf::kDefeated)) continue;
			if ((L.defeat_type == 6 || L.defeat_type == 7) && last != -1) continue;
			if (L.flags2 & ron::sim::leaderflag2::kDefeatFlag) inst = true;
			f.instant_defeat = inst;
			if (last != -1 && last != 6 && last != 7) {
				f.defeat_type = 0;
				break;
			}
			last = L.defeat_type;
			f.defeat_type = last;
		}
	} else {
		f.mode = 1;
		f.defeat_type = me.defeat_type;
		f.instant_defeat = (me.flags2 & ron::sim::leaderflag2::kDefeatFlag) != 0;
		bool inst = false;
		for (int i = 0; i < 8; i++) {
			const ron::sim::Leader& L = g.leaders[size_t(i)];
			if ((L.flags & (lf::kValid | lf::kVictory)) != (lf::kValid | lf::kVictory)) continue;
			f.victory_type = L.victory_type;
			if (L.flags2 & ron::sim::leaderflag2::kVictoryFlag) inst = true;
			f.instant_victory = inst;
			f.team_game = ron::sim::num_team_members(g, i, 0) > 1;
		}
	}
	info.mode = f.mode;
	info.kind = kind_of(f);
	if (f.mode == 2) info.title = str(tables, 1189, "ARMAGEDDON!");
	else info.title = f.mode == 0 ? str(tables, 1187, "VICTORY!") : str(tables, 1188, "DEFEAT!");
	const std::string& scen = f.mode == 0 ? victory_message : defeat_message;
	if (f.mode != 2 && !scen.empty() && (g.scripted_rules || g.ctw)) info.message = scen;
	else if (f.mode != 2) info.message = sentence(tables, f);
	// Game Stats: every valid leader with the terms Leader::compute_score added up, best score first.
	for (int i = 0; i < 8; i++) {
		const ron::sim::Leader& L = g.leaders[size_t(i)];
		if (!(L.flags & lf::kValid)) continue;
		Row r;
		r.who = i;
		r.name = names.leader ? names.leader(i) : std::string("Player ") + std::to_string(i + 1);
		r.color = std::clamp(L.color, 0, 8);
		r.age = names.age ? names.age(L.age) : std::string();
		r.units = L.score_units;
		r.buildings = L.score_buildings;
		r.economy = L.score_economy;
		r.research = L.score_research + L.score_upgrades;
		r.territory = L.score_territory;
		r.wonders = L.score_wonders;
		r.score = L.score;
		r.outcome = (L.flags & lf::kVictory) ? 1 : (L.flags & lf::kDefeated) ? 2 : 0;
		r.local = i == console;
		info.rows.push_back(std::move(r));
	}
	std::stable_sort(info.rows.begin(), info.rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });
	return info;
}

bool Screen::init(const ron::IfaceLayout& layout) {
	win_ = layout.find("ENDGAMEWIN");
	rm_ = layout.find("IFACERENDERMANAGER");
	return win_ != nullptr && rm_ != nullptr;
}

void Screen::open(Info info) {
	info_ = std::move(info);
	stats_ = false;
}

std::vector<std::string> Screen::items() const {
	if (stats_) return {uistr::tr("OK")};
	// The captions are the BUTTON titles of interface.xml; Quit becomes "Continue to Strategic Map" in a campaign battle.
	return {uistr::tr("Examine Map"), uistr::tr("Game Stats"), uistr::tr("View Playback"), info_.quit_caption.empty() ? uistr::tr("Quit") : info_.quit_caption};
}

Screen::Action Screen::action(int i) const {
	if (stats_) return i == 0 ? Action::Close : Action::None;
	switch (i) {
		case 0: return Action::Examine;
		case 1: return Action::Stats;
		case 2: return Action::Playback;
		case 3: return info_.ctw ? Action::Continue : Action::Quit;
		default: return Action::None;
	}
}

// EndGameWin::update_visibility: View Playback is enabled only with the replay preference and outside scripted / campaign games
// and multiplayer; the port records no playback, so it stays disabled.
bool Screen::enabled(int i) const { return stats_ || i != 2; }

void Screen::scroll_plate(UiRenderer& ui, IfaceRect r) const {
	IfacePiece p[9];
	for (int i = 0; i < 9; i++) p[i] = ron::iface_find_piece(*rm_, "scroll", i);
	if (!p[0].valid()) {
		ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), argb(200, 0, 0, 0));
		return;
	}
	const int lw = p[0].width, rw = p[2].width, th = p[0].height, bh = p[6].height;
	const int mw = std::max(0, r.w() - lw - rw), mh = std::max(0, r.h() - th - bh);
	const int xs[3] = {r.l, r.l + lw, r.l + lw + mw}, ws[3] = {lw, mw, rw};
	const int ys[3] = {r.t, r.t + th, r.t + th + mh}, hs[3] = {th, mh, bh};
	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			if (ws[col] <= 0 || hs[row] <= 0) continue;
			ui.add(p[row * 3 + col], float(xs[col]), float(ys[row]), float(ws[col]), float(hs[row]), kBlack, UiType::Alpha);
		}
	}
}

// A TEX_DIPLO button (butnormal / buthighlight / butdown 0..2 of ENDGAMEWIN): 12 + stretched + 12, the caption in font 14 bold.
void Screen::button(UiRenderer& ui, const IfaceRect& r, const std::string& caption, int state, bool enabled, int font) const {
	const char* kind = state == 2 ? "butdown" : state == 1 ? "buthighlight" : "butnormal";
	IfacePiece p[3];
	for (int k = 0; k < 3; k++) p[k] = ron::iface_find_piece(*win_, std::string(kind) + std::to_string(k));
	if (p[0].valid() && p[1].valid() && p[2].valid()) ui.expander(p[0], p[1], p[2], r);
	else ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), argb(255, 70, 70, 70));
	ui.next_layer();
	IfaceRect t = r;
	if (state == 2) t.l += 1, t.r += 1, t.t += 1, t.b += 1;
	ui.text_centered(font, caption, t, enabled ? kWhite : argb(255, 130, 130, 130), kBlack);
	ui.next_layer();
}

void Screen::draw(UiRenderer& ui, int ui_w, int ui_h, int hover, int press, std::vector<IfaceRect>& rects) const {
	rects.clear();
	if (!win_ || !rm_) return;
	ui.next_window();
	ui.fill(0, 0, float(ui_w), float(ui_h), argb(90, 0, 0, 0), UiType::ColorBlend);
	ui.next_layer();
	if (stats_) draw_stats(ui, ui_w, ui_h, hover, press, rects);
	else draw_window(ui, ui_w, ui_h, hover, press, rects);
}

// EndGameWin::on_redraw: rect names are those of interface.xml (title_*, message_*, banner, trophy_*, time, score).
void Screen::draw_window(UiRenderer& ui, int ui_w, int ui_h, int hover, int press, std::vector<IfaceRect>& rects) const {
	const int x0 = (ui_w - kWinW) / 2, y0 = std::max(kScrollPadY + 4, (ui_h - kWinH) / 2 - 30);
	auto rect_of = [&](const char* name) {
		const ron::XmlElement* e = ron::iface_child(*win_, "IFACERECT", name);
		if (!e) return IfaceRect{0, 0, 0, 0};
		const int l = e->get_int("left"), t = e->get_int("top"), w = e->get_int("width"), h = e->get_int("height");
		return IfaceRect{x0 + l, x0 + l + w, y0 + t, y0 + t + h};
	};
	const int title_f = ui.font(18, ron::kFontBold), msg_f = ui.font(14, ron::kFontBold), small_f = ui.font(14, ron::kFontBold);
	ui.next_window();
	scroll_plate(ui, {x0 - kScrollPadX, x0 + kWinW + kScrollPadX, y0 - kScrollPadY, y0 + kWinH + kScrollPadY});
	ui.next_layer();
	const char* suffix = info_.mode == 0 ? "victory" : "defeat";
	if (info_.mode == 2) {
		const IfacePiece tp = ron::iface_find_piece(*win_, "trophy_arma");
		const IfaceRect r = rect_of("trophy_arma");
		if (tp.valid()) ui.add(tp, float(r.l), float(r.t), float(r.w()), float(r.h()), kWhite, UiType::Alpha);
	} else {
		const IfacePiece banner = ron::iface_find_piece(*win_, std::string("banner_") + suffix);
		const IfacePiece trophy = ron::iface_find_piece(*win_, std::string("trophy_") + suffix);
		const IfaceRect br = rect_of("banner"), tr = rect_of(info_.mode == 0 ? "trophy_victory" : "trophy_defeat");
		if (banner.valid()) ui.add(banner, float(br.l), float(br.t), float(br.w()), float(br.h()), kWhite, UiType::Alpha);
		ui.next_layer();
		if (trophy.valid()) ui.add(trophy, float(tr.l), float(tr.t), float(tr.w()), float(tr.h()), kWhite, UiType::Alpha);
	}
	ui.next_layer();
	const IfaceRect title = rect_of(info_.mode == 0 ? "title_victory" : info_.mode == 1 ? "title_defeat" : "title_arma");
	ui.text_centered(title_f, info_.title, title, kWhite, kBlack);
	if (info_.mode != 2 && !info_.message.empty()) {  // Buffer::wrap_cent: the sentence wrapped in the rect and centred in both directions
		const IfaceRect mr = rect_of(info_.mode == 0 ? "message_victory" : "message_defeat");
		const ron::FontMetrics* m = ui.font_metrics(msg_f);
		std::vector<std::string> lines;
		std::string cur;
		size_t i = 0;
		const std::string& text = info_.message;
		while (i < text.size()) {  // greedy on spaces
			size_t j = text.find(' ', i);
			const std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
			const std::string trial = cur.empty() ? word : cur + " " + word;
			if (!cur.empty() && ui.text_width(msg_f, trial) > mr.w()) lines.push_back(cur), cur = word;
			else cur = trial;
			if (j == std::string::npos) break;
			i = j + 1;
		}
		if (!cur.empty()) lines.push_back(cur);
		const int lh = m ? m->line_height : 16;
		int y = mr.t + (mr.h() - int(lines.size()) * lh) / 2;
		for (const std::string& l : lines) {
			ui.text(msg_f, l, float(mr.l + (mr.w() - ui.text_width(msg_f, l)) / 2), float(y), kWhite, kBlack);
			y += lh;
		}
	}
	// "Game Time:" / "Your Score:" in the two boxes (IFACEBORDER small_border), the label right-aligned against the box.
	IfaceBorder border;
	if (const ron::XmlElement* be = ron::iface_child(*win_, "IFACEBORDER", "small_border")) border = ron::iface_border(*be);
	const struct {
		const char* rect;
		std::string label, value;
	} boxes[2] = {{"time", info_.time_label, clock_text(info_.frames)}, {"score", info_.score_label, std::to_string(info_.score)}};
	for (const auto& b : boxes) {
		const IfaceRect r = rect_of(b.rect);
		ui.next_layer();
		ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), argb(150, 0, 0, 0), UiType::ColorBlend);
		ui.next_layer();
		if (border.slot[0].valid()) ui.border(border, r);
		ui.next_layer();
		ui.text_centered(small_f, b.value, r, kWhite, kBlack);
		std::string label = b.label;
		while (!label.empty() && label.back() == ' ') label.pop_back();
		const ron::FontMetrics* fm = ui.font_metrics(small_f);
		const int ty = r.t + (r.h() - (fm ? fm->font_height : 14)) / 2;
		ui.text(small_f, label, float(r.l - 6 - ui.text_width(small_f, label)), float(ty), kWhite, kBlack);
	}
	// The buttons: POSITIONING x = 0, y = 168, 35 apart; the XML widths (250) are fitted to the 248 px window.
	const std::vector<std::string> caps = items();
	for (size_t k = 0; k < caps.size(); k++) {
		const int by = y0 + 168 + int(k) * 35;
		const IfaceRect r{x0, x0 + kWinW, by, by + 32};
		rects.push_back(r);
		const bool en = enabled(int(k));
		button(ui, r, caps[k], !en ? 0 : press == int(k) ? 2 : hover == int(k) ? 1 : 0, en, small_f);
	}
}

// The post-game score table: one row per leader, best score first, the terms of Leader::compute_score (units, buildings, economy,
// research and upgrades, territory, wonders) and their sum; the viewer's row is marked with a lighter plate.
void Screen::draw_stats(UiRenderer& ui, int ui_w, int ui_h, int hover, int press, std::vector<IfaceRect>& rects) const {
	const int title_f = ui.font(18, ron::kFontBold), head_f = ui.font(14, ron::kFontBold), row_f = ui.font(14, -1);
	const ron::FontMetrics* hm = ui.font_metrics(head_f);
	const int rh = (hm ? hm->line_height : 16) + 6;
	const int cols = 9;
	const int widths[cols] = {170, 110, 62, 80, 74, 78, 78, 72, 70};
	int w = 40;
	for (int cw : widths) w += cw;
	const int h = 44 + rh + int(info_.rows.size()) * rh + 20 + 32 + 28;
	const int x = (ui_w - w) / 2, y = std::max(kScrollPadY + 4, (ui_h - h) / 2 - 20);
	ui.next_window();
	scroll_plate(ui, {x - kScrollPadX, x + w + kScrollPadX, y - kScrollPadY, y + h + kScrollPadY});
	ui.next_layer();
	ui.text_centered(title_f, uistr::tr("Game Stats"), {x, x + w, y + 8, y + 36}, kWhite, kBlack);
	// The labels are the install's own (StatWin::draw_normal_stats "Army Units:", "Economy:", "Research:", "Wonders:" ...); the strings are
	// positional, so only the texts known for sure are looked up and trimmed of their colon and padding.
	const char* heads[cols] = {"Player", "Age", "Units", "Buildings", "Economy", "Research", "Territory", "Wonders", "Score"};
	std::vector<int> xs(cols + 1);
	xs[0] = x + 20;
	for (int c = 0; c < cols; c++) xs[size_t(c) + 1] = xs[size_t(c)] + widths[c];
	int ry = y + 44;
	ui.fill(float(x + 14), float(ry + rh - 2), float(w - 28), 1.0f, argb(160, 255, 255, 255), UiType::ColorBlend);
	ui.next_layer();
	for (int c = 0; c < cols; c++) {
		const std::string t = uistr::tr(heads[c]);
		if (c <= 1) ui.text(head_f, t, float(xs[size_t(c)]), float(ry + 2), argb(255, 255, 255, 160), kBlack);
		else ui.text(head_f, t, float(xs[size_t(c) + 1] - 6 - ui.text_width(head_f, t)), float(ry + 2), argb(255, 255, 255, 160), kBlack);
	}
	ry += rh;
	for (const Row& r : info_.rows) {
		if (r.local) ui.fill(float(x + 14), float(ry - 1), float(w - 28), float(rh), argb(60, 255, 255, 255), UiType::ColorBlend);
		ui.next_layer();
		const uint8_t* tc = kText[std::clamp(r.color, 0, 8)];
		const std::string& name = r.name;
		ui.text(row_f, name, float(xs[0]), float(ry + 2), argb(255, tc[0], tc[1], tc[2]), kBlack);
		ui.text(row_f, r.age, float(xs[1]), float(ry + 2), kWhite, kBlack);
		const int vals[7] = {r.units, r.buildings, r.economy, r.research, r.territory, r.wonders, r.score};
		for (int c = 2; c < cols; c++) {
			const std::string t = std::to_string(vals[c - 2]);
			ui.text(row_f, t, float(xs[size_t(c) + 1] - 6 - ui.text_width(row_f, t)), float(ry + 2), c == cols - 1 ? (r.outcome == 1 ? argb(255, 255, 215, 0) : r.outcome == 2 ? argb(255, 170, 170, 170) : kWhite) : kWhite, kBlack);
		}
		ry += rh;
	}
	// The single button.
	const std::vector<std::string> caps = items();
	const IfaceRect br{x + (w - 173) / 2, x + (w - 173) / 2 + 173, y + h - 32 - 14, y + h - 14};
	rects.push_back(br);
	button(ui, br, caps[0], press == 0 ? 2 : hover == 0 ? 1 : 0, true, head_f);
}

}  // namespace endgame
