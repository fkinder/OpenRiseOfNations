// Read-only view of the simulation for rendering and UI. Nothing here mutates the game, and nothing in the simulation
// depends on it. Positions are world units (768 per gameplay tile, the scenario/renderer frame); angles are 32-bit
// binary angles (0 = facing -y, 0x40000000 = +x; degrees = angle * 360 / 2^32).
#pragma once

#include <cstdint>
#include <vector>

namespace ron::sim {

class Game;

enum class AnimState : uint8_t { Idle, Walk, Turn, Other };

struct GuyView {
	int32_t x = 0, y = 0, z = 0;
	uint32_t angle = 0;
	int32_t anim = 0;  // UnitAnim of the guy (CHAR_*)
	int32_t gpiece = -1;  // graphics piece (AssetAnimSource::piece_name gives its unit_graphics.xml <UNIT> name)
	uint32_t cur_time = 0, end_time = 0;  // animation clock in simulation frames (end_time = the animation's length)
};

struct UnitView {
	int32_t who = 0, o = 0;  // owner leader and object slot (o parity picks the female citizen graphic)
	uint16_t uid = 0;
	int32_t type = 0;   // TypeIndex (rules.types[type].graph_name is the graphic)
	int32_t color = 0;  // team colour slot of the owner
	int32_t x = 0, y = 0, z = 0;
	uint32_t angle = 0;  // facing (guy 0's angle: the drawn heading, which lags the unit angle while turning)
	AnimState anim = AnimState::Idle;
	bool alive = false;
	bool on_map = true;  // false: garrisoned / carried (not drawn)
	bool packed = false;
	int32_t group = -1;
	int32_t order = 0;   // OrderType of the current order (0 = none)
	int32_t dest_x = -1, dest_y = -1;  // final destination of the current move (orders_x / orders_y), -1 when idle
	std::vector<GuyView> guys;  // the drawn figures [0, guy_mark) and crew
	// Fog of war (docs/fog-spec.md section 9.3), per player p = 0..7 (bit p):
	uint8_t seen_by = 0;      // drawn for seat p: UnitData::is_seen(p, ignore_cloak = 1) (the owner always)
	uint8_t detected_by = 0;  // UnitData::is_detected(p): the cloaked look is lifted for p
	bool cloaked = false;     // UnitData::is_cloaked
	// Combat. Hit points of this object (a member of an uber chain dies at hits / uber_size, the last one at the
	// remainder); target = the current attack order's target (-1 none); attacking = it attacked this frame or is
	// still in its attack stance (SubObject ATTACKING); recharging = frames until it may attack again.
	int32_t hit_points = 0, max_hit_points = 0;
	int32_t target_who = -1, target_o = -1;
	bool attacking = false;
	int32_t recharging = 0;
	// Economy (docs/economy-spec.md 4): the gather target of a GATHER action (building o of leader gather_who, -1
	// none), whether the worker has arrived once (counts for income), and what it carries back (presentation of
	// the woodcutter / miner trips: 1 timber, 4 metal, -1 nothing).
	int32_t gather_who = -1, gather_o = -1;
	bool gather_arrived = false;
	int32_t carrying = -1;
	int32_t collecting = -1;  // fishermen / merchants: the rare (good TypeIndex) the unit last collected, -1 none
};

// A corpse (DeathObj): one dead figure playing its death animation, then lying until it expires.
struct CorpseView {
	int32_t who = 0, o = 0, type = 0;  // the dead unit (its slot may already hold a new unit after expiry)
	int32_t x = 0, y = 0, z = 0;
	uint32_t angle = 0;
	int32_t anim = 0;     // UnitAnim: CHAR_DEATH_* (15..20) or a burn animation (11..14 on scorch pieces)
	int32_t gpiece = -1;  // the figure's graphics piece
	int32_t frame = 0;    // frames since death (the death animation plays over its first `anim_frames`)
	int32_t anim_frames = 0;  // length of the death animation in frames (AnimSource::frames)
};

// A projectile in flight (or an arrow stuck in the ground).
struct ProjectileView {
	int32_t ammo = -1;  // ammo piece (index into effects_graphics.xml AMMO; AssetAnimSource::ammo_name)
	int32_t x = 0, y = 0, z = 0;  // current position on its ballistic arc (stuck: the impact point)
	int32_t sx = 0, sy = 0, sz = 0, ex = 0, ey = 0, ez = 0;  // launch and aim points
	uint32_t angle = 0;          // heading
	int32_t time = 0, total = 0;  // frames flown / flight time
	bool stuck = false;
	int32_t who = -1, o = -1;  // shooter
};

struct BuildView {
	int32_t who = 0, o = 0;
	int32_t type = 0;
	int32_t color = 0;
	int32_t x = 0, y = 0, z = 0;  // footprint centre
	bool alive = false, active = false, city = false;
	int32_t hit_points = 0, max_hit_points = 0;
	int32_t target_who = -1, target_o = -1;  // the building's current attack target
	bool attacking = false;
	bool under_attack = false;  // build_masks UNDER_ATTACK (0x20), set by take_damage: the minimap blip blinks
	bool started = false;  // construction begun (an unstarted site is hidden from non-allies)
	// Fog of war (docs/fog-spec.md section 9.3), per player p = 0..7 (bit p):
	uint8_t seen_by = 0;      // drawn for seat p: BuildData::is_seen(p) (seen now or remembered)
	uint8_t now_seen_by = 0;  // currently seen by p: BuildData::is_now_seen(p) (WHITE tint; remembered only: GREY)
	uint8_t ever_seen = 0, ever_seen_completed = 0;  // the raw memory masks (per player, not ally-combined)
	// Production (docs/production-spec.md). Construction: progress = job_counter / construct_time (unfinished sites;
	// the site completes when job_counter reaches construct_time). helpers = builders that worked on it last frame.
	int32_t job_counter = 0, construct_time = 0;
	int32_t city_index = -1;  // the city (cities[who] index) the building belongs to, -1 none
	struct QueueEntry {
		int32_t type = -1;         // TypeIndex: unit, tech, building upgrade or DISBAND
		int32_t job_counter = 0;   // progress in 1/100 frame units (accel per frame)
		int32_t time = 0;          // train_time: the item completes when job_counter reaches it
	};
	std::vector<QueueEntry> queue;  // [0] is in progress
	bool repeat = false;            // INFINITE (repeat the queue)
	struct Rally {
		int32_t x = 0, y = 0;  // world units (an object rally point has action 3: x = o, y = who)
		int32_t action = 0;
	};
	std::vector<Rally> rally;  // gather points, head first (the first is where new units go)
	bool rally_inside = false;  // units stay garrisoned
	// Economy: gather buildings' registered gatherers (num_gatherers(0, 0)), those that count for income
	// (num_gatherers(1, 1)) and the slot limit (gather_max); 0 for other buildings.
	int32_t gatherers = 0, gatherers_working = 0, gatherers_max = 0;
};

// A leader's production state (docs/production-spec.md 11; economy-spec 2.1, 7.1).
struct LeaderView {
	int32_t who = 0;
	bool valid = false, human = false;
	int32_t tribe = 0, age = 0;  // age: the number of age techs gained (0 Ancient .. 7 Information)
	int32_t stockpile[6] = {0, 0, 0, 0, 0, 0};  // food, timber, wealth, knowledge, metal, oil
	int32_t pop = 0, pop_cap = 0, control = 0;   // control = population in use (units' pop cost)
	int32_t cities = 0, city_limit = 0;
	int32_t units_built = 0, buildings_built = 0;
	// Economy (docs/economy-spec.md 2): per good the gather rate (resources per 450 frames x16, as the original's HUD
	// rate before the cap), the capped rate shown as income, the commerce cap (x16), over_cap (0 under, 1 capped, 2
	// at the 999 ceiling), and the market prices in wealth per lot of 100 (buy / sell); can_trade = a market and the
	// buy/sell right (Nubians or the tech).
	int32_t gather_rate[6] = {}, income[6] = {}, cap[6] = {}, over_cap[6] = {}, buy[6] = {}, sell[6] = {};
	bool can_trade = false;
	// IFaceResources::draw_infra (ui-spec 6.2): per good (food, timber, wealth, knowledge, metal, oil) the workers walking
	// the leader's units (citizens whose GATHER order targets a farm / woodcutter / mine / university / oil well or platform,
	// caravans for wealth) and the limit (Leader gather_slots; get_caravan_limit for wealth).
	int32_t infra_workers[6] = {}, infra_limit[6] = {};
	// IFaceMainBase::draw_special_resources: the rare goods (TypeIndex 7..49) owned (bit TypeIndex - 6 of rare /
	// rare_conquest) with the count collected (Leader rares_collected).
	struct Rare {
		int32_t good = 0, count = 0;
	};
	std::vector<Rare> rares;
	int32_t armageddon = 0;  // Game::armageddon: nukes left before Armageddon (the timer's count)
};
LeaderView leader_view(const Game& g, int32_t who);

// Fog of war for one seat (docs/fog-spec.md section 9.1). One byte per fog cell (384 x 384 world units; fog_xs =
// 2 x the world's wcoord width), row-major fy * fog_xs + fx:
//   3 visible (WorldData::is_seen), 2 explored and actually seen (was_really_seen), 1 explored only (was_seen:
//   territory, EXPLORE_MAP, reveal_map >= 2), 0 unexplored.
// The original renderer samples level / 3 bilinearly with texel centres at fog-cell centres, maps it through the
// tileset's FOGTEX (art/fow_fade.tga) and draws it as a black alpha overlay (fog-spec 9.2).
struct FogView {
	int32_t who = 0;
	int32_t fog_xs = 0, fog_ys = 0;
	uint32_t changes = 0;  // World::fog_changes when built: unchanged means the levels are unchanged
	std::vector<uint8_t> level;
	uint8_t at(int32_t fx, int32_t fy) const { return level[size_t(fy) * size_t(fog_xs) + size_t(fx)]; }
};

struct GameView {
	int32_t frame = 0, tick = 0;
	uint32_t rng_seed = 0;
	std::vector<UnitView> units;     // alive units of every leader, by leader then slot
	std::vector<BuildView> builds;   // alive buildings
	std::vector<CorpseView> corpses;
	std::vector<ProjectileView> projectiles;
};

// Snapshot of everything drawable. With `with_guys` false the guy lists stay empty.
GameView make_view(const Game& g, bool with_guys = true);
// One unit (alive or not); false if the slot was never allocated.
bool unit_view(const Game& g, int32_t who, int32_t o, UnitView& out, bool with_guys = true);

// Fog levels for seat `who` (0..7; a player > 7 sees everything: all 3). Reuses out.level's storage.
void make_fog_view(const Game& g, int32_t who, FogView& out);
FogView make_fog_view(const Game& g, int32_t who);
// One fog cell's level (0 off the grid).
uint8_t fog_level(const Game& g, int32_t who, int32_t fx, int32_t fy);
// The fog cell of a world position (floor(x / 384)).
int32_t fog_cell(int32_t coord);
// Per object for seat `who`: units by UnitData::is_seen(who, 1), buildings by BuildData::is_seen / is_now_seen.
bool unit_seen_by(const Game& g, int32_t viewer, int32_t who, int32_t o);
bool build_seen_by(const Game& g, int32_t viewer, int32_t who, int32_t o, bool* now_seen = nullptr);

}  // namespace ron::sim
