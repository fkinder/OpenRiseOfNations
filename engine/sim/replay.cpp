#include "sim/replay.hpp"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "sim/random.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- verb table (replay-spec section 9)

namespace {

struct VerbInfo {
	const char* name;
	uint16_t len;  // 0 = variable (GROUP, SPLINE, CHAT)
};

const VerbInfo kVerbs[kReplayVerbCount] = {
	{"GROUP", 0}, {"BEGIN", 1}, {"STANCE", 5}, {"FORM", 13}, {"ATTACK", 17}, {"SIEGE_ATTACK", 13}, {"SWARM_AROUND", 17},
	{"MOVE_TO", 22}, {"MOVE_NEAR", 26}, {"ATTACK_GROUND", 10}, {"PATROL", 10}, {"LAUNCH_PATROL", 25}, {"HALT", 1},
	{"TRANSPORT", 1}, {"SET_TRANSPORT", 5}, {"BOARD_SHIP", 9}, {"REPAIR", 13}, {"TRADE", 21}, {"CITY_GATHER", 9}, {"GATHER", 9},
	{"GARRISON", 13}, {"DISBAND", 5}, {"GATHER_POINT", 17}, {"SPELL", 21}, {"QUEUE_UP", 9}, {"BUILD", 25}, {"EJECTALL", 17},
	{"ALARM", 1}, {"FLIGHT", 25}, {"STOP_SPELL", 1}, {"FOLLOW", 13}, {"GUARD", 13}, {"UNITMASK", 9}, {"BUILDMASK", 9},
	{"HOTKEY", 25}, {"RECALL", 1}, {"SCRAMBLE", 1}, {"TREATY", 13}, {"DECLARE", 13}, {"CLEAR_TRIBUTES", 9}, {"CLEAR_ALL", 9},
	{"ACCEPT", 9}, {"REJECT", 9}, {"TRIBUTE", 17}, {"DEMAND_TRIBUTE", 17}, {"PROPOSE_ATTACK", 17}, {"BUY", 13}, {"SELL", 13},
	{"UNQUEUE", 15}, {"COME_OUT", 11}, {"PING", 9}, {"SPLINE", 0}, {"SPEED_SET", 5}, {"SPEED_UP", 1}, {"SPEED_DOWN", 1},
	{"MP_LOG", 1}, {"CHECK_RANDOM", 5}, {"CHECK_SUMS", 65}, {"NEXT_CHECK_SUM", 6}, {"CHEAT_VIEW_ALL", 5},
	{"CHEAT_GIVE_TECHS", 5}, {"CHEAT_ZERO_TECHS", 5}, {"CHEAT_AI_SPEED_INCREASE", 1}, {"CHEAT_AI_SPEED_NORMAL", 1},
	{"CHEAT_AI_TOGGLE", 1}, {"CHEAT_INCREASE_BUCKETS", 5}, {"CHEAT_ZERO_BUCKETS", 5}, {"CHEAT_INIT_UNIT", 17}, {"CHAT", 0},
	{"CHAT_SET", 9}, {"RESIGN", 5}, {"QUIT", 7}, {"CAMERA", 10}, {"LEADER_OPTIONS", 33}, {"TURN_DATA", 11}, {"RENAME_CITY", 53},
	{"PAUSE", 2}, {"CANNON_TIME", 2}, {"CONSOLE_CMD", 521}, {"PLAYER_SPEED", 9}, {"UNGRACEFUL_PLAYER_DROP", 3}, {"MARWAN", 2},
};

inline int32_t ld32(const uint8_t* p) {
	uint32_t v;
	std::memcpy(&v, p, 4);
	return int32_t(v);
}
inline uint32_t ldu32(const uint8_t* p) { return uint32_t(ld32(p)); }
inline int16_t ld16(const uint8_t* p) {
	uint16_t v;
	std::memcpy(&v, p, 2);
	return int16_t(v);
}
inline uint16_t ldu16(const uint8_t* p) { return uint16_t(ld16(p)); }

void put32(std::vector<uint8_t>& o, uint32_t v) {
	for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void put16(std::vector<uint8_t>& o, uint16_t v) {
	o.push_back(uint8_t(v));
	o.push_back(uint8_t(v >> 8));
}

}  // namespace

const char* replay_verb_name(int opcode) { return opcode >= 0 && opcode < kReplayVerbCount ? kVerbs[opcode].name : nullptr; }

size_t replay_command_length(const uint8_t* p, size_t avail) {
	if (avail < 1 || p[0] >= kReplayVerbCount) return 0;
	const int op = p[0];
	size_t len = kVerbs[op].len;
	if (op == 0) {  // GROUP: u8 num, i8 who, num x u16
		if (avail < 3) return 0;
		len = 3 + 2 * size_t(p[1]);
	} else if (op == 51) {  // SPLINE: u8 type, u8 flags, u8 cmd, u16 n, n x {f32, f32}
		if (avail < 6) return 0;
		len = 6 + 8 * size_t(ldu16(p + 4));
	} else if (op == 68) {  // CHAT: i32 bits, taunt, taunt_num, n, n x u16, u16 0
		if (avail < 17) return 0;
		const int32_t n = ld32(p + 13);
		if (n < 0 || n > 240) return 0;
		len = 19 + 2 * size_t(n);
	}
	return len <= avail ? len : 0;
}

bool replay_has_engine_verb(int opcode) {
	switch (opcode) {
	case 0: case 2: case 3: case 4: case 6: case 7: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 17: case 18:
	case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 30: case 31: case 33: case 35: case 36: case 39: case 41:
	case 42: case 43: case 44: case 46: case 47: case 48: case 73:
	case 34: case 49: case 52: case 53: case 54: case 59: case 60: case 61: case 62: case 63: case 64: case 65: case 66: case 67:  // console verbs
	case 68: case 75: case 76:
		return true;
	default:
		return false;
	}
}

// ---------------------------------------------------------------- byte reader

namespace {

struct ParseError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

class Rd {
public:
	Rd(const uint8_t* b, size_t n, size_t pos = 0) : b_(b), n_(n), p_(pos) {}
	size_t pos() const { return p_; }
	void seek(size_t p) { p_ = p; }
	size_t left() const { return n_ - p_; }
	void need(size_t k) const {
		if (p_ + k > n_) throw ParseError("read past end at " + std::to_string(p_) + " (+" + std::to_string(k) + ", size " + std::to_string(n_) + ")");
	}
	uint8_t u8() { need(1); return b_[p_++]; }
	int8_t i8() { return int8_t(u8()); }
	uint16_t u16() { need(2); const uint16_t v = ldu16(b_ + p_); p_ += 2; return v; }
	int16_t i16() { return int16_t(u16()); }
	uint32_t u32() { need(4); const uint32_t v = ldu32(b_ + p_); p_ += 4; return v; }
	int32_t i32() { return int32_t(u32()); }
	uint64_t u64() { need(8); uint64_t v; std::memcpy(&v, b_ + p_, 8); p_ += 8; return v; }
	const uint8_t* bytes(size_t k) { need(k); const uint8_t* r = b_ + p_; p_ += k; return r; }
	std::u16string str() {
		const uint32_t k = u32();
		if (k > 0x100000) throw ParseError("implausible string length " + std::to_string(k) + " at " + std::to_string(p_ - 4));
		const uint8_t* s = bytes(2 * size_t(k));
		std::u16string out(k, u'\0');
		for (uint32_t i = 0; i < k; ++i) out[i] = ldu16(s + 2 * i);
		return out;
	}
	void tag(const char* name, int expect) {
		const int t = u8();
		if (expect >= 0 && t != expect) {
			char b[160];
			std::snprintf(b, sizeof b, "section tag \"%s\" = 0x%x at %zu, expected 0x%x", name, t, p_ - 1, expect);
			throw ParseError(b);
		}
	}

private:
	const uint8_t* b_;
	size_t n_, p_;
};

constexpr int kTagGame = 0x16, kTagGameInfo = 0x42, kTagPlayer = 0x50, kTagGameRules = 0x92, kTagTribeRules = 0x8f;

void read_game_info(Rd& r, ReplayHeader& h) {
	r.tag("GameInfo", kTagGameInfo);
	h.version_string = r.str();
	h.version = r.u32();
	h.seed = r.u32();
	h.checksum_deep = r.i32();
	h.checksum_window = r.i32();
	h.checksum_threshold = r.i32();
	h.flags = r.u32();
	const uint8_t* s = r.bytes(kSetupFieldCount);
	for (int i = 0; i < kSetupFieldCount; ++i) h.setup[size_t(i)] = s[i];
	for (int i = 0; i < 8; ++i) {
		r.tag("Player", kTagPlayer);
		ReplayPlayer& p = h.players[size_t(i)];
		p = ReplayPlayer();
		p.slot = i;
		p.flags = r.u16();
		if (p.flags & 1) {
			const uint8_t* raw = r.bytes(0x39);
			for (int k = 0; k < 12; ++k) p.synced[k] = ld32(raw + 4 * k);
			p.tribe = raw[0x32];
			p.who = raw[0x33];
			p.team = int8_t(raw[0x34]);
			p.handicap = raw[0x35];
			p.play = raw[0x36];
			p.pauses = raw[0x37];
			p.diff = raw[0x38];
			p.name = r.str();
		}
	}
	h.scenario_workshop_id = r.u64();
	h.scenario_script = r.str();
	h.scenario_path = r.str();
	h.scenario_dir = r.str();
	h.mod_workshop_id = r.u64();
	h.mod_path = r.str();
}

void read_header_tail(Rd& r, ReplayHeader& h, bool with_save_name) {
	const uint8_t* g = r.bytes(0x194);
	h.frame = ld32(g);
	h.tick = ld32(g + 0x10);
	h.num_players = ld32(g + (0x69c - 0x550));
	h.semaphore_bits = r.i32();
	const int32_t size = r.i32();
	if (size < 0 || size > 64) throw ParseError("bad semaphore size " + std::to_string(size));
	const uint8_t* sem = r.bytes(size_t(size));
	h.semaphore.assign(sem, sem + size);
	h.graphic_tick = r.i32();
	h.save_name = with_save_name ? r.str() : std::u16string();
	h.multiplayer = h.semaphore.size() > 0 && (h.semaphore[0] & 0x04) != 0;
	h.conquest = h.semaphore.size() > 2 && (h.semaphore[2] & 0x02) != 0;
}

void read_header(Rd& r, ReplayHeader& h) {
	r.tag("Game", kTagGame);
	read_game_info(r, h);
	read_header_tail(r, h, true);
}

// Frame of the game inside an embedded save, read from the start of WalkDataGame::walk_data (GameInfo, 182 raw bytes, Game).
bool embedded_save_frame(const uint8_t* b, size_t n, size_t at, int32_t& frame) {
	try {
		Rd r(b, n, at);
		ReplayHeader a, h;
		read_game_info(r, a);
		r.bytes(182);
		r.tag("Game", kTagGame);
		read_game_info(r, h);
		read_header_tail(r, h, false);
		frame = h.frame;
		return true;
	} catch (const ParseError&) {
		return false;
	}
}

uint32_t skip_simple_array_u16(Rd& r) {
	const uint32_t n = r.u32();
	if (n) {
		r.u32();
		r.u16();
		r.u8();
		r.bytes(2 * size_t(n));
	}
	return n;
}

struct TypeRange {
	int lo, hi;
	char cls;  // G good, U unit, B build, I item, T tech, S spell, O bonus
};
const TypeRange kTypeRanges[] = {{0, 50, 'G'}, {50, 414, 'U'}, {414, 543, 'B'}, {543, 544, 'I'}, {544, 629, 'T'}, {629, 684, 'S'}, {684, 806, 'O'}};

// Game::walk_rules_data: parse to find the end (the content is the replay's own rules, not decoded here).
void skip_rules(Rd& r, int& types) {
	r.tag("Game Rules", kTagGameRules);
	types = 0;
	for (const TypeRange& tr : kTypeRanges) {
		for (int i = tr.lo; i < tr.hi; ++i) {
			r.bytes(0x5a);
			r.str();
			if (tr.cls == 'G' || tr.cls == 'U' || tr.cls == 'B' || tr.cls == 'I') {
				r.bytes(0x98);
				skip_simple_array_u16(r);
				skip_simple_array_u16(r);
				if (tr.cls == 'G') r.bytes(0x44);
				if (tr.cls == 'U') r.bytes(0x18 + 8 + 4 + 0x2f4);
				if (tr.cls == 'B') r.bytes(0x31);
			} else if (tr.cls == 'T') {
				r.bytes(0x1b);
				for (int k = 0; k < 8; ++k) r.str();
			} else if (tr.cls == 'S') {
				r.bytes(0x30);
			}
			++types;
		}
	}
	r.bytes(0xd40);
	r.bytes(4);
	r.bytes(size_t(0x1ed) * 0x1ed * 2);
	for (int t = 0; t < 24; ++t) {
		r.tag("Tribe rules", kTagTribeRules);
		r.bytes(0x18);
		r.bytes(0x580);
	}
}

int expected_players(const ReplayHeader& h) {
	int n = 0;
	for (const ReplayPlayer& p : h.players) n += (p.flags & 1) && p.who < 8;
	return n;
}

void read_random_info(Rd& r, ReplayRandomInfo& out, int n) {
	out.entries.clear();
	for (int i = 0; i < n; ++i) {
		ReplayRandomInfo::Entry e;
		e.team = r.i8();
		e.leader_value = r.i32();
		out.entries.push_back(e);
	}
	out.world30 = r.i32();
}

// Number of 5-byte entries of a random-game-info block at p that is followed by the 0x92 rules tag.
int random_info_count(const uint8_t* b, size_t n, size_t p, const ReplayHeader& h) {
	const int expect = expected_players(h);
	std::vector<int> order{expect};
	for (int k = 0; k <= 8; ++k)
		if (k != expect) order.push_back(k);
	for (int k : order)
		if (p + 5 * size_t(k) + 4 < n && b[p + 5 * size_t(k) + 4] == kTagGameRules) return k;
	return expect;
}

// Mirror of CommandPackage::process_all: undo the MP obfuscation, then walk the commands skipping the per-command padding.
bool decode_package(const uint8_t* d, size_t n, const ReplayHeader& h, std::vector<uint8_t>& plain, std::vector<ReplayCommand>& cmds) {
	return replay_decode_package(d, n, h.seed, h.multiplayer, plain, cmds);
}

}  // namespace

bool replay_decode_package(const uint8_t* d, size_t n, uint32_t seed, bool multiplayer, std::vector<uint8_t>& plain, std::vector<ReplayCommand>& cmds) {
	plain.assign(d, d + n);
	cmds.clear();
	if (multiplayer) {
		const bool is_drop = n != 0 && plain[0] == 0x50 && n <= 4;
		if (!is_drop) {
			const uint16_t key = uint16_t((seed >> 8) & 0xffff);
			for (size_t i = 0; i + 1 < n; i += 2) {
				const uint16_t v = uint16_t(ldu16(plain.data() + i) ^ key);
				plain[i] = uint8_t(v);
				plain[i + 1] = uint8_t(v >> 8);
			}
		}
	}
	Random pad(seed);
	size_t p = 0;
	while (p < n) {
		const size_t len = replay_command_length(plain.data() + p, n - p);
		if (!len) return false;
		ReplayCommand c;
		c.op = plain[p];
		c.len = uint16_t(len);
		c.at = uint32_t(p);
		p += len;
		if (multiplayer) {
			const int k = pad.get(0, 2);
			c.pad = uint8_t(k);
			p += size_t(k);
		}
		cmds.push_back(c);
	}
	return p == n;
}

namespace {

struct StreamResult {
	std::vector<ReplayPackage> recs;
	bool truncated = false;
	size_t t_at = 0, t_have = 0, t_need = 0;
};

// RecordGame::read_package records from `pos` to EOF. strict (the scan and the exact path) rejects what no real record has.
bool read_stream(const uint8_t* b, size_t n, size_t pos, const ReplayHeader& h, bool strict, int32_t min_frame, StreamResult& out,
                 std::string* err = nullptr) {
	out = StreamResult();
	size_t p = pos;
	int32_t last_frame = min_frame;
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	while (p < n) {
		const size_t at = p;
		const size_t left = n - p;
		if (left < 18) {
			out.truncated = true;
			out.t_at = at;
			out.t_have = left;
			out.t_need = 18;
			break;
		}
		const int size = ld16(b + at + 16);
		if (size >= 0 && size <= 0x200 && at + 18 + size_t(size) > n) {
			out.truncated = true;
			out.t_at = at;
			out.t_have = left;
			out.t_need = 18 + size_t(size);
			break;
		}
		if (size < 0 || size > 0x200) return fail("bad package size " + std::to_string(size) + " at " + std::to_string(at));
		ReplayPackage rec;
		rec.file_at = at;
		rec.frame = ld32(b + at);
		rec.play = ld32(b + at + 4);
		rec.valid = ld32(b + at + 8);
		rec.stamp = ldu32(b + at + 12);
		rec.size = int16_t(size);
		p = at + 18 + size_t(size);
		if (strict) {
			if (rec.play < 0 || rec.play > 7) return fail("bad play " + std::to_string(rec.play) + " at " + std::to_string(at));
			if (rec.size < 1) return fail("empty package at " + std::to_string(at));
			if (rec.frame < last_frame) return fail("frame " + std::to_string(rec.frame) + " before " + std::to_string(last_frame) + " at " + std::to_string(at));
		}
		last_frame = rec.frame;
		if (!decode_package(b + at + 18, size_t(size), h, rec.plain, rec.cmds)) return fail("undecodable package at " + std::to_string(at));
		out.recs.push_back(std::move(rec));
	}
	return true;
}

struct Located {
	size_t pos = 0, rgi_at = 0;
	int n = 0;
	StreamResult stream;
};

// The command stream after an embedded save (or a Conquer the World header): the earliest offset whose records chain to EOF,
// preceded by a plausible random-game-info block (rcxdump.mjs locateStreamAfterSave).
bool locate_stream_after_save(const uint8_t* b, size_t len, size_t start, const ReplayHeader& h, int32_t min_frame, Located& out) {
	const int expect = expected_players(h);
	std::vector<int8_t> teams;
	for (const ReplayPlayer& p : h.players)
		if ((p.flags & 1) && p.who < 8) teams.push_back(p.team);
	std::vector<int> counts{expect};
	for (int k = 0; k <= 8; ++k)
		if (k != expect) counts.push_back(k);
	for (size_t o = start; o <= len; ++o) {
		if (o + 18 <= len) {
			const int32_t play = ld32(b + o + 4);
			const int size = ld16(b + o + 16);
			if (play < 0 || play > 7 || size < 1 || size > 0x200) continue;
		}
		for (int n : counts) {
			const size_t block = 5 * size_t(n) + 4;
			if (o < block) continue;
			const size_t rgi_at = o - block;
			if (rgi_at < start) continue;
			bool ok = true;
			if (n == expect) {
				for (size_t i = 0; i < teams.size(); ++i)
					if (int8_t(b[rgi_at + 5 * i]) != teams[i]) { ok = false; break; }
			} else {
				for (int i = 0; i < n; ++i) {
					const int t = int8_t(b[rgi_at + 5 * size_t(i)]);
					const int32_t l = ld32(b + rgi_at + 5 * size_t(i) + 1);
					if (t < -1 || t > 8 || l < 0 || l > 255) { ok = false; break; }
				}
			}
			if (!ok) continue;
			StreamResult sr;
			if (!read_stream(b, len, o, h, true, min_frame, sr)) continue;
			if (sr.recs.empty() && !sr.truncated) continue;
			out.pos = o;
			out.rgi_at = rgi_at;
			out.n = n;
			out.stream = std::move(sr);
			return true;
		}
	}
	return false;
}

}  // namespace

// ---------------------------------------------------------------- file

bool replay_gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, bool& was_gzip) {
	was_gzip = in.size() > 2 && in[0] == 0x1f && in[1] == 0x8b;
	if (!was_gzip) {
		out = in;
		return true;
	}
	z_stream zs{};
	if (inflateInit2(&zs, 15 + 32) != Z_OK) return false;
	zs.next_in = const_cast<Bytef*>(in.data());
	zs.avail_in = uInt(in.size());
	out.clear();
	std::vector<uint8_t> buf(1 << 20);
	int rc;
	do {
		zs.next_out = buf.data();
		zs.avail_out = uInt(buf.size());
		rc = inflate(&zs, Z_NO_FLUSH);
		out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
	} while (rc == Z_OK);
	inflateEnd(&zs);
	return rc == Z_STREAM_END || !out.empty();
}

bool read_replay(const std::string& path, Replay& out, std::string& error) {
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		error = "cannot read " + path;
		return false;
	}
	std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (!read_replay_bytes(raw, out, error)) return false;
	out.path = path;
	return true;
}

bool read_replay_bytes(const std::vector<uint8_t>& file, Replay& out, std::string& error) {
	out = Replay();
	if (!replay_gunzip(file, out.data, out.gzip)) {
		error = "cannot inflate the file";
		return false;
	}
	out.decompressed_size = out.data.size();
	const uint8_t* b = out.data.data();
	const size_t n = out.data.size();
	try {
		Rd r(b, n);
		read_header(r, out.header);
		out.header_end = r.pos();
		const ReplayHeader& h = out.header;
		const bool save_present = !h.save_name.empty();
		if (save_present && !h.conquest) {
			Rd sr(b, n, r.pos());
			out.has_save = true;
			out.save_at = r.pos();
			const std::u16string magic = sr.str();
			for (char16_t c : magic) out.save_magic.push_back(char(c));
			out.save_version = sr.u32();
			if (out.save_magic == "RoNNativeSave") {
				// the port's own save: length-prefixed, so the stream follows exactly
				const uint32_t len = sr.u32();
				out.native_save = true;
				out.native_at = sr.pos();
				out.native_size = len;
				sr.bytes(len);
				out.save_size = sr.pos() - out.save_at;
				out.save_frame = h.frame;
				Rd rr(b, n, sr.pos());
				read_random_info(rr, out.random_info, expected_players(h));
				out.stream_at = rr.pos();
				StreamResult st;
				std::string serr;
				if (!read_stream(b, n, rr.pos(), h, true, h.frame, st, &serr)) {
					error = serr;
					return false;
				}
				out.packages = std::move(st.recs);
				out.truncated = st.truncated;
				out.truncated_at = st.t_at;
				out.truncated_have = st.t_have;
				out.truncated_need = st.t_need;
				out.located = "exact";
				return true;
			}
			int32_t sf = 0;
			if (embedded_save_frame(b, n, sr.pos(), sf)) out.save_frame = sf;
			try {
				Rd gr(b, n, sr.pos());
				read_game_info(gr, out.save_info);
				out.has_save_info = true;
			} catch (const ParseError&) {
			}
			Located loc;
			if (!locate_stream_after_save(b, n, sr.pos(), h, out.save_frame >= 0 ? out.save_frame : h.frame, loc)) {
				error = "could not locate the command stream after the embedded save";
				return false;
			}
			out.save_size = loc.rgi_at - out.save_at;
			Rd rr(b, n, loc.rgi_at);
			read_random_info(rr, out.random_info, loc.n);
			out.stream_at = loc.pos;
			out.packages = std::move(loc.stream.recs);
			out.truncated = loc.stream.truncated;
			out.truncated_at = loc.stream.t_at;
			out.truncated_have = loc.stream.t_have;
			out.truncated_need = loc.stream.t_need;
			out.located = "scan";
		} else if (h.conquest) {
			out.warnings.push_back("Conquer the World replay: ConquestGame::walk_data header follows and is not decoded; stream located by scan");
			Located loc;
			if (!locate_stream_after_save(b, n, r.pos(), h, h.frame, loc)) {
				error = "could not locate the command stream";
				return false;
			}
			out.stream_at = loc.pos;
			out.packages = std::move(loc.stream.recs);
			out.truncated = loc.stream.truncated;
			out.truncated_at = loc.stream.t_at;
			out.truncated_have = loc.stream.t_have;
			out.truncated_need = loc.stream.t_need;
			out.located = "scan";
		} else {
			read_random_info(r, out.random_info, random_info_count(b, n, r.pos(), h));
			out.has_rules = true;
			out.rules_at = r.pos();
			skip_rules(r, out.rules_types);
			out.rules_size = r.pos() - out.rules_at;
			out.stream_at = r.pos();
			StreamResult sr;
			std::string err;
			if (!read_stream(b, n, r.pos(), h, true, h.frame, sr, &err)) {
				error = err;
				return false;
			}
			out.packages = std::move(sr.recs);
			out.truncated = sr.truncated;
			out.truncated_at = sr.t_at;
			out.truncated_have = sr.t_have;
			out.truncated_need = sr.t_need;
			out.located = "exact";
		}
	} catch (const ParseError& e) {
		error = e.what();
		return false;
	}
	return true;
}

std::vector<uint8_t> Replay::save_bytes() const {
	if (!has_save) return {};
	return std::vector<uint8_t>(data.begin() + std::ptrdiff_t(save_at), data.begin() + std::ptrdiff_t(save_at + save_size));
}

std::vector<uint8_t> Replay::native_save_bytes() const {
	if (!native_save) return {};
	return std::vector<uint8_t>(data.begin() + std::ptrdiff_t(native_at), data.begin() + std::ptrdiff_t(native_at + native_size));
}

size_t Replay::command_count() const {
	size_t n = 0;
	for (const ReplayPackage& p : packages) n += p.cmds.size();
	return n;
}

size_t Replay::check_sum_rows() const {
	size_t n = 0;
	for (const ReplayPackage& p : packages)
		for (const ReplayCommand& c : p.cmds) n += c.op == op::kCheckSums;
	return n;
}

std::vector<uint8_t> ReplayPackage::wire(bool drop_automatic) const {
	std::vector<uint8_t> out;
	out.reserve(plain.size());
	for (const ReplayCommand& c : cmds) {
		if (drop_automatic && (c.op == op::kCheckSums || c.op == op::kTurnData || c.op == op::kCamera)) continue;
		out.insert(out.end(), plain.begin() + c.at, plain.begin() + c.at + c.len);
	}
	return out;
}

bool replay_check_sums(const uint8_t* cmd, size_t len, uint32_t out[16]) {
	if (len != 65 || cmd[0] != op::kCheckSums) return false;
	for (int i = 0; i < 16; ++i) out[i] = ldu32(cmd + 1 + 4 * i);
	return true;
}

std::vector<uint8_t> replay_obfuscate(const std::vector<uint8_t>& wire, uint32_t seed) {
	std::vector<uint8_t> out;
	Random pad(seed);
	size_t p = 0;
	while (p < wire.size()) {
		const size_t len = replay_command_length(wire.data() + p, wire.size() - p);
		if (!len) break;
		out.insert(out.end(), wire.begin() + std::ptrdiff_t(p), wire.begin() + std::ptrdiff_t(p + len));
		p += len;
		const int k = pad.get(0, 2);
		for (int i = 0; i < k; ++i) out.push_back(0);
	}
	const uint16_t key = uint16_t((seed >> 8) & 0xffff);
	for (size_t i = 0; i + 1 < out.size(); i += 2) {
		const uint16_t v = uint16_t(ldu16(out.data() + i) ^ key);
		out[i] = uint8_t(v);
		out[i + 1] = uint8_t(v >> 8);
	}
	return out;
}

// ---------------------------------------------------------------- engine bridge

namespace {

// A cursor over the arguments of one command (after the opcode).
struct Args {
	const uint8_t* p;
	size_t o = 1;
	int32_t i32() { const int32_t v = ld32(p + o); o += 4; return v; }
	uint32_t u32() { return uint32_t(i32()); }
	int8_t i8() { return int8_t(p[o++]); }
	int16_t i16() { const int16_t v = ld16(p + o); o += 2; return v; }
	float f32() {
		const uint32_t v = u32();
		float f;
		std::memcpy(&f, &v, 4);
		return f;
	}
};

}  // namespace

bool replay_to_engine(const uint8_t* cmd, size_t len, Command& out) {
	const size_t need = replay_command_length(cmd, len);
	if (need == 0 || need > len || !replay_has_engine_verb(cmd[0])) return false;
	Args a{cmd};
	Command m;
	switch (cmd[0]) {
	case 0: {  // GROUP
		m.verb = Verb::Group;
		const int num = cmd[1];
		m.who = int8_t(cmd[2]);
		for (int i = 0; i < num; ++i) m.objects.push_back(ldu16(cmd + 3 + 2 * i));
		break;
	}
	case 2: m.verb = Verb::Stance; m.stance = a.i32(); break;
	case 3: m.verb = Verb::Form; m.form32 = a.i32(); m.rotate = a.i32(); m.queued = int8_t(a.i32()); break;
	case 4: m.verb = Verb::Attack; m.ox = a.i32(); m.whom = a.i32(); m.ignore = a.i32(); m.queued = int8_t(a.i32()); break;
	case 6: m.verb = Verb::SwarmAround; m.ox = a.i32(); m.whom = a.i32(); m.queued = int8_t(a.i32()); m.order_kind = a.i32(); break;
	case 7:
		m.verb = Verb::MoveTo;
		m.x = a.i32(); m.y = a.i32(); m.set_angle = a.i32(); m.angle = a.u32();
		m.orders = a.i8(); m.queued = a.i8(); m.form = a.i8(); m.width = a.i8(); m.disembark = a.i8();
		break;
	case 9: m.verb = Verb::AttackGround; m.x = a.i32(); m.y = a.i32(); m.queued = a.i8(); break;
	case 10: m.verb = Verb::Patrol; m.x = a.i32(); m.y = a.i32(); m.queued = a.i8(); break;
	case 11:
		m.verb = Verb::LaunchPatrol;
		m.x = a.i32(); m.y = a.i32(); m.queued = int8_t(a.i32()); m.shift = a.i32(); m.ctrl = a.i32(); m.alt = a.i32();
		break;
	case 12: m.verb = Verb::Halt; break;
	case 13: m.verb = Verb::Transport; break;
	case 14: m.verb = Verb::SetTransport; m.flags = a.i32(); break;
	case 15: m.verb = Verb::BoardShip; m.ox = a.i32(); m.queued = int8_t(a.i32()); break;
	case 17:
		m.verb = Verb::Trade;
		m.ox = a.i32(); m.whom = a.i32(); m.x2 = a.i32(); m.y2 = a.i32(); m.queued = int8_t(a.i32());
		break;
	case 18: m.verb = Verb::CityGather; m.type = a.i32(); m.queued = int8_t(a.i32()); break;
	case 19: m.verb = Verb::Gather; m.ox = a.i32(); m.queued = int8_t(a.i32()); break;
	case 20: m.verb = Verb::Garrison; m.ox = a.i32(); m.whom = a.i32(); m.queued = int8_t(a.i32()); break;
	case 21: m.verb = Verb::Disband; m.all = a.i32(); break;
	case 22: m.verb = Verb::GatherPoint; m.x = a.i32(); m.y = a.i32(); m.action = a.i32(); m.add_to_end = a.i32(); break;
	case 23: m.verb = Verb::Spell; m.ox = a.i32(); m.whom = a.i32(); m.type = a.i32(); m.x = a.i32(); m.y = a.i32(); break;
	case 24: m.verb = Verb::QueueUp; m.type = a.i32(); m.num = a.i32(); break;
	case 25:
		m.verb = Verb::Build;
		m.x = a.i32(); m.y = a.i32(); m.x2 = a.i32(); m.y2 = a.i32(); m.type = a.i32(); m.queued = int8_t(a.i32());
		break;
	case 26: m.verb = Verb::EjectAll; m.back_to_work = a.i32(); m.who = a.i32(); m.eject_o = a.i32(); m.eject_who = a.i32(); break;
	case 28:
		m.verb = Verb::Flight;
		m.ox = a.i32(); m.whom = a.i32(); m.shift = a.i32(); m.ctrl = a.i32(); m.alt = a.i32(); m.orders = int8_t(a.i32());
		break;
	case 30: m.verb = Verb::Follow; m.ox = a.i32(); m.whom = a.i32(); m.queued = int8_t(a.i32()); break;
	case 31: m.verb = Verb::Guard; m.ox = a.i32(); m.whom = a.i32(); m.queued = int8_t(a.i32()); break;
	case 32: m.verb = Verb::UnitMask; m.mask = a.i32(); m.set = a.i32(); break;
	case 33: m.verb = Verb::BuildMask; m.mask = a.i32(); m.set = a.i32(); break;
	case 35: m.verb = Verb::Recall; break;
	case 36: m.verb = Verb::Scramble; break;
	case 39: m.verb = Verb::ClearTributes; m.who = a.i32(); m.whom = a.i32(); break;
	case 41: m.verb = Verb::Accept; m.who = a.i32(); m.whom = a.i32(); break;
	case 42: m.verb = Verb::Reject; m.who = a.i32(); m.whom = a.i32(); break;
	case 43: m.verb = Verb::Tribute; m.who = a.i32(); m.whom = a.i32(); m.good = a.i32(); m.amount = a.i32(); break;
	case 44: m.verb = Verb::DemandTribute; m.who = a.i32(); m.whom = a.i32(); m.good = a.i32(); m.amount = a.i32(); break;
	case 46: m.verb = Verb::Buy; m.who = a.i32(); m.good = a.i32(); m.flags = a.i32(); break;
	case 47: m.verb = Verb::Sell; m.who = a.i32(); m.good = a.i32(); m.flags = a.i32(); break;
	case 48: m.verb = Verb::Unqueue; m.who = a.i32(); m.ox = a.i32(); m.num = a.i32(); m.uid = uint16_t(a.i16()); break;
	case 34: m.verb = Verb::Hotkey; m.hk_group = a.i32(); m.hk_clear = a.i32(); m.hk_valid = a.i32(); m.hk_x = a.f32(); m.hk_y = a.f32(); m.hk_zoom = a.i32(); break;
	case 49: m.verb = Verb::ComeOut; m.who = a.i32(); m.ox = a.i32(); m.uid = uint16_t(a.i16()); break;
	case 52: m.verb = Verb::SpeedSet; m.speed = a.i32(); break;
	case 53: m.verb = Verb::SpeedUp; break;
	case 54: m.verb = Verb::SpeedDown; break;
	case 59: m.verb = Verb::CheatViewAll; m.who = a.i32(); break;
	case 60: m.verb = Verb::CheatGiveTechs; m.who = a.i32(); break;
	case 61: m.verb = Verb::CheatZeroTechs; m.who = a.i32(); break;
	case 62: m.verb = Verb::CheatAiSpeedIncrease; break;
	case 63: m.verb = Verb::CheatAiSpeedNormal; break;
	case 64: m.verb = Verb::CheatAiToggle; break;
	case 65: m.verb = Verb::CheatIncreaseBuckets; m.who = a.i32(); break;
	case 66: m.verb = Verb::CheatZeroBuckets; m.who = a.i32(); break;
	case 67: m.verb = Verb::CheatInitUnit; m.who = a.i32(); m.type = a.i32(); m.x = a.i32(); m.y = a.i32(); break;
	case 68: {  // CHAT: bits, taunt, taunt_num, n, n x u16, u16 0
		m.verb = Verb::Chat;
		m.chat_bits = a.i32(); m.chat_taunt = a.i32(); m.chat_taunt_num = a.i32();
		const int32_t n = a.i32();
		for (int32_t i = 0; i < n; ++i) m.text.push_back(char16_t(ldu16(cmd + a.o + 2 * size_t(i))));
		break;
	}
	case 75: {  // RENAME_CITY: who, o, u16 name[22]; the name ends at its first 0 and the rest is the sender's stack garbage, kept so that the command re-encodes
		m.verb = Verb::RenameCity;
		m.who = a.i32(); m.ox = a.i32();
		for (int i = 0; i < 22; ++i) m.text.push_back(char16_t(ldu16(cmd + a.o + 2 * size_t(i))));
		break;
	}
	case 76: m.verb = Verb::Pause; m.set = cmd[1]; break;
	case 73:
		m.verb = Verb::LeaderOptions;
		m.who = a.i32(); m.peasants = a.i32(); m.peasants_wait = a.i32(); m.buildings = a.i32();
		a.o += 12;  // BitMask<32> {bits, size, flags}
		m.option_flags = a.u32();
		break;
	default:
		return false;
	}
	out = std::move(m);
	return true;
}

bool engine_to_wire(const Command& c, std::vector<uint8_t>& out) {
	auto i32 = [&](int32_t v) { put32(out, uint32_t(v)); };
	auto i8 = [&](int8_t v) { out.push_back(uint8_t(v)); };
	auto q32 = [&](int8_t v) { i32(v); };
	const size_t mark = out.size();
	switch (c.verb) {
	case Verb::Group:
		if (c.objects.size() > 255) return false;
		out.push_back(0);
		out.push_back(uint8_t(c.objects.size()));
		i8(int8_t(c.who));
		for (uint16_t o : c.objects) put16(out, o);
		return true;
	case Verb::Stance: out.push_back(2); i32(c.stance); return true;
	case Verb::Form: out.push_back(3); i32(c.form32); i32(c.rotate); q32(c.queued); return true;
	case Verb::Attack: out.push_back(4); i32(c.ox); i32(c.whom); i32(c.ignore); q32(c.queued); return true;
	case Verb::SwarmAround: out.push_back(6); i32(c.ox); i32(c.whom); q32(c.queued); i32(c.order_kind); return true;
	case Verb::MoveTo:
		out.push_back(7); i32(c.x); i32(c.y); i32(c.set_angle); i32(int32_t(c.angle));
		i8(c.orders); i8(c.queued); i8(c.form); i8(c.width); i8(c.disembark);
		return true;
	case Verb::AttackGround: out.push_back(9); i32(c.x); i32(c.y); i8(c.queued); return true;
	case Verb::Patrol: out.push_back(10); i32(c.x); i32(c.y); i8(c.queued); return true;
	case Verb::LaunchPatrol: out.push_back(11); i32(c.x); i32(c.y); q32(c.queued); i32(c.shift); i32(c.ctrl); i32(c.alt); return true;
	case Verb::Halt: out.push_back(12); return true;
	case Verb::Transport: out.push_back(13); return true;
	case Verb::SetTransport: out.push_back(14); i32(c.flags); return true;
	case Verb::BoardShip: out.push_back(15); i32(c.ox); q32(c.queued); return true;
	case Verb::Trade: out.push_back(17); i32(c.ox); i32(c.whom); i32(c.x2); i32(c.y2); q32(c.queued); return true;
	case Verb::CityGather: out.push_back(18); i32(c.type); q32(c.queued); return true;
	case Verb::Gather: out.push_back(19); i32(c.ox); q32(c.queued); return true;
	case Verb::Garrison: out.push_back(20); i32(c.ox); i32(c.whom); q32(c.queued); return true;
	case Verb::Disband: out.push_back(21); i32(c.all); return true;
	case Verb::GatherPoint: out.push_back(22); i32(c.x); i32(c.y); i32(c.action); i32(c.add_to_end); return true;
	case Verb::Spell: out.push_back(23); i32(c.ox); i32(c.whom); i32(c.type); i32(c.x); i32(c.y); return true;
	case Verb::QueueUp: out.push_back(24); i32(c.type); i32(c.num); return true;
	case Verb::Build: out.push_back(25); i32(c.x); i32(c.y); i32(c.x2); i32(c.y2); i32(c.type); q32(c.queued); return true;
	case Verb::EjectAll: out.push_back(26); i32(c.back_to_work); i32(c.who); i32(c.eject_o); i32(c.eject_who); return true;
	case Verb::Flight: out.push_back(28); i32(c.ox); i32(c.whom); i32(c.shift); i32(c.ctrl); i32(c.alt); i32(c.orders); return true;
	case Verb::Follow: out.push_back(30); i32(c.ox); i32(c.whom); q32(c.queued); return true;
	case Verb::Guard: out.push_back(31); i32(c.ox); i32(c.whom); q32(c.queued); return true;
	case Verb::UnitMask: out.push_back(32); i32(c.mask); i32(c.set); return true;
	case Verb::BuildMask: out.push_back(33); i32(c.mask); i32(c.set); return true;
	case Verb::Recall: out.push_back(35); return true;
	case Verb::Scramble: out.push_back(36); return true;
	case Verb::ClearTributes: out.push_back(39); i32(c.who); i32(c.whom); return true;
	case Verb::Accept: out.push_back(41); i32(c.who); i32(c.whom); return true;
	case Verb::Reject: out.push_back(42); i32(c.who); i32(c.whom); return true;
	case Verb::Tribute: out.push_back(43); i32(c.who); i32(c.whom); i32(c.good); i32(c.amount); return true;
	case Verb::DemandTribute: out.push_back(44); i32(c.who); i32(c.whom); i32(c.good); i32(c.amount); return true;
	case Verb::Buy: out.push_back(46); i32(c.who); i32(c.good); i32(c.flags); return true;
	case Verb::Sell: out.push_back(47); i32(c.who); i32(c.good); i32(c.flags); return true;
	case Verb::Unqueue: out.push_back(48); i32(c.who); i32(c.ox); i32(c.num); put16(out, c.uid); return true;
	case Verb::Hotkey: {
		out.push_back(34); i32(c.hk_group); i32(c.hk_clear); i32(c.hk_valid);
		uint32_t bx, by;
		std::memcpy(&bx, &c.hk_x, 4);
		std::memcpy(&by, &c.hk_y, 4);
		put32(out, bx); put32(out, by); i32(c.hk_zoom);
		return true;
	}
	case Verb::ComeOut: out.push_back(49); i32(c.who); i32(c.ox); put16(out, c.uid); return true;
	case Verb::SpeedSet: out.push_back(52); i32(c.speed); return true;
	case Verb::SpeedUp: out.push_back(53); return true;
	case Verb::SpeedDown: out.push_back(54); return true;
	case Verb::CheatViewAll: out.push_back(59); i32(c.who); return true;
	case Verb::CheatGiveTechs: out.push_back(60); i32(c.who); return true;
	case Verb::CheatZeroTechs: out.push_back(61); i32(c.who); return true;
	case Verb::CheatAiSpeedIncrease: out.push_back(62); return true;
	case Verb::CheatAiSpeedNormal: out.push_back(63); return true;
	case Verb::CheatAiToggle: out.push_back(64); return true;
	case Verb::CheatIncreaseBuckets: out.push_back(65); i32(c.who); return true;
	case Verb::CheatZeroBuckets: out.push_back(66); i32(c.who); return true;
	case Verb::CheatInitUnit: out.push_back(67); i32(c.who); i32(c.type); i32(c.x); i32(c.y); return true;
	case Verb::Chat:
		if (c.text.size() > 240) return false;
		out.push_back(68); i32(c.chat_bits); i32(c.chat_taunt); i32(c.chat_taunt_num); i32(int32_t(c.text.size()));
		for (char16_t ch : c.text) put16(out, uint16_t(ch));
		put16(out, 0);
		return true;
	case Verb::RenameCity: {
		out.push_back(75); i32(c.who); i32(c.ox);
		for (size_t i = 0; i < 22; ++i) put16(out, i < c.text.size() ? uint16_t(c.text[i]) : uint16_t(0));
		return true;
	}
	case Verb::Pause: out.push_back(76); out.push_back(uint8_t(c.set)); return true;
	case Verb::Resign: out.push_back(70); i32(c.who); return true;
	case Verb::Quit: out.push_back(71); i32(c.who); out.push_back(0); out.push_back(0); return true;
	case Verb::LeaderOptions:
		out.push_back(73); i32(c.who); i32(c.peasants); i32(c.peasants_wait); i32(c.buildings);
		i32(32); i32(4); i32(2);  // BitMask<32>: bits, size, and the word the corpus always has as 2; the value follows
		put32(out, c.option_flags);
		return true;
	default:
		out.resize(mark);
		return false;
	}
}

void wire_check_sums(std::vector<uint8_t>& out, const uint32_t sums[16]) {
	out.push_back(op::kCheckSums);
	for (int i = 0; i < 16; ++i) put32(out, sums[i]);
}

void wire_turn_data(std::vector<uint8_t>& out, uint16_t ping_time, uint16_t frame_average, uint16_t wait_time, uint16_t game_lag,
                    uint16_t forced_loads) {
	out.push_back(op::kTurnData);
	put16(out, ping_time);
	put16(out, frame_average);
	put16(out, wait_time);
	put16(out, game_lag);
	put16(out, forced_loads);
}

void wire_camera(std::vector<uint8_t>& out, uint8_t zoom, int32_t x, int32_t y) {
	out.push_back(op::kCamera);
	out.push_back(zoom);
	put32(out, uint32_t(x));
	put32(out, uint32_t(y));
}

void wire_pause(std::vector<uint8_t>& out, uint8_t state) {
	out.push_back(op::kPause);
	out.push_back(state);
}

void wire_resign(std::vector<uint8_t>& out, int32_t play) {
	out.push_back(op::kResign);
	put32(out, uint32_t(play));
}

void wire_chat_set(std::vector<uint8_t>& out, const std::array<uint8_t, 8>& status) {
	out.push_back(op::kChatSet);
	out.insert(out.end(), status.begin(), status.end());
}

bool parse_chat_set(const uint8_t* cmd, size_t len, std::array<uint8_t, 8>& status) {
	if (len != 9 || cmd[0] != op::kChatSet) return false;
	std::copy(cmd + 1, cmd + 9, status.begin());
	return true;
}

void wire_drop(std::vector<uint8_t>& out, uint8_t play, uint8_t option) {
	out.push_back(op::kUngracefulDrop);
	out.push_back(play);
	out.push_back(option);
}

}  // namespace ron::sim
