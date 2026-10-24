#include "net/seat_transport.hpp"

#include <cstring>

namespace ron::net {

SeatTransport::SeatTransport(Transport& inner, const std::array<int, kMaxSlots>& peer_of) : inner_(inner), peer_of_(peer_of) {
	slot_of_.fill(-1);
	for (int s = 0; s < kMaxSlots; ++s)
		if (peer_of_[size_t(s)] >= 0 && peer_of_[size_t(s)] < kMaxSlots) slot_of_[size_t(peer_of_[size_t(s)])] = s;
}

std::vector<int> SeatTransport::peers() const {
	std::vector<int> v;
	for (int p : inner_.peers()) {
		const int s = slot_of(p);
		if (s >= 0) v.push_back(s);
	}
	return v;
}

bool SeatTransport::send(int slot, const Bytes& m) {
	const int p = peer_of(slot);
	return p >= 0 && inner_.send(p, m);
}

bool SeatTransport::poll_impl(int& from, Bytes& m) {
	int pf;
	Bytes b;
	while (inner_.poll(pf, b)) {
		if (!b.empty() && lobbymsg::is_lobby(b[0])) {
			held_.emplace_back(pf, std::move(b));
			continue;
		}
		const int s = slot_of(pf);
		if (s < 0) continue;   // a machine that is not in the game
		from = s;
		m = std::move(b);
		return true;
	}
	return false;
}

bool SeatTransport::timeout_impl(int& peer) {
	int p;
	while (inner_.take_timeout(p)) {
		const int s = slot_of(p);
		if (s < 0) continue;
		peer = s;
		return true;
	}
	return false;
}

bool SeatTransport::take_oob_chat(OobChat& out) {
	for (size_t i = 0; i < held_.size();) {
		if (held_[i].second.empty() || held_[i].second[0] != lobbymsg::kChat) {
			++i;
			continue;
		}
		const int slot = slot_of(held_[i].first);
		const Bytes b = std::move(held_[i].second);
		held_.erase(held_.begin() + std::ptrdiff_t(i));
		if (b.size() < 4 || slot < 0) continue;
		const size_t n = size_t(b[2]) | size_t(b[3]) << 8;
		if (4 + n > b.size()) continue;
		out.slot = slot;
		out.kind = b[1];
		out.text.assign(reinterpret_cast<const char*>(b.data()) + 4, n);
		return true;
	}
	return false;
}

void SeatTransport::release_lobby_traffic() {
	for (size_t i = held_.size(); i-- > 0;) inner_.push_front(held_[i].first, std::move(held_[i].second));
	held_.clear();
}

}  // namespace ron::net
