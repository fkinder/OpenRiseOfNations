// ron-sav: decode an original-game save (.SVX) or the save embedded in a replay (.rcx) with the generic DataWalk interpreter
// (engine/sim/datawalk.*), recompute the save's own verify block and the 15 CHECK_SUMS parts, and compare them.
// Usage: ron-sav <file.SVX|.rcx> [--summary] [--field Path] [--checksums [--csv checksums.csv]] [--verify] [--quiet]
//   (no option)  decode to the last byte, verifying every section tag
//   --summary    header, world, leaders and object counts (never prints player names)
//   --field P    one decoded value, e.g. game.frame, game.info.map_style, world.xs, leaders.list[3].score (repeatable)
//   --verify     recompute every SaveGame::verify_save Adler-32 from the decoded stream and compare
//   --checksums  compute the 15 CHECK_SUMS parts; for a replay, compare with its first CHECK_SUMS (read from the CSV written by
//                `node re/tools/rcxdump.mjs --survey ... --checksums re/oracle/out/checksums.csv`, default location, or --csv)
// Exit code: 1 if the decode fails, 2 if a recomputed value differs from the stored/expected one under --verify.
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "sim/datawalk.hpp"

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

}  // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: ron-sav <file.SVX|.rcx> [--summary] [--field Path] [--checksums [--csv file]] [--verify]\n");
		return 64;
	}
	const std::string path = argv[1];
	bool summary = false, verify = false, checksums = false, quiet = false;
	std::vector<std::string> fields;
	std::string csv;  // default: re/oracle/out/checksums.csv next to the "Recorded Games" folder of the replay, else under the cwd
	for (int i = 2; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--summary") summary = true;
		else if (a == "--verify") verify = true;
		else if (a == "--checksums") checksums = true;
		else if (a == "--quiet") quiet = true;
		else if (a == "--field" && i + 1 < argc) fields.push_back(argv[++i]);
		else if (a == "--csv" && i + 1 < argc) csv = argv[++i];
		else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 64; }
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
		if (name.empty()) return 0;
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
