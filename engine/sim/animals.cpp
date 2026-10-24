// Gaia animals (Animal::process / work / do_idle) and the herd wander target (Herds::process).
#include "sim/animals.hpp"

#include <algorithm>

#include "sim/compass.hpp"
#include "sim/economy.hpp"
#include "sim/objects.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/search.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

bool build_covers_tile(const Build& b, int32_t tx, int32_t ty);  // construct.cpp

namespace {

// Animal::think_farm_animal @5d7700: a pen animal picks a new spot inside its pen every 128 / (aid + 1) frames (its pen cell
// around the farm's first gatherer, else around the farm itself).
void think_farm_animal(Game& g, Unit& u) {
	if (u.o * (int32_t(u.animal_aid) + 1) + g.frame < 0 || (u.o * (int32_t(u.animal_aid) + 1) + g.frame) % 128 != 0) return;
	Build* farm = u.animal_whom >= 0 && u.animal_ox >= 0 ? g.build(u.animal_whom, u.animal_ox) : nullptr;
	if (!farm) return;
	const int32_t ref_o = build_num_gatherers(g, *farm, true, false) == 0 ? int32_t(u.animal_ox) : int32_t(farm->gather_down);
	const Object* ref = g.object(u.animal_whom, ref_o);
	if (!ref) return;
	const int32_t tx = tcoord_of(ref->x), ty = tcoord_of(ref->y);
	if (!build_covers_tile(*farm, tx, ty)) return;
	static const int8_t kCornerX[5] = {0, -1, 1, 1, -1}, kCornerY[5] = {0, -1, -1, 1, 1};  // corner_x / corner_y @adc3e0 / @adc3c0
	const int32_t k = std::clamp<int32_t>(u.animal_aid, 0, 4);
	const int32_t r = g.rnd_draw(8) + 1;
	const int32_t Y = ((move_y[r] + ty) * 3 + kCornerY[k]) * 64 + 0x60;
	const int32_t X = ((move_x[r] + tx) * 3 + kCornerX[k]) * 64 + 0x60;
	const uint32_t angle = find_angle(X - u.x, Y - u.y);
	clear_orders(g, u);  // masks &= ~UNIT_MULTIMOVE, the path, close_orders, clear_partial_path, update_action
	add_move_facing_order(g, u, floor_div3(X >> 4), floor_div3(Y >> 4), angle, 1, false, kQueueLast, false, -1, -1, -1, false);
}

}  // namespace

void animal_do_idle(Game& g, Unit& u) {
	unit_set_anim(g, u, kCharDefault, 0, 1);
	u.collide = 0;
	if (u.ptype->domain != 0) return;  // fish and whales do nothing
	if (u.cara < 0) {
		think_farm_animal(g, u);
		return;
	}
	if (u.guys.empty()) return;
	const Guy& gy = u.guys[0];
	// (the original also tests `cur_anim > 0 || cur_anim < 4` on this byte: always true)
	if (int32_t(gy.cur_time) != int32_t(gy.end_time) - 1) return;
	if (g.rnd_draw(10) >= 3) return;
	if (size_t(u.cara) >= g.herds.size()) return;
	const Game::Herd& h = g.herds[size_t(u.cara)];
	// The point a third of the way from the herd cell to its target, in the middle of the wcoord.
	const int32_t cx = ((h.wx + h.cx * 2) * 0x300 + 0x480) / 3;
	const int32_t cy = ((h.wy + h.cy * 2) * 0x300 + 0x480) / 3;
	if (vector_dist(std::abs(u.x - cx), std::abs(u.y - cy)) > 0x180) {
		// too far from the herd: walk back to a free spot near it
		int32_t sx = cx, sy = cy;
		if (find_nearby_spot(g, *u.ptype, cx, cy, &sx, &sy, 0xc0, -1, 0, 0x55555555, kFilterNotMe, u.o, u.who, 0, 0, -1, 0, -1) == 0)
			add_move_order(g, u, sx, sy, 1, false, kQueueNew, false, -1, -1);
		return;
	}
	// a short random step: 1..4 ucoords in one of the 8 directions (x then y multiplier)
	const int32_t a = g.rnd_draw(8);
	const int32_t mx = g.rnd_draw(4) + 1;
	const int32_t nx = mx * move_x[a + 1] * 0x30 + u.x;
	const int32_t my = g.rnd_draw(4) + 1;
	const int32_t ny = my * move_y[a + 1] * 0x30 + u.y;
	const World& w = g.world;
	if (nx < 0 || ny < 0 || nx >= w.tile_xs * 0xc0 || ny >= w.tile_ys * 0xc0) return;  // WorldData::is_valid(Coord, Coord)
	if (detect_unit_collision(g, u, nx, ny, true, true, 0, false, false) != 0) return;
	add_move_order(g, u, nx, ny, 1, false, kQueueNew, false, -1, -1);
}

void animal_work(Game& g, Unit& u) {
	Order* cur = u.front();
	do_job(g, u, cur ? cur->type : kOrderNone, cur);
	if (!u.alive()) return;
	if (u.cara == -1) return;
	if ((u.o + g.frame) % 16 != 0) return;
	if (size_t(u.cara) >= g.herds.size()) return;
	const int32_t good = g.herds[size_t(u.cara)].good_o;
	if (good == -1 || size_t(good) >= g.goods.size()) return;
	for (int32_t p = 0; p < 8; p++) {
		if (!(g.leaders[size_t(u.who)].flags & leaderflag::kValid)) continue;  // (the original tests the animal's own owner)
		if (is_seen(g, fcoord_of(u.x), fcoord_of(u.y), p)) g.goods[size_t(good)].ever_seen |= uint8_t(1u << p);
	}
}

void animal_process(Game& g, Unit& u) {
	animal_work(g, u);
	if (!u.alive()) return;
	for (int32_t i = 0; i < u.guy_mark && size_t(i) < u.guys.size(); i++) guy_process(g, u, u.guys[size_t(i)]);
	for (int32_t i = u.ptype->squad_size; size_t(i) < u.guys.size(); i++) guy_process(g, u, u.guys[size_t(i)]);
}

void animal_think_bird(Game& g, Unit& u, Order& P) {
	if (u.spell_time >= 0) {
		u.spell_time = int16_t(u.spell_time + 1);
		if ((g.frame & 7) != 0) return;
	}
	u.mana_burn = 0;  // +0x96
	if (u.type == 0x194) return;
	if (u.type != 0x192 || P.wp.empty()) return;
	const World& w = g.world;
	int32_t& wx = P.wp[0][0];
	int32_t& wy = P.wp[0][1];
	int32_t nx, ny;
	if (u.spell_time < 1) {
		nx = g.rnd_draw(0x51) - 0x28 + wx;
		ny = wy - 0x28 + g.rnd_draw(0x51);
	} else {
		nx = g.rnd_draw(0xf) - 7 + wx;
		ny = wy - 7 + g.rnd_draw(0xf);
	}
	if (u.spell_time >= 0) {
		P.oxx = -1;
		P.whose = -1;
	}
	if (nx >= 0 && ny >= 0 && nx < w.tile_xs * 0xc0 && ny < w.tile_ys * 0xc0 &&
	    w.w(wcoord_of(nx), wcoord_of(ny)).region == w.w(wcoord_of(wx), wcoord_of(wy)).region) {
		wx = nx;
		wy = ny;
	}
	if (u.spell_time > 0) {
		u.spell_time = int16_t(u.spell_time + 1);
		const int32_t s = u.spell_time;
		if (s - 1 > 0) {
			const int32_t r = g.rnd_draw(s);
			if (r == 100 || r > 799) {
				u.spell_time = 0;
				int32_t cx = wcoord_of(wx), cy = wcoord_of(wy);
				const Region& reg = g.regions.r[size_t(std::clamp<int32_t>(w.w(cx, cy).region, 0, Regions::kNum - 1))];
				for (int k = 0; k < 30; k++) {
					const int32_t n = reg.size;
					const int32_t idx = n - 1 < 1 ? 0 : g.rnd_draw(n);
					if (size_t(idx) >= reg.coords.size()) break;
					const WCoord c = reg.coords[size_t(idx)];
					int32_t v = g.rnd_draw(0x32) + 1;
					const uint16_t fl = w.w(c.x, c.y).flags;
					if (fl & 0x20) v *= 3;
					if (fl & 0x10) v *= 2;
					if (v > -1) {
						cx = c.x;
						cy = c.y;
					}
				}
				wx = cx * 0x300 + 0x180;
				wy = cy * 0x300 + 0x180;
			}
		}
	}
}

void herds_process(Game& g) {
	if ((g.frame & 63) != 0) return;
	const int32_t n = int32_t(g.herds.size());
	const int32_t i = (g.frame >> 6) % std::max(5, n);
	if (i >= n || !g.herds[size_t(i)].valid()) return;
	Game::Herd& h = g.herds[size_t(i)];
	// Herd::process @741760: two draws, then the target moves one wcoord at most when the cell is open ground.
	const int32_t tx = h.cx - 1 + g.rnd_draw(3);
	const int32_t ty = h.cy - 1 + g.rnd_draw(3);
	const World& w = g.world;
	if (tx < 0 || ty < 0 || tx >= w.xs || ty >= w.ys) return;
	const WData& d = w.w(tx, ty);
	if ((d.flags & 0x70) != 0 || d.blocked >= 8) return;
	h.wx = tx;
	h.wy = ty;
}

}  // namespace ron::sim
