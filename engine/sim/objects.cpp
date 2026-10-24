// Objects: slot allocation, creation and death, the per-wcoord object lists, and the per-frame object passes
// (docs/frame-spec.md sections 6-9; Objects::find_free / init_unit, Unit::init, Object::add_to_world /
// remove_from_world, Objects::process_all / inc_time, Unit::process).
#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/animals.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/heroes.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/search.hpp"
#include "sim/passability.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- access

Unit* Game::unit(int32_t who, int32_t o) {
	if (who < 0 || who >= kNumLeaders || o < 0) return nullptr;
	auto& v = objects[size_t(who)].units;
	return size_t(o) < v.size() ? v[size_t(o)].get() : nullptr;
}
const Unit* Game::unit(int32_t who, int32_t o) const { return const_cast<Game*>(this)->unit(who, o); }

Build* Game::build(int32_t who, int32_t o) {
	if (who < 0 || who >= kNumLeaders || o < kBuildBase) return nullptr;
	auto& v = objects[size_t(who)].builds;
	return size_t(o - kBuildBase) < v.size() ? v[size_t(o - kBuildBase)].get() : nullptr;
}
const Build* Game::build(int32_t who, int32_t o) const { return const_cast<Game*>(this)->build(who, o); }

Object* Game::object(int32_t who, int32_t o) {
	if (o < 0) return nullptr;
	if (o < kUnitLimit) return unit(who, o);
	return build(who, o);
}
const Object* Game::object(int32_t who, int32_t o) const { return const_cast<Game*>(this)->object(who, o); }

Unit* Game::alive_unit(int32_t who, int32_t o) {
	Unit* u = unit(who, o);
	return u && u->alive() ? u : nullptr;
}

int32_t Game::rnd(int32_t n) { return n <= 1 ? 0 : rng.get(0, 0xffff) % n; }

// ---------------------------------------------------------------- type predicates

bool unit_is(const Game& g, const Unit& u, int32_t t, bool strict) {
	(void)g;
	return u.ptype && type_is(*u.ptype, t, strict);
}

bool unit_can_transport(const Unit& u) {
	return ((u.unit_masks & umask::kCanTransport) && !(u.unit_masks2 & umask2::kDisableTransport)) ||
	       (u.ptype && (u.ptype->unit_flags & 0x10));
}

bool unit_is_air(const Unit& u) { return u.ptype && u.ptype->domain == 2; }
bool unit_is_helicopter(const Unit& u) { return u.ptype && (u.ptype->unit_flags & 0x20); }

// Modern infantry: the types the original tests with is(MODERN_INFANTRY) (TODO: confirm the type index; the movement
// code uses it for the 5/4 speed bonus and the guy-0 snap). Until traced, none.
bool unit_is_modern_infantry(const Game& g, const Unit& u) {
	(void)g;
	(void)u;
	return false;
}

int32_t unit_invalid_loc(const Game& g, const Unit& u, int32_t tx, int32_t ty, bool wlevel, bool fog, bool bldg,
                         bool transp, bool p7) {
	Mover m = make_mover(*u.ptype, u.unit_masks, u.unit_masks2, u.who, u.x, u.y);
	m.path_transport = !u.path.empty() && (u.path.back().flags & pathflag::kTransport);
	m.fog_limited = (g.leaders[size_t(u.who)].flags & leaderflag::kHuman) != 0;
	LocEnv env;
	env.ctx = &g;
	env.was_seen = [](const void* ctx, int32_t fx, int32_t fy, int32_t who) {
		return was_seen(*static_cast<const Game*>(ctx), fx, fy, who);
	};
	return invalid_loc(g.world, m, tx, ty, {wlevel, fog, bldg, transp, p7}, env);
}

int32_t needs_transport(const Game& g, int32_t tx1, int32_t ty1, int32_t tx2, int32_t ty2) {
	if (tx1 == tx2 && ty1 == ty2) return 0;
	const World& w = g.world;
	if (!w.in_t(tx1, ty1) || !w.in_t(tx2, ty2)) return 0;
	const bool water1 = w.is_tocean(tx1, ty1), water2 = w.is_tocean(tx2, ty2);
	if (water1 == water2) return 0;
	return water1 ? 1 : 2;
}

// ---------------------------------------------------------------- world links

void add_to_world(Game& g, Object& ob) {
	if (ob.who >= 9) return;  // gaia is never linked
	World& w = g.world;
	const int32_t wx = wcoord_of(ob.x), wy = wcoord_of(ob.y);
	if (!w.in_w(wx, wy)) return;
	WData& d = w.w(wx, wy);
	ob.up = -1;
	if (d.down >= 0) {
		Object* head = g.object(d.down_who, d.down);
		if (head) {
			head->up = ob.o;
			head->up_who = ob.who;
		}
	}
	ob.down = d.down;
	ob.down_who = d.down_who;
	d.down = ob.o;
	d.down_who = ob.who;
	ob.linked = true;
	if (!ob.is_unit) return;
	Unit& u = static_cast<Unit&>(ob);
	// Footprints of every guy [0, guy_mark) (non-air, new_block_radius != 0), filtered by the guy tcoord's region.
	const int32_t r = u.ptype->new_block_radius;
	if (!u.on_map() || u.ptype->domain == 2 || r == 0) return;
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) {
		const Guy& gy = u.guys[size_t(i)];
		const int32_t tx = tcoord_of(gy.x), ty = tcoord_of(gy.y);
		const int32_t region = w.in_t(tx, ty) ? w.get_tregion(tx, ty) : -1;
		w.stamp_footprint(ucoord_of(gy.x), ucoord_of(gy.y), r, region, true);
	}
}

void remove_from_world(Game& g, Object& ob) {
	if (ob.who >= 9) return;
	World& w = g.world;
	if (ob.is_unit) {
		Unit& u = static_cast<Unit&>(ob);
		const int32_t r = u.ptype ? u.ptype->new_block_radius : 0;
		if (u.on_map() && u.ptype && u.ptype->domain != 2 && r != 0) {
			for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) {
				const Guy& gy = u.guys[size_t(i)];
				const int32_t tx = tcoord_of(gy.x), ty = tcoord_of(gy.y);
				const int32_t region = w.in_t(tx, ty) ? w.get_tregion(tx, ty) : -1;
				w.stamp_footprint(ucoord_of(gy.x), ucoord_of(gy.y), r, region, false);
			}
		}
	}
	if (!ob.linked) return;
	const int32_t wx = wcoord_of(ob.x), wy = wcoord_of(ob.y);
	if (ob.up < 0) {
		if (w.in_w(wx, wy)) {
			WData& d = w.w(wx, wy);
			d.down = ob.down;
			d.down_who = ob.down_who;
		}
		if (ob.down >= 0) {
			if (Object* n = g.object(ob.down_who, ob.down)) n->up = -1;
		}
	} else {
		Object* p = g.object(ob.up_who, ob.up);
		if (p) {
			p->down = ob.down;
			p->down_who = ob.down_who;
		}
		if (ob.down >= 0) {
			if (Object* n = g.object(ob.down_who, ob.down)) {
				n->up = ob.up;
				n->up_who = ob.up_who;
			}
		}
	}
	ob.up = -1;
	ob.down = -1;
	ob.linked = false;
}

// ---------------------------------------------------------------- slots

int32_t find_free(Game& g, int32_t who, int32_t base, int32_t limit, int32_t forced) {
	ObjectList& L = g.objects[size_t(who)];
	const bool units = base == 0;
	int32_t& mark = units ? L.unit_mark : L.build_mark;
	auto ensure = [&](int32_t o) {
		if (units) {
			if (L.units.size() <= size_t(o)) L.units.resize(size_t(o) + 1);
			if (!L.units[size_t(o)]) {
				L.units[size_t(o)] = std::make_unique<Unit>();
				L.units[size_t(o)]->who = int8_t(who);
				L.units[size_t(o)]->o = int16_t(o);
			}
		} else {
			const size_t i = size_t(o - kBuildBase);
			if (L.builds.size() <= i) L.builds.resize(i + 1);
			if (!L.builds[i]) {
				L.builds[i] = std::make_unique<Build>();
				L.builds[i]->is_unit = false;
				L.builds[i]->who = int8_t(who);
				L.builds[i]->o = int16_t(o);
			}
		}
	};
	if (forced >= 0) {
		// Load / scenario placement: allocate everything missing up to the forced slot.
		for (int32_t o = mark; o <= forced; o++) ensure(o);
		if (mark <= forced) mark = forced + 1;
		return forced;
	}
	for (int32_t o = base; o < mark; o++) {
		const Object* ob = g.object(who, o);
		if (!ob) continue;
		if (!ob->alive() && ob->hold_frames == 0 && (!units || static_cast<const Unit*>(ob)->o_up < 0)) return o;
	}
	if (mark >= limit) return -1;
	ensure(mark);
	return mark++;
}

// Objects::remove @658980: combat_death.cpp.

// ---------------------------------------------------------------- creation

// The "counted unit" test of Unit::set_type / Unit::close / Objects::init_unit: control_cost != 0, is(FIGHTERBOMBER 308)
// or a government hero (unit_flags & 0x4000000), and not a decoy.
bool unit_counted_type(const Game& g, const Unit& u) {
	return u.ptype->control_cost != 0 || unit_is(g, u, 308) || (u.ptype->unit_flags & 0x4000000);
}
bool unit_counted(const Game& g, const Unit& u) { return unit_counted_type(g, u) && !(u.unit_masks & umask::kDecoy); }

// Objects::init_unit: a follower takes back the counters Unit::init added (the captain alone counts).
void undo_follower_counts(Game& g, Unit& u) {
	if (!unit_counted_type(g, u)) return;
	Leader& l = g.leaders[size_t(u.who)];
	track_unit_type(g, u.who, u.type, -1, u.o);
	l.control -= (u.unit_masks & umask::kDecoy) ? 0 : u.ptype->control_cost;
	l.active--;
	l.units_built--;
}

void unit_init(Game& g, Unit& u, int32_t who, int32_t type, int32_t o, int32_t x, int32_t y) {
	// close(0, -1, 0): the dead slot's path, orders and search are dropped.
	u.path.clear();
	u.orders.clear();
	u.openlist.reset();
	x = ucoord_of(x) * 48 + 24;
	y = ucoord_of(y) * 48 + 24;

	// Object::init with guy_mark = 0 (Unit::init saves and restores it around the call, so add_to_world links the
	// unit without stamping any footprint).
	const int8_t saved_guy_mark = u.guy_mark;
	u.guy_mark = 0;
	u.is_unit = true;
	u.who = int8_t(who);
	u.o = int16_t(o);
	u.flags = objflag::kValid;
	u.type = type;
	u.ptype = &g.type(type);
	u.x = x;
	u.y = y;
	u.z = g.world.in_t(tcoord_of(x), tcoord_of(y)) ? find_tcoord_z(g, tcoord_of(x), tcoord_of(y), 1) : 0;
	u.healing = 0;
	u.inside_down = -1;
	u.up = u.down = -1;
	u.near_o = u.near_who = -1;
	u.damage = 0;
	u.targeted = 0;
	u.visible = 0;
	u.uid = g.objects[size_t(who)].obj_ctr++;
	u.hold_frames = 0;
	if (u.ptype->obj_masks & 0x2000000) u.flags |= objflag::kDetector;
	add_to_world(g, u);
	u.guy_mark = saved_guy_mark;

	u.angle = 0x55555555u;
	u.form = int8_t((type >= 50 && type <= 53) ? 9 : 0);  // civilians walk as a mob
	u.form_mod = -1;
	u.full = 0;
	u.mana_burn = 0;
	u.spell_time = 0;
	u.attrition = 0;
	u.air_alt = 0;
	u.idle = 0;
	u.damage_frame = 0;
	u.damage_o = -1;
	u.damage_who = 0;
	u.waiting = 0;
	u.dest_angle = 0x55555555u;
	u.tolerance = 0;
	u.orders_x = u.orders_y = 0;
	u.los_x = x;
	u.los_y = y;
	u.unit_masks &= ~umask::kMultimove;
	close_orders(g, u, false);
	clear_partial_path(u);
	update_action(g, u);
	u.unit_masks = 0;
	u.unit_masks2 = 0;
	// TODO(frame-spec 8.1): the CEO bit (is(0x165)), Steam statistics: not simulated.
	u.recharging = 0;
	u.num_queued = 0;
	u.queue_time = 0;
	u.safe = 0;
	u.supply = -1;
	u.hero = -1;
	u.inside_up = -1;
	u.inside_up_who = int8_t(who);
	u.group = -1;
	u.gather_down = -1;
	u.good_obj = -1;
	u.rare = 0;      // +0x54
	u.doober = -1;   // +0x86 (workers; caravans get their Caravan slot below)
	u.cara = -1;
	// Stance from the leader options by stance type (TODO: get_stance_type per type; combat default here, the
	// scenario loader overwrites it with the file's stance).
	u.stance = g.leaders[size_t(who)].combat_stance;
	// TODO(frame-spec 8.1): UNIT_CAN_TRANSPORT from the leader's transport level, tribe bonuses 0x12 / 0x77,
	// Supplies / Heroes / Caravans / Specials registration.
	if (u.ptype->unit_flags2 & 4) u.unit_masks |= umask::kPacked;  // packer types start packed
	if (who < 9) g.total_units++;  // GameData.total_units (read by ObjectsData::find_unit_with_radius)

	// Guys: squad_size + crew_size; new ones start cleared (Guy::clear), existing ones keep their state.
	const int32_t n = u.ptype->squad_size + u.ptype->crew_size;
	if (int32_t(u.guys.size()) < n) {
		const size_t old = u.guys.size();
		u.guys.resize(size_t(n));
		for (size_t i = old; i < u.guys.size(); i++) u.guys[i].guy_num = int8_t(i);
	} else {
		u.guys.resize(size_t(n));
	}
	u.guy_mark = int8_t(u.ptype->squad_size);
	for (int32_t i = 0; i < n; i++) {
		Guy& gy = u.guys[size_t(i)];
		gy.type = type;
		gy.o = int16_t(o);
		gy.who = int8_t(who);
		guy_init_real(g, u, gy, 0);  // one game_random draw per guy
		gy.guy_num = int8_t(i);
		gy.variation = 0;
	}
	unit_update_gpiece(g, u);  // Unit::update_gpiece (same pieces as init_real chose: guy_num was already set)
	unit_set_new_location(g, u, x, y, true, true);
	u.collide_o = -1;
	u.collide_who = -1;
	u.collide_guy = -1;
	u.collide_frame = -1;
	u.announce_frame = -1;
	u.o_up = -1;
	u.o_down = -1;
	u.cavarch_o = -1;
	u.play = -1;
	u.collide = 0;
	u.cavarch_who = 0;
	u.cavarch_uid = 0;
	u.openlist.reset();
	u.start_dist = 0;
	u.avoid_sea = u.avoid_land = 0;
	u.avoid_x = u.avoid_y = -1;
	// update_hits / update_los / update_speed / update_armor / update_seen(0): the first fog stamp happens here, even
	// for a unit created inside a building (fog-spec 5.1, 11.6).
	// SubObject::init -> Unit::set_type(T, INIT): the counted leader bookkeeping (production-spec 6.8.1); followers
	// undo it in init_unit.
	if (unit_counted(g, u)) {
		track_unit_type(g, who, type, 1, o);
		g.leaders[size_t(who)].control += u.ptype->control_cost;
		g.leaders[size_t(who)].active++;
	}
	if (u.ptype->unit_flags2 & 8) g.leaders[size_t(who)].caras++;
	// Supplies::init_supply for supply units (economy-spec 7.2).
	if (who < 8 && (u.ptype->unit_flags2 & 0x40)) u.supply = int16_t(supplies_init_supply(g, who, o));
	// The +0x86 slot (Unit.cara, shared with the doober / herd / special slots): a hero takes its Heroes record
	// (Heroes::init_hero; a government hero also stamps gov_hero_frame), a land caravan its Caravan record
	// (Caravans::init_caravan, economy-spec 6.1), a spy / commando (not a citizen) its Specials record (kept in the unused
	// `hero` member, so the economy digest that hashes `cara` only changes for real heroes).
	if (u.ptype->unit_flags2 & 0x20) {
		if (who < 8) {
			u.cara = int16_t(heroes_init_hero(g, who, o));
			if (u.ptype->unit_flags & 0x4000000) g.leaders[size_t(who)].gov_hero_frame = g.frame;
		}
	} else if (who < 8 && (u.ptype->unit_flags2 & 8) && u.ptype->domain == 0) {
		u.cara = int16_t(caravans_init(g, who, o));
	} else if (who < 8 && type > 53 && (u.ptype->unit_flags2 & 0x10)) {
		u.hero = int16_t(specials_init_special(g, who, o));  // (the original's +0x86 "special"; see objects.hpp)
	}
	u.damage_frac = 0;
	if (who < 8) g.leaders[size_t(who)].flags |= 0x800000u;  // LEADER_NEW_UNITS (Unit::init @612d83; compute_score clears it)
	unit_update_hits(g, u);   // Unit::update_hits @60e930 (combat-spec 5.1)
	unit_update_los(g, u);
	unit_update_armor(g, u);  // Unit::update_armor @6054c0
	update_speed(g, u);
	object_update_seen(g, u, false);
	if ((g.leaders[size_t(who)].flags & 0xc) != leaderflag::kHuman) u.unit_masks |= umask::kAiControl;
	g.leaders[size_t(who)].units_built++;
}

namespace {

// UnitData::get_captain @610ab0 (follows o_up to the top of the chain).
int32_t unit_get_captain(Game& g, const Unit& u) {
	const Unit* p = &u;
	for (int guard = 0; p->o_up >= 0 && guard < kUnitLimit; guard++) {
		const Unit* q = g.unit(p->who, p->o_up);
		if (!q) break;
		p = q;
	}
	return p->o;
}

// Objects::init_unit: a follower goes to UnitType::find_nearby_spot around its captain (radii from the captain's
// new_block_radius, probing around the captain's angle), or stays where it is.
void place_follower(Game& g, Unit& u, const Unit& cap) {
	const int32_t r = cap.ptype->new_block_radius;
	int32_t sx = 0, sy = 0;
	if (find_nearby_spot(g, *u.ptype, cap.x, cap.y, &sx, &sy, r * 0x30, r * 0x60 + 0xc0, -1, cap.angle, kFilterNotMe,
	                     u.o, u.who, 0, 0, -1, 0, -1) != 0) {
		sx = u.x;
		sy = u.y;
	}
	unit_set_new_location(g, u, sx, sy, true, true);
}

}  // namespace

int32_t init_unit(Game& g, int32_t who, int32_t type, int32_t x, int32_t y, int32_t forced_o, int32_t o_up,
                  int32_t o_down) {
	if (type < kUnits || type >= kEndGaia) return -1;
	const Type& t = g.type(type);
	const int32_t count = forced_o >= 0 ? 1 : std::max(t.uber_size, 1);
	int32_t prev = -1, captain = -1;
	for (int32_t k = 0; k < count; k++) {
		const int32_t o = find_free(g, who, 0, kUnitLimit, forced_o);
		if (o < 0) return captain >= 0 ? captain : o;
		Unit& u = *g.unit(who, o);
		unit_init(g, u, who, type, o, x, y);
		if (forced_o >= 0) {
			u.o_up = int16_t(o_up);
			u.o_down = int16_t(o_down);
			if (u.o_up < 0) return o;
			undo_follower_counts(g, u);
			// A follower placed on its captain's ucoord moves next to it.
			const int32_t cap = unit_get_captain(g, u);
			if (cap >= 0 && cap != o) {
				const Unit& c = *g.unit(who, cap);
				if (ucoord_of(x) == ucoord_of(c.x) && ucoord_of(y) == ucoord_of(c.y)) place_follower(g, u, c);
			}
			return o;
		}
		u.o_up = int16_t(prev);
		if (prev >= 0) {
			g.unit(who, prev)->o_down = int16_t(o);
			undo_follower_counts(g, u);
			place_follower(g, u, *g.unit(who, unit_get_captain(g, u)));
		} else {
			captain = o;
		}
		prev = o;
	}
	return captain;
}

// Objects::init_build / Build::init / Build::activate: production (build.cpp init_build_site, build_activate_full).

// Death: combat_death.cpp (object_die, unit_close, unit_die).

// ---------------------------------------------------------------- per-frame passes

void unit_process(Game& g, Unit& u) {
	// 1. countdowns
	if (u.recharging) u.recharging--;
	if (u.full) u.full--;
	if (u.waiting) u.waiting--;
	if (u.healing) u.healing--;
	// 2. aircraft fuel and healing in a base (sim/aircraft.cpp); decoys, spell casters (Caster::process_spells, mana).
	unit_process_air(g, u);
	if (!unit_process_casters(g, u)) return;
	unit_process_air_queue(g, u);  // 3. the carrier's aircraft queue (Unit +0xa0, aircraft-spec 7)
	// 4.
	process_healing(g, u);
	if (u.on_map()) {
		// UNIT2_HOLD_ANIM freezes the guy clock for the frame fight sets it in (combat-spec 4.5.3).
		u.unit_masks2 &= ~umask2::kHoldAnim;
		// 5.
		if ((g.frame + u.o) % 16 == 0) {
			u.targeted = int8_t(u.targeted / 4);
			unit_process_cloak(g, u);
			// TODO(frame-spec 9): detected marks.
			if ((g.frame + u.o) % 32 == 0) {
				u.unit_masks2 &= ~umask2::kAttritionSafe;
				process_attrition(g, u);
				// TODO(frame-spec 9): the detected-mark toggle (unit_masks2 bits 0 / 1) that follows.
			}
		}
		// Attrition ticks (combat-spec 7.1): unsupplied units suffer; supplied ones are marked safe.
		if (u.attrition != 0 && (g.frame + u.o) % u.attrition == 0) {
			if (!process_supply(g, u)) {
				if (suffer_attrition(g, u, true) != 0) return;
			} else {
				u.unit_masks2 |= umask2::kAttritionSafe;
			}
		}
		work(g, u);
		if (!u.alive()) return;
		const int32_t squad = u.ptype->squad_size;
		for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) guy_process(g, u, u.guys[size_t(i)]);
		for (int32_t i = squad; size_t(i) < u.guys.size(); i++) guy_process(g, u, u.guys[size_t(i)]);
	} else {
		// 6. garrisoned / carried (garrison-spec 7): transient masks, the AI come_out check, guy z copies.
		unit_process_inside(g, u);
	}
}

namespace {

constexpr int32_t kBird = 402;  // gaia BIRD type

// The gaia bird top-up of Objects::process_all (every 32 frames).
void spawn_birds(Game& g) {
	const World& w = g.world;
	const int32_t target = std::min<int32_t>(10, w.xs * w.ys / 100);
	int32_t have = 0;
	const ObjectList& L = g.objects[kGaiaWho];
	for (int32_t o = 0; o < L.unit_mark; o++) {
		const Unit* u = g.unit(kGaiaWho, o);
		if (u && u->alive() && u->type == kBird) have++;  // TODO: is_animal() && ptype.type == BIRD exactly
	}
	for (int32_t k = 0; k < target - have; k++) {
		const int32_t tx = g.rnd(w.xs), ty = g.rnd(w.ys);
		if (w.w(tx, ty).flags & 0x20) {
			const int32_t o = init_unit(g, kGaiaWho, kBird, tx * 768 + 384, ty * 768 + 384, -1, -1, -1);
			if (o >= 0) add_air_patrol_order(g, *g.unit(kGaiaWho, o), tx * 768 + 384, ty * 768 + 384, -1, -1, false);
		}
	}
}

}  // namespace

void objects_process_all(Game& g) {
	// 1. units, starting with player frame % 10
	for (int32_t k = 0; k < 10; k++) {
		const int32_t who = (g.frame + k) % 10;
		if (!(g.leaders[size_t(who)].flags & leaderflag::kValid)) continue;
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {  // the mark is re-read every iteration
			Unit* u = g.unit(who, o);
			if (!u) continue;
			if (u->alive()) {
				if (is_animal_who(who)) animal_process(g, *u);  // Animal::process @5d72c0 (gaia: herds, birds)
				else unit_process(g, *u);
			}
			else if (u->hold_frames) u->hold_frames--;
		}
	}
	// 2. buildings, players 0..7
	for (int32_t who = 0; who < 8; who++) {
		if (!(g.leaders[size_t(who)].flags & leaderflag::kValid)) continue;
		for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
			Build* b = g.build(who, o);
			if (!b) continue;
			if (b->alive()) {
				// Build::process @61edf0 with Wall::process @640450 (construct.cpp). TODO(fog-spec 6.1): the
				// ACTIVE_PING territory reveal (needs territory).
				build_process(g, *b);
			} else if (b->hold_frames) {
				// A dead building still holding a garrison lets one chain out per frame (Build::process_ejection).
				if (b->build_masks & bmask::kEjecting) build_process_ejection(g, *b);
				b->hold_frames--;
			}
		}
		// walls (3000+): never allocated (frame-spec 8.1)
	}
	// 3. gaia birds and herds
	if (g.frame % 32 == 0) {
		spawn_birds(g);
		herds_process(g);  // every 64 frames Herd::process(herds[(frame / 64) % max(5, herds.length)])
	}
}

void objects_inc_time(Game& g) {
	// TODO(frame-spec 7): Nuke::do_damage, goods, Farms, Doober, Surf.
	for (int32_t who = 0; who < kNumLeaders; who++) {
		if (!(g.leaders[size_t(who)].flags & leaderflag::kValid)) continue;
		for (int32_t o = 0; o < g.objects[size_t(who)].unit_mark; o++) {
			Unit* u = g.unit(who, o);
			if (!u || !u->alive()) continue;
			unit_inc_time(g, *u);
			unit_execute_events(g, *u);
		}
		// buildings: Wall::inc_time (construction hit points; air-defence turret events TODO(combat-spec 4.13.4))
		if (who < 8) {
			for (int32_t o = kBuildBase; o < g.objects[size_t(who)].build_mark; o++) {
				Build* b = g.build(who, o);
				if (b && b->alive()) wall_inc_time(g, *b);
			}
		}
	}
	ammo_inc_time_all(g);  // every live Ammo in slot order
	deaths_inc_time(g);    // every corpse
	farms_inc_time(g);     // Farms::inc_time (G9); Doober / Surf: presentation
}

}  // namespace ron::sim
