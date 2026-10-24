// Multiplayer in ronmap: the room, the command sink and the session runtime (lobby.h).
#import <Foundation/Foundation.h>

#include "lobby.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include "assets/files.hpp"
#include "sim/replay.hpp"
#include "sim/rules.hpp"
#include "sim/rules_xml.hpp"
#include "sim/save.hpp"
#include "../net/demobot.hpp"

namespace mp {

// ---------------------------------------------------------------- the private queue of CommandManager

namespace {

template <typename Tag>
struct Rob {
	static typename Tag::type ptr;
};
template <typename Tag>
typename Tag::type Rob<Tag>::ptr;
template <typename Tag, typename Tag::type P>
struct Init {
	Init() { Rob<Tag>::ptr = P; }
	static Init instance;
};
template <typename Tag, typename Tag::type P>
Init<Tag, P> Init<Tag, P>::instance;
struct LocalTag {
	using type = std::vector<sim::Command> sim::CommandManager::*;
};
template struct Init<LocalTag, &sim::CommandManager::local_>;

double steady_ms() {
	return double(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

void CommandSink::process_turn(sim::Game& g) {
	if (!net) {
		sim::CommandManager::process_turn(g);
		return;
	}
	std::vector<sim::Command>& q = (*this).*Rob<LocalTag>::ptr;
	if (q.empty()) return;
	net::Bytes batch;
	for (const sim::Command& c : q) sim::engine_to_wire(c, batch);
	q.clear();
	if (!batch.empty()) net->submit_wire(batch);
}

void CommandSink::reset() {
	Runtime* keep = net;
	static_cast<sim::CommandManager&>(*this) = sim::CommandManager();
	net = keep;
}

// ---------------------------------------------------------------- the room

Room::~Room() = default;

bool Room::host(uint16_t port, const std::string& name, std::string& err) {
	transport_ = net::TcpTransport::host_open(port, err);
	if (!transport_) return false;
	lobby_ = std::make_unique<net::Lobby>(*transport_);
	host_ = true;
	port_ = port;
	my_name_ = name;
	lobby_->set_name(name);
	lobby_->set_ready(true);
	last_mask_ = transport_->present_mask();
	return true;
}

bool Room::join(const std::string& address, uint16_t port, const std::string& name, int timeout_ms, std::string& err) {
	transport_ = net::TcpTransport::join(address, port, timeout_ms, err);
	if (!transport_) return false;
	lobby_ = std::make_unique<net::Lobby>(*transport_);
	host_ = false;
	port_ = port;
	address_ = address;
	my_name_ = name;
	lobby_->set_name(name);
	last_mask_ = transport_->present_mask();
	return true;
}

void Room::pump() {
	if (!transport_ || in_game_) return;
	lobby_->poll();
	int peer;
	while (transport_->take_timeout(peer)) {
		if (host_) {
			std::string nm = lobby_->name_of(peer);
			if (nm.empty()) nm = "Player " + std::to_string(peer + 1);
			lobby_->peer_left(peer);
			notices_.push_back(nm + " left the room.");
		} else if (peer == 0) {
			host_lost_ = true;
			notices_.push_back("The host closed the room.");
		}
	}
}

std::vector<std::string> Room::lan_addresses() const {
	std::vector<std::string> out;
	ifaddrs* list = nullptr;
	if (getifaddrs(&list) != 0) return out;
	for (ifaddrs* a = list; a; a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
		char buf[INET_ADDRSTRLEN];
		const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
		if (inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf)) out.push_back(buf);
	}
	freeifaddrs(list);
	return out;
}

uint32_t Room::present_mask() const { return transport_ ? transport_->present_mask() : 0; }

int Room::humans() const {
	int n = 0;
	for (uint32_t m = present_mask(); m; m >>= 1) n += m & 1;
	return n;
}

std::string Room::name_of(int slot) const {
	if (!lobby_ || !(present_mask() & (1u << slot))) return std::string();
	std::string n = slot == this->slot() ? my_name_ : lobby_->name_of(slot);
	return n.empty() ? "Player " + std::to_string(slot + 1) : n;
}

void Room::set_name(const std::string& name) {
	my_name_ = name;
	if (lobby_) lobby_->set_name(name);
}

bool Room::ready_of(int slot) const { return lobby_ && lobby_->ready_of(slot); }

void Room::set_ready(bool ready) {
	if (lobby_) lobby_->set_ready(ready);
}

bool Room::guests_ready() const {
	for (int s = 1; s < net::kMaxSlots; ++s)
		if ((present_mask() & (1u << s)) && !ready_of(s)) return false;
	return true;
}

std::string Room::take_notice() {
	if (notices_.empty()) return std::string();
	std::string n = notices_.front();
	notices_.pop_front();
	return n;
}

void Room::say(const std::string& text) {
	if (lobby_ && !text.empty()) lobby_->say(text);
}

const std::deque<net::Lobby::ChatLine>& Room::chat() const {
	static const std::deque<net::Lobby::ChatLine> none;
	return lobby_ ? lobby_->chat_log() : none;
}

void Room::publish(const sim::RandomGameSetup& s) {
	if (host_ && lobby_) net::setup_to_lobby(s, *lobby_);
}

bool Room::read_setup(sim::RandomGameSetup& out) const {
	std::string err;
	return lobby_ && net::lobby_to_setup(*lobby_, out, err);
}

void Room::start(uint32_t seed) {
	if (host_ && lobby_) lobby_->start(seed);
}

bool Room::started() const { return lobby_ && lobby_->started(); }

bool Room::seat_humans(sim::RandomGameSetup& s) const {
	const uint32_t mask = present_mask();
	int top = 0, n = 0;
	for (int i = 0; i < net::kMaxSlots; ++i)
		if (mask & (1u << i)) top = i, ++n;
	s.num_nations = std::clamp(std::max(s.num_nations, top + 1), 2, 8);
	for (int i = 0; i < 8; ++i) s.nation[size_t(i)].human = (mask & (1u << i)) != 0;
	return n >= 2;
}

void spawn_demo_guests(uint16_t port, const std::vector<std::string>& names, const std::vector<std::string>& lines, int ready_mask) {
	for (size_t i = 0; i < names.size(); ++i) {
		std::thread([=]() {
			std::string err;
			std::unique_ptr<net::TcpTransport> t = net::TcpTransport::join("127.0.0.1", port, 5000, err);
			if (!t) return;
			net::Lobby lobby(*t);
			lobby.set_name(names[i]);
			const auto t0 = std::chrono::steady_clock::now();
			bool said = false;
			while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60) && !lobby.started()) {
				lobby.poll();
				int peer;
				while (t->take_timeout(peer)) {}
				if (!said && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(400 + 150 * int(i))) {
					said = true;
					if ((ready_mask >> i) & 1) lobby.set_ready(true);
					if (i < lines.size()) lobby.say(lines[i]);
				}
				t->idle(20);
			}
			if (lobby.started()) {  // what a real guest does next: read the setup the host fixed
				sim::RandomGameSetup got;
				std::string err;
				const bool ok = net::lobby_to_setup(lobby, got, err);
				std::printf("[demo guest %s] START seen: %s, seed %u, %d nations, humans:", names[i].c_str(), ok ? "setup read" : err.c_str(), got.seed, got.num_nations);
				for (int k = 0; k < got.num_nations; ++k) std::printf(" %d", int(got.nation[size_t(k)].human));
				std::printf("\n");
			}
		}).detach();
	}
}

bool parse_spec(const std::string& game_dir, const std::string& spec, sim::RandomGameSetup& out, std::string& err) {
	try {
		const ron::GameFiles files(game_dir);
		sim::Rules rules = sim::load_rules(files);
		sim::start_game(rules, sim::GameSetup());
		out = sim::default_random_setup(2);
		err = sim::parse_random_option(rules, spec, out);
		return err.empty();
	} catch (const std::exception& e) {
		err = e.what();
		return false;
	}
}

std::string default_mp_save_dir() {
	NSString* base = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
	NSString* dir = [[[base stringByAppendingPathComponent:@"OpenRiseOfNations"] stringByAppendingPathComponent:@"Saves"] stringByAppendingPathComponent:@"Multiplayer"];
	[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	return dir.UTF8String;
}

std::string default_record_dir() {
	NSString* base = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
	NSString* dir = [[[base stringByAppendingPathComponent:@"OpenRiseOfNations"] stringByAppendingPathComponent:@"Recorded Games"] stringByAppendingPathComponent:@"Multi"];
	[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	return dir.UTF8String;
}

// ---------------------------------------------------------------- the session

struct Runtime::BotBox {
	std::unique_ptr<ronnet::DemoBot> bot;
};

Runtime::Runtime(std::shared_ptr<Room> room) : room_(std::move(room)) { room_->enter_game(); }

Runtime::~Runtime() { close_recording(); }

double Runtime::now_ms() const { return steady_ms(); }

void Runtime::note(const std::string& text, uint32_t color) {
	Note n;
	n.at_ms = now_ms();
	n.text = text;
	n.color = color;
	notes_.push_back(n);
	if (notes_.size() > 40) notes_.erase(notes_.begin());
	std::printf("[mp] %s\n", text.c_str());
	if (log) log(text);
}

std::string Runtime::seat_name(int slot) const {
	std::string n = room_->name_of(slot);
	if (!n.empty()) return n;
	return "Player " + std::to_string(slot + 1);
}

void Runtime::attach(std::function<sim::Game&()> game, const std::string& scenario, uint32_t seed, const sim::RandomGameSetup& setup, int speed,
                     const net::Bytes* start_save) {
	if (start_save) start_save_ = *start_save;
	game_ = std::move(game);
	scenario_ = scenario;
	seed_ = seed;
	setup_ = setup;
	speed0_ = std::clamp(speed, 0, 4);
	humans_ = 0;
	for (int i = 0; i < setup.num_nations && i < 8; ++i) humans_ += setup.nation[size_t(i)].human;
	room_->lobby().barrier(1);  // SyncPoint: nobody starts before everybody has its game
	barrier_sent_ = true;
}

bool Runtime::ready() {
	if (started_) return true;
	if (!barrier_sent_) return false;
	room_->lobby().poll();
	if (!room_->lobby().barrier_done(1)) return false;
	std::array<int, 8> who{};
	for (int i = 0; i < 8; ++i) who[size_t(i)] = i;
	hooks_ = std::make_unique<net::GameHooks>(game_(), who, net::ChecksumMode::Native, net::ExecPolicy::all_engine(), scenario_, seed_);
	hooks_->before_frame = before_frame;
	hooks_->after_frame = after_frame;
	hooks_->on_restore = [this](const net::Bytes& snap, std::string& err) { return restore ? restore(snap, err) : (err = "no restore", false); };
	net::SessionConfig cfg;
	cfg.local_slot = room_->slot();
	cfg.seed = seed_;
	cfg.speed = speed0_;
	cfg.recover = true;
	cfg.timeout_ms = 20000;
	for (int i = 0; i < setup_.num_nations && i < 8; ++i) cfg.slot_flags[size_t(i)] = net::kPlayerValid | (setup_.nation[size_t(i)].human ? net::kPlayerHuman : 0u);
	session_ = std::make_unique<net::Session>(room_->transport(), *hooks_, cfg);
	last_paused_ = false;
	last_speed_ = speed0_;
	session_->on_pause = [this](int slot, bool paused) { note(seat_name(slot) + (paused ? " paused the game." : " resumed the game."), 0xFFFFE070); };
	session_->on_speed = [this](int slot, int speed) {
		static const char* names[5] = {"Very Slow", "Slow", "Normal", "Fast", "Hyper Fast"};
		note(seat_name(slot) + " set the game speed to " + names[std::clamp(speed, 0, 4)] + ".", 0xFFFFE070);
	};
	session_->on_resign = [this](int slot) {
		note(seat_name(slot) + " resigned.", 0xFFFF9080);
		if (slot == room_->slot()) left_ = true;
	};
	session_->on_drop = [this](int slot, int option, uint32_t) {
		note(seat_name(slot) + (option == net::kDropReplaceWithAi ? " lost the connection; the computer plays on for them." : " lost the connection and left the game."), 0xFFFF9080);
		if (slot == room_->slot()) left_ = true;
	};
	session_->on_desync = [this](const net::DesyncReport& rep, int authority) {
		note("The game is out of sync (" + std::string(rep.first_component >= 0 ? "first differing part " + std::to_string(rep.first_component) : "check sums differ") + ").", 0xFFFF6060);
		note("Recovering from " + seat_name(authority) + "'s game...", 0xFFFFE070);
	};
	session_->on_recover = [this](const net::Bytes& snapshot, int32_t frame, uint32_t epoch) {
		note("Back in sync at frame " + std::to_string(frame) + " (recovery " + std::to_string(epoch) + "); the game goes on.", 0xFF90FF90);
		if (rec_on_ && rec_.is_open()) {
			std::string err;
			rec_.close(&err);
			++rec_segment_;
			rec_path_ = net::record_segment_path(rec_base_, rec_segment_);
			if (!rec_.open(rec_path_, net::record_info_for_segment(rec_first_, snapshot, frame), err)) {
				note("Recording stopped: " + err, 0xFFFF9080);
				rec_on_ = false;
			}
		}
	};
	session_->on_save = [this](const net::Bytes& snapshot, int32_t frame, uint32_t boundary) {
		std::string path = save_path_;
		if (path.empty()) {
			char stamp[64];
			const std::time_t t = std::time(nullptr);
			std::tm tm{};
			localtime_r(&t, &tm);
			std::strftime(stamp, sizeof stamp, "%Y.%m.%d %H'%M'%S", &tm);
			path = default_mp_save_dir() + "/" + stamp + " multiplayer.ronsave";
		}
		FILE* f = std::fopen(path.c_str(), "wb");
		if (f && std::fwrite(snapshot.data(), 1, snapshot.size(), f) == snapshot.size()) {
			last_save_ = path;
			note("Game saved at frame " + std::to_string(frame) + " (turn " + std::to_string(boundary) + ").", 0xFF90FF90);
		} else {
			note("Could not save the game to " + path, 0xFFFF9080);
		}
		if (f) std::fclose(f);
	};
	session_->on_save_missed = [this](uint32_t) { note("This machine was too far ahead to save with the others; try again.", 0xFFFF9080); };
	session_->on_package = [this](int32_t frame, int slot, uint32_t stamp, const net::Bytes& data) {
		if (rec_on_ && rec_.is_open()) rec_.package(frame, slot, stamp, data);
	};
	if (rec_on_) start_recording();
	if (bot_) {
		bot_box_ = std::make_unique<BotBox>();
		bot_box_->bot = std::make_unique<ronnet::DemoBot>([this]() -> const sim::Game& { return game_(); }, room_->slot());
	}
	feed_bot();
	session_->start();
	started_ = true;
	note("The game starts: " + std::to_string(humans_) + " players.", 0xFF90FF90);
	return true;
}

void Runtime::record_to(const std::string& base_path) {
	rec_base_ = base_path;
	rec_on_ = !base_path.empty();
}

void Runtime::start_recording() {
	if (rec_base_.empty()) {
		char stamp[64];
		const std::time_t t = std::time(nullptr);
		std::tm tm{};
		localtime_r(&t, &tm);
		std::strftime(stamp, sizeof stamp, "%Y.%m.%d %H'%M'%S", &tm);
		rec_base_ = default_record_dir() + "/" + stamp + ".rcx";
	}
	std::array<int, 8> who{};
	for (int i = 0; i < 8; ++i) who[size_t(i)] = i;
	rec_first_ = net::record_info_for_random(setup_, seed_, room_->slot(), who, game_(), scenario_);
	if (!start_save_.empty()) rec_first_ = net::record_info_for_segment(rec_first_, start_save_, game_().frame);  // a continued game: its save is embedded
	for (int i = 0; i < setup_.num_nations && i < 8; ++i)
		if (setup_.nation[size_t(i)].human) rec_first_.seats[size_t(i)].name = seat_name(i);
	std::string err;
	rec_segment_ = 1;
	rec_path_ = rec_base_;
	if (!rec_.open(rec_base_, rec_first_, err)) {
		note("Cannot record the game: " + err, 0xFFFF9080);
		rec_on_ = false;
	}
}

void Runtime::close_recording() {
	if (!rec_.is_open()) return;
	std::string err;
	rec_.close(&err);
	if (!err.empty()) std::fprintf(stderr, "recording: %s\n", err.c_str());
}

bool Runtime::submit_wire(const net::Bytes& cmds) {
	if (!session_ || left_) return false;
	if (session_->recovering()) return false;  // the stamps restart: what is ordered now would be lost
	if (!session_->submit(cmds)) {
		note("Too many orders at once; some were not sent.", 0xFFFF9080);
		return false;
	}
	return true;
}

bool Runtime::submit(const sim::Command& c) {
	net::Bytes b;
	if (!sim::engine_to_wire(c, b)) return false;
	return submit_wire(b);
}

void Runtime::request_save() {
	if (!session_ || session_->recovering()) return;
	const uint32_t at = session_->request_save();
	note("Saving the game for everybody at turn " + std::to_string(at) + "...", 0xFFFFE070);
}

void Runtime::toggle_pause() {
	if (!session_) return;
	net::Bytes b;
	sim::wire_pause(b, session_->paused() ? 0 : 1);
	session_->submit(b);
}

void Runtime::speed_step(int delta) {
	if (!session_) return;
	session_->submit(net::Bytes{delta > 0 ? sim::op::kSpeedUp : sim::op::kSpeedDown});
}

void Runtime::resign() {
	if (!session_ || left_) return;
	net::Bytes b;
	sim::wire_resign(b, room_->slot());
	session_->submit(b);
}

void Runtime::feed_bot() {
	if (!bot_box_ || !session_ || left_) return;
	if (bot_epoch_ != session_->epoch()) bot_epoch_ = session_->epoch(), bot_fed_ = 0;
	while (bot_fed_ < session_->local_stamp() + 1) {
		++bot_fed_;
		session_->submit(bot_box_->bot->next(bot_fed_));
	}
}

void Runtime::finish() {
	if (!session_) return;
	room_->lobby().barrier(2);
	const double t0 = now_ms();
	while (!room_->lobby().barrier_done(2) && now_ms() - t0 < 10000) {
		room_->lobby().poll();
		room_->transport().idle(5);
	}
}

std::string Runtime::stall_text() const {
	if (stall_slot_ < 0) return std::string();
	const double waited = now_ms() - stall_since_;
	if (waited < 1500) return std::string();
	return "Waiting for " + seat_name(stall_slot_) + "... " + std::to_string(int(waited / 1000)) + " s";
}

Runtime::Tick Runtime::step(bool& turn) {
	turn = false;
	if (!started_ && !ready()) return Tick::Waiting;
	feed_bot();
	if (diverge_frame_ >= 0 && !diverged_ && game_().frame >= diverge_frame_) {
		diverged_ = true;
		game_().leaders[size_t(room_->slot() & 7)].units_built += 12345;
		note("(test) a divergence was injected into this machine's game.", 0xFFFF9080);
	}
	for (size_t i = 0; i < acts_.size();) {
		if (session_->boundaries() < acts_[i].second) {
			++i;
			continue;
		}
		const std::string what = acts_[i].first;
		acts_.erase(acts_.begin() + std::ptrdiff_t(i));
		if (what == "pause") {
			if (!paused()) toggle_pause();
		} else if (what == "resume") {
			if (paused()) toggle_pause();
		} else if (what == "faster") {
			speed_step(1);
		} else if (what == "slower") {
			speed_step(-1);
		} else if (what == "resign") {
			resign();
		} else if (what == "save") {
			request_save();
		}
	}
	const net::StepResult r = session_->step();
	switch (r.kind) {
	case net::StepKind::Advanced: turn = r.turn; stall_slot_ = -1; return Tick::Advanced;
	case net::StepKind::Stalled:
		if (stall_slot_ != r.slot) {
			stall_slot_ = r.slot;
			stall_since_ = now_ms();
			if (getenv("RON_MP_DEBUG")) std::fprintf(stderr, "[mp] stalled on slot %d at turn %u (frame %d)\n", r.slot, session_->boundaries(), game_().frame);
		}
		return Tick::Stalled;
	case net::StepKind::Recovering: return Tick::Recovering;
	case net::StepKind::Recovered: return Tick::Recovered;
	case net::StepKind::Ended: return Tick::Ended;
	default:
		error_ = r.kind == net::StepKind::Desync ? "The game is out of sync. " + r.text : r.text;
		return Tick::Error;
	}
	return Tick::Error;
}

}  // namespace mp
