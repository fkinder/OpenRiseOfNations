// ron-mapgen: the random-map generator on its own (engine/sim/mapgen*.cpp, docs/randommap-*.md).
//
// Usage: ron-mapgen <game dir> --style N|NAME --size N --seed S --nations K [--teams a,b,..] [--randomize]
//                   [--town N] [--tribes a,b,..] [--png out.png] [--scale N] [--dump out.txt] [--quiet]
//        ron-mapgen <game dir> --compare <file.SVX> [--style N] [--game] [--side out.png] [--quiet]   (--game: compare the world of new_random_game, start cities included)
//        ron-mapgen <game dir> --all [--seed S] [--jobs N] [--styles a-b] [--sizes a-b] [--nations a-b] [--quiet] [--no-conquest]
//        ron-mapgen <game dir> --ctw CAMPAIGN:LINK [--style 24|25] [--difficulty D] [--seed S] [--png out.png] [--dump out.txt] [--quiet]
//        ron-mapgen <game dir> --png-check [DIR]
//
// --ctw generates the battle map of a Conquer the World link (styles 24 / 25, MapConquest; LINK = auto, auto:N or FROM>TO as for ronmap's
// --ctw-battle); --style 25 forces the sea variant on a land link. --all also runs the conquest styles over a few links of every campaign
// (skipped by --no-conquest). --png-check decodes every file under DIR (default conquest/RenderMaps/MapPieces of the game dir) with the PNG
// reader and compares the country silhouettes with the campaigns' COUNTRY rectangles.
//
// --style takes a MapStyleIndex (0..5 pickers, 6..22 the shipped styles, 23 blank map) or a name ("Old World", "OLDWORLD").
// --png writes a top-down preview (water / land by subtype / forest / mountain / rock / river / resources / goodies /
// starts); --dump a text digest (grid hashes per layer, regions, resources, starts, empire placements, draw counts).
// --compare also takes --hfull FILE (every height, save and generated, binary) and --wfull FILE (the differing wcoords, text) for offline analysis; its
// height section ends with the sum of |generated - save| over the aquatic vertices. --ctw takes --image FILE (the square the countries' silhouettes drew).
// --compare loads an original save (re/oracle/Saves/*.SVX), regenerates its map from the save's setup (seed, size, lobby style,
// nations, tribes, teams) and prints, per layer, the number of differing cells and the first one (docs/randommap-spec.md).
// --all runs every shipped style x every map size x nations 2..8, each generation twice (determinism) and timed.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

#include <fstream>
#include <iterator>
#include <set>

#include <filesystem>

#include "assets/png.hpp"
#include "sim/ctw.hpp"
#include "sim/datawalk.hpp"
#include "sim/game.hpp"
#include "sim/mapgen.hpp"
#include "sim/placement.hpp"
#include "sim/trig.hpp"
#include "sim/commands.hpp"
#include "sim/newgame.hpp"
#include "sim/original_save.hpp"
#include "sim/replay.hpp"

using namespace ron;
using namespace ron::sim;
using namespace ron::sim::mapgen;

namespace {

const char* arg_value(int argc, char** argv, const char* name) {
	for (int i = 2; i + 1 < argc; i++) {
		if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
	}
	return nullptr;
}
bool has_flag(int argc, char** argv, const char* name) {
	for (int i = 2; i < argc; i++) {
		if (std::strcmp(argv[i], name) == 0) return true;
	}
	return false;
}

std::vector<int> parse_list(const char* s) {
	std::vector<int> v;
	if (!s) return v;
	std::stringstream ss(s);
	std::string tok;
	while (std::getline(ss, tok, ',')) v.push_back(std::atoi(tok.c_str()));
	return v;
}

bool parse_range(const char* s, int* lo, int* hi) {
	if (!s) return false;
	const char* dash = std::strchr(s, '-');
	*lo = std::atoi(s);
	*hi = dash ? std::atoi(dash + 1) : *lo;
	return true;
}

// ---------------------------------------------------------------- one generation

struct Generated {
	Result res;
	World world;
	Regions regions;
	Random rng{0};
	std::vector<GoodObj> goods, items;
	double seconds = 0;
};

void run_one(const GameFiles& files, const Rules& rules, const Params& p, Generated& g, const ConquestInput* conquest = nullptr) {
	const auto t0 = std::chrono::steady_clock::now();
	ConquestScope scope(conquest);
	Context ctx{files, rules, g.world, g.regions, g.rng, g.goods, g.items};
	g.res = generate(ctx, p);
	g.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

uint64_t fnv(uint64_t h, uint64_t v) {
	for (int i = 0; i < 8; i++) {
		h ^= (v >> (i * 8)) & 0xff;
		h *= 0x100000001b3ull;
	}
	return h;
}

struct Hashes {
	uint64_t land = 0xcbf29ce484222325ull, flags = 0xcbf29ce484222325ull, region = 0xcbf29ce484222325ull,
	         tdata = 0xcbf29ce484222325ull, extra = 0xcbf29ce484222325ull, sub = 0xcbf29ce484222325ull;
};

Hashes hash_world(const World& w) {
	Hashes h;
	for (const WData& d : w.wdata) {
		h.land = fnv(h.land, uint64_t(uint8_t(d.land)));
		h.sub = fnv(h.sub, d.land_sub);
		h.flags = fnv(h.flags, d.flags);
		h.region = fnv(h.region, uint64_t(uint16_t(d.region)) | (uint64_t(uint16_t(d.region2)) << 16));
		h.extra = fnv(h.extra, uint64_t(uint16_t(d.down)) | (uint64_t(d.val) << 16) | (uint64_t(d.goods) << 24) |
		                           (uint64_t(d.blocked) << 32) | (uint64_t(d.bad) << 40) | (uint64_t(uint8_t(d.solid)) << 48));
	}
	for (uint16_t t : w.tdata) h.tdata = fnv(h.tdata, t);
	return h;
}

std::string dump_text(const GameFiles&, const Rules& rules, const Params& p, const Generated& g) {
	const Result& r = g.res;
	char buf[512];
	std::string out;
#define line(...)                              \
	do {                                        \
		std::snprintf(buf, sizeof buf, __VA_ARGS__); \
		out += buf;                             \
		out += '\n';                            \
	} while (0)
	line("ron-mapgen dump");
	line("params style=%d (%s) size=%d seed=%u nations=%d randomize=%d town=%d", p.style, style_name(rules, p.style).c_str(),
	     p.size, p.seed, p.nations, p.randomize_starts ? 1 : 0, p.starting_town);
	std::string teams;
	for (int i = 0; i < p.nations && i < 8; i++) teams += (i ? "," : "") + std::to_string(p.team[size_t(i)]);
	line("teams %s", teams.c_str());
	if (!r.ok) {
		line("ERROR %s", r.error.c_str());
		return out;
	}
	line("picked_style=%d (%s) map=%dx%d world_seed=%u tileset=%s", r.picked_style, style_name(rules, r.picked_style).c_str(), r.xs,
	     r.ys, r.world_seed, r.tileset.c_str());
	line("fallback=%d retries=%d river_beds=%d doobers=%d", r.fallback ? 1 : 0, r.retries, r.river_beds, r.doobers);
	line("territory_limits %d %d %d %d %d %d", r.territory_limits[0], r.territory_limits[1], r.territory_limits[2],
	     r.territory_limits[3], r.territory_limits[4], r.territory_limits[5]);
	line("game_random draws=%llu", (unsigned long long)r.draws);
	for (const auto& ph : r.phases) line("  phase %-20s %llu", ph.first.c_str(), (unsigned long long)ph.second);
	const Hashes h = hash_world(g.world);
	line("hash land=%016llx land_sub=%016llx flags=%016llx region=%016llx tdata=%016llx extra=%016llx", (unsigned long long)h.land,
	     (unsigned long long)h.sub, (unsigned long long)h.flags, (unsigned long long)h.region, (unsigned long long)h.tdata,
	     (unsigned long long)h.extra);
	int land_cells = 0, forest = 0, mountain = 0, rock = 0, river = 0, oil = 0, coast = 0;
	for (const WData& d : g.world.wdata) {
		if (!World::is_ocean(d)) land_cells++;
		if (d.flags & 0x20) forest++;
		if (d.flags & 0x10) mountain++;
		if (d.flags & 0x08) rock++;
		if (d.flags & 0x1000) river++;
		if (d.flags & 0x800) oil++;
		if (d.flags & 0x04) coast++;
	}
	line("cells land=%d forest=%d mountain=%d rock=%d river=%d oil=%d coast=%d sea_map=%d", land_cells, forest, mountain, rock, river,
	     oil, coast, g.world.sea_map);
	int nl = 0, ns = 0;
	std::string rs;
	for (int i = 1; i < 64; i++) {
		if (g.regions.r[size_t(i)].size) {
			nl++;
			rs += " L" + std::to_string(i) + "=" + std::to_string(g.regions.r[size_t(i)].size);
		}
	}
	for (int i = 64; i < 127; i++) {
		if (g.regions.r[size_t(i)].size) {
			ns++;
			rs += " S" + std::to_string(i) + "=" + std::to_string(g.regions.r[size_t(i)].size);
		}
	}
	line("regions land=%d sea=%d%s", nl, ns, rs.c_str());
	line("start_order %d %d %d %d %d %d %d %d", r.start_order[0], r.start_order[1], r.start_order[2], r.start_order[3],
	     r.start_order[4], r.start_order[5], r.start_order[6], r.start_order[7]);
	for (size_t i = 0; i < r.starts.size(); i++) line("start %zu %d %d", i, r.starts[i][0], r.starts[i][1]);
	for (size_t i = 0; i < r.conquest_starts.size(); i++)
		line("conquest start %zu who=%d node=%d armies=%d at %d,%d", i, r.conquest_starts[i].who, r.conquest_starts[i].node,
		     r.conquest_starts[i].armies, r.conquest_starts[i].x, r.conquest_starts[i].y);
	line("mountains %zu oil_locations %zu rivers %zu", r.mountains.size(), r.oil_locations.size(), r.rivers.size());
	for (const auto& rv : r.rivers) {
		if (rv.pts.empty()) continue;
		line("river len=%zu from %d,%d to %d,%d", rv.pts.size(), rv.pts.front()[0], rv.pts.front()[1], rv.pts.back()[0],
		     rv.pts.back()[1]);
	}
	line("resources %zu", g.goods.size());
	for (size_t i = 0; i < g.goods.size(); i++) {
		const GoodObj& go = g.goods[i];
		line("  good %zu %s %d %d", i, to_utf8(rules.types[size_t(go.type)].name).c_str(), wcoord_of(go.x), wcoord_of(go.y));
	}
	line("goodies %zu", g.items.size());
	for (size_t i = 0; i < g.items.size(); i++) line("  goody %zu %d %d", i, wcoord_of(g.items[i].x), wcoord_of(g.items[i].y));
	line("herds %zu animals %zu", r.herds.size(), r.animals.size());
	for (const HerdRec& hr : r.herds) line("  herd %d unit=%d cell=%d,%d good_slot=%d", hr.id, hr.unit_type, hr.wx, hr.wy, hr.good_slot);
	line("empire %zu", r.empire.size());
	for (const EmpireEntry& e : r.empire) {
		const char* kind = e.kind == EmpireEntry::City ? "city" : e.kind == EmpireEntry::Building ? "building" : "unit";
		line("  %s who=%d start=%d type=%d (%s) pos=%d,%d note=%d", kind, e.who, e.start, e.type,
		     to_utf8(rules.types[size_t(e.type)].name).c_str(), e.x, e.y, e.note);
	}
	for (const std::string& n : r.notes) line("note: %s", n.c_str());
#undef line
	return out;
}

// ---------------------------------------------------------------- PNG

struct Png {
	int w = 0, h = 0;
	std::vector<uint8_t> rgb;
	Png(int ww, int hh) : w(ww), h(hh), rgb(size_t(ww) * size_t(hh) * 3, 0) {}
	void put(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
		if (x < 0 || y < 0 || x >= w || y >= h) return;
		uint8_t* p = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
		p[0] = r;
		p[1] = g;
		p[2] = b;
	}
	void rect(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b) {
		for (int y = y0; y < y1; y++) {
			for (int x = x0; x < x1; x++) put(x, y, r, g, b);
		}
	}
	bool save(const std::string& path) const {
		std::vector<uint8_t> raw;
		raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
		for (int y = 0; y < h; y++) {
			raw.push_back(0);
			raw.insert(raw.end(), rgb.begin() + long(size_t(y) * size_t(w) * 3), rgb.begin() + long(size_t(y + 1) * size_t(w) * 3));
		}
		uLongf zlen = compressBound(uLong(raw.size()));
		std::vector<uint8_t> z(zlen);
		if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
		z.resize(zlen);
		FILE* f = std::fopen(path.c_str(), "wb");
		if (!f) return false;
		static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
		std::fwrite(sig, 1, 8, f);
		auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
			uint8_t len[4] = {uint8_t(data.size() >> 24), uint8_t(data.size() >> 16), uint8_t(data.size() >> 8), uint8_t(data.size())};
			std::fwrite(len, 1, 4, f);
			std::vector<uint8_t> body(type, type + 4);
			body.insert(body.end(), data.begin(), data.end());
			std::fwrite(body.data(), 1, body.size(), f);
			const uint32_t crc = uint32_t(crc32(0, body.data(), uInt(body.size())));
			uint8_t c[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
			std::fwrite(c, 1, 4, f);
		};
		std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
		                             uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
		chunk("IHDR", ihdr);
		chunk("IDAT", z);
		chunk("IEND", {});
		std::fclose(f);
		return true;
	}
};

Png render_png(const Generated& g, int scale) {
	const World& w = g.world;
	Png png(w.xs * scale, w.ys * scale);
	static const uint8_t grass[4][3] = {{84, 150, 62}, {118, 156, 62}, {160, 156, 76}, {180, 150, 100}};
	for (int y = 0; y < w.ys; y++) {
		for (int x = 0; x < w.xs; x++) {
			const WData& d = w.w(x, y);
			uint8_t r = 0, gg = 0, b = 0;
			if (World::is_ocean(d)) {
				if (d.land == 1) {
					r = 52, gg = 104, b = 170;
				} else {
					r = 24, gg = 58, b = 120;
				}
			} else {
				const uint8_t* c = grass[d.land_sub & 3];
				r = c[0], gg = c[1], b = c[2];
				if (d.flags & 4) r = 226, gg = 212, b = 150;
			}
			if (d.flags & 0x20) r = 22, gg = 82, b = 34;
			if (d.flags & 0x08) r = 150, gg = 150, b = 150;
			if (d.flags & 0x10) r = 112, gg = 98, b = 96;
			if (d.flags & 0x1000) r = 60, gg = 196, b = 232;
			png.rect(x * scale, y * scale, (x + 1) * scale, (y + 1) * scale, r, gg, b);
		}
	}
	// mountain tcoords darken the cell they sit on a little
	for (const PlacedMountain& m : g.res.mountains) {
		for (const auto& o : m.wcoords) {
			const int x = m.wx + o[0], y = m.wy + o[1];
			if (x >= 0 && y >= 0 && x < w.xs && y < w.ys) png.rect(x * scale + scale / 4, y * scale + scale / 4, (x + 1) * scale - scale / 4, (y + 1) * scale - scale / 4, 84, 72, 72);
		}
	}
	const int dot = std::max(2, scale * 3 / 4);
	auto mark = [&](int wx, int wy, uint8_t r, uint8_t gg, uint8_t b, int size) {
		const int cx = wx * scale + scale / 2, cy = wy * scale + scale / 2;
		png.rect(cx - size / 2, cy - size / 2, cx - size / 2 + size, cy - size / 2 + size, r, gg, b);
	};
	for (const WCoord& o : std::vector<WCoord>()) (void)o;
	for (int y = 0; y < w.ys; y++) {
		for (int x = 0; x < w.xs; x++) {
			if (w.w(x, y).flags & 0x800) mark(x, y, 0, 0, 0, dot);
		}
	}
	for (const GoodObj& go : g.goods) {
		const int t = go.type;
		if (t == 6 || t == 31) mark(wcoord_of(go.x), wcoord_of(go.y), 235, 245, 255, dot);
		else mark(wcoord_of(go.x), wcoord_of(go.y), 255, 214, 0, dot + 1);
	}
	for (const GoodObj& it : g.items) mark(wcoord_of(it.x), wcoord_of(it.y), 232, 60, 205, dot);
	for (size_t i = 0; i < g.res.starts.size(); i++) {
		const int sx = g.res.starts[i][0], sy = g.res.starts[i][1];
		mark(sx, sy, 255, 255, 255, scale + 4);
		mark(sx, sy, 230, 30, 30, scale + 1);
	}
	return png;
}

bool write_png(const std::string& path, const Rules&, const Generated& g, int scale) { return render_png(g, scale).save(path); }

// ---------------------------------------------------------------- Conquer the World battle maps (styles 24 / 25)

}  // namespace
namespace ron::sim::ctw {
// sim/ctw_battle.cpp: the conquest map input of a battle (declared here, it has no header).
bool conquest_battle_input(const ConquestGame& g, const GameFiles& F, const ConquestGame::BattleDef& B, mapgen::ConquestInput* out, std::string* err);
}  // namespace ron::sim::ctw
namespace {

std::string resolve_campaign_stem(const GameFiles& files, const std::string& arg) {
	auto norm = [](std::string s) {
		std::string o;
		for (char ch : s) {
			if (ch != '_' && ch != ' ' && ch != '-') o.push_back(char(std::tolower(static_cast<unsigned char>(ch))));
		}
		return o;
	};
	const std::string want = norm(arg);
	std::string best;
	for (const std::string& stem : ctw::list_campaigns(files)) {
		const std::string n = norm(stem);
		if (n == want) return stem;
		if (best.empty() && n.find(want) != std::string::npos) best = stem;
	}
	return best;
}

// One CtW battle ready to generate: the campaign (kept alive: BattleDef points into nothing, but the input was built from it), the generator
// parameters as new_random_game would build them from BattleDef.setup, and the conquest map input.
struct CtwCase {
	std::unique_ptr<ctw::ConquestGame> game;
	ctw::ConquestGame::BattleDef battle;
	Params p;
	ConquestInput input;
	std::string desc;
};

// links: the player's available attacks of a fresh campaign, in find_available_links order.
std::vector<ctw::LinkChoice> ctw_links(const GameFiles& files, const Rules& rules, const std::string& stem, int difficulty, uint32_t seed,
                                       std::unique_ptr<ctw::ConquestGame>& game, std::string* err) {
	game = std::make_unique<ctw::ConquestGame>();
	ctw::ConquestSetup cs;
	cs.campaign = stem;
	cs.seed = seed;
	cs.difficulty = difficulty;
	const std::string e = game->setup(files, rules, cs);
	if (!e.empty()) {
		if (err) *err = e;
		return {};
	}
	return game->find_available_links(game->player_tribe);
}

bool build_ctw_case(const GameFiles& files, const Rules& rules, CtwCase& c, const ctw::LinkChoice& link, int style_override, uint32_t seed,
                    std::string* err) {
	ctw::ConquestGame& g = *c.game;
	if (link.needs_war) g.set_war(link.attacker, link.defender);
	g.cur_link = link;
	g.cur_style = g.pick_style(link);
	c.battle = g.make_battle(link, g.cur_style);
	const sim::RandomGameSetup& S = c.battle.setup;
	// A campaign's named historical battles are custom .scx maps (loaded, not generated); the generator is exercised on them anyway.
	const bool custom_ignored = !c.battle.custom_map.empty();
	c.battle.custom_map.clear();
	c.p = Params();
	c.p.style = S.map_style;
	c.p.size = S.map_size;
	c.p.seed = seed ? seed : S.seed;
	c.p.nations = S.num_nations;
	c.p.randomize_starts = S.randomize_starts;
	c.p.starting_town = S.starting_town;
	c.p.starting_resources = S.starting_resources;
	c.p.reveal_map = S.reveal_map;
	c.p.ctw = true;
	for (int i = 0; i < 8; i++) {
		c.p.team[size_t(i)] = i < S.num_nations && S.nation[size_t(i)].team >= 0 ? S.nation[size_t(i)].team : 8;
		c.p.tribe[size_t(i)] = i < S.num_nations && S.nation[size_t(i)].tribe >= 0 ? S.nation[size_t(i)].tribe : i;
	}
	std::string e;
	if (!ctw::conquest_battle_input(g, files, c.battle, &c.input, &e)) {
		if (err) *err = e;
		return false;
	}
	if (style_override == kStyleConquest || style_override == kStyleConquestSea) {
		c.input.sea = style_override == kStyleConquestSea;
		c.p.style = style_override;
	} else if (style_override >= 0) {
		if (err) *err = "--ctw takes --style 24 or 25";
		return false;
	}
	(void)rules;
	const ctw::CountryDef& A = g.def->countries[size_t(link.from)];
	const ctw::CountryDef& D = g.def->countries[size_t(link.to)];
	char buf[512];
	std::snprintf(buf, sizeof buf, "%s: %s (%s) attacks %s (%s)%s, conquest map style %d, %zu seats, %d barbarians, %zu supporters, image %dx%d",
	              g.def->file.c_str(), A.id.c_str(), g.tribe_name(link.attacker).c_str(), D.id.c_str(),
	              link.defender >= 0 ? g.tribe_name(link.defender).c_str() : "barbarians", link.sea ? " by sea" : "", c.battle.map_style,
	              c.battle.seats.size(), c.input.barbarians, c.input.supporters.size(), c.input.side, c.input.side);
	c.desc = buf;
	if (custom_ignored) c.desc += " [the style's custom map is ignored]";
	return true;
}

// FROM>TO, auto or auto:N
bool pick_ctw_link(ctw::ConquestGame& g, const std::vector<ctw::LinkChoice>& links, const std::string& spec, ctw::LinkChoice* out, std::string* err) {
	if (links.empty()) {
		if (err) *err = "the player has no attack available";
		return false;
	}
	if (spec.empty() || spec.compare(0, 4, "auto") == 0) {
		const size_t n = spec.size() > 5 ? size_t(std::atoi(spec.c_str() + 5)) : 0;
		*out = links[std::min(n, links.size() - 1)];
		return true;
	}
	const size_t gt = spec.find('>');
	const int from = gt == std::string::npos ? -1 : g.find_node(spec.substr(0, gt));
	const int to = gt == std::string::npos ? -1 : g.find_node(spec.substr(gt + 1));
	for (const ctw::LinkChoice& l : links) {
		if (l.from == from && l.to == to) {
			*out = l;
			return true;
		}
	}
	if (err) *err = "link must be auto, auto:N or FROM>TO (territory ids available to the player)";
	return false;
}

// ---------------------------------------------------------------- --all

struct CaseResult {
	int style = 0, size = 0, nations = 0;
	bool ok = false, crash = false, nondeterministic = false, slow = false, fallback = false;
	double seconds = 0;
	int water_gaps = 0;  // open-water wcoords with a tcoord that is not water (the generator must set them all)
	std::string error;
};

// Open-water wcoords (land 1 or 2, not half-land) whose 16 tcoords are not all water.
int water_gaps(const World& w) {
	int n = 0;
	for (int y = 0; y < w.ys; y++) {
		for (int x = 0; x < w.xs; x++) {
			if (!World::is_ocean(w.w(x, y))) continue;
			bool gap = false;
			for (int j = 0; j < 4 && !gap; j++) {
				for (int i = 0; i < 4 && !gap; i++) gap = !w.is_tocean(x * 4 + i, y * 4 + j);
			}
			if (gap) n++;
		}
	}
	return n;
}

// ---------------------------------------------------------------- --compare

namespace dw = ron::sim::datawalk;

bool read_whole(const std::string& path, std::vector<uint8_t>& out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

bool gunzip_maybe(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
	if (!(in.size() > 2 && in[0] == 0x1f && in[1] == 0x8b)) {
		out = in;
		return true;
	}
	z_stream zs{};
	if (inflateInit2(&zs, 15 + 32) != Z_OK) return false;
	zs.next_in = const_cast<Bytef*>(in.data());
	zs.avail_in = uInt(in.size());
	out.clear();
	std::vector<uint8_t> buf(1 << 20);
	int rc;
	do {
		zs.next_out = buf.data();
		zs.avail_out = uInt(buf.size());
		rc = inflate(&zs, Z_NO_FLUSH);
		out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
	} while (rc == Z_OK);
	inflateEnd(&zs);
	return rc == Z_STREAM_END || !out.empty();
}

int64_t peek_int(const dw::Tree& t, const char* path) {
	dw::FieldValue v;
	std::string err;
	return dw::read_field(t, path, v, err) ? v.i : -1;
}

// The SimpleArray<WCoord> blocks of World::walk_data part 2 / 3, in walk order: start_x, start_y, start_city_x, start_city_y,
// oil_x, oil_y. Elements are int32 or int16 (inferred from the block size).
std::vector<std::vector<int32_t>> world_coord_arrays(const dw::Tree& t) {
	std::vector<std::vector<int32_t>> out;
	const int gid = t.description().find_global("world");
	const dw::Rec* w = gid >= 0 ? t.find_global(gid) : nullptr;
	if (!w) return out;
	for (const dw::Event& e : w->ev) {
		if (e.type != 2 || !e.cd) continue;
		std::vector<int32_t> v;
		const size_t n = size_t(std::max(e.cd->length, 0));
		const size_t esz = n ? e.cd->data.size() / n : 4;
		for (size_t i = 0; i < n && (i + 1) * esz <= e.cd->data.size(); i++) {
			const uint8_t* p = &e.cd->data[i * esz];
			v.push_back(esz == 2 ? int32_t(int16_t(p[0] | (p[1] << 8)))
			                     : int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24));
		}
		out.push_back(v);
		if (out.size() >= 6) break;
	}
	return out;
}

struct Layer {
	std::string name;
	long diff = 0, total = 0;
	std::string first;
};

void print_layer(const Layer& l) {
	std::printf("  %-22s %7ld / %-7ld  %s\n", l.name.c_str(), l.diff, l.total, l.diff ? l.first.c_str() : "exact");
}

template <class F>
Layer cell_layer(const char* name, const World& e, const World& g, F get) {
	Layer l;
	l.name = name;
	l.total = e.size;
	for (int y = 0; y < e.ys; y++) {
		for (int x = 0; x < e.xs; x++) {
			const long a = get(e.w(x, y)), b = get(g.w(x, y));
			if (a != b) {
				if (!l.diff) {
					char buf[96];
					std::snprintf(buf, sizeof buf, "first (%d,%d) exp=%ld got=%ld", x, y, a, b);
					l.first = buf;
				}
				l.diff++;
			}
		}
	}
	return l;
}

// Bit-by-bit: for each bit the cells where only the save has it (-) and where only the generator has it (+).
template <class F>
void bit_layers(const char* name, int nbits, const World& e, const World& g, F get, bool tiles) {
	const int xs = tiles ? e.tile_xs : e.xs, ys = tiles ? e.tile_ys : e.ys;
	for (int b = 0; b < nbits; b++) {
		long only_e = 0, only_g = 0, both = 0;
		std::string first;
		for (int y = 0; y < ys; y++) {
			for (int x = 0; x < xs; x++) {
				const bool a = (get(e, x, y) >> b) & 1, c = (get(g, x, y) >> b) & 1;
				if (a && c) both++;
				if (a && !c) only_e++;
				if (!a && c) only_g++;
				if (a != c && first.empty()) {
					char buf[96];
					std::snprintf(buf, sizeof buf, "first (%d,%d) exp=%d got=%d", x, y, a ? 1 : 0, c ? 1 : 0);
					first = buf;
				}
			}
		}
		if (!only_e && !only_g && !both) continue;
		char label[64];
		std::snprintf(label, sizeof label, "%s bit %02d (0x%x)", name, b, 1u << b);
		std::printf("  %-22s save %6ld  gen %6ld  both %6ld  diff %6ld (-%ld +%ld)  %s\n", label, only_e + both, only_g + both, both,
		            only_e + only_g, only_e, only_g, first.empty() ? "exact" : first.c_str());
	}
}

struct GoodKey {
	int32_t type, x, y;
	bool operator<(const GoodKey& o) const { return type != o.type ? type < o.type : x != o.x ? x < o.x : y < o.y; }
};

bool g_goodlist = false;  // --goodlist: print the goods / items that differ

void compare_goods(const char* name, const Rules& rules, const std::vector<GoodObj>& e, const std::vector<GoodObj>& g) {
	std::multiset<GoodKey> a, b;
	std::map<int, std::pair<int, int>> by_type;
	std::multiset<std::pair<int32_t, int32_t>> ca, cb;
	for (const GoodObj& o : e) {
		if (!o.alive()) continue;
		a.insert({o.type, o.x, o.y});
		ca.insert({o.x, o.y});
		by_type[o.type].first++;
	}
	for (const GoodObj& o : g) {
		if (!o.alive()) continue;
		b.insert({o.type, o.x, o.y});
		cb.insert({o.x, o.y});
		by_type[o.type].second++;
	}
	long exact = 0, cells = 0;
	for (const GoodKey& k : a) {
		auto it = b.find(k);
		if (it != b.end()) {
			exact++;
			b.erase(it);
		}
	}
	for (const auto& k : ca) {
		auto it = cb.find(k);
		if (it != cb.end()) {
			cells++;
			cb.erase(it);
		}
	}
	long na = 0, nb = 0;
	for (const auto& t : by_type) {
		na += t.second.first;
		nb += t.second.second;
	}
	std::printf("  %-22s save %5ld  gen %5ld  exact (type+pos) %5ld  same position any type %5ld\n", name, na, nb, exact, cells);
	if (g_goodlist) {
		std::multiset<GoodKey> a2, b2;
		for (const GoodObj& o : e) if (o.alive()) a2.insert({o.type, o.x, o.y});
		for (const GoodObj& o : g) if (o.alive()) b2.insert({o.type, o.x, o.y});
		for (const GoodKey& k : a2) {
			auto it = b2.find(k);
			if (it != b2.end()) b2.erase(it);
			else std::printf("      only save: type %d at (%d,%d)\n", int(k.type), int(k.x), int(k.y));
		}
		for (const GoodKey& k : b2) std::printf("      only gen : type %d at (%d,%d)\n", int(k.type), int(k.x), int(k.y));
	}
	for (const auto& t : by_type) {
		if (t.second.first == t.second.second) continue;
		std::printf("      type %-3d %-16s save %4d gen %4d\n", t.first, to_utf8(rules.types[size_t(t.first)].name).c_str(), t.second.first, t.second.second);
	}
}


// ---------------------------------------------------------------- --play189
// ron-mapgen <game dir> --play189 <file.rcx> [--save file.SVX] [--frames N] [--trace]
// Builds the replay's random game with new_random_game, feeds the replay's commands (the records of frame <= f run before f's
// do_frame, as apps/sav --replay does) and reports game_random after N frames; with --save, the distance in draws to the save's
// random_seed and the objects that differ (docs/randommap-spec.md "Oracle").
int run_play189(const GameFiles& files, const Rules& rules, const char* rcx_path, int argc, char** argv) {
	Replay rp;
	std::string err;
	if (!read_replay(rcx_path, rp, err)) {
		std::fprintf(stderr, "cannot read %s: %s\n", rcx_path, err.c_str());
		return 1;
	}
	const ReplayHeader& h = rp.header;
	RandomGameSetup S = default_random_setup(std::max(2, h.present_players()));
	S.map_style = h.setup_field(kSetMapStyle);
	S.map_size = h.setup_field(kSetMapSize);
	S.seed = h.seed;
	S.num_nations = h.present_players();
	S.game_rules = h.setup_field(kSetGameRules);
	S.game_speed = h.setup_field(kSetGameSpeed);
	S.pop_limit = h.setup_field(kSetPopLimit);
	S.tech_cost = h.setup_field(kSetTechCost);
	S.rush_rules = h.setup_field(kSetRushRules);
	S.reveal_map = h.setup_field(kSetRevealMap);
	S.victory = h.setup_field(kSetVictory);
	S.elimination = h.setup_field(kSetElimination);
	S.wonderwin = h.setup_field(kSetWonderWin);
	S.score_goal = h.setup_field(kSetScoreGoal);
	S.popwin = h.setup_field(kSetPopWin);
	S.time_limit = h.setup_field(kSetTimeLimit);
	S.econwin = h.setup_field(kSetEconWin);
	S.starting_town = h.setup_field(kSetStartingTown);
	S.starting_resources = h.setup_field(kSetStartingResources);
	S.starting_technology = h.setup_field(kSetStartingTechnology);
	S.ending_technology = h.setup_field(kSetEndingTechnology);
	std::array<int, 8> seat_who{};
	int k = 0;
	for (const ReplayPlayer& pl : h.players) {
		if (!pl.present()) continue;
		NationSetup& n = S.nation[size_t(k)];
		n.tribe = pl.tribe;
		n.team = pl.team >= 0 && pl.team < 8 ? pl.team : -1;
		n.human = k == 0;
		n.difficulty = pl.diff;
		seat_who[size_t(pl.slot & 7)] = pl.who;
		k++;
	}
	std::printf("replay %s: seed %u style %d size %d players %d town %d res %d tech %d..%d frames to %d\n", rcx_path, h.seed, S.map_style, S.map_size,
	            S.num_nations, S.starting_town, S.starting_resources, S.starting_technology, S.ending_technology, rp.last_frame());
	RandomExtras ex;
	std::unique_ptr<Game> g = new_random_game(files, rules, nullptr, S, &ex);
	if (!g) {
		std::fprintf(stderr, "new_random_game failed: %s\n", ex.error.c_str());
		return 1;
	}
	const char* fa = arg_value(argc, argv, "--frames");
	const int frames = fa ? std::atoi(fa) : rp.last_frame();
	const uint32_t seed0 = g->rng.seed();
	std::printf("after new_random_game: frame %d game_random 0x%08x (%llu generator draws)\n", g->frame, seed0, (unsigned long long)ex.result.draws);
	sim::CommandPackage package;
	size_t ri = 0;
	std::map<int, int> skipped;
	for (int f = g->frame; f < frames; f++) {
		while (ri < rp.packages.size() && rp.packages[ri].frame <= f) {
			const ReplayPackage& pk = rp.packages[ri++];
			package.group = -1;
			for (const ReplayCommand& c : pk.cmds) {
				if (c.op == op::kTurnData || c.op == op::kCamera || c.op == op::kCheckSums || c.op == op::kPlayerSpeed) continue;
				Command cmd;
				if (!replay_to_engine(pk.data(c), c.len, cmd)) {
					skipped[c.op]++;
					continue;
				}
				package.process(*g, cmd, pk.play, seat_who[size_t(pk.play & 7)]);
			}
		}
		const uint32_t before = g->rng.seed();
		g->do_frame();
		if (has_flag(argc, argv, "--trace")) {
			long long n = 0;
			for (uint32_t q = before; q != g->rng.seed() && n < 1000000; q = q * 0x19660Du + 0x3C6EF35Fu) n++;
			if (n) std::printf("  frame %d: %lld draws\n", f, n);
		}
	}
	for (const auto& kv : skipped) std::printf("  commands not executed: %s x%d\n", replay_verb_name(kv.first), kv.second);
	const uint32_t seed1 = g->rng.seed();
	auto distance = [](uint32_t from, uint32_t to) -> long long {
		uint32_t f = from;
		for (long long n = 0; n <= 4000000; n++) {
			if (f == to) return n;
			f = f * 0x19660Du + 0x3C6EF35Fu;
		}
		return -1;
	};
	std::printf("after %d frames: game_random 0x%08x (%lld draws after new_random_game)\n", g->frame, seed1, distance(seed0, seed1));
	if (const char* sv = arg_value(argc, argv, "--save")) {
		std::vector<uint8_t> raw, save;
		if (!read_whole(sv, raw) || !gunzip_maybe(raw, save)) {
			std::fprintf(stderr, "cannot read %s\n", sv);
			return 1;
		}
		auto peek = std::make_unique<dw::Tree>(dw::Description::get());
		dw::SaveDecode sd = dw::decode_save(*peek, save.data(), save.size(), 0);
		GameSetup setup;
		setup.starting_technology = int32_t(peek_int(*peek, "game.info.starting_technology"));
		setup.starting_technology2 = int32_t(peek_int(*peek, "game.info.starting_technology2"));
		setup.ending_technology = int32_t(peek_int(*peek, "game.info.ending_technology"));
		setup.game_rules = int32_t(peek_int(*peek, "game.info.game_rules"));
		setup.flag8 = (peek_int(*peek, "game.info.flags") & 8) != 0;
		Rules srules = rules;
		start_game(srules, setup);
		OriginalLoadReport rep;
		std::unique_ptr<Game> eg = load_original_save(files, srules, nullptr, save, &rep);
		if (!eg) {
			std::fprintf(stderr, "load failed: %s\n", rep.error.c_str());
			return 1;
		}
		const uint32_t want = eg->rng.seed();
		std::printf("save frame %d game_random 0x%08x: engine is %lld draws before it (generation + frames); engine-ahead %lld\n", eg->frame, want,
		            distance(seed1, want), distance(want, seed1));
		std::printf("save: draws from the generation end to the save would be %lld if the engine's generation draws were exact\n", distance(seed0, want));
	}
	return 0;
}

int run_compare(const GameFiles& files, const Rules& rules, const char* save_path, int argc, char** argv) {
	std::vector<uint8_t> raw, save;
	if (!read_whole(save_path, raw) || !gunzip_maybe(raw, save)) {
		std::fprintf(stderr, "cannot read %s\n", save_path);
		return 1;
	}
	auto peek = std::make_unique<dw::Tree>(dw::Description::get());
	dw::SaveDecode sd = dw::decode_save(*peek, save.data(), save.size(), 0);
	if (!sd.result.ok) {
		std::fprintf(stderr, "decode failed: %s\n", sd.result.error.c_str());
		return 1;
	}
	GameSetup setup;
	setup.starting_technology = int32_t(peek_int(*peek, "game.info.starting_technology"));
	setup.starting_technology2 = int32_t(peek_int(*peek, "game.info.starting_technology2"));
	setup.ending_technology = int32_t(peek_int(*peek, "game.info.ending_technology"));
	setup.game_rules = int32_t(peek_int(*peek, "game.info.game_rules"));
	setup.flag8 = (peek_int(*peek, "game.info.flags") & 8) != 0;
	Rules srules = rules;
	start_game(srules, setup);
	OriginalLoadReport rep;
	std::unique_ptr<Game> eg = load_original_save(files, srules, nullptr, save, &rep);
	if (!eg) {
		std::fprintf(stderr, "load failed: %s\n", rep.error.c_str());
		return 1;
	}
	Params p;
	p.seed = uint32_t(peek_int(*peek, "game.info.seed"));
	p.size = int(peek_int(*peek, "game.info.map_size"));
	const int lobby_style = int(peek_int(*peek, "game.info.map_style"));
	const int resolved_style = int(peek_int(*peek, "world.map"));
	p.style = lobby_style;
	if (const char* st = arg_value(argc, argv, "--style")) p.style = style_from_name(rules, st);
	int nations = 0;
	for (int i = 0; i < 8; i++) {
		if (!(eg->leaders[size_t(i)].flags & 1)) continue;
		nations = i + 1;
		p.tribe[size_t(i)] = eg->leaders[size_t(i)].tribe;
		p.team[size_t(i)] = eg->info.team[size_t(i)] >= 0 ? eg->info.team[size_t(i)] : 8;
	}
	p.nations = nations;
	p.team_style = eg->info.team_style;
	p.starting_town = eg->info.starting_town;
	p.starting_resources = eg->info.starting_resources;
	p.reveal_map = eg->info.reveal_map;
	std::printf("save %s: frame %d seed %u size %d (%dx%d) lobby style %d resolved %d sea_map %d nations %d team_style %d town %d\n", save_path,
	            eg->frame, p.seed, p.size, eg->world.xs, eg->world.ys, lobby_style, resolved_style, eg->world.sea_map, nations, p.team_style,
	            p.starting_town);
	std::printf("  tribes:");
	for (int i = 0; i < nations; i++) std::printf(" %d", p.tribe[size_t(i)]);
	std::printf("  teams:");
	for (int i = 0; i < nations; i++) std::printf(" %d", p.team[size_t(i)]);
	std::printf("\n");

	Generated g;
	run_one(files, rules, p, g);
	if (!g.res.ok) {
		std::fprintf(stderr, "generation failed: %s\n", g.res.error.c_str());
		return 1;
	}
	if (has_flag(argc, argv, "--game")) {
		// --game: compare the world of new_random_game (generator + start cities + the setup's own passes) instead of the generator's alone,
		// so the effects of the starting cities (val, blocked, bad, solid, roads, building flags) are in the generated side.
		RandomGameSetup S = default_random_setup(std::max(2, nations));
		S.map_style = lobby_style;
		S.map_size = p.size;
		S.seed = p.seed;
		S.num_nations = nations;
		S.starting_town = eg->info.starting_town;
		S.starting_resources = eg->info.starting_resources;
		S.starting_technology = setup.starting_technology;
		S.ending_technology = setup.ending_technology;
		S.reveal_map = eg->info.reveal_map;
		S.no_tribe_powers = (eg->info.flags & 4) != 0;
		for (int i = 0; i < nations; i++) {
			S.nation[size_t(i)].tribe = p.tribe[size_t(i)];
			S.nation[size_t(i)].team = p.team[size_t(i)] >= 0 && p.team[size_t(i)] < 8 ? p.team[size_t(i)] : -1;
			S.nation[size_t(i)].human = i == 0;
		}
		RandomExtras ex;
		std::unique_ptr<Game> ng = new_random_game(files, rules, nullptr, S, &ex);
		if (!ng) {
			std::fprintf(stderr, "new_random_game failed: %s\n", ex.error.c_str());
			return 1;
		}
		g.world = ng->world;
		std::printf("(--game: the world of new_random_game, frame %d)\n", ng->frame);
	}
	const World& E = eg->world;
	const World& G = g.world;
	std::printf("generated: picked style %d (save %d)%s, %dx%d, world.seed %u (save %d), sea_map %d (save %d), tileset %s, %llu draws\n", g.res.picked_style,
	            resolved_style, g.res.picked_style == resolved_style ? "" : " MISMATCH", G.xs, G.ys, g.res.world_seed,
	            int(peek_int(*peek, "world.seed")), G.sea_map, E.sea_map, g.res.tileset.c_str(), (unsigned long long)g.res.draws);
	if (E.xs != G.xs || E.ys != G.ys) {
		std::printf("map sizes differ: nothing else comparable\n");
		return 0;
	}
	g_goodlist = has_flag(argc, argv, "--goodlist");
	if (const char* cs = arg_value(argc, argv, "--cells")) {
		// --cells x0,y0,x1,y1 [--layer land|sub|flags|region|region2|mask]: grids of the save (left) and generator (right)
		const std::vector<int> b = parse_list(cs);
		const char* ly = arg_value(argc, argv, "--layer");
		const std::string layer = ly ? ly : "land";
		const bool tiles = layer == "mask";
		auto val = [&](const World& w, int x, int y) -> int {
			if (layer == "mask") return w.t(x, y);
			const WData& d = w.w(x, y);
			return layer == "land" ? d.land : layer == "sub" ? d.land_sub : layer == "flags" ? d.flags : layer == "region" ? d.region : layer == "blocked" ? d.blocked : layer == "bad" ? d.bad : layer == "solid" ? d.solid : layer == "val" ? d.val : layer == "goods" ? d.goods : d.region2;
		};
		(void)tiles;
		if (b.size() == 4 && layer == "feat") {
			auto ch = [](const WData& d) -> char {
				if (World::is_ocean(d)) return d.land == 1 ? '-' : '~';
				if (d.flags & 0x1000) return 'r';
				if (d.flags & 0x10) return 'M';
				if (d.flags & 0x08) return 'R';
				if (d.flags & 0x20) return 'F';
				if (d.flags & 0x800) return 'o';
				if (d.flags & 0x100) return 'h';
				if (d.flags & 0x04) return 'c';
				return '.';
			};
			for (int y = b[1]; y <= b[3]; y++) {
				std::string l, r;
				for (int x = b[0]; x <= b[2]; x++) l += ch(E.w(x, y));
				for (int x = b[0]; x <= b[2]; x++) r += ch(G.w(x, y));
				std::printf("%3d | %s | %s\n", y, l.c_str(), r.c_str());
			}
		} else if (b.size() == 4) {
			for (int y = b[1]; y <= b[3]; y++) {
				std::string l, r;
				char t[16];
				for (int x = b[0]; x <= b[2]; x++) {
					std::snprintf(t, sizeof t, layer == "flags" || layer == "mask" ? "%04x " : "%3d ", val(E, x, y));
					l += t;
					std::snprintf(t, sizeof t, layer == "flags" || layer == "mask" ? "%04x " : "%3d ", val(G, x, y));
					r += t;
				}
				std::printf("%3d | %s | %s\n", y, l.c_str(), r.c_str());
			}
		}
		return 0;
	}
	if (has_flag(argc, argv, "--subtab")) {
		std::map<std::string, int> tab;
		for (int y = 0; y < E.ys; y++)
			for (int x = 0; x < E.xs; x++) {
				const WData& a = E.w(x, y);
				const WData& b = G.w(x, y);
				char k[160];
				std::snprintf(k, sizeof k, "save[land=%d sub=%d] gen[land=%d sub=%d]", a.land, a.land_sub, b.land, b.land_sub);
				tab[k]++;
			}
		for (const auto& kv : tab) std::printf("  %6d  %s\n", kv.second, kv.first.c_str());
		return 0;
	}
	if (has_flag(argc, argv, "--table")) {
		// contingency of the generator's cell class (water / coast flag) against the save's land, half-land and coast bits
		std::map<std::string, int> tab;
		for (int y = 0; y < E.ys; y++) {
			for (int x = 0; x < E.xs; x++) {
				const WData& a = E.w(x, y);
				const WData& b = G.w(x, y);
				char k[160];
				std::snprintf(k, sizeof k, "gen[%s%s] -> save[land=%d half=%d coast=%d c400=%d]", World::is_ocean(b) ? "water" : "land",
				              (b.flags & 4) ? "+coast" : "", a.land, (a.flags & 0x100) ? 1 : 0, (a.flags & 4) ? 1 : 0, (a.flags & 0x400) ? 1 : 0);
				tab[k]++;
			}
		}
		for (const auto& kv : tab) std::printf("  %6d  %s\n", kv.second, kv.first.c_str());
		return 0;
	}
	if (has_flag(argc, argv, "--mlist")) {
		std::printf("save mountains in placement order (range at wx,wy):");
		for (const PlacedRange& pr : eg->mountains) std::printf(" r%d@(%d,%d)", pr.range, pr.wx, pr.wy);
		std::printf("\ngenerated:");
		for (const PlacedMountain& pm : g.res.mountains) std::printf(" r%d@(%d,%d)", pm.range, pm.wx, pm.wy);
		std::printf("\n");
		return 0;
	}
	if (has_flag(argc, argv, "--mstat")) {
		// distance statistics of the save's mountain cells (flag 0x10): to water (is_ocean or half-land), to the nearest start, to the map edge
		std::map<int, int> dw, ds, de;
		for (int y = 0; y < E.ys; y++)
			for (int x = 0; x < E.xs; x++) {
				if (!(E.w(x, y).flags & 0x10)) continue;
				int bw = 99, bs = 99;
				for (int yy = 0; yy < E.ys; yy++)
					for (int xx = 0; xx < E.xs; xx++) {
						const WData& d = E.w(xx, yy);
						if (World::is_ocean(d) || (d.flags & 0x100)) bw = std::min(bw, vector_dist(xx - x, yy - y));
					}
				for (const auto& sp : g.res.starts) bs = std::min(bs, vector_dist(sp[0] - x, sp[1] - y));
				dw[bw]++;
				ds[std::min(bs, 40)]++;
				de[std::min(std::min(x, y), std::min(E.xs - 1 - x, E.ys - 1 - y))]++;
			}
		std::printf("mountain cells: distance to water (incl. half):");
		for (auto& kv : dw) std::printf(" %d:%d", kv.first, kv.second);
		std::printf("\n distance to the nearest start:");
		for (auto& kv : ds) std::printf(" %d:%d", kv.first, kv.second);
		std::printf("\n distance to the map edge:");
		for (auto& kv : de) std::printf(" %d:%d", kv.first, kv.second);
		std::printf("\n");
		return 0;
	}
	if (has_flag(argc, argv, "--odd")) {
		// the cells where the generator says water and the save says plain land (land 0, not half-land)
		for (int y = 0; y < E.ys; y++)
			for (int x = 0; x < E.xs; x++) {
				const WData& a = E.w(x, y);
				const WData& b = G.w(x, y);
				if (World::is_ocean(b) && a.land == 0 && !(a.flags & 0x100))
					std::printf("odd (%d,%d): save land %d flags %04x region %d | gen land %d flags %04x region %d\n", x, y, a.land, a.flags, a.region, b.land, b.flags, b.region);
			}
		return 0;
	}
	if (has_flag(argc, argv, "--oilorder")) {
		// the oil goods in list order (= placement order in the save), with the region of their cell
		for (int side = 0; side < 2; side++) {
			const World& Wd = side == 0 ? E : G;
			const std::vector<GoodObj>& gl = side == 0 ? eg->goods : g.goods;
			std::printf("%s oil goods in list order:", side == 0 ? "save" : "gen ");
			for (const GoodObj& go : gl) {
				if (go.type != kOilGood) continue;
				const int x = wcoord_of(go.x), y = wcoord_of(go.y);
				std::printf(" (%d,%d r%d)", x, y, Wd.w(x, y).region);
			}
			std::printf("\n");
		}
		return 0;
	}
	if (has_flag(argc, argv, "--oil")) {
		for (int side = 0; side < 2; side++) {
			const World& Wd = side == 0 ? E : G;
			std::printf("%s oil cells:", side == 0 ? "save" : "gen ");
			for (int y = 0; y < Wd.ys; y++)
				for (int x = 0; x < Wd.xs; x++)
					if (Wd.w(x, y).flags & 0x800) std::printf(" (%d,%d%s r%d)", x, y, World::is_ocean(Wd.w(x, y)) ? "w" : "", Wd.w(x, y).region);
			std::printf("\n");
		}
		return 0;
	}
	if (const char* rd = arg_value(argc, argv, "--regdump")) {
		FILE* f = std::fopen(rd, "w");
		if (f) {
			for (const WCoord& c : (has_flag(argc, argv, "--regdump-save") ? eg->regions : g.regions).r[size_t(std::atoi(arg_value(argc, argv, "--regid") ? arg_value(argc, argv, "--regid") : "1"))].coords) std::fprintf(f, "%d %d\n", c.x, c.y);
			std::fclose(f);
		}
		return 0;
	}
	if (const char* rs = arg_value(argc, argv, "--regcells")) {
		const int rid = std::atoi(rs);
		for (int side = 0; side < 2; side++) {
			const Regions& R = side == 0 ? eg->regions : g.regions;
			const Region& rr = R.r[size_t(rid)];
			std::printf("%s region %d: size %d coords %zu first:", side == 0 ? "save" : "gen ", rid, rr.size, rr.coords.size());
			for (size_t i = 0; i < rr.coords.size() && i < 14; i++) std::printf(" (%d,%d)", rr.coords[i].x, rr.coords[i].y);
			std::printf("\n");
		}
		return 0;
	}
	if (const char* hs = arg_value(argc, argv, "--heights")) {
		// --heights x0,y0,x1,y1: the save's height field (vertex coordinates, (4 xs + 1) per row), rounded
		const std::vector<int> b = parse_list(hs);
		if (b.size() == 4) {
			for (int y = b[1]; y <= b[3]; y++) {
				std::printf("%3d |", y);
				for (int x = b[0]; x <= b[2]; x++) std::printf(" %7.1f", double(eg->heights.h[size_t(y) * size_t(eg->heights.vxs) + size_t(x)]));
				std::printf("\n");
			}
			if (!g.res.heights.empty() && has_flag(argc, argv, "--gen")) {
				std::printf("generated:\n");
				for (int y = b[1]; y <= b[3]; y++) {
					std::printf("%3d |", y);
					for (int x = b[0]; x <= b[2]; x++) std::printf(" %7.1f", double(g.res.heights[size_t(y) * size_t(g.res.height_vxs) + size_t(x)]));
					std::printf("\n");
				}
			}
		}
		return 0;
	}
	std::printf("\nlayer                       differing / total   first difference\n");
	print_layer(cell_layer("land", E, G, [](const WData& d) { return long(d.land); }));
	print_layer(cell_layer("water (is_ocean)", E, G, [](const WData& d) { return long(World::is_ocean(d) ? 1 : 0); }));
	print_layer(cell_layer("land_sub", E, G, [](const WData& d) { return long(d.land_sub); }));
	print_layer(cell_layer("WData.flags (all)", E, G, [](const WData& d) { return long(d.flags); }));
	print_layer(cell_layer("region", E, G, [](const WData& d) { return long(d.region); }));
	print_layer(cell_layer("region2", E, G, [](const WData& d) { return long(d.region2); }));
	print_layer(cell_layer("val", E, G, [](const WData& d) { return long(d.val); }));
	print_layer(cell_layer("goods (near bits)", E, G, [](const WData& d) { return long(d.goods); }));
	print_layer(cell_layer("blocked", E, G, [](const WData& d) { return long(d.blocked); }));
	print_layer(cell_layer("bad", E, G, [](const WData& d) { return long(d.bad); }));
	print_layer(cell_layer("solid", E, G, [](const WData& d) { return long(d.solid); }));
	{
		Layer l;
		l.name = "tdata (all)";
		l.total = E.tile_size;
		for (int y = 0; y < E.tile_ys; y++) {
			for (int x = 0; x < E.tile_xs; x++) {
				if (E.t(x, y) != G.t(x, y)) {
					if (!l.diff) {
						char buf[96];
						std::snprintf(buf, sizeof buf, "first tile (%d,%d) exp=0x%04x got=0x%04x", x, y, E.t(x, y), G.t(x, y));
						l.first = buf;
					}
					l.diff++;
				}
			}
		}
		print_layer(l);
		Layer lw;
		lw.name = "tdata water (&0x30==0x20)";
		lw.total = E.tile_size;
		for (int y = 0; y < E.tile_ys; y++) {
			for (int x = 0; x < E.tile_xs; x++) {
				if (E.is_tocean(x, y) != G.is_tocean(x, y)) {
					if (!lw.diff) {
						char buf[96];
						std::snprintf(buf, sizeof buf, "first tile (%d,%d) exp=0x%04x got=0x%04x", x, y, E.t(x, y), G.t(x, y));
						lw.first = buf;
					}
					lw.diff++;
				}
			}
		}
		print_layer(lw);
	}
	if (const char* wf = arg_value(argc, argv, "--wfull")) {  // --wfull file: text, one line per cell: x y save_land save_flags gen_land gen_flags (differing cells only)
		if (FILE* f = std::fopen(wf, "w")) {
			for (int y = 0; y < E.ys; y++) {
				for (int x = 0; x < E.xs; x++) {
					const WData& a = E.w(x, y);
					const WData& b = G.w(x, y);
					if (a.land != b.land || a.flags != b.flags || a.land_sub != b.land_sub)
						std::fprintf(f, "%d %d %d %u %d %u %d %d\n", x, y, int(a.land), unsigned(a.flags), int(b.land), unsigned(b.flags), int(a.land_sub), int(b.land_sub));
				}
			}
			std::fclose(f);
		}
	}
	std::printf("\nWData.flags bit by bit (cells: save, generated, both, differing -only save +only generated):\n");
	bit_layers("flags", 16, E, G, [](const World& w, int x, int y) { return unsigned(w.w(x, y).flags); }, false);
	std::printf("\nTData.mask bit by bit:\n");
	bit_layers("tdata", 16, E, G, [](const World& w, int x, int y) { return unsigned(w.t(x, y)); }, true);

	if (!g.res.heights.empty() && eg->heights.h.size() == g.res.heights.size()) {
		// the height grid: the generator has the land fractals, the coast generators and the sea, not mountains or river troughs
		std::printf("\nheights (%d x %d vertices; class = the save's height):\n", g.res.height_vxs, g.res.height_vys);
		struct Cls {
			const char* name;
			float lo, hi;
			long n = 0, exact = 0, near = 0;
			double maxd = 0;
			int fx = -1, fy = -1;
		};
		Cls cl[4] = {{"sea floor (< -250)", -1e9f, -250.0f}, {"shelf (-250 .. -30)", -250.0f, -30.0f}, {"shore (-30 .. 60)", -30.0f, 60.0f}, {"land (>= 60)", 60.0f, 1e9f}};
		for (int vy = 0; vy < g.res.height_vys; vy++) {
			for (int vx = 0; vx < g.res.height_vxs; vx++) {
				const size_t i = size_t(vy) * size_t(g.res.height_vxs) + size_t(vx);
				const float a = eg->heights.h[i], b = g.res.heights[i];
				for (Cls& c : cl) {
					if (a < c.lo || a >= c.hi) continue;
					c.n++;
					const double d = std::fabs(double(a) - double(b));
					if (a == b) c.exact++;
					if (d < 0.01) c.near++;
					else if (c.fx < 0) {
						c.fx = vx;
						c.fy = vy;
					}
					c.maxd = std::max(c.maxd, d);
				}
			}
		}
		if (const char* hd = arg_value(argc, argv, "--hdump")) {
			// --hdump file: every vertex whose height differs by more than 0.01: x y save gen
			if (FILE* f = std::fopen(hd, "w")) {
				for (int vy = 0; vy < g.res.height_vys; vy++) {
					for (int vx = 0; vx < g.res.height_vxs; vx++) {
						const size_t i = size_t(vy) * size_t(g.res.height_vxs) + size_t(vx);
						const float a = eg->heights.h[i], b = g.res.heights[i];
						if (std::fabs(double(a) - double(b)) > 0.01) std::fprintf(f, "%d %d %.3f %.3f\n", vx, vy, double(a), double(b));
					}
				}
				std::fclose(f);
			}
		}
		if (const char* hf = arg_value(argc, argv, "--hfull")) {
			// --hfull file: every vertex, binary: int32 vxs, vys, then float32 save heights and float32 generated heights (x fastest)
			if (FILE* f = std::fopen(hf, "wb")) {
				const int32_t dims[2] = {g.res.height_vxs, g.res.height_vys};
				std::fwrite(dims, 4, 2, f);
				std::fwrite(eg->heights.h.data(), 4, eg->heights.h.size(), f);
				std::fwrite(g.res.heights.data(), 4, g.res.heights.size(), f);
				std::fclose(f);
			}
		}
		{
			double aq = 0;  // the sum of |generated - save| over the aquatic and shore vertices (save < 60)
			for (size_t i = 0; i < eg->heights.h.size(); i++) {
				if (eg->heights.h[i] < 60.0f) aq += std::fabs(double(eg->heights.h[i]) - double(g.res.heights[i]));
			}
			std::printf("  aquatic |error| sum (save < 60): %.2f\n", aq);
		}
		for (const Cls& c : cl) {
			std::printf("  %-22s %7ld vertices  bit-exact %7ld  within 0.01 %7ld  max |d| %9.3f  first (%d,%d)\n", c.name, c.n, c.exact, c.near, c.maxd, c.fx, c.fy);
		}
	}

	std::printf("\nregions (sizes, sorted; land 1..63 / sea 64..126):\n");
	for (int pass = 0; pass < 2; pass++) {
		for (int side = 0; side < 2; side++) {
			const Regions& R = side == 0 ? eg->regions : g.regions;
			std::vector<int> v;
			for (int i = pass ? 64 : 1; i < (pass ? 127 : 64); i++) {
				if (R.r[size_t(i)].size) v.push_back(R.r[size_t(i)].size);
			}
			std::sort(v.rbegin(), v.rend());
			std::printf("  %s %-4s:", pass ? "sea " : "land", side == 0 ? "save" : "gen");
			for (int s : v) std::printf(" %d", s);
			std::printf("\n");
		}
	}

	if (const char* side = arg_value(argc, argv, "--side")) {
		// expected | generated | difference (red: land or coast differs; orange: forest / mountain / rock differ)
		Generated ex;
		ex.world = eg->world;
		ex.goods = eg->goods;
		ex.items = eg->items;
		const auto arrs = world_coord_arrays(*peek);
		if (arrs.size() >= 2) {
			for (size_t i = 0; i < arrs[0].size() && i < arrs[1].size(); i++) ex.res.starts.push_back({arrs[0][i], arrs[1][i]});
		}
		const int sc = 6;
		Png a = render_png(ex, sc), b = render_png(g, sc);
		Png out(a.w * 3 + 8, a.h);
		for (int y = 0; y < a.h; y++) {
			for (int x = 0; x < a.w; x++) {
				const uint8_t* pa = &a.rgb[(size_t(y) * size_t(a.w) + size_t(x)) * 3];
				const uint8_t* pb = &b.rgb[(size_t(y) * size_t(b.w) + size_t(x)) * 3];
				out.put(x, y, pa[0], pa[1], pa[2]);
				out.put(a.w + 4 + x, y, pb[0], pb[1], pb[2]);
				const int wx = x / sc, wy = y / sc;
				const WData& d1 = E.w(wx, wy);
				const WData& d2 = G.w(wx, wy);
				uint8_t r = 40, gg = 40, bl = 40;
				if (d1.land != d2.land || (d1.flags & 0x104) != (d2.flags & 0x104)) r = 230, gg = 40, bl = 40;
				else if ((d1.flags & 0x78) != (d2.flags & 0x78)) r = 230, gg = 150, bl = 30;
				else if (d1.flags != d2.flags) r = 120, gg = 120, bl = 200;
				out.put(a.w * 2 + 8 + x, y, r, gg, bl);
			}
		}
		if (!out.save(side)) std::fprintf(stderr, "cannot write %s\n", side);
	}

	std::printf("\nresources / goodies:\n");
	compare_goods("goods", rules, eg->goods, g.goods);
	compare_goods("goodies (items)", rules, eg->items, g.items);

	const auto arrays = world_coord_arrays(*peek);
	if (arrays.size() >= 2) {
		std::printf("\nstart positions (save World.start_x/y vs generator):\n  save:");
		for (size_t i = 0; i < arrays[0].size() && i < arrays[1].size(); i++) std::printf(" (%d,%d)", arrays[0][i], arrays[1][i]);
		std::printf("\n  gen: ");
		for (const auto& s : g.res.starts) std::printf(" (%d,%d)", s[0], s[1]);
		std::printf("\n");
		if (arrays.size() >= 6) {
			std::printf("  oil save:");
			for (size_t i = 0; i < arrays[4].size() && i < arrays[5].size(); i++) std::printf(" (%d,%d)", arrays[4][i], arrays[5][i]);
			std::printf("\n  oil gen: ");
			for (const auto& s : g.res.oil_locations) std::printf(" (%d,%d)", s[0], s[1]);
			std::printf("\n");
		}
	}
	return 0;
}


}  // namespace

// ---------------------------------------------------------------- --png-check

int run_png_check(const GameFiles& files, const char* dir_arg, bool quiet) {
	namespace fs = std::filesystem;
	std::string dir = dir_arg ? dir_arg : files.root() + "/conquest/RenderMaps/MapPieces";
	if (dir.empty() || !fs::is_directory(dir)) {
		std::fprintf(stderr, "no directory %s\n", dir.c_str());
		return 2;
	}
	std::vector<std::string> paths;
	for (const auto& e : fs::recursive_directory_iterator(dir)) {
		if (!e.is_regular_file()) continue;
		std::string ext = e.path().extension().string();
		for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
		if (ext == ".png") paths.push_back(e.path().string());
	}
	std::sort(paths.begin(), paths.end());
	int ok = 0, bad = 0;
	uint64_t pixels = 0, opaque = 0;
	std::map<std::string, int> sizes;
	std::map<std::string, std::pair<int, int>> dims;  // file name (lower) -> w, h
	for (const std::string& path : paths) {
		Image img;
		std::string err;
		if (!load_png(path, img, &err)) {
			bad++;
			std::printf("FAIL %s\n", err.c_str());
			continue;
		}
		ok++;
		pixels += uint64_t(img.width) * uint64_t(img.height);
		for (size_t i = 3; i < img.rgba.size(); i += 4) opaque += img.rgba[i] != 0;
		if (!quiet) std::printf("%5d x %-5d %s\n", img.width, img.height, path.substr(dir.size() + (dir.back() == '/' ? 0 : 1)).c_str());
		sizes[std::to_string(img.width) + "x" + std::to_string(img.height)]++;
		std::string name = fs::path(path).filename().string();
		for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
		dims[name] = {img.width, img.height};
	}
	std::printf("png-check: %d files decoded, %d failed, %llu pixels (%llu with alpha != 0), %zu distinct sizes\n", ok, bad,
	            (unsigned long long)pixels, (unsigned long long)opaque, sizes.size());
	// the country silhouettes must be right-left+1 x bottom-top+1 pixels, one per COUNTRY
	Rules rules = load_rules(files);
	int checked = 0, mismatch = 0, missing = 0;
	for (const std::string& stem : ctw::list_campaigns(files)) {
		std::shared_ptr<const ctw::CampaignDef> def;
		try {
			def = ctw::load_campaign(files, rules, stem);
		} catch (const std::exception&) {
			continue;
		}
		for (size_t i = 0; i < def->countries.size(); i++) {
			std::string name = "xpack_temp_country" + std::to_string(i) + def->file + ".png";
			for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
			const auto it = dims.find(name);
			if (it == dims.end()) {
				missing++;
				continue;
			}
			const ctw::CountryDef& c = def->countries[i];
			checked++;
			if (it->second.first != c.right - c.left + 1 || it->second.second != c.bottom - c.top + 1) {
				mismatch++;
				std::printf("size mismatch %s: png %dx%d, COUNTRY %s rect %dx%d\n", name.c_str(), it->second.first, it->second.second,
				            c.id.c_str(), c.right - c.left + 1, c.bottom - c.top + 1);
			}
		}
	}
	std::printf("silhouettes vs COUNTRY rectangles: %d checked, %d mismatched, %d without a file\n", checked, mismatch, missing);
	return bad || mismatch ? 1 : 0;
}

// ---------------------------------------------------------------- --all: the conquest styles

struct ConquestCase {
	std::string campaign, link;
	int style = 24;  // 24 / 25
	bool ok = false, nondeterministic = false, unplaced = false, gaps = false, custom = false;
	int seats = 0, starts = 0;
	double seconds = 0;
	std::string error;
};

int run_conquest_matrix(const GameFiles& files, const Rules& rules, uint32_t base_seed, int jobs, bool quiet, int per_campaign) {
	struct Job {
		std::string stem;
		int link_index;  // into ctw_links
		int style;       // -1 = the link's own, 24 / 25 forced
	};
	std::vector<Job> jobs_list;
	std::vector<ConquestCase> cases;
	for (const std::string& stem : ctw::list_campaigns(files)) {
		std::unique_ptr<ctw::ConquestGame> g;
		std::string err;
		const std::vector<ctw::LinkChoice> links = ctw_links(files, rules, stem, 2, base_seed, g, &err);
		if (links.empty()) continue;
		// a few links: the first, the middle, the last, the first sea link, and a sea variant forced on the first land link
		std::set<size_t> pick = {0, links.size() / 2, links.size() - 1};
		size_t sea = links.size(), land = links.size();
		for (size_t i = 0; i < links.size(); i++) {
			if (links[i].sea && sea == links.size()) sea = i;
			if (!links[i].sea && land == links.size()) land = i;
		}
		if (sea < links.size()) pick.insert(sea);
		std::vector<size_t> order(pick.begin(), pick.end());
		if (int(order.size()) > per_campaign) order.resize(size_t(per_campaign));
		for (size_t i : order) jobs_list.push_back({stem, int(i), -1});
		if (land < links.size()) jobs_list.push_back({stem, int(land), kStyleConquestSea});
		if (sea < links.size()) jobs_list.push_back({stem, int(sea), kStyleConquest});
	}
	cases.resize(jobs_list.size());
	std::atomic<size_t> next{0};
	std::mutex out_mutex;
	auto worker = [&]() {
		for (;;) {
			const size_t i = next.fetch_add(1);
			if (i >= jobs_list.size()) return;
			ConquestCase& c = cases[i];
			const Job& j = jobs_list[i];
			c.campaign = j.stem;
			try {
				CtwCase cc;
				std::string err;
				const std::vector<ctw::LinkChoice> links = ctw_links(files, rules, j.stem, 2, base_seed, cc.game, &err);
				if (links.empty()) throw std::runtime_error(err.empty() ? "no links" : err);
				const ctw::LinkChoice& l = links[size_t(j.link_index)];
				c.link = cc.game->node_name(l.from) + ">" + cc.game->node_name(l.to);
				if (!build_ctw_case(files, rules, cc, l, j.style, 0, &err)) {
					c.custom = err.find("custom map") != std::string::npos;
					throw std::runtime_error(err);
				}
				c.seats = cc.p.nations;
				Generated a, b;
				run_one(files, rules, cc.p, a, &cc.input);
				run_one(files, rules, cc.p, b, &cc.input);
				c.style = a.res.picked_style;
				c.seconds = std::max(a.seconds, b.seconds);
				c.ok = a.res.ok && b.res.ok;
				c.error = a.res.error.empty() ? b.res.error : a.res.error;
				if (c.ok) {
					c.starts = int(a.res.starts.size());
					c.nondeterministic = dump_text(files, rules, cc.p, a) != dump_text(files, rules, cc.p, b);
					c.gaps = water_gaps(a.world) != 0;
					for (const std::string& n : a.res.notes) c.unplaced |= n.find("nable to place") != std::string::npos || n.find("had no start") != std::string::npos;
					// every seat has its own start
					std::set<int> who;
					for (const ConquestStart& s : a.res.conquest_starts) who.insert(s.who);
					// the seat of a main country outside the square (map styles 4-7) has no start by design
					std::set<int> exempt;
					auto inside = [&](int node) { return std::find(cc.input.ids.begin(), cc.input.ids.end(), node) != cc.input.ids.end(); };
					if (!inside(cc.input.node_b)) exempt.insert(cc.input.swap ? cc.input.who_a : cc.input.who_b);
					if (!inside(cc.input.node_a)) exempt.insert(cc.input.swap ? cc.input.who_b : cc.input.who_a);
					for (int k = 0; k < cc.p.nations; k++) c.unplaced |= who.count(k) == 0 && exempt.count(k) == 0;
				}
			} catch (const std::exception& e) {
				c.error = e.what();
			}
			if (!quiet) {
				std::lock_guard<std::mutex> lock(out_mutex);
				std::printf("conquest %-22s %-28s style %d seats %d starts %d: %s%s%s%s%s %.2fs %s\n", c.campaign.c_str(), c.link.c_str(), c.style, c.seats,
				            c.starts, c.ok ? "ok" : "ERROR", c.nondeterministic ? " NONDETERMINISTIC" : "", c.unplaced ? " UNPLACED" : "",
				            c.gaps ? " WATER-GAPS" : "", c.custom ? " (custom map)" : "", c.seconds, c.error.c_str());
			}
		}
	};
	std::vector<std::thread> threads;
	for (int t = 0; t < jobs; t++) threads.emplace_back(worker);
	for (std::thread& t : threads) t.join();
	int ok = 0, bad = 0, nondet = 0, unplaced = 0, gaps = 0;
	double worst = 0;
	for (const ConquestCase& c : cases) {
		if (c.ok) ok++;
		else if (!c.custom) bad++;
		nondet += c.nondeterministic;
		unplaced += c.unplaced;
		gaps += c.gaps;
		worst = std::max(worst, c.seconds);
	}
	std::printf("\nconquest styles 24-25: %zu cases (%d links per campaign + forced variants, seed base %u): ok %d, error %d, nondeterministic %d, seats without a start %d, "
	            "ocean tcoord gaps %d; slowest %.2fs\n",
	            cases.size(), per_campaign, base_seed, ok, bad, nondet, unplaced, gaps, worst);
	return (bad || nondet || unplaced || gaps) ? 1 : 0;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr,
		             "usage: %s <game dir> --style N|NAME --size N --seed S --nations K [--teams a,b,..] [--randomize] [--town N]\n"
		             "          [--tribes a,b,..] [--png out.png] [--scale N] [--dump out.txt] [--quiet]\n"
		             "       %s <game dir> --all [--seed S] [--jobs N] [--styles a-b] [--sizes a-b] [--nations a-b] [--quiet]\n",
		             argv[0], argv[0]);
		return 2;
	}
	try {
		const GameFiles files(argv[1]);
		Rules rules = load_rules(files);
		start_game(rules, GameSetup());
		const bool quiet = has_flag(argc, argv, "--quiet");
		const char* sa = arg_value(argc, argv, "--seed");
		const uint32_t base_seed = sa ? uint32_t(std::strtoul(sa, nullptr, 0)) : 12345u;

		if (const char* cmp = arg_value(argc, argv, "--compare")) return run_compare(files, rules, cmp, argc, argv);
		if (const char* rc = arg_value(argc, argv, "--play189")) return run_play189(files, rules, rc, argc, argv);

		if (has_flag(argc, argv, "--png-check")) {
			const char* d = arg_value(argc, argv, "--png-check");
			return run_png_check(files, d && std::strncmp(d, "--", 2) != 0 ? d : nullptr, quiet);
		}

		if (const char* ctw_spec = arg_value(argc, argv, "--ctw")) {
			const std::string spec = ctw_spec;
			const size_t colon = spec.find(':');
			const std::string stem = resolve_campaign_stem(files, spec.substr(0, colon));
			if (stem.empty()) {
				std::fprintf(stderr, "no campaign '%s'\n", spec.substr(0, colon).c_str());
				return 2;
			}
			const char* da = arg_value(argc, argv, "--difficulty");
			std::string err;
			CtwCase cc;
			const std::vector<ctw::LinkChoice> links = ctw_links(files, rules, stem, da ? std::atoi(da) : 2, base_seed, cc.game, &err);
			ctw::LinkChoice link;
			if (!pick_ctw_link(*cc.game, links, colon == std::string::npos ? "auto" : spec.substr(colon + 1), &link, &err)) {
				std::fprintf(stderr, "--ctw: %s\n", err.c_str());
				return 2;
			}
			const char* st = arg_value(argc, argv, "--style");
			int style_override = -1;
			if (st) style_override = style_from_name(rules, st);
			if (!build_ctw_case(files, rules, cc, link, style_override, 0, &err)) {
				std::fprintf(stderr, "--ctw: %s\n", err.c_str());
				return 1;
			}
			if (!quiet) std::printf("%s\n", cc.desc.c_str());
			Generated g;
			run_one(files, rules, cc.p, g, &cc.input);
			if (!g.res.ok) {
				std::fprintf(stderr, "generation failed: %s\n", g.res.error.c_str());
				return 1;
			}
			if (!quiet) {
				std::printf("%s size %d %dx%d seed %u nations %d: %.3fs, %llu game_random draws, %zu goods, %zu goodies, %zu rivers, %zu starts\n",
				            style_name(rules, g.res.picked_style).c_str(), cc.p.size, g.res.xs, g.res.ys, cc.p.seed, cc.p.nations, g.seconds,
				            (unsigned long long)g.res.draws, g.goods.size(), g.items.size(), g.res.rivers.size(), g.res.starts.size());
				for (const ConquestStart& s : g.res.conquest_starts)
					std::printf("  start who=%d node=%d armies=%d at %d,%d\n", s.who, s.node, s.armies, s.x, s.y);
				for (const std::string& n : g.res.notes) std::printf("  note: %s\n", n.c_str());
			}
			if (const char* im = arg_value(argc, argv, "--image")) {  // the square draw_scenario_area drew: land black, water white, country centres red
				const ConquestInput& ci = cc.input;
				Png png(ci.side, ci.side);
				for (int y = 0; y < ci.side; y++) {
					for (int x = 0; x < ci.side; x++) {
						const uint8_t v = ci.land[size_t(y) * size_t(ci.side) + size_t(x)] ? 40 : 235;
						png.put(x, y, v, v, v);
					}
				}
				for (size_t i = 0; i < ci.ids.size(); i++) png.rect(ci.xs[i] - 1, ci.ys[i] - 1, ci.xs[i] + 2, ci.ys[i] + 2, 220, 30, 30);
				if (!png.save(im)) throw std::runtime_error(std::string("cannot write ") + im);
			}
			if (const char* d = arg_value(argc, argv, "--dump")) {
				FILE* f = std::fopen(d, "wb");
				if (!f) throw std::runtime_error(std::string("cannot write ") + d);
				const std::string t = dump_text(files, rules, cc.p, g);
				std::fwrite(t.data(), 1, t.size(), f);
				std::fclose(f);
			}
			if (const char* pn = arg_value(argc, argv, "--png")) {
				const char* sc = arg_value(argc, argv, "--scale");
				if (!write_png(pn, rules, g, sc ? std::max(1, std::atoi(sc)) : 8)) throw std::runtime_error(std::string("cannot write ") + pn);
			}
			return 0;
		}

		if (has_flag(argc, argv, "--all")) {
			int s_lo = 6, s_hi = 23, z_lo = 0, z_hi = 6, n_lo = 2, n_hi = 8;
			parse_range(arg_value(argc, argv, "--styles"), &s_lo, &s_hi);
			parse_range(arg_value(argc, argv, "--sizes"), &z_lo, &z_hi);
			parse_range(arg_value(argc, argv, "--nations"), &n_lo, &n_hi);
			const char* ja = arg_value(argc, argv, "--jobs");
			const int jobs = std::max(1, ja ? std::atoi(ja) : int(std::thread::hardware_concurrency()));
			std::vector<CaseResult> cases;
			for (int s = s_lo; s <= s_hi; s++) {
				for (int z = z_lo; z <= z_hi; z++) {
					for (int n = n_lo; n <= n_hi; n++) {
						CaseResult c;
						c.style = s;
						c.size = z;
						c.nations = n;
						cases.push_back(c);
					}
				}
			}
			std::atomic<size_t> next{0};
			std::mutex out_mutex;
			auto worker = [&]() {
				for (;;) {
					const size_t i = next.fetch_add(1);
					if (i >= cases.size()) return;
					CaseResult& c = cases[i];
					Params p;
					p.style = c.style;
					p.size = c.size;
					p.nations = c.nations;
					p.seed = base_seed + uint32_t(c.style * 7919 + c.size * 104729 + c.nations * 1299709);
					p.randomize_starts = (c.nations & 1) != 0;
					for (int k = 0; k < 8; k++) p.team[size_t(k)] = (c.nations >= 4 && k < 4) ? k / 2 : 8;
					for (int k = 0; k < 8; k++) p.tribe[size_t(k)] = (k * 5 + c.style) % 24;
					try {
						Generated a, b;
						run_one(files, rules, p, a);
						run_one(files, rules, p, b);
						c.seconds = std::max(a.seconds, b.seconds);
						c.ok = a.res.ok && b.res.ok;
						c.error = a.res.error.empty() ? b.res.error : a.res.error;
						c.fallback = a.res.fallback;
						c.slow = c.seconds >= 2.0;
						if (c.ok) c.water_gaps = water_gaps(a.world);
						if (c.ok) c.nondeterministic = dump_text(files, rules, p, a) != dump_text(files, rules, p, b);
					} catch (const std::exception& e) {
						c.crash = true;
						c.error = e.what();
					}
					if (!quiet) {
						std::lock_guard<std::mutex> lock(out_mutex);
						if (!c.ok || c.crash || c.nondeterministic || c.slow || c.water_gaps) {
							std::printf("style %2d size %d nations %d: %s%s%s%s %.2fs %s\n", c.style, c.size, c.nations,
							            c.crash ? "CRASH " : "", !c.ok && !c.crash ? "ERROR " : "", c.nondeterministic ? "NONDETERMINISTIC " : "",
							            c.slow ? "SLOW" : (c.water_gaps ? "WATER-GAPS" : ""), c.seconds, c.error.c_str());
						}
					}
				}
			};
			std::vector<std::thread> threads;
			for (int t = 0; t < jobs; t++) threads.emplace_back(worker);
			for (std::thread& t : threads) t.join();
			int ok = 0, crash = 0, nondet = 0, slow = 0, err = 0, fb = 0, gaps = 0;
			double worst = 0;
			for (const CaseResult& c : cases) {
				if (c.crash) crash++;
				else if (!c.ok) err++;
				else ok++;
				if (c.nondeterministic) nondet++;
				if (c.slow) slow++;
				if (c.water_gaps) gaps++;
				if (c.fallback) fb++;
				worst = std::max(worst, c.seconds);
			}
			std::printf("\nmatrix: styles %d-%d x sizes %d-%d x nations %d-%d = %zu cases (seed base %u)\n", s_lo, s_hi, z_lo, z_hi,
			            n_lo, n_hi, cases.size(), base_seed);
			std::printf("ok %d, error %d, crash %d, nondeterministic %d, slow(>=2s) %d, used the fallback layout %d, ocean tcoord gaps %d; slowest %.2fs\n",
			            ok, err, crash, nondet, slow, fb, gaps, worst);
			std::printf("\nper style (cases ok/total, fallback layouts, slowest seconds):\n");
			for (int s = s_lo; s <= s_hi; s++) {
				int tot = 0, good = 0, fbs = 0;
				double w2 = 0;
				for (const CaseResult& c : cases) {
					if (c.style != s) continue;
					tot++;
					if (c.ok && !c.nondeterministic && !c.slow) good++;
					if (c.fallback) fbs++;
					w2 = std::max(w2, c.seconds);
				}
				std::printf("  %2d %-22s %3d/%3d  fallback %3d  slowest %.2fs\n", s, style_name(rules, s).c_str(), good, tot, fbs, w2);
			}
			int conquest_rc = 0;
			if (!has_flag(argc, argv, "--no-conquest")) {
				std::printf("\n");
				const char* pc = arg_value(argc, argv, "--conquest-links");
				conquest_rc = run_conquest_matrix(files, rules, base_seed, jobs, quiet, pc ? std::max(1, std::atoi(pc)) : 4);
			}
			return (crash || err || nondet || slow || gaps || conquest_rc) ? 1 : 0;
		}

		Params p;
		const char* st = arg_value(argc, argv, "--style");
		p.style = st ? style_from_name(rules, st) : kStyleOldWorld;
		if (p.style < 0) {
			std::fprintf(stderr, "unknown style '%s'\n", st);
			return 2;
		}
		const char* sz = arg_value(argc, argv, "--size");
		p.size = sz ? std::atoi(sz) : 3;
		p.seed = base_seed;
		const char* na = arg_value(argc, argv, "--nations");
		p.nations = na ? std::atoi(na) : 4;
		const std::vector<int> teams = parse_list(arg_value(argc, argv, "--teams"));
		for (size_t i = 0; i < teams.size() && i < 8; i++) p.team[i] = teams[i];
		const std::vector<int> tribes = parse_list(arg_value(argc, argv, "--tribes"));
		for (size_t i = 0; i < tribes.size() && i < 8; i++) p.tribe[i] = tribes[i];
		p.randomize_starts = has_flag(argc, argv, "--randomize");
		if (const char* ta = arg_value(argc, argv, "--town")) p.starting_town = std::atoi(ta);
		Generated g;
		run_one(files, rules, p, g);
		if (!g.res.ok) {
			std::fprintf(stderr, "generation failed: %s\n", g.res.error.c_str());
			return 1;
		}
		if (!quiet) {
			std::printf("%s (picked %s) size %d %dx%d seed %u nations %d: %.3fs, %llu game_random draws, %zu goods, %zu goodies, "
			            "%zu rivers, %zu starts%s\n",
			            style_name(rules, p.style).c_str(), style_name(rules, g.res.picked_style).c_str(), p.size, g.res.xs, g.res.ys,
			            p.seed, p.nations, g.seconds, (unsigned long long)g.res.draws, g.goods.size(), g.items.size(), g.res.rivers.size(),
			            g.res.starts.size(), g.res.fallback ? " [fallback layout]" : "");
			for (const auto& ph : g.res.phases) std::printf("  %-20s %llu draws\n", ph.first.c_str(), (unsigned long long)ph.second);
			for (const std::string& n : g.res.notes) std::printf("  note: %s\n", n.c_str());
		}
		if (const char* d = arg_value(argc, argv, "--dump")) {
			FILE* f = std::fopen(d, "wb");
			if (!f) throw std::runtime_error(std::string("cannot write ") + d);
			const std::string t = dump_text(files, rules, p, g);
			std::fwrite(t.data(), 1, t.size(), f);
			std::fclose(f);
		}
		if (const char* pn = arg_value(argc, argv, "--png")) {
			const char* sc = arg_value(argc, argv, "--scale");
			if (!write_png(pn, rules, g, sc ? std::max(1, std::atoi(sc)) : 8)) throw std::runtime_error(std::string("cannot write ") + pn);
		}
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
}
