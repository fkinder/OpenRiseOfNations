// A view of a Transport in the game's slot numbering (docs/multiplayer-spec.md 8.6). The lobby assigns the machines game slots (players first, in the
// order of their connections, observers above the nations): this wrapper translates peer numbers to game slots in both directions, so that the
// lockstep Session (which addresses peers by game slot) runs unchanged on a room whose connection order differs from its seat order.
//
// Lobby messages (0xA0..0xAF) that arrive while the game runs are not for the Session: the wrapper keeps them (observers' out-of-band chat, a
// finishing peer's barrier) until the application takes the chat lines or hands the rest back to the raw transport.
#pragma once

#include <array>
#include <deque>
#include <string>

#include "net/lobby.hpp"
#include "net/lockstep.hpp"

namespace ron::net {

class SeatTransport : public Transport {
public:
	// `peer_of[slot]` = the inner transport's peer number of the machine in that game slot, -1 for none.
	SeatTransport(Transport& inner, const std::array<int, kMaxSlots>& peer_of);

	int local_peer() const override { return slot_of_[size_t(inner_.local_peer() & 7)]; }
	std::vector<int> peers() const override;
	bool is_host() const override { return inner_.is_host(); }
	bool send(int slot, const Bytes& m) override;
	uint64_t now_ms() const override { return inner_.now_ms(); }
	void idle(int ms) override { inner_.idle(ms); }

	int peer_of(int slot) const { return slot >= 0 && slot < kMaxSlots ? peer_of_[size_t(slot)] : -1; }
	int slot_of(int peer) const { return peer >= 0 && peer < kMaxSlots ? slot_of_[size_t(peer)] : -1; }

	// Lobby-layer chat lines that arrived during the game (an observer's, which no package carries): the sender's game slot, the text.
	struct OobChat {
		int slot = -1;
		int kind = 0;
		std::string text;
	};
	bool take_oob_chat(OobChat& out);
	// Gives the kept lobby messages (barriers ...) back to the inner transport: call when the Session is done and the Lobby takes over.
	void release_lobby_traffic();

protected:
	bool poll_impl(int& from, Bytes& m) override;
	bool timeout_impl(int& peer) override;

private:
	Transport& inner_;
	std::array<int, kMaxSlots> peer_of_, slot_of_;
	std::deque<std::pair<int, Bytes>> held_;   // lobby messages with the inner peer number
};

}  // namespace ron::net
