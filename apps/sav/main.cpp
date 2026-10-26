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
//   --extract-save F  write the embedded save of a replay (the decompressed stream) to F
//   --setup-check S   (with a replay without an embedded save) the lobby setup decoded from its header against the setup read back from the save S of the same game
//   --shadow-test     (with a save) the 15 parts of the loaded save against the same engine state over an emptied / another save's shadow made fresh (prepare_fresh_shadow);
//                     RON_SHADOW_TEMPLATE=other.SVX names the template
//   --analysis-check  (with a save) the World members the engine does not model (World::analyze_map, the territory limits, the halfland log) as new_random_game
//                     generates them for the save's setup, against the save's own; env RON_REGION_DIFF / RON_WINDOW / RON_WORLD_ARRAYS / RON_HALFLAND / RON_GOOD_Z
//   (a replay without an embedded save is generated from its header: start path of docs/original-save.md section 6.21; with --compare-next a single-player one is
//   compared at the next save's frame; RON_SHADOW_SAVE names the template save, RON_PLACE_TRACE traces Setup::place_unit)
//   --step-diff N     (with a save or replay) which mapped members the engine changes in N frames from the save
//   --compare-next B  (with --replay) run to the frame of replay B's embedded save and list the members that differ from that save
//   --solve-at B      (with --replay) at boundary B: for each of units/builds/groups/guys/leaders/goods/world whose hash differs, test
//                     whether ONE changed byte explains it (Adler-32 algebra) and name the object, member and byte
//   --revert M,..     (with --replay) experiment: put members ("Unit.flags") back to the save's values after the first step
//   --field-report    how many members of each original class the engine maps (the rest stays in the shadow)
//   other options: --quiet --no-diag --mutate N (self-test) --json F --csv F --game DIR --no-art --no-commands --verbose
// Exit code: 1 if the decode fails, 2 if a recomputed value differs from the stored/expected one under --verify / --engine-roundtrip.
#include <cmath>
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
#include "sim/production.hpp"
#include "sim/ai.hpp"
#include "sim/economy.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/pathfinder.hpp"
#include "sim/rules.hpp"
#include "net/playback.hpp"
#include "sim/newgame.hpp"
#include "sim/replay.hpp"
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

void tcoord_scan(sim::EngineProvider& ep, const std::string& path);
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
	if (std::getenv("RON_TCOORD_SCAN")) tcoord_scan(ep, path);
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

PartTrace g_rb_snap;  // RON_REVERT_BYTES: the part's stream at the first boundary

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
		if (std::getenv("RON_TRACE_HDR")) std::printf("   groups container: length %d size %d increment %d flags %02x\n", h.length, h.size, int(h.increment), unsigned(h.flags));
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
	if (part == 13 || part == 14 || part == 12) {  // the global-function parts: rules (Game::walk_rules_data), scenario_data, script_run_time
		const char* gname = part == 12 ? "game" : part == 13 ? "scenario_data" : "run_time_env";
		const char* fname = part == 12 ? "Game::walk_rules_data" : part == 13 ? "ScenarioData::walk_data" : "RunTimeEnv::walk_data";
		w.walk(fname, p.global(gid(gname)), gid(gname), 0);
		return true;
	}
	if (part == 11) {
		dw::Obj world = p.global(gid("world"));
		if (i32(world, 8) > 0) w.walk("World::walk_data", world, gid("world"), 0, {{"part", -1}});
		return true;
	}
	return false;
}

void tcoord_scan(sim::EngineProvider& ep, const std::string& path) {  // RON_TCOORD_SCAN=1: every building's mining list header (length, size, increment) as loaded
	PartTrace tr;
	if (!trace_part(ep, 1, tr)) return;
	const dw::Description& dd = dw::Description::get();
	for (size_t i = 0; i + 3 < tr.ranges.size(); ++i) {
		const auto& r = tr.ranges[i];
		const int fnc = dd.step_function(r.step);
		if (fnc < 0 || std::strcmp(dd.function_name(fnc), "Array<TCoordData>::walk_data") != 0 || r.len != 4) continue;
		const auto& r2 = tr.ranges[i + 1];
		if (r2.len != 4 || r2.who != r.who || r2.o != r.o) continue;
		auto rd32 = [&](size_t at) { return uint32_t(tr.bytes[at]) | uint32_t(tr.bytes[at + 1]) << 8 | uint32_t(tr.bytes[at + 2]) << 16 | uint32_t(tr.bytes[at + 3]) << 24; };
		const auto& r3 = tr.ranges[i + 2];
		std::printf("  MINLIST %s %d/%d length %u size %u inc %02x%02x\n", basename_of(path).c_str(), r.who, r.o, rd32(r.begin), rd32(r2.begin), tr.bytes[r3.begin], tr.bytes[r3.begin + 1]);
		i += 3;
	}
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
	if (const char* at = std::getenv("RON_AT")) {  // RON_AT=off,off,..: the labelled ranges holding those stream offsets
		for (const char* q = at; *q;) {
			char* e = nullptr;
			const long long off = std::strtoll(q, &e, 10);
			if (e == q) break;
			for (const auto& r : tr.ranges) if (int64_t(r.begin) <= off && off < int64_t(r.begin + r.len)) {
				const int fn = d.step_function(r.step);
				std::printf("      AT %lld: %d/%d %s at %zu len %zu\n", off, r.who, r.o, fn >= 0 ? d.function_name(fn) : "?", r.begin, r.len);
			}
			q = *e == ',' ? e + 1 : e;
		}
	}
	if (const char* ro = std::getenv("RON_RANGE_OF")) {  // RON_RANGE_OF=who,o: the stream offsets of that object's hashed ranges
		int rw = -1, rob = -1;
		std::sscanf(ro, "%d,%d", &rw, &rob);
		for (const auto& r : tr.ranges) if (r.who == rw && r.o == rob) {
			const int fn = d.step_function(r.step);
			std::printf("      RANGE_OF %d/%d %s at %zu len %zu\n", rw, rob, fn >= 0 ? d.function_name(fn) : "?", r.begin, r.len);
		}
	}
	if (std::getenv("RON_SOLVE_DUMP") && N <= 20000) {  // the stream and the original's hash, for offline hypothesis tests
		std::printf("    SOLVE_DUMP %s want %08x have %08x bytes ", part_name, want, have);
		for (uint8_t c : tr.bytes) std::printf("%02x", c);
		std::printf("\n");
	}
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
	if (const char* po = std::getenv("RON_SOLVE_OBJ")) {  // RON_SOLVE_OBJ=who,o: two changed bytes (|e| <= 255) anywhere in the ranges of that object
		int pw = 0, pob = 0;
		if (std::sscanf(po, "%d,%d", &pw, &pob) == 2) {
			std::vector<int64_t> pos;
			for (const auto& r : tr.ranges)
				if (r.who == pw && r.o == pob)
					for (size_t k = 0; k < r.len; ++k) pos.push_back(int64_t(r.begin + k));
			if (std::getenv("RON_SOLVE_OBJ_DUMP")) {
				for (const auto& r : tr.ranges)
					if (r.who == pw && r.o == pob) {
						const int fn = d.step_function(r.step);
						int sum = 0;
						std::printf("      range %s step %d begin %zu len %zu sum-from-end-weight %lld bytes:", fn >= 0 ? d.function_name(fn) : "?", r.step, r.begin, r.len, (long long)(N - int64_t(r.begin)));
						for (size_t z = 0; z < r.len; ++z) { sum += tr.bytes[r.begin + z]; if (z < 64) std::printf(" %02x", tr.bytes[r.begin + z]); }
						std::printf(" ... sum %d\n", sum);
					}
			}
			int hits2 = 0;
			for (size_t a1 = 0; a1 < pos.size() && hits2 < 40; ++a1)
				for (size_t a2 = a1 + 1; a2 < pos.size() && hits2 < 40; ++a2) {
					const int64_t w1 = N - pos[a1], w2 = N - pos[a2];
					const int64_t dw_ = ((w1 - w2) % M + M) % M;
					if (dw_ == 0) continue;
					int64_t d1 = ((db - da * w2) % M + M) % M * modpow(dw_, M - 2, M) % M;
					if (d1 > M / 2) d1 -= M;
					int64_t d2 = ((da - d1) % M + M) % M;
					if (d2 > M / 2) d2 -= M;
					if (d1 == 0 || d2 == 0 || d1 < -255 || d1 > 255 || d2 < -255 || d2 > 255) continue;
					const int b1 = tr.bytes[size_t(pos[a1])], b2 = tr.bytes[size_t(pos[a2])];
					if (b1 + d1 < 0 || b1 + d1 > 255 || b2 + d2 < 0 || b2 + d2 > 255) continue;
					auto where = [&](int64_t pp) {
						for (const auto& r : tr.ranges) if (size_t(pp) >= r.begin && size_t(pp) < r.begin + r.len) { const int fn = d.step_function(r.step); std::printf("%s step %d +%zu", fn >= 0 ? d.function_name(fn) : "?", r.step, size_t(pp) - r.begin); return; }
					};
					std::printf("      pair: "); where(pos[a1]); std::printf(" %02x%+lld, ", b1, (long long)d1); where(pos[a2]); std::printf(" %02x%+lld\n", b2, (long long)d2);
					++hits2;
				}
			std::printf("      (%d pair candidates in %zu bytes of %d/%d)\n", hits2, pos.size(), pw, pob);
			// Copy search: one window (1, 2 or 4 bytes) of the object replaced by (a) the value of another window of the same width anywhere in the
			// object (+- 0..2), (b) a constant (0, 1, -1, 0xff.., 0x80000000).
			struct Win { int64_t pos; int wdt; uint32_t v; const char* fn; int step; size_t off; };
			std::vector<Win> wins;
			for (const auto& r : tr.ranges)
				if (r.who == pw && r.o == pob)
					for (size_t off = 0; off < r.len; ++off)
						for (int wdt : {1, 2, 4}) {
							if (off + size_t(wdt) > r.len) continue;
							uint32_t v = 0;
							for (int k = 0; k < wdt; ++k) v |= uint32_t(tr.bytes[r.begin + off + size_t(k)]) << (8 * k);
							const int fn = d.step_function(r.step);
							wins.push_back({int64_t(r.begin + off), wdt, v, fn >= 0 ? d.function_name(fn) : "?", r.step, off});
						}
			auto delta_of = [&](const Win& dst, uint32_t nv, int64_t* ra, int64_t* rb) {
				int64_t sa = 0, sb = 0;
				for (int k = 0; k < dst.wdt; ++k) {
					const int e = int((nv >> (8 * k)) & 0xff) - int((dst.v >> (8 * k)) & 0xff);
					sa += e;
					sb += int64_t(e) * (N - (dst.pos + k));
				}
				*ra = ((sa % M) + M) % M;
				*rb = ((sb % M) + M) % M;
			};
			int hits3 = 0;
			for (const Win& dst : wins) {
				const uint32_t mask = dst.wdt == 4 ? 0xffffffffu : (1u << (8 * dst.wdt)) - 1;
				std::vector<std::pair<uint32_t, std::string>> cands = {{0u, "0"}, {1u, "1"}, {mask, "-1"}, {0x80000000u & mask, "min"}, {0xffu & mask, "0xff"}};
				for (const Win& src : wins) {
					if (src.wdt != dst.wdt || src.pos == dst.pos) continue;
					for (int dl = -2; dl <= 2; ++dl) cands.push_back({(src.v + uint32_t(dl)) & mask, std::string("copy ") + src.fn + " step " + std::to_string(src.step) + " +" + std::to_string(src.off) + " " + std::to_string(dl)});
				}
				for (const auto& c : cands) {
					int64_t sa, sb;
					delta_of(dst, c.first, &sa, &sb);
					if (sa == da && sb == db && c.first != dst.v && hits3 < 40) {
						std::printf("      copy: %s step %d +%zu (%d bytes, %u) := %s (%u)\n", dst.fn, dst.step, dst.off, dst.wdt, dst.v, c.second.c_str(), c.first);
						++hits3;
					}
				}
			}
			std::printf("      (%d copy/constant candidates)\n", hits3);
		}
	}
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
		const int D = std::getenv("RON_SOLVE_D") ? std::atoi(std::getenv("RON_SOLVE_D")) : (N > 300000 ? 64 : 600);
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

// RON_FOOT_SEARCH=1 (with --solve-at B and a differing `world`): which single guy, moved by up to +-3 ucoord cells, makes the CollBlock footprints (the 96-byte
// bitmaps of the world stream) account for the part's Adler delta. The stream positions of the blocks come from the trace.
void footprint_search(sim::Game& game, const PartTrace& tr, uint32_t want) {
	const int64_t M = 65521, N = int64_t(tr.bytes.size());
	const uint32_t have = adler_of(tr.bytes);
	const int64_t tda = ((int64_t(want & 0xffff) - (have & 0xffff)) % M + M) % M, tdb = ((int64_t(want >> 16) - (have >> 16)) % M + M) % M;
	sim::World& w = game.world;
	std::vector<size_t> begins;
	for (const auto& r : tr.ranges) if (r.step == 1351 && r.len == 96) begins.push_back(r.begin);
	std::vector<size_t> blk_begin(w.coll_blocks.size(), size_t(-1));
	size_t k = 0, bad = 0;
	for (int32_t wy = 0; wy < w.ys; ++wy)
		for (int32_t wx = 0; wx < w.xs; ++wx) {
			const int32_t bi = w.w(wx, wy).block;
			if (bi < 0) continue;
			if (k < begins.size()) {
				blk_begin[size_t(bi)] = begins[k];
				if (std::memcmp(&tr.bytes[begins[k]], w.coll_blocks[size_t(bi)].bytes.data(), 96) != 0) ++bad;
			}
			++k;
		}
	std::printf("    footprint search: %zu blocks in the world, %zu block ranges in the stream, %zu blocks differ from the trace (row-major scan); target a=%lld b=%lld\n", k, begins.size(), bad, (long long)tda, (long long)tdb);
	auto bit_set = [&](int32_t X, int32_t Y, int32_t* blk, size_t* byte_pos, uint8_t* mask) -> int {  // 1 set, 0 clear, -1 no block
		if (!w.in_u(X, Y)) return -1;
		const int32_t bi = w.w(X >> 4, Y >> 4).block;
		if (bi < 0) return -1;
		const int32_t idx = (X % 16) * 16 + (Y % 16);
		*blk = bi; *byte_pos = blk_begin[size_t(bi)] + size_t(idx >> 3); *mask = uint8_t(1u << (idx & 7));
		return (w.coll_blocks[size_t(bi)].bytes[size_t(idx >> 3)] & *mask) ? 1 : 0;
	};
	int hits = 0;
	struct Single { int64_t da, db; int who, o, gi, dux, duy; };
	std::vector<Single> singles;
	for (int who = 0; who < 8; ++who)
		for (int32_t o = 0; o < game.objects[size_t(who)].unit_mark; ++o) {
			const sim::Unit* u = game.unit(who, o);
			if (!u || !u->alive() || u->inside_up >= 0) continue;
			const int32_t r = std::min(u->ptype->new_block_radius, 10);
			if (r == 0) continue;
			for (int gi = 0; gi < int(u->guy_mark) && size_t(gi) < u->guys.size(); ++gi) {
				const int32_t oux = u->guys[size_t(gi)].x / 48, ouy = u->guys[size_t(gi)].y / 48;
				for (int dux = -3; dux <= 3; ++dux)
					for (int duy = -3; duy <= 3; ++duy) {
						if (!dux && !duy) continue;
						const int32_t nux = oux + dux, nuy = ouy + duy;
						int64_t da = 0, db = 0;
						bool ok = true;
						auto cell = [&](int32_t X, int32_t Y, bool set) {
							int32_t bi; size_t pos; uint8_t mask;
							const int c = bit_set(X, Y, &bi, &pos, &mask);
							if (c < 0) { if (set && w.in_u(X, Y)) ok = false; return; }
							if (set == (c == 1)) return;
							const int64_t e = set ? int64_t(mask) : -int64_t(mask);
							da += e;
							db += e * (N - int64_t(pos));
						};
						for (int32_t X = oux - r; X <= oux + r && ok; ++X)
							for (int32_t Y = ouy - r; Y <= ouy + r; ++Y)
								if (std::abs(X - nux) > r || std::abs(Y - nuy) > r) cell(X, Y, false);
						for (int32_t X = nux - r; X <= nux + r && ok; ++X)
							for (int32_t Y = nuy - r; Y <= nuy + r; ++Y)
								if (std::abs(X - oux) > r || std::abs(Y - ouy) > r) cell(X, Y, true);
						if (!ok) continue;
						if (((da % M) + M) % M == tda && ((db % M) + M) % M == tdb && hits < 40) {
							++hits;
							std::printf("      FOOT HIT: unit %d/%d guy %d cell (%d,%d) -> (%d,%d) (d %+d,%+d) r %d\n", who, o, gi, oux, ouy, nux, nuy, dux, duy, r);
						}
						if (da != 0 || db != 0) singles.push_back({((da % M) + M) % M, ((db % M) + M) % M, who, o, gi, dux, duy});
					}
			}
		}
	std::printf("    footprint search: %d hit(s) among %zu single-guy moves\n", hits, singles.size());
	std::multimap<std::pair<int64_t, int64_t>, size_t> idx;
	for (size_t i = 0; i < singles.size(); ++i) idx.insert({{singles[i].da, singles[i].db}, i});
	int phits = 0;
	for (size_t i = 0; i < singles.size() && phits < 60; ++i) {
		const int64_t ra = ((tda - singles[i].da) % M + M) % M, rb = ((tdb - singles[i].db) % M + M) % M;
		auto rg = idx.equal_range({ra, rb});
		for (auto it = rg.first; it != rg.second && phits < 60; ++it) {
			const Single& a1 = singles[i]; const Single& a2 = singles[it->second];
			if (it->second <= i || (a1.who == a2.who && a1.o == a2.o && a1.gi == a2.gi)) continue;
			++phits;
			std::printf("      FOOT PAIR: %d/%d g%d (%+d,%+d) + %d/%d g%d (%+d,%+d)\n", a1.who, a1.o, a1.gi, a1.dux, a1.duy, a2.who, a2.o, a2.gi, a2.dux, a2.duy);
		}
	}
	std::printf("    footprint search: %d pair hit(s)\n", phits);
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
std::vector<std::array<int, 3>> g_drops;
bool g_rng_quick = false;
int g_drop_last = -1, g_drop_count = 0;
int g_ins_frame = -1, g_ins_n = -1, g_ins_k = 0, g_ins_count = 0, g_ins_last = -1;  // RON_INSERT_N="F:n:k": k extra draws just before the n-th (1-based) draw of frame F
void rng_hook_fn() {
	if (g_trace_game && g_ins_frame >= 0 && g_trace_game->frame != g_ins_last) { g_ins_last = g_trace_game->frame; g_ins_count = 0; }
	if (g_trace_game && g_ins_frame >= 0 && g_trace_game->frame == g_ins_frame && ++g_ins_count == g_ins_n) {
		uint32_t sd = g_trace_game->rng.seed();
		for (int i = 0; i < g_ins_k; ++i) sd = sd * 0x19660Du + 0x3C6EF35Fu;
		g_trace_game->rng.reseed(sd);
	}
	if (g_trace_game && !g_drops.empty()) {  // RON_DROP_N="F:n:m[,F:n:m..]": m draws less consumed before the n-th (1-based) draw of frame F
		if (g_drop_last != g_trace_game->frame) { g_drop_last = g_trace_game->frame; g_drop_count = 0; }
		++g_drop_count;
		for (const auto& d : g_drops)
			if (d[0] == g_trace_game->frame && d[1] == g_drop_count) {
				const uint32_t ainv = [] { uint32_t x = 0x19660Du; for (int i = 0; i < 5; ++i) x *= 2 - 0x19660Du * x; return x; }();
				uint32_t sd = g_trace_game->rng.seed();
				for (int i = 0; i < d[2]; ++i) sd = (sd - 0x3C6EF35Fu) * ainv;
				g_trace_game->rng.reseed(sd);
			}
	}
	if (g_rng_quick) return;
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
int g_idle_trace_lo = -1, g_idle_trace_hi = -1;  // RON_IDLE_TRACE=F0:F1 prints every idle-variant pick of Guy::set_anim in the frames
void idle_pick_hook(sim::Game& g, sim::Unit& u) {
	g_idle_picks.push_back({g.frame, u.who, u.o, u.type, g.rng.seed()});
	if (g.frame >= g_idle_trace_lo && g.frame <= g_idle_trace_hi)
		std::printf("IDLEPICK frame %d who %d o %d type %d seed %08x pos %d,%d\n", g.frame, u.who, u.o, u.type, g.rng.seed(), u.x, u.y);
}
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

	// the engine's game: from the embedded save, or (a recording without one: a random map of the original) generated from the setup of the
	// header's GameInfo and random-game-info block, the way Setup::build_game builds it from the lobby (net/playback.hpp original_random_setup)
	const bool from_setup = save.empty();
	static std::unique_ptr<RulesCache> cache;
	if (!cache) cache = std::make_unique<RulesCache>(game_dir);
	sim::OriginalLoadReport rep;
	std::unique_ptr<sim::Game> game;
	sim::GameSetup setup;
	const sim::Rules* rules_p = nullptr;
	if (from_setup) {
		sim::Replay srp;
		sim::RandomGameSetup rsetup;
		std::string e;
		if (!sim::read_replay(path, srp, e)) { std::printf("replay: %s\n", e.c_str()); return 1; }
		if (!ron::net::original_random_setup(srp, rsetup, e)) { std::printf("replay: no embedded save and %s\n", e.c_str()); return 1; }
		setup = ron::net::original_rules_setup(srp.header);
		rules_p = &cache->get(setup);
		game = ron::net::original_random_game(cache->files, *rules_p, nullptr, srp, rsetup, e);
		if (!game) { std::printf("replay: the generator failed: %s\n", e.c_str()); return 1; }
		rep.rules_equal = true;
		std::printf("  start: random map generated by new_random_game from the header (style %d, size %d, seed %u, %d nations); %s\n", rsetup.map_style, rsetup.map_size,
		            rsetup.seed, rsetup.num_nations, ron::sim::random_setup_string(rsetup).c_str());
	} else {
		dw::Tree peek(dw::Description::get());
		dw::SaveDecode psd = dw::decode_save(peek, save.data(), save.size(), 0);
		if (!psd.result.ok) { std::printf("replay: decode failed\n"); return 1; }
		setup.starting_technology = int32_t(field_int(peek, "game.info.starting_technology"));
		setup.starting_technology2 = int32_t(field_int(peek, "game.info.starting_technology2"));
		setup.ending_technology = int32_t(field_int(peek, "game.info.ending_technology"));
		setup.game_rules = int32_t(field_int(peek, "game.info.game_rules"));
		setup.flag8 = (field_int(peek, "game.info.flags") & 8) != 0;
		rules_p = &cache->get(setup);
		game = sim::load_original_save(cache->files, *rules_p, nullptr, save, &rep);
		if (!game) { std::printf("replay: load failed: %s\n", rep.error.c_str()); return 1; }
	}
	const sim::Rules& rules = *rules_p;
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

	if (std::getenv("RON_MUZZLE_CHECK") && game->anims) {
		// RON_MUZZLE_CHECK=1: every ammo in flight of the loaded save against the engine's muzzle offsets (GraphicEvents::execute_game_events): the shooter's guys
		// with a release event for the ammo piece, start = guy position + offset; "exact" when (sx, sy, sz) all agree for some guy / event (a shooter that moved
		// since the shot cannot agree). Per piece class: turret (guy_flags 0x100) or not.
		int n = 0, exact[2] = {0, 0}, xy[2] = {0, 0}, none[2] = {0, 0};
		for (size_t ai = 0; ai < game->ammo.size(); ++ai) {
			const sim::Ammo& a = game->ammo[ai];
			if (!a.in_use() || a.who < 0 || a.o < 0) continue;
			const sim::Unit* u0 = game->unit(a.who, a.o);
			if (!u0 || !u0->alive()) continue;
			++n;
			bool ex = false, exy = false, tur = false;
			for (const sim::Guy& gy : u0->guys) {
				const auto* evs = game->anims->release_events(gy.gpiece);
				if (!evs) continue;
				if (gy.guy_flags & 0x100) tur = true;
				for (const sim::ReleaseEvent& e : *evs) {
					if (e.ammo != a.gpiece) continue;
					int32_t off[3];
					sim::guy_muzzle_offset(*game, gy, e, off);
					if (gy.x + off[0] == a.sx && gy.y + off[1] == a.sy) { exy = true; if (gy.z + off[2] == a.sz) ex = true; }
					else if (std::getenv("RON_MUZZLE_CHECK")[0] == '3') std::printf("    cand guy %d node %d anim %d frame %d: guy (%d,%d,%d) off (%d,%d,%d) -> (%d,%d,%d) vs saved (%d,%d,%d) angle %08x turrets %08x %08x %08x %08x\n", gy.guy_num, e.node, e.anim, e.frame, gy.x, gy.y, gy.z, off[0], off[1], off[2], gy.x + off[0], gy.y + off[1], gy.z + off[2], a.sx, a.sy, a.sz, unsigned(gy.angle), unsigned(gy.turret_angles[0]), unsigned(gy.turret_angles[1]), unsigned(gy.turret_angles[2]), unsigned(gy.turret_angles[3]));
				}
			}
			const int c = tur ? 1 : 0;
			if (ex) ++exact[c]; else if (exy) ++xy[c]; else ++none[c];
			if (!ex && std::getenv("RON_MUZZLE_CHECK")[0] == '2') std::printf("  muzzle: ammo %zu from %d/%d (type %d%s) -> %d/%d sx %d sy %d sz %d: %s\n", ai, a.who, a.o, u0->type, tur ? " turret" : "", a.whom, a.ox, a.sx, a.sy, a.sz, exy ? "xy only" : "no match");
		}
		std::printf("  muzzle check: %d ammo in flight; plain exact %d xy-only %d none %d; turret exact %d xy-only %d none %d\n", n, exact[0], xy[0], none[0], exact[1], xy[1], none[1]);
	}

	if (std::getenv("RON_AMMO")) {  // RON_AMMO=1: every ammo of the loaded save
		for (size_t i = 0; i < game->ammo.size(); ++i) {
			const sim::Ammo& a = game->ammo[i];
			if (!a.in_use()) continue;
			const sim::Object* sh = game->object(a.who, a.o);
			std::printf("  AMMO %zu flags %02x piece %d from %d/%d (type %d) -> %d/%d time %u/%u accuracy %d graph_index %d splash %d start (%d,%d,%d) end (%d,%d,%d)\n", i, unsigned(a.flags), a.gpiece, a.who, a.o, sh ? sh->type : -1, a.whom, a.ox, a.cur_time, a.total_time, int(a.accuracy), a.graph_index, a.splash_area, a.sx, a.sy, a.sz, a.ex, a.ey, a.ez);
		}
	}
	if (std::getenv("RON_SCAN_PACKERS")) {  // every unit of the loaded save with a pack / unpack CAST order, with the whole order list
		for (int who = 0; who < sim::kNumLeaders; ++who)
			for (int o = 0; o < game->objects[size_t(who)].unit_mark; ++o) {
				const sim::Unit* tu = game->unit(who, o);
				if (!tu || !tu->alive()) continue;
				bool hit = false;
				for (const sim::Order& od : tu->orders) if (od.type == sim::kCastSpell && od.spell >= 0x28b && od.spell <= 0x292) hit = true;
				if (!hit) continue;
				std::printf("  PK %d/%d type %d at %d,%d masks %08x/%08x spell_time %d idle %d orders_xy %d,%d tol %d dang %08x ang %08x:", who, o, tu->type, tu->x, tu->y, unsigned(tu->unit_masks), unsigned(tu->unit_masks2), int(tu->spell_time), int(tu->idle), tu->orders_x, tu->orders_y, int(tu->tolerance), unsigned(tu->dest_angle), unsigned(tu->angle));
				for (const sim::Order& od : tu->orders)
					std::printf(" [t%d fl%02x x%d y%d ang%08x dest%d tol%d pause%d retry%d att%d timer%d facing%d dx%d dy%d lx%d ly%d cx%d cy%d ox%d oy%d offx%d offy%d | spell%d paid%d ox%d wh%d uid%x]", int(od.type), unsigned(od.flags), od.x, od.y, unsigned(od.angle), od.dest, od.tolerance, od.pause, od.retry, od.attempts, od.timer, od.facing, od.dest_x, od.dest_y, od.last_x, od.last_y, od.coll_x, od.coll_y, od.orig_x, od.orig_y, int(od.off_x), int(od.off_y), od.spell, od.paid, od.ox, od.whom, unsigned(od.uid));
				std::printf("\n");
			}
	}
	if (std::getenv("RON_AA")) {  // RON_AA=1: the air-defence buildings (types 523 - 525) of the loaded save with their animation clock (BuildData.recharging) and target state
		for (int who = 0; who < 8; ++who)
			for (int o = sim::kBuildBase; o < game->objects[size_t(who)].build_mark; ++o) {
				const sim::Build* b = game->build(who, o);
				if (!b || !b->alive() || b->type < 523 || b->type > 525) continue;
				std::printf("  AA %d/%d type %d recharging %d near %d/%d attack %d/%d masks %04x flags %02x gpiece %d jc %08x founder %d age %d\n", who, o, b->type, int(b->recharging), int(b->near_o), int(b->near_who), int(b->attack_ox), int(b->attack_whom), unsigned(b->build_masks), unsigned(b->flags), int(b->gpiece), unsigned(b->job_counter), int(b->founder), int(b->max_age));
			}
	}

	if (std::getenv("RON_TURRETS")) {  // RON_TURRETS=1: the units whose guy 0 carries the turret flag (0x100) in the loaded save, with their pivot state
		for (int who = 0; who < sim::kNumLeaders; ++who)
			for (int o = 0; o < game->objects[size_t(who)].unit_mark; ++o) {
				const sim::Unit* u0 = game->unit(who, o);
				if (!u0 || !u0->alive() || u0->guys.empty() || !(u0->guys[0].guy_flags & 0x100)) continue;
				const sim::Guy& gy = u0->guys[0];
				std::printf("  turret unit %d/%d type %d at (%d,%d) angle %08x guy angle %08x n %d turrets %08x %08x %08x %08x des %08x %08x %08x %08x node %x/%x ox %d/%d order %d cavarch %d\n", who, o, u0->type, u0->x, u0->y, unsigned(u0->angle), unsigned(gy.angle),
				            game->anims ? game->anims->pivot_count(gy.type) : -1, unsigned(gy.turret_angles[0]), unsigned(gy.turret_angles[1]), unsigned(gy.turret_angles[2]), unsigned(gy.turret_angles[3]),
				            unsigned(gy.des_turret_angles[0]), unsigned(gy.des_turret_angles[1]), unsigned(gy.des_turret_angles[2]), unsigned(gy.des_turret_angles[3]), unsigned(gy.node_flags), unsigned(gy.des_node_flags), int(gy.ox), int(gy.whom), int(u0->order_type()), int(u0->cavarch_o));
			}
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
	if (std::getenv("RON_ZCHECK")) {  // RON_ZCHECK=1: every ground guy of the loaded save whose saved z differs from find_data_z(x, y)
		int n = 0, bad = 0;
		for (int w = 0; w < sim::kNumLeaders; ++w) for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) {
			sim::Unit* u = game->unit(w, o);
			if (!u || !u->alive() || !u->ptype || u->ptype->domain != 0 || !u->on_map()) continue;
			for (size_t gi = 0; gi < u->guys.size(); ++gi) {
				const sim::Guy& gy = u->guys[gi];
				const int32_t z = sim::find_data_z(*game, gy.x, gy.y, 0);
				++n;
				if (z != gy.z) { ++bad; if (bad <= 40) std::printf("  ZCHECK %d/%d guy %zu at (%d,%d): saved z %d engine %d (last_z %d) tile mask %04x\n", w, o, gi, gy.x, gy.y, gy.z, z, gy.last_z, unsigned(game->world.t(sim::tcoord_of(gy.x), sim::tcoord_of(gy.y)))); }
			}
		}
		std::printf("  ZCHECK: %d guys, %d differ\n", n, bad);
	}

	// The game script the replay header names (the embedded save has none): "no_nukes" in the 2026.08.30 chain.
	if (hdr && !std::getenv("RON_NO_SCRIPTS")) {
		const Json* hs = hdr->get("scenarioScript");
		if (hs && hs->t == Json::Str && !hs->s.empty()) {
			std::string err;
			if (sim::original_attach_game_script(*game, cache->files, hs->s, &err)) std::printf("  game script \"%s\" attached (Game::scripts created)\n", hs->s.c_str());
			else std::printf("  game script \"%s\" not attached: %s\n", hs->s.c_str(), err.c_str());
		}
	}
	if (from_setup) ron::net::prepare_fresh_shadow(*game);   // the shadow of a generated game fits its engine state (after the prologue and the script)
	if (const char* dpk = std::getenv("RON_DUMP_PART_START")) {
		// the labelled stream of part k (trace_part) of the start state, hex per range: RON_DUMP_PART_START=k
		sim::EngineProvider e0(*game);
		PartTrace t0;
		trace_part(e0, std::atoi(dpk), t0);
		const dw::Description& dd = dw::Description::get();
		std::printf("  part %s at the start: %zu bytes\n", dw::kCheckPartNames[std::atoi(dpk)], t0.bytes.size());
		for (const PartTrace::R& r : t0.ranges) {
			std::printf("    [%5zu +%3zu] who %2d o %4d %-28s", r.begin, r.len, r.who, r.o, dd.function_name(dd.step_function(r.step)));
			for (size_t k = 0; k < r.len && k < 40; ++k) std::printf(" %02x", t0.bytes[r.begin + k]);
			std::printf("\n");
		}
	}
	auto rows = all_check_sums(csv_path, path);
	// A single-player recording carries no rows; with --compare-next its end state is still checked against the next recording's embedded save.
	if (rows.empty() && next_path.empty()) { std::printf("replay: no CHECK_SUMS rows for %s in %s\n", basename_of(path).c_str(), csv_path.c_str()); return 1; }
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
	int64_t last_boundary = expect.empty() ? start : expect.rbegin()->first;
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
		if (!expect.empty()) {
			const auto& e1 = expect.begin()->second;
			std::printf("  save state (frame %lld) against the first row (boundary %lld):", (long long)start, (long long)expect.begin()->first);
			for (int k = 0; k < dw::kCheckPartCount; ++k) std::printf(" %s=%s", dw::kCheckPartNames[k], base.v[k] == e1[size_t(k)] ? "=" : "!");
			std::printf("\n");
		} else {
			std::printf("  (no CHECK_SUMS rows: single player; the state at the next save's frame is compared below)\n");
		}
	}
#ifdef RON_RNG_PROFILE_HOOK
	if (std::getenv("RON_RNG_PROFILE")) sim::g_rng_hook = rng_hook_fn;
	if (const char* tr = std::getenv("RON_RNG_TRACE")) { g_trace_game = game.get(); g_trace_until = std::atoi(tr); }
	if (const char* dn = std::getenv("RON_DROP_N")) {
		g_trace_game = game.get();
		std::stringstream dss(dn);
		std::string dt;
		while (std::getline(dss, dt, ',')) { std::array<int, 3> d{0, 0, 0}; if (std::sscanf(dt.c_str(), "%d:%d:%d", &d[0], &d[1], &d[2]) == 3) g_drops.push_back(d); }
		g_rng_quick = std::getenv("RON_RNG_QUICK") != nullptr;
	}
	if (const char* in = std::getenv("RON_INSERT_N")) { g_trace_game = game.get(); std::sscanf(in, "%d:%d:%d", &g_ins_frame, &g_ins_n, &g_ins_k); }
#endif
	// RON_FISH_TIMELINE=1 (with --compare-next): see fish_timeline() below
	const bool fish_timeline = std::getenv("RON_FISH_TIMELINE") != nullptr;
	std::vector<std::pair<int, uint32_t>> frame_seeds;
	const uint32_t seed_load = game->rng.seed();
	if (const char* it = std::getenv("RON_IDLE_TRACE")) std::sscanf(it, "%d:%d", &g_idle_trace_lo, &g_idle_trace_hi);
	if (fish_timeline || g_idle_trace_lo >= 0) sim::g_idle_pick_hook = idle_pick_hook;
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
			if (const char* rb0 = std::getenv("RON_REVERT_BYTES")) {  // snapshot of the part's stream at F0 (see below); F0 may be any frame
				int rp = 0, r0 = 0, r1 = 0;
				if (std::sscanf(rb0, "%d:%d:%d", &rp, &r0, &r1) == 3 && f == r0) { sim::EngineProvider e0(*game); g_rb_snap = PartTrace(); trace_part(e0, rp, g_rb_snap); }
			}
				if (const char* td = std::getenv("RON_TRACE_DIFF")) {  // RON_TRACE_DIFF=part:B0:B1: the labelled bytes of part `part` that changed between boundaries B0 and B1 (same stream length)
					static PartTrace td0;
					int tp = 0, tb0 = 0, tb1 = 0;
					if (std::sscanf(td, "%d:%d:%d", &tp, &tb0, &tb1) == 3 && (f == tb0 || f == tb1)) {
						sim::EngineProvider etd(*game);
						PartTrace t1;
						if (trace_part(etd, tp, t1)) {
							if (f == tb0) td0 = std::move(t1);
							else if (t1.bytes.size() != td0.bytes.size()) {
								std::printf("  TRACE_DIFF: stream length %zu -> %zu\n", td0.bytes.size(), t1.bytes.size());
								std::map<std::pair<int, int>, size_t> l0, l1;
								for (const PartTrace::R& r : td0.ranges) l0[{r.who, r.o}] += r.len;
								for (const PartTrace::R& r : t1.ranges) l1[{r.who, r.o}] += r.len;
								for (const auto& kv : l0) { auto it = l1.find(kv.first); if (it == l1.end() || it->second != kv.second) std::printf("    object %d/%d: %zu -> %zu bytes\n", kv.first.first, kv.first.second, kv.second, it == l1.end() ? size_t(0) : it->second); }
								for (const auto& kv : l1) if (!l0.count(kv.first)) std::printf("    object %d/%d: new, %zu bytes\n", kv.first.first, kv.first.second, kv.second);
							}
							if (true) {  // aligned by (who, o, step, occurrence): the byte lengths of single objects may differ
								const dw::Description& dd = dw::Description::get();
								std::map<std::tuple<int, int, int, int>, std::pair<size_t, size_t>> idx0;  // -> (begin, len) in td0
								{ std::map<std::tuple<int, int, int>, int> occ; for (const PartTrace::R& r : td0.ranges) { const int k = occ[{r.who, r.o, r.step}]++; idx0[{r.who, r.o, r.step, k}] = {r.begin, r.len}; } }
								std::map<std::tuple<int, int, int>, int> occ1;
								for (const PartTrace::R& r : t1.ranges) {
									const int k = occ1[{r.who, r.o, r.step}]++;
									auto it0 = idx0.find({r.who, r.o, r.step, k});
									if (it0 == idx0.end()) { std::printf("  TDIFF %d/%d step %d #%d: new range of %zu bytes\n", r.who, r.o, r.step, k, r.len); continue; }
									if (it0->second.second != r.len) { std::printf("  TDIFF %d/%d step %d #%d: length %zu -> %zu\n", r.who, r.o, r.step, k, it0->second.second, r.len); continue; }
									size_t first = r.len, last = 0;
									for (size_t q = 0; q < r.len; ++q) if (t1.bytes[r.begin + q] != td0.bytes[it0->second.first + q]) { if (first == r.len) first = q; last = q; }
									if (first == r.len) continue;
									const int fn = dd.step_function(r.step);
									std::printf("  TDIFF %d/%d %s step %d range+%zu..%zu of %zu:", r.who, r.o, fn >= 0 ? dd.function_name(fn) : "?", r.step, first, last, r.len);
									for (size_t q = first; q <= last && q < first + 16; ++q) std::printf(" %02x->%02x", td0.bytes[it0->second.first + q], t1.bytes[r.begin + q]);
									std::printf("\n");
								}
							}
						}
					}
				}
			if (const char* ld = std::getenv("RON_LDR_DUMP")) {  // RON_LDR_DUMP=who:F0:F1: the leader's score fields before each frame of [F0, F1]
				int lw = 0, l0 = 0, l1 = 0;
				if (std::sscanf(ld, "%d:%d:%d", &lw, &l0, &l1) == 3 && f >= l0 && f <= l1) {
					const sim::Leader& L = game->leaders[size_t(lw)];
					std::printf("  LDR f%d who %d score %d combat %d wonders %d research %d upgrades %d economy %d buildings %d units %d territory %d pop %d explored %d units_killed %d kills_cf %d\n", int(f), lw, L.score, L.score_combat, L.score_wonders, L.score_research, L.score_upgrades, L.score_economy, L.score_buildings, L.score_units, L.score_territory, L.score_pop, L.score_explored, int(L.units_killed), int(L.kills_current_frame));
				}
			}
			if (const char* rs = std::getenv("RON_RNG_SKIP")) {  // RON_RNG_SKIP=F:k: before frame F the generator is moved k draws (k < 0: back); experiment for a draw-count offset
				int rf = 0, rk = 0;
				if (std::sscanf(rs, "%d:%d", &rf, &rk) == 2 && f == rf) {
					const uint32_t a = 0x19660Du, c = 0x3C6EF35Fu, ainv = [] { uint32_t x = 0x19660Du; for (int i = 0; i < 5; ++i) x *= 2 - 0x19660Du * x; return x; }();
					uint32_t sd = game->rng.seed();
					for (int i = 0; i < std::abs(rk); ++i) sd = rk > 0 ? sd * a + c : (sd - c) * ainv;
					game->rng.reseed(sd);
				}
			}
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
				for (const char* ue_name : {"RON_UEXP", "RON_UEXP2", "RON_UEXP3"}) if (const char* ue = std::getenv(ue_name)) {
					// experiment: RON_UEXP="who:o@boundary:op,op,.." applies edits to one unit before the sums of that boundary. ops: fl<i>=<hex> (order i flags),
					// m^=<hex> / m2^=<hex> (unit_masks / unit_masks2 xor), idle=<n>, wait=<n>, pop<i> (erase order i), spell_time=<n>, paid<i>=<n>
					int uw = 0, uo = 0, at = 0, n = 0;
					if (std::sscanf(ue, "%d:%d@%d:%n", &uw, &uo, &at, &n) == 3 && f == at) {
						sim::Unit* tu = game->unit(uw, uo);
						std::string ops(ue + n);
						size_t pos = 0;
						while (tu && pos <= ops.size()) {
							size_t e = ops.find(',', pos);
							std::string op = ops.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
							pos = e == std::string::npos ? ops.size() + 1 : e + 1;
							unsigned v = 0; int i = 0;
							if (std::sscanf(op.c_str(), "fl%d=%x", &i, &v) == 2 && size_t(i) < tu->orders.size()) tu->orders[size_t(i)].flags = uint8_t(v);
							else if (std::sscanf(op.c_str(), "m^=%x", &v) == 1) tu->unit_masks ^= v;
							else if (std::sscanf(op.c_str(), "m2^=%x", &v) == 1) tu->unit_masks2 ^= v;
							else if (std::sscanf(op.c_str(), "idle=%d", &i) == 1) tu->idle = uint8_t(i);
							else if (std::sscanf(op.c_str(), "wait=%d", &i) == 1) tu->waiting = int8_t(i);
							else if (std::sscanf(op.c_str(), "spell_time=%d", &i) == 1) tu->spell_time = int16_t(i);
							else if (std::sscanf(op.c_str(), "pop%d", &i) == 1 && size_t(i) < tu->orders.size()) tu->orders.erase(tu->orders.begin() + i);
							else if (std::sscanf(op.c_str(), "paid%d=%d", &i, &n) == 2 && size_t(i) < tu->orders.size()) tu->orders[size_t(i)].paid = n;
							else if (std::sscanf(op.c_str(), "oflags^=%x", &v) == 1) tu->flags ^= v;
							else if (std::sscanf(op.c_str(), "dang=%x", &v) == 1) tu->dest_angle = v;
							else if (std::sscanf(op.c_str(), "ang=%x", &v) == 1) tu->angle = v;
							else if (std::sscanf(op.c_str(), "ox=%d", &i) == 1) tu->orders_x = i;
							else if (std::sscanf(op.c_str(), "oy=%d", &i) == 1) tu->orders_y = i;
							else if (std::sscanf(op.c_str(), "tol=%d", &i) == 1) tu->tolerance = i;
							else if (std::sscanf(op.c_str(), "qt=%d", &i) == 1) tu->queue_time = i;
							else if (std::sscanf(op.c_str(), "rare=%d", &i) == 1) tu->rare = i;
							else if (std::sscanf(op.c_str(), "good_obj=%d", &i) == 1) tu->good_obj = int16_t(i);
							else if (std::sscanf(op.c_str(), "m3^=%x", &v) == 1) tu->unit_masks ^= v;
							else std::printf("  UEXP: unknown op %s\n", op.c_str());
						}
						std::printf("  UEXP applied to %d/%d at %d\n", uw, uo, at);
					}
				}
				if (const char* bt = std::getenv("RON_B14")) {  // RON_B14=who,o,B : brute force the position of one building at boundary B against `builds`
					int bw = 0, bo = 0, bB = 0;
					std::sscanf(bt, "%d,%d,%d", &bw, &bo, &bB);
					if (f == bB) {
						sim::Build* b = game->build(bw, bo);
						const int32_t x0 = b->x, y0 = b->y, z0 = b->z;
						for (int dx = -8; dx <= 8; ++dx)
							for (int dy = -8; dy <= 8; ++dy)
								for (int dz : {0, 1}) {
									b->x = x0 + dx * 96; b->y = y0 + dy * 96;
									b->z = dz ? sim::find_data_z(*game, b->x, b->y, 1) : z0;
									sim::EngineProvider er(*game);
									dw::CheckSumParts gr = dw::compute_check_sums(er);
									if (gr.ok && gr.v[1] == it->second[1]) std::printf("  B14 HIT builds: dx %d dy %d (x %d y %d) z %d (dz %d)\n", dx * 96, dy * 96, b->x, b->y, b->z, dz);
								}
						b->x = x0; b->y = y0; b->z = z0;
					}
				}
				if (const char* ub = std::getenv("RON_UBF")) {  // RON_UBF=who,o,B : single-member perturbations of one unit (and its orders) at boundary B against `units`
					int uw = 0, uo = 0, uB = 0;
					std::sscanf(ub, "%d,%d,%d", &uw, &uo, &uB);
					if (f == uB) {
						sim::Unit* u = game->unit(uw, uo);
						const std::deque<sim::Order> saved_orders = u->orders;
						const uint32_t saved_masks = u->unit_masks, saved_masks2 = u->unit_masks2, saved_angle = u->angle, saved_dang = u->dest_angle;
						int tried = 0, hits = 0;
						struct Pert { std::string what; int64_t da, db; };
						std::vector<Pert> perts;
						int64_t base_a = 0, base_b = 0;
						const int64_t MM = 65521;
						auto ab_of = [&](int64_t* ra, int64_t* rb) {
							sim::EngineProvider er(*game);
							PartTrace tr;
							if (!trace_part(er, 0, tr)) return false;
							const uint32_t h = adler_of(tr.bytes);
							*ra = h & 0xffff;
							*rb = h >> 16;
							return true;
						};
						ab_of(&base_a, &base_b);
						const int64_t want_a = it->second[0] & 0xffff, want_b = it->second[0] >> 16;
						const int64_t tA = ((want_a - base_a) % MM + MM) % MM, tB = ((want_b - base_b) % MM + MM) % MM;
						auto test = [&](const std::string& what) {
							int64_t a = 0, b = 0;
							++tried;
							if (ab_of(&a, &b)) {
								perts.push_back({what, ((a - base_a) % MM + MM) % MM, ((b - base_b) % MM + MM) % MM});
								if (a == want_a && b == want_b) { std::printf("  UBF HIT: %s\n", what.c_str()); ++hits; }
							}
							u->orders = saved_orders;
						};
						auto vals32 = [](int64_t cur) { return std::vector<int64_t>{0, 1, -1, 2, cur + 1, cur - 1, cur + 2, cur - 2, 0xffff, 255, 0x80000000LL, 3}; };
						// Unit-level scalars.
						#define UBF_I(field, ty) { const auto old_ = u->field; for (int64_t v : vals32(int64_t(old_))) { if (ty(v) == old_) continue; u->field = ty(v); test(std::string(#field) + "=" + std::to_string(v)); } u->field = old_; }
						UBF_I(idle, uint8_t) UBF_I(waiting, int8_t) UBF_I(tolerance, int32_t) UBF_I(queue_time, int32_t) UBF_I(orders_x, int32_t) UBF_I(orders_y, int32_t)
						UBF_I(los_x, int32_t) UBF_I(los_y, int32_t) UBF_I(spell_time, int16_t) UBF_I(mana_burn, int16_t) UBF_I(recharging, uint8_t) UBF_I(rare, int32_t)
						UBF_I(good_obj, int16_t) UBF_I(doober, int16_t) UBF_I(cara, int16_t) UBF_I(supply, int16_t) UBF_I(myspeed, int16_t) UBF_I(myarmor, int16_t)
						UBF_I(form, int8_t) UBF_I(form_mod, int8_t) UBF_I(full, int8_t) UBF_I(safe, int8_t) UBF_I(play, int8_t) UBF_I(path_recursion, uint8_t) UBF_I(stance, int8_t)
						UBF_I(collide, int16_t) UBF_I(collide_o, int16_t) UBF_I(collide_guy, int16_t) UBF_I(collide_who, int8_t) UBF_I(collide_frame, int32_t) UBF_I(damage_frame, int32_t)
						UBF_I(attrition, int16_t) UBF_I(num_queued, int16_t) UBF_I(trench_angle, uint32_t) UBF_I(air_alt, int32_t) UBF_I(group, int16_t) UBF_I(hold_frames, uint16_t)
						UBF_I(healing, int16_t) UBF_I(near_o, int16_t) UBF_I(near_who, int16_t) UBF_I(targeted, int8_t) UBF_I(visible, int8_t) UBF_I(damage_frac, int8_t)
						#undef UBF_I
						for (uint32_t cand : {saved_angle, uint32_t(0), saved_dang, saved_angle + 0x80000000u}) { if (cand == saved_dang) continue; u->dest_angle = cand; test("dest_angle=" + std::to_string(cand)); }
						u->dest_angle = saved_dang;
						for (int b = 0; b < 32; ++b) { u->unit_masks = saved_masks ^ (1u << b); test("unit_masks^bit" + std::to_string(b)); u->unit_masks = saved_masks; u->unit_masks2 = saved_masks2 ^ (1u << b); test("unit_masks2^bit" + std::to_string(b)); u->unit_masks2 = saved_masks2; }
						// Orders.
						for (size_t oi = 0; oi < saved_orders.size(); ++oi) {
							const std::string pre = "order" + std::to_string(oi) + ".";
							sim::Order& od = u->orders[oi];
							for (int b = 0; b < 8; ++b) { od.flags ^= uint8_t(1u << b); test(pre + "flags^bit" + std::to_string(b)); }
							#define UBF_O(field) for (int64_t v : vals32(int64_t(od.field))) { if (int32_t(v) == int32_t(od.field)) continue; od.field = decltype(od.field)(v); test(pre + #field + "=" + std::to_string(v)); }
							UBF_O(x) UBF_O(y) UBF_O(dest) UBF_O(tolerance) UBF_O(pause) UBF_O(retry) UBF_O(attempts) UBF_O(timer) UBF_O(facing) UBF_O(dest_x) UBF_O(dest_y)
							UBF_O(last_x) UBF_O(last_y) UBF_O(coll_x) UBF_O(coll_y) UBF_O(orig_x) UBF_O(orig_y) UBF_O(off_x) UBF_O(off_y) UBF_O(ox) UBF_O(whom) UBF_O(uid)
							UBF_O(spell) UBF_O(paid) UBF_O(oxx) UBF_O(whose) UBF_O(id) UBF_O(form_id) UBF_O(in_group) UBF_O(newform) UBF_O(delay) UBF_O(def_x) UBF_O(def_y)
							UBF_O(mandatory) UBF_O(defensive) UBF_O(in_range) UBF_O(ever_in_range) UBF_O(new_ord) UBF_O(find) UBF_O(wait) UBF_O(started) UBF_O(loaded) UBF_O(g_tx) UBF_O(g_ty) UBF_O(build_type)
							#undef UBF_O
							for (uint32_t cand : {uint32_t(0), saved_angle, saved_dang, saved_orders[oi].angle + 0x80000000u}) { if (cand == saved_orders[oi].angle) continue; od.angle = cand; test(pre + "angle=" + std::to_string(cand)); }
						}
						// Dropping or duplicating a node, swapping the two nodes.
						for (size_t oi = 0; oi < saved_orders.size(); ++oi) { u->orders.erase(u->orders.begin() + long(oi)); test("erase order" + std::to_string(oi)); }
						if (saved_orders.size() >= 2) { std::swap(u->orders[0], u->orders[1]); test("swap orders 0,1"); }
						std::printf("  UBF: %d perturbations of %d/%d at %d, %d hits (target delta a=%lld b=%lld)\n", tried, uw, uo, uB, hits, (long long)tA, (long long)tB);
						// Pairs and triples whose (da, db) add up to the target (additive when the bytes are disjoint).
						std::map<std::pair<int64_t, int64_t>, std::pair<size_t, size_t>> pairsum;
						int shown = 0;
						for (size_t i = 0; i < perts.size() && shown < 30; ++i)
							for (size_t j = i + 1; j < perts.size() && shown < 30; ++j) {
								if (perts[i].da == 0 && perts[i].db == 0) continue;
								const int64_t sa = (perts[i].da + perts[j].da) % MM, sb = (perts[i].db + perts[j].db) % MM;
								if (sa == tA && sb == tB) { std::printf("  UBF PAIR: %s + %s\n", perts[i].what.c_str(), perts[j].what.c_str()); ++shown; }
								pairsum[{sa, sb}] = {i, j};
							}
						for (size_t k = 0; k < perts.size() && shown < 30; ++k) {
							auto f2 = pairsum.find({((tA - perts[k].da) % MM + MM) % MM, ((tB - perts[k].db) % MM + MM) % MM});
							if (f2 != pairsum.end() && f2->second.first != k && f2->second.second != k) { std::printf("  UBF TRIPLE: %s + %s + %s\n", perts[k].what.c_str(), perts[f2->second.first].what.c_str(), perts[f2->second.second].what.c_str()); ++shown; }
						}
					}
				}
				if (const char* gt = std::getenv("RON_G14")) {  // RON_G14=who,o,B : brute force the clock members of guy 0 of that unit at boundary B against `guys` and `units`
					int gw = 0, go = 0, gB = 0;
					std::sscanf(gt, "%d,%d,%d", &gw, &go, &gB);
					if (f == gB) {
						sim::Unit* u = game->unit(gw, go);
						sim::Guy g0 = u->guys[0];
						std::vector<int> ets;
						for (int an = 0; an < 4; ++an) ets.push_back(int(game->anims->frames(g0.gpiece, an)));
						for (int an = 0; an < 4; ++an)
							for (int ct = 0; ct < 6; ++ct)
								for (int lt : {ct - 1, -1})
									for (int et : ets) {
										u->guys[0].cur_anim = int8_t(an); u->guys[0].cur_time = uint32_t(ct); u->guys[0].last_time = lt; u->guys[0].end_time = uint32_t(et);
										sim::EngineProvider er(*game);
										dw::CheckSumParts gr = dw::compute_check_sums(er);
										for (int pk : {0, 6}) if (gr.ok && gr.v[pk] == it->second[size_t(pk)]) std::printf("  G14 HIT part %s: anim %d cur_time %d last_time %d end_time %d\n", dw::kCheckPartNames[pk], an, ct, lt, et);
									}
						u->guys[0] = g0;
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
				if (const char* nb = std::getenv("RON_NEAR_AT")) {  // RON_NEAR_AT=F,x0,y0,x1,y1: every unit inside that box before frame F (who/o type x y orders)
					int nf = 0, nx0 = 0, ny0 = 0, nx1 = 0, ny1 = 0;
					if (std::sscanf(nb, "%d,%d,%d,%d,%d", &nf, &nx0, &ny0, &nx1, &ny1) == 5 && f == nf)
						for (int nw = 0; nw < 9; ++nw)
							for (int32_t no = 0; no < game->objects[size_t(nw)].unit_mark; ++no) {
								const sim::Unit* nu = game->unit(nw, no);
								if (!nu || !nu->alive() || nu->x < nx0 || nu->x > nx1 || nu->y < ny0 || nu->y > ny1) continue;
								std::printf("  NEAR f%d %d/%d type %d at (%d,%d) in %d group %d orders", nf, nw, no, nu->type, nu->x, nu->y, nu->inside_up, nu->group);
								for (const sim::Order& od : nu->orders) std::printf(" [t%d %d,%d]", int(od.type), od.x, od.y);
								std::printf("\n");
							}
				}
				if (std::getenv("RON_VANS_SCAN") && f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {  // the Array<CaravanLink> headers of every valid city at that boundary (as the engine hashes them)
					sim::EngineProvider evs(*game);
					PartTrace tv;
					if (trace_part(evs, 8, tv)) {
						const dw::Description& dd = dw::Description::get();
						for (const PartTrace::R& r : tv.ranges) {
							const int fn = dd.step_function(r.step);
							if (fn < 0 || std::strcmp(dd.function_name(fn), "Array<CaravanLink>::walk_data") != 0 || r.len == 8) continue;
							const sim::City* vc = (r.who >= 0 && r.who < 8 && r.o >= 0 && size_t(r.o) < game->cities[size_t(r.who)].size()) ? &game->cities[size_t(r.who)][size_t(r.o)] : nullptr;
							std::printf("  VANS %d/%d len %zu:", r.who, r.o, r.len);
							if (vc && r.len == 4 && tv.bytes[r.begin + 0] != 0) { /* marker handled below */ }
							for (size_t k = 0; k < r.len; ++k) std::printf(" %02x", tv.bytes[r.begin + k]);
							if (vc) std::printf("  [flags %04x o %d pop %d ct %d]", vc->city_flags, vc->o, vc->pop, int(vc->vans.size()));
							std::printf("\n");
						}
					}
				}
				if (const char* dd = std::getenv("RON_DEATH_DUMP")) {  // RON_DEATH_DUMP=F0:F1: every valid corpse before each frame of [F0, F1)
					int d0 = 0, d1 = 0;
					if (std::sscanf(dd, "%d:%d", &d0, &d1) == 2 && f >= d0 && f < d1)
						for (size_t di = 0; di < game->deaths.size(); ++di) {
							const sim::DeathObj& d = game->deaths[di];
							if (!d.valid) continue;
							std::printf("  DEATH f%d #%zu first %d anim %d xy %d,%d z %d unit %d/%d gpiece %d ammo_gp %d deg %.4f angle %08x turret %08x,%08x,%08x,%08x cur %d skel %d nf %04x guy %d\n", int(f), di, d.first_frame, d.anim, d.x, d.y, d.z, d.who, d.o, d.gpiece, d.ammo_piece, double(d.deg), unsigned(d.angle),
								unsigned(d.turret_angles[0]), unsigned(d.turret_angles[1]), unsigned(d.turret_angles[2]), unsigned(d.turret_angles[3]), d.cur_frame, d.skeleton, unsigned(uint16_t(d.node_flags)), int(d.guy));
						}
				}
				if (const char* aa = std::getenv("RON_AMMO_DUMP")) {  // RON_AMMO_DUMP=F0:F1: every ammo in use before each frame of [F0, F1)
					int a0 = 0, a1 = 0;
					if (std::sscanf(aa, "%d:%d", &a0, &a1) == 2 && f >= a0 && f < a1)
						for (size_t ai = 0; ai < game->ammo.size(); ++ai) {
							const sim::Ammo& a = game->ammo[ai];
							if (!a.in_use()) continue;
							const sim::Unit* u0 = game->unit(a.who, a.o);
							std::printf("  AMMO f%d #%zu flags %02x piece %d from %d/%d (type %d) -> %d/%d time %u/%u acc %d idx %d gidx %d start (%d,%d,%d) end (%d,%d,%d)\n", int(f), ai, unsigned(a.flags), a.gpiece, a.who, a.o, u0 ? u0->type : -1, a.whom, a.ox, unsigned(a.cur_time), unsigned(a.total_time), int(a.accuracy), a.index, a.graph_index, a.sx, a.sy, a.sz, a.ex, a.ey, a.ez);
						}
				}
				if (const char* cd = std::getenv("RON_CITY_DUMP")) {  // RON_CITY_DUMP=F0:F1: every valid city's members before each frame of [F0, F1), only when they change
					int c0 = 0, c1 = 0;
					static std::map<std::string, std::string> last_city;
					if (std::sscanf(cd, "%d:%d", &c0, &c1) == 2 && f >= c0 && f < c1)
						for (int cw = 0; cw < 8; ++cw)
							for (size_t ci = 0; ci < game->cities[size_t(cw)].size(); ++ci) {
								const sim::City& c = game->cities[size_t(cw)][ci];
								if (!c.valid()) continue;
								char buf[600];
								std::snprintf(buf, sizeof buf, "fl %04x reg %d xy %d,%d stamps a%d r%d rd%d c%d as%d cs%d tw %d,%d,%d,%d,%d,%d,%d,%d sc %d port %d pd %d tv %d cn %d g%d l%d s%d r%d free %d busy %d gath %d pop %d who %d race %d fnd %d pl %d oc %d land %d filled %d bord %d ocf %d dock %d wcf %d space %d,%d,%d ter %d,%d,%d,%d,%d,%d vans %zu",
									c.city_flags, c.reg, c.x, c.y, c.attack_stamp, c.raid_stamp, c.reduce_stamp, c.capture_stamp, c.assimilation_timer, c.capture_strength,
									c.traded_with[0], c.traded_with[1], c.traded_with[2], c.traded_with[3], c.traded_with[4], c.traded_with[5], c.traded_with[6], c.traded_with[7],
									c.scouted, c.in_port, c.peasant_dist, c.trade_val, c.conquest_node, c.granary, c.lumber_mill, c.smelter, c.refinery, c.free, c.busy, c.gatherers, c.pop, c.who, c.race, c.founder, c.plundered, c.ocean, c.land, c.filled, c.bordering, c.ocean_filled, c.dock_tile, c.was_capital_flags, c.space[0], c.space[1], c.space[2], c.ter[0], c.ter[1], c.ter[2], c.ter[3], c.ter[4], c.ter[5], c.vans.size());
								const std::string key = std::to_string(cw) + "/" + std::to_string(ci);
								if (last_city[key] != buf) { std::printf("  CITY f%d %s %s\n", int(f), key.c_str(), buf); last_city[key] = buf; }
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
							for (const sim::Order& od : u->orders) std::printf("   order type %d ox %d whom %d x %d y %d dest %d tol %d dest_xy %d,%d coll %d,%d pause %d attempts %d g %d,%d goto %d wait %d been %d dm %d flags %02x\n", int(od.type), od.ox, od.whom, od.x, od.y, od.dest, od.tolerance, od.dest_x, od.dest_y, od.coll_x, od.coll_y, od.pause, od.attempts, od.g_tx, od.g_ty, int(od.goto_build), od.wait, int(od.been_there), int(od.dist_mod), unsigned(od.flags));
							for (const sim::Guy& gy : u->guys)
								std::printf("   guy %d: anim %d stopped %d cur_time %u end_time %u last_time %d x %d y %d z %d spd %d/%d flags %04x gpiece %d des %d,%d des_angle %08x angle %08x last %d,%d,%d off %d,%d variation %d node %d/%d hold %d whom %d queued %d ox %d\n", gy.guy_num, gy.cur_anim, gy.stopped, gy.cur_time, gy.end_time, gy.last_time, gy.x, gy.y, gy.z, gy.last_speed, gy.avg_speed, gy.guy_flags, gy.gpiece, gy.des_x, gy.des_y, gy.des_angle, gy.angle, gy.last_x, gy.last_y, gy.last_z, gy.off_x, gy.off_y, gy.variation, gy.node_flags, gy.des_node_flags, gy.hold_attack, gy.whom, gy.queued_attack, gy.ox);
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
							const int32_t size0 = tu->path_size;
							for (int32_t psz = 10; psz <= 60; psz += 10)
							for (uint32_t mask = 0; mask < (1u << n); ++mask)
								for (int fv = 0; fv < 3; ++fv) {
									tu->path_size = psz;
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
									if (gt.ok && gt.v[0] == it->second[0]) { std::printf("  PATH HIT: mask %x flags-variant %d alloc %d\n", mask, fv, psz); ++hits; }
								}
							tu->path_size = size0;
							tu->path = orig;
							std::printf("  path variants of %d/%d tried (%zu entries): %d hits\n", tw, to, n, hits);
						}
					}
				}
				if (const char* wx = std::getenv("RON_WD_REVERT")) {  // RON_WD_REVERT=F0:F1: WData of the cells changed between boundaries F0 and F1 put back (all, then per field and per cell) -> does `world` at F1 match?
					int w0 = 0, w1 = 0;
					static std::vector<sim::WData> wd_snap;
					if (std::sscanf(wx, "%d:%d", &w0, &w1) == 2) {
						if (f == w0) wd_snap = game->world.wdata;
						if (f == w1 && wd_snap.size() == game->world.wdata.size()) {
							const std::vector<sim::WData> cur = game->world.wdata;
							auto test = [&](const char* what, const std::function<void(sim::WData&, const sim::WData&)>& rv, size_t only) {
								game->world.wdata = cur;
								size_t nch = 0;
								for (size_t i = 0; i < cur.size(); ++i) {
									if (only != SIZE_MAX && i != only) continue;
									const sim::WData before = game->world.wdata[i];
									rv(game->world.wdata[i], wd_snap[i]);
									if (std::memcmp(&before, &game->world.wdata[i], sizeof(sim::WData)) != 0) ++nch;
								}
								if (!nch) { game->world.wdata = cur; return; }
								sim::EngineProvider ew(*game);
								PartTrace tw;
								const bool ok = trace_part(ew, 11, tw) && adler_of(tw.bytes) == it->second[11];
								if (ok || only == SIZE_MAX) std::printf("  WD_REVERT %s%s: %zu cells changed, world %s\n", what, only == SIZE_MAX ? "" : " (one cell)", nch, ok ? "MATCHES" : "differs");
								if (ok && only != SIZE_MAX) std::printf("    cell %zu (%zu,%zu)\n", only, only % size_t(game->world.xs), only / size_t(game->world.xs));
								game->world.wdata = cur;
							};
							const std::vector<std::pair<const char*, std::function<void(sim::WData&, const sim::WData&)>>> fields = {
								{"all", [](sim::WData& a, const sim::WData& b) { const int32_t bl = a.block; a = b; a.block = bl; }},
								{"bad", [](sim::WData& a, const sim::WData& b) { a.bad = b.bad; }},
								{"blocked", [](sim::WData& a, const sim::WData& b) { a.blocked = b.blocked; }},
								{"solid", [](sim::WData& a, const sim::WData& b) { a.solid = b.solid; }},
								{"flags", [](sim::WData& a, const sim::WData& b) { a.flags = b.flags; }},
								{"bad+blocked+solid", [](sim::WData& a, const sim::WData& b) { a.bad = b.bad; a.blocked = b.blocked; a.solid = b.solid; }},
								{"was_seen", [](sim::WData& a, const sim::WData& b) { a.was_seen = b.was_seen; }},
								{"down", [](sim::WData& a, const sim::WData& b) { a.down = b.down; a.down_who = b.down_who; }}};
							for (const auto& fl : fields) {
								test(fl.first, fl.second, SIZE_MAX);
								for (size_t i = 0; i < cur.size(); ++i) if (std::memcmp(&cur[i], &wd_snap[i], sizeof(sim::WData)) != 0) test(fl.first, fl.second, i);
							}
						}
					}
				}
				if (const char* ca = std::getenv("RON_CURSOR_ADD")) {  // RON_CURSOR_ADD=d: the collision-block sweep cursor starts d wcoords further (experiment)
					static bool cursor_done = false;
					if (!cursor_done) {
						cursor_done = true;
						const int32_t sz = game->world.size;
						if (sz > 0) game->world.empty_colls = int32_t(((int64_t(game->world.empty_colls) + std::atoll(ca)) % sz + sz) % sz);
					}
				}
				if (const char* rb = std::getenv("RON_REVERT_BYTES")) {  // RON_REVERT_BYTES=part:F0:F1: of the stream bytes the engine changed between boundaries F0 and F1, which 1 to 4 put back make the part match at F1?
					int rp = 0, r0 = 0, r1 = 0;
										if (std::sscanf(rb, "%d:%d:%d", &rp, &r0, &r1) == 3) {
						if (f == r1 && !g_rb_snap.bytes.empty()) {
							sim::EngineProvider e1(*game);
							PartTrace t1;
							trace_part(e1, rp, t1);
							const int64_t M = 65521;
							if (t1.bytes.size() != g_rb_snap.bytes.size()) {
								std::printf("  revert-bytes: stream length changed (%zu -> %zu)\n", g_rb_snap.bytes.size(), t1.bytes.size());
								{  // where: the common prefix and suffix of the two streams bound the inserted / removed region; label it by the new stream's ranges
									const std::vector<uint8_t>& s0 = g_rb_snap.bytes;
									const std::vector<uint8_t>& s1 = t1.bytes;
									size_t pre = 0;
									while (pre < s0.size() && pre < s1.size() && s0[pre] == s1[pre]) ++pre;
									size_t suf = 0;
									while (suf < s0.size() - pre && suf < s1.size() - pre && s0[s0.size() - 1 - suf] == s1[s1.size() - 1 - suf]) ++suf;
									const dw::Description& dd = dw::Description::get();
									{  // the first range whose step or length differs between the two walks (an array that grew / shrank)
										size_t k = 0;
										const auto& r0 = g_rb_snap.ranges;
										const auto& r1 = t1.ranges;
										while (k < r0.size() && k < r1.size() && r0[k].step == r1[k].step && r0[k].len == r1[k].len) ++k;
										std::printf("    ranges: %zu old, %zu new; first difference at range %zu\n", r0.size(), r1.size(), k);
										for (size_t q = k > 3 ? k - 3 : 0; q < k + 4; ++q) {
											for (int side = 0; side < 2; ++side) {
												const auto& rs = side ? r1 : r0;
												const auto& bs = side ? s1 : s0;
												if (q >= rs.size()) continue;
												const int fnc = dd.step_function(rs[q].step);
												std::printf("      %s range %zu %d/%d step %d (%s) len %zu:", side ? "new" : "old", q, rs[q].who, rs[q].o, rs[q].step, fnc >= 0 ? dd.function_name(fnc) : "?", rs[q].len);
												for (size_t z = 0; z < rs[q].len && z < 24; ++z) std::printf(" %02x", bs[rs[q].begin + z]);
												std::printf("\n");
											}
										}
									}
									std::printf("    common prefix %zu, common suffix %zu: new stream [%zu, %zu) differs from old [%zu, %zu)\n", pre, suf, pre, s1.size() - suf, pre, s0.size() - suf);
									for (const auto& r : t1.ranges) {
										if (r.begin + r.len <= pre || r.begin >= s1.size() - suf) continue;
										const int fnc = dd.step_function(r.step);
										std::printf("      new range %d/%d step %d (%s) at %zu len %zu:", r.who, r.o, r.step, fnc >= 0 ? dd.function_name(fnc) : "?", r.begin, r.len);
										for (size_t q = 0; q < r.len && q < 24; ++q) std::printf(" %02x", s1[r.begin + q]);
										std::printf("\n");
									}
								}
							} else {
								const int64_t N = int64_t(t1.bytes.size());
								const uint32_t h1 = adler_of(t1.bytes), want = it->second[size_t(rp)];
								int64_t ta = ((int64_t(want & 0xffff) - (h1 & 0xffff)) % M + M) % M, tb = ((int64_t(want >> 16) - (h1 >> 16)) % M + M) % M;
								struct E { int64_t i, a, b; };
								std::vector<E> pool;
								int64_t ta_ov = -1, tb_ov = -1;
								if (const char* tov = std::getenv("RON_RB_TARGET")) { long long x = 0, y = 0; if (std::sscanf(tov, "%lld,%lld", &x, &y) == 2) { ta_ov = ((x % M) + M) % M; tb_ov = ((y % M) + M) % M; } }
								for (int64_t i = 0; i < N; ++i) if (g_rb_snap.bytes[size_t(i)] != t1.bytes[size_t(i)]) {
									const int64_t e = int64_t(g_rb_snap.bytes[size_t(i)]) - int64_t(t1.bytes[size_t(i)]);
									pool.push_back({i, ((e % M) + M) % M, (((e * (N - i)) % M) + M) % M});
								}
								if (ta_ov >= 0) { ta = ta_ov; tb = tb_ov; }
								std::printf("  revert-bytes part %d: %zu bytes changed between %d and %d; target (a=%lld b=%lld)\n", rp, pool.size(), r0, r1, (long long)ta, (long long)tb);
								if (std::getenv("RON_RB_LIST")) {  // every changed byte with its range label, old and new value
									for (const E& pe : pool) {
										const auto& d0 = dw::Description::get();
										for (const auto& r : t1.ranges) if (size_t(pe.i) >= r.begin && size_t(pe.i) < r.begin + r.len) {
											const int fnc = d0.step_function(r.step);
											std::printf("    RB byte %lld (N-i %lld) %d/%d step %d (%s) +%zu: %02x -> %02x\n", (long long)pe.i, (long long)(N - pe.i), r.who, r.o, r.step, fnc >= 0 ? d0.function_name(fnc) : "?", size_t(pe.i) - r.begin, g_rb_snap.bytes[size_t(pe.i)], t1.bytes[size_t(pe.i)]);
											break;
										}
									}
								}
								auto k2 = [](int64_t a, int64_t b) { return uint64_t(a) << 32 | uint64_t(b); };
								std::unordered_map<uint64_t, std::vector<std::pair<size_t, size_t>>> pairs;
								int shown = 0;
								for (size_t i = 0; i < pool.size(); ++i) {
									if (pool[i].a == ta && pool[i].b == tb && shown++ < 20) std::printf("    REVERT HIT (1): byte %lld\n", (long long)pool[i].i);
									for (size_t j = i + 1; j < pool.size(); ++j) pairs[k2((pool[i].a + pool[j].a) % M, (pool[i].b + pool[j].b) % M)].push_back({i, j});
								}
								auto label = [&](int64_t at) {
									static char buf[96];
									buf[0] = 0;
									for (const auto& r : t1.ranges) if (size_t(at) >= r.begin && size_t(at) < r.begin + r.len) { { const int fnl = dw::Description::get().step_function(r.step); std::snprintf(buf, sizeof buf, "%d/%d %s+%zu", r.who, r.o, fnl >= 0 ? dw::Description::get().function_name(fnl) : "?", size_t(at) - r.begin); } break; }
									return std::string(buf);
								};
								auto f2 = pairs.find(k2(ta, tb));
								if (f2 != pairs.end()) for (auto& pq : f2->second) if (shown++ < 40) std::printf("    REVERT HIT (2): bytes %lld [%s] + %lld [%s]\n", (long long)pool[pq.first].i, label(pool[pq.first].i).c_str(), (long long)pool[pq.second].i, label(pool[pq.second].i).c_str());
								for (size_t i = 0; i < pool.size(); ++i) {
									auto f3 = pairs.find(k2(((ta - pool[i].a) % M + M) % M, ((tb - pool[i].b) % M + M) % M));
									if (f3 == pairs.end()) continue;
									for (auto& pq : f3->second) if (pq.first > i && shown++ < 40) std::printf("    REVERT HIT (3): bytes %lld + %lld + %lld\n", (long long)pool[i].i, (long long)pool[pq.first].i, (long long)pool[pq.second].i);
								}
								std::printf("  revert-bytes: %d hits shown\n", shown);
							}
						}
					}
				}
				if (const char* bd = std::getenv("RON_BLOCK_DUMP")) {  // RON_BLOCK_DUMP=F0:F1: the cells holding a CollBlock at those boundaries (bits set, flags), the sweep cursor
					int b0 = 0, b1 = 0;
					static std::map<size_t, std::pair<int, int>> bsnap;
					if (std::sscanf(bd, "%d:%d", &b0, &b1) >= 1 && (f == b0 || f == b1)) {
						std::map<size_t, std::pair<int, int>> cur;
						for (size_t i = 0; i < game->world.wdata.size(); ++i) {
							const int32_t bi = game->world.wdata[i].block;
							if (bi < 0) continue;
							int bits = 0;
							for (uint8_t by : game->world.coll_blocks[size_t(bi)].bytes) bits += __builtin_popcount(by);
							cur[i] = {bits, game->world.coll_blocks[size_t(bi)].flags};
						}
						std::printf("  BLOCKS at %lld: %zu blocks, cursor %d\n", (long long)f, cur.size(), game->world.empty_colls);
						if (f == b0) bsnap = cur;
						else for (const auto& kv : cur) { auto o = bsnap.find(kv.first); if (o == bsnap.end() || o->second != kv.second) std::printf("    block cell %zu (%zu,%zu): bits %d flags %d%s\n", kv.first, kv.first % size_t(game->world.xs), kv.first / size_t(game->world.xs), kv.second.first, kv.second.second, o == bsnap.end() ? "  NEW" : ""); }
						if (f == b1) for (const auto& kv : bsnap) if (!cur.count(kv.first)) std::printf("    block cell %zu (%zu,%zu) FREED (bits were %d flags %d)\n", kv.first, kv.first % size_t(game->world.xs), kv.first / size_t(game->world.xs), kv.second.first, kv.second.second);
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
					for (int k : {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}) {
						if (got.v[k] == it->second[size_t(k)]) continue;
						PartTrace tr;
						if (trace_part(ep, k, tr)) solve_single_byte(tr, it->second[size_t(k)], dw::kCheckPartNames[k]);
						if (k == 11 && std::getenv("RON_FOOT_SEARCH")) footprint_search(*game, tr, it->second[size_t(k)]);
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
								for (size_t q = 0; q < r.len && q < (std::getenv("RON_LAYOUT_LEN") ? size_t(std::atoi(std::getenv("RON_LAYOUT_LEN"))) : size_t(40)); ++q) std::printf(" %02x", tr.bytes[r.begin + q]);
								std::printf("\n");
							}
						}
						if (const char* ls = std::getenv("RON_LIST_PERM")) {  // RON_LIST_PERM=1 (with --solve-at): every wcoord's object list with one element moved to another position, until the part matches
							const size_t cells = game->world.wdata.size();
							auto relink = [&](size_t c, const std::vector<std::pair<int, int>>& ord) {
								sim::WData& wd = game->world.wdata[c];
								wd.down = ord.empty() ? int16_t(-1) : int16_t(ord[0].second);
								wd.down_who = ord.empty() ? int16_t(0) : int16_t(ord[0].first);
								for (size_t i = 0; i < ord.size(); ++i) {
									sim::Object* ob = game->object(ord[i].first, ord[i].second);
									if (!ob) continue;
									if (i == 0) ob->up = -1;
									else { ob->up = int16_t(ord[i - 1].second); ob->up_who = int8_t(ord[i - 1].first); }
									if (i + 1 < ord.size()) { ob->down = int16_t(ord[i + 1].second); ob->down_who = int16_t(ord[i + 1].first); }
									else ob->down = -1;
								}
							};
							(void)ls;
							int hits = 0, tried = 0;
							for (size_t c = 0; c < cells && hits < 20; ++c) {
								std::vector<std::pair<int, int>> ord;
								for (int w0 = game->world.wdata[c].down_who, o0 = game->world.wdata[c].down; o0 >= 0 && ord.size() < 64;) {
									ord.push_back({w0, o0});
									const sim::Object* ob = game->object(w0, o0);
									if (!ob) break;
									w0 = ob->down_who;
									o0 = ob->down;
								}
								if (ord.size() < 2) continue;
								const std::vector<std::pair<int, int>> orig = ord;
								struct Saved { sim::Object* ob; int16_t up, down, down_who; int8_t up_who; };
								std::vector<Saved> sv;
								for (auto& e : orig) if (sim::Object* ob = game->object(e.first, e.second)) sv.push_back({ob, ob->up, ob->down, ob->down_who, ob->up_who});
								const int16_t h0 = game->world.wdata[c].down, hw0 = game->world.wdata[c].down_who;
								for (size_t a = 0; a < orig.size(); ++a)
									for (size_t b = 0; b < orig.size(); ++b) {
										if (a == b) continue;
										std::vector<std::pair<int, int>> t = orig;
										const auto el = t[a];
										t.erase(t.begin() + ptrdiff_t(a));
										t.insert(t.begin() + ptrdiff_t(b), el);
										relink(c, t);
										sim::EngineProvider epl(*game);
										PartTrace tl;
										++tried;
										if (trace_part(epl, k, tl) && adler_of(tl.bytes) == it->second[size_t(k)]) {
											std::printf("      LIST HIT part %s: cell %zu (%zu,%zu): move element %zu (%d/%d) to position %zu of %zu\n", dw::kCheckPartNames[k], c, c % size_t(game->world.xs), c / size_t(game->world.xs), a, el.first, el.second, b, orig.size());
											++hits;
										}
									}
								for (auto& e : sv) { e.ob->up = e.up; e.ob->down = e.down; e.ob->down_who = e.down_who; e.ob->up_who = e.up_who; }
								game->world.wdata[c].down = h0;
								game->world.wdata[c].down_who = hw0;
							}
							std::printf("      list permutations tried: %d, hits %d\n", tried, hits);
						}
						if (k == 11 && std::getenv("RON_BLOCK_SEARCH")) {  // RON_BLOCK_SEARCH=1 (with --solve-at): the world stream with CollBlocks added (empty) / removed (the engine's empty ones): which set matches?
							const std::vector<uint8_t>& by = tr.bytes;
							const sim::World& wd = game->world;
							const size_t cells = wd.wdata.size();
							auto has = [&](size_t c) { return wd.wdata[c].block >= 0; };
							auto rd4 = [&](size_t at) { return by.size() >= at + 4 ? uint32_t(by[at] | by[at + 1] << 8 | by[at + 2] << 16 | uint32_t(by[at + 3]) << 24) : 0xffffffffu; };
							size_t P = SIZE_MAX;
							for (size_t at = 0; at + 4 <= by.size() && P == SIZE_MAX; ++at) {
								size_t q = at;
								bool ok = true;
								for (size_t c = 0; c < std::min<size_t>(cells, 400) && ok; ++c) {
									const uint32_t hb = rd4(q);
									if (hb != (has(c) ? 1u : 0u)) { ok = false; break; }
									q += 4;
									if (hb) q += 8 + 96;
								}
								if (ok) P = at;
							}
							std::printf("      block search: part 9 starts at %zd (stream %zu, %zu cells)\n", P == SIZE_MAX ? ptrdiff_t(-1) : ptrdiff_t(P), by.size(), cells);
							if (P != SIZE_MAX) {
								const int64_t M = 65521;
								// prefix Adler states of the whole stream
								std::vector<uint32_t> pa(by.size() + 1), pb(by.size() + 1);
								{ uint32_t A = 1, B = 0; pa[0] = A; pb[0] = B; for (size_t i = 0; i < by.size(); ++i) { A = (A + by[i]) % 65521; B = (B + A) % 65521; pa[i + 1] = A; pb[i + 1] = B; } }
								struct Seg { int64_t a, b, len; };
								auto seg = [&](size_t x, size_t y) -> Seg {  // standalone Adler pieces of by[x, y)
									const int64_t len = int64_t(y - x);
									const int64_t S = ((int64_t(pa[y]) - pa[x]) % M + M) % M;
									const int64_t T = (((int64_t(pb[y]) - pb[x] - len % M * ((int64_t(pa[x]) - 1 + M) % M)) % M) + M) % M;
									return {(1 + S) % M, T, len};
								};
								auto cat = [&](const Seg& s1, const Seg& s2) -> Seg { return {(s1.a + s2.a - 1 + M) % M, (s1.b + s2.b + (s2.len % M) * ((s1.a - 1 + M) % M)) % M, s1.len + s2.len}; };
								auto raw = [&](const uint8_t* d, size_t n) -> Seg { int64_t A = 1, B = 0; for (size_t i = 0; i < n; ++i) { A = (A + d[i]) % M; B = (B + A) % M; } return {A, B, int64_t(n)}; };
								std::vector<size_t> pos(cells + 1);
								size_t q = P;
								for (size_t c = 0; c < cells; ++c) { pos[c] = q; q += 4; if (rd4(q - 4)) q += 104; }
								pos[cells] = q;
								const uint32_t want = it->second[11];
								const uint8_t zero0[4] = {0, 0, 0, 0}, one0[4] = {1, 0, 0, 0};
								std::vector<uint8_t> emptyblk;
								emptyblk.insert(emptyblk.end(), one0, one0 + 4);
								{ const uint8_t hdr[8] = {0, 3, 0, 0, 0x60, 0, 0, 0}; emptyblk.insert(emptyblk.end(), hdr, hdr + 8); emptyblk.insert(emptyblk.end(), 96, 0); }
								const Seg segEmptyAdd = raw(emptyblk.data(), emptyblk.size()), segNone = raw(zero0, 4);
								// removable: engine blocks whose 96 bytes are zero
								std::vector<size_t> rem, add;
								for (size_t c = 0; c < cells; ++c) {
									if (has(c)) { bool z = true; for (size_t i = 0; i < 96; ++i) if (by[pos[c] + 12 + i]) { z = false; break; } if (z) rem.push_back(c); }
									else add.push_back(c);
								}
								std::printf("      removable empty blocks: %zu, addable cells: %zu\n", rem.size(), add.size());
								// a modification list sorted by cell: (cell, isAdd)
								auto eval = [&](std::vector<std::pair<size_t, bool>> mods) -> uint32_t {
									std::sort(mods.begin(), mods.end());
									Seg cur = seg(0, pos[0]);
									size_t at = pos[0];
									for (const auto& m : mods) {
										cur = cat(cur, seg(at, pos[m.first]));
										cur = cat(cur, m.second ? segEmptyAdd : segNone);
										at = pos[m.first + 1];
									}
									cur = cat(cur, seg(at, by.size()));
									return uint32_t(cur.b << 16 | cur.a);
								};
								int hits = 0;
								std::printf("      sanity: eval({}) %08x engine %08x\n", eval({}), adler_of(by));
								auto test = [&](const std::vector<std::pair<size_t, bool>>& mods) {
									if (eval(mods) == want && hits++ < 20) {
										std::printf("      BLOCK HIT:");
										for (auto& m : mods) std::printf(" %s (%zu,%zu)", m.second ? "add" : "remove", m.first % size_t(wd.xs), m.first / size_t(wd.xs));
										std::printf("\n");
									}
								};
								const size_t R = rem.size();
								std::vector<std::vector<std::pair<size_t, bool>>> subsets;  // subsets of rem up to size 3
								subsets.push_back({});
								for (size_t i = 0; i < R; ++i) {
									subsets.push_back({{rem[i], false}});
									for (size_t j = i + 1; j < R; ++j) {
										subsets.push_back({{rem[i], false}, {rem[j], false}});
										for (size_t l = j + 1; l < R; ++l) subsets.push_back({{rem[i], false}, {rem[j], false}, {rem[l], false}});
									}
								}
								for (const auto& sb : subsets) {
									if (!sb.empty()) test(sb);
									for (size_t c : add) { auto m = sb; m.push_back({c, true}); test(m); }
									for (size_t ci = 0; ci < add.size() && sb.size() <= 1; ++ci)
										if (sb.size() <= 1) for (size_t cj = ci + 1; cj < add.size() && cj < ci + 40; ++cj) { auto m = sb; m.push_back({add[ci], true}); m.push_back({add[cj], true}); test(m); }
								}
								std::printf("      block search done (%d hits, %zu subsets)\n", hits, subsets.size());
							}
						}
						if (std::getenv("RON_STREAM_MAP")) {  // run-length map of the part's stream: consecutive ranges of one step merged (begin, total bytes, count)
							const dw::Description& dd = dw::Description::get();
							size_t i = 0;
							while (i < tr.ranges.size()) {
								size_t j = i, tot = 0;
								while (j < tr.ranges.size() && tr.ranges[j].step == tr.ranges[i].step && tr.ranges[j].who == tr.ranges[i].who && tr.ranges[j].o == tr.ranges[i].o) tot += tr.ranges[j++].len;
								if (tot >= 16 || j - i > 1 || tr.ranges.size() < 400) {
									const int fnc = dd.step_function(tr.ranges[i].step);
									std::printf("      map %s: begin %zu total %zu count %zu step %d (%s) who %d o %d\n", dw::kCheckPartNames[k], tr.ranges[i].begin, tot, j - i, tr.ranges[i].step, fnc >= 0 ? dd.function_name(fnc) : "?", tr.ranges[i].who, tr.ranges[i].o);
								}
								i = j;
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
				if (const char* tss = std::getenv("RON_TARGET_SEARCH")) {  // RON_TARGET_SEARCH=who/o (+RON_TARGET_W steps of RON_TARGET_STEP, default 12 x 48) at RON_SEARCH_AT: the unit's move target (MoveOrder x / y / dest_x / dest_y / off_x / off_y, Unit.orders_x / y, the final path entry) shifted by (dx, dy)
					if (f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {
						const int W = std::getenv("RON_TARGET_W") ? std::atoi(std::getenv("RON_TARGET_W")) : 12;
						const int STEP = std::getenv("RON_TARGET_STEP") ? std::atoi(std::getenv("RON_TARGET_STEP")) : 48;
						int tw = 0, to = 0;
						std::sscanf(tss, "%d/%d", &tw, &to);
						const std::vector<sim::MemberChange> all = sim::members_of(*game, {"MoveOrder.*", "Unit.*", "PathData.*"});
						struct M { const sim::MemberChange* c; int axis; int kind; int64_t v; };  // kind 0: plain int, 1: off_ (16 bit, value mod 0x300)
						std::vector<M> ms;
						for (const auto& c : all) {
							if (c.id.a != tw || c.id.b != to) continue;
							const std::string& k = c.key;
							int axis = -1, kind = 0;
							if (k == "MoveOrder.x" || k == "MoveOrder.dest_x" || k == "Unit.orders_x") axis = 0;
							else if (k == "MoveOrder.y" || k == "MoveOrder.dest_y" || k == "Unit.orders_y") axis = 1;
							else if (k == "MoveOrder.off_x") axis = 0, kind = 1;
							else if (k == "MoveOrder.off_y") axis = 1, kind = 1;
							else if (k == "PathData.to_x" && c.id.c == 0) axis = 0;
							else if (k == "PathData.to_y" && c.id.c == 0) axis = 1;
							if (axis < 0 || c.now.size() != (kind ? 2u : 4u)) continue;
							int64_t v = 0;
							for (size_t i = 0; i < c.now.size(); ++i) v |= int64_t(c.now[i]) << (8 * i);
							ms.push_back({&c, axis, kind, v});
						}
						std::printf("  target search %d/%d: %zu members\n", tw, to, ms.size());
						auto apply = [&](int dx, int dy) {
							for (const M& m : ms) {
								int64_t nv = m.v + (m.axis == 0 ? dx : dy);
								if (m.kind == 1) {  // off_x / off_y = x - (x / 0x300) * 0x300 of the shifted target
									int64_t base = 0;
									for (const M& q : ms) if (q.kind == 0 && q.axis == m.axis && q.c->key == (m.axis == 0 ? "MoveOrder.x" : "MoveOrder.y")) base = q.v;
									const int64_t t = base + (m.axis == 0 ? dx : dy);
									nv = t - (t / 0x300) * 0x300;
								}
								std::vector<uint8_t> b(m.c->now.size());
								for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t(nv >> (8 * i));
								sim::apply_member_bytes(*game, *m.c, b);
							}
						};
						for (int ix = -W; ix <= W; ++ix)
							for (int iy = -W; iy <= W; ++iy) {
								apply(ix * STEP, iy * STEP);
								sim::EngineProvider epp(*game);
								PartTrace tu;
								if (trace_part(epp, 0, tu) && adler_of(tu.bytes) == it->second[0]) std::printf("    TARGET HIT units: %d/%d dx %d dy %d\n", tw, to, ix * STEP, iy * STEP);
								apply(0, 0);
							}
					}
				}
				if (const char* pss = std::getenv("RON_POS_SEARCH")) {  // RON_POS_SEARCH=who/o[,..] (+RON_POS_W, default 60) at RON_SEARCH_AT: every (dx, dy) shift of the unit's guys' x / y members (x, des_x, last_x, ..) and Unit.x_internal / y_internal that makes units and guys equal the original's
					if (f == (std::getenv("RON_SEARCH_AT") ? std::atoll(std::getenv("RON_SEARCH_AT")) : start + 1)) {
						const int W = std::getenv("RON_POS_W") ? std::atoi(std::getenv("RON_POS_W")) : 60;
						std::set<std::pair<int, int>> objs;
						{ std::stringstream ss(pss); std::string t; while (std::getline(ss, t, ',')) { int a = 0, b = 0; if (std::sscanf(t.c_str(), "%d/%d", &a, &b) == 2) objs.insert({a, b}); } }
						const std::vector<sim::MemberChange> all = sim::members_of(*game, {"Guy.*", "Unit.*"});
						for (const auto& wo : objs) {
							struct M { const sim::MemberChange* c; int axis; bool enc; int32_t v; };
							std::vector<M> ms;
							for (const auto& c : all) {
								if (c.id.a != wo.first || c.id.b != wo.second || c.now.size() != 4) continue;
								const std::string& k = c.key;
								int axis = -1; bool enc = false;
								if (k == "Guy.x" || k == "Guy.des_x" || k == "Guy.last_x") axis = 0;
								else if (k == "Guy.y" || k == "Guy.des_y" || k == "Guy.last_y") axis = 1;
								else if (k == "Unit.x_internal") axis = 0, enc = true;
								else if (k == "Unit.y_internal") axis = 1, enc = true;
								if (axis < 0) continue;
								uint32_t v = uint32_t(c.now[0]) | uint32_t(c.now[1]) << 8 | uint32_t(c.now[2]) << 16 | uint32_t(c.now[3]) << 24;
								if (enc) v ^= 0x63637u;
								ms.push_back({&c, axis, enc, int32_t(v)});
							}
							std::printf("  pos search %d/%d: %zu members\n", wo.first, wo.second, ms.size());
							auto apply = [&](int dx, int dy) {
								for (const M& m : ms) {
									uint32_t nv = uint32_t(m.v + (m.axis == 0 ? dx : dy));
									if (m.enc) nv ^= 0x63637u;
									std::vector<uint8_t> b{uint8_t(nv), uint8_t(nv >> 8), uint8_t(nv >> 16), uint8_t(nv >> 24)};
									sim::apply_member_bytes(*game, *m.c, b);
								}
							};
							for (int dx = -W; dx <= W; ++dx)
								for (int dy = -W; dy <= W; ++dy) {
									apply(dx, dy);
									sim::EngineProvider epp(*game);
									PartTrace tu, tg;
									const bool ok = trace_part(epp, 0, tu) && trace_part(epp, 6, tg);
									if (ok && adler_of(tu.bytes) == it->second[0]) std::printf("    POS HIT units: %d/%d dx %d dy %d (guys %s)\n", wo.first, wo.second, dx, dy, adler_of(tg.bytes) == it->second[6] ? "equal too" : "differ");
									else if (ok && adler_of(tg.bytes) == it->second[6]) std::printf("    POS HIT guys: %d/%d dx %d dy %d\n", wo.first, wo.second, dx, dy);
									apply(0, 0);
								}
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
						if (std::getenv("RON_PAIR_DEBUG")) std::printf("  pair debug: part %d bytes %lld h0 %08x want %08x\n", part, (long long)N, h0, want);
						std::vector<std::string> pair_pats = {"Unit.*", "MoveOrder.*", "AttackOrder.*", "GatherOrder.*", "TargetOrder.*", "GroupOrder.*", "PathData.*"};
						if (const char* pp = std::getenv("RON_PAIR_PATTERNS")) {  // e.g. "Build.*,Object.*" for part 1 (builds): the members searched instead of the unit ones
							pair_pats.clear();
							std::stringstream ps2(pp);
							std::string t2;
							while (std::getline(ps2, t2, ',')) pair_pats.push_back(t2);
						}
						std::vector<sim::MemberChange> all = sim::members_of(*game, pair_pats);
						struct Eff { size_t mi; int dl; int64_t a, b; uint64_t nv; };
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
							std::vector<std::pair<int, uint64_t>> cand;  // (label delta, new value)
							if (std::getenv("RON_PAIR_VALUES")) {  // instead of deltas: the member's values in the other objects of the class, and small constants
								std::set<uint64_t> seen{v};
								auto add = [&](uint64_t x) { x &= mod - 1; if (seen.insert(x).second) cand.push_back({int(int64_t(x) - int64_t(v)), x}); };
								for (const auto& o2 : all) if (o2.key == c.key && o2.now.size() == n) { uint64_t x = 0; for (size_t k = 0; k < n; ++k) x |= uint64_t(o2.now[k]) << (8 * k); add(x); }
								for (uint64_t x : {uint64_t(0), uint64_t(1), uint64_t(2), uint64_t(3), mod - 1}) add(x);
							} else {
								{
								const int64_t step = std::getenv("RON_PAIR_STEP") ? std::atoll(std::getenv("RON_PAIR_STEP")) : 1;  // e.g. 65536 for the angle members
								for (int dl0 = -W; dl0 <= W; ++dl0) {
									const int64_t dl = dl0 * step;
									if (dl != 0) cand.push_back({int(dl), (v + mod + uint64_t(dl % int64_t(mod) + int64_t(mod))) % mod});
								}
							}
							}
							for (const auto& cd : cand) {
								const int dl = cd.first;
								const uint64_t nv = cd.second;
								int64_t sa = 0, sb = 0;
								for (size_t k = 0; k < n; ++k) { const int e = int((nv >> (8 * k)) & 0xff) - int(c.now[k]); sa += e; sb += int64_t(e) * (N - pos[k]); }
								effs.push_back({i, dl, ((sa % M) + M) % M, ((sb % M) + M) % M, nv});
							}
						}
						std::printf("  pair search: %zu members tried, %zu (member, delta) effects; target (a=%lld b=%lld)\n", tried, effs.size(), (long long)target.first, (long long)target.second);
						auto k2 = [](int64_t a, int64_t b) { return uint64_t(a) << 32 | uint64_t(b); };
						std::unordered_map<uint64_t, std::vector<size_t>> by;
						for (size_t i = 0; i < effs.size(); ++i) by[k2(effs[i].a, effs[i].b)].push_back(i);
						int shown = 0;
						std::vector<int> verify_parts;  // RON_PAIR_VERIFY=p,q: a hit is printed only when it also makes those parts equal (the same bytes sit in several parts)
						if (const char* pv = std::getenv("RON_PAIR_VERIFY")) { std::stringstream vs(pv); std::string vt; while (std::getline(vs, vt, ',')) verify_parts.push_back(std::atoi(vt.c_str())); }
						auto verified = [&](std::initializer_list<size_t> idx) {
							if (verify_parts.empty()) return true;
							for (size_t ei : idx) {
								const auto& cm = all[effs[ei].mi];
								std::vector<uint8_t> nb(cm.now.size());
								for (size_t k = 0; k < nb.size(); ++k) nb[k] = uint8_t(effs[ei].nv >> (8 * k));
								sim::apply_member_bytes(*game, cm, nb);
							}
							sim::EngineProvider epv(*game);
							const dw::CheckSumParts gv = dw::compute_check_sums(epv);
							bool ok = gv.ok;
							for (int vp : verify_parts) ok = ok && gv.v[size_t(vp)] == it->second[size_t(vp)];
							for (size_t ei : idx) sim::apply_member_bytes(*game, all[effs[ei].mi], all[effs[ei].mi].now);
							return ok;
						};
						const bool same_only = std::getenv("RON_PAIR_SAME") != nullptr;  // only pairs of members of the same object (RON_PAIR_SAME=1): one object with two wrong members
						for (size_t i = 0; i < effs.size(); ++i) {
							if (effs[i].a == target.first && effs[i].b == target.second && verified({i}) && shown++ < 20) std::printf("    PAIR HIT (1): %s (%d,%d,%d) %+d\n", all[effs[i].mi].key.c_str(), all[effs[i].mi].id.a, all[effs[i].mi].id.b, all[effs[i].mi].id.c, effs[i].dl);
							auto fnd = by.find(k2(((target.first - effs[i].a) % M + M) % M, ((target.second - effs[i].b) % M + M) % M));
							if (fnd == by.end()) continue;
							for (size_t j : fnd->second) if (j > i && effs[j].mi != effs[i].mi && (!same_only || (all[effs[i].mi].id.a == all[effs[j].mi].id.a && all[effs[i].mi].id.b == all[effs[j].mi].id.b)) && verified({i, j}) && shown++ < (same_only ? 400 : 40))
								std::printf("    PAIR HIT (2): %s (%d,%d,%d) %+d  and  %s (%d,%d,%d) %+d\n", all[effs[i].mi].key.c_str(), all[effs[i].mi].id.a, all[effs[i].mi].id.b, all[effs[i].mi].id.c, effs[i].dl,
									all[effs[j].mi].key.c_str(), all[effs[j].mi].id.a, all[effs[j].mi].id.b, all[effs[j].mi].id.c, effs[j].dl);
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
					if (const char* sk = std::getenv("RON_SKIP_CMD")) {  // experiment: RON_SKIP_CMD=NAME[:frame] drops that command (every one, or those of that record frame)
						char nm[48] = {0};
						int sf = -1;
						std::sscanf(sk, "%47[^:]:%d", nm, &sf);
						if (rc.name == nm && (sf < 0 || int(rec.num("frame")) == sf)) continue;
					}
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
					if (const char* ct = std::getenv("RON_CMD_TRACE")) {  // RON_CMD_TRACE=F0:F1: the commands run in those frames (record frame, seat, name, group, args)
						int c0 = 0, c1 = 0;
						std::sscanf(ct, "%d:%d", &c0, &c1);
						if (f >= c0 && f <= c1) {
							std::printf("  CMD f%lld seat %d who %d %s group %d", (long long)f, play, who, rc.name.c_str(), package.group);
							std::function<void(const Json&, const std::string&)> dump = [&](const Json& j, const std::string& pre) {
								for (const auto& kv : j.o) {
									if (kv.second.t == Json::Num) std::printf(" %s%s=%lld", pre.c_str(), kv.first.c_str(), (long long)kv.second.n);
									else if (kv.second.t == Json::Str) std::printf(" %s%s=%s", pre.c_str(), kv.first.c_str(), kv.second.s.c_str());
									else if (kv.second.t == Json::Obj) dump(kv.second, pre + kv.first + ".");
								}
							};
							dump(c, "");
							std::printf("\n");
						}
					}
					package.process(*game, rc.cmd, play, who);
					++cmds_run;
				}
			}
			if (next_frame >= 0 && f == next_frame) break;
			if (const char* tu_all = std::getenv("RON_TRACE_UNIT")) {  // RON_TRACE_UNIT=who,o,F0,F1[;who,o,F0,F1...]: those units' state before each frame of the window
			  std::stringstream tus(tu_all);
			  std::string tu_item;
			  while (std::getline(tus, tu_item, ';')) {
				const char* tu = tu_item.c_str();
				int tw = 0, to = 0, t0 = 0, t1 = 0;
				if (std::sscanf(tu, "%d,%d,%d,%d", &tw, &to, &t0, &t1) == 4 && f >= t0 && f <= t1)
					if (const sim::Unit* u = game->unit(tw, to)) {
						std::printf("  TU f%d %d/%d x %d y %d z %d ang %08x masks %08x in %d/%d coll %d/%d orders", int(f), tw, to, u->x, u->y, u->z, u->angle, u->unit_masks, u->inside_up, u->inside_up_who, u->collide, u->collide_frame);
						for (const sim::Order& od : u->orders) std::printf(" [t%d %d,%d ox %d]", int(od.type), od.x, od.y, od.ox);
						std::printf(" path %zu (size %d)", u->path.size(), u->path_size);
						if (std::getenv("RON_TRACE_PATH")) for (const sim::PathData& pd : u->path) std::printf(" (%d,%d f%x)", pd.x, pd.y, unsigned(pd.flags));  // bottom first
						if (!u->guys.empty()) { const sim::Guy& gy = u->guys[0]; std::printf(" guy anim %d ct %u spd %d/%d ang %08x flags %x stopped %d", gy.cur_anim, gy.cur_time, gy.last_speed, gy.avg_speed, gy.angle, unsigned(gy.guy_flags), int(gy.stopped)); }
						std::printf("\n");
					}
			  }
			}
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
			if (const char* dp = std::getenv("RON_DUMP_PART")) {  // RON_DUMP_PART=part:F:prefix writes the hashed stream of a part before frame F to prefix.bin and its ranges to prefix.rng
				int dpart = 0, df = 0;
				char dpre[256] = {0};
				if (std::sscanf(dp, "%d:%d:%255s", &dpart, &df, dpre) == 3 && f == df) {
					sim::EngineProvider epd(*game);
					PartTrace trd;
					if (trace_part(epd, dpart, trd)) {
						std::ofstream ob(std::string(dpre) + ".bin", std::ios::binary);
						ob.write(reinterpret_cast<const char*>(trd.bytes.data()), std::streamsize(trd.bytes.size()));
						std::ofstream orr(std::string(dpre) + ".rng");
						for (const auto& r : trd.ranges) orr << r.begin << ' ' << r.len << ' ' << r.who << ' ' << r.o << ' ' << r.step << '\n';
					}
				}
			}
			if (const char* ph = std::getenv("RON_PHASE")) {
				// experiment: RON_PHASE="F:who:o:delta[,..]" adds delta to the animation clock of guy 0 of unit who/o before frame F (a hidden pen animal
				// whose cycle is delta frames early/late: list 9 is in no checksum part, so its draws are the only trace it leaves)
				std::stringstream is(ph);
				std::string tk;
				while (std::getline(is, tk, ',')) {
					int fr = 0, wh = 0, oo = 0, dl = 0;
					if (std::sscanf(tk.c_str(), "%d:%d:%d:%d", &fr, &wh, &oo, &dl) == 4 && fr == f) {
						sim::Unit* u = game->unit(wh, oo);
						if (u && !u->guys.empty()) u->guys[0].cur_time = uint32_t(int32_t(u->guys[0].cur_time) + dl);
					}
				}
			}
			if (const char* nr = std::getenv("RON_DUMP_NEAR")) {  // RON_DUMP_NEAR=F0:F1:x:y:r: every object (all lists) within r ucoords of (x, y), before frames F0..F1
				int f0 = 0, f1 = 0, nx = 0, ny = 0, nrr = 0;
				if (std::sscanf(nr, "%d:%d:%d:%d:%d", &f0, &f1, &nx, &ny, &nrr) == 5 && f >= f0 && f <= f1) {
					for (int w = 0; w < sim::kNumLeaders; ++w) {
						for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) {
							const sim::Unit* u = game->unit(w, o);
							if (u && u->alive() && std::abs(u->x - nx) <= nrr && std::abs(u->y - ny) <= nrr) std::printf("  NEAR f%d unit %d/%d type %d at %d,%d orders %zu\n", int(f), w, o, u->type, u->x, u->y, u->orders.size());
						}
						for (int o = sim::kBuildBase; o < game->objects[size_t(w)].build_mark; ++o) {
							const sim::Build* b = game->build(w, o);
							if (b && b->alive() && std::abs(b->x - nx) <= nrr + 1500 && std::abs(b->y - ny) <= nrr + 1500) std::printf("  NEAR f%d build %d/%d type %d at %d,%d flags %x\n", int(f), w, o, b->type, b->x, b->y, unsigned(b->flags));
						}
					}
				}
			}
			if (const char* ue = std::getenv("RON_UNIT_EVENTS")) {  // RON_UNIT_EVENTS=F0:F1: units / buildings created or removed between two boundaries
				int u0 = 0, u1 = 0;
				static std::map<std::pair<int, int>, std::pair<int, int>> uev;  // (who, o) -> (uid, type)
				if (std::sscanf(ue, "%d:%d", &u0, &u1) == 2 && (f == u0 || f == u1)) {
					std::map<std::pair<int, int>, std::pair<int, int>> cur;
					for (int w = 0; w < 10; ++w) {
						for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) { const sim::Unit* u = game->unit(w, o); if (u && u->alive()) cur[{w, o}] = {u->uid, u->type}; }
						for (int o = sim::kBuildBase; o < game->objects[size_t(w)].build_mark; ++o) { const sim::Build* b = game->build(w, o); if (b && b->alive()) cur[{w, o}] = {b->uid, b->type}; }
					}
					if (f == u0) uev = std::move(cur);
					else {
						for (const auto& kv : cur) { auto it2 = uev.find(kv.first); if (it2 == uev.end() || it2->second != kv.second) std::printf("  UEVENT new/changed %d/%d uid %d type %d%s\n", kv.first.first, kv.first.second, kv.second.first, kv.second.second, it2 == uev.end() ? "" : " (slot reused)"); }
						for (const auto& kv : uev) if (!cur.count(kv.first)) std::printf("  UEVENT removed %d/%d uid %d type %d\n", kv.first.first, kv.first.second, kv.second.first, kv.second.second);
					}
				}
			}
			if (const char* oa = std::getenv("RON_OBJ_AT")) {  // RON_OBJ_AT=F0,F1,..:who/o,who/o: type, position, flags, orders of those objects at those boundaries
				std::string spec(oa);
				const size_t colon = spec.find(':');
				if (colon != std::string::npos) {
					std::stringstream fs(spec.substr(0, colon));
					std::string t;
					bool hit = false;
					while (std::getline(fs, t, ',')) if (!t.empty() && f == std::atoll(t.c_str())) hit = true;
					if (hit) {
						std::stringstream os(spec.substr(colon + 1));
						while (std::getline(os, t, ',')) {
							int w = 0, o = 0;
							if (std::sscanf(t.c_str(), "%d/%d", &w, &o) != 2) continue;
							const sim::Object* ob = game->object(w, o);
							if (!ob) { std::printf("  OBJ @%lld %d/%d: none\n", (long long)f, w, o); continue; }
							std::printf("  OBJ @%lld %d/%d: type %d pos %d,%d flags %02x damage %d myhits %d up %d down %d", (long long)f, w, o, ob->type, ob->x, ob->y, ob->flags, ob->damage, ob->myhits, ob->up, ob->down);
							if (ob->is_unit) {
								const sim::Unit* u = static_cast<const sim::Unit*>(ob);
								std::printf(" masks %08x idle %d stance %d tol %d path %zu orders:", u->unit_masks, u->idle, u->stance, u->tolerance, u->path.size());
								for (const sim::Order& od : u->orders) std::printf(" [t%d fl%02x x%d y%d ox%d whom%d dest%d tol%d]", int(od.type), od.flags, od.x, od.y, od.ox, od.whom, od.dest, od.tolerance);
							} else {
								const sim::Build* b = static_cast<const sim::Build*>(ob);
								std::printf(" masks %04x construct %d gather_max %d city %d", b->build_masks, b->construct_hits, b->gather_max, b->city);
							}
							std::printf("\n");
						}
					}
				}
			}
			if (const char* dl = std::getenv("RON_DUMP_LEADER")) {  // RON_DUMP_LEADER=F0,F1,..:who: the census fields of one leader at those boundaries
				std::string spec(dl);
				const size_t colon = spec.find(':');
				if (colon != std::string::npos) {
					std::stringstream fs(spec.substr(0, colon));
					std::string t;
					bool hit = false;
					while (std::getline(fs, t, ',')) if (!t.empty() && f == std::atoll(t.c_str())) hit = true;
					const int w = std::atoi(spec.c_str() + colon + 1);
					if (hit && w >= 0 && w < 8) {
						const sim::Leader& L = game->leaders[size_t(w)];
						std::printf("  LEADER @%lld who %d: active %d combat %d siege %d non_siege %d attack %d peasants %d scholars %d merchants %d caras %d free %d xport %d gatherers %d fishermen %d\n", (long long)f, w,
							L.active, L.combat, L.siege, L.non_siege, L.attack, L.peasants, L.scholars, L.merchants, L.caras, L.free_peasants, L.xport_peasants, L.gatherers, L.fishermen);
						for (size_t ci = 0; ci < game->cities[size_t(w)].size(); ++ci) {
							const sim::City& C = game->cities[size_t(w)][size_t(ci)];
							if (!(C.city_flags & 1)) continue;
							std::printf("    city %zu o %d race %d founder %d flags %04x assim_timer %d capture_stamp %d\n", ci, int(C.o), int(C.race), int(C.founder), unsigned(C.city_flags), int(C.assimilation_timer), int(C.capture_stamp));
						}
						std::printf("    diplos:");
						for (int k = 0; k < 8; ++k) std::printf(" %d", int(L.diplos(k)));
						std::printf("  treaty:");
						for (int k = 0; k < 8; ++k) std::printf(" %d", int(L.treaty(k)));
						std::printf("  flags %08x ally_mask %02x\n", L.flags, unsigned(L.ally_mask));
						std::printf("    filled_gather_slots");
						for (int k = 0; k < 6; ++k) std::printf(" %d", L.filled_gather_slots[size_t(k)]);
						std::printf(" gather_slots");
						for (int k = 0; k < 6; ++k) std::printf(" %d", L.gather_slots[size_t(k)]);
						std::printf("\n");
						auto arr = [&](const char* nm, const uint16_t* a2, int n) { std::printf("    %s:", nm); for (int r = 0; r < n; ++r) if (a2[r]) std::printf(" [%d]=%d", r, a2[r]); std::printf("\n"); };
						arr("reg_peasants", L.reg_peasants.data(), 64);
						arr("reg_free_peasants", L.reg_free_peasants.data(), 64);
						arr("reg_xport_peasants", L.reg_xport_peasants.data(), 64);
						arr("reg_gatherers", L.reg_gatherers.data(), 64);
						arr("reg_gather_slots", L.reg_gather_slots.data(), 64);
						arr("reg_active", L.reg_active.data(), 127);
						arr("reg_combat", L.reg_combat.data(), 127);
						arr("reg_attack", L.reg_attack.data(), 127);
						arr("reg_land", L.reg_land.data(), 64);
					}
				}
			}
			if (const char* dg = std::getenv("RON_DUMP_GOOD")) {  // RON_DUMP_GOOD=F0,F1,..:idx: the good's state and the herd animals / p1 units around it
				std::string spec(dg);
				const size_t colon = spec.find(':');
				if (colon != std::string::npos) {
					std::stringstream fs(spec.substr(0, colon));
					std::string t;
					bool hit = false;
					while (std::getline(fs, t, ',')) if (!t.empty() && f == std::atoll(t.c_str())) hit = true;
					const size_t gi = size_t(std::atoi(spec.c_str() + colon + 1));
					if (hit && gi < game->goods.size()) {
						const sim::GoodObj& gd = game->goods[gi];
						const int32_t fx = sim::fcoord_of(gd.x), fy = sim::fcoord_of(gd.y);
						const size_t fi = size_t(fy) * size_t(game->world.fog_xs) + size_t(fx);
						std::printf("  GOOD @%lld %zu type %d at %d,%d (fog %d,%d wcoord %d,%d) ever_seen %02x seen %02x seen2 %02x seen3 %02x alive %d\n", (long long)f, gi, gd.type, gd.x, gd.y, fx, fy, sim::wcoord_of(gd.x), sim::wcoord_of(gd.y),
							gd.ever_seen, game->world.seen[fi], game->world.seen2[fi], game->world.seen3[fi], int(gd.alive()));
						for (size_t h = 0; h < game->herds.size(); ++h) if (game->herds[h].good_o == int(gi)) std::printf("    herd %zu cell %d,%d type %d\n", h, game->herds[h].cx, game->herds[h].cy, game->herds[h].t);
						for (int w = 0; w < 10; ++w)
							for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) {
								const sim::Unit* u = game->unit(w, o);
								if (!u || !u->alive()) continue;
								const bool herd = w == 8 && u->cara >= 0 && size_t(u->cara) < game->herds.size() && game->herds[size_t(u->cara)].good_o == int(gi);
								const int32_t dx = u->x - gd.x, dy = u->y - gd.y;
								if (herd || (w == 1 && dx * dx + dy * dy < 4000 * 4000)) std::printf("    unit %d/%d type %d pos %d,%d los %d %s\n", w, o, u->type, u->x, u->y, int(u->mylos), herd ? "herd" : "");
							}
					}
				}
			}
			if (const char* stc = std::getenv("RON_SEEN_TRACE")) {  // RON_SEEN_TRACE=who/o,p,F0:F1: every frame of the range, the object's position, fog cell, seen bits and is_seen for p
				int w = 0, o = 0, pl = 0, f0 = 0, f1 = 0;
				if (std::sscanf(stc, "%d/%d,%d,%d:%d", &w, &o, &pl, &f0, &f1) == 5 && f >= f0 && f <= f1) {
					const sim::Object* ob = game->object(w, o);
					if (ob) {
						const int32_t fx = sim::fcoord_of(ob->x), fy = sim::fcoord_of(ob->y);
						const size_t fi = size_t(fy) * size_t(game->world.fog_xs) + size_t(fx);
						std::printf("  SEEN @%lld %d/%d pos %d,%d fog %d,%d seen %02x seen2 %02x seen3 %02x is_seen(p%d)=%d visible %02x\n", (long long)f, w, o, ob->x, ob->y, fx, fy,
							game->world.seen[fi], game->world.seen2[fi], game->world.seen3[fi], pl, int(sim::object_is_seen(*game, *ob, pl, false)), unsigned(uint8_t(ob->visible)));
					}
				}
			}
			if (const char* am = std::getenv("RON_AMMO_AT")) {  // RON_AMMO_AT=F0,F1,..: the engine's non-empty ammo slots at those boundaries
				std::stringstream fs(am);
				std::string t;
				bool hit = false;
				while (std::getline(fs, t, ',')) if (!t.empty() && f == std::atoll(t.c_str())) hit = true;
				if (hit)
					for (size_t i = 0; i < game->ammo.size(); ++i) {
						const sim::Ammo& a = game->ammo[i];
						if (!a.in_use() && a.cur_time == 0 && a.total_time == 0 && a.who < 0) continue;
						std::printf("  AMMO @%lld [%zu] from %d/%d -> %d/%d piece %d traj %d cur %u total %u acc %d flags %x\n", (long long)f, i, int(a.who), int(a.o), int(a.whom), int(a.ox), int(a.gpiece), int(a.traj), unsigned(a.cur_time), unsigned(a.total_time), int(a.accuracy), unsigned(a.flags));
					}
			}
			if (const char* dd = std::getenv("RON_DUMP_DANGER")) {  // RON_DUMP_DANGER=F0,F1,..: the non-zero World.danger cells (player, reg cell, value) at those boundaries
				std::stringstream ss(dd);
				std::string t;
				while (std::getline(ss, t, ','))
					if (!t.empty() && f == std::atoll(t.c_str())) {
						const sim::World& w = game->world;
						std::printf("  DANGER @%lld reg_size %d reg_xs %d:", (long long)f, int(w.reg_size), int(w.reg_xs));
						for (size_t i = 0; i < w.danger.size(); ++i)
							if (w.danger[i] != 0) std::printf(" [%zu/%zu]=%d", i / size_t(w.reg_size), i % size_t(w.reg_size), int(w.danger[i]));
						std::printf("\n");
					}
			}
			if (const char* fd = std::getenv("RON_FRAME_DIFF")) {  // RON_FRAME_DIFF=F0:F1[:pattern]: every mapped member (of the pattern, default all) the engine changes between the boundaries F0 and F1
				int d0 = 0, d1 = 0;
				char pat[256] = "Unit.*,Guy.*,Build.*,Object.*,SubObject.*,WData.*,CollBlock.*,TCoordData.*,Wall.*,WallData.*,City.*,LeaderData.*";
				static std::map<std::pair<std::tuple<int, int, int>, std::string>, std::vector<uint8_t>> fd_snap;
				if (std::sscanf(fd, "%d:%d:%255s", &d0, &d1, pat) >= 2 && (f == d0 || f == d1)) {
					std::map<std::pair<std::tuple<int, int, int>, std::string>, std::vector<uint8_t>> cur;
					std::vector<std::string> pats;
					for (const char* q = pat; *q;) { const char* e = std::strchr(q, ','); pats.emplace_back(q, e ? size_t(e - q) : std::strlen(q)); q = e ? e + 1 : q + std::strlen(q); }
					for (const sim::MemberChange& c : sim::members_of(*game, pats)) cur[{{c.id.a, c.id.b, c.id.c}, c.key}] = c.now;
					if (f == d0) fd_snap = std::move(cur);
					else {
						int shown = 0;
						for (const auto& kv : cur) {
							auto it0 = fd_snap.find(kv.first);
							if (it0 != fd_snap.end() && it0->second == kv.second) continue;
							if (shown++ >= 400) break;
							std::printf("  FDIFF %s (%d,%d,%d):", kv.first.second.c_str(), std::get<0>(kv.first.first), std::get<1>(kv.first.first), std::get<2>(kv.first.first));
							if (it0 != fd_snap.end()) for (uint8_t by : it0->second) std::printf(" %02x", by);
							std::printf(" ->");
							for (uint8_t by : kv.second) std::printf(" %02x", by);
							std::printf("\n");
						}
					}
				}
			}
			if (const char* rb = std::getenv("RON_RAW_BUILDS")) {  // RON_RAW_BUILDS=F: at that boundary every Build's unmapped bytes in [0x48, 0x66) (the shadow image: +0x58 is WallData::gpiece)
				if ((std::atoll(rb) < 0 ? f == start + 1 : f == std::atoll(rb)) && game->original) {
					for (const auto& kv : game->original->segs) {
						const dw::Rec& rec = *kv.first;
						for (const sim::omap::Segment& seg : kv.second) {
							if (!seg.map || seg.map->name != "Build") continue;
							for (const sim::omap::Entry& e : seg.map->entries) if (e.name == "construct_hits") {
								uint8_t raw[4] = {0, 0, 0, 0};
								rec.read(seg.base + 0x58 - 0x54 + e.off, raw, 4);
								const sim::Build* bd = game->build(seg.id.a, seg.id.b);
								{
									const sim::Leader& lf = game->leaders[size_t(bd ? bd->founder : 0)];
									const sim::Leader& lw = game->leaders[size_t(seg.id.a)];
									const sim::Tribe* tr = size_t(lf.tribe) < game->rules->tribes.size() ? &game->rules->tribes[size_t(lf.tribe)] : nullptr;
									std::printf("  RAWBUILD %d/%d type %d active %d who %d founder %d tribe %d build_continent %d unit_continent %d max_age %d age_owner %d age_founder %d gpiece %u computed %d\n", seg.id.a, seg.id.b, bd ? bd->type : -1, bd ? int(bd->flags & sim::objflag::kActive) : 0, seg.id.a, bd ? int(bd->founder) : -1, int(lf.tribe), tr ? tr->build_continent : -1, tr ? tr->unit_continent : -1, bd ? int(bd->max_age) : -1, int(lw.age), int(lf.age), unsigned(raw[0] | raw[1] << 8 | raw[2] << 16 | uint32_t(raw[3]) << 24), bd ? sim::build_gpiece(*game, bd->type, bd->founder, bd->max_age) : -1);
								}
							}
						}
					}
				}
			}
			if (const char* dm = std::getenv("RON_DUMP_MEMBERS")) {  // RON_DUMP_MEMBERS=F:who/o[:pattern]: every mapped member of one object before frame F
				int mf = 0, mw = 0, mo = 0;
				char pat[64] = "*";
				const int nsc = std::sscanf(dm, "%d:%d/%d:%63s", &mf, &mw, &mo, pat);
				if (nsc >= 3 && f == mf) {
					for (const sim::MemberChange& c : sim::members_of(*game, {std::string(pat)})) {
						if (c.id.a != mw || c.id.b != mo) { if (std::getenv("RON_DUMP_ANY") && c.key == std::string(pat)) std::printf("  ANY %s (%d,%d,%d)\n", c.key.c_str(), c.id.a, c.id.b, c.id.c); continue; }
						std::printf("  MEMBER f%d %s (%d,%d,%d):", int(f), c.key.c_str(), c.id.a, c.id.b, c.id.c);
						for (uint8_t by : c.now) std::printf(" %02x", by);
						std::printf("\n");
					}
				}
			}
			if (const char* td = std::getenv("RON_TILE_DUMP")) {  // RON_TILE_DUMP=F:x0:y0:x1:y1: the engine's tile masks (hex) in a tcoord window before frame F
				int tf = 0, a0 = 0, b0 = 0, a1 = 0, b1 = 0;
				if (std::sscanf(td, "%d:%d:%d:%d:%d", &tf, &a0, &b0, &a1, &b1) == 5 && f == tf)
					for (int y = b0; y <= b1; ++y) {
						std::printf("  TILES f%d y%3d:", int(f), y);
						for (int x = a0; x <= a1; ++x) std::printf(" %04x", unsigned(game->world.t(x, y)));
						std::printf("\n");
					}
			}
			if (const char* gd = std::getenv("RON_DUMP_GAIA")) {  // RON_DUMP_GAIA=F0:F1 (+ RON_DUMP_GAIA_WHO, default 9): the gaia list's units before frames F0..F1
				int f0 = 0, f1 = 0;
				if (std::sscanf(gd, "%d:%d", &f0, &f1) == 2 && f >= f0 && f <= f1) {
					const int gw = std::getenv("RON_DUMP_GAIA_WHO") ? std::atoi(std::getenv("RON_DUMP_GAIA_WHO")) : 9;
					for (int o = 0; o < game->objects[size_t(gw)].unit_mark; ++o) {
						const sim::Unit* u = game->unit(gw, o);
						if (!u || !u->alive()) continue;
						std::printf("  GAIA f%d %d/%d type %d x %d y %d orders %zu path %zu aid %d spell_time %d cara %d speed %d", int(f), gw, o, u->type, u->x, u->y, u->orders.size(), u->path.size(), int(u->animal_aid), int(u->spell_time), int(u->cara), int(u->myspeed));
						if (std::getenv("RON_DUMP_GAIA_ORDERS")) {
							if (const sim::Order* o0 = u->front()) std::printf(" ord{t%d x%d y%d dx%d dy%d tol%d att%d ang%x}", int(o0->type), o0->x, o0->y, o0->dest_x, o0->dest_y, o0->tolerance, o0->attempts, unsigned(o0->angle));
							for (const sim::PathData& pd : u->path) std::printf(" p(%d,%d,t%d,f%x)", pd.x, pd.y, pd.tol, unsigned(pd.flags));
						}
						for (const sim::Guy& gy : u->guys) { std::printf(" | anim %d cur %u end %u avg %d last %d", int(gy.cur_anim), unsigned(gy.cur_time), unsigned(gy.end_time), int(gy.avg_speed), int(gy.last_speed)); if (std::getenv("RON_DUMP_GAIA_TURRET")) std::printf(" turret %d,%d,%d,%d des %d,%d,%d,%d track %d,%d ang %x/%x last_ang %x", gy.turret_angles[0], gy.turret_angles[1], gy.turret_angles[2], gy.turret_angles[3], gy.des_turret_angles[0], gy.des_turret_angles[1], gy.des_turret_angles[2], gy.des_turret_angles[3], gy.track_dx, gy.track_dy, unsigned(gy.angle), unsigned(gy.des_angle), unsigned(gy.last_angle)); }
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
			if (const char* ut = std::getenv("RON_UNIT_TRACE")) {  // RON_UNIT_TRACE=who:o:F0:F1: one line per frame (before its do_frame): the unit's state and order list
				int uw = 0, uo = 0, f0 = 0, f1 = 0;
				if (std::sscanf(ut, "%d:%d:%d:%d", &uw, &uo, &f0, &f1) == 4 && f >= f0 && f <= f1 && uo < 0) {
					// o < 0: every unit of leader `uw` with a pack / unpack CAST order (0x28b..0x292) or the packed bit that is moving or casting
					for (int o2 = 0; o2 < game->objects[size_t(uw)].unit_mark; ++o2) {
						const sim::Unit* tu = game->unit(uw, o2);
						if (!tu || !tu->alive()) continue;
						bool hit = false;
						for (const sim::Order& od : tu->orders) if (od.type == sim::kCastSpell && od.spell >= 0x28b && od.spell <= 0x292) hit = true;
						if (!hit) continue;
						std::printf("  UT f%d %d/%d type %d at %d,%d masks %08x spell_time %d idle %d [rare %d good_obj %d doober %d cara %d tol %d qt %d ox %d oy %d los %d,%d dang %08x ang %08x coll %d cf %d df %d] orders", int(f), uw, o2, tu->type, tu->x, tu->y, unsigned(tu->unit_masks), int(tu->spell_time), int(tu->idle), int(tu->rare), int(tu->good_obj), int(tu->doober), int(tu->cara), int(tu->tolerance), int(tu->queue_time), tu->orders_x, tu->orders_y, tu->los_x, tu->los_y, unsigned(tu->dest_angle), unsigned(tu->angle), int(tu->collide), tu->collide_frame, tu->damage_frame);
						for (const sim::Order& od : tu->orders) std::printf(" [%d fl%02x sp%d pd%d]", int(od.type), unsigned(od.flags), od.spell, od.paid);
						if (!tu->guys.empty()) std::printf(" | guy0 anim %d t %u/%u", int(tu->guys[0].cur_anim), unsigned(tu->guys[0].cur_time), unsigned(tu->guys[0].end_time));
						std::printf("\n");
					}
				} else if (std::sscanf(ut, "%d:%d:%d:%d", &uw, &uo, &f0, &f1) == 4 && f >= f0 && f <= f1) {
					const sim::Unit* tu = game->unit(uw, uo);
					if (!tu || !tu->alive()) std::printf("  UT f%d %d/%d dead\n", int(f), uw, uo);
					else {
						std::printf("  UT f%d %d/%d type %d at %d,%d on_map %d masks %08x/%08x spell_time %d mana_burn %d recharging %d idle %d wait %d orders", int(f), uw, uo, tu->type, tu->x, tu->y, int(tu->on_map()), unsigned(tu->unit_masks), unsigned(tu->unit_masks2), int(tu->spell_time), int(tu->mana_burn), int(tu->recharging), int(tu->idle), int(tu->waiting));
						for (const sim::Order& od : tu->orders) std::printf(" [%d fl%02x x%d y%d sp%d pd%d ox%d wh%d]", int(od.type), unsigned(od.flags), od.x, od.y, od.spell, od.paid, od.ox, od.whom);
						if (!tu->guys.empty()) std::printf(" | guy0 anim %d t %u/%u", int(tu->guys[0].cur_anim), unsigned(tu->guys[0].cur_time), unsigned(tu->guys[0].end_time));
						if (std::getenv("RON_UT_FULL")) {
							std::printf(" | ang %08x dang %08x coll %d/%d/%d cf %d path", unsigned(tu->angle), unsigned(tu->dest_angle), int(tu->collide), int(tu->collide_o), int(tu->collide_guy), int(tu->collide_frame));
							for (const sim::Order& od : tu->orders) std::printf(" ord[dest %d dxy %d,%d coll %d,%d ang %08x]", od.dest, od.dest_x, od.dest_y, od.coll_x, od.coll_y, unsigned(od.angle));
							for (const sim::PathData& pd : tu->path) std::printf(" (%d,%d t%d f%x)", pd.x, pd.y, pd.tol, pd.flags);
							if (!tu->guys.empty()) { const sim::Guy& gy = tu->guys[0]; std::printf(" | guy x %d y %d spd %d/%d des %d,%d dang %08x ang %08x last %d,%d", gy.x, gy.y, int(gy.last_speed), int(gy.avg_speed), gy.des_x, gy.des_y, unsigned(gy.des_angle), unsigned(gy.angle), gy.last_x, gy.last_y); }
						}
						std::printf("\n");
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
	if (std::getenv("RON_SEG_AUDIT") && game->original) {  // images that carry segments of more than one class over the same bytes (a stale segment of an earlier class: the provider bug of pass 9)
		std::map<std::string, int> pairs;
		int multi = 0;
		for (const auto& kv : game->original->segs) {
			const auto& sv = kv.second;
			if (sv.size() < 2) continue;
			++multi;
			for (size_t i = 0; i < sv.size(); ++i)
				for (size_t j = i + 1; j < sv.size(); ++j) {
					if (sv[i].map == sv[j].map) continue;
					const int64_t e1 = sv[i].len < 0 ? INT64_MAX : sv[i].base + sv[i].len, e2 = sv[j].len < 0 ? INT64_MAX : sv[j].base + sv[j].len;
					if (std::max(sv[i].base, sv[j].base) >= std::min(e1, e2)) continue;
					pairs[sv[i].map->name + " + " + sv[j].map->name]++;
				}
		}
		std::printf("  segment audit: %d images with more than one segment; overlapping pairs of different classes:\n", multi);
		for (const auto& pr : pairs) std::printf("    %-40s %d\n", pr.first.c_str(), pr.second);
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
		if (std::getenv("RON_STREAM_DIFF")) {
			// the labelled byte differences of every differing part, engine against the next save (RON_STREAM_DIFF=n: the first n differing bytes of each part)
			const int maxn = std::max(1, std::atoi(std::getenv("RON_STREAM_DIFF")));
			const dw::Description& dd = dw::Description::get();
			for (int k : {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11}) {
				if (a1.v[k] == a2.v[k]) continue;
				PartTrace ta, tb;
				trace_part(e1, k, ta);
				trace_part(e2, k, tb);
				std::printf("  stream diff of %s: engine %zu bytes, original %zu bytes\n", dw::kCheckPartNames[k], ta.bytes.size(), tb.bytes.size());
				int shown2 = 0;
				for (size_t i = 0; i < ta.bytes.size() && i < tb.bytes.size() && shown2 < maxn; ++i) {
					if (ta.bytes[i] == tb.bytes[i]) continue;
					++shown2;
					std::printf("    byte %zu: engine %02x original %02x", i, ta.bytes[i], tb.bytes[i]);
					for (const PartTrace::R& r : tb.ranges)
						if (i >= r.begin && i < r.begin + r.len) {
							std::printf("  in %s (range of %zu bytes, who %d o %d, +%zu)", dd.function_name(dd.step_function(r.step)), r.len, r.who, r.o, i - r.begin);
							break;
						}
					std::printf("\n");
				}
			}
		}
		{
			// The terrain height grid (Terrain::master_land_heights, saved): buildings terraform it (TerrainOut::terraform_for_building), so after the
			// same BUILD commands the engine's grid must be the next save's. Reported: vertices that differ, their bounding box and the largest gap.
			const auto& he = game->heights;
			const auto& ht = truth->heights;
			if (he.h.size() == ht.h.size() && !he.h.empty()) {
				int64_t nd = 0;
				int32_t x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
				float worst = 0.0f;
				for (size_t i = 0; i < he.h.size(); ++i) {
					if (he.h[i] == ht.h[i]) continue;
					++nd;
					const int32_t x = int32_t(i % size_t(he.vxs)), y = int32_t(i / size_t(he.vxs));
					x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
					worst = std::max(worst, std::fabs(he.h[i] - ht.h[i]));
				}
				if (const char* hw = std::getenv("RON_HEIGHTS_WINDOW")) {  // RON_HEIGHTS_WINDOW=x0:y0:x1:y1: print engine -> next save heights of the window
					int a0 = 0, b0 = 0, a1 = 0, b1 = 0;
					if (std::sscanf(hw, "%d:%d:%d:%d", &a0, &b0, &a1, &b1) == 4)
						for (int y = b0; y <= b1; ++y) {
							std::printf("   y%3d:", y);
							for (int x = a0; x <= a1; ++x) {
								const size_t i = size_t(y) * size_t(he.vxs) + size_t(x);
								if (he.h[i] == ht.h[i]) std::printf(" %7.2f      =", double(he.h[i]));
								else std::printf(" %7.2f>%7.2f", double(he.h[i]), double(ht.h[i]));
							}
							std::printf("\n");
						}
				}
				std::printf("  terrain heights: %lld of %zu vertices differ from the next save", (long long)nd, he.h.size());
				if (nd) std::printf(" (x %d..%d, y %d..%d, largest gap %.3f)", x0, x1, y0, y1, double(worst));
				std::printf("\n");
			} else {
				std::printf("  terrain heights: not comparable (%zu / %zu samples)\n", he.h.size(), ht.h.size());
			}
		}
		{  // the road candidates (sim/roadgraph.hpp) against the next save's: cells with a different candidate, by member
			const sim::RoadStore& re = game->world.roads;
			const sim::RoadStore& rt = truth->world.roads;
			int64_t cells = 0, diff_cells = 0, d_num = 0, d_arm = 0, d_line = 0, d_elem = 0, d_rot = 0, d_ref = 0, d_sup = 0, d_term = 0, only_e = 0, only_t = 0;
			int shown = 0;
			const size_t n = std::min(re.slot.size(), rt.slot.size());
			for (size_t c = 0; c < n; ++c) {
				const int32_t se = re.slot[c], st = rt.slot[c];
				if (se >= 0 || st >= 0) ++cells;
				if (se < 0 && st < 0) continue;
				if (se < 0 || st < 0) {
					(se < 0 ? only_t : only_e)++;
					if (shown++ < 12) std::printf("  roads: wcoord (%zu,%zu) %s\n", c % size_t(game->world.xs), c / size_t(game->world.xs), se < 0 ? "only in the next save" : "only in the engine");
					++diff_cells;
					continue;
				}
				const uint32_t* a = &re.words[size_t(se) * sim::RoadStore::kWords];
				const uint32_t* b = &rt.words[size_t(st) * sim::RoadStore::kWords];
				bool dc = false;
				if (a[0] != b[0]) { ++d_num; dc = true; }
				for (int k = 0; k < 16; ++k) {
					const uint32_t* x = a + 1 + 4 * k;
					const uint32_t* y = b + 1 + 4 * k;
					if (x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3]) continue;
					dc = true;
					const bool ux = (x[2] & 0xff) == 10, uy = (y[2] & 0xff) == 10;
					if (ux != uy || (!ux && (x[2] & 0xff) != (y[2] & 0xff) && ((x[2] & 0xff) == 9 || (y[2] & 0xff) == 9))) ++d_rot;
					if ((x[1] & 0xff000000u) != (y[1] & 0xff000000u)) ++d_arm;
					if ((x[1] & 0x00ffffffu) != (y[1] & 0x00ffffffu)) ++d_line;
					if (x[0] != y[0]) ++d_elem;
					if ((x[2] >> 16) != (y[2] >> 16)) ++d_ref;
					if ((x[3] & 0xffff) != (y[3] & 0xffff)) ++d_sup;
					if ((x[3] >> 24) != (y[3] >> 24)) ++d_term;
					const bool essential = (x[1] & 0xff000000u) != (y[1] & 0xff000000u) || (x[2] >> 16) != (y[2] >> 16) || (x[3] & 0xffff) != (y[3] & 0xffff) || (x[3] >> 24) != (y[3] >> 24) || ux != uy;  // arms, references, support, terrain-creation, presence: what the simulation can see
					if (essential && shown++ < (std::getenv("RON_ROADS_ALL") ? 40 : 12)) std::printf("  roads: wcoord (%zu,%zu) tile %d: engine %08x %08x %08x %08x  next %08x %08x %08x %08x\n", c % size_t(game->world.xs), c / size_t(game->world.xs), k, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
				}
				if (dc) ++diff_cells;
			}
			std::printf("  road candidates: %lld wcoords with candidates, %lld differ from the next save (only in the engine %lld, only in the save %lld; num %lld, rotation/presence %lld, arms %lld, line bits %lld, element %lld, refs %lld, support %lld, terrain-creation %lld records)\n",
			            (long long)cells, (long long)diff_cells, (long long)only_e, (long long)only_t, (long long)d_num, (long long)d_rot, (long long)d_arm, (long long)d_line, (long long)d_elem, (long long)d_ref, (long long)d_sup, (long long)d_term);
		}
		{  // The World buffers the mapped audit does not cover: fog (seen / seen2 / seen3 / wcoord_seen), danger, tdata, and the WData members other than the list heads.
			const sim::World& we = game->world;
			const sim::World& wt = truth->world;
			auto cmp8 = [&](const char* name, const std::vector<uint8_t>& e, const std::vector<uint8_t>& t, int xs) {
				if (e.size() != t.size()) { std::printf("  world %s: size %zu vs %zu\n", name, e.size(), t.size()); return; }
				long long nd = 0, sum = 0; int shown = 0;
				for (size_t i = 0; i < e.size(); ++i) if (e[i] != t[i]) {
					++nd; sum += int(t[i]) - int(e[i]);
					if (shown++ < 6) std::printf("    %s[%zu] (%d,%d): engine %02x next %02x\n", name, i, int(i % size_t(xs)), int(i / size_t(xs)), e[i], t[i]);
				}
				std::printf("  world %s: %lld of %zu differ (sum of next - engine = %lld)\n", name, nd, e.size(), sum);
			};
			cmp8("seen", we.seen, wt.seen, we.fog_xs);
			cmp8("seen2", we.seen2, wt.seen2, we.fog_xs);
			cmp8("seen3", we.seen3, wt.seen3, we.fog_xs);
			cmp8("wcoord_seen", we.wcoord_seen, wt.wcoord_seen, we.xs);
			{
				long long nd = 0; int shown = 0;
				if (we.danger.size() == wt.danger.size())
					for (size_t i = 0; i < we.danger.size(); ++i) if (we.danger[i] != wt.danger[i]) { ++nd; if (shown++ < 8) std::printf("    danger[%zu/%zu]: engine %d next %d\n", i / size_t(we.reg_size), i % size_t(we.reg_size), int(we.danger[i]), int(wt.danger[i])); }
				std::printf("  world danger: %lld of %zu differ\n", nd, we.danger.size());
				if (nd > 0 && nd < 40) {  // the objects standing in a differing cell (engine state at the end of the run)
					for (size_t i = 0; i < we.danger.size(); ++i) {
						if (we.danger[i] == wt.danger[i]) continue;
						const int32_t pl = int32_t(i / size_t(we.reg_size)), cell = int32_t(i % size_t(we.reg_size));
						std::printf("    cell p%d %d,%d: objects near:", pl, cell % we.reg_xs, cell / we.reg_xs);
						for (int w = 0; w < 8; ++w) {
							for (int o = 0; o < game->objects[size_t(w)].unit_mark; ++o) {
								const sim::Unit* u = game->unit(w, o);
								if (!u || !u->alive() || !u->on_map()) continue;
								const int32_t cx = sim::regcell_of(u->x), cy = sim::regcell_of(u->y);
								if (std::abs(cx - cell % we.reg_xs) <= 1 && std::abs(cy - cell / we.reg_xs) <= 1 && (u->ptype->role & 0x10000))
									std::printf(" %d/%d(t%d @%d,%d seen_by_p=%d attack %d)", w, o, u->type, cx, cy, int(sim::object_is_seen(*game, *u, pl, false)), int(sim::obj_attack(*game, *u)));
							}
							for (int o = sim::kBuildBase; o < game->objects[size_t(w)].build_mark; ++o) {
								const sim::Build* b = game->build(w, o);
								if (!b || !b->alive()) continue;
								const int32_t cx = sim::regcell_of(b->x), cy = sim::regcell_of(b->y);
								if (std::abs(cx - cell % we.reg_xs) <= 1 && std::abs(cy - cell / we.reg_xs) <= 1)
									std::printf(" B%d/%d(t%d @%d,%d act%d seen_by_p=%d)", w, o, b->type, cx, cy, int((b->flags & sim::objflag::kActive) != 0), int(sim::object_is_seen(*game, *b, pl, false)));
							}
						}
						std::printf("\n");
					}
				}
			}
			{
				long long nd = 0; int shown = 0;
				if (we.tdata.size() == wt.tdata.size())
					for (size_t i = 0; i < we.tdata.size(); ++i) if (we.tdata[i] != wt.tdata[i]) { ++nd; if (shown++ < 6) std::printf("    tdata[%zu] (%d,%d): engine %04x next %04x\n", i, int(i % size_t(we.tile_xs)), int(i / size_t(we.tile_xs)), unsigned(we.tdata[i]), unsigned(wt.tdata[i])); }
				std::printf("  world tdata: %lld of %zu differ\n", nd, we.tdata.size());
			}
			if (const char* re = std::getenv("RON_RELEASE_EVENTS")) {  // RON_RELEASE_EVENTS=piece,piece: the animation events of those graphic pieces
				std::stringstream os(re);
				std::string t;
				while (std::getline(os, t, ',')) {
					const int pc = std::atoi(t.c_str());
					const auto* evs = game->anims ? game->anims->release_events(pc) : nullptr;
					std::printf("    RELEASE piece %d:", pc);
					if (evs) for (const sim::ReleaseEvent& e : *evs) std::printf(" [anim %d frame %d ammo %d node %d]", int(e.anim), int(e.frame), int(e.ammo), int(e.node));
					std::printf("\n");
				}
			}
			if (std::getenv("RON_AMMO_DIFF")) {  // the live ammo of the engine (E) and of the next save (T)
				for (int which = 0; which < 2; ++which) {
					const sim::Game& gg = which == 0 ? *game : *truth;
					for (size_t i = 0; i < gg.ammo.size(); ++i) {
						const sim::Ammo& a = gg.ammo[i];
						if (!a.in_use() && a.cur_time == 0 && a.total_time == 0 && a.who < 0) continue;
						std::printf("    AMMO%c [%zu] from %d/%d -> %d/%d piece %d traj %d cur %u total %u acc %d flags %x start (%d,%d) end (%d,%d)\n", which ? 'T' : 'E', i, int(a.who), int(a.o), int(a.whom), int(a.ox), int(a.gpiece), int(a.traj), unsigned(a.cur_time), unsigned(a.total_time), int(a.accuracy), unsigned(a.flags), a.sx, a.sy, a.ex, a.ey);
					}
				}
			}
			if (const char* to = std::getenv("RON_TRUTH_OBJ")) {  // RON_TRUTH_OBJ=who/o,who/o: those units in the engine (E) and in the next save (T)
				std::stringstream os(to);
				std::string t;
				while (std::getline(os, t, ',')) {
					int w = 0, o = 0;
					if (std::sscanf(t.c_str(), "%d/%d", &w, &o) != 2) continue;
					for (int which = 0; which < 2; ++which) {
						const sim::Object* ob = (which == 0 ? game.get() : truth.get())->object(w, o);
						if (!ob || !ob->is_unit) { std::printf("    OBJ%c %d/%d: none\n", which ? 'T' : 'E', w, o); continue; }
						const sim::Unit* u = static_cast<const sim::Unit*>(ob);
						std::printf("    OBJ%c %d/%d: type %d pos %d,%d angle %08x hits %d dmg %d recharging %d idle %d masks %08x collide %d/%d near %d/%d orders:", which ? 'T' : 'E', w, o, u->type, u->x, u->y, u->angle, u->myhits, u->damage, int(u->recharging), int(u->idle), u->unit_masks, int(u->collide), int(u->collide_o), int(u->near_who), int(u->near_o));
						for (const sim::Order& od : u->orders) std::printf(" [t%d fl%02x x%d y%d ox%d whom%d mand%d inr%d]", int(od.type), od.flags, od.x, od.y, od.ox, od.whom, int(od.mandatory), int(od.in_range));
						std::printf("\n");
						for (size_t gi = 0; gi < u->guys.size() && gi < 4; ++gi) {
							const sim::Guy& gy = u->guys[gi];
							std::printf("        guy %zu: pos %d,%d,%d angle %08x anim %d cur_time %u last_time %d end_time %u stopped %d speed %d/%d flags %04x node_flags %04x/%04x gpiece %d hold %d queued %d\n", gi, gy.x, gy.y, gy.z, gy.angle, int(gy.cur_anim), gy.cur_time, gy.last_time, gy.end_time, int(gy.stopped), gy.last_speed, gy.avg_speed, unsigned(gy.guy_flags), unsigned(uint16_t(gy.node_flags)), unsigned(uint16_t(gy.des_node_flags)), int(gy.gpiece), int(gy.hold_attack), int(gy.queued_attack));
						}
					}
				}
			}
			if (std::getenv("RON_CITY_DIFF")) {  // cities whose race / founder / flags differ between the engine and the next save
				for (int w = 0; w < 8; ++w)
					for (size_t ci = 0; ci < game->cities[size_t(w)].size() && ci < truth->cities[size_t(w)].size(); ++ci) {
						const sim::City& A = game->cities[size_t(w)][ci];
						const sim::City& B = truth->cities[size_t(w)][ci];
						if (A.city_flags != B.city_flags || A.race != B.race || A.founder != B.founder || A.assimilation_timer != B.assimilation_timer)
							std::printf("    CITY %d/%zu engine: race %d founder %d flags %04x timer %d capture %d | next: race %d founder %d flags %04x timer %d capture %d\n", w, ci, int(A.race), int(A.founder), unsigned(A.city_flags), int(A.assimilation_timer), int(A.capture_stamp), int(B.race), int(B.founder), unsigned(B.city_flags), int(B.assimilation_timer), int(B.capture_stamp));
					}
			}
			{
				long long nflags = 0, nland = 0, nreg = 0, nwho = 0, nblk = 0, nbad = 0, nsolid = 0, nwas = 0, nlight = 0, nval = 0; int shown = 0;
				if (we.wdata.size() == wt.wdata.size())
					for (size_t i = 0; i < we.wdata.size(); ++i) {
						const sim::WData& x = we.wdata[i]; const sim::WData& y = wt.wdata[i];
						nflags += x.flags != y.flags; nland += (x.land != y.land) || (x.land_sub != y.land_sub); nreg += (x.region != y.region) || (x.region2 != y.region2);
						nwho += (x.who != y.who) || (x.who2 != y.who2); nblk += x.blocked != y.blocked; nbad += x.bad != y.bad; nsolid += x.solid != y.solid; nwas += x.was_seen != y.was_seen;
						nlight += x.light != y.light; nval += (x.val != y.val) || (x.goods != y.goods);
						if (std::getenv("RON_WHO_ALL") && (x.who != y.who || x.who2 != y.who2)) std::printf("    WHO %d %d engine %d/%d next %d/%d\n", int(i % size_t(we.xs)), int(i / size_t(we.xs)), x.who, x.who2, y.who, y.who2);
						if ((x.flags != y.flags || x.who != y.who || x.who2 != y.who2 || x.was_seen != y.was_seen || x.blocked != y.blocked || x.bad != y.bad || x.solid != y.solid || x.light != y.light) && shown++ < 10)
							std::printf("    wdata[%zu] (%d,%d): flags %04x/%04x who %d/%d who2 %d/%d was_seen %02x/%02x blocked %d/%d bad %d/%d solid %d/%d light %d/%d\n", i, int(i % size_t(we.xs)), int(i / size_t(we.xs)), x.flags, y.flags, x.who, y.who, x.who2, y.who2, x.was_seen, y.was_seen, x.blocked, y.blocked, x.bad, y.bad, x.solid, y.solid, x.light, y.light);
					}
				std::printf("  world wdata differing cells: flags %lld land %lld region %lld who %lld blocked %lld bad %lld solid %lld was_seen %lld light %lld val/goods %lld\n", nflags, nland, nreg, nwho, nblk, nbad, nsolid, nwas, nlight, nval);
			}
		}
		if (const char* tw = std::getenv("RON_TILES_WINDOW")) {  // RON_TILES_WINDOW=x0:y0:x1:y1: tile masks (hex) of the next save (T) and the engine (E) in a tcoord window
			int a0 = 0, b0 = 0, a1 = 0, b1 = 0;
			if (std::sscanf(tw, "%d:%d:%d:%d", &a0, &b0, &a1, &b1) == 4)
				for (int y = b0; y <= b1; ++y) {
					std::printf("   T y%3d:", y);
					for (int x = a0; x <= a1; ++x) std::printf(" %04x", unsigned(truth->world.t(x, y)));
					std::printf("   E:");
					for (int x = a0; x <= a1; ++x) std::printf(" %04x", unsigned(game->world.t(x, y)));
					std::printf("\n");
				}
		}
		if (std::getenv("RON_CMP_BUILDS_ALL")) {   // every building of the leaders 0 and 1: type and position in the engine (E) and the next save (T)
			for (int w2 = 0; w2 < 2; ++w2)
				for (int o2 = sim::kBuildBase; o2 < std::max(game->objects[size_t(w2)].build_mark, truth->objects[size_t(w2)].build_mark); ++o2) {
					const sim::Build* be = game->build(w2, o2);
					const sim::Build* bt = truth->build(w2, o2);
					std::printf("    BLD %d/%d  E: %s   T: %s\n", w2, o2, be ? (std::to_string(be->type) + " at " + std::to_string(be->x) + "," + std::to_string(be->y)).c_str() : "none", bt ? (std::to_string(bt->type) + " at " + std::to_string(bt->x) + "," + std::to_string(bt->y)).c_str() : "none");
				}
		}
		if (const char* cb = std::getenv("RON_CMP_BUILD")) {  // RON_CMP_BUILD=who,o: the building as the next save has it (T) and as the engine has it (E)
			int cw = 0, co = 0;
			std::sscanf(cb, "%d,%d", &cw, &co);
			const sim::Build* bt = truth->build(cw, co);
			const sim::Build* be = game->build(cw, co);
			auto line = [](const char* tag, const sim::Build* b) {
				if (!b) { std::printf("  %s: none\n", tag); return; }
				std::printf("  %s: type %d alive %d x %d y %d z %d uid %u flags %02x masks %04x myhits %d damage %d construct_hits %d constr_time %d job_counter %d job_counter_2 %d frame_started %d helpers %d demolition %d "
				            "orig_type %d stance %d founder %d max_age %d city %d city_down %d gather_down %d queued %d recharging %d mylos %d ever_seen %d/%d up %d down %d down_who %d near %d/%d hold %u\n",
				            tag, b->type, int(b->alive()), b->x, b->y, b->z, unsigned(b->uid), unsigned(b->flags), unsigned(b->build_masks), b->myhits, b->damage, b->construct_hits, b->constr_time, b->job_counter, b->job_counter_2,
				            b->frame_started, b->helpers, b->demolition, b->orig_type, int(b->stance), int(b->founder), int(b->max_age), int(b->city), int(b->city_down), int(b->gather_down), int(b->queued), int(b->recharging), int(b->mylos),
				            int(b->ever_seen), int(b->ever_seen_completed), int(b->up), int(b->down), int(b->down_who), int(b->near_o), int(b->near_who), unsigned(b->hold_frames));
			};
			line("T", bt);
			line("E", be);
		}
		if (const char* rl = std::getenv("RON_WLISTS")) {  // RON_WLISTS=N (with --compare-next): the first N wcoords whose object list (WData.down chain) differs: the engine's list and the next save's
			const int limit = std::atoi(rl);
			auto chain = [](const sim::Game& gg, int wx, int wy) {
				std::string r;
				const sim::WData& d = gg.world.w(wx, wy);
				int o = d.down, ow = d.down_who;
				for (int guard = 0; o >= 0 && guard < 64; ++guard) {
					const sim::Object* ob = gg.object(ow, o);
					if (!ob) { r += " ?"; break; }
					char buf[64];
					std::snprintf(buf, sizeof buf, " %d/%d(t%d%s)", ow, o, ob->type, ob->alive() ? "" : " dead");
					r += buf;
					o = ob->down;
					ow = ob->down_who;
				}
				return r;
			};
			int shown = 0, total = 0;
			for (int wy = 0; wy < game->world.ys; ++wy) for (int wx = 0; wx < game->world.xs; ++wx) {
				const std::string a = chain(*game, wx, wy), b = chain(*truth, wx, wy);
				if (a == b) continue;
				++total;
				if (shown++ < limit) std::printf("  WLIST (%d,%d)\n      engine:%s\n      next  :%s\n", wx, wy, a.c_str(), b.c_str());
			}
			std::printf("  WLIST: %d wcoords with a different object list\n", total);
		}
		if (std::getenv("RON_REGIONS")) {  // RON_REGIONS=1 (with --compare-next): every region whose flags / cursor differ from the next save: next save (T) and engine (E)
			for (int ri = 0; ri < sim::Regions::kNum; ++ri) {
				const sim::Region& a = game->regions.r[size_t(ri)];
				const sim::Region& b = truth->regions.r[size_t(ri)];
				if (a.flags != b.flags || a.borders != b.borders || a.size != b.size)
					std::printf("  REGION %3d size E %d T %d flags E %02x T %02x borders E %d T %d climate %d/%d\n", ri, a.size, b.size, unsigned(a.flags), unsigned(b.flags), a.borders, b.borders, a.climate, b.climate);
			}
		}
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
		if (const char* lw = std::getenv("RON_LIST_WDATA")) {  // RON_LIST_WDATA=field (bad blocked solid land_sub flags land region val goods light was_seen): every wcoord where the engine differs from the next save: engine -> original
			const std::string fname = lw;
			auto val = [&](const sim::WData& d) -> int64_t {
				if (fname == "bad") return d.bad;
				if (fname == "blocked") return d.blocked;
				if (fname == "solid") return d.solid;
				if (fname == "land_sub") return d.land_sub;
				if (fname == "flags") return d.flags;
				if (fname == "land") return d.land;
				if (fname == "region") return d.region;
				if (fname == "region2") return d.region2;
				if (fname == "val") return d.val;
				if (fname == "goods") return d.goods;
				if (fname == "light") return d.light;
				if (fname == "was_seen") return d.was_seen;
				if (fname == "who") return d.who;
				if (fname == "who2") return d.who2;
				return 0;
			};
			int n = 0;
			for (size_t i = 0; i < game->world.wdata.size() && i < truth->world.wdata.size(); ++i) {
				const sim::WData& a = game->world.wdata[i];
				const sim::WData& b = truth->world.wdata[i];
				if (val(a) == val(b)) continue;
				if (n++ < 120) std::printf("    wdata %zu (%zu,%zu) %s: engine %lld original %lld   [land %d/%d flags %04x/%04x blocked %d/%d solid %d/%d bad %d/%d who %d/%d]\n", i, i % size_t(game->world.xs), i / size_t(game->world.xs), fname.c_str(), (long long)val(a), (long long)val(b), a.land, b.land, a.flags, b.flags, a.blocked, b.blocked, a.solid, b.solid, a.bad, b.bad, a.who, b.who);
			}
			std::printf("  %d wcoords differ in %s\n", n, fname.c_str());
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
				if (mname == "cur_anim") return u.guys.empty() ? -1 : u.guys[0].cur_anim;  // guy 0: the animation (gaia animals: idle variants)
				if (mname == "cur_time") return u.guys.empty() ? -1 : int64_t(u.guys[0].cur_time);
				if (mname == "end_time") return u.guys.empty() ? -1 : int64_t(u.guys[0].end_time);
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
				if (mname == "up_who") return b.up_who;
				if (mname == "up") return b.up;
				if (mname == "down") return b.down;
				if (mname == "down_who") return b.down_who;
				if (mname == "uid") return b.uid;
				if (mname == "city_down") return b.city_down;
				if (mname == "gather_down") return b.gather_down;
				if (mname == "ever_seen") return b.ever_seen;
				if (mname == "ever_seen_completed") return b.ever_seen_completed;
				if (mname == "helpers") return b.helpers;
				if (mname == "wonder") return b.wonder;
				if (mname == "fort") return b.fort;
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


// --setup-check <save.SVX>: the setup original_random_setup decodes from this recording's header (map style / size, seed, seats with tribe, team,
// human, difficulty, colour, every lobby setting) against the setup read back from the loaded save of the same game: the two texts must be equal.
int setup_check_run(const std::string& rcx, const std::string& save_path, const std::string& game_dir) {
	sim::Replay rp;
	std::string e;
	if (!sim::read_replay(rcx, rp, e)) { std::printf("setup-check: %s\n", e.c_str()); return 1; }
	sim::RandomGameSetup hs;
	if (!ron::net::original_random_setup(rp, hs, e)) { std::printf("setup-check: %s\n", e.c_str()); return 1; }
	std::vector<uint8_t> raw, bytes;
	bool gz = false;
	if (!read_file(save_path, raw) || !maybe_gunzip(raw, bytes, gz)) { std::printf("setup-check: cannot read %s\n", save_path.c_str()); return 1; }
	dw::Tree peek(dw::Description::get());
	dw::SaveDecode psd = dw::decode_save(peek, bytes.data(), bytes.size(), 0);
	if (!psd.result.ok) { std::printf("setup-check: the save does not decode\n"); return 1; }
	sim::GameSetup gs;
	gs.starting_technology = int32_t(field_int(peek, "game.info.starting_technology"));
	gs.starting_technology2 = int32_t(field_int(peek, "game.info.starting_technology2"));
	gs.ending_technology = int32_t(field_int(peek, "game.info.ending_technology"));
	gs.game_rules = int32_t(field_int(peek, "game.info.game_rules"));
	gs.flag8 = (field_int(peek, "game.info.flags") & 8) != 0;
	RulesCache cache(game_dir);
	sim::OriginalLoadReport rep;
	std::unique_ptr<sim::Game> g = sim::load_original_save(cache.files, cache.get(gs), nullptr, bytes, &rep);
	if (!g) { std::printf("setup-check: load failed: %s\n", rep.error.c_str()); return 1; }
	sim::RandomGameSetup ss = ron::net::setup_from_game(*g);
	ss.map_size = int(field_int(peek, "game.info.map_size"));
	ss.game_speed = int(field_int(peek, "game.info.game_speed"));
	const std::string a = sim::random_setup_string(hs), b = sim::random_setup_string(ss);
	std::vector<std::string> ka, kb;
	for (const std::string* src : {&a, &b}) {
		std::stringstream st(src->substr(7));
		std::string t;
		auto& dst = src == &a ? ka : kb;
		while (std::getline(st, t, ';')) dst.push_back(t);
	}
	int diffs = 0;
	for (size_t i = 0; i < ka.size() && i < kb.size(); ++i)
		if (ka[i] != kb[i] && ka[i].compare(0, 5, "speed") != 0) { std::printf("  differs: header %s, save %s\n", ka[i].c_str(), kb[i].c_str()); ++diffs; }
	if (!diffs) std::printf("  setup-check: the header's setup equals the save's (%zu fields)\n", ka.size());
	else std::printf("  setup-check: %d field(s) differ\n", diffs);
	return diffs ? 2 : 0;
}

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
	std::string json_path, next_replay, extract_save, setup_check;
	bool shadow_test = false, analysis_check = false;
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
		else if (a == "--extract-save" && i + 1 < argc) extract_save = argv[++i];
		else if (a == "--setup-check" && i + 1 < argc) setup_check = argv[++i];
		else if (a == "--shadow-test") shadow_test = true;
		else if (a == "--analysis-check") analysis_check = true;
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
		if (!setup_check.empty()) return setup_check_run(path, setup_check, game_dir);
		if (name.empty()) {
			if (replay) {
				// no embedded save: the generator path (replay_run builds the game from the header's setup)
				std::printf("  no embedded save: the game starts from the header's setup (random map, generated by new_random_game)\n");
				if (csv.empty()) {
					size_t kk = path.find("Recorded Games");
					csv = kk != std::string::npos ? path.substr(0, kk) + "out/checksums.csv" : "re/oracle/out/checksums.csv";
				}
				return replay_run(path, std::vector<uint8_t>(), game_dir, frames, json_path, csv, use_commands, verbose, next_replay, no_art, revert);
			}
			if (engine_rt) std::printf("  no embedded save: nothing to round-trip\n");
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
	if (!extract_save.empty()) {
		// the embedded save of a replay (or the decompressed stream of a save) as a file
		std::ofstream of(extract_save, std::ios::binary);
		of.write(reinterpret_cast<const char*>(buf.data() + save_pos), std::streamsize(sd.end - save_pos));
		std::printf("  wrote %zu bytes of the %s to %s\n", sd.end - save_pos, is_save ? "save" : "embedded save", extract_save.c_str());
		return of ? rc : 1;
	}
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
	if (std::getenv("RON_DUMP_CONTAINERS")) {
		// diagnostic: every container of the decoded save, per global, in walk order: walk function, header, elements (RON_DUMP_CONTAINERS=1)
		const dw::Description& d = dw::Description::get();
		std::vector<uint8_t> save(buf.begin() + std::ptrdiff_t(save_pos), buf.begin() + std::ptrdiff_t(sd.end));
		dw::Tree pk(dw::Description::get());
		dw::decode_save(pk, save.data(), save.size(), 0);
		std::set<const dw::Rec*> seen;
		std::function<void(const dw::Rec*, const std::string&, int)> walk = [&](const dw::Rec* r, const std::string& path, int depth) {
			if (!r || !seen.insert(r).second || depth > 6) return;
			std::map<int, int> nth;
			for (const dw::Event& e : r->ev) {
				if (e.type == 0 && e.child) { if (depth < 2) walk(e.child, path + "/" + d.function_name(d.step_function(e.step)), depth + 1); }
				if (e.type != 2 || !e.cd) continue;
				size_t ne = 0;
				for (const dw::Rec* c : e.cd->elems) ne += c != nullptr;
				const int k = nth[e.step]++;
				std::printf("  %-40s %-34s #%d len %d size %d inc %d flags %02x elems %zu data %zu\n", path.c_str(), d.function_name(d.step_function(e.step)), k, e.cd->length, e.cd->size, int(e.cd->increment), e.cd->flags, ne, e.cd->data.size());
			}
		};
		for (int gid = 0; gid < d.global_count(); ++gid) if (const dw::Rec* r = pk.find_global(gid)) walk(r, d.global_name(gid), 0);
		return 0;
	}
	if (const char* dp = std::getenv("RON_DUMP_STREAM")) {
		// diagnostic: the walked bytes of part RON_DUMP_STREAM (13 scenario_data, 14 script_run_time) as runs of non-zero bytes: offset: bytes
		std::vector<uint8_t> save(buf.begin() + std::ptrdiff_t(save_pos), buf.begin() + std::ptrdiff_t(sd.end));
		dw::Tree pk(dw::Description::get());
		dw::decode_save(pk, save.data(), save.size(), 0);
		sim::GameSetup gs;
		gs.starting_technology = int32_t(field_int(pk, "game.info.starting_technology"));
		gs.starting_technology2 = int32_t(field_int(pk, "game.info.starting_technology2"));
		gs.ending_technology = int32_t(field_int(pk, "game.info.ending_technology"));
		gs.game_rules = int32_t(field_int(pk, "game.info.game_rules"));
		gs.flag8 = (field_int(pk, "game.info.flags") & 8) != 0;
		RulesCache cache(game_dir);
		sim::OriginalLoadReport rep;
		std::unique_ptr<sim::Game> g = sim::load_original_save(cache.files, cache.get(gs), nullptr, save, &rep);
		if (!g) return 1;
		sim::EngineProvider e(*g);
		const int part = std::atoi(dp);
		std::vector<uint8_t> bytes;
		{
			const dw::Description& d = dw::Description::get();
			dw::ChecksumWalker w(d, e);
			w.set_hook([&](int, const uint8_t* b, size_t n) { bytes.insert(bytes.end(), b, b + n); });
			w.reset();
			const char* fn = part == 13 ? "ScenarioData::walk_data" : "RunTimeEnv::walk_data";
			const char* gl = part == 13 ? "scenario_data" : "run_time_env";
			const int gid = d.find_global(gl);
			w.walk(fn, e.global(gid), gid, 0);
		}
		if (std::getenv("RON_LIST_GLOBALS")) {
			const dw::Description& dd = dw::Description::get();
			for (int gid = 0; gid < dd.global_count(); ++gid) {
				const dw::Rec* r = pk.find_global(gid);
				std::printf("  global %3d %-40s class %-20s rec %s bytes %zu events %zu\n", gid, dd.global_name(gid), dd.global_class(gid), r ? "yes" : "no", r ? r->bytes.size() : 0, r ? r->ev.size() : 0);
			}
		}
		if (part == 13 && std::getenv("RON_FIND_SCEN")) {
			const dw::Rec* rec = pk.find_global(dw::Description::get().find_global("scenario_data"));
			std::printf("  scenario_data rec: origin %lld, %zu bytes, %zu events\n", (long long)rec->origin, rec->bytes.size(), rec->ev.size());
			for (size_t i = 0; i + 12 <= rec->bytes.size(); ++i) {
				int32_t a, b, c;
				std::memcpy(&a, &rec->bytes[i], 4); std::memcpy(&b, &rec->bytes[i + 4], 4); std::memcpy(&c, &rec->bytes[i + 8], 4);
				if (b == 200 && c == 12000) std::printf("  msg_time/game_msg_time found at image offset %zu (first int %d)\n", i + 4, a);
			}
		}
		std::printf("  stream of part %d: %zu bytes\n", part, bytes.size());
		if (const char* of = std::getenv("RON_DUMP_STREAM_FILE")) { std::ofstream o(of, std::ios::binary); o.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); }
		for (size_t i = 0; i < bytes.size();) {
			if (!bytes[i]) { ++i; continue; }
			size_t j = i;
			while (j < bytes.size() && j - i < 24 && bytes[j]) ++j;
			std::printf("  %5zu:", i);
			for (size_t k = i; k < j; ++k) std::printf(" %02x", bytes[k]);
			std::printf("\n");
			i = j;
		}
		return 0;
	}
	if (analysis_check) {
		// World::analyze_map: the World members the engine does not model, as new_random_game computes them for this save's setup, against the save's own
		std::vector<uint8_t> save(buf.begin() + std::ptrdiff_t(save_pos), buf.begin() + std::ptrdiff_t(sd.end));
		dw::Tree pk(dw::Description::get());
		dw::decode_save(pk, save.data(), save.size(), 0);
		sim::GameSetup gs;
		gs.starting_technology = int32_t(field_int(pk, "game.info.starting_technology"));
		gs.starting_technology2 = int32_t(field_int(pk, "game.info.starting_technology2"));
		gs.ending_technology = int32_t(field_int(pk, "game.info.ending_technology"));
		gs.game_rules = int32_t(field_int(pk, "game.info.game_rules"));
		gs.flag8 = (field_int(pk, "game.info.flags") & 8) != 0;
		RulesCache cache(game_dir);
		sim::OriginalLoadReport rep;
		std::unique_ptr<sim::Game> g = sim::load_original_save(cache.files, cache.get(gs), nullptr, save, &rep);
		if (!g) { std::printf("analysis-check: load failed\n"); return 1; }
		sim::RandomGameSetup S = ron::net::setup_from_game(*g);
		S.map_size = int(field_int(pk, "game.info.map_size"));
		S.randomize_starts = (g->multiplayer || (field_int(pk, "game.info.flags") & 0) != 0);
		if (std::getenv("RON_RANDSTARTS")) S.randomize_starts = std::atoi(std::getenv("RON_RANDSTARTS")) != 0;
		sim::RandomExtras X;
		std::unique_ptr<sim::Game> ng = sim::new_random_game(cache.files, cache.get(gs), nullptr, S, &X);
		if (!ng) { std::printf("analysis-check: generation failed: %s\n", X.error.c_str()); return 1; }
		const dw::Description& dd = dw::Description::get();
		struct Row { const char* name; int64_t mine; };
		const Row rows[] = {{"map", X.result.picked_style}, {"player_territory_limit", X.result.territory_limits[0]}, {"colonized_territory_limit", X.result.territory_limits[3]},
		                    {"player_reg", X.analysis.player_reg}, {"resource_reg", X.analysis.resource_reg}, {"forest_size", X.analysis.forest_size},
		                    {"mountain_size", X.analysis.mountain_size}, {"rock_size", X.analysis.rock_size}, {"total_metal", X.analysis.total_metal},
		                    {"total_oil", X.analysis.total_oil}, {"goodies", X.analysis.goodies}, {"land_resources", X.analysis.land_resources},
		                    {"sea_resources", X.analysis.sea_resources}, {"seed", X.result.world_seed}, {"land_size", ng->world_total_land}, {"sea_map", ng->world.sea_map}};
		int bad = 0;
		std::string line;
		for (const Row& r : rows) {
			dw::FieldValue v;
			std::string err;
			const std::string path = std::string("world.") + r.name;
			if (!dw::read_field(pk, path, v, err)) { std::printf("  %s: %s\n", r.name, err.c_str()); continue; }
			if (v.i != r.mine) { ++bad; std::printf("  world.%s: save %lld, generated %lld\n", r.name, (long long)v.i, (long long)r.mine); }
		}
		(void)dd;
		if (std::getenv("RON_GOOD_Z")) {
			// SubObject +0xc (z_internal, XOR 0x63637) of the save's goods and items against find_data_z at their position
			const dw::Description& d2 = dw::Description::get();
			for (int which = 0; which < 2; ++which) {
				const dw::Rec* grec = pk.find_global(d2.find_global(which ? "items" : "goods"));
				const std::vector<sim::GoodObj>& list = which ? g->items : g->goods;
				std::map<int, int> hist;
				int n = 0;
				if (grec)
					for (const dw::Event& e : grec->ev) {
						if (e.type != 2 || !e.cd) continue;
						for (size_t i = 0; i < e.cd->elems.size() && i < list.size(); ++i) {
							const dw::Rec* r = e.cd->elems[i];
							if (!r || !list[i].alive()) continue;
							int32_t zi = int32_t(r->read_int(0xc, 4, true));
							const int32_t z = zi ^ 0x63637;
							const int32_t zz = sim::find_data_z(*g, list[i].x, list[i].y, 1);
							++hist[z - zz];
							if (++n <= 4) std::printf("  %s %zu type %d at %d,%d: shadow z %d, find_data_z %d\n", which ? "item" : "good", i, list[i].type, list[i].x, list[i].y, z, zz);
						}
					}
				std::printf("  %s: z - find_data_z histogram:", which ? "items" : "goods");
				int shown2 = 0;
				for (const auto& kv : hist) if (shown2++ < 12) std::printf(" %d:%d", kv.first, kv.second);
				std::printf("\n");
			}
		}
		if (const char* win = std::getenv("RON_WINDOW")) {   // RON_WINDOW=x0,y0,x1,y1: land / region / flags of the save (S) and the generated world (G)
			int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			std::sscanf(win, "%d,%d,%d,%d", &x0, &y0, &x1, &y1);
			for (int which = 0; which < 2; ++which) {
				const sim::World& W = (which ? ng.get() : g.get())->world;
				std::printf("  %s: land/region/flags\n", which ? "generated" : "save");
				for (int y = y0; y <= y1; ++y) {
					std::printf("   y%3d:", y);
					for (int x = x0; x <= x1; ++x) {
						const sim::WData& d = W.w(x, y);
						std::printf(" %d/%3d/%04x", int(d.land), int(d.region), unsigned(d.flags));
					}
					std::printf("\n");
				}
			}
		}
		if (std::getenv("RON_WORLD_ARRAYS")) {   // the SimpleArray<WCoord> blocks of World::walk_data part 2 / 3 in walk order (start_x, start_y, start_city_x, start_city_y, oil_x, oil_y)
			const dw::Description& d3 = dw::Description::get();
			const dw::Rec* wr = pk.find_global(d3.find_global("world"));
			int k = 0;
			if (wr)
				for (const dw::Event& e : wr->ev) {
					if (e.type != 2 || !e.cd) continue;
					if (std::string(d3.function_name(d3.step_function(e.step))) != "SimpleArray<WCoord>::walk_data") continue;
					std::printf("  world array %d: len %d size %d inc %d flags %02x:", k++, e.cd->length, e.cd->size, int(e.cd->increment), e.cd->flags);
					for (size_t q = 0; q + 4 <= e.cd->data.size(); q += 4) std::printf(" %d", int32_t(e.cd->data[q] | e.cd->data[q + 1] << 8 | e.cd->data[q + 2] << 16 | uint32_t(e.cd->data[q + 3]) << 24));
					std::printf("\n");
				}
			std::printf("  generated starts:");
			for (const auto& st : X.result.starts) std::printf(" (%d,%d)", st[0], st[1]);
			std::printf("; city entries:");
			for (const auto& en : X.result.empire) if (en.kind == sim::mapgen::EmpireEntry::City) std::printf(" who %d at (%d,%d) = wcoord (%d,%d)", en.who, en.x, en.y, en.x / 768, en.y / 768);
			std::printf("\n");
		}
		if (const char* hl = std::getenv("RON_HALFLAND")) {   // RON_HALFLAND=n: Terrain.halfland_locs / types / subtypes of the save: the first n entries and the cells' flags
			const dw::Description& d3 = dw::Description::get();
			const dw::Rec* tr = pk.find_global(d3.find_global("terrain"));
			std::vector<std::pair<int, int>> locs;
			std::vector<std::vector<int32_t>> ints;
			if (tr)
				for (const dw::Event& e : tr->ev) {
					if (e.type != 2 || !e.cd) continue;
					const std::string fn = d3.function_name(d3.step_function(e.step));
					if (fn == "Array<WCoordData>::walk_data")
						for (const dw::Rec* r : e.cd->elems) locs.push_back({int(r->read_int(0, 4, true)), int(r->read_int(4, 4, true))});
					else if (fn == "SimpleArray<int>::walk_data") {
						std::vector<int32_t> v;
						for (size_t k = 0; k + 4 <= e.cd->data.size(); k += 4) v.push_back(int32_t(e.cd->data[k] | e.cd->data[k + 1] << 8 | e.cd->data[k + 2] << 16 | uint32_t(e.cd->data[k + 3]) << 24));
						ints.push_back(std::move(v));
					}
				}
			const int n = std::atoi(hl);
			std::printf("  halfland_locs %zu entries; int arrays:", locs.size());
			for (auto& v : ints) std::printf(" %zu", v.size());
			std::printf("\n");
			std::map<std::pair<int, int>, int> th;
			for (size_t i = 0; i < locs.size(); ++i) {
				const sim::WData& cell = g->world.w(locs[i].first, locs[i].second);
				if (int(i) < n) std::printf("   [%4zu] (%3d,%3d) type %d sub %d  cell: land %d flags %04x land_sub %d\n", i, locs[i].first, locs[i].second, ints.size() > 0 && i < ints[0].size() ? ints[0][i] : -1, ints.size() > 1 && i < ints[1].size() ? ints[1][i] : -1, int(cell.land), unsigned(cell.flags), int(cell.land_sub));
				++th[{ints.size() > 0 && i < ints[0].size() ? ints[0][i] : -1, ints.size() > 1 && i < ints[1].size() ? ints[1][i] : -1}];
			}
			for (const auto& kv : th) std::printf("   type %d sub %d: %d\n", kv.first.first, kv.first.second, kv.second);
			std::set<std::pair<int, int>> uniq(locs.begin(), locs.end());
			int hl_cells = 0;
			for (int y = 0; y < g->world.ys; ++y) for (int x = 0; x < g->world.xs; ++x) hl_cells += (g->world.w(x, y).flags & 0x100) != 0;
			std::printf("   distinct cells %zu, cells with flag 0x100: %d\n", uniq.size(), hl_cells);
			const auto& gl = X.result.halfland_locs;
			size_t fd = 0;
			while (fd < gl.size() && fd < locs.size() && gl[fd][0] == locs[fd].first && gl[fd][1] == locs[fd].second && ints.size() > 1 && fd < ints[0].size() && ints[0][fd] == X.result.halfland_types[fd] && ints[1][fd] == X.result.halfland_subtypes[fd]) ++fd;
			std::printf("   generated halfland log: %zu entries; first difference at index %zu%s\n", gl.size(), fd, fd == gl.size() && fd == locs.size() ? " (none: equal)" : "");
			if (fd < gl.size() && fd < locs.size())
				std::printf("     save (%d,%d) type %d sub %d; generated (%d,%d) type %d sub %d\n", locs[fd].first, locs[fd].second, ints.size() > 1 && fd < ints[0].size() ? ints[0][fd] : -1, ints.size() > 1 && fd < ints[1].size() ? ints[1][fd] : -1, gl[fd][0], gl[fd][1], X.result.halfland_types[fd], X.result.halfland_subtypes[fd]);
		}
		if (std::getenv("RON_REGION_DIFF")) {
			// the cells whose region differs: (save region, generated region) pairs with their cell counts, the first cell of each, and the region sizes
			std::map<std::pair<int, int>, std::pair<int, std::string>> pairs;
			for (int y = 0; y < g->world.ys && y < ng->world.ys; ++y)
				for (int x = 0; x < g->world.xs && x < ng->world.xs; ++x) {
					const int a = g->world.w(x, y).region, b = ng->world.w(x, y).region;
					if (a == b) continue;
					auto& e = pairs[{a, b}];
					if (e.first++ == 0) e.second = std::to_string(x) + "," + std::to_string(y);
				}
			for (const auto& kv : pairs) std::printf("  region save %d (size %d) / generated %d (size %d): %d cells, first at %s\n", kv.first.first, g->regions.r[size_t(kv.first.first & 127)].size, kv.first.second, ng->regions.r[size_t(kv.first.second & 127)].size, kv.second.first, kv.second.second.c_str());
		}
		std::printf("  analysis-check: %d of %zu World members differ\n", bad, sizeof rows / sizeof rows[0]);
		return bad ? 2 : 0;
	}
	if (shadow_test) {
		// the freshness test of the checksum shadow: the 15 parts of the loaded save, then of the same engine state over a shadow whose object
		// containers were emptied (clear_shadow_containers; what a generated game starts from): they must agree where the engine owns the state
		std::vector<uint8_t> save(buf.begin() + std::ptrdiff_t(save_pos), buf.begin() + std::ptrdiff_t(sd.end));
		dw::Tree pk(dw::Description::get());
		dw::decode_save(pk, save.data(), save.size(), 0);
		sim::GameSetup gs;
		gs.starting_technology = int32_t(field_int(pk, "game.info.starting_technology"));
		gs.starting_technology2 = int32_t(field_int(pk, "game.info.starting_technology2"));
		gs.ending_technology = int32_t(field_int(pk, "game.info.ending_technology"));
		gs.game_rules = int32_t(field_int(pk, "game.info.game_rules"));
		gs.flag8 = (field_int(pk, "game.info.flags") & 8) != 0;
		RulesCache cache(game_dir);
		sim::OriginalLoadReport rep;
		std::unique_ptr<sim::Game> g = sim::load_original_save(cache.files, cache.get(gs), nullptr, save, &rep);
		if (!g) { std::printf("shadow-test: load failed: %s\n", rep.error.c_str()); return 1; }
		const int tparts[] = {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11};
		std::map<int, PartTrace> t1, t2;
		sim::EngineProvider e1(*g);
		const dw::CheckSumParts p1 = dw::compute_check_sums(e1);
		for (int k : tparts) trace_part(e1, k, t1[k]);
		if (std::getenv("RON_SHADOW_TEMPLATE")) {
			// the shadow of ANOTHER save (a different game) under this engine state, then prepared: what a generated game gets
			std::vector<uint8_t> traw, tbytes;
			bool tgz = false;
			if (!read_file(std::getenv("RON_SHADOW_TEMPLATE"), traw) || !maybe_gunzip(traw, tbytes, tgz)) { std::printf("shadow-test: cannot read the template\n"); return 1; }
			sim::OriginalLoadReport trep;
			std::unique_ptr<sim::Game> gt = sim::load_original_save(cache.files, cache.get(gs), nullptr, tbytes, &trep);
			if (!gt) { std::printf("shadow-test: the template does not load\n"); return 1; }
			g->original = gt->original;
			for (int who = 0; who < sim::kNumLeaders; ++who) g->original->leader_ai[who] = who < 8 && (g->leaders[size_t(who)].flags & 1);
			std::printf("  (the shadow is the template's, not the save's own)\n");
		} else {
			ron::net::clear_shadow_containers(*g->original);
		}
		if (!std::getenv("RON_NO_PREPARE")) ron::net::prepare_fresh_shadow(*g);
		sim::EngineProvider e2(*g);
		const dw::CheckSumParts p2 = dw::compute_check_sums(e2);
		for (int k : tparts) trace_part(e2, k, t2[k]);
		const dw::Description& d = dw::Description::get();
		int bad = 0;
		for (int k = 0; k < dw::kCheckPartCount; ++k) {
			std::printf("  %-16s loaded %10u emptied %10u %s\n", dw::kCheckPartNames[k], p1.v[k], p2.v[k], p1.v[k] == p2.v[k] ? "equal" : "DIFFERENT");
			bad += p1.v[k] != p2.v[k];
			if (p1.v[k] == p2.v[k] || !t1.count(k)) continue;
			const PartTrace &a = t1[k], &b = t2[k];
			size_t i = 0;
			while (i < a.bytes.size() && i < b.bytes.size() && a.bytes[i] == b.bytes[i]) ++i;
			std::printf("      stream %zu vs %zu bytes, first difference at byte %zu", a.bytes.size(), b.bytes.size(), i);
			for (const PartTrace::R& r : b.ranges)
				if (i >= r.begin && i < r.begin + r.len) {
					std::printf(" in a range of %zu bytes (who %d o %d) of %s (+%zu)", r.len, r.who, r.o, d.function_name(d.step_function(r.step)), i - r.begin);
					break;
				}
			std::printf("; loaded %02x emptied %02x\n", i < a.bytes.size() ? a.bytes[i] : 0, i < b.bytes.size() ? b.bytes[i] : 0);
		}
		return bad ? 2 : 0;
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
