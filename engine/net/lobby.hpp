// The lobby model (docs/multiplayer-spec.md section 2): a key/value store the host owns and mirrors to every peer, ready flags,
// the host's start message that fixes the settings (the host picks the seed), and a rendezvous barrier. Messages travel on the
// same Transport as the game; game messages that arrive early are given back to it.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "net/lockstep.hpp"

namespace ron::net {

namespace lobbymsg {
constexpr uint8_t kSet = 0xA0, kReady = 0xA1, kStart = 0xA2, kBarrier = 0xA3;
}

class Lobby {
public:
	explicit Lobby(Transport& t) : t_(t) {}

	// Host: stores the value and sends it to the peers. A client's set() is ignored (the lobby is the host's).
	void set(const std::string& key, const std::string& value);
	void set(const std::string& key, int64_t value) { set(key, std::to_string(value)); }
	std::string get(const std::string& key, const std::string& def = std::string()) const;
	int64_t get_int(const std::string& key, int64_t def = 0) const;
	bool has(const std::string& key) const { return kv_.count(key) != 0; }
	const std::map<std::string, std::string>& settings() const { return kv_; }

	// Ready flags (ConnectionData steam_ready_n): `set_ready` tells everyone; all_ready() when every peer (and this one) is ready.
	void set_ready(bool ready);
	bool all_ready() const;

	// Host: fixes the settings and starts the game. `seed` becomes the key game_seed (SetupWin::exec draws it on the host).
	// The START message carries the whole table, so a client that missed a SET is still right.
	void start(uint32_t seed);
	bool started() const { return started_; }
	uint32_t seed() const { return uint32_t(get_int("game_seed", 0)); }

	// Rendezvous (SyncPoint::sync): call barrier(id) and poll until barrier_done(id).
	void barrier(int id);
	bool barrier_done(int id) const;

	// Reads the transport. Returns the number of lobby messages handled.
	int poll();

private:
	Transport& t_;
	std::map<std::string, std::string> kv_;
	std::set<int> ready_;
	bool me_ready_ = false, started_ = false;
	std::map<int, std::set<int>> barrier_;
	std::set<int> my_barriers_;
};

}  // namespace ron::net
