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
//   --revert M,..     (with --replay) experiment: put members ("Unit.flags") back to the save's values after the first step
//   --field-report    how many members of each original class the engine maps (the rest stays in the shadow)
//   other options: --quiet --no-diag --mutate N (self-test) --json F --csv F --game DIR --no-art --no-commands --verbose
// Exit code: 1 if the decode fails, 2 if a recomputed value differs from the stored/expected one under --verify / --engine-roundtrip.
#include <csignal>
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
#include <sstream>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "sim/anim_assets.hpp"
#include "sim/commands.hpp"
#include "sim/datawalk.hpp"
#include "sim/datawalk_engine.hpp"
#include "sim/original_save.hpp"
#include "sim/rules.hpp"

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

struct ReplayCommand {
	bool supported = false;
	sim::Command cmd;
	std::string name;
};

ReplayCommand to_command(const Json& c) {
	static const std::map<std::string, int64_t> queue = {{"FIRST", sim::kQueueFirst}, {"LAST", sim::kQueueLast}, {"NEW", sim::kQueueNew}};
	static const std::map<std::string, int64_t> orders = {{"MOVE_TO", 1}, {"ATTACK_TO", 2}, {"EXPLORE_TO", 3}, {"FLEE_TO", 4}, {"BUILD_AT", 6}, {"GATHER", 7},
		{"REPAIR", 13}, {"ATTACK", 10}};
	ReplayCommand r;
	const Json* nm = c.get("name");
	r.name = nm ? nm->s : "?";
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
	// stepping
	sim::CommandPackage package;
	size_t ri = 0;
	std::map<std::string, int64_t> unsupported;
	int64_t first_unsupported_frame = -1;
	std::string first_unsupported;
	int64_t cmds_run = 0;
	std::unique_ptr<sim::Game> baseline;
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
			if (f < last_boundary) game->do_frame();
		}
	} catch (const std::exception& e) {
		step_error = e.what();
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
		// 15 parts of both states at the same frame
		sim::EngineProvider e1(*game), e2(*truth);
		dw::CheckSumParts a1 = dw::compute_check_sums(e1), a2 = dw::compute_check_sums(e2);
		std::printf("  at frame %lld, engine against the next save:", (long long)next_frame);
		for (int k = 0; k < dw::kCheckPartCount; ++k) std::printf(" %s=%s", dw::kCheckPartNames[k], a1.v[k] == a2.v[k] ? "=" : "!");
		std::printf("\n");
		std::vector<sim::AuditLine> d = sim::diff_mapped_state(*truth, *game);
		std::sort(d.begin(), d.end(), [](const sim::AuditLine& x, const sim::AuditLine& y) { return x.mismatches > y.mismatches; });
		int shown = 0;
		for (const sim::AuditLine& l : d) {
			if (!l.mismatches) continue;
			if (shown++ < 60) std::printf("    differs: %-34s %6lld of %-7lld %s\n", l.member.c_str(), (long long)l.mismatches, (long long)l.objects, l.example.c_str());
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
	bool summary = false, verify = false, checksums = false, quiet = false, engine_rt = false, diag = true;
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
		else if (a == "--no-diag") diag = false;
		else if (a == "--replay") replay = true;
		else if (a == "--frames" && i + 1 < argc) frames = std::atoll(argv[++i]);
		else if (a == "--json" && i + 1 < argc) json_path = argv[++i];
		else if (a == "--no-commands") use_commands = false;
		else if (a == "--verbose") verbose = true;
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
