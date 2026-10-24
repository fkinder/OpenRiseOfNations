// LAN game discovery (docs/multiplayer-spec.md 8.6): a host announces its room in a small UDP datagram once a second (subnet broadcast, the
// limited broadcast, the multicast group 239.255.77.78 and the loopback), and the Multiplayer screen lists what it hears. Sockets only, no
// third-party library; the TCP port in the announcement is the one a guest connects to.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ron::net {

constexpr uint16_t kLanDiscoveryPort = 7778;
constexpr uint8_t kLanProtocol = 1;   // a different value = another build: listed but not joinable

struct LanGame {
	std::string name;        // the host's player name
	std::string map;         // "Old World, Medium" / the scenario's name
	std::string address;     // the host's IPv4 address (from the datagram's source)
	uint16_t port = 0;       // the TCP port to join
	int players = 0;         // machines in the room
	int seats = 0;           // seats (nations)
	uint8_t flags = 0;       // kLanScenario | kLanSaved | kLanCtw
	uint8_t protocol = kLanProtocol;
	uint64_t seen_ms = 0;    // the browser's clock when the last datagram arrived
	bool compatible() const { return protocol == kLanProtocol; }
};
constexpr uint8_t kLanScenario = 1, kLanSaved = 2, kLanCtw = 4;

std::vector<uint8_t> encode_announcement(const LanGame& g);
bool decode_announcement(const uint8_t* p, size_t n, LanGame& out);

class LanAnnouncer {
public:
	~LanAnnouncer();
	bool open(uint16_t udp_port, std::string& err);
	void close();
	bool is_open() const { return fd_ >= 0; }
	void set(const LanGame& g) { game_ = g; }
	void add_target(const std::string& ipv4);       // an extra destination (tests, a known address)
	// Sends the announcement when a second has passed since the last one (or at once the first time). Returns the number of datagrams sent.
	int tick(uint64_t now_ms);
	uint64_t datagrams() const { return sent_; }

private:
	int fd_ = -1;
	uint16_t port_ = 0;
	LanGame game_;
	std::vector<std::string> extra_;
	uint64_t last_ms_ = 0, sent_ = 0;
	bool first_ = true;
};

class LanBrowser {
public:
	~LanBrowser();
	bool open(uint16_t udp_port, std::string& err);
	void close();
	bool is_open() const { return fd_ >= 0; }
	// Reads what arrived; forgets games not heard for `expire_ms`.
	void poll(uint64_t now_ms, uint64_t expire_ms = 4500);
	const std::vector<LanGame>& games() const { return games_; }

private:
	int fd_ = -1;
	std::vector<LanGame> games_;
};

uint64_t lan_now_ms();

}  // namespace ron::net
