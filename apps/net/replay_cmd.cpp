// ron-net replay: N engine instances in loopback, each driven only by its own player's packages of a multiplayer replay.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

#include "commands.hpp"
#include "common.hpp"
#include "net/playback.hpp"
#include "sim/anim_assets.hpp"
#include "sim/datawalk.hpp"
#include "sim/datawalk_engine.hpp"
#include "sim/newgame.hpp"
#include "sim/original_save.hpp"
#include "sim/rules.hpp"
#include "sim/save.hpp"

namespace ronnet {

namespace dw = ron::sim::datawalk;

namespace {

const char* arg_value(int argc, char** argv, const char* name) {
	for (int i = 2; i + 1 < argc; ++i)
		if (!std::strcmp(argv[i], name)) return argv[i + 1];
	return nullptr;
}
bool has_flag(int argc, char** argv, const char* name) {
	for (int i = 2; i < argc; ++i)
		if (!std::strcmp(argv[i], name)) return true;
	return false;
}

using Parts = std::vector<uint32_t>;

// Every CHECK_SUMS command of the replay: record frame -> {tuple -> how many senders}.
std::map<int64_t, std::map<Parts, int>> all_rows(const sim::Replay& rp) {
	std::map<int64_t, std::map<Parts, int>> rows;
	for (const sim::ReplayPackage& pk : rp.packages)
		for (const sim::ReplayCommand& c : pk.cmds) {
			if (c.op != sim::op::kCheckSums) continue;
			uint32_t v[16];
			if (!sim::replay_check_sums(pk.data(c), c.len, v)) continue;
			++rows[pk.frame][Parts(v, v + dw::kCheckPartCount)];
		}
	return rows;
}

}  // namespace

int cmd_replay(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: ron-net replay <file.rcx> [--game DIR] [--frames N] [--delay a,b] [--observers K] [--all-verbs] [--no-art] [--verbose] [--no-commands]\n");
		return 64;
	}
	const std::string path = argv[2];
	const std::string game_dir = arg_value(argc, argv, "--game") ? arg_value(argc, argv, "--game") : game_dir_default();
	const int64_t max_frames = arg_value(argc, argv, "--frames") ? std::atoll(arg_value(argc, argv, "--frames")) : 0;
	const bool verbose = has_flag(argc, argv, "--verbose"), no_art = has_flag(argc, argv, "--no-art");
	const int n_observers = arg_value(argc, argv, "--observers") ? std::atoi(arg_value(argc, argv, "--observers")) : 0;
	int dlo = 0, dhi = 3;
	if (const char* d = arg_value(argc, argv, "--delay")) std::sscanf(d, "%d,%d", &dlo, &dhi);

	sim::Replay rp;
	std::string err;
	if (!sim::read_replay(path, rp, err)) {
		std::printf("%s: %s\n", basename_of(path).c_str(), err.c_str());
		return 1;
	}
	if (!rp.header.multiplayer) {
		std::printf("%s: single-player replay (no CHECK_SUMS, one player): nothing to run in lockstep\n", basename_of(path).c_str());
		return 0;
	}
	if (rp.native_save || (!rp.has_save && sim::is_random_scenario(sim::to_utf8(rp.header.scenario_path)))) {  // a recording of this port
		return replay_native(rp, path, game_dir, dlo, dhi, arg_value(argc, argv, "--save") ? arg_value(argc, argv, "--save") : "", verbose);
	}
	const bool from_setup = !rp.has_save;   // a random map of the original: generated from the header's setup (net/playback.hpp)
	// the start game of every instance: the one implementation the app's playback shares (net/playback.hpp)
	std::string open_err;
	std::unique_ptr<net::Playback> pb = net::Playback::open(path, game_dir, open_err);
	if (!pb) {
		std::printf("%s: %s%s\n", basename_of(path).c_str(), open_err.c_str(), from_setup ? " (`ron-net timing` runs the turn clock of such replays)" : "");
		return from_setup ? 0 : 1;
	}
	if (from_setup) std::printf("%s: no embedded save: random map generated from the header's setup (new_random_game, then the playback prologue)\n", basename_of(path).c_str());
	Feed feed;
	if (!feed.build(rp, err)) {
		std::printf("%s: %s\n", basename_of(path).c_str(), err.c_str());
		return 1;
	}
	if (!from_setup && !rp.has_save_info) {
		std::printf("%s: cannot read the embedded save's game info\n", basename_of(path).c_str());
		return 1;
	}

	std::array<int, 8> seat_who{};
	for (int i = 0; i < 8; ++i) seat_who[size_t(i)] = i;
	for (const sim::ReplayPlayer& p : rp.header.players)
		if (p.present()) seat_who[size_t(p.play & 7)] = p.who;

	// observers sit in slots the replay does not use; they are extra instances that see everything and send nothing
	std::vector<int> observers;
	for (int p = 0; p < 8 && int(observers.size()) < n_observers; ++p)
		if (!rp.header.players[size_t(p)].present() && std::find(feed.humans.begin(), feed.humans.end(), p) == feed.humans.end()) observers.push_back(p);
	if (int(observers.size()) < n_observers) std::printf("  only %zu observer slot(s) are free in this replay\n", observers.size());
	std::vector<int> seats = feed.humans;
	seats.insert(seats.end(), observers.begin(), observers.end());
	net::LoopbackHub hub(8, 1234, dlo, dhi);
	std::vector<Inst> inst;
	std::vector<std::unique_ptr<sim::Game>> games;
	bool rules_equal = false;
	const net::ExecPolicy policy = has_flag(argc, argv, "--all-verbs") ? net::ExecPolicy::all_engine() : net::ExecPolicy::sav_compatible();
	for (int slot : seats) {
		std::string lerr;
		std::unique_ptr<sim::Game> g = pb->make_start(lerr, net::PlaybackEnv(), !no_art);
		if (!g) {
			std::printf("%s: %s\n", basename_of(path).c_str(), lerr.c_str());
			return 1;
		}
		for (const std::string& n : pb->take_notes())
			if (slot == seats.front() || n.find("not attached") != std::string::npos) std::printf("  %s\n", n.c_str());
		if (slot == seats.front()) rules_equal = pb->rules_equal();
		net::ExecPolicy pol = policy;
		if (has_flag(argc, argv, "--no-commands")) pol.execute = false;
		Inst in;
		in.slot = slot;
		in.hooks = std::make_unique<net::GameHooks>(*g, seat_who, net::ChecksumMode::Original, pol);
		in.session = std::make_unique<net::Session>(hub.endpoint(slot), *in.hooks, session_config_for(rp, slot, observers));
		games.push_back(std::move(g));
		inst.push_back(std::move(in));
	}
	for (int p = 0; p < 8; ++p)
		if (std::find(seats.begin(), seats.end(), p) == seats.end()) hub.kill(p);
	net::GameHooks& h0 = static_cast<net::GameHooks&>(*inst[0].hooks);

	// expected value per boundary frame (ron-sav --replay): the row at record frame R describes the boundary R - turn, the first
	// row's boundary is the save frame + 1
	const auto rows = all_rows(rp);
	if (rows.empty()) {
		std::printf("replay: no CHECK_SUMS rows in %s\n", basename_of(path).c_str());
		return 1;
	}
	const int64_t start = games[0]->frame;
	std::map<int64_t, Parts> expect;
	{
		int64_t prev = 0;
		bool first = true;
		for (const auto& kv : rows) {
			auto best = std::max_element(kv.second.begin(), kv.second.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
			expect[first ? start + 1 : prev] = best->first;
			prev = kv.first;
			first = false;
		}
	}
	int64_t last_boundary = expect.rbegin()->first;
	if (max_frames > 0) last_boundary = std::min<int64_t>(last_boundary, start + max_frames);
	std::printf("%s: replay from %s frame %lld, %zu CHECK_SUMS turns, %zu records, rules %s\n", basename_of(path).c_str(), from_setup ? "generated-game" : "save", (long long)start, rows.size(),
	            rp.packages.size(), rules_equal ? "equal to the install's" : "adopted from the save");
	{
		uint32_t v[16];
		h0.checksums(v);
		const Parts& e1 = expect.begin()->second;
		std::printf("  save state (frame %lld) against the first row (boundary %lld):", (long long)start, (long long)expect.begin()->first);
		for (int k = 0; k < dw::kCheckPartCount; ++k) std::printf(" %s=%s", dw::kCheckPartNames[k], v[k] == e1[size_t(k)] ? "=" : "!");
		std::printf("\n");
	}

	const uint32_t M = feed.max_stamp(last_boundary);
	int64_t first_diff[dw::kCheckPartCount], last_match[dw::kCheckPartCount];
	for (int k = 0; k < dw::kCheckPartCount; ++k) first_diff[k] = last_match[k] = -1;
	int64_t boundaries = 0, all_match = 0, last_frame_run = start;
	std::set<int64_t> compared;
	RunStats st;
	std::string run_err;
	auto per_boundary = [&](size_t j) {
		const net::BoundaryInfo& b = inst[0].bis[j];
		last_frame_run = b.frame;
		if (!compared.insert(b.frame).second) return;
		auto it = expect.find(b.frame);
		if (it == expect.end()) return;
		++boundaries;
		int m = 0;
		for (int k = 0; k < dw::kCheckPartCount; ++k) {
			const bool same = b.sums[k] == it->second[size_t(k)];
			m += same;
			if (same) {
				if (first_diff[k] < 0) last_match[k] = b.frame;
			} else if (first_diff[k] < 0) {
				first_diff[k] = b.frame;
			}
			if (verbose && !same) std::printf("  frame %d part %-16s engine %10u expected %10u\n", b.frame, dw::kCheckPartNames[k], b.sums[k], it->second[size_t(k)]);
		}
		all_match += m == dw::kCheckPartCount;
	};
	run_instances(inst, hub, feed, M, st, per_boundary, run_err);

	// ---- the numbers of ron-sav --replay
	std::printf("  simulated to frame %lld (%lld turn boundaries compared, %lld with all 15 parts equal), %llu commands executed\n", (long long)last_frame_run,
	            (long long)boundaries, (long long)all_match, (unsigned long long)h0.commands_run);
	if (!run_err.empty()) std::printf("  STOPPED: %s\n", run_err.c_str());
	if (h0.first_unsupported_frame >= 0) {
		std::map<std::string, uint64_t> by_name;
		for (const auto& kv : h0.unsupported) by_name[sim::replay_verb_name(kv.first)] += kv.second;
		std::printf("  commands the engine does not execute (first at record frame %lld, %s):", (long long)h0.first_unsupported_frame,
		            sim::replay_verb_name(h0.first_unsupported_op));
		for (const auto& kv : by_name) std::printf(" %s x%lld", kv.first.c_str(), (long long)kv.second);
		std::printf("\n");
	}
	for (int k = 0; k < dw::kCheckPartCount; ++k) {
		if (first_diff[k] < 0) std::printf("  %-16s matches through frame %lld\n", dw::kCheckPartNames[k], (long long)last_frame_run);
		else std::printf("  %-16s first differs at boundary frame %lld (matched through %lld)\n", dw::kCheckPartNames[k], (long long)first_diff[k], (long long)last_match[k]);
	}

	// ---- the lockstep contract
	int rc = 0;
	std::printf("  lockstep: %zu instances (slots", inst.size());
	for (const Inst& in : inst) std::printf(" %d", in.slot);
	std::printf("%s), %zu turns run, delivery delay %d..%d ms, ", observers.empty() ? "" : "; the last ones observe", st.boundaries_compared, dlo, dhi);
	if (st.first_disagree < 0 && st.failed_instances == 0 && run_err.empty()) {
		std::printf("the instances agree on all 15 parts at every turn\n");
	} else {
		rc = 1;
		if (st.first_disagree >= 0) std::printf("DISAGREE at %s\n", st.disagree_text.c_str());
		if (st.failed_instances) std::printf("FAILED %s\n", st.first_failure.c_str());
	}
	// the final states are the same too
	{
		uint32_t a[16];
		bool ok = true;
		for (size_t k = 0; k < inst.size(); ++k) {
			uint32_t b[16];
			static_cast<net::GameHooks&>(*inst[k].hooks).checksums(b);
			if (k == 0) std::memcpy(a, b, sizeof a);
			else if (std::memcmp(a, b, sizeof a) != 0) ok = false;
		}
		std::printf("  final state (frame %d on every instance): %s\n", games[0]->frame, ok ? "identical" : "DIFFERENT");
		if (!ok) rc = 1;
	}
	return rc;
}

// ron-net playback: ONE engine instance plays the recording (net/playback.hpp), the way the app's Recorded Games viewer does.
int cmd_playback(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: ron-net playback <file.rcx> [--game DIR] [--frames N] [--all-verbs] [--no-art] [--save out.sav]\n");
		return 64;
	}
	const std::string path = argv[2];
	const std::string game_dir = arg_value(argc, argv, "--game") ? arg_value(argc, argv, "--game") : game_dir_default();
	const int64_t frames = arg_value(argc, argv, "--frames") ? std::atoll(arg_value(argc, argv, "--frames")) : 0;
	std::string err;
	std::unique_ptr<net::Playback> pb = net::Playback::open(path, game_dir, err);
	if (!pb) {
		std::printf("%s: %s\n", basename_of(path).c_str(), err.c_str());
		return 1;
	}
	std::unique_ptr<sim::Game> g = pb->make_start(err, net::PlaybackEnv(), !has_flag(argc, argv, "--no-art"));
	if (!g) {
		std::printf("%s: %s\n", basename_of(path).c_str(), err.c_str());
		return 1;
	}
	for (const std::string& n : pb->take_notes()) std::printf("  %s\n", n.c_str());
	const int rc = net::playback_report(*pb, *g, frames, has_flag(argc, argv, "--all-verbs"));
	if (const char* out = arg_value(argc, argv, "--save")) {
		try {
			sim::save_game(*g, pb->scenario(), pb->seed(), out);
		} catch (const std::exception& e) {
			std::printf("  --save: %s\n", e.what());
		}
	}
	return rc;
}

}  // namespace ronnet
