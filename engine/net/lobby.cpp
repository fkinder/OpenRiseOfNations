#include "net/lobby.hpp"

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
	Bytes m{lobbymsg::kReady, uint8_t(ready ? 1 : 0)};
	t_.broadcast(m, false);
}

bool Lobby::all_ready() const {
	if (!me_ready_) return false;
	for (int p : t_.peers())
		if (!ready_.count(p)) return false;
	return true;
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
			}
			++handled;
			break;
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
			started_ = true;
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
