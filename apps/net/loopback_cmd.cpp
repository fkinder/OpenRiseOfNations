// ron-net loopback: N engine instances of one random game in this process, joined by the loopback transport, each played by a demo bot
// (so there are commands). Options inject a divergence into one instance (the OOS recovery must repair it), record every instance as
// a `.rcx` (net/record.hpp) and write the final native save. The same runner serves `ron-net selftest` (engine part).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "commands.hpp"
#include "common.hpp"
#include "demobot.hpp"
#include "loopback.hpp"
#include "net/seat_transport.hpp"
#include "node.hpp"
#include "sim/anim_assets.hpp"
#include "sim/save.hpp"
#include "sim/commands.hpp"
#include "sim/replay.hpp"
#include "sim/script_host.hpp"
#include "sim/script_ui.hpp"
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
		// a scenario game: the seats are the scenario's players, the human-flagged ones first (the lobby's order, apps/map-mac/lobby.mm)
		const bool scen = !o.scenario.empty();
		ron::Scenario sc0;
		std::string scen_file, scen_rel;
		std::array<int, 8> scen_who{};
		if (scen) {
			scen_file = std::filesystem::exists(o.scenario) ? o.scenario : env.files->resolve(o.scenario);
			if (scen_file.empty()) {
				res.text = "scenario not found: " + o.scenario;
				return res;
			}
			scen_rel = sim::scenario_rel_path(*env.files, scen_file);
			sc0 = ron::parse_scenario(ron::read_file(scen_file));
			std::vector<const ron::ScenarioPlayer*> ps;
			for (const ron::ScenarioPlayer& p : sc0.players)
				if (p.who >= 0 && p.who < 8) ps.push_back(&p);
			std::stable_sort(ps.begin(), ps.end(), [](const ron::ScenarioPlayer* a, const ron::ScenarioPlayer* b) { return (a->is_human != 0) > (b->is_human != 0); });
			setup = sim::default_random_setup(std::max<int>(2, int(ps.size())));
			for (int k = 0; k < 8; ++k) scen_who[size_t(k)] = k < int(ps.size()) ? ps[size_t(k)]->who : k;
		} else {
			const std::string perr = sim::parse_random_option(*env.rules, o.random_spec, setup);
			if (!perr.empty()) {
				res.text = "--random: " + perr;
				return res;
			}
		}
		const int n = o.humans;
		if (n < 1 || n > setup.num_nations) {
			res.text = "humans does not fit the nations";
			return res;
		}
		for (int i = 0; i < 8; ++i) setup.nation[size_t(i)].human = i < n;
		setup.game_speed = 2;
		const int nobs = std::clamp(o.observers, 0, 8 - setup.num_nations);
		net::LoopbackHub hub(8, o.hub_seed, o.dlo, o.dhi);
		for (int p = n + nobs; p < 8; ++p) hub.kill(p);
		std::array<int, 8> who{};
		for (int i = 0; i < 8; ++i) who[size_t(i)] = scen ? scen_who[size_t(i)] : i;
		// the room: machines 0..n-1 play (game slot = machine), machines n.. watch from the slots above the nations
		const uint32_t present = (1u << (n + nobs)) - 1, obs_mask = ((1u << nobs) - 1) << n;
		const net::SeatPlan plan = net::make_seat_plan(present, obs_mask, setup.num_nations, true);
		net::MpSeats mpseats;
		mpseats.plan = plan;
		for (int s = 0; s < 8; ++s) {
			mpseats.name[size_t(s)] = "Player " + std::to_string(s + 1);
			mpseats.handicap[size_t(s)] = size_t(s) < o.handicaps.size() ? o.handicaps[size_t(s)] : 0;
		}
		struct Peer {
			std::unique_ptr<net::SeatTransport> tr;
			std::unique_ptr<EngineNode> node;
			std::unique_ptr<net::Session> session;
			std::unique_ptr<DemoBot> bot;
			uint32_t fed = 0, epoch = 0;
			bool done = false, injected = false;
			uint64_t resent = 0;
		};
		std::vector<Peer> peers(static_cast<size_t>(n + nobs));
		std::string scenario;
		uint32_t seed = 0;
		for (int i = 0; i < n + nobs; ++i) {
			const int slot = plan.slot_of[size_t(i)];
			const bool watcher = slot >= plan.nations;
			sim::RandomExtras extras;
			std::unique_ptr<sim::Game> g;
			if (scen) {
				ron::Scenario sc = sc0;
				for (ron::ScenarioPlayer& pl : sc.players) {
					int seat = -1;
					for (int k = 0; k < setup.num_nations; ++k)
						if (scen_who[size_t(k)] == pl.who) seat = k;
					pl.is_human = seat >= 0 && seat < n ? 4 : 0;
				}
				g = std::make_unique<sim::Game>();
				g->anims = env.art.get();
				g->load(*env.files, sc, *env.rules, 12345);
				sim::script_host_load(*g, *env.files, scen_file, sc);
				extras.setup = setup;
				extras.setup.seed = 12345;
			} else {
				g = sim::new_random_game(*env.files, *env.rules, env.art.get(), setup, &extras);
			}
			if (!g) {
				res.text = "the game could not be built: " + extras.error;
				return res;
			}
			net::apply_mp_seats(*g, who, mpseats);   // a multiplayer game: names, handicaps (the same on every machine)
			g->local_play = watcher ? who[0] : slot;   // the seat this machine looks from (an observer watches from the first seat)
			scenario = scen ? scen_rel : sim::random_setup_string(extras.setup);
			seed = extras.setup.seed;
			Peer& pr = peers[size_t(i)];
			pr.node = std::make_unique<EngineNode>(*env.files, *env.rules, env.art.get(), std::move(g), scenario, seed, who);
			net::SessionConfig cfg;
			cfg.local_slot = slot;
			cfg.seed = seed;
			cfg.speed = 2;
			cfg.recover = o.recover;
			cfg.timeout_ms = 0;
			for (int s = 0; s < 8; ++s) {
				if (s < setup.num_nations) cfg.slot_flags[size_t(s)] = net::kPlayerValid | (s < n ? net::kPlayerHuman : 0u);
				else if (plan.peer_of[size_t(s)] >= 0) cfg.slot_flags[size_t(s)] = net::kPlayerValid | net::kPlayerObserver;
			}
			pr.tr = std::make_unique<net::SeatTransport>(hub.endpoint(i), plan.peer_of);
			pr.session = std::make_unique<net::Session>(*pr.tr, pr.node->hooks(), cfg);
			pr.node->attach(*pr.session);
			EngineNode* nd = pr.node.get();
			if (!watcher) pr.bot = std::make_unique<DemoBot>([nd]() -> const sim::Game& { return nd->game(); }, who[size_t(slot)]);
			if (!o.record_base.empty() && !watcher) {
				const std::string base = o.record_base + "_peer" + std::to_string(i) + ".rcx";
				net::RecordInfo info = net::record_info_for_random(extras.setup, seed, i, who, nd->game(), scenario);
				for (int sl = 0; sl < setup.num_nations && sl < 8; ++sl) {
					info.seats[size_t(sl)].handicap = mpseats.handicap[size_t(sl)];
					if (info.seats[size_t(sl)].human) info.seats[size_t(sl)].name = mpseats.name[size_t(sl)];
				}
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
				if (o.bots && p.bot) p.session->submit(p.bot->next(p.fed));
				if (o.chat_test && &p == &peers[0] && p.fed == 30) {  // a line to everybody and a line to seats 0 and 1 (CHAT bits, docs/replay-spec.md section 9)
					for (int team = 0; team < 2; ++team) {
						sim::Command c;
						c.verb = sim::Verb::Chat;
						c.text = team ? u"for my team only" : u"hello all";
						c.chat_bits = team ? 3 : -1;
						net::Bytes b;
						sim::engine_to_wire(c, b);
						p.session->submit(b);
					}
				}
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
		res.handicap_seen = n > 1 ? peers[0].node->game().leaders[1].handicap : -1;
		if (o.chat_test) {
			// every instance recorded both lines in its console and queued the same presentation events (the line to a recipient mask used to reach only
			// the seats in the mask, which split the saved state: GameHooks::execute); who sees what is the application's filter at display time
			std::string rep = "ok";
			for (size_t i = 0; i < peers.size(); ++i) {
				const sim::Game& gm = peers[i].node->game();
				int lines = 0, hello = 0, team = 0, masked = 0;
				for (const auto& ln : gm.ctl.chat) {
					lines += ln.text == "hello all" || ln.text == "for my team only";
					masked += ln.text == "for my team only" && ln.bits == 3u;
				}
				if (const sim::ScriptPresentation* sp = sim::script_presentation(gm))
					for (const sim::ScriptEvent& e : sp->events)
						if (e.kind == sim::ScriptEvent::kChat) hello += e.text == "hello all", team += e.text == "for my team only";
				if (lines != 2 || masked != 1 || hello != 1 || team != 1)
					rep = "instance " + std::to_string(i) + ": console lines " + std::to_string(lines) + " (mask kept " + std::to_string(masked) + "), events hello " + std::to_string(hello) + " team " + std::to_string(team);
			}
			res.chat_report = rep;
		}
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
		else if (const char* vs = val("--scenario")) o.scenario = vs;
		else if (const char* vo = val("--observers")) o.observers = std::atoi(vo);
		else if (const char* vh = val("--handicap")) {
			for (const char* q = vh; *q;) {
				o.handicaps.push_back(std::atoi(q));
				while (*q && *q != ',') ++q;
				if (*q == ',') ++q;
			}
		}
		else if (!std::strcmp(argv[i], "--chat-test")) o.chat_test = true;
		else if (!std::strcmp(argv[i], "--recover")) o.recover = true;
		else if (!std::strcmp(argv[i], "--no-bot")) o.bots = false;
		else if (!std::strcmp(argv[i], "--verbose")) o.verbose = true;
	}
	if (o.random_spec.empty() && o.scenario.empty()) {
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
