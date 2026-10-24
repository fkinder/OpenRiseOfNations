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
#include <string>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include "commands.hpp"
#include "common.hpp"
#include "loopback.hpp"
#include "sim/replay.hpp"

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
	if (engine) test_engine(t, game, seed);
	std::printf("selftest: %d passed, %d failed\n", t.pass, t.fail);
	return t.fail ? 1 : 0;
}

}  // namespace ronnet
