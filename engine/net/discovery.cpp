#include "net/discovery.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ron::net {

namespace {

constexpr char kMagic[4] = {'R', 'O', 'N', 'L'};
const char* const kGroup = "239.255.77.78";

void put_str(std::vector<uint8_t>& o, const std::string& s) {
	const size_t n = std::min<size_t>(s.size(), 60);
	o.push_back(uint8_t(n));
	o.insert(o.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
}
bool get_str(const uint8_t* p, size_t n, size_t& at, std::string& out) {
	if (at >= n) return false;
	const size_t l = p[at++];
	if (at + l > n) return false;
	out.assign(reinterpret_cast<const char*>(p) + at, l);
	at += l;
	return true;
}

void nonblocking(int fd) {
	const int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

}  // namespace

uint64_t lan_now_ms() {
	return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::vector<uint8_t> encode_announcement(const LanGame& g) {
	std::vector<uint8_t> o(kMagic, kMagic + 4);
	o.push_back(g.protocol);
	o.push_back(uint8_t(g.port));
	o.push_back(uint8_t(g.port >> 8));
	o.push_back(uint8_t(std::clamp(g.players, 0, 255)));
	o.push_back(uint8_t(std::clamp(g.seats, 0, 255)));
	o.push_back(g.flags);
	put_str(o, g.name);
	put_str(o, g.map);
	return o;
}

bool decode_announcement(const uint8_t* p, size_t n, LanGame& out) {
	if (n < 11 || std::memcmp(p, kMagic, 4) != 0) return false;
	out = LanGame();
	out.protocol = p[4];
	out.port = uint16_t(p[5] | p[6] << 8);
	out.players = p[7];
	out.seats = p[8];
	out.flags = p[9];
	size_t at = 10;
	if (!get_str(p, n, at, out.name) || !get_str(p, n, at, out.map)) return false;
	return out.port != 0;
}

// ---------------------------------------------------------------- announcer

LanAnnouncer::~LanAnnouncer() { close(); }

bool LanAnnouncer::open(uint16_t udp_port, std::string& err) {
	close();
	fd_ = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd_ < 0) {
		err = "no UDP socket";
		return false;
	}
	int one = 1;
	setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	unsigned char ttl = 1, loop = 1;
	setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
	setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
#ifdef SO_NOSIGPIPE
	setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	nonblocking(fd_);
	port_ = udp_port;
	first_ = true;
	return true;
}

void LanAnnouncer::close() {
	if (fd_ >= 0) ::close(fd_);
	fd_ = -1;
}

void LanAnnouncer::add_target(const std::string& ipv4) {
	if (std::find(extra_.begin(), extra_.end(), ipv4) == extra_.end()) extra_.push_back(ipv4);
}

int LanAnnouncer::tick(uint64_t now_ms) {
	if (fd_ < 0) return 0;
	if (!first_ && now_ms - last_ms_ < 1000) return 0;
	first_ = false;
	last_ms_ = now_ms;
	const std::vector<uint8_t> dg = encode_announcement(game_);
	int n = 0;
	auto to = [&](in_addr_t addr_net) {
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(port_);
		a.sin_addr.s_addr = addr_net;
		if (sendto(fd_, dg.data(), dg.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof a) == ssize_t(dg.size())) ++n;
	};
	to(htonl(INADDR_BROADCAST));
	in_addr g{};
	inet_pton(AF_INET, kGroup, &g);
	to(g.s_addr);
	to(htonl(INADDR_LOOPBACK));
	ifaddrs* list = nullptr;
	if (getifaddrs(&list) == 0) {
		for (ifaddrs* a = list; a; a = a->ifa_next) {
			if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_BROADCAST) || !a->ifa_dstaddr) continue;
			to(reinterpret_cast<const sockaddr_in*>(a->ifa_dstaddr)->sin_addr.s_addr);   // the subnet's broadcast address
		}
		freeifaddrs(list);
	}
	for (const std::string& e : extra_) {
		in_addr x{};
		if (inet_pton(AF_INET, e.c_str(), &x) == 1) to(x.s_addr);
	}
	sent_ += uint64_t(n);
	return n;
}

// ---------------------------------------------------------------- browser

LanBrowser::~LanBrowser() { close(); }

bool LanBrowser::open(uint16_t udp_port, std::string& err) {
	close();
	fd_ = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd_ < 0) {
		err = "no UDP socket";
		return false;
	}
	int one = 1;
	setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
	setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(udp_port);
	if (bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
		err = std::string("cannot listen for games on UDP port ") + std::to_string(udp_port) + ": " + std::strerror(errno);
		close();
		return false;
	}
	// the multicast group on every interface that has an address (the default interface is not always the LAN)
	in_addr group{};
	inet_pton(AF_INET, kGroup, &group);
	ip_mreq any{};
	any.imr_multiaddr = group;
	any.imr_interface.s_addr = htonl(INADDR_ANY);
	setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &any, sizeof any);
	ifaddrs* list = nullptr;
	if (getifaddrs(&list) == 0) {
		for (ifaddrs* i = list; i; i = i->ifa_next) {
			if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_MULTICAST)) continue;
			ip_mreq m{};
			m.imr_multiaddr = group;
			m.imr_interface = reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr;
			setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m);   // errors (already joined, no multicast) are harmless
		}
		freeifaddrs(list);
	}
	nonblocking(fd_);
	return true;
}

void LanBrowser::close() {
	if (fd_ >= 0) ::close(fd_);
	fd_ = -1;
	games_.clear();
}

void LanBrowser::poll(uint64_t now_ms, uint64_t expire_ms) {
	if (fd_ >= 0) {
		for (int guard = 0; guard < 64; ++guard) {
			uint8_t buf[512];
			sockaddr_in from{};
			socklen_t fl = sizeof from;
			const ssize_t k = recvfrom(fd_, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
			if (k <= 0) break;
			LanGame g;
			if (!decode_announcement(buf, size_t(k), g)) continue;
			char ip[INET_ADDRSTRLEN] = {};
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
			g.address = ip;
			g.seen_ms = now_ms;
			bool found = false;
			for (LanGame& e : games_)
				if (e.address == g.address && e.port == g.port) {
					e = g;
					found = true;
				}
			if (!found) games_.push_back(g);
		}
	}
	games_.erase(std::remove_if(games_.begin(), games_.end(), [&](const LanGame& g) { return now_ms > g.seen_ms && now_ms - g.seen_ms > expire_ms; }), games_.end());
	// A host on this very machine is heard twice (the loopback copy and the one the subnet broadcast brings, from the interface's address):
	// keep the one a guest on another machine could use.
	games_.erase(std::remove_if(games_.begin(), games_.end(),
	                            [&](const LanGame& g) {
		                            if (g.address != "127.0.0.1") return false;
		                            for (const LanGame& o : games_)
			                            if (&o != &g && o.address != "127.0.0.1" && o.port == g.port && o.name == g.name) return true;
		                            return false;
	                            }),
	             games_.end());
	std::sort(games_.begin(), games_.end(), [](const LanGame& a, const LanGame& b) { return a.name != b.name ? a.name < b.name : a.port < b.port; });
}

}  // namespace ron::net
