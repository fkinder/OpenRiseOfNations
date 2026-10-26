// Aircraft (docs/aircraft-spec.md; decompiles in re/decomp/aircraft and re/decomp/aircraft2, disassembly in the dis/
// directories). Containers and capacity, the air orders, the flight model with the guy's bank / pitch floats, fuel and
// landing, the three air order handlers, target searches, the unit frame block, launching and the carrier's
// PLANERELEASE events. The commands are in aircraft_cmd.cpp.
//
// The single-precision float math of bank_aircraft / pitch_aircraft must not be fused.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include "sim/aircraft.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "sim/ai.hpp"
#include "sim/animals.hpp"
#include "sim/combat.hpp"
#include "sim/combat_internal.hpp"
#include "sim/compass.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/guys.hpp"
#include "sim/heroes.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/placement.hpp"
#include "sim/production.hpp"
#include "sim/search.hpp"
#include "sim/transport.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace combat_detail;

bool order_is_air(OrderType t) { return t == kStrafe || t == kAirPatrol || t == kAirAttackGround; }

bool air_trace() {
	static const bool on = std::getenv("RON_AIR_TRACE") != nullptr && std::getenv("RON_AIR_TRACE")[0] != '0';
	return on;
}

namespace {

constexpr uint32_t kMissileMask = 0x8000000u;

bool is_missile_type(const Type& t) { return (uint32_t(t.obj_masks) & kMissileMask) != 0; }
bool is_heli_type(const Type& t) { return (t.unit_flags & utflag::kHelicopter) != 0; }

// The container / target slot (alive or not), null for a bad index.
Object* slot(Game& g, int32_t o, int32_t who) { return o >= 0 && who >= 0 && who < kNumLeaders ? g.object(who, o) : nullptr; }
const Object* slot(const Game& g, int32_t o, int32_t who) {
	return o >= 0 && who >= 0 && who < kNumLeaders ? g.object(who, o) : nullptr;
}

// Trace helper.
void trace(const Game& g, const Unit& u, const char* what, int32_t a = 0, int32_t b = 0) {
	if (!air_trace()) return;
	std::fprintf(stderr, "[air] f%d L%d unit %d (type %d) %s %d %d at %d,%d\n", g.frame, u.who, u.o, u.type, what, a, b, u.x, u.y);
}

// The wcoord cell of a world position (div_3_table[c >> 8]).
inline int32_t wcell(int32_t c) { return c / 768; }

}  // namespace

// ---------------------------------------------------------------- containers (2)

bool can_carry_air(const Object& ob) {
	if (!ob.ptype) return false;
	return type_is(*ob.ptype, airtype::kAirbase, true) || type_is(*ob.ptype, airtype::kCarrier, true) ||
	       type_is(*ob.ptype, airtype::kSilo, true);
}

int32_t num_aircraft_limit(const Game& g, const Object& ob) {
	if (!ob.ptype) return 0;
	if (type_is(*ob.ptype, airtype::kCarrier, true)) return RON_K(g, "max_aircraft_per_carrier");
	if (type_is(*ob.ptype, airtype::kAirbase, true)) return RON_K(g, "max_aircraft_per_airbase");
	if (type_is(*ob.ptype, airtype::kSilo, true)) return 1;
	return 0;
}

int32_t unit_mana_left(const Game& g, const Unit& u) {
	const int32_t left = unit_mana(g, u) - u.mana_burn;
	return left < 0 ? 0 : left;
}

bool has_repeat_air(const Object& ob) {
	if (ob.is_unit) return (static_cast<const Unit&>(ob).unit_masks & 0x200000u) != 0;  // UnitData::has_repeat_air @46cec0
	return (static_cast<const Build&>(ob).build_masks & bmask::kRepeatAir) != 0;         // WallData::has_repeat_air @472410
}

int32_t unit_home_base(const Game& g, const Unit& u, int32_t* who_out) {
	// UnitData::home_base @609dc0. Inside a container: its o (and leader). On the map: the AirOrder's base of the front order
	// (the landing SPECIAL_ANIM names it; the take-off one looks at the order behind it), when that object is active and can
	// carry this type.
	int32_t cw = -1;
	const int32_t inside = unit_get_inside(g, u, &cw);
	if (inside >= 0) {
		if (who_out) *who_out = cw;
		return inside;
	}
	if (u.orders.empty()) return -1;
	size_t idx = 0;
	const Order* f = &u.orders[0];
	if (f->type == kSpecialAnim) {
		if (f->sa_type == 0) return f->sa_data3;  // landing: the base o (the caller's preset *who stays)
		if (f->sa_type == 1 && u.orders.size() > 1) {
			idx = 1;
			f = &u.orders[1];
		}
	}
	(void)idx;
	if (!order_is_air(f->type)) return -1;
	if (who_out) *who_out = f->whose;
	const int32_t o = f->oxx;
	if (o < 0) return -1;
	const Object* b = slot(g, o, f->whose);
	if (!b || !b->alive()) return -1;
	if (!b->is_unit && !(b->flags & objflag::kActive)) return -1;  // is_active: units VALID, buildings ACTIVE
	return object_can_carry_type(g, *b, u.type) ? o : -1;
}

namespace {

// ObjectData::has_nuke @643d40 (the name is a misnomer): does the silo already hold or expect a missile of any kind? A
// missile in its build queue, a missile with a STRAFE order based on it, or a missile inside it.
bool silo_has_missile(const Game& g, const Object& silo) {
	if (!silo.is_unit && silo.alive()) {
		const Build& b = static_cast<const Build&>(silo);
		for (int32_t i = 0; i < b.queued; i++) {
			const int32_t t = b.build_queue.get_queue(i);
			if (t >= 0 && is_unit_type(t) && is_missile_type(g.type(t))) return true;
		}
	}
	const ObjectList& L = g.objects[size_t(silo.who)];
	for (int32_t o = 0; o < L.unit_mark; o++) {
		const Unit* u = g.unit(silo.who, o);
		if (!u || !u->alive() || !is_missile_type(*u->ptype)) continue;
		if (!u->orders.empty() && u->orders.front().type == kStrafe && u->orders.front().oxx == silo.o) return true;
		int32_t w = -1;
		if (unit_get_inside(g, *u, &w) == silo.o && w == silo.who) return true;
	}
	return false;
}

}  // namespace

bool object_can_carry_type(const Game& g, const Object& c, int32_t type) {
	// ObjectData::can_carry(TypeIndex) @645e00. The type must be a unit type.
	if (!is_unit_type(type) || type >= 414) return false;
	const Type& T = g.type(type);
	if (type_is(*c.ptype, airtype::kSilo, false)) {
		// A missile (type vslot 0x114 is_missile = OBJMASK MISSILE) while the silo holds no missile yet (has_nuke @643d40).
		return is_missile_type(T) && !silo_has_missile(g, c);
	}
	if (type_is(*c.ptype, airtype::kAirbase, false)) {
		return T.domain == kDomainAir && !is_missile_type(T) && !type_is(T, airtype::kFighterBomber, false);
	}
	if (type_is(*c.ptype, airtype::kCarrier, false)) return type_is(T, airtype::kFighterBomber, false);
	// Otherwise a land transport: a ground type carried by a unit with a carry capacity.
	return T.domain == kDomainGround && c.is_unit && c.ptype->carry != 0;
}

bool air_can_carry(const Game& g, const Object& c, int32_t o, int32_t who) {
	// The air branch of ObjectData::can_carry(o, who) @6483c0 (this = c). The caller checked: same leader, not itself.
	const Object* cargo = slot(g, o, who);
	if (!cargo || !cargo->ptype) return false;
	const Type& T = *cargo->ptype;
	bool bad;
	if (!is_missile_type(T)) {
		if (type_is(T, airtype::kHelicopter, false)) {
			if (!type_is(*c.ptype, airtype::kAirbase, false)) return false;  // helicopters only on airbases
			bad = false;
		} else {
			bad = type_is(*c.ptype, airtype::kSilo, false);
		}
	} else {
		if (type_is(*c.ptype, airtype::kAirbase, false)) return false;
		bad = type_is(*c.ptype, airtype::kCarrier, false);
	}
	if (bad) return false;
	const bool carrier = type_is(*c.ptype, airtype::kCarrier, false);
	if (type_is(T, airtype::kFighterBomber, false)) {
		if (!carrier) return false;
	} else if (carrier) {
		return false;
	}
	int32_t hw = who;
	const Unit* cu = g.unit(who, o);
	if (cu && unit_home_base(g, *cu, &hw) == c.o && hw == c.who) return true;
	return num_aircraft_here(g, c, 0) < num_aircraft_limit(g, c);
}

int32_t num_aircraft_here(const Game& g, const Object& ob, int32_t count_queue) {
	// ObjectData::num_aircraft_here @645330.
	int32_t count = 0;
	const ObjectList& L = g.objects[size_t(ob.who)];
	for (int32_t i = 0; i < L.unit_mark; i++) {
		const Unit* u = g.unit(ob.who, i);
		if (!u || !u->alive() || u->ptype->domain != kDomainAir) continue;
		int32_t w = 0;
		if (unit_home_base(g, *u, &w) == ob.o && w == ob.who) count++;
	}
	if (type_is(*ob.ptype, airtype::kCarrier, false)) return int32_t(static_cast<const Unit&>(ob).num_queued) + count;
	if (ob.is_unit || count_queue == 0) return count;
	const Build& b = static_cast<const Build&>(ob);
	// count_queue(UNITS) - count_queue(TYPE, HELICOPTER): queued units that are not queued helicopters.
	return (build_count_queue(g, b, 2, 0) - build_count_queue(g, b, 1, airtype::kHelicopter)) + count;
}

// ---------------------------------------------------------------- flying low / high (7)

bool unit_is_flying_low(const Game& g, const Unit& u) {
	// UnitData::is_flying_low @60a140.
	const Type& t = *u.ptype;
	if (t.domain != kDomainAir || is_heli_type(t) || is_missile_type(t)) return false;
	if (!u.on_map()) return false;
	const Order* f = u.front();
	if (!f) return false;
	if (f->type == kStrafe) {
		const Object* tg = slot(g, f->ox, f->whom);
		if (f->ox >= 0 && f->whom >= 0 && tg && tg->alive() && vector_dist(u.x - tg->x, u.y - tg->y) < 0x900) return true;
	} else if (f->type == kAirAttackGround) {
		if (vector_dist(u.x - f->x, u.y - f->y) < 0x900) return true;
	} else if (f->type == kOrderNone) {
		return false;
	}
	// The order's AirOrder (vslot 0xfc get_air_order; +0x18 returning, +4 / +8 the home base).
	if (order_is_air(f->type) && f->returning != 0) {
		const Object* b = slot(g, f->oxx, f->whose);
		if (f->oxx >= 0 && f->whose >= 0 && b && b->alive() && vector_dist(u.x - b->x, u.y - b->y) < 0x900) return true;
	}
	return false;
}

bool unit_is_flying_high(const Game& g, const Unit& u) {
	const Type& t = *u.ptype;
	if (t.domain == kDomainAir && !is_heli_type(t) && !is_missile_type(t) && u.on_map()) return !unit_is_flying_low(g, u);
	return false;
}

// ---------------------------------------------------------------- orders (3)

namespace {

// The QUEUE_NEW preamble of the air adders: UNIT_MULTIMOVE off, path.length = 0, close_orders(0), clear_partial_path,
// update_action.
void air_clear_orders(Game& g, Unit& u) {
	u.unit_masks &= ~umask::kMultimove;
	u.path.clear();
	close_orders(g, u, false);
	clear_partial_path(u);
	update_action(g, u);
}

}  // namespace

void add_air_attack_ground_order(Game& g, Unit& u, int32_t x, int32_t y, int32_t home_o, int32_t home_who, QueuePos q,
                                 bool expl) {
	// @5e41c0
	if (q == kQueueNew) air_clear_orders(g, u);
	Order a;
	a.type = kAirAttackGround;
	a.x = x;
	a.y = y;
	a.oxx = home_o;
	a.whose = home_who;
	a.cruising_alt = 0x640;
	if (expl) a.flags |= orderflag::kExplicit;
	if (q == kQueueFirst) {
		clear_partial_path(u);
		u.orders.push_front(a);
	} else {
		u.orders.push_back(a);
	}
	update_action(g, u);
}

void add_strafe_order(Game& g, Unit& u, int32_t ox, int32_t who, int32_t home_o, int32_t home_who, int32_t mandatory,
                      QueuePos q, bool expl) {
	// @5e48c0. A missile shooting at a valid target becomes an AIR_ATTACK_GROUND at the target's position.
	if (is_missile_type(*u.ptype) && valid_target(g, u, ox, who, nullptr)) {
		const Object* t = slot(g, ox, who);
		add_air_attack_ground_order(g, u, t->x, t->y, home_o, home_who, q, expl);
		return;
	}
	if (q == kQueueNew) air_clear_orders(g, u);
	Order s;
	s.type = kStrafe;
	s.ox = ox;
	s.whom = who;
	const Object* t = slot(g, ox, who);
	if (ox < 0 || who < 0 || !t) {
		s.returning = 1;
		s.uid = 0xffff;
		s.xx = s.yy = -1;
	} else {
		s.uid = t->uid;
		s.returning = 0;
		s.xx = t->x;
		s.yy = t->y;
	}
	s.oxx = home_o;
	s.whose = home_who;
	s.mandatory = int8_t(mandatory);
	s.cruising_alt = 0x640;
	if (expl) s.flags |= orderflag::kExplicit;
	if (q == kQueueFirst) {
		clear_partial_path(u);
		u.orders.push_front(s);
	} else {
		u.orders.push_back(s);
	}
}

void add_air_patrol_order(Game& g, Unit& u, int32_t x, int32_t y, int32_t home_o, int32_t home_who, bool expl) {
	// @5e4350. Helicopters get a move (ATTACK_TO, QUEUE_NEW); planes always replace the list.
	if (is_heli_type(*u.ptype)) {
		add_move_facing_order(g, u, ucoord_of(x), ucoord_of(y), find_angle(2, 0), 2, false, kQueueNew, expl, -1, -1, -1, false);
		return;
	}
	air_clear_orders(g, u);
	Order p;
	p.type = kAirPatrol;
	if (home_o >= 0 && home_who >= 0) {
		// The waypoint follows a base that is a live unit (vslot 8 is is_live_unit: a carrier): stored as an offset from it.
		const Unit* h = g.unit(home_who, home_o);
		if (h && h->alive()) {
			x -= h->x;
			y -= h->y;
		}
	}
	p.wp.push_back({x, y});
	p.wp_index = 0;
	p.returning = 0;
	p.oxx = home_o;
	p.whose = home_who;
	p.cruising_alt = 0x640;
	if (expl) p.flags |= orderflag::kExplicit;
	u.orders.push_back(p);
}

Order* add_spec_anim_order(Game& g, Unit& u, int32_t type, int32_t data1, int32_t data2) {
	// @5e4160: always in front of the others (the list is rotated), flag 4 set.
	Order s;
	s.type = kSpecialAnim;
	s.flags = orderflag::kExplicit;
	s.sa_type = type;
	s.sa_data1 = data1;
	s.sa_data2 = data2;
	clear_partial_path(u);
	u.orders.push_front(s);
	update_action(g, u);
	return &u.orders.front();
}

// ---------------------------------------------------------------- the flight model (4)

namespace {

int32_t cvtt(float f) { return int32_t(f); }  // CVTTSS2SI (the values here are small)

// The type's TURN_SPEED term shared by air_turn_speed and bank_aircraft: (turn_speed >> 8) * unit_turn_speed.
uint32_t base_turn(const Game& g, const Unit& u) {
	return (uint32_t(u.ptype->turn_speed) >> 8) * uint32_t(RON_K(g, "unit_turn_speed"));
}

// Unit::air_turn_speed(dir, p2 = 0) @5ea390.
uint32_t air_turn_speed(const Game& g, const Unit& u, int32_t dir) {
	const bool heli = is_heli_type(*u.ptype);
	uint32_t s = base_turn(g, u);
	int32_t v = -cvtt(u.guys[0].bank);
	if (heli) v *= 5;
	if (((uint32_t(v) ^ uint32_t(dir)) & 0x80000000u) != 0) return 0x5b05b0u;
	s = (s / 0x37u) * uint32_t(v < 0 ? -v : v);
	if (!heli && s < 0x5b05b0u) s = 0x5b05b0u;
	return s;
}

// The current order's AirOrder `returning` flag (vslot 0xfc get_air_order, +0x18); false when it is not an air order.
bool order_returning(const Unit& u) {
	const Order* f = u.front();
	return f && order_is_air(f->type) && f->returning != 0;
}

// Unit::bank_aircraft(want, &speed, facing) @5e9520.
void bank_aircraft(Game& g, Unit& u, uint32_t want, int32_t* speed, int32_t facing) {
	Guy& gy = u.guys[0];
	const bool heli = is_heli_type(*u.ptype);
	float b = -gy.bank;
	if (heli) b = b * 5.0f;
	const uint32_t diff = want - u.angle;
	const uint32_t adiff = diff > 0x80000000u ? ~diff : diff;
	int32_t dir = facing;
	if (dir == 0) dir = diff > 0x80000000u ? -1 : 1;
	uint32_t t = base_turn(g, u);
	if (!heli && t < 0x5b05b0u) t = 0x5b05b0u;
	float raw = float(adiff) / float(int32_t(t));
	raw = raw * 55.0f;
	raw = raw * 0.33f;
	if (5.0f > std::fabs(raw)) raw = raw * 0.5f;
	const bool returning = order_returning(u);
	float maxb;
	if (returning && !heli) {
		maxb = raw + raw;
		if (!(55.0f > maxb)) maxb = 55.0f;
	} else if (u.who >= 8 || iabs(gy.z - find_tcoord_z_at(g, u.x, u.y, 0)) >= 200) {
		maxb = raw;
		if (!(55.0f > maxb)) maxb = 55.0f;
	} else {
		maxb = 0.0f;  // low over the ground: the wings stay level
	}
	const float err = float(dir) * maxb - b;
	const float aerr = std::fabs(err);
	int32_t d = 0;      // [EBP-8]: the turn direction
	bool snap = false;  // the 3 degree snap of the heading (label 9806)
	auto classify = [&]() {
		if (b > 0.0f) d = 1;
		else if (0.0f > b) d = -1;
		else {
			b = 0.0f;
			d = 0;
			snap = adiff < 0x38e38e3u;
		}
	};
	if (2.0f > aerr) {
		if (2.0f > std::fabs(b)) {
			b = 0.0f;
			d = 0;
			snap = adiff < 0x38e38e3u;
		} else {
			classify();
		}
	} else {
		if (err > 0.0f) {
			if (55.0f > b) b = (10.0f > aerr) ? b + aerr : b + 10.0f;
		} else {
			if (b > -55.0f) b = (10.0f > aerr) ? b - aerr : b - 10.0f;
		}
		classify();
	}
	if (snap) u.angle = want;
	else u.angle = u.angle + air_turn_speed(g, u, d) * uint32_t(d);
	if (returning) {
		const uint32_t da = u.angle - want;
		const uint32_t ad = da > 0x80000000u ? ~da : da;
		if (ad < 0x2222220u) {
			u.angle = want;
		} else {
			const uint32_t nz = u.angle + air_turn_speed(g, u, d) * uint32_t(d);
			const uint32_t dn = nz - want;
			const uint32_t adn = dn > 0x80000000u ? ~dn : dn;
			if (adn < ad) u.angle = nz;
		}
	}
	if (returning && b != 0.0f) {
		const int32_t s3 = *speed * 3;
		*speed = (s3 + ((s3 >> 31) & 3)) >> 2;
	}
	if (heli) b = b / 5.0f;
	gy.last_bank = gy.bank;
	gy.bank = -b;
}

// Unit::pitch_aircraft(dx, dy, goal_z, &speed) @5e8de0: the altitude and pitch of the guy; the speed changes with them.
void pitch_aircraft(Game& g, Unit& u, int32_t dx, int32_t dy, int32_t goal_z, int32_t* speed) {
	Guy& gy = u.guys[0];
	const Type& T = *u.ptype;
	const bool heli = is_heli_type(T);
	Order* A = u.front();
	const bool has_air = A && order_is_air(A->type);
	const bool returning = has_air && A->returning != 0;
	int32_t px, py;
	project(u.x, u.y, u.angle, 0x240, &px, &py);
	if (px < 0) px = 0;
	if (py < 0) py = 0;
	{
		const int32_t mx = g.world.xs * 0x300, my = g.world.ys * 0x300;
		if (px >= mx) px = mx - 1;
		if (py >= my) py = my - 1;
	}
	const int32_t z_ahead = find_tcoord_z_at(g, px, py, 0);
	int32_t dist_for_div = 0x240;  // [EBP-0x18]
	bool base_moving = false;      // local_2c: the plane is slower than 3x the carrier's last step
	int32_t target_alt;
	if (returning) {
		const Object* base = slot(g, A->oxx, A->whose);
		if (A->oxx >= 0 && base && base->is_unit) {
			const Unit& bu = static_cast<const Unit&>(*base);
			if (!bu.guys.empty()) base_moving = *speed < bu.guys[0].last_speed * 3;
		}
		const int32_t d = vector_dist(dx, dy);
		dist_for_div = d;
		const int32_t q = d / *speed;
		const int32_t a = z_ahead + 100;
		const int32_t c = goal_z + q * 0x19;
		target_alt = c > a ? c : a;
		if (d < 0x600 && !heli && !base_moving) *speed = *speed / 2;
	} else {
		target_alt = A ? A->cruising_alt + z_ahead : z_ahead;
		if (has_air && A->type == kStrafe && (T.unit_flags & utflag::kStrafing) && A->ox >= 0 && A->whom >= 0) {
			// is_attack and a target (TargetOrder vslot +4)
			const Object* tg = slot(g, A->ox, A->whom);
			if (tg && !is_ally(g, u.who, A->whom)) {
				const uint32_t ang = find_angle(dx, dy);
				if (angle_diff(u.angle, ang) <= 0x40000000u) {
					if (tg->ptype->domain != kDomainAir) {
						target_alt = A->cruising_alt / 2 + z_ahead;
					} else {
						const Unit* tu = static_cast<const Unit*>(tg);
						target_alt = tu->guys.empty() ? 0 : tu->guys[0].z;
						const int32_t d2 = vector_dist(iabs(tu->x - u.x), iabs(tu->y - u.y));
						int32_t k = (0x600 - d2) * 5;
						if (k < 0) k = 0;
						if (k > 300) k = 300;
						if (A->ox == u.o) target_alt += (A->whom < u.who) ? k : -k;
						else target_alt += (A->ox < u.o) ? k : -k;
					}
				}
			}
		}
	}
	int32_t alt = target_alt;
	if (A && alt > A->cruising_alt + z_ahead) alt = A->cruising_alt + z_ahead;  // (non-animals)
	const int32_t z_here = find_tcoord_z_at(g, u.x, u.y, 0);
	const int32_t margin = returning ? 0x32 : 0xc8;
	int32_t T_alt = z_here + margin;
	if (alt > T_alt) T_alt = alt;  // CMOVG: the higher of the two
	float pitch = gy.pitch;
	float bias = 0.0f;
	if (!heli && returning && T_alt < z_here + 500) bias = -20.0f;
	int32_t climb;
	if (heli) {
		const int32_t diff = T_alt - gy.z;
		climb = 0;
		if (diff > 0xc8) climb = 10;
		else if (diff < -200) climb = -10;
	} else {
		int32_t q = dist_for_div / *speed;
		if (q < 1) q = 1;
		climb = (((T_alt - gy.z) / 25) * 20) / q;
	}
	float xm = float(climb) - (bias + pitch);
	int32_t sgn;
	if (xm > 1.0f) {
		sgn = 1;
		if (40.0f > pitch) pitch = pitch + 2.0f;
	} else if (-1.0f > xm) {
		sgn = -1;
		if (pitch > -40.0f) pitch = pitch - 2.0f;
	} else {
		sgn = 0;
	}
	if (1.0f > std::fabs(bias + pitch) && sgn == 0) pitch = -bias;
	const float sum = bias + pitch;
	int32_t dirz;
	if (sum > 0.0f) dirz = 1;
	else dirz = (0.0f <= sum ? 1 : 0) - 1;  // SETBE CL; DEC ECX
	if (sum != 0.0f) {
		if (heli) {
			const int32_t step = dirz * 0x19;
			const int32_t adiff = iabs(gy.z - T_alt), astep = iabs(step);
			if (astep > adiff) {
				gy.last_z = gy.z;
				gy.z = T_alt;
			} else {
				gy.last_z = gy.z;
				gy.z = gy.z + step;
			}
		} else {
			int32_t delta = 0;
			if ((dirz < 0) == (0.0f > sum)) delta = cvtt((sum * 25.0f) / 20.0f);
			gy.last_z = gy.z;
			gy.z = gy.z + delta;
		}
	}
	const float apitch = std::fabs(pitch);
	if (apitch > 20.0f && !base_moving) {
		const int32_t half = *speed / 2;
		float v = 40.0f - apitch;
		const float fh = float(half);
		v = v * fh;
		v = v / 20.0f;
		v = v + fh;
		*speed = cvtt(v);
	}
	gy.last_pitch = gy.pitch;
	gy.pitch = heli ? 0.0f : pitch;
}

}  // namespace

// ---------------------------------------------------------------- fuel and landing (6)

int32_t check_fuel(Game& g, Unit& u, Order& A, int32_t* px, int32_t* py, int32_t* pz) {
	// @5e9be0. A.returning is the "go home" flag.
	const Type& T = *u.ptype;
	if (A.returning == 0 && T.mana == 0) return 0;
	int32_t home_o = A.oxx, home_who = A.whose;
	if (A.returning == 0) {
		if (unit_mana_left(g, u) == 0 && !is_missile_type(T)) A.returning = 1;
	}
	if (A.returning == 0) return 0;
	// The old base still good?
	bool have = false;
	if (home_o >= 0) {
		const Object* b = slot(g, home_o, home_who);
		if (b && b->alive() && (b->is_unit || (b->flags & objflag::kActive)) && can_carry_unit(g, *b, u.o, u.who)) have = true;
	}
	if (!have) {
		// The nearest of the leader's buildings (slots 2000..), then its units (carriers), strictly nearer replaces.
		int32_t bo = -1, bw = -1;
		const ObjectList& L = g.objects[size_t(u.who)];
		for (int32_t i = kBuildBase; i < L.build_mark; i++) {
			const Build* b = g.build(u.who, i);
			if (!b || !b->alive() || !(b->flags & objflag::kActive) || !can_carry_unit(g, *b, u.o, u.who)) continue;
			if (bo < 0) {
				bo = i;
				bw = u.who;
			} else {
				const Object* cur = g.object(bw, bo);
				const int32_t dcur = vector_dist(cur->y - u.y, cur->x - u.x);
				const int32_t dnew = vector_dist(b->y - u.y, b->x - u.x);
				if (dnew < dcur) {
					bo = i;
					bw = u.who;
				}
			}
		}
		for (int32_t i = 0; i < L.unit_mark; i++) {
			const Unit* c = g.unit(u.who, i);
			if (!c || !c->alive() || !can_carry_unit(g, *c, u.o, u.who)) continue;
			if (bo < 0) {
				bo = i;
				bw = u.who;
			} else {
				const Object* cur = g.object(bw, bo);
				const int32_t dcur = vector_dist(cur->y - u.y, cur->x - u.x);
				const int32_t dnew = vector_dist(c->y - u.y, c->x - u.x);
				if (dnew < dcur) {
					bo = i;
					bw = u.who;
				}
			}
		}
		if (bo < 0) {
			if (is_heli_type(T)) {  // no base: hold position
				*px = u.x;
				*py = u.y;
				return 0;
			}
			// "plane out of fuel" (console message and S_PLANE_OUT_OF_FUEL for the human seat): the plane crashes.
			trace(g, u, "out of fuel: no base, crash");
			object_die(g, u, 2, -1, 0.0f);
			return 1;
		}
		A.oxx = bo;
		A.whose = bw;
		home_o = bo;
		home_who = bw;
	}
	const Object* B = slot(g, home_o, home_who);
	*pz = 0;
	*py = 0;
	bool building = true;  // bVar2: the base is a building (airbase, silo)
	*px = B->x - 0xc0;
	*py += B->y;
	*pz += B->z;
	if (B->is_unit && B->alive()) {
		// A carrier: no runway offset; not at the very edge of the map.
		const int32_t cx = wcell(B->x), cy = wcell(B->y);
		*px += 0xc0;
		if (cx == 0 || cy == 0 || cx == g.world.xs - 1 || cy == g.world.ys - 1) return 0;
		building = false;
	}
	uint32_t heading = (B->y <= g.world.ys * 0x240) ? 0u : 0x80000000u;
	if (!building) heading = static_cast<const Unit*>(B)->angle;
	const int32_t x0 = *px, y0 = *py;  // the base point P
	if (!is_heli_type(T)) {
		const int32_t dx = x0 - u.x, dy = y0 - u.y;
		const uint32_t want = find_angle(dx, dy);
		const uint32_t ad = angle_diff(heading, want);
		int32_t d2;
		int32_t nx = x0, ny = y0;
		if (ad > 0x15555555u) {
			// Not lined up with the runway: aim at the point 0xc00 behind the base along its heading.
			const uint32_t a1 = heading + 0x80000000u;
			nx = x0 + sin_a(a1, 0xc00);
			ny = y0 - cos_a(a1, 0xc00);
			d2 = vector_dist(iabs(ny - u.y), iabs(nx - u.x)) + 0x300;
		} else {
			d2 = vector_dist(iabs(dy), iabs(dx)) / 2;
		}
		if (d2 <= 0xc0 || ad >= 0x55555555u) {
			*px = nx;
			*py = ny;
		} else {
			project(x0, y0, heading + 0x80000000u, d2 > 0xc00 ? 0xc00 : d2, px, py);
		}
	}
	// z = the base's z + half the distance from the base point to the (clamped) goal
	int32_t gx = *px, gy2 = *py;
	if (gx < 0) gx = 0;
	if (gy2 < 0) gy2 = 0;
	if (gx >= g.world.xs * 0x300) gx = g.world.xs * 0x300 - 1;
	if (gy2 >= g.world.ys * 0x300) gy2 = g.world.ys * 0x300 - 1;
	*pz += vector_dist(iabs(y0 - gy2), iabs(x0 - gx)) / 2;
	return 0;
}

// ---------------------------------------------------------------- do_air_physics (4)

int32_t do_air_physics(Game& g, Unit& u, Order& A, int32_t x, int32_t y) {
	// @5e86d0. `A` is the current order (its AirOrder part).
	const Type& T = *u.ptype;
	const bool heli = is_heli_type(T);
	// 1. every 8 frames the cruising altitude is rerolled (the draw is simulation state).
	if ((u.o + g.frame) % 8 == 0) {
		const bool animal = is_animal_who(u.who);  // vslot 0x30 is_animal (the gaia birds)
		if (!type_is(T, airtype::kBomber, false) && (!animal || u.type == 0x193) && !heli) {
			const int32_t r = g.rng.get(0, 0xffff);
			A.cruising_alt = (r % 7 + 13) * 100;
		} else {
			A.cruising_alt = 0x640;
			if (animal) A.cruising_alt += u.type == 0x192 ? 200 : -200;
		}
	}
	// 2. fuel (not for animals); the path stack is the 1-point scratch of the goal. A flock bird (403) forgets its recharge and
	// treats its last waypoint as a base: it flies to it and lands there, which kills an animal (land_plane).
	u.path.clear();
	int32_t goal_z = 0;
	if (!is_animal_who(u.who) && check_fuel(g, u, A, &x, &y, &goal_z)) return 0;
	if (u.type == 0x193) {
		u.recharging = 0;
		if (u.orders.size() == 1) {
			A.returning = 1;
			goal_z = find_tcoord_z(g, tcoord_of(u.x), tcoord_of(u.y), 1);
		}
	}
	// 3. the goal is clamped into the world; a clamped goal cannot be landed on this frame.
	bool clamped = false;
	{
		const int32_t ox = x, oy = y;
		if (x < 0) x = 0;
		if (y < 0) y = 0;
		const int32_t mx = g.world.xs * 0x300, my = g.world.ys * 0x300;
		if (x >= mx) x = mx - 1;
		if (y >= my) y = my - 1;
		clamped = ox != x || oy != y;
	}
	const int32_t dx = x - u.x, dy = y - u.y;
	int32_t speed = get_speed(g, u, u.x, u.y, true);
	if (g.ai_speed > 1) speed = g.ai_speed * speed;
	// 5. landing test of a returning plane.
	if (A.returning != 0) {
		const int32_t dist = iabs(dy) + iabs(dx);  // the Manhattan distance
		if (dist < (speed * 3) / 2 && !clamped && (!heli || u.guys[0].z - goal_z < 0x96)) {
			land_plane(g, u);
			return 0;
		}
		if (A.oxx >= 0 && A.whose >= 0) {
			Object* base = slot(g, A.oxx, A.whose);
			if (base && base->is_unit && dist < 0xc00) static_cast<Unit*>(base)->unit_masks |= 0x100000;  // the carrier slows down
		}
	}
	// 6. the goal as the only path point.
	path_push(u, PathData{x, y, 0, 0});
	uint32_t facing = 0;
	if (A.returning == 0 && A.type == kStrafe && !heli) {
		const Object* tg = slot(g, A.ox, A.whom);
		if (A.ox >= 0 && A.whom >= 0 && tg && tg->ptype->domain == kDomainAir && valid_target(g, u, A.ox, A.whom, nullptr)) {
			const int32_t minr = obj_min_range(g, u);
			const int32_t tdx = tg->x - u.x, tdy = tg->y - u.y;
			if (vector_dist(tdx, tdy) < minr * 0xc0) {
				const uint32_t a = find_angle(tdx, tdy);
				const uint32_t d = a - u.angle;
				const uint32_t ad = d > 0x80000000u ? ~d : d;
				facing = ad < 0x40000000u ? 1u : 0u;
			}
		}
	}
	// 7. the heading.
	uint32_t want = find_angle(dx, dy);
	if (!heli) {
		const uint32_t d = want - u.angle;
		const uint32_t ad = d > 0x80000000u ? ~d : d;
		if (ad > 0x20000000u) {
			const int32_t thr = A.returning == 0 ? obj_min_range(g, u) * 0xc0 : 0;
			if (vector_dist(dx, dy) < thr + 0x300) want = u.angle;  // overshoot: keep the heading
		}
	}
	if (A.sharp_turn != 0) want = uint32_t(A.sharp_turn) * 0x40000000u + u.angle;
	bank_aircraft(g, u, want, &speed, int32_t(facing));
	pitch_aircraft(g, u, dx, dy, goal_z, &speed);
	guy_set_angle(g, u, u.guys[0], u.angle, true);
	// 8. the move.
	bool move = true;
	if (heli) {
		const int32_t d = vector_dist(dx, dy) - (A.returning != 0 ? 0x30 : 0xc0);
		move = d >= 0;
		if (!move && u.orders.size() > 1) {
			kill_current_order(g, u, false);
			return 0;
		}
	}
	if (move) {
		int32_t nx, ny;
		project(u.x, u.y, u.angle, speed, &nx, &ny);
		const int32_t tx = tcoord_of(nx), ty = tcoord_of(ny);
		const bool invalid = tx < 0 || ty < 0 || tx >= g.world.tile_xs || ty >= g.world.tile_ys;  // UnitData::invalid_loc, domain 2
		if (invalid) {
			if (nx < 0) nx = 0;
			if (ny < 0) ny = 0;
			if (nx >= g.world.xs * 0x300) nx = g.world.xs * 0x300 - 1;
			if (ny >= g.world.ys * 0x300) ny = g.world.ys * 0x300 - 1;
			if (A.sharp_turn == 0) {
				const int32_t r = g.rng.get(0, 0xffff);
				A.sharp_turn = (r & 1) ? 1 : -1;
			}
		} else {
			A.sharp_turn = 0;
		}
		unit_set_new_location(g, u, nx, ny, false, true);
	}
	// 9. the walk animation unless the unit is recharging a shot.
	if (u.recharging == 0) unit_set_anim(g, u, kCharWalk, 0, 1);
	if (u.type == 0x193) {
		// A flock bird: never "returning" after the call, recharge 1, and it moves on to its next waypoint once within 0x30 of this one.
		A.returning = 0;
		u.recharging = 1;
		if (iabs(x - u.x) + iabs(y - u.y) < 0x30 && u.orders.size() > 1) {
			kill_current_order(g, u, false);
			return 0;
		}
	}
	return 1;
}

// ---------------------------------------------------------------- landing (6)

void land_plane(Game& g, Unit& u) {
	// @5e9950
	if (is_animal_who(u.who)) {  // a bird that reached its last waypoint: gone
		object_die(g, u, 0, -1, 0.0f);
		return;
	}
	Order* A = u.front();
	if (!A || !order_is_air(A->type)) return;
	const bool heli = is_heli_type(*u.ptype);
	const int32_t home_o = A->oxx, home_who = A->whose;
	A->returning = 0;
	bool killed = false;
	if (A->type == kStrafe) {
		if (!(A->ox >= 0 && A->whom >= 0)) {
			if (A->mandatory == 0) kill_current_order(g, u, false);
			else clear_orders(g, u);
			killed = true;
		}
	}
	Object* base = slot(g, home_o, home_who);
	if (!base) return;
	if (!has_repeat_air(*base)) {
		u.unit_masks &= ~umask::kMultimove;
		u.path.clear();
		close_orders(g, u, false);
		clear_partial_path(u);
		update_action(g, u);
	} else if (!killed) {
		if (Order* f = u.front()) f->flags &= uint8_t(~orderflag::kExplicit);
	}
	Order* s = add_spec_anim_order(g, u, 0, 0, heli ? 3 : 1);
	s->sa_data3 = home_o;
	s->sa_data4 = home_who;
	trace(g, u, "lands at", home_o, home_who);
	work(g, u);
}

void do_spec_anim(Game& g, Unit& u, Order& S) {
	// @5e5880. The step counter is set to 1 on entry, so the order always completes in this call.
	if (S.sa_type == 2) return;
	S.sa_frames = 10;
	S.sa_started = 1;
	int32_t thresh = 0;
	if (S.sa_type == 1 && S.ox >= 0) {
		const Object* b = slot(g, S.ox, S.whom);
		if (b && b->ptype && type_is(*b->ptype, airtype::kAirbase, false)) thresh = 1;
	}
	if (S.sa_started < thresh) return;  // (never: kept for the structure of the exe)
	if (S.sa_type == 0) {
		// Landing: the plane goes inside its base (a base that is gone and is not a carrier: the plane is lost).
		const int32_t bo = S.sa_data3, bw = S.sa_data4;
		const Object* B = slot(g, bo, bw);
		const bool building_alive = B && !B->is_unit && B->alive();
		if (!B || (!building_alive && !type_is(*B->ptype, airtype::kCarrier, false))) {
			kill_current_order(g, u, false);
			object_die(g, u, 0, -1, 0.0f);
			return;
		}
		unit_go_inside(g, u, bo, bw);
		kill_current_order(g, u, false);
		return;
	}
	if (thresh != 0) {
		// Take-off from an airbase: the plane appears 0xc0 west of it facing north (helicopters scatter), on the ground.
		const bool heli = is_heli_type(*u.ptype);
		int32_t ddx = -0xc0, ddy = 0;
		if (heli) {
			const int32_t r1 = g.rng.get(0, 0xffff);
			ddx = r1 % 0xb - 0xc5;
			const int32_t r2 = g.rng.get(0, 0xffff);
			ddy = r2 % 0xb - 5;
		}
		const int32_t bx = S.sa_data3, by = S.sa_data4;
		unit_set_angle(g, u, 0, true);
		unit_set_new_location(g, u, bx + ddx, by + ddy, true, true);
		guy_set_new_z(u.guys[0], find_data_z(g, bx, by, 0), true);
		if (heli) guy_set_new_z(u.guys[0], u.guys[0].z + 200, true);
		u.guys[0].last_pitch = u.guys[0].pitch;
		u.guys[0].pitch = 0.0f;
		u.guys[0].last_bank = u.guys[0].bank;
		u.guys[0].bank = 0.0f;
		kill_current_order(g, u, false);
		work(g, u);
		return;
	}
	kill_current_order(g, u, false);
}

// ---------------------------------------------------------------- target searches (5)

namespace {

struct Hit {
	int32_t o, who;
};

// Objects::find_units @65a620 (players' units only, i.e. param_11 = 1): the on-map live units of the leaders passing the
// SearchIndexBH relation to `who`, in the order of the exe (the circle cells of the world grid, or every leader's slots).
void find_units_list(const Game& g, int32_t x, int32_t y, int32_t search, int32_t who, int32_t radius, int32_t filter,
                     int32_t f1, int32_t f2, std::vector<Hit>& out) {
	out.clear();
	const Circles& C = circles();
	const int32_t k = (radius + 0x2ff) / 0x300;
	auto pass = [&](int32_t ow, const Object& ob) {
		if (!ob.is_unit || !ob.alive() || !static_cast<const Unit&>(ob).on_map()) return false;
		if (filter != 0 && !search_valid_filter(g, ow, ob.o, f1, f2, filter)) return false;
		return true;
	};
	if (radius >= 0 && k <= 64 && C.radius[size_t(k)] <= g.total_units) {
		const int32_t wx = wcell(x), wy = wcell(y);
		for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
			const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
			if (!g.world.in_w(cx, cy)) continue;
			const WData& d = g.world.w(cx, cy);
			int32_t co = d.down, cw = d.down_who;
			while (co >= 0) {
				const Object* ob = g.object(cw, co);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				if (cw < 8 && search_valid_search(g, cw, search, who) && pass(cw, *ob)) out.push_back({co, cw});
				co = no;
				cw = nw;
			}
		}
		return;
	}
	for (int32_t lw = 0; lw < 8; lw++) {
		if (!(g.leaders[size_t(lw)].flags & leaderflag::kValid)) continue;
		if (!search_valid_search(g, lw, search, who)) continue;
		const ObjectList& L = g.objects[size_t(lw)];
		for (int32_t o = 0; o < L.unit_mark; o++) {
			const Unit* u = g.unit(lw, o);
			if (!u || !pass(lw, *u)) continue;
			const int32_t d = vector_dist(u->x - x, u->y - y);
			const int32_t br = u->ptype->domain == 1 ? u->ptype->push_size : u->ptype->big_radius;
			if (radius < 0 || d - br <= radius) out.push_back({o, lw});
		}
	}
}

// Objects::find_builds @65a120 (list of VALID buildings of leaders < 8 passing the relation).
void find_builds_list(const Game& g, int32_t x, int32_t y, int32_t search, int32_t who, int32_t radius, std::vector<Hit>& out) {
	out.clear();
	const Circles& C = circles();
	const int32_t k = (radius + 0x2ff) / 0x300;
	if (radius >= 0 && k <= 64 && C.radius[size_t(k)] <= 200) {  // num_def_builds = 200
		const int32_t wx = wcell(x), wy = wcell(y);
		for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
			const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
			if (!g.world.in_w(cx, cy)) continue;
			const WData& d = g.world.w(cx, cy);
			int32_t co = d.down, cw = d.down_who;
			while (co >= 0) {
				const Object* ob = g.object(cw, co);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				if (cw < 8 && search_valid_search(g, cw, search, who) && !ob->is_unit && ob->alive()) out.push_back({co, cw});
				co = no;
				cw = nw;
			}
		}
		return;
	}
	for (int32_t lw = 0; lw < kNumLeaders; lw++) {
		if (!(g.leaders[size_t(lw)].flags & leaderflag::kValid)) continue;
		if (!search_valid_search(g, lw, search, who)) continue;
		const ObjectList& L = g.objects[size_t(lw)];
		for (int32_t o = kBuildBase; o < L.build_mark; o++) {
			const Build* b = g.build(lw, o);
			if (!b || !b->alive()) continue;
			int32_t dx = iabs(b->x - x), dy = iabs(b->y - y);
			dx = std::max(0, dx - b->ptype->x_size * 192);
			dy = std::max(0, dy - b->ptype->y_size * 192);
			if (radius < 0 || vector_dist(dx, dy) <= radius) out.push_back({o, lw});
		}
	}
}

// ObjectsData::find_building_at(tx, ty, search, who, FILTER_ALL) @65ab40: the VALID, ACTIVE building of a leader < 8 whose
// footprint covers the tcoord (o, and its leader in *who_out).
int32_t find_building_at_tile(const Game& g, int32_t tx, int32_t ty, int32_t search, int32_t who, int32_t* who_out) {
	if (who_out) *who_out = -1;
	const World& w = g.world;
	if (!w.in_t(tx, ty) || (w.t(tx, ty) & 3) != 3) return -1;
	for (int32_t k = 0; k < 9; k++) {
		const int32_t wx = move_x[k] + (tx >> 2), wy = move_y[k] + (ty >> 2);
		if (!w.in_w(wx, wy)) continue;
		int32_t o = w.w(wx, wy).down, ow = w.w(wx, wy).down_who;
		while (o >= 0) {
			const Object* ob = g.object(ow, o);
			if (!ob) break;
			const int32_t no = ob->down, nw = ob->down_who;
			if (ow < 8 && !ob->is_unit && ob->alive() && (ob->flags & objflag::kActive) &&
			    (ow < 0 || search_valid_search(g, ow, search, who))) {
				const Footprint f = footprint(*ob->ptype, ob->x, ob->y);
				if (f.x0 <= tx && tx < f.x0 + f.w && f.y0 <= ty && ty < f.y0 + f.h) {
					if (who_out) *who_out = ow;
					return o;
				}
			}
			o = no;
			ow = nw;
		}
	}
	return -1;
}

}  // namespace

int32_t find_new_air_target(Game& g, Unit& u, int32_t x, int32_t y, int32_t type_o, int32_t type_who, int32_t* who_out) {
	// @5ebc70
	if (u.unit_masks & umask::kAiControl) {
		x = u.x;
		y = u.y;
	}
	const int32_t respond = RON_K(g, "aircraft_respond_range");
	std::vector<Hit> hits;
	find_units_list(g, u.x, u.y, search::kEnemy, u.who, respond * 0xc0, filter::kDomain, kDomainAir, 0, hits);
	int32_t best = -1, bo = -1, bw = -1;
	for (const Hit& h : hits) {
		if (!valid_target(g, u, h.o, h.who, nullptr)) continue;
		const Object* t = g.object(h.who, h.o);
		if (vector_dist(t->y - u.y, t->x - u.x) > respond * 0xc0) continue;
		const int32_t sc = compare_target(g, u, h.o, h.who, true, false);
		if (sc > best) {
			best = sc;
			bo = h.o;
			bw = h.who;
		}
	}
	if (best < 0) {
		if (vector_dist(y - u.y, x - u.x) > respond * 0x3c0) return -1;
		if (type_o >= 0) {
			find_units_list(g, x, y, search::kEnemy, u.who, respond * 0x3c0, filter::kTarget, type_o, type_who, hits);
			for (const Hit& h : hits) {
				if (!valid_target(g, u, h.o, h.who, nullptr)) continue;
				const int32_t sc = compare_target(g, u, h.o, h.who, true, false);
				if (sc > best) {
					best = sc;
					bo = h.o;
					bw = h.who;
				}
			}
		}
		if (best < 0) {
			find_units_list(g, x, y, search::kEnemy, u.who, respond * 0xc0, filter::kAll, 0, 0, hits);
			for (const Hit& h : hits) {
				if (!valid_target(g, u, h.o, h.who, nullptr)) continue;
				const Object* t = g.object(h.who, h.o);
				if (vector_dist(t->y - y, t->x - x) > respond * 0xc0) continue;
				const int32_t sc = compare_target(g, u, h.o, h.who, true, false);
				if (sc > best) {
					best = sc;
					bo = h.o;
					bw = h.who;
				}
			}
			if (best < 0) return -1;
		}
	}
	if (who_out) *who_out = bw;
	return bo;
}

int32_t find_new_bomber_target(Game& g, Unit& u, int32_t x, int32_t y, int32_t type_o, int32_t* who_out) {
	// @5eb960: buildings only, score compare_target / (distance to the point / 192 + 1).
	if (u.unit_masks & umask::kAiControl) {
		x = u.x;
		y = u.y;
	}
	const int32_t respond = RON_K(g, "bomber_respond_range");
	if (vector_dist(y - u.y, x - u.x) > respond * 0x3c0) return -1;
	std::vector<Hit> hits;
	int32_t best = -1, bo = -1, bw = -1;
	if (type_o >= 0) {
		find_builds_list(g, x, y, search::kEnemy, u.who, respond * 0x3c0, hits);
		for (const Hit& h : hits) {
			if (!valid_target(g, u, h.o, h.who, nullptr)) continue;
			const Object* t = g.object(h.who, h.o);
			const int32_t sc = compare_target(g, u, h.o, h.who, true, false) / (vector_dist(t->y - y, t->x - x) / 0xc0 + 1);
			if (sc > best) {
				best = sc;
				bo = h.o;
				bw = h.who;
			}
		}
	}
	if (best < 0) {
		find_builds_list(g, x, y, search::kEnemy, u.who, respond * 0xc0, hits);
		for (const Hit& h : hits) {
			if (!valid_target(g, u, h.o, h.who, nullptr)) continue;
			const Object* t = g.object(h.who, h.o);
			if (vector_dist(t->y - y, t->x - x) > respond * 0xc0) continue;
			const int32_t sc = compare_target(g, u, h.o, h.who, true, false) / (vector_dist(t->y - y, t->x - x) / 0xc0 + 1);
			if (sc > best) {
				best = sc;
				bo = h.o;
				bw = h.who;
			}
		}
		if (best < 0) {
			if (who_out) *who_out = -1;
			return -1;
		}
	}
	if (who_out) *who_out = bw;
	return bo;
}

// ---------------------------------------------------------------- STRAFE (5)

namespace {

// game.semaphore byte1 & 2 (a game mode the engine does not model; the AI's own gates read it too): off.
bool semaphore_byte1_bit2(const Game&) { return false; }

bool world_is_valid(const Game& g, int32_t x, int32_t y) {  // WorldData::is_valid(&x, &y)
	return x >= 0 && y >= 0 && x < g.world.tile_xs * 0xc0 && y < g.world.tile_ys * 0xc0;
}

void restrict_point(const Game& g, int32_t& x, int32_t& y) {  // WorldData::restrict
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= g.world.xs * 0x300) x = g.world.xs * 0x300 - 1;
	if (y >= g.world.ys * 0x300) y = g.world.ys * 0x300 - 1;
}

// The last waypoint of an AIR_PATROL order, following a carrier base for the Fighter-Bomber line.
void patrol_last_point(const Game& g, const Unit& u, const Order& P, int32_t* x, int32_t* y) {
	*x = P.wp.empty() ? 0 : P.wp.back()[0];
	*y = P.wp.empty() ? 0 : P.wp.back()[1];
	if (type_is(*u.ptype, airtype::kFighterBomber, false) && P.oxx >= 0) {
		const Object* base = slot(g, P.oxx, P.whose);
		if (base && base->alive()) {
			*x += base->x;
			*y += base->y;
			restrict_point(g, *x, *y);
		}
	}
}

// The firing cone and the fire itself shared by STRAFE and AIR_ATTACK_GROUND.
bool in_firing_cone(const Unit& u, uint32_t ad) {
	if (!is_missile_type(*u.ptype) && ad >= 0xaaaaaaau) {
		if (!type_is(*u.ptype, airtype::kJetFighter, false) || ad > 0x2aaaaaaau) return false;
	}
	return true;
}

}  // namespace

void do_strafe(Game& g, Unit& u, Order& S) {
	// @5eab00. S.returning (+0x3c) = no target, flying home.
	const Type& T = *u.ptype;
	const bool heli = is_heli_type(T);
	const bool bomber = type_is(T, airtype::kBomber, false);
	int32_t gx = -1, gy = -1;
	int32_t to = S.ox, twho = S.whom;
	const int32_t respond = RON_K(g, "aircraft_respond_range");
	if (S.returning == 0) {
		const bool ally_path = twho >= 0 && is_ally(g, u.who, twho);
		if (ally_path) {
			// Following an allied object (a rebase onto a friendly unit): retarget its captain when it is gone.
			Object* Tt = slot(g, to, twho);
			if (!Tt || !Tt->alive()) {
				int32_t cap = to;
				if (Tt && Tt->is_unit) {
					const Unit* cu = static_cast<const Unit*>(Tt);
					for (int guard = 0; cu->o_up >= 0 && guard < 64; guard++) {
						const Unit* p = g.unit(cu->who, cu->o_up);
						if (!p) break;
						cu = p;
					}
					cap = cu->o;
				}
				const Object* Ct = slot(g, cap, twho);
				if (!Ct || !Ct->alive()) {
					if (world_is_valid(g, S.xx, S.yy)) {
						const int32_t px = S.xx, py = S.yy, ho = S.oxx, hw = S.whose;
						kill_current_order(g, u, false);
						add_air_patrol_order(g, u, px, py, ho, hw, false);
						work(g, u);
						return;
					}
					S.returning = 1;
					S.ox = S.whom = -1;
					to = twho = -1;
				} else {
					S.ox = cap;
					S.whom = twho;
				}
			}
			if (S.returning == 0) {
				to = S.ox;
				twho = S.whom;
				if (to >= 0) {
					const Object* tt = slot(g, to, twho);
					if (tt) {
						gx = tt->x;
						gy = tt->y;
						S.xx = tt->x;
						S.yy = tt->y;
					}
					if ((u.o + g.frame) % 16 == 0) {
						int32_t nw = -1;
						const int32_t no = bomber ? find_new_bomber_target(g, u, gx, gy, to, &nw)
						                          : find_new_air_target(g, u, gx, gy, to, twho, &nw);
						if (valid_target(g, u, no, nw, nullptr)) {
							const int32_t ho = S.oxx, hw = S.whose;
							add_strafe_order(g, u, no, nw, ho, hw, 0, kQueueFirst, false);
							work(g, u);
							return;
						}
					}
				}
			}
		} else {
			if (!valid_target(g, u, to, twho, nullptr)) {
				if (is_missile_type(T)) {
					object_die(g, u, 0, -1, 0.0f);
					return;
				}
				if (heli) {
					const int32_t ho = S.oxx, hw = S.whose;
					kill_current_order(g, u, false);
					if (u.order_type() == kOrderNone) add_air_patrol_order(g, u, u.x, u.y, ho, hw, false);
					work(g, u);
					return;
				}
				if (u.orders.size() > 1) {
					kill_current_order(g, u, false);
					unit_set_anim(g, u, kCharWalk, 1, 1);
					work(g, u);
					return;
				}
				if (world_is_valid(g, S.xx, S.yy)) {
					const int32_t px = S.xx, py = S.yy, ho = S.oxx, hw = S.whose;
					kill_current_order(g, u, false);
					add_air_patrol_order(g, u, px, py, ho, hw, false);
					work(g, u);
					return;
				}
				S.returning = 1;
				S.ox = S.whom = -1;
			} else {
				const Object* tg = slot(g, to, twho);
				if (heli && is_in_range(g, u, to, twho, u.x, u.y, 0, nullptr)) {
					// A helicopter in range hovers 0x30 short of the target.
					const uint32_t a = find_angle(tg->x - u.x, tg->y - u.y);
					project(u.x, u.y, a, 0x30, &gx, &gy);
					restrict_point(g, gx, gy);
				} else {
					gx = tg->x;
					gy = tg->y;
					if (tg->is_unit && tg->ptype->domain == kDomainAir && !is_heli_type(*tg->ptype)) {
						const int32_t d = vector_dist(iabs(tg->y - u.y), iabs(tg->x - u.x));
						if (d > 0xc00) {
							project(tg->x, tg->y, static_cast<const Unit*>(tg)->angle, (d - 0xc00) / 3, &gx, &gy);
							restrict_point(g, gx, gy);
						}
					}
				}
			}
		}
	}
	// 3. the flight.
	if (!do_air_physics(g, u, S, gx, gy)) return;
	// 4. the fire gate; 5. the wrap-up.
	bool fire_path = false;
	if (u.recharging == 0 && valid_target(g, u, to, twho, nullptr)) fire_path = true;
	if (!fire_path) {
		bool walk = false;
		if (to >= 0 && twho >= 0) {
			const Object* tg = slot(g, to, twho);
			const uint32_t a = tg ? find_angle(tg->x - u.x, tg->y - u.y) : 0;
			const uint32_t d = u.angle - a;
			const uint32_t ad = d > 0x80000000u ? ~d : d;
			const uint32_t thr = type_is(T, airtype::kJetFighter, false) ? 0x40000000u : 0x15555555u;
			if (ad > thr) walk = !bomber;
		} else {
			walk = !bomber;
		}
		if (walk) unit_set_anim(g, u, kCharWalk, 0, 1);
	} else {
		const Object* tg = slot(g, to, twho);
		bool go = true;
		if (tg->ptype->domain != kDomainAir && u.orders.size() > 1 && S.mandatory == 0) {
			// Attacking a non-air target while other orders wait: the next order decides whether to give up.
			const Order& nx = u.orders[1];
			if (nx.type == kAirPatrol) {
				int32_t wx, wy;
				patrol_last_point(g, u, nx, &wx, &wy);
				if (vector_dist(tg->y - wy, tg->x - wx) > respond * 0x100) {
					kill_current_order(g, u, false);
					return;
				}
			} else if (nx.type == kStrafe) {
				const Object* xt = slot(g, nx.ox, nx.whom);
				if (xt && xt->alive()) {
					if (vector_dist(xt->y - tg->y, xt->x - tg->x) > respond * 0x100) {
						bool keep = false;
						if (tg->is_unit) {
							const Order* act = get_action(*static_cast<const Unit*>(tg));
							if (act && order_is_targeted(act->type) && act->ox == nx.ox && act->whom == nx.whom) keep = true;
						}
						if (!keep) {
							kill_current_order(g, u, false);
							unit_set_anim(g, u, kCharWalk, 1, 1);
							return;
						}
					}
				}
			}
		}
		if (go) {
			const uint32_t a = find_angle(tg->x - u.x, tg->y - u.y);
			const uint32_t d = u.angle - a;
			const uint32_t ad = d > 0x80000000u ? ~d : d;
			if (S.returning == 0 && is_in_range(g, u, to, twho, 0) && in_firing_cone(u, ad)) {
				if (!(T.unit_flags & utflag::kStrafing)) {
					if (bomber || g.fire_proj.empty() || g.fire_proj[size_t(u.type)] < 0) unit_set_anim(g, u, kCharAttack2, 0, 1);
					else fire_ammo(g, u, to, twho);
				} else {
					unit_set_anim(g, u, kCharAttack2, 0, 1);
				}
				if (is_missile_type(T)) {
					object_die(g, u, 0, -1, 0.0f);
					return;
				}
				u.recharging = uint8_t(unit_recharge(g, u, nullptr) + 1);
				if (bomber) u.mana_burn = int16_t(u.mana_burn + RON_K(g, "bombing_mana_cost"));
			}
		}
	}
	// 5. wrap-up (the order that was current may be gone after a kill above: S is only used while it is alive).
	if (u.orders.size() < 2) return;
	if (u.front() != &S) return;
	if (S.mandatory != 0) return;
	if ((T.mana != 0 && unit_mana_left(g, u) == 0) || to < 0 || twho < 0 || S.returning != 0) {
		kill_current_order(g, u, false);
		unit_set_anim(g, u, kCharWalk, 1, 1);
		return;
	}
	if ((g.frame + u.o * 2) % 32 != 0) return;
	const Order& nx = u.orders[1];
	auto give_up_walk = [&]() {
		kill_current_order(g, u, false);
		unit_set_anim(g, u, kCharWalk, 1, 1);
	};
	if (nx.type == kAirPatrol) {
		int32_t wx, wy;
		patrol_last_point(g, u, nx, &wx, &wy);
		int32_t nw = -1;
		const int32_t no = bomber ? find_new_bomber_target(g, u, wx, wy, -1, &nw) : find_new_air_target(g, u, wx, wy, -1, -1, &nw);
		if (valid_target(g, u, no, nw, nullptr)) {
			const Object* nt = slot(g, no, nw);
			S.ox = no;
			S.whom = nw;
			S.uid = nt->uid;
			S.returning = 0;
			return;
		}
		kill_current_order(g, u, false);
		return;
	}
	if (nx.type == kStrafe) {
		const int32_t xo = nx.ox, xw = nx.whom;
		const Object* xt = slot(g, xo, xw);
		if (!xt || !xt->alive()) {
			give_up_walk();
			return;
		}
		int32_t nw = -1;
		const int32_t no = bomber ? find_new_bomber_target(g, u, xt->x, xt->y, xo, &nw)
		                          : find_new_air_target(g, u, xt->x, xt->y, xo, xw, &nw);
		if (valid_target(g, u, no, nw, nullptr)) {
			const Object* nt = slot(g, no, nw);
			S.ox = no;
			S.whom = nw;
			S.uid = nt->uid;
			S.returning = 0;
			return;
		}
		give_up_walk();
		return;
	}
	kill_current_order(g, u, false);
}

// ---------------------------------------------------------------- AIR_PATROL / AIR_ATTACK_GROUND (5)

void do_air_patrol(Game& g, Unit& u, Order& P) {
	// @5ea620
	const Type& T = *u.ptype;
	const bool bomber = type_is(T, airtype::kBomber, false);
	if (int32_t(P.wp.size()) <= P.wp_index) P.wp_index = 0;
	int32_t gx = -1, gy = -1;
	if (P.returning == 0 && !P.wp.empty()) {
		gx = P.wp[size_t(P.wp_index)][0];
		gy = P.wp[size_t(P.wp_index)][1];
		if (P.oxx >= 0 && P.whose >= 0) {
			const Unit* home = g.unit(P.whose, P.oxx);
			if (home && home->alive() && !is_animal_who(u.who)) {  // a live unit (a carrier): the waypoints are offsets from it
				gx += home->x;
				gy += home->y;
				restrict_point(g, gx, gy);
			}
		}
	}
	if (!do_air_physics(g, u, P, gx, gy)) return;
	if (is_animal_who(u.who)) {  // a bird: no targeting, spell_time marks "airborne"
		if (u.spell_time == 0) u.spell_time = 1;
		return;
	}
	if (P.returning == 0) {
		if (vector_dist(u.y - gy, u.x - gx) < 0x240) {
			if (P.wp_index < int32_t(P.wp.size()) - 1) {
				P.wp_index++;
			} else if (u.orders.size() > 1) {
				kill_current_order(g, u, false);
				return;
			}
		}
		if (P.returning == 0 && (u.o + g.frame) % 16 == 0) {
			int32_t wx, wy;
			patrol_last_point(g, u, P, &wx, &wy);
			int32_t nw = -1, no;
			if (!bomber) {
				no = find_new_air_target(g, u, wx, wy, -1, -1, &nw);
				if (!valid_target(g, u, no, nw, nullptr) && semaphore_byte1_bit2(g))
					no = find_new_bomber_target(g, u, u.x, u.y, -1, &nw);
			} else {
				no = find_new_bomber_target(g, u, wx, wy, -1, &nw);
				if (!valid_target(g, u, no, nw, nullptr) && semaphore_byte1_bit2(g))
					no = find_new_air_target(g, u, u.x, u.y, -1, -1, &nw);
			}
			if (valid_target(g, u, no, nw, nullptr)) {
				const Object* nt = slot(g, no, nw);
				if (P.wp_index == int32_t(P.wp.size()) - 1 || nt->ptype->domain == kDomainAir) {
					const int32_t ho = P.oxx, hw = P.whose;
					add_strafe_order(g, u, no, nw, ho, hw, 0, kQueueFirst, false);
					return;
				}
			}
		}
	}
	if ((u.o + g.frame) % 32 == 0) {
		int32_t wx = P.wp.empty() ? 0 : P.wp[size_t(P.wp_index)][0];
		int32_t wy = P.wp.empty() ? 0 : P.wp[size_t(P.wp_index)][1];
		if (type_is(T, airtype::kFighterBomber, false) && P.oxx >= 0) {
			const Object* base = slot(g, P.oxx, P.whose);
			if (base && base->alive()) {
				wx += base->x;
				wy += base->y;
				restrict_point(g, wx, wy);
			}
		}
		int32_t bw = -1;
		const int32_t bo = find_building_at_tile(g, tcoord_of(wx), tcoord_of(wy), search::kEnemy, u.who, &bw);
		if (bo >= 0) {
			const Build* b = g.build(bw, bo);
			if (b && (b->ever_seen & (1u << (u.who & 31))) != 0) {
				const int32_t ho = P.oxx, hw = P.whose;
				add_strafe_order(g, u, bo, bw, ho, hw, 1, kQueueFirst, false);
			}
		}
	}
}

void do_air_attack_ground(Game& g, Unit& u, Order& A) {
	// @5ea420
	const int32_t tx = A.x, ty = A.y;
	if (!do_air_physics(g, u, A, tx, ty)) return;
	if (u.recharging != 0) return;
	const uint32_t a = find_angle(tx - u.x, ty - u.y);
	const uint32_t d = u.angle - a;
	const uint32_t ad = d > 0x80000000u ? ~d : d;
	if (A.returning != 0) return;
	if (!is_in_range_point(g, u, tx, ty, u.x, u.y)) return;
	if (!in_firing_cone(u, ad)) return;
	set_attack(g, u, -1, -1);
	bool animated = false;
	if (!(u.ptype->unit_flags & utflag::kStrafing)) {
		if (!type_is(*u.ptype, airtype::kBomber, false) && !g.fire_proj.empty() && g.fire_proj[size_t(u.type)] >= 0) {
			fire_ammo(g, u, -1, -1);
			animated = true;
		}
	}
	if (!animated) unit_set_anim(g, u, kCharAttack2, 0, 1);
	if (is_missile_type(*u.ptype)) {
		if (air_trace()) std::fprintf(stderr, "[air] f%d L%d missile %d detonates at %d,%d (target %d,%d, range %d tiles, raw %d, dist %d)\n", g.frame, u.who, u.o, u.x, u.y, tx, ty, obj_max_range(g, u), raw_max_range(g, *u.ptype), vector_dist(tx - u.x, ty - u.y));
		object_die(g, u, 0, -1, 0.0f);
		return;
	}
	u.recharging = uint8_t(unit_recharge(g, u, nullptr) + 1);
	if (type_is(*u.ptype, airtype::kBomber, false)) u.mana_burn = int16_t(u.mana_burn + RON_K(g, "bombing_mana_cost"));
}

// ---------------------------------------------------------------- the frame block of Unit::process (6)

void unit_process_air(Game& g, Unit& u) {
	// @610bc0: domain 2, not a decoy, not a caster. Airborne: one fuel point per frame while any is left. Based: the fuel
	// refills AIR_UNIT_MANA_RECHARGE per frame and the hit points recover every aircraft_heal_rate[heal level] frames (a
	// third faster for the Bomber line).
	const Type& T = *u.ptype;
	if (T.domain != kDomainAir || (u.unit_masks & umask::kDecoy) || (T.unit_flags2 & utflag2::kCaster)) return;
	if (u.inside_up < 0) {
		if (unit_mana_left(g, u) != 0) u.mana_burn = int16_t(u.mana_burn + 1);
		u.healing = 0;
		return;
	}
	const int32_t rec = RON_K(g, "air_unit_mana_recharge");
	if (rec < int32_t(u.mana_burn)) u.mana_burn = int16_t(u.mana_burn - rec);
	else u.mana_burn = 0;
	static const int off = constant_offset("aircraft_heal_rate");
	int32_t rate = off < 0 ? 1 : g.rules->constants.at(uint32_t(off + 4 * get_heal_level(g, u.who)));
	if (type_is(T, airtype::kBomber, false)) rate = (rate * 2 + 2) / 3;
	if (rate <= 0) return;
	if ((u.o + g.frame) % rate != 0) return;
	unit_repair_damage(g, u, 1, true, true);
	u.healing = 1;
}

// ---------------------------------------------------------------- launching (8)

namespace {

constexpr int32_t kStealthBomber = 306, kMissileDefenseBonus = 693;

bool list_has(const Object& c, int32_t o) { return std::find(c.launch_list.begin(), c.launch_list.end(), o) != c.launch_list.end(); }
void list_remove(Object& c, int32_t o) {
	auto it = std::find(c.launch_list.begin(), c.launch_list.end(), o);
	if (it != c.launch_list.end()) c.launch_list.erase(it);
}

// ObjectsData::find(x, y, SEARCH_FRIENDLY, who, 0x1800, ...): is one of the leader's objects within 0x1800 of the point.
bool own_object_within(const Game& g, int32_t x, int32_t y, int32_t who, int32_t radius) {
	const Circles& C = circles();
	const int32_t k = (radius + 0x2ff) / 0x300;
	const int32_t wx = wcell(x), wy = wcell(y);
	for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
		const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
		if (!g.world.in_w(cx, cy)) continue;
		int32_t co = g.world.w(cx, cy).down, cw = g.world.w(cx, cy).down_who;
		while (co >= 0) {
			const Object* ob = g.object(cw, co);
			if (!ob) break;
			const int32_t no = ob->down, nw = ob->down_who;
			if (cw < 8 && ob->alive() && cw == who && vector_dist(ob->x - x, ob->y - y) <= radius) return true;
			co = no;
			cw = nw;
		}
	}
	return false;
}

// "Defended": an enemy Air Defense building within 0x900 of the target (the STEALTHBOMBER tech ignores it).
bool air_defended(const Game& g, const Object& c, const Object& target) {
	if (has_tech(g, c.who, kStealthBomber)) return false;
	return objects_find_building(g, target.x, target.y, search::kEnemy, c.who, 0x900, 0, filter::kType, airtype::kAirDefense, 0) >= 0;
}

}  // namespace

void attempt_launch(Game& g, Object& C, int32_t x, int32_t y) {
	// @643a10
	if (C.launch_timer < kFramesBetweenLaunches) return;
	if (C.inside_down < 0 || !can_carry_air(C) || !C.launch_list.empty()) return;
	if (count_inside(g, C, kCountType, airtype::kBiplane) == 0) return;
	int32_t bo = -1, bw = -1;
	int32_t cur = C.inside_down, curw = C.inside_down_who;
	for (int guard = 0; curw >= 0 && cur >= 0 && guard < kUnitLimit; guard++) {
		Unit* v = g.unit(curw, cur);
		if (!v) break;
		if (!v->orders.empty() && v->mana_burn == 0) return;  // one is about to leave already
		if (type_is(*v->ptype, airtype::kBiplane, false) && v->mana_burn == 0) {
			bo = cur;
			bw = curw;
		}
		cur = v->inside_down;
		curw = v->inside_down_who;
	}
	if (bo < 0 || bw < 0) return;
	add_air_patrol_order(g, *g.unit(bw, bo), x, y, C.o, C.who, true);
}

void build_do_missile_launch(Game& g, Build& B) {
	// @622670: the silo's countdown (Build +0x7a, `recharging`) runs while missiles wait in the launch list.
	if (!type_is(*B.ptype, airtype::kSilo, false)) return;
	if (B.launch_list.empty()) {
		B.recharging = 0;
		return;
	}
	B.recharging = int16_t(B.recharging - 1);
	if (B.recharging != 0) return;
	const int32_t mo = B.launch_list.front();
	list_remove(B, mo);
	Unit* m = g.unit(B.who, mo);
	if (!m) return;
	const OrderType ot = m->order_type();
	if (ot == kAirAttackGround || (ot == kStrafe && !is_ally(g, B.who, B.who))) {
		if (type_is(*m->ptype, airtype::kNukeLine, false)) {
			B.visible = int8_t(0xff);  // Build +0x40: the whole map saw the nuke leave
			object_update_seen(g, B, false);
		}
		// (S_MISSILE_LAUNCHED / S_NUKE_LAUNCHED for the console seat, the "detected" notices for the others)
		if (type_is(*m->ptype, airtype::kNukeLine, false)) {
			GameEvent e;
			e.frame = g.frame, e.kind = nukeevent::kLaunched, e.who = B.who, e.o = mo, e.o2 = B.o, e.value = m->type;
			if (!m->orders.empty()) e.x = m->orders.front().x, e.y = m->orders.front().y;
			g.events.push_back(e);
		}
	}
	if (air_trace()) std::fprintf(stderr, "[air] f%d L%d silo %d launches missile %d\n", g.frame, B.who, B.o, mo);
	unit_come_out(g, *m, 0);
	if (m->alive()) unit_process(g, *m);  // vslot 0x9c: the missile processes once as it leaves
}

void do_launch(Game& g, Object& C) {
	// @64f3b0
	int32_t cur = C.inside_down, curw = C.inside_down_who;
	if (!(cur >= 0 && curw >= 0)) return;
	const bool is_unit_c = C.is_unit;
	if (!is_unit_c && static_cast<Build&>(C).recharging != 0) {
		build_do_missile_launch(g, static_cast<Build&>(C));
		return;
	}
	const int8_t old = C.launch_timer;
	C.launch_timer = int8_t(old + 1);
	if (old < kFramesBetweenLaunches) return;
	C.launch_timer = int8_t(kFramesBetweenLaunches);
	bool launched = false;
	for (int guard = 0; curw >= 0 && cur >= 0 && guard < kUnitLimit; guard++) {
		Unit* v = g.unit(curw, cur);
		if (!v) break;
		if (v->orders.empty() || v->mana_burn != 0) {
			// skipped
		} else if (has_repeat_air(C) || (v->orders.front().flags & orderflag::kExplicit)) {
			bool killed = false;
			if (v->order_type() == kStrafe) {
				Order& S = v->orders.front();
				if (S.ox < 0) {
					const int32_t ho = S.oxx, hw = S.whose;
					if ((ho != C.o || hw != C.who) && ho >= 0 && hw >= 0) {
						const Object* H = slot(g, ho, hw);
						if (H && num_aircraft_here(g, *H, 0) >= num_aircraft_limit(g, *H))
							add_air_patrol_order(g, *v, H->x, H->y, C.o, C.who, true);
					}
				} else if (!list_has(C, cur) && !valid_target(g, *v, S.ox, S.whom, nullptr) && !world_is_valid(g, S.xx, S.yy)) {
					kill_current_order(g, *v, false);
					killed = true;
				}
			}
			if (!killed) {
				if (v->order_type() == kAirPatrol) v->orders.front().wp_index = 0;
				if (!list_has(C, cur)) C.launch_list.push_back(cur);
				if (!launched) {
					if (!is_missile_type(*v->ptype)) {
						if (!is_unit_c) {
							trace(g, *v, "launches from", C.o, C.who);
							unit_come_out(g, *v, 0);
						}
					} else {
						// the silo's launch countdown: its own recharge (type +0x1f4)
						static_cast<Build&>(C).recharging = int16_t(C.ptype->recharge);
					}
					C.launch_timer = 0;
				}
				launched = true;
			}
		} else {
			kill_current_order(g, *v, false);
			list_remove(C, cur);
		}
		// (read after the processing: a plane that came out of a building has no successor any more)
		cur = v->inside_down;
		curw = v->inside_down_who;
	}
	// AI auto-launch: every 32 frames for airbases and carriers, every 128 for silos with missiles inside.
	if (g.leaders[size_t(C.who)].flags & leaderflag::kHuman) return;
	if ((g.frame + C.o) % 32 != 0 || (g.leaders[size_t(C.who)].flags2 & leaderflag2::kCombatAiOff)) return;
	const bool silo = type_is(*C.ptype, airtype::kSilo, false);
	int32_t best_leader = -1, best_obj = -1, best_score = -1;
	if (!silo) {
		for (int32_t p = 0; p < 8; p++) {
			const Leader& Lp = g.leaders[size_t(p)];
			if ((Lp.flags & 3) != 3) continue;
			// LeaderData::is_peace(self, p): both sides neither at war nor allied
			const int32_t dab = g.leaders[size_t(C.who)].diplos(p), dba = Lp.diplos(C.who);
			const bool peace = !(p == C.who || dab == 0 || dba == 0 || (dab == 2 && dba == 2));
			if (peace) continue;
			for (int32_t c = 0; c < Lp.city_mark && size_t(c) < g.cities[size_t(p)].size(); c++) {
				const City& Ci = g.cities[size_t(p)][size_t(c)];
				if ((Ci.city_flags & 3) != 3) continue;
				const Object* cb = slot(g, Ci.o, p);
				if (!cb) continue;
				if (air_defended(g, C, *cb)) continue;
				int32_t v = cb->damage / 100 + 1000;
				if (is_ally(g, p, C.who)) v *= 2;
				v = v / (vector_dist(cb->y - C.y, cb->x - C.x) / 0x300 + 1);
				if (v > best_score) {
					best_leader = p;
					best_obj = Ci.o;
					best_score = v;
				}
			}
			if (!is_enemy(g, p, C.who)) continue;
			for (const Fort& F : g.forts[size_t(p)]) {
				if (!(F.fort_flags & 1)) continue;
				const Object* fb = slot(g, F.o, p);
				if (!fb) continue;
				if (air_defended(g, C, *fb)) continue;
				const int32_t v = (fb->damage / 100 + 1000) / (vector_dist(fb->y - C.y, fb->x - C.x) / 0x300 + 1);
				if (v > best_score) {
					best_leader = p;
					best_obj = F.o;
					best_score = v;
				}
			}
			for (const Wonder& W : g.wonder_records[size_t(p)]) {
				if (!(W.wonder_flags & 1)) continue;
				const Object* wb = slot(g, W.o, p);
				if (!wb || type_is(*wb->ptype, airtype::kForbiddenCity, false)) continue;
				if (air_defended(g, C, *wb)) continue;
				int32_t v = wb->damage / 1000 + 1000;
				if (get_team_wonder_value(g, C.who) < get_enemy_wonder_value(g, C.who)) v *= 10;
				if (get_diff(g, C.who) > 2) {
					const int32_t ww = wonder_winning(g);
					if (ww >= 0 && !is_ally(g, C.who, ww)) v *= 100;
				}
				v = v / (vector_dist(wb->y - C.y, wb->x - C.x) / 0x300 + 1);
				if (v > best_score) {
					best_leader = p;
					best_obj = W.o;
					best_score = v;
				}
			}
		}
		if (best_obj < 0) return;
		const Object* target = slot(g, best_obj, best_leader);
		int32_t u_o = C.inside_down, u_w = C.inside_down_who;
		for (int guard = 0; u_o >= 0 && u_w >= 0 && guard < kUnitLimit; guard++) {
			Unit* v = g.unit(u_w, u_o);
			if (!v) break;
			const int32_t nxt = v->inside_down, nxtw = v->inside_down_who;
			if (v->ptype->domain == kDomainAir) {
				clear_orders(g, *v);
				const int32_t dist = vector_dist(target->y - C.y, target->x - C.x);
				const int32_t range = get_speed(g, *v, v->x, v->y, true) * unit_mana(g, *v);
				if (dist < range) {
					if (is_ally(g, best_leader, C.who) && !type_is(*v->ptype, airtype::kBiplane, false)) {
						// allies' cities get fighters only
					} else {
						add_air_patrol_order(g, *v, target->x, target->y, C.o, C.who, false);
					}
				}
			}
			u_o = nxt;
			u_w = nxtw;
		}
		return;
	}
	// Silo: every 128 frames, with a missile inside.
	if ((g.frame + C.o) % 128 != 0 || num_inside(g, C, 1) == 0) return;
	Unit* first = g.unit(C.inside_down_who, C.inside_down);
	if (!first) return;
	bool nuke = false;
	if (type_is(*first->ptype, airtype::kNukeLine, false)) {
		if (get_armageddon(g) - 2 <= g.armageddon) return;
		nuke = true;
	}
	const int32_t range = unit_mana(g, *first) * get_speed(g, *first, first->x, first->y, true);
	for (int32_t p = 0; p < 8; p++) {
		const Leader& Lp = g.leaders[size_t(p)];
		if (!(Lp.flags & 1) || !is_enemy(g, C.who, p)) continue;
		if (has_preq(g, p, kMissileDefenseBonus)) continue;
		auto consider = [&](int32_t v, const Object& tb) {
			const int32_t dist = vector_dist(tb.y - C.y, tb.x - C.x);
			if (dist <= range) {
				v = v / (dist / 0x1200 + 1);
				if (v > best_score) {
					best_leader = p;
					best_obj = tb.o;
					best_score = v;
				}
			}
		};
		for (int32_t c = 0; c < Lp.city_mark && size_t(c) < g.cities[size_t(p)].size(); c++) {
			const City& Ci = g.cities[size_t(p)][size_t(c)];
			if (!(Ci.city_flags & 1)) continue;
			const Build* cb = g.build(p, Ci.o);
			if (!cb || cb->ever_seen == 0) continue;
			if (nuke) {
				if (!own_object_within(g, cb->x, cb->y, C.who, 0x1800)) consider(city_num_buildings(g, p, c) * (obj_hits_left(g, *cb) + 1000), *cb);
			} else if (obj_hits_left(g, *cb) >= 500) {
				int32_t v = cb->damage + 1000;
				if (!(Ci.city_flags & 2)) {
					const int32_t r = g.rng.get(0, 0xffff);
					if (r % 10 != 0) continue;
					v = v / 50;
				}
				consider(v, *cb);
			}
		}
		for (const Fort& F : g.forts[size_t(p)]) {
			if (!(F.fort_flags & 1)) continue;
			const Build* fb = g.build(p, F.o);
			if (!fb || fb->ever_seen == 0) continue;
			if (nuke) {
				if (!own_object_within(g, fb->x, fb->y, C.who, 0x1800)) consider(obj_hits_left(g, *fb) + 1000, *fb);
			} else if (obj_hits_left(g, *fb) >= 500) {
				consider(fb->damage + 1000, *fb);
			}
		}
		for (const Wonder& W : g.wonder_records[size_t(p)]) {
			if (!(W.wonder_flags & 1)) continue;
			const Build* wb = g.build(p, W.o);
			if (!wb || type_is(*wb->ptype, airtype::kForbiddenCity, false) || wb->ever_seen == 0) continue;
			int32_t v = wb->damage / 100 + 1000;
			if (get_team_wonder_value(g, C.who) < get_enemy_wonder_value(g, C.who)) v *= 10;
			if (get_diff(g, C.who) > 2) {
				const int32_t ww = wonder_winning(g);
				if (ww >= 0 && !is_ally(g, C.who, ww)) v *= 100;
			}
			if (nuke && own_object_within(g, wb->x, wb->y, C.who, 0x1800)) continue;
			consider(v, *wb);
		}
		if (!nuke) {
			for (int32_t bo : Lp.mil_trainers) {
				const Build* tb = g.build(p, bo);
				if (!tb || !tb->alive() || tb->ever_seen == 0) continue;
				const int32_t dist = vector_dist(tb->y - C.y, tb->x - C.x);
				if (dist <= range) {
					const int32_t v = (obj_hits_left(g, *tb) + 1000) / (dist / 0x300 + 1);  // (these use 0x300, the others 0x1200)
					if (v > best_score) {
						best_leader = p;
						best_obj = tb->o;
						best_score = v;
					}
				}
			}
		}
	}
	if (best_obj < 0) return;
	const Object* target = slot(g, best_obj, best_leader);
	if (target && C.inside_down >= 0 && C.inside_down_who >= 0)
		add_air_attack_ground_order(g, *first, target->x, target->y, -1, -1, kQueueNew, true);
}

// ---------------------------------------------------------------- the carrier's launch events (8)

void carrier_launch_events(Game& g, Unit& c, Guy& gy) {
	// Guy::execute_events @5d99c0, the aircraft-carrier block, then GraphicEvents::execute_game_events @8e48e0 for the
	// type-5 (PLANERELEASE) events. Unit +0x5c (`trench_angle`) is the launch animation counter: bit 30 = one plane
	// (CHAR_ATTACK1), bit 31 = two or more (CHAR_ATTACK2), the low 30 bits the frame.
	if (!g.anims) return;
	int32_t cur_anim = gy.cur_anim;
	uint32_t cur_time = gy.cur_time;
	int32_t last_time = gy.cur_time == 0 ? -1 : gy.last_time;
	if ((c.trench_angle & 0xc0000000u) == 0 && !c.launch_list.empty()) c.trench_angle = c.launch_list.size() > 1 ? 0xc0000000u : 0x40000000u;
	if ((c.trench_angle & 0xc0000000u) != 0) {
		c.trench_angle += 1;
		cur_anim = (int32_t(c.trench_angle) < 0) + 0xb;
		const int32_t frames = g.anims->frames(gy.gpiece, cur_anim);
		cur_time = c.trench_angle & 0x3fffffffu;
		if (frames < int32_t(cur_time)) {
			cur_anim = gy.cur_anim;
			cur_time = gy.cur_time;
			c.trench_angle = 0;
			last_time = gy.cur_time == 0 ? -1 : gy.last_time;
		} else {
			last_time = int32_t(gy.last_time - int32_t(gy.cur_time)) + int32_t(cur_time);
		}
	}
	const std::vector<PlaneRelease>* evs = g.anims->plane_releases(gy.gpiece);
	if (!evs) return;
	for (const PlaneRelease& e : *evs) {
		if (e.anim != cur_anim || !(last_time < e.frame) || !(uint32_t(e.frame) <= cur_time)) continue;
		if (c.launch_list.empty()) continue;
		const int32_t head = c.launch_list.front();
		Unit* pl = head >= 0 ? g.unit(c.who, head) : nullptr;
		if (!pl || !pl->alive()) {
			list_remove(c, head);
			continue;
		}
		unit_come_out(g, *pl, 0);
		list_remove(c, head);
		if (!pl->alive()) continue;
		// ❓ The deck node position (GraphicPieces::get_position of the PLANERELEASE node at the event frame) is render data;
		// the plane appears at the carrier's centre with the event's angle added to the carrier's heading.
		unit_set_new_location(g, *pl, c.x, c.y, true, true);
		Guy& pg = pl->guys[0];
		pg.last_z = pg.z;
		pg.z = gy.z;
		const uint32_t deg = uint32_t(double(e.angle) * 4294967296.0 / 360.0);
		unit_set_angle(g, *pl, deg + c.angle, false);
		pg.last_pitch = pg.pitch;
		pg.pitch = 0.0f;
		trace(g, *pl, "released by the carrier", c.o, head);
	}
}

// ---------------------------------------------------------------- Build::train, gather points (2, 8)

int32_t build_train_air(Game& g, Build& B, Unit& U) {
	// @62f9b0, the branch of `obj_masks & 0x200` (aircraft-spec / production-spec 6.4.1).
	const Type& T = *U.ptype;
	const bool heli = is_heli_type(T), missile = is_missile_type(T);
	auto come_out_or_die = [&]() -> int32_t {
		if (unit_come_out(g, U, 0) != 0) {
			object_die(g, U, 0, -1, 0.0f);
			return 2;
		}
		return 0;
	};
	if (B.gather.empty()) return unit_is(g, U, airtype::kHelicopter) ? come_out_or_die() : 0;
	if (!missile && !heli) {
		// A plane: a patrol through the gather points (the first one makes the order, the others are appended).
		for (const GatherPoint& p : B.gather) {
			if (!world_is_valid(g, p.x, p.y)) continue;
			if (U.orders.empty()) {
				add_air_patrol_order(g, U, p.x, p.y, B.o, B.who, true);
			} else if (Order* f = U.front(); f && f->type == kAirPatrol) {
				f->wp.push_back({p.x, p.y});
			}
		}
		return 0;
	}
	const GatherPoint& p = B.gather.front();
	if (world_is_valid(g, p.x, p.y)) {
		int32_t bw = -1;
		const int32_t bo = find_building_at_tile(g, tcoord_of(p.x), tcoord_of(p.y), search::kAll, -1, &bw);
		if (bo >= 0 && (bo != B.o || bw != B.who)) {
			if (is_enemy(g, B.who, bw)) {
				if (missile && has_preq(g, bw, kMissileDefenseBonus)) return 1;  // (console message for the human seat)
				add_strafe_order(g, U, bo, bw, B.o, B.who, 1, kQueueNew, true);
				return 0;
			}
			const Object* fb = slot(g, bo, bw);
			if (fb && can_carry_unit(g, *fb, U.o, B.who) && fb->alive() && !missile) {
				add_strafe_order(g, U, -1, -1, bo, bw, 1, kQueueNew, true);
				return 0;
			}
		}
		if (!missile) add_air_patrol_order(g, U, p.x, p.y, B.o, B.who, true);
		return 0;
	}
	if (!heli) return 0;
	if (num_aircraft_here(g, B, 0) <= num_aircraft_limit(g, B)) return 0;
	return come_out_or_die();
}

void carrier_new_wing(Game& g, Unit& C) {
	unit_action_unqueue(g, C);  // Unit::action_unqueue(1): a new carrier has no queue, so a no-op
	const uint32_t seed0 = g.rng.seed();
	const int32_t f = current_upgrade(g, C.who, ptype::kFighterBomber);
	int32_t made = 0;
	for (int32_t i = 0; i < num_aircraft_limit(g, C); i++) {
		const int32_t a = init_unit(g, C.who, f, C.x, C.y, -1, -1, -1);
		if (a >= 0) {
			unit_go_inside(g, *g.unit(C.who, a), C.o, C.who);
			made++;
		}
	}
	if (air_trace()) {
		// How many game_random draws the wing took (the generator is a plain LCG: step a copy until it meets the seed).
		Random r(seed0);
		int32_t draws = 0;
		while (r.seed() != g.rng.seed() && draws < 256) r.next(), draws++;
		std::fprintf(stderr, "[air] f%d L%d carrier %d new wing: %d planes, %d game_random draws\n", g.frame, C.who, C.o, made, draws);
	}
}

void carrier_closed(Game& g, Unit& C) {
	// Unit::close @60ee50: the planes still queued on the carrier leave the leader's counters (no refund); a carrier that
	// closes turns the carrier-relative waypoints of the patrols based on it into absolute ones (the planes keep flying them).
	if (C.num_queued != 0) {
		track_queued(g, C.who, current_upgrade(g, C.who, airtype::kFighterBomber), -C.num_queued);
		C.num_queued = 0;
	}
	if (!type_is(*C.ptype, airtype::kCarrier, false)) return;
	const int32_t n = g.objects[size_t(C.who)].unit_mark;
	for (int32_t o = 0; o < n; o++) {
		Unit* v = g.unit(C.who, o);
		if (!v || !v->alive() || v->ptype->domain != kDomainAir || is_heli_type(*v->ptype) || v->orders.empty()) continue;
		Order& P = v->orders.front();
		if (P.type != kAirPatrol || P.oxx != C.o || P.whose != C.who) continue;
		for (auto& w : P.wp) {
			w[0] += C.x;
			w[1] += C.y;
		}
	}
}

void airbase_gather_cleared(Game& g, Build& B) {
	// Build::clear_gather @623180, after the list is emptied: every plane based here goes home (airborne) or drops its
	// orders and its place in the launch list (inside).
	if (!(B.build_masks & bmask::kHoldsAir) || !type_is(*B.ptype, airtype::kAirbase, false)) return;
	const ObjectList& L = g.objects[size_t(B.who)];
	for (int32_t i = 0; i < L.unit_mark; i++) {
		Unit* v = g.unit(B.who, i);
		if (!v || !v->alive() || v->ptype->domain != kDomainAir) continue;
		int32_t w = 0;
		if (unit_home_base(g, *v, &w) != B.o || w != B.who) continue;
		if (!v->on_map()) {
			clear_orders(g, *v);
			list_remove(B, i);
		} else {
			add_strafe_order(g, *v, -1, -1, B.o, B.who, 0, kQueueNew, false);
		}
	}
}

void airbase_gather_added(Game& g, Build& B) {
	// Build::add_gather_point @622e70: every plane based here gets its patrol through the gather list again (an object
	// rally point, action 3, is a strafe at that object).
	if (!(B.build_masks & bmask::kHoldsAir) || !type_is(*B.ptype, airtype::kAirbase, false)) return;
	const ObjectList& L = g.objects[size_t(B.who)];
	for (int32_t i = 0; i < L.unit_mark; i++) {
		Unit* v = g.unit(B.who, i);
		if (!v || !v->alive() || v->ptype->domain != kDomainAir) continue;
		int32_t w = 0;
		if (unit_home_base(g, *v, &w) != B.o || w != B.who) continue;
		int32_t alt = 0, sharp = 0;
		if (const Order* f = v->front(); f && order_is_air(f->type)) {
			alt = f->cruising_alt;
			sharp = f->sharp_turn;
		}
		bool have_patrol = false;
		for (const GatherPoint& p : B.gather) {
			if (p.action == 3) {
				add_strafe_order(g, *v, p.x, p.y, B.o, B.who, 1, kQueueNew, false);
			} else if (!have_patrol) {
				add_air_patrol_order(g, *v, p.x, p.y, B.o, B.who, true);
				have_patrol = true;
			} else if (Order* f = v->front(); f && f->type == kAirPatrol) {
				f->wp.push_back({p.x, p.y});
			}
		}
		if (have_patrol && (alt != 0 || sharp != 0)) {
			if (Order* f = v->front(); f && f->type == kAirPatrol) {
				f->cruising_alt = alt;
				f->sharp_turn = sharp;
			}
		}
	}
}

// ---------------------------------------------------------------- harness

std::vector<std::string> air_describe(const Game& g, int32_t who) {
	static const char* const kNames[] = {"NONE", "MOVE_TO", "ATTACK_TO", "EXPLORE_TO", "FLEE_TO", "PATROL", "BUILD_AT", "GATHER",
	                                     "BOARD_SHIP", "AWAIT_BOARD", "ATTACK", "FOLLOW", "GUARD", "REPAIR", "CAST_SPELL",
	                                     "TRADE_ROUTE", "STRAFE", "AIR_PATROL", "CHANGE_FORM", "GROUP_MOVE", "GROUP_ATTACK",
	                                     "GROUP_ATTACK_TO", "GROUP_PATROL", "ATTACK_GROUND", "AIR_ATTACK_GROUND", "SPECIAL_ANIM",
	                                     "GARRISON", "THINK"};
	std::vector<std::string> out;
	if (who < 0 || who >= kNumLeaders || !(g.leaders[size_t(who)].flags & leaderflag::kValid)) return out;
	char buf[400];
	const ObjectList& L = g.objects[size_t(who)];
	for (int32_t o = 0; o < L.build_mark; o++) {
		const Object* ob = g.object(who, o);
		if (!ob || !ob->alive() || !ob->ptype || !can_carry_air(*ob)) continue;
		std::string inside;
		int32_t cur = ob->inside_down, curw = ob->inside_down_who;
		for (int32_t guard = 0; cur >= 0 && curw >= 0 && guard < kUnitLimit; guard++) {
			const Unit* v = g.unit(curw, cur);
			if (!v) break;
			inside += (inside.empty() ? "" : ",") + std::to_string(cur);
			cur = v->inside_down;
			curw = v->inside_down_who;
		}
		std::string ll;
		for (int32_t l : ob->launch_list) ll += (ll.empty() ? "" : ",") + std::to_string(l);
		std::snprintf(buf, sizeof buf, "base %d:%-4d type 0x%x (%d,%d) here %d (with queue %d) / %d timer %d launch [%s] inside [%s]", who, o,
		              ob->type, ob->x, ob->y, num_aircraft_here(g, *ob, 0), num_aircraft_here(g, *ob, 1), num_aircraft_limit(g, *ob),
		              int(ob->launch_timer), ll.c_str(), inside.c_str());
		out.emplace_back(buf);
	}
	for (int32_t o = 0; o < L.unit_mark; o++) {
		const Unit* u = g.unit(who, o);
		if (!u || !u->alive() || !u->ptype || u->ptype->domain != kDomainAir) continue;
		int32_t bw = -1;
		const int32_t bo = unit_home_base(g, *u, &bw);
		const Order* f = u->front();
		std::string ord = f ? (size_t(f->type) < sizeof kNames / sizeof *kNames ? kNames[size_t(f->type)] : "?") : "-";
		if (f && f->type == kSpecialAnim) ord += f->sa_type == 0 ? "(land)" : "(takeoff)";
		const int32_t fuel = unit_mana_left(g, *u);
		std::snprintf(buf, sizeof buf,
		              "air  %d:%-4d type 0x%x (%6d,%6d z%5d) fuel %d/%d burn %d base %d:%d %s%s order %s%s%s alt %d sharp %d%s",
		              who, o, u->type, u->x, u->y, u->guys.empty() ? 0 : u->guys[0].z, fuel, unit_mana(g, *u), int(u->mana_burn),
		              bo, bw, u->on_map() ? "AIR" : "INSIDE", unit_is_flying_low(g, *u) ? "/low" : unit_is_flying_high(g, *u) ? "/high" : "", ord.c_str(), f && f->type == kStrafe && f->ox >= 0 ? " ->" : "",
		              f && f->type == kStrafe && f->ox >= 0 ? (std::to_string(f->whom) + ":" + std::to_string(f->ox)).c_str() : "",
		              f ? f->cruising_alt : 0, f ? f->sharp_turn : 0, f && order_is_air(f->type) && f->returning ? " RETURNING" : "");
		out.emplace_back(buf);
	}
	return out;
}

}  // namespace ron::sim
