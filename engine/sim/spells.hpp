// Spell casting (docs/transport-spec.md section 4, docs/spells-spec.md): the CAST_SPELL order (Unit::add_cast_order,
// Unit::do_cast, both branches), the spell type predicates and costs (SpellTypeData::is_castable / is_valid_target /
// get_range / get_job_time, SpellType::pay_cast_costs / unpay_cast_costs), the SpellType::cast dispatch and the casts
// (spells_cast.cpp: spies, special forces, conversions, city casts; packers.cpp: PACK / UNPACK and the merchant spots).
// The dead crafts (spells-spec 0: PILFER, ASSIMILATE, RALLY, JAM RADAR, ...) are not castable in the shipped data.
#pragma once

#include <cstdint>

#include "sim/objects.hpp"

namespace ron::sim {

class Game;

// Craft indices (TypeIndex 0x275..0x2ab are the spell types, rules-spec crafts 629..683).
namespace spell {
constexpr int32_t kFirst = 0x275, kLast = 0x2ab, kDefault = 0x296, kTransport = 0x28a, kDisband = 0x29a;
}

// TypeData::is_spell_type (0x275 <= t <= 0x2ab) is production.hpp's; is_pack (0x28b 0x28d 0x28f 0x291) and is_unpack
// (0x28c 0x28e 0x290 0x292):
bool spell_is_pack(int32_t t);
bool spell_is_unpack(int32_t t);

// Unit::add_cast_order(ox, whom, x, y, spell, q, explicit) @5e4a60. Returns the stored order.
Order* add_cast_order(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t x, int32_t y, int32_t spell, QueuePos q,
                      bool expl);

// SpellTypeData::is_castable(o, who, p3) @675bc0 (spells-spec 2.1): 3 = castable, 1 / 2 = "not now" (non-zero for
// Group::action_spell), 0 = never.
int32_t spell_is_castable(const Game& g, int32_t spell, int32_t o, int32_t who, int32_t p3);
// SpellTypeData::is_valid_target(who, o, tw, *reason) @6763c0 (spells-spec 2.2): the target (o of leader tw) may be the
// object of `spell` cast by leader `who`. `reason` (may be null) gets the UI code (0 ok, 1..6, 8).
int32_t spell_is_valid_target(const Game& g, int32_t spell, int32_t who, int32_t o, int32_t tw, int32_t* reason);
// SpellTypeData::get_range(who, to, tw) @676a80 (spells-spec 2.3): 1/192 tile units, 0 = no range test.
int32_t spell_get_range(const Game& g, int32_t spell, int32_t who, int32_t to, int32_t tw);
// SpellType::pay_cast_costs(o, who) @676c40: 0 paid, 1 cannot pay. unpay_cast_costs @676ff0 gives it back.
int32_t pay_cast_costs(Game& g, int32_t spell, int32_t o, int32_t who);
void unpay_cast_costs(Game& g, int32_t spell, int32_t o, int32_t who);
// SpellTypeData::get_job_time(o, who) @675800: the spell's JOB_TIME with the French / Antipater / spy / pack / sabotage
// modifiers.
int32_t spell_job_time(const Game& g, int32_t spell, int32_t o, int32_t who);

// Unit::do_cast(order) @5ebfe0, the handler of the current CAST_SPELL order: the untargeted branch (spell flags & 0xe
// == 0: general powers, TRANSPORT, PACK / UNPACK, CIVILIAN ...) and the targeted one (range walk, face, cloak, job time).
int32_t do_cast(Game& g, Unit& u, Order& o);
// SpellType::cast(o, who, p3, p4) @676ce0: is_castable == 3, then the switch on the spell (p3, p4 = the target's o and
// leader, or the area's x and y).
void spell_cast(Game& g, int32_t spell, int32_t o, int32_t who, int32_t p3, int32_t p4);

// ---- spells_cast.cpp: the cast bodies (spells-spec 4, 6)
void cast_bribe(Game& g, int32_t spell, int32_t o, int32_t who, int32_t to, int32_t tw);          // @672d00
void cast_counterintel(Game& g, int32_t o, int32_t who, int32_t to, int32_t tw);                  // @673740
void cast_double_agent(Game& g, int32_t o, int32_t who, int32_t to, int32_t tw);                  // @673a80
void cast_sabotage(Game& g, int32_t spell, int32_t o, int32_t who, int32_t to, int32_t tw);       // @6720e0
void cast_depopulate(Game& g, int32_t spell, int32_t o, int32_t who);                            // @671e50
void cast_paradrop(Game& g, int32_t o, int32_t who, int32_t x, int32_t y);                   // @671f80 (aircraft-spec 9)
void cast_disband(Game& g, int32_t o, int32_t who);                                              // @671ed0
void cast_civilian(Game& g, int32_t o, int32_t who);                                             // @6704a0
void cast_to_arms(Game& g, int32_t o, int32_t who);                                              // @670880
void cast_move_capital(Game& g, int32_t o, int32_t who);                                         // @671760
void cast_immortal(Game& g, int32_t spell, int32_t o, int32_t who);                               // @670740 / @670600 (dead)
// Unit +0x54 made_from is `Unit::rare` (the union with a merchant's good): the type a militia / citizen was made from, 0 = none.
// DISBAND and DEPOPULATE through the queue (Build::finished) and the group verb both end in cast_disband.
// War prices (spells-spec 6.8, Leader::pay_dow / LeaderData::afford_dow) are ai_diplo.cpp's pay_dow / afford_dow.

// ---- packers.cpp: PACK / UNPACK and the merchant spots (spells-spec 5)
bool cast_is_packing(const Unit& u);               // UnitData::is_packing @60aa60: the front order is a pack cast
bool cast_is_unpacking(const Unit& u);             // UnitData::is_unpacking @60a4b0
bool cast_is_packing_or_unpacking(const Unit& u);  // @60a410
bool cast_is_entering_or_exiting(const Unit& u);   // @60a6f0: a SPECIAL_ANIM of type 0 / 1
bool cast_is_busy(const Unit& u);                  // @60a370
bool unit_is_casting_spell(const Unit& u, int32_t spell);  // UnitData::is_casting @60a530
// UnitData::get_packer_stance @610970 (PackerStanceIndex): PACKER_NEVER for a type without a PACKER stance type, else the
// unit's stance byte. AUTO = 0 unpacks after an idle threshold (think_attack, do_guard).
namespace packer {
constexpr int32_t kAuto = 0, kNever = 1;
}
int32_t unit_packer_stance(const Unit& u);
// The mana a unit can still put into a cast (Group::action_spell's inline formula): what is left plus the mana of the cast it
// is running, capped at its maximum. (UnitData::mana_left itself is aircraft.hpp's unit_mana_left.)
int32_t unit_mana_available(const Game& g, const Unit& u);
void cast_pack(Game& g, int32_t o, int32_t who);              // @670be0
void cast_unpack(Game& g, int32_t o, int32_t who);            // @6709c0
bool good_merchant_spot(Game& g, Unit& u, int32_t tx, int32_t ty);  // UnitData::good_merchant_spot @6068a0
// Unit::find_merchant_spot(x, y, r, *tx, *ty) @603ab0: 1 and the tile when a free rare spot lies within the spiral of
// radius index r around (x, y).
int32_t find_merchant_spot(Game& g, Unit& u, int32_t x, int32_t y, int32_t r, int32_t* tx, int32_t* ty);
// Unit::unpack_merchant(r) @6038e0 (econ_trade.cpp's unpack_merchant calls it).
int32_t unpack_merchant_spot(Game& g, Unit& u, int32_t r);

// ---- the AI (ai_unit.cpp think_spellcaster spy path) and the reactive casts (combat_target.cpp)
// ObjectsData::find (units and buildings) / find_unit (units only, + FILTER_COMBAT) with FILTER_SPELL_TARGET, the way
// Unit::think_spellcaster calls them: the nearest object of the SearchIndexBH relation `search` (ai.hpp search::) within
// `radius` that passes spell_target_filter. Returns its o (leader in g.find_who) or -1.
int32_t spell_find_target(const Game& g, int32_t spell, int32_t who, int32_t x, int32_t y, int32_t search, int32_t radius,
                          bool units_only, bool combat_only);
// Search::valid_filter FILTER_SPELL_TARGET (index 0x14, @67e385): the per-object test behind spell_find_target.
bool spell_target_filter(const Game& g, int32_t spell, int32_t who, int32_t o, int32_t ow);
// The reactive casts of Unit::target_opportunity @5fffc0 (spells-spec 4.5) for a unit with no order: returns true when
// a BRIBE (spy) or SABOTAGE (commando) cast order was queued on the attacker.
bool spell_reactive_cast(Game& g, Unit& u, int32_t atk_o, int32_t atk_who);

// LeaderData::has_spell @6e0bc0: the leader may use the craft (a spell with flag 1 needs the tech bit as well as type_avail).
bool leader_has_spell(const Game& g, int32_t who, int32_t spell);
// ScenarioFuncSet::cast_spell @9e2140 (general_forced_march / general_entrench ...): a valid unit of a valid leader, a
// spell type the leader has and is_castable == 3, then Unit::add_cast_order(QUEUE_NEW, not explicit). 1 or -1.
int32_t scenario_cast_spell(Game& g, int32_t who, int32_t unit_o, int32_t ox, int32_t whom, int32_t x, int32_t y, int32_t spell);

struct Group;
// Group::action_spell(spell, ox, whom, x, y) @6fe1a0 (replay cmd 23 SPELL; spells-spec 3.4): the "only one caster" pick
// (flag 0x1000), the TRANSPORT branch, then per member the castable / mana / price / is_valid_target tests and a CAST_SPELL order
// (explicit; flag 0x40 crafts replace the member's running casts) for units, or the immediate cast / a queue item for
// buildings. Not ported: GroupOut::validate_spell beyond its UI messages (❓ 11 (6)), the carrier's BUILDAIR queue
// (aircraft-spec 7), the ignore_orders prelude.
void group_action_spell(Game& g, Group& gr, int32_t spell, int32_t ox, int32_t whom, int32_t x, int32_t y);

}  // namespace ron::sim
