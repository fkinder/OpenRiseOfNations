#include "net/record.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>

#include "sim/rules.hpp"
#include "sim/save.hpp"

namespace ron::net {

namespace {

struct W {
	std::vector<uint8_t> b;
	void u8(uint32_t v) { b.push_back(uint8_t(v)); }
	void u16(uint32_t v) {
		u8(v);
		u8(v >> 8);
	}
	void u32(uint32_t v) {
		for (int i = 0; i < 4; ++i) u8(v >> (8 * i));
	}
	void u64(uint64_t v) {
		u32(uint32_t(v));
		u32(uint32_t(v >> 32));
	}
	void str(const std::string& s) {  // String::walk_data: u32 UTF-16 units, then the units (ASCII here)
		u32(uint32_t(s.size()));
		for (unsigned char c : s) u16(c);
	}
	void raw(const void* p, size_t n) {
		const uint8_t* q = static_cast<const uint8_t*>(p);
		b.insert(b.end(), q, q + n);
	}
	void zeros(size_t n) { b.insert(b.end(), n, 0); }
};

constexpr uint32_t kGameInfoVersion = 0x6ac012c;  // the corpus' "(Version: 00.2024.06.2000)"
constexpr const char* kVersionString = "(Version: 00.2024.06.2000)";

std::vector<uint8_t> header_bytes(const RecordInfo& in) {
	W w;
	w.u8(0x16);  // Game
	w.u8(0x42);  // GameInfo
	w.str(kVersionString);
	w.u32(kGameInfoVersion);
	w.u32(in.seed);
	w.u32(0);    // checksum_deep
	w.u32(256);  // checksum_window_size
	w.u32(0);    // checksum_failure_threshold
	w.u32(in.info_flags);
	w.raw(in.setup.data(), in.setup.size());
	int present = 0;
	for (int i = 0; i < 8; ++i) {
		const RecordSeat& s = in.seats[size_t(i)];
		w.u8(0x50);
		if (!s.present) {
			w.u16(0);
			continue;
		}
		++present;
		uint16_t flags = 1;
		if (s.human) flags |= 4;
		if (s.local) flags |= 2;
		w.u16(flags);
		uint8_t raw[0x39] = {};
		raw[0x30] = uint8_t(flags);
		raw[0x31] = uint8_t(flags >> 8);
		raw[0x32] = uint8_t(s.tribe);
		raw[0x33] = uint8_t(s.who);
		raw[0x34] = uint8_t(int8_t(s.team));
		raw[0x35] = 0;
		raw[0x36] = uint8_t(i);
		raw[0x37] = 0;
		raw[0x38] = uint8_t(s.diff);
		w.raw(raw, sizeof raw);
		w.str(s.name);
	}
	w.u64(0);                 // scenario workshop id
	w.str(std::string());     // scenario_script
	w.str(in.scenario_path);  // scenario_path (port: the random setup text)
	w.str(std::string());     // scenario dir
	w.u64(0);                 // mod workshop id
	w.str(std::string());     // mod install path
	uint8_t game[0x194] = {};
	const int32_t frame = in.frame, to_break = -1, tick = in.tick, np = present;
	std::memcpy(game, &frame, 4);
	std::memcpy(game + 4, &to_break, 4);
	std::memcpy(game + 0x10, &tick, 4);
	std::memcpy(game + (0x69c - 0x550), &np, 4);
	w.raw(game, sizeof game);
	w.u32(0x100);  // semaphore: bits
	w.u32(0x20);   // size
	uint8_t sem[0x20] = {};
	sem[0] = 0x0c;  // multiplayer | recording
	w.raw(sem, sizeof sem);
	w.u32(0);       // graphic_tick
	w.str(in.native_save.empty() ? std::string() : in.save_name);
	return std::move(w.b);
}

}  // namespace

RecordInfo record_info_for_random(const sim::RandomGameSetup& s, uint32_t seed, int local_slot, const std::array<int, 8>& who_of, const sim::Game& g,
                                  const std::string& scenario_text) {
	RecordInfo in;
	in.seed = seed;
	in.frame = g.frame;
	in.tick = g.tick;
	in.scenario_path = scenario_text;
	auto set = [&](sim::ReplaySetupField f, int v) { in.setup[size_t(f)] = uint8_t(v); };
	set(sim::kSetTeamStyle, 1);
	set(sim::kSetMapStyle, s.map_style);
	set(sim::kSetMapSize, s.map_size);
	set(sim::kSetPlayers, s.num_nations);
	set(sim::kSetGameSpeed, s.game_speed);
	set(sim::kSetGameRules, s.game_rules);
	set(sim::kSetDifficulty, 2);
	set(sim::kSetStartingTown, s.starting_town);
	set(sim::kSetStartingResources, s.starting_resources);
	set(sim::kSetStartingResources2, s.starting_resources);
	set(sim::kSetTechCost, s.tech_cost);
	set(sim::kSetRevealMap, s.reveal_map);
	set(sim::kSetPopLimit, s.pop_limit);
	set(sim::kSetRushRules, s.rush_rules);
	set(sim::kSetStartingTechnology, s.starting_technology);
	set(sim::kSetStartingTechnology2, s.starting_technology);
	set(sim::kSetEndingTechnology, s.ending_technology);
	set(sim::kSetElimination, s.elimination);
	set(sim::kSetVictory, s.victory);
	set(sim::kSetWonderWin, s.wonderwin);
	set(sim::kSetScoreGoal, s.score_goal);
	set(sim::kSetPopWin, s.popwin);
	set(sim::kSetTimeLimit, s.time_limit);
	set(sim::kSetEconWin, s.econwin);
	if (s.no_tribe_powers) in.info_flags |= 4;
	for (int i = 0; i < s.num_nations && i < 8; ++i) {
		RecordSeat& seat = in.seats[size_t(i)];
		const int who = who_of[size_t(i)];
		seat.present = true;
		seat.human = s.nation[size_t(i)].human;
		seat.local = i == local_slot;
		seat.who = who;
		seat.tribe = who >= 0 && who < int(g.leaders.size()) ? g.leaders[size_t(who)].tribe : s.nation[size_t(i)].tribe;
		seat.team = int(s.nation[size_t(i)].team);
		seat.diff = s.nation[size_t(i)].difficulty;
		seat.name = seat.human ? "Player " + std::to_string(i + 1) : "Computer " + std::to_string(i + 1);
		in.random_info.emplace_back(int8_t(seat.team), int32_t(seat.tribe));
	}
	in.world30 = s.map_style;
	if (g.rules) in.rules_image = sim::write_image(*g.rules);
	return in;
}

RecordInfo record_info_for_segment(const RecordInfo& first, const std::vector<uint8_t>& native_save, int32_t frame) {
	RecordInfo in = first;
	in.native_save = native_save;
	in.rules_image.clear();
	in.frame = frame;
	return in;
}

std::string record_segment_path(const std::string& base, int segment) {
	if (segment <= 1) return base;
	std::string stem = base;
	std::string ext;
	const size_t dot = base.rfind('.');
	if (dot != std::string::npos && base.find('/', dot) == std::string::npos) {
		stem = base.substr(0, dot);
		ext = base.substr(dot);
	}
	return stem + "_" + std::to_string(segment) + ext;
}

Recorder::~Recorder() {
	if (out_.is_open()) close();
}

bool Recorder::open(const std::string& path, const RecordInfo& in, std::string& error) {
	if (out_.is_open()) close();
	out_.open(path, std::ios::binary | std::ios::trunc);
	if (!out_) {
		error = "cannot write " + path;
		return false;
	}
	path_ = path;
	seed_ = in.seed;
	packages_ = 0;
	const std::vector<uint8_t> head = header_bytes(in);
	out_.write(reinterpret_cast<const char*>(head.data()), std::streamsize(head.size()));
	W w;
	if (!in.native_save.empty()) {
		w.str("RoNNativeSave");
		w.u32(sim::kSaveVersion);
		w.u32(uint32_t(in.native_save.size()));
		w.raw(in.native_save.data(), in.native_save.size());
	}
	for (const auto& e : in.random_info) {
		w.u8(uint8_t(e.first));
		w.u32(uint32_t(e.second));
	}
	w.u32(uint32_t(in.world30));
	if (in.native_save.empty()) w.raw(in.rules_image.data(), in.rules_image.size());
	out_.write(reinterpret_cast<const char*>(w.b.data()), std::streamsize(w.b.size()));
	out_.flush();
	return bool(out_);
}

void Recorder::package(int32_t frame, int slot, uint32_t stamp, const Bytes& data) {
	if (!out_.is_open() || data.empty() || data.size() > kMaxPackageBytes) return;
	Bytes payload;
	if (stamp == 0 && data.size() <= 4 && data[0] == sim::op::kUngracefulDrop) payload = data;  // clear, as process_drops writes it
	else payload = sim::replay_obfuscate(data, seed_);
	W w;
	w.u32(uint32_t(frame));
	w.u32(uint32_t(slot));
	w.u32(1);  // valid
	w.u32(stamp);
	w.u16(uint32_t(payload.size()));
	w.raw(payload.data(), payload.size());
	out_.write(reinterpret_cast<const char*>(w.b.data()), std::streamsize(w.b.size()));
	out_.flush();
	++packages_;
}

bool Recorder::close(std::string* error) {
	if (!out_.is_open()) return true;
	out_.close();
	std::vector<uint8_t> plain;
	{
		std::ifstream f(path_, std::ios::binary);
		plain.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}
	const std::string tmp = path_ + ".tmp";
	gzFile gz = gzopen(tmp.c_str(), "wb6");
	if (!gz) {
		if (error) *error = "cannot write " + tmp;
		return false;
	}
	size_t off = 0;
	while (off < plain.size()) {
		const unsigned n = unsigned(std::min<size_t>(plain.size() - off, 1u << 20));
		if (gzwrite(gz, plain.data() + off, n) != int(n)) {
			gzclose(gz);
			if (error) *error = "gzip write failed";
			return false;
		}
		off += n;
	}
	gzclose(gz);
	std::error_code ec;
	std::filesystem::rename(tmp, path_, ec);
	if (ec) {
		if (error) *error = "cannot rename " + tmp + ": " + ec.message();
		return false;
	}
	return true;
}

}  // namespace ron::net
