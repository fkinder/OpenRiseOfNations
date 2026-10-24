// The engine loopback runner shared by `ron-net loopback` and `ron-net selftest` (apps/net/loopback_cmd.cpp).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ronnet {

struct LoopbackOptions {
	std::string game_dir, random_spec;
	int humans = 2;
	int32_t frames = 300;
	int dlo = 0, dhi = 2;
	uint32_t hub_seed = 1;
	uint64_t deadlock_rounds = 400000;
	bool recover = false, bots = true, verbose = false;
	std::vector<std::pair<int, int>> diverge;  // (frame, peer): a divergence is injected into the peer when its game reaches the frame
	std::string record_base;                   // records "<base>_peer<i>.rcx" (and "_2", "_3" ... segments after a recovery)
	std::string save_out;                      // peer 0's final native save
};

struct LoopbackResult {
	bool ok = false, identical = false;
	std::string text, scenario;
	uint32_t seed = 0;
	int peers = 0, recoveries = 0;  // recoveries: snapshot loads summed over the peers
	unsigned recovered_events = 0;
	uint32_t epoch = 0;
	int32_t frame = 0;
	uint64_t rounds = 0, final_fnv = 0;
	std::vector<uint64_t> fnv;
	std::map<int, std::vector<std::string>> segments;
};

LoopbackResult run_loopback(const LoopbackOptions& o);
int cmd_loopback(int argc, char** argv);

}  // namespace ronnet
