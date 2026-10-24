// One engine instance of a network test: a Game, its GameHooks (native check sums), the OOS restore callback and the `.rcx` recorder with
// its segment chain (a recovery closes the file and opens the next one with the snapshot embedded, as the original's recorder does).
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "assets/files.hpp"
#include "net/game_link.hpp"
#include "net/lockstep.hpp"
#include "net/record.hpp"
#include "sim/anims.hpp"
#include "sim/game.hpp"
#include "sim/rules.hpp"

namespace ronnet {

namespace sim = ron::sim;
namespace net = ron::net;

class EngineNode {
public:
	EngineNode(const ron::GameFiles& files, const sim::Rules& rules, const sim::AnimSource* art, std::unique_ptr<sim::Game> g, const std::string& scenario,
	           uint32_t seed, const std::array<int, 8>& who, net::ExecPolicy policy = net::ExecPolicy::all_engine());

	sim::Game& game() { return *game_; }
	net::GameHooks& hooks() { return *hooks_; }
	// The session's callbacks: packages go to the recorder, a recovery starts the next segment. Call once, after the Session exists.
	void attach(net::Session& s);
	// Starts recording (segment 1 = `base`). `info` is the header of a game that starts here.
	bool record(const std::string& base, const net::RecordInfo& info, std::string& error);
	void finish_recording();
	std::vector<std::string> segments() const { return segments_; }
	// The native save of the current state (what two peers compare at the end of a run).
	std::vector<uint8_t> save_bytes() const;
	int recoveries = 0;
	std::string error;
	std::function<void()> on_restored;   // after a snapshot replaced the game

private:
	const ron::GameFiles& files_;
	const sim::Rules& rules_;
	const sim::AnimSource* art_;
	std::unique_ptr<sim::Game> game_;
	std::unique_ptr<net::GameHooks> hooks_;
	net::Recorder rec_;
	net::RecordInfo first_;
	std::string base_;
	int segment_ = 0;
	std::vector<std::string> segments_;
	bool recording_ = false;
};

uint64_t fnv64(const std::vector<uint8_t>& b);

}  // namespace ronnet
