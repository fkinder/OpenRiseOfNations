// ron-sav: decode an original-game save (.SVX) or the save embedded in a replay (.rcx) with the generic DataWalk interpreter
// (engine/sim/datawalk.*), recompute the save's own verify block and the 15 CHECK_SUMS parts, and compare them.
// Usage: ron-sav <file.SVX|.rcx> [--summary] [--field Path] [--checksums [--csv checksums.csv]] [--verify] [--quiet]
//   (no option)  decode to the last byte, verifying every section tag
//   --summary    header, world, leaders and object counts (never prints player names)
//   --field P    one decoded value, e.g. game.frame, game.info.map_style, world.xs, leaders.list[3].score (repeatable)
//   --verify     recompute every SaveGame::verify_save Adler-32 from the decoded stream and compare
//   --checksums  compute the 15 CHECK_SUMS parts; for a replay, compare with its first CHECK_SUMS (read from the CSV written by
//                `node re/tools/rcxdump.mjs --survey ... --checksums re/oracle/out/checksums.csv`, default location, or --csv)
//   --engine-roundtrip  load the save into the engine (sim/original_save.hpp) and compare, entry by entry, the verify block and the 15
//                CHECK_SUMS parts computed over the engine's state (EngineProvider) with the save's own; every mismatch names the walk
//                function, the step and the object. Needs the game install (--game DIR, default ~/Games/RiseOfNations).
//   --replay <file.rcx> --frames N   load the replay's embedded save, run the engine's Game::do_frame N frames (feeding the replay's
//                commands through CommandPackage where the engine supports the verb), compute the 15 parts at every turn boundary and compare
//                them with the CHECK_SUMS rows of the replay: per part, the first boundary that differs. Needs node (re/tools/rcxdump.mjs) for
//                the command stream (or --json file) and the CSV of re/tools/rcxdump.mjs --checksums.
//   --step-diff N     (with a save or replay) which mapped members the engine changes in N frames from the save
//   --compare-next B  (with --replay) run to the frame of replay B's embedded save and list the members that differ from that save
//   --solve-at B      (with --replay) at boundary B: for each of units/builds/groups/guys/leaders/goods/world whose hash differs, test
//                     whether ONE changed byte explains it (Adler-32 algebra) and name the object, member and byte
//   --revert M,..     (with --replay) experiment: put members ("Unit.flags") back to the save's values after the first step
//   --field-report    how many members of each original class the engine maps (the rest stays in the shadow)
//   other options: --quiet --no-diag --mutate N (self-test) --json F --csv F --game DIR --no-art --no-commands --verbose
// Exit code: 1 if the decode fails, 2 if a recomputed value differs from the stored/expected one under --verify / --engine-roundtrip.
#include <csignal>
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <sstream>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "sim/anim_assets.hpp"
#include "sim/commands.hpp"
#include "sim/datawalk.hpp"
#include "sim/datawalk_engine.hpp"
#include "sim/fog.hpp"
#include "sim/combat.hpp"
#include "sim/orders.hpp"
#include "sim/original_save.hpp"
#include "sim/ai.hpp"
#include "sim/economy.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/pathfinder.hpp"
#include "sim/rules.hpp"
#include "sim/rules_xml.hpp"

namespace dw = ron::sim::datawalk;

namespace {

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return true;
}

// gzip (or plain) to bytes. A truncated gzip returns what was inflated.
bool maybe_gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool& was_gzip) {
	was_gzip = in.size() > 2 && in[0] == 0x1f && in[1] == 0x8b;
	if (!was_gzip) { out = in; return true; }
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

std::string basename_of(const std::string& p) {
	size_t s = p.find_last_of('/');
	return s == std::string::npos ? p : p.substr(s + 1);
}

struct Expected {
	bool found = false;
	int64_t frame = 0;
	uint32_t v[dw::kCheckPartCount] = {};
	int players_agreeing = 0, players = 0;
};

// First CHECK_SUMS of a replay from the CSV (rows: file,frame,play,units,...,all); the tuple most players agree on.
Expected first_check_sums(const std::string& csv_path, const std::string& replay_path) {
	Expected e;
	std::ifstream f(csv_path);
	if (!f) return e;
	const std::string base = basename_of(replay_path);
	std::string line;
	std::getline(f, line);
	int64_t first_frame = -1;
	std::map<std::vector<uint32_t>, int> votes;
	while (std::getline(f, line)) {
		if (line.empty() || line[0] != '"') continue;
		size_t q = line.find('"', 1);
		if (q == std::string::npos) continue;
		std::string file = line.substr(1, q - 1);
		if (basename_of(file) != base) continue;
		std::vector<std::string> cols;
		std::stringstream ss(line.substr(q + 2));
		std::string c;
		while (std::getline(ss, c, ',')) cols.push_back(c);
		if (cols.size() < 2 + dw::kCheckPartCount) continue;
		int64_t frame = std::atoll(cols[0].c_str());
		if (first_frame < 0) first_frame = frame;
		if (frame != first_frame) break;
		std::vector<uint32_t> v;
		for (int k = 0; k < dw::kCheckPartCount; ++k) v.push_back(uint32_t(std::strtoull(cols[2 + k].c_str(), nullptr, 10)));
		++votes[v];
		++e.players;
	}
	if (first_frame < 0) return e;
	auto best = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
	e.found = true;
	e.frame = first_frame;
	for (int k = 0; k < dw::kCheckPartCount; ++k) e.v[k] = best->first[size_t(k)];
	e.players_agreeing = best->second;
	return e;
}

int64_t field_int(const dw::Tree& t, const char* path) {
	dw::FieldValue v;
	std::string err;
	return dw::read_field(t, path, v, err) ? v.i : -1;
}

void print_summary(dw::Tree& tree) {
	const auto& d = tree.description();
	static const char* kGame[] = {"game.frame", "game.tick", "game.info.version", "game.info.seed", "game.info.team_style", "game.info.map_style",
		"game.info.map_size", "game.info.players", "game.info.game_speed", "game.info.game_rules", "game.info.difficulty", "game.info.starting_town",
		"game.info.scenario_type", "game.info.script_type"};
	std::printf("summary:\n");
	for (const char* p : kGame) {
		dw::FieldValue v;
		std::string err;
		if (dw::read_field(tree, p, v, err)) std::printf("  %-26s %s\n", p, v.text.c_str());
	}
	static const char* kWorld[] = {"world.xs", "world.ys", "world.size", "world.tile_size", "world.fog_size", "world.map", "world.sea_map", "world.land_size", "world.seed"};
	for (const char* p : kWorld) {
		dw::FieldValue v;
		std::string err;
		if (dw::read_field(tree, p, v, err)) std::printf("  %-26s %s\n", p, v.text.c_str());
	}
	// leaders
	for (int k = 0; k < 9; ++k) {
		char path[64];
		std::snprintf(path, sizeof path, "leaders.list[%d].leader_flags", k);
		dw::FieldValue fl;
		std::string err;
		if (!dw::read_field(tree, path, fl, err) || !(fl.i & 1)) continue;
		std::printf("  leader %d:", k);
		for (const char* m : {"tribe", "gov", "score", "city_num", "village_num"}) {
			std::snprintf(path, sizeof path, "leaders.list[%d].%s", k, m);
			dw::FieldValue v;
			if (dw::read_field(tree, path, v, err)) std::printf(" %s=%s", m, v.text.c_str());
		}
		std::printf("\n");
	}
	// objects per list: alive units / builds / walls / animals by data type
	dw::TreeProvider prov(tree);
	int fn = d.find_function("MultiPtrArray<Object>::walk_data");
	int step = d.find_step(fn, dw::StepKind::Container);
	dw::Obj objects = prov.global(d.find_global("objects"));
	prov.begin_walk();
	for (int who = 0; who < 9; ++who) {
		dw::Site site{step, int64_t(who) * 0x1c + 4};
		dw::ContainerHeader h;
		if (!prov.container(objects, site, h)) continue;
		int64_t cnt[4] = {0, 0, 0, 0}, dead = 0;
		for (int64_t i = 0; i < h.length; ++i) {
			if (!prov.present(objects, site, i)) { ++dead; continue; }
			dw::Obj e = prov.element(objects, site, i);
			int t = prov.data_type(objects, site, i);
			if (e && (*prov.image(e, 8, 1) & 1) && t >= 0 && t < 4) ++cnt[t]; else ++dead;
		}
		if (cnt[0] + cnt[1] + cnt[2] + cnt[3])
			std::printf("  objects[%d]: units %lld builds %lld walls %lld animals %lld (empty or dead slots %lld)\n", who, (long long)cnt[0], (long long)cnt[1],
				(long long)cnt[2], (long long)cnt[3], (long long)dead);
	}
}

struct Loaded {
	std::vector<uint8_t> data;
	bool gz = false;
	bool replay = false;
	dw::Tree* header = nullptr;   // replay header (Game::walk_data)
	size_t save_pos = 0;
};


// ------------------------------------------------------------------------------------------------ engine round trip

namespace sim = ron::sim;

// Wraps a Provider and records (or compares) a fingerprint of everything it returns, in call order. Two runs of the same
// walk over two providers of the same state give the same log; the first difference names the object and member.
class LogProvider : public dw::Provider {
public:
	struct Rec {
		uint8_t kind;
		int step;
		int64_t a, b;
		uint64_t h;
		const void* obj;
	};
	LogProvider(dw::Provider& inner, std::vector<Rec>* record, const std::vector<Rec>* expect)
		: in_(inner), record_(record), expect_(expect) {}
	bool diverged = false;
	size_t at = 0;
	Rec got{}, want{};
	std::vector<uint8_t> got_bytes;

	void begin_walk() override { in_.begin_walk(); }
	dw::Obj global(int gid) override { return in_.global(gid); }
	const uint8_t* image(dw::Obj o, int64_t off, uint32_t n) override {
		const uint8_t* p = in_.image(o, off, n);
		log(Rec{1, cur_step_, off, n, fnv(p, n), o}, p, n);
		return p;
	}
	dw::Obj child(dw::Obj o, const dw::Site& site, int64_t index) override {
		cur_step_ = site.step;
		dw::Obj c = in_.child(o, site, index);
		log(Rec{2, site.step, site.base, index, c ? 1u : 0u, o}, nullptr, 0);
		return c;
	}
	bool local(dw::Obj o, const dw::Site& site, int64_t& value) override {
		cur_step_ = site.step;
		bool ok = in_.local(o, site, value);
		log(Rec{3, site.step, site.base, ok, uint64_t(value), o}, nullptr, 0);
		return ok;
	}
	bool container(dw::Obj o, const dw::Site& site, dw::ContainerHeader& hdr) override {
		cur_step_ = site.step;
		bool ok = in_.container(o, site, hdr);
		uint64_t h = fnv(reinterpret_cast<const uint8_t*>(&hdr.length), 4) ^ (uint64_t(hdr.size) << 20) ^ (uint64_t(uint16_t(hdr.increment)) << 40) ^ (uint64_t(hdr.flags) << 56);
		log(Rec{4, site.step, site.base, ok, h, o}, nullptr, 0);
		return ok;
	}
	bool present(dw::Obj o, const dw::Site& site, int64_t i) override {
		bool v = in_.present(o, site, i);
		log(Rec{5, site.step, site.base, i, v, o}, nullptr, 0);
		return v;
	}
	int32_t data_type(dw::Obj o, const dw::Site& site, int64_t i) override {
		int32_t v = in_.data_type(o, site, i);
		log(Rec{6, site.step, site.base, i, uint64_t(uint32_t(v)), o}, nullptr, 0);
		return v;
	}
	dw::Obj element(dw::Obj o, const dw::Site& site, int64_t i) override {
		dw::Obj e = in_.element(o, site, i);
		log(Rec{7, site.step, site.base, i, uint64_t(reinterpret_cast<uintptr_t>(e)), o}, nullptr, 0);
		return e;
	}
	const uint8_t* bytes(dw::Obj o, const dw::Site& site, uint64_t n) override {
		cur_step_ = site.step;
		const uint8_t* p = in_.bytes(o, site, n);
		log(Rec{8, site.step, site.base, int64_t(n), p ? fnv(p, size_t(n)) : 0, o}, p, size_t(n));
		return p;
	}
	const uint8_t* string(dw::Obj o, const dw::Site& site, uint32_t& len) override {
		cur_step_ = site.step;
		const uint8_t* p = in_.string(o, site, len);
		log(Rec{9, site.step, site.base, len, p ? fnv(p, 2ull * len) : 0, o}, nullptr, 0);
		return p;
	}

private:
	static uint64_t fnv(const uint8_t* p, size_t n) {
		uint64_t h = 1469598103934665603ull;
		for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
		return h;
	}
	void log(const Rec& r, const uint8_t* bytes, size_t n) {
		if (record_) record_->push_back(r);
		if (expect_ && !diverged) {
			if (at >= expect_->size()) { diverged = true; got = r; want = Rec{}; }
			else {
				const Rec& e = (*expect_)[at];
				if (e.kind != r.kind || e.h != r.h || e.a != r.a || e.b != r.b) {
					diverged = true;
					got = r;
					want = e;
					if (bytes) got_bytes.assign(bytes, bytes + n);
				}
			}
			++at;
		}
	}
	dw::Provider& in_;
	std::vector<Rec>* record_;
	const std::vector<Rec>* expect_;
	int cur_step_ = -1;
};

std::string describe_place(sim::Game& g, const void* obj, int64_t off, int step) {
	const dw::Description& d = dw::Description::get();
	std::string out;
	const dw::Rec* rec = static_cast<const dw::Rec*>(obj);
	char b[256];
	if (step >= 0) {
		std::snprintf(b, sizeof b, "walk %s step %d (kind %d)", d.function_name(d.step_function(step)), step, int(d.step_kind(step)));
		out += b;
	}
	if (!rec) return out;
	auto it = g.original->segs.find(rec);
	if (it != g.original->segs.end()) {
		for (const sim::omap::Segment& s : it->second) {
			const int64_t lo = off - s.base;
			if (lo < 0 || (s.len >= 0 && lo >= s.len)) continue;
			std::snprintf(b, sizeof b, "; object %s(%d,%d,%d)", s.map->name.c_str(), s.id.a, s.id.b, s.id.c);
			out += b;
			const int n = d.field_count(s.map->cls.c_str());
			for (int i = 0; i < n; ++i) {
				const dw::FieldInfo* f = d.field_at(s.map->cls.c_str(), i);
				if (lo >= f->offset && lo < f->offset + f->size) {
					std::snprintf(b, sizeof b, " member %s.%s (+0x%x, offset 0x%llx into the image)", s.map->cls.c_str(), f->name, f->offset, (unsigned long long)lo);
					out += b;
					break;
				}
			}
			break;
		}
	} else {
		std::snprintf(b, sizeof b, "; image of class '%s'%s%s at offset 0x%llx (shadow, unbound)", rec->cls, rec->gid >= 0 ? " global " : "", rec->gid >= 0 ? d.global_name(rec->gid) : "", (unsigned long long)off);
		out += b;
	}
	return out;
}

// Runs `fn` over the tree provider (recording) and over the engine provider (comparing); describes the first divergence.
std::string diff_runs(sim::Game& g, const std::function<void(dw::Provider&)>& fn) {
	std::vector<LogProvider::Rec> log;
	{
		LogProvider lp(g.original->tree_provider(), &log, nullptr);
		fn(lp);
	}
	sim::EngineProvider ep(g);
	LogProvider lp(ep, nullptr, &log);
	fn(lp);
	if (!lp.diverged) return "no divergence found in the provider calls";
	char b[512];
	std::string where = describe_place(g, lp.got.obj, lp.got.kind == 1 ? lp.got.a : 0, lp.got.step);
	std::snprintf(b, sizeof b, "call %zu kind %d: engine h=%016llx expected h=%016llx; %s", lp.at - 1, lp.got.kind, (unsigned long long)lp.got.h, (unsigned long long)lp.want.h, where.c_str());
	std::string out = b;
	if (lp.got.kind == 1) {
		// the first differing byte of an image request
		const dw::Rec* rec = static_cast<const dw::Rec*>(lp.got.obj);
		std::vector<uint8_t> want(size_t(lp.got.b));
		rec->read(lp.got.a, want.data(), want.size());
		for (size_t i = 0; i < want.size() && i < lp.got_bytes.size(); ++i) {
			if (want[i] != lp.got_bytes[i]) {
				std::snprintf(b, sizeof b, " [first differing byte +%zu: engine 0x%02x, original 0x%02x; %s]", i, lp.got_bytes[i], want[i],
					describe_place(g, lp.got.obj, lp.got.a + int64_t(i), -1).c_str());
				out += b;
				break;
			}
		}
	}
	return out;
}

std::string game_dir_default() {
	const char* home = std::getenv("HOME");
	return std::string(home ? home : "") + "/Games/RiseOfNations";
}

// Rules per setup, loaded once from the install.
struct RulesCache {
	explicit RulesCache(const std::string& dir) : files(dir) {}
	ron::GameFiles files;
	std::unique_ptr<sim::Rules> base;
	std::map<std::string, std::unique_ptr<sim::Rules>> by_setup;
	const sim::Rules& get(const sim::GameSetup& s) {
		if (!base) base = std::make_unique<sim::Rules>(sim::load_rules(files));
		char key[96];
		std::snprintf(key, sizeof key, "%d/%d/%d/%d/%d", s.starting_technology, s.starting_technology2, s.ending_technology, s.game_rules, int(s.flag8));
		auto it = by_setup.find(key);
		if (it != by_setup.end()) return *it->second;
		auto r = std::make_unique<sim::Rules>(*base);
		sim::start_game(*r, s);
		return *by_setup.emplace(key, std::move(r)).first->second;
	}
};

// --astar-check: re-runs PathFinder::find_upath for the units of a save whose path stack ends in a fresh U-level path (entries with the
// "fine" flag 2 on top of a coarser goal entry) while the unit stands on a cell centre (the snap of a repath), and compares the result
// with the stack the original left. A repath snaps the unit to the centre of its UCoord and then waits `pause` frames, so for those
// units the surroundings are still the ones the original searched in. The truth for the A* port: no other source of U-level paths.
int astar_check(const std::string& path, const std::vector<uint8_t>& save, const std::string& game_dir, bool verbose) {
	dw::Tree peek(dw::Description::get());
	dw::SaveDecode psd = dw::decode_save(peek, save.data(), save.size(), 0);
	if (!psd.result.ok) { std::printf("%s: decode failed: %s\n", basename_of(path).c_str(), psd.result.error.c_str()); return 1; }
	sim::GameSetup setup;
	setup.starting_technology = int32_t(field_int(peek, "game.info.starting_technology"));
	setup.starting_technology2 = int32_t(field_int(peek, "game.info.starting_technology2"));
	setup.ending_technology = int32_t(field_int(peek, "game.info.ending_technology"));
	setup.game_rules = int32_t(field_int(peek, "game.info.game_rules"));
	setup.flag8 = (field_int(peek, "game.info.flags") & 8) != 0;
	static std::unique_ptr<RulesCache> cache;
	if (!cache) cache = std::make_unique<RulesCache>(game_dir);
	const sim::Rules& rules = cache->get(setup);
	sim::OriginalLoadReport rep;
	std::unique_ptr<sim::Game> g0 = sim::load_original_save(cache->files, rules, nullptr, save, &rep);
	if (!g0) { std::printf("%s: LOAD FAILED: %s\n", basename_of(path).c_str(), rep.error.c_str()); return 1; }
	struct Cand { int who, o, k; };
	std::vector<Cand> cands;
	for (int who = 0; who < 8; ++who)
		for (int o = 0; o < g0->objects[size_t(who)].unit_mark; ++o) {
			const sim::Unit* u = g0->unit(who, o);
			if (!u || !u->alive() || !u->on_map() || u->path.size() < 3) continue;
			if (u->x % 48 != 24 || u->y % 48 != 24) continue;
			if (!std::getenv("RON_ASTAR_ANY") && u->collide_frame != g0->frame - 1) continue;  // repathed in the frame just before the save: no step taken since
			int k = int(u->path.size()) - 1;
			while (k >= 0 && (u->path[size_t(k)].flags & 2)) --k;
			const int fine = int(u->path.size()) - 1 - k;
			if (k < 0 || fine < 2) continue;
			cands.push_back({who, o, k});
		}
	int match = 0, differ = 0, fail = 0;
	const int rps[] = {-1, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 24};  // -1: the value in the save
	for (const Cand& c : cands) {
		bool done = false;
		std::vector<sim::PathData> last_st, last_want;
		int last_r = 0, near = 0, movers_near = 0;
		for (int fast = 0; fast < 2 && !done; ++fast)
			for (int rp : rps) {
				std::unique_ptr<sim::Game> g = sim::load_original_save(cache->files, rules, nullptr, save, &rep);
				if (!g) { ++fail; done = true; break; }
				const sim::Unit* u = g->unit(c.who, c.o);
				if (std::getenv("RON_ASTAR_REWIND")) {
					// The search ran inside frame F-1 (F = the save's frame): units processed after the searcher in that frame (player rotation
					// (F-1+k)%10, then index) had not moved yet; put them back at the position they started the frame from (guy 0's last_x/last_y).
					const int F1 = g->frame - 1;
					auto key = [&](int w2, int o2) { return ((w2 - F1) % 10 + 10) % 10 * 100000 + o2; };
					const int ukey = key(c.who, c.o);
					for (int w2 = 0; w2 < 10; ++w2)
						for (int o2 = 0; o2 < g->objects[size_t(w2)].unit_mark; ++o2) {
							sim::Unit* v = g->unit(w2, o2);
							if (!v || !v->alive() || !v->on_map() || v->guys.empty() || key(w2, o2) <= ukey) continue;
							const sim::Guy& g0 = v->guys[0];
							if (g0.last_x == g0.x && g0.last_y == g0.y) continue;
							sim::unit_set_new_location(*g, *v, g0.last_x, g0.last_y, true, false);
						}
				}
				const std::vector<sim::PathData> want = u->path;
				// (the search limit is 500 / repaths^2 and the save holds the counter at the end of the frame, after later repaths and the next
				// halving: try the saved value and a range of smaller ones)
				if (rp >= 0) g->daemon.repaths[size_t(c.who)] = rp;
				std::vector<sim::PathData> st(want.begin(), want.begin() + std::ptrdiff_t(c.k) + 1);
				const int32_t r = sim::find_upath(*g, st, c.who, c.o, fast != 0);
				bool same = r > 0 && st.size() == want.size();
				for (size_t i = 0; same && i < st.size(); ++i) same = st[i].x == want[i].x && st[i].y == want[i].y && st[i].tol == want[i].tol && st[i].flags == want[i].flags;
				if (std::getenv("RON_ASTAR_DETAIL") && std::getenv("RON_ASTAR_DETAIL")[0] == '3' && rp == 2 && fast == 0) {
					// the validity of the cells around the unit as the A* sees them: collision (pathing) and invalid_loc
					const sim::Unit& uu = *u;
					const int cx = sim::ucoord_of(uu.x), cy = sim::ucoord_of(uu.y), rr = uu.ptype->new_block_radius;
					std::printf("      unit cell (%d,%d) radius %d\n", cx, cy, rr);
					for (int k = 1; k <= 3; ++k) {
						int hx = -1, hy = -1;
						const int hit = sim::collide_here(*g, uu, cx + k, cy + k, rr, &hx, &hy, true);
						std::printf("      probe (%d,%d): hit %d at (%d,%d)\n", cx + k, cy + k, hit, hx, hy);
					}
					for (int w2 = 0; w2 < 9; ++w2)
						for (int o2 = 0; o2 < g->objects[size_t(w2)].unit_mark; ++o2) {
							const sim::Unit* v = g->unit(w2, o2);
							if (!v || !v->alive() || !v->on_map() || (w2 == c.who && o2 == c.o)) continue;
							if (std::abs(sim::ucoord_of(v->x) - cx) > 8 || std::abs(sim::ucoord_of(v->y) - cy) > 8) continue;
							std::printf("      nearby unit %d/%d type %d radius %d at (%d,%d) cell, guy0 last (%d,%d), orders %zu front %d, avg_speed %d, flags %x masks %x\n", w2, o2, v->type, v->ptype->new_block_radius,
								sim::ucoord_of(v->x), sim::ucoord_of(v->y), v->guys.empty() ? -1 : sim::ucoord_of(v->guys[0].last_x), v->guys.empty() ? -1 : sim::ucoord_of(v->guys[0].last_y), v->orders.size(), v->orders.empty() ? -1 : int(v->orders.front().type), v->guys.empty() ? -1 : v->guys[0].avg_speed, v->flags, v->unit_masks);
						}
					std::printf("      set collision bits around (x %d..%d, y %d..%d):\n", cx - 4, cx + 8, cy - 3, cy + 6);
					for (int yy = cy - 3; yy <= cy + 6; ++yy) {
						std::printf("        %4d ", yy);
						for (int xx = cx - 4; xx <= cx + 8; ++xx) std::printf("%c", g->world.coll_get(xx, yy) ? '#' : '.');
						std::printf("\n");
					}
					for (int dy = -1; dy <= 6; ++dy) {
						std::printf("      row %d:", cy + dy);
						for (int dx = -1; dx <= 8; ++dx) {
							int hx = 0, hy = 0;
							const int hit = sim::collide_here(*g, uu, cx + dx, cy + dy, rr, &hx, &hy, true);
							const int inv = sim::unit_invalid_loc(*g, uu, (cx + dx) >> 2, (cy + dy) >> 2, false, true, false, true, false);
							std::printf(" %c", inv ? 'T' : hit ? 'X' : '.');
						}
						std::printf("\n");
					}
				}
				if (std::getenv("RON_ASTAR_DETAIL")) {
					size_t fd = 0;
					while (fd < st.size() && fd < want.size() && st[fd].x == want[fd].x && st[fd].y == want[fd].y && st[fd].flags == want[fd].flags) ++fd;
					std::printf("    attempt unit %d/%d fast %d repaths %d: result %d, %zu entries (original %zu), first difference at entry %zu (k = %d)\n", c.who, c.o, fast, rp, r, st.size(), want.size(), fd, c.k);
					if (std::getenv("RON_ASTAR_DETAIL")[0] == '2' && rp == 2 && fast == 0) {
						std::printf("      original:");
						for (size_t i = c.k; i < want.size(); ++i) std::printf(" (%d,%d)", want[i].x / 48, want[i].y / 48);
						std::printf("\n      engine:  ");
						for (size_t i = c.k; i < st.size(); ++i) std::printf(" (%d,%d)", st[i].x / 48, st[i].y / 48);
						std::printf("\n      unit at (%d,%d)\n", u->x / 48, u->y / 48);
					}
				}
				near = movers_near = 0;
				for (int w2 = 0; w2 < 9; ++w2)
					for (int o2 = 0; o2 < g->objects[size_t(w2)].unit_mark; ++o2) {
						const sim::Unit* v = g->unit(w2, o2);
						if (!v || !v->alive() || !v->on_map() || (w2 == c.who && o2 == c.o)) continue;
						if (std::abs(v->x - u->x) > 576 || std::abs(v->y - u->y) > 576) continue;
						++near;
						if (!v->guys.empty() && v->guys[0].avg_speed != 0) ++movers_near;
					}
				if (same) {
					++match;
					if (verbose) std::printf("  unit %d/%d: path of %zu entries reproduced (fast %d, repaths %d); type %d radius %d; %d units within 12 cells, %d of them moving\n", c.who, c.o, want.size(), fast, rp, u->type, u->ptype->new_block_radius, near, movers_near);
					done = true;
					break;
				}
				last_st = st;
				last_want = want;
				last_r = r;
			}
		if (!done) {
			++differ;
			const sim::Unit* u = g0->unit(c.who, c.o);
			std::printf("  unit %d/%d at %d,%d (type %d): path DIFFERS (original %zu entries, engine %zu, result %d); radius %d; %d units within 12 cells, %d of them moving\n    original:", c.who, c.o, u->x, u->y, u->type, last_want.size(), last_st.size(), last_r, u->ptype->new_block_radius, near, movers_near);
			for (size_t i = c.k; i < last_want.size(); ++i) std::printf(" (%d,%d t%d f%x)", last_want[i].x, last_want[i].y, last_want[i].tol, last_want[i].flags);
			std::printf("\n    engine:  ");
			for (size_t i = c.k; i < last_st.size(); ++i) std::printf(" (%d,%d t%d f%x)", last_st[i].x, last_st[i].y, last_st[i].tol, last_st[i].flags);
			std::printf("\n");
		}
	}
	std::printf("%s: astar check at frame %d: %zu candidate units, %d reproduced, %d differ%s\n", basename_of(path).c_str(), g0->frame, cands.size(), match, differ, fail ? " (load failures)" : "");
	return 0;
}

int engine_roundtrip(const std::string& path, const std::vector<uint8_t>& save, const std::string& game_dir, bool quiet, bool diag, int mutate) {
	// The game setup the rules depend on, read from the save's GameInfo.
	dw::Tree peek(dw::Description::get());
	dw::SaveDecode psd = dw::decode_save(peek, save.data(), save.size(), 0);
	if (!psd.result.ok) { std::printf("%s: decode failed: %s\n", basename_of(path).c_str(), psd.result.error.c_str()); return 1; }
	sim::GameSetup setup;
	setup.starting_technology = int32_t(field_int(peek, "game.info.starting_technology"));
	setup.starting_technology2 = int32_t(field_int(peek, "game.info.starting_technology2"));
	setup.ending_technology = int32_t(field_int(peek, "game.info.ending_technology"));
	setup.game_rules = int32_t(field_int(peek, "game.info.game_rules"));
	setup.flag8 = (field_int(peek, "game.info.flags") & 8) != 0;
	static std::unique_ptr<RulesCache> cache;
	if (!cache) cache = std::make_unique<RulesCache>(game_dir);
	const sim::Rules& rules = cache->get(setup);

	sim::OriginalLoadReport rep;
	std::unique_ptr<sim::Game> game = sim::load_original_save(cache->files, rules, nullptr, save, &rep);
	if (!game) { std::printf("%s: LOAD FAILED: %s\n", basename_of(path).c_str(), rep.error.c_str()); return 1; }
	std::printf("%s: engine round trip: loaded frame %d (%lld cells)\n", basename_of(path).c_str(), game->frame, (long long)rep.cells);
	for (const std::string& n : rep.notes) std::printf("  note: %s\n", n.c_str());
	if (!rep.rules_note.empty()) std::printf("  rules: %s\n", rep.rules_note.c_str());

	if (mutate) {
		// Self-test: change engine members and expect the corresponding checks to fail (proves they are emitted from the engine).
		if (mutate == 1) game->frame += 1;
		if (mutate == 2) game->world.wdata[size_t(game->world.size / 2)].flags ^= 4;
		if (mutate == 3) { for (auto& u : game->objects[0].units) if (u && u->alive()) { u->x += 1; break; } }
		if (mutate == 4) { for (auto& b : game->objects[0].builds) if (b && b->alive()) { b->myhits += 1; break; } }
		if (mutate == 6) { for (auto& u : game->objects[0].units) if (u && u->alive() && !u->guys.empty()) { u->guys[0].x += 1; break; } }
		if (mutate == 7) { for (auto& u : game->objects[0].units) if (u && u->alive() && !u->path.empty()) { u->path[0].x += 1; break; } }
		if (mutate == 8) game->leaders[0].bucket[0] += 1;
		if (mutate == 9) game->leaders[0].score += 1;
		if (mutate == 10) { for (auto& u : game->objects[0].units) if (u && u->alive() && !u->guys.empty()) { u->guys.pop_back(); break; } }
		if (mutate == 11) { for (auto& u : game->objects[0].units) if (u && u->alive() && !u->orders.empty() && u->orders.front().type == 1) { u->orders.front().x += 1; break; } }
		if (mutate == 12) { for (auto& u : game->objects[0].units) if (u && u->alive() && u->orders.size() > 1) { u->orders.pop_back(); break; } }
		if (mutate == 13) { for (auto& u : game->objects[0].units) if (u && u->alive() && !u->orders.empty() && u->orders.front().type == 7) { u->orders.front().type = sim::kMoveTo; break; } }
		if (mutate == 14) { for (auto& c : game->cities[0]) if (c.valid()) { c.pop += 1; break; } }
		if (mutate == 15) { for (auto& gr : game->groups.g) if (gr.num > 0) { gr.list[0] ^= 1; break; } }
		if (mutate == 16) { if (!game->goods.empty()) game->goods[0].x += 1; }
		if (mutate == 17) { for (auto& b : game->objects[0].builds) if (b && b->alive() && b->build_queue.queue_size > 0) { b->build_queue.queue[0].job_counter += 1; break; } }
		if (mutate == 5) { for (auto& u : game->objects[0].units) if (u && u->alive() && u->ptype && !(u->type >= 50 && u->type <= 53)) { u->cara ^= 1; break; } }
	}
	int audit_bad = 0, mapped_members = 0;
	{
		std::vector<sim::AuditLine> audit = sim::audit_mapping(*game);
		int nbad = 0;
		for (const sim::AuditLine& l : audit) {
			if (!l.mismatches) continue;
			++nbad;
			std::printf("  audit MISMATCH %-34s %lld of %lld objects: %s\n", l.member.c_str(), (long long)l.mismatches, (long long)l.objects, l.example.c_str());
		}
		std::printf("  audit: %zu mapped members, %d differ from the original bytes\n", audit.size(), nbad);
		audit_bad = nbad;
		mapped_members = int(audit.size());
	}
	int bad = audit_bad;
	// The verify block: engine provider against the stored values.
	sim::EngineProvider ep(*game);
	std::vector<dw::VerifyEntry> es = dw::compute_verify(ep, game->original->verify);
	std::map<std::string, std::pair<int, int>> byname;  // name -> (entries, mismatches)
	std::vector<std::pair<std::string, int>> first_bad;
	std::set<std::string> seen_bad;
	for (const auto& e : es) {
		auto& c = byname[e.name];
		++c.first;
		if (!e.match) {
			++c.second;
			if (seen_bad.insert(e.name).second) first_bad.push_back({e.name, e.index});
		}
	}
	int vbad = 0;
	for (const auto& kv : byname) vbad += kv.second.second;
	std::printf("  verify: %zu entries, %d mismatch\n", es.size(), vbad);
	bad += vbad;
	for (const auto& fb : first_bad) {
		const auto& c = byname[fb.first];
		std::printf("    MISMATCH %-16s %d of %d entries differ (first: index %d)\n", fb.first.c_str(), c.second, c.first, fb.second);
		if (diag) {
			std::vector<dw::LocalValue> one;
			for (int k = 0; k <= fb.second; ++k) one.push_back(dw::LocalValue{nullptr, 0});
			// the stored list entries of that name, in order, up to the failing index
			one.clear();
			int n = 0;
			for (const dw::LocalValue& lv : game->original->verify) if (fb.first == lv.name && n++ <= fb.second) one.push_back(lv);
			std::string why = diff_runs(*game, [&](dw::Provider& p) { dw::compute_verify(p, one); });
			std::printf("      first difference: %s\n", why.c_str());
		}
	}
	// The 15 parts: engine provider against the tree provider.
	dw::TreeProvider& tp = game->original->tree_provider();
	dw::CheckSumParts want = dw::compute_check_sums(tp);
	sim::EngineProvider ep2(*game);
	dw::CheckSumParts got = dw::compute_check_sums(ep2);
	if (!want.ok || !got.ok) { std::printf("  parts: walk failed: %s\n", (want.ok ? got.error : want.error).c_str()); return 1; }
	int pbad = 0;
	for (int k = 0; k < dw::kCheckPartCount; ++k) {
		const bool m = want.v[k] == got.v[k];
		pbad += !m;
		if (!m || !quiet) std::printf("  part %-16s engine %10u  original %10u  %s\n", dw::kCheckPartNames[k], got.v[k], want.v[k], m ? "match" : "MISMATCH");
	}
	bad += pbad;
	if (pbad && diag) {
		std::string why = diff_runs(*game, [&](dw::Provider& p) { dw::compute_check_sums(p); });
		std::printf("      first difference over the 15 parts: %s\n", why.c_str());
	}
	std::printf("  result: %s (%d verify mismatches, %d of 15 parts differ, %d of %d mapped members differ)\n", bad ? "FAIL" : "PASS", vbad, pbad, audit_bad, mapped_members);
	return bad ? 2 : 0;
}


// ------------------------------------------------------------------------------------------------ replay stepping

// A minimal JSON value (the output of `rcxdump --json`).
struct Json {
	enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
	double n = 0;
	std::string s;
	std::vector<Json> a;
	std::vector<std::pair<std::string, Json>> o;
	const Json* get(const char* k) const {
		for (const auto& kv : o) if (kv.first == k) return &kv.second;
		return nullptr;
	}
	int64_t num(const char* k, int64_t def = 0) const {
		const Json* j = get(k);
		return j && j->t == Num ? int64_t(j->n) : def;
	}
};

struct JsonParser {
	const std::string& b;
	size_t p = 0;
	explicit JsonParser(const std::string& s) : b(s) {}
	void ws() { while (p < b.size() && (b[p] == ' ' || b[p] == '\n' || b[p] == '\r' || b[p] == '\t')) ++p; }
	bool parse(Json& j) {
		ws();
		if (p >= b.size()) return false;
		const char c = b[p];
		if (c == '{') {
			j.t = Json::Obj;
			++p;
			ws();
			if (b[p] == '}') { ++p; return true; }
			for (;;) {
				ws();
				Json k;
				if (!parse(k) || k.t != Json::Str) return false;
				ws();
				if (b[p++] != ':') return false;
				Json v;
				if (!parse(v)) return false;
				j.o.emplace_back(std::move(k.s), std::move(v));
				ws();
				if (b[p] == ',') { ++p; continue; }
				if (b[p] == '}') { ++p; return true; }
				return false;
			}
		}
		if (c == '[') {
			j.t = Json::Arr;
			++p;
			ws();
			if (b[p] == ']') { ++p; return true; }
			for (;;) {
				Json v;
				if (!parse(v)) return false;
				j.a.push_back(std::move(v));
				ws();
				if (b[p] == ',') { ++p; continue; }
				if (b[p] == ']') { ++p; return true; }
				return false;
			}
		}
		if (c == '"') {
			j.t = Json::Str;
			++p;
			while (p < b.size() && b[p] != '"') {
				if (b[p] == '\\' && p + 1 < b.size()) { ++p; j.s.push_back(b[p] == 'n' ? '\n' : b[p]); if (b[p] == 'u') p += 4; }
				else j.s.push_back(b[p]);
				++p;
			}
			++p;
			return true;
		}
		if (!std::strncmp(b.c_str() + p, "true", 4)) { j.t = Json::Bool; j.n = 1; p += 4; return true; }
		if (!std::strncmp(b.c_str() + p, "false", 5)) { j.t = Json::Bool; p += 5; return true; }
		if (!std::strncmp(b.c_str() + p, "null", 4)) { p += 4; return true; }
		char* end = nullptr;
		j.n = std::strtod(b.c_str() + p, &end);
		if (end == b.c_str() + p) return false;
		j.t = Json::Num;
		p = size_t(end - b.c_str());
		return true;
	}
};

// An argument that rcxdump prints as a name ("NEW", "MOVE_TO") or as a number.
int64_t json_enum(const Json& args, const char* key, const std::map<std::string, int64_t>& names, int64_t def) {
	const Json* j = args.get(key);
	if (!j) return def;
	if (j->t == Json::Num) return int64_t(j->n);
	auto it = names.find(j->s);
	return it == names.end() ? def : it->second;
}


// ---- hash tracing: the bytes a CHECK_SUMS part hashes, labelled, and a solver for a small difference of two hashes ----------

int64_t g_solve_at = -1;  // --solve-at B: at that boundary name the changed byte of every part that differs by one byte

struct PartTrace {
	struct R { size_t begin, len; int who, o, step; };
	std::vector<uint8_t> bytes;
	std::vector<R> ranges;
};

// Runs part `part` (units 0, builds 1, groups 5, guys 6, leaders 7, goods 10, world 11) with a hook that records every hashed range.
bool trace_part(dw::Provider& p, int part, PartTrace& out) {
	const dw::Description& d = dw::Description::get();
	dw::ChecksumWalker w(d, p);
	int cur_who = -1, cur_o = -1;
	w.set_hook([&](int step, const uint8_t* b, size_t n) {
		out.ranges.push_back({out.bytes.size(), n, cur_who, cur_o, step});
		out.bytes.insert(out.bytes.end(), b, b + n);
	});
	auto gid = [&](const char* n) { return d.find_global(n); };
	auto i32 = [&](dw::Obj o, int64_t off) { const uint8_t* b = p.image(o, off, 4); return int32_t(b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24); };
	dw::Obj objects = p.global(gid("objects")), leaders = p.global(gid("leaders")), obj_base = p.global(gid("obj_base"));
	const int off_valid = d.find_field("Objects", "valid")->offset, off_unit = d.find_field("Objects", "unit_mark")->offset,
	          off_build = d.find_field("Objects", "build_mark")->offset;
	const int fn_multi = d.find_function("MultiPtrArray<Object>::walk_data");
	const int step_lists = d.find_step(fn_multi, dw::StepKind::Container);
	auto active = [&](int who) { return (i32(leaders, int64_t(who) * 0x6eec) & 1) != 0; };
	w.reset();
	if (part == 0 || part == 1 || part == 6) {
		if (!i32(objects, off_valid)) return true;
		const int32_t lo = i32(obj_base, part == 1 ? 4 : 0);
		const int fn_guys = d.find_function("PtrArray<Guy>::walk_data");
		for (int who = 0; who < (part == 1 ? 8 : 9); ++who) {
			if (!active(who)) continue;
			const int32_t hi = i32(objects, (part == 1 ? off_build : off_unit) + 4 * who);
			dw::Site site{step_lists, int64_t(who) * 0x1c + 4};
			dw::ContainerHeader h;
			if (!p.container(objects, site, h)) return false;
			for (int64_t o = lo; o < std::min<int64_t>(hi, h.length); ++o) {
				if (!p.present(objects, site, o)) continue;
				dw::Obj e = p.element(objects, site, o);
				if (!e || !(*p.image(e, 8, 1) & 1)) continue;
				cur_who = who;
				cur_o = int(o);
				if (part == 6) w.walk(fn_guys, e, -1, 0xe4);
				else w.walk(d.dispatch_function(step_lists, p.data_type(objects, site, o)), e, -1, 0);
			}
		}
		return true;
	}
	if (part == 7) {
		const int fn = d.find_function("LeaderData::walk_data");
		for (int who = 0; who < 8; ++who) { cur_who = who; w.walk(fn, leaders, gid("leaders"), int64_t(who) * 0x6eec); }
		return true;
	}
	if (part == 5) {
		dw::Obj groups = p.global(gid("groups"));
		const int fn = d.find_function("Group::walk_data");
		dw::Site site{d.find_step(d.find_function("Array<Group>::walk_data"), dw::StepKind::Container, -1, 0), 0};
		dw::ContainerHeader h;
		if (!p.container(groups, site, h)) return false;
		for (int64_t i = 0; i < h.length; ++i) {
			dw::Obj e = p.element(groups, site, i);
			if (!e) continue;
			cur_o = int(i);
			w.walk(fn, e, -1, 0);
		}
		cur_o = -1;
		w.add_image(groups, 28, 32);
		return true;
	}
	if (part == 8) {
		dw::Obj cities = p.global(gid("cities"));
		const int fn_city = d.find_function("City::walk_data");
		const int step = d.find_step(d.find_function("PtrArray<City>::walk_data"), dw::StepKind::Container);
		for (int who = 0; who < 8; ++who) {
			if (!active(who)) continue;
			dw::Site site{step, int64_t(who) * 0x1c};
			dw::ContainerHeader h;
			if (!p.container(cities, site, h)) return false;
			for (int64_t i = 0; i < h.length; ++i) {
				if (!p.present(cities, site, i)) continue;
				dw::Obj e = p.element(cities, site, i);
				if (!e) continue;
				const uint8_t* fl = p.image(e, 4, 2);
				if (!(fl[0] & 1)) continue;
				cur_who = who;
				cur_o = int(i);
				w.walk(fn_city, e, -1, 0);
			}
		}
		return true;
	}
	if (part == 3 || part == 4) {
		dw::Obj objs = objects;
		if (!i32(objects, off_valid)) return true;
		const int fn = d.find_function(part == 3 ? "AmmoData::walk_data" : "DeathObjData::walk_data");
		const int arr = d.find_function(part == 3 ? "PtrArray<Ammo>::walk_data" : "ObjectArray<DeathObj>::walk_data");
		dw::Site site{d.find_step(arr, dw::StepKind::Container, -1, 0), int64_t(d.find_field("Objects", part == 3 ? "ammo_objs" : "death_objs")->offset)};
		dw::ContainerHeader h;
		if (!p.container(objs, site, h)) return false;
		for (int64_t i = 0; i < h.length; ++i) {
			dw::ContainerKind ck = d.container_kind(site.step);
			if ((ck == dw::ContainerKind::PtrArray || ck == dw::ContainerKind::MultiPtrArray) && !p.present(objs, site, i)) continue;
			dw::Obj e = p.element(objs, site, i);
			if (!e) continue;
			if (part == 3 ? !(*p.image(e, 4, 1) & 3) : !i32(e, 0)) continue;
			cur_o = int(i);
			w.walk(fn, e, -1, 0);
		}
		return true;
	}
	if (part == 9 || part == 10) {
		dw::Obj goods = p.global(gid(part == 9 ? "items" : "goods"));
		const int fn = d.find_function(part == 9 ? "Item::walk_data" : "Good::walk_data");
		dw::Site site{d.find_step(d.find_function(part == 9 ? "PtrArray<Item>::walk_data" : "PtrArray<Good>::walk_data"), dw::StepKind::Container), 0};
		dw::ContainerHeader h;
		if (!p.container(goods, site, h)) return false;
		for (int64_t i = 0; i < h.length; ++i) {
			if (!p.present(goods, site, i)) continue;
			dw::Obj e = p.element(goods, site, i);
			if (!e || !(*p.image(e, 8, 1) & 1)) continue;
			cur_o = int(i);
			w.walk(fn, e, -1, 0);
		}
		return true;
	}
	if (part == 11) {
		dw::Obj world = p.global(gid("world"));
		if (i32(world, 8) > 0) w.walk("World::walk_data", world, gid("world"), 0, {{"part", -1}});
		return true;
	}
	return false;
}

uint32_t adler_of(const std::vector<uint8_t>& b) {
	uint32_t a = 1, s = 0;
	for (uint8_t x : b) { a = (a + x) % 65521; s = (s + a) % 65521; }
	return (s << 16) | a;
}

int64_t modpow(int64_t b, int64_t e, int64_t m) { int64_t r = 1; b %= m; while (e) { if (e & 1) r = r * b % m; b = b * b % m; e >>= 1; } return r; }

// If the original's hash differs from `trace`'s by one changed byte (value difference e in -255..255), names the byte(s).
void solve_single_byte(const PartTrace& tr, uint32_t want, const char* part_name) {
	const dw::Description& d = dw::Description::get();
	const int64_t M = 65521, N = int64_t(tr.bytes.size());
	const uint32_t have = adler_of(tr.bytes);
	int64_t da = ((int64_t(want & 0xffff) - (have & 0xffff)) % M + M) % M, db = ((int64_t(want >> 16) - (have >> 16)) % M + M) % M;
	std::printf("    solve %s: %lld bytes hashed, delta a=%lld b=%lld\n", part_name, (long long)N, (long long)da, (long long)db);
	int shown = 0;
	for (int e = -255; e <= 255 && shown < 12; ++e) {
		if (e == 0) continue;
		const int64_t ee = ((e % M) + M) % M;
		if (ee != da) continue;
		const int64_t w0 = db * modpow(ee, M - 2, M) % M;  // the weight N - i (mod M) of the changed byte
		for (int64_t w = w0; w <= N; w += M) {
			if (w < 1) continue;
			const int64_t i = N - w;
			for (const auto& r : tr.ranges) {
				if (size_t(i) < r.begin || size_t(i) >= r.begin + r.len) continue;
				const int fn = d.step_function(r.step);
				std::printf("      single byte e=%+d at stream %lld: who %d o %d, %s step %d range %lld..%lld byte +%zu (engine 0x%02x -> original 0x%02x)\n", e, (long long)i, r.who, r.o,
					fn >= 0 ? d.function_name(fn) : "?", r.step, (long long)d.step_const(r.step, 0), (long long)d.step_const(r.step, 1), size_t(i) - r.begin, tr.bytes[size_t(i)], (tr.bytes[size_t(i)] + e) & 0xff);
				{
					std::printf("        range bytes (engine, %zu):", r.len);
					for (size_t q = 0; q < std::min<size_t>(r.len, 24); ++q) std::printf(" %02x", tr.bytes[r.begin + q]);
					std::printf("%s\n", r.len > 24 ? " ..." : "");
					const size_t ri = size_t(&r - tr.ranges.data());
					for (size_t q = ri > 3 ? ri - 3 : 0; q < std::min(tr.ranges.size(), ri + 4); ++q) {
						const auto& rr = tr.ranges[q];
						const int f2 = d.step_function(rr.step);
						std::printf("          %s range %zu: step %d (%s) len %zu:", q == ri ? ">>" : "  ", q, rr.step, f2 >= 0 ? d.function_name(f2) : "?", rr.len);
						for (size_t z = 0; z < std::min<size_t>(rr.len, 16); ++z) std::printf(" %02x", tr.bytes[rr.begin + z]);
						std::printf("\n");
					}
				}
				++shown;
				break;
			}
		}
	}
	if (!shown) std::printf("      (not a single changed byte)\n");
	// A whole column: the same byte of every object of one walk step off by the same e (a counter every object keeps).
	{
		std::map<std::pair<int, size_t>, std::pair<int64_t, int64_t>> col;  // (step, byte in range) -> (count, weight sum)
		for (const auto& r : tr.ranges)
			for (size_t k = 0; k < r.len && k < 64; ++k) {
				auto& cw = col[{r.step, k}];
				cw.first++;
				cw.second += N - int64_t(r.begin + k);
			}
		int shown_col = 0;
		for (const auto& kv : col) {
			for (int e = -8; e <= 8 && shown_col < 12; ++e) {
				if (e == 0) continue;
				if (((int64_t(e) * kv.second.first) % M + M) % M != da || ((int64_t(e) * kv.second.second) % M + M) % M != db) continue;
				const int fn = d.step_function(kv.first.first);
				std::printf("      column: every object's byte +%zu of %s step %d range %lld.. off by %+d (%lld objects)\n", kv.first.second, fn >= 0 ? d.function_name(fn) : "?", kv.first.first,
					(long long)d.step_const(kv.first.first, 0), e, (long long)kv.second.first);
				++shown_col;
			}
		}
	}
	// One integer member (1, 2 or 4 bytes) that is off by a small amount (|delta| <= D) anywhere in the stream.
	{
		const int D = N > 300000 ? 64 : 600;
		int hits = 0;
		for (const auto& r : tr.ranges) {
			for (size_t off = 0; off < r.len && hits < 16; ++off) {
				for (int wdt : {1, 2, 4}) {
					if (off + size_t(wdt) > r.len) continue;
					uint32_t v = 0;
					for (int k = 0; k < wdt; ++k) v |= uint32_t(tr.bytes[r.begin + off + size_t(k)]) << (8 * k);
					const uint64_t mod = wdt == 4 ? (uint64_t(1) << 32) : (uint64_t(1) << (8 * wdt));
					for (int dl = -D; dl <= D && hits < 16; ++dl) {
						if (dl == 0) continue;
						const uint32_t nv = uint32_t((uint64_t(v) + mod + uint64_t(int64_t(dl) % int64_t(mod) + int64_t(mod))) % mod);
						int64_t sa = 0, sb = 0;
						for (int k = 0; k < wdt; ++k) {
							const int e = int((nv >> (8 * k)) & 0xff) - int((v >> (8 * k)) & 0xff);
							sa += e;
							sb += int64_t(e) * (N - int64_t(r.begin + off + size_t(k)));
						}
						if (((sa % M) + M) % M != da || ((sb % M) + M) % M != db) continue;
						const int fn = d.step_function(r.step);
						std::printf("      %d-byte member off by %+d at stream %zu: who %d o %d, %s step %d range %lld..%lld +%zu (engine %u -> original %u)\n", wdt, dl, r.begin + off, r.who, r.o,
							fn >= 0 ? d.function_name(fn) : "?", r.step, (long long)d.step_const(r.step, 0), (long long)d.step_const(r.step, 1), off, v, nv);
						++hits;
					}
				}
			}
		}
		if (!hits) std::printf("      (no integer member off by at most %d)\n", D);
	}
	// Two bytes of the same member of two different objects, changed by the same amount e (a counter that runs differently
	// in two objects): e + e = da and e (w1 + w2) = db.
	{
		std::vector<int> range_of(size_t(N), -1);
		for (size_t k = 0; k < tr.ranges.size(); ++k)
			for (size_t i = 0; i < tr.ranges[k].len; ++i) range_of[tr.ranges[k].begin + i] = int(k);
		int pairs = 0;
		for (int e = -64; e <= 64 && pairs < 10; ++e) {
			if (e == 0 || ((2 * e) % M + M) % M != da) continue;
			const int64_t need = db * modpow(((e % M) + M) % M, M - 2, M) % M;  // w1 + w2 mod M
			for (int64_t i = 0; i < N && pairs < 10; ++i) {
				const int64_t w1 = N - i;
				for (int64_t w2 = (need - w1 % M + M) % M; w2 <= N; w2 += M) {
					if (w2 < 1) continue;
					const int64_t j = N - w2;
					if (j <= i) continue;
					const auto& ri = tr.ranges[size_t(range_of[size_t(i)])];
					const auto& rj = tr.ranges[size_t(range_of[size_t(j)])];
					if (ri.step != rj.step || size_t(i) - ri.begin != size_t(j) - rj.begin || (ri.who == rj.who && ri.o == rj.o)) continue;
					const int fn = d.step_function(ri.step);
					std::printf("      same member of two objects, e=%+d each: %s step %d range %lld..%lld byte +%zu: (who %d o %d) and (who %d o %d)\n", e,
						fn >= 0 ? d.function_name(fn) : "?", ri.step, (long long)d.step_const(ri.step, 0), (long long)d.step_const(ri.step, 1), size_t(i) - ri.begin, ri.who, ri.o, rj.who, rj.o);
					++pairs;
				}
			}
		}
	}
}


int g_shift_part = -1;
int64_t g_shift_at = -1;
std::map<int64_t, PartTrace> g_shift_traces;

// Hypothesis: the original's object(s) are a few frames ahead of / behind the engine's. For each labelled object (who, o) of part
// `g_shift_part` and each other frame t within 8 of g_shift_at, the effect of substituting the object's bytes by those of frame t;
// then one or two such substitutions that reproduce the original's hash at g_shift_at.
void shift_search(uint32_t want) {
	const int64_t M = 65521;
	auto it0 = g_shift_traces.find(g_shift_at);
	if (it0 == g_shift_traces.end()) return;
	const PartTrace& base = it0->second;
	const int64_t N = int64_t(base.bytes.size());
	// object segments of a trace: (who, o) -> [begin, end) (the objects' ranges are contiguous)
	auto segments = [](const PartTrace& t) {
		std::map<std::pair<int, int>, std::pair<size_t, size_t>> seg;
		for (const auto& r : t.ranges) {
			auto key = std::make_pair(r.who, r.o);
			auto f = seg.find(key);
			if (f == seg.end()) seg[key] = {r.begin, r.begin + r.len};
			else f->second.second = r.begin + r.len;
		}
		return seg;
	};
	const auto bseg = segments(base);
	// prefix states (a, b) at every byte, and suffix transforms from every byte: (sum of bytes, b-contribution from a = 0)
	std::vector<std::pair<int64_t, int64_t>> pre(size_t(N) + 1);
	pre[0] = {1, 0};
	for (int64_t i = 0; i < N; ++i) {
		const int64_t a = (pre[size_t(i)].first + base.bytes[size_t(i)]) % M;
		pre[size_t(i) + 1] = {a, (pre[size_t(i)].second + a) % M};
	}
	std::vector<std::pair<int64_t, int64_t>> suf(size_t(N) + 1);  // from a = 0 and b = 0 start
	suf[size_t(N)] = {0, 0};
	for (int64_t i = N - 1; i >= 0; --i) {
		// processing byte i then the rest: a_i = x_i; b gets a_i * (1 + rest length) + ...
		const int64_t rest = N - 1 - i;
		const int64_t x = base.bytes[size_t(i)];
		suf[size_t(i)] = {(suf[size_t(i) + 1].first + x) % M, (suf[size_t(i) + 1].second + x * (rest + 1)) % M};
	}
	const uint32_t h0 = adler_of(base.bytes);
	struct Cand { std::pair<int, int> obj; int64_t t; };
	std::vector<Cand> hits;
	int64_t trials = 0;
	for (const auto& kv : g_shift_traces) {
		if (kv.first == g_shift_at) continue;
		const PartTrace& other = kv.second;
		const auto oseg = segments(other);
		for (const auto& sg : bseg) {
			auto f = oseg.find(sg.first);
			if (f == oseg.end()) continue;
			// base[0, begin) + other segment + base[end, N)
			std::pair<int64_t, int64_t> st = pre[sg.second.first];
			for (size_t q = f->second.first; q < f->second.second; ++q) {
				st.first = (st.first + other.bytes[q]) % M;
				st.second = (st.second + st.first) % M;
			}
			const int64_t Lsuf = N - int64_t(sg.second.second);
			const int64_t a = (st.first + suf[sg.second.second].first) % M;
			const int64_t b = (st.second + st.first * Lsuf + suf[sg.second.second].second) % M;
			++trials;
			if ((uint32_t(b) << 16 | uint32_t(a)) == want) hits.push_back({sg.first, kv.first});
		}
	}
	std::printf("  time-shift search: %lld (object, frame) substitutions (base hash %u, wanted %u)\n", (long long)trials, h0, want);
	for (const auto& h : hits) std::printf("    SHIFT HIT: object (%d,%d) as at frame %lld\n", h.obj.first, h.obj.second, (long long)h.t);
	if (hits.empty()) std::printf("    (no single object at another frame explains it)\n");
}


// Group::action_unitmask @6fcb90 (no Verb in sim/commands.hpp yet): toggles `mask` on the members of the current group. The first member
// decides (set unless the mask is AI control 0x40000, cleared when it already has the bit); the others follow the first one's new state.
// The command's `set` field is not read by the original. AUTO_EXPLORE (0x100) additionally forgets the unit's orders.
void apply_unitmask(sim::Game& g, int32_t group, uint32_t mask) {
	if (group < 0 || group >= sim::kNumGroups) return;
	sim::Group& gr = g.groups.g[size_t(group)];
	if (gr.buildings) return;
	bool follow = mask != 0x40000;
	for (int32_t i = 0; i < gr.num; ++i) {
		sim::Unit* u = g.unit(gr.who, gr.list[size_t(i)]);
		if (!u || !u->alive()) continue;
		if (mask == 0x100 && sim::unit_is_plane(*u) && u->ptype->domain == 2 && !(u->ptype->unit_flags & 0x20)) continue;
		const bool now = (u->unit_masks & mask) == 0 ? follow : false;
		if (now) u->unit_masks |= mask;
		else u->unit_masks &= ~mask;
		follow = now;
		if (mask == 0x100) {
			u->flags |= sim::objflag::kNewThink;
			sim::clear_orders(g, *u);
		}
	}
}

int64_t g_pshift_at = -1;
std::map<int64_t, std::map<std::pair<std::tuple<int, int, int>, std::string>, std::vector<uint8_t>>> g_pshift_vals;

// Hypothesis: a few guys of the original have, at boundary B, some of their members (positions, ...) as the engine has them at another
// frame. The engine is at frame B+8 here: we need the state at B, so the search re-derives the base from the recorded member values
// (guys part only, byte positions found by inverting each member; additive effects; one or two guys, four member sets).
void partial_shift_search(sim::Game& game, int64_t B, uint32_t want) {
	const int64_t M = 65521;
	// Reconstruct the state at B by writing the recorded values back into the (later) engine state for every Guy member.
	const auto& base = g_pshift_vals[B];
	std::vector<sim::MemberChange> all = sim::members_of(game, {"Guy.*"});
	for (const auto& mc : all) {
		auto it = base.find({{mc.id.a, mc.id.b, mc.id.c}, mc.key});
		if (it != base.end()) sim::apply_member_bytes(game, mc, it->second);
	}
	sim::EngineProvider ep(game);
	PartTrace t0;
	trace_part(ep, 6, t0);
	const int64_t N = int64_t(t0.bytes.size());
	const uint32_t h0 = adler_of(t0.bytes);
	if (h0 == want) { std::printf("  partial-shift: the reconstructed state already matches\n"); return; }
	const int64_t ta = ((int64_t(want & 0xffff) - (h0 & 0xffff)) % M + M) % M, tb = ((int64_t(want >> 16) - (h0 >> 16)) % M + M) % M;
	// member positions: one inverted trace per member kind over all guys
	std::map<std::string, std::vector<const sim::MemberChange*>> bykey;
	for (const auto& mc : all) bykey[mc.key].push_back(&mc);
	std::map<std::pair<std::tuple<int, int, int>, std::string>, std::vector<int64_t>> pos;
	for (auto& kv : bykey) {
		const size_t n = kv.second.front()->now.size();
		if (n == 0 || n > 8) continue;
		for (const auto* c : kv.second) {
			std::vector<uint8_t> cur(n);
			auto it = base.find({{c->id.a, c->id.b, c->id.c}, c->key});
			cur = it != base.end() ? it->second : c->now;
			std::vector<uint8_t> inv(n);
			for (size_t k = 0; k < n; ++k) inv[k] = uint8_t(~cur[k]);
			sim::apply_member_bytes(game, *c, inv);
		}
		sim::EngineProvider ep3(game);
		PartTrace t1;
		const bool ok = trace_part(ep3, 6, t1) && t1.bytes.size() == t0.bytes.size();
		for (const auto* c : kv.second) {
			auto it = base.find({{c->id.a, c->id.b, c->id.c}, c->key});
			sim::apply_member_bytes(game, *c, it != base.end() ? it->second : c->now);
		}
		if (!ok) continue;
		size_t ri = 0;
		std::map<std::tuple<int, int>, std::vector<int64_t>> byobj;
		for (size_t q = 0; q < t0.bytes.size(); ++q) {
			while (ri + 1 < t0.ranges.size() && t0.ranges[ri].begin + t0.ranges[ri].len <= q) ++ri;
			if (t0.bytes[q] != t1.bytes[q]) byobj[{t0.ranges[ri].who, t0.ranges[ri].o}].push_back(int64_t(q));
		}
		// guys of one unit follow each other in the stream: the k-th guy of the unit owns the k-th group of n changed bytes
		for (auto& bo : byobj) {
			size_t gi = 0;
			for (size_t q = 0; q + n <= bo.second.size(); q += n, ++gi)
				pos[{{std::get<0>(bo.first), std::get<1>(bo.first), int(gi)}, kv.first}] = std::vector<int64_t>(bo.second.begin() + std::ptrdiff_t(q), bo.second.begin() + std::ptrdiff_t(q + n));
		}
	}
	const std::vector<std::vector<std::string>> sets = {
		{"Guy.x", "Guy.y", "Guy.des_x", "Guy.des_y", "Guy.last_x", "Guy.last_y"},
		{"Guy.x", "Guy.y", "Guy.des_x", "Guy.des_y", "Guy.last_x", "Guy.last_y", "Guy.z", "Guy.last_z"},
		{"Guy.x", "Guy.y", "Guy.des_x", "Guy.des_y", "Guy.last_x", "Guy.last_y", "Guy.z", "Guy.last_z", "Guy.angle", "Guy.des_angle", "Guy.last_angle", "Guy.last_speed", "Guy.avg_speed"},
		{"Guy.cur_time", "Guy.last_time", "Guy.end_time", "Guy.cur_anim", "Guy.stopped"},
		{"Guy.x", "Guy.y", "Guy.des_x", "Guy.des_y", "Guy.last_x", "Guy.last_y", "Guy.z", "Guy.last_z", "Guy.angle", "Guy.des_angle", "Guy.last_angle", "Guy.last_speed", "Guy.avg_speed", "Guy.cur_time", "Guy.last_time", "Guy.end_time", "Guy.cur_anim", "Guy.stopped"}};
	struct Cand { std::tuple<int, int, int> obj; int64_t t; int set; int64_t a, b; };
	std::vector<Cand> cands;
	std::set<std::tuple<int, int, int>> objs;
	for (const auto& kv : pos) objs.insert(kv.first.first);
	for (const auto& kv : g_pshift_vals) {
		const int64_t t = kv.first;
		if (t == B) continue;
		for (const auto& ob : objs)
			for (size_t si = 0; si < sets.size(); ++si) {
				int64_t sa = 0, sb = 0;
				bool any = false;
				for (const std::string& key : sets[si]) {
					auto pi = pos.find({ob, key});
					auto vb = base.find({ob, key});
					auto vt = kv.second.find({ob, key});
					if (pi == pos.end() || vb == base.end() || vt == kv.second.end()) continue;
					for (size_t k = 0; k < pi->second.size() && k < vb->second.size(); ++k) {
						const int e = int(vt->second[k]) - int(vb->second[k]);
						if (!e) continue;
						any = true;
						sa += e;
						sb += int64_t(e) * (N - pi->second[k]);
					}
				}
				if (any) cands.push_back({ob, t, int(si), ((sa % M) + M) % M, ((sb % M) + M) % M});
			}
	}
	std::printf("  partial-shift search: %zu candidates; target a=%lld b=%lld\n", cands.size(), (long long)ta, (long long)tb);
	std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> byeff;
	for (size_t i = 0; i < cands.size(); ++i) byeff[{cands[i].a, cands[i].b}].push_back(i);
	int shown = 0;
	auto name = [&](const Cand& c) { char b[96]; std::snprintf(b, sizeof b, "guy (%d,%d,%d) members-set %d as at frame %lld", std::get<0>(c.obj), std::get<1>(c.obj), std::get<2>(c.obj), c.set, (long long)c.t); return std::string(b); };
	for (size_t i = 0; i < cands.size() && shown < 20; ++i) {
		if (cands[i].a == ta && cands[i].b == tb) { std::printf("    PSHIFT HIT (1): %s\n", name(cands[i]).c_str()); ++shown; }
		auto f = byeff.find({((ta - cands[i].a) % M + M) % M, ((tb - cands[i].b) % M + M) % M});
		if (f == byeff.end()) continue;
		for (size_t j : f->second) if (j > i && cands[j].obj != cands[i].obj && shown < 20) { std::printf("    PSHIFT HIT (2): %s + %s\n", name(cands[i]).c_str(), name(cands[j]).c_str()); ++shown; }
	}
	if (!shown) {
		// three guys: pairs of candidates of the wide member sets in a map, then the third one
		std::vector<size_t> wide;
		for (size_t i = 0; i < cands.size(); ++i) if (cands[i].set == 2 || cands[i].set == 4) wide.push_back(i);
		std::map<std::pair<int64_t, int64_t>, std::pair<size_t, size_t>> pr;
		for (size_t x = 0; x < wide.size(); ++x)
			for (size_t y = x + 1; y < wide.size(); ++y)
				if (cands[wide[x]].obj != cands[wide[y]].obj)
					pr[{(cands[wide[x]].a + cands[wide[y]].a) % M, (cands[wide[x]].b + cands[wide[y]].b) % M}] = {wide[x], wide[y]};
		int s3 = 0;
		for (size_t k : wide) {
			auto f = pr.find({((ta - cands[k].a) % M + M) % M, ((tb - cands[k].b) % M + M) % M});
			if (f == pr.end()) continue;
			const auto [i, j] = f->second;
			if (cands[k].obj == cands[i].obj || cands[k].obj == cands[j].obj) continue;
			if (s3++ < 10) std::printf("    PSHIFT HIT (3): %s + %s + %s\n", name(cands[i]).c_str(), name(cands[j]).c_str(), name(cands[k]).c_str());
		}
		if (!s3) std::printf("    (no one-, two- or three-guy partial time shift)\n");
	}
}

struct ReplayCommand {
	bool supported = false;
	sim::Command cmd;
	std::string name;
	int32_t mask_arg = 0;  // UNITMASK: the mask (applied by apply_unitmask, no Verb)
};

ReplayCommand to_command(const Json& c) {
	static const std::map<std::string, int64_t> queue = {{"FIRST", sim::kQueueFirst}, {"LAST", sim::kQueueLast}, {"NEW", sim::kQueueNew}};
	static const std::map<std::string, int64_t> orders = {{"MOVE_TO", 1}, {"ATTACK_TO", 2}, {"EXPLORE_TO", 3}, {"FLEE_TO", 4}, {"BUILD_AT", 6}, {"GATHER", 7},
		{"REPAIR", 13}, {"ATTACK", 10}};
	ReplayCommand r;
	const Json* nm = c.get("name");
	r.name = nm ? nm->s : "?";
	if (r.name == "UNITMASK") { if (const Json* ap0 = c.get("args")) r.mask_arg = int32_t(ap0->num("unitmask")); }
	static const Json empty;
	const Json* ap = c.get("args");
	const Json& a = ap ? *ap : empty;
	sim::Command& m = r.cmd;
	const std::string& n = r.name;
	if (n == "GROUP") {
		m.verb = sim::Verb::Group;
		m.who = int32_t(a.num("who"));
		if (const Json* o = a.get("objects"))
			for (const Json& x : o->a) m.objects.push_back(uint16_t(x.n));
		r.supported = true;
	} else if (n == "MOVE_TO") {
		m.verb = sim::Verb::MoveTo;
		m.x = int32_t(a.num("to_x")); m.y = int32_t(a.num("to_y"));
		m.set_angle = int32_t(a.num("set_angle")); m.angle = uint32_t(a.num("angle"));
		m.orders = int8_t(json_enum(a, "orders", orders, 1));
		m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		m.form = int8_t(a.num("form", -1)); m.width = int8_t(a.num("width", -1)); m.disembark = int8_t(a.num("disembark"));
		r.supported = true;
	} else if (n == "BUILD") {
		m.verb = sim::Verb::Build;
		m.x = int32_t(a.num("x")); m.y = int32_t(a.num("y")); m.x2 = int32_t(a.num("x2")); m.y2 = int32_t(a.num("y2"));
		m.type = int32_t(a.num("type")); m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = true;
	} else if (n == "GATHER") {
		m.verb = sim::Verb::Gather;
		m.ox = int32_t(a.num("ox")); m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = true;
	} else if (n == "SWARM_AROUND") {
		m.verb = sim::Verb::SwarmAround;
		m.ox = int32_t(a.num("ox")); m.whom = int32_t(a.num("whom"));
		m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew)); m.order_kind = int32_t(json_enum(a, "orders", orders, 6));
		r.supported = true;
	} else if (n == "QUEUE_UP") {
		m.verb = sim::Verb::QueueUp;
		m.type = int32_t(a.num("type")); m.num = int32_t(a.num("num", 1));
		r.supported = true;
	} else if (n == "GATHER_POINT") {
		m.verb = sim::Verb::GatherPoint;
		m.x = int32_t(a.num("x")); m.y = int32_t(a.num("y")); m.action = int32_t(a.num("action")); m.add_to_end = int32_t(a.num("add_to_end"));
		r.supported = true;
	} else if (n == "UNQUEUE") {
		m.verb = sim::Verb::Unqueue;
		m.who = int32_t(a.num("who")); m.ox = int32_t(a.num("o")); m.num = int32_t(a.num("type")); m.uid = uint16_t(a.num("uid"));
		r.supported = true;
	} else if (n == "LEADER_OPTIONS") {
		m.verb = sim::Verb::LeaderOptions;
		m.who = int32_t(a.num("who")); m.peasants = int32_t(a.num("peasants")); m.peasants_wait = int32_t(a.num("peasants_wait"));
		m.buildings = int32_t(a.num("buildings")); m.option_flags = uint32_t(a.num("flags"));
		r.supported = true;
	} else if (n == "HALT") {
		m.verb = sim::Verb::Halt;
		r.supported = true;
	} else if (n == "ATTACK") {
		m.verb = sim::Verb::Attack;
		m.ox = int32_t(a.num("ox")); m.whom = int32_t(a.num("whom")); m.ignore = int32_t(a.num("ignore"));
		m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = true;
	} else if (n == "FOLLOW" || n == "GUARD") {
		m.verb = n == "FOLLOW" ? sim::Verb::Follow : sim::Verb::Guard;
		m.ox = int32_t(a.num("ox")); m.whom = int32_t(a.num("whom"));
		m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = std::getenv("RON_NO_GUARD") == nullptr;  // (RON_NO_GUARD=1: the behaviour before FOLLOW / GUARD were executed)
	} else if (n == "STANCE") {
		m.verb = sim::Verb::Stance;
		m.stance = int32_t(a.num("stance"));
		r.supported = true;
	} else if (n == "FORM") {
		m.verb = sim::Verb::Form;
		m.form32 = int32_t(a.num("form")); m.rotate = int32_t(a.num("rotate")); m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = true;
	} else if (n == "DISBAND") {
		m.verb = sim::Verb::Disband;
		m.all = int32_t(a.num("all"));
		r.supported = true;
	} else if (n == "BUILDMASK") {
		m.verb = sim::Verb::BuildMask;
		m.mask = int32_t(a.num("buildmask")); m.set = int32_t(a.num("set"));
		r.supported = true;
	} else if (n == "TRANSPORT") {
		m.verb = sim::Verb::Transport;
		r.supported = true;
	} else if (n == "SET_TRANSPORT") {
		m.verb = sim::Verb::SetTransport;
		m.flags = int32_t(a.num("flag"));
		r.supported = true;
	} else if (n == "BOARD_SHIP") {
		m.verb = sim::Verb::BoardShip;
		m.ox = int32_t(a.num("ox")); m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = true;
	} else if (n == "TRADE") {
		m.verb = sim::Verb::Trade;
		m.ox = int32_t(a.num("ox")); m.whom = int32_t(a.num("whom")); m.x2 = int32_t(a.num("oxx")); m.y2 = int32_t(a.num("whose"));
		m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = true;
	} else if (n == "CITY_GATHER") {
		m.verb = sim::Verb::CityGather;
		m.type = int32_t(a.num("t")); m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = true;
	} else if (n == "GARRISON") {
		m.verb = sim::Verb::Garrison;
		m.ox = int32_t(a.num("ox")); m.whom = int32_t(a.num("whom")); m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		r.supported = true;
	} else if (n == "SPELL") {
		m.verb = sim::Verb::Spell;
		m.ox = int32_t(a.num("ox")); m.whom = int32_t(a.num("whom")); m.type = int32_t(a.num("type")); m.x = int32_t(a.num("x")); m.y = int32_t(a.num("y"));
		r.supported = true;
	} else if (n == "EJECTALL") {
		m.verb = sim::Verb::EjectAll;
		m.back_to_work = int32_t(a.num("back_to_work")); m.who = int32_t(a.num("who", -1)); m.eject_o = int32_t(a.num("eject_o", -1)); m.eject_who = int32_t(a.num("eject_who", -1));
		r.supported = true;
	} else if (n == "BUY" || n == "SELL") {
		m.verb = n == "BUY" ? sim::Verb::Buy : sim::Verb::Sell;
		m.who = int32_t(a.num("who")); m.good = int32_t(a.num("good")); m.flags = int32_t(a.num("flags"));
		r.supported = true;
	} else if (n == "TRIBUTE" || n == "DEMAND_TRIBUTE") {
		m.verb = n == "TRIBUTE" ? sim::Verb::Tribute : sim::Verb::DemandTribute;
		m.who = int32_t(a.num("who")); m.whom = int32_t(a.num("whom")); m.good = int32_t(a.num("good")); m.amount = int32_t(a.num("amount"));
		r.supported = true;
	} else if (n == "ATTACK_GROUND" || n == "PATROL" || n == "LAUNCH_PATROL") {
		m.verb = n == "ATTACK_GROUND" ? sim::Verb::AttackGround : n == "PATROL" ? sim::Verb::Patrol : sim::Verb::LaunchPatrol;
		m.x = int32_t(a.num("to_x")); m.y = int32_t(a.num("to_y")); m.queued = int8_t(json_enum(a, "queued", queue, sim::kQueueNew));
		m.shift = int32_t(a.num("shift")); m.ctrl = int32_t(a.num("ctrl")); m.alt = int32_t(a.num("alt"));
		r.supported = true;
	} else if (n == "FLIGHT") {
		m.verb = sim::Verb::Flight;
		m.ox = int32_t(a.num("ox")); m.whom = int32_t(a.num("whom")); m.shift = int32_t(a.num("shift")); m.ctrl = int32_t(a.num("ctrl")); m.alt = int32_t(a.num("alt"));
		m.orders = int8_t(a.num("orders", 1));
		r.supported = true;
	} else if (n == "RECALL" || n == "SCRAMBLE") {
		m.verb = n == "RECALL" ? sim::Verb::Recall : sim::Verb::Scramble;
		r.supported = true;
	} else if (n == "CLEAR_TRIBUTES" || n == "ACCEPT" || n == "REJECT") {
		m.verb = n == "CLEAR_TRIBUTES" ? sim::Verb::ClearTributes : n == "ACCEPT" ? sim::Verb::Accept : sim::Verb::Reject;
		m.who = int32_t(a.num("who")); m.whom = int32_t(a.num("whom"));
		r.supported = true;
	}
	return r;
}

// Every CHECK_SUMS row of one replay in the CSV: record frame -> {tuple -> players that sent it}.
std::map<int64_t, std::map<std::vector<uint32_t>, int>> all_check_sums(const std::string& csv_path, const std::string& replay_path) {
	std::map<int64_t, std::map<std::vector<uint32_t>, int>> rows;
	std::ifstream f(csv_path);
	if (!f) return rows;
	const std::string base = basename_of(replay_path);
	std::string line;
	std::getline(f, line);
	while (std::getline(f, line)) {
		if (line.empty() || line[0] != '"') continue;
		size_t q = line.find('"', 1);
		if (q == std::string::npos || basename_of(line.substr(1, q - 1)) != base) continue;
		std::vector<std::string> cols;
		std::stringstream ss(line.substr(q + 2));
		std::string c;
		while (std::getline(ss, c, ',')) cols.push_back(c);
		if (cols.size() < 2 + dw::kCheckPartCount) continue;
		std::vector<uint32_t> v;
		for (int k = 0; k < dw::kCheckPartCount; ++k) v.push_back(uint32_t(std::strtoull(cols[2 + k].c_str(), nullptr, 10)));
		++rows[std::atoll(cols[0].c_str())][v];
	}
	return rows;
}

#ifdef RON_RNG_PROFILE_HOOK
std::map<std::string, int64_t> g_rng_sites;
sim::Game* g_trace_game = nullptr;
int g_trace_until = -1;
int g_ins_frame = -1, g_ins_n = -1, g_ins_k = 0, g_ins_count = 0, g_ins_last = -1;  // RON_INSERT_N="F:n:k": k extra draws just before the n-th (1-based) draw of frame F
void rng_hook_fn() {
	if (g_trace_game && g_ins_frame >= 0 && g_trace_game->frame != g_ins_last) { g_ins_last = g_trace_game->frame; g_ins_count = 0; }
	if (g_trace_game && g_ins_frame >= 0 && g_trace_game->frame == g_ins_frame && ++g_ins_count == g_ins_n) {
		uint32_t sd = g_trace_game->rng.seed();
		for (int i = 0; i < g_ins_k; ++i) sd = sd * 0x19660Du + 0x3C6EF35Fu;
		g_trace_game->rng.reseed(sd);
	}
	void* fr[10];
	const int n = backtrace(fr, 10);
	std::string key;
	for (int i = 1; i < n && i < 8; ++i) {
		Dl_info di;
		if (!dladdr(fr[i], &di) || !di.dli_sname) continue;
		int st = 0;
		char* dm = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &st);
		std::string nm = st == 0 && dm ? dm : di.dli_sname;
		std::free(dm);
		if (nm.find("Random::") != std::string::npos || nm.find("rnd") != std::string::npos) continue;
		nm = nm.substr(0, nm.find('('));
		key += (key.empty() ? "" : " < ") + nm;
		if (key.size() > 150) break;
	}
	g_rng_sites[key]++;
	if (g_trace_game && g_trace_game->frame <= g_trace_until) std::printf("RNGTRACE frame %d seed %08x %s\n", g_trace_game->frame, g_trace_game->rng.seed(), key.c_str());
}
#endif

struct IdlePick { int frame; int who; int o; int type; uint32_t seed_after; };
std::vector<IdlePick> g_idle_picks;
void idle_pick_hook(sim::Game& g, sim::Unit& u) { g_idle_picks.push_back({g.frame, u.who, u.o, u.type, g.rng.seed()}); }
// The gaia animals of list 8 re-pick their idle variant in lock step ("cur_time" = frames since the last pick, all fish of one burst picked in
// the same frame): the variants stored in a save are the classes (r < 70 -> 0, < 83 -> 1, < 96 -> 2, else 3) of CONSECUTIVE generator outputs
// (the fish are processed in object order in Objects::inc_time, nothing else draws in between). So the saved fish give the position of the
// original's generator, counted in draws from the load, at every burst frame of the last ~400 frames before the save: a draw-count timeline of
// the original that needs no object-level truth. `seed_load` is the generator right after the load effects (position 0).
void fish_timeline_report(sim::Game& truth, int next_frame, uint32_t seed_load, const std::vector<std::pair<int, uint32_t>>& frame_seeds, uint32_t engine_end) {
	const int64_t J = std::getenv("RON_FISH_J") ? std::atoll(std::getenv("RON_FISH_J")) : 3000000;
	std::vector<uint8_t> cls(size_t(J + 1));
	std::unordered_map<uint32_t, int64_t> index_of;
	index_of.reserve(size_t(J) + 1);
	uint32_t sd = seed_load;
	index_of[sd] = 0;
	for (int64_t j = 1; j <= J; ++j) {
		sd = sd * 0x19660Du + 0x3C6EF35Fu;
		const int32_t r = int32_t((int64_t(sd & 0xFFFFu) * 0xFFFF) >> 16) % 100;
		cls[size_t(j)] = r < 70 ? 0 : r < 83 ? 1 : r < 96 ? 2 : 3;
		index_of.emplace(sd, j);
	}
	struct Pick { int o; int var; int type; };  // var -1: no information (an animal with a single idle animation)
	std::map<int, std::vector<Pick>> bursts;
	for (int o = 0; o < truth.objects[8].unit_mark; ++o) {
		sim::Unit* u = truth.unit(8, o);
		if (!u || !u->alive() || u->guys.empty()) continue;
		const sim::Guy& gy = u->guys[0];
		const int f = next_frame - 1 - int(gy.cur_time);
		const bool has_var = u->type == 411;
		bursts[f].push_back({o, has_var ? int(gy.cur_anim) : -1, u->type});
	}
	std::printf("  fish timeline (draws since the load; a variant only fixes the position modulo 65,536, so the one within reach of the save's generator is taken)\n");
	auto oe = index_of.find(truth.rng.seed());
	auto ee = index_of.find(engine_end);
	const int64_t n_orig = oe == index_of.end() ? -1 : oe->second, n_eng = ee == index_of.end() ? -1 : ee->second;
	std::printf("  generator at the save: original draw %lld, engine draw %lld\n", (long long)n_orig, (long long)n_eng);
	int shown = 0;
	int64_t prev_f = -1, prev_do = -1, prev_de = -1;
	for (auto it = bursts.rbegin(); it != bursts.rend() && shown < 60; ++it) {
		const std::vector<Pick>& v = it->second;
		int informative = 0;
		for (const Pick& p : v) informative += p.var >= 0;
		if (informative < 6 || n_orig < 0) continue;
		std::vector<int64_t> hits;
		const int64_t lo = std::max<int64_t>(0, n_orig - 70LL * (next_frame - it->first));
		for (int64_t j = lo; j <= n_orig && hits.size() < 3; ++j) {
			bool ok = true;
			for (size_t k = 0; k < v.size() && ok; ++k) ok = v[k].var < 0 || cls[size_t(j + 1 + int64_t(k))] == v[k].var;
			if (ok) hits.push_back(j);
		}
		int64_t eng_start = -1;
		for (size_t q = 0; q < frame_seeds.size(); ++q) if (frame_seeds[q].first == it->first) {
			auto a2 = index_of.find(frame_seeds[q].second);
			if (a2 != index_of.end()) eng_start = a2->second;
		}
		int64_t eng_first_pick = -1, eng_npicks = 0;
		for (const IdlePick& ip : g_idle_picks) if (ip.frame == it->first && ip.who == 8 && ip.type == 411) {
			auto a3 = index_of.find(ip.seed_after);
			if (eng_first_pick < 0 && a3 != index_of.end()) eng_first_pick = a3->second - 1;
			++eng_npicks;
		}
		std::printf("    burst at frame %d (%d frames before the save): %zu animals (%d with a variant), original draws after it:", it->first, next_frame - it->first, v.size(), informative);
		for (int64_t h : hits) std::printf(" %lld", (long long)(n_orig - h));
		if (hits.empty()) std::printf(" (no match)");
		if (eng_start >= 0 && n_eng >= 0) std::printf("; engine draws after that frame's start: %lld", (long long)(n_eng - eng_start));
		if (hits.size() == 1) std::printf("; original draws before the burst since the load: %lld, engine: %lld (%lld fish)", (long long)hits[0], (long long)eng_first_pick, (long long)eng_npicks);
		if (hits.size() == 1 && eng_start >= 0 && n_eng >= 0) {
			const int64_t d_o = n_orig - hits[0], d_e = n_eng - eng_start;
			std::printf("  (original - engine %+lld)", (long long)(d_o - d_e));
			if (prev_f >= 0 && prev_f != it->first) std::printf("  rate since the previous burst: original %.2f engine %.2f per frame", double(d_o - prev_do) / double(it->first - prev_f) * -1.0, double(d_e - prev_de) / double(it->first - prev_f) * -1.0);
			prev_f = it->first; prev_do = d_o; prev_de = d_e;
		}
		std::printf("\n");
		++shown;
	}
	auto e = index_of.find(engine_end);
	std::printf("  (engine end seed is draw %lld since the load)\n", e == index_of.end() ? -1LL : (long long)e->second);
}

int replay_run(const std::string& path, const std::vector<uint8_t>& save, const std::string& game_dir, int64_t max_frames, const std::string& json_path,
               const std::string& csv_path, bool use_commands, bool verbose, const std::string& next_path = std::string(), bool no_art = false, const std::vector<std::string>& revert = {}) {
	// the command stream
	std::string jp = json_path;
	if (jp.empty()) {
		jp = "/tmp/ron-sav-replay-" + std::to_string(::getpid()) + ".json";
		auto quote = [](const std::string& x) {
			std::string q = "'";
			for (char c : x) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
			return q + "'";
		};
		std::string cmd = "node re/tools/rcxdump.mjs " + quote(path) + " --json " + quote(jp) + " > /dev/null 2>&1";
		if (std::system(cmd.c_str()) != 0) { std::printf("replay: could not run node re/tools/rcxdump.mjs (run from the repo root, or pass --json)\n"); return 1; }
	}
	std::vector<uint8_t> jb;
	if (!read_file(jp, jb)) { std::printf("replay: cannot read %s\n", jp.c_str()); return 1; }
	std::string js(jb.begin(), jb.end());
	Json root;
	JsonParser jparse(js);
	if (!jparse.parse(root)) { std::printf("replay: cannot parse the command stream\n"); return 1; }
	if (json_path.empty()) std::remove(jp.c_str());
	const Json* recs = root.get("records");
	const Json* hdr = root.get("header");
	std::map<int, int> seat_who;
	if (hdr) if (const Json* pl = hdr->get("players")) for (size_t i = 0; i < pl->a.size(); ++i) seat_who[int(pl->a[i].num("play", int64_t(i)))] = int(pl->a[i].num("who", int64_t(i)));

	// the engine's game
	dw::Tree peek(dw::Description::get());
	dw::SaveDecode psd = dw::decode_save(peek, save.data(), save.size(), 0);
	if (!psd.result.ok) { std::printf("replay: decode failed\n"); return 1; }
	sim::GameSetup setup;
	setup.starting_technology = int32_t(field_int(peek, "game.info.starting_technology"));
	setup.starting_technology2 = int32_t(field_int(peek, "game.info.starting_technology2"));
	setup.ending_technology = int32_t(field_int(peek, "game.info.ending_technology"));
	setup.game_rules = int32_t(field_int(peek, "game.info.game_rules"));
	setup.flag8 = (field_int(peek, "game.info.flags") & 8) != 0;
	static std::unique_ptr<RulesCache> cache;
	if (!cache) cache = std::make_unique<RulesCache>(game_dir);
	const sim::Rules& rules = cache->get(setup);
	sim::OriginalLoadReport rep;
	std::unique_ptr<sim::Game> game = sim::load_original_save(cache->files, rules, nullptr, save, &rep);
	if (!game) { std::printf("replay: load failed: %s\n", rep.error.c_str()); return 1; }
	// The art: animation lengths and release frames come from the install's .bha files (without them every animation is 3 frames).
	std::unique_ptr<sim::AssetAnimSource> art;
	if (!no_art) {
		art = sim::load_anim_source(cache->files, *game->rules);
		game->anims = art.get();
	}
	if (std::getenv("RON_ANIM_CHECK") && game->anims) {
		// The saved end_time of every guy against the animation length the engine derives from the install for its
		// (gpiece, cur_anim): they agree for any guy whose clip was started by Guy::set_anim.
		std::map<std::tuple<int, int, int, int>, int> bad;  // (who class, type, anim, saved end -> engine frames) count
		int total = 0, agree = 0;
		for (int who = 0; who < sim::kNumLeaders; ++who)
			for (int o = 0; o < game->objects[size_t(who)].unit_mark; ++o) {
				sim::Unit* u0 = game->unit(who, o);
				if (!u0 || !u0->alive()) continue;
				for (const sim::Guy& gy : u0->guys) {
					++total;
					const int fr = game->anims->frames(gy.gpiece, gy.cur_anim);
					if (std::getenv("RON_ANIM_CHECK")[0] == '2') std::printf("guy who %d type %d anim %d time %u/%u\n", who, u0->type, gy.cur_anim, gy.cur_time, gy.end_time);
					if (int(gy.end_time) == fr) ++agree;
					else ++bad[{who, u0->type, gy.cur_anim, int(gy.end_time) * 1000 + fr}];
				}
			}
		std::printf("anim check: %d guys, %d agree\n", total, agree);
		for (const auto& kv : bad)
			std::printf("  who %d type %d anim %d saved %d engine %d: %d guys\n", std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), std::get<3>(kv.first) / 1000, std::get<3>(kv.first) % 1000, kv.second);
	}

	if (std::getenv("RON_MASK_CHECK")) {
		// units with a given unit_masks bit (hex in RON_MASK_CHECK), by type
		const uint32_t bit = uint32_t(std::strtoul(std::getenv("RON_MASK_CHECK"), nullptr, 16));
		std::map<std::pair<int, int>, int> cnt;
		for (int who = 0; who < sim::kNumLeaders; ++who)
			for (int o = 0; o < game->objects[size_t(who)].unit_mark; ++o) {
				sim::Unit* u0 = game->unit(who, o);
				if (u0 && u0->alive() && (u0->unit_masks & bit)) ++cnt[{who, u0->type}];
			}
		for (const auto& kv : cnt) std::printf("mask %x: who %d type %d: %d\n", bit, kv.first.first, kv.first.second, kv.second);
		for (int w = 0; w < 10; ++w) {
			const sim::Leader& L = game->leaders[size_t(w)];
			std::printf("leader %d: flags %08x flags2 %08x production_ai %d prod_script_on %d age %d gov %d tribe %d opt_peasants %d\n", w, L.flags, L.flags2, L.production_ai, L.prod_script_on, L.age, L.gov, L.tribe, L.opt_peasants);
		}
		{
			std::map<std::string, int> ut, bt;
			for (int w = 0; w < 2; ++w) {
				for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) { sim::Unit* u0 = game->unit(w, o); if (u0 && u0->alive()) ut[sim::to_utf8(u0->ptype->type_name)]++; }
				for (int o = 2000; o < game->objects[size_t(w)].build_mark; ++o) { sim::Build* b0 = game->build(w, o); if (b0 && b0->alive()) bt[sim::to_utf8(b0->ptype->type_name) + (b0->flags & 4 ? "" : "(constr)")]++; }
			}
			{ int nol = 0, tot = 0; for (int w = 0; w < 10; ++w) for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) { sim::Unit* u0 = game->unit(w, o); if (u0 && u0->alive()) { ++tot; if (u0->openlist) ++nol; } } std::printf("units with openlist: %d of %d\n", nol, tot); }
			{
				std::map<std::string, int> oh;
				for (int w = 0; w < 10; ++w) for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) {
					sim::Unit* u0 = game->unit(w, o);
					if (!u0 || !u0->alive()) continue;
					const int ot = u0->orders.empty() ? -1 : int(u0->orders.front().type);
					const int ga = u0->guys.empty() ? -1 : int(u0->guys[0].cur_anim);
					oh["who " + std::to_string(w > 1 ? w : 0) + " " + sim::to_utf8(u0->ptype->type_name) + " order " + std::to_string(ot) + " anim " + std::to_string(ga)]++;
				}
				for (auto& kv : oh) std::printf("  %s: %d\n", kv.first.c_str(), kv.second);
			}
			std::printf("units:"); for (auto& kv : ut) std::printf(" %s=%d", kv.first.c_str(), kv.second); std::printf("\nbuilds:");
			for (auto& kv : bt) std::printf(" %s=%d", kv.first.c_str(), kv.second); std::printf("\n");
		}
		{
			std::map<int, int> ch;
			for (int w = 0; w < 8; ++w) for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) { sim::Unit* u0 = game->unit(w, o); if (!u0 || !u0->alive()) continue; const int age = game->frame - u0->collide_frame; if (u0->collide_frame >= 0 && age >= 0 && age < 400) ch[age / 20]++; }
			std::printf("collide_frame age histogram (per 20 frames):"); for (auto& kv : ch) std::printf(" %d:%d", kv.first * 20, kv.second); std::printf("\n");
		}
		{
			int na = 0, nd = 0;
			for (const sim::Ammo& a : game->ammo) if (a.in_use()) { ++na; std::printf("ammo piece %d who %d o %d whom %d ox %d t %u/%u\n", a.gpiece, a.who, a.o, a.whom, a.ox, a.cur_time, a.total_time); }
			for (const sim::DeathObj& d : game->deaths) if (d.valid) { ++nd; std::printf("death who %d o %d gpiece %d ammo_piece %d first_frame %d cur_frame %d at %d,%d\n", d.who, d.o, d.gpiece, d.ammo_piece, d.first_frame, d.cur_frame, d.x, d.y); }
			std::printf("ammo in use %d (of %zu), deaths valid %d\n", na, game->ammo.size(), nd);
		}
		std::printf("game: frame %d ai_speed %d editor %d scripted_rules %d\n", game->frame, game->ai_speed, int(game->editor), int(game->scripted_rules));
	}
	if (std::getenv("RON_LEADERS")) {
		for (int w = 0; w < 10; ++w) std::printf("  leader %d: flags %08x flags2 %08x tribe %d age %d prod_ai %d prod_script_on %d\n", w, game->leaders[size_t(w)].flags, game->leaders[size_t(w)].flags2, game->leaders[size_t(w)].tribe, game->leaders[size_t(w)].age, game->leaders[size_t(w)].production_ai, game->leaders[size_t(w)].prod_script_on);
	}
	// What the original's load does besides restoring bytes (leader 9's init draws, the pens' animals): RON_NO_LOAD_EFFECTS=1 skips it.
	if (std::getenv("RON_LOAD_STAGES")) {
		// the draws of each stage of original_post_load_effects (the generator distance)
		auto dist = [](uint32_t a, uint32_t b) { int64_t n = 0; for (uint32_t s = a; s != b && n < 100000; s = s * 0x19660Du + 0x3C6EF35Fu) ++n; return n; };
		const uint32_t s0 = game->rng.seed();
		game->leaders[9].flags = 0x2000007u;  // Leader::init(.., tribe -1, ..) is the reset branch: no draw
		game->leaders[9].tribe = 0;
		const uint32_t s1 = game->rng.seed();
		int pens = 0;
		for (size_t i = 0; i < game->farms.size(); ++i) if (game->farms[i].valid) { const uint32_t a = game->rng.seed(); sim::farms_add_animals(*game, int32_t(i)); if (a != game->rng.seed()) ++pens; }
		for (size_t i = 0; i < game->farms.size(); ++i) { const auto& f = game->farms[i]; const sim::Build* b = f.who >= 0 && f.o >= 0 ? game->build(f.who, f.o) : nullptr; std::printf("  farm %zu: valid %d type %d who %d o %d building %s flags %02x type %d\n", i, int(f.valid), int(f.farm_type), f.who, f.o, b ? "yes" : "no", b ? unsigned(b->flags) : 0u, b ? b->type : -1); }
		std::printf("load stages: leader 9 init %lld draws, %d pens with animals: %lld draws\n", (long long)dist(s0, s1), pens, (long long)dist(s1, game->rng.seed()));
		return 0;
	}
	if (const char* plan = std::getenv("RON_LOAD_INIT")) {
		// experiment: Leader::init-style AI draws for the listed leader slots, in order ("8,9"), instead of original_post_load_effects
		std::stringstream ps(plan);
		std::string tk;
		while (std::getline(ps, tk, ',')) {
			const int k = std::atoi(tk.c_str());
			const uint32_t fl = game->leaders[size_t(k)].flags;
			game->leaders[size_t(k)].flags = 0;
			sim::leader_init_ai(*game, k);
			game->leaders[size_t(k)].flags = fl | 7;
		}
		for (size_t i = 0; i < game->farms.size(); ++i) if (game->farms[i].valid) sim::farms_add_animals(*game, int32_t(i));
	} else if (!std::getenv("RON_NO_LOAD_EFFECTS")) sim::original_post_load_effects(*game);
	if (const char* ex = std::getenv("RON_LOAD_EXTRA")) for (int i = std::atoi(ex); i > 0; --i) game->rng.get(0, 0xffff);  // experiment: more load-time draws

	// The game script the replay header names (the embedded save has none): "no_nukes" in the 2026.08.30 chain.
	if (hdr && !std::getenv("RON_NO_SCRIPTS")) {
		const Json* hs = hdr->get("scenarioScript");
		if (hs && hs->t == Json::Str && !hs->s.empty()) {
			std::string err;
			if (sim::original_attach_game_script(*game, cache->files, hs->s, &err)) std::printf("  game script \"%s\" attached (Game::scripts created)\n", hs->s.c_str());
			else std::printf("  game script \"%s\" not attached: %s\n", hs->s.c_str(), err.c_str());
		}
	}
	auto rows = all_check_sums(csv_path, path);
	if (rows.empty()) { std::printf("replay: no CHECK_SUMS rows for %s in %s\n", basename_of(path).c_str(), csv_path.c_str()); return 1; }
	const int64_t start = game->frame;
	std::printf("%s: replay from save frame %lld, %zu CHECK_SUMS turns, %zu records, rules %s\n", basename_of(path).c_str(), (long long)start, rows.size(),
		recs ? recs->a.size() : size_t(0), rep.rules_equal ? "equal to the install's" : "adopted from the save");

	// expected value per boundary: row at record frame R describes the boundary R - turn, where turn = the gap to the previous row
	// (6 frames at game speed 2; the first row's boundary is the save frame + 1).
	std::map<int64_t, std::vector<uint32_t>> expect;  // boundary -> parts
	std::map<int64_t, int> agree;
	{
		int64_t prev = start + 1 - 6 + 6;  // first boundary
		bool first = true;
		for (const auto& kv : rows) {
			auto best = std::max_element(kv.second.begin(), kv.second.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
			const int64_t boundary = first ? start + 1 : prev;
			expect[boundary] = best->first;
			agree[boundary] = best->second;
			prev = kv.first;
			first = false;
		}
	}
	int64_t last_boundary = expect.rbegin()->first;
	if (max_frames > 0) last_boundary = std::min<int64_t>(last_boundary, start + max_frames);
	// --compare-next: run to the frame of the next replay's embedded save (the same game, saved when a player rejoined)
	std::vector<uint8_t> next_save;
	int64_t next_frame = -1;
	if (!next_path.empty()) {
		std::vector<uint8_t> raw2, buf2;
		bool gz2 = false;
		if (!read_file(next_path, raw2) || !maybe_gunzip(raw2, buf2, gz2)) { std::printf("replay: cannot read %s\n", next_path.c_str()); return 1; }
		dw::Tree h2(dw::Description::get());
		dw::DecodeResult r2 = dw::decode_function(h2, buf2.data(), buf2.size(), 0, "Game::walk_data", "game", 0x10);
		std::string nm2;
		size_t p2 = r2.ok ? dw::read_string(buf2.data(), buf2.size(), r2.end, nm2) : std::string::npos;
		if (p2 == std::string::npos || nm2.empty()) { std::printf("replay: %s has no embedded save\n", next_path.c_str()); return 1; }
		dw::Tree t2(dw::Description::get());
		dw::SaveDecode s2 = dw::decode_save(t2, buf2.data(), buf2.size(), p2);
		if (!s2.result.ok) { std::printf("replay: the next save does not decode\n"); return 1; }
		next_save.assign(buf2.begin() + std::ptrdiff_t(p2), buf2.begin() + std::ptrdiff_t(s2.end));
		next_frame = field_int(t2, "game.frame");
		last_boundary = std::max<int64_t>(last_boundary, next_frame);
		std::printf("  next replay's embedded save is at frame %lld\n", (long long)next_frame);
	}

	{
		// the state of the save itself against the first row: the parts that do not change in one frame must match already
		sim::EngineProvider ep(*game);
		dw::CheckSumParts base = dw::compute_check_sums(ep);
		const auto& e1 = expect.begin()->second;
		std::printf("  save state (frame %lld) against the first row (boundary %lld):", (long long)start, (long long)expect.begin()->first);
		for (int k = 0; k < dw::kCheckPartCount; ++k) std::printf(" %s=%s", dw::kCheckPartNames[k], base.v[k] == e1[size_t(k)] ? "=" : "!");
		std::printf("\n");
	}
#ifdef RON_RNG_PROFILE_HOOK
	if (std::getenv("RON_RNG_PROFILE")) sim::g_rng_hook = rng_hook_fn;
	if (const char* tr = std::getenv("RON_RNG_TRACE")) { g_trace_game = game.get(); g_trace_until = std::atoi(tr); }
	if (const char* in = std::getenv("RON_INSERT_N")) { g_trace_game = game.get(); std::sscanf(in, "%d:%d:%d", &g_ins_frame, &g_ins_n, &g_ins_k); }
#endif
	// RON_FISH_TIMELINE=1 (with --compare-next): see fish_timeline() below
	const bool fish_timeline = std::getenv("RON_FISH_TIMELINE") != nullptr;
	std::vector<std::pair<int, uint32_t>> frame_seeds;
	const uint32_t seed_load = game->rng.seed();
	if (fish_timeline) sim::g_idle_pick_hook = idle_pick_hook;
	// stepping
	sim::CommandPackage package;
	size_t ri = 0;
	std::map<std::string, int64_t> unsupported;
	int64_t first_unsupported_frame = -1;
	std::string first_unsupported;
	int64_t cmds_run = 0;
	std::unique_ptr<sim::Game> baseline;
	std::unique_ptr<sim::Game> baseline_search;
	if (std::getenv("RON_REVERT_SEARCH") || std::getenv("RON_BIT_SEARCH") || std::getenv("RON_FOG_DIFF")) baseline_search = sim::load_original_save(cache->files, rules, nullptr, save, &rep);
	sim::Game* baseline_for_search = baseline_search.get();
	if (!revert.empty()) {
		baseline = sim::load_original_save(cache->files, rules, nullptr, save, &rep);
		std::printf("  experiment: after the first step the members");
		for (const auto& m : revert) std::printf(" %s", m.c_str());
		std::printf(" are put back to the save's values\n");
	}
	int64_t first_diff[dw::kCheckPartCount];
	int64_t last_match[dw::kCheckPartCount];
	for (int k = 0; k < dw::kCheckPartCount; ++k) first_diff[k] = -1, last_match[k] = -1;
	int64_t boundaries = 0, all_match = 0;
	std::string step_error;
	try {
		for (int64_t f = start; f <= last_boundary; ++f) {
			if (game->frame != f) { step_error = "frame counter " + std::to_string(game->frame) + " != " + std::to_string(f); break; }
			auto it = expect.find(f);
			if (it != expect.end()) {
				if (baseline && f == start + 1) std::printf("  (reverted %lld member values)\n", (long long)sim::revert_members(*game, *baseline, revert));
				if (f == start + 1 && std::getenv("RON_GOODS_SEARCH")) {
					// experiment: which single change of a good's ever_seen makes the goods part equal the original's?
					for (size_t gi = 0; gi < game->goods.size(); ++gi) {
						if (!game->goods[gi].alive()) continue;
						const uint8_t old = game->goods[gi].ever_seen;
						for (int m : {1, 2, 3, 0xff, 0}) {
							game->goods[gi].ever_seen = m == 0 ? uint8_t(0) : uint8_t(old | m);
							if (game->goods[gi].ever_seen == old) continue;
							sim::EngineProvider ep2(*game);
							dw::CheckSumParts t = dw::compute_check_sums(ep2);
							if (t.v[10] == it->second[10]) std::printf("  GOODS HIT: good %zu type %d ever_seen %02x -> %02x\n", gi, game->goods[gi].type, old, game->goods[gi].ever_seen);
						}
						game->goods[gi].ever_seen = old;
					}
				}
				if (const char* pe = std::getenv("RON_PATCH")) {
					// experiment: RON_PATCH="Guy.des_x:1,22,0:+1024@18345" adds a delta to one member of one object at that boundary
					char key[64] = {0};
					int a = 0, b = 0, c = 0, at = 0;
					long long dl = 0;
					if (std::sscanf(pe, "%63[^:]:%d,%d,%d:%lld@%d", key, &a, &b, &c, &dl, &at) == 6 && f == at) {
						for (const auto& mc : sim::members_of(*game, {key}))
							if (mc.id.a == a && mc.id.b == b && mc.id.c == c) {
								uint64_t v = 0;
								for (size_t k = 0; k < mc.now.size() && k < 8; ++k) v |= uint64_t(mc.now[k]) << (8 * k);
								v += uint64_t(dl);
								std::vector<uint8_t> nb(mc.now.size());
								for (size_t k = 0; k < nb.size() && k < 8; ++k) nb[k] = uint8_t(v >> (8 * k));
								sim::apply_member_bytes(*game, mc, nb);
								std::printf("  patched %s of (%d,%d,%d) by %+lld\n", key, a, b, c, dl);
							}
					}
				}
				if (const char* rt = std::getenv("RON_RNG_TRY")) {
					// experiment: does the `world` / `units` / `guys` hash match when the generator is k draws ahead of / behind the engine's, at boundary RON_RNG_TRY?
					if (f == std::atoll(rt)) {
						const uint32_t a = 0x19660Du, c = 0x3C6EF35Fu, ainv = [] { uint32_t x = 0x19660Du; for (int i = 0; i < 5; ++i) x *= 2 - 0x19660Du * x; return x; }();
						const uint32_t s0 = game->rng.seed();
						for (int k = -300; k <= 300; ++k) {
							uint32_t sd = s0;
							for (int i = 0; i < std::abs(k); ++i) sd = k > 0 ? sd * a + c : (sd - c) * ainv;
							game->rng.reseed(sd);
							sim::EngineProvider er(*game);
							dw::CheckSumParts gr = dw::compute_check_sums(er);
							for (int pk : {0, 6, 11}) if (gr.ok && gr.v[pk] == it->second[size_t(pk)]) std::printf("  RNG HIT: part %s matches with the generator %d draws %s\n", dw::kCheckPartNames[pk], std::abs(k), k >= 0 ? "ahead" : "behind");
						}
						game->rng.reseed(s0 + 1);
						{ sim::EngineProvider er(*game); dw::CheckSumParts gr = dw::compute_check_sums(er); sim::EngineProvider e0(*game); (void)e0;
						  game->rng.reseed(s0); sim::EngineProvider er0(*game); dw::CheckSumParts g0 = dw::compute_check_sums(er0);
						  std::printf("  parts that include the generator state:"); for (int pk = 0; pk < dw::kCheckPartCount; ++pk) if (gr.v[pk] != g0.v[pk]) std::printf(" %s", dw::kCheckPartNames[pk]); std::printf("\n"); }
					}
				}
				if (const char* ga = std::getenv("RON_GUY_TRY")) {
					// experiment: one guy at a time, a group of members moved together by d in [-D, D] (angles / positions / speeds): does the part `guys` (or units) match at boundary RON_GUY_TRY?
					if (f == std::atoll(ga)) {
						const int D = std::getenv("RON_GUY_D") ? std::atoi(std::getenv("RON_GUY_D")) : 400;
						struct Grp { const char* name; std::vector<const char*> keys; int shift; };
						const std::vector<Grp> groups = {{"angles", {"Guy.angle", "Guy.des_angle", "Guy.last_angle"}, 16}, {"x", {"Guy.x", "Guy.des_x", "Guy.last_x"}, 0}, {"y", {"Guy.y", "Guy.des_y", "Guy.last_y"}, 0},
						                                 {"z", {"Guy.z", "Guy.last_z"}, 0}, {"speed", {"Guy.avg_speed", "Guy.last_speed"}, 0}, {"time", {"Guy.cur_time", "Guy.last_time"}, 0}, {"end", {"Guy.end_time"}, 0}};
						std::set<std::tuple<int, int, int>> movers;
						if (baseline_for_search) for (const auto& c : sim::changed_members(*game, *baseline_for_search)) if (c.key.rfind("Guy.", 0) == 0 && c.key != "Guy.cur_time" && c.key != "Guy.last_time") movers.insert({c.id.a, c.id.b, c.id.c});
						std::printf("  guy try: %zu guys moved\n", movers.size());
						int hits = 0;
						for (const auto& gr : groups) {
							std::vector<sim::MemberChange> mem;
							for (const char* k : gr.keys) for (auto& mc : sim::members_of(*game, {k})) if (movers.count({mc.id.a, mc.id.b, mc.id.c})) mem.push_back(mc);
							std::map<std::tuple<int, int, int>, std::vector<const sim::MemberChange*>> by;
							for (const auto& mc : mem) by[{mc.id.a, mc.id.b, mc.id.c}].push_back(&mc);
							for (auto& kv : by) for (int d = -D; d <= D; ++d) {
								if (d == 0) continue;
								for (const auto* mc : kv.second) {
									uint32_t v = 0;
									for (size_t k = 0; k < mc->now.size() && k < 4; ++k) v |= uint32_t(mc->now[k]) << (8 * k);
									v += uint32_t(int64_t(d) << gr.shift);
									std::vector<uint8_t> nb(mc->now.size());
									for (size_t k = 0; k < nb.size() && k < 4; ++k) nb[k] = uint8_t(v >> (8 * k));
									sim::apply_member_bytes(*game, *mc, nb);
								}
								sim::EngineProvider eg(*game);
								dw::CheckSumParts gg = dw::compute_check_sums(eg);
								for (int pk : {0, 6, 11}) if (gg.ok && gg.v[pk] == it->second[size_t(pk)]) { std::printf("  GUY HIT: part %s, group %s of guy (%d,%d,%d) by %+d\n", dw::kCheckPartNames[pk], gr.name, std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), d); ++hits; }
								for (const auto* mc : kv.second) sim::apply_member_bytes(*game, *mc, mc->now);
							}
						}
						std::printf("  guy try: %d hits\n", hits);
					}
				}
				if (std::getenv("RON_CITIZENS_AT") && f == std::atoll(std::getenv("RON_CITIZENS_AT"))) {  // the engine's citizens at that boundary: (stance, first order type, idle>=12): count
					for (int who = 0; who < 2; ++who) {
						std::map<std::tuple<int, int, int>, int> hist;
						int n = 0;
						for (int o = 0; o < game->objects[size_t(who)].unit_mark; ++o) {
							sim::Unit* u = game->unit(who, o);
							if (!u || !u->alive() || (u->type != 50 && u->type != 51)) continue;
							++n;
							hist[{u->stance, u->orders.empty() ? -1 : int(u->orders.front().type), u->idle >= 12 ? 1 : 0}]++;
							if (u->orders.empty() && std::getenv("RON_CITIZENS_LIST")) std::printf("      idle unit %d/%d at %d,%d idle %d masks %08x\n", who, o, u->x, u->y, u->idle, u->unit_masks);
						}
						std::printf("  frame %lld engine player %d: %d citizens (stance, first order type, idle>=12): count\n", (long long)f, who, n);
						for (const auto& kv : hist) std::printf("      (%d, %d, %d): %d\n", std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), kv.second);
					}
				}
				if (const char* db = std::getenv("RON_DUMP_BUILD_AT")) {  // RON_DUMP_BUILD_AT=who,o (+ RON_SEARCH_AT): the engine's building at that boundary
					if (f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {
						int dw = 0, dob = 0;
						std::sscanf(db, "%d,%d", &dw, &dob);
						if (sim::Build* b = game->build(dw, dob))
							std::printf("build %d/%d type %d (%s) frame %d: flags %02x masks %04x inside_down %d/%d launch_timer %d launch_list %zu recharging %d queued %d damage %d myhits %d city %d\n", dw, dob, b->type, b->ptype ? sim::to_utf8(b->ptype->type_name).c_str() : "?", game->frame, b->flags, unsigned(b->build_masks), b->inside_down, b->inside_down_who, b->launch_timer, b->launch_list.size(), b->recharging, b->queued, b->damage, b->myhits, b->city);
					}
				}
				if (const char* du = std::getenv("RON_DUMP_UNIT_AT")) {  // RON_DUMP_UNIT_AT=who,o (+ RON_SEARCH_AT): the engine's unit with every Unit member and its guys, at that boundary
					if (f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {
						int dw = 0, dob = 0;
						std::sscanf(du, "%d,%d", &dw, &dob);
						for (int dbi : {36, 40, 137}) if (sim::Unit* du2 = game->unit(0, dbi)) std::printf("slot 0/%d type %d ptype %d flags %x hold %d o_up %d\n", dbi, du2->type, int(du2->ptype != nullptr), du2->flags, du2->hold_frames, du2->o_up);
						std::printf("unit_mark %d/%d/%d build_mark %d\n", game->objects[0].unit_mark, game->objects[1].unit_mark, game->objects[8].unit_mark, game->objects[0].build_mark);
						if (sim::Unit* u = game->unit(dw, dob)) {
							std::printf("unit %d/%d type %d frame %d: x %d y %d z %d angle %08x dest_angle %08x flags %02x up %d down %d down_who %d inside_down %d/%d inside_up %d/%d uid %d idle %d tol %d queue_time %d masks %08x/%08x\n", dw, dob, u->type, game->frame, u->x, u->y, u->z, u->angle, u->dest_angle, u->flags, u->up, u->down, u->down_who, u->inside_down, u->inside_down_who, u->inside_up, u->inside_up_who, u->uid, u->idle, u->tolerance, u->queue_time, u->unit_masks, u->unit_masks2);
							std::printf("   orders_xy %d,%d los %d,%d group %d supply %d hero %d collide %d/%d/%d o_up %d o_down %d gather_down %d good_obj %d cavarch %d/%d/%d damage_o %d damage_who %d form %d/%d full %d waiting %d recharging %d stance %d safe %d collide_who %d guy_mark %d play %d rare %d doober %d cara %d path %zu (size %d) orders %zu\n", u->orders_x, u->orders_y, u->los_x, u->los_y, u->group, u->supply, u->hero, u->collide, u->collide_o, u->collide_guy, u->o_up, u->o_down, u->gather_down, u->good_obj, u->cavarch_o, u->cavarch_who, u->cavarch_uid, u->damage_o, u->damage_who, u->form, u->form_mod, u->full, u->waiting, u->recharging, u->stance, u->safe, u->collide_who, u->guy_mark, u->play, u->rare, u->doober, u->cara, u->path.size(), u->path_size, u->orders.size());
							std::printf("   myhits %d damage %d mylos %d myspeed %d myarmor %d healing %d near %d/%d visible %d targeted %d infiltrated %d dmgfrac %d mana_burn %d spell_time %d attrition %d num_queued %d collide_frame %d damage_frame %d announce %d air_alt %d\n", u->myhits, u->damage, u->mylos, u->myspeed, u->myarmor, u->healing, u->near_o, u->near_who, u->visible, u->targeted, u->infiltrated, u->damage_frac, u->mana_burn, u->spell_time, u->attrition, u->num_queued, u->collide_frame, u->damage_frame, u->announce_frame, u->air_alt);
							for (const sim::PathData& pd : u->path) std::printf("   path (%d,%d t%d f%x)\n", pd.x, pd.y, pd.tol, pd.flags);
							for (const sim::Order& od : u->orders) std::printf("   order type %d\n", int(od.type));
							for (const sim::Guy& gy : u->guys)
								std::printf("   guy %d: anim %d stopped %d cur_time %u end_time %u last_time %d x %d y %d z %d spd %d/%d flags %04x gpiece %d des %d,%d des_angle %08x angle %08x last %d,%d,%d off %d,%d variation %d node %d/%d hold %d whom %d queued %d ox %d\n", gy.guy_num, gy.cur_anim, gy.stopped, gy.cur_time, gy.end_time, gy.last_time, gy.x, gy.y, gy.z, gy.last_speed, gy.avg_speed, gy.guy_flags, gy.gpiece, gy.des_x, gy.des_y, gy.des_angle, gy.angle, gy.last_x, gy.last_y, gy.last_z, gy.off_x, gy.off_y, gy.variation, gy.node_flags, gy.des_node_flags, gy.hold_attack, gy.whom, gy.queued_attack, gy.ox);
						}
					}
				}
				if (const char* ps = std::getenv("RON_PAIR_SEARCH")) {
					// experiment: every small-integer member (<= 4 bytes) of the listed units (Unit.*, their order bodies, path entries) off by a delta in [-W, W] (RON_PAIR_W, default 70);
					// one or two of them together must give the target delta of part RON_PAIR_PART (default units) at boundary RON_SEARCH_AT
					if (f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {
						const int W = std::getenv("RON_PAIR_W") ? std::atoi(std::getenv("RON_PAIR_W")) : 70;
						const int part = std::getenv("RON_PAIR_PART") ? std::atoi(std::getenv("RON_PAIR_PART")) : 0;
						std::set<std::pair<int, int>> objs;
						{ std::stringstream ss(ps); std::string t; while (std::getline(ss, t, ',')) { int a = 0, b = 0; if (std::sscanf(t.c_str(), "%d/%d", &a, &b) == 2) objs.insert({a, b}); } }
						const int64_t M = 65521;
						PartTrace t0;
						{ sim::EngineProvider ep0(*game); trace_part(ep0, part, t0); }
						const int64_t N = int64_t(t0.bytes.size());
						const uint32_t h0 = adler_of(t0.bytes), want = it->second[size_t(part)];
						auto subm = [&](uint32_t x, uint32_t y) { return std::pair<int64_t, int64_t>{(int64_t(x & 0xffff) - int64_t(y & 0xffff) + M) % M, (int64_t(x >> 16) - int64_t(y >> 16) + M) % M}; };
						const auto target = subm(want, h0);
						std::vector<sim::MemberChange> all = sim::members_of(*game, {"Unit.*", "MoveOrder.*", "AttackOrder.*", "GatherOrder.*", "TargetOrder.*", "GroupOrder.*", "PathData.*"});
						struct Eff { size_t mi; int dl; int64_t a, b; };
						std::vector<Eff> effs;
						size_t tried = 0;
						for (size_t i = 0; i < all.size(); ++i) {
							const auto& c = all[i];
							if (!objs.count({c.id.a, c.id.b}) || c.now.empty() || c.now.size() > 4) continue;
							++tried;
							const size_t n = c.now.size();
							std::vector<uint8_t> inv(n);
							for (size_t k = 0; k < n; ++k) inv[k] = uint8_t(~c.now[k]);
							sim::apply_member_bytes(*game, c, inv);
							sim::EngineProvider ep3(*game);
							PartTrace t1;
							const bool ok = trace_part(ep3, part, t1) && t1.bytes.size() == t0.bytes.size();
							sim::apply_member_bytes(*game, c, c.now);
							if (!ok) continue;
							std::vector<int64_t> pos(n, -1);
							int cnt = 0;
							for (size_t q = 0; q < t0.bytes.size(); ++q) if (t0.bytes[q] != t1.bytes[q])
								for (size_t k = 0; k < n; ++k) if (t0.bytes[q] == c.now[k] && t1.bytes[q] == inv[k] && !(cnt & (1 << k))) { pos[k] = int64_t(q); cnt |= 1 << k; break; }
							if (cnt != (1 << n) - 1) continue;
							uint64_t v = 0;
							for (size_t k = 0; k < n; ++k) v |= uint64_t(c.now[k]) << (8 * k);
							const uint64_t mod = uint64_t(1) << (8 * n);
							for (int dl = -W; dl <= W; ++dl) {
								if (dl == 0) continue;
								const uint64_t nv = (v + mod + uint64_t(int64_t(dl) % int64_t(mod) + int64_t(mod))) % mod;
								int64_t sa = 0, sb = 0;
								for (size_t k = 0; k < n; ++k) { const int e = int((nv >> (8 * k)) & 0xff) - int(c.now[k]); sa += e; sb += int64_t(e) * (N - pos[k]); }
								effs.push_back({i, dl, ((sa % M) + M) % M, ((sb % M) + M) % M});
							}
						}
						std::printf("  pair search: %zu members tried, %zu (member, delta) effects; target (a=%lld b=%lld)\n", tried, effs.size(), (long long)target.first, (long long)target.second);
						auto k2 = [](int64_t a, int64_t b) { return uint64_t(a) << 32 | uint64_t(b); };
						std::unordered_map<uint64_t, std::vector<size_t>> by;
						for (size_t i = 0; i < effs.size(); ++i) by[k2(effs[i].a, effs[i].b)].push_back(i);
						int shown = 0;
						for (size_t i = 0; i < effs.size(); ++i) {
							if (effs[i].a == target.first && effs[i].b == target.second && shown++ < 20) std::printf("    PAIR HIT (1): %s (%d,%d,%d) %+d\n", all[effs[i].mi].key.c_str(), all[effs[i].mi].id.a, all[effs[i].mi].id.b, all[effs[i].mi].id.c, effs[i].dl);
							auto fnd = by.find(k2(((target.first - effs[i].a) % M + M) % M, ((target.second - effs[i].b) % M + M) % M));
							if (fnd == by.end()) continue;
							for (size_t j : fnd->second) if (j > i && effs[j].mi != effs[i].mi && shown++ < 40)
								std::printf("    PAIR HIT (2): %s (%d,%d,%d) %+d  and  %s (%d,%d,%d) %+d\n", all[effs[i].mi].key.c_str(), all[effs[i].mi].id.a, all[effs[i].mi].id.b, all[effs[i].mi].id.c, effs[i].dl,
									all[effs[j].mi].key.c_str(), all[effs[j].mi].id.a, all[effs[j].mi].id.b, all[effs[j].mi].id.c, effs[j].dl);
						}
					}
				}
				if (const char* fg = std::getenv("RON_FOG_DIFF")) {
					// diagnostic: how many fog bytes (seen / seen2 / seen3 / wcoord_seen) the engine changed since the loaded save, by bit pattern, at boundary RON_FOG_DIFF
					if (baseline_for_search && f == std::atoll(fg)) {
						const sim::World& a = baseline_for_search->world;
						const sim::World& b = game->world;
						auto cmp = [&](const char* name, const std::vector<uint8_t>& x, const std::vector<uint8_t>& y) {
							std::map<std::pair<int, int>, int> h;
							int n = 0;
							for (size_t i = 0; i < x.size() && i < y.size(); ++i) if (x[i] != y[i]) { ++n; h[{x[i], y[i]}]++; }
							std::printf("  fog %s: %d bytes changed since the save:", name, n);
							int shown = 0;
							for (const auto& kv : h) if (shown++ < 12) std::printf(" %02x->%02x x%d", kv.first.first, kv.first.second, kv.second);
							std::printf("\n");
						};
						cmp("seen", a.seen, b.seen);
						cmp("seen2", a.seen2, b.seen2);
						cmp("seen3", a.seen3, b.seen3);
						cmp("wcoord_seen", a.wcoord_seen, b.wcoord_seen);
						int nw = 0;
						for (size_t i = 0; i < a.wdata.size() && i < b.wdata.size(); ++i) if (a.wdata[i].was_seen != b.wdata[i].was_seen) ++nw;
						std::printf("  WData.was_seen changed in %d wcoords\n", nw);
					}
				}
				if (const char* cm = std::getenv("RON_COLL_MAP")) {
					// diagnostic: engine collision bits against the union of the squad guys' footprints, around ucoord x0,y0 (+-12); '.' clear, '#' set and ideal, 'S' stale (set, not ideal), 'M' missing (ideal, clear)
					int cx = 0, cy = 0, at = 0;
					if (std::sscanf(cm, "%d,%d@%d", &cx, &cy, &at) == 3 && f == at) {
						std::set<std::pair<int, int>> ideal;
						for (int who = 0; who < sim::kNumLeaders; ++who)
							for (int o = 0; o < game->objects[size_t(who)].unit_mark; ++o) {
								sim::Unit* u = game->unit(who, o);
								if (!u || !u->alive() || !u->on_map()) continue;
								const int r = u->ptype->new_block_radius;
								if (r == 0) continue;
								for (int i = 0; i < u->guy_mark && size_t(i) < u->guys.size(); ++i)
									for (int dx = -r; dx <= r; ++dx) for (int dy = -r; dy <= r; ++dy) ideal.insert({sim::ucoord_of(u->guys[size_t(i)].x) + dx, sim::ucoord_of(u->guys[size_t(i)].y) + dy});
							}
						int stale = 0, missing = 0;
						for (int y = cy - 12; y <= cy + 12; ++y) {
							std::printf("   %5d ", y);
							for (int x = cx - 12; x <= cx + 12; ++x) {
								const bool sset = game->world.coll_get(x, y), id = ideal.count({x, y}) != 0;
								std::printf("%c", sset && id ? '#' : sset ? 'S' : id ? 'M' : '.');
								stale += sset && !id;
								missing += !sset && id;
							}
							std::printf("\n");
						}
						std::printf("  coll map around %d,%d at frame %lld: %d stale, %d missing (this window)\n", cx, cy, (long long)f, stale, missing);
					}
				}
				if (const char* bs = std::getenv("RON_BIT_SEARCH")) {
					// experiment: single collision-bit flips (within R ucoords of every unit that moved since the save) as the explanation of a `world` mismatch:
					// 1 to 3 flips whose Adler effect equals the target delta
					if (baseline_for_search && f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {
						const int R = std::atoi(bs);
						const int part = 11;
						const int64_t M = 65521;
						std::set<std::pair<int, int>> pool;
						for (int who = 0; who < sim::kNumLeaders; ++who)
							for (int o = 0; o < game->objects[size_t(who)].unit_mark; ++o) {
								sim::Unit* a = game->unit(who, o);
								sim::Unit* b0 = baseline_for_search->unit(who, o);
								if (!a || !b0 || !a->alive() || (a->x == b0->x && a->y == b0->y)) continue;
								for (const sim::Unit* uu : {a, b0}) for (int dx = -R; dx <= R; ++dx) for (int dy = -R; dy <= R; ++dy) pool.insert({sim::ucoord_of(uu->x) + dx, sim::ucoord_of(uu->y) + dy});
							}
						PartTrace t0;
						{ sim::EngineProvider ep0(*game); trace_part(ep0, part, t0); }
						const uint32_t h0 = adler_of(t0.bytes), want = it->second[size_t(part)];
						auto sub = [&](uint32_t x, uint32_t y) { return std::pair<int64_t, int64_t>{(int64_t(x & 0xffff) - int64_t(y & 0xffff) + M) % M, (int64_t(x >> 16) - int64_t(y >> 16) + M) % M}; };
						const auto target = sub(want, h0);
						struct Eff { int ux, uy; int64_t a, b; };
						std::vector<Eff> effs;
						for (const auto& q : pool) {
							if (!game->world.in_u(q.first, q.second)) continue;
							const bool was = game->world.coll_get(q.first, q.second);
							if (!was && !game->world.coll_block(q.first >> 4, q.second >> 4)) continue;
							game->world.coll_set(q.first, q.second, !was);
							sim::EngineProvider ep1(*game);
							PartTrace t1;
							const bool ok = trace_part(ep1, part, t1) && t1.bytes.size() == t0.bytes.size();
							game->world.coll_set(q.first, q.second, was);
							if (!ok) continue;
							const auto e = sub(adler_of(t1.bytes), h0);
							effs.push_back({q.first, q.second, e.first, e.second});
						}
						std::printf("  bit search: %zu flips in a pool of %zu ucoords; target (a=%lld b=%lld)\n", effs.size(), pool.size(), (long long)target.first, (long long)target.second);
						for (const auto& e : effs) if (e.a == target.first && e.b == target.second) std::printf("    BIT HIT (1): %d,%d\n", e.ux, e.uy);
						std::unordered_map<uint64_t, std::vector<std::pair<size_t, size_t>>> pr;
						auto k2 = [](int64_t a, int64_t b) { return uint64_t(a) << 32 | uint64_t(b); };
						for (size_t i = 0; i < effs.size(); ++i) for (size_t j = i + 1; j < effs.size(); ++j) pr[k2((effs[i].a + effs[j].a) % M, (effs[i].b + effs[j].b) % M)].push_back({i, j});
						int shown = 0;
						auto it2 = pr.find(k2(target.first, target.second));
						if (it2 != pr.end()) for (auto& pq : it2->second) if (shown++ < 10) std::printf("    BIT HIT (2): %d,%d + %d,%d\n", effs[pq.first].ux, effs[pq.first].uy, effs[pq.second].ux, effs[pq.second].uy);
						for (size_t k = 0; k < effs.size() && shown < 30; ++k) {
							auto f3 = pr.find(k2(((target.first - effs[k].a) % M + M) % M, ((target.second - effs[k].b) % M + M) % M));
							if (f3 == pr.end()) continue;
							for (auto& pq : f3->second) if (pq.first > k && shown++ < 30) std::printf("    BIT HIT (3): %d,%d + %d,%d + %d,%d\n", effs[k].ux, effs[k].uy, effs[pq.first].ux, effs[pq.first].uy, effs[pq.second].ux, effs[pq.second].uy);
						}
					}
				}
				if (std::getenv("RON_REVERT_SEARCH") && baseline_for_search && f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {
					// experiment: which single (object, member) put back to the save's value makes a differing part equal the original's?
					const int part = std::atoi(std::getenv("RON_REVERT_SEARCH"));  // 0 units, 1 builds, 6 guys, ...
					std::vector<sim::MemberChange> ch = sim::changed_members(*game, *baseline_for_search);
					if (const char* inc = std::getenv("RON_REVERT_INCLUDE")) {  // comma list of exact keys: only those stay in the pool
						std::vector<std::string> keep;
						std::stringstream ss(inc);
						std::string t;
						while (std::getline(ss, t, ',')) keep.push_back(t);
						ch.erase(std::remove_if(ch.begin(), ch.end(), [&](const sim::MemberChange& c) { return std::find(keep.begin(), keep.end(), c.key) == keep.end(); }), ch.end());
					}
					if (const char* ex = std::getenv("RON_REVERT_EXCLUDE")) {  // comma list of class prefixes ("Guy.") left out of the pool
						std::vector<std::string> pre;
						std::stringstream ss(ex);
						std::string t;
						while (std::getline(ss, t, ',')) pre.push_back(t);
						ch.erase(std::remove_if(ch.begin(), ch.end(), [&](const sim::MemberChange& c) {
							for (const auto& q : pre) if (c.key.compare(0, q.size(), q) == 0) return true;
							return false;
						}), ch.end());
					}
					std::printf("  revert search over %zu changed (object, member) pairs for part %s\n", ch.size(), dw::kCheckPartNames[part]);
					PartTrace t0;
					{
						sim::EngineProvider ep0(*game);
						trace_part(ep0, part, t0);
					}
					const uint32_t h0 = adler_of(t0.bytes), want = it->second[size_t(part)];
					const int64_t M = 65521;
					auto sub = [&](uint32_t x, uint32_t y) { return std::pair<int64_t, int64_t>{(int64_t(x & 0xffff) - int64_t(y & 0xffff) + M) % M, (int64_t(x >> 16) - int64_t(y >> 16) + M) % M}; };
					const auto target = sub(want, h0);
					std::vector<std::pair<int64_t, int64_t>> eff(ch.size());
					std::vector<char> usable(ch.size(), 0);
					for (size_t i = 0; i < ch.size(); ++i) {
						sim::apply_member(*game, ch[i], true);
						sim::EngineProvider ep2(*game);
						PartTrace tr;
						if (trace_part(ep2, part, tr) && tr.bytes.size() == t0.bytes.size()) {
							eff[i] = sub(adler_of(tr.bytes), h0);
							usable[i] = eff[i].first != 0 || eff[i].second != 0;
							if (eff[i] == target) std::printf("    HIT (1): reverting %s of object (%d,%d,%d)\n", ch[i].key.c_str(), ch[i].id.a, ch[i].id.b, ch[i].id.c);
						}
						sim::apply_member(*game, ch[i], false);
					}
					std::printf("  target delta (a=%lld b=%lld); effects computed\n", (long long)target.first, (long long)target.second);
					auto key = [&](std::pair<int64_t, int64_t> v) { return uint64_t(v.first) << 32 | uint64_t(v.second); };
					auto addp = [&](std::pair<int64_t, int64_t> x, std::pair<int64_t, int64_t> y) { return std::pair<int64_t, int64_t>{(x.first + y.first) % M, (x.second + y.second) % M}; };
					auto subp = [&](std::pair<int64_t, int64_t> x, std::pair<int64_t, int64_t> y) { return std::pair<int64_t, int64_t>{(x.first - y.first + M) % M, (x.second - y.second + M) % M}; };
					// Each changed member as a small integer: where its bytes sit in the stream, then the deltas that would fix the hash.
					if (std::getenv("RON_DELTA_SEARCH")) {
						const int D = std::atoi(std::getenv("RON_DELTA_SEARCH"));
						const int64_t N = int64_t(t0.bytes.size());
						int found = 0;
						for (size_t i = 0; i < ch.size(); ++i) {
							const auto& c = ch[i];
							const size_t n = c.now.size();
							if (n == 0 || n > 8) continue;
							std::vector<uint8_t> inv(n);
							for (size_t k = 0; k < n; ++k) inv[k] = uint8_t(~c.now[k]);
							sim::apply_member_bytes(*game, c, inv);
							sim::EngineProvider ep3(*game);
							PartTrace t1;
							const bool ok = trace_part(ep3, part, t1) && t1.bytes.size() == t0.bytes.size();
							sim::apply_member_bytes(*game, c, c.now);
							if (!ok) continue;
							std::vector<int64_t> pos(n, -1);
							bool clean = true;
							for (size_t q = 0; q < t0.bytes.size(); ++q)
								if (t0.bytes[q] != t1.bytes[q]) {
									size_t k = 0;
									for (; k < n; ++k) if (t0.bytes[q] == c.now[k] && t1.bytes[q] == inv[k] && pos[k] < 0) break;
									if (k == n) { clean = false; break; }
									pos[k] = int64_t(q);
								}
							if (!clean) continue;
							uint64_t v = 0;
							for (size_t k = 0; k < n; ++k) v |= uint64_t(c.now[k]) << (8 * k);
							const uint64_t mod = n == 8 ? 0 : (uint64_t(1) << (8 * n));
							for (int dl = -D; dl <= D; ++dl) {
								if (dl == 0) continue;
								const uint64_t nv = n == 8 ? v + uint64_t(int64_t(dl)) : (v + mod + uint64_t(int64_t(dl) % int64_t(mod)) ) % mod;
								int64_t sa = 0, sb = 0;
								for (size_t k = 0; k < n; ++k) {
									if (pos[k] < 0) continue;
									const int e = int((nv >> (8 * k)) & 0xff) - int(c.now[k]);
									sa += e;
									sb += int64_t(e) * (N - pos[k]);
								}
								if (((sa % M) + M) % M == target.first && ((sb % M) + M) % M == target.second) {
									std::printf("    DELTA HIT: %s of object (%d,%d,%d) %+d (engine %llu -> original %llu)\n", c.key.c_str(), c.id.a, c.id.b, c.id.c, dl, (unsigned long long)v, (unsigned long long)nv);
									++found;
								}
							}
						}
						std::printf("  delta search: %d hits over %zu members\n", found, ch.size());
					}
					// Several small deltas at once: every changed member of the pool with a delta in [-W, W], two or three of them together.
					if (std::getenv("RON_MULTI_DELTA")) {
						const int W = std::atoi(std::getenv("RON_MULTI_DELTA"));
						const int64_t N = int64_t(t0.bytes.size());
						struct Eff { size_t mi; int dl; int64_t a, b; };
						std::vector<Eff> effs;
						for (size_t i = 0; i < ch.size(); ++i) {
							const auto& c = ch[i];
							const size_t n = c.now.size();
							if (n == 0 || n > 4) continue;
							std::vector<uint8_t> inv(n);
							for (size_t k = 0; k < n; ++k) inv[k] = uint8_t(~c.now[k]);
							sim::apply_member_bytes(*game, c, inv);
							sim::EngineProvider ep3(*game);
							PartTrace t1;
							const bool ok = trace_part(ep3, part, t1) && t1.bytes.size() == t0.bytes.size();
							sim::apply_member_bytes(*game, c, c.now);
							if (!ok) continue;
							std::vector<int64_t> pos(n, -1);
							int cnt = 0;
							for (size_t q = 0; q < t0.bytes.size(); ++q) if (t0.bytes[q] != t1.bytes[q])
								for (size_t k = 0; k < n; ++k) if (t0.bytes[q] == c.now[k] && t1.bytes[q] == inv[k] && !(cnt & (1 << k))) { pos[k] = int64_t(q); cnt |= 1 << k; break; }
							if (cnt != (1 << n) - 1) continue;
							uint64_t v = 0;
							for (size_t k = 0; k < n; ++k) v |= uint64_t(c.now[k]) << (8 * k);
							const uint64_t mod = uint64_t(1) << (8 * n);
							for (int dl = -W; dl <= W; ++dl) {
								if (dl == 0) continue;
								const uint64_t nv = (v + mod + uint64_t(int64_t(dl) % int64_t(mod) + int64_t(mod))) % mod;
								int64_t sa = 0, sb = 0;
								for (size_t k = 0; k < n; ++k) {
									const int e = int((nv >> (8 * k)) & 0xff) - int(c.now[k]);
									sa += e;
									sb += int64_t(e) * (N - pos[k]);
								}
								effs.push_back({i, dl, ((sa % M) + M) % M, ((sb % M) + M) % M});
							}
						}
						std::printf("  multi-delta: %zu (member, delta) effects\n", effs.size());
						std::unordered_map<uint64_t, std::vector<size_t>> by;
						auto k2 = [](int64_t a, int64_t b) { return uint64_t(a) << 32 | uint64_t(b); };
						for (size_t i = 0; i < effs.size(); ++i) by[k2(effs[i].a, effs[i].b)].push_back(i);
						int shown = 0;
						for (size_t i = 0; i < effs.size() && shown < 12; ++i) {
							auto f = by.find(k2(((target.first - effs[i].a) % M + M) % M, ((target.second - effs[i].b) % M + M) % M));
							if (f == by.end()) continue;
							for (size_t j : f->second) if (j > i && effs[j].mi != effs[i].mi && shown < 12) {
								std::printf("    MULTI HIT (2): %s (%d,%d,%d) %+d  and  %s (%d,%d,%d) %+d\n", ch[effs[i].mi].key.c_str(), ch[effs[i].mi].id.a, ch[effs[i].mi].id.b, ch[effs[i].mi].id.c, effs[i].dl,
									ch[effs[j].mi].key.c_str(), ch[effs[j].mi].id.a, ch[effs[j].mi].id.b, ch[effs[j].mi].id.c, effs[j].dl);
								++shown;
							}
						}
						if (std::getenv("RON_MULTI3")) {
							std::unordered_map<uint64_t, std::pair<size_t, size_t>> pr;
							for (size_t i = 0; i < effs.size(); ++i) for (size_t j = i + 1; j < effs.size(); ++j) if (effs[i].mi != effs[j].mi)
								pr[k2((effs[i].a + effs[j].a) % M, (effs[i].b + effs[j].b) % M)] = {i, j};
							for (size_t k = 0; k < effs.size() && shown < 24; ++k) {
								auto f = pr.find(k2(((target.first - effs[k].a) % M + M) % M, ((target.second - effs[k].b) % M + M) % M));
								if (f == pr.end()) continue;
								const auto [i, j] = f->second;
								if (k > j && effs[k].mi != effs[i].mi && effs[k].mi != effs[j].mi) {
									std::printf("    MULTI HIT (3): %s (%d,%d,%d) %+d, %s (%d,%d,%d) %+d, %s (%d,%d,%d) %+d\n", ch[effs[i].mi].key.c_str(), ch[effs[i].mi].id.a, ch[effs[i].mi].id.b, ch[effs[i].mi].id.c, effs[i].dl,
										ch[effs[j].mi].key.c_str(), ch[effs[j].mi].id.a, ch[effs[j].mi].id.b, ch[effs[j].mi].id.c, effs[j].dl, ch[effs[k].mi].key.c_str(), ch[effs[k].mi].id.a, ch[effs[k].mi].id.b, ch[effs[k].mi].id.c, effs[k].dl);
									++shown;
								}
							}
						}
					}
					// Every (object, member) of a class as a small integer off by up to W: one trace per member kind with all objects inverted at once.
					if (const char* ad = std::getenv("RON_ALL_DELTA")) {
						const int W = std::atoi(ad);
						const char* pref = std::getenv("RON_ALL_PREFIX") ? std::getenv("RON_ALL_PREFIX") : "Unit.*";
						const int64_t N = int64_t(t0.bytes.size());
						std::vector<sim::MemberChange> all = sim::members_of(*game, {pref});
						std::map<std::string, std::vector<const sim::MemberChange*>> by_key;
						for (const auto& c : all) if (c.now.size() > 0 && c.now.size() <= 4) by_key[c.key].push_back(&c);
						int found = 0;
						for (auto& kv : by_key) {
							const size_t n = kv.second.front()->now.size();
							// invert this member in all objects
							for (const auto* c : kv.second) { std::vector<uint8_t> inv(n); for (size_t k = 0; k < n; ++k) inv[k] = uint8_t(~c->now[k]); sim::apply_member_bytes(*game, *c, inv); }
							sim::EngineProvider ep3(*game);
							PartTrace t1;
							const bool okt = trace_part(ep3, part, t1) && t1.bytes.size() == t0.bytes.size();
							for (const auto* c : kv.second) sim::apply_member_bytes(*game, *c, c->now);
							if (!okt) continue;
							// positions of changed bytes grouped by object (who, o) of the ranges
							std::map<std::pair<int, int>, std::vector<int64_t>> pos;
							size_t ri = 0;
							for (size_t q = 0; q < t0.bytes.size(); ++q) {
								while (ri + 1 < t0.ranges.size() && t0.ranges[ri].begin + t0.ranges[ri].len <= q) ++ri;
								if (t0.bytes[q] != t1.bytes[q]) pos[{t0.ranges[ri].who, t0.ranges[ri].o}].push_back(int64_t(q));
							}
							for (const auto* c : kv.second) {
								auto it6 = pos.find({c->id.a, c->id.b});
								if (it6 == pos.end() || it6->second.size() != n) continue;
								uint64_t v = 0;
								for (size_t k = 0; k < n; ++k) v |= uint64_t(c->now[k]) << (8 * k);
								const uint64_t mod = uint64_t(1) << (8 * n);
								for (int dl = -W; dl <= W; ++dl) {
									if (dl == 0) continue;
									const uint64_t nv = (v + mod + uint64_t(int64_t(dl) % int64_t(mod) + int64_t(mod))) % mod;
									int64_t sa = 0, sb = 0;
									for (size_t k = 0; k < n; ++k) {
										const int e = int((nv >> (8 * k)) & 0xff) - int(c->now[k]);
										sa += e;
										sb += int64_t(e) * (N - it6->second[k]);
									}
									if (((sa % M) + M) % M == target.first && ((sb % M) + M) % M == target.second) {
										std::printf("    ALL-DELTA HIT: %s of object (%d,%d,%d) %+d (engine %llu -> original %llu)\n", c->key.c_str(), c->id.a, c->id.b, c->id.c, dl, (unsigned long long)v, (unsigned long long)nv);
										++found;
									}
								}
							}
						}
						std::printf("  all-delta search over %zu member kinds: %d hits\n", by_key.size(), found);
					}
					// One guy at another place: x / des_x / last_x moved by dx and y / des_y / last_y by dy (subsets of them).
					if (std::getenv("RON_CLUSTER_SEARCH")) {
						const int R = std::atoi(std::getenv("RON_CLUSTER_SEARCH"));
						const int64_t N = int64_t(t0.bytes.size());
						std::vector<std::string> keys = {"Guy.x", "Guy.y", "Guy.des_x", "Guy.des_y", "Guy.last_x", "Guy.last_y"};
						std::vector<sim::MemberChange> all = sim::members_of(*game, keys);
						std::map<std::tuple<int, int, int>, std::vector<const sim::MemberChange*>> by_obj;
						std::set<std::tuple<int, int, int>> moved;
						for (const auto& c : ch) if (c.key.rfind("Guy.", 0) == 0) moved.insert({c.id.a, c.id.b, c.id.c});
						for (const auto& c : all) if (moved.count({c.id.a, c.id.b, c.id.c})) by_obj[{c.id.a, c.id.b, c.id.c}].push_back(&c);
						int found = 0;
						std::printf("  cluster search over %zu guys\n", by_obj.size());
						for (auto& kv : by_obj) {
							struct Mem { int kind; int64_t pos[4]; uint32_t v; bool ok; };
							std::vector<Mem> mm;
							for (const sim::MemberChange* c : kv.second) {
								Mem m{};
								m.ok = false;
								if (c->now.size() != 4) continue;
								const std::string& k = c->key;
								m.kind = k == "Guy.x" ? 0 : k == "Guy.des_x" ? 1 : k == "Guy.last_x" ? 2 : k == "Guy.y" ? 3 : k == "Guy.des_y" ? 4 : 5;
								std::vector<uint8_t> inv(4);
								for (int q = 0; q < 4; ++q) inv[size_t(q)] = uint8_t(~c->now[size_t(q)]);
								sim::apply_member_bytes(*game, *c, inv);
								sim::EngineProvider ep3(*game);
								PartTrace t1;
								const bool okt = trace_part(ep3, part, t1) && t1.bytes.size() == t0.bytes.size();
								sim::apply_member_bytes(*game, *c, c->now);
								if (!okt) continue;
								int cnt = 0;
								for (size_t q = 0; q < t0.bytes.size(); ++q) if (t0.bytes[q] != t1.bytes[q]) {
									for (int z = 0; z < 4; ++z) if (t0.bytes[q] == c->now[size_t(z)] && t1.bytes[q] == inv[size_t(z)] && !(cnt & (1 << z))) { m.pos[z] = int64_t(q); cnt |= 1 << z; break; }
								}
								m.v = uint32_t(c->now[0] | c->now[1] << 8 | c->now[2] << 16 | uint32_t(c->now[3]) << 24);
								m.ok = cnt == 15;
								if (m.ok) mm.push_back(m);
							}
							auto effect = [&](const Mem& m, int dv) {
								const uint32_t nv = m.v + uint32_t(dv);
								int64_t sa = 0, sb = 0;
								for (int z = 0; z < 4; ++z) {
									const int e = int((nv >> (8 * z)) & 0xff) - int((m.v >> (8 * z)) & 0xff);
									sa += e;
									sb += int64_t(e) * (N - m.pos[z]);
								}
								return std::pair<int64_t, int64_t>{((sa % M) + M) % M, ((sb % M) + M) % M};
							};
							// effects of shifting a subset of the x-kind (kinds 0..2) or y-kind (3..5) members
							auto subset_effects = [&](int base) {
								std::vector<std::tuple<int, int, std::pair<int64_t, int64_t>>> out;  // subset mask, shift, effect
								for (int mask = 1; mask < 8; ++mask)
									for (int dv = -R; dv <= R; ++dv) {
										if (dv == 0) continue;
										std::pair<int64_t, int64_t> e{0, 0};
										bool ok = true;
										for (int b = 0; b < 3; ++b) if (mask & (1 << b)) {
											const Mem* mp = nullptr;
											for (const Mem& m : mm) if (m.kind == base + b) mp = &m;
											if (!mp) { ok = false; break; }
											const auto ee = effect(*mp, dv);
											e = {(e.first + ee.first) % M, (e.second + ee.second) % M};
										}
										if (ok) out.push_back({mask, dv, e});
									}
								return out;
							};
							if (std::getenv("RON_CLUSTER_VERBOSE")) std::printf("    guy (%d,%d,%d): %zu of %zu members located\n", std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), mm.size(), kv.second.size());
							const auto ex = subset_effects(0), ey = subset_effects(3);
							auto keyp = [](std::pair<int64_t, int64_t> v) { return uint64_t(v.first) << 32 | uint64_t(v.second); };
							std::unordered_map<uint64_t, size_t> ymap;
							for (size_t i = 0; i < ey.size(); ++i) ymap[keyp(std::get<2>(ey[i]))] = i;
							auto report = [&](int mx, int dx, int my, int dy) {
								std::printf("    CLUSTER HIT: guy (%d,%d,%d) x-members %d shifted %+d, y-members %d shifted %+d\n", std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), mx, dx, my, dy);
								++found;
							};
							for (const auto& e1 : ex) {
								if (std::get<2>(e1) == target) report(std::get<0>(e1), std::get<1>(e1), 0, 0);
								const std::pair<int64_t, int64_t> need{((target.first - std::get<2>(e1).first) % M + M) % M, ((target.second - std::get<2>(e1).second) % M + M) % M};
								auto it5 = ymap.find(keyp(need));
								if (it5 != ymap.end()) report(std::get<0>(e1), std::get<1>(e1), std::get<0>(ey[it5->second]), std::get<1>(ey[it5->second]));
							}
							for (const auto& e2 : ey) if (std::get<2>(e2) == target) report(0, 0, std::get<0>(e2), std::get<1>(e2));
						}
						std::printf("  cluster search: %d hits\n", found);
					}
					std::unordered_map<uint64_t, std::vector<size_t>> single;
					for (size_t i = 0; i < ch.size(); ++i) if (usable[i]) single[key(eff[i])].push_back(i);
					int shown2 = 0;
					for (size_t i = 0; i < ch.size() && shown2 < 20; ++i) {
						if (!usable[i]) continue;
						auto it2 = single.find(key(subp(target, eff[i])));
						if (it2 == single.end()) continue;
						for (size_t j : it2->second) if (j > i && shown2++ < 20)
							std::printf("    HIT (2): revert %s (%d,%d,%d) + %s (%d,%d,%d)\n", ch[i].key.c_str(), ch[i].id.a, ch[i].id.b, ch[i].id.c, ch[j].key.c_str(), ch[j].id.a, ch[j].id.b, ch[j].id.c);
					}
					std::unordered_map<uint64_t, std::pair<size_t, size_t>> pairs;
					for (size_t i = 0; i < ch.size(); ++i) if (usable[i]) for (size_t j = i + 1; j < ch.size(); ++j) if (usable[j]) pairs[key(addp(eff[i], eff[j]))] = {i, j};
					int shown3 = 0;
					for (size_t k = 0; k < ch.size() && shown3 < 20; ++k) {
						if (!usable[k]) continue;
						auto it3 = pairs.find(key(subp(target, eff[k])));
						if (it3 == pairs.end()) continue;
						const auto [i, j] = it3->second;
						if (i == k || j == k) continue;
						if (k < i && shown3++ < 20)
							std::printf("    HIT (3): revert %s (%d,%d,%d) + %s (%d,%d,%d) + %s (%d,%d,%d)\n", ch[k].key.c_str(), ch[k].id.a, ch[k].id.b, ch[k].id.c, ch[i].key.c_str(), ch[i].id.a, ch[i].id.b, ch[i].id.c, ch[j].key.c_str(), ch[j].id.a, ch[j].id.b, ch[j].id.c);
					}
					int shown4 = 0;
					for (const auto& kv : pairs) {
						if (shown4 >= 10) break;
						const auto p1 = std::pair<int64_t, int64_t>{int64_t(kv.first >> 32), int64_t(kv.first & 0xffffffffu)};
						auto it4 = pairs.find(key(subp(target, p1)));
						if (it4 == pairs.end()) continue;
						const auto a = kv.second, b = it4->second;
						if (a.first < b.first && a.first != b.second && a.second != b.first && a.second != b.second && shown4++ < 10)
							std::printf("    HIT (4): %s(%d,%d,%d) %s(%d,%d,%d) %s(%d,%d,%d) %s(%d,%d,%d)\n", ch[a.first].key.c_str(), ch[a.first].id.a, ch[a.first].id.b, ch[a.first].id.c, ch[a.second].key.c_str(), ch[a.second].id.a, ch[a.second].id.b, ch[a.second].id.c, ch[b.first].key.c_str(), ch[b.first].id.a, ch[b.first].id.b, ch[b.first].id.c, ch[b.second].key.c_str(), ch[b.second].id.a, ch[b.second].id.b, ch[b.second].id.c);
					}
				}
				if (const char* tp = std::getenv("RON_TRY_PATH")) {
					// experiment: variants of one unit's path stack (every subset of its entries x flags 0 / 2 / as is) against the expected `units` hash at boundary RON_SEARCH_AT (default: the first)
					int tw = 0, to = 0;
					std::sscanf(tp, "%d,%d", &tw, &to);
					if (f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {
						sim::Unit* tu = game->unit(tw, to);
						if (tu) {
							const std::vector<sim::PathData> orig = tu->path;
							const size_t n = orig.size();
							int hits = 0;
							for (uint32_t mask = 0; mask < (1u << n); ++mask)
								for (int fv = 0; fv < 3; ++fv) {
									std::vector<sim::PathData> v;
									for (size_t i = 0; i < n; ++i) if (mask >> i & 1) {
										sim::PathData pd = orig[i];
										if (fv == 1 && i > 0) pd.flags = 0;
										if (fv == 2 && i > 0) pd.flags = 2;
										v.push_back(pd);
									}
									tu->path = v;
									sim::EngineProvider ept(*game);
									dw::CheckSumParts gt = dw::compute_check_sums(ept);
									if (gt.ok && gt.v[0] == it->second[0]) { std::printf("  PATH HIT: mask %x flags-variant %d\n", mask, fv); ++hits; }
								}
							tu->path = orig;
							std::printf("  path variants of %d/%d tried (%zu entries): %d hits\n", tw, to, n, hits);
						}
					}
				}
				sim::EngineProvider ep(*game);
				dw::CheckSumParts got = dw::compute_check_sums(ep);
				if (!got.ok) { step_error = "check sums: " + got.error; break; }
				++boundaries;
				int m = 0;
				for (int k = 0; k < dw::kCheckPartCount; ++k) {
					const bool same = got.v[k] == it->second[size_t(k)];
					m += same;
					if (same) { if (first_diff[k] < 0) last_match[k] = f; }
					else if (first_diff[k] < 0) first_diff[k] = f;
					if (verbose && !same) std::printf("  frame %lld part %-16s engine %10u expected %10u\n", (long long)f, dw::kCheckPartNames[k], got.v[k], it->second[size_t(k)]);
				}
				all_match += m == dw::kCheckPartCount;
				if (f == g_solve_at) {
					for (int k : {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11}) {
						if (got.v[k] == it->second[size_t(k)]) continue;
						PartTrace tr;
						if (trace_part(ep, k, tr)) solve_single_byte(tr, it->second[size_t(k)], dw::kCheckPartNames[k]);
						if (std::getenv("RON_OBJ_LIST")) {  // (who, o) of every object the part's stream walked, with its first byte offset
							std::set<std::pair<int, int>> seen_o;
							for (const auto& r : tr.ranges) if (seen_o.insert({r.who, r.o}).second && (r.who == 0 || r.who == 1)) std::printf("      obj %d/%d at %zu\n", r.who, r.o, r.begin);
						}
						if (const char* ol = std::getenv("RON_OBJ_LAYOUT")) {  // the hashed ranges of one object: step kind, constants (offset / size), bytes
							int lw = 0, lo = 0;
							std::sscanf(ol, "%d,%d", &lw, &lo);
							const dw::Description& dd = dw::Description::get();
							for (const auto& r : tr.ranges) if (r.who == lw && r.o == lo) {
								const int fnc = dd.step_function(r.step);
								std::printf("      layout %s step %d (%s) kind %d a %lld b %lld len %zu:", fnc >= 0 ? dd.function_name(fnc) : "?", r.step, dd.step_kind(r.step) == dw::StepKind::Local ? dd.local_name(r.step) : "", int(dd.step_kind(r.step)),
									(long long)dd.step_const(r.step, 0), (long long)dd.step_const(r.step, 1), r.len);
								for (size_t q = 0; q < r.len && q < 40; ++q) std::printf(" %02x", tr.bytes[r.begin + q]);
								std::printf("\n");
							}
						}
						if (const char* tl = std::getenv("RON_TAIL")) {  // the labelled ranges within the last N bytes of the part's stream
							const size_t n = tr.bytes.size(), lim = size_t(std::atoi(tl));
							const dw::Description& dd = dw::Description::get();
							for (const auto& r : tr.ranges) if (n - r.begin <= lim) {
								const int fnc = dd.step_function(r.step);
								std::printf("      tail %s: N-i %zu len %zu who %d o %d fn %s\n", dw::kCheckPartNames[k], n - r.begin, r.len, r.who, r.o, fnc >= 0 ? dd.function_name(fnc) : "?");
							}
						}
					}
				}
			}
			if (g_pshift_at >= 0 && f >= g_pshift_at - 8 && f <= g_pshift_at + 8) {
				std::map<std::pair<std::tuple<int, int, int>, std::string>, std::vector<uint8_t>> snap;
				for (const auto& mc : sim::members_of(*game, {"Guy.*"})) snap[{{mc.id.a, mc.id.b, mc.id.c}, mc.key}] = mc.now;
				g_pshift_vals[f] = std::move(snap);
				if (f == g_pshift_at + 8) partial_shift_search(*game, g_pshift_at, expect.at(g_pshift_at)[6]);
			}
			if (g_shift_part >= 0 && g_shift_at >= 0 && f >= g_shift_at - 10 && f <= g_shift_at + 10) {
				sim::EngineProvider eps(*game);
				PartTrace tr;
				if (trace_part(eps, g_shift_part, tr)) g_shift_traces[f] = std::move(tr);
				if (f == g_shift_at + 10 && it != expect.end()) {}
				auto bt = expect.find(g_shift_at);
				if (f == g_shift_at + 10 && bt != expect.end()) shift_search(bt->second[size_t(g_shift_part)]);
			}
			// the records of this frame, before the step
			while (recs && ri < recs->a.size() && recs->a[ri].num("frame") <= f) {
				const Json& rec = recs->a[ri++];
				const int play = int(rec.num("play"));
				const int who = seat_who.count(play) ? seat_who[play] : play;
				package.group = -1;
				const Json* cm = rec.get("cmds");
				if (!cm) continue;
				for (const Json& c : cm->a) {
					ReplayCommand rc = to_command(c);
					static const char* ignored[] = {"TURN_DATA", "CAMERA", "CHECK_SUMS", "PLAYER_SPEED", "PAUSE", "HOTKEY"};
					bool skip = false;
					for (const char* i : ignored) skip = skip || rc.name == i;
					if (skip) continue;
					if (rc.name == "UNITMASK" && use_commands) {
						apply_unitmask(*game, package.group, uint32_t(rc.mask_arg));
						++cmds_run;
						continue;
					}
					if (!rc.supported || !use_commands) {
						++unsupported[rc.name];
						if (first_unsupported_frame < 0) { first_unsupported_frame = f; first_unsupported = rc.name; }
						continue;
					}
					package.process(*game, rc.cmd, play, who);
					++cmds_run;
				}
			}
			if (next_frame >= 0 && f == next_frame) break;
			if (const char* ins = std::getenv("RON_INSERT")) {
				// experiment: RON_INSERT="F:k[,F:k..]" draws k extra numbers (k < 0: puts the generator back |k| draws) just before frame F's do_frame
				std::stringstream is(ins);
				std::string tk;
				while (std::getline(is, tk, ',')) {
					int fr = 0, k = 0;
					if (std::sscanf(tk.c_str(), "%d:%d", &fr, &k) == 2 && fr == f) {
						const uint32_t ainv = [] { uint32_t x = 0x19660Du; for (int i = 0; i < 5; ++i) x *= 2 - 0x19660Du * x; return x; }();
						uint32_t sd = game->rng.seed();
						for (int i = 0; i < std::abs(k); ++i) sd = k > 0 ? sd * 0x19660Du + 0x3C6EF35Fu : (sd - 0x3C6EF35Fu) * ainv;
						game->rng.reseed(sd);
					}
				}
			}
			if (const char* gd = std::getenv("RON_DUMP_GAIA")) {  // RON_DUMP_GAIA=F0:F1 (+ RON_DUMP_GAIA_WHO, default 9): the gaia list's units before frames F0..F1
				int f0 = 0, f1 = 0;
				if (std::sscanf(gd, "%d:%d", &f0, &f1) == 2 && f >= f0 && f <= f1) {
					const int gw = std::getenv("RON_DUMP_GAIA_WHO") ? std::atoi(std::getenv("RON_DUMP_GAIA_WHO")) : 9;
					for (int o = 0; o < game->objects[size_t(gw)].unit_mark; ++o) {
						const sim::Unit* u = game->unit(gw, o);
						if (!u || !u->alive()) continue;
						std::printf("  GAIA f%d %d/%d type %d x %d y %d orders %zu path %zu aid %d spell_time %d cara %d", int(f), gw, o, u->type, u->x, u->y, u->orders.size(), u->path.size(), int(u->animal_aid), int(u->spell_time), int(u->cara));
						for (const sim::Guy& gy : u->guys) { std::printf(" | anim %d cur %u end %u", int(gy.cur_anim), unsigned(gy.cur_time), unsigned(gy.end_time)); if (std::getenv("RON_DUMP_GAIA_TURRET")) std::printf(" turret %d,%d,%d,%d des %d,%d,%d,%d track %d,%d ang %x/%x last_ang %x", gy.turret_angles[0], gy.turret_angles[1], gy.turret_angles[2], gy.turret_angles[3], gy.des_turret_angles[0], gy.des_turret_angles[1], gy.des_turret_angles[2], gy.des_turret_angles[3], gy.track_dx, gy.track_dy, unsigned(gy.angle), unsigned(gy.des_angle), unsigned(gy.last_angle)); }
						std::printf("\n");
					}
				}
			}
			if (const char* pw = std::getenv("RON_PERTURB_WDATA")) {  // experiment: RON_PERTURB_WDATA="F:x:y:field:delta" changes one WData byte-field before frame F (field: who, who2, flags, region, blocked, land)
				int pf = 0, px = 0, py = 0, pd = 0; char fld[16] = {0};
				if (std::sscanf(pw, "%d:%d:%d:%15[a-z0-9]:%d", &pf, &px, &py, fld, &pd) == 5 && f == pf) {
					sim::WData& wd = game->world.w(px, py);
					const std::string fs = fld;
					if (fs == "who") wd.who = int8_t(wd.who + pd); else if (fs == "who2") wd.who2 = int8_t(wd.who2 + pd);
					else if (fs == "flags") wd.flags = uint16_t(wd.flags ^ pd); else if (fs == "region") wd.region = int16_t(wd.region + pd);
					else if (fs == "blocked") wd.blocked = uint8_t(wd.blocked + pd); else if (fs == "land") wd.land = int8_t(wd.land + pd);
				}
			}
			if (const char* fd = std::getenv("RON_DUMP_FARMS")) {  // RON_DUMP_FARMS=F0:F1: per farm, before frames F0..F1: owner, gate, bare count, chance
				int f0 = 0, f1 = 0;
				if (std::sscanf(fd, "%d:%d", &f0, &f1) == 2 && f >= f0 && f <= f1) {
					for (size_t i = 0; i < game->farms.size(); ++i) {
						const sim::FarmRec& fr = game->farms[i];
						const sim::Build* b = fr.who >= 0 && fr.o >= 0 ? game->build(fr.who, fr.o) : nullptr;
						int bare = 0; for (auto& row : fr.percent) for (float p : row) bare += p <= 0.0f ? 1 : 0;
						std::printf("  FARM f%d #%zu valid %d who %d o %d type %d bld %s flags %x bare %d\n", int(f), i, int(fr.valid), int(fr.who), int(fr.o), int(fr.farm_type), b ? (b->alive() ? "alive" : "dead") : "none", b ? unsigned(b->flags) : 0u, bare);
					}
				}
			}
			if (f < last_boundary) {
				if (fish_timeline) frame_seeds.push_back({game->frame, game->rng.seed()});
				game->do_frame();
			}
		}
	} catch (const std::exception& e) {
		step_error = e.what();
	}
#ifdef RON_RNG_PROFILE_HOOK
	if (sim::g_rng_hook) {
		std::vector<std::pair<int64_t, std::string>> v;
		int64_t tot = 0;
		for (auto& kv : g_rng_sites) { v.push_back({kv.second, kv.first}); tot += kv.second; }
		std::sort(v.rbegin(), v.rend());
		std::printf("  RNG draws by site (%lld in %d frames):\n", (long long)tot, game->frame - int(start));
		for (size_t i = 0; i < v.size() && i < 30; ++i) std::printf("    %8lld  %s\n", (long long)v[i].first, v[i].second.c_str());
	}
#endif
	if (std::getenv("RON_ANIMCHK") && game->anims) {
		// end_time stored in the save against the engine's AnimSource::frames for the guy's current animation
		std::map<std::string, std::pair<int, int>> tally;
		for (int w = 0; w < 10; ++w) for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) {
			sim::Unit* u0 = game->unit(w, o);
			if (!u0 || !u0->alive()) continue;
			for (const sim::Guy& gy : u0->guys) {
				const uint32_t want = uint32_t(game->anims->frames(gy.gpiece, gy.cur_anim));
				char b[96]; std::snprintf(b, sizeof b, "type %d anim %d", u0->type, int(gy.cur_anim));
				auto& t = tally[b];
				if (want == gy.end_time) ++t.first; else { ++t.second; if (t.second <= 1) std::printf("  animchk mismatch %s: save end_time %u engine %u (gpiece %d)\n", b, gy.end_time, want, int(gy.gpiece)); }
			}
		}
		int ok = 0, bad = 0;
		for (auto& kv : tally) { ok += kv.second.first; bad += kv.second.second; }
		std::printf("  animchk: %d guys agree, %d differ\n", ok, bad);
	}
	if (std::getenv("RON_UNITTYPES")) {
		for (int w = 0; w < 2; ++w) {
			std::map<std::string, int> h;
			for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) { sim::Unit* u0 = game->unit(w, o); if (u0 && u0->alive()) h[sim::to_utf8(u0->ptype->type_name) + "[" + std::to_string(u0->type) + "]"]++; }
			std::printf("  who %d at frame %d:", w, game->frame); for (auto& kv : h) std::printf(" %s=%d", kv.first.c_str(), kv.second); std::printf("\n");
			{ std::map<std::string, int> hb; int act = 0; for (int o = 2000; o < game->objects[size_t(w)].build_mark; ++o) { sim::Build* b0 = game->build(w, o); if (b0 && b0->alive()) { hb[sim::to_utf8(b0->ptype->type_name) + "[" + std::to_string(b0->type) + "]"]++; act += (b0->flags & sim::objflag::kActive) ? 1 : 0; } }
			  std::printf("  builds who %d (%d active):", w, act); for (auto& kv : hb) std::printf(" %s=%d", kv.first.c_str(), kv.second); std::printf("\n"); }
		}
	}
	if (std::getenv("RON_MAKEROAD")) {
		for (int w = 0; w < 8; ++w) for (int o = 2000; o < game->objects[size_t(w)].build_mark; ++o) { sim::Build* b0 = game->build(w, o); if (b0 && b0->alive() && (b0->build_masks & sim::bmask::kMakeRoad)) std::printf("  MAKE_ROAD pending at frame %d: who %d o %d type %d city %d (frame+o)%%16 = %d\n", game->frame, w, o, b0->type, int(b0->city), (game->frame + o) % 16); }
	}
	if (std::getenv("RON_L9")) {
		std::map<std::string, int> h9;
		for (int o = 0; o < game->objects[9].unit_mark; ++o) { sim::Unit* u0 = game->unit(9, o); if (u0 && u0->alive()) h9[sim::to_utf8(u0->ptype->type_name)]++; }
		std::printf("  list 9 at frame %d:", game->frame); for (auto& kv : h9) std::printf(" %s=%d", kv.first.c_str(), kv.second);
		if (std::getenv("RON_L9") && std::atoi(std::getenv("RON_L9")) > 1)
			for (int o = 0; o < game->objects[9].unit_mark; ++o) {
				sim::Unit* u0 = game->unit(9, o);
				if (!u0 || !u0->alive()) continue;
				std::printf("\n    9/%d %s pos %d,%d cara %d orders", o, sim::to_utf8(u0->ptype->type_name).c_str(), u0->x, u0->y, u0->cara);
				for (const sim::Order& od : u0->orders) std::printf(" [t%d]", int(od.type));
				for (const sim::Guy& gy : u0->guys) std::printf(" (a%d %u/%u)", gy.cur_anim, gy.cur_time, gy.end_time);
			}
		int pens = 0, farms = 0; for (const auto& f : game->farms) if (f.valid) { ++farms; if (f.farm_type & 1) ++pens; }
		std::printf("; farms %d pens %d\n", farms, pens);
	}
	std::printf("  simulated to frame %d (%lld turn boundaries compared, %lld with all 15 parts equal), %lld commands executed\n", game->frame, (long long)boundaries,
		(long long)all_match, (long long)cmds_run);
	if (!step_error.empty()) std::printf("  STOPPED: %s\n", step_error.c_str());
	if (first_unsupported_frame >= 0) {
		std::printf("  commands the engine does not execute (first at record frame %lld, %s):", (long long)first_unsupported_frame, first_unsupported.c_str());
		for (const auto& kv : unsupported) std::printf(" %s x%lld", kv.first.c_str(), (long long)kv.second);
		std::printf("\n");
	}
	for (int k = 0; k < dw::kCheckPartCount; ++k) {
		if (first_diff[k] < 0) std::printf("  %-16s matches through frame %lld\n", dw::kCheckPartNames[k], (long long)(game->frame));
		else std::printf("  %-16s first differs at boundary frame %lld (matched through %lld)\n", dw::kCheckPartNames[k], (long long)first_diff[k], (long long)last_match[k]);
	}
	if (next_frame >= 0) {
		if (game->frame != next_frame) { std::printf("  could not reach the next save's frame (engine at %d)\n", game->frame); return 0; }
		sim::OriginalLoadReport rn;
		auto truth = sim::load_original_save(cache->files, rules, nullptr, next_save, &rn);
		if (!truth) { std::printf("  next save: load failed: %s\n", rn.error.c_str()); return 1; }
		if (fish_timeline) fish_timeline_report(*truth, next_frame, seed_load, frame_seeds, game->rng.seed());
		// 15 parts of both states at the same frame
		sim::EngineProvider e1(*game), e2(*truth);
		dw::CheckSumParts a1 = dw::compute_check_sums(e1), a2 = dw::compute_check_sums(e2);
		{
			// The distance between the two generator states, in draws: how many draws the engine made more (+) or fewer (-) than the original.
			const uint32_t a = 0x19660Du, c = 0x3C6EF35Fu, ainv = [] { uint32_t x = 0x19660Du; for (int i = 0; i < 5; ++i) x *= 2 - 0x19660Du * x; return x; }();
			const uint32_t eng = game->rng.seed(), org = truth->rng.seed();
			int64_t dist = INT64_MIN;
			uint32_t f = eng, b = eng;
			for (int64_t n = 0; n <= 4000000; ++n) {
				if (f == org) { dist = -n; break; }  // the original is n draws ahead of the engine
				if (b == org) { dist = n; break; }
				f = f * a + c;
				b = (b - c) * ainv;
			}
			if (dist == INT64_MIN) std::printf("  game_random: engine 0x%08x original 0x%08x: more than 4,000,000 draws apart\n", eng, org);
			else std::printf("  game_random: engine 0x%08x original 0x%08x: the engine made %+lld draws against the original\n", eng, org, (long long)dist);
		}
		std::printf("  at frame %lld, engine against the next save:", (long long)next_frame);
		for (int k = 0; k < dw::kCheckPartCount; ++k) std::printf(" %s=%s", dw::kCheckPartNames[k], a1.v[k] == a2.v[k] ? "=" : "!");
		std::printf("\n");
		std::vector<sim::AuditLine> d = sim::diff_mapped_state(*truth, *game);
		std::sort(d.begin(), d.end(), [](const sim::AuditLine& x, const sim::AuditLine& y) { return x.mismatches > y.mismatches; });
		int shown = 0;
		for (const sim::AuditLine& l : d) {
			if (!l.mismatches) continue;
			if (shown++ < (std::getenv("RON_DIFF_ALL") ? 100000 : 60) || (std::getenv("RON_DIFF_FILTER") && l.member.find(std::getenv("RON_DIFF_FILTER")) != std::string::npos)) std::printf("    differs: %-34s %6lld of %-7lld %s\n", l.member.c_str(), (long long)l.mismatches, (long long)l.objects, l.example.c_str());
		}
		if (std::getenv("RON_CITIZENS")) {  // citizens per player: (stance, first order type, idle>=12) histogram, original first then the engine
			for (int which = 0; which < 2; ++which) {
				sim::Game& gg = which == 0 ? *truth : *game;
				for (int who = 0; who < 2; ++who) {
					std::map<std::tuple<int, int, int>, int> hist;
					int n = 0;
					for (int o = 0; o < gg.objects[size_t(who)].unit_mark; ++o) {
						sim::Unit* u = gg.unit(who, o);
						if (!u || !u->alive() || (u->type != 50 && u->type != 51)) continue;
						++n;
						hist[{u->stance, u->orders.empty() ? -1 : int(u->orders.front().type), u->idle >= 12 ? 1 : 0}]++;
					}
					std::printf("  %s player %d: %d citizens (stance, first order type, idle>=12): count\n", which == 0 ? "original" : "engine  ", who, n);
					for (const auto& kv : hist) std::printf("      (%d, %d, %d): %d\n", std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), kv.second);
				}
			}
		}
		if (std::getenv("RON_CARAVANS")) {
			for (int which = 0; which < 2; ++which) {
				const sim::Game& gg = which ? *truth : *game;
				std::printf("  caravans (%s) at frame %d:\n", which ? "original" : "engine", gg.frame);
				for (int w = 0; w < 2; ++w) for (int o = 0; o < gg.objects[size_t(w)].unit_mark; ++o) {
					const sim::Unit* u0 = gg.unit(w, o);
					if (!u0 || !u0->alive() || (u0->type != 59 && u0->type != 61)) continue;
					std::printf("    %d/%d t%d inside_up %d pos %d,%d orders", w, o, u0->type, u0->inside_up, u0->x, u0->y);
					for (const sim::Order& od : u0->orders) std::printf(" [t%d]", int(od.type));
					for (const sim::Guy& gy : u0->guys) std::printf(" (a%d %u/%u)", gy.cur_anim, gy.cur_time, gy.end_time);
					std::printf("\n");
				}
			}
		}
		if (std::getenv("RON_FARMS")) {
			for (int which = 0; which < 2; ++which) {
				const sim::Game& gg = which ? *truth : *game;
				int farms = 0, drawing = 0, bare_total = 0, pens = 0;
				std::map<int, int> hb;
				for (const auto& f : gg.farms) {
					if (!f.valid) continue;
					if (f.farm_type & 1) { ++pens; continue; }
					++farms;
					int bare = 0;
					for (int x = 0; x < 4; ++x) for (int y = 0; y < 4; ++y) if (!(f.percent[size_t(x)][size_t(y)] > 0.0f)) ++bare;
					bare_total += bare;
					hb[bare]++;
					if (bare >= 5) ++drawing;
				}
				std::printf("  farms (%s): %d wheat, %d pens, bare cells %d, farms with >= 5 bare %d; bare histogram:", which ? "original" : "engine", farms, pens, bare_total, drawing);
				for (auto& kv : hb) std::printf(" %d:%d", kv.first, kv.second);
				std::printf("\n");
			}
		}
		if (const char* lm = std::getenv("RON_LIST_UNIT_DIFF")) {  // every unit whose member `lm` (los_x los_y idle tolerance group uid collide unit_masks ...) differs: engine -> original
			const std::string mname = lm;
			auto val = [&](const sim::Unit& u) -> int64_t {
				if (mname == "los_x") return u.los_x;
				if (mname == "los_y") return u.los_y;
				if (mname == "idle") return u.idle;
				if (mname == "tolerance") return u.tolerance;
				if (mname == "group") return u.group;
				if (mname == "uid") return u.uid;
				if (mname == "collide") return u.collide;
				if (mname == "unit_masks") return int64_t(u.unit_masks);
				if (mname == "recharging") return u.recharging;
				if (mname == "damage_frame") return u.damage_frame;
				if (mname == "flags") return u.flags;
				if (mname == "stance") return u.stance;
				if (mname == "cara") return u.cara;
				if (mname == "unit_masks2") return int64_t(u.unit_masks2);
				if (mname == "hold") return (u.unit_masks2 & 0x10) != 0;
				if (mname == "type") return u.type;
				return 0;
			};
			int n = 0;
			for (int who = 0; who < sim::kNumLeaders; ++who)
				for (int o = 0; o < game->objects[size_t(who)].unit_mark && o < truth->objects[size_t(who)].unit_mark; ++o) {
					sim::Unit* a = game->unit(who, o);
					sim::Unit* b = truth->unit(who, o);
					if (!a || !b || !a->alive() || !b->alive() || val(*a) == val(*b)) continue;
					if (n++ < 60) std::printf("    unit %d/%d type %s pos %d,%d (orig %d,%d) %s: engine %lld original %lld; order %d, los %d,%d / %d,%d\n", who, o, sim::to_utf8(a->ptype->type_name).c_str(), a->x, a->y, b->x, b->y, lm, (long long)val(*a), (long long)val(*b),
						a->orders.empty() ? -1 : int(a->orders.front().type), a->los_x, a->los_y, b->los_x, b->los_y);
				}
			std::printf("  %d units differ in %s\n", n, lm);
		}
		if (const char* lb = std::getenv("RON_LIST_BUILD_DIFF")) {  // every building whose member `lb` (mylos myhits max_age stance build_masks type queued recharging city) differs: engine -> original
			const std::string mname = lb;
			auto val = [&](const sim::Build& b) -> int64_t {
				if (mname == "mylos") return b.mylos;
				if (mname == "myhits") return b.myhits;
				if (mname == "max_age") return b.max_age;
				if (mname == "stance") return b.stance;
				if (mname == "build_masks") return b.build_masks;
				if (mname == "type") return b.type;
				if (mname == "queued") return b.queued;
				if (mname == "recharging") return b.recharging;
				if (mname == "city") return b.city;
				if (mname == "damage") return b.damage;
				if (mname == "flags") return b.flags;
				return 0;
			};
			int n = 0;
			for (int who = 0; who < sim::kNumLeaders; ++who)
				for (int o = sim::kBuildBase; o < game->objects[size_t(who)].build_mark && o < truth->objects[size_t(who)].build_mark; ++o) {
					sim::Build* a = game->build(who, o);
					sim::Build* b = truth->build(who, o);
					if (!a || !b || !a->alive() || !b->alive() || val(*a) == val(*b)) continue;
					if (n++ < 60) std::printf("    build %d/%d type %d %s %s: engine %lld original %lld (age %d/%d)\n", who, o, a->type, a->ptype ? sim::to_utf8(a->ptype->type_name).c_str() : "?", lb, (long long)val(*a), (long long)val(*b), int(a->max_age), int(b->max_age));
				}
			std::printf("  %d buildings differ in %s\n", n, lb);
		}
		std::printf("  %d mapped members differ between the engine and the next save\n", shown);
	}
	return 0;
}

}  // namespace

// A crash while stepping the engine prints the call stack (symbols of this binary) instead of dying silently.
static void on_crash(int sig) {
	void* frames[48];
	const int n = backtrace(frames, 48);
	const char msg[] = "\nron-sav: crashed (signal) in the engine; call stack:\n";
	(void)!write(2, msg, sizeof msg - 1);
	backtrace_symbols_fd(frames, n, 2);
	std::signal(sig, SIG_DFL);
	std::raise(sig);
}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::signal(SIGSEGV, on_crash);
	std::signal(SIGBUS, on_crash);
	std::signal(SIGABRT, on_crash);
	if (argc < 2) {
		std::fprintf(stderr, "usage: ron-sav <file.SVX|.rcx> [--summary] [--field Path] [--checksums [--csv file]] [--verify]\n");
		return 64;
	}
	const std::string path = argv[1];
	bool summary = false, verify = false, checksums = false, quiet = false, engine_rt = false, diag = true, astar_chk = false;
	int mutate = 0;
	bool replay = false, use_commands = true, verbose = false, no_art = false, field_report = false;
	int64_t frames = 0, step_diff = 0;
	std::string json_path, next_replay;
	std::vector<std::string> revert;
	std::string game_dir = game_dir_default();
	std::vector<std::string> fields;
	std::string csv;  // default: re/oracle/out/checksums.csv next to the "Recorded Games" folder of the replay, else under the cwd
	for (int i = 2; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--summary") summary = true;
		else if (a == "--verify") verify = true;
		else if (a == "--checksums") checksums = true;
		else if (a == "--quiet") quiet = true;
		else if (a == "--engine-roundtrip") engine_rt = true;
		else if (a == "--astar-check") astar_chk = true;
		else if (a == "--no-diag") diag = false;
		else if (a == "--replay") replay = true;
		else if (a == "--frames" && i + 1 < argc) frames = std::atoll(argv[++i]);
		else if (a == "--json" && i + 1 < argc) json_path = argv[++i];
		else if (a == "--no-commands") use_commands = false;
		else if (a == "--verbose") verbose = true;
		else if (a == "--solve-at" && i + 1 < argc) g_solve_at = std::atoll(argv[++i]);
		else if (a == "--pshift-search" && i + 1 < argc) g_pshift_at = std::atoll(argv[++i]);
		else if (a == "--shift-search" && i + 2 < argc) { g_shift_part = std::atoi(argv[++i]); g_shift_at = std::atoll(argv[++i]); }
		else if (a == "--step-diff" && i + 1 < argc) step_diff = std::atoll(argv[++i]);
		else if (a == "--compare-next" && i + 1 < argc) next_replay = argv[++i];
		else if (a == "--no-art") no_art = true;
		else if (a == "--field-report") field_report = true;
		else if (a == "--revert" && i + 1 < argc) { std::stringstream ss(argv[++i]); std::string t; while (std::getline(ss, t, ',')) revert.push_back(t); }
		else if (a == "--mutate" && i + 1 < argc) mutate = std::atoi(argv[++i]);
		else if (a == "--game" && i + 1 < argc) game_dir = argv[++i];
		else if (a == "--field" && i + 1 < argc) fields.push_back(argv[++i]);
		else if (a == "--csv" && i + 1 < argc) csv = argv[++i];
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 64; }
	}

	if (field_report) {
		// How many members of the original's walked ranges each engine map covers (the rest stays in the shadow).
		int total_walked = 0, total_mapped = 0, entries = 0;
		for (const sim::omap::ClassMap* m : sim::omap::all_maps()) {
			sim::omap::FieldCount fc = sim::omap::count_fields(*m);
			entries += int(m->entries.size());
			total_walked += fc.walked;
			total_mapped += fc.mapped;
			std::printf("%-20s PDB class %-18s %3zu entries; walked members %4d, mapped %4d, in the shadow %4d", m->name.c_str(), m->cls.c_str(), m->entries.size(), fc.walked, fc.mapped, fc.walked - fc.mapped);
			if (!fc.unmapped.empty() && fc.unmapped.size() <= 40) {
				std::printf(":");
				for (const std::string& u : fc.unmapped) std::printf(" %s", u.c_str());
			} else if (!fc.unmapped.empty()) {
				std::printf(": %s ... (%zu)", fc.unmapped.front().c_str(), fc.unmapped.size());
			}
			std::printf("\n");
		}
		std::printf("total: %d entries; walked members of the classes above %d, mapped %d, shadow %d\n", entries, total_walked, total_mapped, total_walked - total_mapped);
		return 0;
	}
	std::vector<uint8_t> raw, buf;
	bool gz = false;
	if (!read_file(path, raw)) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 66; }
	if (!maybe_gunzip(raw, buf, gz)) { std::fprintf(stderr, "cannot inflate %s\n", path.c_str()); return 66; }
	const dw::Description& desc = dw::Description::get();

	// Save or replay? A save starts with a String "RoN...Save"; a replay with the Game section (tag byte 0x16).
	std::string magic;
	bool is_save = dw::read_string(buf.data(), buf.size(), 0, magic) != std::string::npos && magic.compare(0, 3, "RoN") == 0 && magic.find("Save") != std::string::npos;
	size_t save_pos = 0;
	dw::Tree header(desc);
	if (!is_save) {
		dw::DecodeResult r = dw::decode_function(header, buf.data(), buf.size(), 0, "Game::walk_data", "game", 0x10);
		std::printf("%s: replay (%s, %zu bytes), header Game::walk_data %s at byte %zu, %llu tags\n", basename_of(path).c_str(), gz ? "gzip" : "plain",
			buf.size(), r.ok ? "decoded" : "FAILED", r.end, (unsigned long long)r.tags);
		if (!r.ok) { std::printf("  error: %s\n", r.error.c_str()); return 1; }
		std::string name;
		size_t p = dw::read_string(buf.data(), buf.size(), r.end, name);
		if (p == std::string::npos) { std::printf("  error: no save_name after the header\n"); return 1; }
		const dw::Rec* g = header.find_global(desc.find_global("game"));
		if (g->read_int(0x814 + 0xc + 2, 1, false) & 2) { std::printf("  Conquer the World replay: not supported\n"); return 1; }
		std::printf("  header: frame %lld, save_name %s\n", (long long)field_int(header, "game.frame"), name.empty() ? "empty (starts from scratch, no embedded save)" : "present");
		if (name.empty()) {
			if (replay || engine_rt) std::printf("  no embedded save: the game starts from the setup (random map, rules block in the stream), which needs the map generator path; not supported here\n");
			return 0;
		}
		save_pos = p;
	}

	dw::Tree tree(desc);
	dw::SaveDecode sd = dw::decode_save(tree, buf.data(), buf.size(), save_pos);
	const size_t file_end = buf.size();
	std::printf("save: magic=%s version=0x%x, walk %zu..%zu (%zu bytes), verify block to %zu, %llu section tags verified, %zu verify entries\n", sd.magic.c_str(),
		sd.version, sd.walk_begin, sd.result.ok ? sd.end : sd.result.end, (sd.result.ok ? sd.end : sd.result.end) - sd.walk_begin, sd.end,
		(unsigned long long)sd.result.tags, sd.verify.size());
	if (!sd.result.ok) {
		std::printf("  DECODE FAILED at %zu: %s\n", sd.result.end, sd.result.error.c_str());
		return 1;
	}
	if (is_save) std::printf("  %s: %zu bytes, decode %s the last byte of the file\n", basename_of(path).c_str(), file_end, sd.end == file_end ? "reaches" : "STOPS BEFORE");
	else std::printf("  embedded save ends at %zu of %zu (%zu bytes follow: random-game-info block and command stream)\n", sd.end, file_end, file_end - sd.end);
	std::printf("  %zu records, %zu events\n", tree.rec_count(), tree.event_count());
	if (!is_save) std::printf("  embedded save: %zu bytes at offset %zu; frames: replay header %lld, save %lld\n", sd.end - save_pos, save_pos,
		(long long)field_int(header, "game.frame"), (long long)field_int(tree, "game.frame"));
	int rc = (is_save && sd.end != file_end) ? 1 : 0;
	if (step_diff > 0) {
		// which mapped members does the engine change when it simulates N frames from this save?
		std::vector<uint8_t> save(buf.begin() + std::ptrdiff_t(save_pos), buf.begin() + std::ptrdiff_t(sd.end));
		dw::Tree pk(dw::Description::get());
		dw::decode_save(pk, save.data(), save.size(), 0);
		sim::GameSetup setup;
		setup.starting_technology = int32_t(field_int(pk, "game.info.starting_technology"));
		setup.starting_technology2 = int32_t(field_int(pk, "game.info.starting_technology2"));
		setup.ending_technology = int32_t(field_int(pk, "game.info.ending_technology"));
		setup.game_rules = int32_t(field_int(pk, "game.info.game_rules"));
		setup.flag8 = (field_int(pk, "game.info.flags") & 8) != 0;
		static std::unique_ptr<RulesCache> cache;
		if (!cache) cache = std::make_unique<RulesCache>(game_dir);
		const sim::Rules& rules = cache->get(setup);
		sim::OriginalLoadReport r1, r2;
		auto ga = sim::load_original_save(cache->files, rules, nullptr, save, &r1);
		auto gb = sim::load_original_save(cache->files, rules, nullptr, save, &r2);
		if (!ga || !gb) { std::printf("load failed\n"); return 1; }
		std::unique_ptr<sim::AssetAnimSource> art;
		if (!no_art) { art = sim::load_anim_source(cache->files, *gb->rules); gb->anims = art.get(); }
		for (int64_t k = 0; k < step_diff; ++k) gb->do_frame();
		if (const char* ch = std::getenv("RON_DUMP_CHAIN")) {
			int who = 0, o = 0;
			std::sscanf(ch, "%d,%d", &who, &o);
			for (sim::Game* gg : {ga.get(), gb.get()}) {
				const sim::Object* ob = gg->object(who, o);
				if (!ob) continue;
				const int32_t wx = sim::wcoord_of(ob->x), wy = sim::wcoord_of(ob->y);
				std::printf("frame %d: object (%d,%d) up %d/%d down %d/%d in wcoord (%d,%d); chain from WData.down:", gg->frame, who, o, ob->up, ob->up_who, ob->down, ob->down_who, wx, wy);
				const sim::WData& d = gg->world.w(wx, wy);
				int32_t co = d.down, cw = d.down_who;
				for (int guard = 0; co >= 0 && guard < 64; ++guard) {
					std::printf(" (%d,%d)", cw, co);
					const sim::Object* c = gg->object(cw, co);
					if (!c) break;
					co = c->down;
					cw = c->down_who;
				}
				std::printf(" end %d/%d\n", co, cw);
			}
		}
		if (const char* fnn = std::getenv("RON_HDR_HIST")) {
			// histogram of the header ranges (container headers) of one walk function in the units part, original (loaded) vs engine-stepped
			for (sim::Game* gg : {ga.get(), gb.get()}) {
				sim::EngineProvider epx(*gg);
				PartTrace tr;
				trace_part(epx, 0, tr);
				const dw::Description& dd = dw::Description::get();
				std::map<std::string, int> h;
				for (const auto& r : tr.ranges) {
					const int fnc = dd.step_function(r.step);
					if (fnc < 0 || std::strcmp(dd.function_name(fnc), fnn) != 0) continue;
					char buf[96];
					std::string k;
					for (size_t q = 0; q < r.len && q < 12; ++q) { std::snprintf(buf, sizeof buf, "%02x ", tr.bytes[r.begin + q]); k += buf; }
					h[std::to_string(r.len) + ": " + k]++;
				}
				std::printf("-- %s headers at frame %d:\n", fnn, gg->frame);
				for (const auto& kv : h) std::printf("   %6d x %s\n", kv.second, kv.first.c_str());
			}
		}
		if (std::getenv("RON_ANIM_HIST")) {
			std::map<std::tuple<int, int, int>, int> hist;  // (type, anim, end_time) -> count
			for (int who = 0; who < 8; ++who)
				for (int o = 0; o < ga->objects[size_t(who)].unit_mark; ++o) {
					sim::Unit* u0 = ga->unit(who, o);
					if (!u0 || !u0->alive()) continue;
					for (const sim::Guy& gy : u0->guys) hist[{u0->type, gy.cur_anim, int(gy.end_time)}]++;
				}
			for (const auto& kv : hist) std::printf("hist type %d anim %d end %d: %d guys\n", std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), kv.second);
		}
		if (std::getenv("RON_ANIM_TABLE")) {
			// every guy whose animation changed between the two states: before -> after (with the remaining frames before)
			for (int who = 0; who < sim::kNumLeaders; ++who)
				for (int o = 0; o < ga->objects[size_t(who)].unit_mark; ++o) {
					sim::Unit* u0 = ga->unit(who, o);
					sim::Unit* u1 = gb->unit(who, o);
					if (!u0 || !u1 || !u0->alive()) continue;
					for (size_t k = 0; k < u0->guys.size() && k < u1->guys.size(); ++k) {
						const sim::Guy& a = u0->guys[k];
						const sim::Guy& b = u1->guys[k];
						if (a.cur_anim == b.cur_anim && a.end_time == b.end_time) continue;
						std::printf("guy %d/%d/%zu %s anim %d t %u/%u -> anim %d t %u/%u (gpiece %d)\n", who, o, k, sim::to_utf8(u0->ptype->type_name).c_str(), a.cur_anim, a.cur_time, a.end_time, b.cur_anim, b.cur_time, b.end_time, b.gpiece);
					}
				}
		}
		if (const char* sw = std::getenv("RON_SCAN_UNITS")) {
			const int who = std::atoi(sw);
			for (int o = 0; o < ga->objects[size_t(who)].unit_mark; ++o) {
				sim::Unit* u = ga->unit(who, o);
				if (!u || !u->alive()) continue;
				std::printf("u%d/%d type %d %s pos %d,%d orders:", who, o, u->type, sim::to_utf8(u->ptype->type_name).c_str(), u->x, u->y);
				for (const sim::Order& od : u->orders) std::printf(" [t%d ox %d whom %d bt %d bt_there %d wait %d g %d,%d]", int(od.type), od.ox, od.whom, od.build_type, od.been_there, od.wait, od.g_tx, od.g_ty);
				std::printf(" group %d\n", u->group);
			}
		}
		if (std::getenv("RON_SCAN_GUYS")) {
			int n = 0, odd = 0;
			for (int who = 0; who < sim::kNumLeaders; ++who)
				for (int o = 0; o < ga->objects[size_t(who)].unit_mark; ++o) {
					sim::Unit* u = ga->unit(who, o);
					if (!u || !u->alive()) continue;
					for (const sim::Guy& gy : u->guys) {
						++n;
						if (gy.stopped && (gy.des_x != gy.x || gy.des_y != gy.y)) {
							++odd;
							if (odd < 30) std::printf("stopped guy %d/%d g%d type %d anim %d des %d,%d at %d,%d (diff %d,%d) time %u/%u\n", who, o, gy.guy_num, u->type, gy.cur_anim, gy.des_x, gy.des_y, gy.x, gy.y, gy.des_x - gy.x, gy.des_y - gy.y, gy.cur_time, gy.end_time);
						}
					}
				}
			std::printf("guys %d, stopped with des != pos: %d\n", n, odd);
			int offn = 0;
			for (int who = 0; who < sim::kNumLeaders; ++who)
				for (int o = 0; o < ga->objects[size_t(who)].unit_mark; ++o) {
					sim::Unit* u = ga->unit(who, o);
					if (!u || !u->alive()) continue;
					for (const sim::Guy& gy : u->guys)
						if ((gy.off_x != 0 || gy.off_y != 0) && offn++ < 15)
							std::printf("guy with offset %d/%d type %d anim %d off %d,%d pos %d,%d des %d,%d\n", who, o, u->type, gy.cur_anim, gy.off_x, gy.off_y, gy.x, gy.y, gy.des_x, gy.des_y);
				}
			std::printf("guys with a nonzero offset: %d\n", offn);
		}
		if (std::getenv("RON_DUMP_MOVERS")) {
			// every unit whose position, path, orders or collision bookkeeping changed: before -> after
			for (int who = 0; who < sim::kNumLeaders; ++who)
				for (int o = 0; o < ga->objects[size_t(who)].unit_mark; ++o) {
					sim::Unit* a = ga->unit(who, o);
					sim::Unit* b = gb->unit(who, o);
					if (!a || !b || !a->alive()) continue;
					if (a->x == b->x && a->y == b->y && a->path.size() == b->path.size() && a->collide == b->collide && a->collide_frame == b->collide_frame && a->orders.size() == b->orders.size()) continue;
					std::printf("mover %d/%d %s: pos %d,%d -> %d,%d angle %08x -> %08x flags %02x -> %02x idle %d -> %d masks %08x -> %08x path %zu -> %zu orders %zu -> %zu collide %d/%d/%d fr %d -> %d/%d/%d fr %d\n", who, o,
						sim::to_utf8(a->ptype->type_name).c_str(), a->x, a->y, b->x, b->y, a->angle, b->angle, a->flags, b->flags, a->idle, b->idle, a->unit_masks, b->unit_masks, a->path.size(), b->path.size(), a->orders.size(), b->orders.size(),
						a->collide, a->collide_o, a->collide_who, a->collide_frame, b->collide, b->collide_o, b->collide_who, b->collide_frame);
					for (const sim::Unit* uu : {a, b}) {
						std::printf("      path of %s (bottom first):", uu == a ? "before" : "after ");
						for (const sim::PathData& pd : uu->path) std::printf(" (%d,%d t%d f%x)", pd.x, pd.y, pd.tol, pd.flags);
						std::printf("\n");
					}
				}
		}
		if (const char* du = std::getenv("RON_DUMP_UNIT")) {
			int who = 0, o = 0;
			std::sscanf(du, "%d,%d", &who, &o);
			for (sim::Game* gg : {ga.get(), gb.get()}) {
				sim::Unit* u = gg->unit(who, o);
				if (!u) continue;
				std::printf("unit %d/%d type %d (%s) frame %d: x %d y %d angle %08x flags %02x idle %d orders %zu path %zu cara %d collide %d masks %08x guys %zu guy_mark %d\n", who, o, u->type, sim::to_utf8(u->ptype->type_name).c_str(), gg->frame, u->x, u->y,
					u->angle, u->flags, u->idle, u->orders.size(), u->path.size(), u->cara, u->collide, u->unit_masks, u->guys.size(), u->guy_mark);
				for (const sim::Guy& gy : u->guys)
					std::printf("   guy %d: anim %d stopped %d cur_time %u end_time %u last_time %d x %d y %d spd %d/%d flags %04x\n", gy.guy_num, gy.cur_anim, gy.stopped, gy.cur_time, gy.end_time, gy.last_time, gy.x, gy.y, gy.last_speed, gy.avg_speed, gy.guy_flags);
				for (const sim::Guy& gy : u->guys)
					std::printf("      gpiece %d des %d,%d des_angle %08x angle %08x last %d,%d off %d,%d track %d,%d z %d\n", gy.gpiece, gy.des_x, gy.des_y, gy.des_angle, gy.angle, gy.last_x, gy.last_y, gy.off_x, gy.off_y, gy.track_dx, gy.track_dy, gy.z);
			}
		}
		if (std::getenv("RON_DUMP_GOODS")) {
			for (size_t i = 0; i < gb->goods.size(); ++i) {
				const auto& g0 = ga->goods[i];
				const auto& g1 = gb->goods[i];
				if (!g1.alive()) continue;
				const int32_t fx = sim::fcoord_of(g1.x), fy = sim::fcoord_of(g1.y);
				const size_t fi = size_t(fy) * size_t(gb->world.fog_xs) + size_t(fx);
				std::printf("good %zu type %d at %d,%d ever_seen %02x -> %02x seen2 %02x seen %02x res %d\n", i, g1.type, g1.x, g1.y, g0.ever_seen, g1.ever_seen,
					gb->world.seen2[fi], gb->world.seen[fi], int(gb->world.in_t(g1.x / 192, g1.y / 192) && (gb->world.t(g1.x / 192, g1.y / 192) & sim::tmask::kResource)));
			}
		}
		std::vector<sim::AuditLine> d = sim::diff_mapped_state(*ga, *gb);
		std::sort(d.begin(), d.end(), [](const sim::AuditLine& x, const sim::AuditLine& y) { return x.mismatches > y.mismatches; });
		std::printf("%s: %lld frames from save frame %d: members changed by the engine (count of objects, of the number bound)\n", basename_of(path).c_str(), (long long)step_diff, ga->frame);
		for (const sim::AuditLine& l : d) if (l.mismatches) std::printf("  %-34s %6lld of %-7lld %s\n", l.member.c_str(), (long long)l.mismatches, (long long)l.objects, l.example.c_str());
		return 0;
	}
	if (replay) {
		if (is_save) { std::printf("--replay needs a .rcx file\n"); return 64; }
		if (csv.empty()) {
			size_t kk = path.find("Recorded Games");
			csv = kk != std::string::npos ? path.substr(0, kk) + "out/checksums.csv" : "re/oracle/out/checksums.csv";
		}
		std::vector<uint8_t> save(buf.begin() + std::ptrdiff_t(save_pos), buf.begin() + std::ptrdiff_t(sd.end));
		return replay_run(path, save, game_dir, frames, json_path, csv, use_commands, verbose, next_replay, no_art, revert);
	}
	if (engine_rt) {
		std::vector<uint8_t> save(buf.begin() + std::ptrdiff_t(save_pos), buf.begin() + std::ptrdiff_t(sd.end));
		return engine_roundtrip(path, save, game_dir, quiet, diag, mutate);
	}
	if (astar_chk) {
		std::vector<uint8_t> save(buf.begin() + std::ptrdiff_t(save_pos), buf.begin() + std::ptrdiff_t(sd.end));
		return astar_check(path, save, game_dir, verbose);
	}

	if (summary) print_summary(tree);
	for (const std::string& f : fields) {
		dw::FieldValue v;
		std::string err;
		if (dw::read_field(tree, f, v, err)) std::printf("%s = %s  (offset 0x%llx, size %d)\n", f.c_str(), v.text.c_str(), (unsigned long long)v.offset, v.size);
		else { std::printf("%s: %s\n", f.c_str(), err.c_str()); rc = 1; }
	}

	if (verify) {
		dw::TreeProvider prov(tree);
		std::vector<dw::VerifyEntry> es = dw::compute_verify(prov, sd.verify);
		static const char* kFamilies[] = {"good", "leader", "object_list", "wdata_cell", "tdata_tile", "seen_cell"};
		std::map<std::string, std::pair<int, int>> fam;  // name -> (entries, matches)
		std::map<std::string, std::string> fam_first_bad;
		int total = 0, ok = 0, errs = 0;
		for (const auto& e : es) {
			bool is_fam = false;
			for (const char* f : kFamilies) if (e.name == std::string(f)) is_fam = true;
			++total;
			if (e.match) ++ok;
			if (!e.error.empty()) ++errs;
			if (is_fam) {
				auto& c = fam[e.name];
				++c.first;
				if (e.match) ++c.second;
				else if (!fam_first_bad.count(e.name)) {
					char b[160];
					std::snprintf(b, sizeof b, "index %d stored %u computed %u%s%s", e.index, e.stored, e.computed, e.error.empty() ? "" : " error: ", e.error.c_str());
					fam_first_bad[e.name] = b;
				}
				continue;
			}
			if (!quiet || !e.match)
				std::printf("  verify %-16s stored %10u computed %10u  %s%s%s\n", e.name, e.stored, e.computed, e.match ? "match" : "MISMATCH", e.error.empty() ? "" : "  ", e.error.c_str());
		}
		for (const auto& kv : fam) {
			std::printf("  verify %-16s %d entries, %d match%s%s\n", kv.first.c_str(), kv.second.first, kv.second.second,
				kv.second.first == kv.second.second ? "" : "; first mismatch ", fam_first_bad.count(kv.first) ? fam_first_bad[kv.first].c_str() : "");
		}
		std::printf("verify: %d of %d entries match (%d walk errors)\n", ok, total, errs);
		if (ok != total) rc = rc ? rc : 2;
	}

	if (checksums) {
		dw::TreeProvider prov(tree);
		dw::CheckSumParts parts = dw::compute_check_sums(prov);
		if (!parts.ok) { std::printf("checksums: FAILED: %s\n", parts.error.c_str()); return 1; }
		Expected ex;
		if (!is_save) {
			if (csv.empty()) {
				size_t k = path.find("Recorded Games");
				csv = k != std::string::npos ? path.substr(0, k) + "out/checksums.csv" : "re/oracle/out/checksums.csv";
			}
			ex = first_check_sums(csv, path);
		}
		const int64_t save_frame = field_int(tree, "game.frame");
		std::printf("checksums (save frame %lld", (long long)save_frame);
		if (ex.found) std::printf(", first CHECK_SUMS at record frame %lld, %d of %d players agree", (long long)ex.frame, ex.players_agreeing, ex.players);
		else if (!is_save) std::printf(", no CHECK_SUMS row found in %s", csv.c_str());
		std::printf("):\n");
		int matches = 0;
		for (int k = 0; k < dw::kCheckPartCount; ++k) {
			std::printf("  %-16s %10u", dw::kCheckPartNames[k], parts.v[k]);
			if (ex.found) {
				bool m = parts.v[k] == ex.v[k];
				matches += m;
				std::printf("  expected %10u  %s", ex.v[k], m ? "match" : "differ");
			}
			std::printf("\n");
		}
		if (ex.found) std::printf("checksums: %d of %d parts match the first CHECK_SUMS\n", matches, dw::kCheckPartCount);
	}
	return rc;
}
