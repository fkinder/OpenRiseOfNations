// A plain TCP Transport for a LAN game (sockets only, no third-party library). A star: the host (slot 0) listens, every client
// connects to it and gets the next slot number; the host relays client-to-client traffic, so the lockstep semantics (reliable,
// ordered per sender) hold and nothing about the sim changes (multiplayer-spec 8.1 "star-relay via host is allowed").
//
// Frame on the wire: u32 length of what follows | u8 kind | u8 source slot | u8 destination slot | payload.
// Kinds: 0 data, 1 welcome (host -> client: its slot, the number of peers, [bit mask of the slots present]), 2 peer gone (host -> clients:
// the slot), 3 peer joined (host -> clients: the slot; lobby mode only).
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "net/lockstep.hpp"

namespace ron::net {

// "host" or "host:port" (a host name or an IPv4 address, "tcp://" allowed before it) as typed in a join field or given on a command line. The port
// is 1024..65535 when present, `default_port` otherwise. False with `error` for an empty / malformed text.
struct Endpoint {
	std::string host;
	uint16_t port = 7777;
};
bool parse_endpoint(const std::string& text, uint16_t default_port, Endpoint& out, std::string& error);
// The IPv4 addresses a host name resolves to (an address literal resolves to itself). Empty with `error` when it cannot be found.
std::vector<std::string> resolve_ipv4(const std::string& host, std::string& error);
// The non-loopback IPv4 addresses of this machine's interfaces that are up (what a LAN player types), and the text a host shows or prints for
// internet play: no UPnP, the LAN address, the port to forward and the NAT limitation (docs/multiplayer-spec.md 8.7).
std::vector<std::string> local_addresses();
std::string public_host_note(uint16_t port);

class TcpTransport : public Transport {
public:
	// Listens on `port` and waits until `clients` peers have connected (or `timeout_ms` passed). Null with `error` on failure.
	static std::unique_ptr<TcpTransport> host(uint16_t port, int clients, int timeout_ms, std::string& error);
	// Lobby mode: listens at once and returns; clients are accepted whenever the transport is pumped (poll / idle / accept_new), each
	// gets the lowest free slot 1..7, and the others are told. A client that leaves shows up as a timeout notice. `close_door()` ends the
	// accepting when the game starts: the set of peers is final.
	static std::unique_ptr<TcpTransport> host_open(uint16_t port, std::string& error);
	void accept_new();
	void close_door();
	void reopen_door();   // host: listens again on the port host_open used (the room is a lobby again after a game)
	// Host: ends a client's connection (the host removing a player); the slot is free for the next arrival. The usual timeout notice follows.
	void disconnect(int slot);
	bool door_open() const { return open_ && listen_fd_ >= 0; }
	uint32_t present_mask() const;  // bit s: slot s is connected (this peer included)
	// Connects to a host and waits for its welcome. Retries the connection until `timeout_ms` (the host may not be listening yet).
	static std::unique_ptr<TcpTransport> join(const std::string& address, uint16_t port, int timeout_ms, std::string& error);
	~TcpTransport() override;

	int local_peer() const override { return me_; }
	std::vector<int> peers() const override;
	bool is_host() const override { return me_ == 0; }
	bool send(int peer, const Bytes& m) override;
	uint64_t now_ms() const override;
	void idle(int ms) override;
	int peer_count() const { return total_; }

protected:
	bool poll_impl(int& from, Bytes& m) override;
	bool timeout_impl(int& peer) override;

private:
	struct Conn {
		int fd = -1;
		int slot = -1;
		bool alive = false;
		Bytes in, out;
	};
	TcpTransport() = default;
	void pump_io();
	bool read_conn(Conn& c);
	void flush_conn(Conn& c);
	void handle_frame(Conn& from, uint8_t kind, uint8_t src, uint8_t dst, const uint8_t* p, size_t n);
	void write_frame(Conn& c, uint8_t kind, uint8_t src, uint8_t dst, const uint8_t* p, size_t n);
	void lost(Conn& c);

	int me_ = 0, total_ = 1;
	uint16_t port_ = 0;
	bool open_ = false;  // lobby mode
	int listen_fd_ = -1;
	std::vector<Conn> conns_;  // host: one per client; client: the connection to the host (one entry)
	std::vector<bool> gone_;
	std::deque<std::pair<int, Bytes>> inbox_;
	std::deque<int> timeouts_;
};

}  // namespace ron::net
