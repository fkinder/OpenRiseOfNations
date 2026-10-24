#include "net/lockstep.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "sim/replay.hpp"

namespace ron::net {

namespace {

constexpr int kTimings[5] = {200, 125, 67, 50, 1};  // ms per simulation frame by game speed (TurnControl.timings @afc4a4)
// Room kept in the local package for what the layer appends itself: CHECK_SUMS 65 + TURN_DATA 11 (twice in the first package) + CAMERA 10.
constexpr size_t kReserve = 65 + 11 + 11 + 10;
const char* const kPartNames[15] = {"units", "builds", "walls", "ammo", "deaths", "groups", "guys", "leaders", "cities", "items", "goods",
                                    "world", "rules", "scenario_data", "script_run_time"};

inline uint32_t ld32(const uint8_t* p) {
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}
inline uint16_t ld16(const uint8_t* p) {
	uint16_t v;
	std::memcpy(&v, p, 2);
	return v;
}
inline void put32(Bytes& o, uint32_t v) {
	for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}

}  // namespace

// ---------------------------------------------------------------- messages

Bytes pack_package_msg(uint32_t stamp, int play, const Bytes& data) {
	Bytes m;
	m.reserve(8 + data.size());
	m.push_back(msg::kPackage);
	put32(m, stamp);
	m.push_back(uint8_t(int8_t(play)));
	m.push_back(uint8_t(data.size()));
	m.push_back(uint8_t(data.size() >> 8));
	m.insert(m.end(), data.begin(), data.end());
	return m;
}

bool unpack_package_msg(const Bytes& m, uint32_t& stamp, int& play, Bytes& data) {
	if (m.size() < 8 || m[0] != msg::kPackage) return false;
	stamp = ld32(m.data() + 1);
	play = int8_t(m[5]);
	const size_t n = ld16(m.data() + 6);
	if (n > kMaxPackageBytes || m.size() != 8 + n) return false;
	data.assign(m.begin() + 8, m.end());
	return true;
}

// ---------------------------------------------------------------- transport

void Transport::broadcast(const Bytes& m, bool include_self) {
	for (int p : peers()) send(p, m);
	if (include_self) self_.emplace_back(local_peer(), m);
}

bool Transport::poll(int& from, Bytes& m) {
	if (!front_.empty()) {
		from = front_.front().first;
		m = std::move(front_.front().second);
		front_.pop_front();
		return true;
	}
	if (!self_.empty()) {
		from = self_.front().first;
		m = std::move(self_.front().second);
		self_.pop_front();
		return true;
	}
	return poll_impl(from, m);
}

bool Transport::take_timeout(int& peer) { return timeout_impl(peer); }

LoopbackHub::LoopbackHub(int n, uint32_t seed, int min_delay_ms, int max_delay_ms)
	: n_(std::min(n, kMaxSlots)), seed_(seed), min_delay_(min_delay_ms), max_delay_(std::max(min_delay_ms, max_delay_ms)) {
	for (int i = 0; i < n_; ++i) {
		eps_.push_back(std::make_unique<LoopbackTransport>(*this, i));
		alive_[size_t(i)] = true;
		budget_[size_t(i)] = -1;
	}
}

void LoopbackHub::kill(int peer) {
	if (peer < 0 || peer >= n_ || !alive_[size_t(peer)]) return;
	alive_[size_t(peer)] = false;
	for (int i = 0; i < n_; ++i) {
		link_[size_t(i)][size_t(peer)].clear();
		if (alive_[size_t(i)]) timeouts_[size_t(i)].push_back(peer);
	}
}

void LoopbackHub::kill_after_sends(int peer, int sends) { budget_[size_t(peer)] = sends; }

size_t LoopbackHub::in_flight() const {
	size_t n = 0;
	for (const auto& row : link_)
		for (const auto& q : row) n += q.size();
	return n;
}

void LoopbackHub::send(int from, int to, const Bytes& m) {
	if (!alive_[size_t(from)] || !alive_[size_t(to)]) return;
	int& b = budget_[size_t(from)];
	if (b >= 0) {
		if (b == 0) {
			kill(from);
			return;
		}
		--b;
	}
	++sent_;
	const uint32_t span = uint32_t(max_delay_ - min_delay_ + 1);
	const uint64_t delay = uint64_t(min_delay_) + (span > 1 ? (rnd() >> 8) % span : 0);
	uint64_t at = clock_ + delay;
	at = std::max(at, last_at_[size_t(from)][size_t(to)]);
	last_at_[size_t(from)][size_t(to)] = at;
	link_[size_t(from)][size_t(to)].push_back({at, m});
}

bool LoopbackHub::poll(int me, int& from, Bytes& m) {
	if (!alive_[size_t(me)]) return false;
	for (int f = 0; f < n_; ++f) {
		if (held_[size_t(f)][size_t(me)]) continue;
		auto& q = link_[size_t(f)][size_t(me)];
		if (q.empty() || q.front().at > clock_) continue;
		from = f;
		m = std::move(q.front().m);
		q.pop_front();
		return true;
	}
	return false;
}

bool LoopbackHub::timeout(int me, int& peer) {
	auto& q = timeouts_[size_t(me)];
	if (q.empty()) return false;
	peer = q.front();
	q.pop_front();
	return true;
}

std::vector<int> LoopbackTransport::peers() const {
	std::vector<int> v;
	for (int i = 0; i < hub_.n_; ++i)
		if (i != me_ && hub_.alive_[size_t(i)]) v.push_back(i);
	return v;
}
bool LoopbackTransport::send(int peer, const Bytes& m) {
	if (peer < 0 || peer >= hub_.n_ || peer == me_) return false;
	hub_.send(me_, peer, m);
	return true;
}
uint64_t LoopbackTransport::now_ms() const { return hub_.clock_; }
bool LoopbackTransport::poll_impl(int& from, Bytes& m) { return hub_.poll(me_, from, m); }
bool LoopbackTransport::timeout_impl(int& peer) { return hub_.timeout(me_, peer); }

// ---------------------------------------------------------------- report

std::string DesyncReport::text() const {
	char b[256];
	std::string s;
	std::snprintf(b, sizeof b, "out of sync at stamp %u (frame %d)", stamp, frame);
	s = b;
	if (peer_reported) {
		std::snprintf(b, sizeof b, ": reported by slot %d", reporter);
		s += b;
	}
	if (first_component >= 0) {
		std::snprintf(b, sizeof b, "; first differing part: %s", kPartNames[first_component]);
		s += b;
	}
	for (int p = 0; p < kMaxSlots; ++p) {
		if (!present[size_t(p)]) continue;
		std::snprintf(b, sizeof b, "; slot %d all=%u", p, sums[size_t(p)][15]);
		s += b;
	}
	return s;
}

// ---------------------------------------------------------------- session

Session::Session(Transport& t, SimHooks& h, const SessionConfig& c) : t_(t), h_(h), cfg_(c) {
	speed_ = std::clamp(c.speed, 0, 4);
	saved_speed_ = speed_;
	int humans = 0;
	for (int s = 0; s < kMaxSlots; ++s) {
		Slot& sl = slot_[size_t(s)];
		sl.flags = c.slot_flags[size_t(s)];
		if (s == c.local_slot) sl.flags |= kPlayerLocal;
		sl.connected = (sl.flags & kPlayerValid) && (sl.flags & kPlayerHuman);
		humans += (sl.flags & 5u) == 5u;
	}
	min_turn_time_ = std::clamp(80 * humans, 400, 600);
	turn_length_ = std::max(1, 200 / kTimings[speed_]);
}

bool Session::valid_sender(int slot) const {
	if (slot < 0 || slot >= kMaxSlots) return false;
	const uint32_t f = slot_[size_t(slot)].flags;
	return (f & kPlayerValid) && !(f & kPlayerWon) && !(f & (kPlayerDropped | kPlayerResigned | kPlayerDefeated));
}

bool Session::expected(int slot) const {
	if (!valid_sender(slot)) return false;
	const Slot& s = slot_[size_t(slot)];
	return !(s.flags & kPlayerObserver) && (s.connected || s.drop.pending);
}

bool Session::local_active() const {
	const Slot& s = slot_[size_t(cfg_.local_slot)];
	return valid_sender(cfg_.local_slot) && !(s.flags & kPlayerObserver) && s.connected;
}

bool Session::submit(const uint8_t* cmds, size_t len) {
	if (recovering_) return false;  // the stamps restart: what is typed now would be lost
	size_t p = 0;
	while (p < len) {
		const size_t n = sim::replay_command_length(cmds + p, len - p);
		if (!n) return false;
		p += n;
	}
	if (local_pkg_.size() + len + kReserve > kMaxPackageBytes) return false;
	local_pkg_.insert(local_pkg_.end(), cmds, cmds + len);
	return true;
}

void Session::issue_turn_data(Bytes& pkg) {
	uint16_t ping = uint16_t(cfg_.ping_time & 0xff7f);
	if (h_.game_over()) ping |= 0x80;
	sim::wire_turn_data(pkg, ping);
}

void Session::send_local_package() {
	const bool active = local_active();
	if (active) {
		issue_turn_data(local_pkg_);
		uint8_t zoom = 0;
		int32_t x = 0, y = 0;
		h_.camera(zoom, x, y);
		sim::wire_camera(local_pkg_, zoom, x, y);
	}
	++local_stamp_;
	if (active) t_.broadcast(pack_package_msg(local_stamp_, cfg_.local_slot, local_pkg_), true);
	local_pkg_.clear();
}

uint32_t Session::request_save(uint32_t ahead) {
	const uint32_t at = boundaries_ + std::max<uint32_t>(2, ahead);
	Bytes m{msg::kSaveAt};
	put32(m, at);
	t_.broadcast(m, true);
	return at;
}

void Session::start() {
	if (started_) return;
	started_ = true;
	if (local_active()) issue_turn_data(local_pkg_);
	send_local_package();
}

void Session::fail(const std::string& text) {
	if (errored_) return;
	errored_ = true;
	error_ = text;
}

// ---- receive

void Session::pump() {
	int peer;
	while (t_.take_timeout(peer)) report_timeout(peer);
	int from;
	Bytes m;
	while (t_.poll(from, m)) handle(from, m);
}

void Session::handle(int from, const Bytes& m) {
	if (m.empty()) return;
	// While a recovery runs, everything a peer sent before its RecReady belongs to the epoch that is being repaired: packages, DSYNC and the
	// drop agreement alike (per-sender order makes the RecReady the boundary between the two epochs).
	if (recovering_ && !(rec_.ready_mask & (1u << (from & 7)))) {
		switch (m[0]) {
		case msg::kPackage: case msg::kDsync: case msg::kDropFlag: case msg::kDropState: case msg::kDropVote: case msg::kDropDecision: case msg::kDropNeed: return;
		default: break;
		}
	}
	switch (m[0]) {
	case msg::kPackage: accept_package(from, m); break;
	case msg::kDropFlag:
		if (m.size() == 5) on_drop_flag(from, int(int32_t(ld32(m.data() + 1))));
		break;
	case msg::kDropState:
		if (m.size() == 6) on_drop_state(from, m[1], ld32(m.data() + 2));
		break;
	case msg::kDropVote:
		if (m.size() == 3) on_drop_vote(from, m[1], int8_t(m[2]));
		break;
	case msg::kDropDecision:
		if (m.size() == 7) on_drop_decision(m[1], int8_t(m[2]), ld32(m.data() + 3));
		break;
	case msg::kDropNeed:
		if (m.size() == 6) on_drop_need(from, m[1], ld32(m.data() + 2));
		break;
	case msg::kDsync:
		// a DSYNC sent before the peer's RecReady belongs to the epoch that is being repaired
		if (recovering_ && !(rec_.ready_mask & (1u << (from & 7)))) break;
		if (m.size() == 5 && from >= 0 && from < kMaxSlots) slot_[size_t(from)].dsync = int32_t(ld32(m.data() + 1));
		break;
	case msg::kSaveAt:
		if (m.size() == 5 && !recovering_) {
			const uint32_t at = ld32(m.data() + 1);
			if (at > boundaries_) save_at_.insert(at);
			else if (on_save_missed) on_save_missed(at);
		}
		break;
	case msg::kRecBegin:
	case msg::kRecData:
	case msg::kRecReady:
	case msg::kRecNeed:
		if (cfg_.recover) recovery_message(from, m);
		break;
	default: break;  // lobby traffic and anything unknown
	}
}

void Session::accept_package(int from, const Bytes& m) {
	uint32_t stamp;
	int play;
	Bytes data;
	if (!unpack_package_msg(m, stamp, play, data) || play < 0 || play >= kMaxSlots) return;
	if (!valid_sender(play)) return;  // the original ignores packages of players that are gone (VALID, not WON, none of 0xd0)
	// During a recovery everything a peer sent before its RecReady belongs to the old epoch (per-sender order): ignored.
	if (recovering_ && !(rec_.ready_mask & (1u << (from & 7)))) return;
	Slot& s = slot_[size_t(play)];
	if (s.drop.pending && from == play) return;  // the final stamp of a given-up player is frozen; only relays count now
	if (stamp <= s.last_accepted) {
		++duplicates_;  // a resend: idempotent
		return;
	}
	if (stamp != s.last_accepted + 1) {
		fail("package gap from slot " + std::to_string(play) + ": stamp " + std::to_string(stamp) + " after " + std::to_string(s.last_accepted));
		return;
	}
	if (s.fifo.size() >= kFifoCapacity) {
		fail("package FIFO of slot " + std::to_string(play) + " is full");
		return;
	}
	s.fifo.push_back({stamp, std::move(data)});
	s.last_accepted = stamp;
}

void Session::clear_slot_queue(int slot) {
	slot_[size_t(slot)].fifo.clear();
}

// ---- the tick

StepResult Session::step() {
	StepResult r;
	if (!started_) start();
	pump();
	if (errored_) {
		r.kind = StepKind::Error;
		r.text = error_;
		return r;
	}
	// A peer's DSYNC is acted on at the end of the boundary that reaches its stamp (end_turn), as the original does: every peer then
	// freezes on the same row of check sums, which the recovery's authority vote needs.
	if (in_desync_) {
		if (cfg_.recover) return recovery_step();
		r.kind = StepKind::Desync;
		r.text = desync_.text();
		return r;
	}
	if (ended_) {
		r.kind = StepKind::Ended;
		return r;
	}
	if (!in_tick_) {
		++turn_counter_;
		++ticks_;
		in_tick_ = true;
	}
	bool boundary = false;
	if (turn_counter_ >= turn_length_) {
		r = run_turn();
		if (r.kind != StepKind::Advanced) return r;
		boundary = true;
	}
	in_tick_ = false;
	if (!paused_) {
		h_.step_frame();
		if (h_.frame() % 15 == 0) check_cannon_time();
	}
	r = StepResult();
	r.turn = boundary;
	return r;
}

bool Session::begin_turn(std::string& err) {
	for (Slot& s : slot_) {
		s.has_sums = false;
		s.sums.fill(0);
		s.turn_data = false;
	}
	for (auto it = save_at_.begin(); it != save_at_.end() && *it <= boundaries_;) {  // a request this peer never reached in time
		if (on_save_missed) on_save_missed(*it);
		it = save_at_.erase(it);
	}
	BoundaryInfo bi;
	bi.turn = boundaries_ + 1;
	bi.sent_stamp = local_stamp_ + 1;
	bi.frame = h_.frame();
	bi.turn_length = turn_length_;
	bi.sums_ok = h_.checksums(bi.sums);
	if (!bi.sums_ok) {
		err = "the simulation could not compute its check sums";
		return false;
	}
	++boundaries_;
	last_boundary_ = bi;
	if (local_active()) sim::wire_check_sums(local_pkg_, bi.sums);
	if (on_boundary) on_boundary(bi);
	if (!save_at_.empty() && save_at_.count(boundaries_)) {  // the agreed boundary of a synchronized save: the same state on every peer
		save_at_.erase(boundaries_);
		Bytes snap;
		if (h_.snapshot(snap) && on_save) on_save(snap, h_.frame(), boundaries_);
	}
	send_local_package();
	target_stamp_ = local_stamp_ - 1;
	return true;
}

StepResult Session::run_turn() {
	StepResult r;
	if (phase_ == 0) {
		std::string err;
		if (!begin_turn(err)) {
			fail(err);
			r.kind = StepKind::Error;
			r.text = err;
			return r;
		}
		phase_ = 1;
		next_slot_ = 0;
	}
	if (phase_ == 1) {
		if (!gate_drops(r)) return r;
		phase_ = 2;
	}
	// phase 2: every expected sender's package of stamp s - 1, in slot order
	while (next_slot_ < kMaxSlots) {
		const int p = next_slot_;
		if (!expected(p)) {
			++next_slot_;
			continue;
		}
		Slot& s = slot_[size_t(p)];
		if (s.fifo.empty()) {  // cannot happen: the gate waited for every expected package
			fail("package of slot " + std::to_string(p) + " vanished");
			r.kind = StepKind::Error;
			r.text = error_;
			return r;
		}
		Pkt pk = std::move(s.fifo.front());
		s.fifo.pop_front();
		if (pk.stamp != target_stamp_) {
			const std::string text = "Invalid command package from slot " + std::to_string(p) + ": stamp " + std::to_string(pk.stamp) +
			                         ", expected " + std::to_string(target_stamp_);
			fail(text);
			r.kind = StepKind::Error;
			r.text = text;
			return r;
		}
		s.last_executed = pk.stamp;
		execute_package(p, pk.stamp, pk.data);
		s.hist.push_back(std::move(pk));
		while (s.hist.size() > size_t(kHistoryPackages)) s.hist.pop_front();
		if (errored_) {
			r.kind = StepKind::Error;
			r.text = error_;
			return r;
		}
		++next_slot_;
	}
	stall_slot_ = -1;
	end_turn();
	phase_ = 0;
	if (in_desync_) {
		if (cfg_.recover) return recovery_step();
		r.kind = StepKind::Desync;
		r.text = desync_.text();
		return r;
	}
	return r;
}

// The start of a boundary, before any package runs. Every peer must see the same thing here, so two things happen before the first
// package of the turn is executed (never in the middle of it): the drops whose agreed final stamp is behind this boundary are
// applied (the original does the same at the top of process_turn), and the wait for late packages happens, so that a drop decision
// that arrives while waiting is still applied before any slot runs.
bool Session::gate_drops(StepResult& stall) {
	for (int p = 0; p < kMaxSlots; ++p) {
		DropCtl& d = slot_[size_t(p)].drop;
		if (!d.decided || target_stamp_ <= d.final_stamp) continue;
		// The agreed final stamp is behind us: apply the drop at the start of this boundary, the same one on every peer.
		Bytes cmd;
		sim::wire_drop(cmd, uint8_t(p), uint8_t(d.option));
		int sender = p;
		for (int q = 0; q < kMaxSlots; ++q)
			if (q != p && expected(q)) {
				sender = std::min(p, q);
				break;
			}
		h_.begin_package(sender, 0);
		lockstep_verb(sender, cmd.data(), cmd.size());
		h_.execute(sender, cmd.data(), cmd.size());
		h_.end_package(sender);
		if (on_package) on_package(h_.frame(), sender, 0, cmd);
	}
	for (int p = 0; p < kMaxSlots; ++p) {
		if (!expected(p) || !slot_[size_t(p)].fifo.empty()) continue;
		if (stall_slot_ != p) {
			stall_slot_ = p;
			stall_since_ = t_.now_ms();
		}
		const uint64_t waited = t_.now_ms() - stall_since_;
		if (on_waiting) on_waiting(p, waited);
		if (cfg_.timeout_ms && waited >= cfg_.timeout_ms) report_timeout(p);
		const DropCtl& dc = slot_[size_t(p)].drop;
		if (dc.decided && target_stamp_ <= dc.final_stamp && (need_sent_ == 0 || t_.now_ms() - need_sent_ >= 25)) {
			// The agreed final stamp says the package exists: some peer has it. Ask (idempotent by the stamp rule).
			need_sent_ = t_.now_ms() ? t_.now_ms() : 1;
			Bytes m{msg::kDropNeed, uint8_t(p)};
			put32(m, target_stamp_);
			t_.broadcast(m, false);
		}
		stall.kind = StepKind::Stalled;
		stall.slot = p;
		return false;
	}
	stall_slot_ = -1;
	return true;
}

void Session::execute_package(int slot, uint32_t stamp, const Bytes& data) {
	h_.begin_package(slot, stamp);
	size_t p = 0;
	while (p < data.size()) {
		const size_t n = sim::replay_command_length(data.data() + p, data.size() - p);
		if (!n) {
			fail("malformed package from slot " + std::to_string(slot) + " at byte " + std::to_string(p));
			break;
		}
		lockstep_verb(slot, data.data() + p, n);
		h_.execute(slot, data.data() + p, n);
		p += n;
	}
	h_.end_package(slot);
	if (on_package) on_package(h_.frame(), slot, stamp, data);
}

void Session::lockstep_verb(int slot, const uint8_t* cmd, size_t len) {
	Slot& s = slot_[size_t(slot)];
	switch (cmd[0]) {
	case sim::op::kCheckSums: {  // CommandPackage::process_check_sums: checksums[play] = all
		if (len != 65) break;
		for (int i = 0; i < 16; ++i) s.sums[size_t(i)] = ld32(cmd + 1 + 4 * i);
		s.has_sums = true;
		break;
	}
	case sim::op::kTurnData: {  // TurnControl.last_ping_times[play], turn_flags
		if (len != 11) break;
		const uint16_t ping = ld16(cmd + 1);
		s.turn_data = true;
		s.ping = ping & 0xff7f;
		if (ping & 0x80) h_.view_all(slot);
		break;
	}
	case sim::op::kPause: process_pause(slot, cmd[1]); break;
	case sim::op::kSpeedSet: set_speed(int(int32_t(ld32(cmd + 1))), slot); break;
	case sim::op::kSpeedUp: set_speed(speed_ + 1, slot); break;
	case sim::op::kSpeedDown: set_speed(speed_ - 1, slot); break;
	case sim::op::kCannonTime: start_cannon_time(slot, cmd[1]); break;
	case sim::op::kResign:
	case sim::op::kQuit: {  // Player::resign(0) / quit(1): flag 0x40, the sender leaves the expected set
		int play = int(int32_t(ld32(cmd + 1)));
		if (play < 0 || play >= kMaxSlots) play = slot;
		slot_[size_t(play)].flags |= kPlayerResigned;
		clear_slot_queue(play);
		if (cmd[0] == sim::op::kQuit && play == cfg_.local_slot) ended_ = true;
		if (on_resign) on_resign(play);
		break;
	}
	case sim::op::kUngracefulDrop: apply_drop(cmd[1], cmd[2]); break;
	default: break;
	}
}

void Session::process_pause(int slot, int state) {
	const bool before = paused_;
	if (state) {
		if (paused_) return;
		Slot& s = slot_[size_t(slot)];
		if (cfg_.max_pauses > 0 && s.pauses >= cfg_.max_pauses) return;  // over quota: feedback to the issuer only
		++s.pauses;
		paused_ = true;
	} else {
		paused_ = false;
	}
	if (paused_ != before && on_pause) on_pause(slot, paused_);
}

void Session::set_speed(int s, int slot) {
	if (s < 0 || s > 4) return;
	const bool changed = s != speed_;
	speed_ = s;
	if (cannon_) saved_speed_ = s;
	if (changed && on_speed) on_speed(slot, s);
}

void Session::start_cannon_time(int slot, int state) {
	if (!state || cannon_ || !h_.can_cannon_time(slot)) return;
	cannon_ = true;
	saved_speed_ = speed_;
	speed_ = 0;
	cannon_stamp_ = h_.frame();
}

void Session::check_cannon_time() {
	if (cannon_ && h_.frame() - cannon_stamp_ > 0x4a) {
		cannon_ = false;
		speed_ = saved_speed_;
	}
}

void Session::apply_drop(int slot, int option) {
	if (slot < 0 || slot >= kMaxSlots) return;
	Slot& s = slot_[size_t(slot)];
	s.drop = DropCtl();
	if (option == kDropReplaceWithAi) s.flags &= ~(kPlayerHuman | kPlayerDropped);
	else s.flags |= kPlayerDropped;
	s.connected = false;
	clear_slot_queue(slot);
	if (slot == cfg_.local_slot && option != kDropReplaceWithAi) ended_ = true;
	if (on_drop) on_drop(slot, option, boundaries_);
}

void Session::compute_next_turn() {
	// TurnControl::compute_next_turn_multi: T = clamp(max(2 x max ping of the TURN_DATA executed this turn, min_turn_time), <= 1000)
	int max_ping = 0;
	bool any = false;
	for (const Slot& s : slot_)
		if (s.turn_data) {
			any = true;
			max_ping = std::max(max_ping, s.ping);
		}
	if (!any) {
		turn_length_ = 1;
		return;
	}
	int T = std::max(2 * max_ping, min_turn_time_);
	T = std::min(T, 1000);
	const int per = kTimings[speed_];
	turn_length_ = std::max(1, (T + per - 1) / per);
}

void Session::end_turn() {
	// CheckSums::check_all / CommandPackage::end_process: all non-zero sums agree and no peer reported an out-of-sync
	uint32_t ref = 0;
	bool mismatch = false;
	int reporter = -1;
	for (const Slot& s : slot_) {
		if (!s.has_sums || s.sums[15] == 0) continue;
		if (ref == 0) ref = s.sums[15];
		else if (s.sums[15] != ref) mismatch = true;
	}
	for (int q = 0; q < kMaxSlots; ++q) {
		const Slot& s = slot_[size_t(q)];
		if (q != cfg_.local_slot && s.dsync >= 0 && s.dsync <= int64_t(local_stamp_)) {
			mismatch = true;
			reporter = q;
		}
	}
	if (mismatch) {
		++fail_count_;
		if (fail_count_ >= cfg_.checksum_failure_threshold) enter_desync(reporter >= 0, reporter);
		if (in_desync_) return;
	}
	compute_next_turn();
	turn_counter_ = 0;
}

void Session::enter_desync(bool peer_reported, int reporter) {
	if (in_desync_) return;
	in_desync_ = true;
	desync_ = DesyncReport();
	desync_.stamp = target_stamp_ ? target_stamp_ : local_stamp_;
	desync_.frame = h_.frame();
	desync_.peer_reported = peer_reported;
	desync_.reporter = reporter;
	int first = -1, second = -1;
	for (int p = 0; p < kMaxSlots; ++p) {
		const Slot& s = slot_[size_t(p)];
		if (!s.has_sums) continue;
		desync_.present[size_t(p)] = true;
		desync_.sums[size_t(p)] = s.sums;
		if (first < 0) first = p;
		else if (second < 0 && s.sums[15] != slot_[size_t(first)].sums[15]) second = p;
	}
	if (first >= 0 && second >= 0)
		for (int k = 0; k < 15; ++k)
			if (slot_[size_t(first)].sums[size_t(k)] != slot_[size_t(second)].sums[size_t(k)]) {
				desync_.first_component = k;
				break;
			}
	if (cfg_.recover) recovering_ = true;
	if (!peer_reported) {
		Bytes m{msg::kDsync};
		put32(m, local_stamp_);
		t_.broadcast(m, false);
	}
}

// ---------------------------------------------------------------- drops (multiplayer-spec section 3, decision 8.2.2)

std::string Session::describe_drop(int p) const {
	const DropCtl& d = slot_[size_t(p)].drop;
	char b[512];
	std::snprintf(b, sizeof b, "flags=%x mine=%d pending=%d decided=%d relayed=%d sent=%d option=%d final=%u states=[", d.flag_mask, int(d.flagged_by_me), int(d.pending),
	              int(d.decided), int(d.relayed), int(d.decision_sent), d.option, d.final_stamp);
	std::string s = b;
	for (int q = 0; q < kMaxSlots; ++q) s += (q ? "," : "") + std::to_string(d.state[size_t(q)]);
	s += "] votes=[";
	for (int q = 0; q < kMaxSlots; ++q) s += (q ? "," : "") + std::to_string(d.vote[size_t(q)]);
	return s + "]";
}

void Session::report_timeout(int p) {
	if (p < 0 || p >= kMaxSlots || p == cfg_.local_slot || !expected(p)) return;
	DropCtl& d = slot_[size_t(p)].drop;
	if (d.flagged_by_me) return;
	d.flagged_by_me = true;
	d.flag_mask |= 1u << cfg_.local_slot;
	Bytes m{msg::kDropFlag};
	put32(m, uint32_t(p));
	t_.broadcast(m, false);
	for (int q = 0; q < kMaxSlots; ++q) drop_check_consensus(q);
}

void Session::on_drop_flag(int from, int p) {
	if (p < 0 || p >= kMaxSlots || from < 0 || from >= kMaxSlots) return;
	slot_[size_t(p)].drop.flag_mask |= 1u << from;
	for (int q = 0; q < kMaxSlots; ++q) drop_check_consensus(q);
}

void Session::drop_check_consensus(int p) {
	DropCtl& d = slot_[size_t(p)].drop;
	if (d.pending || d.decided || !d.flagged_by_me || !expected(p)) return;
	uint32_t voters = 0;
	for (int q = 0; q < kMaxSlots; ++q) {
		const Slot& sq = slot_[size_t(q)];
		if (q != p && expected(q) && !sq.drop.pending && !sq.drop.decided) voters |= 1u << q;
	}
	if ((d.flag_mask & voters) != voters) return;
	d.pending = true;
	d.remaining = voters;
	Bytes st{msg::kDropState, uint8_t(p)};
	put32(st, slot_[size_t(p)].last_accepted);
	t_.broadcast(st, true);
	Bytes vt{msg::kDropVote, uint8_t(p), uint8_t(int8_t(cfg_.drop_default))};
	t_.broadcast(vt, true);
	// A slot that just became pending is no voter in the other agreements any more.
	for (int q = 0; q < kMaxSlots; ++q)
		if (q != p) drop_progress(q);
}

int Session::arbiter(int p) const {
	for (int q = 0; q < kMaxSlots; ++q) {
		const Slot& sq = slot_[size_t(q)];
		if (q != p && expected(q) && !sq.drop.pending && !sq.drop.decided) return q;
	}
	return -1;
}

void Session::on_drop_state(int from, int p, uint32_t stamp) {
	if (p >= kMaxSlots || from >= kMaxSlots) return;
	slot_[size_t(p)].drop.state[size_t(from)] = stamp;
	drop_progress(p);
}

void Session::on_drop_vote(int from, int p, int option) {
	if (p >= kMaxSlots || from >= kMaxSlots) return;
	slot_[size_t(p)].drop.vote[size_t(from)] = option;
	drop_progress(p);
}

void Session::drop_progress(int p) {
	Slot& sp = slot_[size_t(p)];
	DropCtl& d = sp.drop;
	if (!d.pending) return;
	uint32_t voters = 0;
	for (int q = 0; q < kMaxSlots; ++q) {
		const Slot& sq = slot_[size_t(q)];
		if (q != p && expected(q) && !sq.drop.pending && !sq.drop.decided) voters |= 1u << q;
	}
	int64_t final_stamp = 0;
	for (int q = 0; q < kMaxSlots; ++q) {
		if (!(voters & (1u << q))) continue;
		if (d.state[size_t(q)] < 0) return;  // not everyone has reported yet
		final_stamp = std::max(final_stamp, d.state[size_t(q)]);
	}
	d.final_stamp = uint32_t(final_stamp);
	if (!d.relayed) {
		// PackageFifo::resend: whoever holds packages of p that another peer lacks sends them (idempotent by the stamp rule).
		d.relayed = true;
		for (int q = 0; q < kMaxSlots; ++q) {
			if (!(voters & (1u << q)) || q == cfg_.local_slot) continue;
			for (uint32_t st = uint32_t(d.state[size_t(q)]) + 1; st <= sp.last_accepted; ++st) {
				const Pkt* found = nullptr;
				for (const Pkt& k : sp.hist)
					if (k.stamp == st) found = &k;
				for (const Pkt& k : sp.fifo)
					if (k.stamp == st) found = &k;
				if (found) t_.send(q, pack_package_msg(st, p, found->data));
			}
		}
	}
	bool all_votes = true;
	for (int q = 0; q < kMaxSlots; ++q)
		if ((voters & (1u << q)) && d.vote[size_t(q)] < 0) all_votes = false;
	if (d.decided || !all_votes || arbiter(p) != cfg_.local_slot || d.decision_sent) return;
	d.decision_sent = true;
	int count[6] = {};
	for (int q = 0; q < kMaxSlots; ++q)
		if (voters & (1u << q)) {
			int v = d.vote[size_t(q)];
			if (v < 0 || v > 3) v = kDropPlayer;  // SAVE_AND_EXIT / EXIT are local choices: the slot is dropped
			++count[v];
		}
	int best = kDropReplaceWithAi;  // ties go to REPLACE_WITH_AI (DropControl::check_vote_time_out)
	for (int v = 0; v < 4; ++v)
		if (count[v] > count[best]) best = v;
	Bytes m{msg::kDropDecision, uint8_t(p), uint8_t(int8_t(best))};
	put32(m, d.final_stamp);
	t_.broadcast(m, true);
}

void Session::on_drop_need(int from, int p, uint32_t stamp) {
	if (p >= kMaxSlots || from >= kMaxSlots) return;
	const Slot& sp = slot_[size_t(p)];
	for (const Pkt& k : sp.hist)
		if (k.stamp == stamp) {
			t_.send(from, pack_package_msg(stamp, p, k.data));
			return;
		}
	for (const Pkt& k : sp.fifo)
		if (k.stamp == stamp) {
			t_.send(from, pack_package_msg(stamp, p, k.data));
			return;
		}
}

void Session::on_drop_decision(int p, int option, uint32_t final_stamp) {
	if (p >= kMaxSlots) return;
	DropCtl& d = slot_[size_t(p)].drop;
	if (d.decided) return;
	d.decided = true;
	d.pending = true;
	d.option = option;
	d.final_stamp = final_stamp;
}

}  // namespace ron::net
