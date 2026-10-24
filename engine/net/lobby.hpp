// The lobby model (docs/multiplayer-spec.md section 2): a key/value store the host owns and mirrors to every peer, ready flags,
// the host's start message that fixes the settings (the host picks the seed), and a rendezvous barrier. Messages travel on the
// same Transport as the game; game messages that arrive early are given back to it.
#pragma once

#include <cstdint>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "net/lockstep.hpp"

namespace ron::net {

namespace lobbymsg {
constexpr uint8_t kSet = 0xA0, kReady = 0xA1, kStart = 0xA2, kBarrier = 0xA3, kChat = 0xA4, kName = 0xA5, kSnapshot = 0xA6, kFile = 0xA7;
// The seat rules (multiplayer-spec 8.6): a guest's request for a seat setting, the host's answer, the host removing a peer, the start countdown,
// and the end of a game (the room goes back to its lobby).
constexpr uint8_t kRequest = 0xA8, kReply = 0xA9, kKick = 0xAA, kCountdown = 0xAB, kReset = 0xAC;
inline bool is_lobby(uint8_t b) { return b >= 0xA0 && b <= 0xAF; }
}  // namespace lobbymsg

// ---------------------------------------------------------------- seats

// Who sits where, in the *game's* slot numbering (the package order 0..7). Players take game slots 0..players-1 in the order of their peer
// numbers (so with nobody missing, slot = peer, as before); seats beyond them up to `nations` are computer players; observers take the slots
// from `nations` up (spec 2.2: "slots above the player count"). `identity` (a saved game continues: the humans must sit where they sat)
// gives every player the slot equal to its peer number.
struct SeatPlan {
	int nations = 2;                  // seats 0..nations-1 exist
	int players = 0, observers = 0;
	bool overflow = false;            // the observers do not fit above the nations (nations + observers > 8) or a player's peer is >= nations
	std::array<int, kMaxSlots> peer_of;   // game slot -> peer, -1: nobody (a computer seat or unused)
	std::array<int, kMaxSlots> slot_of;   // peer -> game slot, -1: not in the game
	SeatPlan() {
		peer_of.fill(-1);
		slot_of.fill(-1);
	}
	bool is_observer_slot(int slot) const { return slot >= nations && slot < kMaxSlots && peer_of[size_t(slot)] >= 0; }
	// Bit s: game slot s is played by a human.
	uint32_t human_mask() const {
		uint32_t m = 0;
		for (int s = 0; s < nations; ++s)
			if (peer_of[size_t(s)] >= 0) m |= 1u << s;
		return m;
	}
};
// `present`: bit p = peer p is in the room; `observers`: bit p = peer p watches; `wanted_nations`: the host's number of nations (raised to cover the players).
// `forced[peer]` (optional): a player that must take that game slot (a Conquer the World battle seats the campaign's player on its own side).
SeatPlan make_seat_plan(uint32_t present, uint32_t observers, int wanted_nations, bool identity, const std::array<int, kMaxSlots>* forced = nullptr);

// The fields a guest may ask the host to change about its own seat.
enum class SeatField : uint8_t { Tribe = 1, Team = 2, Color = 3, Handicap = 4, Observer = 5 };
// The host's answer to a request.
enum ReplyCode : uint8_t { kAccepted = 0, kInvalid = 1, kTaken = 2, kNoRoom = 3, kDenied = 4, kLocked = 5 };
const char* reply_text(int code);
const char* seat_field_name(SeatField f);

struct SeatRequest {
	uint32_t id = 0;
	int peer = 0, seat = -1;       // the requester and the seat (game slot) it sits in; -1 for an observer
	SeatField field = SeatField::Tribe;
	int value = 0;
	bool pending = false;          // host: waiting for the host's decision (approval mode Ask); false = already applied to the table
};

// Limits the rules give (rules.xml): the number of nations (tribes), handicap entries, colours.
struct LobbyLimits {
	int tribes = 24, handicaps = 21, colors = 8, teams = 4, max_observers = 6;
};

class Lobby {
public:
	explicit Lobby(Transport& t) : t_(t) {}

	// Host: stores the value and sends it to the peers. A client's set() is ignored (the lobby is the host's).
	void set(const std::string& key, const std::string& value);
	void set(const std::string& key, int64_t value) { set(key, std::to_string(value)); }
	std::string get(const std::string& key, const std::string& def = std::string()) const;
	int64_t get_int(const std::string& key, int64_t def = 0) const;
	bool has(const std::string& key) const {
		auto it = kv_.find(key);
		return it != kv_.end() && !it->second.empty();   // an emptied key is gone
	}
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
	// kind: 0 to everybody, 1 to the sender's team, 2 a whisper (to = the receiver), 3 a system line (host announcements: the countdown).
	struct ChatLine {
		int from = 0;
		std::string text;
		int kind = 0, to = -1;
	};
	// Chat to a target: everybody, the team (peers whose seat has the same team, observers talk to observers), or one peer by number.
	// Returns false when nobody would hear it (no team, no such peer).
	bool say_to(int kind, int peer, const std::string& text);
	bool take_chat(ChatLine& out);
	const std::deque<ChatLine>& chat_log() const { return log_; }
	// Host: the transport's timeout notice for a peer that left the room.
	void peer_left(int slot);
	std::set<int> present() const;   // the peers in the room, this one included

	// ---- seat rules (multiplayer-spec 8.6)
	void set_limits(const LobbyLimits& l) { limits_ = l; }
	const LobbyLimits& limits() const { return limits_; }
	// The plan of the current room: players, computer seats, observers (reads obs_N, num_nations and from_save from the table).
	SeatPlan plan(int wanted_nations = -1) const;   // wanted < 0: the table's num_nations
	bool observer_of(int peer) const { return get_int("obs_" + std::to_string(peer)) != 0; }
	// A guest asks for a change of its own seat (the host's answer arrives as a Reply); the host's own change is applied at once (observer only;
	// the host edits the other seat fields directly). Returns the id of the request.
	uint32_t request(SeatField f, int value);
	struct Reply {
		SeatField field = SeatField::Tribe;
		int value = 0, code = 0;
	};
	bool take_reply(Reply& out);
	// Host: how a valid request is handled. Auto: applied to the table at once (the application mirrors it into its own setup through
	// take_request); Ask: queued until the host calls answer().
	enum class Approval { Auto, Ask };
	void set_approval(Approval a) { approval_ = a; }
	Approval approval() const { return approval_; }
	// Host: requests the application must know about: applied ones (pending = false) and the ones that wait for a decision.
	bool take_request(SeatRequest& out);
	const std::deque<SeatRequest>& pending() const { return pending_; }
	void answer(uint32_t id, bool yes);
	// What the host would answer a request now (the rules; no approval): kAccepted or the reason.
	ReplyCode check(int peer, SeatField f, int value) const;
	// Host: writes the change into the table (the Auto path and answer(yes) use it; also for the host's own observer toggle).
	void apply(int peer, SeatField f, int value);

	// Host: removes a peer from the room (it is told, then the transport drops it). The application calls TcpTransport::disconnect.
	void kick(int peer);
	bool kicked() const { return kicked_; }                      // this peer was removed by the host
	// Host: the start countdown (seconds left; <= 0 cancels). Everybody sees it as system lines and through countdown().
	void countdown_set(int seconds);
	int countdown() const { return countdown_; }
	// After a game: the room is a lobby again (the host announces it; every peer clears START, barriers, files and the ready flags).
	void reset_game();

	// A file the host hands every peer before START (a saved game that the session continues, multiplayer-spec 6 "Loaded MP game": the port's
	// SyncFile): zlib-compressed, chunked, CRC-32 checked. Peers read it with file_complete() / file().
	void send_file(const Bytes& raw);
	bool file_complete() const { return file_done_; }
	const Bytes& file() const { return file_; }
	bool file_failed() const { return file_bad_; }

	// Host: fixes the settings and starts the game. `seed` becomes the key game_seed (SetupWin::exec draws it on the host).
	// The START message carries the whole table, so a client that missed a SET is still right.
	// Also fixes the seat plan (keys seat_N = the game slot of peer N, nations_final) and returns false, sending nothing, when it does not fit
	// (no player, observers above the eight slots).
	bool start(uint32_t seed);
	bool started() const { return started_; }
	// The plan START fixed (every peer reads the same one); an empty plan (players == 0) before START.
	SeatPlan start_plan() const;
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
	LobbyLimits limits_;
	Approval approval_ = Approval::Auto;
	uint32_t next_id_ = 1;
	std::deque<SeatRequest> events_, pending_;
	std::deque<Reply> replies_;
	bool kicked_ = false;
	int countdown_ = -1;
	bool me_ready_ = false, started_ = false;
	Bytes file_, file_comp_;
	uint32_t file_raw_ = 0, file_crc_ = 0, file_chunks_ = 0, file_got_ = 0, file_chunk_ = 0;
	std::vector<bool> file_have_;
	bool file_done_ = false, file_bad_ = false;
	std::map<int, std::set<int>> barrier_;
	std::set<int> my_barriers_;
};

}  // namespace ron::net
