// Building road search: see sim/roads.hpp.
#include "sim/roads.hpp"

#include <algorithm>
#include <map>
#include <cstdlib>

#include "sim/compass.hpp"
#include "sim/fog.hpp"
#include "sim/game.hpp"
#include "sim/guys.hpp"
#include "sim/pathfinder.hpp"
#include "sim/pathfinder_search.hpp"
#include "sim/placement.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

constexpr int32_t kTile = 0xc0;  // world units per tcoord

// WallData::covers_tile @6439b0: the object's footprint (tile_corner .. + x_size / y_size).
bool covers_tile(const Object& ob, int32_t tx, int32_t ty) {
	if (!ob.ptype) return false;
	const Footprint f = footprint(*ob.ptype, ob.x, ob.y);
	return tx >= f.x0 && tx < f.x0 + f.w && ty >= f.y0 && ty < f.y0 + f.h;
}

struct Ends {
	int32_t src_o, src_pl, dst_o, dst_pl;
};

// One tile of PathFinderData::valid_roadcoord @688740 (the recursive diagonal-corner call tests exactly this part).
bool tile_ok(const Game& g, const PathFinder& pf, int32_t x, int32_t y, const Ends& e) {
	const World& w = g.world;
	if (x < 0 || y < 0 || x >= w.tile_xs * kTile || y >= w.tile_ys * kTile) return false;
	const int32_t tx = x / kTile, ty = y / kTile;
	const uint16_t td = w.t(tx, ty);
	if (!pf.can_transport && (td & 0x30) == 0x20) return false;
	const bool blocked = (td & 0x4000) != 0;
	const bool occupied = (td & 3) == 3 || (td & 0x80) != 0;
	if ((blocked || occupied) && (blocked || (td & 0x30) != 0x10)) {
		// not a plain road tile: allowed only when a building stands there (not just the 0x4000 bit) and it lies inside the footprint
		// of the source or the destination CITY
		if (!occupied) return false;
		const Object* s = g.object(e.src_pl, e.src_o);
		if (s && s->ptype && is_city_type(*s->ptype) && covers_tile(*s, tx, ty)) return true;
		const Object* d = g.object(e.dst_pl, e.dst_o);
		if (!d || !d->ptype || !is_city_type(*d->ptype)) return false;
		if (!covers_tile(*d, tx, ty)) return false;
	}
	return true;
}

bool valid_roadcoord(const Game& g, const PathFinder& pf, int32_t x, int32_t y, int32_t px, int32_t py, const Ends& e) {
	if (!tile_ok(g, pf, x, y, e)) return false;
	return px == x || py == y || tile_ok(g, pf, x, py, e) || tile_ok(g, pf, px, y, e);
}

// PathFinder::calc_road_cost @686300 (docs/movement-spec.md 12.3). `z` is PathNode.z_val (a short).
struct RoadNode {
	int32_t x = 0, y = 0;
	int16_t z = 0;
};

int32_t calc_road_cost(Game& g, const PathFinder& pf, RoadNode& N, int32_t px, int32_t py, int32_t pz, int32_t src_pl, int32_t dst_pl,
                       int32_t d) {
	const World& w = g.world;
	const int32_t tx = N.x / kTile, ty = N.y / kTile;
	const int32_t r = g.rnd_draw(20);  // the one draw, before everything else
	int32_t cost = r + pf.road_base_val;
	int32_t rnd = r;
	const uint16_t td = w.t(tx, ty);
	if ((td & 0x30) == 0x20) {
		if (pf.avoid_sea == 0) {
			if ((w.t(px / kTile, py / kTile) & 0x30) != 0x20) cost += pf.road_cross_coast;
		} else {
			cost += pf.road_avoid_sea;
		}
		N.z = 0;
	} else {
		bool friendly = false;
		const WData& wd = w.w(tx >> 2, ty >> 2);
		const int32_t owner = wd.who;
		if (owner < 0) {
			cost += pf.road_noone;
		} else {
			const bool own_or_mutual = src_pl == owner || (g.leaders[size_t(owner)].diplos(src_pl) == 2 && g.leaders[size_t(src_pl)].diplos(owner) == 2);
			if (own_or_mutual && is_ally(g, owner, dst_pl)) friendly = true;
			else cost += pf.road_enemy;
		}
		if ((td & 0x6000) == 0x2000 && (td & 0x30) != 0x10) cost += pf.road_bad_path;
		if (wd.flags & 8) {
			cost += pf.road_river * 2;
			rnd += pf.road_river * 2;
		}
		if (td & 0x800) {
			cost += pf.road_river;
			rnd += pf.road_river;
		}
		int32_t z = find_tcoord_z(g, tx, ty, 0);
		if (z < 1) z = 0;
		N.z = int16_t(z);
		const int32_t m = std::min(std::abs(z - std::abs(pz)), pf.road_z_max);
		rnd += m * 3;
		cost += m * 3;
		const bool road = (td & 0x30) == 0x10;
		const int32_t bad8 = pf.road_bad_path / 8;  // (x + ((x >> 31) & 7)) >> 3
		if (!friendly) {
			if (road) {
				if (pz < 0) cost -= rnd;
				else cost += pf.road_bad_path * 4;
				N.z = int16_t(-z);
			} else {
				cost += pf.road_bad_path * 4;
			}
		} else if (road) {
			if (pz < 0) cost = (cost - rnd) / 2;
			else cost += bad8;
			N.z = int16_t(-z);
		} else {
			cost += bad8;
		}
	}
	if (!was_seen(g, tx >> 1, ty >> 1, src_pl)) cost *= 2;
	if (d & 1) cost = (cost * 7) / 5;
	return cost;
}

}  // namespace

namespace {

// PathFinder::find_road @688a40 and astar_caravan_road @685990. `rec` null: a building road (4-connected, one 3200-expansion
// budget that fails); a caravan's record: 8-connected, the search is suspended in the record when the budget is spent and
// `restore` (find_road_restore @685950) resumes it. `stack` is the caravan's road (empty on entry) or the building's.
int32_t find_road_core(Game& g, std::vector<PathData>& stack, int32_t src_o, int32_t src_pl, int32_t dst_o, int32_t dst_pl,
                       CaravanRec* rec, bool restore) {
	PathFinder& pf = g.pathfinder;
	World& w = g.world;
	const Object* so = g.object(src_pl, src_o);
	const Object* dobj = g.object(dst_pl, dst_o);
	if (!so || !dobj || !so->ptype || !dobj->ptype) {
		stack.clear();
		return -1;
	}
	// endpoint tiles: a city -> its centre tile; otherwise the far footprint corner (tile_corner + size - 1)
	auto end_tile = [&](const Object& ob, int32_t* tx, int32_t* ty) {
		if (is_city_type(*ob.ptype)) {
			*tx = ob.x / kTile;
			*ty = ob.y / kTile;
		} else {
			const Footprint f = footprint(*ob.ptype, ob.x, ob.y);
			*tx = f.x0 - 1 + ob.ptype->x_size;
			*ty = f.y0 - 1 + ob.ptype->y_size;
		}
	};
	int32_t sx, sy, gx, gy;
	end_tile(*so, &sx, &sy);
	end_tile(*dobj, &gx, &gy);
	if (!restore &&
	    (gx < 0 || gy < 0 || gx >= w.tile_xs || gy >= w.tile_ys || sx < 0 || sy < 0 || sx >= w.tile_xs || sy >= w.tile_ys || (sx == gx && sy == gy))) {
		stack.clear();
		return -1;
	}
	pf.avoid_land = 0;
	pf.avoid_sea = w.w(sx >> 2, sy >> 2).region == w.w(gx >> 2, gy >> 2).region ? 1 : 0;
	const Unit* cu = rec ? g.unit(rec->who, rec->o) : nullptr;
	pf.can_transport = (rec && cu && unit_can_transport(*cu)) ? 1 : 0;
	pf.saving = 0;

	// ---- astar_caravan_road
	int32_t S_x = sx * kTile + 0x60, S_y = sy * kTile + 0x60, G_x = gx * kTile + 0x60, G_y = gy * kTile + 0x60;
	const Ends e{src_o, src_pl, dst_o, dst_pl};
	const bool resume = rec && rec->making_road != 0 && rec->trees;
	std::shared_ptr<SavedSearch> sp;
	int32_t dir0;
	if (!resume) {
		sp = std::make_shared<SavedSearch>();
		PathNode s;
		s.x = S_x;
		s.y = S_y;
		s.metric = uint32_t(w.tile_xs * (S_y / kTile) + (S_x / kTile));
		s.length = 0;
		s.timeout = 1;
		s.estimate = s.value = (vector_dist(std::abs(S_x - G_x), std::abs(S_y - G_y)) * 0x3c) / 0xc0;
		s.z_val = int16_t(find_tcoord_z(g, S_x / kTile, S_y / kTile, 0));
		sp->insert_open(sp->new_node(s));
		const int32_t adx = std::abs(S_x - G_x), ady = std::abs(S_y - G_y);
		if (ady < adx) dir0 = (G_x < S_x ? 1 : 0) * 4 + 3;
		else dir0 = G_y < S_y ? 1 : 5;
		if (rec) rec->making_road = 0;
	} else {
		sp = std::move(rec->trees);
		rec->trees.reset();
		dir0 = rec->search_dir0;
		G_x = rec->search_gx;
		G_y = rec->search_gy;
		rec->making_road = 0;
	}
	SavedSearch& L = *sp;
	int32_t count = 0;
	PathNode* goal = nullptr;
	bool sliced = false;
	while (true) {
		if (L.open.empty()) break;
		PathNode* P = L.first_open_node();
		if (std::abs(P->x - G_x) + std::abs(P->y - G_y) < 1) {
			if (count < 0xc80) {
				goal = P;
				break;
			}
			L.insert_open(P);
			sliced = true;
			break;
		}
		if (count > 0xc7f) {  // the budget is spent: a caravan suspends the search, a building road fails
			L.insert_open(P);
			sliced = true;
			break;
		}
		const int32_t pz = P->z_val;
		int32_t k = dir0 + 1;
		for (int32_t it = 0; it < 8; it++, k++) {
			const int32_t d = k < 9 ? k : k - 8;
			if (!rec && (d & 1)) continue;  // orthoOnly
			PathNode N;
			N.x = move_x[d] * kTile + P->x;
			N.y = move_y[d] * kTile + P->y;
			N.metric = uint32_t(int32_t(P->metric) + w.tile_xs * move_y[d] + move_x[d]);
			if (!valid_roadcoord(g, pf, N.x, N.y, P->x, P->y, e)) continue;
			count++;
			RoadNode nr;
			nr.x = N.x, nr.y = N.y;
			int32_t c = calc_road_cost(g, pf, nr, P->x, P->y, pz, src_pl, dst_pl, d);
			if (N.x == G_x && N.y == G_y) c /= 2;
			N.length = P->length + c;
			if (L.closed.count(N.metric)) continue;
			SavedSearch::OpenKey qk;
			if (PathNode* q = L.find_open(N.metric, &qk)) {
				if (q->length <= N.length) continue;
				L.open.erase(qk);
				L.refs.erase(N.metric);
			}
			N.parent = P;
			if (!rec) {
				N.estimate = (vector_dist(std::abs(N.x - G_x), std::abs(N.y - G_y)) * 0x3c) / 0x180;
			} else {
				// asm @685fd9: vector_dist(N.x - gx, -gy) with no abs (an original quirk), * 40 / 16 truncating toward zero
				const int32_t v = vector_dist(N.x - G_x, -G_y) * 40;
				N.estimate = (v + ((v >> 31) & 0xf)) >> 4;
			}
			N.value = N.estimate + N.length;
			N.timeout = P->timeout + 1;
			N.z_val = nr.z;
			L.insert_open(L.new_node(N));
		}
		L.closed[P->metric] = P;
	}
	kill_lists(pf);  // find_road @688a40 empties the pathfinder's own trees after every search
	if (sliced && rec) {
		rec->making_road = 1;
		rec->trees = sp;
		rec->search_dir0 = dir0;
		rec->search_gx = G_x;
		rec->search_gy = G_y;
		rec->search_count = count;
		return -1;
	}
	if (sliced) {
		stack.clear();
		return -1;
	}
	if (!goal) return int32_t(stack.size());  // the open list ran dry: astar returns 0, find_road the (empty) stack's length
	// emission: from the goal's parent toward the start, excluding the start; tiles that are already BLOCKED are skipped
	PathNode* n = goal->parent;
	if (n && n->parent) {
		do {
			const int32_t tx = n->x / kTile, ty = n->y / kTile;
			if (!(w.t(tx, ty) & 0x4000)) {
				PathData pd;
				pd.x = n->x;
				pd.y = n->y;
				pd.tol = 0x60;
				pd.flags = 0;
				stack.push_back(pd);
			}
			n = n->parent;
		} while (n->parent);
	}
	return int32_t(stack.size());
}

}  // namespace

int32_t find_road(Game& g, std::vector<PathData>& stack, int32_t src_o, int32_t src_pl, int32_t dst_o, int32_t dst_pl) {
	stack.clear();
	return find_road_core(g, stack, src_o, src_pl, dst_o, dst_pl, nullptr, false);
}

int32_t find_road_caravan(Game& g, CaravanRec& rec, int32_t src_o, int32_t src_pl, int32_t dst_o, int32_t dst_pl, bool restore) {
	std::vector<PathData> tmp;
	tmp.swap(rec.road);  // (cleared by Caravan::clear_road before the search)
	tmp.clear();
	const int32_t r = find_road_core(g, tmp, src_o, src_pl, dst_o, dst_pl, &rec, restore);
	// the emission pushes one node per Stack::push: replay the allocation growth (increase_size(-1) doubles, 0 -> 1)
	rec.road.clear();
	for (const PathData& pd : tmp) {
		if (rec.road_size <= int32_t(rec.road.size())) rec.road_size += rec.road_size != 0 ? rec.road_size : 1;
		rec.road.push_back(pd);
	}
	return r;
}

void place_roads_regen(Game& g, Build& b, bool add, Regen regen) {
	static const bool off = std::getenv("RON_NO_ROAD_SEARCH") != nullptr;  // A/B switch for digest comparisons
	if (off) return;
	if (!add || !b.ptype || is_city_type(*b.ptype) || b.city < 0) return;
	if (!connects_to_roads(*b.ptype) && (b.ptype->build_flags & kBuildFlagG)) return;
	if (regen == Regen::Total) {
		if (b.alive()) b.build_masks |= bmask::kMakeRoad;
		return;
	}
	if (regen != Regen::Force || !b.alive()) return;
	auto& cl = g.cities[size_t(b.who)];
	if (size_t(b.city) >= cl.size()) return;
	std::vector<PathData> road;
	find_road(g, road, b.o, b.who, cl[size_t(b.city)].o, b.who);
	// laid from the stack top (the node next to the building) down
	while (!road.empty()) {
		const PathData pd = road.back();
		road.pop_back();
		const int32_t tx = pd.x / kTile, ty = pd.y / kTile;
		if (g.world.in_t(tx, ty) && !(g.world.t(tx, ty) & 0x4000)) g.world.set_road_at(tx, ty, true);
	}
}

}  // namespace ron::sim
