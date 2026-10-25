// Nuclear explosions (docs/nukes-spec.md). Nuke::add_nuke @92ba30, Nuke::do_damage @92bc80, Nuke::been_damaged_before
// @925c20, TerrainOut::terraform_for_nuke @869ca0 (heights only), the nuclear branch of Ammo::do_damage @678060 and the nuke
// embargo of LeaderData (@6d52b0 / @6d52c0 / @6d5350). None of it draws from game_random itself (the sound and the smoulder
// particles use presentation generators); the draws come from Object::do_damage, which the explosion calls.
#include "sim/nukes.hpp"

#include <algorithm>
#include <cstdio>

#include "sim/ai.hpp"
#include "sim/combat_internal.hpp"
#include "sim/compass.hpp"
#include "sim/fog.hpp"
#include "sim/trig.hpp"
#include "sim/victory.hpp"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace ron::sim {

using namespace combat_detail;

namespace {

// NukeOut::init (@92c9a0) max_nuke_frames = 0x6e, Nuke::init (@92c960) the fireball: 10 frames of delay, then 30 of growth.
constexpr int32_t kMaxNukeFrames = 110, kDelay = 10, kGrow = 30;
// A finished explosion stays in the list another 0x140 frames (the smoulder), then do_damage drops it.
constexpr int32_t kKeepFrames = 0x140;
// nuke_crater_size @c06290 = 3 (radius[3] = 49 tiles), nuke_crater_depth @c0628c = 30.0f.
constexpr int32_t kCraterSize = 3;
constexpr float kCraterDepth = 30.0f;
constexpr int32_t kMissileDefenseBonus = 693;  // LeaderData::has_preq(MISSILE_DEFENSE_BONUS)
constexpr int32_t kNukeLine = 0x13b;            // NUCLEARMISSILE 315; is(0x13b, 0) is the whole line (the ICBM too)
constexpr int32_t kSpaceProgram = 0x21e;

// Nuke::been_damaged_before(who, o, i).
bool been_damaged_before(const NukeBlast& b, int32_t who, int32_t o) {
	for (size_t k = 0; k < b.hit_o.size(); k++)
		if (b.hit_o[k] == o && b.hit_who[k] == who) return true;
	return false;
}

// TerrainOut::terraform_for_nuke @869ca0: sinks the corners of the 7 x 7 tiles around the impact by 30 (once per tile, and
// only where the result stays above zero). The second pass of the original (normals, tangents, terrain visibility) is
// presentation. Terrain::refresh_good_z only moves goods' z, which the engine does not keep.
void terraform_for_nuke(Game& g, int32_t x, int32_t y) {
	World& w = g.world;
	const int32_t cx = tcoord_of(x), cy = tcoord_of(y);
	const int32_t cells = kMoveRadius[kCraterSize];
	for (int32_t k = 0; k < cells; k++) {
		const int32_t tx = cx + move_x[k], ty = cy + move_y[k];
		// The original compares the tile with the wcoord dimensions on the last two tests (tx != xs - 1, ty != ys - 1).
		if (tx < 0 || ty < 0 || tx >= w.tile_xs || ty >= w.tile_ys || tx == 0 || ty == 0 || tx == w.xs - 1 || ty == w.ys - 1) continue;
		const uint16_t m = w.t(tx, ty) & tmask::kLowField;
		if (m == 3 || m == 2) continue;
		if (w.is_cliff_at(tx, ty)) continue;
		if (w.w(tx >> 2, ty >> 2).land != 0) continue;
		const int32_t key = tx + w.xs * ty * 4;
		if (std::find(g.nukes.craters.begin(), g.nukes.craters.end(), key) != g.nukes.craters.end()) continue;
		g.nukes.craters.push_back(key);
		const int32_t vxs = g.heights.vxs;
		auto sink = [&](size_t i) {
			if (i >= g.heights.h.size()) return;
			const float v = g.heights.h[i] - kCraterDepth;
			if (0.0f < v) g.heights.h[i] = v;
		};
		const size_t i0 = size_t(vxs) * size_t(ty) + size_t(tx), i1 = size_t(vxs) * size_t(ty + 1) + size_t(tx);
		sink(i0);
		sink(i0 + 1);
		sink(i1);
		sink(i1 + 1);
	}
}

// Objects::find_units @65a620 as Nuke::do_damage calls it (SEARCH_ALL, who -1, FILTER_ALL, players' units only): the on-map
// live units of leaders 0..7, in the order of the exe (the circle cells of the world grid when the game holds at least as many
// units as the circle has cells, else every leader's slots).
void find_units(const Game& g, int32_t x, int32_t y, int32_t radius, std::vector<std::pair<int32_t, int32_t>>& out) {
	out.clear();
	const Circles& C = circles();
	const int32_t k = (radius + 0x2ff) / 0x300;
	auto pass = [&](const Object* ob) { return ob && ob->is_unit && ob->alive() && static_cast<const Unit*>(ob)->on_map(); };
	if (radius >= 0 && k >= 0 && k <= 64 && C.radius[size_t(k)] <= g.total_units) {
		const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
		for (int32_t i = 0; i < C.radius[size_t(k)]; i++) {
			const int32_t cx = wx + C.x[size_t(i)], cy = wy + C.y[size_t(i)];
			if (!g.world.in_w(cx, cy)) continue;
			const WData& d = g.world.w(cx, cy);
			int32_t co = d.down, cw = d.down_who;
			while (co >= 0) {
				const Object* ob = g.object(cw, co);
				if (!ob) break;
				const int32_t no = ob->down, nw = ob->down_who;
				if (cw >= 0 && cw < 8 && search_valid_search(g, cw, search::kAll, -1) && pass(ob)) out.emplace_back(co, cw);
				co = no;
				cw = nw;
			}
		}
		return;
	}
	for (int32_t lw = 0; lw < 8; lw++) {
		if (!(g.leaders[size_t(lw)].flags & leaderflag::kValid)) continue;
		const ObjectList& L = g.objects[size_t(lw)];
		for (int32_t o = 0; o < L.unit_mark; o++) {
			const Unit* u = g.unit(lw, o);
			if (!pass(u)) continue;
			const int32_t d = vector_dist(u->x - x, u->y - y);
			const int32_t br = u->ptype->domain == 1 ? u->ptype->push_size : u->ptype->big_radius;
			if (radius < 0 || d - br <= radius) out.emplace_back(o, lw);
		}
	}
}

// What the victim's owner does when the nuke fell on its territory: Leader::action_declare(shooter, 0, free, 0), free = 1
// (the victim's leader flag 4 is clear), only if it is not already at war with the shooter.
void declare_on_shooter(Game& g, int32_t victim, int32_t shooter, int32_t x, int32_t y) {
	Leader& V = g.leaders[size_t(victim)];
	if (V.flags & 4) return;
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	if (!g.world.in_w(wx, wy) || g.world.w(wx, wy).who != victim) return;
	if (is_enemy(g, victim, shooter)) return;
	leader_action_declare(g, victim, shooter, 0, int32_t((~(V.flags >> 2)) & 1), 0);
}

}  // namespace

void nuke_add(Game& g, int32_t x, int32_t y, int32_t r0, int32_t r1, int32_t who, int32_t o) {
	NukeBlast b;
	b.x = x;
	b.y = y;
	b.r0 = r0;
	b.r1 = r1;
	b.frame = g.frame;
	b.who = who;
	b.o = o;
	g.nukes.blasts.push_back(std::move(b));
	terraform_for_nuke(g, x, y);
}

int32_t nuke_num_current(const Game& g) {
	int32_t n = 0;
	for (const NukeBlast& b : g.nukes.blasts)
		if (g.frame - b.frame < kMaxNukeFrames) n++;
	return n;
}

void nuke_do_damage(Game& g) {
	std::vector<NukeBlast>& list = g.nukes.blasts;
	// Finished explosions leave by swapping the last one into their slot (and the slot is looked at again).
	for (size_t i = 0; i < list.size();) {
		const NukeBlast& b = list[i];
		if (g.frame < b.frame || kMaxNukeFrames + b.frame + kKeepFrames <= g.frame) {
			if (i + 1 != list.size()) list[i] = std::move(list.back());
			list.pop_back();
		} else {
			i++;
		}
	}
	// (NukeOut::graph_do_damage: the smouldering particles, presentation.)
	std::vector<std::pair<int32_t, int32_t>> found;
	for (size_t i = 0; i < list.size(); i++) {
		const int32_t frame = g.frame;
		const int32_t d = frame - list[i].frame;
		// Armageddon: the one frame the explosion is exactly max_nuke_frames old.
		if (float(d - 1) / float(kMaxNukeFrames) < 1.0f && 1.0f <= float(d) / float(kMaxNukeFrames)) armageddon_nuke_detonated(g);
		if (!(list[i].frame <= frame && frame < kDelay + kGrow + list[i].frame)) continue;
		float f = 0.0f;
		if (frame >= list[i].frame + kDelay) f = float(frame - list[i].frame - kDelay) / float(kGrow);
		const int32_t R = int32_t(float(list[i].r1) * f + (1.0f - f) * float(list[i].r0));
		const int32_t who = list[i].who, so = list[i].o;
		if (who == -1 || so == -1) continue;
		Object* sh = who >= 0 && who < kNumLeaders ? g.object(who, so) : nullptr;
		if (!sh) continue;
		const int32_t nx = list[i].x, ny = list[i].y;
		const int32_t r1i = list[i].r1;  // (the divisor is the float radius truncated)
		if (r1i == 0) continue;           // (the original divides by zero)
		// ---- units
		find_units(g, nx, ny, R, found);
		for (const auto& e : found) {
			const int32_t idx = e.first, vw = e.second;
			Unit* u = g.unit(vw, idx);
			if (!u || !u->alive() || !u->on_map()) continue;
			if (been_damaged_before(list[i], vw, idx)) continue;
			if (type_is(*u->ptype, kNukeLine, false)) continue;
			int32_t dist = vector_dist(nx - u->x, ny - u->y);
			if (dist < 0) dist = 0;
			if (dist > R) continue;
			const int32_t pct = 0x100 - (dist << 8) / r1i;
			if (pct <= 0) continue;
			const uint32_t angle = find_angle(u->x - nx, u->y - ny);
			do_damage(g, *sh, idx, vw, angle, 1, -1, pct, true, false);
			list[i].hit_o.push_back(idx);
			list[i].hit_who.push_back(vw);
			if (war_allowed(g) && who != vw) declare_on_shooter(g, vw, who, nx, ny);
		}
		// ---- buildings (and whatever else lies in the wcoords around)
		const int32_t rr = std::min(R / 8 + 1, 10);
		const int32_t wcx = wcoord_of(nx), wcy = wcoord_of(ny);
		const int32_t cells = std::min(kMoveRadius[rr], kNumMoves - 1);  // k = 0 .. radius[rr] inclusive; move[441] is off the map
		for (int32_t k = 0; k <= cells; k++) {
			const int32_t wx = wcx + move_x[k], wy = wcy + move_y[k];
			if (!g.world.in_w(wx, wy)) continue;
			int32_t co = g.world.w(wx, wy).down, cw = g.world.w(wx, wy).down_who;
			for (int32_t guard = 0; co >= 0 && guard < 4096; guard++) {
				Object* ob = g.object(cw, co);
				if (!ob) break;
				const int32_t vw = cw, vo = co;
				if (!been_damaged_before(list[i], vw, vo) && !ob->is_unit && ob->alive()) {
					const int32_t dx = std::max(0, iabs(nx - ob->x) - ob->ptype->x_size * 96);
					const int32_t dy = std::max(0, iabs(ny - ob->y) - ob->ptype->y_size * 96);
					const int32_t dist = vector_dist(dx, dy);
					int32_t pct;
					if (dist < R && (pct = 0x100 - (dist << 8) / r1i) >= 0) {
						const uint32_t angle = find_angle(ob->x - sh->x, ob->y - sh->y);
						do_damage(g, *sh, vo, vw, angle, 0, -1, pct, true, false);
						list[i].hit_o.push_back(vo);
						list[i].hit_who.push_back(vw);
						if (war_allowed(g) && who != vw) declare_on_shooter(g, vw, who, nx, ny);
					}
				}
				// the next object of the wcoord list is read after the damage
				co = ob->down;
				cw = ob->down_who;
			}
		}
	}
}

bool nuke_missile_impact(Game& g, const Ammo& a, Object& shooter) {
	const bool nuclear = type_is(*shooter.ptype, kNukeLine, false);
	// The territory owner at the impact; a missile that falls on the sea is judged by the first land owner within 120 cells.
	const World& w = g.world;
	const int32_t bx = wcoord_of(a.ex), by = wcoord_of(a.ey);
	int32_t owner = -1;
	if (w.in_w(bx, by)) {
		if (!w.is_ocean(bx, by)) {
			owner = w.w(bx, by).who;
		} else {
			for (int32_t k = 1; k < 120; k++) {
				const int32_t cx = bx + move_x[k], cy = by + move_y[k];
				if (cx < 0 || cy < 0 || cx >= w.xs || cy >= w.ys) continue;
				if (w.is_ocean(cx, cy)) continue;
				owner = w.w(cx, cy).who;
				if (owner >= 0) break;
			}
		}
	}
	if (owner >= 0 && owner < 8 && has_preq(g, owner, kMissileDefenseBonus) && a.who != owner) {
		GameEvent e;
		e.frame = g.frame, e.kind = nukeevent::kIntercepted, e.who = a.who, e.other = owner, e.o = a.o, e.x = a.ex, e.y = a.ey;
		g.events.push_back(e);
		return true;  // (the console messages and S_MISSILE_SHIELD): the ammo closes, nobody is hurt
	}
	if (!nuclear) {
		if (a.who >= 0 && a.who < 8) g.leaders[size_t(a.who)].missiles_used++;  // LeaderData +0x7c0 (the nuke-hit sound)
		return false;
	}
	nuke_detonate(g, a.who, a.o, a.ex, a.ey, shooter.ptype->splash_area);
	return true;
}

void nuke_detonate(Game& g, int32_t who, int32_t o, int32_t x, int32_t y, int32_t splash) {
	if (who >= 0 && who < 8) {
		g.nukes.stamp[size_t(who)] = g.frame;  // LeaderData +0x7b4
		g.leaders[size_t(who)].nukes_used++;   // +0x7bc
	}
	nuke_add(g, x, y, 100, splash * 192, who, o);
	// Everyone sees the ground around the explosion: the circle of clamp(splash + 1, 0, 64) tiles, fog cells of two tiles.
	const Circles& C = circles();
	const int32_t cr = std::clamp(splash + 1, 0, 64);
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	for (int32_t i = 0; i < C.radius[size_t(cr)]; i++) {
		const int32_t px = tx + C.x[size_t(i)], py = ty + C.y[size_t(i)];
		if (px < 0 || py < 0 || px >= g.world.tile_xs || py >= g.world.tile_ys) continue;
		fog_set_seen2(g.world, px >> 1, py >> 1, 0xff, false);
	}
	GameEvent e;
	e.frame = g.frame, e.kind = nukeevent::kDetonated, e.who = who, e.o = o, e.x = x, e.y = y, e.value = splash * 192;
	g.events.push_back(e);
	nuke_do_damage(g);
}

int32_t get_my_nuke_embargo(const Game& g, int32_t who) {
	if (who < 0 || who >= 8) return 0;
	const int32_t stamp = g.nukes.stamp[size_t(who)];
	if (stamp == 0 || has_wonder(g, who, kSpaceProgram)) return 0;
	const int32_t v = ((RON_K(g, "nuke_embargo_world") * g.armageddon + RON_K(g, "nuke_embargo_nation") * g.leaders[size_t(who)].nukes_used) -
	                   g.frame) +
	                  RON_K(g, "nuke_embargo_base") + stamp;
	return v < 0 ? 0 : v;
}

int32_t get_nuke_embargo(const Game& g, int32_t who) {
	if (has_wonder(g, who, kSpaceProgram)) return 0;
	int32_t best = 0;
	for (int32_t i = 0; i < 8; i++) {
		if (!(g.leaders[size_t(i)].flags & leaderflag::kValid)) continue;
		if (i != who && !is_ally(g, who, i)) continue;
		best = std::max(best, get_my_nuke_embargo(g, i));
	}
	return best;
}

}  // namespace ron::sim
