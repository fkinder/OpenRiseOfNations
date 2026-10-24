// ron-net host | join: a real LAN game of N processes over TCP (engine/net/tcp.*). The host picks the settings and the seed in the lobby,
// every peer builds the same random game (new_random_game), and the lockstep session runs it. A small bot plays each human seat
// (so there are commands to synchronise); with --frames N the game stops at frame N and every process writes its native save,
// which must be byte-identical.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "commands.hpp"
#include "common.hpp"
#include "net/lobby.hpp"
#include "net/tcp.hpp"
#include "sim/anim_assets.hpp"
#include "sim/newgame.hpp"
#include "sim/replay.hpp"
#include "sim/rules.hpp"
#include "sim/rules_xml.hpp"
#include "sim/script_host.hpp"
#include "sim/save.hpp"

namespace ronnet {

namespace {

const char* arg_value(int argc, char** argv, const char* name) {
	for (int i = 3; i + 1 < argc; ++i)
		if (!std::strcmp(argv[i], name)) return argv[i + 1];
	return nullptr;
}
bool has_flag(int argc, char** argv, const char* name) {
	for (int i = 3; i < argc; ++i)
		if (!std::strcmp(argv[i], name)) return true;
	return false;
}

int32_t find_type_by_name(const sim::Game& g, const std::string& want) {
	auto low = [](std::string v) {
		for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
		return v;
	};
	const std::string w = low(want);
	for (size_t i = 0; i < g.rules->types.size(); ++i) {
		const sim::Type& t = g.rules->types[i];
		if (!t.loaded) continue;
		if (low(sim::to_utf8(t.type_name.empty() ? t.name : t.type_name)) == w) return int32_t(i);
	}
	return -1;
}

// A deterministic player: every few turns it trains a citizen at one of its buildings and sends its citizens walking. Local to the
// process that plays the seat; what it does reaches the others only through the packages.
class DemoBot {
public:
	DemoBot(const sim::Game& g, int seat) : g_(g), seat_(seat), rng_(0x9e3779b9u * uint32_t(seat + 1)) { citizen_ = find_type_by_name(g, "Citizen"); }

	net::Bytes next(uint32_t stamp) {
		net::Bytes out;
		if (stamp < 3) return out;
		const sim::ObjectList& ol = g_.objects[size_t(seat_)];
		if (stamp % 7 == 0) {  // train a citizen
			std::vector<uint16_t> bld;
			for (size_t i = 0; i < ol.builds.size() && bld.empty(); ++i) {
				const sim::Build* b = ol.builds[i].get();
				if (b && b->alive() && b->type == city_type()) bld.push_back(uint16_t(sim::kBuildBase + int(i)));
			}
			if (!bld.empty()) {
				sim::Command gr;
				gr.verb = sim::Verb::Group;
				gr.who = seat_;
				gr.objects = bld;
				sim::engine_to_wire(gr, out);
				sim::Command q;
				q.verb = sim::Verb::QueueUp;
				q.type = citizen_;
				q.num = 1;
				sim::engine_to_wire(q, out);
			}
		}
		if (stamp % 5 == 0) {  // walk
			std::vector<uint16_t> units;
			int32_t cx = 0, cy = 0;
			for (int pass = 0; pass < 2 && units.empty(); ++pass) {  // citizens first, else whatever the seat has
				for (int o = 0; o < ol.unit_mark && units.size() < 40; ++o) {
					const sim::Unit* u = o < int(ol.units.size()) ? ol.units[size_t(o)].get() : nullptr;
					if (!u || !u->alive() || u->o_up >= 0 || (pass == 0 && u->type != citizen_)) continue;
					units.push_back(uint16_t(o));
					cx = u->x;
					cy = u->y;
				}
			}
			if (!units.empty()) {
				const int32_t span = 768 * 12;
				sim::Command gr;
				gr.verb = sim::Verb::Group;
				gr.who = seat_;
				gr.objects = units;
				sim::engine_to_wire(gr, out);
				sim::Command m;
				m.verb = sim::Verb::MoveTo;
				m.x = std::max<int32_t>(768, std::min<int32_t>(g_.world.xs * 768 - 768, cx + int32_t(rng_.below(2 * span)) - span));
				m.y = std::max<int32_t>(768, std::min<int32_t>(g_.world.ys * 768 - 768, cy + int32_t(rng_.below(2 * span)) - span));
				m.orders = 1;
				m.queued = sim::kQueueNew;
				m.form = -1;
				m.width = -1;
				sim::engine_to_wire(m, out);
			}
		}
		return out;
	}

private:
	struct Lcg {
		uint32_t s;
		explicit Lcg(uint32_t seed) : s(seed) {}
		uint32_t below(uint32_t n) {
			s = s * 1664525u + 1013904223u;
			return n ? (s >> 8) % n : 0;
		}
	};
	int32_t city_type() {
		if (city_ < 0) city_ = find_type_by_name(g_, "City Center");
		return city_;
	}
	const sim::Game& g_;
	int seat_;
	Lcg rng_;
	int32_t citizen_ = -1, city_ = -1;
};

uint64_t fnv64(const std::vector<uint8_t>& b) {
	uint64_t h = 1469598103934665603ull;
	for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
	return h;
}

}  // namespace

int cmd_netgame(int argc, char** argv) {
	if (argc < 3) {
		std::fprintf(stderr, "usage: ron-net host <port> (--random STYLE,SIZE,SEED,NATIONS[,TRIBES..] | --scenario FILE [--seed S]) [--humans N] [--frames N] [--save F] [--game DIR]\n"
		                     "       ron-net join <port> [--address A] [--frames N] [--save F] [--game DIR]\n");
		return 64;
	}
	const bool host = !std::strcmp(argv[1], "host");
	const uint16_t port = uint16_t(std::atoi(argv[2]));
	const std::string game_dir = arg_value(argc, argv, "--game") ? arg_value(argc, argv, "--game") : game_dir_default();
	const int32_t frames = arg_value(argc, argv, "--frames") ? std::atoi(arg_value(argc, argv, "--frames")) : 600;
	const int humans = arg_value(argc, argv, "--humans") ? std::atoi(arg_value(argc, argv, "--humans")) : 2;
	const int timeout_ms = arg_value(argc, argv, "--timeout") ? std::atoi(arg_value(argc, argv, "--timeout")) * 1000 : 60000;
	const bool bot_on = !has_flag(argc, argv, "--no-bot");
	const char* tag = host ? "host" : "join";

	try {
		const ron::GameFiles files(game_dir);
		sim::Rules rules = sim::load_rules(files);
		sim::start_game(rules, sim::GameSetup());

		// ---- the connection and the lobby
		std::string err;
		std::unique_ptr<net::TcpTransport> tcp;
		sim::RandomGameSetup setup;
		std::string scenario_file;  // host: a scenario instead of a random map
		uint32_t scenario_seed = 0;
		if (host) {
			const char* spec = arg_value(argc, argv, "--random");
			const char* scen = arg_value(argc, argv, "--scenario");
			if (!spec && !scen) {
				std::fprintf(stderr, "host needs --random STYLE,SIZE,SEED,NATIONS or --scenario FILE\n");
				return 64;
			}
			if (spec) {
				setup = sim::default_random_setup(2);
				const std::string perr = sim::parse_random_option(rules, spec, setup);
				if (!perr.empty()) {
					std::fprintf(stderr, "--random: %s\n", perr.c_str());
					return 64;
				}
				if (humans < 1 || humans > 8 || humans > setup.num_nations) {
					std::fprintf(stderr, "--humans %d does not fit %d nations\n", humans, setup.num_nations);
					return 64;
				}
				for (int i = 0; i < 8; ++i) setup.nation[size_t(i)].human = i < humans;
				setup.game_speed = 2;
			} else {
				std::string f = scen;
				std::error_code ec;
				if (!std::filesystem::exists(f, ec)) f = files.resolve(f);
				if (f.empty()) {
					std::fprintf(stderr, "scenario not found: %s\n", scen);
					return 64;
				}
				scenario_file = sim::scenario_rel_path(files, f);
				scenario_seed = arg_value(argc, argv, "--seed") ? uint32_t(std::strtoul(arg_value(argc, argv, "--seed"), nullptr, 0))
				                                                 : uint32_t(std::chrono::steady_clock::now().time_since_epoch().count() & 0x7fffffff);
			}
			std::printf("[%s] listening on port %u for %d client(s)\n", tag, port, humans - 1);
			tcp = net::TcpTransport::host(port, humans - 1, timeout_ms, err);
		} else {
			const char* addr = arg_value(argc, argv, "--address") ? arg_value(argc, argv, "--address") : "127.0.0.1";
			tcp = net::TcpTransport::join(addr, port, timeout_ms, err);
		}
		if (!tcp) {
			std::fprintf(stderr, "[%s] %s\n", tag, err.c_str());
			return 2;
		}
		net::Transport& t = *tcp;
		const int me = t.local_peer();
		std::printf("[%s] connected: slot %d of %d\n", tag, me, tcp->peer_count());

		net::Lobby lobby(t);
		if (host) {
			if (scenario_file.empty()) {
				net::setup_to_lobby(setup, lobby);
				lobby.start(setup.seed);  // the host's seed becomes game_seed
			} else {
				lobby.set("scenario", scenario_file);
				lobby.set("humans", humans);
				lobby.start(scenario_seed);
			}
		}
		const auto t0 = std::chrono::steady_clock::now();
		while (!lobby.started()) {
			lobby.poll();
			t.idle(5);
			if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeout_ms)) {
				std::fprintf(stderr, "[%s] the host never started the game\n", tag);
				return 2;
			}
		}

		// ---- the same game on every peer
		auto art = sim::load_anim_source(files, rules);
		std::unique_ptr<sim::Game> game;
		std::string scenario;
		uint32_t seed = 0;
		int nseats = 2, nhumans = humans;
		sim::RandomExtras extras;
		if (lobby.has("scenario")) {
			scenario = lobby.get("scenario");
			seed = lobby.seed();
			nhumans = int(lobby.get_int("humans", 2));
			const std::string file = std::filesystem::exists(scenario) ? scenario : files.resolve(scenario);
			if (file.empty()) {
				std::fprintf(stderr, "[%s] scenario %s is not in this install\n", tag, scenario.c_str());
				return 2;
			}
			const ron::Scenario sc = ron::parse_scenario(ron::read_file(file));
			game = std::make_unique<sim::Game>();
			game->anims = art.get();
			game->load(files, sc, rules, seed);
			sim::script_host_load(*game, files, file, sc);
			nseats = nhumans;
			std::printf("[%s] lobby: scenario %s seed %u humans %d\n", tag, scenario.c_str(), seed, nhumans);
		} else {
			if (!net::lobby_to_setup(lobby, setup, err)) {
				std::fprintf(stderr, "[%s] %s\n", tag, err.c_str());
				return 2;
			}
			std::printf("[%s] lobby: style %d size %d seed %u nations %d\n", tag, setup.map_style, setup.map_size, setup.seed, setup.num_nations);
			game = sim::new_random_game(files, rules, art.get(), setup, &extras);
			if (!game) {
				std::fprintf(stderr, "[%s] the random game could not be built: %s\n", tag, extras.error.c_str());
				return 2;
			}
			scenario = sim::random_setup_string(extras.setup);
			seed = extras.setup.seed;
			nseats = setup.num_nations;
			nhumans = 0;
			for (int i = 0; i < setup.num_nations; ++i) nhumans += setup.nation[size_t(i)].human;
		}

		lobby.barrier(1);  // SyncPoint: nobody starts before everybody has its game
		while (!lobby.barrier_done(1)) {
			lobby.poll();
			t.idle(5);
		}

		std::array<int, 8> who{};
		for (int i = 0; i < 8; ++i) who[size_t(i)] = i;
		net::GameHooks hooks(*game, who, net::ChecksumMode::Native, net::ExecPolicy::all_engine(), scenario, seed);
		net::SessionConfig cfg;
		cfg.local_slot = me;
		cfg.seed = seed;
		cfg.speed = 2;
		cfg.timeout_ms = 20000;
		for (int i = 0; i < std::min(nseats, 8); ++i) cfg.slot_flags[size_t(i)] = net::kPlayerValid | (i < nhumans ? net::kPlayerHuman : 0u);
		net::Session session(t, hooks, cfg);
		uint32_t boundaries = 0;
		session.on_boundary = [&](const net::BoundaryInfo&) { ++boundaries; };
		session.on_drop = [&](int slot, int option, uint32_t boundary) {
			std::printf("[%s] player %d dropped (option %d) at turn %u, frame %d\n", tag, slot, option, boundary, game->frame);
		};
		session.on_resign = [&](int slot) { std::printf("[%s] player %d resigned at frame %d\n", tag, slot, game->frame); };
		DemoBot bot(*game, me);
		uint32_t fed = 0;
		auto feed = [&]() {
			while (fed < session.local_stamp() + 1) {
				++fed;
				if (bot_on) session.submit(bot.next(fed));
			}
		};
		feed();
		session.start();

		// ---- play
		uint64_t stalls = 0;
		int rc = 0;
		while (game->frame < frames) {
			feed();
			const net::StepResult r = session.step();
			if (r.kind == net::StepKind::Advanced) continue;
			if (r.kind == net::StepKind::Stalled) {
				++stalls;
				t.idle(1);
				continue;
			}
			std::fprintf(stderr, "[%s] stopped at frame %d: %s%s\n", tag, game->frame, r.kind == net::StepKind::Desync ? "DESYNC " : "", r.text.c_str());
			rc = 3;
			break;
		}
		std::printf("[%s] frame %d after %u turns (%llu waits), turn length %d, stamp %u\n", tag, game->frame, boundaries, (unsigned long long)stalls, session.turn_length(),
		            session.local_stamp());

		// ---- the end state
		if (rc == 0) {
			const std::vector<uint8_t> bytes = sim::save_game_bytes(*game, scenario, seed);
			uint32_t sums[16];
			hooks.checksums(sums);
			std::printf("[%s] native save: %zu bytes, fnv64 %016llx, state sum %u, commands run %llu\n", tag, bytes.size(), (unsigned long long)fnv64(bytes), sums[15],
			            (unsigned long long)hooks.commands_run);
			if (const char* out = arg_value(argc, argv, "--save")) {
				std::ofstream f(out, std::ios::binary);
				f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
			}
		}
		// nobody closes before everybody is done: a close with unread data would reset the connection and lose what is in flight
		lobby.barrier(2);
		const auto t1 = std::chrono::steady_clock::now();
		while (!lobby.barrier_done(2) && std::chrono::steady_clock::now() - t1 < std::chrono::seconds(10)) {
			lobby.poll();
			t.idle(5);
		}
		return rc;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "[%s] error: %s\n", tag, e.what());
		return 2;
	}
}

}  // namespace ronnet
