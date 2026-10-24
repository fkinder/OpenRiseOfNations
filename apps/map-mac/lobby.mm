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

void Room::enter_game() {
	in_game_ = true;
	announcer_.close();
	if (transport_) transport_->close_door();   // the set of machines is final
}

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
			const bool removed = pending_kicks_.erase(peer) != 0;
			lobby_->peer_left(peer);
			joined_.erase(peer);
			notices_.push_back(nm + (removed ? " was removed from the room." : " left the room."));
		} else if (peer == 0) {
			host_lost_ = true;
			notices_.push_back("The host closed the room.");
		}
	}
	if (!host_ && lobby_->kicked() && !kicked_) {
		kicked_ = true;
		host_lost_ = true;
		notices_.push_back("The host removed you from the room.");
	}
	// who has come in: said once the name is known
	const uint32_t mask = present_mask();
	for (int s = 0; s < net::kMaxSlots; ++s) {
		if (s == slot()) continue;
		if ((mask & (1u << s)) && !(last_mask_ & (1u << s))) joined_.insert(s);
		if (!(mask & (1u << s))) joined_.erase(s);
	}
	last_mask_ = mask;
	for (auto it = joined_.begin(); it != joined_.end();) {
		const std::string nm = lobby_->name_of(*it);
		if (nm.empty()) {
			++it;
			continue;
		}
		if (host_ && blocked_name(nm)) {  // a name the host blocked
			notices_.push_back(nm + " tried to join but is blocked.");
			pending_kicks_.insert(*it);
			lobby_->kick(*it);
			transport_->disconnect(*it);
		} else {
			notices_.push_back(nm + " joined the room" + (lobby_->observer_of(*it) ? " as an observer." : "."));
		}
		it = joined_.erase(it);
	}
	net::Lobby::Reply rep;
	while (lobby_->take_reply(rep)) {
		if (rep.code == net::kAccepted) continue;
		notices_.push_back(std::string("Your request for ") + net::seat_field_name(rep.field) + " was not granted: " + net::reply_text(rep.code) + ".");
	}
	if (host_ && announcer_.is_open()) {
		announce_.players = humans();
		announce_.seats = std::max(2, int(lobby_->get_int("num_nations", 2)));
		announcer_.set(announce_);
		announcer_.tick(net::lan_now_ms());
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

int Room::players() const { return lobby_ ? lobby_->plan().players : 0; }

int Room::observers() const { return lobby_ ? lobby_->plan().observers : 0; }

net::SeatPlan Room::plan() const { return lobby_ ? lobby_->plan() : net::SeatPlan(); }

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

bool Room::start(uint32_t seed) { return host_ && lobby_ && lobby_->start(seed); }

bool Room::started() const { return lobby_ && lobby_->started(); }

bool Room::seat_humans(sim::RandomGameSetup& s) const {
	if (!lobby_) return false;
	const net::SeatPlan pl = lobby_->plan(s.num_nations);
	s.num_nations = pl.nations;
	for (int i = 0; i < 8; ++i) s.nation[size_t(i)].human = i < pl.nations && pl.peer_of[size_t(i)] >= 0;
	return humans() >= 2 && pl.players >= 1 && !pl.overflow;
}

bool Room::say_to(int kind, int peer, const std::string& text) { return lobby_ && lobby_->say_to(kind, peer, text); }

void Room::request(net::SeatField f, int value) {
	if (lobby_) lobby_->request(f, value);
}

void Room::set_approval_ask(bool ask) {
	ask_ = ask;
	if (lobby_) lobby_->set_approval(ask ? net::Lobby::Approval::Ask : net::Lobby::Approval::Auto);
}

void Room::set_max_observers(int n) {
	if (host_ && lobby_) lobby_->set("max_observers", std::clamp(n, 0, 6));
}

int Room::max_observers() const { return lobby_ ? int(lobby_->get_int("max_observers", 0)) : 0; }

void Room::kick(int peer, bool block) {
	if (!host_ || !lobby_ || peer == slot() || !(present_mask() & (1u << peer))) return;
	if (block) {
		const std::string nm = lobby_->name_of(peer);
		if (!nm.empty()) blocked_.insert(nm);
	}
	pending_kicks_.insert(peer);
	lobby_->kick(peer);
	transport_->disconnect(peer);
}

bool Room::blocked_name(const std::string& n) const { return blocked_.count(n) != 0; }

void Room::begin_countdown(int seconds) {
	if (!host_ || !lobby_) return;
	countdown_end_ = steady_ms() + seconds * 1000.0;
	countdown_shown_ = seconds;
	lobby_->countdown_set(seconds);
}

void Room::cancel_countdown() {
	if (countdown_end_ <= 0) return;
	countdown_end_ = 0;
	countdown_shown_ = 0;
	if (lobby_) lobby_->countdown_set(0);
}

int Room::countdown_left() const { return countdown_end_ > 0 ? std::max(0, int((countdown_end_ - steady_ms() + 999.0) / 1000.0)) : 0; }

bool Room::tick_countdown() {
	if (countdown_end_ <= 0) return false;
	const int left = countdown_left();
	if (left <= 0) {
		countdown_end_ = 0;
		return true;
	}
	if (left != countdown_shown_) {
		countdown_shown_ = left;
		lobby_->countdown_set(left);
	}
	return false;
}

void Room::leave_game() {
	if (!lobby_) return;
	in_game_ = false;
	if (host_ && transport_) transport_->reopen_door();
	countdown_end_ = 0;
	last_mask_ = present_mask();
	joined_.clear();
	lobby_->reset_game();
	if (!host_) lobby_->set_ready(false);
}

uint16_t lan_port() {
	if (const char* e = getenv("RON_LAN_PORT")) {
		const int v = std::atoi(e);
		if (v > 0 && v < 65536) return uint16_t(v);
	}
	return net::kLanDiscoveryPort;
}

void Room::announce(const std::string& map_text, uint8_t flags) {
	if (!host_ || in_game_) return;
	announce_.name = my_name_;
	announce_.map = map_text;
	announce_.port = port_;
	announce_.flags = flags;
	if (!announcer_.is_open()) {
		std::string err;
		if (!announcer_.open(lan_port(), err)) {
			std::fprintf(stderr, "lan announcements: %s\n", err.c_str());
			return;
		}
		if (const char* e = getenv("RON_LAN_TARGET")) announcer_.add_target(e);   // tests: an extra unicast destination
	}
	announce_.players = humans();
	announce_.seats = std::max(2, int(lobby_->get_int("num_nations", 2)));
	announcer_.set(announce_);
}

void Room::stop_announcing() { announcer_.close(); }

void Room::publish_scenario(const ScenarioInfo& si) {
	if (!host_ || !lobby_) return;
	net::Lobby& l = *lobby_;
	l.set("scenario_game", 1);
	l.set("scenario", si.rel);
	l.set("scen_title", si.title);
	l.set("scen_seats", si.seats);
	for (int k = 0; k < 8; ++k) {
		auto key = [&](const char* b) { return std::string(b) + "_" + std::to_string(k); };
		l.set(key("scen_who"), k < si.seats ? si.who[size_t(k)] : -1);
		l.set(key("scen_tribe"), si.tribe[size_t(k)]);
		l.set(key("scen_color"), si.color[size_t(k)]);
		l.set(key("scen_diff"), si.difficulty[size_t(k)]);
		l.set(key("scen_human"), si.human[size_t(k)] ? 1 : 0);
		l.set(key("scen_name"), si.name[size_t(k)]);
	}
}

bool Room::read_scenario(ScenarioInfo& out) const {
	if (!lobby_ || lobby_->get_int("scenario_game") == 0) return false;
	const net::Lobby& l = *lobby_;
	out = ScenarioInfo();
	out.rel = l.get("scenario");
	out.title = l.get("scen_title");
	out.seats = int(l.get_int("scen_seats"));
	if (out.rel.empty() || out.seats < 1 || out.seats > 8) return false;
	for (int k = 0; k < 8; ++k) {
		auto key = [&](const char* b) { return std::string(b) + "_" + std::to_string(k); };
		out.who[size_t(k)] = int(l.get_int(key("scen_who"), k));
		out.tribe[size_t(k)] = int(l.get_int(key("scen_tribe"), -1));
		out.color[size_t(k)] = int(l.get_int(key("scen_color"), k));
		out.difficulty[size_t(k)] = int(l.get_int(key("scen_diff"), 2));
		out.human[size_t(k)] = l.get_int(key("scen_human")) != 0;
		out.name[size_t(k)] = l.get(key("scen_name"));
	}
	return true;
}

bool load_scenario_info(const std::string& game_dir, const std::string& path, ScenarioInfo& out, std::string& err) {
	try {
		const ron::GameFiles files(game_dir);
		std::string full = path;
		if (!files.resolve(path).empty()) full = files.resolve(path);
		const ron::Scenario sc = ron::parse_scenario(ron::read_file(full));
		std::vector<const ron::ScenarioPlayer*> ps;
		for (const ron::ScenarioPlayer& p : sc.players)
			if (p.who >= 0 && p.who < 8) ps.push_back(&p);
		if (ps.size() < 2) {
			err = "the scenario has fewer than two players";
			return false;
		}
		std::stable_sort(ps.begin(), ps.end(), [](const ron::ScenarioPlayer* a, const ron::ScenarioPlayer* b) { return (a->is_human != 0) > (b->is_human != 0); });
		out = ScenarioInfo();
		out.rel = sim::scenario_rel_path(files, full);
		out.title = sc.name;
		out.seats = int(ps.size());
		for (size_t k = 0; k < ps.size(); ++k) {
			out.who[k] = ps[k]->who;
			out.tribe[k] = ps[k]->tribe;
			out.color[k] = ps[k]->color;
			out.difficulty[k] = std::clamp(ps[k]->difficulty, 0, 5);
			out.team[k] = -1;
			out.human[k] = ps[k]->is_human != 0;
			out.name[k] = ps[k]->name;
		}
		return true;
	} catch (const std::exception& e) {
		err = e.what();
		return false;
	}
}

// ---------------------------------------------------------------- Conquer the World

namespace {
struct BlobW {
	std::string b;
	void i32(int32_t v) {
		for (int i = 0; i < 4; ++i) b.push_back(char(uint32_t(v) >> (8 * i)));
	}
	void str(const std::string& s) {
		i32(int32_t(s.size()));
		b += s;
	}
	void ints(const std::vector<int>& v) {
		i32(int32_t(v.size()));
		for (int x : v) i32(x);
	}
};
struct BlobR {
	const std::string& b;
	size_t p = 0;
	bool bad = false;
	explicit BlobR(const std::string& s) : b(s) {}
	int32_t i32() {
		if (p + 4 > b.size()) return bad = true, 0;
		uint32_t v = 0;
		for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(b[p + size_t(i)])) << (8 * i);
		p += 4;
		return int32_t(v);
	}
	std::string str() {
		const int32_t n = i32();
		if (bad || n < 0 || p + size_t(n) > b.size()) return bad = true, std::string();
		std::string s = b.substr(p, size_t(n));
		p += size_t(n);
		return s;
	}
	std::vector<int> ints() {
		const int32_t n = i32();
		std::vector<int> v;
		if (n < 0 || n > 64) return bad = true, v;
		for (int i = 0; i < n && !bad; ++i) v.push_back(i32());
		return v;
	}
};
}  // namespace

std::string encode_battle(const sim::ctw::ConquestGame::BattleDef& d) {
	BlobW w;
	w.i32(d.link.from), w.i32(d.link.to), w.i32(d.link.attacker), w.i32(d.link.defender), w.i32(d.link.sea);
	w.ints(d.link.att_supporters), w.ints(d.link.def_supporters);
	w.i32(d.link.declare_war_cost), w.i32(d.link.needs_war), w.i32(d.link.overrun);
	w.i32(d.style.category), w.i32(d.style.index), w.str(d.style.custom_id), w.str(d.style.info_text), w.str(d.style.goal1), w.str(d.style.goal2);
	w.i32(int32_t(d.seats.size()));
	for (const auto& s : d.seats) w.i32(s.slot), w.i32(s.team), w.i32(s.human), w.i32(s.barbarian), w.str(s.name), w.i32(s.node);
	w.str(sim::random_setup_string(d.setup));
	w.str(d.custom_map), w.str(d.setup_script), w.str(d.runtime_script), w.str(d.bonus_script), w.str(d.objective), w.str(d.description);
	w.str(d.victory_text), w.str(d.defeat_text);
	w.i32(d.time_limit_min), w.i32(int32_t(d.seed)), w.str(d.tileset), w.i32(d.defender_level);
	for (int a : d.start_armies) w.i32(a);
	w.ints(d.side);
	w.i32(d.defender_default), w.i32(d.map_style);
	return w.b;
}

bool decode_battle(const std::string& blob, sim::ctw::ConquestGame::BattleDef& d) {
	BlobR r(blob);
	d = sim::ctw::ConquestGame::BattleDef();
	d.link.from = r.i32(), d.link.to = r.i32(), d.link.attacker = r.i32(), d.link.defender = r.i32(), d.link.sea = r.i32() != 0;
	d.link.att_supporters = r.ints(), d.link.def_supporters = r.ints();
	d.link.declare_war_cost = r.i32(), d.link.needs_war = r.i32() != 0, d.link.overrun = r.i32() != 0;
	d.style.category = r.i32(), d.style.index = r.i32(), d.style.custom_id = r.str(), d.style.info_text = r.str(), d.style.goal1 = r.str(), d.style.goal2 = r.str();
	const int n = r.i32();
	if (r.bad || n < 0 || n > 8) return false;
	for (int i = 0; i < n; ++i) {
		sim::ctw::ConquestGame::BattleSeat s;
		s.slot = r.i32(), s.team = r.i32(), s.human = r.i32() != 0, s.barbarian = r.i32() != 0, s.name = r.str(), s.node = r.i32();
		d.seats.push_back(s);
	}
	if (!sim::parse_random_setup(r.str(), d.setup)) return false;
	d.setup.ctw = true;
	d.custom_map = r.str(), d.setup_script = r.str(), d.runtime_script = r.str(), d.bonus_script = r.str(), d.objective = r.str(), d.description = r.str();
	d.victory_text = r.str(), d.defeat_text = r.str();
	d.time_limit_min = r.i32(), d.seed = uint32_t(r.i32()), d.tileset = r.str(), d.defender_level = r.i32();
	for (int& a : d.start_armies) a = r.i32();
	d.side = r.ints();
	d.defender_default = r.i32() != 0, d.map_style = r.i32();
	return !r.bad;
}

bool ctw_host_start(Room& room, ctwui::Session& session, std::string& err) {
	if (!room.is_host()) {
		err = "only the host starts a campaign battle";
		return false;
	}
	sim::ctw::ConquestGame::BattleDef B = session.battle;
	const int n = int(B.seats.size());
	net::Lobby& lob = room.lobby();
	const net::SeatPlan now = lob.plan(n);
	int host_seat = 0;
	for (int i = 0; i < n; ++i)
		if (B.seats[size_t(i)].human) {
			host_seat = i;
			break;
		}
	// the guests take the other seats, the host's opponents first
	std::vector<int> others;
	for (int pass = 0; pass < 2; ++pass)
		for (int i = 0; i < n; ++i) {
			if (i == host_seat) continue;
			const bool opp = B.seats[size_t(i)].team != B.seats[size_t(host_seat)].team;
			if ((pass == 0) == opp) others.push_back(i);
		}
	std::vector<int> guests;
	int host_peer = room.slot();
	if (!lob.observer_of(host_peer))
		for (int p = 0; p < net::kMaxSlots; ++p)
			if (p != host_peer && now.slot_of[size_t(p)] >= 0 && now.slot_of[size_t(p)] < now.nations && !lob.observer_of(p)) guests.push_back(p);
	if (lob.observer_of(host_peer) || guests.size() > others.size()) {
		err = lob.observer_of(host_peer) ? "the host must play the campaign's side" : "this battle has " + std::to_string(n) + " seats, but " + std::to_string(guests.size() + 1) + " players are in the room; some can watch instead";
		return false;
	}
	for (int p = 0; p < net::kMaxSlots; ++p) lob.set("force_" + std::to_string(p), "");
	lob.set("force_" + std::to_string(host_peer), host_seat);
	std::array<bool, 8> human{};
	human[size_t(host_seat)] = true;
	for (size_t k = 0; k < guests.size(); ++k) {
		lob.set("force_" + std::to_string(guests[k]), others[k]);
		human[size_t(others[k])] = true;
	}
	for (int i = 0; i < n; ++i) {
		B.seats[size_t(i)].human = human[size_t(i)];
		B.setup.nation[size_t(i)].human = human[size_t(i)];
	}
	session.battle = B;
	room.publish(B.setup);                        // the table's rules and seats (the guests read them to start)
	lob.set("num_nations", n);
	lob.set("ctw_game", 1);
	lob.set("ctw_seq", int64_t(++room.ctw_seq));
	lob.set("ctw_battle", encode_battle(B));
	lob.send_file(session.game.save_bytes());
	if (!room.start(B.seed)) {
		err = "the battle's seats do not fit the room";
		return false;
	}
	return true;
}

std::shared_ptr<ctwui::Session> ctw_battle_session(Room& room, std::shared_ptr<ctwui::Session> host_session, const std::string& game_dir, std::string& err) {
	sim::ctw::ConquestGame::BattleDef B;
	if (!decode_battle(room.lobby().get("ctw_battle"), B)) {
		err = "the battle the host sent is damaged";
		return nullptr;
	}
	std::shared_ptr<ctwui::Session> s = host_session;
	if (!s) {  // a guest: a copy of the host's campaign
		if (!room.lobby().file_complete()) {
			err = "the host's campaign did not arrive";
			return nullptr;
		}
		const std::string tmp = std::string(NSTemporaryDirectory().UTF8String) + "ron-ctw-" + std::to_string(::getpid()) + ".ctw";
		FILE* f = std::fopen(tmp.c_str(), "wb");
		const net::Bytes& b = room.lobby().file();
		if (!f || std::fwrite(b.data(), 1, b.size(), f) != b.size()) {
			if (f) std::fclose(f);
			err = "cannot write the campaign copy";
			return nullptr;
		}
		std::fclose(f);
		s = ctwui::load_session(game_dir, tmp, &err);
		std::remove(tmp.c_str());
		if (!s) return nullptr;
	}
	s->battle = B;
	return s;
}

void ctw_host_result(Room& room, ctwui::Session& session, int outcome) {
	session.battle_finished(outcome);
	net::Lobby& lob = room.lobby();
	lob.set("ctw_outcome", outcome);
	lob.send_file(session.game.save_bytes());
	lob.set("ctw_done", lob.get_int("ctw_seq"));
}

bool ctw_guest_result(Room& room, uint32_t& seq, std::vector<uint8_t>& campaign, int& outcome) {
	net::Lobby& lob = room.lobby();
	const uint32_t done = uint32_t(lob.get_int("ctw_done", 0));
	if (done == 0 || done == seq || !lob.file_complete()) return false;
	seq = done;
	campaign = lob.file();
	outcome = int(lob.get_int("ctw_outcome", 0));
	return true;
}

uint32_t seat_text_color(int color) {
	// TeamColors::init defaults (TEAM_COLOR_TEXT_*), the same table the game's chat uses (script_view.mm): red, blue, pink, green, yellow, cyan, white, orange
	static const uint8_t kText[9][3] = {{255, 100, 100}, {70, 100, 255}, {188, 0, 250}, {0, 236, 33}, {255, 239, 55}, {0, 255, 255}, {255, 255, 255}, {255, 182, 6}, {255, 255, 255}};
	const int c = std::clamp(color, 0, 8);
	return 0xFF000000u | uint32_t(kText[c][0]) << 16 | uint32_t(kText[c][1]) << 8 | kText[c][2];
}

void spawn_demo_hosts(int seconds) {
	std::thread([seconds]() {
		net::LanAnnouncer a, b, c;
		std::string err;
		if (!a.open(lan_port(), err) || !b.open(lan_port(), err) || !c.open(lan_port(), err)) return;
		net::LanGame g;
		g.name = "Ayla";
		g.map = "Great Lakes, Standard";
		g.port = 7777;
		g.players = 2;
		g.seats = 4;
		a.set(g);
		g.name = "Marcus";
		g.map = "Italy (multiplayer)";
		g.port = 7778;
		g.players = 3;
		g.seats = 8;
		g.flags = net::kLanScenario;
		b.set(g);
		g.name = "Old Bob";
		g.map = "";
		g.port = 7780;
		g.protocol = 0;
		c.set(g);
		const auto t0 = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
			const uint64_t now = net::lan_now_ms();
			a.tick(now), b.tick(now), c.tick(now);
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}).detach();
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
					if (getenv("RON_MP_DEMO_REQ")) {  // the seat choices of a guest: team, colour, handicap, a nation (the host's rules answer each)
						static const int team[3] = {1, 0, 1}, color[3] = {4, 5, 7}, hand[3] = {2, 0, 5}, tribe[3] = {10, 3, 17};
						lobby.request(net::SeatField::Team, team[i % 3]);
						lobby.request(net::SeatField::Color, color[i % 3]);
						lobby.request(net::SeatField::Handicap, hand[i % 3]);
						lobby.request(net::SeatField::Tribe, tribe[i % 3]);
					}
					if (getenv("RON_MP_DEMO_OBS") && i + 1 == names.size()) lobby.request(net::SeatField::Observer, 1);
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

Runtime::Runtime(std::shared_ptr<Room> room) : room_(std::move(room)) {
	plan_ = room_->lobby().start_plan();
	if (plan_.players == 0) plan_ = room_->lobby().plan();
	slot_ = std::max(0, plan_.slot_of[size_t(room_->slot() & 7)]);
	room_->enter_game();
}

void Runtime::set_who(const std::array<int, 8>& w) { who_ = w; }

std::string Runtime::who_name(int who) const {
	for (int s = 0; s < plan_.nations && s < 8; ++s)
		if (who_[size_t(s)] == who && plan_.peer_of[size_t(s)] >= 0) return seat_name(s);
	return std::string();
}

void Runtime::prepare_game(sim::Game& g) {
	if (!g.leaders.empty() && !g.multiplayer) net::apply_mp_seats(g, who_, net::lobby_mp_seats(room_->lobby()));
}

void Runtime::say(const std::string& text) {
	if (text.empty()) return;
	room_->lobby().say_to(0, -1, text);
	note(room_->name_of(room_->slot()) + ": " + text, 0xFFC0C0C0);
}

void Runtime::chat(const std::string& text, bool team_only) {
	if (!session_ || observer() || left_ || text.empty()) return;
	const sim::Game& g = game_();
	const int me = who();
	int32_t bits = -1;
	if (team_only) {
		bits = 1 << me;
		for (int i = 0; i < 8; ++i) {
			if (i == me || !(g.leaders[size_t(i)].flags & sim::leaderflag::kValid)) continue;
			if (g.leaders[size_t(me)].diplos(i) == 2 && g.leaders[size_t(i)].diplos(me) == 2) bits |= 1 << i;
		}
	}
	sim::Command c;
	c.verb = sim::Verb::Chat;
	c.text = std::u16string(text.begin(), text.end());   // plain ASCII in the tests; the HUD converts real UTF-8
	c.chat_bits = bits;
	submit(c);
}

void Runtime::set_chat_mute(int speaker_who, bool mute) {
	if (!session_ || observer() || left_) return;
	std::array<uint8_t, 8> st = mute_[size_t(who() & 7)];
	st[size_t(speaker_who & 7)] = mute ? 1 : 0;
	net::Bytes b;
	sim::wire_chat_set(b, st);
	submit_wire(b);
}

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
	const int peer = slot >= 0 && slot < 8 ? plan_.peer_of[size_t(slot)] : -1;
	if (peer >= 0) {
		const std::string n = room_->lobby().name_of(peer);
		if (!n.empty()) return n;
		return "Player " + std::to_string(slot + 1);
	}
	return slot >= 0 && slot < plan_.nations ? "Computer " + std::to_string(slot + 1) : "Observer";
}

void Runtime::attach(std::function<sim::Game&()> game, const std::string& scenario, uint32_t seed, const sim::RandomGameSetup& setup, int speed,
                     const net::Bytes* start_save) {
	if (start_save) start_save_ = *start_save;
	game_ = std::move(game);
	scenario_ = scenario;
	seed_ = seed;
	setup_ = setup;
	speed0_ = std::clamp(speed, 0, 4);
	humans_ = plan_.players;
	setup_.num_nations = plan_.nations;
	for (int i = 0; i < 8; ++i) setup_.nation[size_t(i)].human = i < plan_.nations && plan_.peer_of[size_t(i)] >= 0;
	room_->lobby().barrier(1);  // SyncPoint: nobody starts before everybody has its game
	barrier_sent_ = true;
}

bool Runtime::ready() {
	if (started_) return true;
	if (!barrier_sent_) return false;
	room_->lobby().poll();
	if (!room_->lobby().barrier_done(1)) return false;
	hooks_ = std::make_unique<net::GameHooks>(game_(), who_, net::ChecksumMode::Native, net::ExecPolicy::all_engine(), scenario_, seed_);
	hooks_->before_frame = before_frame;
	hooks_->after_frame = after_frame;
	hooks_->on_restore = [this](const net::Bytes& snap, std::string& err) { return restore ? restore(snap, err) : (err = "no restore", false); };
	net::SessionConfig cfg;
	cfg.local_slot = slot_;
	cfg.seed = seed_;
	cfg.speed = speed0_;
	cfg.recover = recover_;
	cfg.timeout_ms = 20000;
	for (int i = 0; i < 8; ++i) {
		if (i < plan_.nations) cfg.slot_flags[size_t(i)] = net::kPlayerValid | (plan_.peer_of[size_t(i)] >= 0 ? net::kPlayerHuman : 0u);
		else if (plan_.peer_of[size_t(i)] >= 0) cfg.slot_flags[size_t(i)] = net::kPlayerValid | net::kPlayerObserver;
	}
	seat_tr_ = std::make_unique<net::SeatTransport>(room_->transport(), plan_.peer_of);
	session_ = std::make_unique<net::Session>(*seat_tr_, *hooks_, cfg);
	last_paused_ = false;
	last_speed_ = speed0_;
	session_->on_pause = [this](int slot, bool paused) { note(seat_name(slot) + (paused ? " paused the game." : " resumed the game."), 0xFFFFE070); };
	session_->on_speed = [this](int slot, int speed) {
		static const char* names[5] = {"Very Slow", "Slow", "Normal", "Fast", "Hyper Fast"};
		note(seat_name(slot) + " set the game speed to " + names[std::clamp(speed, 0, 4)] + ".", 0xFFFFE070);
	};
	session_->on_resign = [this](int slot) {
		note(seat_name(slot) + " resigned.", 0xFFFF9080);
		if (slot == slot_) left_ = true;
	};
	session_->on_drop = [this](int slot, int option, uint32_t) {
		note(seat_name(slot) + (option == net::kDropReplaceWithAi ? " lost the connection; the computer plays on for them." : " lost the connection and left the game."), 0xFFFF9080);
		if (slot == slot_) left_ = true;
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
		// CHAT_SET: the sender's chat filter (the engine's Command has no field for it, so the table is kept here, identically on every machine)
		for (size_t at = 0; at < data.size();) {
			const size_t n = sim::replay_command_length(data.data() + at, data.size() - at);
			if (n == 0) break;
			std::array<uint8_t, 8> st{};
			if (sim::parse_chat_set(data.data() + at, n, st)) mute_[size_t(who_of(slot) & 7)] = st;
			at += n;
		}
	};
	if (rec_on_) start_recording();
	if (bot_ && !observer()) {
		bot_box_ = std::make_unique<BotBox>();
		bot_box_->bot = std::make_unique<ronnet::DemoBot>([this]() -> const sim::Game& { return game_(); }, who());
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
	// a game that is not a random map (a scenario) cannot be rebuilt from its header: its start state is embedded as a native save
	if (start_save_.empty() && !sim::is_random_scenario(scenario_) && hooks_) hooks_->snapshot(start_save_);
	const net::MpSeats seats = net::lobby_mp_seats(room_->lobby());
	rec_first_ = net::record_info_for_random(setup_, seed_, slot_, who_, game_(), scenario_);
	if (!start_save_.empty()) rec_first_ = net::record_info_for_segment(rec_first_, start_save_, game_().frame);  // a continued game: its save is embedded
	for (int i = 0; i < setup_.num_nations && i < 8; ++i) {
		if (setup_.nation[size_t(i)].human) rec_first_.seats[size_t(i)].name = seat_name(i);
		rec_first_.seats[size_t(i)].handicap = std::clamp(seats.handicap[size_t(i)], 0, 255);
	}
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
	if (!session_ || left_ || observer()) return false;   // observers send no packages
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
	sim::wire_resign(b, slot_);
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
	if (seat_tr_) seat_tr_->release_lobby_traffic();   // a barrier a faster machine sent while this one was still stepping
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
	if (seat_tr_) {
		net::SeatTransport::OobChat oc;
		while (seat_tr_->take_oob_chat(oc)) note(seat_name(oc.slot) + " (observer): " + oc.text, 0xFFC0C0C0);
	}
	if (diverge_frame_ >= 0 && !diverged_ && game_().frame >= diverge_frame_) {
		diverged_ = true;
		game_().leaders[size_t(who() & 7)].units_built += 12345;
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
		} else if (what.rfind("chat=", 0) == 0) {
			chat(what.substr(5), false);
		} else if (what.rfind("team=", 0) == 0) {
			chat(what.substr(5), true);
		} else if (what.rfind("ignore=", 0) == 0) {
			set_chat_mute(std::atoi(what.c_str() + 7), true);
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
