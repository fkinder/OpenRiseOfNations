// Shared pieces of ron-net: the replay feed (a replay's packages per player), the multi-instance loopback runner and helpers.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/game_link.hpp"
#include "net/lockstep.hpp"
#include "sim/replay.hpp"

namespace ronnet {

namespace sim = ron::sim;
namespace net = ron::net;

std::string basename_of(const std::string& p);
std::string game_dir_default();
std::vector<std::string> list_rcx(const std::string& path);  // a file, or a directory searched recursively, sorted

// A replay's packages by player and stamp.
struct Feed {
	const sim::Replay* rp = nullptr;
	std::vector<int> humans;  // slots that sent packages
	std::array<std::vector<const sim::ReplayPackage*>, 8> by_stamp;  // [slot][stamp], index 0 unused
	std::array<uint32_t, 8> last{};          // last stamp in the file per slot
	std::array<uint32_t, 8> resigned_at{};   // stamp of the slot's own RESIGN / QUIT (0 = none)
	size_t max_player_bytes = 0;             // largest package without the automatic commands
	bool build(const sim::Replay& r, std::string& err);
	// Highest stamp every expected player has a package for, with record frame <= frame_limit.
	uint32_t max_stamp(int64_t frame_limit) const;
	const sim::ReplayPackage* find(int slot, uint32_t stamp) const {
		const auto& v = by_stamp[size_t(slot)];
		return stamp < v.size() ? v[stamp] : nullptr;
	}
};

// One engine instance (or a null one) in a loopback game.
struct Inst {
	int slot = 0;
	std::unique_ptr<net::SimHooks> hooks;
	std::unique_ptr<net::Session> session;
	uint32_t fed = 0;
	bool done = false, failed = false;
	std::string status;
	std::vector<net::BoundaryInfo> bis;
};

struct RunStats {
	uint64_t rounds = 0;
	size_t boundaries_compared = 0;
	int64_t first_disagree = -1;  // 0-based boundary index
	std::string disagree_text;
	int failed_instances = 0;
	std::string first_failure;
};

// Runs the instances in lockstep over `hub`, feeding each from its own player's packages, until every instance has executed
// stamp M (or failed). Boundaries are compared across instances; `per_boundary(j)` is called once for each boundary j (0-based)
// all instances have reached, in order. Returns false on a deadlock.
bool run_instances(std::vector<Inst>& inst, net::LoopbackHub& hub, const Feed& feed, uint32_t M, RunStats& st,
                   const std::function<void(size_t)>& per_boundary, std::string& err);

// `observers`: extra slots that take part as observers (VALID | OBSERVER): they receive and execute every package, send none.
net::SessionConfig session_config_for(const sim::Replay& rp, int slot, const std::vector<int>& observers = {});

}  // namespace ronnet
