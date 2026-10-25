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
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ctw_ui.h"
#include "net/discovery.hpp"
#include "net/game_link.hpp"
#include "net/lobby.hpp"
#include "net/lockstep.hpp"
#include "net/record.hpp"
#include "net/seat_transport.hpp"
#include "net/tcp.hpp"
#include "sim/commands.hpp"
#include "sim/game.hpp"
#include "sim/newgame.hpp"

namespace mp {

struct ScenarioInfo;
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
	int humans() const;                                       // machines in the room (players and observers)
	int players() const;                                      // machines that play
	int observers() const;                                    // machines that only watch
	net::SeatPlan plan() const;                               // who sits where (game slots), as the room is now
	std::string name_of(int slot) const;                      // "" when nobody is there
	bool ready_of(int slot) const;
	void set_ready(bool ready);
	bool guests_ready() const;                                // host: every connected guest said ready
	std::string take_notice();                                // one line about a departure / the host going away, "" when none
	bool host_lost() const { return host_lost_; }
	void set_name(const std::string& name);                   // after joining (the headless guests name themselves by their slot)

	void say(const std::string& text);
	// Chat to a target: kind 0 everybody, 1 the team, 2 one peer (`peer`). Returns false when nobody would hear it.
	bool say_to(int kind, int peer, const std::string& text);
	const std::deque<net::Lobby::ChatLine>& chat() const;

	// ---- seat rules (multiplayer-spec 8.6)
	// Guest: asks the host to change this player's own seat (nation, team, colour, handicap, observer); the answer comes back as a notice.
	// Host: the same call applies an observer toggle at once (the host edits the other fields directly).
	void request(net::SeatField f, int value);
	void set_approval_ask(bool ask);                          // host: guests' nation / team / colour requests wait for the host's decision
	bool approval_ask() const { return ask_; }
	void set_max_observers(int n);                            // host: rules.xml "maxobservers" 0..6
	int max_observers() const;
	void kick(int peer, bool block = false);                  // host: removes a player (block: the same name cannot come back to this room)
	bool kicked() const { return kicked_; }                   // guest: the host removed this machine
	bool blocked_name(const std::string& n) const;
	uint32_t ctw_seq = 0;                                     // Conquer the World battles played in this room (the host counts; the table carries it)
	// Host: the start countdown (the original ticks "n seconds" in the chat); the screen calls tick_countdown every frame while it runs.
	void begin_countdown(int seconds);
	void cancel_countdown();
	bool counting() const { return countdown_end_ > 0; }
	bool tick_countdown();                                    // true when it reached 0 (the host starts the game)
	int countdown_left() const;
	// After a game: the same room, a lobby again (guests keep their connection).
	void leave_game();
	bool in_game() const { return in_game_; }

	// ---- LAN announcements (a host's room appears in the Multiplayer screen's list of games)
	// Host: what the announcement says (the map text and flags: net::kLanScenario ...). Sent once a second by pump() until the game starts.
	void announce(const std::string& map_text, uint8_t flags = 0);
	void stop_announcing();

	// The setup. Host: edit locally, publish after every change. Guest: read what the host published.
	void publish(const sim::RandomGameSetup& s);
	bool read_setup(sim::RandomGameSetup& out) const;
	// Host: fixes the table and sends START with `seed` (the setup's human seats must be the connected slots: see seat_humans).
	bool start(uint32_t seed);   // false when the plan does not fit (no player, observers above the eight slots)
	bool started() const;
	// Marks the seats of `s` that players sit in as human and the others as computer players (the room's plan: players take seats in the order
	// they joined, observers sit above the nations); raises num_nations to cover them. Returns false when the game cannot start: fewer than two
	// machines in the room, no player, or the observers do not fit in the eight slots.
	bool seat_humans(sim::RandomGameSetup& s) const;
	// A scenario game: the host's choice, as the lobby table carries it (seats = the scenario's players, humans first).
	void publish_scenario(const ScenarioInfo& si);
	bool read_scenario(ScenarioInfo& out) const;

	net::TcpTransport& transport() { return *transport_; }
	net::Lobby& lobby() { return *lobby_; }
	// Hands the connection to the game: the Room keeps the pointers, the Runtime uses them. (The room is not pumped any more.)
	void enter_game();

private:
	std::unique_ptr<net::TcpTransport> transport_;
	std::unique_ptr<net::Lobby> lobby_;
	bool host_ = false, in_game_ = false, host_lost_ = false;
	uint16_t port_ = 0;
	std::string address_, my_name_;
	std::deque<std::string> notices_;
	uint32_t last_mask_ = 1;
	bool kicked_ = false, ask_ = false;
	std::set<std::string> blocked_;
	std::set<int> pending_kicks_, joined_;
	double countdown_end_ = 0;
	int countdown_shown_ = 0;
	net::LanAnnouncer announcer_;
	net::LanGame announce_;
};

// A scenario game's seats (docs/multiplayer-spec.md 8.6): the scenario's players, ordered so the human-flagged ones come first; game slot k plays
// the leader `who[k]`.
struct ScenarioInfo {
	std::string rel;      // path relative to the install
	std::string title;
	int seats = 0;
	std::array<int, 8> who{}, tribe{}, color{}, difficulty{}, team{};
	std::array<bool, 8> human{};   // the scenario's own flag
	std::array<std::string, 8> name;
};
bool load_scenario_info(const std::string& game_dir, const std::string& path, ScenarioInfo& out, std::string& err);

// ---------------------------------------------------------------- Conquer the World battles in multiplayer (docs/multiplayer-spec.md 8.6)
//
// The host's campaign plays a battle with the room's players in its seats. The host fixes who sits where (itself on the campaign player's side,
// the guests on the other seats), the BattleDef travels as a blob in the lobby table and the campaign (ConquestGame::save_bytes) as the lobby's
// file; every machine builds the battle with ConquestGame::build_battle from its own copy of the campaign. A battle's Game cannot be rebuilt
// from a save, so there is no out-of-sync repair and no recording in these games. After the battle the host applies the outcome to its campaign
// (Session::battle_finished) and sends the new campaign to the guests, who replace their copies.
std::string encode_battle(const sim::ctw::ConquestGame::BattleDef& b);
bool decode_battle(const std::string& blob, sim::ctw::ConquestGame::BattleDef& out);
// Host: seats the room (the campaign player's seat for the host, the other seats for the guests in order, the rest computer players), publishes the
// battle and the campaign, sends START. False with `err` when the players do not fit the battle's seats.
bool ctw_host_start(Room& room, ctwui::Session& session, std::string& err);
// Every machine, after START: the Session to build the battle from (the host's own; a guest's copy is loaded from the campaign file the host sent)
// with `battle` set from the table. Null with `err` on failure.
std::shared_ptr<ctwui::Session> ctw_battle_session(Room& room, std::shared_ptr<ctwui::Session> host_session, const std::string& game_dir, std::string& err);
// Host, once the battle is decided: the outcome (1 won, 0 lost, 2 retreated) is applied to the campaign and the new campaign sent to the guests.
void ctw_host_result(Room& room, ctwui::Session& session, int outcome);
// Guest: the campaign the host sent after the battle (empty until it is complete). `outcome` is the host's.
bool ctw_guest_result(Room& room, uint32_t& seq, std::vector<uint8_t>& campaign, int& outcome);

// Test hook for the connect screen's snapshot: rooms of other machines announce themselves on the LAN port for `seconds` (from a thread of this process).
void spawn_demo_hosts(int seconds);

// The UDP port of the LAN announcements (RON_LAN_PORT overrides it: tests run several rooms on one machine).
uint16_t lan_port();

// The colour a team colour slot is drawn in (the chat and message text colours of the original's TeamColors, ARGB).
uint32_t seat_text_color(int color);

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
	int slot() const { return slot_; }                     // the game slot (the Session's numbering)
	int humans() const { return humans_; }                 // players with a seat (observers not counted)
	uint32_t epoch() const { return session_ ? session_->epoch() : 0; }
	bool recovering() const { return session_ && session_->recovering(); }
	bool left() const { return left_; }                    // this player resigned / was dropped: the game goes on without it
	// The seat plan START fixed: this machine's game slot (the Session's slot; for a room whose order differs from the seat order it is not the
	// connection number), its leader, whether it only watches.
	const net::SeatPlan& plan() const { return plan_; }
	bool observer() const { return plan_.slot_of[size_t(room_->slot() & 7)] >= plan_.nations; }
	int who() const { return observer() ? who_[0] : who_[size_t(slot_ & 7)]; }   // observers: the leader they watch from (the first seat's)
	int who_of(int slot) const { return who_[size_t(slot & 7)]; }
	void set_who(const std::array<int, 8>& w);
	void set_recover(bool) {}   // out-of-sync repair from a snapshot is always on: a Conquer the World battle loads from its save like any game now (docs/ctw-spec.md 5.3), so the callers that switched it off for battles (main.mm) have no effect
	// Makes the Game a multiplayer game: names, handicaps, the multiplayer flag (net::apply_mp_seats with the room's table). Every machine calls
	// this on its fresh Game before attach() (a game continued from a save already carries them).
	void prepare_game(sim::Game& g);
	// An observer's chat (no package carries it): out of band to everybody. Players use the CHAT verb through the CommandSink.
	void say(const std::string& text);
	// A player's chat line as a CHAT command (the HUD's chat box does the same through the CommandSink): to everybody, or to this player and its allies.
	void chat(const std::string& text, bool team_only);
	// CHAT_SET: this player hears / ignores the broadcast chat of a leader. chat_muted answers for any (listener, speaker) pair as the
	// executed CHAT_SET packages left the table (identical on every machine).
	void set_chat_mute(int speaker_who, bool mute);
	bool chat_muted(int listener_who, int speaker_who) const { return mute_[size_t(listener_who & 7)][size_t(speaker_who & 7)] != 0; }
	std::string who_name(int who) const;                    // the lobby name of the player who plays that leader ("" for a computer)

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
	net::SeatPlan plan_;
	int slot_ = 0;
	std::array<int, 8> who_{{0, 1, 2, 3, 4, 5, 6, 7}};
	std::array<std::array<uint8_t, 8>, 8> mute_{};   // [listener leader][speaker leader] != 0: the listener ignores the speaker's broadcast chat
	std::unique_ptr<net::SeatTransport> seat_tr_;
	std::function<sim::Game&()> game_;
	std::unique_ptr<net::GameHooks> hooks_;
	std::unique_ptr<net::Session> session_;
	sim::RandomGameSetup setup_;
	std::string scenario_, error_;
	uint32_t seed_ = 0;
	int humans_ = 1, speed0_ = 2;
	bool started_ = false, barrier_sent_ = false, left_ = false, bot_ = false, recover_ = true;
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
