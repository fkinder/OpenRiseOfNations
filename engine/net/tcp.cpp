#include "net/tcp.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace ron::net {

namespace {

void set_nonblocking(int fd) {
	const int fl = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void tune(int fd) {
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

uint64_t steady_ms() {
	return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

uint64_t TcpTransport::now_ms() const { return steady_ms(); }

TcpTransport::~TcpTransport() {
	for (Conn& c : conns_)
		if (c.fd >= 0) close(c.fd);
	if (listen_fd_ >= 0) close(listen_fd_);
}

std::unique_ptr<TcpTransport> TcpTransport::host(uint16_t port, int clients, int timeout_ms, std::string& error) {
	std::unique_ptr<TcpTransport> t(new TcpTransport());
	t->me_ = 0;
	t->listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (t->listen_fd_ < 0) {
		error = "socket failed";
		return nullptr;
	}
	int one = 1;
	setsockopt(t->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	if (bind(t->listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(t->listen_fd_, 8) != 0) {
		error = std::string("cannot listen on port ") + std::to_string(port) + ": " + std::strerror(errno);
		return nullptr;
	}
	const uint64_t deadline = steady_ms() + uint64_t(timeout_ms);
	while (int(t->conns_.size()) < clients) {
		const uint64_t now = steady_ms();
		if (now >= deadline) {
			error = "timed out waiting for " + std::to_string(clients) + " client(s); " + std::to_string(t->conns_.size()) + " connected";
			return nullptr;
		}
		pollfd pf{t->listen_fd_, POLLIN, 0};
		if (::poll(&pf, 1, int(std::min<uint64_t>(deadline - now, 200))) <= 0) continue;
		const int fd = accept(t->listen_fd_, nullptr, nullptr);
		if (fd < 0) continue;
		tune(fd);
		set_nonblocking(fd);
		Conn c;
		c.fd = fd;
		c.slot = int(t->conns_.size()) + 1;
		c.alive = true;
		t->conns_.push_back(std::move(c));
	}
	t->total_ = clients + 1;
	t->gone_.assign(size_t(t->total_), false);
	for (Conn& c : t->conns_) {
		const uint8_t w[2] = {uint8_t(c.slot), uint8_t(t->total_)};
		t->write_frame(c, 1, 0, uint8_t(c.slot), w, 2);
	}
	return t;
}

std::unique_ptr<TcpTransport> TcpTransport::host_open(uint16_t port, std::string& error) {
	std::unique_ptr<TcpTransport> t(new TcpTransport());
	t->me_ = 0;
	t->open_ = true;
	t->total_ = kMaxSlots;
	t->gone_.assign(size_t(kMaxSlots), true);
	t->gone_[0] = false;
	t->listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if (t->listen_fd_ < 0) {
		error = "socket failed";
		return nullptr;
	}
	int one = 1;
	setsockopt(t->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	if (bind(t->listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(t->listen_fd_, 8) != 0) {
		error = std::string("cannot listen on port ") + std::to_string(port) + ": " + std::strerror(errno);
		return nullptr;
	}
	set_nonblocking(t->listen_fd_);
	return t;
}

uint32_t TcpTransport::present_mask() const {
	uint32_t m = 1u << me_;
	if (me_ == 0) {
		for (const Conn& c : conns_)
			if (c.alive) m |= 1u << c.slot;
	} else {
		for (int i = 0; i < total_ && i < kMaxSlots; ++i)
			if (!(size_t(i) < gone_.size() && gone_[size_t(i)])) m |= 1u << i;
	}
	return m;
}

void TcpTransport::close_door() {
	open_ = false;
	if (listen_fd_ >= 0) close(listen_fd_);
	listen_fd_ = -1;
}

void TcpTransport::accept_new() {
	if (!open_ || listen_fd_ < 0) return;
	for (;;) {
		const int fd = accept(listen_fd_, nullptr, nullptr);
		if (fd < 0) return;
		int slot = -1;
		for (int s = 1; s < kMaxSlots && slot < 0; ++s) {
			bool used = false;
			for (const Conn& c : conns_) used = used || (c.alive && c.slot == s);
			if (!used) slot = s;
		}
		if (slot < 0) {  // full
			close(fd);
			continue;
		}
		tune(fd);
		set_nonblocking(fd);
		Conn* mine = nullptr;
		for (Conn& c : conns_)
			if (!c.alive && c.slot == slot) mine = &c;
		if (!mine) {
			conns_.emplace_back();
			mine = &conns_.back();
		}
		*mine = Conn();
		mine->fd = fd;
		mine->slot = slot;
		mine->alive = true;
		gone_[size_t(slot)] = false;
		const uint8_t s8 = uint8_t(slot);
		for (Conn& o : conns_)
			if (o.alive && &o != mine) write_frame(o, 3, 0, uint8_t(o.slot), &s8, 1);
		const uint8_t w[3] = {uint8_t(slot), uint8_t(total_), uint8_t(present_mask())};
		write_frame(*mine, 1, 0, uint8_t(slot), w, 3);
	}
}

std::unique_ptr<TcpTransport> TcpTransport::join(const std::string& address, uint16_t port, int timeout_ms, std::string& error) {
	std::unique_ptr<TcpTransport> t(new TcpTransport());
	const uint64_t deadline = steady_ms() + uint64_t(timeout_ms);
	int fd = -1;
	for (;;) {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(port);
		if (inet_pton(AF_INET, address.c_str(), &a.sin_addr) != 1) {
			// a host name ("localhost", "my-mac.local"): the first IPv4 address it resolves to
			addrinfo hints{}, *res = nullptr;
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			if (getaddrinfo(address.c_str(), nullptr, &hints, &res) != 0 || !res) {
				error = "cannot find the host \"" + address + "\"";
				close(fd);
				return nullptr;
			}
			a.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
			freeaddrinfo(res);
		}
		if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) break;
		close(fd);
		fd = -1;
		if (steady_ms() >= deadline) {
			error = "cannot connect to " + address + ":" + std::to_string(port);
			return nullptr;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	tune(fd);
	set_nonblocking(fd);
	Conn c;
	c.fd = fd;
	c.slot = 0;
	c.alive = true;
	t->conns_.push_back(std::move(c));
	t->me_ = -1;
	while (t->me_ < 0) {
		if (steady_ms() >= deadline) {
			error = "no welcome from the host";
			return nullptr;
		}
		t->idle(50);
		t->pump_io();
		if (!t->conns_[0].alive) {
			error = "the host closed the connection";
			return nullptr;
		}
	}
	return t;
}

std::vector<int> TcpTransport::peers() const {
	std::vector<int> v;
	if (me_ == 0) {
		for (const Conn& c : conns_)
			if (c.alive) v.push_back(c.slot);
	} else {
		for (int i = 0; i < total_; ++i)
			if (i != me_ && !(size_t(i) < gone_.size() && gone_[size_t(i)])) v.push_back(i);
	}
	return v;
}

void TcpTransport::write_frame(Conn& c, uint8_t kind, uint8_t src, uint8_t dst, const uint8_t* p, size_t n) {
	if (!c.alive) return;
	const uint32_t len = uint32_t(3 + n);
	for (int i = 0; i < 4; ++i) c.out.push_back(uint8_t(len >> (8 * i)));
	c.out.push_back(kind);
	c.out.push_back(src);
	c.out.push_back(dst);
	c.out.insert(c.out.end(), p, p + n);
	flush_conn(c);
}

void TcpTransport::flush_conn(Conn& c) {
	while (c.alive && !c.out.empty()) {
		const ssize_t k = ::send(c.fd, c.out.data(), c.out.size(), 0);
		if (k > 0) {
			c.out.erase(c.out.begin(), c.out.begin() + k);
			continue;
		}
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
		lost(c);
		return;
	}
}

bool TcpTransport::send(int peer, const Bytes& m) {
	if (peer < 0 || peer >= total_ || peer == me_) return false;
	if (me_ == 0) {
		for (Conn& c : conns_)
			if (c.slot == peer && c.alive) {
				write_frame(c, 0, 0, uint8_t(peer), m.data(), m.size());
				return true;
			}
		return false;
	}
	write_frame(conns_[0], 0, uint8_t(me_), uint8_t(peer), m.data(), m.size());
	return true;
}

void TcpTransport::lost(Conn& c) {
	if (!c.alive) return;
	c.alive = false;
	close(c.fd);
	c.fd = -1;
	if (me_ == 0) {
		timeouts_.push_back(c.slot);
		const uint8_t s = uint8_t(c.slot);
		for (Conn& o : conns_)
			if (o.alive) write_frame(o, 2, 0, uint8_t(o.slot), &s, 1);
	} else {
		// The only link is the host's: everybody is unreachable from here.
		if (gone_.empty()) return;
		for (int i = 0; i < total_; ++i)
			if (i != me_ && !gone_[size_t(i)]) {
				gone_[size_t(i)] = true;
				timeouts_.push_back(i);
			}
	}
}

bool TcpTransport::read_conn(Conn& c) {
	bool any = false;
	uint8_t buf[4096];
	while (c.alive) {
		const ssize_t k = ::recv(c.fd, buf, sizeof buf, 0);
		if (k > 0) {
			c.in.insert(c.in.end(), buf, buf + k);
			any = true;
			continue;
		}
		if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
		lost(c);
		break;
	}
	size_t p = 0;
	while (c.in.size() - p >= 4) {
		uint32_t len;
		std::memcpy(&len, c.in.data() + p, 4);
		if (len < 3 || len > (1u << 17)) {
			lost(c);
			break;
		}
		if (c.in.size() - p < 4 + size_t(len)) break;
		const uint8_t* f = c.in.data() + p + 4;
		handle_frame(c, f[0], f[1], f[2], f + 3, len - 3);
		p += 4 + len;
	}
	if (p) c.in.erase(c.in.begin(), c.in.begin() + std::ptrdiff_t(p));
	return any;
}

void TcpTransport::handle_frame(Conn& from, uint8_t kind, uint8_t src, uint8_t dst, const uint8_t* p, size_t n) {
	if (kind == 1 && me_ < 0 && (n == 2 || n == 3)) {  // welcome
		me_ = p[0];
		total_ = p[1];
		gone_.assign(size_t(total_), false);
		if (n == 3)
			for (int i = 0; i < total_; ++i) gone_[size_t(i)] = i < 8 && !((p[2] >> i) & 1);
		return;
	}
	if (kind == 3 && n == 1) {  // peer joined (lobby)
		if (p[0] < total_ && p[0] < int(gone_.size())) gone_[p[0]] = false;
		return;
	}
	if (kind == 2 && n == 1) {  // peer gone
		const int s = p[0];
		if (s < total_ && !gone_[size_t(s)]) {
			gone_[size_t(s)] = true;
			timeouts_.push_back(s);
		}
		return;
	}
	if (kind != 0) return;
	if (me_ == 0) {
		if (dst == 0) {
			inbox_.emplace_back(from.slot, Bytes(p, p + n));
		} else {
			for (Conn& c : conns_)
				if (c.slot == dst && c.alive) write_frame(c, 0, src, dst, p, n);
		}
	} else if (dst == me_) {
		inbox_.emplace_back(src, Bytes(p, p + n));
	}
}

void TcpTransport::pump_io() {
	accept_new();
	for (size_t i = 0; i < conns_.size(); ++i) {
		Conn& c = conns_[i];
		if (!c.alive) continue;
		flush_conn(c);
		read_conn(c);
	}
}

void TcpTransport::idle(int ms) {
	std::vector<pollfd> fds;
	if (open_ && listen_fd_ >= 0) fds.push_back({listen_fd_, POLLIN, 0});
	for (const Conn& c : conns_)
		if (c.alive) fds.push_back({c.fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
	if (fds.empty()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		return;
	}
	::poll(fds.data(), nfds_t(fds.size()), ms);
}

bool TcpTransport::poll_impl(int& from, Bytes& m) {
	if (inbox_.empty()) pump_io();
	if (inbox_.empty()) return false;
	from = inbox_.front().first;
	m = std::move(inbox_.front().second);
	inbox_.pop_front();
	return true;
}

bool TcpTransport::timeout_impl(int& peer) {
	if (timeouts_.empty()) pump_io();
	if (timeouts_.empty()) return false;
	peer = timeouts_.front();
	timeouts_.pop_front();
	return true;
}

}  // namespace ron::net
