// The simulation's object model (docs/frame-spec.md, docs/orders-spec.md, docs/movement-spec.md): PDB enums, orders,
// the path stack, guys, units and buildings. The Game that owns them is in sim/game.hpp.
//
// Conventions: coordinates are plain world units (the original stores x/y/z XORed with 0x63637; a save or checksum
// walk must XOR). Angles are 32-bit binary angles in uint32_t (the original's int; arithmetic is mod 2^32).
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "sim/rules.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- enums (PDB names)

// SubObject.flags
namespace objflag {
constexpr uint8_t kValid = 0x01, kStarted = 0x02, kActive = 0x04, kIdling = 0x08, kNewThink = 0x10, kCity = 0x20,
                  kDetector = 0x40, kAttacking = 0x80;
}

// LeaderFlagIndex (Leader.flags)
namespace leaderflag {
constexpr uint32_t kValid = 0x1, kActive = 0x2, kHuman = 0x4, kCoopSolo = 0x8, kVictory = 0x20, kDefeated = 0x40,
                   kGameOver = 0x80, kCapitalPlundered = 0x400000,
                   kCanTransportCiv = 0x100, kCanTransportMil = 0x200, kCanTransportSct = 0x400,
                   kRevealEnemy = 0x800, kExploreMap = 0x1000, kActivePing = 0x2000,  // fog (docs/fog-spec.md 1.3)
                   kIdlePeasant = 0x80000, kRecompute = 0x2000000;
}
// LeaderFlag2Index bits used here (Leader.flags2)
namespace leaderflag2 {
constexpr uint32_t kNoCityDefeat = 0x1, kUnitAiOff = 0x2, kCombatAiOff = 0x8, kVictoryFlag = 0x100, kDefeatFlag = 0x200;
}

// UnitMask (Unit.unit_masks)
namespace umask {
constexpr uint32_t kDecoy = 0x1, kFacing = 0x2, kInCombat = 0x4, kPathClear = 0x8, kZoc = 0x10, kWaiting = 0x40,
                   kAttrition = 0x80, kAutoExplore = 0x100, kBuilder = 0x400, kUnderCover = 0x800, kBlowCover = 0x1000,
                   kCloakRunning = 0x2000, kAztecDamage = 0x4000, kMarchRunning = 0x8000, kCoverBlown = 0x10000,
                   kCasting = 0x20000, kAiControl = 0x40000, kPacked = 0x80000, kSlowDown = 0x100000,
                   kSevereAttrition = 0x400000, kCanTransport = 0x800000, kAiOff = 0x1000000,
                   kEntrenched = 0x2000000, kMultimove = 0x4000000;
}
// UnitMask2 (Unit.unit_masks2)
namespace umask2 {
constexpr uint32_t kUnderAttack = 0x1, kAttackCivies = 0x4, kNewJam = 0x8, kHoldAnim = 0x10, kKeepTarget = 0x100,
                   kMarineTransport = 0x200, kMarines = 0x800, kAntipaterEntr = 0x1000, kDisableTransport = 0x2000, kMoveInForest = 0x4000,
                   kPassiveCloak = 0x8000, kTheCeo = 0x10000, kMarineEntrench = 0x20000, kAttritionSafe = 0x40000;
}

// OrderIndex
enum OrderType : int8_t {
	kOrderNone = 0, kMoveTo = 1, kAttackTo = 2, kExploreTo = 3, kFleeTo = 4, kPatrol = 5, kBuildAt = 6, kGather = 7,
	kBoardShip = 8, kAwaitBoard = 9, kAttack = 10, kFollow = 11, kGuard = 12, kRepair = 13, kCastSpell = 14,
	kTradeRoute = 15, kStrafe = 16, kAirPatrol = 17, kChangeForm = 18, kGroupMove = 19, kGroupAttack = 20,
	kGroupAttackTo = 21, kGroupPatrol = 22, kAttackGround = 23, kAirAttackGround = 24, kSpecialAnim = 25,
	kGarrison = 26, kThink = 27, kNumUnitOrders = 28
};

// QueuePos
enum QueuePos : int32_t { kQueueFirst = 0, kQueueLast = 1, kQueueNew = 2 };

// UnitAnim (CHAR_*)
enum UnitAnim : int32_t {
	kCharDefault = 0, kCharIdle1 = 1, kCharIdle2 = 2, kCharIdle3 = 3, kCharSlog = 7, kCharWalk = 8, kCharJog = 9,
	kCharAttackWalk = 10, kCharAttack1 = 11, kCharAttack2 = 12, kCharAttack3 = 13, kCharAttackSpecial = 14,
	kCharDeathStab1 = 15, kCharDeathSploded2 = 20, kCharTurnLeft = 21, kCharTurnRight = 22, kCharPack = 23,
	kCharUnpack = 24
};

// FormIndex
enum FormIndex : int32_t {
	kFormLine = 0, kFormRefused = 1, kFormEnvelop = 2, kFormERight = 3, kFormELeft = 4, kFormSparse = 5,
	kFormSquare = 6, kFormWedge = 7, kFormColumn = 8, kFormMob = 9, kFormNext = -1, kFormSame = -2, kFormBack = -3
};

// StanceTypes / CombatStanceIndex
enum StanceType : int32_t { kStanceNone = -1, kStanceCombat = 0, kStanceWorker = 1, kStanceCaster = 2, kStancePacker = 3 };

// UnitOrder.flags
namespace orderflag {
constexpr uint8_t kPathed = 0x01, kExplicit = 0x04, kReturnHome = 0x08, kApproach = 0x10, kDisembark = 0x20,
                  kFlag80 = 0x80;
}

// PathData.flags (docs/movement-spec.md section 3.2)
namespace pathflag {
constexpr uint32_t kFinal = 0x01, kFine = 0x02, kTransport = 0x04, kAround = 0x08, kBuilding = 0x10, kRoad = 0x20;
// This port's addition: the KEEP formation's slot waypoint (refreshed every frame by do_group_move, sim/groups.hpp).
constexpr uint32_t kSlot = 0x80;
}

// ---------------------------------------------------------------- orders

// One UnitOrder of any class. The original has a class per OrderIndex sharing virtual bases; here a single record
// holds the union of the fields (only those of `type`'s class are meaningful).
struct Order {
	OrderType type = kOrderNone;
	uint8_t flags = 0;  // UnitOrder.flags (orderflag::)

	// MoveOrder (MOVE_TO, ATTACK_TO, EXPLORE_TO, FLEE_TO, CHANGE_FORM, GROUP_MOVE, GROUP_ATTACK_TO)
	int32_t x = 0, y = 0;
	uint32_t angle = 0;
	int32_t dest = 0, tolerance = 0, pause = 0, retry = 0, attempts = 0, timer = 0;
	int32_t facing = -1;
	int32_t dest_x = 0, dest_y = 0, last_x = -1, last_y = -1, coll_x = 0, coll_y = 0, orig_x = 0, orig_y = 0;
	int16_t off_x = 0, off_y = 0;

	// TargetOrder (ATTACK, GATHER, BUILD_AT, ...)
	int32_t ox = -1, whom = -1;
	uint16_t uid = 0xffff;
	// GarrisonOrder (+0x14 find): look for another building of the target's city when the target is full.
	int8_t find = 0;
	// CastOrder (CAST_SPELL; transport-spec 4.1): x / y +0x14 / +0x18 above (the cast target), paid +0x1c (the cost was
	// charged), spell +0x20 (the craft index, 0x275..0x2ab; 0x28a TRANSPORT).
	int32_t spell = 0, paid = 0;

	// GroupOrder (GROUP_MOVE, GROUP_ATTACK_TO, GROUP_ATTACK, GROUP_PATROL)
	int32_t oxx = -1, whose = -1, id = -1, form_id = 0;
	uint32_t group_angle = 0;
	int32_t in_group = 0;

	// FormOrder (CHANGE_FORM)
	int32_t newform = 0, delay = 0;

	// AttackOrder
	int32_t def_x = -1, def_y = -1;
	int8_t mandatory = 0, defensive = 0, in_range = 0, ever_in_range = 0, new_ord = 0;

	// GatherOrder (GATHER, 0x34; economy-spec 0): current resource tile tx/ty +0x14/+0x18, build_type +0x1c, wait
	// +0x20, goto_build +0x24, non_flat_gather +0x25, dist_mod +0x26, been_there +0x27.
	int32_t g_tx = -1, g_ty = -1, build_type = -1, wait = 0;
	int8_t goto_build = 0, non_flat = 0, dist_mod = 0, been_there = 0;

	// TradeOrder (TRADE_ROUTE, 0x34; economy-spec 6.1): the B building oxx/whose (GroupOrder fields above),
	// started +0x1c, loaded +0x20, uid2 +0x24.
	int32_t started = 0, loaded = 0;
	uint16_t uid2 = 0xffff;

	// AirOrder (STRAFE 16 at +0x24, AIR_PATROL 17 at +0x40, AIR_ATTACK_GROUND 24 at +0x14; aircraft-spec 3). The home base
	// is `oxx` / `whose` above (a STRAFE / AIR_PATROL / AIR_ATTACK_GROUND has no GroupOrder, so the pair is free), the
	// attack point of AIR_ATTACK_GROUND is `x` / `y`. `returning` is the "fly home" flag (check_fuel, land_plane).
	int32_t cruising_alt = 0, sharp_turn = 0, old = 0, returning = 0;
	// StrafeOrder +0x40 / +0x44: the last known position of the target (-1: none).
	int32_t xx = -1, yy = -1;
	// AirPatrolOrder (PatrolOrder): the x_pos / y_pos arrays (one pair per waypoint) and the current index (+0x3c).
	std::vector<std::array<int32_t, 2>> wp;
	int32_t wp_index = 0;
	// SpecialAnimOrder (SPECIAL_ANIM, 0x2c; orders-spec 3.1): type +8 (0 landing: data3 / data4 = the base's o / who; 1 walk-out
	// of a building: data3 / data4 = its position), started +0xc, frames +0x10, data1 +0x14 (gpiece), data2 +0x18, data3 +0x1c,
	// data4 +0x20; ox / whom +0x24 / +0x28 are `ox` / `whom` above.
	int32_t sa_type = 0, sa_started = 0, sa_frames = 0, sa_data1 = 0, sa_data2 = 0, sa_data3 = 0, sa_data4 = 0;
};

// UnitOrder virtual predicates (docs/orders-spec.md section 3.3).
bool order_is_move(OrderType t);      // MoveOrder and subclasses: 1-4, 18, 19, 21
bool order_is_targeted(OrderType t);  // TargetOrder subclasses
bool order_is_attack(OrderType t);    // 10, 16, 20, 23, 24
bool order_is_group(OrderType t);     // 19, 20, 21, 22
inline bool order_is_pathed(const Order& o) { return order_is_move(o.type) && (o.flags & orderflag::kPathed); }

// ---------------------------------------------------------------- path stack

// PathData {to_x, to_y, tolerance, flags}. Unit.path is a Stack<PathData>: back() is the top (next point).
struct PathData {
	int32_t x = 0, y = 0, tol = 0;
	uint32_t flags = 0;
};

// ---------------------------------------------------------------- guys

// GuyData (0xbc): one figure of a unit. Simulation state (walked): position, angles, destination, animation clock.
// Defaults are Guy::clear @5db590 (a new guy starts at (-0x600, -0x600), off the map, so its first
// CollCheck::move_unit only sets the new footprint).
struct Guy {
	int32_t type = 0x32;  // TypeIndex
	int32_t x = -0x600, y = -0x600, z = 0;
	uint32_t angle = 0, last_angle = 0;
	std::array<int32_t, 4> turret_angles{}, des_turret_angles{};
	int32_t track_dx = 0, track_dy = 0;
	int32_t des_x = 0, des_y = 0;
	uint32_t des_angle = 0;
	int32_t last_x = 0, last_y = 0, last_z = 0;
	uint32_t cur_time = 0, end_time = 0;
	int32_t last_time = -1, last_speed = 0, avg_speed = 0;
	int32_t gpiece = -1;  // render-side graphic piece (not simulated here)
	int16_t o = -1, ox = -1, variation = 0, off_x = 0, off_y = 0;
	int16_t node_flags = 0, des_node_flags = 0;
	uint16_t guy_flags = 0;
	int8_t cur_anim = 0, stopped = 0, hold_attack = 0, whom = -1, queued_attack = 0, who = 0, guy_num = 0;
	// Aircraft banking (GuyData +0x44 bank, +0x48 previous bank, +0x4c pitch, +0x50 previous pitch; aircraft-spec 4):
	// Unit::bank_aircraft / pitch_aircraft keep them in single-precision floats and the heading and speed depend on them.
	float bank = 0.0f, last_bank = 0.0f, pitch = 0.0f, last_pitch = 0.0f;
};

// ---------------------------------------------------------------- objects

// SubObject + Object (ObjectData) fields shared by units and buildings.
struct Object {
	bool is_unit = true;
	uint8_t flags = 0;  // objflag::
	int8_t who = 0;
	int16_t o = -1;
	int32_t x = -1, y = -1, z = -1;  // plain (the original stores them ^ 0x63637)
	int32_t type = -1;               // TypeIndex
	const Type* ptype = nullptr;
	int32_t myhits = 0, damage = 0;
	int16_t inside_down = -1;
	// Per-wcoord object list (WData.down/down_who is the head): `down` = next (older) object, `up` = previous.
	int16_t up = -1, down = -1, down_who = 0;
	int8_t up_who = -1;  // a never-used slot holds 0xff here (Object::add_to_world writes it only when it links behind a head)
	bool linked = false;  // currently in a wcoord list (gaia is never linked)
	uint16_t uid = 0, hold_frames = 0;
	int16_t near_o = -1, near_who = -1, healing = 0;
	int8_t targeted = 0, visible = 0;  // visible (+0x40): bit p = revealed to p by attacking it (fog-spec 1.2)
	int8_t damage_frac = 0;  // ObjectData +0x3b: sixteenths of a hit point (take_damage)
	int8_t inside_down_who = -1;  // ObjectData +0x3e: leader of inside_down (the first carried object)
	int8_t mylos = 0;             // ObjectData +0x3c: LOS in tcoords, cached by update_los (fog-spec 4)
	int8_t infiltrated = 0;       // ObjectData +0x3a: bit p = a spy of p infiltrated this building
	// Aircraft containers (airbase, carrier, missile silo; aircraft-spec 8): Object +0x41 launch_timer (frames since the
	// last launch, saturating at FRAMES_BETWEEN_LAUNCHES) and +0x44 the launch list (the `o` of the based aircraft that
	// have an order and wait to leave, a set in insertion order).
	int8_t launch_timer = 0;
	std::vector<int32_t> launch_list;

	bool alive() const { return (flags & objflag::kValid) != 0; }
};

struct SavedSearch;  // a time-sliced PathFinder search (pathfinder.cpp)
void free_saved_search(SavedSearch* s);
struct SavedSearchDel {
	void operator()(SavedSearch* s) const { free_saved_search(s); }
};

// UnitData (0x158).
struct Unit : Object {
	int32_t collide_frame = 0, damage_frame = 0;
	uint32_t angle = 0;
	int32_t air_alt = 0;
	uint32_t dest_angle = 0, trench_angle = 0;
	int32_t tolerance = 0, queue_time = 0;
	uint32_t unit_masks = 0, unit_masks2 = 0;
	int32_t orders_x = 0, orders_y = 0, los_x = 0, los_y = 0;
	int16_t group = -1, inside_up = -1, supply = -1, hero = -1;
	int16_t collide = 0, collide_o = -1, collide_guy = 0, o_up = -1, o_down = -1, gather_down = -1, good_obj = -1;
	int16_t mana_burn = 0, spell_time = 0, myspeed = 0, myarmor = 0, attrition = 0, num_queued = 0, cavarch_o = -1,
	        damage_o = -1;
	uint16_t cavarch_uid = 0;
	int8_t cavarch_who = -1, damage_who = 0, form = 0, form_mod = 0, full = 0, waiting = 0;
	uint8_t recharging = 0, path_recursion = 0, idle = 0;
	int8_t stance = 0, safe = 0, collide_who = -1, inside_up_who = -1, guy_mark = 0, play = 0;
	// Economy (economy-spec 0): +0x54 rare (union: the good a merchant / fisherman collects, the previous type of a
	// scholar militia; Unit::init writes 0), +0x86 held doober (>= 0 while a woodcutter / miner holds wood or ore; the
	// Doober objects themselves are presentation, TODO) / caravan slot of a caravan (cara). The same +0x86 field holds
	// the record slot of a hero (Leader::heroes[cara], sim/heroes.hpp). A spy / commando keeps its Specials slot
	// (Leader::specials[hero]) in the `hero` member above (+0x86 too in the PDB, "special"), not in `cara`: a unit is
	// never two of these, but the harness digest hashes `cara`.
	int32_t rare = 0;
	int16_t doober = -1, cara = -1;
	// AnimalData +0x150..+0x154 (gaia animals): ox / whom of the farm pen a farm animal belongs to, aid (its index).
	int16_t animal_ox = -1, animal_whom = -1;
	int8_t animal_aid = 0;

	std::vector<PathData> path;  // Stack<PathData>; back() = top
	// Stack<PathData>'s allocation (`size`) and `increment`: Unit::init gives a fresh stack 10 / 10, a push grows it by the increment when
	// size <= length, nothing shrinks it. Not saved natively (a native load starts from the length).
	int32_t path_size = 0;
	uint8_t path_inc = 10;
	// Stack<PathData>::push grows the allocation by `increment` whenever size <= length: it ends at the smallest multiple of the increment that holds
	// the longest the stack has been. Called after every push (path_push).
	void path_grew() {
		while (path_size < int32_t(path.size()) && path_inc > 0) path_size += path_inc;
	}
	std::deque<Order> orders;    // execution order: front() = current order (head_node->prev in the original)
	std::vector<Guy> guys;       // PtrArray<Guy>: squad_size + crew_size

	// Time-sliced search (not walked: saves and checksums never see it).
	std::unique_ptr<SavedSearch, SavedSearchDel> openlist;
	int32_t temp_x = 0, temp_y = 0, avoid_x = -1, avoid_y = -1;
	int32_t tol = 0, offset = 0, start_dist = 0, valid_hit = 0, avoid_land = 0, avoid_sea = 0, endx = 0, endy = 0,
	        traversed = 0;
	int32_t announce_frame = 0;

	Order* front() { return orders.empty() ? nullptr : &orders.front(); }
	const Order* front() const { return orders.empty() ? nullptr : &orders.front(); }
	OrderType order_type() const { return orders.empty() ? kOrderNone : orders.front().type; }  // UnitData::order_type
	bool on_map() const { return inside_up < 0; }  // is_on_map
	PathData* top() { return path.empty() ? nullptr : &path.back(); }
};

// WallData.build_masks (+0x60, s16; PDB unnamed enum BUILDMASK_*; docs/production-spec.md section 3).
namespace bmask {
constexpr uint32_t kPathing = 0x1, kCompGather = 0x2, kMandatory = 0x4, kHoldsAir = 0x8, kAttacking = 0x10,
                   kUnderAttack = 0x20, kInfinite = 0x40, kRepeatAir = 0x80, kMakeRoad = 0x100, kNeutralized = 0x200,
                   kConstructing = 0x400, kInUse = 0x800, kActivated = 0x1000, kOrphan = 0x2000, kEjecting = 0x4000,
                   kJammed = 0x8000;
}

// QueueItem (0x14 bytes, production-spec 6.1.1): progress, type, and the captured price (first three non-zero goods).
struct QueueItem {
	int32_t job_counter = 0;
	int16_t type = 0;
	std::array<int16_t, 3> good{};
	std::array<int16_t, 3> cost{};
};

// BuildQueue (BuildQueueData): a FIXED array of queue_size slots (2, 10 or 20); the used prefix is Build.queued.
// init() zeroes every slot and sets queue[0].type = -1; un_queue shifts down without clearing the vacated slot (its
// stale copy is part of the saved state, production-spec 6.1.1).
struct BuildQueue {
	int32_t queue_size = 0;
	std::vector<QueueItem> queue;
	void init(int32_t n) {  // BuildQueue::init @6307d0
		queue_size = n;
		queue.assign(size_t(n), QueueItem{});
		if (n > 0) {
			queue[0].type = -1;
			queue[0].job_counter = 0;
		}
	}
	void close() {  // BuildQueue::close @6306c0
		queue_size = 0;
		queue.clear();
	}
	// BuildQueue::set_queue @6309f0 (cost6 null: goods/costs untouched).
	void set_queue(int32_t i, int32_t type, const int32_t* cost6, bool keep_jc);
	// BuildQueue::un_queue @630ad0 (n = queued before the decrement).
	void un_queue(int32_t i, int32_t n);
	int32_t get_queue(int32_t i) const { return i >= 0 && i < queue_size ? queue[size_t(i)].type : -1; }
	int32_t get_job_counter(int32_t i) const { return i >= 0 && i < queue_size ? queue[size_t(i)].job_counter : -1; }
};

// GatherPoint (0x10): a rally point (production-spec 6.6). action: 0 move, 1 work/garrison, 2 attack, 3 air/object
// (x, y then hold an object's o, who).
struct GatherPoint {
	int32_t x = 0, y = 0;
	uint8_t action = 0;
};

// BuildData + WallData (docs/production-spec.md sections 3-8; field offsets in the comments).
struct Build : Object {
	uint32_t build_masks = 0;  // +0x60 bmask:: (16 bits in the original)
	// Combat (combat-spec 2.16, 4.13): the target (attack_ox +0x7c, attack_whom +0x81), the volley countdown
	// (recharging +0x7a; an animation clock for air-defence turrets, the Kremlin / Terra Cotta timers, and on
	// unfinished sites the count of frames with construction work).
	int16_t attack_ox = -1, recharging = 0;
	int8_t attack_whom = -1;
	// WallData: job_counter +0x48 (construction progress in 1/100 frame; turret angle once active), job_counter_2 +0x4c
	// (progress not reduced by damage, used by refunds), constr_time +0x50, construct_hits +0x54, frame_started +0x5c,
	// helpers +0x64 (builders served this frame), demolition +0x65.
	int32_t job_counter = 0, job_counter_2 = 0, constr_time = 0, construct_hits = 0, frame_started = -1;
	// WallData::gpiece +0x58 (Wall::update_gpiece @643070): the graphics piece of the building's art, part of the saved state and of the builds
	// checksum: kFirstBuild + style * 129 + age * 1161 + (type - 414) for the style / age the install has art for (build_gpiece in build.cpp).
	int32_t gpiece = 0;
	int32_t helpers = 0, demolition = 0;
	int32_t orig_type = -1;  // +0x6c
	int8_t stance = 0;       // +0x7e
	int8_t founder = 0;      // +0x7f
	int8_t gather_max = 0;   // +0x80
	int16_t queued = 0;      // +0x82 (u8): used queue slots
	uint8_t max_age = 0;     // +0x83
	int16_t gather_down = -1;  // +0x70: head of the gatherer list
	int16_t city = -1;         // +0x72: city index (the city object's own index for a city), -1 none
	int16_t city_down = -1;    // +0x74: next building of the city list
	int16_t wonder = -1;       // +0x76
	int16_t fort = -1;         // +0x78 (union fort / dock / farm / oil_well)
	int16_t infiltrate = 0;    // +0x84
	BuildQueue build_queue;    // +0x88
	// gather_from (+0x98 MiningList): claimed gather tiles (tcoords) and the deposit (mtn +0xb4, cliff +0xb5).
	std::vector<std::array<int32_t, 2>> gather_from;
	int8_t mtn = -1, cliff = -1;
	std::vector<GatherPoint> gather;  // +0xb8 GatherPointList, in iteration order from the head
	// WallData +0x62 / +0x63: per-player memory of the building and of its finished state (Wall::check_ever_seen).
	uint8_t ever_seen = 0, ever_seen_completed = 0;
};

// Good (GoodData, 0x30) and Item (ItemData, 0x2c): resources and ruins. Not in the per-leader object lists: the
// global goods / items arrays, linked into a wcoord list as its terminal (WData.down = -2 / -3, down_who = index).
struct GoodObj {
	int32_t type = -1;           // TypeIndex (goods 0..49; items 543)
	int32_t x = 0, y = 0;        // centre (plain world units)
	uint8_t flags = 0;           // SubObject.flags (objflag::kValid)
	uint8_t ever_seen = 0;       // +0x20: bit p = discovered by p (World::reveal_fog)
	bool alive() const { return (flags & objflag::kValid) != 0; }
};

// ---------------------------------------------------------------- combat objects

// Ammo (AmmoData, 0x6c; combat-spec 4.1): one projectile in flight, or an arrow stuck in the ground (flags == 1).
// The pool Objects.ammo_objs is walked in slot order; a new projectile takes the lowest free slot.
namespace ammobits {
constexpr uint8_t kStuck = 0x01, kFlying = 0x02, kVerify = 0x04, kMissed = 0x08, kNoDamage = 0x10;
}
struct Ammo {
	uint8_t flags = 0;  // ammobits:: (flags & 3: slot in use)
	int8_t rolling = 0;
	int16_t accuracy = 0;
	int32_t gpiece = -1;  // ammo piece: index into the AMMO list (the original's gpiece - first_ammo_piece)
	int32_t sx = 0, sy = 0, sz = 0, ex = 0, ey = 0, ez = 0;
	uint32_t cur_time = 0, total_time = 0;
	uint32_t angle = 0;
	int32_t splash_area = 0, index = 0, graph_index = 0;
	int32_t who = -1, o = -1, num_guys = 0, whom = -1, ox = -1;
	int32_t traj = 0;
	float v1z = 0.0f, dx = 0.0f, bank_dx = 0.0f, bank_dy = 0.0f;
	int32_t start_roll_angle = 0;
	bool in_use() const { return (flags & 3) != 0; }
};

// DeathObj (DeathObjData, 0xa4; combat-spec 6.3): one dead guy's corpse. Objects.deaths holds 100 of them.
struct DeathObj {
	int32_t valid = 0, first_frame = -1, anim = -1;
	int32_t x = -1, y = -1, z = -1, who = -1, o = -1, gpiece = -1, ammo_piece = -1;
	float deg = 0.0f;
	uint32_t angle = 0;
	int32_t cur_frame = -1, skeleton = -1;  // skeleton: >= 0 when the guy's piece has one (lifetime + 627)
	int8_t guy = -1;                         // crew index (guy - squad_size), -1 (0xff) for squad guys
};

// Stack<PathData>::push on Unit::path: push and grow the allocation.
inline void path_push(Unit& u, const PathData& p) {
	u.path.push_back(p);
	u.path_grew();
}

}  // namespace ron::sim
