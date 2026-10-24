#include "net/lobby.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace ron::net {

namespace {

void put_str(Bytes& o, const std::string& s) {
	const size_t n = s.size() > 0xffff ? 0xffff : s.size();
	o.push_back(uint8_t(n));
	o.push_back(uint8_t(n >> 8));
	o.insert(o.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
}

bool get_str(const Bytes& m, size_t& p, std::string& out) {
	if (p + 2 > m.size()) return false;
	const size_t n = size_t(m[p]) | size_t(m[p + 1]) << 8;
	p += 2;
	if (p + n > m.size()) return false;
	out.assign(reinterpret_cast<const char*>(m.data()) + p, n);
	p += n;
	return true;
}

std::string seat_key(const char* base, int n) { return std::string(base) + "_" + std::to_string(n); }

}  // namespace

SeatPlan make_seat_plan(uint32_t present, uint32_t observers, int wanted_nations, bool identity, const std::array<int, kMaxSlots>* forced) {
	SeatPlan p;
	std::vector<int> players, watchers;
	for (int i = 0; i < kMaxSlots; ++i) {
		if (!((present >> i) & 1u)) continue;
		((observers >> i) & 1u ? watchers : players).push_back(i);
	}
	p.players = int(players.size());
	p.observers = int(watchers.size());
	int need = std::max(2, p.players);
	if (identity && !players.empty()) need = std::max(need, players.back() + 1);
	if (forced)
		for (int peer : players)
			if ((*forced)[size_t(peer)] >= 0) need = std::max(need, (*forced)[size_t(peer)] + 1);
	p.nations = std::clamp(std::max(wanted_nations, need), 2, kMaxSlots);
	std::array<bool, kMaxSlots> taken{};
	std::vector<int> loose;
	for (int peer : players) {
		int slot = identity ? peer : forced ? (*forced)[size_t(peer)] : -1;
		if (slot >= 0 && (slot >= p.nations || taken[size_t(slot)])) {
			p.overflow = true;
			slot = -1;
		}
		if (slot < 0 && !identity) {
			loose.push_back(peer);
			continue;
		}
		if (slot < 0) continue;
		taken[size_t(slot)] = true;
		p.peer_of[size_t(slot)] = peer;
		p.slot_of[size_t(peer)] = slot;
	}
	int next = 0;
	for (int peer : loose) {
		while (next < p.nations && taken[size_t(next)]) ++next;
		if (next >= p.nations) {
			p.overflow = true;
			break;
		}
		taken[size_t(next)] = true;
		p.peer_of[size_t(next)] = peer;
		p.slot_of[size_t(peer)] = next;
	}
	for (size_t k = 0; k < watchers.size(); ++k) {
		const int slot = p.nations + int(k);
		if (slot >= kMaxSlots) {
			p.overflow = true;
			continue;
		}
		p.peer_of[size_t(slot)] = watchers[k];
		p.slot_of[size_t(watchers[k])] = slot;
	}
	return p;
}

const char* reply_text(int code) {
	switch (code) {
	case kAccepted: return "accepted";
	case kInvalid: return "that is not allowed";
	case kTaken: return "that choice is already taken by another seat";
	case kNoRoom: return "there is no room for that";
	case kDenied: return "the host said no";
	case kLocked: return "the host fixed the seats of this game";
	default: return "refused";
	}
}

const char* seat_field_name(SeatField f) {
	switch (f) {
	case SeatField::Tribe: return "nation";
	case SeatField::Team: return "team";
	case SeatField::Color: return "colour";
	case SeatField::Handicap: return "handicap";
	case SeatField::Observer: return "observer seat";
	}
	return "setting";
}

std::set<int> Lobby::present() const {
	std::set<int> s{t_.local_peer()};
	for (int p : t_.peers()) s.insert(p);
	return s;
}

SeatPlan Lobby::plan(int wanted_nations) const {
	uint32_t present_mask = 0, obs = 0;
	for (int p : present()) {
		present_mask |= 1u << p;
		if (observer_of(p)) obs |= 1u << p;
	}
	std::array<int, kMaxSlots> forced;
	forced.fill(-1);
	for (int p = 0; p < kMaxSlots; ++p) forced[size_t(p)] = int(get_int(seat_key("force", p), -1));
	return make_seat_plan(present_mask, obs, wanted_nations >= 0 ? wanted_nations : int(get_int("num_nations", 2)), get_int("identity") != 0, &forced);
}

SeatPlan Lobby::start_plan() const {
	SeatPlan p;
	if (!has("nations_final")) return p;
	p.nations = int(get_int("nations_final", 2));
	for (int peer = 0; peer < kMaxSlots; ++peer) {
		const int slot = int(get_int(seat_key("seat", peer), -1));
		if (slot < 0 || slot >= kMaxSlots) continue;
		p.peer_of[size_t(slot)] = peer;
		p.slot_of[size_t(peer)] = slot;
		(slot < p.nations ? p.players : p.observers) += 1;
	}
	return p;
}

ReplyCode Lobby::check(int peer, SeatField f, int v) const {
	if (get_int("from_save") != 0 || get_int("identity") != 0) return kLocked;
	const SeatPlan pl = plan();
	if (pl.slot_of[size_t(peer & 7)] < 0) return kInvalid;
	if (f == SeatField::Observer) {
		if (v != 0 && v != 1) return kInvalid;
		const bool now = observer_of(peer);
		if (now == (v == 1)) return kAccepted;
		const int wanted = int(get_int("num_nations", 2));
		if (v == 1) {  // becomes an observer: somebody must still play, the host allows that many, and the slots must fit
			if (pl.players - 1 < 1) return kNoRoom;
			if (pl.observers + 1 > std::min<int>(int(get_int("max_observers", 0)), limits_.max_observers)) return kNoRoom;
			if (std::max(std::max(2, pl.players - 1), wanted) + pl.observers + 1 > kMaxSlots) return kNoRoom;
		} else {       // becomes a player
			if (std::max(std::max(2, pl.players + 1), wanted) + pl.observers - 1 > kMaxSlots) return kNoRoom;
		}
		return kAccepted;
	}
	if (observer_of(peer)) return kInvalid;
	const int seat = pl.slot_of[size_t(peer & 7)];
	if (f == SeatField::Handicap) return v >= 0 && v < limits_.handicaps ? kAccepted : kInvalid;
	if (get_int("scenario_game") != 0 && f != SeatField::Handicap) return kLocked;  // a scenario fixes nation, colour and team
	switch (f) {
	case SeatField::Tribe:
		if (v < -1 || v >= limits_.tribes) return kInvalid;
		if (v >= 0)
			for (int s = 0; s < pl.nations; ++s)
				if (s != seat && int(get_int(seat_key("tribe", s), -1)) == v) return kTaken;
		return kAccepted;
	case SeatField::Team: return v >= -1 && v < limits_.teams ? kAccepted : kInvalid;
	case SeatField::Color:
		if (v < 0 || v >= limits_.colors) return kInvalid;
		for (int s = 0; s < pl.nations; ++s) {
			if (s == seat) continue;
			int c = int(get_int(seat_key("color", s), -1));
			if (c < 0 || c > 7) c = s;
			if (c == v) return kTaken;
		}
		return kAccepted;
	default: return kInvalid;
	}
}

void Lobby::apply(int peer, SeatField f, int v) {
	if (!t_.is_host()) return;
	if (f == SeatField::Observer) {
		set(seat_key("obs", peer), v);
		return;
	}
	const int seat = plan().slot_of[size_t(peer & 7)];
	if (seat < 0) return;
	switch (f) {
	case SeatField::Tribe: set(seat_key("tribe", seat), v); break;
	case SeatField::Team: set(seat_key("team", seat), v); break;
	case SeatField::Color: set(seat_key("color", seat), v); break;
	case SeatField::Handicap: set(seat_key("handicap", seat), v); break;
	default: break;
	}
}

uint32_t Lobby::request(SeatField f, int value) {
	const uint32_t id = next_id_++;
	if (t_.is_host()) {
		const int me = t_.local_peer();
		Reply r;
		r.field = f, r.value = value, r.code = check(me, f, value);
		if (r.code == kAccepted) apply(me, f, value);
		replies_.push_back(r);
		return id;
	}
	Bytes m{lobbymsg::kRequest, uint8_t(f)};
	for (int i = 0; i < 4; ++i) m.push_back(uint8_t(uint32_t(value) >> (8 * i)));
	t_.send(0, m);
	return id;
}

bool Lobby::take_reply(Reply& out) {
	if (replies_.empty()) return false;
	out = replies_.front();
	replies_.pop_front();
	return true;
}

bool Lobby::take_request(SeatRequest& out) {
	if (events_.empty()) return false;
	out = events_.front();
	events_.pop_front();
	return true;
}

void Lobby::answer(uint32_t id, bool yes) {
	if (!t_.is_host()) return;
	for (auto it = pending_.begin(); it != pending_.end(); ++it) {
		if (it->id != id) continue;
		SeatRequest r = *it;
		pending_.erase(it);
		int code = kDenied;
		if (yes) {
			code = check(r.peer, r.field, r.value);  // the room may have changed while the host thought about it
			if (code == kAccepted) {
				apply(r.peer, r.field, r.value);
				r.pending = false;
				r.seat = plan().slot_of[size_t(r.peer & 7)];
				events_.push_back(r);
			}
		}
		Bytes m{lobbymsg::kReply, uint8_t(r.field)};
		for (int i = 0; i < 4; ++i) m.push_back(uint8_t(uint32_t(r.value) >> (8 * i)));
		m.push_back(uint8_t(code));
		t_.send(r.peer, m);
		return;
	}
}

void Lobby::kick(int peer) {
	if (!t_.is_host() || peer == t_.local_peer()) return;
	t_.send(peer, Bytes{lobbymsg::kKick});
}

void Lobby::countdown_set(int seconds) {
	if (!t_.is_host()) return;
	countdown_ = seconds > 0 ? seconds : -1;
	Bytes m{lobbymsg::kCountdown, uint8_t(seconds > 0 ? seconds : 0xFF)};
	t_.broadcast(m, false);
	ChatLine l;
	l.from = -1;
	l.kind = 3;
	l.text = seconds > 0 ? "The game starts in " + std::to_string(seconds) + (seconds == 1 ? " second..." : " seconds...") : "The countdown was cancelled.";
	inbox_.push_back(l);
	log_.push_back(l);
}

void Lobby::reset_game() {
	// Every machine calls this when its own game is over (nothing is broadcast: the machines finish at their own pace, and the files and table
	// entries the host sent while the game ran must survive). The file stays until the next one replaces it.
	started_ = false;
	barrier_.clear();
	my_barriers_.clear();
	ready_.clear();
	me_ready_ = false;
	countdown_ = -1;
	events_.clear();
	pending_.clear();
	if (t_.is_host()) {
		for (int p : t_.peers()) set(seat_key("ready", p), 0);
		for (int p = 0; p < kMaxSlots; ++p) set(seat_key("force", p), "");
		set("from_save", "");
		set("ctw_game", "");
		set_ready(true);
	}
}

bool Lobby::say_to(int kind, int peer, const std::string& text) {
	if (text.empty()) return false;
	Bytes m{lobbymsg::kChat, uint8_t(kind)};
	put_str(m, text);
	const SeatPlan pl = plan();
	const int me = t_.local_peer();
	std::vector<int> to;
	if (kind == 0) {
		to = t_.peers();
	} else if (kind == 1) {
		const bool obs = observer_of(me);
		const int myseat = pl.slot_of[size_t(me)];
		const int myteam = !obs && myseat >= 0 ? int(get_int(seat_key("team", myseat), -1)) : -1;
		if (!obs && myteam < 0) return false;
		for (int p : t_.peers()) {
			const int seat = pl.slot_of[size_t(p)];
			if (obs ? observer_of(p) : (!observer_of(p) && seat >= 0 && int(get_int(seat_key("team", seat), -1)) == myteam)) to.push_back(p);
		}
		if (to.empty()) return false;
	} else if (kind == 2) {
		if (peer < 0 || peer >= kMaxSlots || peer == me) return false;
		const std::vector<int> ps = t_.peers();
		if (std::find(ps.begin(), ps.end(), peer) == ps.end()) return false;
		to.push_back(peer);
	} else {
		return false;
	}
	for (int p : to) t_.send(p, m);
	ChatLine l;
	l.from = me;
	l.text = text;
	l.kind = kind;
	l.to = peer;
	log_.push_back(l);
	if (log_.size() > 200) log_.pop_front();
	return true;
}

void Lobby::set(const std::string& key, const std::string& value) {
	if (!t_.is_host()) return;
	kv_[key] = value;
	Bytes m{lobbymsg::kSet};
	put_str(m, key);
	put_str(m, value);
	t_.broadcast(m, false);
}

std::string Lobby::get(const std::string& key, const std::string& def) const {
	auto it = kv_.find(key);
	return it == kv_.end() ? def : it->second;
}

int64_t Lobby::get_int(const std::string& key, int64_t def) const {
	auto it = kv_.find(key);
	if (it == kv_.end() || it->second.empty()) return def;
	return std::strtoll(it->second.c_str(), nullptr, 10);
}

void Lobby::set_ready(bool ready) {
	me_ready_ = ready;
	if (t_.is_host()) {
		set("ready_" + std::to_string(t_.local_peer()), ready ? 1 : 0);
		return;
	}
	Bytes m{lobbymsg::kReady, uint8_t(ready ? 1 : 0)};
	t_.broadcast(m, false);
}

bool Lobby::ready_of(int slot) const {
	if (slot == t_.local_peer()) return me_ready_;
	return get_int("ready_" + std::to_string(slot)) != 0;
}

bool Lobby::all_peers_ready() const {
	for (int p : t_.peers())
		if (!ready_of(p)) return false;
	return true;
}

void Lobby::set_name(const std::string& name) {
	if (t_.is_host()) {
		set("name_" + std::to_string(t_.local_peer()), name);
		return;
	}
	Bytes m{lobbymsg::kName};
	put_str(m, name);
	t_.send(0, m);
}

void Lobby::say(const std::string& text) { say_to(0, -1, text); }

bool Lobby::take_chat(ChatLine& out) {
	if (inbox_.empty()) return false;
	out = std::move(inbox_.front());
	inbox_.pop_front();
	return true;
}

void Lobby::peer_left(int slot) {
	ready_.erase(slot);
	known_.erase(slot);
	if (t_.is_host()) {
		set("ready_" + std::to_string(slot), "");
		set("name_" + std::to_string(slot), "");
		set("obs_" + std::to_string(slot), "");
		for (auto it = pending_.begin(); it != pending_.end();) it = it->peer == slot ? pending_.erase(it) : it + 1;
	}
}

bool Lobby::all_ready() const {
	if (!me_ready_) return false;
	for (int p : t_.peers())
		if (!ready_.count(p)) return false;
	return true;
}

void Lobby::send_file(const Bytes& raw) {
	if (!t_.is_host()) return;
	uLongf cap = compressBound(uLong(raw.size()));
	Bytes comp(cap);
	if (compress2(comp.data(), &cap, raw.data(), uLong(raw.size()), 3) != Z_OK) return;
	comp.resize(cap);
	const uint32_t chunk = 16000;
	const uint32_t chunks = uint32_t(std::max<size_t>(1, (comp.size() + chunk - 1) / chunk));
	const uint32_t crc = uint32_t(crc32(0, raw.data(), uInt(raw.size())));
	auto put32 = [](Bytes& o, uint32_t v) {
		for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
	};
	Bytes begin{lobbymsg::kFile, 0};
	put32(begin, uint32_t(raw.size()));
	put32(begin, uint32_t(comp.size()));
	put32(begin, crc);
	put32(begin, chunks);
	put32(begin, chunk);
	t_.broadcast(begin, false);
	for (uint32_t i = 0; i < chunks; ++i) {
		Bytes d{lobbymsg::kFile, 1};
		put32(d, i);
		const size_t a = size_t(i) * chunk, e = std::min(comp.size(), a + chunk);
		d.insert(d.end(), comp.begin() + std::ptrdiff_t(a), comp.begin() + std::ptrdiff_t(e));
		t_.broadcast(d, false);
	}
}

bool Lobby::start(uint32_t seed) {
	if (!t_.is_host()) return false;
	const SeatPlan pl = plan();
	if (pl.players < 1 || pl.overflow) return false;
	for (int p = 0; p < kMaxSlots; ++p) kv_.erase(seat_key("seat", p));
	for (int p : present()) kv_[seat_key("seat", p)] = std::to_string(pl.slot_of[size_t(p)]);
	kv_["nations_final"] = std::to_string(pl.nations);
	kv_["num_nations"] = std::to_string(pl.nations);
	countdown_ = -1;
	kv_["game_seed"] = std::to_string(seed);
	Bytes m{lobbymsg::kStart};
	const uint32_t n = uint32_t(kv_.size());
	for (int i = 0; i < 4; ++i) m.push_back(uint8_t(n >> (8 * i)));
	for (const auto& kv : kv_) {
		put_str(m, kv.first);
		put_str(m, kv.second);
	}
	t_.broadcast(m, false);
	started_ = true;
	return true;
}

void Lobby::barrier(int id) {
	my_barriers_.insert(id);
	Bytes m{lobbymsg::kBarrier};
	for (int i = 0; i < 4; ++i) m.push_back(uint8_t(uint32_t(id) >> (8 * i)));
	t_.broadcast(m, false);
}

bool Lobby::barrier_done(int id) const {
	if (!my_barriers_.count(id)) return false;
	auto it = barrier_.find(id);
	for (int p : t_.peers())
		if (it == barrier_.end() || !it->second.count(p)) return false;
	return true;
}

int Lobby::poll() {
	int handled = 0;
	if (t_.is_host() && !started_) {
		// a peer that appeared since the last poll gets the whole table
		const std::vector<int> now = t_.peers();
		for (int p : now)
			if (!known_.count(p)) {
				known_.insert(p);
				Bytes m{lobbymsg::kSnapshot};
				const uint32_t n = uint32_t(kv_.size());
				for (int i = 0; i < 4; ++i) m.push_back(uint8_t(n >> (8 * i)));
				for (const auto& kv : kv_) {
					put_str(m, kv.first);
					put_str(m, kv.second);
				}
				t_.send(p, m);
			}
		for (auto it = known_.begin(); it != known_.end();) {
			if (std::find(now.begin(), now.end(), *it) == now.end()) it = known_.erase(it);
			else ++it;
		}
	}
	std::vector<std::pair<int, Bytes>> other;
	int from;
	Bytes m;
	while (t_.poll(from, m)) {
		if (m.empty()) continue;
		switch (m[0]) {
		case lobbymsg::kSet: {
			size_t p = 1;
			std::string k, v;
			if (get_str(m, p, k) && get_str(m, p, v)) kv_[k] = v;
			++handled;
			break;
		}
		case lobbymsg::kReady:
			if (m.size() == 2) {
				if (m[1]) ready_.insert(from);
				else ready_.erase(from);
				if (t_.is_host()) set("ready_" + std::to_string(from), m[1] ? 1 : 0);
			}
			++handled;
			break;
		case lobbymsg::kFile: {
			auto ld = [&](size_t at) {
				uint32_t v;
				std::memcpy(&v, m.data() + at, 4);
				return v;
			};
			if (m.size() == 22 && m[1] == 0) {  // begin: raw, comp, crc, chunks, chunk size
				file_raw_ = ld(2);
				file_crc_ = ld(10);
				file_chunks_ = ld(14);
				file_chunk_ = ld(18);
				const uint32_t comp = ld(6);
				if (file_chunks_ == 0 || file_chunks_ > (1u << 20) || comp > (1u << 30) || file_raw_ > (1u << 30) || file_chunk_ == 0) {
					file_bad_ = true;
				} else {
					file_comp_.assign(comp, 0);
					file_have_.assign(file_chunks_, false);
					file_got_ = 0;
					file_done_ = file_bad_ = false;
				}
			} else if (m.size() >= 6 && m[1] == 1 && !file_have_.empty()) {
				const uint32_t i = ld(2);
				const size_t n = m.size() - 6, at = size_t(i) * file_chunk_;
				if (i < file_chunks_ && !file_have_[i] && at + n <= file_comp_.size()) {
					std::memcpy(file_comp_.data() + at, m.data() + 6, n);
					file_have_[i] = true;
					if (++file_got_ == file_chunks_) {
						Bytes raw(file_raw_);
						uLongf outn = uLongf(raw.size());
						if (uncompress(raw.data(), &outn, file_comp_.data(), uLong(file_comp_.size())) == Z_OK && outn == raw.size() &&
						    uint32_t(crc32(0, raw.data(), uInt(raw.size()))) == file_crc_) {
							file_ = std::move(raw);
							file_done_ = true;
						} else {
							file_bad_ = true;
						}
					}
				} else {
					file_bad_ = true;
				}
			}
			++handled;
			break;
		}
		case lobbymsg::kChat: {
			size_t p = 2;
			ChatLine l;
			l.from = from;
			l.kind = m.size() > 1 ? m[1] : 0;
			l.to = l.kind == 2 ? t_.local_peer() : -1;
			if (m.size() >= 4 && get_str(m, p, l.text)) {
				inbox_.push_back(l);
				log_.push_back(l);
				if (log_.size() > 200) log_.pop_front();
			}
			++handled;
			break;
		}
		case lobbymsg::kName: {
			size_t p = 1;
			std::string v;
			if (t_.is_host() && get_str(m, p, v)) {
				std::string unique = v;
				for (int n = 2;; ++n) {  // two players with one name would be indistinguishable in chat and in the game
					bool dup = false;
					for (int q : present())
						if (q != from && get("name_" + std::to_string(q)) == unique) dup = true;
					if (!dup) break;
					unique = v + " (" + std::to_string(n) + ")";
				}
				set("name_" + std::to_string(from), unique);
			}
			++handled;
			break;
		}
		case lobbymsg::kSnapshot:
		case lobbymsg::kStart: {
			if (m.size() < 5) break;
			uint32_t n;
			std::memcpy(&n, m.data() + 1, 4);
			size_t p = 5;
			std::map<std::string, std::string> kv;
			for (uint32_t i = 0; i < n; ++i) {
				std::string k, v;
				if (!get_str(m, p, k) || !get_str(m, p, v)) break;
				kv[k] = v;
			}
			kv_ = kv;
			if (m[0] == lobbymsg::kStart) started_ = true;
			++handled;
			break;
		}
		case lobbymsg::kRequest:
			if (t_.is_host() && m.size() == 6 && from >= 0 && from < kMaxSlots) {
				SeatRequest r;
				r.id = next_id_++;
				r.peer = from;
				r.field = SeatField(m[1]);
				int32_t v;
				std::memcpy(&v, m.data() + 2, 4);
				r.value = v;
				const int code = m[1] >= 1 && m[1] <= 5 ? check(from, r.field, v) : kInvalid;
				r.seat = plan().slot_of[size_t(from)];
				if (code == kAccepted && approval_ == Approval::Auto) {
					apply(from, r.field, v);
					events_.push_back(r);
				} else if (code == kAccepted) {
					r.pending = true;
					for (auto it = pending_.begin(); it != pending_.end();)  // a newer request for the same field replaces the old one
						it = it->peer == from && it->field == r.field ? pending_.erase(it) : it + 1;
					pending_.push_back(r);
					events_.push_back(r);
				}
				if (code != kAccepted || approval_ == Approval::Auto) {
					Bytes rep{lobbymsg::kReply, m[1]};
					for (int i = 0; i < 4; ++i) rep.push_back(uint8_t(uint32_t(v) >> (8 * i)));
					rep.push_back(uint8_t(code));
					t_.send(from, rep);
				}
			}
			++handled;
			break;
		case lobbymsg::kReply:
			if (m.size() == 7) {
				Reply r;
				r.field = SeatField(m[1]);
				int32_t v;
				std::memcpy(&v, m.data() + 2, 4);
				r.value = v;
				r.code = m[6];
				replies_.push_back(r);
			}
			++handled;
			break;
		case lobbymsg::kKick:
			kicked_ = true;
			++handled;
			break;
		case lobbymsg::kCountdown:
			if (m.size() == 2) {
				countdown_ = m[1] == 0xFF ? -1 : m[1];
				ChatLine l;
				l.from = -1;
				l.kind = 3;
				l.text = countdown_ > 0 ? "The game starts in " + std::to_string(countdown_) + (countdown_ == 1 ? " second..." : " seconds...") : "The countdown was cancelled.";
				inbox_.push_back(l);
				log_.push_back(l);
			}
			++handled;
			break;
		case lobbymsg::kReset: {
			const bool host = t_.is_host();
			if (!host) reset_game();
			++handled;
			break;
		}
		case lobbymsg::kBarrier:
			if (m.size() == 5) {
				int32_t id;
				std::memcpy(&id, m.data() + 1, 4);
				barrier_[id].insert(from);
			}
			++handled;
			break;
		default: other.emplace_back(from, std::move(m)); break;
		}
	}
	for (size_t i = other.size(); i-- > 0;) t_.push_front(other[i].first, std::move(other[i].second));
	return handled;
}

}  // namespace ron::net
