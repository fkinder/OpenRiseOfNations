// Combat stat getters and damage (docs/combat-spec.md sections 5.1-5.5, 6.4): ObjectData::get_damage,
// Object::do_damage (scaling, leader statistics, building collateral), Object::take_damage (accumulation, uber
// thresholds, kill credit, death dispatch), kill and score values.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "sim/aircraft.hpp"
#include "sim/heroes.hpp"
#include "sim/ai.hpp"
#include "sim/combat_internal.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace combat_detail;

namespace {

int32_t trunc256(int32_t x) { return x / 256; }  // (x + ((x >> 31) & 0xff)) >> 8

// BuildTypeData::basic_type: the root of the type's `from` chain (TypeData +0x3c).
int32_t basic_build_type(const Game& g, int32_t t) {
	for (int guard = 0; guard < 64; guard++) {
		const Type& ty = g.type(t);
		if (ty.from < kBuilds || ty.from >= kEndBuilds || ty.from == t) return t;
		t = ty.from;
	}
	return t;
}

}  // namespace

// ---------------------------------------------------------------- type predicates

bool type_is_siege(const Type& t) { return t.type >= kUnits && t.type < kEndGaia && (t.unit_flags & utflag::kSiege); }
bool type_is_tank(const Type& t) { return t.type >= kUnits && t.type < kEndGaia && (t.unit_flags & utflag::kTank); }
bool unit_is_plane(const Unit& u) {
	return u.ptype && u.ptype->domain == kDomainAir && !(u.ptype->unit_flags & utflag::kHelicopter);
}
bool obj_is_wonder(const Object& ob) { return !ob.is_unit && ob.type > 0x20d && ob.type < 0x21f; }
bool is_build_active(const Object& ob) { return !ob.is_unit && (ob.flags & objflag::kActive); }
bool unit_is_moving(const Unit& u) { return order_is_move(u.order_type()); }

// BuildTypeData::is_training_building @639de0 / is_military_trainer @63bcf0: build_flags bits of the basic type.
bool build_is_training(const Game& g, int32_t t) {
	return (g.type(basic_build_type(g, t)).build_flags & 0x80000000u) != 0;
}
bool build_is_military_trainer(const Game& g, int32_t t) {
	return (g.type(basic_build_type(g, t)).build_flags & 0x40000000u) != 0;
}

// ---------------------------------------------------------------- stat getters

int32_t raw_max_range(const Game& g, const Type& t) {
	if (t.type == g.range_swap_type) return g.range_swap_value;
	return t.max_range;
}

// obj_attack / obj_armor / obj_hits / obj_max_range / obj_min_range: stats.cpp (combat-spec 5.1).

int32_t obj_hits_left(const Game& g, const Object& ob) {
	// ObjectData::hits_left @6535c0: hits - damage, not below 0.
	const int32_t h = obj_hits(g, ob) - ob.damage;
	return h > 0 ? h : 0;
}

int32_t unit_recharge(const Game& g, const Unit& u, int32_t* sup) {
	// @60fdf0: siege units out of supply (UnitData::in_supply, attrition.cpp) reload slower.
	if (sup) *sup = -1;
	const int32_t r = u.ptype->recharge;
	if (!type_is_siege(*u.ptype)) return r;
	const bool supplied = unit_in_supply(g, u, sup);
	if ((!RON_K(g, "artillery_under_attack_fires_slowly") || !(u.unit_masks2 & umask2::kUnderAttack)) && supplied)
		return r;
	if (type_is(*u.ptype, 267)) return r * 2;
	return r * 3 / 2;
}

int32_t unit_stance_type(const Type& t) {
	// UnitTypeData::get_stance_type @61d350
	if (t.role & role::kCombat) return (t.unit_flags2 & utflag2::kPacker) ? kStancePacker : kStanceCombat;
	if (t.type >= 50 && t.type <= 53) return kStanceWorker;
	return (t.unit_flags2 & 6) == 2 ? kStanceCaster : kStanceNone;
}

int32_t get_combat_stance(const Game& g, const Object& ob) {
	if (ob.is_unit) {
		const Unit& u = static_cast<const Unit&>(ob);
		return unit_stance_type(*u.ptype) == kStanceCombat ? u.stance : kStandGround;
	}
	// BuildTypeData::get_stance_type @6396c0: training buildings other than cities, forts, 430/431 (packer), 436,
	// 420, 447 and 520 have the COMBAT stance type.
	const Build& b = static_cast<const Build&>(ob);
	const Type& t = *b.ptype;
	bool combat = false;
	if (build_is_training(g, t.type) && !type_is(t, kVillage) && !type_is(t, 0x1bb)) {
		const int32_t i = t.type;
		combat = i != 0x1ae && i != 0x1af && i != 0x1b4 && i != 0x1a4 && i != 0x1bf && i != 0x208;
	}
	return combat ? b.stance : kStandGround;
}

int32_t obj_captain(const Game& g, const Object& ob) {
	if (!ob.is_unit) return ob.o;
	const Unit* p = static_cast<const Unit*>(&ob);
	for (int guard = 0; p->o_up >= 0 && guard < kUnitLimit; guard++) {
		const Unit* q = g.unit(p->who, p->o_up);
		if (!q) break;
		p = q;
	}
	return p->o;
}

int32_t unit_curr_uber_size(const Game& g, const Unit& u, bool from_here) {
	// @60a760: from the captain (unless from_here), 1 + the living members down the o_down chain.
	const Unit* p = &u;
	if (!from_here) p = g.unit(u.who, obj_captain(g, u));
	int32_t n = 1;
	for (int guard = 0; p && p->o_down >= 0 && guard < kUnitLimit; guard++) {
		const Unit* d = g.unit(p->who, p->o_down);
		if (!d || !d->alive()) break;
		n++;
		p = d;
	}
	return n;
}

// ---------------------------------------------------------------- values

int32_t get_score_value(const Game& g, int32_t type) {
	int32_t t = type;
	if (t == 66 || t == 67 || t == 68) t = 50;  // militia line priced as the citizen (score value 0)
	const Type& ty = g.type(t);
	int32_t v = 0;
	for (int32_t c : ty.costs) v += c;
	if (type >= kUnits && type < kEndGaia) return RON_K(g, "unit_cost_factor") * v;
	if (type >= kBuilds && type < kEndBuilds) return RON_K(g, "build_cost_factor") * v;
	if (type >= kSpells && type < kEndSpells) return RON_K(g, "spell_cost_factor") * v;
	return RON_K(g, "tech_cost_factor") * v;
}

int32_t get_kill_value(const Game& g, int32_t type, int32_t who) {
	// UnitTypeData::get_kill_value @61d3d0: + (support costs) x (num_units + num_queued) of the victim's owner.
	// TODO(economy): num_queued (production queues) is not tracked yet (0).
	const int32_t sv = get_score_value(g, type);
	if (who < 0 || who >= kNumLeaders || type < kUnits || type >= kEndUnits) return sv;
	const Type& t = g.type(type);
	return (t.support_cost[0] + t.support_cost[1]) * int32_t(g.leaders[size_t(who)].num_units[size_t(type - kUnits)]) + sv;
}

int32_t sum_rules_cost(const Game& g, int32_t type) {
	const Type& ty = g.type(type);
	int32_t v = 0;
	for (int32_t c : ty.costs) v += c;
	if (type >= kUnits && type < kEndGaia) return v * RON_K(g, "unit_cost_factor");
	if (type >= kBuilds && type < kEndBuilds) return v * RON_K(g, "build_cost_factor");
	return v;
}

// ---------------------------------------------------------------- get_damage

int32_t flank_level(uint32_t target_angle, uint32_t attack_angle) {
	const uint32_t d = target_angle - attack_angle - 0x80000000u;
	if (d < 0x2aaaaaaau || d > 0xd5555555u) return 0;
	return ((d + 0xa0000000u) > 0x40000000u ? 1 : 0) + 1;
}

int32_t entrench_sector(uint32_t trench_angle, uint32_t attack_angle) {
	const uint32_t x = trench_angle - attack_angle;
	if (x + 0x55555556u < 0xaaaaaaacu) return ((x + 0x20000000u) > 0x40000000u ? 1 : 0) + 1;
	return 0;
}

int32_t get_damage_core(const Rules& r, const Type& att, const Type& def, const DamageSituation& s, int32_t* death) {
	// @644130 (the order of the tests is the binary's; see combat-spec 5.2).
	const int32_t C_flank = r.constants.at(uint32_t(constant_offset("flank_bonus")));
	auto K = [&](const char* n) {
		const int off = constant_offset(n);
		return off >= 0 ? r.constants.at(uint32_t(off)) : 0;
	};
	(void)C_flank;
	const bool att_unit = att.type >= kUnits && att.type < kBuilds, def_unit = def.type >= kUnits && def.type < kBuilds;
	const bool att_build = att.type >= kBuilds && att.type < kEndBuilds;
	const bool def_build = def.type >= kBuilds && def.type < kEndBuilds;
	const int32_t bal = r.balance_at(att.type, def.type);
	int32_t dt = kDeathShot;
	int32_t armor = s.armor;
	const int32_t attack = s.attack;
	const uint32_t amask = uint32_t(att.obj_masks), dmask = uint32_t(def.obj_masks);
	// (Forts/castles and city buildings with GUNPOWDER_AGE turn ARCHERY into GUN on a local copy of the mask: no
	// reader below, so nothing to do.)
	int32_t d = attack * bal / 100;
	if (amask & objmask::kMusketInf) armor = armor * 133 / 100;
	if (att_build && (dmask & objmask::kSiege) && def_unit && (def.unit_flags2 & utflag2::kPacker) && !s.target_packed)
		d /= 3;
	if (att_build && def_unit && def.attack != 0 && s.target_moving) {
		if (!(dmask & objmask::kFoot)) {
			if (type_is_siege(def) || (def.unit_flags2 & utflag2::kSupply) || (def.unit_flags2 & utflag2::kCaravan)) d /= 2;
		} else {
			d = d * 3 / 4;
		}
	}
	if (att_build && def_build && !s.target_active) d *= 4;
	if (def_build && ((att.type >= 50 && att.type <= 53) || type_is(att, 66)) && !s.target_in_attacker_territory) d /= 2;
	if (type_is(att, 313) && def_build && !s.target_active) d /= 2;
	if (att_unit && att.domain == kDomainAir && !(amask & objmask::kMissile) && type_is(def, 534, true))
		d = (100 - K("red_fort_air_defense")) * d / 100;
	if (def_unit) {
		if (s.target_packed) d *= 2;
		if ((def.unit_flags2 & utflag2::kPacker) && !s.target_packed) armor += 1;
	}
	if (amask & objmask::kExplosive) dt = kDeathSploded;
	if (amask & objmask::kBombard) dt = kDeathSploded;
	if (att_build) dt = s.building_shot == 4 ? kDeathSploded : kDeathShot;
	if (s.wellington) d += K("wellington_siege_attack");
	if (s.japanese_ages >= 0) {
		int32_t k = K("japanese_damage");
		if (k < 0) k = -(k * s.japanese_ages);
		d = (k + 100) * d / 100;
	}
	if ((att_build || (att.domain == kDomainGround && def.domain != kDomainGround)) && def_unit &&
	    (s.target_severe_attrition || (def.domain == kDomainSea && s.team_style2_not_target)))
		d *= 2;
	if (s.splash) {
		if (def_unit) d /= def.uber_size;
		d = att.splash_percent * d / 100;
		if (def_unit) {
			if (type_is_siege(def) && (def.unit_flags2 & utflag2::kPacker) && !s.target_packed) d *= 3;
			if (type_is(def, 323)) d = d * 25 / 100;
		}
	}
	if (def_unit && s.target_in_river) d = trunc256(K("river_modifier") * d);
	if (def_unit && s.target_decoy) {
		armor = 0;
		d = std::max(attack, d) * 1000;
	}
	if (def_build && s.target_in_unfriendly_territory && !s.scenario_no_border_penalty) {
		armor = 0;
		if (def.attack == 0) d *= 4;  // (!w.is_build || type attack == 0)
	}
	if (att_unit && def_unit && s.has_angles && !(amask & objmask::kCivilian) && !(dmask & objmask::kCivilian) &&
	    !(amask & objmask::kAir) && !(dmask & objmask::kAir) && !(amask & objmask::kNaval) && !(dmask & objmask::kNaval)) {
		const int32_t f = flank_level(s.target_angle, s.attack_angle);
		if (f) {
			int32_t b = K("flank_bonus");
			if (dmask & objmask::kVehicle) b = trunc256(K("vehicle_flank_bonus") * b);
			else if (dmask & objmask::kMounted) b = trunc256(K("cavalry_flank_bonus") * b);
			d = (b * f + 100) * d / 100;
		}
	}
	int32_t hp = (d + 5) / 10 - armor;
	if (s.overkill && att.max_range != 0 && att_unit && def_unit) {
		hp = trunc256(K("overkill_damage") * hp);
		if (type_is(def, 265) && !type_is_siege(att)) hp /= 2;
	}
	if ((dmask & (objmask::kLightInf | objmask::kModernInf | objmask::kMusketInf)) && s.target_on_rocky)
		hp = trunc256(K("rocky_modifier") * hp);
	if (def.domain != kDomainAir && att.domain != kDomainAir && !type_is_siege(att) && s.height_diff > 0)
		hp += (s.height_diff * K("height_bonus") * hp) / (K("height_increment") * 100);
	if (def_unit && s.target_entrenched && !type_is(att, 131)) {
		if (s.splash || entrench_sector(s.trench_angle, s.attack_angle) == 0) {
			hp = trunc256(K("entrenchment_modifier") * hp);
			if (s.antipater) hp = trunc256(K("antipater_entrench_bonus") * hp);
		}
	}
	if (s.cossack && K("russian_cossack_damage") != 0 &&
	    ((def.unit_flags2 & utflag2::kSupply) || type_is_siege(def)))
		hp = (K("russian_cossack_damage") + 100) * hp / 100;
	if (hp < 1 && !(att.domain == kDomainGround && def.domain == kDomainSea) &&
	    (dmask & objmask::kAir) == ((amask >> 3) & objmask::kAir) && !s.splash)
		hp = 1;
	if (def.type == 541 && att.domain == kDomainAir && K("super_immune")) hp = 0;
	if (def_build && s.recapture_own_city) hp = trunc256(K("recapture_city_modifier") * hp);
	if (death) *death = dt;
	return hp;
}

HitScale scale_damage(const Type& att, bool att_is_unit, int32_t hp, int32_t mult, bool ammo, bool zoc) {
	int32_t v = hp;
	if (zoc) v *= 2;
	if (att_is_unit) {
		v *= mult;
		if (v <= 256) v = 256;
		if (ammo) v /= att.ammo_per_att;
		v /= att.uber_size;
	} else {
		v = v * mult / att.ammo_per_att;
	}
	const int32_t q = v / 16;
	return {q / 16, q % 16};
}

int32_t get_damage(Game& g, const Object& att, int32_t ox, int32_t whom, uint32_t angle, bool splash, bool overkill,
                   int32_t* death) {
	const Object* tp = obj_at(g, ox, whom);
	if (!tp || !att.ptype || !tp->ptype) {
		if (death) *death = kDeathShot;
		return 0;
	}
	const Object& t = *tp;
	const Unit* tu = as_unit(&t);
	const Unit* au = as_unit(&att);
	const World& w = g.world;
	DamageSituation s;
	s.armor = obj_armor(g, t);
	s.attack = obj_attack(g, att);
	if (tu) {
		s.target_packed = (tu->unit_masks & umask::kPacked) != 0;
		s.target_moving = unit_is_moving(*tu);
		s.target_severe_attrition = (tu->unit_masks & umask::kSevereAttrition) != 0;
		s.target_in_river = tu->z < 0;
		s.target_decoy = (tu->unit_masks & umask::kDecoy) != 0;
		s.target_entrenched = (tu->unit_masks & umask::kEntrenched) != 0;
		s.antipater = (tu->unit_masks2 & umask2::kAntipaterEntr) != 0;
		s.trench_angle = tu->trench_angle;
	} else {
		s.target_active = (t.flags & objflag::kActive) != 0;
		const int32_t wx = wcoord_of(t.x), wy = wcoord_of(t.y);
		const int32_t owner = w.in_w(wx, wy) ? w.w(wx, wy).who : -1;
		s.target_in_attacker_territory = owner == att.who;
		// WallData::in_unfriendly_territory @63eca0: owned, not the building's owner, not mutually allied.
		s.target_in_unfriendly_territory = owner >= 0 && owner != t.who && !is_ally(g, owner, t.who);
		// A finished city whose race (+0x5f, the leader it is assimilated to) is the attacker: recapture modifier.
		if ((t.flags & objflag::kActive) && (t.flags & objflag::kCity) && t.who >= 0 && t.who < 8) {
			const Build& tb2 = static_cast<const Build&>(t);
			if (tb2.city >= 0 && size_t(tb2.city) < g.cities[size_t(t.who)].size())
				s.recapture_own_city = g.cities[size_t(t.who)][size_t(tb2.city)].race == att.who;
		}
	}
	// TODO(combat-spec 5.2): team style 2 targets, Wellington, Japanese and Russian powers (leader state).
	if (!att.is_unit) s.building_shot = get_shot(g, att);
	if (tu && au) {
		s.has_angles = true;
		s.target_angle = tu->angle;
		s.attack_angle = angle;
	}
	s.attack_angle = angle;
	if (overkill && obj_max_range(g, att) != 0 && au && tu && tu->damage_frame != 0 &&
	    g.frame - tu->damage_frame < RON_K(g, "overkill_frames") && obj_captain(g, att) != tu->damage_o)
		s.overkill = true;
	{
		const int32_t wx = wcoord_of(t.x), wy = wcoord_of(t.y);
		s.target_on_rocky = w.in_w(wx, wy) && (w.w(wx, wy).flags & wflag::kRock);
	}
	if (t.ptype->domain != kDomainAir && att.ptype->domain != kDomainAir && !type_is_siege(*att.ptype) && t.z < att.z)
		s.height_diff = att.z - t.z;
	s.splash = splash;
	return get_damage_core(*g.rules, *att.ptype, *t.ptype, s, death);
}

// ---------------------------------------------------------------- do_damage

void do_damage(Game& g, Object& att, int32_t ox, int32_t whom, uint32_t angle, int32_t guys, int32_t ammo,
               int32_t mult, bool splash, bool no_react) {
	// @64a480
	if (mult < 1) return;
	Unit* au = as_unit(&att);
	if (au && (au->unit_masks & umask::kDecoy)) return;
	Object* tp = obj_at(g, ox, whom);
	if (!tp) return;
	Object& t = *tp;
	int32_t dt = kDeathShot;
	int32_t hp = get_damage(g, att, ox, whom, angle, splash, true, &dt);
	int32_t death = dt;
	if (ammo >= 0 && size_t(ammo) < g.ammo.size() && g.anims &&
	    (g.anims->ammo_flags(g.ammo[size_t(ammo)].gpiece) & ammoflag::kFlame))
		death = kDeathScorch;
	if (Unit* tu = as_unit(&t); tu && !no_react) {
		if (Unit* cap = g.unit(tu->who, obj_captain(g, *tu))) target_opportunity(g, *cap, att.o, att.who, false);
		if (tu->unit_masks & umask::kZoc) hp *= 2;
		if (tu->damage_frame == 0 || g.frame - tu->damage_frame >= RON_K(g, "overkill_frames")) {
			tu->damage_frame = g.frame;
			tu->damage_o = int16_t(obj_captain(g, att));
			tu->damage_who = att.who;
		}
	}
	// Object::attempt_launch: a damaged carrier / airbase scrambles a fighter at the attacker (sim/aircraft.cpp).
	if (!(uint32_t(att.ptype->obj_masks) & 0x8000000u) && att.alive()) attempt_launch(g, t, att.x, att.y);
	int32_t v;
	if (au) {
		v = hp * mult;
		if (v <= 256) v = 256;
		if (ammo >= 0) v /= att.ptype->ammo_per_att;
		v /= att.ptype->uber_size;
	} else {
		v = hp * mult / att.ptype->ammo_per_att;
	}
	const int32_t q = v / 16;
	const int32_t hp_int = q / 16, frac = q % 16;
	// Leader bookkeeping (walked LeaderData).
	const int32_t add = (au && type_is_siege(*att.ptype) && !t.is_unit) ? 5 : 1;
	if (att.who >= 0 && att.who < 8) g.leaders[size_t(att.who)].hits_current_frame = int16_t(g.leaders[size_t(att.who)].hits_current_frame + add);
	if (t.who >= 0 && t.who < 8) g.leaders[size_t(t.who)].hits_current_frame = int16_t(g.leaders[size_t(t.who)].hits_current_frame + add);
	if (att.who != t.who && att.who >= 0 && att.who < 8 && t.who >= 0 && t.who < 8) {
		g.leaders[size_t(att.who)].treaty(t.who) |= 2;
		g.leaders[size_t(t.who)].treaty(att.who) |= 2;
		// TODO(combat-spec 5.3): attack / raid / capital / ally stamps with their 300-frame throttles, city stamps,
		// Armies::emergency for AI owners.
	}
	const bool tgt_was_active_build = is_build_active(t);
	// local_70: an active city not yet reduced before this hit (the "last reduced" stamp is not throttled then).
	const bool city_was_standing = !t.is_unit && (t.flags & objflag::kCity) && is_build_active(t) && obj_hits(g, t) > t.damage;
	static const bool trace = std::getenv("RON_COMBAT_TRACE") != nullptr;  // developer trace (stderr), never sim state
	if (trace)
		std::fprintf(stderr, "f%d dmg %d:%d -> %d:%d hp %d frac %d (get_damage %d) ammo %d mult %d\n", g.frame, att.who, att.o, whom, ox,
		             hp_int, frac, hp, ammo, mult);
	const int32_t killed = take_damage(g, t, hp_int, frac, death, ammo, false, angle, att.o, att.who, splash);
	if (killed) {
		if (!is_wall(t)) return;
		if (att.who >= 0 && att.who < 8) {
			Leader& L = g.leaders[size_t(att.who)];
			L.buildings_razed++;
			const int32_t kv = get_kill_value(g, t.type, whom);
			if (!is_ally(g, att.who, whom)) {
				L.score_combat += kv / 5;
			} else {
				L.score_combat -= kv / 5;
				if (L.score_combat < 0) L.score_combat = 0;
			}
			// Build::plunder(att.who) @623660 (heroes-spec 7.2): not allied, not splash, the attacker is not an aircraft.
			if (att.ptype->domain != kDomainAir && !splash && !is_ally(g, whom, att.who))
				build_plunder(g, static_cast<Build&>(t), att.who);
			const int16_t k = tgt_was_active_build ? 5 : 2;
			L.kills_current_frame = int16_t(L.kills_current_frame + k);
		}
		if (whom >= 0 && whom < 8) {
			const int16_t k = tgt_was_active_build ? 5 : 2;
			g.leaders[size_t(whom)].kills_current_frame = int16_t(g.leaders[size_t(whom)].kills_current_frame + k);
		}
		return;
	}
	// TODO(combat-spec 5.3): flamethrower ejection / entrenchment removal, Sunka Wakan, garrison ejection by fire
	// rafts and aircraft.
	if (t.is_unit) return;
	// ---- building collateral (combat-spec 5.5): units next to a damaged building.
	if (au && au->ptype->domain != kDomainAir && !(type_is_siege(*att.ptype) && att.ptype->domain == kDomainSea)) {
		const Circles& C = circles();
		const int32_t cx = wcoord_of(t.x), cy = wcoord_of(t.y);
		const int32_t reach = 192 * std::max(t.ptype->x_size, t.ptype->y_size);
		const bool ground_siege = type_is_siege(*att.ptype) && att.ptype->domain == kDomainGround;
		for (int32_t k = 0; k < C.radius[1]; k++) {
			const int32_t wx = cx + C.x[size_t(k)], wy = cy + C.y[size_t(k)];
			if (!g.world.in_w(wx, wy)) continue;
			int32_t o = g.world.w(wx, wy).down, w2 = g.world.w(wx, wy).down_who;
			while (o >= 0) {
				Object* ob = g.object(w2, o);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				// valid_search(ENEMY of the attacker), alive, on the map, not the target itself.
				if (is_enemy(g, ob->who, att.who) && ob->alive() && on_map(*ob) && !(ob->o == ox && ob->who == whom) &&
				    vector_dist(ob->x - t.x, ob->y - t.y) <= reach) {
					int32_t m = -1;
					if (!ground_siege) {
						const int32_t rr = std::max(obj_max_range(g, att) * 192, 0x180);
						if (vector_dist(ob->x - att.x, ob->y - att.y) <= rr) m = mult / 8;
					} else {
						m = mult / 4;
					}
					// TODO(combat-spec 5.5): the Korean "build under fire" suppression.
					if (m >= 0) do_damage(g, att, ob->o, ob->who, angle, guys, ammo, m, false, true);
				}
				o = no;
				w2 = nw;
			}
		}
	}
	// ---- cities (combat-spec 5.3 tail, capture-spec 2.4): an enemy hit on a city tries Build::check_capture; a
	// fully reduced city makes a computer leader's siege attacker charge with its army, and stamps the city's
	// reduce_stamp (+0x1c; 300-frame throttle once it was already reduced; the UI event is presentation).
	if (!is_city_type(*t.ptype) || whom == att.who) return;
	if (check_capture(g, t, att.o, att.who)) return;
	Object* tq = obj_at(g, ox, whom);
	if (!tq || !(tq->flags & objflag::kCity) || !is_build_active(*tq)) return;
	if (tq->damage < obj_hits(g, *tq)) return;
	const int32_t city = static_cast<Build*>(tq)->city;
	if (att.o >= 0 && att.who >= 0 && att.who < 8 && !(g.leaders[size_t(att.who)].flags & leaderflag::kHuman) && au &&
	    type_is_siege(*att.ptype) && (au->unit_masks & umask::kAiControl)) {
		const int32_t a = object_get_army(g, *au);
		if (a >= 0) army_charge(g, g.armies[size_t(att.who)][size_t(a)], ox, whom);
	}
	if (city < 0 || whom < 0 || whom >= 8 || size_t(city) >= g.cities[size_t(whom)].size()) return;
	City& C = g.cities[size_t(whom)][size_t(city)];
	if (!city_was_standing && g.frame - C.reduce_stamp < 300) return;
	C.reduce_stamp = g.frame;
}

// ---------------------------------------------------------------- take_damage

int32_t take_damage(Game& g, Object& tgt, int32_t hp, int32_t frac, int32_t death, int32_t ammo, bool quiet,
                    uint32_t angle, int32_t att_o, int32_t att_who, bool splash) {
	// @652020
	if (!tgt.alive()) return 0;
	if (hp < 1 && frac < 1) frac = 1;
	const Object* att = obj_at(g, att_o, att_who);
	if (!quiet) {
		// TODO(combat-spec 5.4): only when not multiplayer and difficulty < 2.
		if (tgt.who < 8) g.leaders[size_t(tgt.who)].frame_attacked = g.frame;
		if (tgt.damage == 0 && is_wall(tgt) && g.rnd_draw(100) < 5) {  // @6520ef
			const bool fortlike = type_is(*tgt.ptype, 0x1bb) || type_is(*tgt.ptype, 437) || type_is(*tgt.ptype, 415);
			if (fortlike && att && att->ptype && type_is_siege(*att->ptype)) {
				const int32_t n = g.rnd_draw(2) + 3;  // @652199
				(void)n;  // TODO(gaia): Objects::add_flock(tile, -1, n) and its own draws.
			}
		}
		// TODO(combat-spec 5.4): AI war declaration on missile hits.
	}
	const int32_t t = int8_t(tgt.damage_frac + frac);
	const int32_t before = tgt.damage;
	tgt.damage += hp + t / 16;
	tgt.damage_frac = int8_t(t % 16);
	if (!tgt.is_unit) {
		Build& b = static_cast<Build&>(tgt);
		if (!quiet && !(b.flags & objflag::kActive) && tgt.damage > 0 && !(att && att->ptype && att->ptype->domain == kDomainAir)) {
			b.job_counter -= 50 * (tgt.damage - before);
			if (b.job_counter < 0) b.job_counter = 0;
		}
		if (!quiet && att_who != tgt.who) {
			// UNDER_ATTACK | ATTACKING, except for a citizen or militia attacker standing off its own territory.
			bool mark = true;
			if (att_o >= 0 && att_who >= 0 && att) {
				const bool civ = att->type == ptype::kPeasants || att->type == ptype::kPeasantsKorean ||
				                 type_is(*att->ptype, ptype::kMilitia);
				if (civ) {
					const int32_t wx = wcoord_of(b.x), wy = wcoord_of(b.y);
					mark = g.world.in_w(wx, wy) && g.world.w(wx, wy).who == att_who;
				}
			}
			if (mark) b.build_masks |= 0x30;
			// The building's city: UNDER_ATTACK | ATTACKING | EVER_ATTACKED (stops the city's healing, capture-spec 2.5).
			if (b.city >= 0 && tgt.who >= 0 && tgt.who < 8 && size_t(b.city) < g.cities[size_t(tgt.who)].size())
				g.cities[size_t(tgt.who)][size_t(b.city)].city_flags |= 0xe;
			// TODO(garrison): the AI alarm (Group::action_alarm of the city for cities and garrison types).
		}
		// A destroyed building lets its garrison out (production-spec 6.11: only with someone inside).
		if (b.inside_down >= 0 && tgt.damage >= obj_hits(g, tgt) && !can_carry_domain(g, tgt, kDomainAir))
			eject_contents(g, tgt, 1, -1, 0, 1);
		if ((b.flags & objflag::kCity) && (b.flags & objflag::kActive)) {
			if (b.damage >= obj_hits(g, b)) {
				b.flags |= objflag::kNewThink;
				b.damage = obj_hits(g, b);
			}
			return 0;
		}
	}
	// TODO(combat-spec 5.4): the AI peasant alarm (cities).
	int32_t thr = obj_hits(g, tgt);
	Unit* tu = as_unit(&tgt);
	if (tu) {
		if (!quiet) set_in_danger(g, *tu, false);
		const int32_t u = tu->ptype->uber_size;
		if (u > 1) thr = (tu->o_up < 0 && unit_curr_uber_size(g, *tu) == 1) ? thr - (u - 1) * thr / u : thr / u;
		// TODO(combat-spec 5.4): Aztec damage mask (tribe bonus 0).
	}
	if (tgt.damage < thr) {
		if (Object* cap = g.object(tgt.who, obj_captain(g, tgt))) cap->flags |= objflag::kNewThink;
		// TODO(combat-spec 5.4): an AI-owned damaged unfinished site with build_masks 0x2000 is disbanded (RNG).
		return 0;
	}
	// ---- death
	const Ammo* am = ammo >= 0 && size_t(ammo) < g.ammo.size() ? &g.ammo[size_t(ammo)] : nullptr;
	if (tu && !(tu->unit_masks & umask::kDecoy) && death == kDeathSploded &&
	    (has_objmask(tgt, objmask::kFoot) || has_objmask(tgt, objmask::kMounted))) {
		// Exploded figures face away from the blast. TODO(@6524c8): the ammo case's find_angle arguments are not
		// recovered; the shot direction is used.
		const uint32_t a = am ? find_angle(am->ex - am->sx, am->ey - am->sy) : angle + 0x80000000u;
		for (int32_t i = 0; i < tu->guy_mark && size_t(i) < tu->guys.size(); i++) guy_set_angle(g, *tu, tu->guys[size_t(i)], a, true);
		for (size_t i = size_t(tu->ptype->squad_size); i < tu->guys.size(); i++) guy_set_angle(g, *tu, tu->guys[i], a, true);
	}
	int32_t gpiece = -1;
	float deg = 0.0f;
	if (am) {
		gpiece = am->gpiece;
		deg = float(int32_t((uint64_t(find_angle(am->ex - am->sx, am->ey - am->sy)) * 360u) >> 32));  // presentation
	}
	const bool decoy = tu && (tu->unit_masks & umask::kDecoy);
	// TODO(caravans): Caravans::new_danger.
	if (is_wall(tgt)) {
		if (tgt.who < 8) {
			g.leaders[size_t(tgt.who)].buildings_lost++;
			g.scen.last_razed[size_t(tgt.who)] = tgt.o;  // ScenarioData::last_razed
		}
	} else if (tu->o_up < 0 && unit_curr_uber_size(g, *tu) == 1 && !decoy) {
		if (tgt.who < 8) g.leaders[size_t(tgt.who)].units_lost++;
		if (att_o >= 0) {
			if (tgt.who < 8) g.leaders[size_t(tgt.who)].deaths_current_frame++;
			if (att_who >= 0 && att_who < 8) {
				Leader& L = g.leaders[size_t(att_who)];
				L.kills_current_frame++;
				L.units_killed++;
				// The victim: the unit itself, or the first passenger of a transport.
				const Object* victim = &tgt;
				if (tu->ptype->unit_flags & utflag::kTransport) victim = g.object(tu->inside_down_who, tu->inside_down);
				if (victim && victim->is_unit) {
					const int32_t kv = get_kill_value(g, victim->type, victim->who);
					if (!is_ally(g, att_who, victim->who)) {
						L.score_combat += kv / 5;
					} else {
						L.score_combat -= kv / 5;
						if (L.score_combat < 0) L.score_combat = 0;
					}
					// Unit::plunder (Object::take_damage @652020; heroes-spec 7.1): on a kill by a live non-allied object that
					// is not splash fire. Aztec barracks / stable / dock units plunder as Aztecs; spies, generals and
					// merchants plunder plainly; anyone else only with THE DESPOT / Spitamenes in range.
					Unit& dying = *tu;
					const Object* aobj = g.object(att_who, att_o);
					if (aobj && aobj->is_unit && aobj->alive() && !splash && !is_ally(g, victim->who, att_who)) {
						bool call = false;
						int32_t aztec = 0, despot = 0;
						const int32_t aw = aobj->ptype->where;
						if (has_tribe_bonus(g, att_who, 0) && (aw == 0x1ab || aw == 0x1ac || aw == 0x1b0)) {
							call = true;
							aztec = 1;
						} else if (type_is(*victim->ptype, 0x3b) || type_is(*victim->ptype, 0x36) || type_is(*victim->ptype, 0x3f)) {
							call = true;  // a spy, general or the like dies: plundered plainly
						} else {
							const Leader& AL = g.leaders[size_t(att_who)];
							const bool d = (AL.num_units[size_t(hero::kTheDespot - kUnits)] != 0 &&
							                find_hero(g, dying.x, dying.y, att_who, 0, hero::kTheDespot, 0) >= 0) ||
							               (AL.num_units[size_t(hero::kSpitamenes - kUnits)] != 0 &&
							                find_hero(g, dying.x, dying.y, att_who, 0, hero::kSpitamenes, 0) >= 0);
							if (d && RON_K(g, "thedespot_unit_plunder") != 0) {
								call = true;
								despot = 1;
							}
						}
						if (call) unit_plunder(g, dying, att_o, att_who, aztec, despot);
					}
					// The Inca refund: the victim's owner gets part of the price back.
					if (!is_ally(g, victim->who, att_who) && RON_K(g, "inca_refund") != 0 && has_tribe_bonus(g, victim->who, 2))
						unit_plunder(g, dying, dying.o, dying.who, 0, 0);
				}
			}
		}
	}
	(void)splash;
	const int32_t overflow = tgt.damage - thr;
	object_die(g, tgt, death, gpiece, deg);
	if (!decoy) return 1;
	if (overflow > 0) {
		const int32_t cap = obj_captain(g, tgt);
		if (cap >= 0 && cap != tgt.o) {
			if (Object* c = g.object(tgt.who, cap)) take_damage(g, *c, overflow, 0, death, ammo, quiet, angle, cap, tgt.who, false);
		}
	}
	return 2;
}

}  // namespace ron::sim
