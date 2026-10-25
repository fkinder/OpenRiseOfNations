// Script run time of the simulation (docs/script-spec.md 6.2-6.4).
#include "sim/script_host.hpp"

#include <filesystem>
#include <fstream>

#include "script/bhs_builtins.hpp"
#include "script/bhs_compiler.hpp"
#include "sim/ctw.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/script_funcs_internal.hpp"

namespace ron::sim {

using bhs::U16;
using bhs::VP;

// ---------------------------------------------------------------- timers

int ScriptTimers::add(const U16& name, int32_t expiry) {
	// ScriptTimers::add_timer @a049e0: refused when 100 timers exist (even if `name` is one of them).
	if (list.size() >= 100) return -1;
	for (size_t k = 0; k < list.size(); k++) {
		if (bhs::ieq(list[k].name, name)) {
			list.erase(list.begin() + long(k));
			break;
		}
	}
	size_t at = 0;
	while (at < list.size() && list[at].expiry <= expiry) at++;  // ordered_insert by metric
	list.insert(list.begin() + long(at), Timer{name, expiry});
	return 1;
}

int ScriptTimers::check(const U16& name, int32_t now) {
	for (size_t k = 0; k < list.size(); k++) {
		if (!bhs::ieq(list[k].name, name)) continue;
		if (list[k].expiry <= now) {
			list.erase(list.begin() + long(k));
			return 1;
		}
		return 0;
	}
	return -1;
}

int ScriptTimers::remove(const U16& name) {
	for (size_t k = 0; k < list.size(); k++) {
		if (bhs::ieq(list[k].name, name)) {
			list.erase(list.begin() + long(k));
			return 1;
		}
	}
	return -1;
}

// ---------------------------------------------------------------- host

ScriptHost::ScriptHost(Game& g) : game(g) {
	find_counters.fill(-1);
	rt.log = [this](const std::string& m) {
		if (messages.size() < 200) messages.push_back("f" + std::to_string(game.frame) + ": " + m);
	};
	install_script_funcs(*this);
}

ScriptHost::~ScriptHost() = default;

bool ScriptHost::compile(const std::string& path, std::string* err) {
	const U16 key = bhs::to_u16(path);
	for (const auto& f : rt.files) {
		if (f && bhs::ieq(bhs::to_u16(f->source_file), key)) return true;  // loaded and unchanged
	}
	bhs::CompileOptions o;
	o.builtins = &rt.registry;
	o.resolve = [this](const std::string& p) -> std::string {
		if (!p.empty() && p[0] == '/' && std::filesystem::exists(p)) return p;
		return files ? files->resolve(p) : std::string();
	};
	bhs::CompileResult r = bhs::compile(path, o);
	if (!r.ok) {
		if (err) *err = r.errors.empty() ? path + ": compile failed" : r.errors.front();
		return false;
	}
	for (const bhs::CompiledFile& f : r.files) {
		std::string e;
		if (rt.load_image(f.path, f.image, &e) < 0) {
			if (err) *err = f.path + ": " + e;
			return false;
		}
	}
	return true;
}

int ScriptHost::run(const U16& name, const std::vector<VP>& args) {
	runs++;
	const int st = rt.run_script(name, args);
	if (st != bhs::Runtime::kOk) {
		run_errors++;
		run_failures[bhs::to_utf8(name) + ": status " + std::to_string(st)]++;
	}
	return st;
}

void ScriptHost::run_frame_scripts() {
	// Game::do_frame @591ef0 step 5 (GameInfo.scenario_script) and step 6 (general powers, frame > 0).
	if (!scenario_script.empty()) run(scenario_script, {});
	if (game.frame > 0 && !general_powers_script.empty()) run(general_powers_script, {});
	if (ctw && game.ctw) ctw->place_reinforcements(game);  // step 6: ConquestGame::place_reinforcements
}

std::vector<std::pair<std::string, int>> ScriptHost::unported() const {
	std::vector<std::pair<std::string, int>> out;
	for (size_t k = 0; k < rt.unported_calls.size(); k++) {
		if (rt.unported_calls[k] > 0) out.emplace_back(rt.registry.funcs()[k].name8, rt.unported_calls[k]);
	}
	return out;
}

// ---------------------------------------------------------------- presentation

namespace {
// The length of a RIFF WAVE file in ms (data chunk size / fmt byte rate), -1 when it is not a readable WAV.
int32_t wav_length_ms(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return -1;
	char hdr[12];
	if (!f.read(hdr, 12) || std::string(hdr, 4) != "RIFF" || std::string(hdr + 8, 4) != "WAVE") return -1;
	uint32_t byte_rate = 0;
	for (int guard = 0; guard < 64; guard++) {
		char ch[8];
		if (!f.read(ch, 8)) return -1;
		const uint32_t n = uint8_t(ch[4]) | uint32_t(uint8_t(ch[5])) << 8 | uint32_t(uint8_t(ch[6])) << 16 |
		                   uint32_t(uint8_t(ch[7])) << 24;
		const std::string id(ch, 4);
		if (id == "fmt ") {
			char fmt[16] = {};
			if (!f.read(fmt, n < 16 ? std::streamsize(n) : 16)) return -1;
			byte_rate = uint8_t(fmt[8]) | uint32_t(uint8_t(fmt[9])) << 8 | uint32_t(uint8_t(fmt[10])) << 16 |
			            uint32_t(uint8_t(fmt[11])) << 24;
			if (n > 16) f.seekg(std::streamoff(n - 16 + (n & 1)), std::ios::cur);
			else if (n & 1) f.seekg(1, std::ios::cur);
		} else if (id == "data") {
			if (!byte_rate) return -1;
			return int32_t(uint64_t(n) * 1000 / byte_rate);
		} else {
			f.seekg(std::streamoff(n + (n & 1)), std::ios::cur);
		}
	}
	return -1;
}
}  // namespace

bool ScriptHost::sound_done() const {
	if (!sound.active) return true;
	if (input.attached && input.sound_state >= 0) return input.sound_state != 0;
	if (sound.ms < 0) return true;
	return int64_t(game.frame - sound.start_frame) * 1000 >= int64_t(sound.ms) * 15;
}

int32_t ScriptHost::play_sound(const U16& file) {
	// ScenarioData::sound is stopped and closed, then reopened on GameInfo.scenario_dir + file.
	sound.active = false;
	const std::string name = bhs::to_utf8(file);
	const std::string rel = scenario_dir + name;
	const std::string path = files ? files->resolve(rel) : std::string();
	ScriptEvent& e = sf::push_event(game, ScriptEvent::kSound);
	e.text = name;
	e.text2 = rel;
	if (path.empty()) {
		e.a = -1;
		return -1;
	}
	sound.name = name;
	sound.path = rel;
	sound.start_frame = game.frame;
	sound.ms = wav_length_ms(path);
	sound.active = true;
	e.a = sound.ms;
	return 1;
}

const ScriptPresentation* script_presentation(const Game& g) { return g.scripts ? &g.scripts->pres : nullptr; }
ScriptInput* script_input(Game& g) { return g.scripts ? &g.scripts->input : nullptr; }

namespace sf {
ScriptPresentation& pres(Game& g) { return g.scripts->pres; }
ScriptInput& input(Game& g) { return g.scripts->input; }
ScriptEvent& push_event(Game& g, ScriptEvent::Kind k) {
	ScriptPresentation& p = g.scripts->pres;
	while (p.events.size() >= ScriptPresentation::kMaxEvents) p.events.pop_front();
	ScriptEvent e;
	e.seq = p.next_seq++;
	e.frame = g.frame;
	e.kind = k;
	p.events.push_back(std::move(e));
	return p.events.back();
}
}  // namespace sf

// ---------------------------------------------------------------- loading

namespace {
std::string game_relative(const GameFiles& files, const std::string& p) {
	namespace fs = std::filesystem;
	std::string root = fs::path(files.root()).lexically_normal().string();
	std::string a = fs::path(p).lexically_normal().string();
	if (!root.empty() && root.back() != '/') root += '/';
	if (a.rfind(root, 0) == 0) a = a.substr(root.size());
	for (char& c : a) {
		if (c == '/') c = '\\';
	}
	return a;
}

bool is_human_seat(const Leader& l) { return (l.flags & 0xc) == leaderflag::kHuman; }
}  // namespace

ScriptLoadReport script_host_load(Game& g, const GameFiles& files, const std::string& scenario_path, const Scenario& s) {
	ScriptLoadReport rep;
	g.scripts = std::make_unique<ScriptHost>(g);
	ScriptHost& h = *g.scripts;
	h.files = &files;
	h.tileset = s.terrain.tileset;
	{  // GameInfo.scenario_dir
		const std::string rel = game_relative(files, scenario_path);
		const size_t slash = rel.find_last_of('\\');
		h.scenario_dir = slash == std::string::npos ? std::string() : rel.substr(0, slash + 1);
	}
	// Setup::build_game @5ac190: the scenario script.
	if (s.scenario_flags & 1) {
		std::string rel = game_relative(files, scenario_path);
		const size_t dot = rel.find_last_of('.');
		const size_t slash = rel.find_last_of('\\');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) rel = rel.substr(0, dot);
		const std::string base = slash == std::string::npos ? rel : rel.substr(slash + 1);
		const std::string bhs = rel + ".bhs";
		std::string err;
		if (!files.resolve(bhs).empty() || std::filesystem::exists(bhs)) {
			if (h.compile(bhs, &err)) {
				h.scenario_script = bhs::to_u16(base);
				rep.scenario_script = base;
			} else {
				rep.errors.push_back("Script failed to compile: " + err);  // the name is cleared
			}
		} else {
			rep.errors.push_back("scenario script " + bhs + " not found");
		}
	}
	// General powers (game_rules 10 / scripted rules, or Conquer the World).
	if (g.scripted_rules || g.ctw) {
		const std::string gp = "scenario\\scriptlibrary\\general_powers.bhs";
		std::string err;
		if (h.compile(gp, &err)) {
			h.general_powers_script = u"general_powers";
			rep.general_powers_script = "general_powers";
		} else {
			rep.errors.push_back(err);
		}
	}
	// Leaders::init_production_script @6ed490 (Game::run; not when a save is loaded).
	for (int32_t who = 0; who < 8; who++) {
		const Leader& l = g.leaders[size_t(who)];
		if (!(l.flags & leaderflag::kValid) || is_human_seat(l) || l.prod_script.empty()) continue;
		const std::string p = "ai\\scripts\\" + l.prod_script + ".bhs";  // leaders.prod_script_path + name + ".bhs"
		std::string err;
		if (h.compile(p, &err)) {
			bool seen = false;
			for (const std::string& q : rep.production_scripts) seen |= q == p;
			if (!seen) rep.production_scripts.push_back(p);
		} else {
			rep.errors.push_back(err);
		}
	}
	return rep;
}

// Leader::production_ai and Leader::init's AI part are in sim/ai.hpp (ai_production.cpp, ai_setup.cpp).

}  // namespace ron::sim
