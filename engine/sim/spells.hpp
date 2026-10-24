// Spell casting (docs/transport-spec.md section 4): the CAST_SPELL order (Unit::add_cast_order, Unit::do_cast), the
// spell type predicates and costs (SpellTypeData::is_castable / get_job_time, SpellType::pay_cast_costs /
// unpay_cast_costs) and the SpellType::cast dispatch. Only the untargeted branch of do_cast and the TRANSPORT spell
// (craft 0x28a) are simulated; every other spell is a TODO(spells).
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

// SpellTypeData::is_castable(o, who, p3) @675bc0: 3 = castable. Only TRANSPORT (0x28a) is decoded; the other spells
// return 0 (TODO(spells)).
int32_t spell_is_castable(const Game& g, int32_t spell, int32_t o, int32_t who, int32_t p3);
// SpellType::pay_cast_costs(o, who) @676c40: 0 paid, 1 cannot pay. unpay_cast_costs @676ff0 gives it back.
int32_t pay_cast_costs(Game& g, int32_t spell, int32_t o, int32_t who);
void unpay_cast_costs(Game& g, int32_t spell, int32_t o, int32_t who);
// SpellTypeData::get_job_time(o, who) @675800: the spell's JOB_TIME with the French / Antipater / spy / pack / sabotage
// modifiers.
int32_t spell_job_time(const Game& g, int32_t spell, int32_t o, int32_t who);

// Unit::do_cast(order) @5ebfe0, the handler of the current CAST_SPELL order. The untargeted branch only (spell flags
// & 0xe == 0); a targeted spell kills its order (TODO(spells)).
int32_t do_cast(Game& g, Unit& u, Order& o);
// SpellType::cast(o, who, p3, p4) @676ce0: is_castable == 3, then the switch on the spell. 0x28a -> cast_transport.
void spell_cast(Game& g, int32_t spell, int32_t o, int32_t who, int32_t p3, int32_t p4);

}  // namespace ron::sim
