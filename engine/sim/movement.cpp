// Per-frame unit movement (docs/movement-spec.md sections 3.5, 4, 5, 9.1, 10.5; docs/orders-spec.md section 6):
// Unit::do_move and the move-order handlers built on it, the straight-line check find_path with its building detours
// (go_around_building), move_step (one frame of motion) and the unit speeds.
//
// The unit collision layer (detect / resolve unit collision, CollCheck) is in collision.cpp.
#include <algorithm>

#include "sim/ai.hpp"
#include "sim/combat.hpp"
#include "sim/economy.hpp"
#include "sim/game.hpp"
#include "sim/groups.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/pathfinder.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

// Stack<PathData>::peek / pop. The original reads the stale bottom slot list[0] on an empty stack; no caller here
// does so in practice, and an empty stack yields a zero entry.
PathData peek(const Unit& u) { return u.path.empty() ? PathData{} : u.path.back(); }
PathData pop(Unit& u) {
	if (u.path.empty()) return PathData{};
	const PathData d = u.path.back();
	u.path.pop_back();
	return d;
}

// GameDaemon::repaths is indexed by the player byte (8 entries).
int32_t& repaths_of(Game& g, int32_t who) {
	static int32_t scratch = 0;  // TODO(frame-spec): what the original reads for who >= 8 (memory after repaths)
	if (who >= 0 && who < int32_t(g.daemon.repaths.size())) return g.daemon.repaths[size_t(who)];
	scratch = 0;
	return scratch;
}

// Whether `m` is still one of the unit's orders (kill_current_order destroys the head order).
bool order_alive(const Unit& u, const Order* m) {
	for (const Order& o : u.orders)
		if (&o == m) return true;
	return false;
}

// UnitData::get_order()->is_move (vtable +0x14), as UnitData::is_moving.
bool has_move_order(const Unit& u) { return !u.orders.empty() && order_is_move(u.orders.front().type); }

// TData medium == water (the find_path transport re-flagging test).
bool water_at(const Game& g, int32_t x, int32_t y) {
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	return g.world.in_t(tx, ty) && (g.world.t(tx, ty) & tmask::kMedium) == tmask::kWater;
}

// WorldData::get_tregion of a Coord's tcoord (-1 off the map, where the original reads out of bounds).
int32_t tregion_at(const Game& g, int32_t x, int32_t y) {
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	return g.world.in_t(tx, ty) ? g.world.get_tregion(tx, ty) : -1;
}

bool invalid(const Game& g, const Unit& u, int32_t tx, int32_t ty, bool wlevel = false, bool fog = false,
             bool p7 = false) {
	return unit_invalid_loc(g, u, tx, ty, wlevel, fog, false, false, p7) != 0;
}

// ObjectData::num_inside(0) != 0: TODO(transport): the original counts captains in the inside chain (decoys and
// governor heroes weighted); here any passenger counts.
bool has_passengers(const Unit& u) { return u.on_map() && u.inside_down >= 0; }

// GameAccess::ai_speed (a game-speed multiplier for AI testing). TODO(game options): always 1 here.
constexpr int32_t kAiSpeed = 1;

}  // namespace

// ---------------------------------------------------------------- speeds (section 9.1)

// Unit::update_speed @6055c0: myspeed of the captain, copied down the o_down chain.
void update_speed(Game& g, Unit& u0) {
	Unit* up = &u0;
	while (up->o_up >= 0) {  // UnitData::is_captain
		Unit* n = g.unit(up->who, up->o_up);
		if (!n) break;
		up = n;
	}
	Unit& u = *up;
	const Type& t = *u.ptype;
	const Constants& c = g.rules->constants;
	int32_t base = t.moves;
	if (t.domain == 1 && (t.unit_flags & 0x10)) {
		// TODO(techs): += (leader +0x6eb8 -> +0xe8 ^ 0x63187, the transport level) * military_transport_bonus (0x3c)
	}
	if (u.unit_masks2 & umask2::kMarineTransport) {
		// TODO(techs): += the same level * americans_marine_speed_bonus (0x86c)
	}
	int32_t s = c.at(0x4) * base;  // unit_move_speed
	// TODO(leader): objmask 0x2000 with the leader's whales bit: s = (whales_ships_move + 100) * s / 100
	if (unit_is(g, u, 0x68)) s = s * 36 / 32;
	else if (unit_is(g, u, 0x66)) s = s * 34 / 32;
	else if (unit_is(g, u, 100)) s = s * 32 / 27;
	else if (unit_is(g, u, 0x62)) s = s * 30 / 24;
	// TODO(tribe bonuses, wonders, techs), in this order: Bantu (tribe bonus 3; where 0x1ab, types 50/51 or is 0x42)
	// (bantu_units_move + 100) * s / 100; where 0x1ae/0x1af: French (bonus 10) french_siege_move, Versailles (wonder
	// 0x218) versailles_units_move; air with the aluminum bit aluminum_air_speed; is 0x3a: s += spy_upgrade * s / 4;
	// heroes: general_upgrade; supply: supply_upgrade; types 50/51 with tribe bonus 0: aztec_move_speed.
	u.myspeed = int16_t(s);
	for (int32_t d = u.o_down; d >= 0;) {
		Unit* f = g.unit(u.who, d);
		if (!f) break;
		f->myspeed = u.myspeed;
		d = f->o_down;
	}
}

// UnitData::speed @60aae0.
int32_t unit_speed(const Game& g, const Unit& u) {
	int32_t s = u.myspeed;
	if (u.ptype->domain != 0) return s;
	// Iroquois spearmen (is 0xa6) in allied territory.
	if (unit_is(g, u, 0xa6)) {
		const int32_t wx = wcoord_of(u.x), wy = wcoord_of(u.y);
		const int32_t terr = g.world.in_w(wx, wy) ? g.world.w(wx, wy).who : -1;
		if (terr >= 0 && is_ally(g, u.who, terr)) {
			const Constants& c = g.rules->constants;  // iroquois_spear_speed[4] @0x838
			if (unit_is(g, u, 0xa6, true)) s += c.at(0x838);
			else if (unit_is(g, u, 0xa7, true)) s += c.at(0x83c);
			else if (unit_is(g, u, 0xa8, true)) s += c.at(0x840);
			else if (unit_is(g, u, 0xa9, true)) s += c.at(0x844);
		}
	}
	if (g.leaders[size_t(u.who)].flags & 0x8000) {
		// TODO(heroes): forced march: a forced-march hero on the unit or HeroesData::find_hero within
		// (x_size + y_size) * 0x60 -> v = forced_march_speed * unit_move_speed (types 0x166/0x167 on either side:
		// forced_march_speed * alexander_forced_march_speed * unit_move_speed / 256); s = max(v, myspeed).
	}
	// TODO(CTW): Conquer-the-World hero multipliers (Spitamenes, Blucher, Porus, Charles, Napoleon on siege).
	return s;
}

// UnitData::get_speed @608720 (vtable +0x17c).
int32_t get_speed(const Game& g, const Unit& u, int32_t x, int32_t y, bool nocap) {
	int32_t s = unit_speed(g, u);
	const Order* act = get_action(u);
	const OrderType at = act ? act->type : kOrderNone;
	if (at == kAttack) s = s * 9 / 8;
	else if (at == kGuard) s = (u.unit_masks & umask::kAiControl) ? s * 10 / 8 : s * 9 / 8;
	if (u.ptype->domain == 0) {
		if (u.unit_masks & umask::kZoc) s /= 2;
		const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
		if (u.z < 1 && g.world.in_t(tx, ty) && (g.world.t(tx, ty) & tmask::kRiverB)) s /= 2;  // ford
		// TODO(heroes): ObjectData::has_general(0, 0x162) >= 0 and is_siege (unit_flags 0x20000) -> s *= 2
	}
	if (!nocap && u.group >= 0 && u.group < kNumGroups && at == kOrderNone) {
		const int32_t gs = g.groups.g[size_t(u.group)].speed;  // Group +0x40
		if (gs != 0 && gs < s) s = gs;
	}
	return s < 3 ? 3 : s;
}

// ---------------------------------------------------------------- go_around_building @5fc350

int32_t go_around_building(Game& g, Unit& u, int32_t X, int32_t Y, uint32_t angle, int32_t step) {
	Order* m = update_order(u);  // ->get_move_order()
	if (m && !order_is_move(m->type)) m = nullptr;
	const int32_t tx0 = tcoord_of(X), ty0 = tcoord_of(Y);
	const int32_t sx = sin_a(angle, step), dy = -cos_a(angle, step);
	int32_t tx1 = tcoord_of(X + sx);
	const int32_t ty1 = tcoord_of(Y + dy);
	auto inv = [&](int32_t tx, int32_t ty) { return invalid(g, u, tx, ty); };
	auto fail = [&]() -> int32_t {
		if (m) m->dest = 0;
		u.unit_masks &= ~umask::kPathClear;
		u.path_recursion = 10;
		return 0;
	};

	// Probe direction: along the blocked tile's column (0, +-1) or row (+-1, 0).
	bool along_y;
	if (tx0 != tx1 && ty0 != ty1) {
		// Diagonal step into the blocked tile.
		if (iabs(ty0 - ty1) + iabs(tx0 - tx1) > 2) return fail();
		if (!inv(tx0, ty1)) {
			along_y = true;
		} else {
			if (!inv(tx1, ty0)) tx1 = tx0;
			else if (step > 1) return go_around_building(g, u, X, Y, angle, step - 1);  // doubly blocked corner
			along_y = tx0 != tx1 && iabs(dy) > iabs(sx);
		}
	} else if (tx0 != tx1) {
		along_y = true;
	} else {
		along_y = ty0 == ty1;
	}
	const int32_t dirx = along_y ? 0 : (sx > 0 ? 1 : -1);
	const int32_t diry = along_y ? (dy > 0 ? 1 : -1) : 0;

	// Walk tile by tile from the blocked point until the original row/column is blocked too (exit found when that
	// tile is valid) or a valid tile is reached; off the map fails.
	const int32_t lim_x = g.world.tile_xs * 192, lim_y = g.world.tile_ys * 192;
	auto probe = [&](int32_t ddx, int32_t ddy, int32_t& cx, int32_t& cy) -> bool {
		cx = X + sx;
		cy = Y + dy;
		for (;;) {
			const int32_t nx = cx + ddx * 192, ny = cy + ddy * 192;
			if (nx < 0 || ny < 0 || nx >= lim_x || ny >= lim_y) return false;
			cx = nx;
			cy = ny;
			const bool line = ddx != 0 ? inv(tcoord_of(cx), ty0) : inv(tx0, tcoord_of(cy));
			if (line) return !inv(tcoord_of(cx), tcoord_of(cy));
			if (!inv(tcoord_of(cx), tcoord_of(cy))) return true;
		}
	};
	int32_t c1x = 0, c1y = 0, c2x = 0, c2y = 0;
	const bool f1 = probe(dirx, diry, c1x, c1y);
	const bool f2 = probe(-dirx, -diry, c2x, c2y);
	auto centre = [](int32_t c) { return tcoord_of(c) * 192 + 96; };
	bool pick2;
	if (!f1) {
		if (!f2) return fail();
		pick2 = true;
	} else if (!f2) {
		pick2 = false;
	} else if (u.x == centre(c1x) && u.y == centre(c1y)) {
		pick2 = true;
	} else if (u.x == centre(c2x) && u.y == centre(c2y)) {
		pick2 = false;
	} else {
		// The exit nearer the latched destination (Manhattan); ties keep the first.
		const int32_t mdx = m ? m->dest_x : 0, mdy = m ? m->dest_y : 0;
		const int32_t d1 = iabs(c1x - mdx) + iabs(c1y - mdy);
		const int32_t d2 = iabs(c2x - mdx) + iabs(c2y - mdy);
		pick2 = d2 < d1;
	}
	const int32_t ex = pick2 ? c2x : c1x, ey = pick2 ? c2y : c1y;
	const int32_t ddx = pick2 ? -dirx : dirx, ddy = pick2 ? -diry : diry;
	if (iabs(ex - X) > 0x300 || iabs(ey - Y) > 0x300) return fail();

	// Push the exit, a flag-8 midpoint and the corner (top): the corner is walked first. Points keep the order's
	// offset inside the tile.
	const int32_t offx = m ? m->off_x : 0, offy = m ? m->off_y : 0;
	const int32_t adjx = offx % 192 / 2 - 48, adjy = offy % 192 / 2 - 48;
	const int32_t etx = tcoord_of(ex), ety = tcoord_of(ey);
	int32_t ctx, cty;  // corner tile
	if (ddx != 0) {
		ctx = etx;
		cty = ty0;
		if (inv(etx, ty0)) {
			ctx = etx - ddx;
		}
	} else {
		ctx = tx0;
		cty = ety;
		if (inv(tx0, ety)) {
			cty = ety - ddy;
		}
	}
	const bool detour = ddx != 0 ? inv(etx, ty0) : inv(tx0, ety);
	if (detour) {
		const int32_t Ex = etx * 192 + 96, Ey = ety * 192 + 96;
		u.path.push_back(PathData{Ex + adjx, Ey + adjy, 0, 0});
		const int32_t Cx = ctx * 192 + 96, Cy = cty * 192 + 96;
		int32_t mx = (Cx + Ex) / 2, my = (Cy + Ey) / 2;
		if (Ex - Cx > 0) mx--;
		if (Ey - Cy > 0) my--;
		u.path.push_back(PathData{mx, my, 0, pathflag::kAround});
	}
	u.path.push_back(PathData{ctx * 192 + 96 + adjx, cty * 192 + 96 + adjy, 0, 0});
	return 1;
}

// ---------------------------------------------------------------- find_path @5fb910

int32_t find_path(Game& g, Unit& u, int32_t dx, int32_t dy) {
	if (u.ptype->unit_flags & 0x20) return 0;  // helicopters fly straight
	if (invalid(g, u, tcoord_of(u.x), tcoord_of(u.y), true)) {
		// Standing on an invalid tile: jump to the nearest free spot (UnitType::find_nearby_spot) and re-plan.
		int32_t sx = 0, sy = 0;
		find_nearby_spot(g, *u.ptype, u.x, u.y, &sx, &sy, 0, -1, 0, 0x55555555u, kFilterNotMe, u.o, u.who, 0, 0, -1, 0,
		                 -1);
		unit_set_new_location(g, u, sx, sy, true, false);
		return 1;
	}
	if (dx == u.x && dy == u.y) return 0;
	Order* m = update_order(u);  // ->get_move_order()
	if (m && !order_is_move(m->type)) m = nullptr;
	int32_t rdx = dx - u.x, rdy = dy - u.y;
	const int32_t wdist = iabs(wcoord_of(u.x) - wcoord_of(dx)) + iabs(wcoord_of(u.y) - wcoord_of(dy));
	if (wdist > 4 && !invalid(g, u, tcoord_of(dx), tcoord_of(dy), true, true)) return 1;  // too far: re-plan
	const uint32_t a0 = find_angle(rdx, rdy);
	int32_t s = get_speed(g, u, u.x, u.y, false);
	if (s <= 3) s = 3;
	int32_t sx = sin_a(a0, s), sy = cos_a(a0, s);
	if (u.path_recursion != 0xff) u.path_recursion++;

	// Pull the target out of an obstacle, toward the unit, moving the latched destination and a matching top entry
	// along with it.
	if (invalid(g, u, tcoord_of(dx), tcoord_of(dy))) {
		for (;;) {
			if (iabs(sx) > iabs(rdx)) sx = rdx;
			if (iabs(sy) > iabs(rdy)) sy = -rdy;
			if (m && dx == m->dest_x && dy == m->dest_y) {
				m->dest_x -= sx;
				m->dest_y += sy;
			}
			if (!u.path.empty()) {
				PathData& t = u.path.back();
				if (t.x == dx && t.y == dy) {
					t.x -= sx;
					t.y += sy;
				}
			}
			dx -= sx;
			dy += sy;
			rdx = dx - u.x;
			rdy = dy - u.y;
			if (sx == 0 && sy == 0) break;
			if (!invalid(g, u, tcoord_of(dx), tcoord_of(dy))) break;
		}
	}
	if (u.ptype->domain == 2) return 0;
	rdx = dx - u.x;
	rdy = dy - u.y;
	int32_t adx = iabs(rdx), ady = iabs(rdy);
	int32_t L = adx + ady;
	if (rdx == 0 && rdy == 0) {
		u.avoid_x = dx;
		u.avoid_y = dy;
		return 1;
	}
	if (L <= s) return 0;

	// March toward the target in steps of s, re-aiming each step, testing every new tile once.
	int32_t px = u.x, py = u.y;
	int32_t last_tx = -1, last_ty = -1;
	int32_t prev = L;
	for (;;) {
		if (prev < L) return 0;  // the Manhattan remainder grew
		prev = L;
		const uint32_t a = find_angle(rdx, rdy);
		int32_t stx = sin_a(a, s), sty = cos_a(a, s);
		if (iabs(stx) > adx) stx = rdx;
		if (iabs(sty) > ady) sty = -rdy;
		px += stx;
		py -= sty;
		const int32_t tx = tcoord_of(px), ty = tcoord_of(py);
		if (tx != last_tx || ty != last_ty) {
			last_tx = tx;
			last_ty = ty;
			if (invalid(g, u, tx, ty)) {
				// Blocked: detour around the building from the previous point, then check the detour's first leg.
				if (u.path_recursion >= 10) return 1;
				const int32_t old_len = int32_t(u.path.size());
				go_around_building(g, u, px - stx, py + sty, a, s);
				if (u.path_recursion == 10) return 1;
				// Take the new entries off the stack, dropping a leading run equal to the unit's position (never
				// the last one). (fx, fy): the first kept point; (lx, ly): the last one looked at.
				const PathData t0 = peek(u);
				int32_t fx = t0.x, fy = t0.y, lx = t0.x, ly = t0.y;
				std::vector<PathData> tmp;
				const int32_t new_len = int32_t(u.path.size());
				for (int32_t i = old_len; i < new_len; i++) {
					const PathData p = pop(u);
					lx = p.x;
					ly = p.y;
					if (fx == u.x && fy == u.y && i < new_len - 1) {
						const PathData q = peek(u);
						fx = lx = q.x;
						fy = ly = q.y;
					} else {
						tmp.push_back(p);
					}
				}
				const int32_t mlx = m ? m->last_x : 0, mly = m ? m->last_y : 0;
				if ((fx == u.x && fy == u.y) || (fx == mlx && fy == mly) || (lx == u.x && ly == u.y) ||
				    (lx == mlx && ly == mly) || (u.x == mlx && u.y == mly) || (dx == fx && dy == fy))
					return 1;
				const int32_t r = find_path(g, u, fx, fy);
				if (r == 2) return 2;
				if (r != 0) return 1;
				if (!(u.unit_masks & umask::kPathClear)) {
					// Put the detour back, re-flagging transport (4) where consecutive points change medium.
					while (!tmp.empty()) {
						const PathData p = tmp.back();
						tmp.pop_back();
						if (!u.path.empty()) {
							PathData q = pop(u);
							q.flags = water_at(g, p.x, p.y) == water_at(g, q.x, q.y) ? q.flags & ~pathflag::kTransport
							                                                         : q.flags | pathflag::kTransport;
							u.path.push_back(q);
						}
						u.path.push_back(p);
					}
					if (m) {
						m->last_x = u.x;
						m->last_y = u.y;
					}
					PathData q = pop(u);
					q.flags = water_at(g, u.x, u.y) == water_at(g, q.x, q.y) ? q.flags & ~pathflag::kTransport
					                                                         : q.flags | pathflag::kTransport;
					u.path.push_back(q);
					u.unit_masks |= umask::kPathClear;
				}
				return 0;
			}
		}
		rdx = dx - px;
		rdy = dy - py;
		adx = iabs(rdx);
		if (iabs(stx) >= adx && iabs(sty) >= iabs(rdy)) return 0;  // within one step on both axes
		ady = iabs(rdy);
		L = ady + adx;
		if (L <= s) return 0;
	}
}

// ---------------------------------------------------------------- move_step @5faf30

int32_t move_step(Game& g, Unit& u, Order& m, int32_t speed) {
	if (u.unit_masks & umask::kEntrenched) {
		u.unit_masks2 &= ~0x21000u;  // (GraphicEvents::remove_entrench: render only)
		u.unit_masks &= ~umask::kEntrenched;
	}
	int32_t walk = kCharWalk;
	if (u.cavarch_o >= 0 && (u.ptype->unit_flags & 0x200000)) {
		// TODO(combat): fires-while-moving: each squad guy gets the turret target (guy +0x8e/+0x9f) and
		// Guy::set_all_pivots; without a target node on guy 0 the target is dropped (cavarch_o = -1, guy pivots
		// reset), else the walk animation is CHAR_ATTACKWALK.
	}
	const int32_t x0 = u.x, y0 = u.y;
	const int32_t Dx = m.dest_x - x0, Dy = m.dest_y - y0;
	const PathData wp = peek(u);
	const uint32_t a = find_angle(Dx, Dy);
	unit_set_angle(g, u, a, false);

	// Heading of guy 0 after this frame's turn (hn) and the turn still left (rem).
	uint32_t h = u.guys[0].angle;
	const uint32_t d = a - h;
	uint32_t rem = d > 0x80000000u ? ~d : d;
	uint32_t hn;
	uint32_t ts = 0;
	if (rem < 0x2222220u || rem <= (ts = guy_turn_speed(g, u, u.guys[0], false))) {
		rem = 0;
		hn = a;
	} else if (d < 0x80000001u) {
		rem -= ts;
		hn = h + ts;
	} else {
		rem -= ts;
		hn = h - ts;
	}
	const int32_t ady = iabs(Dy), adx = iabs(Dx), L = adx + ady;

	if (!(u.ptype->unit_flags & 0x20) || rem > 0x40000000u) {
		// Ground and sea units (and helicopters far off their heading).
		const int32_t k = (uint32_t(u.ptype->turn_speed) < 0xe38e38cu) + 1;  // 1 fast turner, 2 slow (20 deg)
		if (L < k * 192 || (wp.flags & pathflag::kTransport)) {
			if (rem != 0) {
				guy_do_turn(g, u, u.guys[0], a, hn, 0, true);  // turn in place
				return 1;
			}
		} else {
			bool ok;
			if (u.ptype->domain == 0 && !(u.ptype->obj_masks & 0x200000)) ok = rem < 0x20000000u;
			else ok = L < k * 384 ? rem < 0x20000000u : rem < 0x38e38e3au;
			if (!ok) {
				guy_do_turn(g, u, u.guys[0], a, hn, 0, true);
				return 1;
			}
		}
		if (rem < 0x20000000u / uint32_t(k)) {
			if (u.unit_masks & umask::kSlowDown) {  // soft collision last frame
				speed /= 2;
				u.unit_masks &= ~umask::kSlowDown;
			}
		} else {
			speed /= 2;
		}
		guy_do_turn(g, u, u.guys[0], a, hn, 0, false);
	} else {
		// Helicopters: turn, then re-derive the heading from the guy's new angle.
		guy_do_turn(g, u, u.guys[0], a, hn, 0, false);
		Guy& g0 = u.guys[0];
		h = g0.angle;
		const uint32_t d2 = a - h;
		const uint32_t r2 = d2 > 0x80000000u ? ~d2 : d2;
		if (r2 < 0x2222220u) {
			hn = a;
		} else {
			const uint32_t ts1 = guy_turn_speed(g, u, g0, true);
			hn = ts1 < r2 ? (d2 < 0x80000001u ? h + ts1 : h - ts1) : a;
		}
	}

	if (speed < L) {
		// Partial step along the current heading hn.
		int32_t sx = sin_a(hn, speed), sy = cos_a(hn, speed);
		if (L < speed * 2) {
			if (adx < iabs(sx)) sx = Dx;
			if (ady < iabs(sy)) sy = -Dy;
		}
		int32_t nx = x0 + sx, ny = y0 - sy;
		if (nx < 0 || ny < 0 || nx >= g.world.tile_xs * 192 || ny >= g.world.tile_ys * 192) return 1;
		bool blocked = false;
		if (detect_unit_collision(g, u, nx, ny, false, true, 0, false, false) != 0) {
			blocked = true;
			if ((wp.flags & pathflag::kFine) && detect_unit_collision(g, u, wp.x, wp.y, true, true, 0, false, false) == 0 &&
			    adx < 0x61 && ady < 0x61) {
				nx = m.dest_x;  // step straight onto a free fine waypoint
				ny = m.dest_y;
				blocked = false;
			}
		}
		const bool tile_change = tcoord_of(x0) != tcoord_of(nx) || tcoord_of(y0) != tcoord_of(ny);
		if (blocked) {
			unit_set_anim(g, u, kCharDefault);
			if (rem != 0) return 0;
			const Unit* b = g.unit(u.collide_who, u.collide_o);
			const int32_t bbr = b ? b->ptype->big_radius : 0;
			if ((bbr + u.ptype->big_radius) * 3 <= L || u.collide < 0x1a ||
			    ((wp.tol == 0 || (wp.flags & pathflag::kFine)) && !(wp.flags & pathflag::kFinal))) {
				resolve_unit_collision(g, u, nx, ny);
				return 0;
			}
			u.tolerance = L * 2;  // stuck close to a toleranced point: counts as arrived
		} else {
			if (tile_change && invalid(g, u, tcoord_of(nx), tcoord_of(ny))) {
				u.unit_masks &= ~umask::kPathClear;  // forces a new straight-line check
				return 0;
			}
			unit_set_anim(g, u, walk);
			if (unit_set_new_location(g, u, nx, ny, false, false) == 0) return 1;
		}
		if (u.tolerance < iabs(m.dest_y - u.y) + iabs(m.dest_x - u.x)) return 1;
		m.dest = 0;
		const PathData p = pop(u);
		if (!(p.flags & pathflag::kFinal)) return 1;
		if (u.orders.size() == 1) unit_set_angle(g, u, m.angle, false);
		m.flags &= uint8_t(~orderflag::kPathed);
		if ((m.flags & orderflag::kDisembark) && (u.ptype->unit_flags & 0x20) && has_passengers(u)) {
			kill_current_order(g, u);
			// TODO(transport): Group::action_eject_all on a temporary one-unit group.
			return 1;
		}
	} else {
		// Last step: land exactly on the destination. A blocked last step still counts as reaching it.
		const int32_t nx = m.dest_x, ny = m.dest_y;
		const bool tile_change = tcoord_of(x0) != tcoord_of(nx) || tcoord_of(y0) != tcoord_of(ny);
		uint32_t pflags;
		if (detect_unit_collision(g, u, nx, ny, false, true, 0, false, false) == 0) {
			if (!tile_change || !invalid(g, u, tcoord_of(nx), tcoord_of(ny))) {
				int32_t anim = walk;
				if (Dx == 0 && Dy == 0 && u.orders.size() < 2) anim = kCharDefault;
				unit_set_anim(g, u, anim);
				if (unit_set_new_location(g, u, nx, ny, false, false) == 0) return 1;
			}
			m.dest = 0;
			pflags = pop(u).flags;
		} else {
			u.collide_o = -1;
			u.collide_who = -1;
			m.dest = 0;
			pflags = pop(u).flags;
			unit_set_anim(g, u, kCharDefault);
		}
		if (!(pflags & pathflag::kFinal)) return 1;
		const uint32_t ang = m.angle;
		m.flags &= uint8_t(~orderflag::kPathed);
		if ((m.flags & orderflag::kDisembark) && (u.ptype->unit_flags & 0x20) && has_passengers(u)) {
			kill_current_order(g, u);
			// TODO(transport): Group::action_eject_all on a temporary one-unit group.
			return 1;
		}
		const Order* act = get_action(u);
		if (u.orders.size() == 1 || (act && act->type == kGather)) unit_set_angle(g, u, ang, false);
	}
	kill_current_order(g, u);
	return 1;
}

// ---------------------------------------------------------------- do_move @5f7b30

int32_t do_move(Game& g, Unit& u, Order& m) {
	const Type& t = *u.ptype;
	// 1. Fires-while-moving units.
	if ((t.unit_flags & 0x200000) && obj_attack(g, u) != 0) {
		// combat-spec 3.10 / 4.4: no moving fire while walking to an attack (unless MELEE_AND_RANGED) or on HOLD_FIRE;
		// after a failed search (cavarch_o < -1) only every 32 frames.
		const Order* ca = get_action(u);
		if (get_combat_stance(g, u) == kHoldFire || (ca && ca->type == kAttack && !(t.unit_flags & 0x400))) {
			u.cavarch_o = -1;
		} else if (u.cavarch_o >= -1 || (u.o + g.frame) % 32 == 0) {
			cavarch_fight(g, u);
			if (!u.alive() || u.front() != &m) return 0;
		}
	}

	// 2. Resume a time-sliced U-level search.
	if (u.openlist) {
		const int32_t dt = g.frame - u.collide_frame;
		const Order* act = get_action(u);
		if (act) {
			if (act->type == kAttack) {
				if (dt > 0 && dt % 4 == 0 && repaths_of(g, u.who) != 0 && act->mandatory == 0) {
					find_new_target(g, u, nullptr, true);
					u.collide_frame = g.frame;
					return 0;
				}
			} else if (act->type == kGather && dt > 0 && (dt + 2) % 8 == 0 && vector_dist(m.x - u.x, m.y - u.y) < 0x120) {
				u.avoid_x = m.x;
				u.avoid_y = m.y;
				// The GatherOrder gives up its tile (economy-spec 4.3): tx = ty = -1, goto_build = 1, wait = -1, and a
				// worker drops its held doober.
				if (Order* ga = update_action(g, u)) {
					ga->g_tx = -1;
					ga->g_ty = -1;
					ga->goto_build = 1;
					ga->wait = -1;
				}
				if (u.type >= 50 && u.type <= 53 && u.doober >= 0) remove_hold_doober(g, u);
				kill_current_order(g, u);
				return 0;
			}
		}
		if (dt > 3 && (dt & 1) &&
		    detect_unit_collision(g, u, m.coll_x, m.coll_y, true, true, 0, false, true) == 0) {
			// The blocker left: drop the search and plan again.
			m.dest = 0;
			clear_partial_path(u);
			m.flags |= orderflag::kPathed;
			u.collide = 0;
			return 0;
		}
		if ((u.o + g.frame) % 4 == 0) repaths_of(g, u.who)++;
		u.collide++;
		m.dest = 0;
		find_upath_restore(g, u.path, u.who, u.o);  // may kill the order: m is not touched below
		if (u.openlist) return 0;
		if (g.frame - 5 <= u.collide_frame) return 0;
		u.collide = 0;
		return 0;
	}

	// 3. Order lifetime.
	if (m.timer > 0) {
		if (m.timer == 1) {
			kill_current_order(g, u);
			work(g, u);
			return 0;
		}
		m.timer--;
	}

	// 4. Actions riding on the move.
	const Order* act0 = get_action(u);
	const OrderType act_t = act0 ? act0->type : kOrderNone;
	if (act_t == kAttack) {
		Order* a = update_action(g, u);
		bool valid = false;
		if (a && a->ox >= 0 && a->whom >= 0) {
			const Object* tg = g.object(a->whom, a->ox);
			valid = tg && tg->alive() && int16_t(tg->uid) == int16_t(a->uid);
		}
		if (valid) {
			// @5f7d1c: a ranged unit whose target is in range (from here) stops; a ranged captain on a non-mandatory
			// attack switches to a better unit target in range (combat-spec 3.10 change_target).
			if (raw_max_range(g, t) != 0) {
				const int32_t ox = a->ox, whom = a->whom, mand = a->mandatory;
				Object& T = *g.object(whom, ox);
				if (T.is_unit) {
					const uint32_t d = static_cast<Unit&>(T).angle - u.angle + 0x80000000u;
					const uint32_t fl = d > 0xd5555555u ? 0 : (d >= 0x60000000u && d <= 0xa0000000u) ? 1 : 2;
					const bool frontal = d < 0x2aaaaaaau || fl == 0 || !unit_is_moving(static_cast<Unit&>(T));
					if (frontal && is_in_range(g, u, ox, whom, mand == 0 ? 1 : 0) &&
					    !find_collision(g, u.x, u.y, u.o, u.who, 1)) {
						kill_current_order(g, u);
						return 0;
					}
					if (mand == 0 && u.o_up < 0) {
						int32_t tt = -1, tw = -1;
						if ((u.o + g.frame) % 16 == 0) {
							const int32_t filt = (T.is_unit && !(t.unit_flags & 0x20000)) ? 1 : 2;
							tt = find_melee_target(g, u, -1, &tw, false, false, filt);
						}
						if (u.near_o >= 0 && u.near_who >= 0) {
							int32_t co = u.near_o, cw = u.near_who;
							if (tt >= 0 && is_in_range(g, u, tt, tw, 0)) {
								co = tt;
								cw = tw;
							}
							Object* C = g.object(cw, co);
							bool civ = false;
							if (C && C->is_unit) {
								const Type& ct = *C->ptype;
								if (u.unit_masks2 & umask2::kAttackCivies) civ = (ct.role & 0x10000) && !(ct.unit_flags & 0x2000);
								else civ = !(ct.role & 0x10000);
							}
							if (C && C->alive() && is_in_range(g, u, co, cw, 0) && !poor_target(g, u, co, cw) && C->is_unit &&
							    !civ) {
								Order* a2 = update_action(g, u);
								if (a2) a2->in_range = 1;
								unit_change_target(g, u, ox, whom, co, cw);
								return 0;
							}
						}
					}
				} else if ((!(u.unit_masks & umask::kAiControl) || !(t.unit_flags & 0x20000)) &&
				           is_in_range(g, u, ox, whom, 0) && valid_target(g, u, ox, whom, nullptr) &&
				           !find_collision(g, u.x, u.y, u.o, u.who, 1)) {
					kill_current_order(g, u);
					return 0;
				}
			}
		} else if (vector_dist(u.x - m.x, u.y - m.y) <= 0x480 && !(t.unit_flags & 0x10)) {
			repath(g, u);
			return 0;
		}
	} else if (act_t == kTradeRoute) {
		// TODO(trade): no valid trade partner (TradeOrder +0x20 ? (+0x14, +0x18) : (+8, +0xc) negative) ->
		// kill_current_order, return 1.
	}

	// 5. Waiting after a failed search.
	if (m.retry != 0) {
		if (--m.retry == 0) m.attempts += 3;
		return 0;
	}

	// 6. Modern infantry without a general snap to guy 0 and pack.
	if (((u.o * 17 + g.frame) & 0x7f) == 0 && !(u.unit_masks & umask::kInCombat) && unit_is_modern_infantry(g, u) &&
	    !u.guys.empty()) {
		// TODO(heroes): only when ObjectData::has_general(0x8000, -1) < 0 (no general nearby).
		const uint32_t ga = u.guys[0].angle;
		const int32_t gx = u.guys[0].x, gy = u.guys[0].y;
		unit_set_angle(g, u, ga, true);
		unit_set_new_location(g, u, gx, gy, true, true);
		unit_set_anim(g, u, kCharPack);
		m.retry = int32_t(u.guys[0].end_time);
		m.attempts = -3;
		return 0;
	}

	// 7.
	if (m.attempts != 0) m.attempts--;

	// 8. Plan: W level, then T level, from the unit to the order target.
	if (!(m.flags & orderflag::kPathed) || u.path.empty()) {
		const PathData fin{m.x, m.y, 0, pathflag::kFinal};
		u.path.push_back(fin);
		auto top_is_fin = [&] {
			if (u.path.empty()) return false;
			const PathData& p = u.path.back();
			return p.x == m.x && p.y == m.y && p.flags == pathflag::kFinal && p.tol == 0;
		};
		int32_t r = find_wpath(g, u.path, u.x, u.y, u.who, u.o);
		if (r < 1) {
			if (!top_is_fin()) u.path.push_back(fin);
			r = find_tpath(g, u.path, u.x, u.y, u.who, u.o);
			if (r < 1 && !top_is_fin()) u.path.push_back(fin);
		}
		m.flags |= orderflag::kPathed;
		if (u.path.size() > 10) return 1;  // the unit does not move this frame
	}

	// 9. Latch the next sub-destination.
	if (m.dest == 0) {
		PathData top = peek(u);
		m.dest = 1;
		u.unit_masks &= ~umask::kPathClear;
		m.dest_x = top.x;
		m.dest_y = top.y;
		u.tolerance = top.tol;
		uint32_t flags = top.flags;
		if (act_t == kTradeRoute && (flags & pathflag::kRoad)) {
			// TODO(trade): caravan road bookkeeping (TerrainOut::caravan_step off water; on a broken road
			// Caravan::verify_road and clear 0x20 on every path entry).
		}
		if ((flags & pathflag::kFinal) || act_t == kTradeRoute) {
			// A target in another region: walk it with tolerance 0 and flag 4 (u.tolerance keeps the old value).
			const int32_t r_unit = tregion_at(g, u.x, u.y);
			const int32_t r_top = tregion_at(g, top.x, top.y);
			if (r_top != r_unit) {
				PathData p = pop(u);
				p.tol = 0;
				p.flags |= pathflag::kTransport;
				flags = p.flags;
				u.path.push_back(p);
			}
		}
		if (detect_unit_collision(g, u, top.x, top.y, false, false, 0, false, false) != 0) {
			// Someone stands on the point.
			if (flags & pathflag::kFinal) {
				const OrderType at = action_type(u);
				if (at == kTradeRoute || at == kGather || at == kAttack || at == kBuildAt) {
					kill_current_order(g, u);
					return 0;
				}
			}
			const Unit* occ = g.unit(u.collide_who, u.collide_o);
			if (occ && !has_move_order(*occ)) {
				const int32_t t3 = occ->ptype->big_radius * 3;
				if (u.tolerance < t3) {
					u.tolerance = t3;
					PathData p = pop(u);
					p.tol = t3;
					flags = p.flags;
					u.path.push_back(p);
				}
			}
		}
		if (vector_dist(u.x - m.dest_x, u.y - m.dest_y) <= u.tolerance) {
			// Arrived before moving.
			m.dest = 0;
			const PathData p = pop(u);
			if (!(p.flags & pathflag::kFinal)) return 1;
			if (u.orders.size() == 1) unit_set_angle(g, u, m.angle, false);
			m.flags &= uint8_t(~orderflag::kPathed);
			kill_current_order(g, u);
			return 0;
		}
		if ((flags & pathflag::kBuilding) && resolve_block(g, u) != 0) return 1;
	}

	// 10. Speed.
	int32_t s = get_speed(g, u, u.x, u.y, false);
	if (kAiSpeed > 1) s *= kAiSpeed;
	if (unit_is_modern_infantry(g, u)) s = s * 5 / 4;

	if (!(u.unit_masks & umask::kPathClear)) {
		// 11. Verify the straight line to the latched point.
		u.path_recursion = 0;
		int32_t r = find_path(g, u, m.dest_x, m.dest_y);
		if (r == 0) {
			const PathData top = peek(u);
			if (top.x == u.x && top.y == u.y) {
				u.unit_masks &= ~umask::kPathClear;
			} else {
				u.unit_masks |= umask::kPathClear;
				m.dest = 1;
				m.dest_x = top.x;
				m.dest_y = top.y;
				u.tolerance = top.tol;
			}
		} else if (r == 2) {
			return 1;
		}
		if (u.unit_masks & umask::kPathClear) {
			u.avoid_x = u.avoid_y = -1;
			return move_step(g, u, m, s);
		}
		// Blocked: local re-plan.
		if (invalid(g, u, tcoord_of(m.x), tcoord_of(m.y), true, false, true) && u.orders.size() > 1) {
			kill_current_order(g, u);
			return 1;
		}
		const int32_t R = g.rnd_draw(5);  // @5f899d: one game_random draw per blocked frame
		const int32_t far = R == 2 ? 0x600 : R == 0 ? 0x1800 : 0xf00;
		bool used_w;
		if (iabs(m.dest_y - u.y) + iabs(m.dest_x - u.x) > far) {
			const size_t len = u.path.size();
			r = find_wpath(g, u.path, u.who, u.o);
			if (r > 0 && len == u.path.size()) r = 0;
			used_w = true;
		} else {
			const PathData top = peek(u);
			if (!(top.flags & pathflag::kFinal) && top.x == m.last_x && top.y == m.last_y) pop(u);
			size_t len = u.path.size();
			if (u.collide == 0) {
				if (len != 0) {
					// Drop stale T-level waypoints.
					while (!u.path.empty()) {
						const PathData& p = u.path.back();
						if ((p.flags & 0x21) || uint32_t(p.tol - 1) > 0x5fu) break;
						u.path.pop_back();
					}
					len = u.path.size();
				}
				r = find_tpath(g, u.path, u.who, u.o);
			} else {
				r = find_upath(g, u.path, u.who, u.o, false);
			}
			if (r > 0 && len == u.path.size()) r = 0;
			used_w = false;
		}
		// find_upath kills the order when its search fails; the original then keeps writing through the freed
		// MoveOrder. Those writes are dropped here.
		const bool alive = order_alive(u, &m);
		if (r == 0 && !u.path.empty()) {
			const PathData top = peek(u);
			if (!(top.flags & pathflag::kFinal)) {
				pop(u);
				if ((top.flags & pathflag::kTransport) && !u.path.empty()) {
					PathData p = pop(u);
					p.flags |= pathflag::kTransport;
					u.path.push_back(p);
				}
				if (alive) m.dest = 0;
				return 1;
			}
			pop(u);
			if (alive) m.flags &= uint8_t(~orderflag::kPathed);
			r = -1;  // shares the kill below
		}
		if (r == -1) {
			// No path, or a time-sliced U search: the order is killed (and its saved search freed).
			kill_current_order(g, u);
			const OrderType nt = u.order_type();
			if (nt == kAttack || nt == kBuildAt) kill_current_order(g, u);
			return 1;
		}
		// Retarget (r > 0, or r == 0 with an empty path).
		if (!alive) return 1;  // deviation: the original continues on the freed order (see above)
		m.last_x = m.last_y = -1;
		m.dest = 1;
		const PathData top = peek(u);
		if ((top.flags & pathflag::kBuilding) && resolve_block(g, u) != 0) return 1;
		u.path_recursion = 0;
		r = find_path(g, u, top.x, top.y);
		if (r == 0) u.unit_masks |= umask::kPathClear;
		else if (r == 2) return 1;
		if (!(u.unit_masks & umask::kPathClear)) {
			if (!used_w) resolve_block(g, u);
			return 1;
		}
		const PathData nt = peek(u);
		m.dest_x = nt.x;
		m.dest_y = nt.y;
		u.tolerance = nt.tol;
	} else if (m.pause != 0) {
		// 12. Straight line already verified: pause after a collision repath.
		m.pause--;
		if (m.type != kAttackTo && m.type != kGroupAttackTo) return 1;
		if (t.attack != 0) return 1;
		unit_set_anim(g, u, kCharDefault);
		return 1;
	}
	if (!(u.unit_masks & umask::kPathClear)) return 1;
	// 13.
	u.avoid_x = u.avoid_y = -1;
	return move_step(g, u, m, s);
}

// ---------------------------------------------------------------- the move-type handlers (orders-spec section 6)

// Unit::do_attack_to @5f2320.
int32_t do_attack_to(Game& g, Unit& u, Order& m) {
	const Order* mp = &m;
	const int32_t r = do_move(g, u, m);
	// Only while the same order is still current (pointer identity, as the original; m is not dereferenced).
	if (u.front() == mp && (u.o + g.frame) % 15 == 0) {
		if (u.ptype->attack != 0 && !(u.ptype->unit_flags2 & 0x40)) {
			// TODO(AI armies): unless an AI army keeps the unit in formation (army +0x2c and > 6 W cells away).
			find_melee_target(g, u, -1, nullptr, false, true, 0);
		} else {
			do_attack_to_pause(g, u, m);
		}
	}
	return r;
}

// Unit::do_explore_to @5f24a0.
int32_t do_explore_to(Game& g, Unit& u, Order& m) {
	const Order* mp = &m;
	const int32_t r = do_move(g, u, m);
	if ((u.o + g.frame) % 15 == 0 && u.front() == mp && u.o_up < 0) {
		ai_find_goody_box(g, u);  // Unit::find_goody_box @5f2540
	}
	return r;
}

// Unit::do_flee_to @5f2480.
int32_t do_flee_to(Game& g, Unit& u, Order& m) { return do_move(g, u, m); }

}  // namespace ron::sim
