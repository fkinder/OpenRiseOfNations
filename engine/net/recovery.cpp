// Out-of-sync recovery of the lockstep layer (docs/multiplayer-spec.md sections 5.2 and 8.2.4). The original's flow (CommandManager::
// recover_from_oos @93e9e0): the authority saves the game, SyncFile::send's the file to everybody, every peer reloads it, and the
// command manager restarts (stamps from 1, the frame goes on). The port, step by step:
//   1. A peer freezes at the end of the boundary whose check sums disagree (Session::end_turn). All peers freeze on the same row of sums.
//   2. The authority is chosen from that row alone (pick_authority): the host when two peers took part, else the peer whose sum agrees
//      with the most others (lowest slot wins ties). No message is needed to agree on it.
//   3. The authority takes SimHooks::snapshot, zlib-compresses it and sends RecBegin (epoch, sizes, CRC-32 of the raw bytes, chunk
//      count) and RecData chunks to every other peer (observers too). Everyone, the authority included, restores the same bytes.
//   4. Each peer broadcasts RecReady(epoch) after its restore; everything a peer sent before its RecReady is old-epoch traffic and is
//      dropped (per-sender order), which is why the new stamps cannot be mixed up with the old ones.
//   5. When every peer's RecReady is in, the session restarts: stamp 1 is sent, turn_length starts over, step() returns Recovered once.
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "net/lockstep.hpp"

namespace ron::net {

namespace {

constexpr int kTimings[5] = {200, 125, 67, 50, 1};

inline uint32_t ld32(const uint8_t* p) {
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}
inline void put32(Bytes& o, uint32_t v) {
	for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}

}  // namespace

int Session::pick_authority(const DesyncReport& r, const std::array<bool, kMaxSlots>& alive) {
	std::vector<int> cand;
	for (int p = 0; p < kMaxSlots; ++p)
		if (r.present[size_t(p)] && alive[size_t(p)]) cand.push_back(p);
	if (cand.empty()) {
		for (int p = 0; p < kMaxSlots; ++p)
			if (alive[size_t(p)]) return p;
		return -1;
	}
	if (cand.size() <= 2) return cand.front();  // "authority = host if <= 2 peers": the host is the lowest slot
	int best = -1, best_n = -1;
	for (int c : cand) {
		int n = 0;
		for (int d : cand)
			if (d != c && r.sums[size_t(d)][15] == r.sums[size_t(c)][15]) ++n;
		if (n > best_n) best = c, best_n = n;  // ascending order: the lowest slot wins a tie
	}
	return best;
}

// ---------------------------------------------------------------- the flow

StepResult Session::recovery_step() {
	StepResult r;
	if (rec_.failed) {
		fail(rec_.error);
		r.kind = StepKind::Error;
		r.text = error_;
		return r;
	}
	const std::vector<int> peers = t_.peers();
	auto is_alive = [&](int p) { return p == cfg_.local_slot || std::find(peers.begin(), peers.end(), p) != peers.end(); };
	if (!rec_.begun) recovery_begin();
	// The authority died before the snapshot reached us. First ask the other peers: one that loaded it already serves it (RecNeed). Only if
	// nobody answers within the grace period the next candidate of the same report takes a fresh snapshot (nobody loaded the old one then).
	if (!rec_.snapshot_taken && rec_.authority != cfg_.local_slot && !is_alive(rec_.authority)) {
		if (rec_.need_at == 0) {
			rec_.need_at = std::max<uint64_t>(1, t_.now_ms());
			Bytes m{msg::kRecNeed};
			put32(m, rec_.epoch);
			t_.broadcast(m, false);
		} else if (t_.now_ms() - rec_.need_at >= 300) {
			std::array<bool, kMaxSlots> alive{};
			for (int p = 0; p < kMaxSlots; ++p) alive[size_t(p)] = is_alive(p) && p != rec_.authority;
			rec_.authority = pick_authority(desync_, alive);
			rec_.comp.clear();
			rec_.have.clear();
			rec_.chunks = rec_.got = 0;
			rec_.need_at = 0;
		}
	}
	if (rec_.authority == cfg_.local_slot && !rec_.snapshot_taken) recovery_send_snapshot();
	else if (!rec_.loaded) recovery_try_assemble();
	if (rec_.failed) {
		fail(rec_.error);
		r.kind = StepKind::Error;
		r.text = error_;
		return r;
	}
	if (rec_.loaded && !rec_.ready_sent) {
		rec_.ready_sent = true;
		Bytes m{msg::kRecReady};
		put32(m, rec_.epoch);
		m.push_back(1);
		t_.broadcast(m, true);  // the copy to ourselves is the marker that our old-epoch traffic is drained
	}
	if (rec_.ready_sent) {
		uint32_t need = 1u << cfg_.local_slot;
		for (int p : peers) need |= 1u << p;
		if ((rec_.ready_mask & need) == need) {
			recovery_finish();
			r.kind = StepKind::Recovered;
			return r;
		}
	}
	r.kind = StepKind::Recovering;
	char b[160];
	std::snprintf(b, sizeof b, "recovering from slot %d's snapshot (%u of %u chunks)", rec_.authority, rec_.got, rec_.chunks);
	r.text = b;
	return r;
}

void Session::recovery_begin() {
	rec_.begun = true;
	rec_.epoch = epoch_ + 1;
	std::array<bool, kMaxSlots> alive{};
	alive[size_t(cfg_.local_slot)] = true;
	for (int p : t_.peers()) alive[size_t(p)] = true;
	// A snapshot that arrived before this peer froze names its sender as the authority already.
	const int voted = pick_authority(desync_, alive);
	if (rec_.authority < 0) rec_.authority = voted;
	if (on_desync) on_desync(desync_, rec_.authority);
}

bool Session::recovery_pack(const Bytes& raw) {
	uLongf cap = compressBound(uLong(raw.size()));
	Bytes comp(cap);
	if (compress2(comp.data(), &cap, raw.data(), uLong(raw.size()), 3) != Z_OK) return false;
	comp.resize(cap);
	rec_.out_chunk = uint32_t(std::max<size_t>(256, cfg_.recover_chunk));
	rec_.out_chunks = uint32_t(std::max<size_t>(1, (comp.size() + rec_.out_chunk - 1) / rec_.out_chunk));
	rec_.out_crc = uint32_t(crc32(0, raw.data(), uInt(raw.size())));
	rec_.out_comp = std::move(comp);
	rec_.raw_size = uint32_t(raw.size());
	rec_.comp_size = uint32_t(rec_.out_comp.size());
	return true;
}

void Session::recovery_send_to(int peer) {
	Bytes begin{msg::kRecBegin};
	put32(begin, rec_.epoch);
	put32(begin, rec_.raw_size);
	put32(begin, rec_.comp_size);
	put32(begin, rec_.out_crc);
	put32(begin, rec_.out_chunks);
	put32(begin, uint32_t(rec_.frame));
	put32(begin, rec_.out_chunk);
	t_.send(peer, begin);
	for (uint32_t i = 0; i < rec_.out_chunks; ++i) {
		Bytes d{msg::kRecData};
		put32(d, rec_.epoch);
		put32(d, i);
		const size_t a = size_t(i) * rec_.out_chunk, e = std::min(rec_.out_comp.size(), a + rec_.out_chunk);
		d.insert(d.end(), rec_.out_comp.begin() + std::ptrdiff_t(a), rec_.out_comp.begin() + std::ptrdiff_t(e));
		t_.send(peer, d);
	}
}

void Session::recovery_send_snapshot() {
	Bytes raw;
	if (!h_.snapshot(raw)) {
		rec_.failed = true;
		rec_.error = "the simulation could not take a snapshot";
		return;
	}
	rec_.frame = h_.frame();
	if (!recovery_pack(raw)) {
		rec_.failed = true;
		rec_.error = "cannot compress the snapshot";
		return;
	}
	for (int p : t_.peers()) recovery_send_to(p);
	rec_.snapshot_taken = true;
	rec_.chunks = rec_.out_chunks;
	rec_.got = rec_.chunks;
	recovery_apply(raw);  // the authority reloads its own snapshot too: all instances restart from the same bytes
}

void Session::recovery_message(int from, const Bytes& m) {
	if (from < 0 || from >= kMaxSlots || m.size() < 5) return;
	const uint32_t epoch = ld32(m.data() + 1);
	if (epoch != epoch_ + 1) return;  // an old or a future recovery
	switch (m[0]) {
	case msg::kRecBegin: {
		if (m.size() != 29 || rec_.snapshot_taken || rec_.loaded) return;
		rec_.authority = from;
		rec_.raw_size = ld32(m.data() + 5);
		rec_.comp_size = ld32(m.data() + 9);
		rec_.crc = ld32(m.data() + 13);
		rec_.chunks = ld32(m.data() + 17);
		rec_.frame = int32_t(ld32(m.data() + 21));
		rec_.chunk_size = ld32(m.data() + 25);
		if (rec_.chunks == 0 || rec_.chunks > (1u << 20) || rec_.comp_size > (1u << 30) || rec_.raw_size > (1u << 30) || rec_.chunk_size == 0 ||
		    uint64_t(rec_.chunks) * rec_.chunk_size < rec_.comp_size) {
			rec_.failed = true;
			rec_.error = "a malformed snapshot header from slot " + std::to_string(from);
			return;
		}
		rec_.comp.assign(rec_.comp_size, 0);
		rec_.have.assign(rec_.chunks, false);
		rec_.got = 0;
		break;
	}
	case msg::kRecData: {
		if (m.size() < 9 || from != rec_.authority || rec_.have.empty()) return;
		const uint32_t i = ld32(m.data() + 5);
		if (i >= rec_.chunks || rec_.have[i]) return;
		const size_t n = m.size() - 9, at = size_t(i) * rec_.chunk_size;
		if (at + n > rec_.comp.size()) {
			rec_.failed = true;
			rec_.error = "snapshot chunk out of range";
			return;
		}
		std::memcpy(rec_.comp.data() + at, m.data() + 9, n);
		rec_.have[i] = true;
		++rec_.got;
		break;
	}
	case msg::kRecNeed:
		// a peer lacks the snapshot (the authority is gone): whoever has it loaded serves it
		if (rec_.loaded && !rec_.raw.empty()) {
			if (rec_.out_comp.empty() && !recovery_pack(rec_.raw)) return;
			recovery_send_to(from);
		} else {
			rec_.need_mask |= 1u << from;  // answered as soon as this peer has loaded it
		}
		break;
	case msg::kRecReady:
		if (m.size() != 6) return;
		if (!m[5]) {
			rec_.failed = true;
			rec_.error = "slot " + std::to_string(from) + " could not load the snapshot";
			return;
		}
		rec_.ready_mask |= 1u << from;
		break;
	default: break;
	}
}

void Session::recovery_try_assemble() {
	if (rec_.loaded || rec_.chunks == 0 || rec_.got < rec_.chunks) return;
	Bytes raw(rec_.raw_size);
	uLongf n = uLongf(raw.size());
	if (uncompress(raw.data(), &n, rec_.comp.data(), uLong(rec_.comp.size())) != Z_OK || n != raw.size() ||
	    uint32_t(crc32(0, raw.data(), uInt(raw.size()))) != rec_.crc) {
		rec_.failed = true;
		rec_.error = "the snapshot from slot " + std::to_string(rec_.authority) + " is corrupt (size or CRC mismatch)";
		return;
	}
	rec_.snapshot_taken = true;
	recovery_apply(raw);
}

void Session::recovery_apply(const Bytes& raw) {
	std::string err;
	if (!h_.restore(raw, err)) {
		rec_.failed = true;
		rec_.error = "cannot load the snapshot: " + err;
		Bytes m{msg::kRecReady};
		put32(m, rec_.epoch);
		m.push_back(0);
		t_.broadcast(m, false);
		return;
	}
	recovery_reset_epoch_state();
	rec_.loaded = true;
	rec_.raw = raw;
	rec_.frame = h_.frame();
	if (rec_.need_mask) {
		if (rec_.out_comp.empty()) recovery_pack(rec_.raw);
		for (int p = 0; p < kMaxSlots; ++p)
			if ((rec_.need_mask & (1u << p)) && p != cfg_.local_slot) recovery_send_to(p);
		rec_.need_mask = 0;
	}
	if (on_recover) on_recover(raw, h_.frame(), rec_.epoch);
}

// Everything counted per stamp starts over (CommandManager::init + TurnControl::init after the reload).
void Session::recovery_reset_epoch_state() {
	for (Slot& s : slot_) {
		s.fifo.clear();
		s.hist.clear();
		s.last_accepted = s.last_executed = 0;
		s.dsync = -1;
		s.drop = DropCtl();
		s.has_sums = false;
		s.sums.fill(0);
		s.turn_data = false;
	}
	local_pkg_.clear();
	local_stamp_ = 0;
}

void Session::recovery_finish() {
	epoch_ = rec_.epoch;
	last_rec_bytes_ = rec_.raw.size();
	recovering_ = false;
	in_desync_ = false;
	started_ = false;
	in_tick_ = false;
	phase_ = 0;
	next_slot_ = 0;
	target_stamp_ = 0;
	turn_counter_ = 0;
	fail_count_ = 0;
	stall_slot_ = -1;
	need_sent_ = 0;
	turn_length_ = std::max(1, 200 / kTimings[speed_]);
	rec_ = Recovery();
	// A peer that died during the recovery is given up on the usual way, now that the stamps mean something again.
	const std::vector<int> peers = t_.peers();
	for (int p = 0; p < kMaxSlots; ++p)
		if (p != cfg_.local_slot && expected(p) && std::find(peers.begin(), peers.end(), p) == peers.end()) report_timeout(p);
}

}  // namespace ron::net
