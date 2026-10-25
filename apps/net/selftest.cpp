// ron-net selftest: the acceptance criteria of docs/multiplayer-spec.md 8.4 that need no original and no engine, on a toy
// simulation (a hash of everything it is told) over the loopback transport: agreement under random delivery delay, stall,
// stamp gap / duplicate, pause, speed change, resign, drops (4 options, random kill points) and out-of-sync detection.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <thread>
#include <string>

#include <unistd.h>

#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>

#include "commands.hpp"
#include "net/discovery.hpp"
#include "net/lobby.hpp"
#include "net/seat_transport.hpp"
#include "net/tcp.hpp"
#include "common.hpp"
#include "loopback.hpp"
#include "sim/newgame.hpp"
#include "sim/replay.hpp"
#include "sim/save.hpp"

namespace ronnet {

namespace {

uint64_t mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	return h;
}

struct Rng {
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed * 2654435761u + 88172645463325252ull) {}
	uint32_t next() {
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		return uint32_t(s >> 11);
	}
	uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

// A simulation whose whole state is a hash: any difference in what it executes, in which order, or when, changes it.
class ToySim : public net::SimHooks {
public:
	int32_t frame() const override { return f; }
	void step_frame() override {
		state = mix(state, uint64_t(f) + 1);
		++f;
	}
	bool checksums(uint32_t out[16]) override {
		uint32_t sum = 0;
		for (int i = 0; i < 15; ++i) {
			out[i] = uint32_t(mix(state, uint64_t(i)) >> 16);
			if (out[i] == 0) out[i] = 1;
			sum += out[i];
		}
		out[15] = sum ? sum : 1;
		return true;
	}
	void begin_package(int slot, uint32_t stamp) override { state = mix(state, 0x1000 + uint64_t(slot) * 1000003 + stamp); }
	void execute(int slot, const uint8_t* cmd, size_t len) override {
		if (cmd[0] == sim::op::kCheckSums || cmd[0] == sim::op::kCamera) return;  // the layer's own bookkeeping
		uint64_t h = mix(state, uint64_t(slot));
		for (size_t i = 0; i < len; ++i) h = mix(h, cmd[i]);
		state = h;
		++commands;
	}
	// OOS recovery: the whole state as bytes (plus `pad_bytes` of noise so that a snapshot needs many chunks)
	bool snapshot(net::Bytes& out) override {
		++snaps;
		out.clear();
		auto put = [&](uint64_t v) {
			for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
		};
		put(uint64_t(uint32_t(f)));
		put(state);
		put(commands);
		Rng r(77);
		for (size_t i = 0; i < pad_bytes; ++i) out.push_back(uint8_t(r.next()));
		return true;
	}
	bool restore(const net::Bytes& b, std::string& err) override {
		if (b.size() < 24 + pad_bytes) {
			err = "short snapshot";
			return false;
		}
		auto get = [&](size_t at) {
			uint64_t v = 0;
			for (int i = 0; i < 8; ++i) v |= uint64_t(b[at + size_t(i)]) << (8 * i);
			return v;
		};
		f = int32_t(uint32_t(get(0)));
		state = get(8);
		commands = get(16);
		++restores;
		return true;
	}
	int32_t f = 0;
	uint64_t state = 1;
	uint64_t commands = 0;
	size_t pad_bytes = 0;
	int snaps = 0, restores = 0;
};

struct Node {
	std::unique_ptr<ToySim> sim;
	std::unique_ptr<net::Session> s;
	std::vector<net::BoundaryInfo> bis;
	net::StepResult last;
	uint32_t fed = 0, epoch = 0;
	std::vector<std::pair<int, uint32_t>> drops;  // (slot, boundary) of each applied drop
	Rng rng{1};
	int resigns = 0;
	bool dead = false;
};

struct Cluster {
	Cluster(int n, uint32_t seed, int dlo, int dhi, std::function<void(net::SessionConfig&)> tweak = nullptr, int humans = -1)
		: hub(n, seed, dlo, dhi) {
		if (humans < 0) humans = n;
		for (int i = 0; i < n; ++i) {
			Node nd;
			nd.sim = std::make_unique<ToySim>();
			nd.rng = Rng(uint64_t(seed) * 977 + uint64_t(i));
			net::SessionConfig c;
			c.local_slot = i;
			c.seed = seed;
			c.speed = 2;
			c.max_pauses = 0;
			for (int p = 0; p < n; ++p) c.slot_flags[size_t(p)] = net::kPlayerValid | (p < humans ? net::kPlayerHuman : 0u);
			if (tweak) tweak(c);
			nd.s = std::make_unique<net::Session>(hub.endpoint(i), *nd.sim, c);
			nodes.push_back(std::move(nd));
		}
		for (size_t i = 0; i < nodes.size(); ++i) {
			Node& nd = nodes[i];
			nd.s->on_boundary = [&nd](const net::BoundaryInfo& b) { nd.bis.push_back(b); };
			nd.s->on_drop = [&nd](int slot, int, uint32_t boundary) { nd.drops.emplace_back(slot, boundary); };
			nd.s->on_resign = [&nd](int) { ++nd.resigns; };
		}
	}
	// random commands for the next stamp
	void feed(Node& nd, bool random_cmds) {
		if (nd.epoch != nd.s->epoch()) nd.epoch = nd.s->epoch(), nd.fed = 0;  // a recovery restarted the stamps
		while (nd.fed < nd.s->local_stamp() + 1) {
			++nd.fed;
			if (!random_cmds) continue;
			const int k = int(nd.rng.below(3));
			for (int c = 0; c < k; ++c) {
				net::Bytes b;
				if (nd.rng.below(2)) {
					b = {12};  // HALT
				} else {
					b = {24, uint8_t(nd.rng.next()), 0, 0, 0, 1, 0, 0, 0};  // QUEUE_UP type, 1
				}
				nd.s->submit(b);
			}
		}
	}
	// one round: every live node steps once; returns how many advanced
	int round(bool random_cmds = true) {
		int adv = 0;
		for (Node& nd : nodes) {
			if (nd.dead) continue;
			feed(nd, random_cmds);
			nd.last = nd.s->step();
			adv += nd.last.kind == net::StepKind::Advanced;
		}
		hub.advance(1);
		return adv;
	}
	// until every live node has run `boundaries` turn boundaries (or `max_rounds`)
	bool run_to(uint32_t boundaries, int max_rounds = 200000, bool random_cmds = true) {
		for (int r = 0; r < max_rounds; ++r) {
			bool all = true;
			for (const Node& nd : nodes)
				if (!nd.dead && nd.last.kind != net::StepKind::Desync && nd.last.kind != net::StepKind::Error && nd.s->boundaries() < boundaries) all = false;
			if (all) return true;
			round(random_cmds);
		}
		return false;
	}
	// are the sums of the first `n` boundaries equal on all live nodes that reached them?
	bool agree(std::string& why, size_t from = 0) const {
		size_t n = SIZE_MAX;
		for (const Node& nd : nodes)
			if (!nd.dead) n = std::min(n, nd.bis.size());
		const Node* ref = nullptr;
		for (const Node& nd : nodes)
			if (!nd.dead) {
				ref = &nd;
				break;
			}
		if (!ref) return true;
		for (size_t j = from; j < n; ++j)
			for (const Node& nd : nodes) {
				if (nd.dead) continue;
				if (std::memcmp(nd.bis[j].sums, ref->bis[j].sums, sizeof nd.bis[j].sums) != 0 || nd.bis[j].frame != ref->bis[j].frame) {
					why = "boundary " + std::to_string(j + 1) + " differs";
					return false;
				}
			}
		return true;
	}
	net::LoopbackHub hub;
	std::vector<Node> nodes;
};

struct Tally {
	int pass = 0, fail = 0;
	void check(bool ok, const std::string& name, const std::string& detail = std::string()) {
		std::printf("  %s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
		(ok ? pass : fail)++;
	}
};

void test_agreement(Tally& t, uint32_t seed) {
	for (int n : {2, 3, 4, 8}) {
		Cluster c(n, seed + uint32_t(n), 0, 9);
		const bool ran = c.run_to(300);
		std::string why;
		const bool ag = c.agree(why);
		bool tl = true;
		for (const Node& nd : c.nodes) tl = tl && (n <= 5 ? nd.s->turn_length() == 6 : nd.s->turn_length() == (n == 8 ? 9 : 8));
		t.check(ran && ag && tl, "agreement, " + std::to_string(n) + " peers, delay 0..9 ms",
		        ag ? "300 turns equal; turn_length " + std::to_string(c.nodes[0].s->turn_length()) : why);
	}
}

void test_stall(Tally& t, uint32_t seed) {
	Cluster c(4, seed, 0, 2);
	c.run_to(40);
	// peer 1's packages stop reaching 0, 2 and 3
	for (int to : {0, 2, 3}) c.hub.hold(1, to, true);
	for (int r = 0; r < 100; ++r) c.round();
	const int32_t f0 = c.nodes[0].sim->f, f2 = c.nodes[2].sim->f, f3 = c.nodes[3].sim->f;
	const uint32_t b0 = c.nodes[0].s->boundaries();
	for (int r = 0; r < 300; ++r) c.round();
	const bool frozen = c.nodes[0].sim->f == f0 && c.nodes[2].sim->f == f2 && c.nodes[3].sim->f == f3 && c.nodes[0].s->boundaries() == b0 &&
	                    c.nodes[0].last.kind == net::StepKind::Stalled && c.nodes[0].last.slot == 1;
	t.check(frozen, "stall: withholding one package freezes the others at the same boundary", "stalled on slot " + std::to_string(c.nodes[0].last.slot));
	std::string why;
	t.check(c.agree(why), "stall: no divergence while stalled", why);
	for (int to : {0, 2, 3}) c.hub.hold(1, to, false);
	const bool resumed = c.run_to(b0 + 60);
	t.check(resumed && c.agree(why), "stall: resumes when the package is delivered, sums equal", why);
}

void test_protocol(Tally& t, uint32_t seed) {
	Cluster c(2, seed, 0, 1);
	c.run_to(20);
	const uint64_t dup0 = c.nodes[1].s->duplicates();
	// a duplicate of an accepted stamp is ignored
	const net::Bytes dup = net::pack_package_msg(c.nodes[0].s->last_accepted(0), 0, net::Bytes{12});
	c.hub.endpoint(0).send(1, dup);
	c.run_to(30);
	std::string why;
	t.check(c.nodes[1].s->duplicates() == dup0 + 1 && c.nodes[1].last.kind != net::StepKind::Error && c.agree(why), "protocol: a duplicate stamp is ignored");
	// a gap is a protocol error
	const net::Bytes gap = net::pack_package_msg(c.nodes[1].s->last_accepted(0) + 5, 0, net::Bytes{12});
	c.hub.endpoint(0).send(1, gap);
	for (int r = 0; r < 20; ++r) c.round();
	t.check(c.nodes[1].last.kind == net::StepKind::Error, "protocol: a stamp gap is rejected", c.nodes[1].last.text);
}

void test_pause_speed(Tally& t, uint32_t seed) {
	Cluster c(3, seed, 0, 3);
	c.run_to(30);
	net::Bytes pause;
	sim::wire_pause(pause, 1);
	c.nodes[1].s->submit(pause);
	c.run_to(40);
	const int32_t f = c.nodes[0].sim->f;
	const uint32_t stamp = c.nodes[0].s->local_stamp();
	c.run_to(c.nodes[0].s->boundaries() + 25);
	bool ok = c.nodes[0].s->paused() && c.nodes[0].sim->f == f && c.nodes[2].sim->f == f && c.nodes[0].s->local_stamp() > stamp + 20;
	t.check(ok, "pause: the frame stands still, stamps keep going", "frame " + std::to_string(f) + ", stamps " + std::to_string(stamp) + " -> " + std::to_string(c.nodes[0].s->local_stamp()));
	net::Bytes unpause;
	sim::wire_pause(unpause, 0);
	c.nodes[2].s->submit(unpause);
	c.run_to(c.nodes[0].s->boundaries() + 20);
	std::string why;
	t.check(!c.nodes[0].s->paused() && c.nodes[0].sim->f > f && c.agree(why), "pause: any player can unpause, sums equal", why);
	// speed: SPEED_SET 4 (1 ms per frame): turn_length = ceil(400 / 1) = 400 frames; SPEED_SET 0 (200 ms): 2 frames; SPEED_UP/DOWN step by one
	auto set_speed = [&](int s) {
		net::Bytes b{sim::op::kSpeedSet};
		for (int i = 0; i < 4; ++i) b.push_back(uint8_t(uint32_t(s) >> (8 * i)));
		c.nodes[0].s->submit(b);
		c.run_to(c.nodes[0].s->boundaries() + 4);
	};
	set_speed(0);
	bool tl0 = true;
	for (const Node& nd : c.nodes) tl0 = tl0 && nd.s->turn_length() == 2;
	set_speed(4);
	bool tl4 = true;
	for (const Node& nd : c.nodes) tl4 = tl4 && nd.s->turn_length() == 400;
	net::Bytes down{sim::op::kSpeedDown};
	c.nodes[1].s->submit(down);
	c.run_to(c.nodes[0].s->boundaries() + 3);
	bool tl3 = true;
	for (const Node& nd : c.nodes) tl3 = tl3 && nd.s->turn_length() == 8;  // speed 3 = 50 ms: ceil(400 / 50)
	set_speed(2);
	t.check(tl0 && tl4 && tl3 && c.agree(why), "speed: turn_length follows timings[speed] identically on every peer (0 -> 2, 4 -> 400, 3 -> 8)", why);
}

void test_resign(Tally& t, uint32_t seed) {
	Cluster c(3, seed, 0, 3);
	c.run_to(30);
	net::Bytes r;
	sim::wire_resign(r, 2);
	c.nodes[2].s->submit(r);
	c.run_to(60);
	bool ok = true;
	for (const Node& nd : c.nodes) ok = ok && !nd.s->expected(2) && nd.resigns == 1 && (nd.s->flags(2) & net::kPlayerResigned);
	std::string why;
	t.check(ok && c.agree(why), "resign: the leaver leaves the expected set, the survivors carry on with equal sums", why);
	t.check(c.nodes[2].s->local_active() == false && c.nodes[0].s->fifo_size(2) == 0, "resign: the leaver sends nothing more and no package queues up");
}

void test_drops(Tally& t, uint32_t seed, int trials) {
	for (int option = 0; option < 4; ++option) {
		int good = 0;
		std::string first_bad;
		for (int trial = 0; trial < trials; ++trial) {
			const uint32_t sd = seed * 131 + uint32_t(option * 17 + trial);
			Rng pick(sd);
			const int n = 3 + int(pick.below(2));  // 3 or 4 peers
			Cluster c(n, sd, 0, 4);
			for (Node& nd : c.nodes) nd.s->set_drop_preference(option);
			const int victim = int(pick.below(uint32_t(n)));
			const uint32_t when = 20 + pick.below(60);
			c.run_to(when);
			// the victim dies in the middle of its next broadcast: it reaches only the first `k` links (0 .. n-1)
			c.hub.kill_after_sends(victim, int(pick.below(uint32_t(n))));
			const uint32_t end = when + 80;
			for (int r = 0; r < 20000; ++r) {
				bool all = true;
				for (size_t i = 0; i < c.nodes.size(); ++i)
					if (int(i) != victim && c.nodes[i].s->boundaries() < end && c.nodes[i].last.kind != net::StepKind::Error) all = false;
				if (all) break;
				c.round();
				if (!c.hub.alive(victim)) c.nodes[size_t(victim)].dead = true;
			}
			c.nodes[size_t(victim)].dead = true;
			bool ok = true;
			std::string why;
			std::set<std::pair<int, uint32_t>> applied;
			for (size_t i = 0; i < c.nodes.size(); ++i) {
				if (int(i) == victim) continue;
				const Node& nd = c.nodes[i];
				if (nd.drops.size() != 1 || nd.drops[0].first != victim) ok = false, why = "node " + std::to_string(i) + " applied " + std::to_string(nd.drops.size()) + " drops";
				else applied.insert(nd.drops[0]);
				if (nd.s->boundaries() < end) ok = false, why = "node " + std::to_string(i) + " stuck at boundary " + std::to_string(nd.s->boundaries());
				const uint32_t f = nd.s->flags(victim);
				const bool flagged = option == net::kDropReplaceWithAi ? !(f & net::kPlayerHuman) && !(f & net::kPlayerDropped) : (f & net::kPlayerDropped) != 0;
				if (!flagged) ok = false, why = "slot flags 0x" + std::to_string(f) + " do not match option " + std::to_string(option);
			}
			if (applied.size() > 1) ok = false, why = "the drop was applied at different boundaries";
			std::string ag;
			if (!c.agree(ag)) ok = false, why = ag;
			if (!ok && std::getenv("RON_NET_DEBUG")) {
				std::printf("    debug: option %d trial %d seed %u victim %d turn %u peers %d: %s\n", option, trial, sd, victim, when, n, why.c_str());
				for (size_t i = 0; i < c.nodes.size(); ++i)
					std::printf("      node %zu: boundaries %u stamp %u last_accepted(victim) %u executed(victim) %u step %d slot %d text %s fifo(victim) %zu\n", i,
					            c.nodes[i].s->boundaries(), c.nodes[i].s->local_stamp(), c.nodes[i].s->last_accepted(victim), c.nodes[i].s->last_executed(victim),
					            int(c.nodes[i].last.kind), c.nodes[i].last.slot, c.nodes[i].last.text.c_str(), c.nodes[i].s->fifo_size(victim));
				for (size_t i = 0; i < c.nodes.size(); ++i) std::printf("      node %zu drop: %s\n", i, c.nodes[i].s->describe_drop(victim).c_str());
			}
			if (ok) ++good;
			else if (first_bad.empty()) first_bad = "seed " + std::to_string(sd) + ", victim " + std::to_string(victim) + " at turn " + std::to_string(when) + ": " + why;
		}
		static const char* names[4] = {"DROP_PLAYER", "DIPLOMACY", "SURVIVAL", "REPLACE_WITH_AI"};
		t.check(good == trials, std::string("drop ") + names[option] + ": survivors apply it at the same boundary with equal sums (random victim, turn and cut broadcast)",
		        std::to_string(good) + "/" + std::to_string(trials) + (first_bad.empty() ? "" : "; first failure " + first_bad));
	}
}

void test_desync(Tally& t, uint32_t seed) {
	for (int threshold : {0, 3}) {
		Cluster c(3, seed + uint32_t(threshold), 0, 3, [threshold](net::SessionConfig& cfg) { cfg.checksum_failure_threshold = threshold; });
		c.run_to(30);
		c.nodes[1].sim->state ^= 0x10;  // one bit of one instance's state flips
		const uint32_t at = c.nodes[1].s->boundaries();
		for (int r = 0; r < 5000; ++r) {
			bool all = true;
			for (const Node& nd : c.nodes) all = all && nd.last.kind == net::StepKind::Desync;
			if (all) break;
			c.round();
		}
		bool all = true;
		uint32_t worst = 0;
		for (const Node& nd : c.nodes) {
			all = all && nd.last.kind == net::StepKind::Desync;
			worst = std::max(worst, nd.s->boundaries() - at);
		}
		const uint32_t limit = threshold ? uint32_t(threshold) + 2 : 3;
		t.check(all && worst <= limit, "desync: a flipped bit is reported by every peer (threshold " + std::to_string(threshold) + ")",
		        "within " + std::to_string(worst) + " turns; " + (all ? c.nodes[0].s->desync().text().substr(0, 70) : "not all reported"));
		// frozen afterwards: no further frames
		const int32_t f = c.nodes[0].sim->f;
		for (int r = 0; r < 50; ++r) c.round();
		t.check(c.nodes[0].sim->f == f, "desync: the simulation stays frozen after the report");
	}
}


// ---- OOS recovery (multiplayer-spec 5.2, 8.2.4)

struct RecoveryCase {
	const char* name;
	int peers;
	int bad;          // the peer whose state is flipped
	int authority;    // the authority the majority rule must pick
	int observers;    // extra slots that only watch
	size_t pad;       // snapshot noise, bytes
	int dhi;          // delivery delay 0..dhi ms
};

void test_recovery(Tally& t, uint32_t seed) {
	const RecoveryCase cases[] = {
		{"2 peers, the guest diverges: the host is the authority", 2, 1, 0, 0, 0, 4},
		{"2 peers, the host diverges: the host is still the authority (<= 2 peers)", 2, 0, 0, 0, 0, 4},
		{"3 peers, peer 1 diverges: the majority (0, 2) wins, lowest slot is the authority", 3, 1, 0, 0, 0, 9},
		{"3 peers, peer 0 diverges: slot 1 is the authority", 3, 0, 1, 0, 0, 9},
		{"4 peers, peer 2 diverges, 300 KB snapshot in many chunks", 4, 2, 0, 0, 300000, 9},
		{"4 peers, peer 0 diverges, 300 KB snapshot in many chunks", 4, 0, 1, 0, 300000, 5},
		{"3 humans + 1 observer, peer 1 diverges: the observer loads the snapshot too", 3, 1, 0, 1, 20000, 5},
	};
	for (const RecoveryCase& rc : cases) {
		const int total = rc.peers + rc.observers;
		Cluster c(total, seed + uint32_t(rc.peers * 7 + rc.bad), 0, rc.dhi, [&](net::SessionConfig& cfg) {
			cfg.recover = true;
			cfg.recover_chunk = 4096;
			for (int o = 0; o < rc.observers; ++o) cfg.slot_flags[size_t(rc.peers + o)] = net::kPlayerValid | net::kPlayerObserver;
		}, rc.peers);
		for (Node& nd : c.nodes) nd.sim->pad_bytes = rc.pad;
		c.run_to(30);
		c.nodes[size_t(rc.bad)].sim->state ^= 0x10;
		bool recovered = false;
		for (int r = 0; r < 200000 && !recovered; ++r) {
			c.round();
			recovered = true;
			for (const Node& nd : c.nodes) recovered = recovered && nd.s->epoch() == 1 && !nd.s->recovering();
		}
		size_t from = SIZE_MAX;
		for (const Node& nd : c.nodes) from = std::min(from, nd.bis.size());
		const uint32_t b0 = c.nodes[0].s->boundaries();
		const bool ran = recovered && c.run_to(b0 + 80);
		std::string why;
		bool ok = ran;
		int snap_nodes = 0, auth = -1;
		for (size_t i = 0; i < c.nodes.size(); ++i) {
			const Node& nd = c.nodes[i];
			if (nd.sim->snaps) ++snap_nodes, auth = int(i);
			if (nd.sim->restores != 1) ok = false, why = "node " + std::to_string(i) + " loaded " + std::to_string(nd.sim->restores) + " snapshots";
			if (nd.s->epoch() != 1) ok = false, why = "epoch " + std::to_string(nd.s->epoch());
			if (nd.s->local_stamp() > 120) ok = false, why = "stamps did not restart: " + std::to_string(nd.s->local_stamp());
		}
		if (snap_nodes != 1 || auth != rc.authority) ok = false, why = "authority " + std::to_string(auth) + ", expected " + std::to_string(rc.authority);
		// sums agree on every boundary after the recovery (boundaries[from..] of every node; they start at stamp 1 of the new epoch)
		std::string ag;
		if (!c.agree(ag, from)) ok = false, why = ag;
		t.check(ok, std::string("recovery: ") + rc.name, ok ? "authority " + std::to_string(auth) + ", " + std::to_string(c.nodes[0].s->boundaries() - b0) + " turns after, stamp " + std::to_string(c.nodes[0].s->local_stamp()) + ", state equal" : why);
	}
	// two recoveries in a row, random delays
	{
		Cluster c(3, seed + 5, 0, 9, [](net::SessionConfig& cfg) { cfg.recover = true; });
		c.run_to(25);
		c.nodes[2].sim->state ^= 1;
		for (int r = 0; r < 100000; ++r) {
			c.round();
			if (c.nodes[0].s->epoch() == 1 && c.nodes[1].s->epoch() == 1 && c.nodes[2].s->epoch() == 1) break;
		}
		c.run_to(c.nodes[0].s->boundaries() + 40);
		c.nodes[1].sim->state ^= 2;
		size_t from2 = SIZE_MAX;
		bool again = false;
		for (int r = 0; r < 100000 && !again; ++r) {
			c.round();
			again = c.nodes[0].s->epoch() == 2 && c.nodes[1].s->epoch() == 2 && c.nodes[2].s->epoch() == 2;
		}
		for (const Node& nd : c.nodes) from2 = std::min(from2, nd.bis.size());
		const bool ran = again && c.run_to(c.nodes[0].s->boundaries() + 60);
		std::string ag2;
		const bool eq = ran && c.agree(ag2, from2);
		t.check(eq, "recovery: twice in one game, the stamps restart each time", ran ? "epoch " + std::to_string(c.nodes[0].s->epoch()) : "did not recover again");
	}
	// the authority disappears while the snapshot is in transit: whoever loaded it serves the peer that lacks it
	for (int variant = 0; variant < 2; ++variant) {
		Cluster c(3, seed + 11 + uint32_t(variant), 0, 3, [](net::SessionConfig& cfg) {
			cfg.recover = true;
			cfg.recover_chunk = 2048;
		});
		for (Node& nd : c.nodes) nd.sim->pad_bytes = 60000;
		c.run_to(30);
		// variant 0: the authority (slot 0) dies after sending peer 1 the whole snapshot and peer 2 a part of it
		// variant 1: it dies before a single byte of the snapshot left
		c.hub.kill_after_sends(0, variant == 0 ? 40 : 0);
		c.nodes[2].sim->state ^= 0x20;
		bool done = false;
		for (int r = 0; r < 300000 && !done; ++r) {
			c.round();
			if (!c.hub.alive(0)) c.nodes[0].dead = true;
			done = c.nodes[1].s->epoch() == 1 && c.nodes[2].s->epoch() == 1;
		}
		if (!done && std::getenv("RON_NET_DEBUG"))
			for (size_t i = 1; i < c.nodes.size(); ++i)
				std::printf("    debug: node %zu epoch %u recovering %d authority %d bytes %zu last %d '%s' alive0 %d\n", i, c.nodes[i].s->epoch(), int(c.nodes[i].s->recovering()),
				            c.nodes[i].s->recovery_authority(), c.nodes[i].s->recovery_bytes(), int(c.nodes[i].last.kind), c.nodes[i].last.text.c_str(), int(c.hub.alive(0)));
		size_t from3 = SIZE_MAX;
		for (size_t i = 1; i < c.nodes.size(); ++i) from3 = std::min(from3, c.nodes[i].bis.size());
		const bool ran = done && c.run_to(c.nodes[1].s->boundaries() + 40);
		std::string ag3;
		if (done && !ran && std::getenv("RON_NET_DEBUG"))
			for (size_t i = 1; i < c.nodes.size(); ++i)
				std::printf("    debug: node %zu boundaries %u stamp %u epoch %u last %d '%s' slot %d drop0 %s\n", i, c.nodes[i].s->boundaries(), c.nodes[i].s->local_stamp(), c.nodes[i].s->epoch(),
				            int(c.nodes[i].last.kind), c.nodes[i].last.text.c_str(), c.nodes[i].last.slot, c.nodes[i].s->describe_drop(0).c_str());
		const bool eq = ran && c.agree(ag3, from3);
		t.check(eq, std::string("recovery: the authority dies ") + (variant == 0 ? "mid-transfer: the peer that has the snapshot serves the one that lacks it" : "before sending anything: the next candidate takes a snapshot") + ", the survivors end equal",
		        ran ? "ok" : ag3.empty() ? "no recovery" : ag3);
	}
}


// ---- the engine over the loopback: recovery in a real game, recording, replay of the recording (needs the game install)

std::vector<uint8_t> slurp(const std::string& p) {
	std::ifstream f(p, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void test_engine(Tally& t, const std::string& dir, uint32_t seed) {
	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::exists(dir + "/Data", ec)) {
		std::printf("  SKIP engine tests: no game install at %s (--game DIR)\n", dir.c_str());
		return;
	}
	const fs::path tmp = fs::temp_directory_path() / ("ron-net-selftest-" + std::to_string(getpid()));
	fs::create_directories(tmp, ec);
	// 1. two engine instances, a divergence injected into the guest, recovery, recording
	{
		LoopbackOptions o;
		o.game_dir = dir;
		o.random_spec = "18,0," + std::to_string(seed) + ",2";
		o.humans = 2;
		o.frames = 360;
		o.recover = true;
		o.diverge = {{150, 1}};
		o.record_base = (tmp / "two").string();
		o.save_out = (tmp / "two.sav").string();
		o.dhi = 3;
		const LoopbackResult r = run_loopback(o);
		t.check(r.ok && r.identical && r.epoch == 1 && r.recoveries == 2, "engine, 2 instances: a divergence injected at frame 150 is repaired from the authority's snapshot, final native saves identical",
		        r.ok ? "frame " + std::to_string(r.frame) + ", 2 snapshot loads, fnv64 " + std::to_string(r.final_fnv) : r.text);
		bool two_segments = r.segments.size() == 2;
		for (const auto& kv : r.segments) two_segments = two_segments && kv.second.size() == 2;
		t.check(two_segments, "recording: every instance wrote a segment chain (the first from the start, the second from the recovery snapshot)");
		// the reader and the replayer: each segment of each instance
		const std::vector<uint8_t> live = slurp((tmp / "two.sav").string());
		int replayed = 0, matched = 0;
		for (const auto& kv : r.segments) {
			const std::string& last = kv.second.back();
			sim::Replay rp;
			std::string err;
			const bool read = sim::read_replay(last, rp, err) && rp.native_save && rp.header.multiplayer && rp.packages.size() > 10;
			const std::string out = (tmp / ("replay" + std::to_string(kv.first) + ".sav")).string();
			const int rc = read ? replay_native(rp, last, dir, 0, 4, out, false) : 1;
			++replayed;
			matched += rc == 0 && slurp(out) == live;
			if (!read) t.check(false, "recording: " + last + " cannot be read back", err);
		}
		t.check(replayed == 2 && matched == 2, "replay: the last segment of each instance's recording replays to the live game's final native save, byte for byte", std::to_string(matched) + "/" + std::to_string(replayed));
		// the first segment ends at the freeze: replayed, it must reach the state the recovery's snapshot was taken from, one frame step later
		sim::Replay first, second;
		std::string err;
		const bool ok = r.segments.count(0) && sim::read_replay(r.segments.at(0)[0], first, err) && sim::read_replay(r.segments.at(0)[1], second, err);
		t.check(ok && !first.native_save && second.native_save && second.header.frame == first.packages.back().frame, "recording: the second segment starts at the frame where the first one stopped",
		        ok ? "frames " + std::to_string(first.header.frame) + ".." + std::to_string(first.packages.back().frame) + " | " + std::to_string(second.header.frame) + ".." + std::to_string(second.packages.back().frame) : err);
	}
	// 2. three instances, no divergence: the recording of the third replays to the final state; the lockstep layer without recovery in the way
	{
		LoopbackOptions o;
		o.game_dir = dir;
		o.random_spec = "16,0," + std::to_string(seed + 1) + ",3";
		o.humans = 3;
		o.frames = 300;
		o.recover = true;
		o.record_base = (tmp / "three").string();
		o.save_out = (tmp / "three.sav").string();
		o.dhi = 5;
		const LoopbackResult r = run_loopback(o);
		bool replay_ok = false;
		if (r.ok && r.segments.count(2)) {
			sim::Replay rp;
			std::string err;
			const std::string out = (tmp / "three_replay.sav").string();
			replay_ok = sim::read_replay(r.segments.at(2)[0], rp, err) && !rp.native_save && replay_native(rp, r.segments.at(2)[0], dir, 0, 3, out, false) == 0 &&
			            slurp(out) == slurp((tmp / "three.sav").string());
		}
		t.check(r.ok && r.recoveries == 0 && r.epoch == 0 && replay_ok, "engine, 3 instances, no divergence: no recovery, and the recording of one instance (random game from the header) replays to the final native save", r.ok ? "frame " + std::to_string(r.frame) : r.text);
	}
	// 3. the lobby's rules in a running game (docs/multiplayer-spec.md 8.6): two players and an observer, handicaps, chat to everybody and to a team
	{
		LoopbackOptions o;
		o.game_dir = dir;
		o.random_spec = "16,0," + std::to_string(seed + 2) + ",3";
		o.humans = 3;   // three seats: the third is outside the team mask of the chat test
		o.observers = 1;
		o.handicaps = {0, 5, 0};
		o.chat_test = true;
		o.frames = 240;
		o.dhi = 4;
		const LoopbackResult r = run_loopback(o);
		t.check(r.ok && r.identical && r.fnv.size() == 4, "engine, 3 players + 1 observer (game slot above the nations): all four instances end in identical native saves", r.ok ? "frame " + std::to_string(r.frame) : r.text);
		t.check(r.handicap_seen == 5, "handicap: the seat's lobby handicap is in the game state (Leader::handicap), the game is a multiplayer game", "leader 1 handicap " + std::to_string(r.handicap_seen));
		t.check(r.chat_report == "ok", "chat: a line to everybody and a line to a recipient mask (seats 0 and 1, the third seat outside it) leave every instance in the same state: no out-of-sync, the mask is kept for the display filter", r.chat_report);
	}
	// 4. scenario games: the scenario's players are the seats (the human-flagged ones first), Game::load on every peer with the same seed
	for (int variant = 0; variant < 2; ++variant) {
		LoopbackOptions o;
		o.game_dir = dir;
		o.scenario = variant ? "scenario/Tutorial1/tut1.scx" : "scenario/Custom/italy_mp/italy_mp.scx";
		o.humans = 2;
		o.observers = variant;   // italy_mp uses all eight seats: nobody can watch; the tutorial has room for one observer
		o.frames = 200;
		o.dhi = 3;
		const LoopbackResult r = run_loopback(o);
		t.check(r.ok && r.identical && int(r.fnv.size()) == 2 + variant,
		        std::string("engine, scenario game (") + (variant ? "Tutorial 1, 2 players + 1 observer" : "italy_mp, 8 seats, 2 players") + "): identical native saves", r.ok ? "frame " + std::to_string(r.frame) : r.text);
	}
	// 5. Conquer the World battles (generated map, docs/multiplayer-spec.md 8.7): the recording's header carries the battle's setup text (ctw=1, the conquest
	//    map input) and the start state; ron-net replay plays it back in lockstep, the instances agree at every turn and end at the frame the game ended at.
	//    One variant with two players, one with a single player (a battle of the campaign played alone against the computer).
	for (int players = 2; players >= 1; --players) {
		LoopbackOptions o;
		o.game_dir = dir;
		o.ctw_battle = "new_world:auto";
		o.humans = players;
		o.frames = 160;
		o.record_base = (tmp / ("ctw" + std::to_string(players))).string();
		o.save_out = (tmp / ("ctw" + std::to_string(players) + ".sav")).string();
		o.dhi = 3;
		const LoopbackResult r = run_loopback(o);
		const std::string what = std::string("engine, Conquer the World battle (new_world:auto, ") + (players == 2 ? "2 players" : "1 player") + ")";
		t.check(r.ok && r.identical && int(r.fnv.size()) == players, what + ": lockstep, identical native saves", r.ok ? "frame " + std::to_string(r.frame) : r.text);
		bool header_ok = false, replay_ok = false;
		std::string why;
		if (r.ok && r.segments.count(players - 1)) {
			const std::string file = r.segments.at(players - 1)[0];
			sim::Replay rp;
			std::string err;
			if (!sim::read_replay(file, rp, err)) {
				why = err;
			} else {
				const std::string text = sim::to_utf8(rp.header.scenario_path);
				sim::RandomGameSetup parsed;
				header_ok = rp.native_save && rp.header.multiplayer && sim::is_random_scenario(text) && text.find(";ctw=1") != std::string::npos &&
				            text.find(";conq=") != std::string::npos && sim::parse_random_setup(text, parsed) && parsed.ctw && !parsed.conquest.empty();
				const std::string out = (tmp / ("ctw" + std::to_string(players) + "_replay.sav")).string();
				const int rc = replay_native(rp, file, dir, 0, 3, out, false);
				const std::vector<uint8_t> got = slurp(out);
				int32_t frame = -1;
				try {
					frame = sim::read_save_info(got).frame;
				} catch (const std::exception&) {
				}
				replay_ok = rc == 0 && frame == r.frame && got == slurp(o.save_out);   // and, since a loaded battle steps like the live one, byte for byte
				why = "replay frame " + std::to_string(frame) + " of " + std::to_string(r.frame) + ", rc " + std::to_string(rc);
			}
		}
		t.check(header_ok, what + ": the recording's header holds the battle's setup text (ctw=1, conq=) and the start state");
		t.check(replay_ok, what + ": ron-net replay plays the recording in lockstep (instances agree at every turn) up to the frame the game ended at", why);
	}
	// 6. a battle can be repaired like any game now that a loaded battle steps like the live one: a divergence injected into the guest is repaired from the authority's
	//    snapshot (generated map, and a custom map whose scripts and campaign come back from the save), the final native saves agree
	for (const char* battle : {"new_world:auto", "alexander:auto"}) {
		LoopbackOptions o;
		o.game_dir = dir;
		o.ctw_battle = battle;
		o.humans = 2;
		o.frames = 360;
		o.recover = true;
		o.diverge = {{150, 1}};
		o.dhi = 3;
		const LoopbackResult r = run_loopback(o);
		t.check(r.ok && r.identical && r.epoch == 1 && r.recoveries == 2, std::string("engine, Conquer the World battle (") + battle + "): a divergence injected at frame 150 is repaired from the authority's snapshot, final native saves identical",
		        r.ok ? "frame " + std::to_string(r.frame) + ", " + std::to_string(r.recoveries) + " snapshot loads" : r.text);
	}
	fs::remove_all(tmp, ec);
}

void test_no_wallclock(Tally& t, uint32_t seed) {
	// the same inputs under two different delivery schedules give the same states (the clock only decides when a tick happens)
	std::vector<uint64_t> states;
	for (int variant = 0; variant < 3; ++variant) {
		Cluster c(3, seed, variant == 0 ? 0 : 0, variant == 0 ? 0 : variant * 11);
		c.run_to(150);
		// compare at boundary 150, which every variant has reached
		states.push_back(mix(mix(c.nodes[0].bis[149].sums[0], c.nodes[0].bis[149].sums[7]), c.nodes[0].bis[149].sums[15]));
	}
	t.check(states[0] == states[1] && states[1] == states[2], "determinism: the delivery delay changes when turns run, never what they compute");
}


// ---------------------------------------------------------------- the lobby rules (multiplayer-spec 8.6)

// N lobbies over the loopback transport: machine 0 is the host.
struct Room {
	explicit Room(int n) : hub(n, 1, 0, 0) {
		for (int i = 0; i < n; ++i) lobby.push_back(std::make_unique<net::Lobby>(hub.endpoint(i)));
	}
	void pump(int rounds = 30) {
		for (int r = 0; r < rounds; ++r) {
			for (auto& l : lobby) l->poll();
			hub.advance(1);
		}
	}
	net::LoopbackHub hub;
	std::vector<std::unique_ptr<net::Lobby>> lobby;
	net::Lobby& host() { return *lobby[0]; }
	net::Lobby& at(int i) { return *lobby[size_t(i)]; }
	int reply(int i, net::SeatField f, int v) {  // a request from machine i and the host's answer code
		net::Lobby::Reply r;
		while (at(i).take_reply(r)) {}
		at(i).request(f, v);
		pump();
		return at(i).take_reply(r) ? r.code : -1;
	}
};

void test_lobby_rules(Tally& t) {
	// -- the seat plan: players in connection order, observers above the nations
	{
		const net::SeatPlan p = net::make_seat_plan(0b1111, 0b0010, 3, false);
		const bool ok = p.players == 3 && p.observers == 1 && p.nations == 3 && p.peer_of[0] == 0 && p.peer_of[1] == 2 && p.peer_of[2] == 3 && p.peer_of[3] == 1 && p.slot_of[1] == 3 &&
		                p.slot_of[2] == 1 && !p.overflow && p.human_mask() == 0b0111;
		t.check(ok, "seats: players take the seats in connection order, the observer sits above the nations (4 machines, machine 1 watches)");
		const net::SeatPlan q = net::make_seat_plan(0b1011, 0, 2, true);   // a saved game: players keep their numbers
		t.check(q.nations == 4 && q.peer_of[3] == 3 && q.peer_of[2] == -1 && q.slot_of[1] == 1, "seats: a saved game's players keep the seats they connected in");
		const net::SeatPlan o = net::make_seat_plan(0xff, 0b11110000, 5, false);
		t.check(o.overflow && o.nations == 5, "seats: observers that do not fit above the nations are reported");
		std::array<int, 8> forced;
		forced.fill(-1);
		forced[0] = 1, forced[1] = 0;
		const net::SeatPlan f = net::make_seat_plan(0b11, 0, 2, false, &forced);
		t.check(f.peer_of[1] == 0 && f.peer_of[0] == 1, "seats: a battle can seat a player on a given side");
	}
	// -- guests' requests
	Room room(4);
	net::Lobby& h = room.host();
	h.set("num_nations", 4);
	for (int i = 0; i < 4; ++i) h.set("tribe_" + std::to_string(i), -1), h.set("team_" + std::to_string(i), -1), h.set("color_" + std::to_string(i), -1);
	h.set("max_observers", 1);
	room.pump();
	for (int i = 1; i < 4; ++i) room.at(i).set_name(i == 2 ? "Ayla" : "Marcus");   // two guests with the same name
	h.set_name("Host");
	room.pump();
	t.check(h.get("name_1") == "Marcus" && h.get("name_3") != "Marcus" && !h.get("name_3").empty(), "names: a second player of the same name gets a different one", h.get("name_1") + " / " + h.get("name_3"));
	t.check(room.reply(1, net::SeatField::Tribe, 5) == net::kAccepted && h.get_int("tribe_1") == 5 && room.at(2).get_int("tribe_1") == 5, "request: a free nation is granted at once and every machine sees it");
	net::SeatRequest rq;
	t.check(h.take_request(rq) && rq.seat == 1 && rq.field == net::SeatField::Tribe && rq.value == 5 && !rq.pending, "request: the host's application is told which seat changed");
	t.check(room.reply(2, net::SeatField::Tribe, 5) == net::kTaken && h.get_int("tribe_2") == -1, "request: a nation another seat holds is refused");
	t.check(room.reply(2, net::SeatField::Tribe, 99) == net::kInvalid, "request: a nation that does not exist is refused");
	t.check(room.reply(2, net::SeatField::Color, 5) == net::kAccepted && room.reply(3, net::SeatField::Color, 5) == net::kTaken && room.reply(3, net::SeatField::Color, 6) == net::kAccepted,
	        "request: a colour is unique among the seats");
	t.check(room.reply(1, net::SeatField::Color, 0) == net::kTaken && room.reply(1, net::SeatField::Color, 6) == net::kTaken && room.reply(1, net::SeatField::Color, 9) == net::kInvalid,
	        "request: the colour a seat has by default (its number) counts as taken");
	t.check(room.reply(1, net::SeatField::Team, 1) == net::kAccepted && h.get_int("team_1") == 1 && room.reply(1, net::SeatField::Team, 7) == net::kInvalid, "request: teams 1-4 or none");
	t.check(room.reply(3, net::SeatField::Handicap, 4) == net::kAccepted && h.get_int("handicap_3") == 4 && room.reply(3, net::SeatField::Handicap, 99) == net::kInvalid, "request: handicap within the rules' list");
	// -- the host decides
	h.set_approval(net::Lobby::Approval::Ask);
	while (h.take_request(rq)) {}
	room.at(1).request(net::SeatField::Tribe, 8);
	room.pump();
	bool pend = h.pending().size() == 1 && h.get_int("tribe_1") == 5;
	const uint32_t id = pend ? h.pending().front().id : 0;
	h.answer(id, false);
	room.pump();
	net::Lobby::Reply rep;
	bool denied = false;
	while (room.at(1).take_reply(rep)) denied = denied || rep.code == net::kDenied;
	t.check(pend && denied && h.get_int("tribe_1") == 5 && h.pending().empty(), "approval: a request waits for the host; refused, the table is unchanged and the guest is told");
	room.at(1).request(net::SeatField::Tribe, 8);
	room.pump();
	h.answer(h.pending().empty() ? 0 : h.pending().front().id, true);
	room.pump();
	bool applied = false;
	while (h.take_request(rq)) applied = applied || (!rq.pending && rq.value == 8 && rq.seat == 1);
	t.check(h.get_int("tribe_1") == 8 && applied && room.at(3).get_int("tribe_1") == 8, "approval: allowed, the table changes and the application is told");
	h.set_approval(net::Lobby::Approval::Auto);
	// -- observers
	t.check(room.reply(2, net::SeatField::Observer, 1) == net::kAccepted && h.observer_of(2), "observers: a guest may ask to watch within the host's limit");
	t.check(room.reply(3, net::SeatField::Observer, 1) == net::kNoRoom, "observers: the host's maximum (1) is respected");
	{
		const net::SeatPlan p = h.plan();
		t.check(p.players == 3 && p.observers == 1 && p.slot_of[2] >= p.nations && p.slot_of[1] == 1 && p.slot_of[3] == 2, "observers: the plan moves the watcher above the nations and closes the gap");
	}
	t.check(room.reply(2, net::SeatField::Tribe, 1) == net::kInvalid, "observers: a watcher has no seat to change");
	t.check(room.reply(2, net::SeatField::Observer, 0) == net::kAccepted && !h.observer_of(2), "observers: a watcher may join the players again");
	// -- locks
	h.set("scenario_game", 1);
	t.check(room.reply(1, net::SeatField::Tribe, 2) == net::kLocked && room.reply(1, net::SeatField::Handicap, 2) == net::kAccepted, "scenario: nation, team and colour are the scenario's, the handicap is still free");
	h.set("scenario_game", "");
	h.set("identity", 1);
	t.check(room.reply(1, net::SeatField::Handicap, 2) == net::kLocked, "saved game: every seat is fixed");
	h.set("identity", "");
	// -- start fixes the plan for everybody
	room.reply(2, net::SeatField::Observer, 1);
	t.check(h.start(77), "start: the host fixes the plan and the seed");
	room.pump();
	{
		const net::SeatPlan a = h.start_plan(), b = room.at(3).start_plan();
		bool same = a.nations == b.nations && a.players == 3 && a.observers == 1;
		for (int s = 0; s < 8; ++s) same = same && a.peer_of[size_t(s)] == b.peer_of[size_t(s)];
		t.check(same && room.at(3).started() && room.at(3).seed() == 77, "start: every machine reads the same plan and seed");
	}
	// -- the room after a game
	for (int i = 0; i < 4; ++i) room.at(i).reset_game();
	room.pump();
	t.check(!h.started() && !room.at(2).started(), "room: after a game it is a lobby again");
	// -- chat targets, kick, countdown
	room.at(2).set_ready(false);
	h.set("team_0", 2);
	h.set("team_1", 2);
	h.set("team_2", 3);
	room.pump();
	for (int i = 0; i < 4; ++i)
		for (net::Lobby::ChatLine l; room.at(i).take_chat(l);) {}
	h.say_to(1, -1, "team plan");   // the host and the machine in seat 1 are on team 3 (index 2); seat 2 is on team 4
	room.pump();
	net::Lobby::ChatLine cl;
	int heard = 0, who = -1;
	for (int i = 1; i < 4; ++i)
		if (room.at(i).take_chat(cl)) ++heard, who = i;
	t.check(heard == 1 && who == 1 && cl.kind == 1 && cl.text == "team plan", "chat: a team line reaches the machines of the team and nobody else", "heard by " + std::to_string(heard));
	room.at(1).say_to(2, 3, "psst");
	room.pump();
	heard = 0;
	for (int i = 0; i < 4; ++i)
		if (room.at(i).take_chat(cl)) ++heard, who = i;
	t.check(heard == 1 && who == 3 && cl.kind == 2 && cl.from == 1, "chat: a whisper reaches only its receiver");
	h.countdown_set(5);
	room.pump();
	net::Lobby::ChatLine sys;
	bool seen = false;
	while (room.at(2).take_chat(sys)) seen = seen || (sys.kind == 3 && sys.text.find("5 seconds") != std::string::npos);
	t.check(seen && room.at(2).countdown() == 5, "countdown: the guests see the host's ticks as system lines");
	h.countdown_set(0);
	room.pump();
	t.check(room.at(2).countdown() < 0, "countdown: cancelled for everybody");
	h.kick(3);
	room.pump();
	t.check(room.at(3).kicked() && !room.at(1).kicked(), "kick: the removed machine is told");
}

// ---------------------------------------------------------------- seats in the lockstep layer

// The same scenario with raw transports (an observer in the highest slot, the victim a player in the middle): the layer itself.
void test_drop_with_observer(Tally& t, uint32_t seed) {
	int good = 0;
	std::string first_bad;
	const int trials = 8;
	for (int trial = 0; trial < trials; ++trial) {
		const uint32_t sd = seed * 977 + uint32_t(trial);
		Cluster c(4, sd, 0, 5, [](net::SessionConfig& cfg) { cfg.slot_flags[3] = net::kPlayerValid | net::kPlayerObserver; }, 3);
		Rng pick(sd);
		c.run_to(30 + pick.below(40));
		const int victim = int(pick.below(3));
		c.hub.kill_after_sends(victim, int(pick.below(4)));
		const uint32_t end = c.nodes[0].s->boundaries() + 120;
		for (int r = 0; r < 40000; ++r) {
			bool all = true;
			for (size_t i = 0; i < c.nodes.size(); ++i)
				if (int(i) != victim && c.nodes[i].s->boundaries() < end && c.nodes[i].last.kind != net::StepKind::Error) all = false;
			if (all) break;
			c.round();
			if (!c.hub.alive(victim)) c.nodes[size_t(victim)].dead = true;
		}
		c.nodes[size_t(victim)].dead = true;
		std::string why;
		bool ok = c.agree(why);
		for (size_t i = 0; i < c.nodes.size() && ok; ++i)
			if (int(i) != victim && (c.nodes[i].drops.size() != 1 || c.nodes[i].s->boundaries() < end)) ok = false, why = "node " + std::to_string(i) + ": " + std::to_string(c.nodes[i].drops.size()) + " drops, boundary " + std::to_string(c.nodes[i].s->boundaries());
		if (ok) ++good;
		else if (first_bad.empty()) first_bad = "seed " + std::to_string(sd) + ", victim " + std::to_string(victim) + ": " + why;
	}
	t.check(good == trials, "drop with an observer in the room: the players and the observer apply it at the same boundary with equal sums", std::to_string(good) + "/" + std::to_string(trials) + (first_bad.empty() ? "" : "; first failure " + first_bad));
}

void test_seat_transport(Tally& t, uint32_t seed) {
	// four machines, one of which only watches and one of which dies; "permuted": machine 1 watches and machine 3 dies, so the game slots differ from the
	// machine numbers (0 <- machine 0, 1 <- machine 2, 2 <- machine 3, observer slot 3 <- machine 1); "straight": machine 3 watches and machine 2 dies
	for (int variant = 0; variant < 2; ++variant) {
		const uint32_t obs_mask = variant ? 0b0010 : 0b1000;
		const int victim = variant ? 3 : 2;
		net::LoopbackHub hub(4, seed + uint32_t(variant), 0, 6);
		const net::SeatPlan plan = net::make_seat_plan(0b1111, obs_mask, 3, false);
		struct M {
			std::unique_ptr<net::SeatTransport> tr;
			std::unique_ptr<ToySim> sim;
			std::unique_ptr<net::Session> s;
			std::vector<net::BoundaryInfo> bis;
			std::vector<std::pair<int, uint32_t>> drops;
			uint32_t fed = 0;
			Rng rng{1};
		};
		std::vector<M> ms(4);
		for (int i = 0; i < 4; ++i) {
			M& m = ms[size_t(i)];
			m.sim = std::make_unique<ToySim>();
			m.rng = Rng(seed * 31 + uint32_t(i));
			net::SessionConfig c;
			c.local_slot = plan.slot_of[size_t(i)];
			c.seed = seed;
			c.speed = 2;
			c.max_pauses = 0;
			for (int sl = 0; sl < 8; ++sl) {
				if (sl < plan.nations) c.slot_flags[size_t(sl)] = net::kPlayerValid | net::kPlayerHuman;
				else if (plan.peer_of[size_t(sl)] >= 0) c.slot_flags[size_t(sl)] = net::kPlayerValid | net::kPlayerObserver;
			}
			m.tr = std::make_unique<net::SeatTransport>(hub.endpoint(i), plan.peer_of);
			m.s = std::make_unique<net::Session>(*m.tr, *m.sim, c);
			M* mp = &m;
			m.s->on_boundary = [mp](const net::BoundaryInfo& b) { mp->bis.push_back(b); };
			m.s->on_drop = [mp](int slot, int, uint32_t boundary) { mp->drops.emplace_back(slot, boundary); };
		}
		auto feed = [&](int i) {
			M& m = ms[size_t(i)];
			const bool watcher = plan.slot_of[size_t(i)] >= plan.nations;
			while (m.fed < m.s->local_stamp() + 1) {
				++m.fed;
				if (watcher) continue;
				net::Bytes b = {24, uint8_t(m.rng.next()), 0, 0, 0, 1, 0, 0, 0};
				m.s->submit(b);
			}
		};
		for (int i = 0; i < 4; ++i) {
			feed(i);
			ms[size_t(i)].s->start();
		}
		bool killed = false;
		uint32_t at = 0;
		for (int r = 0; r < 400000; ++r) {
			uint32_t lo = UINT32_MAX;
			for (int i = 0; i < 4; ++i) {
				if (killed && i == victim && !hub.alive(victim)) continue;   // a machine that is still alive finishes its broadcast and dies at the cut
				feed(i);
				ms[size_t(i)].s->step();
				if (i != victim) lo = std::min(lo, ms[size_t(i)].s->boundaries());
			}
			hub.advance(1);
			if (!killed && lo >= 60) {
				hub.kill_after_sends(victim, 1);   // the machine dies in the middle of a broadcast
				killed = true;
				at = lo;
			}
			if (killed && lo >= at + 80) break;
		}
		std::vector<int> live;
		for (int i = 0; i < 4; ++i)
			if (i != victim) live.push_back(i);
		bool equal = true;
		size_t first_bad = 0, n = SIZE_MAX;
		for (int i : live) n = std::min(n, ms[size_t(i)].bis.size());
		for (size_t j = 0; j < n && equal; ++j)
			for (int i : live)
				if (std::memcmp(ms[size_t(i)].bis[j].sums, ms[size_t(live[0])].bis[j].sums, sizeof ms[0].bis[j].sums) != 0) equal = false, first_bad = j + 1;
		const std::string name = std::string(variant ? "permuted" : "straight") + " seating";
		if ((!equal || n <= 100) && std::getenv("RON_NET_DEBUG"))
			for (int i = 0; i < 4; ++i)
				std::printf("    machine %d (slot %d): boundaries %u frame %d commands %llu drops %zu %s\n", i, plan.slot_of[size_t(i)], ms[size_t(i)].s->boundaries(), ms[size_t(i)].sim->f,
				            (unsigned long long)ms[size_t(i)].sim->commands, ms[size_t(i)].drops.size(), ms[size_t(i)].s->describe_drop(plan.slot_of[size_t(victim)]).c_str());
		t.check(equal && n > 100, "seats, " + name + ": players and an observer agree on every turn", std::to_string(n) + " turns" + (equal ? "" : ", first difference at turn " + std::to_string(first_bad)));
		const int obs = variant ? 1 : 3;
		// the observer's state is the players' state (the sums above compare it turn by turn); it ran the players' commands, sent none of its own
		t.check(ms[size_t(obs)].sim->commands > 100 && ms[size_t(obs)].s->boundaries() >= ms[size_t(live[0] == obs ? live[1] : live[0])].s->boundaries() - 2,
		        "seats, " + name + ": the observer executed the players' commands and kept up with them", std::to_string(ms[size_t(obs)].sim->commands) + " commands");
		bool dropped = true;
		for (int i : live) dropped = dropped && !ms[size_t(i)].drops.empty() && ms[size_t(i)].drops[0] == ms[size_t(live[0])].drops[0] && ms[size_t(i)].drops[0].first == plan.slot_of[size_t(victim)];
		t.check(dropped, "seats, " + name + ": the dead machine is dropped under its game slot (" + std::to_string(plan.slot_of[size_t(victim)]) + "), at the same boundary everywhere");
	}
}

// ---------------------------------------------------------------- a room over real sockets

void test_tcp_room(Tally& t) {
	const uint16_t port = uint16_t(42000 + (getpid() % 1000));
	std::string err;
	std::unique_ptr<net::TcpTransport> host = net::TcpTransport::host_open(port, err);
	if (!host) {
		t.check(false, "room over TCP: listen", err);
		return;
	}
	std::unique_ptr<net::TcpTransport> g1, g2;
	std::string e1, e2;
	std::thread th1([&]() { g1 = net::TcpTransport::join("127.0.0.1", port, 5000, e1); });
	std::thread th2([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		g2 = net::TcpTransport::join("127.0.0.1", port, 5000, e2);
	});
	net::Lobby hl(*host);
	for (int i = 0; i < 300 && (!g1 || !g2); ++i) {
		hl.poll();
		host->idle(10);
	}
	th1.join();
	th2.join();
	if (!g1 || !g2) {
		t.check(false, "room over TCP: two guests join", e1 + " " + e2);
		return;
	}
	net::Lobby l1(*g1), l2(*g2);
	auto pump = [&](int n = 60) {
		for (int i = 0; i < n; ++i) {
			hl.poll(), l1.poll(), l2.poll();
			int p;
			while (host->take_timeout(p)) hl.peer_left(p);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	};
	hl.set("num_nations", 3);
	hl.set("max_observers", 1);
	hl.set_name("Host");
	l1.set_name("Ayla");
	l2.set_name("Marcus");
	pump();
	t.check(g1->local_peer() == 1 && g2->local_peer() == 2 && hl.name_of(2) == "Marcus" && l1.name_of(2) == "Marcus", "room over TCP: guests get the lowest free slots, names reach everybody");
	l1.request(net::SeatField::Color, 5);
	l2.request(net::SeatField::Observer, 1);
	pump();
	net::Lobby::Reply r1, r2;
	const bool a1 = l1.take_reply(r1), a2 = l2.take_reply(r2);
	t.check(a1 && a2 && r1.code == net::kAccepted && r2.code == net::kAccepted && hl.get_int("color_1") == 5 && hl.observer_of(2) && l1.observer_of(2), "room over TCP: a colour and an observer request are granted and mirrored");
	l1.say_to(2, 2, "psst");
	pump();
	net::Lobby::ChatLine cl;
	t.check(l2.take_chat(cl) && cl.text == "psst" && cl.kind == 2 && !hl.take_chat(cl), "room over TCP: a whisper reaches its receiver only");
	hl.kick(2);
	host->disconnect(2);
	pump(100);
	t.check(l2.kicked() && !l1.kicked() && host->peers().size() == 1, "room over TCP: the host removes a guest: it is told, the connection ends, the other guest stays");
	hl.set("identity", "");
	t.check(hl.start(5) && hl.plan().players == 2, "room over TCP: START fixes the plan of the machines that remain");
}

// ---------------------------------------------------------------- internet play groundwork: host names, ports, the host's note (docs/multiplayer-spec.md 8.7)

void test_endpoints(Tally& t) {
	net::Endpoint ep;
	std::string err;
	bool ok = net::parse_endpoint("games.example.org:7790", 7777, ep, err) && ep.host == "games.example.org" && ep.port == 7790;
	ok = ok && net::parse_endpoint("  192.168.0.12  ", 7777, ep, err) && ep.host == "192.168.0.12" && ep.port == 7777;
	ok = ok && net::parse_endpoint("tcp://my-mac.local:27461", 7777, ep, err) && ep.host == "my-mac.local" && ep.port == 27461;
	t.check(ok, "join address: HOST, HOST:PORT, a name with a dash / dots, tcp:// and stray spaces parse; the default port is 7777");
	bool bad = !net::parse_endpoint("", 7777, ep, err) && !net::parse_endpoint(":7777", 7777, ep, err) && !net::parse_endpoint("host:80", 7777, ep, err) &&
	           !net::parse_endpoint("host:abc", 7777, ep, err) && !net::parse_endpoint("host:99999", 7777, ep, err) && !net::parse_endpoint("a b:7777", 7777, ep, err);
	t.check(bad, "join address: an empty host, a missing host, a port outside 1024..65535 or not a number, and a space inside the name are refused with a message", err);
	const std::vector<std::string> lo = net::resolve_ipv4("localhost", err), lit = net::resolve_ipv4("10.1.2.3", err);
	t.check(!lo.empty() && std::find(lo.begin(), lo.end(), "127.0.0.1") != lo.end() && lit.size() == 1 && lit[0] == "10.1.2.3", "DNS: localhost resolves to 127.0.0.1, an address literal to itself");
	std::string e2;
	const bool none = net::resolve_ipv4("no-such-host.invalid", e2).empty() && e2.find("cannot find the host") == 0;
	t.check(none, "DNS: an unknown name is reported as \"cannot find the host\" instead of hanging", e2);
	// a guest joins a room by host name and port; an unreachable address gives up within the timeout
	const uint16_t port = uint16_t(27460 + (getpid() % 39));
	std::string herr;
	std::unique_ptr<net::TcpTransport> host = net::TcpTransport::host_open(port, herr);
	if (!host) {
		t.check(false, "join by host name: listen on " + std::to_string(port), herr);
		return;
	}
	std::unique_ptr<net::TcpTransport> guest;
	std::string jerr;
	std::thread th([&]() { guest = net::TcpTransport::join("localhost", port, 5000, jerr); });
	for (int i = 0; i < 300 && !guest; ++i) {
		host->accept_new();
		host->idle(10);
	}
	th.join();
	t.check(guest && guest->local_peer() == 1 && host->peers().size() == 1, "join by host name: \"localhost\" + port reaches the room and gets slot 1", jerr);
	guest.reset();
	const auto t0 = std::chrono::steady_clock::now();
	std::string uerr;
	auto lost = net::TcpTransport::join("203.0.113.7", 27499, 600, uerr);   // TEST-NET-3: nobody answers
	const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
	t.check(!lost && ms < 3000 && uerr.find("cannot connect") == 0, "join: an address nobody answers at fails inside the timeout with a message (not the system's minutes)", uerr + ", " + std::to_string(ms) + " ms");
	const std::string note = net::public_host_note(7777);
	t.check(note.find("7777") != std::string::npos && note.find("forward") != std::string::npos && note.find("UPnP") != std::string::npos, "--public: the host's note names the port to forward and says there is no UPnP");
}

// ---------------------------------------------------------------- LAN discovery

void test_discovery(Tally& t) {
	const uint16_t port = uint16_t(41000 + (getpid() % 1000));
	net::LanGame g;
	g.name = "Alice";
	g.map = "Old World, Medium";
	g.port = 7777;
	g.players = 2;
	g.seats = 4;
	g.flags = net::kLanScenario;
	const std::vector<uint8_t> dg = net::encode_announcement(g);
	net::LanGame back;
	t.check(net::decode_announcement(dg.data(), dg.size(), back) && back.name == "Alice" && back.map == "Old World, Medium" && back.port == 7777 && back.players == 2 && back.seats == 4 &&
	            back.flags == net::kLanScenario && back.compatible(),
	        "discovery: an announcement survives the wire format");
	std::vector<uint8_t> bad = dg;
	bad[0] = 'X';
	t.check(!net::decode_announcement(bad.data(), bad.size(), back) && !net::decode_announcement(dg.data(), 6, back), "discovery: foreign or short datagrams are ignored");
	net::LanAnnouncer a;
	net::LanBrowser b;
	std::string err;
	if (!a.open(port, err) || !b.open(port, err)) {
		t.check(false, "discovery: sockets", err);
		return;
	}
	a.set(g);
	uint64_t now = 1000;
	const int sent = a.tick(now);
	bool found = false;
	for (int i = 0; i < 100 && !found; ++i) {
		b.poll(now);
		found = !b.games().empty();
		if (!found) usleep(10000);
	}
	t.check(sent >= 2 && found && b.games()[0].name == "Alice" && b.games()[0].port == 7777,
	        "discovery: a browser hears the host's announcement (UDP broadcast / multicast / loopback)", "datagrams sent " + std::to_string(sent) + (found ? ", from " + b.games()[0].address : ", none heard"));
	g.players = 3;
	a.set(g);
	a.tick(now + 1500);
	for (int i = 0; i < 100; ++i) {
		b.poll(now + 1500);
		if (!b.games().empty() && b.games()[0].players == 3) break;
		usleep(10000);
	}
	t.check(b.games().size() == 1 && b.games()[0].players == 3, "discovery: repeated announcements update one entry");
	b.poll(now + 20000);
	t.check(b.games().empty(), "discovery: a host that stops announcing disappears from the list");
}

// ---------------------------------------------------------------- CHAT_SET

void test_chat_set(Tally& t) {
	std::array<uint8_t, 8> st{};
	st[2] = 1, st[5] = 1;
	std::vector<uint8_t> w;
	sim::wire_chat_set(w, st);
	std::array<uint8_t, 8> back{};
	t.check(w.size() == 9 && w[0] == 69 && sim::replay_command_length(w.data(), w.size()) == 9 && sim::parse_chat_set(w.data(), w.size(), back) && back == st,
	        "CHAT_SET: 9 bytes on the wire, the table survives the codec");
}

}  // namespace

int cmd_selftest(int argc, char** argv) {
	uint32_t seed = 1;
	int trials = 12;
	std::string game = game_dir_default();
	bool engine = true;
	for (int i = 2; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--no-engine")) engine = false;
		if (i + 1 >= argc) continue;
		if (!std::strcmp(argv[i], "--seed")) seed = uint32_t(std::atoll(argv[i + 1]));
		if (!std::strcmp(argv[i], "--trials")) trials = std::atoi(argv[i + 1]);
		if (!std::strcmp(argv[i], "--game")) game = argv[i + 1];
	}
	Tally t;
	std::printf("ron-net selftest (seed %u)\n", seed);
	test_agreement(t, seed);
	test_stall(t, seed);
	test_protocol(t, seed);
	test_pause_speed(t, seed);
	test_resign(t, seed);
	test_drops(t, seed, trials);
	test_desync(t, seed);
	test_recovery(t, seed);
	test_no_wallclock(t, seed);
	test_lobby_rules(t);
	test_drop_with_observer(t, seed);
	test_seat_transport(t, seed);
	test_tcp_room(t);
	test_endpoints(t);
	test_discovery(t);
	test_chat_set(t);
	if (engine) test_engine(t, game, seed);
	std::printf("selftest: %d passed, %d failed\n", t.pass, t.fail);
	return t.fail ? 1 : 0;
}

}  // namespace ronnet
