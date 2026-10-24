// Conquer the World: the reinforcement armies of a battle (docs/ctw-spec.md 5.3): setup_reinforcements @789d70 (who
// sends an army and when), find_reinforcement_loc @78a3d0 (the map edge an army enters from, from the strategic map's
// geometry: find_angle / angle_to_degrees / map_to_wcoord / get_scenario_map_rect), ReinforcementArmy::place_army @7aa560 and
// ConquestGame::place_reinforcements @798880 (the per-frame arrival), and the maths of get_conquest_invasion_dir @9e5ef0.
//
// Ported from the decompiles in re/decomp/ctw2. Deviations: the sound / message-window / notice presentation of
// place_army is reduced to `battle_notices`; the Terra Cotta Army reinforcement (time -1) is placed when the owner's civic
// line reaches CTW_WONDER_MIN_CIVICS, as place_reinforcements tests.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sim/ctw.hpp"
#include "sim/game.hpp"
#include "sim/groups.hpp"
#include "sim/orders.hpp"
#include "sim/production.hpp"
#include "sim/search.hpp"

namespace ron::sim::ctw {

namespace {

// find_angle @92d130: the angle of (x, -y) as a 32-bit binary angle (a quarter turn = 0x40000000), a 4th-order
// polynomial of min / max. All arithmetic wraps like the original's 32-bit registers.
int32_t find_angle(int32_t x, int32_t y_in) {
	const int32_t y = int32_t(0u - uint32_t(y_in));
	if (x == 0) return y > 0 ? 0 : int32_t(0x80000000u);
	if (y == 0) return x > 0 ? 0x40000000 : int32_t(0xc0000000u);
	const int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
	const bool swapped = ay < ax;  // EBX
	const int32_t hi = swapped ? ax : ay, lo = swapped ? ay : ax;
	const int32_t ratio = int32_t((int64_t(lo) << 14) / hi);
	const int32_t t = 0x1333 - ratio;
	const int32_t at = t < 0 ? -t : t;
	const uint32_t u = uint32_t((0x2800 - ((at * 0xb00) >> 14)) * ratio) & 0xffffc000u;
	const uint32_t q = u * 4u;
	if (x < 1) {
		if (y < 1) return swapped ? int32_t(0u - q - 0x40000000u) : int32_t(q + 0x80000000u);
		return swapped ? int32_t(q - 0x40000000u) : int32_t(0u - q);
	}
	if (y > 0) return swapped ? int32_t(0x40000000u - q) : int32_t(q);
	return swapped ? int32_t(q + 0x40000000u) : int32_t(0x80000000u - q);
}

// angle_to_degrees @a28e00 (unsigned arithmetic: the angle is split into quarters, eighths and finer fractions).
int32_t angle_to_degrees(uint32_t in) {
	const uint32_t q = in >> 0x1e;
	uint32_t a = in - q * 0x40000000u;
	const uint32_t h = a >> 0x1d;
	a = a - h * 0x20000000u;
	const uint32_t b = a % 0x15555555u;
	const uint32_t c = b % 0xaaaaaaau;
	return int32_t((c % 0x38e38e3u + 0x5b05b0u) / 0xb60b60u +
	               (c / 0x38e38e3u + ((h + q * 2u) * 3u + b / 0xaaaaaaau + (a / 0x15555555u) * 2u) * 3u) * 5u);
}

// fast_angle_to_degrees @a28f70: the degrees of the angle's top byte.
float fast_angle_to_degrees(int32_t angle) {
	const uint32_t idx = uint32_t(angle >> 24) & 0xffu;
	return float(angle_to_degrees(idx << 24));
}

}  // namespace

// find_angle + angle_to_degrees of a map delta (x right, y down): 0 north, 90 east, 180 south, 270 west (selftest hook).
int angle_degrees_of(int dx, int dy) { return angle_to_degrees(uint32_t(find_angle(dx, dy))); }

// ---------------------------------------------------------------- strategic map geometry

float ConquestGame::map_scale() const {
	// ConquestMap::init_land @769190: map_scale = map_width / map_tex_width (both from the campaign's ROOT).
	float w = 1000.0f;
	if (def) {
		const auto it = def->root.find("map_width");
		if (it != def->root.end()) w = std::strtof(it->second.c_str(), nullptr);
	}
	const int tex = def ? def->tex_width : 0;
	return tex > 0 ? w / float(tex) : 1.0f;
}

float ConquestGame::map_width() const {
	if (def) {
		const auto it = def->root.find("map_width");
		if (it != def->root.end()) return std::strtof(it->second.c_str(), nullptr);
	}
	return 1000.0f;
}

void ConquestGame::country_ws(int node, float* x, float* y) const {
	const float s = map_scale();
	*x = float(def->countries[size_t(node)].x) * s;
	*y = float(def->countries[size_t(node)].y) * s;
}

namespace {

struct Rect {
	float left = 0, top = 0, right = 0, bottom = 0;
};

float fabsf_(float v) { return v < 0 ? -v : v; }

// ConquestCountries::get_scenario_map_rect @75c3b0, the strategic-map-pixel branch (param_5 = 1) for two different nodes:
// a square around the two countries (wrapping the map's width when that is shorter), by conquest map style.
Rect scenario_map_rect(const ConquestGame& g, int n1, int n2, int map_style) {
	float x1, y1, x2, y2;
	g.country_ws(n1, &x1, &y1);
	g.country_ws(n2, &x2, &y2);
	const float mw = g.map_width();
	const float dx = x1 - x2, dy = y1 - y2;
	const float mn = std::min(x1, x2), mx = std::max(x1, x2);
	float wrap = (mw - mx) + mn;
	if (dx > 0.0f) wrap = -wrap;
	float cx = 0, cy = 0, size = 0;
	switch (map_style) {
	case 0: case 1: case 2: case 3: case 8: {
		cx = x1 - dx * 0.5f;
		cy = y1 - dy * 0.5f;
		float ext = dx;
		if (fabsf_(dx) > fabsf_(wrap)) {
			cx = x1 - wrap * 0.5f;
			ext = wrap;
			if (x1 > x2) cx -= mw;
		}
		size = std::max(fabsf_(ext), fabsf_(dy)) * 1.3f;
		break;
	}
	case 4: case 6: case 7:
	case 5: {
		const bool second = map_style != 5;
		cx = second ? x2 : x1;
		cy = second ? y2 : y1;
		size = std::max(fabsf_(fabsf_(dx) > fabsf_(wrap) ? wrap : dx), fabsf_(dy));
		break;
	}
	default: break;
	}
	Rect r;
	r.left = cx - size * 0.5f;
	r.top = cy - size * 0.5f;
	r.right = r.left + size;
	r.bottom = r.top + size;
	return r;
}

}  // namespace

// ConquestGame::find_reinforcement_loc @78a3d0: `a` = the defended node (link[0]), `b` = the attacker's node (link[1]),
// `n` the army's origin node; `att` = the army belongs to the attacker's nation. W / H: the battle world's size in W cells.
void ConquestGame::find_reinforcement_loc(int world_w, int world_h, int a, int b, int n, int map_style, bool att, Reinforcement* out) const {
	int z;
	switch (map_style) {
	case 0: case 1: case 2: case 3: case 8: z = att ? b : a; break;
	case 4: case 6: case 7: z = a; break;
	case 5: z = b; break;
	default: return;
	}
	float nx, ny, zx, zy;
	country_ws(n, &nx, &ny);
	country_ws(z, &zx, &zy);
	const int32_t dx = int32_t((nx - zx) * 100.0f), dy = int32_t((ny - zy) * 100.0f);
	int32_t deg = angle_to_degrees(uint32_t(find_angle(dx, dy)));
	if (deg == 0 || deg == 0x5a || deg == 0xb4 || deg == 0x10e) deg++;
	const int32_t W = world_w, H = world_h;
	const int32_t diag = int32_t(std::sqrt(float(W * W + H * H)));
	// map_to_wcoord @789c90 with the scenario rect of (b, a)
	const Rect rc = scenario_map_rect(*this, b, a, map_style);
	float px = zx;
	if (px < rc.left || rc.right <= px || zy < rc.top || rc.bottom <= zy) px = px - map_width();
	const int32_t ox = int32_t(((px - rc.left) / (rc.right - rc.left)) * float(W));
	const int32_t oy = int32_t(((zy - rc.top) / (rc.bottom - rc.top)) * float(W));
	if (getenv("CTW_DEBUG"))
		std::fprintf(stderr, "find_reinforcement_loc: W %d H %d a %d b %d n %d z %d style %d att %d dx %d dy %d deg %d rect %.1f %.1f %.1f %.1f -> o (%d,%d)\n", W, H,
		             a, b, n, z, map_style, int(att), dx, dy, deg, double(rc.left), double(rc.top), double(rc.right), double(rc.bottom), ox, oy);
	if (n == b) {
		out->loc_x = out->dest_x = ox;
		out->loc_y = out->dest_y = oy;
	} else {
		const int32_t yy = H - oy;  // EBP+0x18
		const float rad = float(deg) * 0.017453292f;
		const int32_t cs = int32_t(std::cos(rad) * float(diag));
		const int32_t sn = int32_t(std::sin(rad) * float(diag));
		const float r = float(cs) / float(sn);
		const int32_t c = yy - int32_t(float(ox) * r);
		out->dest_x = ox;
		out->dest_y = H - yy;
		int32_t lx = out->loc_x, ly = out->loc_y;
		if (deg < 0x5a) {
			ly = 0;
			lx = W - 1;
			const int32_t ix = int32_t(float(W - c) / r);
			const int32_t iy = int32_t(float(W) * r + float(c));
			if (ix < W) lx = ix;
			else if (iy < W) ly = W - iy;
		} else if (deg < 0xb4) {
			lx = W - 1;
			ly = W - 1;
			const int32_t ix = int32_t(float(-c) / r);
			const int32_t iy = int32_t(float(W) * r + float(c));
			if (ix < W) lx = ix;
			else if (iy > 0) ly = W - iy;
		} else if (deg < 0x10e) {
			lx = 0;
			ly = W - 1;
			const int32_t ix = int32_t(float(-c) / r);
			if (ix > 0) lx = W - ix;
			else if (c > 0) ly = W - c;
		} else {
			lx = 0;
			ly = 0;
			const int32_t ix = int32_t(float(W - c) / r);
			if (ix > 0) lx = ix;
			else if (c < W) ly = W - c;
		}
		out->loc_x = lx;
		out->loc_y = ly;
	}
}

// get_conquest_invasion_dir @9e5ef0: the direction (degrees) from the defended node toward the attacker's.
int ConquestGame::invasion_dir() const {
	if (cur_link.from < 0 || cur_link.to < 0) return -1;
	float xa, ya, xb, yb;
	country_ws(cur_link.to, &xa, &ya);
	country_ws(cur_link.from, &xb, &yb);
	const int32_t y = int32_t((yb - ya) * 100.0f), x = int32_t((xb - xa) * 100.0f);
	return int(fast_angle_to_degrees(find_angle(x, y)));
}

// ---------------------------------------------------------------- setup_reinforcements @789d70 (the list and the times)

void ConquestGame::setup_reinforcements(const LinkChoice& l, const StyleDef& style) {
	reinforcements.clear();
	reinf_att_side.clear();
	const int a = l.to, b = l.from;  // link[0] defended, link[1] the attacker's node
	// ten arrival times: each draw's window is shifted by everything drawn before it
	int lo = crules->min_arrival_time, hi = crules->max_arrival_time;
	int times[10];
	for (int k = 0; k < 10; k++) {
		times[k] = rget(lo, hi);
		lo += times[k];
		hi += times[k];
	}
	int counter[2] = {0, 0};
	const int att_owner = node_eff_owner(b), def_owner = node_eff_owner(a);
	for (int n : nodes[size_t(a)].links) {
		if (n == b) continue;
		const int o = node_eff_owner(n);
		bool add = false;
		bool from_attacker = false;
		auto available = [&](int node) {
			if (nodes[size_t(node)].owner < 0) return false;
			for (const auto& list : pieces) {
				for (const Piece& p : list) {
					if (p.node == node && !p.moved) return true;
				}
			}
			return false;
		};
		if (o == att_owner) {
			add = available(n);
			from_attacker = add;
		}
		if (!add && o == def_owner && has_army(n)) add = true;
		if (!add) continue;
		Reinforcement r;
		r.side = o != player_tribe ? 1 : 0;
		r.time = times[std::min(9, counter[r.side])] * 60;
		counter[r.side]++;
		r.node = n;
		reinforcements.push_back(r);
		reinf_att_side.push_back(from_attacker);
	}
	if (style.is_default) {
		// the Terra Cotta Army (wonder 0x211): its owner's army appears when the civic line reaches CTW_WONDER_MIN_CIVICS
		for (int seat = 0; seat < 2; seat++) {
			const int slot = seat == 0 ? player_tribe : (l.attacker == player_tribe ? l.defender : l.attacker);
			if (slot < 0 || (slot != att_owner && slot != def_owner)) continue;
			const Leader& L = leaders[size_t(slot)];
			if (std::find(L.wonders.begin(), L.wonders.end(), 529) == L.wonders.end()) continue;
			int wn = -1;
			for (size_t i = 0; i < nodes.size() && wn < 0; i++) {
				if (nodes[i].wonder == 529) wn = int(i);
			}
			if (wn < 0 || wn == a) continue;
			Reinforcement r;
			r.side = seat;
			r.time = -1;
			r.node = wn;
			reinforcements.push_back(r);
			reinf_att_side.push_back(slot == att_owner);
		}
	}
}

// Fills the entry locations once the battle world exists (find_reinforcement_loc runs inside setup_reinforcements in the
// original, after the world is built). `map_style` is the conquest map style of the battle's style.
void ConquestGame::locate_reinforcements(const LinkChoice& l, int map_style, int world_w, int world_h) {
	const int a = l.to, b = l.from;
	for (size_t i = 0; i < reinforcements.size(); i++) {
		Reinforcement& r = reinforcements[i];
		const bool att = i < reinf_att_side.size() && reinf_att_side[i];
		find_reinforcement_loc(world_w, world_h, a, b, r.time < 0 ? r.node : r.node, map_style, att, &r);
		if (r.time < 0 && r.node == b) {
			// the wonder's node is the attacker's own: the entry snaps to the nearest map edge
			const int ix = world_w - r.loc_x, iy = world_h - r.loc_y;
			if (r.loc_x < ix && r.loc_x < r.loc_y && r.loc_x < iy) r.loc_x = 0;
			else if (ix < r.loc_x && ix < r.loc_y && ix < iy) r.loc_x = world_w - 1;
			else if (r.loc_y < r.loc_x && r.loc_y < ix && r.loc_y < iy) r.loc_y = 0;
			else r.loc_y = world_h - 1;
		}
		r.node = r.time < 0 ? -1 : r.node;
	}
}

// ---------------------------------------------------------------- the arrival (place_reinforcements / place_army)

namespace {

bool tile_ok_for_army(const Game& g, int x, int y, int seat) {
	// ReinforcementArmy::check_edge: a tile that is no third party's region and is unowned / the seat's own / an ally's
	if (!g.world.in_w(x, y)) return false;
	const WData& w = g.world.w(x, y);
	const bool third = w.region != g.leaders[0].home_reg && w.region != g.leaders[1].home_reg;
	if (third) return false;
	const int o = w.who;
	if (o < 0 || o == seat) return true;
	return g.leaders[size_t(seat)].diplos(o) == 2 && g.leaders[size_t(o)].diplos(seat) == 2;
}

bool enemy_territory(const Game& g, int x, int y, int seat) {
	// WorldData::is_enemy_territory @6b2490
	if (!g.world.in_w(x, y)) return false;
	const int o = g.world.w(x, y).who;
	if (o < 0 || o == seat) return false;
	return !(g.leaders[size_t(seat)].diplos(o) == 2 && g.leaders[size_t(o)].diplos(seat) == 2);
}

// ReinforcementArmy::check_edge @7aa400: scans the edge `edge` (1 top, 2 right, 3 bottom, 4 left) for the first acceptable
// tile; x / y receive it.
bool check_edge(const Game& g, int edge, int seat, int* x, int* y) {
	const int W = g.world.xs, H = g.world.ys;
	int x0, y0, xl, yl;
	switch (edge) {
	case 1: x0 = 0; y0 = 0; xl = W; yl = 1; break;
	case 2: x0 = W - 1; y0 = 0; xl = W; yl = H; break;
	case 3: x0 = 0; y0 = H - 1; xl = W; yl = H; break;
	case 4: x0 = 0; y0 = 0; xl = 1; yl = H; break;
	default: return false;
	}
	for (int px = x0; px < xl; px++) {
		for (int py = y0; py < yl; py++) {
			if (tile_ok_for_army(g, px, py, seat)) {
				*x = px;
				*y = py;
				return true;
			}
		}
	}
	return false;
}

int place_one(Game& g, int seat, int type, int32_t x, int32_t y) {
	if (type < 0) return -1;
	const Type& T = g.type(type);
	if (!T.loaded) return -1;
	int32_t ox = x, oy = y;
	if (find_nearby_spot(g, T, x, y, &ox, &oy, 0, 4 * 768, 0, 0x80000000u, 0, -1, -1, 0, 0, -1, 0, -1) != 0) {
		ox = x;
		oy = y;
	}
	return init_unit(g, seat, type, ox, oy);
}

int find_type(const sim::Rules& r, const std::string& name) {
	std::string t = lower(name);
	for (int i = kUnits; i < kEndUnits; i++) {
		const Type& T = r.types[size_t(i)];
		if (!T.loaded) continue;
		if (lower(to_utf8(T.name)) == t || lower(to_utf8(T.type_name)) == t || lower(to_utf8(T.graph_name)) == t) return i;
	}
	return -1;
}

}  // namespace

// ReinforcementArmy::place_army @7aa560: one copy of the ARMIES composition appears at the entry tile and marches to the
// destination (ATTACK_TO).
void ConquestGame::place_reinforcement(Game& g, Reinforcement& r) {
	const int seat = r.side;
	int x = r.loc_x, y = r.loc_y;
	{
		bool third = false;
		if (g.world.in_w(x, y)) {
			const int16_t reg = g.world.w(x, y).region;
			third = reg < 0x40 && reg != g.leaders[0].home_reg && reg != g.leaders[1].home_reg;
		}
		if (enemy_territory(g, x, y, seat) || third) {
			int edge = y == 0 ? 1 : x == 0 ? 4 : x == g.world.xs - 1 ? 2 : y == g.world.ys - 1 ? 3 : 0;
			bool found = false;
			for (int tries = 0; tries < 4 && !found; tries++) {
				int ex = x, ey = y;
				if (check_edge(g, edge, seat, &ex, &ey)) {
					x = ex;
					y = ey;
					found = true;
				}
				edge = edge + 1 == 5 ? 1 : edge + 1;
			}
			if (!found) {
				x = r.loc_x;
				y = r.loc_y;
			}
		}
	}
	const int slot = battle_slot[size_t(seat)];
	const bool human = battle_human[size_t(seat)];
	bool any = false;
	Group grp;
	group_clear(grp, -1);
	grp.id = -1;
	for (const ArmyUnit& au : crules->army) {
		if (!human && au.min_diff > difficulty) continue;
		int t = find_type(*rules, au.type);
		if (t < 0) continue;
		const int up = current_upgrade(g, seat, t);
		if (up >= kUnits && up < kEndUnits) t = up;
		if (tribe_can_type(g, seat, t) == 0) {
			const int gr = get_graft(g, seat, t);
			if (gr >= kUnits && gr < kEndUnits) t = gr;
		}
		if (slot >= 0 && leaders[size_t(slot)].gunpowder_off) {
			// no_gunpowder: fall back along the previous-unit chain while the type needs gunpowder (object mask 0x40)
			for (int guard = 0; guard < 8; guard++) {
				const Type& T = g.type(t);
				if (!(T.obj_masks & 0x40) || T.from < 0) break;
				t = T.from;
			}
		}
		for (int k = 0; k < au.num; k++) {
			const int o = place_one(g, seat, t, x * 768 + 384, y * 768 + 384);
			if (o >= 0) {
				group_add(g, grp, o, seat, 0, 0);
				any = true;
			}
		}
	}
	if (any && group_get_num(g, grp) > 0) {
		const int gi = groups_push_group(g, seat, grp, 0);
		if (gi >= 0) {
			group_action_move_to(g, g.groups.g[size_t(gi)], r.dest_x * 768 + 384, r.dest_y * 768 + 384, kQueueLast, false, 0,
			                     kAttackTo, 0, -1, -1, 0);
		}
		reinf_arrived[size_t(seat)] = 1;
		const std::string from = r.node >= 0 ? node_name(r.node) : std::string("the wonder");
		battle_notices.push_back("Reinforcements arrive from " + from + ".");
	}
}

// ConquestGame::place_reinforcements @798880: every frame of a battle.
void ConquestGame::place_reinforcements(Game& g) {
	if (reinforcements.empty() || reinforce_mark >= int(reinforcements.size())) return;
	for (Reinforcement& r : reinforcements) {
		if (r.placed) continue;
		bool due;
		if (r.time >= 0) due = g.tick > r.time;
		else due = g.leaders[size_t(r.side)].epoch[1] == K(*rules, "ctw_wonder_min_civics");
		if (!due) continue;
		place_reinforcement(g, r);
		r.placed = true;
		reinforce_mark++;
	}
}

}  // namespace ron::sim::ctw
