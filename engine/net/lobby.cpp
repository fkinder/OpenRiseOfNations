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

}  // namespace

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

void Lobby::say(const std::string& text) {
	Bytes m{lobbymsg::kChat};
	put_str(m, text);
	t_.broadcast(m, false);
	ChatLine l;
	l.from = t_.local_peer();
	l.text = text;
	log_.push_back(l);
	if (log_.size() > 200) log_.pop_front();
}

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

void Lobby::start(uint32_t seed) {
	if (!t_.is_host()) return;
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
			size_t p = 1;
			ChatLine l;
			l.from = from;
			if (get_str(m, p, l.text)) {
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
			if (t_.is_host() && get_str(m, p, v)) set("name_" + std::to_string(from), v);
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
