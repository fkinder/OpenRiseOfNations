// ron-net replay of a recording this port wrote (net/record.hpp): the embedded native save (or the random game of the header's
// scenario_path) is loaded once per human seat; each instance is fed only its own player's packages through the lockstep layer, so
// the replay checks what a live session checks (the instances agree at every turn) and ends in the state the recorded game ended in.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

#include "commands.hpp"
#include "common.hpp"
#include "net/game_link.hpp"
#include "net/playback.hpp"
#include "node.hpp"
#include "sim/anim_assets.hpp"
#include "sim/newgame.hpp"
#include "sim/rules.hpp"
#include "sim/rules_xml.hpp"
#include "sim/save.hpp"

namespace ronnet {

int replay_native(const sim::Replay& rp, const std::string& path, const std::string& game_dir, int dlo, int dhi, const std::string& save_out, bool verbose) {
	const std::string name = basename_of(path);
	std::string err;
	Feed feed;
	if (!feed.build(rp, err)) {
		std::printf("%s: %s\n", name.c_str(), err.c_str());
		return 1;
	}
	try {
		std::string oerr;
		std::unique_ptr<net::Playback> pb = net::Playback::open(path, game_dir, oerr);
		if (!pb) {
			std::printf("%s: %s\n", name.c_str(), oerr.c_str());
			return 1;
		}
		const std::string scenario = pb->scenario();
		const uint32_t seed = pb->seed();
		const std::array<int, 8> who = pb->seat_who();
		net::LoopbackHub hub(8, 1234, dlo, dhi);
		std::vector<std::unique_ptr<EngineNode>> nodes;
		std::vector<Inst> inst;
		for (int slot : feed.humans) {
			std::unique_ptr<sim::Game> g = pb->make_start(oerr);
			if (!g) {
				std::printf("%s: %s\n", name.c_str(), oerr.c_str());
				return 1;
			}
			nodes.push_back(std::make_unique<EngineNode>(pb->files(), *pb->own_rules(), pb->own_art(), std::move(g), scenario, seed, who));
			Inst in;
			in.slot = slot;
			in.session = std::make_unique<net::Session>(hub.endpoint(slot), nodes.back()->hooks(), session_config_for(rp, slot));
			inst.push_back(std::move(in));
			inst.back().hooks.reset();  // the hooks belong to the node
		}
		for (int p = 0; p < 8; ++p)
			if (std::find(feed.humans.begin(), feed.humans.end(), p) == feed.humans.end()) hub.kill(p);
		const uint32_t M = feed.max_stamp(INT64_MAX);
		std::printf("%s: native recording, starts at frame %d (%s), %zu records, %u turns to run, %zu instance(s)\n", name.c_str(), nodes[0]->game().frame,
		            rp.native_save ? "embedded save" : "random game from the header", rp.packages.size(), M, inst.size());
		RunStats st;
		std::string run_err;
		run_instances(inst, hub, feed, M, st, nullptr, run_err);
		int rc = 0;
		std::printf("  lockstep: %zu turns compared, delivery delay %d..%d ms, ", st.boundaries_compared, dlo, dhi);
		if (st.first_disagree < 0 && st.failed_instances == 0 && run_err.empty()) {
			std::printf("the instances agree at every turn\n");
		} else {
			rc = 1;
			if (st.first_disagree >= 0) std::printf("DISAGREE at %s\n", st.disagree_text.c_str());
			if (st.failed_instances) std::printf("FAILED %s\n", st.first_failure.c_str());
			if (!run_err.empty()) std::printf("STOPPED %s\n", run_err.c_str());
		}
		const std::vector<uint8_t> first = nodes[0]->save_bytes();
		bool same = true;
		for (size_t k = 1; k < nodes.size(); ++k) same = same && nodes[k]->save_bytes() == first;
		std::printf("  final state: frame %d, native save %zu bytes, fnv64 %016llx, %s\n", nodes[0]->game().frame, first.size(), (unsigned long long)fnv64(first),
		            same ? "identical on every instance" : "DIFFERENT between instances");
		if (!same) rc = 1;
		if (!save_out.empty()) {
			std::ofstream f(save_out, std::ios::binary);
			f.write(reinterpret_cast<const char*>(first.data()), std::streamsize(first.size()));
		}
		(void)verbose;
		return rc;
	} catch (const std::exception& e) {
		std::printf("%s: error: %s\n", name.c_str(), e.what());
		return 1;
	}
}

}  // namespace ronnet
