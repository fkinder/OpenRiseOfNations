// A new random-map game: Setup::build_game for a random map (docs/randommap-spec.md section 1) on top of the generator
// (sim/mapgen.hpp), ending in the state shape Game::load leaves (docs/sim-implementation.md "Random games").
#include "sim/newgame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "assets/tga.hpp"
#include "assets/xml.hpp"
#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/heroes.hpp"
#include "sim/mountains.hpp"
#include "sim/production.hpp"
#include "sim/script_host.hpp"
#include "sim/search.hpp"
#include "sim/territory.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- setup: defaults, text form, option grammar

RandomGameSetup default_random_setup(int num_nations) {
	RandomGameSetup s;
	s.num_nations = std::max(2, std::min(8, num_nations));
	s.nation[0].human = true;
	return s;
}

// ---------------------------------------------------------------- the conquest map input in the setup text

namespace {

std::string pct_encode(const std::string& v) {
	std::string o;
	for (unsigned char c : v) {
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-') {
			o += char(c);
		} else {
			static const char* hex = "0123456789abcdef";
			o += '%';
			o += hex[c >> 4];
			o += hex[c & 15];
		}
	}
	return o;
}

bool pct_decode(const std::string& v, std::string& out) {
	out.clear();
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i] != '%') {
			out += v[i];
			continue;
		}
		if (i + 2 >= v.size()) return false;
		auto hv = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
		const int a = hv(v[i + 1]), b = hv(v[i + 2]);
		if (a < 0 || b < 0) return false;
		out += char(a * 16 + b);
		i += 2;
	}
	return true;
}

std::vector<std::string> split_text(const std::string& s, char sep) {
	std::vector<std::string> out;
	size_t p = 0;
	for (;;) {
		const size_t e = s.find(sep, p);
		if (e == std::string::npos) {
			out.push_back(s.substr(p));
			return out;
		}
		out.push_back(s.substr(p, e - p));
		p = e + 1;
	}
}

bool to_int(const std::string& s, long long* v) {
	if (s.empty()) return false;
	char* end = nullptr;
	*v = std::strtoll(s.c_str(), &end, 10);
	return end && *end == 0;
}

}  // namespace

std::string conquest_input_string(const mapgen::ConquestInput& in) {
	std::ostringstream o;
	o << "1." << (in.sea ? 1 : 0) << '.' << in.side << '.' << in.node_a << '.' << in.node_b << '.' << in.who_b << '.' << in.who_a << '.'
	  << (in.swap ? 1 : 0) << '.' << in.barbarians << '.' << (in.no_rare ? 1 : 0) << '.' << pct_encode(in.tileset) << '.';
	for (size_t i = 0; i < in.supporters.size(); i++)
		o << (i ? "_" : "") << (in.supporters[i].near_second ? 1 : 0) << ':' << in.supporters[i].armies;
	o << '.';
	for (size_t i = 0; i < in.ids.size() && i < in.xs.size() && i < in.ys.size(); i++) o << (i ? "_" : "") << in.ids[i] << ':' << in.xs[i] << ':' << in.ys[i];
	o << '.';
	// the land bitmap: alternating run lengths, water first
	bool cur = false;
	size_t run = 0;
	bool first = true;
	for (uint8_t v : in.land) {
		const bool b = v != 0;
		if (b == cur) {
			run++;
			continue;
		}
		o << (first ? "" : ",") << run;
		first = false;
		cur = b;
		run = 1;
	}
	o << (first ? "" : ",") << run;
	return o.str();
}

bool parse_conquest_input(const std::string& text, mapgen::ConquestInput& out) {
	const std::vector<std::string> f = split_text(text, '.');
	if (f.size() != 14 || f[0] != "1") return false;
	long long v[10];
	const int idx[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
	for (int i = 0; i < 9; i++) {
		if (!to_int(f[size_t(idx[i])], &v[i])) return false;
	}
	mapgen::ConquestInput c;
	c.sea = v[0] != 0;
	c.side = int(v[1]);
	c.node_a = int(v[2]);
	c.node_b = int(v[3]);
	c.who_b = int(v[4]);
	c.who_a = int(v[5]);
	c.swap = v[6] != 0;
	c.barbarians = int(v[7]);
	c.no_rare = v[8] != 0;
	if (c.side <= 0 || c.side > 8192) return false;
	if (!pct_decode(f[10], c.tileset)) return false;
	if (!f[11].empty()) {
		for (const std::string& e : split_text(f[11], '_')) {
			const std::vector<std::string> p = split_text(e, ':');
			long long a, b;
			if (p.size() != 2 || !to_int(p[0], &a) || !to_int(p[1], &b)) return false;
			mapgen::ConquestSupporter sp;
			sp.near_second = a != 0;
			sp.armies = int(b);
			c.supporters.push_back(sp);
		}
	}
	if (!f[12].empty()) {
		for (const std::string& e : split_text(f[12], '_')) {
			const std::vector<std::string> p = split_text(e, ':');
			long long a, b, d;
			if (p.size() != 3 || !to_int(p[0], &a) || !to_int(p[1], &b) || !to_int(p[2], &d)) return false;
			c.ids.push_back(int(a));
			c.xs.push_back(int(b));
			c.ys.push_back(int(d));
		}
	}
	const size_t cells = size_t(c.side) * size_t(c.side);
	c.land.assign(cells, 0);
	size_t pos = 0;
	bool cur = false;
	for (const std::string& e : split_text(f[13], ',')) {
		long long n;
		if (!to_int(e, &n) || n < 0 || pos + size_t(n) > cells) return false;
		if (cur) std::fill(c.land.begin() + std::ptrdiff_t(pos), c.land.begin() + std::ptrdiff_t(pos + size_t(n)), uint8_t(1));
		pos += size_t(n);
		cur = !cur;
	}
	if (pos != cells) return false;
	c.valid = true;
	out = std::move(c);
	return true;
}

namespace {
std::string g_battle_setup_text;
}
void set_battle_setup_text(const std::string& text) { g_battle_setup_text = text; }
std::string battle_setup_text() { return g_battle_setup_text; }

std::string ctw_map_battle_string(const CtwMapBattle& b) {
	std::string o = "ctwmap:" + pct_encode(b.map);
	if (!b.runtime.empty()) o += ";btrt=" + pct_encode(b.runtime);
	if (!b.setup.empty()) o += ";btsu=" + pct_encode(b.setup);
	if (!b.players.empty()) {
		o += ";pl=";
		for (size_t i = 0; i < b.players.size(); i++) {
			const CtwMapBattle::Player& p = b.players[i];
			o += (i ? "_" : "") + std::to_string(p.who) + "." + std::to_string(p.tribe) + "." + std::to_string(p.is_human) + "." +
			     std::to_string(p.difficulty) + "." + pct_encode(p.name);
		}
	}
	return o;
}

bool parse_ctw_map_battle(const std::string& text, CtwMapBattle& out) {
	if (!is_ctw_map_scenario(text)) return false;
	CtwMapBattle b;
	bool have_map = false;
	std::stringstream ss(text.substr(7));
	std::string item;
	bool first = true;
	while (std::getline(ss, item, ';')) {
		if (first) {
			first = false;
			if (!pct_decode(item, b.map) || b.map.empty()) return false;
			have_map = true;
			continue;
		}
		const size_t eq = item.find('=');
		if (eq == std::string::npos) return false;
		const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
		if (k == "btrt") { if (!pct_decode(v, b.runtime)) return false; }
		else if (k == "btsu") { if (!pct_decode(v, b.setup)) return false; }
		else if (k == "pl") {
			std::stringstream ps(v);
			std::string e;
			while (std::getline(ps, e, '_')) {
				std::vector<std::string> f;
				std::stringstream fs(e);
				std::string x;
				while (std::getline(fs, x, '.')) f.push_back(x);
				if (e.size() && e.back() == '.') f.push_back(std::string());
				CtwMapBattle::Player p;
				long long a, t, h, d;
				if (f.size() != 5 || !to_int(f[0], &a) || !to_int(f[1], &t) || !to_int(f[2], &h) || !to_int(f[3], &d) || !pct_decode(f[4], p.name)) return false;
				p.who = int(a);
				p.tribe = int(t);
				p.is_human = int(h);
				p.difficulty = int(d);
				b.players.push_back(std::move(p));
			}
		}
		else return false;
	}
	if (!have_map) return false;
	out = std::move(b);
	return true;
}

std::string random_setup_string(const RandomGameSetup& s) {
	std::ostringstream o;
	o << "random:style=" << s.map_style << ";size=" << s.map_size << ";seed=" << s.seed << ";n=" << s.num_nations
	  << ";town=" << s.starting_town << ";res=" << s.starting_resources << ";tech=" << s.starting_technology
	  << ";end=" << s.ending_technology << ";vic=" << s.victory << ";elim=" << s.elimination << ";score=" << s.score_goal
	  << ";time=" << s.time_limit << ";wonder=" << s.wonderwin << ";popwin=" << s.popwin << ";econ=" << s.econwin
	  << ";poplimit=" << s.pop_limit << ";techcost=" << s.tech_cost << ";rush=" << s.rush_rules << ";reveal=" << s.reveal_map
	  << ";rules=" << s.game_rules << ";speed=" << s.game_speed << ";randstarts=" << (s.randomize_starts ? 1 : 0)
	  << ";nopowers=" << (s.no_tribe_powers ? 1 : 0);
	if (s.ctw) o << ";ctw=1";
	if (!s.conquest.empty()) o << ";conq=" << s.conquest;
	if (!s.battle_runtime.empty()) o << ";btrt=" << pct_encode(s.battle_runtime);
	if (!s.battle_setup.empty()) o << ";btsu=" << pct_encode(s.battle_setup);
	for (int i = 0; i < 8; i++) {
		const NationSetup& n = s.nation[size_t(i)];
		o << ";p" << i << "=" << n.tribe << "," << n.team << "," << (n.human ? 1 : 0) << "," << n.difficulty << "," << n.color;
	}
	return o.str();
}

bool parse_random_setup(const std::string& text, RandomGameSetup& out) {
	if (!is_random_scenario(text)) return false;
	RandomGameSetup s;
	std::stringstream ss(text.substr(7));
	std::string item;
	bool any = false;
	while (std::getline(ss, item, ';')) {
		const size_t eq = item.find('=');
		if (eq == std::string::npos) return false;
		const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
		const long long n = std::strtoll(v.c_str(), nullptr, 10);
		any = true;
		if (k == "style") s.map_style = int(n);
		else if (k == "size") s.map_size = int(n);
		else if (k == "seed") s.seed = uint32_t(std::strtoull(v.c_str(), nullptr, 10));
		else if (k == "n") s.num_nations = int(n);
		else if (k == "town") s.starting_town = int(n);
		else if (k == "res") s.starting_resources = int(n);
		else if (k == "tech") s.starting_technology = int(n);
		else if (k == "end") s.ending_technology = int(n);
		else if (k == "vic") s.victory = int(n);
		else if (k == "elim") s.elimination = int(n);
		else if (k == "score") s.score_goal = int(n);
		else if (k == "time") s.time_limit = int(n);
		else if (k == "wonder") s.wonderwin = int(n);
		else if (k == "popwin") s.popwin = int(n);
		else if (k == "econ") s.econwin = int(n);
		else if (k == "poplimit") s.pop_limit = int(n);
		else if (k == "techcost") s.tech_cost = int(n);
		else if (k == "rush") s.rush_rules = int(n);
		else if (k == "reveal") s.reveal_map = int(n);
		else if (k == "rules") s.game_rules = int(n);
		else if (k == "speed") s.game_speed = int(n);
		else if (k == "randstarts") s.randomize_starts = n != 0;
		else if (k == "nopowers") s.no_tribe_powers = n != 0;
		else if (k == "ctw") s.ctw = n != 0;
		else if (k == "conq") s.conquest = v;
		else if (k == "btrt") { if (!pct_decode(v, s.battle_runtime)) return false; }
		else if (k == "btsu") { if (!pct_decode(v, s.battle_setup)) return false; }
		else if (k.size() == 2 && k[0] == 'p' && k[1] >= '0' && k[1] <= '7') {
			NationSetup& ns = s.nation[size_t(k[1] - '0')];
			int f[5] = {-1, -1, 0, 2, -1};
			std::stringstream ps(v);
			std::string tok;
			for (int i = 0; i < 5 && std::getline(ps, tok, ','); i++) f[i] = std::atoi(tok.c_str());
			ns.tribe = f[0], ns.team = f[1], ns.human = f[2] != 0, ns.difficulty = f[3], ns.color = f[4];
		} else {
			return false;
		}
	}
	if (!any) return false;
	out = s;
	return true;
}

std::string parse_random_option(const Rules& rules, const std::string& spec, RandomGameSetup& out) {
	std::vector<std::string> tok;
	std::stringstream ss(spec);
	std::string t;
	while (std::getline(ss, t, ',')) tok.push_back(t);
	if (tok.size() < 4) return "--random STYLE,SIZE,SEED,NATIONS[,TRIBES...] needs at least four values";
	RandomGameSetup s = default_random_setup(2);
	s.map_style = mapgen::style_from_name(rules, tok[0]);
	if (s.map_style < 0 || s.map_style > mapgen::kStyleBlank) return "unknown map style '" + tok[0] + "'";
	s.map_size = std::atoi(tok[1].c_str());
	if (s.map_size < 0 || s.map_size > 6) return "map size must be 0..6";
	s.seed = uint32_t(std::strtoull(tok[2].c_str(), nullptr, 0));
	s.num_nations = std::atoi(tok[3].c_str());
	if (s.num_nations < 1 || s.num_nations > 8) return "nations must be 1..8";
	for (size_t i = 4; i < tok.size() && i - 4 < 8; i++) {
		const std::string& v = tok[i];
		s.nation[i - 4].tribe = (v == "r" || v == "R") ? -1 : std::atoi(v.c_str());
		if (s.nation[i - 4].tribe >= int(rules.tribes.size())) return "tribe index out of range: " + v;
	}
	out = s;
	return std::string();
}

// ---------------------------------------------------------------- terrain synthesized from the generated world

namespace {

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
	uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu ^ (c + 0x165667b1u) * 0xc2b2ae35u;
	h ^= h >> 15;
	h *= 0x2c1b3c6du;
	h ^= h >> 12;
	h *= 0x297a2d39u;
	h ^= h >> 15;
	return h;
}

// Smooth value noise in [-1, 1] on a lattice of `cell` tcoords.
float value_noise(uint32_t seed, int x, int y, int cell, uint32_t salt) {
	const int ix = (x >= 0 ? x : x - cell + 1) / cell, iy = (y >= 0 ? y : y - cell + 1) / cell;
	float fx = float(x - ix * cell) / float(cell), fy = float(y - iy * cell) / float(cell);
	fx = fx * fx * (3 - 2 * fx);
	fy = fy * fy * (3 - 2 * fy);
	auto v = [&](int cx, int cy) { return float(hash3(seed ^ salt, uint32_t(cx), uint32_t(cy)) & 0xffff) / 32767.5f - 1.0f; };
	const float a = v(ix, iy), b = v(ix + 1, iy), c = v(ix, iy + 1), d = v(ix + 1, iy + 1);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

struct Template {
	float height = 0;
	Image tex;
	bool ok = false;
};

// The terrain the renderer draws: Terrain::init's job for a random map. The original builds heights in TerrainOut (never
// decoded); this is an approximation in the ranges the shipped scenarios use (docs/terrain-spec.md "Units and grids"):
// land 0..400 with a low-frequency swell, shelf water about -214, deep ocean -273, shoreline in between, rivers carved,
// mountains raised by their template (adjust_for_mountains(0): template z added to the heights).
Terrain synthesize_terrain(const Game& g, const GameFiles& files, const mapgen::Result& res) {
	const World& w = g.world;
	Terrain t;
	t.tileset = res.tileset;
	t.xs = w.xs;
	t.ys = w.ys;
	const int xs = w.xs, ys = w.ys, txs = xs * 4, tys = ys * 4, vxs = t.vxs(), vys = t.vys();
	t.wcoords.resize(size_t(xs) * size_t(ys));
	t.coord_flags.resize(size_t(xs) * size_t(ys));
	for (int y = 0; y < ys; y++) {
		for (int x = 0; x < xs; x++) {
			const WData& d = w.w(x, y);
			WCoordCell c;
			uint16_t f = 0;
			if (d.flags & wflag::kForest) f |= wfile::kForest;
			if (d.flags & wflag::kRock) f |= wfile::kRocks;
			if (d.flags & wflag::kCoast) f |= wfile::kCoast;
			if (d.flags & wflag::kRiver) f |= wfile::kRiver;
			if (d.flags & wflag::kOil) f |= 0x10;
			if (d.flags & wflag::kCliff) f |= 0x40;
			if (d.flags & wflag::kHalfLand) f |= wfile::kHalfWater;
			c.flags = f;
			c.land = (d.land == 1 || d.land == 2) ? uint8_t(d.land) : uint8_t(0);
			c.land_sub = d.land_sub;
			t.wcoords[size_t(y) * size_t(xs) + size_t(x)] = c;
			uint16_t cf = 0;
			const bool half = (d.flags & wflag::kHalfLand) != 0;
			if (half) cf = 0xa08d;
			else if (d.land == 2) cf = 0x3009;
			else if (d.land == 1) cf = 0x3089;
			else if (d.flags & wflag::kRiver) cf = 0x11;
			else if (d.flags & wflag::kCoast) cf = 0x4;
			t.coord_flags[size_t(y) * size_t(xs) + size_t(x)] = cf;
		}
	}
	t.tcoord_mask.assign(size_t(txs) * size_t(tys), 0);
	for (int ty = 0; ty < tys; ty++) {
		for (int tx = 0; tx < txs; tx++) {
			const uint16_t m = w.t(tx, ty);
			uint16_t f = 0;
			if ((m & tmask::kMedium) == tmask::kWater) f |= tfile::kWater;
			if (m & tmask::kCoastal) f |= tfile::kCoastal;
			if (m & tmask::kRiverB) f |= tfile::kRiver;
			t.tcoord_mask[size_t(ty) * size_t(txs) + size_t(tx)] = f;
		}
	}
	// Heights. The generator's coastlines follow wcoord edges (no shelf or shoreline cells), so the land height field and a
	// blurred water indicator are blended: the shoreline is the 0.5 contour of the blur (rounded corners), the water depth
	// goes from the shelf (-214) to the deep ocean (-273) with the distance from the coast.
	const uint32_t seed = res.world_seed;
	// The generator's own height grid (Terrain::init: land fractals, coast pass, shelf, river troughs, adjust_for_mountains(0), oil plateaus,
	// cliffs) is the original's TerrainOut; the approximation below stays only for a Result without one.
	const bool gen_h = res.height_vxs == vxs && res.height_vys == vys && res.heights.size() == size_t(vxs) * size_t(vys);
	if (gen_h) {
		t.heights = res.heights;
	} else {
		std::vector<float> ltarget(size_t(txs) * size_t(tys), -1e9f);  // land tcoords only
		std::vector<float> ind(size_t(vxs) * size_t(vys), 0.0f);        // water indicator at the vertices
		for (int ty = 0; ty < tys; ty++) {
			for (int tx = 0; tx < txs; tx++) {
				const uint16_t m = w.t(tx, ty);
				if ((m & tmask::kMedium) == tmask::kWater) continue;
				float h = 250.0f + 70.0f * value_noise(seed, tx, ty, 6, 1) + 55.0f * value_noise(seed, tx, ty, 17, 2);
				if (m & tmask::kRiverB) h = std::min(h, 130.0f);
				if ((m & tmask::kLowField) == tmask::kMountain) h = std::max(h, 300.0f);
				ltarget[size_t(ty) * size_t(txs) + size_t(tx)] = h;
			}
		}
		t.heights.assign(size_t(vxs) * size_t(vys), 0.0f);
		std::vector<float> hl(size_t(vxs) * size_t(vys), 250.0f);
		for (int vy = 0; vy < vys; vy++) {
			for (int vx = 0; vx < vxs; vx++) {
				float wsum = 0, lsum = 0;
				int nw = 0, n = 0;
				for (int dy = -1; dy <= 0; dy++) {
					for (int dx = -1; dx <= 0; dx++) {
						const int tx = vx + dx, ty = vy + dy;
						if (tx < 0 || ty < 0 || tx >= txs || ty >= tys) continue;
						n++;
						const float lt = ltarget[size_t(ty) * size_t(txs) + size_t(tx)];
						if (lt < -1e8f) nw++;
						else lsum += lt, wsum += 1.0f;
					}
				}
				ind[size_t(vy) * size_t(vxs) + size_t(vx)] = n ? float(nw) / float(n) : 0.0f;
				if (wsum > 0) hl[size_t(vy) * size_t(vxs) + size_t(vx)] = lsum / wsum;
			}
		}
		// Three separable box blurs (radius 3) of the indicator: about a Gaussian of sigma 3.5 tcoords.
		{
			std::vector<float> blur(ind.size());
			auto box = [&](std::vector<float>& v, bool horizontal) {
				const int R = 3;
				for (int vy = 0; vy < vys; vy++) {
					for (int vx = 0; vx < vxs; vx++) {
						float sum = 0;
						int n = 0;
						for (int k = -R; k <= R; k++) {
							const int x = horizontal ? vx + k : vx, y = horizontal ? vy : vy + k;
							if (x < 0 || y < 0 || x >= vxs || y >= vys) continue;
							sum += v[size_t(y) * size_t(vxs) + size_t(x)];
							n++;
						}
						blur[size_t(vy) * size_t(vxs) + size_t(vx)] = sum / float(n);
					}
				}
				v = blur;
			};
			for (int pass = 0; pass < 3; pass++) {
				box(ind, true);
				box(ind, false);
			}
		}
		auto smooth = [](float a, float b, float x) {
			const float u = std::max(0.0f, std::min(1.0f, (x - a) / (b - a)));
			return u * u * (3 - 2 * u);
		};
		for (size_t i = 0; i < t.heights.size(); i++) {
			const float s = smooth(0.2f, 0.8f, ind[i]);
			const float hw = -214.0f - 59.0f * smooth(0.75f, 1.0f, ind[i]);
			t.heights[i] = hl[i] * (1.0f - s) + hw * s;
		}
	}
	// Mountains: the stamps, and the templates' z added to the vertices they cover.
	std::map<int, Template> templates;
	std::vector<Template> list;
	if (!gen_h) {
		try {
			ron::for_each_element(ron::parse_xml(files.read_text("Data/effects_graphics.xml")), "MOUNTAINS", [&](const XmlElement& sec) {
				if (!list.empty()) return;
				for (const XmlElement& m : sec.children) {
					if (m.name != "MOUNTAIN") continue;
					Template tp;
					tp.height = m.get_float("height", 0);
					if (const XmlElement* c = m.child("TEMPLATE_TEX")) {
						const std::string path = files.resolve(normalize_game_path(c->get("file")));
						if (!path.empty()) {
							tp.tex = ron::parse_tga(ron::read_file(path));
							tp.ok = tp.tex.width > 0 && tp.tex.height > 0;
						}
					}
					list.push_back(std::move(tp));
				}
			});
		} catch (const std::exception&) {
		}
	}
	MountainTemplates sim_templates(files);
	for (const PlacedMountain& pm : res.mountains) {
		const MountainRange* mr = sim_templates.get(pm.range);
		MountainStamp st;
		st.wx = uint8_t(pm.wx);
		st.wy = uint8_t(pm.wy);
		st.range_index = pm.range;
		st.num_coords = uint8_t(mr ? mr->face_wcoords : int32_t(pm.wcoords.size()));
		t.mountains.push_back(st);
		if (gen_h || pm.range < 0 || size_t(pm.range) >= list.size() || !list[size_t(pm.range)].ok) continue;
		const Template& tp = list[size_t(pm.range)];
		const int W = tp.tex.width, H = tp.tex.height;
		const int ox = (W >> 1) % 16, oy = (H >> 1) % 16;
		for (int y = oy; y < H; y += 4) {
			for (int x = ox; x < W; x += 4) {
				const uint8_t* px = &tp.tex.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4];
				if (!px[3]) continue;
				const int vx = pm.wx * 4 + int(std::lround(float(x - (W >> 1)) * 0.25f));
				const int vy = pm.wy * 4 + int(std::lround(float(y - (H >> 1)) * 0.25f));
				if (vx < 0 || vy < 0 || vx >= vxs || vy >= vys) continue;
				float& h = t.heights[size_t(vy) * size_t(vxs) + size_t(vx)];
				h = std::max(h, 280.0f) + float(px[2]) * tp.height * 3.0f / 255.0f;
			}
		}
	}
	// Rivers: the generator's polylines; their banks are already low.
	for (const mapgen::RiverPath& r : res.rivers) {
		if (r.pts.size() < 2) continue;
		t.rivers.push_back(r.pts);
	}
	// Land doobers (bushes and rocks): the generator only counts them; the placement here is cosmetic.
	{
		int want = std::min(res.doobers, 1200);
		for (int i = 0, tries = 0; i < want && tries < want * 8; tries++) {
			const uint32_t h = hash3(seed, uint32_t(tries), 0xd00be5u);
			const int wx = int(h % uint32_t(xs)), wy = int((h >> 8) % uint32_t(ys));
			const WData& d = w.w(wx, wy);
			if (World::is_ocean(d) || (d.flags & (wflag::kForest | wflag::kMountain | wflag::kRiver | wflag::kCoast | wflag::kHalfLand |
			                                       wflag::kBuilding | wflag::kRock | wflag::kOil)))
				continue;
			Doober db;
			db.x = wx * 768 + int((h >> 16) % 768u);
			db.y = wy * 768 + int((hash3(h, 1, 2) >> 4) % 768u);
			db.kind = (h >> 28) < 4 ? 1 : 0;
			t.doobers.push_back(db);
			i++;
		}
	}
	return t;
}

// ---------------------------------------------------------------- the Game

// Leader::init's diplomacy part (game.cpp keeps its copy local): tribe < 0 = Leaders::init (allied with itself, war with
// everyone); a seat with a tribe is allied with its team and otherwise at war when teams are locked, else at peace.
void init_diplomacy(Game& g, int32_t who, int32_t tribe) {
	Leader& l = g.leaders[size_t(who)];
	const bool locked = g.info.team_style != 0 && g.info.team_style != 8 && g.info.team_style != 11;
	for (int32_t i = 0; i < 8; i++) {
		l.treaty(i) = g.info.reveal_map == 3 ? 1 : 0;
		if (tribe < 0) {
			l.diplo[size_t(i)] = i == who ? 2 : 0;
			continue;
		}
		const bool team = is_team(g, who, i);
		l.diplo[size_t(i)] = team ? 2 : locked ? 0 : 1;
		if (team) l.treaty(i) |= 1;
	}
	if (tribe < 0) {
		l.ally_mask = uint8_t(1u << (uint32_t(who) & 31));
		return;
	}
	leader_init_ally_mask(g, who);
}

int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// The setup with its random picks made (tribes, the random start age) and every value in range. The picks use their own
// generator seeded from the setup seed, so they never touch game_random.
RandomGameSetup resolve_setup(const RandomGameSetup& in, const Rules& rules) {
	RandomGameSetup s = in;
	s.num_nations = clampi(s.num_nations, 1, 8);
	s.map_size = clampi(s.map_size, 0, 6);
	s.map_style = clampi(s.map_style, 0, mapgen::kStyleBlank);
	s.starting_town = clampi(s.starting_town, 0, 3);
	s.starting_resources = clampi(s.starting_resources, 0, 12);
	s.starting_technology = clampi(s.starting_technology, 0, 9);
	s.ending_technology = clampi(s.ending_technology, 0, 7);
	s.pop_limit = clampi(s.pop_limit, 0, 5);
	s.reveal_map = clampi(s.reveal_map, 0, 3);
	Random rr(s.seed * 2654435761u ^ 0x5eed1234u);
	const int ntribes = std::max(1, int(rules.tribes.size()));
	std::set<int> used;
	for (int i = 0; i < s.num_nations; i++) {
		if (s.nation[size_t(i)].tribe >= 0 && s.nation[size_t(i)].tribe < ntribes) used.insert(s.nation[size_t(i)].tribe);
	}
	for (int i = 0; i < s.num_nations; i++) {
		NationSetup& n = s.nation[size_t(i)];
		if (n.tribe < 0 || n.tribe >= ntribes) {
			int t = rr.get(0, ntribes);
			for (int guard = 0; used.count(t) && guard < 200; guard++) t = rr.get(0, ntribes);
			n.tribe = t;
			used.insert(t);
		}
		n.team = n.team >= 0 && n.team < 4 ? n.team : -1;
		n.difficulty = clampi(n.difficulty, 0, 5);
		if (n.color < 0 || n.color > 7) n.color = i;
	}
	if (s.starting_technology == 9) s.starting_technology = rr.get(0, 8);
	return s;
}

// Setup::build_empire's products, created in the list's order (city, its buildings, the units).
void create_empires(Game& g, const mapgen::Result& res) {
	std::array<int32_t, 8> city_o;
	city_o.fill(-1);
	std::array<int32_t, 8> farm_o;
	farm_o.fill(-1);
	for (const mapgen::EmpireEntry& e : res.empire) {
		const int32_t who = e.who;
		if (who < 0 || who >= 8) continue;
		if (e.kind == mapgen::EmpireEntry::City) {
			// Setup::build_cities: Build::init, activate(0, 0, 0), cities_built++, update_seen(ally), territory.
			const int32_t o = init_build_site(g, who, e.type, e.x, e.y, false, -1);
			if (o < 0) continue;
			Build& b = *g.build(who, o);
			build_activate_full(g, b, false, false, false);
			g.leaders[size_t(who)].cities_built++;
			update_seen_ally(g, b);
			city_o[size_t(who)] = o;
			if (b.city >= 0) compute_all_territory(g);
		} else if (e.kind == mapgen::EmpireEntry::Building) {
			if (city_o[size_t(who)] < 0) continue;
			// Leader::produce_building(t, city.o, 0): the AI's site search + init_build.
			produce_building(g, who, e.type, city_o[size_t(who)], 0);
		} else {
			if (farm_o[size_t(who)] < 0) {
				const ObjectList& ol = g.objects[size_t(who)];
				for (int32_t o = kBuildBase; o < ol.build_mark; o++) {
					const Build* b = g.build(who, o);
					if (b && b->alive() && b->type == ptype::kFarm) {
						farm_o[size_t(who)] = o;
						break;
					}
				}
			}
			if (e.note == 1) {
				// A starting citizen of a town start: created at the first farm, comes out, and gathers there.
				const Build* farm = farm_o[size_t(who)] >= 0 ? g.build(who, farm_o[size_t(who)]) : nullptr;
				if (!farm) {
					const Build* c = city_o[size_t(who)] >= 0 ? g.build(who, city_o[size_t(who)]) : nullptr;
					if (!c) continue;
					const int32_t o = init_unit(g, who, e.type, c->x, c->y);
					if (o >= 0) unit_come_out(g, *g.unit(who, o), 0);
					continue;
				}
				const int32_t o = init_unit(g, who, e.type, farm->x, farm->y);
				if (o < 0) continue;
				Unit& u = *g.unit(who, o);
				unit_come_out(g, u, 0);
				if (g.info.starting_resources != 8) add_gather_order(g, u, farm_o[size_t(who)], kQueueNew, false);
			} else if (e.x < 0) {
				// Setup::place_unit found no spot with a city: Build::train(city, type).
				const int32_t co = city_o[size_t(who)];
				if (co >= 0) build_train(g, *g.build(who, co), e.type);
			} else {
				const int32_t o = init_unit(g, who, e.type, e.x, e.y);
				if (o < 0) continue;
				Unit& u = *g.unit(who, o);
				if (e.note == 4) {
					unit_come_out(g, u, 0);
				} else if (e.note == 2) {
					// Greek scholars go inside their university.
					const ObjectList& ol = g.objects[size_t(who)];
					for (int32_t bo = kBuildBase; bo < ol.build_mark; bo++) {
						const Build* b = g.build(who, bo);
						if (b && b->alive() && b->type == ptype::kUniversity) {
							unit_go_inside(g, u, bo, who);
							break;
						}
					}
				}
			}
		}
	}
}

// Herd::create_units for every herd (the animals' positions are the herd cell centres; spread with find_nearby_spot).
void create_herds(Game& g, const mapgen::Result& res) {
	for (const mapgen::HerdAnimal& a : res.animals) {
		if (a.unit_type < 0 || a.unit_type >= kNumTypes || !g.type(a.unit_type).loaded) continue;
		int32_t x = a.x, y = a.y, ox = 0, oy = 0;
		const Type& T = g.type(a.unit_type);
		if (find_nearby_spot(g, T, a.x, a.y, &ox, &oy, 0, 3 * 768, 0, 0x80000000u, 0, -1, -1, 0, 0, -1, 0, -1) == 0) x = ox, y = oy;
		const int32_t o = init_unit(g, kGaiaWho, a.unit_type, x, y);
		if (o < 0) continue;
		Unit& u = *g.unit(kGaiaWho, o);
		u.angle = a.angle;
		u.dest_angle = a.angle;
		unit_set_angle(g, u, u.angle, true);
	}
}

std::string type_key(const Type& t) { return to_utf8(t.type_name.empty() ? t.name : t.type_name); }

// The finished game as scenario records (what resolve_battlefield and the renderer's farm / goods code read).
void snapshot_objects(const Game& g, const RandomGameSetup& S, Scenario& sc) {
	const Rules& rules = *g.rules;
	for (int i = 0; i < S.num_nations; i++) {
		ScenarioPlayer p;
		p.who = i;
		p.tribe = S.nation[size_t(i)].tribe;
		p.color = S.nation[size_t(i)].color;
		p.is_human = S.nation[size_t(i)].human ? 4 : 0;
		p.name = g.leaders[size_t(i)].name;
		p.difficulty = S.nation[size_t(i)].difficulty;
		for (int32_t t = 544; t < 551 && t - 544 < g.leaders[size_t(i)].age; t++) p.techs.push_back(type_key(rules.types[size_t(t)]));
		sc.players.push_back(p);
	}
	int seq = 0;
	for (int32_t who = 0; who < kNumLeaders; who++) {
		const ObjectList& ol = g.objects[size_t(who)];
		for (int32_t o = kBuildBase; o < ol.build_mark; o++) {
			const Build* b = g.build(who, o);
			if (!b || !b->alive()) continue;
			ScenarioBuilding sb;
			sb.who = who;
			sb.o = o;
			sb.type = type_key(*b->ptype);
			sb.cx = b->x;
			sb.cy = b->y;
			sb.active = (b->flags & objflag::kActive) != 0;
			sb.city = (b->flags & objflag::kCity) != 0;
			sb.seq = seq++;
			sc.buildings.push_back(sb);
		}
		for (int32_t o = 0; o < ol.unit_mark; o++) {
			const Unit* u = g.unit(who, o);
			if (!u || !u->alive()) continue;
			int32_t iw = -1;
			if (unit_get_inside(g, *u, &iw) >= 0) continue;
			ScenarioUnit su;
			su.who = who;
			su.o = o;
			su.angle = u->angle;
			su.type = type_key(*u->ptype);
			su.cx = u->x;
			su.cy = u->y;
			su.seq = seq++;
			sc.units.push_back(su);
		}
	}
	for (const GoodObj& go : g.goods) {
		if (!go.alive() || go.type < 0) continue;
		ScenarioGood sg;
		sg.type = to_utf8(rules.types[size_t(go.type)].name);
		sg.cx = go.x;
		sg.cy = go.y;
		sc.goods.push_back(sg);
	}
	for (const GoodObj& it : g.items) {
		if (!it.alive()) continue;
		ScenarioGood sg;
		sg.type = "Ruins";
		sg.cx = it.x;
		sg.cy = it.y;
		sc.items.push_back(sg);
	}
}

}  // namespace

std::unique_ptr<Game> new_random_game(const GameFiles& files, const Rules& rules, const AnimSource* anims,
                                      const RandomGameSetup& setup, RandomExtras* extras) {
	RandomExtras local;
	RandomExtras& X = extras ? *extras : local;
	X = RandomExtras();
	const RandomGameSetup S = resolve_setup(setup, rules);
	X.setup = S;
	auto gp = std::make_unique<Game>();
	Game& g = *gp;
	g.anims = anims;

	// ---- GameInfo (the lobby), the way Game::init_rules_and_teams reads it.
	GameInfo& gi = g.info;
	const int n = S.num_nations;
	bool any_team = false;
	int first_ai_diff = -1;
	for (int i = 0; i < n; i++) {
		const NationSetup& ns = S.nation[size_t(i)];
		gi.team[size_t(i)] = int8_t(ns.team);
		if (ns.team >= 0) any_team = true;
		if (!ns.human && first_ai_diff < 0) first_ai_diff = ns.difficulty;
	}
	gi.team_style = uint8_t(any_team ? 12 : 1);  // Custom Teams / Survival of the Fittest
	gi.reveal_map = uint8_t(S.reveal_map);
	gi.rush_rules = uint8_t(S.rush_rules);
	gi.starting_town = uint8_t(S.starting_town);
	gi.starting_resources = uint8_t(S.starting_resources);
	gi.game_rules = uint8_t(S.game_rules);
	gi.flags = S.no_tribe_powers ? 4u : 0u;
	gi.difficulty = uint8_t(first_ai_diff >= 0 ? first_ai_diff : 2);
	gi.starting_technology = S.starting_technology;
	gi.ending_technology = S.ending_technology;
	gi.tech_cost = uint8_t(S.tech_cost);
	gi.victory = uint8_t(S.victory);
	gi.elimination = uint8_t(S.elimination);
	gi.score_goal = uint8_t(S.score_goal);
	gi.time_limit = uint8_t(S.time_limit);
	gi.wonderwin = uint8_t(S.wonderwin);
	gi.popwin = uint8_t(S.popwin);
	gi.econwin = uint8_t(S.econwin);
	gi.pop_limit = uint8_t(S.pop_limit);
	gi.num_players = n;
	gi.num_nations = n;
	{
		std::set<int> teams;
		int loose = 0;
		for (int i = 0; i < n; i++) {
			if (S.nation[size_t(i)].team >= 0) teams.insert(S.nation[size_t(i)].team);
			else loose++;
		}
		gi.num_sides = int32_t(teams.size()) + loose;
	}
	g.own_rules.reset();
	g.rules = &rules;
	g.base_rules = &rules;
	g.scen = ScenarioData();
	g.buildings_gather = g.building_unit_bonus = g.building_resource_bonus = true;
	g.scripted_rules = gi.game_rules == 10;
	g.ctw = S.ctw;  // Conquer the World battle (sim/ctw.hpp): scripts and the CtW branches of the simulation

	// ---- Game::run: game_random := GameInfo.seed, Game::init_starting_resources (S1 / S2); Setup::build_game then reseeds
	// (inside the generator) and draws world.seed first.
	g.rng.reseed(S.seed);
	init_starting_resources(g);

	// ---- Setup::build_game: the map (Map::make), start order, herds
	mapgen::Params p;
	p.style = S.map_style;
	p.size = S.map_size;
	p.seed = S.seed;
	p.nations = n;
	p.randomize_starts = S.randomize_starts;
	p.team_style = gi.team_style;
	p.starting_town = S.starting_town;
	p.starting_resources = S.starting_resources;
	p.reveal_map = S.reveal_map;
	for (int i = 0; i < 8; i++) {
		p.team[size_t(i)] = i < n && S.nation[size_t(i)].team >= 0 ? S.nation[size_t(i)].team : 8;
		p.tribe[size_t(i)] = i < n ? S.nation[size_t(i)].tribe : i;
	}
	// A save of a Conquer the World battle (or the battle builder, which keeps its own scope open) supplies the conquest map input.
	mapgen::ConquestInput conquest_in;
	std::unique_ptr<mapgen::ConquestScope> conquest_scope;
	if (!S.conquest.empty() && !mapgen::conquest_input()) {
		if (!parse_conquest_input(S.conquest, conquest_in)) {
			X.error = "malformed conquest map input in the setup";
			return nullptr;
		}
		conquest_scope = std::make_unique<mapgen::ConquestScope>(&conquest_in);
	}
	X.result = mapgen::generate_into(g, files, p);
	const mapgen::Result& res = X.result;
	if (!res.ok) {
		X.error = res.error;
		return nullptr;
	}
	g.frame = 0;
	g.tick = 0;

	// ---- the terrain the renderer draws and the simulation's heights (Terrain::init)
	X.scenario.name = "Random Map: " + mapgen::style_name(rules, res.picked_style) + ", " + std::to_string(res.xs) + "x" +
	                  std::to_string(res.ys) + ", seed " + std::to_string(S.seed);
	X.name = X.scenario.name;
	X.scenario.world_seed = res.world_seed;
	X.scenario.terrain = synthesize_terrain(g, files, res);
	g.heights.vxs = X.scenario.terrain.vxs();
	g.heights.vys = X.scenario.terrain.vys();
	g.heights.h = X.scenario.terrain.heights;

	// Mountain ranges as MountainsData keeps them (gather tiles of mines).
	g.mountains.clear();
	for (const PlacedMountain& m : res.mountains) {
		PlacedRange pr;
		pr.wx = m.wx;
		pr.wy = m.wy;
		pr.range = m.range;
		pr.solid = m.wcoords;
		pr.tiles = m.tcoords;
		pr.tcoord_count = int32_t(m.tcoords.size());
		g.mountains.push_back(std::move(pr));
	}

	// ---- Leaders::init, then the seats in start order (Setup::build_game step 7: Leader::init(who, tribe, who))
	for (int32_t who = 0; who < kNumLeaders; who++) {
		g.leaders[size_t(who)] = Leader();
		init_diplomacy(g, who, -1);
		leader_init_economy(g, who, false);
		leader_calc_attrition(g, who);
		leader_calc_anti_attrition(g, who);
	}
	heroes_init(g);
	g.everyone_mask = 0;
	bool have_local = false;
	for (int i = 0; i < n; i++) {
		const NationSetup& ns = S.nation[size_t(i)];
		Leader& l = g.leaders[size_t(i)];
		l.tribe = ns.tribe;
		l.flags |= leaderflag::kValid | leaderflag::kActive;
		if (ns.human) l.flags |= leaderflag::kHuman;
		g.everyone_mask |= 1u << uint32_t(i);
	}
	for (int k = 0; k < 8; k++) {
		const int32_t who = res.start_order[size_t(k)];
		if (who < 0 || who >= n) continue;
		const NationSetup& ns = S.nation[size_t(who)];
		Leader& l = g.leaders[size_t(who)];
		l.tribe = ns.tribe;
		l.color = ns.color;
		init_diplomacy(g, who, ns.tribe);
		leader_init_techs(g, who);
		leader_init_economy(g, who, true);
		leader_calc_attrition(g, who);
		leader_calc_anti_attrition(g, who);
		if (ns.human) l.flags |= leaderflag::kHuman;
		else l.flags &= ~leaderflag::kHuman;
		leader_init_ai(g, who);
		l.difficulty = ns.difficulty;
		l.name = ns.human ? std::string("Player") : to_utf8(rules.tribes[size_t(ns.tribe)].name);
		if (ns.human) l.flags |= leaderflag::kHuman;
		else l.flags &= ~leaderflag::kHuman;
		l.flags |= leaderflag::kValid | leaderflag::kActive;
		if (ns.human && !have_local) {
			g.local_play = who;
			have_local = true;
		}
	}
	g.leaders[kGaiaWho].flags = leaderflag::kValid;
	g.leaders[kGaiaWho].color = kGaiaWho;
	armies_init(g);
	init_combat_data(g);
	compute_all_territory(g);

	// ---- the objects: empires (step 9), then every herd's animals (step 10)
	g.scenario_mode = g.scripted_rules || g.ctw;
	create_empires(g, res);
	create_herds(g, res);
	// A capital: the first city of a seat (Build::activate names it in the original's lobby flow).
	for (int i = 0; i < n; i++) {
		for (City& c : g.cities[size_t(i)]) {
			if (!c.valid()) continue;
			if (!(c.city_flags & cityflag::kCapital)) c.city_flags |= cityflag::kCapital | cityflag::kOriginalCapital;
			break;
		}
	}
	// World::analyze_map, World::compute_all_territory, then Game::run's first-frame preparations.
	g.world_total_land = 0;
	for (int32_t r = 1; r < 64; r++) g.world_total_land += g.regions.r[size_t(r)].size;
	compute_all_territory(g);
	calc_danger(g);
	update_all_seen(g);

	// ---- the scenario-side records (the renderer's view of the finished game) and the scripts (AI production scripts)
	for (int i = 0; i < 8; i++) {
		X.scenario.camera_x[size_t(i)] = -1;
		X.scenario.camera_y[size_t(i)] = -1;
		X.scenario.camera_zoom[size_t(i)] = -1;
	}
	{
		const int i = g.local_play;  // the camera starts on the console seat
		for (const mapgen::EmpireEntry& e : res.empire) {
			if (e.who != i) continue;
			if (e.kind == mapgen::EmpireEntry::City) {
				X.scenario.camera_x[size_t(i)] = e.x;
				X.scenario.camera_y[size_t(i)] = e.y;
			} else if (e.start >= 0 && size_t(e.start) < res.starts.size()) {
				X.scenario.camera_x[size_t(i)] = res.starts[size_t(e.start)][0] * 768 + 384;
				X.scenario.camera_y[size_t(i)] = res.starts[size_t(e.start)][1] * 768 + 384;
			}
			break;
		}
	}
	X.scenario.reveal_map = S.reveal_map;
	X.scenario.has_settings = true;
	snapshot_objects(g, S, X.scenario);
	X.scenario.terrain.heights = g.heights.h;  // the start towns' Build::init terraform is in the grid the renderer draws
	Scenario no_scripts;
	script_host_load(g, files, std::string(), no_scripts);
	if (g.scripts && (!S.battle_runtime.empty() || !S.battle_setup.empty())) {
		// ConquestGame::build_battle's style scripts: the runtime script every frame, the setup script (not run here: its effects are in a save)
		ScriptHost& h = *g.scripts;
		auto base_name = [](std::string p) {
			for (char& c : p) {
				if (c == '\\') c = '/';
			}
			const size_t slash = p.find_last_of('/');
			if (slash != std::string::npos) p = p.substr(slash + 1);
			const size_t dot = p.find_last_of('.');
			return dot == std::string::npos ? p : p.substr(0, dot);
		};
		std::string err;
		if (!S.battle_runtime.empty() && h.compile(S.battle_runtime, &err)) h.scenario_script = bhs::to_u16(base_name(S.battle_runtime));
		if (!S.battle_setup.empty()) h.compile(S.battle_setup, &err);
	}
	return gp;
}

}  // namespace ron::sim
