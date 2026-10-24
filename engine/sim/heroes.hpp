// Heroes (docs/heroes-spec.md): the per-leader hero records (Heroes / HeroData / Caster), finding a general in range
// (HeroesData::find_hero, ObjectData::has_general), the general powers' bookkeeping (Caster::process_spells,
// Leader::verify_spell_flags), cloaking and cover (Unit::process_cloak), government heroes and plunder.
// The five general casts themselves (SpellType::cast_ambush ...) are in sim/spells.cpp.
#pragma once

#include <cstdint>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;
struct Caster;

// TypeIndex of the heroes the formulas name (heroes-spec 1).
namespace hero {
constexpr int32_t kGeneral = 54, kTheDespot = 0x160, kTheSenator = 0x161, kTheMonarch = 0x162, kThePresident = 0x163,
                  kTheComrade = 0x164, kTheCeo = 0x165, kAlexander = 0x166, kNapoleon = 0x167, kParmenio = 0x168,
                  kAntipater = 0x169, kPtolemy = 0x16a, kDarius = 0x16b, kMemnon = 0x16c, kSpitamenes = 0x16d,
                  kPorus = 0x16f, kWellington = 0x170, kCharles = 0x171, kBlucher = 0x175, kKutusov = 0x176;
}

// UnitData::is_hero @46ce60 (unit_flags2 & 0x20), is_special @46cea0 (0x10), is_gov_hero @46ce70 (unit_flags & 0x4000000).
bool unit_is_hero(const Unit& u);
bool unit_is_special(const Unit& u);
bool unit_is_gov_hero(const Unit& u);

// Heroes::init @73a970: 20 preallocated records per leader (and Specials::init). Called by Game::load.
void heroes_init(Game& g);
// Heroes::init_hero @73a330 / close_hero @73a480: the record slot goes in Unit.cara (the original shares +0x86 between
// caravans, heroes, specials, doobers: a hero is never a caravan).
int32_t heroes_init_hero(Game& g, int32_t who, int32_t o);
void heroes_close_hero(Game& g, int32_t who, int32_t slot);
// Specials::init_special @7401c0 / close_special @740310 (spies and commandos: unit_flags2 & 0x10).
int32_t specials_init_special(Game& g, int32_t who, int32_t o);
void specials_close_special(Game& g, int32_t who, int32_t slot);

// HeroData::get_radius @739e50, in tiles.
int32_t hero_get_radius(const Game& g, const Caster& h);
// HeroesData::find_hero(x, y, who, mask, type, extra) @73a1b0: the first in-use hero of `who` (slot order) that is a
// live on-map unit, of `type` (type < 0: any), with `unit_masks & mask` (mask 0: any), whose radius covers (x, y)
// (vector_dist - extra <= radius * 0xc0). Returns the HERO UNIT's o (Caster.o), not the record slot (the spec says slot;
// UnitData::speed @60aae0 indexes objects[who] with it), or -1.
int32_t find_hero(const Game& g, int32_t x, int32_t y, int32_t who, uint32_t mask, int32_t type, int32_t extra);
// ObjectData::has_general(mask, type) @646b00: the o of the hero giving the bonus, or -1. A unit that is itself an active
// hero of that type (and mask) is its own general; otherwise find_hero around the object (buildings: extra
// (y_size + x_size) * 0x60), after the cheap "owns type" test.
int32_t has_general(const Game& g, const Object& ob, uint32_t mask, int32_t type);
// LeaderData::has_general(x, y, mask, type, extra) @6e1050.
int32_t leader_has_general(const Game& g, int32_t who, int32_t x, int32_t y, uint32_t mask, int32_t type, int32_t extra);

// Caster::process_spells(o, who, force) @739ad0: expires the hero's / special's running powers (all of them when forced).
void caster_process_spells(Game& g, Caster& c, int32_t o, int32_t who, bool force);
// Leader::verify_spell_flags @6ce190: drops kAmbushActive / kMarchActive (leader flags 0x4000 / 0x8000) when no hero has
// the matching unit mask.
void leader_verify_spell_flags(Game& g, int32_t who);
// Appends an ActiveSpell to the hero's record (the push in cast_march / cast_ambush).
void hero_add_spell(Game& g, const Unit& u, int32_t t, int32_t duration);

// UnitData::mana @609a50: the unit's mana maximum (0 for a non-caster).
int32_t unit_mana(const Game& g, const Unit& u);

// Unit::process @610bc0, step 2: decoys' lifetime, the spellcasters' Caster::process_spells and mana regeneration.
// Returns false when the unit closed itself (the rest of Unit::process is skipped).
bool unit_process_casters(Game& g, Unit& u);
// Unit::process_cloak @5e1c20 (every 16 frames).
void unit_process_cloak(Game& g, Unit& u);

// Government heroes (heroes-spec 5): a living gov hero of the leader (find_unit FILTER_GOV_HERO), and the Senate / city
// respawn test shared by Build::activate and City::assimilate: train get_gov_hero() when gov_hero_frame >= 0, none is
// alive and gov_hero_frame <= frame.
bool leader_has_gov_hero_unit(const Game& g, int32_t who);
int32_t find_gov_hero_unit(const Game& g, int32_t who, int32_t x, int32_t y);  // nearest, or -1
void build_try_train_gov_hero(Game& g, Build& b);

// Unit::plunder(o_killer, who_killer, aztec, despot) @605b00 (`u` = the dying unit) and Build::plunder(who_razer) @623660.
void unit_plunder(Game& g, Unit& u, int32_t o_killer, int32_t who_killer, int32_t aztec, int32_t despot);
void build_plunder(Game& g, Build& b, int32_t who_razer);

}  // namespace ron::sim
