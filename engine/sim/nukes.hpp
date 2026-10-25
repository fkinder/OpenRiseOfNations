// Nuclear explosions (docs/nukes-spec.md): Nuke::add_nuke @92ba30, Nuke::do_damage @92bc80 (called first thing in every
// Objects::inc_time and once more by the impact itself), the crater (TerrainOut::terraform_for_nuke @869ca0), the
// missile-defence test and the nuclear branch of Ammo::do_damage @678060, and the nuke embargo (LeaderData::get_nuke_embargo
// @6d52c0), which is what the detonation stamps. The Armageddon counter itself is victory.cpp's armageddon_nuke_detonated;
// Nuke::do_damage adds to it 110 frames after each detonation began.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ron::sim {

class Game;
struct Ammo;
struct Object;

// One explosion in the original's global `nuke_effect` (Nuke, parallel arrays): where (x, y), the radius at the start and at
// the end of the blast (r0 = 100, r1 = splash_area * 192), the frame it began, the shooter (who, o: the missile unit) and the
// objects it has already hurt (been_damaged_before: one hit per object and nuke).
struct NukeBlast {
	int32_t x = 0, y = 0, r0 = 100, r1 = 0, frame = 0, who = -1, o = -1;
	std::vector<int32_t> hit_o, hit_who;
};

// The state added by the nukes (saved in section NUKE, docs/save-format.md). `craters` is TerrainOut +0x4bd4, the tiles
// terraform_for_nuke has already sunk (a tile is lowered once, however many nukes fall on it); `stamp` is LeaderData +0x7b4,
// the frame of the leader's last detonation (0: none), which starts its trade embargo.
struct NukeState {
	std::vector<NukeBlast> blasts;
	std::vector<int32_t> craters;
	std::array<int32_t, 8> stamp{};
};

// GameEvent kinds written here (the tool log; never read by the simulation): 27 a nuclear missile left its silo (who, o =
// the missile, o2 = the silo, x / y = the target), 28 a nuclear explosion began (who, o = the missile, x / y = the centre,
// value = the final radius), 29 a missile was stopped by missile defence (who = the shooter, other = the defending owner,
// o = the missile, x / y = the impact).
namespace nukeevent {
constexpr int32_t kLaunched = 27, kDetonated = 28, kIntercepted = 29;
}

// Nuke::add_nuke(x, y, r0, r1, who, o): a new explosion at the current frame, and the crater.
void nuke_add(Game& g, int32_t x, int32_t y, int32_t r0, int32_t r1, int32_t who, int32_t o);
// Nuke::do_damage: prunes finished explosions, counts Armageddon, hurts what the growing fireball reaches this frame.
void nuke_do_damage(Game& g);
// The nuclear half of Ammo::do_damage: the leader's counters and stamp, Nuke::add_nuke(x, y, 100, splash * 192, who, o), the
// ground everyone sees, the event, and the first Nuke::do_damage. `o` is the missile unit of leader `who` (the shooter of the
// damage); `splash` is its type's splash_area.
void nuke_detonate(Game& g, int32_t who, int32_t o, int32_t x, int32_t y, int32_t splash);
// The missile branch of Ammo::do_damage's splash, for a shooter with OBJMASK_MISSILE: the missile-defence test (the territory
// owner at the impact, or the nearest land owner when it falls on water) and, for the nuclear line, Nuke::add_nuke + the
// first Nuke::do_damage. Returns true when the ammo is done (shielded, or nuclear) and false when the ordinary splash goes on.
bool nuke_missile_impact(Game& g, const Ammo& a, Object& shooter);
// NukeOut::num_current_nukes: explosions still inside their 110 frames.
int32_t nuke_num_current(const Game& g);

// LeaderData::get_my_nuke_embargo @6d5350 / get_nuke_embargo @6d52c0 / is_nuke_embargo @6d52b0: frames left of the trade
// embargo a detonation puts on its leader (and, for the allies, on them); the Space Program wonder lifts it.
int32_t get_my_nuke_embargo(const Game& g, int32_t who);
int32_t get_nuke_embargo(const Game& g, int32_t who);
inline bool is_nuke_embargo(const Game& g, int32_t who) { return get_nuke_embargo(g, who) != 0; }

}  // namespace ron::sim
