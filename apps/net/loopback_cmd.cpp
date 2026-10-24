// ron-net loopback: N engine instances of one random game in this process, joined by the loopback transport, each played by a demo bot
// (so there are commands). Options inject a divergence into one instance (the OOS recovery must repair it), record every instance as
// a `.rcx` (net/record.hpp) and write the final native save. The same runner serves `ron-net selftest` (engine part).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "commands.hpp"
#include "common.hpp"
#include "demobot.hpp"
#include "loopback.hpp"
#include "node.hpp"
#include "sim/anim_assets.hpp"
#include "sim/newgame.hpp"
#include "sim/rules.hpp"
#include "sim/rules_xml.hpp"

namespace ronnet {

namespace {

struct Env {
	std::unique_ptr<ron::GameFiles> files;
	std::unique_ptr<sim::Rules> rules;
	std::unique_ptr<sim::AssetAnimSource> art;
};

Env& env_for(const std::string& dir) {
	static std::unique_ptr<Env> e;
	static std::string loaded;
	if (!e || loaded != dir) {
		e = std::make_unique<Env>();
		e->files = std::make_unique<ron::GameFiles>(dir);
		e->rules = std::make_unique<sim::Rules>(sim::load_rules(*e->files));
		sim::start_game(*e->rules, sim::GameSetup());
		e->art = sim::load_anim_source(*e->files, *e->rules);
		loaded = dir;
	}
	return *e;
}

}  // namespace

LoopbackResult run_loopback(const LoopbackOptions& o) {
	LoopbackResult res;
	try {
		Env& env = env_for(o.game_dir);
		sim::RandomGameSetup setup = sim::default_random_setup(2);
		const std::string perr = sim::parse_random_option(*env.rules, o.random_spec, setup);
		if (!perr.empty()) {
			res.text = "--random: " + perr;
			return res;
		}
		const int n = o.humans;
		if (n < 1 || n > setup.num_nations) {
			res.text = "humans does not fit the nations";
			return res;
		}
		for (int i = 0; i < 8; ++i) setup.nation[size_t(i)].human = i < n;
		setup.game_speed = 2;
		net::LoopbackHub hub(8, o.hub_seed, o.dlo, o.dhi);
		for (int p = n; p < 8; ++p) hub.kill(p);
		std::array<int, 8> who{};
		for (int i = 0; i < 8; ++i) who[size_t(i)] = i;
		struct Peer {
			std::unique_ptr<EngineNode> node;
			std::unique_ptr<net::Session> session;
			std::unique_ptr<DemoBot> bot;
			uint32_t fed = 0, epoch = 0;
			bool done = false, injected = false;
			uint64_t resent = 0;
		};
		std::vector<Peer> peers(static_cast<size_t>(n));
		std::string scenario;
		uint32_t seed = 0;
		for (int i = 0; i < n; ++i) {
			sim::RandomExtras extras;
			std::unique_ptr<sim::Game> g = sim::new_random_game(*env.files, *env.rules, env.art.get(), setup, &extras);
			if (!g) {
				res.text = "the random game could not be built: " + extras.error;
				return res;
			}
			scenario = sim::random_setup_string(extras.setup);
			seed = extras.setup.seed;
			Peer& pr = peers[size_t(i)];
			pr.node = std::make_unique<EngineNode>(*env.files, *env.rules, env.art.get(), std::move(g), scenario, seed, who);
			net::SessionConfig cfg;
			cfg.local_slot = i;
			cfg.seed = seed;
			cfg.speed = 2;
			cfg.recover = o.recover;
			cfg.timeout_ms = 0;
			for (int s = 0; s < setup.num_nations; ++s) cfg.slot_flags[size_t(s)] = net::kPlayerValid | (s < n ? net::kPlayerHuman : 0u);
			pr.session = std::make_unique<net::Session>(hub.endpoint(i), pr.node->hooks(), cfg);
			pr.node->attach(*pr.session);
			EngineNode* nd = pr.node.get();
			pr.bot = std::make_unique<DemoBot>([nd]() -> const sim::Game& { return nd->game(); }, i);
			if (!o.record_base.empty()) {
				const std::string base = o.record_base + "_peer" + std::to_string(i) + ".rcx";
				const net::RecordInfo info = net::record_info_for_random(extras.setup, seed, i, who, nd->game(), scenario);
				std::string err;
				if (!pr.node->record(base, info, err)) {
					res.text = err;
					return res;
				}
			}
		}
		auto feed = [&](Peer& p) {
			if (p.epoch != p.session->epoch()) {  // the stamps restarted
				p.epoch = p.session->epoch();
				p.fed = 0;
			}
			while (p.fed < p.session->local_stamp() + 1) {
				++p.fed;
				if (o.bots) p.session->submit(p.bot->next(p.fed));
			}
		};
		for (Peer& p : peers) {
			feed(p);
			p.session->start();
		}
		uint64_t idle = 0, rounds = 0;
		std::string stop;
		for (;;) {
			bool all = true, progressed = false;
			for (size_t i = 0; i < peers.size(); ++i) {
				Peer& p = peers[i];
				if (p.done) continue;
				all = false;
				for (const auto& dv : o.diverge)
					if (dv.second == int(i) && !p.injected && p.node->game().frame >= dv.first) {
						p.injected = true;
						p.node->game().leaders[i].units_built += 12345;  // a field of the saved state: the check sums see it
						if (o.verbose) std::printf("  peer %zu: divergence injected at frame %d\n", i, p.node->game().frame);
					}
				feed(p);
				const net::StepResult r = p.session->step();
				switch (r.kind) {
				case net::StepKind::Advanced:
					progressed = true;
					if (r.turn && p.node->game().frame >= o.frames) p.done = true;
					break;
				case net::StepKind::Stalled: break;
				case net::StepKind::Recovering: progressed = true; break;
				case net::StepKind::Recovered:
					progressed = true;
					++res.recovered_events;
					if (o.verbose) std::printf("  peer %zu: recovered (epoch %u) at frame %d\n", i, p.session->epoch(), p.node->game().frame);
					break;
				default:
					p.done = true;
					stop = "peer " + std::to_string(i) + " stopped: " + (r.kind == net::StepKind::Desync ? "DESYNC " : "") + r.text;
					break;
				}
			}
			hub.advance(1);
			++rounds;
			if (o.verbose && rounds % 20000 == 0)
				for (size_t i = 0; i < peers.size(); ++i)
					std::printf("  [round %llu] peer %zu: frame %d boundaries %u stamp %u epoch %u in_turn %d paused %d turn_length %d done %d\n", (unsigned long long)rounds, i,
					            peers[i].node->game().frame, peers[i].session->boundaries(), peers[i].session->local_stamp(), peers[i].session->epoch(),
					            int(peers[i].session->in_turn()), int(peers[i].session->paused()), peers[i].session->turn_length(), int(peers[i].done));
			if (all || !stop.empty()) break;
			if (!progressed) {
				if (++idle > o.deadlock_rounds) {
					stop = "deadlock: no peer can advance";
					if (o.verbose)
						for (size_t i = 0; i < peers.size(); ++i)
							std::printf("  peer %zu: frame %d boundaries %u stamp %u epoch %u in_turn %d paused %d\n", i, peers[i].node->game().frame, peers[i].session->boundaries(),
							            peers[i].session->local_stamp(), peers[i].session->epoch(), int(peers[i].session->in_turn()), int(peers[i].session->paused()));
					break;
				}
			} else {
				idle = 0;
			}
		}
		for (Peer& p : peers) p.node->finish_recording();
		res.rounds = rounds;
		res.peers = n;
		res.frame = peers[0].node->game().frame;
		res.epoch = peers[0].session->epoch();
		bool same = true;
		std::vector<uint8_t> first;
		for (size_t i = 0; i < peers.size(); ++i) {
			std::vector<uint8_t> b = peers[i].node->save_bytes();
			res.fnv.push_back(fnv64(b));
			res.recoveries += peers[i].node->recoveries;
			if (i == 0) first = std::move(b);
			else if (b != first) same = false;
			if (peers[i].session->epoch() != peers[0].session->epoch() || peers[i].node->game().frame != res.frame) same = false;
			for (const std::string& sgm : peers[i].node->segments()) res.segments[int(i)].push_back(sgm);
			if (!peers[i].node->error.empty()) stop = "peer " + std::to_string(i) + ": " + peers[i].node->error;
		}
		res.identical = same;
		res.final_fnv = res.fnv.empty() ? 0 : res.fnv[0];
		if (!o.save_out.empty() && !first.empty()) {
			std::ofstream f(o.save_out, std::ios::binary);
			f.write(reinterpret_cast<const char*>(first.data()), std::streamsize(first.size()));
		}
		res.scenario = scenario;
		res.seed = seed;
		res.ok = stop.empty() && same;
		res.text = stop.empty() ? (same ? "ok" : "the final states differ") : stop;
	} catch (const std::exception& e) {
		res.text = std::string("error: ") + e.what();
	}
	return res;
}

int cmd_loopback(int argc, char** argv) {
	LoopbackOptions o;
	o.game_dir = game_dir_default();
	for (int i = 2; i < argc; ++i) {
		auto val = [&](const char* name) -> const char* { return !std::strcmp(argv[i], name) && i + 1 < argc ? argv[++i] : nullptr; };
		if (const char* v = val("--random")) o.random_spec = v;
		else if (const char* v2 = val("--humans")) o.humans = std::atoi(v2);
		else if (const char* v3 = val("--frames")) o.frames = std::atoi(v3);
		else if (const char* v4 = val("--delay")) std::sscanf(v4, "%d,%d", &o.dlo, &o.dhi);
		else if (const char* v5 = val("--diverge")) {
			int f = 0, p = 1;
			std::sscanf(v5, "%d:%d", &f, &p);
			o.diverge.emplace_back(f, p);
		} else if (const char* v6 = val("--record")) o.record_base = v6;
		else if (const char* v7 = val("--save")) o.save_out = v7;
		else if (const char* v8 = val("--game")) o.game_dir = v8;
		else if (const char* v9 = val("--hub-seed")) o.hub_seed = uint32_t(std::atoll(v9));
		else if (const char* v10 = val("--deadlock-rounds")) o.deadlock_rounds = uint64_t(std::atoll(v10));
		else if (!std::strcmp(argv[i], "--recover")) o.recover = true;
		else if (!std::strcmp(argv[i], "--no-bot")) o.bots = false;
		else if (!std::strcmp(argv[i], "--verbose")) o.verbose = true;
	}
	if (o.random_spec.empty()) {
		std::fprintf(stderr, "usage: ron-net loopback --random STYLE,SIZE,SEED,NATIONS[,TRIBES..] [--humans N] [--frames F] [--delay a,b] [--recover]\n"
		                     "         [--diverge FRAME:PEER]... [--record BASE] [--save FILE] [--game DIR] [--verbose]\n");
		return 64;
	}
	const LoopbackResult r = run_loopback(o);
	std::printf("loopback: %d peer(s), frame %d, %llu rounds, %d snapshot load(s) (%u recovery event(s)), epoch %u: %s\n", r.peers, r.frame, (unsigned long long)r.rounds,
	            r.recoveries, r.recovered_events, r.epoch, r.text.c_str());
	for (size_t i = 0; i < r.fnv.size(); ++i) std::printf("  peer %zu native save fnv64 %016llx\n", i, (unsigned long long)r.fnv[i]);
	for (const auto& kv : r.segments) {
		std::printf("  peer %d recording:", kv.first);
		for (const std::string& s : kv.second) std::printf(" %s", s.c_str());
		std::printf("\n");
	}
	return r.ok ? 0 : 1;
}

}  // namespace ronnet
