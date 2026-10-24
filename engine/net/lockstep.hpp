// The lockstep layer of multiplayer (docs/multiplayer-spec.md section 8): turns, per-player packages, the late-package wait,
// pause, drops, desync detection. Transport-agnostic: the wire is an abstract `Transport` (reliable, ordered per sender), the
// simulation an abstract `SimHooks`. The layer knows the package format (the verbs of docs/replay-spec.md section 9, through
// sim/replay.hpp) but nothing about the engine's Game.
//
// One peer's view of a game, exactly the original's (multiplayer-spec section 1.3):
//   - every OS-loop iteration is a *tick*; `step()` runs one. A tick increments `turn_counter`; at `turn_length` it is a turn
//     boundary: compute the 16 check sums of the state, queue CHECK_SUMS (+ TURN_DATA + CAMERA) into the local package, broadcast
//     it as stamp s, then execute every expected sender's package of stamp s - 1 in slot order 0..7 (waiting for late ones),
//     compare the sums that came with them, and recompute `turn_length` from the TURN_DATA that were executed. Then, unless the
//     game is paused, the simulation steps one frame. A command therefore runs one turn after it was issued.
//   - the only wall-clock use is deciding when a tick happens (the caller's loop) and when a silent peer is given up on.
//
// Everything is single-threaded: call `step()` (which also pumps the transport) from one loop; `submit()` between steps.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ron::net {

using Bytes = std::vector<uint8_t>;

constexpr int kMaxSlots = 8;
constexpr size_t kMaxPackageBytes = 512;     // payload of one package (NetMsg_CommandPackageData)
constexpr size_t kFifoCapacity = 20;         // PackageFifo entries
constexpr int kHistoryPackages = 8;          // executed packages kept per sender for drop resends (the original keeps 3)

// Player.flags (PlayerFlags, multiplayer-spec section 2.2).
enum PlayerFlag : uint32_t {
	kPlayerValid = 0x1, kPlayerLocal = 0x2, kPlayerHuman = 0x4, kPlayerObserver = 0x8, kPlayerDropped = 0x10,
	kPlayerResigned = 0x40, kPlayerDefeated = 0x80, kPlayerWon = 0x100
};

// MPDropOptions (multiplayer-spec section 3).
enum DropOption : int { kDropPlayer = 0, kDropDiplomacy = 1, kDropSurvival = 2, kDropReplaceWithAi = 3, kDropSaveAndExit = 4, kDropExit = 5 };

// Message types (the first byte of every message). 7, 13, 14, 29 are the original's NetMsgType; 0x87 is IPT_DSYNCMSG; 0x91 is the
// port's DROP_STATE (the final-stamp agreement of multiplayer-spec 8.2.2) and DROP_NEED (a peer asks for a package of a given-up player). 0xA0.. belong to the lobby (net/lobby.hpp).
namespace msg {
constexpr uint8_t kPackage = 7, kDropVote = 13, kDropDecision = 14, kDropFlag = 29, kDsync = 0x87, kDropState = 0x91, kDropNeed = 0x92;
}

// ---------------------------------------------------------------- transport

// A set of peers addressed by slot number 0..7 (peer id == lobby slot). Messages keep their boundaries; delivery is reliable
// and ordered per sender. `broadcast(msg, true)` also delivers to the sender itself, as the original's send_all does.
class Transport {
public:
	virtual ~Transport() = default;
	virtual int local_peer() const = 0;
	virtual std::vector<int> peers() const = 0;  // the other peers still connected
	virtual bool is_host() const = 0;
	virtual bool send(int peer, const Bytes& m) = 0;
	virtual uint64_t now_ms() const = 0;
	// Blocks up to `ms` for incoming traffic (sockets); the loopback and tests do nothing.
	virtual void idle(int ms) { (void)ms; }

	// Everyone else, then (include_self) the sender itself.
	void broadcast(const Bytes& m, bool include_self);
	// Non-blocking receive: pushed-back and self messages first, then the wire.
	bool poll(int& from, Bytes& m);
	// Gives a message back (the lobby does this for game messages that arrived early).
	void push_front(int from, Bytes m) { front_.emplace_front(from, std::move(m)); }
	// A peer whose connection ended since the last call (on_player_timed_out).
	bool take_timeout(int& peer);

protected:
	virtual bool poll_impl(int& from, Bytes& m) = 0;
	virtual bool timeout_impl(int& peer) = 0;

private:
	std::deque<std::pair<int, Bytes>> front_, self_;
};

// N endpoints in one process. Messages are delivered after a deterministic pseudo-random delay (per-link order kept), on a
// logical clock the caller advances. Test controls: hold a link, kill a peer (also in the middle of a broadcast).
class LoopbackHub;
class LoopbackTransport : public Transport {
public:
	LoopbackTransport(LoopbackHub& hub, int me) : hub_(hub), me_(me) {}
	int local_peer() const override { return me_; }
	std::vector<int> peers() const override;
	bool is_host() const override { return me_ == 0; }
	bool send(int peer, const Bytes& m) override;
	uint64_t now_ms() const override;

protected:
	bool poll_impl(int& from, Bytes& m) override;
	bool timeout_impl(int& peer) override;

private:
	LoopbackHub& hub_;
	int me_;
};

class LoopbackHub {
public:
	explicit LoopbackHub(int n, uint32_t seed = 1, int min_delay_ms = 0, int max_delay_ms = 0);
	LoopbackTransport& endpoint(int i) { return *eps_[size_t(i)]; }
	int size() const { return n_; }
	uint64_t now() const { return clock_; }
	void advance(uint64_t ms = 1) { clock_ += ms; }
	void set_delay(int min_ms, int max_ms) { min_delay_ = min_ms, max_delay_ = max_ms; }
	void hold(int from, int to, bool on) { held_[size_t(from)][size_t(to)] = on; }  // withheld messages queue up and arrive on release
	void kill(int peer);                          // the peer is gone: nothing more to or from it; the others get a timeout notice
	void kill_after_sends(int peer, int sends);   // the peer dies at its (sends+1)-th link send (a broadcast cut in the middle)
	bool alive(int peer) const { return alive_[size_t(peer)]; }
	uint64_t messages_sent() const { return sent_; }
	size_t in_flight() const;

private:
	friend class LoopbackTransport;
	struct Item {
		uint64_t at;
		Bytes m;
	};
	void send(int from, int to, const Bytes& m);
	bool poll(int me, int& from, Bytes& m);
	bool timeout(int me, int& peer);
	uint32_t rnd() { return seed_ = seed_ * 1664525u + 1013904223u; }

	int n_;
	uint32_t seed_;
	int min_delay_, max_delay_;
	uint64_t clock_ = 0, sent_ = 0;
	std::vector<std::unique_ptr<LoopbackTransport>> eps_;
	std::array<std::array<std::deque<Item>, kMaxSlots>, kMaxSlots> link_;
	std::array<std::array<bool, kMaxSlots>, kMaxSlots> held_{};
	std::array<std::array<uint64_t, kMaxSlots>, kMaxSlots> last_at_{};
	std::array<bool, kMaxSlots> alive_{};
	std::array<int, kMaxSlots> budget_{};
	std::array<std::deque<int>, kMaxSlots> timeouts_;
};

// ---------------------------------------------------------------- simulation side

// What the lockstep layer needs from the simulation (multiplayer-spec 8.1 "Sim contract").
class SimHooks {
public:
	virtual ~SimHooks() = default;
	virtual int32_t frame() const = 0;
	virtual void step_frame() = 0;
	// The 15 CHECK_SUMS parts of the current state in out[0..14] and their 32-bit sum in out[15]. False: cannot compute.
	virtual bool checksums(uint32_t out[16]) = 0;
	// Executing one sender's package: begin, every command (opcode first, padding removed), end. The lockstep verbs (CHECK_SUMS,
	// TURN_DATA, PAUSE, SPEED_*, CANNON_TIME, RESIGN, QUIT, UNGRACEFUL_PLAYER_DROP) are handled by the layer *and* also given here.
	virtual void begin_package(int slot, uint32_t stamp) { (void)slot, (void)stamp; }
	virtual void execute(int slot, const uint8_t* cmd, size_t len) = 0;
	virtual void end_package(int slot) { (void)slot; }
	// Cannon time needs a use left in the issuer's leader data (TurnControl::start_cannon_time).
	virtual bool can_cannon_time(int slot) { (void)slot; return true; }
	// The sender of a TURN_DATA with bit 7 of ping_time set sees the whole map (Game::action_cheat_view_all).
	virtual void view_all(int slot) { (void)slot; }
	// The local CAMERA command (presentation only; the default writes zeros).
	virtual void camera(uint8_t& zoom, int32_t& x, int32_t& y) { zoom = 0, x = 0, y = 0; }
	// True once the game is over (TURN_DATA then carries bit 7).
	virtual bool game_over() const { return false; }
};

// ---------------------------------------------------------------- session

struct SessionConfig {
	int local_slot = 0;
	uint32_t seed = 0;                                // GameInfo.seed (informational: wire packages carry no padding)
	int speed = 2;                                    // game_speed: timings[speed] = {200, 125, 67, 50, 1} ms per frame
	std::array<uint32_t, kMaxSlots> slot_flags{};     // PlayerFlag bits per slot at the start (VALID, HUMAN, OBSERVER ...)
	int max_pauses = 10;                              // pauses per player; <= 0 = unlimited (GameInfo.flags & 0x40)
	int checksum_failure_threshold = 0;               // GameInfo.checksum_failure_threshold: mismatches tolerated (0 = the first is fatal)
	int drop_default = kDropReplaceWithAi;            // this peer's vote when a player is dropped
	uint64_t timeout_ms = 0;                          // stall time after which the awaited peer is given up on (0 = only transport notices)
	int ping_time = 72;                               // TURN_DATA ping_time: the original's constant 200 stored as 200 & 0xff7f (section 1.4)
};

struct DesyncReport {
	uint32_t stamp = 0;       // the stamp being executed when the comparison failed
	int32_t frame = 0;
	bool peer_reported = false;
	int reporter = -1;
	std::array<bool, kMaxSlots> present{};
	std::array<std::array<uint32_t, 16>, kMaxSlots> sums{};
	int first_component = -1;  // first of the 15 parts that differ between the first two senders
	std::string text() const;
};

enum class StepKind { Advanced, Stalled, Desync, Ended, Error };
struct StepResult {
	StepKind kind = StepKind::Advanced;
	int slot = -1;          // Stalled: the slot whose package is awaited
	bool turn = false;      // Advanced: a turn boundary ran to completion in this tick
	std::string text;
};

// Reported at the start of every turn boundary, before any package of the turn runs.
struct BoundaryInfo {
	uint32_t turn = 0;       // 1-based count of boundaries
	uint32_t sent_stamp = 0;  // the stamp of the package this boundary broadcasts
	int32_t frame = 0;
	uint32_t sums[16] = {};
	bool sums_ok = false;
	int turn_length = 0;
};

class Session {
public:
	Session(Transport& transport, SimHooks& hooks, const SessionConfig& config);

	// Sends stamp 1 (CommandManager::start). Submit the opening commands (LEADER_OPTIONS ...) before.
	void start();
	// Appends commands (a package payload: complete commands back to back) to the local package. False when they do not fit in
	// 512 bytes (room is kept for the automatic CHECK_SUMS / TURN_DATA / CAMERA) or are malformed; nothing is added then.
	bool submit(const uint8_t* cmds, size_t len);
	bool submit(const Bytes& cmds) { return submit(cmds.data(), cmds.size()); }
	// Transport -> FIFOs (also done by step()).
	void pump();
	// At most one simulation tick.
	StepResult step();

	// The transport reported (or the caller decided) that a peer is silent: starts the drop agreement of multiplayer-spec 3.
	void report_timeout(int slot);
	// This peer's vote for the next drop (DropOption).
	void set_drop_preference(int option) { cfg_.drop_default = option; }
	void request_end() { ended_ = true; }

	// ---- observers of the state
	const SessionConfig& config() const { return cfg_; }
	int local_slot() const { return cfg_.local_slot; }
	int turn_length() const { return turn_length_; }
	int turn_counter() const { return turn_counter_; }
	int speed() const { return speed_; }
	bool paused() const { return paused_; }
	uint32_t local_stamp() const { return local_stamp_; }
	uint32_t last_accepted(int slot) const { return slot_[size_t(slot)].last_accepted; }
	uint32_t last_executed(int slot) const { return slot_[size_t(slot)].last_executed; }
	uint32_t boundaries() const { return boundaries_; }
	uint64_t ticks() const { return ticks_; }
	uint32_t flags(int slot) const { return slot_[size_t(slot)].flags; }
	bool expected(int slot) const;      // the slot's package is awaited at a boundary
	bool local_active() const;          // this peer sends packages
	bool in_turn() const { return phase_ != 0; }
	bool ended() const { return ended_; }
	const DesyncReport& desync() const { return desync_; }
	const std::string& error() const { return error_; }
	uint64_t duplicates() const { return duplicates_; }   // packages ignored because their stamp was already accepted
	size_t fifo_size(int slot) const { return slot_[size_t(slot)].fifo.size(); }
	std::string describe_drop(int slot) const;  // the state of the drop agreement about a slot (diagnostics)

	// ---- callbacks
	std::function<void(const BoundaryInfo&)> on_boundary;                                          // start of each turn boundary
	std::function<void(int32_t frame, int slot, uint32_t stamp, const Bytes& data)> on_package;    // after each executed package
	std::function<void(int slot, uint64_t waited_ms)> on_waiting;                                  // each step spent waiting on a slot
	std::function<void(int slot, int option, uint32_t boundary)> on_drop;                          // a drop was applied
	std::function<void(int slot)> on_resign;

private:
	struct Pkt {
		uint32_t stamp = 0;
		Bytes data;
	};
	struct DropCtl {
		uint32_t flag_mask = 0;     // which peers flagged the slot
		bool flagged_by_me = false;
		bool pending = false;       // consensus reached: everyone remaining gave the slot up
		bool decided = false;
		bool relayed = false, decision_sent = false;
		uint32_t remaining = 0;     // peers that take part in the agreement
		std::array<int64_t, kMaxSlots> state{};  // each peer's last accepted stamp of the slot (-1 unknown)
		std::array<int, kMaxSlots> vote{};       // -1 none
		int option = kDropReplaceWithAi;
		uint32_t final_stamp = 0;
		DropCtl() {
			state.fill(-1);
			vote.fill(-1);
		}
	};
	struct Slot {
		uint32_t flags = 0;
		bool connected = false;
		uint32_t last_accepted = 0, last_executed = 0;
		std::deque<Pkt> fifo, hist;
		int pauses = 0;
		bool turn_data = false;
		int ping = 0;
		bool has_sums = false;
		std::array<uint32_t, 16> sums{};
		int64_t dsync = -1;
		DropCtl drop;
	};

	void handle(int from, const Bytes& m);
	void accept_package(int from, const Bytes& m);
	StepResult run_turn();
	bool begin_turn(std::string& err);
	void send_local_package();
	void issue_turn_data(Bytes& pkg);
	bool gate_drops(StepResult& stall);
	void execute_package(int slot, uint32_t stamp, const Bytes& data);
	void lockstep_verb(int slot, const uint8_t* cmd, size_t len);
	void end_turn();
	void compute_next_turn();
	void fail(const std::string& text);
	void enter_desync(bool peer_reported, int reporter);
	void apply_drop(int slot, int option);
	void clear_slot_queue(int slot);
	void process_pause(int slot, int state);
	void set_speed(int s);
	void start_cannon_time(int slot, int state);
	void check_cannon_time();
	bool valid_sender(int slot) const;
	// drops
	void drop_check_consensus(int p);
	void drop_progress(int p);
	void on_drop_flag(int from, int p);
	void on_drop_state(int from, int p, uint32_t stamp);
	void on_drop_vote(int from, int p, int option);
	void on_drop_decision(int p, int option, uint32_t final_stamp);
	void on_drop_need(int from, int p, uint32_t stamp);
	int arbiter(int p) const;
	void send_to_all(const Bytes& m, bool include_self) { t_.broadcast(m, include_self); }

	Transport& t_;
	SimHooks& h_;
	SessionConfig cfg_;
	std::array<Slot, kMaxSlots> slot_;

	Bytes local_pkg_;
	uint32_t local_stamp_ = 0;
	int speed_ = 2, saved_speed_ = 2;
	int turn_length_ = 2, turn_counter_ = 0;
	int min_turn_time_ = 400;
	bool paused_ = false, in_tick_ = false, started_ = false, ended_ = false;
	bool cannon_ = false;
	int32_t cannon_stamp_ = 0;
	uint32_t boundaries_ = 0;
	uint64_t ticks_ = 0, duplicates_ = 0;
	int phase_ = 0;      // 0 idle, 1 sent (drop gate), 2 executing
	int next_slot_ = 0;
	uint32_t target_stamp_ = 0;
	int fail_count_ = 0;
	bool errored_ = false, in_desync_ = false;
	std::string error_;
	DesyncReport desync_;
	int stall_slot_ = -1;
	uint64_t stall_since_ = 0, need_sent_ = 0;
	BoundaryInfo last_boundary_;
};

// Builds and splits the COMMANDPACKAGEDATA message: u8 7, u32 stamp, i8 play, i16 size, data.
Bytes pack_package_msg(uint32_t stamp, int play, const Bytes& data);
bool unpack_package_msg(const Bytes& m, uint32_t& stamp, int& play, Bytes& data);

}  // namespace ron::net
