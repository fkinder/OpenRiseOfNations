// Multiplayer in ronmap (docs/multiplayer-spec.md section 8, docs/ui-port.md section 10.3): the LAN room that the Multiplayer lobby
// screens of the front end drive, and the runtime that plays the lockstep session inside the game window.
//
//   Room     the connection (a TcpTransport star: the host listens on a port, guests connect to host:port), the lobby table the host owns
//            (net::Lobby: the map setup, names, ready flags, chat, the START with the host's seed), the seats. No drawing here.
//   Runtime  the game side: net::Session + net::GameHooks over the MapRenderer's Game. The local player's commands reach the session
//            through CommandSink (they are not executed locally; they run one turn later, on every machine, in slot order), remote
//            players' packages arrive through the transport; pause / speed / resign / drops / desyncs become notes for the screen; an
//            out-of-sync repair loads the authority's snapshot; the whole session is recorded as a `.rcx`.
//
// Nothing in engine/sim changes: CommandSink only reads the queue CommandManager::issue_* filled (it reaches the private vector through
// an explicit template instantiation, the one place the language allows it) and turns it into wire commands instead of executing it.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/game_link.hpp"
#include "net/lobby.hpp"
#include "net/lockstep.hpp"
#include "net/record.hpp"
#include "net/tcp.hpp"
#include "sim/commands.hpp"
#include "sim/game.hpp"
#include "sim/newgame.hpp"

namespace mp {

namespace net = ron::net;
namespace sim = ron::sim;

// ---------------------------------------------------------------- the room

class Room {
public:
	~Room();
	// Host: listens on `port` (lobby mode: guests come and go until the game starts). Guest: connects to address:port (blocks up to
	// `timeout_ms`). False with `err` on failure.
	bool host(uint16_t port, const std::string& name, std::string& err);
	bool join(const std::string& address, uint16_t port, const std::string& name, int timeout_ms, std::string& err);
	// Reads the transport: new guests, departures, table updates, chat. Call every frame while the lobby is up.
	void pump();

	bool is_host() const { return host_; }
	bool open() const { return transport_ != nullptr; }
	int slot() const { return transport_ ? transport_->local_peer() : 0; }
	uint16_t port() const { return port_; }
	const std::string& address() const { return address_; }
	std::vector<std::string> lan_addresses() const;           // the host's IPv4 addresses a guest can type
	uint32_t present_mask() const;                            // bit s: slot s is connected (this peer included)
	int humans() const;
	std::string name_of(int slot) const;                      // "" when nobody is there
	bool ready_of(int slot) const;
	void set_ready(bool ready);
	bool guests_ready() const;                                // host: every connected guest said ready
	std::string take_notice();                                // one line about a departure / the host going away, "" when none
	bool host_lost() const { return host_lost_; }
	void set_name(const std::string& name);                   // after joining (the headless guests name themselves by their slot)

	void say(const std::string& text);
	const std::deque<net::Lobby::ChatLine>& chat() const;

	// The setup. Host: edit locally, publish after every change. Guest: read what the host published.
	void publish(const sim::RandomGameSetup& s);
	bool read_setup(sim::RandomGameSetup& out) const;
	// Host: fixes the table and sends START with `seed` (the setup's human seats must be the connected slots: see seat_humans).
	void start(uint32_t seed);
	bool started() const;
	// Marks the seats of `s` that connected peers sit in as human and the others as computer players; raises num_nations to cover the highest
	// connected slot. Returns false when fewer than two humans are connected (the lobby needs a second player to start).
	bool seat_humans(sim::RandomGameSetup& s) const;

	net::TcpTransport& transport() { return *transport_; }
	net::Lobby& lobby() { return *lobby_; }
	// Hands the connection to the game: the Room keeps the pointers, the Runtime uses them. (The room is not pumped any more.)
	void enter_game() { in_game_ = true; }

private:
	std::unique_ptr<net::TcpTransport> transport_;
	std::unique_ptr<net::Lobby> lobby_;
	bool host_ = false, in_game_ = false, host_lost_ = false;
	uint16_t port_ = 0;
	std::string address_, my_name_;
	std::deque<std::string> notices_;
	uint32_t last_mask_ = 1;
};

// Test hook for the lobby screenshots: guests that join 127.0.0.1:port from threads of this process, say a line each and get ready.
void spawn_demo_guests(uint16_t port, const std::vector<std::string>& names, const std::vector<std::string>& lines, int ready_mask);

// Parses "STYLE,SIZE,SEED,NATIONS[,TRIBES...]" with the install's rules (headless --host).
bool parse_spec(const std::string& game_dir, const std::string& spec, sim::RandomGameSetup& out, std::string& err);

// ---------------------------------------------------------------- commands

// The local player's CommandManager. With a Runtime attached `process_turn` hands what the issue_* calls queued to the session instead of
// executing it; without, it is the plain CommandManager.
class Runtime;
class CommandSink : public sim::CommandManager {
public:
	Runtime* net = nullptr;
	void process_turn(sim::Game& g);
	// A fresh issuer (a loaded game / a recovery: the group memory of the old game means nothing).
	void reset();
};

// ---------------------------------------------------------------- the session

struct Note {
	double at_ms = 0;
	std::string text;
	uint32_t color = 0xFFFFFFFF;
};

class Runtime {
public:
	explicit Runtime(std::shared_ptr<Room> room);
	~Runtime();

	// The Game exists (its owner replaces it on a recovery: `game` always answers the current one). `scenario` / `seed` are what a native
	// save of it records. Joins the rendezvous barrier; `ready()` turns true when every machine has its game.
	// `start_save`: the game continues a saved one (the app loaded it already): the recording then starts with that save embedded.
	void attach(std::function<sim::Game&()> game, const std::string& scenario, uint32_t seed, const sim::RandomGameSetup& setup, int speed,
	            const net::Bytes* start_save = nullptr);
	bool ready();
	bool started() const { return started_; }
	// The application's frame wrapper and its answer to a snapshot (build the Game from the bytes, make it current, return true).
	std::function<void()> before_frame, after_frame;
	std::function<bool(const net::Bytes& snapshot, std::string& err)> restore;
	std::function<void(const std::string&)> log;

	enum class Tick { Waiting, Advanced, Stalled, Recovering, Recovered, Ended, Error };
	// One session step. `turn` is set when a turn boundary ran in it.
	Tick step(bool& turn);
	std::string error() const { return error_; }

	// Local input.
	bool submit(const sim::Command& c);                    // one engine command (a GROUP then its verb arrive as two calls)
	bool submit_wire(const net::Bytes& cmds);
	void toggle_pause();
	void speed_step(int delta);
	void resign();
	// A synchronized save: every machine snapshots its game at the same turn boundary and writes its own copy (`save_path_` when set, else the
	// Saves/Multiplayer folder). The copies are byte-identical; the host's reloads a session (Room::lobby().send_file).
	void request_save();
	void set_save_path(const std::string& p) { save_path_ = p; }
	std::string last_save() const { return last_save_; }
	bool paused() const { return session_ ? session_->paused() : false; }
	int speed() const { return session_ ? session_->speed() : 2; }
	int slot() const { return room_->slot(); }
	int humans() const { return humans_; }
	uint32_t epoch() const { return session_ ? session_->epoch() : 0; }
	bool recovering() const { return session_ && session_->recovering(); }
	bool left() const { return left_; }                    // this player resigned / was dropped: the game goes on without it

	// What the player is told: pause, speed, resign, drop and desync messages with their time, newest last.
	void note(const std::string& text, uint32_t color = 0xFFFFFFFF);
	const std::vector<Note>& notes() const { return notes_; }
	std::string stall_text() const;                        // "Waiting for <name>..." while a package is late, else ""
	std::string seat_name(int slot) const;

	// Recording (default on): a `.rcx` per segment in `dir` (the app support folder), or exactly `path` when given.
	void record_to(const std::string& base_path);
	std::string record_path() const { return rec_path_; }

	// Test hooks (ronmap --mp-bot / --mp-diverge): a demo player, and a divergence injected into this machine's state at a frame.
	void enable_bot(bool on) { bot_ = on; }
	void diverge_at(int32_t frame) { diverge_frame_ = frame; }
	// Test hook (ronmap --mp-act): a pause / resume / faster / slower / resign issued by this machine when the session has run `turn` boundaries.
	void add_act(const std::string& what, uint32_t turn) { acts_.push_back({what, turn}); }

	// Headless end of a game: waits (up to 10 s) until every machine is done, so that nobody closes a connection other machines still read.
	void finish();
	net::Session* session() { return session_.get(); }
	net::GameHooks* hooks() { return hooks_.get(); }
	Room& room() { return *room_; }
	double now_ms() const;

private:
	void feed_bot();
	void start_recording();
	void close_recording();

	std::shared_ptr<Room> room_;
	std::function<sim::Game&()> game_;
	std::unique_ptr<net::GameHooks> hooks_;
	std::unique_ptr<net::Session> session_;
	sim::RandomGameSetup setup_;
	std::string scenario_, error_;
	uint32_t seed_ = 0;
	int humans_ = 1, speed0_ = 2;
	bool started_ = false, barrier_sent_ = false, left_ = false, bot_ = false;
	int32_t diverge_frame_ = -1;
	bool diverged_ = false;
	uint32_t bot_fed_ = 0, bot_epoch_ = 0;
	struct BotBox;
	std::unique_ptr<BotBox> bot_box_;
	std::vector<Note> notes_;
	std::vector<std::pair<std::string, uint32_t>> acts_;
	net::Bytes start_save_;
	std::string save_path_, last_save_;
	bool last_paused_ = false;
	int last_speed_ = 2;
	int stall_slot_ = -1;
	double stall_since_ = 0;
	// recording
	net::Recorder rec_;
	net::RecordInfo rec_first_;
	std::string rec_base_, rec_path_;
	int rec_segment_ = 0;
	bool rec_on_ = true;
};

// The default folder of recordings (~/Library/Application Support/OpenRiseOfNations/Recorded Games/Multi).
std::string default_record_dir();
// The folder of multiplayer saves (~/Library/Application Support/OpenRiseOfNations/Saves/Multiplayer).
std::string default_mp_save_dir();

}  // namespace mp
