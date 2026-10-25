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
	std::string ctw_battle;                    // "CAMPAIGN[:LINK]" (alexander:auto, napoleon:auto:2, world:FROM>TO): a Conquer the World battle on its generated map; the first `humans` seats are players
	std::string scenario;                      // a scenario file (instead of random_spec): its players are the seats, the human-flagged ones first
	int humans = 2;
	int observers = 0;                         // machines that only watch (game slots above the nations; docs/multiplayer-spec.md 8.6)
	std::vector<int> handicaps;                // per human seat (rules.xml "handicaps" index); the games are made multiplayer games either way
	bool chat_test = false;                    // peer 0 sends a team chat and a broadcast chat at stamp 30; the result reports what every instance recorded
	int32_t frames = 300;
	int dlo = 0, dhi = 2;
	uint32_t hub_seed = 1;
	uint64_t deadlock_rounds = 400000;
	bool recover = false, bots = true, verbose = false;
	std::vector<std::pair<int, int>> diverge;  // (frame, peer): a divergence is injected into the peer when its game reaches the frame
	bool record_start_save = false;            // every recording embeds the start state as a native save (always so for a Conquer the World battle)
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
	std::string chat_report;                     // chat_test: "ok" or what differed
	int handicap_seen = -1;                      // peer 0's leader 1 handicap in the final state
	std::map<int, std::vector<std::string>> segments;
};

LoopbackResult run_loopback(const LoopbackOptions& o);
int cmd_loopback(int argc, char** argv);

}  // namespace ronnet
