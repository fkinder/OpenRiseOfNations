// Player commands for local play (docs/replay-spec.md sections 7-9, docs/orders-spec.md sections 1-2): the issuer
// side (CommandManager::issue_*, add_group) and the executor (CommandPackage::process_*), for the verbs the movement,
// combat and production cores support: GROUP, ATTACK, STANCE, FORM, MOVE_TO, HALT, SWARM_AROUND, DISBAND,
// GATHER_POINT, QUEUE_UP, BUILD, BUILDMASK, UNQUEUE, GARRISON, EJECTALL.
//
// Usage per simulation step (frame-spec.md section 2): issue commands any time while frame F is current, then
// CommandManager::process_turn(game) executes them in issue order, then game.do_frame() turns F into F + 1.
#pragma once

#include <cstdint>
#include <vector>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;

// Opcodes (replay-spec.md section 9).
enum class Verb : uint8_t {
	Group = 0, Stance = 2, Form = 3, Attack = 4, SwarmAround = 6, MoveTo = 7, Halt = 12, Disband = 21, GatherPoint = 22,
	QueueUp = 24, Build = 25, BuildMask = 33, Unqueue = 48,
	// economy (docs/economy-spec.md section 11)
	Trade = 17, CityGather = 18, Gather = 19, Garrison = 20, EjectAll = 26, ClearTributes = 39, Accept = 41, Reject = 42, Tribute = 43,
	DemandTribute = 44, Buy = 46, Sell = 47, LeaderOptions = 73
};

// One decoded command (the package payload of one verb).
struct Command {
	Verb verb = Verb::Halt;
	// GROUP: i8 who, u16 objects[num] (num == 0 = repeat the last group received from this seat)
	int32_t who = 0;
	std::vector<uint16_t> objects;
	// MOVE_TO: i32 to_x, to_y, set_angle, angle; i8 orders, queued, form, width, disembark
	int32_t x = 0, y = 0, set_angle = 0;
	uint32_t angle = 0;
	int8_t orders = 1, queued = kQueueNew, form = -1, width = -1, disembark = 0;
	// FORM: i32 form, rotate, queued (form in `form32`); STANCE: i32 option
	int32_t form32 = 0, rotate = 0, stance = 0;
	// ATTACK (17 bytes): i32 ox, i32 whom, i32 ignore, queued (QueuePos in `queued`)
	int32_t ox = -1, whom = -1, ignore = 0;
	// SWARM_AROUND (17): i32 ox, i32 whom, queued, i32 orders (OrderType: 6 BUILD_AT, 13 REPAIR, 7 GATHER)
	int32_t order_kind = 6;
	// BUILD (25): i32 x, y, x2, y2, type, queued (x, y above; x2/y2 unused by the executor)
	int32_t x2 = 0, y2 = 0, type = -1;
	// QUEUE_UP (9): i32 type, i32 num. DISBAND (5): i32 all. BUILDMASK (9): i32 mask, i32 set.
	int32_t num = 1, all = 0, mask = 0, set = 0;
	// GATHER_POINT (17): i32 x, y, action, add_to_end
	int32_t action = 0, add_to_end = 0;
	// UNQUEUE (15): i32 who, o, index, i16 uid (who in `who`, o in `ox`, index in `num`)
	uint16_t uid = 0;
	// GATHER (9): i32 ox, queued. CITY_GATHER (9): i32 t (in `type`), queued. TRADE (21): i32 ox, whom, oxx, whose,
	// queued (oxx / whose in `x2` / `y2`). BUY / SELL (13): i32 who, good, flags. TRIBUTE / DEMAND_TRIBUTE (17): i32
	// who, whom, good, amount. CLEAR_TRIBUTES / ACCEPT / REJECT (9): i32 who, whom. LEADER_OPTIONS (33): i32 who,
	// peasants, peasants_wait, buildings, BitMask<32> flags.
	int32_t good = 0, amount = 0, flags = 0, peasants = 0, peasants_wait = 2, buildings = 0;
	uint32_t option_flags = 0;
	// GARRISON (13): i32 ox, i32 whom, queued (ox / whom / queued above). EJECTALL (17): i32 back_to_work, i32 who,
	// i32 eject_o, i32 eject_who (`who` above; who < 0 = the group's own contents).
	int32_t back_to_work = 0, eject_o = -1, eject_who = -1;
};

// CommandPackage: the executor. `group` (the current group set by GROUP) persists across packages.
struct CommandPackage {
	int32_t group = -1;
	// last_objects_received / last_uids_received / last_num_received per seat (for GROUP with num == 0).
	struct Last {
		std::vector<uint16_t> objects, uids;
	};
	std::vector<Last> last = std::vector<Last>(8);

	// CommandPackage::process (one command from seat `play`; `play_who` = GameInfo.player[play].who).
	void process(Game& g, const Command& c, int32_t play, int32_t play_who);
};

// CommandManager: the local issuer. Commands are queued in the local package and executed by process_turn.
class CommandManager {
public:
	int32_t play = 0;  // the local seat; in single player its leader is `play_who`
	int32_t play_who = 0;

	// add_group @94bb60: keep alive captains (alive, o < unit_mark, o_up < 0); emits num = 0 when the selection
	// equals the last group sent. Returns false (nothing queued) when no object is valid.
	bool issue_group(const Game& g, const std::vector<int32_t>& objs);
	// issue_* = add_group + the verb. Return false when nothing was issued.
	bool issue_move_to(const Game& g, const std::vector<int32_t>& objs, int32_t x, int32_t y, QueuePos q = kQueueNew,
	                   int32_t form = -1, bool set_angle = false, uint32_t angle = 0, int32_t orders = 1,
	                   int32_t width = -1);
	bool issue_halt(const Game& g, const std::vector<int32_t>& objs);
	// ATTACK: the current group attacks objects[whom][ox] (CommandPackage::process_attack -> Group::action_attack
	// with mandatory 1). `ignore` bits skip siege (4), special (2) and spy-class (1) members.
	bool issue_attack(const Game& g, const std::vector<int32_t>& objs, int32_t ox, int32_t whom, QueuePos q = kQueueNew,
	                  int32_t ignore = 0);
	bool issue_form(const Game& g, const std::vector<int32_t>& objs, int32_t form, int32_t rotate = 0,
	                QueuePos q = kQueueNew);
	bool issue_stance(const Game& g, const std::vector<int32_t>& objs, int32_t stance);
	// Production (docs/production-spec.md section 9). BUILD: the group (citizens) places a site of `type` at (x, y),
	// pays for it and goes to build it (Group::action_build).
	bool issue_build(const Game& g, const std::vector<int32_t>& objs, int32_t type, int32_t x, int32_t y,
	                 QueuePos q = kQueueNew);
	// SWARM_AROUND: workers build (order 6 BUILD_AT) or repair (13 REPAIR) objects[whom][ox].
	bool issue_swarm_around(const Game& g, const std::vector<int32_t>& objs, int32_t ox, int32_t whom, int32_t order,
	                        QueuePos q = kQueueNew);
	// QUEUE_UP: the group's buildings train / research `type`, `num` times (Group::action_queue_up).
	bool issue_queue(const Game& g, const std::vector<int32_t>& buildings, int32_t type, int32_t num = 1);
	// GATHER_POINT: the buildings' rally point (x, y) (x < 0: clear); action 0 move, 1 garrison/work, 2 attack;
	// add_to_end appends a waypoint.
	bool issue_gather_point(const Game& g, const std::vector<int32_t>& buildings, int32_t x, int32_t y, int32_t action = 0,
	                        bool add_to_end = false);
	// DISBAND: units die, unfinished sites are cancelled (refund), finished buildings queue a demolition.
	bool issue_disband(const Game& g, const std::vector<int32_t>& objs, bool all = false);
	// BUILDMASK: toggles INFINITE (0x40, repeat the queue) or REPEAT_AIR (0x80).
	bool issue_buildmask(const Game& g, const std::vector<int32_t>& buildings, int32_t mask, bool set = true);
	// UNQUEUE (no group): building `o` of the local leader drops queue item `index` (-1 the last; -5 five; <= -10 all).
	bool issue_unqueue(const Game& g, int32_t o, int32_t index = -1);

	// Economy (docs/economy-spec.md section 11).
	// GATHER: the group's workers go to work at the gather building `ox` (nearest own gather building to it).
	bool issue_gather(const Game& g, const std::vector<int32_t>& objs, int32_t ox, QueuePos q = kQueueNew);
	// CITY_GATHER: the group's cities send new workers to their buildings of type t (t < 0: farm / woodcutter / mine).
	bool issue_city_gather(const Game& g, const std::vector<int32_t>& cities, int32_t t, QueuePos q = kQueueNew);
	// GARRISON: the group's units walk to the building objects[whom][ox] and go inside (Group::action_garrison).
	bool issue_garrison(const Game& g, const std::vector<int32_t>& objs, int32_t ox, int32_t whom, QueuePos q = kQueueNew);
	// EJECTALL: the group's buildings / carriers let their contents out (who < 0); `back_to_work` ejects only citizens
	// and lets them resume their orders. With who >= 0 and eject_o a building, the units of leader `who` come out of it.
	bool issue_eject_all(const Game& g, const std::vector<int32_t>& objs, bool back_to_work = false, int32_t who = -1,
	                     int32_t eject_o = -1, int32_t eject_who = -1);
	// TRADE: caravans run a route from city building ox (whom) to oxx (whose); oxx < 0 picks the best partner.
	bool issue_trade(const Game& g, const std::vector<int32_t>& objs, int32_t ox, int32_t whom, int32_t oxx = -1,
	                 int32_t whose = -1, QueuePos q = kQueueNew);
	// BUY / SELL lots of 100 of `good` (flags 1: x5, 4: x10 (buy), 2: all) for the local leader.
	bool issue_buy(int32_t good, int32_t flags = 0);
	bool issue_sell(int32_t good, int32_t flags = 0);
	// TRIBUTE (amount > 0 given) / DEMAND_TRIBUTE (asked), CLEAR_TRIBUTES, ACCEPT / REJECT with leader whom.
	bool issue_tribute(int32_t whom, int32_t good, int32_t amount, bool demand = false);
	bool issue_clear_tributes(int32_t whom);
	bool issue_respond(int32_t whom, bool accept);
	// LEADER_OPTIONS: the auto-gather delay (peasants 0..5), the caravan delay and the building option.
	bool issue_leader_options(int32_t peasants, int32_t peasants_wait, int32_t buildings, uint32_t flags);

	// CommandManager::process_turn @93ef10 (solo): execute the local package in order and record it.
	void process_turn(Game& g);

	struct Record {
		int32_t frame = 0;
		Command cmd;
	};
	std::vector<Record> recorded;  // everything executed, with the frame it was recorded at

private:
	CommandPackage package_;
	std::vector<Command> local_;
	std::vector<uint16_t> last_sent_;
	std::vector<uint16_t> last_sent_uids_;
	int32_t last_sent_who_ = -1;
};

}  // namespace ron::sim
