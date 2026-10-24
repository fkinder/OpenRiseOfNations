// PACK / UNPACK (docs/spells-spec.md section 5): SpellType::cast_pack / cast_unpack, the merchant spots
// (UnitData::good_merchant_spot, Unit::find_merchant_spot, Unit::unpack_merchant) and the UnitData predicates that read
// the current CAST_SPELL order (is_packing, is_unpacking, is_busy, is_casting, mana_left).
#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/compass.hpp"
#include "sim/economy.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/guys.hpp"
#include "sim/heroes.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

// ---------------------------------------------------------------- UnitData predicates

namespace {
// The CastOrder's spell when the current (front) order is a CAST_SPELL, else -1 (order vslot +0xf4).
int32_t front_cast_spell(const Unit& u) {
	const Order* f = u.front();
	return f && f->type == kCastSpell ? f->spell : -1;
}
}  // namespace

bool cast_is_packing(const Unit& u) {  // @60aa60
	const int32_t s = front_cast_spell(u);
	return s >= 0 && spell_is_pack(s);
}
bool cast_is_unpacking(const Unit& u) {  // @60a4b0
	const int32_t s = front_cast_spell(u);
	return s >= 0 && spell_is_unpack(s);
}
bool cast_is_packing_or_unpacking(const Unit& u) {  // @60a410
	const int32_t s = front_cast_spell(u);
	return s >= 0 && (spell_is_pack(s) || spell_is_unpack(s));
}
bool cast_is_entering_or_exiting(const Unit& u) {  // @60a6f0: the front order's SpecialAnimOrder is type 0 / 1
	const Order* f = u.front();
	return f && f->type == kSpecialAnim && (f->sa_type == 0 || f->sa_type == 1);
}
bool cast_is_busy(const Unit& u) { return cast_is_packing_or_unpacking(u) || cast_is_entering_or_exiting(u); }  // @60a370

int32_t unit_packer_stance(const Unit& u) {  // @610970
	if (unit_stance_type(*u.ptype) != kStancePacker) return packer::kNever;
	return u.stance;
}

bool unit_is_casting_spell(const Unit& u, int32_t sp) {  // UnitData::is_casting @60a530
	if (sp == 0x27c) return (u.unit_masks & umask::kMarchRunning) != 0;
	if (sp == 0x27b) return (u.unit_masks & umask::kCloakRunning) != 0;
	return false;
}

int32_t unit_mana_available(const Game& g, const Unit& u) {
	// The mana still available: what is left plus the mana of the cast the unit is running (its action order).
	int32_t running = 0;
	const Order* a = get_action(u);
	if (a && a->type == kCastSpell && a->spell >= 0 && a->spell < kNumTypes) running = g.type(a->spell).mana;
	const int32_t m = unit_mana(g, u);
	int32_t left = std::max(0, m - u.mana_burn);
	left += running;
	return left < m ? left : m;
}

// ---------------------------------------------------------------- merchant spots (5)

namespace {

bool merchant_type(int32_t t) { return t == 0x3d || t == 0x3e || t == 400; }

// calc_gather(.., p7 = 1, p8 = 1, x, y) != 0 without its writes into the unit (the original only fills its out
// arguments; the port's unit_collect also stores rare / good_obj / the crowded bit).
bool collect_ok(Game& g, Unit& u, int32_t x, int32_t y) {
	const int32_t rare = u.rare;
	const int16_t good_obj = u.good_obj;
	const uint32_t masks = u.unit_masks;
	int32_t tmp[6];
	const bool ok = unit_collect(g, u, tmp, nullptr, nullptr, true, x, y, 1) != 0;
	u.rare = rare;
	u.good_obj = good_obj;
	u.unit_masks = masks;
	return ok;
}

bool tile_ok_for_merchant(const Game& g, int32_t tx, int32_t ty) {
	if (!g.world.in_t(tx, ty)) return false;
	const uint16_t m = g.world.t(tx, ty);
	return !(m & 0x4000) && (m & 3) != 3 && !(m & 0x80);
}

}  // namespace

bool good_merchant_spot(Game& g, Unit& u, int32_t tx, int32_t ty) {
	// @6068a0: the four tiles (tx, ty) (tx-1, ty) (tx, ty-1) (tx-1, ty-1) are free of the 0x4000 / 3 / 0x80 bits and a rare
	// is collectable from the corner.
	if (!tile_ok_for_merchant(g, tx, ty) || !tile_ok_for_merchant(g, tx - 1, ty) || !tile_ok_for_merchant(g, tx, ty - 1) ||
	    !tile_ok_for_merchant(g, tx - 1, ty - 1))
		return false;
	return collect_ok(g, u, tx * 0xc0, ty * 0xc0);
}

int32_t find_merchant_spot(Game& g, Unit& u, int32_t x, int32_t y, int32_t r, int32_t* otx, int32_t* oty) {
	// @603ab0
	const int32_t tx0 = tcoord_of(x), ty0 = tcoord_of(y);
	if (!collect_ok(g, u, x, y)) return 0;
	const int32_t n = r >= 0 && r <= 10 ? kMoveRadius[r] : 0;
	for (int32_t i = 0; i < n; i++) {
		const int32_t tx = tx0 + move_x[i], ty = ty0 + move_y[i];
		if (!g.world.in_t(tx, ty)) continue;
		if (!good_merchant_spot(g, u, tx, ty)) continue;
		if (unit_invalid_loc(g, u, tx, ty, false, false, false, false, false)) continue;
		if (detect_unit_collision(g, u, tx * 0xc0, ty * 0xc0, true, true, 0, false, false)) continue;
		if (otx) *otx = tx;
		if (oty) *oty = ty;
		return 1;
	}
	return 0;
}

int32_t unpack_merchant_spot(Game& g, Unit& u, int32_t r) {
	// @6038e0: find the spot, queue the UNPACK cast, then a MOVE_TO the spot's corner in front of it (the cast runs on
	// arrival).
	int32_t tx = 0, ty = 0;
	if (!find_merchant_spot(g, u, u.x, u.y, r, &tx, &ty)) return 0;
	u.unit_masks &= ~umask::kAutoExplore;
	add_cast_order(g, u, -1, -1, -1, -1, 0x28c, kQueueNew, false);
	const int32_t mx = tx * 0xc0 + 0x18, my = ty * 0xc0 + 0x18;
	const uint32_t a = find_angle(mx - u.x, my - u.y);
	add_move_facing_order(g, u, tx * 4, ty * 4, a, 1, false, kQueueFirst, false, -1, -1, -1, false);
	clear_partial_path(u);
	update_action(g, u);
	return 1;
}

// ---------------------------------------------------------------- the casts (5)

void cast_pack(Game& g, int32_t o, int32_t who) {
	// @670be0
	Unit* u = g.unit(who, o);
	if (!u || !u->alive() || !u->on_map()) return;
	if (merchant_type(u->type)) {  // merchants and fur trappers (not fishermen): the 2x2 market block is freed
		const int32_t tx = tcoord_of(u->x), ty = tcoord_of(u->y);
		const int32_t cx[4] = {tx, tx - 1, tx, tx - 1}, cy[4] = {ty, ty, ty - 1, ty - 1};
		for (int k = 0; k < 4; k++)
			if (g.world.in_t(cx[k], cy[k])) g.world.set_blocked_at(cx[k], cy[k], false);
		g.leaders[size_t(who)].flags |= leaderflag::kRecompute;
	}
	u->unit_masks |= umask::kPacked;
	// (Unit +0x14c = -1: ❓ spells-spec 11 (1), no reader found)
	unit_update_los(g, *u);
	object_update_seen(g, *u, false);
	unit_update_gpiece(g, *u);
	unit_set_new_location(g, *u, u->x, u->y, true, true);
}

void cast_unpack(Game& g, int32_t o, int32_t who) {
	// @6709c0
	Unit* u = g.unit(who, o);
	if (!u || !u->alive() || !u->on_map()) return;
	if (merchant_type(u->type)) {
		const int32_t tx = tcoord_of(u->x), ty = tcoord_of(u->y);
		if (!good_merchant_spot(g, *u, tx, ty)) return;  // stays packed; the order dies in do_cast
		unit_set_new_location(g, *u, tx * 0xc0, ty * 0xc0, true, true);
		const int32_t cx[4] = {tx, tx - 1, tx, tx - 1}, cy[4] = {ty, ty, ty - 1, ty - 1};
		for (int k = 0; k < 4; k++)
			if (g.world.in_t(cx[k], cy[k])) g.world.set_blocked_at(cx[k], cy[k], true);
		g.leaders[size_t(who)].flags |= leaderflag::kRecompute;
	}
	u->unit_masks &= ~umask::kPacked;
	unit_update_los(g, *u);
	object_update_seen(g, *u, false);
	unit_update_gpiece(g, *u);
	unit_set_new_location(g, *u, u->x, u->y, true, true);
}

}  // namespace ron::sim
