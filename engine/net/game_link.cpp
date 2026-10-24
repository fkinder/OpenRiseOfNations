#include "net/game_link.hpp"

#include <algorithm>
#include <cstring>

#include "sim/datawalk.hpp"
#include "sim/capture.hpp"
#include "sim/datawalk_engine.hpp"
#include "sim/replay.hpp"
#include "sim/save.hpp"

namespace ron::net {

namespace {
bool is_aircraft_verb(int op) { return op == 9 || op == 10 || op == 11 || op == 28 || op == 35 || op == 36; }
// The console / turn-control verbs ron-sav does not feed (sim/commands_extra.cpp): HOTKEY, COME_OUT, SPEED_*, the cheats, CHAT, RENAME_CITY, PAUSE.
bool is_console_verb(int op) {
	return op == 34 || op == 49 || (op >= 52 && op <= 54) || (op >= 59 && op <= 68) || op == 75 || op == 76;
}
// Commands the instance never counts as unsupported: the lockstep layer's own and presentation (apps/sav/main.cpp's list).
bool is_quiet_verb(int op) {
	return op == sim::op::kTurnData || op == sim::op::kCamera || op == sim::op::kCheckSums || op == sim::op::kPlayerSpeed;
}
}  // namespace

ExecPolicy ExecPolicy::all_engine() {
	ExecPolicy p;
	p.player_exit = true;
	for (int op = 0; op < 128; ++op) p.allow[size_t(op)] = sim::replay_has_engine_verb(op);
	return p;
}

ExecPolicy ExecPolicy::sav_compatible() {
	ExecPolicy p = all_engine();
	p.player_exit = false;
	for (int op = 0; op < 128; ++op)
		if (is_aircraft_verb(op) || is_console_verb(op)) p.allow[size_t(op)] = false;
	return p;
}

GameHooks::GameHooks(sim::Game& g, const std::array<int, 8>& seat_who, ChecksumMode mode, ExecPolicy policy,
                     const std::string& scenario_rel, uint32_t seed)
	: g_(&g), who_(seat_who), mode_(mode), policy_(policy), scenario_(scenario_rel), seed_(seed) {}

void GameHooks::begin_package(int, uint32_t) { package_.group = -1; }

void GameHooks::step_frame() {
	if (before_frame) before_frame();
	g_->do_frame();
	if (after_frame) after_frame();
}

bool GameHooks::snapshot(Bytes& out) {
	try {
		out = sim::save_game_bytes(*g_, scenario_, seed_);
	} catch (const std::exception& e) {
		last_error = e.what();
		return false;
	}
	return true;
}

bool GameHooks::restore(const Bytes& snap, std::string& err) {
	if (!on_restore) {
		err = "no restore callback";
		return false;
	}
	package_ = sim::CommandPackage();  // the group state of the old game means nothing in the new one
	return on_restore(snap, err);
}

// Player::resign / quit / drop on the leader of a seat (multiplayer-spec section 3.7). Co-op seats that share a leader are not modelled.
void GameHooks::player_exit(int op, const uint8_t* cmd, size_t len, int sender) {
	int seat = sender;
	int option = -1;
	if (op == sim::op::kUngracefulDrop) {
		if (len != 3) return;
		seat = cmd[1];
		option = cmd[2];
	} else if (len >= 5) {
		int32_t v;
		std::memcpy(&v, cmd + 1, 4);
		if (v >= 0 && v < 8) seat = v;
	}
	const int who = who_[size_t(seat & 7)];
	if (who < 0 || who >= sim::kNumLeaders) return;
	if (option == 3) {  // REPLACE_WITH_AI: the leader stays and the computer plays it
		g_->leaders[size_t(who)].flags &= ~sim::leaderflag::kHuman;
		return;
	}
	sim::leader_defeat(*g_, who, option >= 0 ? 7 : 6, -1, 0);  // reasons: 6 resigned, 7 disconnected
}

void GameHooks::execute(int slot, const uint8_t* cmd, size_t len) {
	const int op = cmd[0];
	if (policy_.player_exit && (op == sim::op::kResign || op == sim::op::kQuit || op == sim::op::kUngracefulDrop)) {
		player_exit(op, cmd, len, slot);
		++commands_run;
		return;
	}
	if (is_quiet_verb(op)) return;
	if ((op == sim::op::kPause || op == sim::op::kHotkey) && !policy_.allow[size_t(op & 127)]) return;  // presentation for ron-sav
	sim::Command c;
	if (!policy_.execute || !policy_.allow[size_t(op & 127)] || !sim::replay_to_engine(cmd, len, c)) {
		++unsupported[op];
		if (first_unsupported_frame < 0) {
			first_unsupported_frame = g_->frame;
			first_unsupported_op = op;
		}
		return;
	}
	// CHAT: the engine pushes a presentation event (saved, so checksummed) only when the line is for the local leader; with a recipient mask that
	// would make the machines of the players outside the mask differ from the others (an out-of-sync report on the first team chat). The line is
	// processed as if the local leader were its sender (always in its own mask), so every machine records the same event; the application shows it
	// only to the recipients (apps/map-mac/main.mm mpFilteredPresentation reads the mask from the console's chat list).
	const int32_t keep_local = g_->local_play;
	if (op == sim::op::kChat) g_->local_play = who_[size_t(slot & 7)];
	package_.process(*g_, c, slot, who_[size_t(slot & 7)]);
	g_->local_play = keep_local;
	++commands_run;
}

bool GameHooks::checksums(uint32_t out[16]) {
	uint32_t sum = 0;
	if (mode_ == ChecksumMode::Original) {
		sim::EngineProvider ep(*g_);
		sim::datawalk::CheckSumParts parts = sim::datawalk::compute_check_sums(ep);
		if (!parts.ok) {
			last_error = parts.error;
			return false;
		}
		for (int k = 0; k < sim::datawalk::kCheckPartCount; ++k) {
			out[k] = parts.v[k];
			sum += parts.v[k];
		}
	} else {
		// The native save is "RONS" u32 version u32 flags, then sections: tag(4) u64 length u32 crc payload. The stored CRC-32 of
		// each section (but META and END!) is one part: 15 sections with scripts (INFO RULE CORE WRLD REGN LEAD OBJS GOOD CITY GRUP
		// PATH COMB AIST SCEN SCRP).
		std::vector<uint8_t> b;
		try {
			b = sim::save_game_bytes(*g_, scenario_, seed_);
		} catch (const std::exception& e) {
			last_error = e.what();
			return false;
		}
		for (int k = 0; k < 16; ++k) out[k] = 0;
		size_t p = 12;
		int k = 0;
		while (p + 16 <= b.size()) {
			uint64_t n;
			uint32_t crc;
			std::memcpy(&n, b.data() + p + 4, 8);
			std::memcpy(&crc, b.data() + p + 12, 4);
			const bool skip = !std::memcmp(b.data() + p, "META", 4) || !std::memcmp(b.data() + p, "END!", 4);
			if (!skip && k < 15) {
				out[k++] = crc;
				sum += crc;
			}
			if (n > b.size()) break;
			p += 16 + size_t(n);
		}
	}
	out[15] = sum;
	return true;
}

// ---------------------------------------------------------------- lobby <-> setup

namespace {
std::string key_n(const char* base, int n) { return std::string(base) + "_" + std::to_string(n); }
}  // namespace

void setup_to_lobby(const sim::RandomGameSetup& s, Lobby& l) {
	l.set("map_style", s.map_style);
	l.set("map_size", s.map_size);
	l.set("num_nations", s.num_nations);
	l.set("gamerules", s.game_rules);
	l.set("gamespeed", s.game_speed);
	l.set("pop_limit", s.pop_limit);
	l.set("tech_cost", s.tech_cost);
	l.set("rush_rules", s.rush_rules);
	l.set("reveal_map", s.reveal_map);
	l.set("victory", s.victory);
	l.set("elimination", s.elimination);
	l.set("score_goal", s.score_goal);
	l.set("time_limit", s.time_limit);
	l.set("wonderwin", s.wonderwin);
	l.set("popwin", s.popwin);
	l.set("econwin", s.econwin);
	l.set("starting_town", s.starting_town);
	l.set("starting_resources", s.starting_resources);
	l.set("starting_technology", s.starting_technology);
	l.set("ending_technology", s.ending_technology);
	l.set("randomize_starts", s.randomize_starts ? 1 : 0);
	l.set("no_tribe_powers", s.no_tribe_powers ? 1 : 0);
	for (int i = 0; i < 8; ++i) {
		const sim::NationSetup& n = s.nation[size_t(i)];
		l.set(key_n("slot_type", i), i >= s.num_nations ? 2 : n.human ? 0 : 1);  // 0 human, 1 computer, 2 closed
		l.set(key_n("tribe", i), n.tribe);
		l.set(key_n("team", i), n.team);
		l.set(key_n("diff", i), n.difficulty);
		l.set(key_n("color", i), n.color);
	}
}

bool lobby_to_setup(const Lobby& l, sim::RandomGameSetup& s, std::string& error) {
	if (!l.has("map_style") || !l.has("map_size")) {  // game_seed arrives with START: a lobby screen reads the setup before it
		error = "the lobby has no map settings";
		return false;
	}
	s = sim::RandomGameSetup();
	s.map_style = int(l.get_int("map_style"));
	s.map_size = int(l.get_int("map_size"));
	s.seed = uint32_t(l.get_int("game_seed", 0));
	s.num_nations = int(l.get_int("num_nations", 2));
	s.game_rules = int(l.get_int("gamerules"));
	s.game_speed = int(l.get_int("gamespeed", 1));
	s.pop_limit = int(l.get_int("pop_limit", 5));
	s.tech_cost = int(l.get_int("tech_cost", 3));
	s.rush_rules = int(l.get_int("rush_rules"));
	s.reveal_map = int(l.get_int("reveal_map", 1));
	s.victory = int(l.get_int("victory"));
	s.elimination = int(l.get_int("elimination"));
	s.score_goal = int(l.get_int("score_goal"));
	s.time_limit = int(l.get_int("time_limit"));
	s.wonderwin = int(l.get_int("wonderwin"));
	s.popwin = int(l.get_int("popwin"));
	s.econwin = int(l.get_int("econwin"));
	s.starting_town = int(l.get_int("starting_town", 1));
	s.starting_resources = int(l.get_int("starting_resources", 1));
	s.starting_technology = int(l.get_int("starting_technology"));
	s.ending_technology = int(l.get_int("ending_technology", 7));
	s.randomize_starts = l.get_int("randomize_starts") != 0;
	s.no_tribe_powers = l.get_int("no_tribe_powers") != 0;
	for (int i = 0; i < 8; ++i) {
		sim::NationSetup& n = s.nation[size_t(i)];
		n.human = l.get_int(key_n("slot_type", i), 1) == 0;
		n.tribe = int(l.get_int(key_n("tribe", i), -1));
		n.team = int(l.get_int(key_n("team", i), -1));
		n.difficulty = int(l.get_int(key_n("diff", i), 2));
		n.color = int(l.get_int(key_n("color", i), -1));
	}
	if (s.num_nations < 2 || s.num_nations > 8) {
		error = "bad num_nations";
		return false;
	}
	return true;
}

MpSeats lobby_mp_seats(const Lobby& l) {
	MpSeats m;
	m.plan = l.start_plan();
	if (m.plan.players == 0) m.plan = l.plan();
	for (int slot = 0; slot < 8; ++slot) {
		const int peer = m.plan.peer_of[size_t(slot)];
		std::string nm = peer >= 0 ? l.get("name_" + std::to_string(peer)) : std::string();
		if (nm.empty() && peer >= 0) nm = "Player " + std::to_string(slot + 1);
		m.name[size_t(slot)] = nm;
		m.handicap[size_t(slot)] = int(l.get_int(key_n("handicap", slot), 0));
	}
	return m;
}

void apply_mp_seats(sim::Game& g, const std::array<int, 8>& who_of, const MpSeats& m) {
	g.multiplayer = true;
	for (int slot = 0; slot < m.plan.nations && slot < 8; ++slot) {
		if (m.plan.peer_of[size_t(slot)] < 0) continue;   // a computer seat keeps its nation's name and its difficulty
		const int who = who_of[size_t(slot)];
		if (who < 0 || who >= int(g.leaders.size())) continue;
		sim::Leader& L = g.leaders[size_t(who)];
		if (!m.name[size_t(slot)].empty()) L.name = m.name[size_t(slot)];
		L.handicap = std::clamp(m.handicap[size_t(slot)], 0, 255);
	}
}

}  // namespace ron::net
