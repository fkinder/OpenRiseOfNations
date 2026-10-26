// Firing (docs/combat-spec.md section 4): Unit::fight, cavarch_fight, the per-unit combat state helpers,
// Object::fire_ammo, the Ammo pool (Objects::add_ammo, Ammo::init / inc_time / hit tests / do_damage), animation
// release events (Guy::execute_events), building fire (Build::do_attack, find_target, garrison arrows), and the
// load-time fire_proj decision from the art.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sim/aircraft.hpp"
#include "sim/combat_internal.hpp"
#include "sim/fog.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/rules_xml.hpp"
#include "sim/search.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

// Ammo flight math is single-precision with the original's operation order; no fused multiply-add.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ron::sim {

using namespace combat_detail;

namespace {

constexpr float kGravZ = -10.4875f;  // GRAV_Z @cab378 (GraphicPieces::init)

int32_t ftoi(float f) { return int32_t(double(f)); }  // cvtss2sd + cvttsd2si
float itof(uint32_t u) { return float(double(u)); }

void restrict_xy(const Game& g, int32_t* x, int32_t* y) {
	*x = std::clamp(*x, 0, g.world.xs * 768 - 1);
	*y = std::clamp(*y, 0, g.world.ys * 768 - 1);
}
bool world_is_valid(const Game& g, int32_t x, int32_t y) {
	return x >= 0 && y >= 0 && x < g.world.xs * 768 && y < g.world.ys * 768;
}
uint32_t ammo_gfx_flags(const Game& g, int32_t gpiece) { return g.anims ? g.anims->ammo_flags(gpiece) : 0; }

// The guy z the ammo code uses for a target (guy 0 for units, the object z for buildings).
int32_t target_z(const Object& t) {
	if (t.is_unit) {
		const Unit& u = static_cast<const Unit&>(t);
		return u.guys.empty() ? u.z : u.guys[0].z;
	}
	return t.z;
}

int32_t ums(const Game& g) { return g.rules->constants.at(0x4); }  // unit_move_speed

// angle_to_degrees @a28e00 (unsigned arithmetic: the angle is split into quarters, eighths and finer fractions) and
// fast_angle_to_degrees @a28f70 (the degrees of the angle's top byte, as a float).
int32_t angle_to_degrees_u(uint32_t in) {
	const uint32_t q = in >> 0x1e;
	uint32_t a = in - q * 0x40000000u;
	const uint32_t h = a >> 0x1d;
	a = a - h * 0x20000000u;
	const uint32_t b = a % 0x15555555u;
	const uint32_t c = b % 0xaaaaaaau;
	return int32_t((c % 0x38e38e3u + 0x5b05b0u) / 0xb60b60u +
	               (c / 0x38e38e3u + ((h + q * 2u) * 3u + b / 0xaaaaaaau + (a / 0x15555555u) * 2u) * 3u) * 5u);
}
float fast_angle_to_degrees_f(int32_t angle) { return float(angle_to_degrees_u((uint32_t(angle >> 24) & 0xffu) << 24)); }

// Transform<float>::rotate(deg, (0, 0, 1)) + fill_matrix @4214d0 (Quat::set @420c70: the degrees reduced into [0, 360), the half-degree
// cosine / sine tables of @a29010 / @a29110, cosf((i * 0.5f) * 0.017453292f); Matrix::fill @420df0 with x = y = 0): the 3 x 3 part of the matrix.
struct RotZ {
	float m0, m1, m4, m5;
};
RotZ rot_z(int32_t deg) {
	int32_t d = deg;
	if (d < 0) {
		const int32_t n = -d;
		d = 0x167 - n % 0x168;
	} else if (d >= 0x168) {
		d %= 0x168;
	}
	static float cos_t[360], sin_t[360];
	static bool init = false;
	if (!init) {
		for (int i = 0; i < 360; i++) {
			cos_t[i] = std::cos(float(i) * 0.5f * 0.017453292f);
			sin_t[i] = std::sin(float(i) * 0.5f * 0.017453292f);
		}
		init = true;
	}
	const float w = cos_t[d];
	const float z = 1.0f * sin_t[d];
	const float z2 = z + z;
	const float zz2 = z * z2;
	const float wz2 = w * z2;
	RotZ r;
	r.m0 = 1.0f - (zz2 + 0.0f);
	r.m5 = 1.0f - (zz2 + 0.0f);
	r.m1 = 0.0f - wz2;
	r.m4 = wz2 + 0.0f;
	return r;
}

// GraphicPieces::get_position @90b750 (combat-spec 4.17) for a unit piece: the muzzle offset of the release node, as the truncated
// integers GraphicEvents::execute_game_events adds to the shot's start point. `restrictions` is has_restrictions(piece), `pivot_deg` the
// four turret angles in degrees (null: the plain branch).
void muzzle_offset(const Game& g, const Guy& gy, int32_t piece, const ReleaseEvent& e, int32_t restrictions, const float* pivot_deg,
                   int32_t out[3]) {
	out[0] = out[1] = out[2] = 0;
	if (!g.anims) return;
	float pos[3];
	if (!g.anims->attach_position(piece, e.node, e.anim, e.frame, pos)) return;
	float pscale = 1.0f, ox = 0, oy = 0;
	if (!g.anims->track(piece, &pscale, &ox, &oy)) pscale = 1.0f;
	const float s = kGuyScale * pscale;
	const float deg_f = fast_angle_to_degrees_f(int32_t(uint32_t(gy.angle) + 0x80000000u));
	float v[3];
	if ((e.node & 3) < restrictions && pivot_deg) {
		// The turret branch: the pivot node's position (plain, scaled) is brought back to model units, the offset (relative to the pivot)
		// is rotated by the turret angle plus the heading and added.
		float pp[3];
		const bool have = g.anims->attach_position(piece, (e.node & 3) + 4, e.anim, e.frame, pp);
		float pv[3] = {0, 0, 0};
		if (have) {
			const RotZ r0 = rot_z(int32_t(deg_f));
			pv[0] = pp[0] * r0.m0 + pp[1] * r0.m4 + pp[2] * 0.0f;
			pv[1] = pp[0] * r0.m1 + pp[1] * r0.m5 + pp[2] * 0.0f;
			pv[2] = pp[0] * 0.0f + pp[1] * 0.0f + pp[2] * 1.0f;
			pv[1] = -pv[1];
			pv[0] = pv[0] * s;
			pv[1] = s * pv[1];
			pv[2] = pv[2] * s;
		}
		const float inv = 1.0f / s;
		const float y_old = pv[1];
		pv[0] = pv[0] * inv;
		pv[1] = -y_old * inv;
		pv[2] = pv[2] * inv;
		const RotZ r = rot_z(int32_t(pivot_deg[e.node & 3] + deg_f));
		const float vy = pv[1];
		v[0] = pv[0] + (pos[0] * r.m0 + pos[1] * r.m4 + pos[2] * 0.0f);
		v[1] = -((pos[0] * r.m1 + pos[1] * r.m5 + pos[2] * 0.0f) + vy);
		v[2] = pv[2] + (pos[0] * 0.0f + pos[1] * 0.0f + pos[2] * 1.0f);
	} else {
		const RotZ r = rot_z(int32_t(deg_f));
		v[0] = pos[0] * r.m0 + pos[1] * r.m4 + pos[2] * 0.0f;
		v[1] = pos[0] * r.m1 + pos[1] * r.m5 + pos[2] * 0.0f;
		v[2] = pos[0] * 0.0f + pos[1] * 0.0f + pos[2] * 1.0f;
		v[1] = -v[1];
	}
	out[0] = int32_t(v[0] * s);
	out[1] = int32_t(s * v[1]);
	out[2] = int32_t(v[2] * s);
}

// ObjectsData::find_unit (Ammo::check_hit): the nearest unit to (x, y) within `radius` (domain filter: air /
// non-air). TODO(combat-spec 4.10): the original walks its search tables; here leaders 0..8 in slot order, strict
// nearest (the result is the same unless two units are exactly as far).
int32_t find_unit_near(Game& g, int32_t x, int32_t y, int32_t shooter_who, bool non_friendly, int32_t radius,
                       int32_t domain_mode, int32_t* who_out, int32_t* dist_out) {
	int32_t best = -1, bw = -1, bd = 0x7fffffff;
	for (int32_t w = 0; w < 9; w++) {
		if (non_friendly && w == shooter_who) continue;
		const ObjectList& L = g.objects[size_t(w)];
		for (int32_t o = 0; o < L.unit_mark; o++) {
			const Unit* u = g.unit(w, o);
			if (!u || !u->alive() || !u->on_map()) continue;
			const bool air = u->ptype->domain == kDomainAir;
			if (domain_mode == 1 && !air) continue;
			if (domain_mode == 2 && air) continue;
			const int32_t d = vector_dist(u->x - x, u->y - y);
			if (d <= radius && d < bd) {
				bd = d;
				best = o;
				bw = w;
			}
		}
	}
	*who_out = bw;
	*dist_out = bd;
	return best;
}

// WorldData / Objects::find_building_at: the building covering tcoord (tx, ty), or -1.
int32_t find_building_at(Game& g, int32_t tx, int32_t ty, int32_t* who_out) {
	for (int32_t w = 0; w < 8; w++) {
		const ObjectList& L = g.objects[size_t(w)];
		for (int32_t o = kBuildBase; o < L.build_mark; o++) {
			const Build* b = g.build(w, o);
			if (!b || !b->alive()) continue;
			const Footprint f = footprint(*b->ptype, b->x, b->y);
			if (tx >= f.x0 && tx < f.x0 + f.w && ty >= f.y0 && ty < f.y0 + f.h) {
				*who_out = w;
				return o;
			}
		}
	}
	*who_out = -1;
	return -1;
}

bool ammo_hit_target(Game& g, Ammo& a) {
	// Ammo::hit_target @678f90
	Object* t = obj_at(g, a.ox, a.whom);
	if (!t || !t->alive() || !on_map(*t)) {
		a.whom = a.ox = -1;
		return false;
	}
	bool hit;
	if (t->is_unit) {
		int32_t d = vector_dist(t->x - a.ex, t->y - a.ey);
		if (a.accuracy > 100) d /= 2;
		hit = d <= t->ptype->target_size;
	} else {
		hit = iabs(a.ex - t->x) <= t->ptype->x_size * 96 && iabs(a.ey - t->y) <= t->ptype->y_size * 96;
	}
	if (!hit) a.whom = a.ox = -1;
	return hit;
}

bool ammo_check_hit(Game& g, Ammo& a, int32_t domain) {
	// Ammo::check_hit @678d90
	const Object* sh = obj_at(g, a.o, a.who);
	const bool non_friendly = sh && sh->ptype->domain == kDomainAir && !has_objmask(*sh, objmask::kMissile);
	const int32_t mode = domain == kDomainAir ? 1 : (domain == kDomainGround || domain == kDomainSea) ? 2 : 0;
	int32_t w = -1, d = 0;
	const int32_t o = find_unit_near(g, a.ex, a.ey, a.who, non_friendly, 384, mode, &w, &d);
	if (o >= 0) {
		const Unit* u = g.unit(w, o);
		if (u && u->ptype->target_size >= d) {
			a.ox = o;
			a.whom = w;
			return true;
		}
	}
	if (world_is_valid(g, a.ex, a.ey)) {
		const int32_t b = find_building_at(g, tcoord_of(a.ex), tcoord_of(a.ey), &w);
		if (b >= 0) {
			a.ox = b;
			a.whom = w;
			return true;
		}
	}
	a.whom = a.ox = -1;
	return false;
}

void ammo_close(Ammo& a) { a.flags = 0; }  // Ammo::close @6791a0 (the graphics side is presentation)

void ammo_do_damage(Game& g, Ammo& a) {
	// Ammo::do_damage @678060
	if (!alive_at(g, a.ox, a.whom)) a.whom = a.ox = -1;
	Object* sh = a.who >= 0 && a.o >= 0 ? g.object(a.who, a.o) : nullptr;
	if (!sh) {
		ammo_close(a);
		return;
	}
	const Object* orig = obj_at(g, a.ox, a.whom);
	const int32_t D = orig ? orig->ptype->domain : kDomainGround;
	if (a.splash_area == 0) {
		bool hit = ammo_hit_target(g, a);
		if (!hit) hit = ammo_check_hit(g, a, D);
		Object* t = obj_at(g, a.ox, a.whom);
		if (hit && t && t->alive() && !(a.ox == a.o && a.whom == a.who)) {
			do_damage(g, *sh, a.ox, a.whom, find_angle(a.ex - a.sx, a.ey - a.sy), a.num_guys, a.index, 0x100, false,
			          false);
			ammo_close(a);
			return;
		}
		int32_t jx = a.ex + g.rng.get(0, 0xffff) % 41 - 20;  // @678ca7
		int32_t jy = a.ey + g.rng.get(0, 0xffff) % 41 - 20;  // @678cc4
		restrict_xy(g, &jx, &jy);
		// Ammo::do_damage @678060: only a PUNCTURE ammo piece (verify_ammo_flags(piece, 1): the arrows) can stick, and the jittered point only
		// decides whether it does: the stuck arrow keeps the impact point (the two draws are made for every shot without a target).
		const int32_t tx = tcoord_of(jx), ty = tcoord_of(jy);
		if ((ammo_gfx_flags(g, a.gpiece) & ammoflag::kPuncture) && world_is_valid(g, jx, jy) && g.world.in_t(tx, ty) &&
		    (g.world.t(tx, ty) & tmask::kMedium) != tmask::kWater) {
			a.flags = ammobits::kStuck;  // a stuck arrow keeps its slot 200 frames
			a.cur_time = 0;
			return;
		}
		ammo_close(a);
		return;
	}
	// ---- splash
	if (has_objmask(*sh, objmask::kMissile)) {
		// Cruise / nuclear missiles (combat-spec 4.11, docs/nukes-spec.md): missile defence, Nuke::add_nuke + Nuke::do_damage.
		if (nuke_missile_impact(g, a, *sh)) {
			ammo_close(a);
			return;
		}
	}
	if (!ammo_hit_target(g, a)) ammo_check_hit(g, a, D);
	const int32_t po = a.ox, pw = a.whom;
	int32_t cx = a.ex, cy = a.ey;
	if (sh->is_unit && (sh->ptype->unit_flags & utflag::kFireship)) {
		cx = (a.sx + a.ex) / 2;
		cy = (a.sy + a.ey) / 2;
	}
	const uint32_t angle = find_angle(cx - a.sx, cy - a.sy);
	const int32_t rr = std::min(a.splash_area / 4 + 1, 10);
	const int32_t wcx = wcoord_of(cx), wcy = wcoord_of(cy);
	const int32_t cells = std::min(kMoveRadius[rr], kNumMoves - 1);  // k = 0 .. radius[rr] inclusive
	for (int32_t k = 0; k <= cells; k++) {
		const int32_t wx = wcx + move_x[k], wy = wcy + move_y[k];
		if (!g.world.in_w(wx, wy)) continue;
		int32_t o = g.world.w(wx, wy).down, ow = g.world.w(wx, wy).down_who;
		while (o >= 0) {
			Object* X = g.object(ow, o);
			if (!X) break;
			const int32_t xo = o, xw = ow;
			o = X->down;
			ow = X->down_who;
			a.whom = xw;
			a.ox = xo;
			if (xo == a.o && xw == a.who) continue;
			if (xw >= 8) continue;
			const bool primary = xo == po && xw == pw;
			if (!primary) {
				if (xw == a.who) continue;
				if (is_ally(g, a.who, xw)) continue;
			}
			if (!X->is_unit) {
				if (D == kDomainAir) continue;
				const int32_t ddx = std::max(0, iabs(cx - X->x) - X->ptype->x_size * 192);
				const int32_t ddy = std::max(0, iabs(cy - X->y) - X->ptype->y_size * 192);
				const int32_t pct = 256 - (vector_dist(ddx, ddy) * 256) / (a.splash_area * 192);
				if (pct >= 0 && X->alive()) do_damage(g, *sh, xo, xw, angle, a.num_guys, a.index, pct, !primary, false);
			} else {
				if (!X->alive() || !on_map(*X)) continue;
				if ((D != kDomainAir) != (X->ptype->domain != kDomainAir)) continue;
				if (has_objmask(*X, objmask::kMissile)) continue;
				const int32_t dd = std::max(0, vector_dist(cx - X->x, cy - X->y) - 192 - X->ptype->guy_radius);
				const int32_t pct = 256 - (dd * 256) / (a.splash_area * 192);
				if (pct > 0) do_damage(g, *sh, xo, xw, angle, a.num_guys, a.index, pct, !primary, false);
			}
		}
	}
	ammo_close(a);
}

void ammo_init(Game& g, Ammo& a, const AmmoPackage& pkg, int32_t index, int32_t graph_index) {
	// Ammo::init @67bbf0
	a.flags &= 0xe3;
	a.who = pkg.who;
	a.o = pkg.o;
	a.whom = pkg.whom;
	a.ox = pkg.ox;
	Object* sh = g.object(pkg.who, pkg.o);
	if (!sh || pkg.gpiece < 0) return;
	const bool bomber = type_is(*sh->ptype, 304);
	const uint32_t gfx = ammo_gfx_flags(g, pkg.gpiece);
	const bool miss_gfx = (gfx & ammoflag::kMissile) != 0;
	if (gfx & ammoflag::kDoNoDamage) a.flags |= ammobits::kNoDamage;
	a.index = index;
	a.graph_index = graph_index;
	Unit* su = as_unit(sh);
	const Order* G = nullptr;
	if (su && (su->order_type() == kAttackGround || su->order_type() == kAirAttackGround)) G = su->front();
	Object* T = nullptr;
	if (!G) {
		T = obj_at(g, a.ox, a.whom);
		if (a.ox < 0 || a.whom < 0 || !T || !T->alive()) return;  // not activated: the slot stays free
		if (T->is_unit && T->ptype->domain == kDomainAir && !(T->ptype->unit_flags & utflag::kHelicopter) &&
		    !has_objmask(*T, objmask::kMissile)) {
			// Anti-air rolls: the target's band, the low one while it strafes / returns close to its target or base.
			const bool low = unit_is_flying_low(g, static_cast<const Unit&>(*T));
			if (has_objmask(*sh, objmask::kAntiAir)) {
				if (sh->ptype->domain != kDomainAir) {
					const int32_t r1 = g.rng.get(0, 0xffff) % 100;  // @67c079
					const int32_t Tv = low ? T->ptype->fly_low : T->ptype->fly_high;
					if (r1 >= Tv) {
						a.flags |= ammobits::kNoDamage;
					} else {
						const int32_t r2 = g.rng.get(0, 0xffff) % 100;  // @67c0ba / @67c126
						const int32_t Sv = low ? sh->ptype->fly_low : sh->ptype->fly_high;
						if (!(r2 < Sv)) a.flags |= ammobits::kNoDamage;
					}
				}
			} else {
				const int32_t r = g.rng.get(0, 0xffff) % 100;  // @67c00c
				const int32_t Sv = low ? sh->ptype->fly_low : sh->ptype->fly_high;
				if (!(r < Sv)) a.flags |= ammobits::kNoDamage;
			}
		}
	}
	a.gpiece = pkg.gpiece;
	a.start_roll_angle = 0;
	a.num_guys = su ? su->guy_mark : 0;
	// TODO(combat-spec 4.18.3): MISSILE shooters start at the shooter plus a global offset (unknown); pkg here.
	a.sx = pkg.x;
	a.sy = pkg.y;
	a.sz = pkg.z;
	bool spline = false;
	if (bomber) {
		project(a.sx, a.sy, sh->is_unit ? static_cast<Unit*>(sh)->angle : 0u, 192, &a.ex, &a.ey);
		restrict_xy(g, &a.ex, &a.ey);
		a.ez = find_data_z(g, a.ex, a.ey, 0);
		a.angle = find_angle(a.ex - a.sx, a.ey - a.sy);
	} else {
		const int32_t d = G ? vector_dist(G->x - a.sx, G->y - a.sy) : attack_dist(g, *sh, a.ox, a.whom);
		int32_t acc = sh->ptype->to_hit - (d / 192) * sh->ptype->attenuate;
		acc = std::max(acc, 5);
		int32_t r;
		const int32_t F0 = (RON_K(g, "target_radius") * 100) / ((100 - acc) / 5 + acc);
		const int32_t F = acc > 100 ? F0 / 4 : F0;
		bool zb = false;
		if (su && (sh->ptype->unit_flags & utflag::kStrafing)) {
			r = 0;
		} else if (G) {
			r = F;  // TODO(attack ground): g_exact gives 0
		} else if (T->is_unit && T->ptype->domain == kDomainGround) {
			r = F;
			a.flags |= ammobits::kVerify;
			zb = !miss_gfx;
		} else {
			r = 192;
		}
		a.accuracy = int16_t(acc);
		if (has_objmask(*sh, objmask::kMissile)) r = type_is(*sh->ptype, 315) ? 0 : 2 * r;
		auto off = [&](int32_t rr) { return (rr - 1 >= 1 ? g.rng.get(0, 0xffff) % rr : 0) - rr / 2; };
		if (G) {
			a.ex = G->x + off(r);  // @67c6c6
			a.ey = G->y + off(r);  // @67c703
			a.ez = std::max(0, find_data_z(g, G->x, G->y, 0));
			a.angle = find_angle(a.ex - a.sx, a.ey - a.sy);
		} else {
			a.angle = find_angle(T->x - a.sx, T->y - a.sy);
			a.ex = T->x;
			a.ey = T->y;
			if (r != 0 && !T->is_unit && !type_is_siege(*sh->ptype) && !type_is_tank(*sh->ptype))
				project(a.ex, a.ey, a.angle - 0x80000000u, T->ptype->x_size * 48, &a.ex, &a.ey);
			a.ex += off(r);  // @67c8b7
			a.ey += off(r);  // @67c8e9
			// Ammo::init @67bbf0: the impact height is the target OBJECT's z (+0xc; +75 when the shot is a verified ground hit), only an
			// aircraft target uses its guy 0's z (the unit z sits on the terrain, a guy's z follows its footprint).
			a.ez = T->z + (zb ? 75 : 0);
			if (T->is_unit && T->ptype->domain == kDomainAir) a.ez = target_z(*T);
			a.ez = std::max(a.ez, 0);
		}
		// Strafing shooters (a unit with UNITTYPE_STRAFING, @67c9dd) that do not shoot a plane: while guy 0 is in an ATTACK2
		// animation the impact point slides along the heading with the animation phase, trunc((cur / end - 0.3f) * 6.0f *
		// 192.0f) ahead of the shot (single-precision chain), and sits 0x30 to the side: the left of the heading when the
		// release node is even, the right when odd (aircraft-spec 5.1).
		const Object* slide_target = (a.ox >= 0 && a.whom >= 0) ? obj_at(g, a.ox, a.whom) : nullptr;
		if (su && (sh->ptype->unit_flags & utflag::kStrafing) &&
		    !(slide_target && slide_target->ptype->domain == kDomainAir) && !su->guys.empty() &&
		    unit_anim_cat(su->guys[0].cur_anim) == kCharAttack2) {
			const Guy& g0 = su->guys[0];
			float phase = itof(g0.cur_time) / itof(g0.end_time);
			phase = phase - 0.3f;
			phase = phase * 6.0f;
			phase = phase * 192.0f;
			project(a.ex, a.ey, su->angle, ftoi(phase), &a.ex, &a.ey);
			project(a.ex, a.ey, (pkg.angle & 1) == 0 ? su->angle + 0x40000000u : su->angle - 0x40000000u, 0x30, &a.ex, &a.ey);
		}
	}
	a.ex = std::clamp(a.ex, 0, g.world.xs * 768 - 1);
	a.ey = std::clamp(a.ey, 0, g.world.ys * 768 - 1);
	a.splash_area = sh->ptype->splash_area;
	a.flags |= ammobits::kFlying;
	a.cur_time = 0;
	// ---- flight time
	const int32_t U = ums(g);
	uint32_t total;
	if (miss_gfx) {
		// TODO(combat-spec 4.18.1): rockets, torpedoes and missiles fly a Spline (Spline::calc_from_dir); they are
		// given the plain unit flight time here (a known deviation).
		const float D = std::sqrt(float((a.ex - a.sx) * (a.ex - a.sx) + (a.ey - a.sy) * (a.ey - a.sy)));
		const int32_t sp = std::max(1, sh->ptype->proj_speed * U);
		total = uint32_t(ftoi(D / float(sp)));
	} else if (!sh->is_unit) {
		const float D = std::sqrt(float((a.ex - a.sx) * (a.ex - a.sx) + (a.ey - a.sy) * (a.ey - a.sy)));
		if (get_shot(g, *sh) == 0) total = uint32_t(ftoi(D / float(U * 90)));
		else total = uint32_t(ftoi(D / float(sh->ptype->proj_speed * U)));
	} else if (has_objmask(*sh, objmask::kMissile)) {
		// TODO(combat-spec 4.18.1): Spline::calc_nuke_spline.
		const float D = std::sqrt(float((a.ex - a.sx) * (a.ex - a.sx) + (a.ey - a.sy) * (a.ey - a.sy)));
		total = uint32_t(ftoi(D / float(std::max(1, unit_speed(g, *static_cast<Unit*>(sh))))));
	} else if (bomber) {
		total = uint32_t(ftoi(std::sqrt(float(2 * (a.ez - a.sz)) / kGravZ)));
	} else if (type_is_siege(*sh->ptype)) {
		total = uint32_t(obj_max_range(g, *sh) * 192 / std::max(1, sh->ptype->proj_speed * U));
	} else {
		const float D = std::sqrt(float((a.ex - a.sx) * (a.ex - a.sx) + (a.ey - a.sy) * (a.ey - a.sy)));
		total = uint32_t(ftoi(D / float(std::max(1, sh->ptype->proj_speed * U))));
	}
	if (total < 1) total = 1;
	a.total_time = total;
	// ---- lead
	if (!bomber && !G && !spline && T && T->is_unit) {
		const Unit& tu = static_cast<const Unit&>(*T);
		const OrderType ot = tu.order_type();
		if (order_is_move(ot) || ot == kStrafe || ot == kAirPatrol || ot == kAirAttackGround) {
			const uint32_t ang = tu.angle;
			const int32_t s = tu.guys.empty() ? 0 : tu.guys[0].avg_speed;
			a.ex += sin_a(ang, s) * int32_t(total);
			a.ey -= cos_a(ang, s) * int32_t(total);
			restrict_xy(g, &a.ex, &a.ey);
		}
	}
	a.rolling = 0;
	a.traj = spline ? 2 : 1;
	const float Tt = itof(total);
	a.v1z = (float(a.ez - a.sz) - ((kGravZ * 0.5f) * Tt) * Tt) / Tt;
	const float fy = float(a.ey - a.sy), fx = float(a.ex - a.sx);
	a.dx = std::sqrt(fy * fy + fx * fx) / Tt;
	a.bank_dx = a.bank_dy = 0.0f;
}

void ammo_inc_time(Game& g, Ammo& a) {
	// Ammo::inc_time @67d380
	for (int guard = 0; guard < 4; guard++) {
		a.cur_time++;
		if (Object* sh = (a.o >= 0 && a.who >= 0) ? g.object(a.who, a.o) : nullptr; sh && !sh->alive())
			sh->hold_frames = uint16_t(sh->hold_frames + 1);
		if (!(a.flags & ammobits::kFlying)) {
			if (a.cur_time >= 200) ammo_close(a);
			return;
		}
		if (a.cur_time < a.total_time) {
			// (splines: TODO.) Real projectiles just fly; crash debris (Ammo::init_crash) drifts and tests the terrain.
			if (a.traj != 1) return;
			if (a.bank_dx != 0.0f || a.bank_dy != 0.0f) {
				// The drift (@67d3e0): the impact point moves by the (truncated) bank every frame, and dx follows.
				a.ex += ftoi(a.bank_dx);
				a.ey += ftoi(a.bank_dy);
				restrict_xy(g, &a.ex, &a.ey);
				const int32_t ddx = a.ex - a.sx, ddy = a.ey - a.sy;
				a.dx = std::sqrt(float(ddx * ddx + ddy * ddy)) / itof(a.total_time);
			}
			if (a.gpiece != kCrashPiece) return;  // get_gpiece_type != 0: only a unit piece (the wreck) tests the terrain
			const float t = itof(a.cur_time), k = t / itof(a.total_time);
			const int32_t px = ftoi(float(a.ex - a.sx) * k + float(a.sx));
			const int32_t py = ftoi(float(a.ey - a.sy) * k + float(a.sy));
			const int32_t h = find_data_z(g, px, py, 0);
			const float z = (a.v1z * t + float(a.sz)) + ((kGravZ * 0.5f) * t) * t;
			if (float(h) <= z) return;
			a.ex = px;
			a.ey = py;
			a.ez = h;
			a.total_time = a.cur_time + 1;
			continue;  // impact in this very call
		}
		if ((a.flags & ammobits::kVerify) && !(a.flags & ammobits::kMissed)) {
			if (!ammo_hit_target(g, a) && !ammo_check_hit(g, a, kDomainGround)) a.flags |= ammobits::kMissed;
		}
		if (a.flags & ammobits::kMissed) {
			if (a.cur_time > 3 * a.total_time) {
				ammo_close(a);
				return;
			}
			const float t = itof(a.cur_time), k = t / itof(a.total_time);
			const float z = (a.v1z * t + float(a.sz)) + ((kGravZ * 0.5f) * t) * t;
			const int32_t px = ftoi(float(a.ex - a.sx) * k + float(a.sx));
			const int32_t py = ftoi(float(a.ey - a.sy) * k + float(a.sy));
			if (!world_is_valid(g, px, py)) {
				ammo_close(a);
				return;
			}
			const int32_t h = find_data_z(g, px, py, 0);
			if (z > float(h)) return;
			a.ex = px;
			a.ey = py;
			a.ez = ftoi(z);
			a.total_time = a.cur_time;
		}
		restrict_xy(g, &a.ex, &a.ey);
		if (a.flags & ammobits::kNoDamage) ammo_close(a);
		else ammo_do_damage(g, a);
		return;
	}
}

bool build_find_target(Game& g, Build& b) {
	// Build::find_target @622c80
	const int32_t r = (std::max(b.ptype->x_size, b.ptype->y_size) + 2 * obj_max_range(g, b)) * 96;
	int32_t w = -1;
	const int32_t ox = find_nearby_target(g, b, r, &w, true, false, 0);
	b.build_masks &= ~4u;
	b.attack_ox = int16_t(ox);
	b.attack_whom = int8_t(w);
	return ox >= 0;
}

}  // namespace

// ---------------------------------------------------------------- load-time data

void init_combat_data(Game& g) {
	g.ammo.assign(200, Ammo());  // Objects::init: 200 free slots
	g.ammo_counter = 0;
	for (DeathObj& d : g.deaths) d = DeathObj();
	g.range_swap_type = -1;
	init_fire_proj(g);
}

void init_fire_proj(Game& g) {
	g.fire_proj.assign(size_t(kNumTypes), -1);
	// GraphicEvents::init_unit_events @8e2520 per unit type, on the piece the type's units use (variants agree in
	// the shipped data; the first leader with an existing piece is used): rules value 1 -> first ammo piece; a
	// resolved RELEASEEVENT or NORELEASE -> 0 (event firing, -1 here); INSTANTHIT -> that ammo; then the
	// validation (an attacker firing from events needs a release in ATTACK* / ATTACKWALK before the end of the
	// animation, else first ammo piece).
	for (int32_t t = kUnits; t < kEndGaia; t++) {
		const Type& ty = g.type(t);
		int32_t fp = ty.fire_proj == 1 ? 0 : -1;
		if (fp < 0 || !g.anims) {
			g.fire_proj[size_t(t)] = fp;
			continue;
		}
		int32_t piece = -1;
		for (int32_t w = 0; w <= 8 && piece < 0; w++) {
			if (!(g.leaders[size_t(w)].flags & leaderflag::kValid)) continue;
			const int32_t p = unit_gpiece(g, t, w, 0, 0, false);
			if (p >= 0 && g.anims->piece_exists(p)) piece = p;
		}
		if (piece < 0) {
			const int32_t p = unit_gpiece(g, t, -1, 0, 0, false);
			if (p >= 0 && g.anims->piece_exists(p)) piece = p;
		}
		if (piece >= 0) {
			const auto* evs = g.anims->release_events(piece);
			if (evs)
				for (const ReleaseEvent& e : *evs)
					if (e.resolved) fp = -1;
			const bool norel = g.anims->no_release(piece);
			if (norel) fp = -1;
			const int32_t ih = g.anims->instant_hit(piece);
			if (ih >= 0) fp = ih;
			if (ty.attack != 0 && fp == -1 && !norel && !(uint32_t(ty.obj_masks) & objmask::kMissile) && !type_is(ty, 62)) {
				bool ok = false;
				if (evs)
					for (const ReleaseEvent& e : *evs)
						if (e.anim >= 10 && e.anim <= 14 && e.frame < g.anims->frames(piece, e.anim)) ok = true;
				if (!ok) fp = 0;
			}
		}
		g.fire_proj[size_t(t)] = fp;
	}
}

// ---------------------------------------------------------------- state helpers

int32_t set_attack(Game& g, Unit& u, int32_t ox, int32_t whom) {
	// @5fce70: every squad guy remembers the target; a unit whose piece has pivot restrictions (ranged, valid target on the map) aims all its guys' turrets
	// and returns the result of the last guy (1: every turret takes the target, the body need not turn).
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) {
		u.guys[size_t(i)].ox = int16_t(ox);
		u.guys[size_t(i)].whom = int8_t(whom);
	}
	if (u.guys.empty() || !g.anims || g.anims->pivot_restrictions(u.guys[0].gpiece) == 0 || u.ptype->max_range == 0 || ox < 0 || whom < 0) return 0;
	const Object* t = g.object(whom, ox);
	if (!t) return 0;
	const bool on_map = !t->is_unit || static_cast<const Unit*>(t)->on_map();
	if (!(t->alive() && on_map)) return 0;
	int32_t r = 0;
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) r = guy_set_all_pivots(g, u, u.guys[size_t(i)]);
	return r;
}

void set_attacking(Game& g, Unit& u, int32_t whom) {
	// @5ff5b0
	if (whom >= 0 && whom < 8 && u.who < 8) {
		g.leaders[size_t(u.who)].treaty(whom) |= 1;
		g.leaders[size_t(whom)].treaty(u.who) |= 1;
	}
	u.flags |= objflag::kAttacking;
	if (whom >= 0 && whom < 8 && !(u.visible & (1 << whom))) {
		u.visible = int8_t(u.visible | (1 << whom));
		if (!is_seen(g, floor_div3(u.x >> 7), floor_div3(u.y >> 7), whom)) object_update_local_seen(g, u);
	}
}

void set_in_danger(Game& g, Unit& u0, bool flag) {
	// @5fcfb0
	Unit* u = &u0;
	if (!flag && u->o_up >= 0) {
		if (Unit* c = g.unit(u->who, obj_captain(g, *u))) u = c;
	}
	for (int guard = 0; u && guard < kUnitLimit; guard++) {
		for (int32_t i = 0; i < u->guy_mark && size_t(i) < u->guys.size(); i++) u->guys[size_t(i)].guy_flags |= 0x20;  // Unit::set_in_danger @5fcfb0 loops [0, guy_mark)
		u->unit_masks |= umask::kInCombat;
		if (u->o_down < 0) break;
		Unit* d = g.unit(u->who, u->o_down);
		if (!d || !d->alive()) break;
		u = d;
	}
}

int32_t get_shot(const Game& g, const Object& b) {
	// BuildData::get_shot @62dd90
	const int32_t t = b.type;
	if (t == 441 || t == 445) return 1;
	if (t == 442 || t == 523 || t == 524) return 2;
	if (t == 446) return 4;
	if (t == 521 || t == 522) return 0;
	if (t == 525) return 5;
	const int32_t a = b.who >= 0 && b.who < kNumLeaders ? g.leaders[size_t(b.who)].age : 0;
	return a <= 2 ? 0 : a <= 4 ? 1 : 2;
}

int32_t get_base_garrison_arrows(const Game& g, const Build& b) {
	// @62e340: the type's base arrows, plus the Maya bonus (tribe bonus 1) unless this is an active city building whose
	// city is not assimilated to its owner (city +0x5f != +0x5e).
	int32_t base = b.ptype->base_arrows;
	if (!has_tribe_bonus(g, b.who, 1)) return base;
	if ((b.flags & objflag::kCity) && (b.flags & objflag::kActive) && b.city >= 0 && b.who >= 0 && b.who < 8 &&
	    size_t(b.city) < g.cities[size_t(b.who)].size()) {
		const City& C = g.cities[size_t(b.who)][size_t(b.city)];
		if (C.race != C.who) return base;
	}
	return base + RON_K(g, "maya_garrison_arrows");
}

int32_t get_garrison_arrows(const Game& g, const Build& b) {
	// @62e3d0
	const int32_t div = (obj_attack(g, b) + 5) / 10;
	if (div == 0) return 0;
	const int32_t base = get_base_garrison_arrows(g, b);
	int32_t S = count_inside(g, b, kCountGarrisonArrows, 0);
	if (base == 0) S += div / 2;
	return base + std::min(S / div, b.ptype->most_shots);
}

// ---------------------------------------------------------------- firing

void fire_ammo(Game& g, Object& s, int32_t ox, int32_t whom) {
	// @64c8b0
	AmmoPackage pkg;
	int32_t gpiece;
	Unit* su = as_unit(&s);
	if (su) {
		gpiece = g.fire_proj.empty() ? 0 : g.fire_proj[size_t(s.type)];
		if ((s.type == 61 || s.type == 62 || s.type == 400) && !(su->unit_masks & umask::kPacked))
			gpiece = 0 + (g.leaders[size_t(s.who)].age > 2 ? 1 : 0);
		pkg.angle = su->angle;
	} else {
		gpiece = get_shot(g, s);
		pkg.angle = 0;
	}
	pkg.who = s.who;
	pkg.o = s.o;
	pkg.whom = whom;
	pkg.ox = ox;
	pkg.gpiece = gpiece;
	const bool ground = su && (su->order_type() == kAttackGround || su->order_type() == kAirAttackGround);
	if (!(ox >= 0 && whom >= 0) && !ground) return;
	if (su) {
		for (int32_t i = 0; i < su->guy_mark && size_t(i) < su->guys.size(); i++) {
			const Guy& gy = su->guys[size_t(i)];
			pkg.x = gy.x;
			pkg.y = gy.y;
			pkg.z = gy.z + 100;
			add_ammo(g, pkg);
		}
	} else {
		const int32_t W = s.ptype->x_size * 96, H = s.ptype->y_size * 96;
		for (int32_t k = 0; k < s.ptype->ammo_per_att; k++) {
			pkg.x = s.x - W / 2 + (W - 1 >= 1 ? g.rng.get(0, 0xffff) % W : 0);  // @64cc73
			pkg.y = s.y - H / 2 + (H - 1 >= 1 ? g.rng.get(0, 0xffff) % H : 0);  // @64cca4
			pkg.z = s.z + 250;
			add_ammo(g, pkg);
		}
	}
}

static bool trace_on() { static const bool t = std::getenv("RON_COMBAT_TRACE") != nullptr; return t; }
int32_t add_ammo(Game& g, const AmmoPackage& pkg) {
	// @658b10: the lowest free slot (the pool grows); the graph_index counter advances even when init bails out.
	size_t i = 0;
	while (i < g.ammo.size() && g.ammo[i].in_use()) i++;
	if (i == g.ammo.size()) g.ammo.emplace_back();
	ammo_init(g, g.ammo[i], pkg, int32_t(i), g.ammo_counter);
	if (trace_on()) {
		const Ammo& a = g.ammo[i];
		const Object* shooter = g.object(pkg.who, pkg.o);
		std::fprintf(stderr, "f%d ammo %zu from %d:%d (type %d) -> %d:%d piece %d flags %x total %u acc %d end (%d,%d) start (%d,%d)\n",
		             g.frame, i, pkg.who, pkg.o, shooter ? shooter->type : -1, pkg.whom, pkg.ox, pkg.gpiece, a.flags, a.total_time, a.accuracy, a.ex, a.ey, a.sx, a.sy);
	}
	g.ammo_counter++;
	return int32_t(i);
}

void ammo_inc_time_all(Game& g) {
	// The count is re-read: ammo created during the loop flies one tick this frame too.
	for (size_t i = 0; i < g.ammo.size(); i++)
		if (g.ammo[i].in_use()) ammo_inc_time(g, g.ammo[i]);
}

void guy_muzzle_offset(const Game& g, const Guy& gy, const ReleaseEvent& e, int32_t out[3]) {
	float pivot_deg[4];
	for (int k = 0; k < 4; k++) pivot_deg[k] = fast_angle_to_degrees_f(gy.turret_angles[size_t(k)]);
	muzzle_offset(g, gy, gy.gpiece, e, g.anims ? g.anims->pivot_restrictions(gy.gpiece) : 0, pivot_deg, out);
}

// ---------------------------------------------------------------- turrets (combat-spec 4.17: pivot restrictions)

void guy_reset_pivots(Game& g, Unit& u, Guy& gy, int32_t keep) {
	// Guy::reset_pivots @5d8a80: the count comes from the guy's graphics piece, the limits from the unit type (Guy +8). Every turret rests at the middle of
	// its arc (degrees_to_angle of the integer mean, the C division of the asm), half a turn further for a wrapping arc (min > max).
	(void)u;
	if (!g.anims) return;
	const int32_t n = g.anims->pivot_restrictions(gy.gpiece);
	for (int32_t k = 0; k < n && k < 4; k++) {
		int32_t mn = 0, mx = 0;
		g.anims->pivot_limits(gy.type, 4 + k, &mn, &mx);
		uint32_t a = uint32_t(degrees_to_angle((mn + mx) / 2));
		gy.des_turret_angles[size_t(k)] = int32_t(mn > mx ? a + 0x80000000u : a);
		if (keep) gy.turret_angles[size_t(k)] = gy.des_turret_angles[size_t(k)];
	}
}

int32_t guy_set_all_pivots(Game& g, Unit& u, Guy& gy) {
	// Guy::set_all_pivots @5d8bc0 (asm read): aims every turret of the guy's type at the guy's target (an object, or the ground point of an ATTACK_GROUND
	// order). Returns 0 when the type has no turrets or some turret cannot take the target (the target direction is outside the front +-45 degrees, or
	// outside the limits of that pivot), else 1. A turret that can takes des_turret_angles = the target's direction relative to the guy's heading and sets
	// its bit in des_node_flags; its bit in node_flags (the gate of GraphicEvents::execute_game_events) when the turret is within 15 degrees of it.
	int32_t ret = 1;
	if (!(gy.guy_flags & 0x100) || !g.anims) return 0;
	const int32_t n = g.anims->pivot_count(gy.type);
	if (n == 0) return 0;
	gy.node_flags = 0;
	gy.des_node_flags = 0;
	const Object* tg = (gy.ox >= 0 && gy.whom >= 0) ? g.object(gy.whom, gy.ox) : nullptr;
	const bool ground = u.order_type() == kAttackGround;
	if (!(tg && (tg->flags & objflag::kValid)) && !ground) return 1;
	int32_t tx, ty;
	if (ground) {
		const Order* o = u.front();
		tx = o ? o->x : 0;
		ty = o ? o->y : 0;
	} else {
		tx = tg->x;
		ty = tg->y;
	}
	for (int32_t k = 0; k < n && k < 4; k++) {
		int32_t mn = 0, mx = 0;
		g.anims->pivot_limits(gy.type, 4 + k, &mn, &mx);
		// get_position(gpiece, node 4 + k, anim 0, frame 0, heading = (float)angle_to_degrees(angle + 0x80000000), no pivots): the pivot node's offset
		// (plain branch, (int) of the scaled floats).
		int32_t off[3] = {0, 0, 0};
		float pos[3];
		if (g.anims->attach_position(gy.gpiece, 4 + k, 0, 0, pos)) {
			float pscale = 1.0f, ox = 0, oy = 0;
			if (!g.anims->track(gy.gpiece, &pscale, &ox, &oy)) pscale = 1.0f;
			const float s = kGuyScale * pscale;
			const RotZ r = rot_z(angle_to_degrees_exact(uint32_t(gy.angle) + 0x80000000u));
			float v[3];
			v[0] = pos[0] * r.m0 + pos[1] * r.m4 + pos[2] * 0.0f;
			v[1] = pos[0] * r.m1 + pos[1] * r.m5 + pos[2] * 0.0f;
			v[2] = pos[0] * 0.0f + pos[1] * 0.0f + pos[2] * 1.0f;
			v[1] = -v[1];
			off[0] = int32_t(v[0] * s);
			off[1] = int32_t(s * v[1]);
			off[2] = int32_t(v[2] * s);
		}
			const uint32_t ang = find_angle(tx - u.x - off[0], ty - u.y - off[1]);
		const uint32_t rel = ang - gy.angle;
		float deg = float(angle_to_degrees_exact(rel));
		if (deg > 180.0f) deg -= 360.0f;
		if (deg > 45.0f || deg < -45.0f) ret = 0;
		const float fmn = float(mn), fmx = float(mx);
		bool ok;
		if (mn < mx) ok = !(fmn > deg) && !(deg > fmx);
		else ok = fmn > deg ? !(deg > fmx) : true;
		if (!ok) {
			ret = 0;
			continue;
		}
		gy.des_turret_angles[size_t(k)] = int32_t(rel);
		uint32_t d = rel - uint32_t(gy.turret_angles[size_t(k)]);
		if (d > 0x80000000u) d = ~d;
		gy.des_node_flags = int16_t(uint16_t(gy.des_node_flags) | uint16_t(1u << k));
		if (d < 0xaaaaaaau) gy.node_flags = int16_t(uint16_t(gy.node_flags) | uint16_t(1u << k));
	}
	return ret;
}

void guy_execute_events(Game& g, Unit& u, Guy& gy) {
	// Guy::execute_events @5d99c0 -> GraphicEvents::execute_game_events @8e48e0 (release events; sounds and the
	// carrier launch sequence are not simulated here).
	if (!g.anims) return;
	const auto* evs = g.anims->release_events(gy.gpiece);
	if (!evs) return;
	AmmoPackage pkg;
	pkg.x = gy.x;
	pkg.y = gy.y;
	pkg.z = gy.z;
	const int32_t cur_anim = gy.cur_anim;
	const uint32_t cur_time = gy.cur_time;
	const int32_t last_time = cur_time == 0 ? -1 : gy.last_time;
	pkg.who = u.who;
	pkg.o = u.o;
	pkg.angle = gy.angle;
	bool skip = false;
	int32_t ox = -1, whom = -1;
	uint32_t uid = 0xffffffffu;
	const Order* f = u.front();
	if (f && order_is_attack(f->type)) {
		if (!order_is_group(f->type)) {
			ox = f->ox;
			whom = f->whom;
			uid = f->uid;
		} else {
			ox = f->ox;  // GroupAttackOrder oxxx / whosoever (TODO: tracked separately in the original)
			whom = f->whom;
			uid = f->uid;
		}
		if (f->type == kAttackGround || f->type == kAirAttackGround) {
			skip = true;
			ox = whom = -1;
		}
	} else if (f && f->type == kSpecialAnim) {
		ox = f->ox;
		whom = f->whom;
	} else if (u.cavarch_o >= 0) {
		ox = u.cavarch_o;
		whom = u.cavarch_who;
		uid = u.cavarch_uid;
	}
	if (unit_anim_cat(cur_anim) == kCharAttack2 && !skip) {
		if (ox < 0 || whom < 0) return;
		const Object* t = obj_at(g, ox, whom);
		if (!t || !t->alive() || t->uid != uint16_t(uid)) return;
	}
	pkg.ox = ox;
	pkg.whom = whom;
	for (const ReleaseEvent& e : *evs) {
		if (e.anim != cur_anim || !(last_time < e.frame) || !(uint32_t(e.frame) <= cur_time)) continue;
		const bool gtarget = u.order_type() == kAttackGround || u.order_type() == kAirAttackGround;
		if (!((whom != -1 && ox != -1) || gtarget)) continue;
		// A turret muzzle (node below the type's pivot count) releases only once its turret has reached its aim (node_flags bit).
		const int32_t restrictions = g.anims->pivot_restrictions(gy.gpiece);
		if (restrictions != 0 && (e.node & 3) < restrictions && !((uint32_t(gy.node_flags) >> (e.node & 3)) & 1u)) continue;
		// GraphicEvents::execute_game_events @8e48e0: the shot starts at the guy plus the muzzle offset of the release node.
		// The release nodes below has_restrictions(piece) (the <RESTRICTION> count of the type) are turret muzzles: the offset of the pivot
		// node plus the node's offset rotated by the turret angle (GraphicPieces::get_position's restricted branch).
		float pivot_deg[4];
		for (int k = 0; k < 4; k++) pivot_deg[k] = fast_angle_to_degrees_f(gy.turret_angles[size_t(k)]);
		int32_t off[3];
		muzzle_offset(g, gy, gy.gpiece, e, restrictions, pivot_deg, off);
		AmmoPackage p = pkg;
		p.x += off[0];
		p.y += off[1];
		p.z += off[2];
		p.gpiece = e.ammo;
		p.angle = uint32_t(e.node);
		add_ammo(g, p);
	}
}

// ---------------------------------------------------------------- fight

int32_t fight(Game& g, Unit& u, int32_t ox, int32_t whom, int32_t p3, int32_t p4, int32_t p5) {
	// @5fd4d0
	const int32_t ox0 = ox, whom0 = whom;
	if (ox >= 0 && whom >= 0 && !p5) {
		const Order* ord = update_order(u);
		const Object* t = obj_at(g, ox, whom);
		if (ord && t && t->uid != ord->uid) ox = whom = -1;
	}
	bool rt = false;
	{
		const Order* go = u.front();
		if ((u.ptype->unit_flags2 & utflag2::kRetargets) &&
		    !(ox >= 0 && whom >= 0 && valid_target(g, u, ox, whom, nullptr) && go && !(go->flags & orderflag::kApproach)))
			rt = true;
	}
	if (u.recharging != 0 && !rt) {
		if (p5) return 0;
		if (!(u.ptype->unit_flags & utflag::kMeleeAndRanged)) {
			const Order* go = u.front();
			if (!go || go->new_ord == 0) return 0;
			if (!u.guys.empty() && go->ox == u.guys[0].ox && go->whom == u.guys[0].whom) return 0;
			if (!u.guys.empty() && u.guys[0].cur_anim >= 0 && u.guys[0].cur_anim < 4) return 0;
			unit_set_anim(g, u, kCharDefault, 1, 1);
			return 0;
		}
		unit_set_new_location(g, u, uc(u.x), uc(u.y), true, false);
	} else if (!p5) {
		unit_set_new_location(g, u, uc(u.x), uc(u.y), true, false);
	}
	// ---- followers copy the captain's target
	if (u.o_up >= 0 && !p5 && u.collide_frame < g.frame - 1) {
		Unit* C = g.unit(u.who, obj_captain(g, u));
		const Order* ca = C ? get_action(*C) : nullptr;
		if (ca && ca->type == kAttack) {
			const int32_t co = ca->ox, cw = ca->whom;
			const uint16_t cuid = ca->uid;
			if ((co != ox || cw != whom) && valid_target(g, u, co, cw, nullptr) &&
			    (get_combat_stance(g, u) < 2 || is_in_range(g, u, co, cw, 0))) {
				ox = co;
				whom = cw;
				if (u.order_type() == kAttack) {
					Order* o = u.front();
					o->ox = co;
					o->whom = cw;
					o->uid = cuid;
				}
			}
		}
	}
	int32_t reason = 0;
	auto retarget_R = [&]() -> int32_t {
		if (!p4) {
			if (u.waiting < 5 && g.leaders[size_t(u.who)].retarget_budget > 10) {
				if (!p5) u.unit_masks2 |= umask2::kHoldAnim;
				u.waiting = int8_t(u.waiting + 2);
				return 0;
			}
			if (!p5) {
				find_new_target(g, u, nullptr, false);
			} else {
				int32_t w = -1;
				const int32_t t = find_melee_target(g, u, -1, &w, true, false, 0);
				u.cavarch_o = int16_t(t);
				u.cavarch_who = int8_t(w);
				if (t >= 0) u.cavarch_uid = g.object(w, t)->uid;
			}
		}
		g.leaders[size_t(u.who)].retarget_budget++;
		if (p5) return 0;
		if (u.order_type() != kAttack) return 0;
		if (u.recharging != 0 && !rt) return 0;
		u.unit_masks2 |= umask2::kHoldAnim;
		return 0;
	};
	if (!valid_target(g, u, ox, whom, &reason)) {
		if (p5) {
			u.cavarch_o = -1;
			if (!p3 || ox < 0) return retarget_R();
			const Object* t = obj_at(g, ox, whom);
			const int32_t c = t ? obj_captain(g, *t) : ox;
			if (c == ox || !valid_target(g, u, c, whom, nullptr)) return retarget_R();
			u.cavarch_who = int8_t(whom);
			u.cavarch_o = int16_t(c);
			u.cavarch_uid = g.object(whom, c)->uid;
			return 0;
		}
		if (reason != 0 && (p3 || unit_ai(u))) {
			// TODO(cities): walk next to a capturable city (find_nearby_spot around the midpoint, Army::charge).
		}
		const Order* a = get_action(u);
		if (!a || a->type != kAttack) return 0;
		if (!p3 || ox < 0) return retarget_R();
		const Object* t = obj_at(g, ox, whom);
		const int32_t c = t ? obj_captain(g, *t) : ox;
		if (c != ox && valid_target(g, u, c, whom, nullptr)) {
			Order* o = update_order(u);
			o->whom = whom;
			o->ox = c;
			o->uid = g.object(whom, c)->uid;
			if (!(o->flags & orderflag::kApproach)) {
				o->flags |= orderflag::kApproach;
				work(g, u);
			} else {
				o->flags &= uint8_t(~orderflag::kApproach);
			}
			return 0;
		}
		// UnitData::next_action_type: the action after the current one.
		OrderType nat = kOrderNone;
		if (u.orders.size() >= 2) nat = u.orders[1].type;
		if (nat == kOrderNone || nat == kAttackTo || nat == kGroupAttackTo) return retarget_R();
		kill_current_order(g, u);
		return 0;
	}
	// ---- AI guard drop (@5fdcdb)
	if (!p5) {
		const Order* actv = get_activity(u);
		if (actv && actv->type == kGuard) {
			bool local = false, drop = false;
			if (u.o_up < 0 && unit_ai(u)) {
				local = true;
				if (g.rng.get(0, 0xffff) & 1) drop = true;
			}
			if (!drop && !check_target(g, u, ox, whom, true, nullptr, true, local, false)) drop = true;
			if (drop) {
				kill_current_order(g, u);
				find_melee_target(g, u, -1, nullptr, false, true, 0);
				if (u.order_type() == kAttack && u.recharging == 0) u.unit_masks2 |= umask2::kHoldAnim;
				return 0;
			}
		}
	}
	// ---- re-evaluation (@5fde6e)
	if (!p3 && !p4 && u.recharging == 0 && u.o_up < 0) {
		bool evaluate = true;
		if (!p5) {
			const Order* o = update_order(u);
			if (o && order_is_group(o->type) && !(o->oxx == u.o && o->whose == u.who)) evaluate = false;
		}
		if (evaluate) {
			Object& T = *g.object(whom, ox);
			bool civ = false;
			if (T.is_unit) {
				const Type& tt = *T.ptype;
				if (u.unit_masks2 & umask2::kAttackCivies) civ = (tt.role & role::kCombat) && !(tt.unit_flags & utflag::kFireship);
				else civ = !(tt.role & role::kCombat);
				const int32_t r = g.rng.get(0, 0xffff);  // @5fde6e
				const Order* o = update_order(u);
				if (r % 5 == 0 || (o && (o->flags & orderflag::kApproach))) civ = false;
			}
			bool look = !T.is_unit || civ;
			if (!look && !p5 && poor_target(g, u, ox, whom)) look = true;
			if (look) {
				int32_t w = -1;
				const int32_t t = p5 ? find_melee_target(g, u, -1, &w, true, false, 0) : find_new_target(g, u, &w, false);
				if (t != ox || w != whom) {
					if (t >= 0 && w >= 0) {
						if (p5) {
							u.cavarch_who = int8_t(w);
							u.cavarch_o = int16_t(t);
							u.cavarch_uid = g.object(w, t)->uid;
							return 0;
						}
						if (u.order_type() != kAttack || u.recharging != 0) return 0;
						if (t == ox0 && w == whom0) return 0;
						Order* o = update_order(u);
						o->ox = t;
						o->whom = w;
						o->uid = g.object(w, t)->uid;
						u.unit_masks2 |= umask2::kHoldAnim;
						return 0;
					}
					if (p5) {
						u.cavarch_o = int16_t(t);
						u.cavarch_who = int8_t(w);
					}
					return 0;
				}
			}
		}
	}
	// (a discarded get_combat_stance call)
	// ---- range test
	int32_t dist = 0;
	const bool in = is_in_range(g, u, ox, whom, u.x, u.y, 0, &dist);
	if (!p5) {
		Order* o = update_order(u);
		if (!o) return 0;
		o->in_range = int8_t(in);
		if (in) {
			o->ever_in_range = 1;
		} else if ((u.ptype->unit_flags2 & utflag2::kPacker) && !(u.unit_masks & umask::kPacked) && o->ever_in_range) {
			kill_current_order(g, u);
			return 0;
		}
	}
	if (in) {
		// ---- packers and packable siege
		if (!p5 && (u.ptype->unit_flags2 & utflag2::kPacker) && !type_is(*u.ptype, 62, true)) {
			if (u.unit_masks & umask::kPacked) {
				// An AI siege weapon out-ranging a building that can shoot back first looks for a better spot; every other
				// packed packer unpacks before it fires (combat-spec 4.3.7).
				const Object* TT = g.object(whom, ox);
				if ((u.unit_masks & umask::kAiControl) && type_is_siege(*u.ptype) && TT && !TT->is_unit && TT->ptype->attack != 0) {
					const int32_t mine = obj_max_range(g, u), theirs = obj_max_range(g, *TT);
					if (theirs + 1 < mine && is_in_range(g, *TT, u.o, u.who, 0)) {
						int32_t apx = 0, apy = 0;
						if (find_attack_pos(g, u, ox, whom, 0, &apx, &apy, 0) != 0 && (apx != u.x || apy != u.y)) {
							add_move_order(g, u, apx, apy, 1, false, kQueueNew, false, -1, -1);
							return 0;
						}
					}
				}
				add_cast_order(g, u, -1, -1, -1, -1, 0x28c, kQueueFirst, false);
				return 0;
			} else if (g.object(whom, ox)->is_unit && type_is_siege(*u.ptype)) {
				// TODO(attack ground): unpacked siege attacks units by ATTACK_GROUND at the target's position.
			}
		}
		u.unit_masks |= 0x11000u;
		int32_t turret = 1;
		if (ox >= 0 && whom >= 0) {
			if (p5) {
				u.cavarch_o = int16_t(ox);
				u.cavarch_who = int8_t(whom);
				u.cavarch_uid = g.object(whom, ox)->uid;
			}
			turret = set_attack(g, u, ox, whom);
		}
		Object& T = *g.object(whom, ox);
		if (T.ptype->domain == kDomainAir && has_objmask(u, objmask::kAntiAir)) turret = 1;
		uint32_t a;
		if (!turret) {
			a = find_angle(T.x - u.x, T.y - u.y);
			if (!T.is_unit && T.alive()) {
				// Face a wall square-on when it covers a neighbouring tile with the BLOCKED bit.
				const int32_t tx = tcoord_of(u.x), ty = tcoord_of(u.y);
				const Footprint fp = footprint(*T.ptype, T.x, T.y);
				auto covers = [&](int32_t x, int32_t y) { return x >= fp.x0 && x < fp.x0 + fp.w && y >= fp.y0 && y < fp.y0 + fp.h; };
				auto blocked = [&](int32_t x, int32_t y) { return g.world.in_t(x, y) && (g.world.t(x, y) & tmask::kBlocked); };
				if (!covers(tx, ty)) {
					if (covers(tx - 1, ty) && blocked(tx - 1, ty)) a = 0xc0000000u;
					else if (covers(tx, ty - 1) && blocked(tx, ty - 1)) a = 0;
					else if (covers(tx + 1, ty) && blocked(tx + 1, ty)) a = 0x40000000u;
					else if (covers(tx, ty + 1) && blocked(tx, ty + 1)) a = 0x80000000u;
				}
			}
			if ((u.ptype->unit_flags & utflag::kAttackSideways) && !(u.type == 389 && T.ptype->domain == kDomainSea)) {
				const uint32_t m = a - 0x40000000u, p = a + 0x40000000u;
				a = abs_angle(u.angle - m) < abs_angle(u.angle - p) ? m : p;
			}
		} else {
			a = u.angle;
		}
		if (!p5) {
			if (a != u.angle) unit_set_angle(g, u, a, false);
			const int32_t squad = u.ptype->squad_size;
			if (squad < int32_t(u.guys.size()) || u.ptype->domain == kDomainSea || squad == 1) {
				for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) u.guys[size_t(i)].des_angle = a;
				for (size_t i = size_t(squad); i < u.guys.size(); i++) u.guys[i].des_angle = a;
			} else {
				for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) {
					Guy& gy = u.guys[size_t(i)];
					gy.des_angle = find_angle(T.x - gy.des_x, T.y - gy.des_y);
				}
			}
			unit_set_new_location(g, u, uc(u.x), uc(u.y), true, false);
			// ---- animation choice
			int32_t anim = kCharAttack1, rnd = 1;
			bool immortal_melee = false;
			const Type& pt = *u.ptype;
			if (pt.unit_flags & utflag::kRockingAttack) {
				anim = int32_t(find_angle(T.x - u.x, T.y - u.y) - a) < 0 ? kCharAttack1 : kCharAttack2;
				rnd = 0;
			} else if (pt.unit_flags & utflag::kMeleeAndRanged) {
				if (raw_max_range(g, pt) != 0) anim = kCharAttackSpecial, rnd = 0;
			} else if (type_is(pt, 162)) {
				if (dist < 192) immortal_melee = true;
			} else if (u.type == 389) {
				anim = T.ptype->domain == kDomainSea ? kCharAttack1 : kCharAttack2;
				rnd = 0;
			} else if (has_objmask(u, objmask::kAntiAir)) {
				// TODO(jamming): is_jammed is not simulated (never jammed); the draw is made.
				(void)g.rnd(100);  // GameAccess::rnd @43ccae
			}
			if (immortal_melee) {
				unit_set_anim(g, u, kCharAttackSpecial, 0, 0);
				for (int32_t i = 0; i < u.guy_mark; i++) do_damage(g, u, ox, whom, a, u.guy_mark, -1, 0x100, false, false);
				if (!u.alive()) return 0;
			} else {
				unit_set_anim(g, u, anim, 0, rnd);
			}
			set_attacking(g, u, whom);
			Order* o = update_order(u);
			if (o) o->new_ord = 0;
			if (u.recharging != 0) return 0;
			if (immortal_melee) goto recharge;
		} else {
			set_attacking(g, u, whom);
		}
		// ---- delivering the attack
		{
			const Type& pt = *u.ptype;
			const int32_t fp = g.fire_proj.empty() ? 0 : g.fire_proj[size_t(pt.type)];
			if (raw_max_range(g, pt) == 0) {
				if (fp >= 0 || p5) {
					const int32_t n = u.guy_mark;
					for (int32_t i = 0; i < n; i++) do_damage(g, u, ox, whom, a, n, -1, 0x100, false, false);
				}
			} else if (!(pt.unit_flags & utflag::kBroadside)) {
				const bool am = type_is(pt, 62, true) && !(u.unit_masks & umask::kPacked);
				if (fp >= 0 || am) {
					const int32_t save = fp;
					if (am && !g.fire_proj.empty()) g.fire_proj[size_t(pt.type)] = 7;  // first_ammo_piece + 7
					fire_ammo(g, u, ox, whom);
					if (am && !g.fire_proj.empty()) g.fire_proj[size_t(pt.type)] = save;
					if (pt.unit_flags & utflag::kFireship) {
						object_die(g, u, kDeathSploded, -1, 0.0f);
						return 0;
					}
				}
			}
			// (UNITTYPE_BROADSIDE: no shipped type has it; TODO.)
		}
	recharge:
		if (!u.alive()) return 0;
		{
			int32_t sup = -1;
			u.recharging = uint8_t(unit_recharge(g, u, &sup));
			// TODO(AI armies): army target when supplied.
			for (Guy& gy : u.guys) gy.guy_flags &= uint16_t(~4u);
		}
		return 1;
	}
	if (p5) {
		int32_t w = -1;
		const int32_t t = find_melee_target(g, u, -1, &w, true, false, 0);
		u.cavarch_o = int16_t(t);
		u.cavarch_who = int8_t(w);
		if (t >= 0) u.cavarch_uid = g.object(w, t)->uid;
		return 0;
	}
	// ---- out of range: chase or give up
	int32_t tox = ox, tw = whom;
	bool proceed = true;
	if ((u.ptype->unit_flags2 & utflag2::kPacker) && !p3) {
		tox = find_new_target(g, u, &tw, false);
		proceed = tox >= 0 && tw >= 0;
	}
	if (!proceed || p4) return 0;
	int32_t px = 0, py = 0;
	auto give_up = [&]() {
		if (u.order_type() == kAttack) {
			const Order* o = update_order(u);
			if (o->ox == tox && o->whom == tw) {
				bool home = false;
				int32_t hx = 0, hy = 0;
				if (o->defensive) {
					hx = o->def_x;
					hy = o->def_y;
					home = hx >= 0 && hy >= 0;
				}
				kill_current_order(g, u);
				if (home && !update_order(u)) {
					Order* m = add_move_order(g, u, hx, hy, 1, false, kQueueNew, false, -1, -1);
					if (m) m->flags |= orderflag::kReturnHome;
				}
			}
		}
	};
	const bool hold = (!p3 && ((get_combat_stance(g, u) == kStandGround && !on_duty(u)) ||
	                           ((u.unit_masks & umask::kEntrenched) && !(u.unit_masks2 & umask2::kMarineEntrench))));
	if (hold || !find_attack_pos(g, u, tox, tw, 0, &px, &py, 0)) {
		find_new_target(g, u, nullptr, false);
		give_up();
		if (u.order_type() == kAttack && u.recharging == 0) u.unit_masks2 |= umask2::kHoldAnim;
		return 0;
	}
	if (get_combat_stance(g, u) == kDefensive && !p3) {
		const Order* o = u.front();
		if (o && o->defensive && o->def_x >= 0 && o->def_y >= 0) {
			const int32_t L = std::max(RON_K(g, "unit_defensive_respond_range") * 192, obj_max_range(g, u) * 192);
			if (vector_dist(u.x - o->def_x, u.y - o->def_y) >= L) {
				find_new_target(g, u, nullptr, false);
				if (u.order_type() != kAttack || u.recharging != 0) return 0;
				const Order* o2 = u.front();
				if (o2->ox == ox0 && o2->whom == whom0) {
					give_up();
					return 0;
				}
				u.unit_masks2 |= umask2::kHoldAnim;
				return 0;
			}
		}
	}
	add_move_order(g, u, px, py, 1, false, kQueueFirst, false, -1, -1);
	if (px != u.x && py != u.y) {
		Order* act = update_action(g, u);
		if (act) {
			if (!(act->flags & orderflag::kApproach)) {
				act->flags |= orderflag::kApproach;
				work(g, u);
			} else {
				act->flags &= uint8_t(~orderflag::kApproach);
			}
		}
	}
	return 0;
}

void cavarch_fight(Game& g, Unit& u) {
	// @5ff4b0
	const int32_t o0 = u.cavarch_o, w0 = u.cavarch_who;
	const bool mar = (u.ptype->unit_flags & utflag::kMeleeAndRanged) != 0;
	const int32_t st = g.range_swap_type, sv = g.range_swap_value;
	if (mar) {
		g.range_swap_type = u.ptype->type;
		g.range_swap_value = u.ptype->second_max_range;
	}
	fight(g, u, u.cavarch_o, u.cavarch_who, int32_t(u.unit_masks2 & umask2::kKeepTarget), 0, 1);
	if (mar) {
		g.range_swap_type = st;
		g.range_swap_value = sv;
	}
	if (o0 >= 0 && w0 >= 0 && (u.cavarch_o != o0 || u.cavarch_who != w0)) {
		const Object* old = g.object(w0, o0);
		if (!(u.cavarch_who == w0 && old && obj_captain(g, *old) == u.cavarch_o)) u.unit_masks2 &= ~umask2::kKeepTarget;
	}
	if (u.cavarch_o < 0 && o0 < 0) u.cavarch_o = -2;
}

// ---------------------------------------------------------------- buildings

void build_process_attack(Game& g, Build& b) {
	// The attack part of Build::process @61edf0 (ACTIVE buildings).
	if (b.ptype->attack == 0) return;
	if (b.attack_ox < 0 || b.attack_whom < 0) {
		if ((b.o + g.frame) % 32 == 0) build_do_attack(g, b);
		else if (b.near_o >= 0 && b.near_who >= 0 && is_in_range(g, b, b.near_o, b.near_who, 0)) build_do_attack(g, b);
	} else {
		build_do_attack(g, b);
	}
	// TODO(combat-spec 4.13.1): anti-air turret angle (job_counter) stepping.
}

int32_t build_do_attack(Game& g, Build& b) {
	// @6228f0
	const bool aa = has_objmask(b, objmask::kAntiAir) && !type_is(*b.ptype, 521, true) && !type_is(*b.ptype, 522, true);
	if (!aa && b.recharging != 0) {
		b.recharging--;
		return 0;
	}
	if ((g.frame + b.o) % 32 == 0 && !(b.flags & objflag::kAttacking)) b.visible = 0;
	b.flags &= uint8_t(~objflag::kAttacking);
	if (obj_hits_left(g, b) == 0) return 0;
	// TODO(jamming): ObjectData::is_jammed.
	b.build_masks &= ~0x8000u;
	const int32_t arrows = get_garrison_arrows(g, b);
	if (arrows == 0) return 0;
	if (!(b.build_masks & 4)) build_find_target(g, b);
	if (b.attack_ox < 0 || b.attack_whom < 0) {
		b.build_masks &= ~4u;
		b.attack_whom = -1;
		b.attack_ox = -1;
		return 0;
	}
	if (!valid_target(g, b, b.attack_ox, b.attack_whom, nullptr)) {
		build_find_target(g, b);
		return 0;
	}
	if (!(b.build_masks & 4) && (g.frame == 0 || (b.o + 14 + g.frame) % 32 == 0)) {
		const Object* t = g.object(b.attack_whom, b.attack_ox);
		bool repick = !t->is_unit;
		if (!repick) {
			const Unit& tu = static_cast<const Unit&>(*t);
			const Order* ta = get_action(tu);
			repick = unit_is_moving(tu) || (!(tu.ptype->role & role::kCombat) && !(ta && ta->type == kCastSpell));
		}
		if (repick) {
			build_find_target(g, b);
			return 0;
		}
	}
	if (is_in_range(g, b, b.attack_ox, b.attack_whom, 0)) {
		if (aa) return 0;  // TODO(combat-spec 4.13.4): air-defence turrets fire from Wall::inc_time events
		const int32_t whom = b.attack_whom;
		fire_ammo(g, b, b.attack_ox, b.attack_whom);
		b.flags |= objflag::kAttacking;
		if (whom >= 0 && whom < 8 && !(b.visible & (1 << whom))) {
			b.visible = int8_t(b.visible | (1 << whom));
			if (!(b.ever_seen & (1 << whom))) object_update_local_seen(g, b);
		}
		b.recharging = int16_t(int16_t(b.ptype->recharge) / arrows);
		return 1;
	}
	b.build_masks &= ~4u;
	b.attack_whom = -1;
	b.attack_ox = -1;
	return 0;
}

}  // namespace ron::sim
