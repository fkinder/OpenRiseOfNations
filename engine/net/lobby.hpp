// The lobby model (docs/multiplayer-spec.md section 2): a key/value store the host owns and mirrors to every peer, ready flags,
// the host's start message that fixes the settings (the host picks the seed), and a rendezvous barrier. Messages travel on the
// same Transport as the game; game messages that arrive early are given back to it.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>

#include "net/lockstep.hpp"

namespace ron::net {

namespace lobbymsg {
constexpr uint8_t kSet = 0xA0, kReady = 0xA1, kStart = 0xA2, kBarrier = 0xA3, kChat = 0xA4, kName = 0xA5, kSnapshot = 0xA6, kFile = 0xA7;
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

	// ---- the room (lobby screen): who is here, chat. The host mirrors names and ready flags into the table (keys name_N, ready_N, N = slot),
	// so a late joiner learns them from the snapshot the host sends to every new peer.
	void set_name(const std::string& name);
	std::string name_of(int slot) const { return get("name_" + std::to_string(slot)); }
	bool ready_of(int slot) const;
	bool all_peers_ready() const;   // every peer other than this one has said ready
	void say(const std::string& text);                // chat line to everybody (the host relays)
	struct ChatLine {
		int from = 0;
		std::string text;
	};
	bool take_chat(ChatLine& out);
	const std::deque<ChatLine>& chat_log() const { return log_; }
	// Host: the transport's timeout notice for a peer that left the room.
	void peer_left(int slot);

	// A file the host hands every peer before START (a saved game that the session continues, multiplayer-spec 6 "Loaded MP game": the port's
	// SyncFile): zlib-compressed, chunked, CRC-32 checked. Peers read it with file_complete() / file().
	void send_file(const Bytes& raw);
	bool file_complete() const { return file_done_; }
	const Bytes& file() const { return file_; }
	bool file_failed() const { return file_bad_; }

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
	std::set<int> known_;                // host: peers that have been sent the table
	std::deque<ChatLine> inbox_, log_;
	bool me_ready_ = false, started_ = false;
	Bytes file_, file_comp_;
	uint32_t file_raw_ = 0, file_crc_ = 0, file_chunks_ = 0, file_got_ = 0, file_chunk_ = 0;
	std::vector<bool> file_have_;
	bool file_done_ = false, file_bad_ = false;
	std::map<int, std::set<int>> barrier_;
	std::set<int> my_barriers_;
};

}  // namespace ron::net
