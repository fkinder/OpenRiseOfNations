// Range, target validity and target acquisition (docs/combat-spec.md sections 2 and 3.6): attack_dist,
// is_in_range, valid_target(_const), check_target, poor_target, compare_target, find_nearby_target,
// find_melee_target, think_attack and the combat part of Unit::think, find_new_target, target_opportunity and its
// group form.
#include <algorithm>

#include "sim/aircraft.hpp"
#include "sim/ai.hpp"
#include "sim/combat_internal.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

using namespace combat_detail;

// Fog of war (sim/fog.hpp, docs/fog-spec.md 7.2): ObjectData::is_seen vslot 0x48 (p2 = ignore the cloak test).
bool obj_is_seen(const Game& g, const Object& t, int32_t who, int32_t p2) { return object_is_seen(g, t, who, p2 != 0); }

namespace {

bool tree_at(const Game& g, int32_t x, int32_t y) {
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	return g.world.in_t(tx, ty) && (g.world.t(tx, ty) & tmask::kMedium) == tmask::kTree;
}

// UnitData::is_flying_high / is_flying_low (@60a310 / @60a140): sim/aircraft.cpp.
bool is_flying_high(const Game& g, const Object& t) { return t.is_unit && unit_is_flying_high(g, static_cast<const Unit&>(t)); }
bool is_flying_low(const Game& g, const Object& t) { return t.is_unit && unit_is_flying_low(g, static_cast<const Unit&>(t)); }

// The order a unit's (non-facing) scans treat as its current mandatory attack.
const Order* front_attack(const Unit& u) {
	const Order* f = u.front();
	return f && order_is_attack(f->type) ? f : nullptr;
}

int32_t health_level(const Game& g, const Object& ob) {
	// ObjectData::health_level @6534b0 (units: total damage of the chain; TODO: UnitData::total_damage, the
	// member's own damage is used).
	const int32_t h = obj_hits(g, ob);
	int32_t left = h - ob.damage;
	if (left < 0 || h < 0) left = 0;
	else if (h < left) left = h;
	if (h * 90 / 100 <= left) return 0;
	if (h * 75 / 100 <= left) return 1;
	if (left < h * 50 / 100) {
		if (h * 25 / 100 <= left) return 3;
		if (left < h * 10 / 100) return (left < 1) + 5;
		return 4;
	}
	return 2;
}

bool is_defensive(const Game& g, const Object& w) {
	(void)g;
	// WallData::is_defensive @473650: towers, forts, airbases, or any armed building.
	return type_is(*w.ptype, 0x1b7) || type_is(*w.ptype, 0x1bb) || type_is(*w.ptype, 0x1bf) || w.ptype->attack != 0;
}

}  // namespace

bool build_is_training(const Game& g, int32_t t);
bool build_is_military_trainer(const Game& g, int32_t t);

// ---------------------------------------------------------------- range

int32_t attack_dist(const Game& g, const Object& s, int32_t ox, int32_t whom, int32_t x, int32_t y) {
	// @6488f0
	const Object* tp = obj_at(g, ox, whom);
	if (!tp || !tp->ptype) return 0x7fffffff;
	const Object& t = *tp;
	int32_t dx = iabs(uc(x) - uc(t.x)), dy = iabs(uc(y) - uc(t.y));
	if (s.is_unit && unit_is_plane(static_cast<const Unit&>(s)) && !(uint32_t(s.ptype->obj_masks) & objmask::kMissile))
		return vector_dist(dx, dy);
	auto shrink = [](int32_t& v, int32_t h) { v = v > h ? v - h : 0; };
	if (is_wall(t)) {
		shrink(dx, t.ptype->x_size * 96);
		shrink(dy, t.ptype->y_size * 96);
	} else {
		const int32_t r = t.ptype->block_radius + 24;
		shrink(dx, r);
		shrink(dy, r);
	}
	if (is_wall(s)) {
		shrink(dx, s.ptype->x_size * 96);
		shrink(dy, s.ptype->y_size * 96);
	} else {
		const int32_t r = s.ptype->block_radius + 24;
		shrink(dx, r);
		shrink(dy, r);
	}
	return vector_dist(dx, dy);
}

int32_t attack_dist(const Game& g, const Object& s, int32_t ox, int32_t whom) {
	return attack_dist(g, s, ox, whom, s.x, s.y);
}

bool is_in_range(const Game& g, const Object& s, int32_t ox, int32_t whom, int32_t x, int32_t y, int32_t slack,
                 int32_t* dist) {
	// @6486b0
	const Object* tp = obj_at(g, ox, whom);
	if (!tp || !tp->alive()) return false;
	if (tp->is_unit && !static_cast<const Unit*>(tp)->on_map()) return false;
	if (tree_at(g, x, y)) return false;
	const int32_t d = attack_dist(g, s, ox, whom, uc(x), uc(y));
	if (dist) *dist = d;
	if (raw_max_range(g, *s.ptype) == 0) {
		const int32_t limit = type_is(*s.ptype, 132) ? 0xf6 : 0x66;
		return d <= limit;
	}
	const int32_t mn = obj_min_range(g, s);
	if (d < mn * 192 - 6) {
		const int32_t a = s.is_unit ? s.ptype->big_radius : 0;
		const int32_t b = tp->is_unit ? tp->ptype->big_radius : 0;
		if (a + d + b < mn * 192 - 6) return false;
	}
	return d + (slack ? 144 : 0) <= obj_max_range(g, s) * 192 + 6;
}

bool is_in_range(const Game& g, const Object& s, int32_t ox, int32_t whom, int32_t slack) {
	return is_in_range(g, s, ox, whom, s.x, s.y, slack, nullptr);
}

bool is_in_range_point(const Game& g, const Object& s, int32_t tx, int32_t ty, int32_t sx, int32_t sy) {
	// @64e4a0
	// A MISSILE that is not inside a container (get_inside(0) < 0, the JS at 64e4da) is in range of any point: it fires its
	// ground ammo and dies the first frame it runs (aircraft-spec 7; the exe contradicts combat-spec's "inside" reading).
	if ((uint32_t(s.ptype->obj_masks) & objmask::kMissile) && s.is_unit && static_cast<const Unit&>(s).on_map())
		return true;
	int32_t d = vector_dist(tx - sx, ty - sy);
	if (s.is_unit) d = std::max(d - s.ptype->big_radius, 0);
	else d = std::max(d - (s.ptype->x_size + s.ptype->y_size) * 48, 0);
	if (raw_max_range(g, *s.ptype) == 0) return d <= 102;
	if (d < obj_min_range(g, s) * 192 - 6) return false;
	return d <= obj_max_range(g, s) * 192 + 6;
}

// ---------------------------------------------------------------- validity

bool valid_target_const(const Game& g, const Object& s, int32_t ox, int32_t whom) {
	// @6472c0
	if (ox < 0 || whom < 0 || whom > 7 || whom == s.who || !is_enemy(g, s.who, whom)) return false;
	const Object* tp = obj_at(g, ox, whom);
	if (!tp || !tp->alive() || !tp->ptype) return false;
	const Object& t = *tp;
	if (!obj_is_seen(g, t, s.who, 0)) return false;
	if (t.is_unit && !static_cast<const Unit&>(t).on_map()) return false;
	if (s.is_unit && (s.ptype->unit_flags & (utflag::kFireship | utflag::kSubmarine))) {
		if (t.ptype->domain != kDomainSea || is_wall(t)) return false;
	}
	if (obj_max_range(g, s) == 0 && tree_at(g, t.x, t.y)) return false;
	const uint32_t smask = uint32_t(s.ptype->obj_masks);
	if (t.ptype->domain == kDomainAir && t.is_unit) {
		if (raw_max_range(g, *s.ptype) == 0) return false;
		if (t.ptype->unit_flags & utflag::kHelicopter) {
			if (type_is_siege(*s.ptype) || type_is_tank(*s.ptype) || (smask & objmask::kMissile)) return false;
			return !type_is(*s.ptype, 304);
		}
		if (type_is(*t.ptype, 306) && s.ptype->domain != kDomainAir && !can_carry_air(s)) return false;
		const int32_t sh = s.ptype->fly_high, sl = s.ptype->fly_low, th = t.ptype->fly_high, tl = t.ptype->fly_low;
		if (!(smask & objmask::kAntiAir)) {
			if (sh == 0 && sl == 0) return false;
			if (th == 0 && (tl == 0 || is_flying_high(g, t))) return false;
			if (th != 0 && tl == 0 && is_flying_low(g, t)) return false;
			if (sh == 0 && is_flying_high(g, t)) return false;
			if (sl == 0 && is_flying_low(g, t)) return false;
		} else if (s.ptype->domain != kDomainAir) {
			if (sh == 0) {
				if (sl == 0) return false;
				if (is_flying_high(g, t)) return false;
			}
			if (sl == 0 && is_flying_low(g, t)) return false;
		}
		return !((uint32_t(t.ptype->obj_masks) & objmask::kMissile) || (smask & objmask::kMissile));
	}
	if (t.ptype->domain == kDomainSea && t.is_unit && (t.ptype->unit_flags & utflag::kSubmarine) &&
	    s.ptype->domain == kDomainGround)
		return false;
	if ((!s.is_unit || s.ptype->domain == kDomainGround) && (smask & objmask::kAntiAir)) return false;
	return true;
}

bool check_capture_eligible(const Game& g, const Object& t) {
	// @62d1d0
	if (t.is_unit || !is_city_type(*t.ptype) || !(t.flags & objflag::kActive)) return false;
	return (t.flags & objflag::kCity) ? health_level(g, t) > 5 : health_level(g, t) > 4;
}

// Build::check_capture @6276a0: sim/capture.cpp (docs/capture-spec.md).

bool valid_target(Game& g, Object& s, int32_t ox, int32_t whom, int32_t* capturable) {
	// @648ba0
	if (capturable) *capturable = 0;
	if (!valid_target_const(g, s, ox, whom)) return false;
	Object& t = *obj_at(g, ox, whom);
	if (t.alive() && is_build_active(t) && check_capture_eligible(g, t)) {
		if (capturable) *capturable = 1;
		if (!(uint32_t(s.ptype->obj_masks) & objmask::kMissile)) {
			if (check_capture(g, t, s.o, s.who) && capturable) *capturable = 0;
			const Unit* su = as_unit(&s);
			if (su && su->on_map() && has_objmask(s, objmask::kVehicle) && has_objmask(s, objmask::kWarMachine) &&
			    su->order_type() == kAttack && su->front()->ox == ox && su->front()->whom == whom &&
			    su->front()->mandatory)
				return true;
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------- scoring

bool poor_target(Game& g, Object& s, int32_t ox, int32_t whom) {
	// @64a270
	const Object* tp = obj_at(g, ox, whom);
	if (!s.is_unit || !tp || !tp->is_unit) return false;
	const Unit& t = static_cast<const Unit&>(*tp);
	const Unit& su = static_cast<const Unit&>(s);
	if (!t.guys.empty() && (t.guys[0].guy_flags & 0x40)) {
		if (!has_objmask(s, objmask::kAntiAir)) return true;
		return attack_dist(g, s, ox, whom) > obj_max_range(g, s) * 192;
	}
	if (order_is_move(t.order_type())) {
		const int32_t ts = get_speed(g, t, t.x, t.y, false), ms = get_speed(g, su, su.x, su.y, false);
		if (ts > ms) {
			const uint32_t a = find_angle(t.x - s.x, t.y - s.y);
			const uint32_t d = t.angle - a - 0x80000000u;
			if (d < 0x2aaaaaaau) return false;
			if (flanking(d) >= 2) {
				const int32_t lim = (s.ptype->role & role::kRanged) ? obj_max_range(g, s) * 192 : 192;
				return attack_dist(g, s, ox, whom) > lim;
			}
		}
	}
	return false;
}

int32_t compare_target(Game& g, Object& s, int32_t ox, int32_t whom, bool unchecked, bool easy) {
	// @64e5c0
	Object& t = *obj_at(g, ox, whom);
	const bool isB = !t.is_unit;
	const bool act = t.is_unit ? t.alive() : (t.flags & objflag::kActive) != 0;
	const bool aaB = isB && has_objmask(t, objmask::kAntiAir);
	const Unit* su = as_unit(&s);
	const bool human_s = leader_human(g, s.who);
	bool raid = false, aiS = false, seaS = false;
	if (su && get_combat_stance(g, s) == kRaid) {
		raid = true;
		aiS = unit_ai(*su);
		seaS = s.ptype->domain == kDomainSea;
		if (aiS && seaS) raid = !has_objmask(s, objmask::kSiege);
	}
	int32_t v = 4 * sum_rules_cost(g, t.type);
	if (obj_is_wonder(t)) v /= 25;
	if (act && isB) {
		if (aiS && seaS) {
			if (s.ptype->unit_flags & utflag::kFireWhileMoving) return 0;
			if (type_is(*t.ptype, 432) && !has_objmask(s, objmask::kSiege)) return 0;
		}
		if (easy) v *= 10;
		if (!raid && !human_s) {
			if (t.flags & objflag::kCity) {
				if (easy) return 100;
				v *= 5;
			} else if (is_defensive(g, t) && !aaB) {
				v *= 4;
				if (easy) {
					v *= 5;
					v *= 2;
				}
			} else if (build_is_military_trainer(g, t.type)) {
				v *= 3;
				const Build& tb = static_cast<const Build&>(t);
				if (!easy && type_is(*t.ptype, 520) && (tb.queued != 0 || tb.inside_down >= 0)) v *= 5;
				if (seaS && aiS && has_objmask(s, objmask::kSiege) && type_is(*t.ptype, 432)) v *= 5000;
			} else if (build_is_training(g, t.type)) {
				v *= 2;
			}
		}
	}
	if (t.ptype->attack != 0) {
		const int32_t hl = obj_hits_left(g, t);
		if (hl != 0 && !aaB) {
			if (act && raid && isB) {
				v /= 20;
			} else {
				v = obj_attack(g, t) * v * 100 / hl;
				if (isB) v *= 10;
			}
		}
	}
	if (!s.is_unit) {
		const Build& sb = static_cast<const Build&>(s);
		if (sb.attack_ox == ox && sb.attack_whom == whom) {
			if (get_garrison_arrows(g, sb) > 1) v /= 2;
			else v *= 2;
		}
		if (t.damage != 0) v = v * 3 / 2;
		if (t.is_unit && unit_is_moving(static_cast<const Unit&>(t))) v /= 4;
		if (t.is_unit && (t.ptype->unit_flags2 & utflag2::kSupply)) v *= 5000;
	}
	const int32_t dmg = get_damage(g, s, ox, whom, find_angle(t.x - s.x, t.y - s.y), false, false, nullptr);
	if (easy) {
		if (dmg == 0) return 0;
		v = v / dmg;
	} else {
		v = dmg * v;
	}
	const bool decoy = t.is_unit && (static_cast<const Unit&>(t).unit_masks & umask::kDecoy);
	if (isB) {
		if (!raid) {
			int32_t a = t.ptype->attack;
			if (aaB && s.ptype->domain != kDomainAir) a = 0;
			const bool empty_city = (t.flags & objflag::kCity) && num_inside(g, t, 0) == 0;
			if (!empty_city) {
				if (a != 0 && !human_s) {
					if (has_objmask(s, objmask::kSiege)) v += 1000000;
					else v *= 5;
				}
				if (has_objmask(s, objmask::kSiege) && a != 0) v += 100000;
			}
		}
	} else {
		const Unit& U = static_cast<const Unit&>(t);
		const Type& ut = *U.ptype;
		if (ut.role & role::kCombat) v *= 20;
		if (ut.unit_flags2 & utflag2::kCaster) {
			const Order* ua = get_action(U);
			if (ua && ua->type == kCastSpell) {
				v *= 20;
				if (ua->ox == s.o && ua->whom == s.who) v += 10000000;
			} else if (type_is(ut, 58)) {
				v += 6000000;
			}
		}
		if (decoy && unit_is_detected(g, U, s.who)) {
			v /= 4;
		} else if (!raid) {
			if (ut.role & role::kCombat) v += 1000000;
			else if (ut.unit_flags2 & utflag2::kSupply) v += !s.is_unit ? 4000000 : 100000;
			else v += 100000;
		} else if (!aiS) {
			if (U.type == 50 || U.type == 51 || (ut.unit_flags2 & utflag2::kCaravan)) v += 9000000;
			else v /= 10;
		} else if (!seaS) {
			if (U.type == 50 || U.type == 51 || (ut.unit_flags2 & utflag2::kCaravan)) v += 900000;
			else if (ut.role & role::kCombat) v += 10000;
			else v /= 10;
		} else {
			if (type_is(*s.ptype, 336)) {
				if (type_is(ut, 317)) return 0;
				if (ut.unit_flags & utflag::kFireWhileMoving) v /= 10;
			} else if ((s.ptype->unit_flags & utflag::kFireWhileMoving) && type_is(ut, 336)) {
				v += 1000000;
			} else if (ut.unit_flags & utflag::kTransport) {
				v += 900000;
			} else if (type_is(ut, 317)) {
				v += 100000;
			} else if (ut.role & role::kCombat) {
				v += 10000;
			}
		}
		if (s.ptype->domain == kDomainGround && has_objmask(s, objmask::kSiege) && ut.domain != kDomainSea &&
		    !(uint32_t(ut.obj_masks) & objmask::kSiege))
			v /= 10000;
		if (ut.domain == kDomainSea && s.ptype->domain == kDomainSea && type_is(*s.ptype, 323)) {
			if (type_is(ut, 340)) v /= 400;
			else if (ut.unit_flags & utflag::kTransport) v *= 40;
		}
		v /= (U.full + 1);
	}
	if ((t.flags & objflag::kCity) && act && obj_hits_left(g, t) == 0) v = 99999;
	else if (v < 0) v = 9999999;
	if (unchecked && !raid && !is_in_range(g, s, ox, whom, 0)) v /= 5;
	int32_t p = (v + 99) / 100;
	if (p > 100000) p = (p - 99996) / 5 + 100000;
	p = std::max(p, 15);
	if (decoy) p = 1;
	if (t.ptype->domain == kDomainAir && !has_objmask(s, objmask::kAntiAir)) p = 2;
	return p;
}

bool check_target(Game& g, Object& s, int32_t ox, int32_t whom, bool duty, int32_t* dist, bool guarding,
                  bool use_poor, bool facing) {
	// @649e00
	if (dist) *dist = attack_dist(g, s, ox, whom);
	Object& t = *obj_at(g, ox, whom);
	if (s.is_unit && !facing) {
		Unit& su = static_cast<Unit&>(s);
		const int32_t ttx = tcoord_of(t.x), tty = tcoord_of(t.y), stx = tcoord_of(s.x), sty = tcoord_of(s.y);
		bool diff = false;
		if (g.world.in_t(ttx, tty) && g.world.in_t(stx, sty))
			diff = g.world.get_tregion(ttx, tty) != g.world.get_tregion(stx, sty);
		if (unit_ai(su) && has_objmask(s, objmask::kSiege) && s.ptype->domain == kDomainSea) diff = false;
		if ((!duty && get_combat_stance(g, s) == kDefensive && update_order(su) != nullptr) || diff) {
			if (!is_in_range(g, s, ox, whom, 0)) return false;
		}
		if (guarding) {
			const Order* gact = get_activity(su);
			if (gact && gact->type == kGuard && gact->ox >= 0 && gact->whom >= 0) {
				int32_t mult = 2;
				if (t.is_unit && !is_worker_type(t.type) && unit_ai(su)) mult = 3;
				if (su.o_up >= 0) {
					const Unit* cap = g.unit(su.who, obj_captain(g, su));
					const Order* ca = cap ? get_action(*cap) : nullptr;
					if (ca && ca->type == kAttack && ca->ox == ox && ca->whom == whom) return true;
				}
				const int32_t Rg = RON_K(g, "unit_guard_respond_range") * mult * 96;
				// guard point: GuardOrder +0x1c/+0x20 (kept in Order.x / Order.y here)
				if (vector_dist(t.x - gact->x, t.y - gact->y) > Rg || vector_dist(s.x - gact->x, s.y - gact->y) > Rg)
					return false;
			}
		}
	}
	if (is_wall(t) && !(t.ptype->build_flags & 0x10)) {
		const int32_t wx = wcoord_of(t.x), wy = wcoord_of(t.y);
		if (g.world.in_w(wx, wy) && g.world.w(wx, wy).who == -1) return false;
	}
	if (use_poor && !facing && poor_target(g, s, ox, whom)) return false;
	return true;
}

// ---------------------------------------------------------------- the scan

int32_t find_nearby_target(Game& g, Object& s, int32_t r, int32_t* whom, bool act, bool facing, int32_t filter) {
	// @648da0
	Unit* su = as_unit(&s);
	const bool mr_and_r = su && (s.ptype->unit_flags & utflag::kMeleeAndRanged);
	*whom = -1;
	if (obj_attack(g, s) == 0) return -1;
	int32_t minr = 0, maxr = 0;
	bool ai = false;
	int32_t cur_o = -1, cur_w = -1;
	if (su) {
		minr = obj_min_range(g, s) * 192;
		maxr = obj_max_range(g, s) * 192;
		ai = unit_ai(*su);
		if (!facing) {
			const Order* f = front_attack(*su);
			if (f && f->mandatory) {
				cur_o = f->ox;
				cur_w = f->whom;
			}
		}
	}
	bool easy = false;
	if (!leader_human(g, s.who) && get_diff(g, s.who) == 0) {
		easy = true;
		filter = filter == 1 ? 0 : filter;
	}
	int32_t R;
	if (r == 0) {
		R = 32;
	} else {
		R = (r + 767) / 768;
		if (!s.is_unit) R++;
		if (has_objmask(s, objmask::kAntiAir)) R++;
		if (su && get_combat_stance(g, s) == kRaid) R++;
		R = std::min(R, 32);
	}
	bool duty = false, must_range = true, guarding = false, ai_siege = false;
	if (su && !facing) {
		duty = on_duty(*su);
		const Order* actv = get_activity(*su);
		guarding = actv && actv->type == kGuard;
		must_range = get_combat_stance(g, s) == kStandGround ||
		             ((su->unit_masks & umask::kEntrenched) && !(su->unit_masks2 & umask2::kMarineEntrench));
		if (s.ptype->unit_flags2 & utflag2::kPacker) {
			if (!(su->unit_masks & umask::kPacked)) {
				must_range = true;
			} else if (!leader_human(g, s.who) && !easy && type_is_siege(*s.ptype)) {
				must_range = true;
				ai_siege = true;
				// TODO(AI armies): the army fast path (get_army() >= 0 with a valid army target in reach).
			}
		}
	}
	int32_t cx = wcoord_of(s.x), cy = wcoord_of(s.y);
	if (guarding) {
		const Order* gact = get_activity(*su);
		if (gact) {
			cx = wcoord_of(gact->x);
			cy = wcoord_of(gact->y);
		}
	}
	const Circles& C = circles();
	int32_t near_best = 9999999, best_score = 0, best_o = -1, best_w = 0, units_seen = 0;
	const int32_t n = C.radius[size_t(R)];
	bool stop = false;
	for (int32_t i = 0; i < n && !stop; i++) {
		const int32_t wx = cx + C.x[size_t(i)], wy = cy + C.y[size_t(i)];
		if (!g.world.in_w(wx, wy)) continue;
		int32_t o = g.world.w(wx, wy).down, ow = g.world.w(wx, wy).down_who;
		while (o >= 0 && !stop) {
			Object* tp = g.object(ow, o);
			if (!tp) break;
			const int32_t tox = o, tw = ow;
			o = tp->down;
			ow = tp->down_who;
			Object& t = *tp;
			if (!t.alive() || !valid_target(g, s, tox, tw, nullptr)) continue;
			if (filter & 1) {
				if (!is_live_unit(t)) continue;
			} else if (filter & 2) {
				if (!is_wall(t)) continue;
				if ((filter & 0x20000) && obj_attack(g, t) == 0 && !obj_is_wonder(t) &&
				    !build_is_military_trainer(g, t.type))
					continue;
			} else if ((filter & 0x20000) && is_wall(t)) {
				if (obj_attack(g, t) == 0) continue;
			}
			if (mr_and_r && !facing && t.ptype->domain == kDomainSea) continue;
			int32_t dist = 0;
			if (!check_target(g, s, tox, tw, duty, &dist, guarding, true, facing)) continue;
			if (dist < near_best && (s.is_unit || t.is_unit)) {
				near_best = dist;
				s.near_o = int16_t(tox);
				s.near_who = int16_t(tw);
			}
			if (r > 0 && dist > r) continue;
			bool unchecked;
			if (!must_range && (!guarding || (t.ptype->attack != 0 && (!ai || !has_objmask(t, objmask::kAntiAir))))) {
				unchecked = true;
			} else if (is_in_range(g, s, tox, tw, s.x, s.y, 0, nullptr)) {
				unchecked = false;
			} else if (ai_siege && !t.is_unit && (obj_attack(g, t) != 0 || obj_is_wonder(t))) {
				unchecked = false;
			} else {
				continue;
			}
			if (su) {
				if (minr != 0) {
					if (dist < minr) dist = minr + (maxr - dist);
				} else if (maxr != 0 && dist + 0x180 < maxr) {
					dist = dist / 2;
				}
			}
			const int32_t dd = dist + (t.targeted + 8) * 0x30;
			const int32_t score = compare_target(g, s, tox, tw, unchecked, easy);
			int32_t s2 = score / (dd / 0xc0 + 1);
			if (tox == cur_o && tw == cur_w) s2 /= 2;
			if (t.is_unit) units_seen++;
			if (filter & 0x10) {
				if (!t.is_unit) s2 /= 2;
			} else if (filter & 0x20) {
				if (t.is_unit) s2 /= 2;
			}
			if (facing) {
				const uint32_t a = find_angle(t.x - s.x, t.y - s.y);
				uint32_t d = a - (su ? su->angle : 0u);
				if (d > 0x80000000u) d = ~d;
				if (d < 0x15555555u) s2 <<= 2;
				else if (d < 0x2aaaaaaau) s2 *= 2;
				else if (d > 0x60000000u) continue;
				else if (d > 0x40000000u) s2 /= 10;
			}
			if (s2 == 0 && score != 0) s2 = 1;
			if (s2 > best_score) {
				best_score = s2;
				best_o = tox;
				best_w = tw;
			}
			if (units_seen > 10 && best_o >= 0) stop = true;
		}
	}
	if (n < 1 || near_best > 0xf00) {
		s.near_o = -1;
		s.near_who = -1;
	}
	if (best_o < 0) return -1;
	Object& T = *g.object(best_w, best_o);
	T.targeted = int8_t(std::min(T.targeted + 1, 100));
	if (act && !facing) {
		if (!s.is_unit) {
			Build& b = static_cast<Build&>(s);
			b.build_masks &= ~4u;
			b.attack_ox = int16_t(best_o);
			b.attack_whom = int8_t(best_w);
		} else {
			int32_t grp = su->group;
			int32_t mandatory = 0;
			if (grp >= 0) {
				Group& gr = g.groups.g[size_t(grp)];
				group_normalize(g, gr);
				if (!group_member(g, gr, su->o, su->who)) grp = -1;
			}
			if (grp >= 0 && type_is_siege(*s.ptype) && unit_ai(*su) && is_wall(T) && (T.flags & objflag::kCity)) {
				mandatory = 1;
				// TODO(AI armies): army target / position / angle.
			}
			const OrderType ot = su->order_type();
			const bool guard_now = [&] {
				const Order* actv = get_activity(*su);
				return actv && actv->type == kGuard;
			}();
			if (guard_now) {
				if (ot == kAttackTo) kill_current_order(g, *su);
				add_attack_order(g, *su, best_o, best_w, kQueueFirst, mandatory, false);
			} else if (ot == kAttackTo || ot == kGroupAttackTo) {
				if (T.ptype->domain == kDomainAir && !has_objmask(s, objmask::kAntiAir)) return -1;
				if (ot == kGroupAttackTo && grp >= 0 && group_is_attacking_to(g, g.groups.g[size_t(grp)]) &&
				    !type_is_siege(*s.ptype)) {
					group_action_attack(g, g.groups.g[size_t(grp)], best_o, best_w, 0, kQueueFirst, 4);
				} else {
					add_attack_order(g, *su, best_o, best_w, kQueueFirst, mandatory, false);
				}
			} else {
				add_attack_order(g, *su, best_o, best_w,
				                 get_combat_stance(g, s) == kDefensive ? kQueueFirst : kQueueNew, mandatory, false);
			}
		}
	}
	*whom = best_w;
	return best_o;
}

bool on_duty(const Unit& u) {
	// @5fff70
	if (!(u.ptype->role & role::kCombat)) return false;
	const Order* a = get_activity(u);
	if (!a) return false;
	const OrderType t = a->type;
	return t == kAttackTo || t == kGuard || t == kGroupPatrol || t == kPatrol || t == kGroupAttackTo;
}

int32_t find_melee_target(Game& g, Unit& u, int32_t r, int32_t* whom_out, bool facing, bool act, int32_t filter) {
	// @5ff9c0
	int32_t dummy = -1;
	int32_t* whom = whom_out ? whom_out : &dummy;
	*whom = -1;
	bool skip_gates = false;
	if (u.o_up >= 0 && !facing) {
		Unit* cap = g.unit(u.who, obj_captain(g, u));
		const Order* a = cap ? get_action(*cap) : nullptr;
		if (!a || a->type != kAttack) return -1;
		const int32_t ox = a->ox, w = a->whom;
		const int32_t mand = a->mandatory;
		bool ok = valid_target(g, u, ox, w, nullptr);
		if (ok) {
			const bool req = get_combat_stance(g, u) == kStandGround ||
			                 ((u.unit_masks & umask::kEntrenched) && !(u.unit_masks2 & umask2::kMarineEntrench) &&
			                  (cap->unit_masks & umask::kEntrenched) && !(cap->unit_masks2 & umask2::kMarineEntrench));
			if (req && !is_in_range(g, u, ox, w, 0)) ok = false;
		}
		if (ok) {
			if (act) {
				const OrderType ot = u.order_type();
				const QueuePos q = (ot == kAttackTo || ot == kGroupAttackTo || ot == kGuard) ? kQueueFirst : kQueueNew;
				add_attack_order(g, u, ox, w, q, mand, false);
			}
			*whom = w;
			return ox;
		}
		skip_gates = on_duty(u);
	} else if (!facing) {
		skip_gates = on_duty(u);
	}
	if (!skip_gates) {
		if (get_combat_stance(g, u) >= kHoldFire) return -1;
		if (leader_human(g, u.who)) {
			// UnitData::is_cloaked @60a6a0
			const uint32_t uf = u.ptype->unit_flags;
			if (unit_is_cloaked(u) && !(u.unit_masks & umask::kBlowCover)) {
				if (u.unit_masks & umask::kUnderCover) return -1;
				if (!(u.unit_masks2 & umask2::kPassiveCloak) && !(uf & utflag::kSubmarine) && !type_is(*u.ptype, 68))
					return -1;
			}
		}
	}
	if (u.order_type() == kAttackTo || u.order_type() == kGroupAttackTo) {
		if (type_is_siege(*u.ptype)) {
			if (!leader_human(g, u.who)) filter = 0x20002;
		} else if (!type_is_tank(*u.ptype)) {
			filter = 0x20010;
		}
		if (get_combat_stance(g, u) == kRaze) filter = 0x20;
	}
	const int32_t mr = r < 1 ? obj_max_range(g, u) : 0;
	const int32_t stance = get_combat_stance(g, u);
	const int32_t urr = RON_K(g, "unit_respond_range"), udr = RON_K(g, "unit_defensive_respond_range");
	if (r < 0) {
		if (stance == kDefensive) {
			r = mr ? mr * 192 : 288;
			r = std::max(r, udr * 192);
		} else {
			r = mr ? (mr + 1) * 192 + (stance == kAggressive ? 384 : 0) : 288;
			r = std::max(r, urr * 192);
			if (unit_ai(u)) r = std::max(r, urr * 384);
		}
	} else if (r == 0) {
		if (stance == kDefensive) {
			r = udr * 192;
			if (mr && r <= mr * 192) r = mr * 192;
		} else {
			if ((u.o + g.frame) % 1024 == 0) r = 0x6000;
			else if ((u.o + g.frame) % 256 == 0) r = 0x3000;
			else r = urr * 384;
			if (mr != 0 && (mr + 1) * 192 >= r) r = (mr + 1) * 192;
		}
	}
	const int32_t t = find_nearby_target(g, u, r, whom, act, facing, filter);
	if (t < 0 && (u.ptype->unit_flags & utflag::kFireWhileMoving)) {
		// Guy::reset_pivots for every guy (turret pivots). TODO(combat-spec 2.20.3): turrets are presentation here.
	}
	return t;
}

// ---------------------------------------------------------------- think

int32_t think_attack(Game& g, Unit& u) {
	// @5f5a80
	bool player_style = !unit_ai(u);
	const Leader& L = g.leaders[size_t(u.who)];
	if (player_style) {
		if (unit_stance_type(*u.ptype) == kStanceCombat && u.stance >= kHoldFire) return 0;
	} else if (!(L.flags & leaderflag::kActive)) {
		player_style = true;
	}
	if (L.flags2 & leaderflag2::kCombatAiOff) player_style = true;
	if ((u.ptype->unit_flags2 & utflag2::kPacker) && (u.unit_masks & umask::kPacked) && !type_is(*u.ptype, 62, true)) {
		// TODO(packers): get_packer_stance; PACKER_AUTO casts UNPACK after an idle threshold. Packing is not
		// simulated, so packed packers behave as PACKER_NEVER.
		if (player_style) return 0;
	}
	int32_t R = -1;
	bool on_own_city = false;
	int32_t army = -1;
	if (!player_style) {
		R = 0;
		bool region_flag = false;
		const int32_t tx = tcoord_of(u.x), ty = tcoord_of(u.y), wx = wcoord_of(u.x), wy = wcoord_of(u.y);
		const int32_t treg = g.world.in_t(tx, ty) ? g.world.get_tregion(tx, ty) : 0;
		if (g.world.in_t(tx, ty) && (g.world.t(tx, ty) & tmask::kCity) && g.world.in_w(wx, wy) &&
		    g.world.w(wx, wy).who == u.who) {
			on_own_city = true;
			objects_find_city(g, u.x, u.y, search::kFriendly, u.who, 0x200, filter::kAll, 0, 0);  // (result unused)
			if (u.damage != 0 && ((u.ptype->obj_masks & 0x1020) || u.healing != 0)) R = -1;
			else region_flag = (L.strategy[size_t(treg & 63)] & 4) != 0;
		}
		if (u.ptype->role & 0x10) R = -1;
		const int32_t t = u.ptype->type;
		if (t == 61 || t == 62 || t == 400 || (u.ptype->unit_flags2 & 8)) {
			R = -1;
		} else if (R >= 0) {
			army = unit_add_to_army(g, u);  // Unit::add_to_army @5f7740 (ai-spec 12.3)
			R = -1;
		}
		if (region_flag) R = -1;
	}
	// TODO: game.semaphore byte1 & 2 forces R = 0x2d00.
	const bool mar = (u.ptype->unit_flags & utflag::kMeleeAndRanged) != 0;
	const int32_t save_type = g.range_swap_type, save_val = g.range_swap_value;
	if (mar) {
		g.range_swap_type = u.ptype->type;
		g.range_swap_value = u.ptype->second_max_range;
	}
	int32_t w = -1;
	const int32_t t = find_melee_target(g, u, R, &w, false, true, 0);
	if (mar) {
		g.range_swap_type = save_type;
		g.range_swap_value = save_val;
	}
	if (t >= 0 && w >= 0) {
		const Object* T = g.object(w, t);
		if (T && T->is_unit) {
			if (u.group >= 0) {
				Group& gr = g.groups.g[size_t(u.group)];
				if (group_member(g, gr, u.o, u.who)) group_target_opportunity(g, gr, t, w, u.o, true);
			}
			return 1;
		}
	}
	(void)on_own_city;
	(void)army;  // TODO(AI): retreat to the best own city when army < 0 (combat-spec 2.3 step 7, Unit::go_to_city).
	return 0;
}

void think_combat(Game& g, Unit& u) {
	// Unit::think @5f6e40, the combat parts (combat-spec 2.2). The economy / AI branches are TODO (orders.cpp).
	if (u.o_up >= 0) {
		Unit* cap = g.unit(u.who, obj_captain(g, u));
		const Order* a = cap ? get_action(*cap) : nullptr;
		if (!a || a->type != kAttack) return;
		const int32_t ox = a->ox, w = a->whom, mand = a->mandatory;
		if (valid_target(g, u, ox, w, nullptr)) add_attack_order(g, u, ox, w, kQueueNew, mand, false);
		return;
	}
	if (u.type == 50 || u.type == 51) {
		u.unit_masks &= ~0x78000000u;
		g.leaders[size_t(u.who)].flags |= leaderflag::kIdlePeasant;
	}
	if (!(u.flags & objflag::kNewThink) && u.idle > 2 && (u.o + g.frame) % 16 != 0) return;
	if ((g.leaders[size_t(u.who)].flags2 & leaderflag2::kUnitAiOff) || (u.unit_masks & umask::kAiOff)) return;
	if (!(u.unit_masks & umask::kAutoExplore)) {
		const bool on_tick = u.idle == 1 || (u.o + g.frame) % 32 == 0;
		if (!on_tick) {
			if (u.near_o >= 0 && u.near_who >= 0 && get_combat_stance(g, u) < kHoldFire &&
			    alive_at(g, u.near_o, u.near_who) && is_in_range(g, u, u.near_o, u.near_who, 0)) {
				// TODO(packers): a PACKED packer with a non-AUTO packer stance only clears NEW_THINK.
				add_attack_order(g, u, u.near_o, u.near_who, kQueueNew, 0, false);
				u.flags &= uint8_t(~objflag::kNewThink);
				return;
			}
		} else {
			bool r = false;
			if (type_is(*u.ptype, 62, true)) {
				// Dutch armed merchants: a packed one thinks as a merchant first. TODO(packers): is_packing_or_unpacking.
				r = ((u.unit_masks & umask::kPacked) && think_merchant(g, u) != 0) || think_attack(g, u) != 0;
			}
			if (!r && u.ptype->attack != 0 && (u.ptype->role & role::kCombat)) r = think_attack(g, u) != 0;
			if (r) {
				u.flags &= uint8_t(~objflag::kNewThink);
				return;
			}
		}
	}
	think_economy(g, u);  // LAB_005f7179: peasants, caravans, rare collectors, fishermen, merchants (econ_gather.cpp)
}

int32_t find_new_target(Game& g, Unit& u, int32_t* whom, bool stand_ground) {
	// @5ff6a0
	int32_t dx = 0, dy = 0;
	const bool defpos = get_combat_stance(g, u) == kDefensive && find_def_pos(g, u, &dx, &dy);
	repath(g, u);
	Order* f = u.front();
	if (f && order_is_group(f->type) && u.group >= 0) {
		group_kill_group_move(g, g.groups.g[size_t(u.group)], f->id);  // TODO: Group::kill_group_order (non-move)
		if (u.front() == f && order_is_group(f->type)) kill_current_order(g, u);
	} else {
		kill_current_order(g, u);
	}
	const int8_t saved = u.stance;
	if (stand_ground) u.stance = kStandGround;
	const int32_t t = find_melee_target(g, u, -1, whom, false, true, 0);
	u.stance = saved;
	if (defpos) {
		if (t >= 0 && u.order_type() == kAttack) {
			Order* a = u.front();
			a->defensive = 1;
			a->def_x = dx;
			a->def_y = dy;
		} else if (u.order_type() == kOrderNone) {
			Order* m = add_move_order(g, u, uc(dx), uc(dy), 1, false, kQueueNew, false, -1, -1);
			if (m) {
				m->dest = 0;
				m->last_x = m->last_y = -1;
				m->orig_x = m->orig_y = -1;
				m->facing = -1;
				m->flags |= orderflag::kReturnHome;
			}
		}
	}
	return t;
}

void target_opportunity(Game& g, Unit& u0, int32_t ox, int32_t whom, bool from_group) {
	// @5fffc0
	Unit* up = &u0;
	if (whom == u0.who) return;
	if (whom < 0 || whom >= kNumLeaders) return;
	if (g.leaders[size_t(u0.who)].diplos(whom) != 0 && g.leaders[size_t(whom)].diplos(u0.who) != 0) return;
	if (!from_group && u0.group >= 0 && !(u0.ptype->role & role::kCombat)) {
		Group& gr = g.groups.g[size_t(u0.group)];
		if (group_member(g, gr, u0.o, u0.who)) {
			group_target_opportunity(g, gr, ox, whom, u0.o, false);
			return;
		}
	}
	for (int guard = 0; up->o_up >= 0 && guard < kUnitLimit; guard++) {
		Unit* c = g.unit(up->who, up->o_up);
		if (!c) break;
		up = c;
		from_group = false;
	}
	Unit& u = *up;
	if (!valid_target(g, u, ox, whom, nullptr)) return;
	if (get_combat_stance(g, u) == kHoldFire) return;
	const bool pk = false;  // TODO(packers): PACKER_NEVER and (PACKED or packing)
	const OrderType ot0 = u.order_type();
	if (ot0 == kStrafe || ot0 == kAirPatrol || ot0 == kAirAttackGround) return;
	Object& T = *g.object(whom, ox);
	bool moving = false;
	if (order_is_move(ot0)) {
		moving = true;
		if (T.is_unit) {
			const int32_t sz = u.ptype->big_radius + T.ptype->big_radius;
			if (vector_dist(u.x - T.x, u.y - T.y) <= sz) {
				const Order* aa = get_action(u);
				const Object* at = aa && aa->type == kAttack ? obj_at(g, aa->ox, aa->whom) : nullptr;
				if (!(aa && aa->type == kAttack && at && vector_dist(u.x - at->x, u.y - at->y) <= 2 * sz))
					u.unit_masks |= umask::kZoc;
			}
		}
	}
	if (ot0 == kFleeTo) return;
	Order* a = update_action(g, u);
	bool keepdef = false, switched = false;
	int32_t kdx = 0, kdy = 0;
	if (a && order_is_attack(a->type)) {
		if (a->ox == ox && a->whom == whom) return;
		if (a->ox >= 0 && a->whom >= 0) {
			if (a->mandatory) return;
			const Object* ct = obj_at(g, a->ox, a->whom);
			const bool cur_siege_unit = ct && ct->is_unit && type_is_siege(*ct->ptype);
			if (has_objmask(u, 0x840002u) && T.is_unit && !type_is_siege(*T.ptype) && cur_siege_unit) return;
			if (ct && ct->is_unit && (ct->ptype->role & role::kCombat)) return;
			if (!T.is_unit) return;
			const int32_t st = get_combat_stance(g, u);
			if (st == kRaid || st == kRaze) return;
			if ((ct && ct->is_unit) || has_objmask(u, 0x840002u)) {
				if (T.alive() && obj_attack(g, T) != 0) {
					if (is_in_range(g, u, a->ox, a->whom, 0)) return;
					if (!is_in_range(g, u, ox, whom, 0)) return;
				}
			}
		}
		if (a->defensive) keepdef = find_def_pos(g, u, &kdx, &kdy);
		repath(g, u);
		kill_current_order(g, u);
		switched = true;
		a = update_action(g, u);
	} else if (a && a->type == kCastSpell) {
		// TODO(spells): SABOTAGE casts halve spell_time when the attacker is neither the cast's target.
		return;
	}
	if (!switched && !moving) {
		if (!a) {
			// TODO(spells): spies cast BRIBE, commandos SABOTAGE on their attacker.
		}
		const bool worker_or_idle = is_worker_type(u.type) || u.orders.empty();
		if (((has_objmask(u, objmask::kCivilian) && raw_max_range(g, *u.ptype) == 0) || u.ptype->attack == 0 || pk) &&
		    worker_or_idle && (!(u.ptype->unit_flags2 & utflag2::kPacker) || (u.unit_masks & umask::kPacked)) &&
		    !((u.ptype->unit_flags2 & utflag2::kHero) && (u.unit_masks & (umask::kCloakRunning | umask::kMarchRunning)))) {
			int32_t dist = 0x600;
			if ((u.ptype->unit_flags2 & utflag2::kHero) || (u.ptype->unit_flags2 & utflag2::kSupply))
				dist = (!T.is_unit || raw_max_range(g, *T.ptype) == 0) ? 0x300 : 0x180;
			int32_t fx = u.x, fy = u.y;
			find_nearby_spot(g, *u.ptype, u.x, u.y, &fx, &fy, dist, -1, 0, find_angle(u.x - T.x, u.y - T.y),
			                 kFilterNotMe, u.o, u.who, 0, 0, -1, 0, -1);
			add_move_order(g, u, fx, fy, 4, false, kQueueFirst, false, -1, -1);
			return;
		}
	}
	if (!switched && !on_duty(u) && ot0 != kOrderNone) return;
	if (u.ptype->attack != 0 &&
	    (!a || !(a->flags & orderflag::kExplicit) || unit_ai(u) || a->type == kAttackTo ||
	     a->type == kGroupAttackTo || a->type == kPatrol || a->type == kGroupPatrol)) {
		if (u.order_type() == kOrderNone) {
			if (obj_max_range(g, u) < obj_max_range(g, T) && T.ptype->domain == kDomainSea &&
			    u.ptype->domain == kDomainGround) {
				int32_t px = 0, py = 0;
				if (!find_attack_pos(g, u, ox, whom, 1, &px, &py, 1)) {
					// TODO(combat-spec 2.14 step 11): move away through a one-unit temporary group.
					return;
				}
			}
			if (get_combat_stance(g, u) == kDefensive) {
				add_attack_order(g, u, ox, whom, kQueueNew, 0, false);
				if (keepdef && u.order_type() == kAttack) {
					u.front()->def_x = kdx;
					u.front()->def_y = kdy;
				}
				return;
			}
		}
		if (get_combat_stance(g, u) != kHoldFire) add_attack_order(g, u, ox, whom, kQueueFirst, 0, false);
	}
}

void group_target_opportunity(Game& g, Group& gr, int32_t ox, int32_t whom, int32_t member_o, bool from_unit) {
	// @7107d0 (throttled by Group +0x38, think_frame)
	if (g.frame - gr.think_frame <= 14) return;
	gr.think_frame = g.frame;
	const Unit* mu = g.unit(gr.who, member_o);
	const int32_t cap = mu ? obj_captain(g, *mu) : member_o;
	const Object* T = obj_at(g, ox, whom);
	for (int32_t i = 0; i < gr.num; i++) {
		Unit* m = g.unit(gr.who, gr.list[size_t(i)]);
		if (!m || !m->alive() || !m->on_map() || m->o_up >= 0 || !(m->ptype->role & role::kCombat)) continue;
		if (!from_unit && m->o == cap) {
			target_opportunity(g, *m, ox, whom, true);
			continue;
		}
		const Order* a = get_action(*m);
		const OrderType ot = m->order_type();
		if ((a && a->type != kOrderNone) || (ot != kOrderNone && ot != kAttackTo && ot != kGroupAttackTo)) {
			target_opportunity(g, *m, ox, whom, true);
		} else if (T) {
			const int32_t r = std::min(vector_dist(T->x - m->x, T->y - m->y) + 192,
			                           RON_K(g, "unit_respond_range") * 576);
			find_melee_target(g, *m, r, nullptr, false, true, 0);
		}
	}
}

}  // namespace ron::sim
