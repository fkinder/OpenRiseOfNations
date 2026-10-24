#include "script_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "assets/xml.hpp"
#include "sim/game.hpp"
#include "sim/rules_xml.hpp"

using ron::IfacePiece;
using ron::IfaceRect;
using ron::metal::argb;
using ron::metal::rgb;
using ron::metal::UiRenderer;
using ron::metal::UiType;
using ron::sim::ScriptEvent;
using ron::sim::ScriptPresentation;

namespace scriptview {

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFF, kBlack = 0xFF000000, kRed = 0xFFFF0000;
constexpr int kMessageLength = 5;                    // player_profile.prefs.message_length default
constexpr int kStdTicks = kMessageLength * 45;       // MessageWin's standard duration (225 ticks)
constexpr int kScrollPadX = 8, kScrollPadY = 4;      // IFaceRenderManager scroll_pad_* (ui-spec 6.5)

// TeamColors::init defaults read from the exe's basecolors.obj data (TEAM_COLOR_NEON_* / TEAM_COLOR_TEXT_*), team slots
// red, blue, pink, green, yellow, cyan, white, orange, 8 = white.
constexpr uint8_t kNeon[9][3] = {{255, 0, 0},     {70, 100, 255}, {188, 0, 250},   {0, 255, 0},     {255, 255, 0},
                                 {0, 255, 255},   {255, 255, 255}, {255, 128, 0},  {255, 255, 255}};
constexpr uint8_t kText[9][3] = {{255, 100, 100}, {70, 100, 255}, {188, 0, 250},   {0, 236, 33},    {255, 239, 55},
                                 {0, 255, 255},   {255, 255, 255}, {255, 182, 6},  {255, 255, 255}};

// The presentation queue's colours are RGBA (sim/script_ui.hpp scriptcolor); the UI layer takes ARGB.
uint32_t rgba_to_argb(uint32_t c) { return (c & 0xffu) << 24 | c >> 8; }

}  // namespace

bool View::init(const ron::GameFiles& files, const ron::IfaceLayout& layout, std::string& error) {
	files_ = &files;
	layout_ = &layout;
	rm_el_ = layout.find("IFACERENDERMANAGER");
	msg_el_ = layout.find("MESSAGEWIN");
	popup_el_ = layout.find("POPUP");
	if (!rm_el_ || !msg_el_) {
		error = "interface.xml lacks IFACERENDERMANAGER / MESSAGEWIN";
		return false;
	}
	try {
		const ron::XmlElement eg = ron::parse_xml(files.read_text("Data/effects_graphics.xml"));
		ron::for_each_element(eg, "SPLINEARROW", [&](const ron::XmlElement& e) {
			arrow_tex_ = ron::normalize_game_path(e.get("texture"));
			arrow_head_px_ = e.get_int("tex_arrowhead", 18);
		});
		// EFFECTS/MINIMAP/PING styles, in the order notices::Note::ping indexes them.
		static const char* order[7] = {"default", "attack", "wonder", "citylost", "citywon", "waypoint", "resource"};
		ping_styles_.assign(7, hud::PingStyle());
		ron::for_each_element(eg, "PING", [&](const ron::XmlElement& e) {
			const std::string name = ron::to_lower(e.get("name"));
			for (int i = 0; i < 7; i++) {
				if (name != order[i] || !ping_styles_[size_t(i)].phases.empty()) continue;
				hud::PingStyle& st = ping_styles_[size_t(i)];
				st.name = name;
				st.rotates = e.get_int("rotates") != 0;
				st.rotate_speed = e.get_float("rotate_speed");
				for (const ron::XmlElement& ph : e.children) {
					if (ph.name != "PHASE") continue;
					hud::PingPhase p;
					p.start = ph.get_float("start_pct", 1), p.end = ph.get_float("end_pct", 0);
					p.color = uint32_t(std::strtoul(ph.get("color", "0xFFFFFFFF").c_str(), nullptr, 16));
					p.usealpha = ph.get_int("usealpha") != 0, p.usescale = ph.get_int("usescale") != 0;
					p.scaleby = ph.get_float("scaleby"), p.scalespeed = ph.get_float("scalespeed");
					p.file = ron::normalize_game_path(ph.get("texture"));
					st.phases.push_back(p);
				}
			}
		});
		const ron::XmlElement sg = ron::parse_xml(files.read_text("Data/soundglobals.xml"));
		ron::for_each_element(sg, "GLOBAL", [&](const ron::XmlElement& e) {
			const ron::XmlElement* k = e.child("KEY");
			if (!k) return;
			std::vector<std::string> tags;
			for (const ron::XmlElement& c : e.children) {
				if (c.name == "SOUND") tags.push_back(ron::to_lower(c.get("#text")));
			}
			globals_.push_back({k->get("#text"), tags});
		});
		const ron::XmlElement sf = ron::parse_xml(files.read_text("Data/soundfiles.xml"));
		ron::for_each_element(sf, "SOUNDFILE", [&](const ron::XmlElement& e) {
			sound_files_.push_back({ron::to_lower(e.get("tag")), ron::normalize_game_path(e.get("file"))});
		});
	} catch (const std::exception& e) {
		std::fprintf(stderr, "script view: %s\n", e.what());
	}
	return true;
}

std::string View::sound_file(const std::string& key) {
	for (const auto& [k, tags] : globals_) {
		if (k != key || tags.empty()) continue;
		rng_ = rng_ * 1103515245u + 12345u;  // SoundGlobal::play: a random entry of the category
		const std::string& tag = tags[(rng_ >> 16) % tags.size()];
		for (const auto& [t, f] : sound_files_) {
			if (t == tag) return files_->resolve(f);
		}
	}
	return {};
}

uint32_t View::neon(const Env& env, int who) const {
	int c = 8;
	if (env.game && who >= 0 && size_t(who) < env.game->leaders.size()) c = std::clamp(env.game->leaders[size_t(who)].color, 0, 8);
	return rgb(kNeon[c][0], kNeon[c][1], kNeon[c][2]);
}

uint32_t View::text_color(const Env& env, int who) const {
	int c = 8;
	if (env.game && who >= 0 && size_t(who) < env.game->leaders.size()) c = std::clamp(env.game->leaders[size_t(who)].color, 0, 8);
	return rgb(kText[c][0], kText[c][1], kText[c][2]);
}

void View::add_message(const std::string& text, uint32_t color, int ticks, const Env& env, const std::string& prefix,
                       uint32_t prefix_color) {
	if (text.empty() && prefix.empty()) return;
	Message m;
	m.text = text, m.prefix = prefix, m.color = color, m.prefix_color = prefix_color;
	m.start = env.tick;
	m.end = env.tick + ticks;
	messages_.push_back(std::move(m));
	if (messages_.size() > 200) messages_.erase(messages_.begin());
}

void View::say(const std::string& text, const Env& env) { add_message(text, 0xFFFFFFFF, kStdTicks, env); }

// A message or banner the simulation caused (notices.hpp). MessageWin::add_message with a location also calls add_event: a
// minimap ping of the note's style (a few 8-tick cycles), the ANNOUNCE_CIRCLE on the terrain and an entry Space jumps to.
void View::post(const notices::Note& n, const Env& env, const Callbacks& cb) {
	if (n.kind == notices::Note::Notice) {
		notices_.push_back({n.image, n.text, env.ms});
		return;
	}
	add_message(n.text, n.argb, n.ticks > 0 ? n.ticks : kStdTicks, env);
	if (n.located) {
		hud::Ping pg;
		pg.x = n.x, pg.y = n.y;
		pg.rgb[0] = pg.rgb[1] = pg.rgb[2] = 255;
		pg.start_tick = env.tick;
		pg.end_tick = env.tick + 8 * 6;
		pg.flash = true, pg.square = false;
		pg.style = std::clamp(n.ping, 0, int(ping_styles_.size()) - 1);
		pings_.push_back(pg);
		announces_.push_back({n.x, n.y, env.tick});
		events_.push_back({n.x, n.y});
		if (events_.size() > 200) events_.erase(events_.begin());
	}
	if (!n.sound.empty() && cb.play_effect) {
		const std::string f = sound_file(n.sound.compare(0, 2, "S_") == 0 ? n.sound.substr(2) : n.sound);
		if (!f.empty()) cb.play_effect(f);
	}
}

// MessageWin::next_event @007e9190: the location of the next located event, newest first, wrapping.
bool View::next_event(float& x, float& y) {
	if (events_.empty()) return false;
	next_event_ = (next_event_ + 1) % int(events_.size());
	const auto& e = events_[events_.size() - 1 - size_t(next_event_)];
	x = e.first, y = e.second;
	return true;
}

void View::reset(const ScriptPresentation* p) {
	messages_.clear();
	instructions_.clear();
	feedback_.clear();
	feedback_start_ = -1000000;
	scen_msg_start_ = -1;
	scen_msg_seq_ = 0;
	bubbles_.clear();
	notices_.clear();
	pings_.clear();
	announces_.clear();
	events_.clear();
	next_event_ = -1;
	marks_.clear();
	popup_open_ = goals_open_ = false;
	over_ = won_ = false;
	over_text_.clear();
	last_seq_ = 0;
	if (!p) return;
	for (const ScriptEvent& e : p->events) {
		if (e.kind == ScriptEvent::kInstruction) instructions_ = e.text;
		last_seq_ = std::max(last_seq_, e.seq);
	}
	if (p->next_seq > 0) last_seq_ = std::max(last_seq_, p->next_seq - 1);
	// Objective marks already completed in the saved game show as completed, not as newly ticked.
	for (const ron::sim::ScriptObjective& o : p->objectives) marks_.push_back({o.id, o.text, o.frame, o.completed ? 0 : -1});
}

void View::consume(const ScriptPresentation& p, const Env& env, const Callbacks& cb) {
	for (const ScriptEvent& e : p.events) {
		if (e.seq <= last_seq_) continue;
		last_seq_ = e.seq;
		switch (e.kind) {
		case ScriptEvent::kGameMessage:  // MessageWin::add_message (duration -1 = message_length * 45)
			add_message(e.text, rgba_to_argb(e.color), kStdTicks, env);
			break;
		case ScriptEvent::kScenarioMessage:  // add_scen_message: scenario_clock = graphic_tick
			scen_msg_start_ = env.tick;
			scen_msg_seq_ = e.seq;
			break;
		case ScriptEvent::kInstruction: instructions_ = e.text; break;
		case ScriptEvent::kFeedback:
			feedback_ = e.text;
			feedback_start_ = env.tick;
			break;
		case ScriptEvent::kChat: {  // LeaderOut::display_chat: "Name (Nation): " in the sender's text colour, 450 ticks
			std::string prefix;
			if (env.game && e.who >= 0 && size_t(e.who) < env.game->leaders.size()) {
				const ron::sim::Leader& L = env.game->leaders[size_t(e.who)];
				std::string nation;
				if (env.game->rules && L.tribe >= 0 && size_t(L.tribe) < env.game->rules->tribes.size())
					nation = ron::sim::to_utf8(env.game->rules->tribes[size_t(L.tribe)].name);
				prefix = L.name + (nation.empty() ? "" : " (" + nation + ")") + ": ";
			}
			add_message(e.text, kWhite, 450, env, prefix, text_color(env, e.who));
			break;
		}
		case ScriptEvent::kNotice: notices_.push_back({e.text2, e.text, env.ms}); break;
		case ScriptEvent::kBubble:  // add_bubble_message: a bubble plus a message in the console's neon colour
			bubbles_.push_back({e.text, float(e.x), float(e.y), env.tick, e.who});
			add_message(e.text, neon(env, e.who), kStdTicks, env);
			break;
		case ScriptEvent::kSound:
			if (e.a >= 0 && cb.play_voice) {
				const std::string path = files_->resolve(e.text2);
				if (!path.empty()) cb.play_voice(path);
			}
			break;
		case ScriptEvent::kStopSound:
			if (cb.stop_voice) cb.stop_voice();
			break;
		case ScriptEvent::kMusicMood:
			if (cb.music_mood) cb.music_mood(e.text);
			break;
		case ScriptEvent::kPing: {
			// Leader::receive_ping only on the target's machine (text = target leader, -1 = everyone); Scene::add_ping
			// clears the previous ping, adds a located MessageWin event (minimap ping + ANNOUNCE_CIRCLE) and plays S_PING.
			const int to = std::atoi(e.text.c_str());
			if (to >= 0 && to != env.local) break;
			pings_.clear();
			hud::Ping pg;
			pg.x = float(e.x), pg.y = float(e.y);
			const uint32_t c = neon(env, e.who);
			pg.rgb[0] = uint8_t(c >> 16), pg.rgb[1] = uint8_t(c >> 8), pg.rgb[2] = uint8_t(c);
			pg.start_tick = env.tick;
			pg.end_tick = env.tick + 180;  // ping_finish_frame = graphic_tick + 180
			pg.flash = e.a != 0;
			pg.style = 0;
			pings_.push_back(pg);
			announces_.push_back({float(e.x), float(e.y), env.tick});
			if (cb.play_effect) {
				const std::string f = sound_file("PING");
				if (!f.empty()) cb.play_effect(f);
			}
			break;
		}
		case ScriptEvent::kClearPings: pings_.clear(); break;
		case ScriptEvent::kCamera:
			if (e.text == "clear_selection") {
				if (cb.clear_selection) cb.clear_selection();
			} else if (e.text == "highlight_option") {
				if (cb.highlight_option) cb.highlight_option(env.hud ? env.hud->option_index(e.text2) : -1);
			} else if (e.a != ron::sim::CameraRequest::kNone && cb.camera) {
				cb.camera(e.a, e.x, e.y, e.who);
			}
			break;
		case ScriptEvent::kClearMessages: messages_.clear(); break;
		case ScriptEvent::kGoalDialog: goals_open_ = true; break;
		case ScriptEvent::kScreenshot:
			if (cb.screenshot) cb.screenshot(e.text);
			break;
		case ScriptEvent::kPause:
			if (cb.pause) cb.pause(e.a != 0);
			break;
		case ScriptEvent::kVictoryScreen:  // display_results / display_aging_results / save_results: the scenario's high-score
			break;                         // lists, which the original writes into the objectives (nothing posts this yet)
		case ScriptEvent::kPopup: break;  // answered synchronously through ScriptInput::popup
		}
	}
	// Objective completion stamps in graphic ticks (end_scen_objective's graphic_tick; the check box wipes in over 15
	// ticks and the entry goes 30 ticks later).
	std::vector<ObjectiveMark> marks;
	for (const ron::sim::ScriptObjective& o : p.objectives) {
		ObjectiveMark m{o.id, o.text, o.frame, -1};
		for (const ObjectiveMark& old : marks_) {
			if (old.id == o.id && old.frame == o.frame) m.done_tick = old.done_tick;
		}
		if (o.completed && m.done_tick < 0) m.done_tick = env.tick;
		if (!o.completed) m.done_tick = -1;
		marks.push_back(m);
	}
	marks_ = std::move(marks);
}

void View::game_over(bool won, const std::string& message, const Env& env, const Callbacks& cb) {
	if (over_) return;
	over_ = true;
	won_ = won;
	over_text_ = message;
	notices_.push_back({won ? "victory_notice" : "defeat_notice", won ? "Victory!" : "Defeat!", env.ms});
	if (cb.play_effect) {
		const std::string f = sound_file(won ? "YOU_WIN" : "YOU_LOSE");
		if (!f.empty()) cb.play_effect(f);
	}
}

void View::open_popup(const ScriptEvent& e) {
	popup_open_ = true;
	popup_ = Popup{};
	popup_.text = e.text;
	for (const std::string* b : {&e.text2, &e.text3, &e.text4}) {
		if (int(popup_.buttons.size()) < e.a) popup_.buttons.push_back(b->empty() ? "OK" : *b);
	}
	if (popup_.buttons.empty()) popup_.buttons.push_back("OK");
}

std::vector<std::string> View::wrap(UiRenderer& ui, int f, const std::string& text, int width) const {
	// Buffer::wrap: greedy on spaces, hard breaks on newlines; a word wider than the line stands alone.
	std::vector<std::string> lines;
	size_t i = 0;
	while (i <= text.size()) {
		size_t nl = text.find('\n', i);
		const std::string para = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
		std::string line;
		size_t k = 0;
		while (k < para.size()) {
			size_t sp = para.find(' ', k);
			const std::string word = para.substr(k, sp == std::string::npos ? std::string::npos : sp - k);
			const std::string cand = line.empty() ? word : line + " " + word;
			if (!line.empty() && ui.text_width(f, cand) > width) {
				lines.push_back(line);
				line = word;
			} else {
				line = cand;
			}
			if (sp == std::string::npos) break;
			k = sp + 1;
		}
		lines.push_back(line);
		if (nl == std::string::npos) break;
		i = nl + 1;
	}
	return lines;
}

// IFaceRenderManager::draw_scroll @0051cdb0: the IFACERENDERMANAGER "scroll" 9-slice (TEX_CIV) stretched over the rect
// (corners native, edges and middle stretched), tinted.
void View::scroll_plate(UiRenderer& ui, IfaceRect r, uint32_t tint) const {
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

// MessageWin::draw_scen_objectives @007e75b0.
int View::draw_objectives(UiRenderer& ui, const ScriptPresentation& p, const Env& env, int x, int y) {
	objectives_rect_ = {0, 0, 0, 0};
	struct Row {
		const ron::sim::ScriptObjective* o;
		int done;
	};
	std::vector<Row> rows;
	for (size_t i = 0; i < p.objectives.size(); i++) {
		const int done = i < marks_.size() ? marks_[i].done_tick : -1;
		if (done >= 0 && float(env.tick - done) / 15.0f > 2.0f) continue;  // removed 30 ticks after completion
		if (p.objectives[i].text.empty()) continue;
		rows.push_back({&p.objectives[i], done});
	}
	if (rows.empty()) return y;
	const IfacePiece on = ron::iface_find_piece(*msg_el_, "check_on"), off = ron::iface_find_piece(*msg_el_, "check_off");
	bool checks = false;
	for (const Row& r : rows) checks = checks || r.o->announce;
	if (checks && (!on.valid() || !off.valid())) return y;
	const int cw = checks ? off.width + 6 : 0, ch = checks ? off.height + 6 : 0;
	const int f = ui.font_tag("scenario_objectives");
	const ron::FontMetrics* m = ui.font_metrics(f);
	if (!m) return y;
	const int fh = m->font_height, lh = m->line_height;
	int text_w = 0;
	for (const Row& r : rows) text_w = std::max(text_w, ui.text_width(f, r.o->text));
	int w = text_w + cw;
	if (w > env.ui_w / 3) {
		w = env.ui_w / 3;
		text_w = w - cw;
	}
	int h = 0;
	std::vector<std::vector<std::string>> lines;
	for (const Row& r : rows) {
		lines.push_back(wrap(ui, f, r.o->text, checks ? text_w : w));
		const int wh = int(lines.back().size()) * lh;
		// Text-only entries: the decompile sizes them wrap_height + 1 but advances wrap + 5 when drawing; the box is sized
		// for the drawing so the last line stays on the plate.
		h += checks ? 2 * ch - off.height - fh + wh : wh + 5;
	}
	if (checks) h += off.height - ch;
	else h -= 4;
	objectives_rect_ = {x, x + w, y, y + h};
	scroll_plate(ui, {x - kScrollPadX, x + w + kScrollPadX, y - kScrollPadY, y + h + kScrollPadY}, kBlack);
	ui.next_layer();
	int cy = y;
	for (size_t i = 0; i < rows.size(); i++) {
		const Row& r = rows[i];
		if (checks && r.o->announce) {
			ui.add(off, float(x), float(cy), 0, 0, kWhite, UiType::Alpha2);
			if (r.done >= 0) {
				const float fr = std::min(1.0f, float(env.tick - r.done) / 15.0f);
				if (fr > 0) ui.add_sub(on, 0, 0, float(on.width) * fr, float(on.height), float(x), float(cy), float(on.width) * fr,
				                       float(on.height), kWhite, UiType::Alpha2);
			}
		}
		const uint32_t col = rgba_to_argb(r.o->color);
		if (checks) {
			int ty = cy + (ch - fh) / 2;
			for (const std::string& l : lines[i]) {
				ui.text(f, l, float(x + cw), float(ty), col);
				ty += lh;
			}
			cy = ty + (ch - fh);
		} else {
			for (const std::string& l : lines[i]) {
				ui.text(f, l, float(x), float(cy), col);
				cy += lh;
			}
			cy += 5;
		}
	}
	ui.next_layer();
	return y + h + kScrollPadY + 10;
}

// MessageWin::on_redraw's list: the last 10 live entries, wrapped to the running maximum of min(width, w / 3), on one
// 0x50000000 quad with 3 px of padding; colours with a 1,1 black shadow.
void View::draw_messages(UiRenderer& ui, const Env& env, int x, int y) {
	const int f = ui.font_tag("message_size");
	const ron::FontMetrics* m = ui.font_metrics(f);
	if (!m) return;
	std::vector<const Message*> live;
	for (const Message& msg : messages_) {
		if (env.tick < msg.end) live.push_back(&msg);
	}
	if (live.size() > 10) live.erase(live.begin(), live.end() - 10);
	if (live.empty()) return;
	int wrap_w = 0;
	for (const Message* msg : live) wrap_w = std::max(wrap_w, std::min(ui.text_width(f, msg->prefix + msg->text), env.ui_w / 3));
	std::vector<std::vector<std::string>> lines;
	int total = 0;
	for (const Message* msg : live) {
		lines.push_back(wrap(ui, f, msg->prefix + msg->text, wrap_w));
		total += int(lines.back().size());
	}
	ui.fill(float(x - 3), float(y - 3), float(wrap_w + 6), float(total * m->line_height + 6), 0x50000000, UiType::ColorBlend);
	ui.next_layer();
	int cy = y;
	for (size_t i = 0; i < live.size(); i++) {
		for (size_t k = 0; k < lines[i].size(); k++) {
			const std::string& l = lines[i][k];
			if (k == 0 && !live[i]->prefix.empty() && l.size() >= live[i]->prefix.size()) {
				const std::string pre = l.substr(0, live[i]->prefix.size()), rest = l.substr(live[i]->prefix.size());
				ui.text(f, pre, float(x), float(cy), live[i]->prefix_color, kBlack);
				ui.text(f, rest, float(x + ui.text_width(f, pre)), float(cy), live[i]->color, kBlack);
			} else {
				ui.text(f, l, float(x), float(cy), live[i]->color, kBlack);
			}
			cy += m->line_height;
		}
	}
	ui.next_layer();
}

// Feedback (red plate, white text with a black shadow, message_length * 45 ticks) or instructions (black plate, white
// text), then the scenario message (print_msg: red plate, white text, scenario_duration ticks; -1 forever): centred,
// width min(text, w / 2) + 1, top at 45 (100 with the mini library open; it is not drawn yet).
void View::draw_top_centre(UiRenderer& ui, const ScriptPresentation* p, const Env& env) {
	const int top = env.hud && env.hud_visible ? env.hud->feedback_top() : 45;  // 100 with the mini library expanded
	auto box = [&](int f, const std::string& text, uint32_t plate, uint32_t shadow, int extra) {
		const ron::FontMetrics* m = ui.font_metrics(f);
		if (!m || text.empty()) return;
		const int w = std::min(ui.text_width(f, text), env.ui_w / 2) + extra;
		const std::vector<std::string> lines = wrap(ui, f, text, w);
		const int h = int(lines.size()) * m->line_height;
		const int x = (env.ui_w - w) / 2;
		scroll_plate(ui, {x - kScrollPadX, x + w + kScrollPadX, top - kScrollPadY, top + h + kScrollPadY}, plate);
		ui.next_layer();
		int y = top;
		for (const std::string& l : lines) {
			ui.text(f, l, float(x + (w - ui.text_width(f, l)) / 2), float(y), kWhite, shadow);
			y += m->line_height;
		}
		ui.next_layer();
	};
	const int ff = ui.font_tag("feedback_size");
	if (!feedback_.empty() && env.tick - feedback_start_ <= kStdTicks) {
		box(ff, feedback_, kRed, kBlack, 1);
	} else if (!instructions_.empty()) {
		box(ff, instructions_, kBlack, 0, 1);
	}
	if (p && !p->scenario_message.empty() && scen_msg_start_ >= 0) {
		const int dur = p->scenario_message_ms;  // ScenarioData::msg_time: graphic ticks (set_msg_time = seconds * 20), -1 forever
		if (dur != 0 && (dur < 0 || env.tick - scen_msg_start_ <= dur)) box(ui.font_tag("scenario_size"), p->scenario_message, kRed, 0, 0);
	}
}

// TextBubble::draw_update @007e6820: lifetime 120 ticks (message_length 5); grows over 12 ticks (scale 0.2 -> 1, alpha
// 128 -> 255, the rect rising on ((2s - 1)^2 - 1) * 50 px) and fades over the last 36; rect +-100 x (-70 .. +10) px
// around the projected point, bubble_size font, the neon team colour with a black shadow.
void View::draw_bubbles(UiRenderer& ui, const ScriptPresentation* p, const Env& env) {
	if (p && !p->display_bubble_text) {
		bubbles_.clear();
		return;
	}
	const int life = 120;
	const int f = ui.font_tag("bubble_size");
	const ron::FontMetrics* m = ui.font_metrics(f);
	if (!m) return;
	for (size_t i = 0; i < bubbles_.size();) {
		const Bubble& b = bubbles_[i];
		const int t = env.tick - b.start;
		float s = 1, alpha = 255;
		bool dead = false;
		if (t < 12) {
			s = float(t) / 12.0f * 0.8f + 0.2f;
			alpha = float(t) / 12.0f * 128.0f + 128.0f;
		} else if (t - 12 >= life - 120) {
			const float fr = float((t - 12) - (life - 120)) / float(std::min(life - 12, 108)) * 3.0f;
			if (fr >= 1) dead = true;
			alpha = (1 - fr) * 255;
		}
		if (dead) {
			bubbles_.erase(bubbles_.begin() + long(i));
			continue;
		}
		float sx, sy;
		if (env.project && env.project(b.x, b.y, sx, sy)) {
			const float k = s * 2 - 1;
			const float off = (k * k - 1) * 50;
			const float top = sy - 70 + off, bottom = sy + 10 + off;
			const uint32_t c = neon(env, b.color_leader);
			const int a = int(std::clamp(alpha, 0.0f, 255.0f));
			const float tw = float(ui.text_width(f, b.text)) * s, th = float(m->font_height) * s;
			ui.text_scaled(f, b.text, sx - tw / 2, (top + bottom) / 2 - th / 2, s, (c & 0x00FFFFFF) | uint32_t(a) << 24,
			               uint32_t(a) << 24);
		}
		i++;
	}
	ui.next_layer();
}

// AnimNotice (ui-spec 6.1): the TEX_ALERTS icon starts centred at native size and, after message_length * 400 + 500 ms,
// moves in 500 ms to half size at (w, h); 22 px bold white text with a black shadow 5 px under the icon while waiting;
// a scroll plate behind.
void View::draw_notices(UiRenderer& ui, const Env& env) {
	const double delay = kMessageLength * 400 + 500;
	const int f = ui.font(22, ron::kFontBold);  // "22 px bold" (AnimNotice::init)
	for (size_t i = 0; i < notices_.size();) {
		const Notice& n = notices_[i];
		const double fr = std::clamp((env.ms - n.start_ms - delay) / 500.0, 0.0, 1.0);
		if (fr >= 1.0) {
			notices_.erase(notices_.begin() + long(i));
			continue;
		}
		const IfacePiece icon = env.hud ? env.hud->base_piece(n.image) : IfacePiece{};
		const int iw = icon.valid() ? icon.width : 0, ih = icon.valid() ? icon.height : 0;
		const float x0 = float(env.ui_w - iw) / 2, y0 = float(env.ui_h - ih) / 2;
		const float x1 = float(env.ui_w), y1 = float(env.ui_h);
		const float x = x0 + (x1 - x0) * float(fr), y = y0 + (y1 - y0) * float(fr);
		const float sc = 1.0f - 0.5f * float(fr);
		IfaceRect plate{int(x), int(x + iw * sc), int(y), int(y + ih * sc)};
		const ron::FontMetrics* m = ui.font_metrics(f);
		const int tw = f >= 0 ? ui.text_width(f, n.text) : 0;
		if (fr == 0 && m && !n.text.empty()) {
			const int cx = env.ui_w / 2;
			plate.l = std::min(plate.l, cx - tw / 2), plate.r = std::max(plate.r, cx + tw / 2);
			plate.b = int(y) + ih + 5 + m->font_height;
		}
		if (plate.w() > 0 && plate.h() > 0)
			scroll_plate(ui, {plate.l - kScrollPadX, plate.r + kScrollPadX, plate.t - kScrollPadY, plate.b + kScrollPadY}, kBlack);
		ui.next_layer();
		if (icon.valid()) ui.add(icon, x, y, float(iw) * sc, float(ih) * sc, kWhite, UiType::Alpha);
		if (fr == 0 && m && !n.text.empty()) ui.text(f, n.text, float(env.ui_w / 2 - tw / 2), y + float(ih) + 5, kWhite, kBlack);
		ui.next_layer();
		i++;
	}
}

// ScreenArrowSpline::draw @009108b0: the SPLINEARROW texture (arrowhead in the first tex_arrowhead texels) along the
// line (Spline::make_2d_strip) as a strip arrow_width = diag(screen) * 0.4 * 0.03 px wide, colour RED with alpha |8 - frame % 17| * 24 + 58;
// draw_arrows drops arrows whose end time (Game::tick, game seconds) has passed.
void View::draw_arrows(UiRenderer& ui, const ScriptPresentation& p, const Env& env) {
	if (p.arrows.empty() || arrow_tex_.empty()) return;
	std::string id;
	const ron::metal::UiTexture* t = ui.file_texture(arrow_tex_, &id);
	if (!t) return;
	const int frame = env.game ? env.game->frame : 0, tick = env.game ? env.game->tick : 0;
	const int alpha = std::abs(8 - frame % 17) * 24 + 58;
	const uint32_t col = (uint32_t(alpha) << 24) | 0xFF0000;
	const float width = float(int(std::sqrt(float(env.ui_w * env.ui_w + env.ui_h * env.ui_h)) * 0.4f * 0.03f));
	const float head_u = float(arrow_head_px_);
	for (const ron::sim::ScriptArrow& a : p.arrows) {
		if (a.seconds > 0 && tick > a.frame / 15 + a.seconds) continue;
		float x1 = float(a.x1), y1 = float(a.y1), x2 = float(a.x2), y2 = float(a.y2);
		if (a.world) {
			if (!env.project || !env.project(float(a.x1 * 192 + 96), float(a.y1 * 192 + 96), x1, y1) ||
			    !env.project(float(a.x2 * 192 + 96), float(a.y2 * 192 + 96), x2, y2))
				continue;
		}
		const float dx = x2 - x1, dy = y2 - y1, len = std::sqrt(dx * dx + dy * dy);
		if (len < 1) continue;
		const float ux = dx / len, uy = dy / len, nx = -uy * width / 2, ny = ux * width / 2;
		// ArrowSpline::map_texture: the arrowhead texels (u < tex_arrowhead) cover the last tenth of the line
		// (next_at_dist_reverse 0.1); the shaft texels are stretched over the rest, tail at u = 1.
		const float hl = len * 0.1f;
		const float mx = x2 - ux * hl, my = y2 - uy * hl;
		const float tw = float(t->w), th = float(t->h);
		// Shaft: u from the tail (tw) to the start of the head; head: u to 0 at the tip.
		const float shaft[8] = {x1 + nx, y1 + ny, mx + nx, my + ny, mx - nx, my - ny, x1 - nx, y1 - ny};
		const float suv[8] = {tw, 0, head_u, 0, head_u, th, tw, th};
		ui.add_quad(id, shaft, suv, col, UiType::ColorAlphaMod);
		const float tip[8] = {mx + nx, my + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, mx - nx, my - ny};
		const float tuv[8] = {head_u, 0, 0, 0, 0, th, head_u, th};
		ui.add_quad(id, tip, tuv, col, UiType::ColorAlphaMod);
	}
	ui.next_layer();
}

// BasePop (POPUP/POPUPSTYLE "default"): bg_color (39, 40, 43, 224), the popup_dialog font, 3-slice buttons. Layout
// approximated (BasePop::calculate_dimensions not ported): centred, text wrapped to 360 px, buttons in a row.
void View::draw_popup(UiRenderer& ui, const Env& env) {
	popup_buttons_.clear();
	const int f = ui.font_tag("popup_dialog");
	const ron::FontMetrics* m = ui.font_metrics(f);
	if (!m) return;
	const ron::XmlElement* style = nullptr;
	if (popup_el_) {
		for (const ron::XmlElement& c : popup_el_->children) {
			if (c.name == "POPUPSTYLE" && ron::to_lower(c.get("name")) == "default") style = &c;
		}
	}
	uint32_t bg = argb(224, 39, 40, 43);
	if (style) {
		for (const ron::XmlElement& c : style->children) {
			if (c.name == "IFACECOLOR" && c.get("name") == "bg_color")
				bg = argb(c.get_int("alpha"), c.get_int("red"), c.get_int("green"), c.get_int("blue"));
		}
	}
	const std::vector<std::string> lines = wrap(ui, f, popup_.text, 360);
	int tw = 0;
	for (const auto& l : lines) tw = std::max(tw, ui.text_width(f, l));
	const int bw = 173, bh = 32, gap = 12;
	const int nb = int(popup_.buttons.size());
	const int w = std::max(tw, nb * bw + (nb - 1) * gap) + 48;
	const int h = int(lines.size()) * m->line_height + bh + 24 * 3;
	const int x = (env.ui_w - w) / 2, y = (env.ui_h - h) / 2;
	ui.next_window();
	ui.fill(float(x), float(y), float(w), float(h), bg, UiType::ColorBlend);
	ui.frame(float(x), float(y), float(w), float(h), argb(255, 120, 120, 120));
	ui.next_layer();
	int ty = y + 24;
	for (const auto& l : lines) {
		ui.text(f, l, float(x + (w - ui.text_width(f, l)) / 2), float(ty), kWhite, kBlack);
		ty += m->line_height;
	}
	const int by = y + h - 24 - bh;
	int bx = x + (w - (nb * bw + (nb - 1) * gap)) / 2;
	for (int i = 0; i < nb; i++) {
		const IfaceRect r{bx, bx + bw, by, by + bh};
		popup_buttons_.push_back(r);
		const char* kind = popup_.pressed == i ? "butdown" : popup_.hover == i ? "buthighlight" : "butnormal";
		IfacePiece p[3];
		if (style) {
			for (int k = 0; k < 3; k++) p[k] = ron::iface_find_piece(*style, std::string(kind) + std::to_string(k));
		}
		if (p[0].valid() && p[1].valid() && p[2].valid()) ui.expander(p[0], p[1], p[2], r);
		else ui.fill(float(r.l), float(r.t), float(r.w()), float(r.h()), argb(255, 70, 70, 70));
		ui.next_layer();
		ui.text_centered(f, popup_.buttons[size_t(i)], r, kWhite, kBlack);
		bx += bw + gap;
	}
	ui.next_layer();
}

int View::popup_button_at(int x, int y) const {
	for (size_t i = 0; i < popup_buttons_.size(); i++) {
		if (popup_buttons_[i].contains(x, y)) return int(i);
	}
	return -1;
}

// ObjectivesDlg (OBJECTIVESDLG "replaywin", 480 x 330): the local leader's goals (add_goal), completed ones checked,
// and a close button. Approximated layout.
void View::draw_goal_dialog(UiRenderer& ui, const Env& env) {
	const int f = ui.font_tag("popup_dialog");
	const ron::FontMetrics* m = ui.font_metrics(f);
	if (!m || !env.game) return;
	const int w = 480, h = 330, x = (env.ui_w - w) / 2, y = (env.ui_h - h) / 2;
	ui.next_window();
	ui.fill(float(x), float(y), float(w), float(h), argb(224, 39, 40, 43), UiType::ColorBlend);
	ui.frame(float(x), float(y), float(w), float(h), argb(255, 120, 120, 120));
	ui.next_layer();
	ui.text_centered(f, "Objectives", {x, x + w, y + 12, y + 40}, kWhite, kBlack);
	int ty = y + 54;
	const IfacePiece on = ron::iface_find_piece(*msg_el_, "check_on"), off = ron::iface_find_piece(*msg_el_, "check_off");
	if (env.local >= 0 && env.local < 8) {
		for (const auto& g : env.game->scen.goals[size_t(env.local)]) {
			const IfacePiece& c = g.completed ? on : off;
			if (c.valid()) ui.add(c, float(x + 16), float(ty), 0, 0, kWhite, UiType::Alpha2);
			for (const std::string& l : wrap(ui, f, ron::sim::to_utf8(g.message), w - 32 - 30)) {
				ui.text(f, l, float(x + 16 + 30), float(ty + 4), kWhite, kBlack);
				ty += m->line_height;
			}
			ty += 10;
			if (ty > y + 54 + 215) break;
		}
	}
	goals_ok_ = {x + w / 2 - 60, x + w / 2 + 60, y + h - 44, y + h - 14};
	ui.fill(float(goals_ok_.l), float(goals_ok_.t), float(goals_ok_.w()), float(goals_ok_.h()), argb(255, 70, 70, 70));
	ui.next_layer();
	ui.text_centered(f, "OK", goals_ok_, kWhite, kBlack);
	ui.next_layer();
}

bool View::goal_dialog_close_at(int x, int y) const { return goals_ok_.contains(x, y); }

// The game-over line (MessageWin: game_over_message_size, centred under the resource panel) with the scenario's
// victory / defeat message (set_victory_message / set_defeat_message).
void View::draw_game_over(UiRenderer& ui, const Env& env) {
	if (!over_) return;
	const int f = ui.font_tag("game_over_message_size");
	const ron::FontMetrics* m = ui.font_metrics(f);
	if (!m) return;
	const int top = env.hud ? env.hud->resource_bottom() + 20 : 60;
	const std::string line = won_ ? "Victory!" : "Defeat!";
	ui.next_window();
	ui.text(f, line, float((env.ui_w - ui.text_width(f, line)) / 2), float(top), won_ ? rgb(255, 255, 0) : kWhite, kBlack);
	if (!over_text_.empty()) {
		int y = top + m->line_height + 4;
		for (const std::string& l : wrap(ui, f, over_text_, env.ui_w / 2)) {
			ui.text(f, l, float((env.ui_w - ui.text_width(f, l)) / 2), float(y), kWhite, kBlack);
			y += m->line_height;
		}
	}
	ui.next_layer();
}

void View::draw(UiRenderer& ui, const ScriptPresentation* p, const Env& env) {
	// Expire pings and announce circles.
	pings_.erase(std::remove_if(pings_.begin(), pings_.end(), [&](const hud::Ping& pg) { return env.tick >= pg.end_tick; }), pings_.end());
	announces_.erase(std::remove_if(announces_.begin(), announces_.end(), [&](const Announce& a) { return env.tick - a.tick >= 8; }),
	                 announces_.end());
	ui.next_window();
	// MessageWin::on_redraw: anchor x = 10 + 50 per column of rare goods (the HUD's draw_special_resources), y = resource panel height + 20.
	const int ax = 10 + (env.hud && env.hud_visible ? 50 * env.hud->rare_columns() : 0), ay = (env.hud && env.hud_visible) ? env.hud->resource_bottom() + 20 : 20;
	int y = ay;
	if (p) y = draw_objectives(ui, *p, env, ax, y);
	draw_messages(ui, env, ax, y);
	draw_top_centre(ui, p, env);
	draw_bubbles(ui, p, env);
	if (p) draw_arrows(ui, *p, env);
	draw_notices(ui, env);
	draw_game_over(ui, env);
	if (goals_open_) draw_goal_dialog(ui, env);
	if (popup_open_) draw_popup(ui, env);
}

}  // namespace scriptview
