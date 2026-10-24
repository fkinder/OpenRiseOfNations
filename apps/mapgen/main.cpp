// ron-mapgen: the random-map generator on its own (engine/sim/mapgen*.cpp, docs/randommap-*.md).
//
// Usage: ron-mapgen <game dir> --style N|NAME --size N --seed S --nations K [--teams a,b,..] [--randomize]
//                   [--town N] [--tribes a,b,..] [--png out.png] [--scale N] [--dump out.txt] [--quiet]
//        ron-mapgen <game dir> --all [--seed S] [--jobs N] [--styles a-b] [--sizes a-b] [--nations a-b] [--quiet]
//
// --style takes a MapStyleIndex (0..5 pickers, 6..22 the shipped styles, 23 blank map) or a name ("Old World", "OLDWORLD").
// --png writes a top-down preview (water / land by subtype / forest / mountain / rock / river / resources / goodies /
// starts); --dump a text digest (grid hashes per layer, regions, resources, starts, empire placements, draw counts).
// --all runs every shipped style x every map size x nations 2..8, each generation twice (determinism) and timed.
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

#include "sim/mapgen.hpp"

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

void run_one(const GameFiles& files, const Rules& rules, const Params& p, Generated& g) {
	const auto t0 = std::chrono::steady_clock::now();
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

bool write_png(const std::string& path, const Rules& rules, const Generated& g, int scale) {
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
	(void)rules;
	return png.save(path);
}

// ---------------------------------------------------------------- --all

struct CaseResult {
	int style = 0, size = 0, nations = 0;
	bool ok = false, crash = false, nondeterministic = false, slow = false, fallback = false;
	double seconds = 0;
	std::string error;
};

}  // namespace

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
						if (c.ok) c.nondeterministic = dump_text(files, rules, p, a) != dump_text(files, rules, p, b);
					} catch (const std::exception& e) {
						c.crash = true;
						c.error = e.what();
					}
					if (!quiet) {
						std::lock_guard<std::mutex> lock(out_mutex);
						if (!c.ok || c.crash || c.nondeterministic || c.slow) {
							std::printf("style %2d size %d nations %d: %s%s%s%s %.2fs %s\n", c.style, c.size, c.nations,
							            c.crash ? "CRASH " : "", !c.ok && !c.crash ? "ERROR " : "", c.nondeterministic ? "NONDETERMINISTIC " : "",
							            c.slow ? "SLOW" : "", c.seconds, c.error.c_str());
						}
					}
				}
			};
			std::vector<std::thread> threads;
			for (int t = 0; t < jobs; t++) threads.emplace_back(worker);
			for (std::thread& t : threads) t.join();
			int ok = 0, crash = 0, nondet = 0, slow = 0, err = 0, fb = 0;
			double worst = 0;
			for (const CaseResult& c : cases) {
				if (c.crash) crash++;
				else if (!c.ok) err++;
				else ok++;
				if (c.nondeterministic) nondet++;
				if (c.slow) slow++;
				if (c.fallback) fb++;
				worst = std::max(worst, c.seconds);
			}
			std::printf("\nmatrix: styles %d-%d x sizes %d-%d x nations %d-%d = %zu cases (seed base %u)\n", s_lo, s_hi, z_lo, z_hi,
			            n_lo, n_hi, cases.size(), base_seed);
			std::printf("ok %d, error %d, crash %d, nondeterministic %d, slow(>=2s) %d, used the fallback layout %d; slowest %.2fs\n", ok,
			            err, crash, nondet, slow, fb, worst);
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
			return (crash || err || nondet || slow) ? 1 : 0;
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
