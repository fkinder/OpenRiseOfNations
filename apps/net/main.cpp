// ron-net: multiplayer for the engine (docs/multiplayer-spec.md section 8): the lockstep layer (engine/net), the replay reader
// (engine/sim/replay.*) and their tests.
//
//   ron-net survey <file.rcx|dir> [--json out.jsonl]     the replay reader's numbers per file (compare with rcxdump.mjs: re/tools/netcmp.mjs)
//   ron-net timing <file.rcx|dir> [--delay a,b]          the turn clock alone: the lockstep layer, fed only the stamps, PAUSE and RESIGN of a
//                                                        replay, must reproduce the frame column of every record (no engine)
//   ron-net replay <file.rcx> [--game DIR] [--frames N] [--delay a,b] [--observers K] [--all-verbs] [--no-art] [--verbose] [--no-commands]
//                                                        N engine instances in loopback, each fed only its own player's packages from an MP replay
//                                                        with an embedded save; checks (a) the instances agree at every turn and (b) reports the
//                                                        first turn each of the 15 parts differs from the replay's own CHECK_SUMS (as ron-sav --replay)
//   ron-net selftest [--seed N]                          stall, pause, speed, resign, drop (4 options, random kill points), desync: toy simulation
//   ron-net loopback --random STYLE,SIZE,SEED,NATIONS [--humans N] [--frames F] [--recover] [--diverge FRAME:PEER] [--record BASE] [--save F]
//                                                        N engine instances of one random game in this process; the OOS recovery and the .rcx recording
//   ron-net host <port> --random STYLE,SIZE,SEED,NATIONS[,TRIBES..] [--humans N] [--frames N] [--save F] [--game DIR]
//   ron-net join <port> [--address A] [--frames N] [--save F] [--game DIR]
//                                                        a real LAN game of two (or more) processes; both end with identical native saves
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

#include "commands.hpp"
#include "common.hpp"

using namespace ronnet;

namespace {

// ---------------------------------------------------------------- survey

uint32_t fnv_byte(uint32_t h, uint8_t b) { return (h ^ b) * 16777619u; }
uint32_t fnv_u32(uint32_t h, uint32_t v) {
	for (int i = 0; i < 4; ++i) h = fnv_byte(h, uint8_t(v >> (8 * i)));
	return h;
}

int cmd_survey(int argc, char** argv) {
	if (argc < 3) return 64;
	std::string json;
	bool codec = false;
	for (int i = 3; i < argc; ++i) {
		if (i + 1 < argc && !std::strcmp(argv[i], "--json")) json = argv[i + 1];
		if (!std::strcmp(argv[i], "--codec")) codec = true;
	}
	// --codec: every command the engine has a Command for must come back byte for byte from replay_to_engine + engine_to_wire,
	// and the multiplayer obfuscation must undo itself
	uint64_t codec_cmds = 0, codec_bad = 0, codec_raw = 0, obf_pkgs = 0, obf_bad = 0;
	std::map<std::string, uint64_t> codec_bad_verbs;
	std::vector<std::string> out;
	int ok = 0, fail = 0;
	const std::vector<std::string> files = list_rcx(argv[2]);
	for (const std::string& f : files) {
		sim::Replay rp;
		std::string err;
		if (!sim::read_replay(f, rp, err)) {
			std::printf("FAIL %s: %s\n", basename_of(f).c_str(), err.c_str());
			++fail;
			continue;
		}
		uint32_t fp = 2166136261u;
		std::map<std::string, size_t> verbs;
		size_t cmds = 0, sums = 0;
		for (const sim::ReplayPackage& pk : rp.packages) {
			fp = fnv_u32(fp, uint32_t(pk.frame));
			fp = fnv_u32(fp, uint32_t(pk.play));
			fp = fnv_u32(fp, uint32_t(pk.valid));
			fp = fnv_u32(fp, pk.stamp);
			fp = fnv_u32(fp, uint32_t(int32_t(pk.size)));
			for (uint8_t b : pk.plain) fp = fnv_byte(fp, b);
			for (const sim::ReplayCommand& c : pk.cmds) {
				++cmds;
				++verbs[sim::replay_verb_name(c.op)];
				sums += c.op == sim::op::kCheckSums;
				if (!codec) continue;
				sim::Command ec;
				if (!sim::replay_to_engine(pk.data(c), c.len, ec)) {
					++codec_raw;
					continue;
				}
				++codec_cmds;
				std::vector<uint8_t> back;
				if (!sim::engine_to_wire(ec, back) || back.size() != c.len || std::memcmp(back.data(), pk.data(c), c.len) != 0) {
					if (codec_bad++ < 3) {
						std::printf("  codec: %s differs in %s:", sim::replay_verb_name(c.op), basename_of(f).c_str());
						for (size_t k = 0; k < c.len; ++k) std::printf(" %02x", pk.data(c)[k]);
						std::printf("  ->");
						for (uint8_t b : back) std::printf(" %02x", b);
						std::printf("\n");
					}
					++codec_bad_verbs[sim::replay_verb_name(c.op)];
				}
			}
			if (codec && rp.header.multiplayer && !(pk.plain.size() <= 4 && pk.plain[0] == 0x50)) {
				// wire -> obfuscated (XOR + padding from the seed) -> the reader's decode must give the wire back
				const std::vector<uint8_t> w = pk.wire(false);
				const std::vector<uint8_t> ob = sim::replay_obfuscate(w, rp.header.seed);
				std::vector<uint8_t> plain;
				std::vector<sim::ReplayCommand> cmds2;
				++obf_pkgs;
				bool ok = sim::replay_decode_package(ob.data(), ob.size(), rp.header.seed, true, plain, cmds2) && cmds2.size() == pk.cmds.size();
				std::vector<uint8_t> w2;
				for (const sim::ReplayCommand& c : cmds2) w2.insert(w2.end(), plain.begin() + c.at, plain.begin() + c.at + c.len);
				if (!ok || w2 != w) ++obf_bad;
			}
		}
		const int rgi = rp.header.conquest ? -1 : int(rp.random_info.entries.size());
		char head[1024];
		std::snprintf(head, sizeof head,
		              "{\"file\":\"%s\",\"packages\":%zu,\"commands\":%zu,\"sums\":%zu,\"lastFrame\":%d,\"startFrame\":%d,\"headerFrame\":%d,"
		              "\"saveSize\":%zu,\"rgi\":%d,\"streamAt\":%zu,\"trunc\":%lld,\"fp\":%u,\"verbs\":{",
		              basename_of(f).c_str(), rp.packages.size(), cmds, sums, rp.packages.empty() ? 0 : rp.packages.back().frame,
		              int(rp.start_frame()), int(rp.header.frame), rp.has_save ? rp.save_size : size_t(0), rgi, rp.stream_at,
		              rp.truncated ? (long long)rp.truncated_at : -1LL, fp);
		std::string line = head;
		bool first = true;
		for (const auto& kv : verbs) {
			line += (first ? "\"" : ",\"") + kv.first + "\":" + std::to_string(kv.second);
			first = false;
		}
		line += "}}";
		out.push_back(line);
		std::printf("OK   %s  %s style=%d size=%d players=%d/%zu save=%s pkgs=%zu cmds=%zu sums=%zu frames=%d..%d %s%s\n", basename_of(f).c_str(),
		            rp.header.multiplayer ? "MP" : "SP", rp.header.setup_field(sim::kSetMapStyle), rp.header.setup_field(sim::kSetMapSize),
		            rp.header.present_players(), rp.random_info.entries.size(), rp.has_save ? std::to_string(rp.save_size).c_str() : "no",
		            rp.packages.size(), cmds, sums, int(rp.start_frame()), rp.packages.empty() ? 0 : rp.packages.back().frame, rp.located.c_str(),
		            rp.truncated ? " TRUNCATED" : "");
		++ok;
	}
	std::printf("%d/%zu replays decoded\n", ok, files.size());
	if (codec) {
		std::printf("codec: %llu engine commands re-encoded, %llu differ (", (unsigned long long)codec_cmds, (unsigned long long)codec_bad);
		for (const auto& kv : codec_bad_verbs) std::printf("%s x%llu ", kv.first.c_str(), (unsigned long long)kv.second);
		std::printf("); %llu commands without an engine verb stay raw; obfuscation round trip: %llu multiplayer packages, %llu differ\n", (unsigned long long)codec_raw,
		            (unsigned long long)obf_pkgs, (unsigned long long)obf_bad);
	}
	if (!json.empty()) {
		std::ofstream o(json);
		for (const std::string& l : out) o << l << "\n";
	}
	return fail ? 1 : 0;
}

// ---------------------------------------------------------------- timing

// A simulation that only counts frames: enough to check the turn clock.
class NullSim : public net::SimHooks {
public:
	explicit NullSim(int32_t start) : f(start) {}
	int32_t frame() const override { return f; }
	void step_frame() override { ++f; }
	bool checksums(uint32_t out[16]) override {
		uint32_t sum = 0;
		for (int i = 0; i < 15; ++i) {
			out[i] = uint32_t(f) * 2654435761u + uint32_t(i) * 40503u + 1u;
			sum += out[i];
		}
		out[15] = sum;
		return true;
	}
	void execute(int, const uint8_t*, size_t) override {}
	int32_t f;
};

struct Delay {
	int lo = 0, hi = 0;
};
Delay parse_delay(int argc, char** argv) {
	Delay d;
	for (int i = 2; i + 1 < argc; ++i)
		if (!std::strcmp(argv[i], "--delay")) std::sscanf(argv[i + 1], "%d,%d", &d.lo, &d.hi);
	return d;
}

int timing_one(const std::string& file, const Delay& delay, bool quiet) {
	sim::Replay rp;
	std::string err;
	if (!sim::read_replay(file, rp, err)) {
		std::printf("FAIL %s: %s\n", basename_of(file).c_str(), err.c_str());
		return 1;
	}
	if (!rp.header.multiplayer) {
		std::printf("SKIP %s: single player\n", basename_of(file).c_str());
		return 0;
	}
	Feed feed;
	if (!feed.build(rp, err)) {
		std::printf("FAIL %s: %s\n", basename_of(file).c_str(), err.c_str());
		return 1;
	}
	const uint32_t M = feed.max_stamp(INT64_MAX);
	net::LoopbackHub hub(8, 7, delay.lo, delay.hi);
	std::vector<Inst> inst;
	for (int slot : feed.humans) {
		Inst in;
		in.slot = slot;
		in.hooks = std::make_unique<NullSim>(rp.start_frame());
		in.session = std::make_unique<net::Session>(hub.endpoint(slot), *in.hooks, session_config_for(rp, slot));
		inst.push_back(std::move(in));
	}
	// the hub must only hold the human slots
	for (int p = 0; p < 8; ++p)
		if (std::find(feed.humans.begin(), feed.humans.end(), p) == feed.humans.end()) hub.kill(p);
	uint64_t checked = 0, bad = 0;
	std::string first_bad;
	inst[0].session->on_package = [&](int32_t frame, int slot, uint32_t stamp, const net::Bytes&) {
		const sim::ReplayPackage* pk = feed.find(slot, stamp);
		if (!pk) return;
		++checked;
		if (pk->frame != frame) {
			if (!bad++) first_bad = "slot " + std::to_string(slot) + " stamp " + std::to_string(stamp) + ": record frame " + std::to_string(pk->frame) + ", clock " + std::to_string(frame);
		}
	};
	RunStats st;
	if (!run_instances(inst, hub, feed, M, st, nullptr, err)) {
		std::printf("FAIL %s: %s\n", basename_of(file).c_str(), err.c_str());
		return 1;
	}
	const bool good = bad == 0 && st.failed_instances == 0 && st.first_disagree < 0 && checked > 0;
	if (!quiet || !good)
		std::printf("%s %s: %zu humans, %u turns run, %llu packages checked against the record frames, %llu differ%s%s%s\n", good ? "OK  " : "FAIL",
		            basename_of(file).c_str(), feed.humans.size(), M, (unsigned long long)checked, (unsigned long long)bad,
		            first_bad.empty() ? "" : (" (first: " + first_bad + ")").c_str(), st.failed_instances ? (" " + st.first_failure).c_str() : "",
		            st.first_disagree >= 0 ? (" " + st.disagree_text).c_str() : "");
	return good ? 0 : 1;
}

int cmd_timing(int argc, char** argv) {
	if (argc < 3) return 64;
	const Delay d = parse_delay(argc, argv);
	int bad = 0, n = 0;
	for (const std::string& f : list_rcx(argv[2])) {
		bad += timing_one(f, d, false);
		++n;
	}
	std::printf("timing: %d files, %d failed\n", n, bad);
	return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::signal(SIGPIPE, SIG_IGN);
	if (argc < 2) {
		std::fprintf(stderr,
		             "usage: ron-net survey|timing <file.rcx|dir>\n       ron-net replay <file.rcx> [--game DIR] [--frames N] [--delay a,b]\n"
		             "       ron-net selftest\n       ron-net host <port> --random SPEC | join <port>\n");
		return 64;
	}
	const std::string cmd = argv[1];
	if (cmd == "survey") return cmd_survey(argc, argv);
	if (cmd == "timing") return cmd_timing(argc, argv);
	if (cmd == "replay") return cmd_replay(argc, argv);
	if (cmd == "selftest") return cmd_selftest(argc, argv);
	if (cmd == "host" || cmd == "join") return cmd_netgame(argc, argv);
	if (cmd == "loopback") return cmd_loopback(argc, argv);
	std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
	return 64;
}
