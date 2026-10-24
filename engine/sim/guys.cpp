// Guys and the unit-side calls that drive them (docs/movement-spec.md sections 5, 9.2, 9.3, 10.1; docs/frame-spec.md
// sections 7-9). Ported from re/decomp/move_guy (Guy_*.c, GuyData_*.c, asm/) and re/decomp/move (Unit_set_*).
//
// Not maintained here (render-only state the original keeps in the same objects): gpiece (Guy::update_gpiece),
// pivots and turret limits (Guy::reset_pivots / set_all_pivots), bank / pitch (GuyData +0x44..+0x50, walked but not
// in our Guy), turret_inc (+0x40, walked), last_norm, curr_bbox, the river cache, the per-node event arrays
// (+0xc4..+0xe0) and the render-time copies (+0xbc/+0xc0) written by set_anim.
//
// Art data in the simulation (docs/anim-spec.md): each guy has a graphics piece (gpiece, Guy::update_gpiece), and
// that piece's AnimationPacket tells whether an animation exists, its frame count (end_time) and whether it loops
// (g.anims, sim/anims.hpp). A null g.anims stands for "no art": nothing present, 3-frame animations, looping by
// category (the original cannot run like that: "No Animation Packet").
#include "sim/ai.hpp"
#include "sim/aircraft.hpp"
#include "sim/animals.hpp"
#include "sim/guys.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>

#include "sim/compass.hpp"
#include "sim/combat.hpp"
#include "sim/game.hpp"
#include "sim/garrison.hpp"
#include "sim/groups.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/spells.hpp"
#include "sim/transport.hpp"
#include "sim/trig.hpp"

// The terrain z and walk-ratio math must be single-precision IEEE with no fused multiply-add.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ron::sim {

namespace {

// ---------------------------------------------------------------- constants and small helpers

// Constants byte offsets (struct Constants; GuyData::turn_speed @5de340 reads +0x8/+0xc, set_anim +0x4).
constexpr uint32_t kCUnitMoveSpeed = 0x4, kCUnitTurnSpeed = 0x8, kCUnitPackTurnBonus = 0xc;
// TODO: GameAccess::ai_speed (game option; Guy::move multiplies crew steps by it when > 1). 1 = normal.
constexpr int32_t kAiSpeed = 1;
// TODO: global land_height (#NEW_TERRAIN_CONST, stock 10) used when the terrain has no height grid.
constexpr float kLandHeight = 10.0f;
// TerrainOut +0x4b7c: tiles per wcoord.
constexpr int32_t kTilesPerW = 4;

// UnitAnim values without a name in objects.hpp.
constexpr int32_t kAnimGroupIdle1 = 4, kAnimGroupIdle2 = 5, kAnimGroupIdle3 = 6, kAnimAttack3 = 13,
                  kAnimWalkWithWood = 26, kAnimWalkToWood = 28, kAnimMineOre = 29, kAnimWalkWithOre = 30,
                  kAnimWalkToOre = 32;
constexpr int32_t kTypeBird = 0x192;  // 402; 0x193/0x194 are the other gaia fliers that jog at random

// UnitAnimCat @af4370 (read from the exe).
constexpr int32_t kUnitAnimCat[38] = {0,  0,  0,  0,  0,  0,  0,  8,  8,  8,  10, 12, 12, 12, 12, 15, 15, 15, 15,
                                      15, 15, 21, 22, 23, 24, 25, 8,  27, 8,  29, 8,  31, 8,  33, 34, 35, 36, 37};

int32_t cat(int32_t anim) { return unit_anim_cat(anim); }

int32_t konst(const Game& g, uint32_t off) { return g.rules->constants.at(off); }

// cvttss2si: truncation, 0x80000000 for NaN and out-of-range values.
int32_t cvttss2si(float f) {
	if (!(f > -2147483904.0f && f < 2147483648.0f)) return INT32_MIN;
	return int32_t(f);
}

uint32_t abs_angle(uint32_t d) { return d > 0x80000000u ? ~d : d; }  // the angle_diff idiom

// The guy's piece has an AnimationPacket (Guy::set_anim's "No Animation Packet" test).
bool anim_packet(const Game& g, const Guy& gy) { return g.anims && g.anims->has_packet(gy.gpiece); }

// "Animation packet has action `anim`" (AnimationPacket::get_animobj != 0).
bool anim_has(const Game& g, const Guy& gy, int32_t anim) { return g.anims && g.anims->has_anim(gy.gpiece, anim); }

// end_time of an animation (animmgr.frames of the action's entry, 3 without one; Guy::set_anim @5db507).
uint32_t anim_frames(const Game& g, const Guy& gy, int32_t anim) {
	return g.anims ? uint32_t(g.anims->frames(gy.gpiece, anim)) : 3u;
}

// animmgr.loopings of the action's entry (inlined in Guy::inc_time @5d9e10). Without art: the anim_graphics.xml
// sections by category (attacks incl. ATTACKWALK, deaths, pack/unpack and dumps are NONLOOPING in the stock files).
bool anim_loops(const Game& g, const Guy& gy, int32_t anim) {
	if (g.anims) return g.anims->loops(gy.gpiece, anim);
	const int32_t c = cat(anim);
	return !(c == 10 || c == 12 || c == 15 || anim == kCharPack || anim == kCharUnpack || anim == 27 || anim == 31);
}

bool is_captain(const Unit& u) { return u.o_up < 0; }  // UnitData::is_captain @46ceb0
bool is_peasant(const Unit& u) { return u.type == 50 || u.type == 51; }  // ObjectData::is_peasant @46d310

// WorldData::is_valid @43f360
bool world_valid(const Game& g, int32_t x, int32_t y) {
	return x >= 0 && y >= 0 && x < g.world.tile_xs * kTCoordUnits && y < g.world.tile_ys * kTCoordUnits;
}

// Clamp a guy destination to the map (Guy::set_angle / set_new_location crew order: low bounds first).
void clamp_low_first(const Game& g, int32_t& x, int32_t& y) {
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	const int32_t mx = g.world.xs * kWCoordUnits, my = g.world.ys * kWCoordUnits;
	if (x >= mx) x = mx - 1;
	if (y >= my) y = my - 1;
}

// The crew retarget shared by Guy::set_angle @5d9010 and Guy::set_new_location @5d86f0: the trailing figure's
// destination is the lead's position plus its track offset rotated by `a`. Clamped only with a non-zero track.
void crew_target(const Game& g, Guy& c, int32_t lx, int32_t ly, uint32_t a) {
	c.des_x = lx;
	c.des_y = ly;
	bool moved = false;
	if (c.track_dx != 0) {
		const int32_t sy = sin_a(a, c.track_dx);
		const int32_t sx = sin_a(a + 0x40000000u, c.track_dx);
		c.des_x += sx;
		c.des_y += sy;
		moved = true;
	}
	if (c.track_dy != 0) {
		const int32_t sy = sin_a(a + 0x40000000u, c.track_dy);
		const int32_t sx = sin_a(a + 0x80000000u, c.track_dy);
		c.des_x += sx;
		c.des_y += sy;
		moved = true;
	}
	if (moved) clamp_low_first(g, c.des_x, c.des_y);
}

// ObjectData::get_inside @651a80: the outermost container (o, *who) of a garrisoned unit, -1 if none.
int32_t get_inside(const Game& g, const Unit& u, int32_t* who) {
	*who = -1;
	if (u.inside_up < 0) return -1;
	int32_t w = u.inside_up_who, o = u.inside_up;
	const Object* c = g.object(w, o);
	while (c && c->is_unit) {
		const Unit& cu = static_cast<const Unit&>(*c);
		if (cu.inside_up < 0) break;
		w = cu.inside_up_who;
		o = cu.inside_up;
		c = g.object(w, o);
	}
	*who = w;
	return o;
}

// UnitData::curr_uber_size(0) @60a760: alive members from the captain down the o_down chain.
int32_t curr_uber_size(const Game& g, const Unit& u) {
	const Unit* c = &u;
	while (!is_captain(*c)) {
		const Unit* up = g.unit(c->who, c->o_up);
		if (!up) break;
		c = up;
	}
	int32_t n = 1;
	for (const Unit* d = c->o_down >= 0 ? g.unit(c->who, c->o_down) : nullptr; d && d->alive();
	     d = d->o_down >= 0 ? g.unit(d->who, d->o_down) : nullptr)
		n++;
	return n;
}

// ObjectsData::find(x, y, SEARCH_NON_ALLIED, who, range, FILTER_ALL, ...) == -1 @65c6b0, as used by the group idle:
// no alive object of players 0..7 that the searcher is not allied with, linked in the wcoord lists, within
// vector_dist `range`. Approximation (TODO): the original only scans the wcoords of circle_x/circle_y (radius table
// not dumped, movement-spec open question 7), uses Search::valid_search for the diplomacy test, and also writes
// objects+0x1fc / +0x200 (the latter is read by invalid_loc).
bool no_non_allied_near(const Game& g, const Unit& u, int32_t range) {
	for (int32_t w = 0; w < 8; w++) {
		if (is_ally(g, u.who, w)) continue;
		const ObjectList& L = g.objects[size_t(w)];
		for (const auto& p : L.units)
			if (p && p->alive() && p->linked && vector_dist(p->x - u.x, p->y - u.y) <= range) return false;
		for (const auto& p : L.builds)
			if (p && p->alive() && p->linked && vector_dist(p->x - u.x, p->y - u.y) <= range) return false;
	}
	return true;
}

// The CHAR_GROUP_IDLE attempt of Guy::set_anim (@5da7b5..5dab9a) for the captain of a 3-man uber unit standing
// still: when both followers stand idle at about guy_spacing beside it, square to its facing, with exactly one on
// its right, the captain's guy plays GROUP_IDLE2 and each follower's guy 0 gets set_anim(GROUP_IDLE1 / 3, 1, 1)
// (which, being DEFAULT category, re-picks an idle variant and draws game_random). Returns the anim (5 or 0).
int32_t try_group_idle(Game& g, Unit& u, Guy& gy) {
	const Type& t = *u.ptype;
	if (curr_uber_size(g, u) != t.uber_size || t.uber_size != 3) return 0;
	if (!no_non_allied_near(g, u, 0xc00)) return 0;
	const int32_t o1 = u.o_down;
	Unit* u1 = o1 >= 0 ? g.unit(u.who, o1) : nullptr;
	if (!u1) return 0;
	const int32_t o2 = u1->o_down;
	if (o1 <= gy.o || o2 <= gy.o) return 0;
	Unit* u2 = g.unit(u.who, o2);
	if (!u2 || u1->order_type() != kOrderNone || u2->order_type() != kOrderNone) return 0;

	int32_t far = 0, leader = -1;
	for (int32_t m = u.o_down; m >= 0;) {
		const Unit* um = g.unit(u.who, m);
		if (!um) break;
		const int32_t d = vector_dist(iabs(u.x - um->x), iabs(u.y - um->y));
		if (iabs(d - t.guy_spacing) > 0x30) far = 1;
		const uint32_t a = find_angle(um->x - u.x, um->y - u.y) - u.angle;
		int32_t bad;
		if (a < 0x80000000u) {  // on the captain's right
			bad = leader < 0 ? far : 1;
			leader = m;
		} else {
			bad = far;
		}
		const uint32_t aa = abs_angle(a);
		if (aa < 0x38e38e3au || aa > 0x471c71c6u) bad = 1;  // not within 80..100 degrees
		const uint32_t d2 = abs_angle(u.angle - u1->angle);
		far = d2 > 0x71c71c6u ? 1 : bad;  // facings within 10 degrees
		m = um->o_down;
	}
	if (leader < 0 || far != 0) return 0;

	gy.cur_time = 0;
	gy.cur_anim = int8_t(kAnimGroupIdle2);
	for (int32_t m = u.o_down; m >= 0;) {
		Unit* um = g.unit(u.who, m);
		if (!um) break;
		if (!um->guys.empty())
			guy_set_anim(g, *um, um->guys[0], m == leader ? kAnimGroupIdle1 : kAnimGroupIdle3, 1, 1);
		m = um->o_down;
	}
	return kAnimGroupIdle2;
}

// cur_time -= min(cur_time, end0) (@5db1fc / @5db4fa)
void consume_time(Guy& gy, uint32_t end0) { gy.cur_time -= gy.cur_time < end0 ? gy.cur_time : end0; }

}  // namespace

int32_t unit_anim_cat(int32_t anim) { return anim >= 0 && anim < 38 ? kUnitAnimCat[anim] : anim; }

// ---------------------------------------------------------------- terrain z

int32_t find_tcoord_z(const Game& g, int32_t tx, int32_t ty, int32_t flag) {
	const Heights& H = g.heights;
	if (H.h.empty()) return cvttss2si(kLandHeight);  // TerrainOut +0x498 == 0
	const World& w = g.world;
	// @8544a0: water test with the wcoord bounds (tx >> 2 < xs); applies whatever the flag.
	if (w.in_t(tx, ty) && (w.t(tx, ty) & tmask::kMedium) == tmask::kWater) return 0;
	const int64_t row = int64_t(w.xs) * 4 + 1;
	const int64_t i0 = row * (int64_t(ty) + 1) + tx, i1 = row * ty + tx + 1;
	// Off-map tiles read outside the grid in the original; never reached by the callers (guarded here).
	if (i0 < 0 || i1 < 0 || size_t(i0) >= H.h.size() || size_t(i1) >= H.h.size()) return 0;
	float f = H.h[size_t(i0)];
	f = f + H.h[size_t(i1)];
	f = f * 0.5f;
	const int32_t z = cvttss2si(f);
	if (z < 0 && flag == 1) return 0;
	return z;
}

int32_t find_tcoord_z_at(const Game& g, int32_t x, int32_t y, int32_t flag) {
	return find_tcoord_z(g, tcoord_of(x), tcoord_of(y), flag);
}

int32_t find_data_z(const Game& g, int32_t x, int32_t y, int32_t flag) {
	const Heights& H = g.heights;
	if (H.h.empty()) return cvttss2si(kLandHeight);
	const World& w = g.world;
	int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	// flag 1 only: a water tile (on the map) is 0.
	if (flag == 1 && tx >= 0 && ty >= 0 && tx < w.tile_xs && ty < w.tile_ys &&
	    (w.t(tx, ty) & tmask::kMedium) == tmask::kWater)
		return 0;
	if (tx < 0) tx = 0;
	if (ty < 0) ty = 0;
	if (tx >= w.xs * kTilesPerW) tx = w.xs * kTilesPerW - 1;
	if (ty >= w.ys * kTilesPerW) ty = w.ys * kTilesPerW - 1;
	const int64_t v = (int64_t(w.xs) * 4 + 1) * ty + tx;
	const int64_t rowsize = H.vxs;  // global rowsize (= 4xs+1)
	if (v < 0 || size_t(v + rowsize + 1) >= H.h.size()) return 0;  // malformed grid (never with a loaded map)
	const float h00 = H.h[size_t(v)], h10 = H.h[size_t(v + 1)], h01 = H.h[size_t(v + rowsize)],
	            h11 = H.h[size_t(v + rowsize + 1)];
	const int32_t a = x - tx * kTCoordUnits, b = y - ty * kTCoordUnits;
	const float k = 0.0052083335f;  // 0x3baaaaab = 1/192
	float r;
	if (a <= kTCoordUnits - b) {  // upper-left triangle
		float s3 = h10 - h00;
		s3 = s3 * k;
		s3 = s3 * float(a);
		float s4 = h01 - h00;
		s4 = s4 * k;
		s4 = s4 * float(b);
		r = s3 + h00;
		r = r + 0.0f;
		r = r + s4;
	} else {  // lower-right triangle
		float s4 = h01 - h11;
		s4 = s4 * k;
		s4 = s4 * float(kTCoordUnits - a);
		float s2 = h11 - h10;
		s2 = s2 * k;
		s2 = s2 * float(b);
		r = h10 + s2;
		r = r + 0.0f;
		r = r + s4;
	}
	const int32_t z = cvttss2si(r);
	if (z < 0 && flag == 1) return 0;
	return z;
}

// ---------------------------------------------------------------- unit side

int32_t unit_set_new_location(Game& g, Unit& u, int32_t x, int32_t y, bool p3, bool p4) {
	const World& w = g.world;
	const int32_t otx = tcoord_of(u.x), oty = tcoord_of(u.y);
	const int32_t owx = wcoord_of(u.x), owy = wcoord_of(u.y);
	const int32_t ntx = tcoord_of(x), nty = tcoord_of(y);
	const bool on_map = u.on_map();
	const bool wc = on_map && (owx != wcoord_of(x) || owy != wcoord_of(y));
	const bool tc = on_map && (otx != ntx || oty != nty);

	bool check_transform = wc;
	if (!wc && tc) {
		// Tile-only change: the old wcoord's HALF_LAND bit decides whether the amphibious test runs.
		const bool half = w.in_w(owx, owy) && (w.w(owx, owy).flags & wflag::kHalfLand) != 0;
		// @5f8e60: tribe bonus 0x12 (Iroquois) ground units: unit_masks 0x800 (UNDER_COVER) = "standing on a tree tile".
		// It is written on every tile-only move, whatever HALF_LAND says.
		if (has_tribe_bonus(g, u.who, 0x12) && u.ptype->domain == 0) {
			if (w.in_t(ntx, nty) && (w.t(ntx, nty) & tmask::kMedium) == tmask::kTree) u.unit_masks |= umask::kUnderCover;
			else u.unit_masks &= ~umask::kUnderCover;
		}
		check_transform = half;
	}
	if (check_transform && unit_can_transport(u)) {
		// @5f8f30: amphibious units (docs/transport-spec.md 3). A ground unit entering a water tile queues the TRANSPORT
		// cast at the front and does NOT move (the exe returns 0 right after add_cast_order; the cast creates the barge
		// next to it); a sea unit (the barge) leaving the water ejects its contents, dies when empty and does not move.
		const int32_t d = u.ptype->domain;
		const uint16_t medium = w.in_t(ntx, nty) ? uint16_t(w.t(ntx, nty) & tmask::kMedium) : uint16_t(0);
		if (d == 0 && medium == tmask::kWater) {
			if (transport_trace())
				std::fprintf(stderr, "[transport] f%d L%d unit %d type %d enters water at %d,%d -> cast order\n", g.frame, u.who, u.o,
				             u.type, x, y);
			add_cast_order(g, u, -1, -1, -1, -1, spell::kTransport, kQueueFirst, false);
			return 0;
		}
		if (d == 1 && medium != tmask::kWater) {
			if (transport_trace())
				std::fprintf(stderr, "[transport] f%d L%d barge %d type %d leaves the water at %d,%d (inside %d)\n", g.frame, u.who, u.o,
				             u.type, x, y, num_inside(g, u, 1));
			eject_contents(g, u, 0, -1, 1, 1);
			if (num_inside(g, u, 1) == 0) object_die(g, u, 0, -1, 0.0f);
			return 0;
		}
	}
	if (wc) remove_from_world(g, u);

	u.x = x;
	u.y = y;
	u.z = find_tcoord_z(g, ntx, nty, 1);
	if (wc) {
		add_to_world(g, u);
		// @5f90e7: Unit::explore_goody (ruins) for non-decoy ground units (not gaia animals: vslot 0x30) entering a GOODY
		// wcoord (the test reads WData.flags as a short: < 0 is FLAG_GOODY 0x8000).
		if (!is_animal_who(u.who) && !(u.unit_masks & umask::kDecoy) && u.ptype->domain == 0 &&
		    (g.world.w(wcoord_of(x), wcoord_of(y)).flags & 0x8000))
			explore_goody(g, u);
	}
	if (tc) {
		// A new 384-unit fog cell: Object::update_seen(teleport == 0) (a rim update for a normal move, the full disc for
		// a teleport), at the moment of the move.
		if ((otx >> 1) != fcoord_of(x) || (oty >> 1) != fcoord_of(y)) object_update_seen(g, u, !p3);
		// TODO(@5f912d): unit_masks2 0x10000 (the CEO) calls update_ceo_position(old tile, new tile).
	}
	unit_place_guys(g, u, p3, p4);
	return 1;
}

void unit_place_guys(Game& g, Unit& u, bool teleport, bool set_angle) {
	const int32_t n = u.guy_mark;
	if (u.guys.empty() || n <= 0) return;  // the original divides by zero for n == 0 (never alive)
	if (n == 1) {
		Guy& g0 = u.guys[0];
		if (set_angle) guy_set_angle(g, u, g0, u.angle, teleport);
		g0.des_x = u.x;
		g0.des_y = u.y;
		if (teleport) guy_set_new_location(g, u, g0, g0.des_x, g0.des_y, true);
		return;
	}
	const int32_t sp = (u.form == kFormSparse ? 2 : 1) * u.ptype->guy_spacing;
	int32_t A = sp == 0 ? 0 : sin_a(u.angle - 0x40000000u, sp);
	int32_t B = sp == 0 ? 0 : sin_a(u.angle, sp);
	const int32_t cols = u.form == kFormColumn ? (n != 1) + 1 : (n > 2 ? 3 : n);
	const int32_t R = (n - 1) / cols;
	if (u.unit_masks & umask::kFacing) {
		A = -A;
		B = -B;
	}
	const int32_t ox = (R * B) / 2 - ((cols - 1) * A) / 2;
	const int32_t oy = (R * A) / 2 + ((cols - 1) * B) / 2;
	const int32_t mx = g.world.xs * kWCoordUnits, my = g.world.ys * kWCoordUnits;
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) {
		Guy& gy = u.guys[size_t(i)];
		const int32_t row = i / cols, col = i % cols;
		if (set_angle) guy_set_angle(g, u, gy, u.angle, teleport);
		gy.des_x = col * A - row * B + u.x + ox;
		gy.des_y = u.y - col * B - row * A + oy;
		// Upper bound first (@5f9362).
		if (gy.des_x >= mx) gy.des_x = mx - 1;
		else if (gy.des_x < 0) gy.des_x = 0;
		if (gy.des_y >= my) gy.des_y = my - 1;
		else if (gy.des_y < 0) gy.des_y = 0;
		if (teleport) guy_set_new_location(g, u, gy, gy.des_x, gy.des_y, true);
	}
}

void unit_set_angle(Game& g, Unit& u, uint32_t a, bool now) {
	const uint32_t d = a - u.angle;
	if (d > 0x3fffffffu && d < 0xc0000001u) {  // more than 90 degrees: mirror the guy grid
		u.unit_masks ^= umask::kFacing;
		if (u.group >= 0 && u.group < kNumGroups) {
			Group& gr = g.groups.g[size_t(u.group)];
			int32_t leader = -1;
			if (gr.num < 1) {
				leader = -1;
			} else if (gr.buildings == 0) {
				const int32_t idx = group_find_leader(g, gr);  // groups.hpp: list index (the original returns o)
				leader = idx >= 0 && idx < kGroupMax ? gr.list[size_t(idx)] : -1;
			} else {
				leader = gr.list[0];
			}
			if (leader == u.o) gr.facing = gr.facing == 0;
		}
	}
	u.angle = a;
	if (!u.guys.empty()) guy_set_angle(g, u, u.guys[0], a, now);
}

void unit_set_anim(Game& g, Unit& u, int32_t anim, int32_t p2, int32_t p3) {
	// @616f40: hold_attack is cleared unless guy 0 (re-read every time) is attacking.
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) {
		if (cat(u.guys[0].cur_anim) != kCharAttack2) u.guys[size_t(i)].hold_attack = 0;
		guy_set_anim(g, u, u.guys[size_t(i)], anim, p2, p3);
	}
	for (size_t i = size_t(u.ptype->squad_size); i < u.guys.size(); i++) {
		if (cat(u.guys[0].cur_anim) != kCharAttack2) u.guys[i].hold_attack = 0;
		guy_set_anim(g, u, u.guys[i], anim, p2, p3);
	}
}

void unit_inc_time(Game& g, Unit& u) {
	// @610b40: on the map, or the two types (52/53) that animate inside buildings.
	if (!(u.inside_up < 0 || u.type == 0x34 || u.type == 0x35)) return;
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) guy_inc_time(g, u, u.guys[size_t(i)]);
	for (size_t i = size_t(u.ptype->squad_size); i < u.guys.size(); i++) guy_inc_time(g, u, u.guys[i]);
}

void unit_execute_events(Game& g, Unit& u) {
	// @60edc0: alive (is_active = flags & 1), on the map and not UNIT2_HOLD_ANIM -> Guy::execute_events @5d99c0 for
	// guys [0, guy_mark); otherwise only GraphicEvents::verify_load (render).
	if (!u.alive() || !u.on_map() || (u.unit_masks2 & umask2::kHoldAnim)) return;
	// Guy::execute_events @5d99c0 (combat_fight.cpp): release events fire projectiles. An aircraft carrier also runs its
	// launch sequence (Unit.trench_angle +0x5c as the animation counter, the PLANERELEASE events; sim/aircraft.cpp).
	const bool carrier = u.ptype->domain == kDomainSea && unit_is(g, u, 0x15f, true);
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) {
		if (carrier) carrier_launch_events(g, u, u.guys[size_t(i)]);
		guy_execute_events(g, u, u.guys[size_t(i)]);
		if (!u.alive()) return;
	}
}

// ---------------------------------------------------------------- graphics pieces

int32_t unit_gpiece(const Game& g, int32_t type, int32_t who, int32_t o, int32_t crew, bool packed) {
	using namespace gpiece;
	if (!g.anims) return -1;
	const AnimSource& a = *g.anims;
	if (uint32_t(type - 0x192) < 0xcu) {
		// Gaia animals: one of three pieces by (GameInfo.seed + o) % 3, falling back to lower ones.
		int32_t p = -1;
		for (int32_t k = int32_t((g.seed + uint32_t(o)) % 3u); k >= 0; k--) {
			p = kFirstBird - 0x192 * 3 + type * 3 + k;
			if (a.piece_exists(p)) return p;
		}
		return p;  // "NO GAIAN GPIECE - VERY BAD" (ignored)
	}
	int32_t bucket = 0, style = 0;
	if (who != -1) {
		const Leader& l = g.leaders[size_t(who)];
		const int32_t age = l.age;  // (LeaderData age ^ 0x62766) + param_7 (0 here)
		bucket = age < 5 ? age / 3 : 2;
		const Tribe* tr = size_t(l.tribe) < g.rules->tribes.size() ? &g.rules->tribes[size_t(l.tribe)] : nullptr;
		style = tr ? tr->unit_continent : 0;
		if ((type == 0x3d || type == 0x3e || type == 400) && !packed && tr) {
			// Merchants: build-style pieces (KOREAN 3, NEUROPE 1, IROQUOIS 6, COLONIAL 7, EINDIAN 8) in the AGE3
			// bucket (Iroquois also AGE0); an ARAB (5) build style uses the ARAB unit style.
			const int32_t bs = tr->build_continent;
			if (bs == 5) style = 1;
			if ((bucket == 1 || (bucket == 0 && bs == 6)) && (bs == 3 || bs == 1 || bs >= 6)) {
				int32_t p = kTotalUnit - 6;
				if (bs == 1) p = kTotalUnit - 5;
				else if (bs == 6) p = kTotalUnit - 6 + 2 + (bucket != 0 ? 1 : 0);
				else if (bs == 7) p = kTotalUnit - 2;
				else if (bs == 8) p = kTotalUnit - 1;
				if (a.piece_exists(p)) return p;
			}
		}
	}
	auto piece = [&](int32_t b, int32_t s, bool gender) {
		return kPerAge * b + kPerStyle * s + kPerCrew * crew + (gender ? kPerGender : 0) + kFirstUnit + type - 50;
	};
	// The gender slot (-FEMALE / -PACKED): packed units, or odd o for types without UnitType flag 4 (unit_flags2).
	const bool gender = packed || !((g.type(type).unit_flags2 & 4) || o == -1 || (o & 1) == 0);
	if (gender) {
		for (int32_t b = bucket; b >= 0; b--)
			if (a.piece_exists(piece(b, style, true))) return piece(b, style, true);
		for (int32_t b = bucket; b >= 0; b--)
			if (a.piece_exists(piece(b, 0, true))) return piece(b, 0, true);
	}
	for (int32_t b = bucket; b >= 0; b--)
		if (a.piece_exists(piece(b, style, false))) return piece(b, style, false);
	for (int32_t b = bucket; b >= 0; b--)
		if (a.piece_exists(piece(b, 0, false))) return piece(b, 0, false);
	return kFirstUnit;  // "NO DEFAULT UNIT GPIECE - VERY BAD" (ignored)
}

void guy_update_gpiece(Game& g, Unit& u, Guy& gy) {
	// Merchants also count as packed while packing (UnitData::is_packing: a CAST_SPELL order of a pack spell; no
	// cast orders exist yet).
	const bool packed = (u.unit_masks & umask::kPacked) != 0;
	gy.gpiece = unit_gpiece(g, u.type, gy.who, gy.o, gy.guy_num, packed);
	// (GraphicEvents::verify_load is render data.) Guys other than guy 0 take the piece's track offsets in world
	// units, trunc((ox * guy_scale) * scale) and trunc((guy_scale * oy) * scale) in single precision (crew trailing
	// positions, Guy::set_angle / set_new_location); everyone else 0.
	float scale = 1.0f, ox = 0.0f, oy = 0.0f;
	if (gy.guy_num != 0 && g.anims && g.anims->track(gy.gpiece, &scale, &ox, &oy)) {
		gy.track_dx = cvttss2si((ox * kGuyScale) * scale);
		gy.track_dy = cvttss2si((kGuyScale * oy) * scale);
	} else {
		gy.track_dx = 0;
		gy.track_dy = 0;
	}
}

void unit_update_gpiece(Game& g, Unit& u) {
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) guy_update_gpiece(g, u, u.guys[size_t(i)]);
	const int32_t end = std::min(u.ptype->squad_size + u.ptype->crew_size, int32_t(u.guys.size()));
	for (int32_t i = u.ptype->squad_size; i < end; i++) guy_update_gpiece(g, u, u.guys[size_t(i)]);
}

// ---------------------------------------------------------------- Guy

void guy_init_real(Game& g, Unit& u, Guy& gy, int32_t keep) {
	// Guy::update_gpiece (the piece keys the animation data; the crew track_dx/dy it sets are zeroed below and set
	// again by the Unit::update_gpiece that follows in Unit::init).
	guy_update_gpiece(g, u, gy);
	gy.cur_anim = 0;
	const int32_t r = g.rnd_draw(100);  // @5db6e9: the one draw per guy
	if (r >= 90) gy.cur_anim = kCharIdle3;
	else if (r >= 80) gy.cur_anim = kCharIdle2;
	else if (r >= 70) gy.cur_anim = kCharIdle1;
	gy.cur_time = 0;
	gy.last_time = -1;
	gy.end_time = 0;
	// The packet must have the variant; a piece without a packet reports "No Animation Packet" and keeps the drawn
	// variant (as does a null g.anims).
	if (anim_packet(g, gy) && !anim_has(g, gy, gy.cur_anim)) gy.cur_anim = 0;
	gy.turret_angles = {};
	gy.des_turret_angles = {};
	// TODO(render data): Guy::reset_pivots(1) sets turret_angles / des_turret_angles from the graphics piece's pivot
	// restrictions (degrees_to_angle of each limit).
	if (keep == 0) {
		// bank, last_bank, pitch, last_pitch = 0 (not in Guy)
		gy.des_x = 0;
		gy.des_y = 0;
		gy.des_angle = 0;
	}
	gy.stopped = 1;
	gy.hold_attack = 0;
	gy.queued_attack = 0;
	gy.guy_flags = 0;
	gy.last_speed = 0;
	gy.avg_speed = 0;
	gy.track_dx = 0;
	gy.track_dy = 0;
	// TODO(render data): guy_flags 0x100 (turret) when GraphicPieces::pivot_restrictions has pivots for the type.
	if (anim_has(g, gy, kCharTurnRight)) gy.guy_flags |= 8;  // packet has more than 22 actions incl. TURN_RIGHT
	const Type& t = *u.ptype;
	if ((t.unit_flags2 & 4) == 0) {
		// ObjectData::has_objmask -> ObjectType vfunc +0xf0 (obj_masks & mask).
		if ((t.obj_masks & 0x20) || (t.obj_masks & 0x1000) || (t.unit_flags & 0x10)) {
			if ((t.unit_flags & 2) == 0) gy.guy_flags |= 0x10;  // turns instantly when standing
		}
	} else {
		gy.guy_flags |= 8;  // packers
	}
	if (t.domain == 2 && (t.unit_flags & 0x20) == 0) gy.guy_flags |= 0x40;  // UnitData::is_plane
	if (t.type == 0x34 || t.type == 0x35) gy.guy_flags |= 0x80;
}

uint32_t guy_turn_speed(const Game& g, const Unit& u, const Guy& gy, bool full) {
	uint32_t base = 0x40000000u;
	if (gy.guy_num < u.ptype->squad_size) {
		base = (uint32_t(u.ptype->turn_speed) >> 8) * uint32_t(konst(g, kCUnitTurnSpeed));
		if (u.unit_masks & umask::kPacked) base *= uint32_t(konst(g, kCUnitPackTurnBonus));
	} else if (gy.track_dx != 0 || gy.track_dy != 0) {
		return 0x40000000u;
	}
	if (gy.last_speed == 0 && (gy.guy_flags & 0x10)) return 0x80000000u;
	if (full) return base;
	const uint32_t lo = uint32_t(konst(g, kCUnitTurnSpeed)) * 0xb60bu;  // 1 degree
	const uint32_t div = uint32_t(gy.avg_speed / 4 + 1);
	const uint32_t q = div ? base / div : base;  // div 0 needs avg_speed in [-7, -4] (never)
	return q > lo ? q : lo;
}

int32_t guy_get_speed(const Game& g, const Unit& u, const Guy& gy) {
	int32_t s = get_speed(g, u, gy.x, gy.y, true);  // vtable +0x17c with nocap 1
	// Squad guys of a unit whose current order is a group order (UnitOrder vfunc +0x2c is_group) get +9.
	if (gy.guy_num < u.ptype->squad_size && !u.orders.empty() && order_is_group(u.orders.front().type)) s += 9;
	return s;
}

void guy_set_angle(Game& g, Unit& u, Guy& gy, uint32_t a, bool now) {
	gy.des_angle = a;
	if (now) {
		gy.angle = a;
		gy.last_angle = a;
	}
	if (gy.guy_num != 0) return;
	for (size_t i = size_t(u.ptype->squad_size); i < u.guys.size(); i++) {
		Guy& c = u.guys[i];
		c.des_angle = a;
		crew_target(g, c, gy.x, gy.y, a);
		if (now) {
			guy_set_angle(g, u, c, c.des_angle, true);
			guy_set_new_location(g, u, c, c.des_x, c.des_y, true);
		}
	}
}

void guy_do_turn(Game& g, Unit& u, Guy& gy, uint32_t target, uint32_t new_angle, int32_t p3, bool anim) {
	if (new_angle != gy.angle) {
		gy.guy_flags |= 2;
		if (anim && (gy.guy_flags & 8))
			guy_set_anim(g, u, gy, target - gy.angle < 0x80000001u ? kCharTurnRight : kCharTurnLeft, 0, 1);
	}
	const uint32_t saved = gy.des_angle;
	gy.angle = new_angle;
	guy_set_angle(g, u, gy, new_angle, false);
	gy.des_angle = saved;
	if (gy.guy_num != 0) return;
	for (size_t i = size_t(u.ptype->squad_size); i < u.guys.size(); i++) {
		Guy& c = u.guys[i];
		if (c.track_dx == 0 && c.track_dy == 0) guy_do_turn(g, u, c, target, new_angle, p3, anim);
	}
}

uint32_t guy_turn_towards(Game& g, Unit& u, Guy& gy, uint32_t target, int32_t /*p2*/, bool anim) {
	const uint32_t d = target - gy.angle;
	const uint32_t ad = abs_angle(d);
	uint32_t nw = target, rem = 0;
	if (ad >= 0x2222220u) {  // 3 degrees snap
		const uint32_t ts = guy_turn_speed(g, u, gy, true);
		if (ts < ad) {
			rem = ad - ts;
			nw = d < 0x80000001u ? gy.angle + ts : gy.angle - ts;
		}
	}
	guy_do_turn(g, u, gy, target, nw, 1, anim);
	return rem;
}

uint32_t guy_turn_angles(const Game& g, const Unit& u, const Guy& gy, uint32_t target, uint32_t* out, bool full,
                         bool half) {
	const uint32_t d = target - gy.angle;
	const uint32_t ad = abs_angle(d);
	if (ad < 0x2222220u) {
		*out = target;
		return 0;
	}
	uint32_t ts = guy_turn_speed(g, u, gy, full);
	if (half) ts >>= 1;
	if (ts < ad) {
		*out = d < 0x80000001u ? gy.angle + ts : gy.angle - ts;
		return ad - ts;
	}
	*out = target;
	return 0;
}

void guy_set_new_location(Game& g, Unit& u, Guy& gy, int32_t x, int32_t y, bool now) {
	const Type& t = *u.ptype;
	if (t.domain != 2 && gy.guy_num < t.squad_size) {
		// Squad figures of non-air units own a collision footprint (docs/movement-spec.md section 10.1).
		const int32_t oux = ucoord_of(gy.x), ouy = ucoord_of(gy.y), nux = ucoord_of(x), nuy = ucoord_of(y);
		if (oux != nux || ouy != nuy) coll_move_unit(g, oux, ouy, nux, nuy, t.new_block_radius);
	}
	gy.x = x;
	gy.y = y;
	if (t.domain == 2) {
		if (t.unit_flags & 0x20) {  // helicopters follow the terrain 30 units per call
			int32_t d = find_data_z(g, x, y, 0) - gy.z + 1000;
			if (d < -30) d = -30;
			if (d > 30) d = 30;
			gy.z += d;
		}  // planes keep their z
	} else if (t.domain == 1) {
		gy.z = 0;
	} else {
		gy.z = find_data_z(g, x, y, 0);
	}
	if (now) {
		gy.last_x = x;
		gy.last_y = y;
		gy.last_z = gy.z;
	}
	if (gy.guy_num != 0) return;
	for (size_t i = size_t(t.squad_size); i < u.guys.size(); i++) {
		Guy& c = u.guys[i];
		c.des_angle = gy.angle;
		crew_target(g, c, x, y, gy.angle);
		if (now) {
			guy_set_angle(g, u, c, c.des_angle, true);
			guy_set_new_location(g, u, c, c.des_x, c.des_y, true);
		}
	}
}

void guy_set_new_z(Guy& gy, int32_t z, bool now) {
	if (now) {
		gy.last_z = z;
		gy.z = z;
	} else {
		gy.last_z = gy.z;
		gy.z = z;
	}
}

void guy_update_z(Game& g, Unit& u, Guy& gy) { gy.z = u.ptype->domain != 1 ? find_data_z(g, gy.x, gy.y, 0) : 0; }

void guy_move(Game& g, Unit& u, Guy& gy) {
	const Type& t = *u.ptype;
	gy.last_x = gy.x;
	gy.last_y = gy.y;
	if (!(gy.guy_flags & 0x40)) gy.last_z = gy.z;
	gy.last_angle = gy.angle;

	if (gy.des_x == gy.x && gy.des_y == gy.y) {
		gy.last_speed = 0;
		if (gy.des_angle == gy.angle) {
			// Settled: a pending attack animation starts now; a finished walk drops to DEFAULT one frame later.
			const int8_t h = gy.hold_attack;
			if (h == 0) {
				if (gy.cur_anim == kCharWalk && gy.stopped != 0) guy_set_anim(g, u, gy, kCharDefault, 0, 1);
				gy.stopped = 1;
			} else {
				if (h <= 1) guy_set_anim(g, u, gy, kCharAttack1, 0, 1);
				else guy_set_anim(g, u, gy, h, 0, 0);
				// Guy::set_all_pivots: turrets only (guy_flags 0x100, render data; TODO).
				gy.hold_attack = 0;
				gy.stopped = 1;
			}
		} else {
			// Turning in place.
			if (t.domain == 1 || u.order_type() == kSpecialAnim) {
				gy.stopped = 1;
			} else {
				const int8_t h = gy.hold_attack;
				if (h == 0) {
					if (gy.cur_anim != kCharTurnLeft && gy.cur_anim != kCharTurnRight && gy.cur_anim != kCharAttackWalk)
						guy_set_anim(g, u, gy, kCharWalk, 0, 1);
				} else if (gy.cur_anim != kCharTurnLeft && gy.cur_anim != kCharTurnRight) {
					if (h > 1) guy_set_anim(g, u, gy, h, 0, 0);
					else guy_set_anim(g, u, gy, kCharAttack1, 0, 1);
				}
				// set_all_pivots for the hold_attack cases (turrets; TODO).
				gy.stopped = 0;
			}
			if (!(gy.guy_flags & 2)) guy_turn_towards(g, u, gy, gy.des_angle, 0, true);
		}
	} else {
		int32_t anim = kCharWalk;
		if (u.cavarch_o >= 0 && (t.unit_flags & 0x200000)) anim = kCharAttackWalk;
		const uint32_t m = u.unit_masks;
		if (m & 0x78000000u) {
			if (m & 0x10000000u) anim = kAnimWalkToWood;
			else if (m & 0x08000000u) anim = kAnimWalkWithWood;
			else if (m & 0x40000000u) anim = kAnimWalkToOre;
			else if (m & 0x20000000u) anim = kAnimWalkWithOre;
			// @5d9454: a worker walking for wood or ore whose action is a GATHER order that has been at its building
			// (been_there) marks the active gather building as in use: recharging++ and build_masks 0x800, once a frame
			// (Wall::process clears the bit). Without it the camp's counter lags the original by the walking frames.
			const Order* act0 = get_action(u);
			if (act0 && act0->type == kGather) {
				const Order* act = update_action(g, u);
				if (act && act->ox >= 0 && act->whom >= 0 && act->been_there != 0) {
					Object* tb = g.object(act->whom, act->ox);
					if (tb && !tb->is_unit && (tb->flags & objflag::kActive) && is_gather_type(*tb->ptype)) {
						Build& bb = static_cast<Build&>(*tb);
						if (!(bb.build_masks & bmask::kInUse)) {
							bb.recharging++;
							bb.build_masks |= bmask::kInUse;
						}
					}
				}
			}
		}
		if (!(gy.guy_flags & 0x40) && (!(t.unit_flags2 & 4) || (u.unit_masks & umask::kPacked)))
			guy_set_anim(g, u, gy, anim, 0, 1);

		const int32_t dx = gy.des_x - gy.x, dy = gy.des_y - gy.y;
		if (gy.guy_num == 0 || (gy.track_dx == 0 && gy.track_dy == 0)) {
			// Squad guys (and untracked crew) snap to their slot.
			gy.last_speed = vector_dist(dx, dy);
			guy_set_new_location(g, u, gy, gy.des_x, gy.des_y, false);
		} else {
			// Tracked crew (towed guns, trailing figures): turn first, then step along their own heading.
			const uint32_t rem = guy_turn_towards(g, u, gy, find_angle(dx, dy), 0, false);
			if (rem > 2u * guy_turn_speed(g, u, gy, true)) return;  // no stopped / avg_speed update
			const int32_t sp = guy_get_speed(g, u, gy) * 11 / 8;
			gy.last_speed = sp;
			const int32_t step = kAiSpeed > 1 ? kAiSpeed * sp : sp;
			const int32_t ady = iabs(dy), adx = iabs(dx);
			if (ady + adx <= step) {
				gy.last_speed = vector_dist(dx, dy);
				guy_set_new_location(g, u, gy, gy.des_x, gy.des_y, false);
			} else {
				int32_t sx = sin_a(gy.angle, step);
				int32_t sy = cos_a(gy.angle, step);
				if (iabs(sx) > adx) sx = dx;
				if (iabs(sy) > ady) sy = -dy;
				// ❓ off_x/off_y are only written by Unit::init; the subtraction sticks even when the step is invalid.
				gy.x -= gy.off_x;
				gy.y -= gy.off_y;
				const int32_t nx = gy.x + sx, ny = gy.y - sy;
				if (world_valid(g, nx, ny)) guy_set_new_location(g, u, gy, nx, ny, false);
			}
		}
		gy.stopped = 0;
	}
	gy.avg_speed = (gy.avg_speed * 3 + gy.last_speed) / 4;
}

void guy_process(Game& g, Unit& u, Guy& gy) {
	guy_move(g, u, gy);
	gy.guy_flags &= uint16_t(~2u);
	if (gy.guy_flags & 0x100) {
		// Turrets step 15 degrees per frame towards their targets; arrival sets node_flags bit k.
		for (int k = 0; k < 4; k++) {
			uint32_t cur = uint32_t(gy.turret_angles[size_t(k)]);
			const uint32_t des = uint32_t(gy.des_turret_angles[size_t(k)]);
			bool arrived = cur == des;
			if (!arrived) {
				const uint32_t d = cur - des;
				if (abs_angle(d) < 0xaaaaaaau) {
					cur = des;
					arrived = true;
				} else {
					cur = d < 0x80000000u ? cur - 0xaaaaaaau : cur + 0xaaaaaaau;
				}
				gy.turret_angles[size_t(k)] = int32_t(cur);
			}
			if (arrived) gy.node_flags = int16_t(uint16_t(gy.node_flags) | uint16_t(1u << k));
		}
	}
	if ((g.frame + int32_t(gy.o)) % 64 == 0 && gy.avg_speed == 0) {
		// Idle guys re-stamp their footprint (docs/movement-spec.md section 10.1); bits are never cleared here.
		const Type& t = *u.ptype;
		if (t.domain != 2 && gy.guy_num < t.squad_size && t.new_block_radius != 0) {
			const int32_t region = g.world.get_tregion(tcoord_of(gy.x), tcoord_of(gy.y));
			g.world.stamp_footprint(ucoord_of(gy.x), ucoord_of(gy.y), t.new_block_radius, region, true);
		}
		gy.guy_flags &= uint16_t(~0x20u);
	}
}

void guy_set_anim(Game& g, Unit& u, Guy& gy, int32_t anim, int32_t p2, int32_t p3) {
	const uint32_t end0 = gy.end_time;
	const int32_t gtype = gy.type;
	const bool settled_pos = gy.des_x == gy.x - gy.off_x && gy.des_y == gy.y - gy.off_y;

	// ---- requests that may return early (@5da32f..5da5ef)
	bool general = true;  // false: jump straight to the common part (L5ef)
	if (anim == kCharAttackWalk) {
		if (gy.cur_anim != kCharAttackWalk) {
			if (u.recharging > 5) anim = kCharWalk;  // still reloading: plain walk
			else u.recharging = 0;
		}
	} else if (anim == kCharDefault && p2 == 0) {
		general = false;
		const int32_t cc = cat(gy.cur_anim);
		if (cc == kCharDefault) {
			if (gy.cur_time < end0) return;
		} else if (cc == kCharWalk) {
			if (!settled_pos) {
				// Still walking: let the cycle finish, then restart it.
				if (gy.cur_time < end0) return;
				gy.cur_time = 0;
				return;
			}
		} else if ((gy.cur_anim == kCharTurnLeft || gy.cur_anim == kCharTurnRight) && gy.des_angle != gy.angle) {
			if (gy.cur_time < end0) return;
			gy.cur_time = 0;
			return;
		}
	}
	if (general) {
		if (gy.who >= 8 && anim == kCharWalk) {
			if (cat(gy.cur_anim) == kCharWalk && gy.cur_time < end0) return;
		} else {
			if (cat(anim) == kCharAttack2 && !(gy.guy_flags & 0x40)) {
				if (!settled_pos || gy.des_angle != gy.angle) {
					gy.hold_attack = p3 ? 1 : int8_t(anim);  // played when the guy arrives (Guy::move)
					return;
				}
				if (cat(gy.cur_anim) == kCharAttack2) {
					gy.queued_attack = p3 ? 1 : int8_t(anim);  // played when the current attack ends
					return;
				}
			}
			if ((anim == kCharTurnLeft || anim == kCharTurnRight) && !anim_has(g, gy, anim)) anim = kCharDefault;
		}
	}

	// ---- L5ef: categories
	int32_t curcat = cat(gy.cur_anim);
	const bool f80 = (gy.guy_flags & 0x80) != 0;  // types 52/53
	if (f80 && gy.cur_anim >= 25) curcat = kCharDefault;
	int32_t newcat = cat(anim);
	if (p2 == 0) {
		if (f80 && anim >= 25) newcat = kCharDefault;
		if (curcat == newcat && (curcat != kCharWalk || gtype == kTypeBird) && gy.cur_time < end0) return;
	}
	gy.last_time = -1;
	const bool packet = anim_packet(g, gy);  // local_14: the AnimationPacket of the guy's piece

	if (newcat == kCharDefault) {
		// ---- DEFAULT: idle variants (@5da711)
		const int32_t stagger = (g.frame + 0x2e + int32_t(gy.o)) % 16;
		const bool cur_group = gy.cur_anim >= kAnimGroupIdle1 && gy.cur_anim <= kAnimGroupIdle3;
		int32_t chosen = kCharDefault;
		bool validate = true;  // false: L5db0d5 (no packet) -> DEFAULT
		if (is_captain(u) && !cur_group && gy.guy_num == 0 && stagger == 0 && packet &&
		    anim_has(g, gy, kAnimGroupIdle2)) {
			// Every 16 frames the captain's guy tries the group idle instead of drawing a variant.
			chosen = try_group_idle(g, u, gy);
		} else {
			// @5dac68: game_random, skipped while the unit has a time-sliced search saved.
			const int32_t r = u.openlist ? 0 : g.rnd_draw(100);
			if (g_idle_pick_hook && !u.openlist) g_idle_pick_hook(g, u);
			if (curcat == kCharDefault && p3) {
				if (gy.guy_flags & 0x20) {
					if (r >= 70) chosen = kCharIdle1;
				} else if (r >= 70) {
					if (r < 83) {
						chosen = kCharIdle1;
					} else {
						bool rough = false;
						if (is_peasant(u) && world_valid(g, u.x, u.y))
							rough = (g.world.t(tcoord_of(u.x), tcoord_of(u.y)) & tmask::kLowField) != 0;
						chosen = rough ? kCharIdle1 : (r >= 96) + 2;
					}
				}
			}
			if (f80 && u.inside_up != -1) {
				// Types 52/53 inside a building: working anims 25.. (first occupant) or 29.. (others).
				int32_t cwho = -1;
				const int32_t co = get_inside(g, u, &cwho);
				chosen += 25;
				const Object* c = g.object(cwho, co);
				if (c && c->inside_down != gy.o) {
					chosen += 4;
					if (chosen == kAnimWalkToOre && c->inside_down != -1) {
						// Someone else in the chain already plays 32: use 29.
						// TODO: Object has no inside_down_who (+0x3e); the container's who is used for the chain.
						const Object* cur = c;
						while (cur && cur->inside_down != -1) {
							const Object* y = g.object(cur->who, cur->inside_down);
							if (!y) break;
							if (y->is_unit && (y->type == 0x34 || y->type == 0x35)) {
								const Unit& yu = static_cast<const Unit&>(*y);
								if (!yu.guys.empty() && yu.guys[0].cur_anim == kAnimWalkToOre) {
									chosen = kAnimMineOre;
									break;
								}
							}
							cur = y;
						}
					}
				}
			}
			// L5daea4
			if (!cur_group) {
				if (!packet) {
					validate = false;
				} else if (anim_has(g, gy, kAnimGroupIdle2)) {
					// Join a group idle another figure of this unit already plays (slot by figure index).
					int32_t k = -1;
					for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) {
						const Guy& o = u.guys[size_t(i)];
						if (&o != &gy && o.cur_anim >= kAnimGroupIdle1 && o.cur_anim <= kAnimGroupIdle3) {
							k = i;
							break;
						}
					}
					if (k >= 0) {
						const int32_t mark = u.guy_mark, squad = u.ptype->squad_size;
						int32_t idx = 0;
						while (idx < mark && &u.guys[size_t(idx)] != &gy) idx++;
						if (idx == squad) {
							idx = squad;
							while (size_t(idx) < u.guys.size() && &u.guys[size_t(idx)] != &gy) idx++;
						}
						if (idx > squad) idx -= squad;
						if (u.unit_masks & umask::kFacing) gy.cur_anim = int8_t(int8_t(squad) - int8_t(idx) + 3);
						else gy.cur_anim = int8_t(int8_t(idx) + 4);
						chosen = gy.cur_anim;
						gy.cur_time = u.guys[size_t(k)].cur_time;
					}
				}
			}
		}
		// L5daba0: the variant must exist and the unit must not be entrenched.
		int32_t v = chosen;
		if (!validate || !packet || !anim_has(g, gy, chosen) || (u.unit_masks & umask::kEntrenched)) v = 0;
		else if (chosen >= kAnimGroupIdle1 && chosen <= kAnimGroupIdle3) v = -1;  // already set above
		if (v >= 0) {
			if (gy.cur_anim == v) {
				consume_time(gy, end0);
			} else {
				gy.cur_anim = int8_t(v);
				gy.cur_time = 0;
			}
		}
	} else if (newcat == kCharAttack2) {
		// ---- ATTACK2: attack variants (@5db20e)
		int32_t v;
		if (p3) {
			const int32_t r = g.rnd_draw(100);  // @5db21d
			v = r < 30 ? kCharAttack1 : (r > 70 ? kAnimAttack3 : kCharAttack2);
		} else {
			v = anim;
		}
		if (!anim_has(g, gy, v)) v = kCharAttack2;  // (a null packet crashes the original here)
		if (gy.cur_anim == v) {
			consume_time(gy, end0);
		} else {
			gy.cur_anim = int8_t(v);
			gy.cur_time = 0;
		}
	} else if (newcat == kCharWalk) {
		// ---- WALK: slog / walk / jog by speed (@5db2b8)
		const Type& wt = g.type(gtype);
		const float num = float(gy.avg_speed);
		const float den = float(int32_t(uint32_t(wt.moves) * uint32_t(konst(g, kCUnitMoveSpeed))));
		const float ratio = num / den;  // divss
		int32_t v = kCharWalk;
		if (gy.who == kGaiaWho) {
			if (gtype == 0x192 || gtype == 0x194 || gtype == 0x193) {
				if (g.rnd_draw(100) >= 50) v = kCharJog;  // @5db339: gaia birds
			}
		} else if (0.6f > ratio) {
			v = kCharSlog;
		} else if (ratio > 1.1f) {
			v = kCharJog;
		}
		const uint32_t m = u.unit_masks;
		if (m & 0x78000000u) {
			if (m & 0x10000000u) v = kAnimWalkToWood;
			else if (m & 0x08000000u) v = kAnimWalkWithWood;
			else if (m & 0x40000000u) v = kAnimWalkToOre;
			else if (m & 0x20000000u) v = kAnimWalkWithOre;
		}
		if (!packet || !anim_has(g, gy, v)) v = kCharWalk;
		if (curcat == kCharWalk) {
			if (gy.cur_anim == v) {
				if (gy.cur_time >= end0) gy.cur_time -= end0;  // else keep running
			} else {
				// The original rescales by get_anim_time(cur_anim) / get_anim_time(cur_anim) (animmgr.times in ms,
				// 200 without an entry, 0 taken as 1): both calls read the old animation, so this is cur_time * T / T
				// in u32 (identity unless the product wraps).
				uint32_t T = 1;
				if (packet) {
					T = g.anims->time_ms(gy.gpiece, gy.cur_anim);
					if (T == 0) T = 1;
				}
				gy.cur_anim = int8_t(v);
				gy.cur_time = (gy.cur_time * T) / T;
			}
		} else {
			gy.cur_anim = int8_t(v);
			gy.cur_time = 0;
		}
	} else {
		// ---- every other category plays the category itself
		if (gy.cur_anim != newcat) {
			gy.cur_anim = int8_t(newcat);
			gy.cur_time = 0;
		} else {
			consume_time(gy, end0);
		}
	}
	// L5db507
	gy.end_time = anim_frames(g, gy, gy.cur_anim);
}

void guy_inc_time(Game& g, Unit& u, Guy& gy) {
	int32_t inc = 1;
	if ((gy.guy_flags & 4) && cat(gy.cur_anim) == kCharAttack2) inc = 2;
	if (u.unit_masks2 & umask2::kHoldAnim) inc = 0;
	if (gy.guy_num < u.ptype->squad_size || cat(gy.cur_anim) == kCharWalk) {
		gy.last_time = int32_t(gy.cur_time);
		gy.cur_time += uint32_t(inc);
		while (gy.end_time <= gy.cur_time) {
			// A piece that did not load is the original's fatal "No Animation Packet" (ignored: return). A null
			// g.anims uses the looping table of anim_loops and 3-frame animations instead.
			if (g.anims && !g.anims->piece_exists(gy.gpiece)) return;
			const int32_t a = gy.cur_anim;
			if (anim_loops(g, gy, a)) {
				guy_set_anim(g, u, gy, a, 0, 1);  // restart (DEFAULT re-picks an idle variant: game_random)
			} else if (cat(a) != kCharAttack2) {
				guy_set_anim(g, u, gy, a == kCharAttackWalk ? kCharWalk : kCharDefault, 0, 1);
			} else {
				guy_set_anim(g, u, gy, kCharDefault, 0, 0);
				const bool hero = (u.ptype->unit_flags2 & 0x20) != 0;  // UnitData::is_hero
				if (!hero && gy.queued_attack != 0) {
					const int32_t q = gy.queued_attack;
					gy.queued_attack = 0;
					if (q < 2) guy_set_anim(g, u, gy, kCharAttack2, 0, 1);
					else guy_set_anim(g, u, gy, q, 0, 0);
				}
			}
		}
		if (cat(gy.cur_anim) != kCharAttack2 && gy.queued_attack != 0 && gy.cur_anim != kCharWalk) {
			const int32_t q = gy.queued_attack;
			gy.queued_attack = 0;
			if (q < 2) guy_set_anim(g, u, gy, kCharAttack1, 0, 1);
			else guy_set_anim(g, u, gy, q, 0, 0);
		}
		if (gy.guy_num == 0 && cat(gy.cur_anim) == kCharAttack2) {
			// The crew snaps to its slots and plays guy 0's attack in sync.
			for (size_t i = size_t(u.ptype->squad_size); i < u.guys.size(); i++) {
				Guy& c = u.guys[i];
				guy_set_new_location(g, u, c, c.des_x, c.des_y, true);
				c.cur_anim = gy.cur_anim;
				c.cur_time = gy.cur_time;
			}
		}
	} else if (!u.guys.empty()) {
		// Crew figures that are not walking mirror guy 0's clock.
		gy.cur_anim = u.guys[0].cur_anim;
		gy.cur_time = u.guys[0].cur_time;
	}
	// GuyOut::graph_inc_frame (render, players < 8).
}

}  // namespace ron::sim
