// Leader-dependent unit and building stats (docs/combat-spec.md section 5.1): ObjectData / UnitData / BuildData ::attack,
// armor, hits, max_range and min_range, Unit::update_hits / update_armor (captain computes, copied down the o_down
// chain), the upgrade counters and BuildTypeData::get_building_range. Hero (general) terms go through sim/heroes.hpp
// (has_general / find_hero); the garrison terms count what is inside (sim/garrison.hpp).
#include <algorithm>

#include "sim/combat_internal.hpp"
#include "sim/fog.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/production.hpp"

namespace ron::sim {

using namespace combat_detail;

namespace {

// Tribe bonuses (has_tribe_bonus index).
constexpr int32_t kTbNubian = 4, kTbRoman = 6, kTbTurk = 8, kTbBritish = 11, kTbIroquois = 18, kTbDutch = 22;
// Wonders (TypeIndex).
constexpr int32_t kWTerraCotta = 0x211, kWColosseum = 0x212, kWTikal = 0x214, kWLiberty = 0x219, kWEiffel = 0x21c,
                  kWSpaceProgram = 0x21e;
// Generals and heroes (TypeIndex).
constexpr int32_t kTheMonarch = 0x162, kTheCitizen = 0x164, kTheCeo = 0x165, kAlexander = 0x166, kNapoleon = 0x167,
                  kParmenio = 0x168, kAntipater = 0x169, kPtolemy = 0x16a, kDarius = 0x16b, kMemnon = 0x16c,
                  kWellington = 0x170, kDjezzar = 0x172, kBlucher = 0x175, kSpitamenes = 0x16d, kTheSenator = 0x161;
// Rare goods (TypeIndex).
constexpr int32_t kRareCopper = 24, kRareObsidian = 32, kRareCattle = 37, kRareRum = 43, kRareBananas = 45;
// Object masks, type classes and flags read here.
constexpr int32_t kMaskFootArcher = 0x400, kMaskHorseArcher = 0x4000;
constexpr int32_t kWhereStable = 0x1ac, kWhereBarracks = 0x1ab, kWhereFactoryA = 0x1ae, kWhereFactoryB = 0x1af;
constexpr uint32_t kUtGovHero = 0x4000000;  // is_gov_hero (Type unit_flags)
constexpr int32_t kTower = 0x1b7, kFort = 0x1bb;

// A rules constant array entry: element i of the array that starts at `name`.
int32_t KA(const Game& g, const char* name, int32_t i) {
	const int off = constant_offset(name);
	return off < 0 ? 0 : g.rules->constants.at(uint32_t(off + 4 * i));
}

int32_t pct(int32_t k, int32_t v) { return (k + 100) * v / 100; }

bool valid_who(int32_t who) { return who >= 0 && who < kNumLeaders; }
const Leader& LD(const Game& g, int32_t who) { return g.leaders[size_t(who)]; }

// "owns(t)": LeaderData.num_units[t - 50] != 0.
bool owns(const Game& g, int32_t who, int32_t t) {
	return valid_who(who) && t >= kUnits && t < kEndUnits && LD(g, who).num_units[size_t(t - kUnits)] != 0;
}
// "general(t)": owns(t) and a hero of type t in range (ObjectData::has_general).
bool general(const Game& g, const Unit& u, int32_t t) { return owns(g, u.who, t) && has_general(g, u, 0, t) >= 0; }

// HeroesData::find_hero(x, y, who, mask 0, type, slack) @73a1b0 around an object (sim/heroes.hpp).
int32_t find_hero_near(const Game& g, const Object& ob, int32_t type, int32_t radius) {
	return find_hero(g, ob.x, ob.y, ob.who, 0, type, radius);
}

// The leader's general type: tribe_can_type(types[0x36]) ? 0x36 : its tribe substitute (LeaderData::get_graft).
int32_t leader_general_type(const Game& g, int32_t who) { return get_graft(g, who, 0x36); }
bool general_garrisoned(const Game& g, const Build& b) {
	return count_inside(g, b, kCountType, leader_general_type(g, b.who)) != 0;
}

bool is_caravan_obj(const Object& ob) { return ob.is_unit && ob.ptype && (ob.ptype->unit_flags2 & 8) != 0; }
bool is_supply_obj(const Object& ob) { return ob.is_unit && ob.ptype && (ob.ptype->unit_flags2 & 0x40) != 0; }
bool is_hero_obj(const Object& ob) { return ob.ptype && (ob.ptype->unit_flags2 & 0x20) != 0; }
bool is_active_obj(const Object& ob) { return (ob.flags & objflag::kActive) != 0; }
bool is_merchant_type(int32_t t) { return t == 0x3d || t == 0x3e || t == 400; }

// "dutch set": types 0x3d, 0x3e, 400, caravans and supply units, minus government heroes (patch version >= 9).
bool in_dutch_set(const Object& ob) {
	if (!ob.ptype || !ob.is_unit) return false;
	if (ob.ptype->unit_flags & kUtGovHero) return false;
	return is_merchant_type(ob.type) || is_caravan_obj(ob) || is_supply_obj(ob);
}

bool dutch_applies(const Game& g, const Object& ob, int32_t k) {
	return valid_who(ob.who) && has_tribe_bonus(g, ob.who, kTbDutch) && k != 0 && is_active_obj(ob) && in_dutch_set(ob);
}

// ObjectData::armor @647db0.
int32_t object_armor(const Game& g, const Object& ob) {
	int32_t a = ob.ptype->armor;
	const int32_t k = RON_K(g, "dutch_attack_bonus");
	if (dutch_applies(g, ob, k)) a += LD(g, ob.who).age * k;
	return a;
}

// Object::update_hits @647010: the base hits, the militia line replacing citizens once its tech is known.
int32_t object_update_hits(const Game& g, const Object& ob) {
	int32_t t = ob.type;
	if ((t == 0x32 || t == 0x33) && valid_who(ob.who)) {
		const Leader& l = LD(g, ob.who);
		if (l.has_tech_bit(68)) t = 68;
		else if (l.has_tech_bit(67)) t = 67;
		else if (l.has_tech_bit(66)) t = 66;
	}
	return g.type(t).hits;
}

const Unit* captain_of(const Game& g, const Unit& u) {
	const Unit* p = &u;
	while (p->o_up >= 0) {
		const Unit* n = g.unit(p->who, p->o_up);
		if (!n) break;
		p = n;
	}
	return p;
}

// BuildData's three kinds of "city building" (flags & 0x20 with a city) used by attack and max_range.
bool is_city_building(const Build& b) { return (b.flags & objflag::kCity) && b.city >= 0; }
bool is_tower_or_fort(const Type& t) { return type_is(t, kTower) || type_is(t, kFort); }

}  // namespace

// ---------------------------------------------------------------- upgrade counters

// LeaderData::get_general_upgrade @6e0830: GENERALS_UPGRADE_1..3 (0x305..0x307).
int32_t get_general_upgrade(const Game& g, int32_t who) {
	int32_t n = 0;
	for (int32_t t = 0x305; t <= 0x307; t++) n += has_preq(g, who, t) ? 1 : 0;
	return n;
}

// LeaderData::get_spy_upgrade @6e1090: SPIES_UPGRADE_1..3 (0x308..0x30a).
int32_t get_spy_upgrade(const Game& g, int32_t who) {
	int32_t n = 0;
	for (int32_t t = 0x308; t <= 0x30a; t++) n += has_preq(g, who, t) ? 1 : 0;
	return n;
}

// get_temple_range: temple_upgrade_range[k], k = 3 / 2 / 1 / 0 for TEMPLERANGE4 / 3 / 2 (0x2d0 / 0x2cf / 0x2ce) / none.
int32_t get_temple_range(const Game& g, int32_t who) {
	const int32_t k = has_preq(g, who, 0x2d0) ? 3 : has_preq(g, who, 0x2cf) ? 2 : has_preq(g, who, 0x2ce) ? 1 : 0;
	return KA(g, "temple_upgrade_range", k);
}

// ---------------------------------------------------------------- attack

// ObjectData::attack @6469f0 (type.attack is already x10).
int32_t object_attack(const Game& g, const Object& ob) {
	if (!ob.ptype) return 0;
	int32_t a = ob.ptype->attack;
	if (!valid_who(ob.who)) return a;
	if (is_caravan_obj(ob) && has_preq(g, ob.who, 0x2ba)) a = RON_K(g, "caravan_attack_bonus");
	const int32_t k = RON_K(g, "dutch_attack_bonus");
	if (dutch_applies(g, ob, k)) a += LD(g, ob.who).age * k * 10;
	return a;
}

namespace {

// UnitData::attack @6103c0.
int32_t unit_attack(const Game& g, const Unit& u) {
	int32_t a = object_attack(g, u);
	if (a == 0) return 0;
	if (!valid_who(u.who)) return a;
	const Type& t = *u.ptype;
	const int32_t who = u.who;
	if (has_wonder(g, who, kWTerraCotta)) a += RON_K(g, "terra_cotta_attack") * 10;
	if (has_general(g, u, 0, kTheCitizen) >= 0) a += RON_K(g, "thecitizen_attack_bonus") * 10;  // (no owns test)
	if ((t.obj_masks & kMaskHorseArcher) && has_rare(g, who, kRareObsidian)) a += RON_K(g, "obsidian_archers_attack") * 10;
	if ((t.obj_masks & kMaskFootArcher) && has_rare(g, who, kRareObsidian)) a += RON_K(g, "obsidian_archers_attack") * 10;
	if (t.domain == 2 && has_wonder(g, who, kWSpaceProgram)) a = pct(RON_K(g, "space_air_attack"), a);
	const bool siege = type_is_siege(t);
	if (general(g, u, kAlexander)) {
		a += RON_K(g, "alexander_attack") * 10;
		if (type_is(t, 0x84)) a += RON_K(g, "alexander_heavy_inf_attack") * 10;
	}
	if (siege && general(g, u, kNapoleon)) a += RON_K(g, "napoleon_siege_attack") * 10;
	if (type_is(t, 0xe3) && general(g, u, kParmenio)) a += RON_K(g, "parmenio_heavy_cav_attack") * 10;
	if (type_is(t, 0x109) && general(g, u, kPtolemy)) a += RON_K(g, "ptolemy_siege_attack") * 10;
	if (type_is(t, 0xbb) && general(g, u, kSpitamenes)) a += RON_K(g, "spitamenes_horse_archer_attack") * 10;
	if (t.where == kWhereStable && general(g, u, kBlucher)) a += RON_K(g, "blucher_cav_attack") * 10;
	if (general(g, u, kDjezzar)) a += RON_K(g, "djezzar_attack") * 10;
	if (general(g, u, kMemnon) && type_is(t, 0x18a)) a += RON_K(g, "memnon_merc_attack") * 10;
	if (t.where == kWhereStable && general(g, u, kTheMonarch)) a += RON_K(g, "themonarch_cav_attack") * 10;
	return a;
}

// BuildData::attack @62e610.
int32_t build_attack(const Game& g, const Build& b) {
	int32_t a = object_attack(g, b);
	if (!valid_who(b.who)) return a;
	if (type_is(*b.ptype, kTower) || type_is(*b.ptype, kFort) || is_city_building(b)) {
		if (general_garrisoned(g, b)) a += RON_K(g, "general_building_attack") * 10;
		if (has_rare(g, b.who, kRareObsidian)) a += RON_K(g, "obsidian_archers_attack") * 10;
		if (owns(g, b.who, kAntipater) && count_inside(g, b, kCountStrictType, kAntipater) != 0)
			a += RON_K(g, "antipater_garrison_attack_bonus") * 10;
	}
	return a;
}

}  // namespace

int32_t obj_attack(const Game& g, const Object& ob) {
	if (!ob.ptype) return 0;
	return ob.is_unit ? unit_attack(g, static_cast<const Unit&>(ob)) : build_attack(g, static_cast<const Build&>(ob));
}

// ---------------------------------------------------------------- armor

// Unit::update_armor @6054c0: the captain's myarmor, copied down the o_down chain.
void unit_update_armor(Game& g, Unit& u0) {
	Unit& u = *const_cast<Unit*>(captain_of(g, u0));
	int32_t a = object_armor(g, u);
	if ((u.type == 0x32 || u.type == 0x33) && valid_who(u.who) && has_rare(g, u.who, kRareCattle))
		a += RON_K(g, "cattle_citizen_armor");
	u.myarmor = int16_t(a);
	for (int32_t d = u.o_down; d >= 0;) {
		Unit* f = g.unit(u.who, d);
		if (!f) break;
		f->myarmor = u.myarmor;
		d = f->o_down;
	}
}

namespace {

// UnitData::armor @610160.
int32_t unit_armor(const Game& g, const Unit& u) {
	int32_t a = u.myarmor;
	if (!valid_who(u.who)) return a;
	const Type& t = *u.ptype;
	const int32_t who = u.who;
	int32_t h;
	if (is_active_obj(u) && is_hero_obj(u)) h = u.o;
	else h = find_hero_near(g, u, -1, 0);  // radius (x_size + y_size) * 96 only for buildings; a unit is not one
	if (h >= 0) {
		a += RON_K(g, "general_rally_armor") * (get_general_upgrade(g, who) + 1);
		if (owns(g, who, kPtolemy) && general(g, u, kPtolemy)) a += RON_K(g, "ptolemy_armor");
		if (owns(g, who, kDarius) && general(g, u, kDarius)) a += RON_K(g, "darius_armor");
		if (owns(g, who, kWellington) && general(g, u, kWellington)) a += RON_K(g, "wellington_defense");
		if (owns(g, who, kBlucher) && t.where == kWhereStable && general(g, u, kBlucher))
			a += RON_K(g, "blucher_cav_armor");
		if (owns(g, who, kMemnon) && type_is(t, 0x18a) && general(g, u, kMemnon)) a += RON_K(g, "memnon_merc_armor");
		if (has_general(g, u, 0, kTheCitizen) >= 0) a += RON_K(g, "thecitizen_armor_bonus");  // (no owns tests)
		if (t.where == kWhereStable && has_general(g, u, 0, kTheMonarch) >= 0) a += RON_K(g, "themonarch_cav_armor");
	}
	return a;
}

// WallData::armor @63fa60.
int32_t build_armor(const Game& g, const Build& b) {
	int32_t a = object_armor(g, b);
	if (valid_who(b.who)) {
		const bool active = is_active_obj(b);
		bool senator = active && is_hero_obj(b) && type_is(*b.ptype, kTheSenator);
		if (!senator && owns(g, b.who, kTheSenator))
			senator = find_hero_near(g, b, kTheSenator, (b.ptype->x_size + b.ptype->y_size) * 96) >= 0;
		if (senator) a += RON_K(g, "thesenator_build_armor");
	}
	if (!is_active_obj(b)) a /= 2;
	return a;
}

}  // namespace

int32_t obj_armor(const Game& g, const Object& ob) {
	if (!ob.ptype) return 0;
	return ob.is_unit ? unit_armor(g, static_cast<const Unit&>(ob)) : build_armor(g, static_cast<const Build&>(ob));
}

// ---------------------------------------------------------------- hits

// Unit::update_hits(p) @60e930: a follower defers to its captain; the captain's value is copied down the chain.
void unit_update_hits(Game& g, Unit& u0) {
	Unit& u = *const_cast<Unit*>(captain_of(g, u0));
	const Type& t = *u.ptype;
	const int32_t who = u.who;
	int32_t h = object_update_hits(g, u);
	if (valid_who(who)) {
		if (u.unit_masks2 & umask2::kMarineTransport) h += RON_K(g, "americans_marine_hp_bonus");
		if (has_rare(g, who, kRareCopper) && (t.domain == 1 || t.where == kWhereFactoryA || t.where == kWhereFactoryB))
			h = pct(RON_K(g, "copper_factory_hp"), h);
		if (has_rare(g, who, kRareBananas)) h = pct(RON_K(g, "bananas_hp_bonus"), h);
		const int32_t iro = RON_K(g, "iroquois_extra_hits");
		if (has_tribe_bonus(g, who, kTbIroquois) && iro != 0 && t.where == kWhereBarracks &&
		    (t.attack != 0 || type_is(t, 0x45)))
			h = pct(iro, h);
		const int32_t dutch = RON_K(g, "dutch_hp_bonus");
		if (has_tribe_bonus(g, who, kTbDutch) && dutch != 0 && in_dutch_set(u)) h += LD(g, who).age * dutch;
		if (type_is(t, 0x3a)) h = KA(g, "spy_upgrade_hp", std::clamp(get_spy_upgrade(g, who), 0, 3));
		if (is_hero_obj(u)) {
			const int32_t gu = get_general_upgrade(g, who);
			h += gu * gu * h / 2;
		}
		const int32_t nub = RON_K(g, "nubian_hit_points");
		if ((is_merchant_type(u.type) || is_caravan_obj(u)) && has_tribe_bonus(g, who, kTbNubian) && nub != 0)
			h = pct(nub, h);
		if (is_supply_obj(u)) h += KA(g, "supply_hp_upgrade", get_supply_upgrade(g, who));
	}
	u.myhits = h;
	for (int32_t d = u.o_down; d >= 0;) {
		Unit* f = g.unit(u.who, d);
		if (!f) break;
		f->myhits = u.myhits;
		d = f->o_down;
	}
}

// UnitData::hits @610890 = myhits; BuildData::hits(0) @62e740 = construct_hits, scaled while an active building's queue
// head is one of the two scaling jobs.
int32_t obj_hits(const Game& g, const Object& ob) {
	if (ob.is_unit) return ob.myhits;
	const Build& b = static_cast<const Build&>(ob);
	int32_t h = b.construct_hits;
	if ((b.flags & objflag::kActive) && b.queued > 0 && !b.build_queue.queue.empty()) {
		const QueueItem& head = b.build_queue.queue[0];
		if (head.type == ptype::kDisband || head.type == ptype::kDepopulate) {  // 0x29a / 0x286
			const int32_t T = train_time(g, b.o, b.who, head.type);
			const int32_t j = head.job_counter;
			if (T > 0) {
				const float v = ((float)(T - j) * (float)h) / (float)T;
				h = std::max(1, (int32_t)v);
			}
		}
	}
	return h;
}

// ---------------------------------------------------------------- range

namespace {

// UnitData::max_range @60fe90.
int32_t unit_max_range(const Game& g, const Unit& u) {
	const Type& t = *u.ptype;
	int32_t r = raw_max_range(g, t);
	if (r == 0) return 0;
	if (!valid_who(u.who)) return r;
	const int32_t who = u.who;
	const bool siege = type_is_siege(t);
	if (siege) {
		if (has_tribe_bonus(g, who, kTbTurk)) r += RON_K(g, "turk_siege_range");
		if (has_wonder(g, who, kWLiberty)) r += RON_K(g, "liberty_siege_range");
		if (has_wonder(g, who, kWEiffel)) r += RON_K(g, "eiffel_siege_range");
	} else if (owns(g, who, kTheCeo) && r != 0 && general(g, u, kTheCeo)) {
		r += RON_K(g, "theceo_unit_range");
	}
	if (owns(g, who, kPtolemy) && general(g, u, kPtolemy)) r += RON_K(g, "ptolemy_range_bonus");
	if (owns(g, who, kNapoleon) && siege && general(g, u, kNapoleon)) r += RON_K(g, "napoleon_siege_range");
	if (t.domain == 1 && has_rare(g, who, kRareRum)) r += RON_K(g, "rum_naval_range");
	if ((t.obj_masks & (kMaskFootArcher | kMaskHorseArcher)) && has_rare(g, who, kRareObsidian))
		r += RON_K(g, "obsidian_archers_range");
	return r;
}

}  // namespace

// BuildTypeData::get_building_range(who) @639990.
int32_t building_range(const Game& g, const Type& t, int32_t who) {
	int32_t r = t.max_range;
	if (!valid_who(who)) return r;
	const bool tower = type_is(t, kTower), fort = type_is(t, kFort);
	if (tower || fort) {
		if (has_wonder(g, who, kWColosseum)) r += RON_K(g, "colosseum_fort_range");
		if (has_tribe_bonus(g, who, kTbBritish)) r += RON_K(g, "british_tower_range");
	}
	if (fort) {
		if (has_tribe_bonus(g, who, kTbRoman)) r += RON_K(g, "roman_fort_range");
		const int32_t k = has_preq(g, who, 0x2d6) ? 3 : has_preq(g, who, 0x2d5) ? 2 : has_preq(g, who, 0x2d4) ? 1 : 0;
		r += KA(g, "fort_upgrade_range", k);
	} else if (tower) {
		if (has_tribe_bonus(g, who, kTbRoman)) r += RON_K(g, "roman_fort_range");
		const Leader& l = LD(g, who);
		const int32_t k = l.has_tech_bit(68) ? 3 : l.has_tech_bit(67) ? 2 : l.has_tech_bit(66) ? 1 : 0;
		r += KA(g, "tower_fort_range", k);
	}
	return r;
}

namespace {

// BuildData::max_range @62e470.
int32_t build_max_range(const Game& g, const Build& b) {
	int32_t r = building_range(g, *b.ptype, b.who);
	if (!valid_who(b.who)) return r;
	// Towers and forts: a garrisoned general. City buildings: the temple range, then the general again (the original's
	// two blocks; a building that is both gets the general term twice).
	if (is_tower_or_fort(*b.ptype) && general_garrisoned(g, b)) r += RON_K(g, "general_building_range");
	if (is_city_building(b)) {
		if (size_t(b.city) < g.cities[size_t(b.who)].size() &&
		    (g.cities[size_t(b.who)][size_t(b.city)].city_flags & cityflag::kHasTemple)) {
			int32_t v = get_temple_range(g, b.who);
			if (has_wonder(g, b.who, kWTikal)) v = ((RON_K(g, "tikal_temple_hp") + 100) * v + 99) / 100;
			r += v;
		}
		if (general_garrisoned(g, b)) r += RON_K(g, "general_building_range");
	}
	return r;
}

}  // namespace

int32_t obj_max_range(const Game& g, const Object& ob) {
	if (!ob.ptype) return 0;
	return ob.is_unit ? unit_max_range(g, static_cast<const Unit&>(ob)) : build_max_range(g, static_cast<const Build&>(ob));
}

// UnitData::min_range @6100b0 / ObjectData::min_range @646c20 = the type's (tiles).
int32_t obj_min_range(const Game& g, const Object& ob) {
	(void)g;
	return ob.ptype ? ob.ptype->min_range : 0;
}

}  // namespace ron::sim
