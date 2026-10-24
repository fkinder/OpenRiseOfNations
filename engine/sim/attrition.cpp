// Healing, supply and attrition (docs/combat-spec.md section 7, docs/economy-spec.md 7.2-7.3): the leader's attrition
// values (Leader::calc_attrition / calc_anti_attrition), the Supplies list, Unit::process_healing, Unit::repair_damage
// with member regeneration, Unit::process_attrition, Unit::process_supply and Unit::suffer_attrition.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "sim/combat_internal.hpp"
#include "sim/fog.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/production.hpp"
#include "sim/orders.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"
#include "sim/economy.hpp"

namespace ron::sim {

bool war_allowed(const Game& g);  // ai.hpp (Game::war_allowed @594670)

namespace {

using namespace combat_detail;

// A rules constant array entry: element i of the array that starts at `name`.
int32_t KA(const Game& g, const char* name, int32_t i) {
	const int off = constant_offset(name);
	return off < 0 ? 0 : g.rules->constants.at(uint32_t(off + 4 * i));
}

// Tech TypeIndex ranges the level getters count (from the getters' immediates).
constexpr int32_t kAttrition1 = 0x2dd, kAttrition4 = 0x2e0;    // ATTRITION1..4 (Leader::calc_attrition)
constexpr int32_t kHealFaster1 = 0x2ef, kHealFaster3 = 0x2f1;  // HEAL_FASTER_1..3 (get_heal_level)
constexpr int32_t kSupplyWagons1 = 0x2fb, kSupplyWagons3 = 0x2fd;  // SUPPLY_WAGONS_1..3 (get_supply_upgrade)
constexpr int32_t kForaging1 = 0x2fe, kForaging2 = 0x2ff, kForaging3 = 0x300;
// Wonders and other types read here.
constexpr int32_t kTerraCottaWonder = 0x211, kColosseum = 0x212, kRedFort = 0x216, kVersailles = 0x218,
                  kStatueOfLiberty = 0x219, kKremlin = 0x21a;
constexpr int32_t kFortType = 0x1bb, kTowerType = 0x1b7;  // BuildTypeData fort (vslot 0xfc) and TOWER
constexpr int32_t kAircraftCarrier = 0x15f, kFishermen = 0x13d, kMilitiaClass = 0x42, kSpyClass = 0x3a;
// Rare goods (Leader.rare / rare_conquest bit = TypeIndex - 6).
constexpr int32_t kRareCitrus = 26 - 6, kRareTitanium = 36 - 6;
// Tribe bonuses.
constexpr int32_t kTbFrench = 10, kTbRussian = 13, kTbMongol = 0x11, kTbIroquois = 0x12;

bool has_rare(const Leader& l, int32_t bit) {
	return ((l.rare[size_t(bit >> 3)] | l.rare_conquest[size_t(bit >> 3)]) >> (bit & 7)) & 1;
}

// ObjectData::has_general (groups.cpp): generals are not simulated (-1), so the Antipater / Napoleon / Monarch / CEO
// healing and the general supply branches never fire.
// The leader's alive units of type t (LeaderData +0x56fe num_units[t - 50]).
int32_t num_units(const Game& g, int32_t who, int32_t t) {
	return g.leaders[size_t(who)].num_units[size_t(t - kUnits)];
}

bool is_supply_unit(const Unit& u) { return (u.ptype->unit_flags2 & 0x40) != 0; }  // UnitData::is_supply
bool is_hero_unit(const Unit& u) { return (u.ptype->unit_flags2 & 0x20) != 0; }    // UnitData::is_hero
bool is_special_unit(const Unit& u) { return (u.ptype->unit_flags2 & 0x10) != 0; } // UnitData::is_special
// UnitData::is_caravan (type vslot 0x130): the caravan bit the leader's caras counter follows.
bool is_caravan_unit(const Unit& u) { return (u.ptype->unit_flags2 & 8) != 0; }
bool unit_idle(const Unit& u) { return u.orders.empty(); }  // UnitData::is_idle @46fa40

// The owner of the W-cell under (x, y) (WData +0xf; -1 neutral).
int32_t cell_owner(const Game& g, int32_t x, int32_t y) {
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	return g.world.in_w(wx, wy) ? g.world.w(wx, wy).who : -1;
}

// The healing tail every branch shares: repair_damage(1, show, 1), a captain with no damage left drops
// UNIT_AZTEC_DAMAGE, and healing = max(healing, rate).
void heal_one(Game& g, Unit& u, int32_t rate, bool show, bool clear_aztec_if_whole) {
	unit_repair_damage(g, u, 1, show, true);
	if (clear_aztec_if_whole && u.o_up < 0 && u.damage == 0) u.unit_masks &= ~umask::kAztecDamage;
	u.healing = int16_t(std::max<int32_t>(rate, u.healing));
}

bool on_tick(const Game& g, const Unit& u, int32_t rate) { return rate != 0 && (g.frame + u.o) % rate == 0; }

// RON_ATTRITION_TRACE: developer trace on stderr (never sim state).
bool trace_on() {
	static const bool t = std::getenv("RON_ATTRITION_TRACE") != nullptr;
	return t;
}

}  // namespace

// ---------------------------------------------------------------- leader side

int32_t get_heal_level(const Game& g, int32_t who) {
	int32_t n = 0;
	for (int32_t t = kHealFaster1; t <= kHealFaster3; t++) n += has_preq(g, who, t) ? 1 : 0;
	return n;
}

int32_t get_supply_upgrade(const Game& g, int32_t who) {
	int32_t n = 0;
	for (int32_t t = kSupplyWagons1; t <= kSupplyWagons3; t++) n += has_preq(g, who, t) ? 1 : 0;
	return n;
}

void leader_calc_attrition(Game& g, int32_t who) {
	// @6cdea0: the consecutive ATTRITION1..4 techs pick attrition_upgrade[n + 3]; then the Colosseum, Russian, CtW and
	// Kremlin percentages, each keeping at least 1. give_att_disabled: 0.
	Leader& l = g.leaders[size_t(who)];
	int32_t v = 0;
	if (l.give_att_disabled == 0) {
		int32_t n = 0;
		for (int32_t t = kAttrition1; t <= kAttrition4 && has_preq(g, who, t); t++) n++;
		if (n != 0) v = KA(g, "attrition_upgrade", n + 3);
		auto pct = [&](int32_t k) {
			v = ((k + 100) * v) / 100;
			if (v == 0) v = 1;
		};
		if (has_wonder(g, who, kColosseum)) pct(RON_K(g, "colosseum_attrition"));
		if (has_tribe_bonus(g, who, kTbRussian)) pct(RON_K(g, "russian_attrition"));
		// Partisans (card 10, LeaderData +0x6900): this leader's attrition against enemy units on its land, +ctw_attrition %.
		if (g.ctw && g.leaders[size_t(who)].num_bonus_cards[10] != 0) pct(RON_K(g, "ctw_attrition"));
		if (has_wonder(g, who, kKremlin)) pct(RON_K(g, "kremlin_attrition"));
	}
	l.att = v;
}

void leader_calc_anti_attrition(Game& g, int32_t who) {
	// @6cdcc0, SSE single precision: 256.0f, then for the Foraging level, the Statue of Liberty, Mongols and Titanium
	// in that order anti = (anti * 100.0f) / (float)(100 - u), or 0 (and stop) when u >= 100.
	Leader& l = g.leaders[size_t(who)];
	l.anti_att = 256.0f;
	if (l.take_att_disabled != 0) {
		l.anti_att = 0.0f;
		return;
	}
	auto apply = [&](int32_t u) {
		if (u > 99) {
			l.anti_att = 0.0f;
			return false;
		}
		l.anti_att = (l.anti_att * 100.0f) / float(100 - u);
		return true;
	};
	int32_t lvl = has_preq(g, who, kForaging3) ? 2 : has_preq(g, who, kForaging2) ? 1 : has_preq(g, who, kForaging1) ? 0
	                                                                                                               : -1;
	if (lvl >= 0 && !apply(KA(g, "attrition_upgrade", lvl))) return;
	if (has_wonder(g, who, kStatueOfLiberty) && !apply(RON_K(g, "liberty_attrition"))) return;
	if (has_tribe_bonus(g, who, kTbMongol) && !apply(RON_K(g, "mongol_attrition"))) return;
	if (has_rare(l, kRareTitanium)) apply(RON_K(g, "titanium_attrition"));
}

// ---------------------------------------------------------------- Supplies

int32_t supplies_init_supply(Game& g, int32_t who, int32_t o) {
	// @73ad40: the first dead record below supply_mark, else a new one at the end; supply_mark covers it.
	auto& L = g.supplies[size_t(who)];
	Leader& l = g.leaders[size_t(who)];
	int32_t s = -1;
	for (int32_t i = 0; i < l.supply_mark; i++)
		if (!(L[size_t(i)].flags & 1)) {
			s = i;
			break;
		}
	if (s < 0) {
		if (l.supply_mark < int32_t(L.size())) {
			s = l.supply_mark;
		} else {
			L.emplace_back();
			s = int32_t(L.size()) - 1;
		}
	}
	l.supply_mark = std::max(l.supply_mark, s + 1);
	Game::Supply& r = L[size_t(s)];
	r.o = int16_t(s);
	r.unit = int16_t(o);
	r.who = int8_t(who);
	r.flags = 1;
	return s;
}

void supplies_close_supply(Game& g, int32_t who, int32_t s) {
	// @73acc0: clear the record, then drop dead records off the top of supply_mark.
	auto& L = g.supplies[size_t(who)];
	if (s < 0 || size_t(s) >= L.size()) return;
	L[size_t(s)].flags &= uint8_t(~1u);
	L[size_t(s)].who = -1;
	L[size_t(s)].unit = -1;
	Leader& l = g.leaders[size_t(who)];
	while (l.supply_mark > 0 && !(L[size_t(l.supply_mark - 1)].flags & 1)) l.supply_mark--;
}

int32_t supplies_find_supply(const Game& g, int32_t x, int32_t y, int32_t who) {
	// @73aba0: the first live, active, on-map supply unit whose SupplyData::get_radius @73b560 (supply_radius +
	// supply_radius_upgrade * get_supply_upgrade, + terra_cotta_range with the Terra Cotta wonder; tiles) * 192
	// reaches (x, y).
	if (who < 0 || who >= 8) return -1;
	const auto& L = g.supplies[size_t(who)];
	const Leader& l = g.leaders[size_t(who)];
	for (int32_t i = 0; i < l.supply_mark && size_t(i) < L.size(); i++) {
		const Game::Supply& r = L[size_t(i)];
		if (!(r.flags & 1)) continue;
		const Unit* u = g.unit(who, r.unit);
		if (!u || !u->alive() || !u->on_map()) continue;
		int32_t rad = RON_K(g, "supply_radius") + RON_K(g, "supply_radius_upgrade") * get_supply_upgrade(g, r.who);
		if (has_wonder(g, r.who, kTerraCottaWonder)) rad += RON_K(g, "terra_cotta_range");
		if (vector_dist(x - u->x, y - u->y) <= rad * 192) return i;
	}
	return -1;
}

bool unit_in_supply(const Game& g, const Unit& u, int32_t* sup) {
	// @609ef0: never for non-ground units; on its own cell; else a supply unit in range (its index in *sup).
	if (sup) *sup = -1;
	if (u.ptype->domain != kDomainGround) return true;
	if (cell_owner(g, u.x, u.y) == u.who) return true;
	const int32_t s = supplies_find_supply(g, u.x, u.y, u.who);
	if (s < 0) return false;
	if (sup) *sup = s;
	return true;
}

// ---------------------------------------------------------------- unit side

bool unit_has_damage(const Game& g, const Unit& u, bool from_here) {
	// @609b50: from the captain (unless from_here) down the chain: a dead member, damage or damage_frac.
	const Unit* p = from_here ? &u : g.unit(u.who, obj_captain(g, u));
	for (int guard = 0; p && guard < kUnitLimit; guard++) {
		if (!p->alive() || p->damage != 0 || p->damage_frac != 0) return true;
		if (p->o_down < 0) return false;
		p = g.unit(p->who, p->o_down);
	}
	return false;
}

int32_t unit_get_attrition(const Game& g, const Unit& u, int32_t owner) {
	// @608fd0. The territory owner's att is the divisor (0: no attrition); the victim's anti_att scales the base.
	const int32_t att = g.leaders[size_t(owner)].att;
	if (att == 0 || RON_K(g, "attrition") == 0) return 0;
	int32_t base = 0x100;
	if (type_is_siege(*u.ptype)) {
		const int32_t s = RON_K(g, "siege_attrition");
		if (s > 99) return 0;
		base = 0x6400 / (100 - s);
	}
	int32_t v;
	if (!unit_is(g, u, kMilitiaClass)) {
		const Leader& me = g.leaders[size_t(u.who)];
		v = int32_t(float(base) * me.anti_att * 0.00390625f);  // MULSS, MULSS, CVTTSS2SI
		if (has_preq(g, u.who, kForaging1) && unit_idle(u)) return 0;
	} else {
		v = (base * 100) / (RON_K(g, "militia_attrition") + 100);
	}
	if (u.type == 0x3d || u.type == 0x3e || u.type == 400) v /= 2;  // merchants
	if (u.ptype->domain == kDomainAir) v /= 2;
	int32_t div = att;
	const int32_t gap = g.leaders[size_t(owner)].age - g.leaders[size_t(u.who)].age;
	if (gap >= 0) div = ((RON_K(g, "attrition_aged_up") * gap + 100) * att + 99) / 100;
	return v / div;
}

int32_t process_attrition_impl(Game& g, Unit& u);

int32_t process_attrition(Game& g, Unit& u) {
	const int32_t r = process_attrition_impl(g, u);
	if (trace_on() && (u.attrition != 0 || cell_owner(g, u.x, u.y) != u.who))
		std::fprintf(stderr, "att f%d %d:%d cell %d period %d masks %s%s dmg %d\n", g.frame, u.who, u.o,
		             cell_owner(g, u.x, u.y), u.attrition, (u.unit_masks & umask::kAttrition) ? "A" : "-",
		             (u.unit_masks & umask::kSevereAttrition) ? "S" : "-", u.damage);
	return r;
}

int32_t process_attrition_impl(Game& g, Unit& u) {
	// @5e11a0 (every 32 frames, on map): clears ATTRITION | SEVERE_ATTRITION and the period, then decides whether
	// the cell's owner makes this unit suffer and how often (u.attrition, in frames). The messages and the warning
	// sound are UI; their once-per-900-frames stamp is leader state.
	u.unit_masks &= ~(umask::kAttrition | umask::kSevereAttrition);
	u.attrition = 0;
	const int32_t me = u.who;
	const int32_t owner = cell_owner(g, u.x, u.y);
	const int32_t tx = tcoord_of(u.x), ty = tcoord_of(u.y);
	for (const Game::RevealPoint& p : g.attrition_free_points[size_t(me)])
		if (vector_dist(ty - p.ty, tx - p.tx) <= p.radius) return 0;
	Leader& L = g.leaders[size_t(me)];
	if (owner < 0) {
		// Neutral land: only with set_neutral_attrition, which becomes the period (not for ships). The original then
		// reads the flags of leaders[-1] (the memory before the array), which is taken here as not a live leader.
		if (L.neutral_attrition == 0) return 0;
		if (u.ptype->domain != kDomainSea) u.attrition = int16_t(L.neutral_attrition);
		return 0;
	}
	if (owner == me) return 0;
	const Leader& O = g.leaders[size_t(owner)];
	if (!(O.flags & leaderflag::kValid) || !(O.flags & leaderflag::kActive)) return 0;
	if (L.diplos(owner) == 2 && O.diplos(me) == 2) return 0;
	if (L.take_att_disabled != 0) return 0;
	// Logistics (card 9, CtW, not the team style 2): this leader's units are immune to attrition (@5e11a0).
	if (g.ctw && g.info.team_style != 2 && L.num_bonus_cards[9] != 0) return 0;
	if (O.give_att_disabled != 0) return 0;
	if (is_worker_type(u.type)) {
		// TODO(economy): a gathering worker is exempt only when its gather action's byte +0x25 is set (not traced);
		// workers are never exempt here.
	} else if (!unit_is_merchant(u) && !is_hero_unit(u) && !is_supply_unit(u)) {
		if (u.ptype->attack == 0) return 0;
		if (is_special_unit(u)) return 0;
		if (unit_is(g, u, kSpyClass)) return 0;
		if (is_caravan_unit(u)) return 0;
	}
	if (u.ptype->domain == kDomainSea) return 0;
	if (L.neutral_attrition != 0) return 0;
	// TODO(ctw): in Conquer the World outside team style 2, conquest bonus 9 exempts the unit.

	bool warn = false, grace = false, broke = false;
	int32_t period = 0;
	if (!is_enemy(g, me, owner) || (g.ctw && !war_allowed(g))) {
		// Not at war: a recently broken alliance (inside ally_to_war_grace) or a recent peace (ally_to_war_delay)
		// only warns; otherwise the peace attrition applies (and SEVERE_ATTRITION blocks supply).
		const int32_t delay = RON_K(g, "ally_to_war_delay");
		const int32_t b1 = L.broke_alliance[size_t(owner)], b2 = O.broke_alliance[size_t(me)];
		int32_t since = 0;
		if (b1 != 0 && (since = g.frame - b1) < delay) broke = true;
		else if (b2 != 0 && (since = g.frame - b2) < delay) broke = true;
		if (broke) grace = since < RON_K(g, "ally_to_war_grace");
		if (!grace) {
			const int32_t m1 = L.made_peace[size_t(owner)], m2 = O.made_peace[size_t(me)];
			if ((m1 != 0 && g.frame - m1 < delay) || (m2 != 0 && g.frame - m2 < delay)) grace = true;
		}
		warn = true;
		period = RON_K(g, "peace_attrition");
	} else if (g.info.team_style == 2) {
		// Assassin: a unit off its leader's target suffers the assassin attrition. TODO(assassin):
		// LeaderData::get_target; until it exists every enemy counts as the target (plain war attrition).
	}
	if (warn) {
		if (L.attrition_warn_stamp == 0 || g.frame - L.attrition_warn_stamp > 900)
			L.attrition_warn_stamp = (broke && grace) ? std::max(1, RON_K(g, "ally_to_war_grace") + g.frame - 900) : g.frame;
		if (!(L.treaty(owner) & 1)) leader_meet(g, me, owner);
		u.unit_masks |= umask::kAttrition | umask::kSevereAttrition;
		if (grace) return 0;
		u.attrition = int16_t(period);
		if (u.ptype->domain == kDomainAir) u.attrition = int16_t(int16_t(period) / 2);
	}
	if (u.ptype->domain == kDomainGround) {
		if (!(u.unit_masks & umask::kSevereAttrition) && is_supply_unit(u)) return 0;
		int32_t a = unit_get_attrition(g, u, owner);
		if (a != 0) {
			a = RON_K(g, "attrition") * a;
			a = (a + ((a >> 31) & 0xff)) >> 8;  // signed / 256
			if (a < 1) a = 1;
			if (u.attrition == 0 || a < u.attrition) u.attrition = int16_t(a);
			u.unit_masks |= umask::kAttrition;
			if (!(L.treaty(owner) & 1)) leader_meet(g, me, owner);
		}
	}
	return 0;
}

bool process_supply(Game& g, Unit& u) {
	// @5e0560: never while SEVERE_ATTRITION (peace attrition), never for supply units and militia; a supply unit in
	// range, or a supplying general (types 0x16b, 0x176, 0x16e) the leader owns.
	if (u.unit_masks & umask::kSevereAttrition) return false;
	if (is_supply_unit(u) || unit_is(g, u, kMilitiaClass)) return false;
	if (supplies_find_supply(g, u.x, u.y, u.who) >= 0) return true;
	for (int32_t t : {0x16b, 0x176, 0x16e})
		if (num_units(g, u.who, t) != 0 && has_general(g, u, 0, t) >= 0) return true;
	return false;
}

int32_t suffer_attrition(Game& g, Unit& u, bool show) {
	// @5e1a10: the first-hit stamp (its messages are UI), then take_damage: 1 hp for a single unit, else 6/16 (a
	// 3-member chain) or 16/n sixteenths, death 1, quiet, no attacker.
	Leader& L = g.leaders[size_t(u.who)];
	if (L.attrition_hit_stamp == 0) L.attrition_hit_stamp = g.frame;
	int32_t hp, frac;
	if (u.ptype->uber_size == 1) {
		hp = 1;
		frac = 0;
	} else {
		const int32_t n = unit_curr_uber_size(g, u);
		hp = 0;
		frac = n == 3 ? 6 : 16 / n;
	}
	const int32_t r = take_damage(g, u, hp, frac, 1, -1, true, 0x55555555u, -1, -1, false);
	(void)show;  // UnitOut::suffer_graphic_attrition (UI)
	return r;
}

// ---------------------------------------------------------------- healing

void unit_repair_damage(Game& g, Unit& u, int32_t hp, bool show, bool conquest) {
	// @60de10. The CtW heal cards scale the amount; `left` (amount minus the damage before the clamp) overflows down
	// the chain and regenerates dead members. The heal underlay (internal_random) is UI.
	(void)show;
	// In Conquer the World with healing cards (Logistics, LeaderData +0x68ff): hp = ((cards * 100 + ctw_heal_bonus) / 100) * hp.
	if (conquest && g.ctw) {
		const int32_t cards = g.leaders[size_t(u.who)].num_bonus_cards[9];
		if (cards != 0) hp = ((cards * 100 + RON_K(g, "ctw_heal_bonus")) / 100) * hp;
	}
	const int32_t left = hp - u.damage;
	if (u.damage != 0) {
		const int32_t h = obj_hits(g, u);
		if (h < u.damage) u.damage = h;
		u.damage = hp < u.damage ? u.damage - hp : 0;
		u.damage_frac = 0;
	}
	if (left <= 0) return;
	if (u.o_down >= 0) {
		Unit* d = g.unit(u.who, u.o_down);
		if (d && d->alive()) {
			unit_repair_damage(g, *d, left, show, false);
			return;
		}
	}
	if (!(unit_curr_uber_size(g, u) < u.ptype->uber_size)) return;
	if (!(u.inside_up >= 0 || !(u.unit_masks & umask::kInCombat))) return;
	// Regenerate the dead member kept at the chain tail (o_down): Unit::init on its slot, chain links restored, it
	// starts with hits / uber_size damage, the stance and unit_masks of u, and walks out to a free spot.
	if (u.o_down < 0) return;  // (an error report in the original)
	const int32_t who = u.who;
	const int32_t s = find_free(g, who, 0, 2000, u.o_down);
	if (s < 0) return;
	Unit* n = g.unit(who, s);
	if (!n) return;
	const int16_t up = n->o_up, down = n->o_down;
	unit_init(g, *n, who, u.type, s, u.x, u.y);  // one Guy::init_real game_random draw per guy
	n->o_down = down;
	n->o_up = up;
	n->damage = obj_hits(g, u) / u.ptype->uber_size;
	n->stance = u.stance;
	n->unit_masks = u.unit_masks;
	if (u.inside_up < 0) {
		int32_t sx = n->x, sy = n->y;
		find_nearby_spot(g, *n->ptype, n->x, n->y, &sx, &sy, 0, -1, 0, 0x55555555u, kFilterNotMe, n->o, who, 0, 0,
		                 -1, 0, -1);
		add_move_order(g, *n, sx, sy, 1, false, kQueueNew, false, -1, -1);
	}
	if (u.inside_up >= 0) unit_insert_inside(g, *n, u.inside_up, u.inside_up_who);  // a garrisoned chain: the same container
	// The group the chain belongs to takes the chain again (Group::kill then Group::add of u); the console's
	// selection and hotkey groups do the same (UI, not simulated).
	if (u.group >= 0 && u.group < int32_t(g.groups.g.size())) {
		Group& gr = g.groups.g[size_t(u.group)];
		group_kill(g, gr, u.o, who, 0, 0);
		group_add(g, gr, u.o, who, 0, 0);
	}
	unit_repair_damage(g, *n, left, show, false);
}

void process_healing(Game& g, Unit& u) {
	// @5e0670: captains that are not aircraft and have damage somewhere in the chain.
	if (u.o_up >= 0) return;
	const int32_t dom = u.ptype->domain;
	if (dom == kDomainAir) return;
	if (!unit_has_damage(g, u, true)) return;
	const int32_t who = u.who;
	const Leader& L = g.leaders[size_t(who)];
	const int32_t rate0 = KA(g, "unit_heal_rate", 0);
	if (!u.on_map()) {
		// Garrisoned (combat-spec 7.2): heal at unit_heal_rate[heal level] (Red Fort faster, ships in a dock 2/3) in a
		// building that trains this unit, a city or fort, or a tower.
		int32_t xw;
		const int32_t x = unit_get_inside(g, u, &xw);
		const Object* c = x >= 0 ? g.object(xw, x) : nullptr;
		if (!c || !c->alive() || !c->ptype) return;
		int32_t rate = KA(g, "unit_heal_rate", get_heal_level(g, who));
		if (type_is(*c->ptype, kRedFort) || has_wonder_bits(g, who, kRedFort) == 2)
			rate = rate * 100 / (RON_K(g, "red_fort_heal") + 100);
		if (dom == kDomainSea && type_is(*c->ptype, 0x1b0)) rate = (2 * rate + 2) / 3;  // a dock
		if (rate < 1) rate = 1;
		if (!on_tick(g, u, rate)) return;
		const int32_t where = u.ptype->where;
		if ((where >= 0 && type_is(*c->ptype, where)) || (c->flags & objflag::kCity) || type_is(*c->ptype, kFortType) ||
		    type_is(*c->ptype, kTowerType))
			heal_one(g, u, rate, true, true);
		return;
	}
	if (dom == kDomainSea) {
		// Ships: Citrus (unless the type opts out), and aircraft carriers themselves.
		if (!(u.ptype->unit_flags & 0x10) && has_rare(L, kRareCitrus) && on_tick(g, u, rate0)) heal_one(g, u, rate0, true, false);
		if (!unit_is(g, u, kAircraftCarrier, true)) return;
		if (!on_tick(g, u, rate0)) return;
		heal_one(g, u, rate0, true, false);
		return;
	}
	if (is_supply_unit(u)) return;
	int32_t rate = RON_K(g, "supply_heal_rate");
	// Antipater-type generals (0x169 / 0x170).
	if (num_units(g, who, 0x169) != 0 || num_units(g, who, 0x170) != 0) {
		const int32_t r = RON_K(g, "antipater_heal_rate");
		if (on_tick(g, u, r) && (has_general(g, u, 0, 0x169) >= 0 || has_general(g, u, 0, 0x170) >= 0))
			heal_one(g, u, r, true, true);
		rate = RON_K(g, "supply_heal_rate");
	}
	// TODO(ctw): the Conquer the World hero heals at unit_heal_rate[0].
	// Iroquois: ground units that are not moving (nor blowing their cover) heal in allied territory.
	if (has_tribe_bonus(g, who, kTbIroquois) && !(u.unit_masks & umask::kBlowCover) && !unit_is_moving(u) &&
	    dom == kDomainGround) {
		const int32_t r = KA(g, "iroquois_heal_rate", get_heal_level(g, who));
		if (on_tick(g, u, r)) {
			const int32_t c = cell_owner(g, u.x, u.y);
			if (c >= 0 && is_ally(g, who, c)) heal_one(g, u, r, true, true);
		}
	}
	// Government heroes that heal (0x161 allied land, 0x163 own or neutral land, 0x165 unless suffering
	// unsupplied attrition), every french_supply_heal_rate frames.
	const int32_t fr = RON_K(g, "french_supply_heal_rate");
	if (on_tick(g, u, fr)) {
		if (num_units(g, who, 0x161) != 0) {
			const int32_t c = cell_owner(g, u.x, u.y);
			if (c >= 0 && is_ally(g, who, c) && has_general(g, u, 0, 0x161) >= 0) heal_one(g, u, fr, true, true);
		}
		if (num_units(g, who, 0x163) != 0) {
			const int32_t c = cell_owner(g, u.x, u.y);
			if ((c < 0 || is_ally(g, who, c)) && has_general(g, u, 0, 0x163) >= 0) heal_one(g, u, fr, true, true);
		}
		if (num_units(g, who, 0x165) != 0) {
			const bool unsupplied = (u.unit_masks & umask::kAttrition) && !(u.unit_masks2 & umask2::kAttritionSafe) &&
			                        !is_supply_unit(u);
			if (!unsupplied && has_general(g, u, 0, 0x165) >= 0) heal_one(g, u, fr, true, true);
		}
	}
	// Supply wagons: supply_heal_rate (+ the French bonus; Versailles averages in its own rate).
	if (has_tribe_bonus(g, who, kTbFrench)) rate += fr;
	if (has_wonder(g, who, kVersailles)) {
		const int32_t v = RON_K(g, "versailles_supply_heal_rate");
		if (v != 0) rate = rate == 0 ? v : (v + rate) / 4;
	}
	if (on_tick(g, u, rate) && supplies_find_supply(g, u.x, u.y, who) >= 0) heal_one(g, u, rate, true, true);
	// Civilians (workers, caravans, merchants, fishermen) out of combat, in allied territory or at sea.
	const int32_t cr = RON_K(g, "civilian_heal_rate");
	if (on_tick(g, u, cr) && !(u.unit_masks2 & umask2::kUnderAttack)) {
		if (!is_worker_type(u.type) && !is_caravan_unit(u) && !unit_is_merchant(u) && u.type != kFishermen) return;
		const int32_t c = cell_owner(g, u.x, u.y);
		if (dom == kDomainSea || (c >= 0 && is_ally(g, who, c))) {
			unit_repair_damage(g, u, 1, false, true);
			u.healing = int16_t(std::max<int32_t>(cr, u.healing));
		}
	}
}

}  // namespace ron::sim
