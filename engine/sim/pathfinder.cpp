// PathFinder (pathfinder.obj): find_wpath / find_tpath / find_upath, astar_path, calc_cost, the validity predicates
// and the time-sliced U-level search saved in the unit. docs/movement-spec.md sections 3.3, 6, 7, 8;
// re/tools/astar-ref.mjs is the reference model of astar_path, its path emission and the find_upath post pass.
//
// Integer arithmetic follows the original exactly: C truncating division, arithmetic shifts, floor coordinate
// conversions (div_3_table), 32-bit wrapping sums. No game_random draws except the U-level retry draws in astar_path.
#include "sim/pathfinder.hpp"
#include "sim/pathfinder_search.hpp"

#include <deque>
#include <utility>

#include "sim/compass.hpp"
#include "sim/game.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

int32_t get_target(const Game& g, int32_t who);  // LeaderData::get_target @6da000 (sim/ai.hpp)

// ---------------------------------------------------------------- containers

void free_saved_search(SavedSearch* s) { delete s; }

namespace {

constexpr int32_t kBlocked = 0x7fffffff;

inline int32_t add32(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
inline int32_t mul32(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }

const Type& utype(const Game& g, const Unit& u) { return u.ptype ? *u.ptype : g.type(u.type); }

// The pathfinder's own lists (created on first use) and the blocklist pointer into them.
SavedSearch& own_lists(PathFinder& pf) {
	if (!pf.lists) pf.lists.reset(new SavedSearch());
	pf.blocklist = &pf.lists->blocks;
	return *pf.lists;
}

Unit& pathing_unit(Game& g) { return *g.unit(g.pathfinder.pathing_unit_who, g.pathfinder.pathing_unit_o); }

void set_pathing_unit(PathFinder& pf, int32_t who, int32_t o) {
	pf.pathing_unit_who = who;
	pf.pathing_unit_o = o;
}

// Stack<PathData>::pop: the original clamps length to 1 first and reads stale list[0] on an empty stack; callers never
// pop an empty stack, so an empty one yields a zero entry here.
PathData pop(std::vector<PathData>& s) {
	if (s.empty()) return PathData{};
	PathData d = s.back();
	s.pop_back();
	return d;
}
int32_t push(std::vector<PathData>& s, const PathData& d) {
	s.push_back(d);
	return int32_t(s.size());
}

// ObjectData::is_worker @46fa10: TypeData.type in 50..53.
bool is_worker(const Game& g, const Unit& u) {
	const int32_t t = utype(g, u).type;
	return t == 0x32 || t == 0x33 || t == 0x34 || t == 0x35;
}
// UnitData::is_attacking @60a5b0: the current order's is_attack (vtable +0x18).
bool is_attacking(const Unit& u) {
	const Order* o = u.front();
	return o && order_is_attack(o->type);
}
// UnitData::is_fleeing @46efa0: the current order is FLEE_TO.
bool is_fleeing(const Unit& u) { return u.order_type() == kFleeTo; }
// The current order is a move order (orders list head, vtable +0x14 is_move).
bool front_is_move(const Unit& u) {
	const Order* o = u.front();
	return o && order_is_move(o->type);
}

// is_blocked_at on the linear TData index, as the original reads it: the corridor test of calc_cost probes up to 3
// tiles outside the current one, and a column off the left/right edge reads the neighbouring row's tile.
// TODO(movement-spec 8.1): an index before row 0 or after the last row reads memory outside TData in the original;
// treated as not blocked here.
bool blocked_lin(const World& w, int32_t tx, int32_t ty, bool iroquois) {
	const int64_t i = int64_t(ty) * w.tile_xs + tx;
	if (i < 0 || i >= int64_t(w.tdata.size())) return false;
	const uint16_t m = w.tdata[size_t(i)];
	return (m & tmask::kBlocked) && !(iroquois && (m & tmask::kMedium) == tmask::kTree);
}

int32_t heuristic(int32_t step, int32_t dx, int32_t dy) {
	const int32_t h = vector_dist(dx, dy);
	return step == 0x30 ? mul32(h, 10) : mul32(h, 60) / step;
}

// The game_random retry draw of a failed or capped U-level search: rnd(3) + 6.
void set_retry(Game& g, Order& m) { m.retry = g.rnd_draw(3) + 6; }

inline void add_safe(Unit& u) { u.safe = int8_t(uint8_t(u.safe) + 30); }  // char += 30, wraps

// The emission at the end of astar_path (section 7.5, step 3). `e` = the node to emit from.
int32_t emit_path(Game& g, Unit& u, std::vector<PathData>& stack, PathNode* e, int32_t step, bool reached,
                  int32_t ox, int32_t oy) {
	PathFinder& pf = g.pathfinder;
	const World& w = g.world;
	if (!e->parent) return 1;  // start = goal
	PathNode* cur = e->parent;  // W level, not reached, no transport: the goal cell is already on the stack
	if (step != 0x300 || reached || e->transport) {
		cur = e;
		if (step == 0xc0) {
			// Mark building-footprint nodes from e to the start's child. The emission then starts at the LAST marked
			// node (the one closest to the start): the asm @684aa8 moves the emit pointer (EBX) to every building node.
			PathNode* q = e;
			for (;;) {
				const uint16_t m = w.t(tcoord_of(q->x), tcoord_of(q->y));
				if ((m & tmask::kBlocked) && (m & 3) == 3) {
					q->building = 1;
					cur = q;
				}
				q = q->parent;
				if (!q->parent) break;
			}
		}
	}
	const bool has_move = front_is_move(u);
	for (PathNode* c = cur; c; c = c->parent) {
		if (!c->parent && step != 0x30) break;  // the start node is emitted only at U level
		int32_t x = c->x, y = c->y, tol;
		if (step == 0x300) {
			tol = 0x180;
			if (has_move) {
				x += ox - 0x180;
				y += oy - 0x180;
			}
		} else if (step == 0x30) {
			tol = 0;
		} else {
			if (has_move) {
				x += ox % 0xc0 - 0x60;
				y += oy % 0xc0 - 0x60;
			}
			tol = (!pf.anti_unit && unit_can_transport(u)) ? 0 : 0x60;
		}
		uint32_t flags = pf.anti_unit ? 2u : (step == 0x30 ? 2u : 0u);
		if (c->building) flags |= pathflag::kBuilding;
		// Stack<PathData>::peek on an empty stack is never reached (the goal entry is below); treat it as different.
		if (stack.empty() || x != stack.back().x || y != stack.back().y) {
			if (c->transport) {
				flags |= pathflag::kTransport;
				tol = 0;
			}
			stack.push_back(PathData{x, y, tol, flags});
		}
	}
	// Stack<PathData>::push grows the allocation by `increment` whenever size <= length: the emission may overshoot what the
	// straightening leaves, and the allocation stays.
	if (&stack == &u.path)
		while (u.path_size < int32_t(stack.size()) && u.path_inc > 0) u.path_size += u.path_inc;
	return 1;
}

// The find_upath straightening pass (findUpathPost in astar-ref.mjs), stack length > 3.
void straighten(std::vector<PathData>& stack, int32_t unit_x, int32_t unit_y) {
	if (stack.size() <= 3) return;
	std::vector<PathData> keep;
	PathData w0 = pop(stack);
	if (w0.x == unit_x && w0.y == unit_y && !(w0.flags & pathflag::kFinal)) w0 = pop(stack);
	keep.push_back(w0);
	if ((w0.flags & 2) && stack.size() > 2) {
		int32_t ax = w0.x, ay = w0.y;
		PathData b = pop(stack);
		PathData c = stack.back();
		while ((b.flags & 2) && (c.flags & 2) && stack.size() > 1) {
			bool keep_b = true, advance = true;
			if (b.flags & pathflag::kTransport) {
				keep_b = true;
			} else if (ax - b.x == b.x - c.x && ay - b.y == b.y - c.y) {
				keep_b = false;  // collinear: drop B, A := B
			} else if (iabs(ax - c.x) == 0x30 && iabs(ay - c.y) == 0x30) {
				keep_b = false;  // corner between diagonal neighbours: drop B, keep A
				advance = false;
			}
			if (keep_b) keep.push_back(b);
			if (advance) {
				ax = b.x;
				ay = b.y;
			}
			b = pop(stack);
			c = stack.back();
		}
		stack.push_back(b);
	}
	while (!keep.empty()) {
		stack.push_back(keep.back());
		keep.pop_back();
	}
}

// PathFinder::find_upath @682f30 (the body behind both wrappers).
int32_t find_upath_body(Game& g, std::vector<PathData>& stack, int32_t x, int32_t y, int32_t who, int32_t o,
                        bool fast) {
	PathFinder& pf = g.pathfinder;
	const World& w = g.world;
	set_pathing_unit(pf, who, o);
	Unit& u = pathing_unit(g);
	pf.scouting = 0;
	pf.sx = tcoord_of(x);
	pf.sy = tcoord_of(y);
	pf.offx = pf.offy = 0;

	if (pf.saving == 0) {
		const int32_t ux = ucoord_of(x), uy = ucoord_of(y);
		PathData gl = pop(stack);
		int32_t gux = ucoord_of(gl.x), guy = ucoord_of(gl.y);
		if (gux < 0 || guy < 0 || gux >= w.xs * 16 || guy >= w.ys * 16) {
			stack.clear();
			return -1;
		}
		if (ux == gux && uy == guy) {
			gl.tol = 0;
			return push(stack, gl);
		}
		// Pull-in for ground and sea units that cannot transport. Note: these early returns skip kill_lists (the
		// valid cache and block snapshot filled here stay for the next U-level search), as in the original.
		if (utype(g, u).domain < 2 && !unit_can_transport(u)) {
			for (;;) {
				if (w.get_tregion(gux >> 2, guy >> 2) == w.get_tregion(ux >> 2, uy >> 2) &&
				    valid_ucoord(g, u, gl.x, gl.y, uint32_t(mul32(mul32(w.xs, guy), 16) + gux)))
					break;
				if (iabs(x - gl.x) < 0x18 && iabs(y - gl.y) < 0x18) {
					// asm @68322e: not final -> 0; final -> push it (keeping its tolerance) and return
					if (!(gl.flags & pathflag::kFinal)) return 0;
					return push(stack, gl);
				}
				const uint32_t a = find_angle(x - gl.x, y - gl.y);
				gl.x += sin_a(a, 0x18);
				gl.y -= cos_a(a, 0x18);
				gux = ucoord_of(gl.x);
				guy = ucoord_of(gl.y);
				if (gux == ux && guy == uy) return push(stack, gl);
			}
		}
		if (iabs(ux - gux) + iabs(uy - guy) < 2) {
			gl.tol = 0;
			return push(stack, gl);
		}
		push(stack, gl);
		push(stack, PathData{gux * 0x30 + 0x18, guy * 0x30 + 0x18, gl.tol, 0});
		push(stack, PathData{ux * 0x30 + 0x18, uy * 0x30 + 0x18, 0, 0});
	}

	pf.offx = pf.offy = 0;
	pf.anti_unit = 1;
	int32_t r = astar_path(g, stack, 0x30, fast);
	pf.iroquois = 0;
	pf.anti_unit = 0;
	if (r < 1) {
		if (pf.saving == 0) {
			r = 0;
			if (!stack.empty() && !(stack.back().flags & pathflag::kFinal)) stack.pop_back();
			Order* m = update_order(u);
			if (!(m && order_is_move(m->type) && m->retry != 0)) kill_current_order(g, u, false);
		}
		// saving != 0: a time-sliced search returns -1 with its trees in the unit
	} else {
		straighten(stack, u.x, u.y);
		r = int32_t(stack.size());
	}
	kill_lists(pf);
	return r;
}

int32_t repaths_of(const Game& g, int32_t who) {
	// TODO(movement-spec 6): who 8/9 index past GameDaemon.repaths[8] in the original (the following fields).
	return (who >= 0 && who < int32_t(g.daemon.repaths.size())) ? g.daemon.repaths[size_t(who)] : 0;
}

int32_t search_limit(const Game& g, int32_t who, int32_t num) {
	const int32_t r = repaths_of(g, who);
	int32_t sq = mul32(r, r);
	if (sq < 1) sq = 1;
	return num / sq;
}

}  // namespace

// ---------------------------------------------------------------- lists

void kill_lists(PathFinder& pf) {
	if (pf.lists) pf.lists->clear();
}

void clear_partial_path(Unit& u) {
	if (!u.openlist) return;
	u.openlist.reset();
	// +0x118, +0x11c, +0x128, +0x12c, +0x140, +0x144, +0x148, +0x13c, +0x138 (start_dist and valid_hit are kept)
	u.temp_x = u.temp_y = 0;
	u.tol = 0;
	u.offset = 0;
	u.endx = u.endy = 0;
	u.traversed = 0;
	u.avoid_sea = u.avoid_land = 0;
}

// ---------------------------------------------------------------- validity

bool valid_ucoord(Game& g, Unit& u, int32_t x, int32_t y, uint32_t key) {
	PathFinder& pf = g.pathfinder;
	const World& w = g.world;
	if (x < 0 || y < 0 || x >= w.tile_xs * 0xc0 || y >= w.tile_ys * 0xc0) return false;
	SavedSearch& L = own_lists(pf);
	const auto it = L.valid.find(key);
	if (it != L.valid.end()) {
		pf.valid_hit++;
		return it->second != 0;
	}
	int32_t v = 0;
	if (unit_invalid_loc(g, u, tcoord_of(x), tcoord_of(y), false, true, false, true, false) == 0) {
		// The pathing collision probe (CollCheck through the search's block snapshot). The original's 5th argument
		// is an unset register (ECX); 0 here.
		v = detect_unit_collision(g, u, x, y, true, true, 0, true, false) == 0 ? 1 : 0;
	}
	L.valid[key] = v;
	return v != 0;
}

bool valid_tcoord(Game& g, const Unit& u, int32_t tx, int32_t ty) {
	return unit_invalid_loc(g, u, tx, ty, false, true, true, true, false) == 0;
}

bool valid_wcoord(Game& g, const Unit& u, int32_t x, int32_t y, int32_t depth, int32_t gx, int32_t gy) {
	if (x == u.avoid_x && y == u.avoid_y) return false;
	int32_t r = unit_invalid_loc(g, u, tcoord_of(x), tcoord_of(y), true, depth > 1, false, true, false);
	if (r == 3 && wcoord_of(x) == wcoord_of(gx) && wcoord_of(y) == wcoord_of(gy)) r = 0;  // water hazard at the goal
	return r == 0;
}

// ---------------------------------------------------------------- calc_cost

int32_t calc_cost(Game& g, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t d, int32_t step, int32_t depth,
                  int32_t* transport) {
	PathFinder& pf = g.pathfinder;
	const World& w = g.world;
	Unit& u = pathing_unit(g);
	const int32_t P = uint8_t(u.who);
	*transport = 0;
	const int32_t tx1 = tcoord_of(x1), ty1 = tcoord_of(y1), tx2 = tcoord_of(x2), ty2 = tcoord_of(y2);
	const int32_t wx2 = wcoord_of(x2), wy2 = wcoord_of(y2);
	int32_t base = 0x100, extra = 0;

	auto danger = [&]() -> int32_t {
		const int64_t i = int64_t(P) * w.reg_size + int64_t(regcell_of(y2)) * w.reg_xs + regcell_of(x2);
		return (i >= 0 && i < int64_t(w.danger.size())) ? w.danger[size_t(i)] : 0;
	};

	if (step != 0x30) {
		bool team = true;  // the W-level team-style terms run for both W branches
		if (step == 0x300 && !was_really_seen(g, floor_div3(x2 >> 7), floor_div3(y2 >> 7), P)) {
			base = 0x124;
			if (pf.army && (w.w(wx2, wy2).flags & 0x200)) base = 0x2480;
			if (pf.scouting) base = 8;
			extra = pf.scouting ? 0 : 8;
		} else {
			const WData& wd = w.w(wx2, wy2);
			const int32_t owner = wd.who;
			if (step == 0xc0) {
				const uint16_t m = w.t(tx2, ty2);
				base = (m & tmask::kBadPath) ? 0x400 : 0x100;
				int32_t k = pf.no_danger ? 0 : danger();
				k = add32(k, ((m & 3) == 3 && (m & tmask::kBlocked)) ? 4000 : 0);  // building_danger @685900
				int32_t v = owner == P ? k - 4 : k;
				if (owner >= 0 && is_enemy(g, P, owner)) v += 4;
				extra = v < 0 ? 0 : v;
			} else {
				const int32_t b = pf.scouting ? 0x400 : 0x100;
				int32_t k = pf.no_danger ? 0 : danger() / 8;
				int32_t kk;
				if (!World::is_ocean(wd)) {
					if (owner == P) k -= 4;
					else if (owner >= 0 && is_enemy(g, P, owner)) k += 4;
					kk = pf.avoid_land ? k + 200 : k;
				} else {
					kk = pf.avoid_sea ? k + 200 : k;
				}
				const WData& wv = w.w(tx2 >> 2, ty2 >> 2);
				const int32_t v = pf.iroquois ? int32_t(wv.solid) : int32_t(wv.blocked);
				kk = add32(kk, mul32(v, 20));
				if (v >= 13) kk = add32(kk, 100000);
				base = b;
				if (pf.army) {
					if (v >= 5) kk = add32(kk, 10000);
					if (wv.flags & 0x200) base = b << 5;
				}
				extra = kk < 0 ? 0 : kk;
				// Corridor test on the source tile: a W step must not squeeze between blocked tiles.
				if (depth < 10 && (v != 0 || pf.iroquois)) {
					const bool iq = pf.iroquois != 0;
					auto B = [&](int32_t tx, int32_t ty) { return blocked_lin(w, tx, ty, iq); };
					const int32_t tx = tx1, ty = ty1;
					bool blk = false;
					switch (d) {
					case 1: blk = B(tx - 2, ty - 2) || B(tx - 3, ty - 3); break;
					case 2: blk = B(tx - 2, ty - 3) && B(tx - 1, ty - 3) && B(tx, ty - 3) && B(tx + 1, ty - 3); break;
					case 3: blk = B(tx + 1, ty - 2) || B(tx + 2, ty - 3); break;
					case 4: blk = B(tx + 2, ty - 2) && B(tx + 2, ty - 1) && B(tx + 2, ty) && B(tx + 2, ty + 1); break;
					case 5: blk = B(tx + 1, ty + 1) || B(tx + 2, ty + 2); break;
					case 6: blk = B(tx - 2, ty + 2) && B(tx - 1, ty + 2) && B(tx, ty + 2) && B(tx + 1, ty + 2); break;
					case 7: blk = B(tx - 2, ty + 1) || B(tx - 3, ty + 2); break;
					case 8: blk = B(tx - 3, ty - 2) && B(tx - 3, ty - 1) && B(tx - 3, ty) && B(tx - 3, ty + 1); break;
					default: break;
					}
					if (blk) return kBlocked;
				}
			}
			if (is_fleeing(u)) extra = mul32(extra, 3);
			{
				// PathFinder::calc_cost @684e50 (decompile lines 392 - 405): while the rush rule holds (Game::current_age < rushrules, and for the
				// timed rules (>= 9) before frame minutes * 900), a cell of a leader who is not an ally costs 500 more.
				const int32_t rr = g.info.rush_rules;
				if (rr != 0 && owner >= 0 && !is_ally(g, P, owner)) {
					int32_t cur = 0;
					for (int32_t j = 0; j < 8; j++) {
						if ((g.leaders[size_t(j)].flags & leaderflag::kValid) && g.leaders[size_t(j)].age > cur) cur = g.leaders[size_t(j)].age;
					}
					bool rush = cur < rr;
					if (rush && rr >= 9) {
						const CategoryList* cl = g.rules ? g.rules->category_list("rush_rules") : nullptr;
						const int32_t mins = cl && size_t(rr) < cl->items.size() ? cl->items[size_t(rr)].data[0] : 0;
						rush = g.frame < mins * 900;
					}
					if (rush) extra = add32(extra, 500);
				}
			}
			if (step != 0x300) team = false;
		}
		if (team && (pf.army || pf.worker)) {
			// PathFinder::calc_cost (decompile lines 407 - 426): team styles 0, 8 and 11 add 5000 for a cell of a leader at peace with the
			// pathing unit's owner (LeaderData::is_peace: neither at war nor allied both ways), style 2 for a cell of any leader that is neither
			// the owner's target nor the owner itself.
			const uint8_t ts = g.info.team_style;
			const int32_t co = w.w(wx2, wy2).who;
			if ((ts == 0 || ts == 8 || ts == 11) && co >= 0 && !is_ally(g, P, co) && !is_enemy(g, P, co)) extra = add32(extra, 5000);
			if (ts == 2 && co >= 0 && co != get_target(g, P) && co != P) extra = add32(extra, 5000);
		}
	}

	// Transport terms (all steps).
	int32_t nt = needs_transport(g, tx1, ty1, tx2, ty2);
	const bool C = unit_can_transport(u);
	if (nt < 1 || !C || depth < 2) {
		if (depth == 1) {
			nt = needs_transport(g, pf.sx, pf.sy, tx2, ty2);  // from the unit's start tile
			if (nt > 0 && C) {
				if (pf.avoid_sea == 2) return kBlocked;
				if (nt != 1) extra = add32(extra, (pf.avoid_sea == 0 && pf.avoid_land == 0) ? 250 : 1000);
				*transport = 1;
			}
		}
	} else {
		if (pf.avoid_sea == 2) return kBlocked;
		if (nt != 1) extra = add32(extra, (pf.avoid_sea == 0 && pf.avoid_land == 0) ? 500 : 2000);
		if (step == 0x30) extra = int32_t(uint32_t(extra) << 2);
		*transport = 1;
	}
	if (nt <= 0 && step == 0x300 && (w.w(wx2, wy2).flags & wflag::kHalfLand)) base *= 3;
	return add32(add32((base * 32) / 256, extra), (d & 1) * 8);
}

// ---------------------------------------------------------------- astar_path

int32_t astar_path(Game& g, std::vector<PathData>& stack, int32_t step, bool fast) {
	PathFinder& pf = g.pathfinder;
	const World& w = g.world;
	Unit& u = pathing_unit(g);
	const Type& ut = utype(g, u);

	pf.iroquois = int32_t(u.unit_masks2 & umask2::kMoveInForest);
	int32_t cap = 50, dirinc = 1, mult = 1, W;
	if (step == 0x30) {
		cap = 500;
		mult = (ut.new_block_radius + 1) / 2;
		if (mult < 1) mult = 1;
		W = w.xs << 4;
		dirinc = fast ? 2 : 1;
	} else if (step == 0xc0) {
		W = w.tile_xs;
	} else {
		W = w.xs;
	}
	const int32_t stride = mult * step;

	{
		const Order* act = get_action(u);
		const OrderType ot = u.order_type();
		pf.no_danger = ((act == nullptr || act->type != kAttack) && uint8_t(u.who) < 8 && ot != kAttackTo &&
		                ot != kGroupAttackTo)
		                   ? 0
		                   : 1;
	}
	int32_t ox = 0, oy = 0;  // MoveOrder.off_x/off_y of a move order
	if (front_is_move(u)) {
		if (const Order* m = update_order(u)) {
			ox = m->off_x;
			oy = m->off_y;
		}
	}

	int32_t gx, gy, tol, dir0, start_dist, T0 = 0;
	SavedSearch* L;
	if (pf.saving) {
		// Resume: free the pathfinder's own trees and adopt the unit's (section 3.3).
		pf.lists = std::move(u.openlist);
		L = &own_lists(pf);
		pf.valid_hit = u.valid_hit;
		dir0 = u.offset;
		gx = u.endx;
		gy = u.endy;
		tol = u.tol;
		pf.avoid_land = u.avoid_land;
		pf.avoid_sea = u.avoid_sea;
		T0 = u.traversed;
		start_dist = u.start_dist;
	} else {
		L = &own_lists(pf);
		pf.valid_hit = 0;
		const PathData S = pop(stack);
		const PathData G = pop(stack);
		gx = G.x;
		gy = G.y;
		tol = stack.empty() ? 0 : stack.back().tol;

		PathNode s;
		s.x = S.x;
		s.y = S.y;
		if (step == 0x300) s.metric = uint32_t(wcoord_of(S.x) + mul32(wcoord_of(S.y), w.xs));
		else if (step == 0xc0) s.metric = uint32_t(tcoord_of(S.x) + mul32(tcoord_of(S.y), w.tile_xs));
		else s.metric = uint32_t(ucoord_of(S.x) + mul32(mul32(ucoord_of(S.y), w.xs), 16));
		s.estimate = s.value = heuristic(step, S.x - gx, S.y - gy);
		L->insert_open(L->new_node(s));

		// Avoid flags.
		bool same;
		if (step == 0x300) same = w.w(wcoord_of(S.x), wcoord_of(S.y)).region == w.w(wcoord_of(gx), wcoord_of(gy)).region;
		else same = w.get_tregion(tcoord_of(gx), tcoord_of(gy)) == w.get_tregion(tcoord_of(S.x), tcoord_of(S.y));
		if (same) {
			const bool water = step == 0x300 ? w.is_ocean(wcoord_of(S.x), wcoord_of(S.y))
			                                 : (w.t(tcoord_of(S.x), tcoord_of(S.y)) & tmask::kMedium) == tmask::kWater;
			if (!(ut.unit_flags & 0x10) || !(u.unit_masks & umask::kAiControl)) {
				if (water) {
					pf.avoid_sea = 0;
					pf.avoid_land = 1;
				} else {
					pf.avoid_sea = 1;
					pf.avoid_land = 0;
					const Order* act = get_action(u);
					if (act && act->type == kAttack) pf.avoid_sea = 2;
				}
			} else {
				pf.avoid_sea = pf.avoid_land = 0;
			}
		} else {
			pf.avoid_sea = pf.avoid_land = 0;
			const Order* o = u.front();
			if (o && (o->flags & orderflag::kDisembark)) pf.avoid_land = 1;
		}

		const int32_t adx = iabs(S.x - gx), ady = iabs(S.y - gy);
		start_dist = adx + ady;
		if (ady < adx) dir0 = gx < S.x ? 7 : 3;
		else dir0 = S.y <= gy ? 5 : 1;
	}

	if (!L->open.empty()) {
		const int32_t goal_tol = tol / 2 + stride;
		int32_t E = 0;  // expansions of this call
		do {
			PathNode* n = L->first_open_node();
			const int32_t man = iabs(n->x - gx) + iabs(n->y - gy);
			const bool slice_hit = pf.anti_unit && pf.limit < E;
			if (man <= goal_tol || cap * 64 <= T0 + E || slice_hit) {
				if (slice_hit && goal_tol < man && !fast) {
					// Time slice: put n back and move the search into the unit.
					L->insert_open(n);
					pf.saving = 1;
					u.openlist = std::move(pf.lists);
					own_lists(pf);
					u.valid_hit = pf.valid_hit;
					u.tol = tol;
					u.offset = dir0;
					u.endx = gx;
					u.endy = gy;
					u.start_dist = start_dist;
					u.traversed = T0 + E;
					u.avoid_land = pf.avoid_land;
					u.avoid_sea = pf.avoid_sea;
					return -1;
				}
				pf.saving = 0;
				bool reached = false;
				PathNode* e = n;
				if (!(T0 + E < cap * 64 || fast)) {
					if (step == 0xc0) {
						if (pf.anti_unit) return 0;
					} else if (step == 0x30) {
						Order* m = update_order(u);
						if (m && order_is_move(m->type)) set_retry(g, *m);
						add_safe(u);
						return 0;
					}
					// Closest-node fallback: drain the open list in pop order; only a strictly closer node replaces.
					int32_t bd = vector_dist(e->x - gx, e->y - gy);
					while (!L->open.empty()) {
						PathNode* q = L->first_open_node();
						const int32_t qd = vector_dist(q->x - gx, q->y - gy);
						if (qd < bd) {
							e = q;
							bd = qd;
						}
					}
					if (needs_transport(g, tcoord_of(e->x), tcoord_of(e->y), tcoord_of(gx), tcoord_of(gy)) != 0 &&
					    unit_can_transport(u) && !stack.empty())
						stack.back().flags |= pathflag::kTransport;
					reached = true;
				}
				return emit_path(g, u, stack, e, step, reached, ox, oy);
			}

			int32_t dir = dir0 + 1;
			for (int32_t cnt = 1; cnt < 9; cnt += dirinc, dir += dirinc) {
				const int32_t d = dir < 9 ? dir : dir - 8;
				const int32_t mx = move_x[d], my = move_y[d];
				PathNode p;
				p.x = mx * stride + n->x;
				p.y = my * stride + n->y;
				p.metric = uint32_t(mx) + n->metric + uint32_t(mul32(my, W));  // one cell per step, NOT scaled by mult

				bool ok;
				if (step == 0x300) {
					if (n->timeout < 2) ok = valid_wcoord(g, u, p.x + ox - 0x180, p.y + oy - 0x180, n->timeout, gx, gy);
					else ok = valid_wcoord(g, u, p.x, p.y, n->timeout, gx, gy);
				} else if (step == 0xc0) {
					ok = unit_invalid_loc(g, u, tcoord_of(p.x), tcoord_of(p.y), false, true, true, true, false) == 0;
				} else if (mult < 2 || (d & 1) == 0) {
					ok = valid_ucoord(g, u, p.x, p.y, p.metric);
				} else {
					// Large unit, diagonal: every intermediate cell. The cache key mixes world units into the cell
					// index (k*step*dx + k*step*dy*W): bug-faithful, it can alias another cell's cached answer.
					ok = true;
					int32_t ax = mx * step, ay = my * step;
					for (int32_t k = 1; k <= mult; k++) {
						if (!valid_ucoord(g, u, n->x + ax, n->y + ay, uint32_t(ax) + n->metric + uint32_t(mul32(ay, W)))) {
							ok = false;
							break;
						}
						ax += mx * step;
						ay += my * step;
					}
				}
				if (!ok) continue;

				E += (pf.anti_unit && step == 0xc0) ? 5 : 1;
				int32_t tr = 0;
				int32_t c = calc_cost(g, n->x, n->y, p.x, p.y, d, step, n->timeout + 1, &tr);
				if (c == kBlocked) continue;
				p.transport = uint8_t(tr);
				if (p.x == gx && p.y == gy) c = c / 2;
				p.length = add32(n->length, c);
				if (L->closed.count(p.metric)) continue;
				SavedSearch::OpenKey qk;
				if (PathNode* q = L->find_open(p.metric, &qk)) {
					if (q->length <= p.length) continue;
					// remove q (removed before the T-level cutoff below can still discard p)
					L->open.erase(qk);
					L->refs.erase(p.metric);
				}
				const int32_t h = heuristic(step, p.x - gx, p.y - gy);
				p.estimate = h;
				p.value = add32(p.length, h);
				p.timeout = n->timeout + 1;
				p.parent = n;
				if (step == 0xc0 && h / 32 + p.timeout > 120) continue;
				L->insert_open(L->new_node(p));
			}
			L->closed[n->metric] = n;
		} while (!L->open.empty());
	}

	// Open list exhausted (section 7.6).
	pf.saving = 0;
	if (step == 0x30) {
		Order* m = u.front();
		if (m && order_is_move(m->type) && m->attempts < 13) set_retry(g, *m);
		add_safe(u);
	}
	return 0;
}

// ---------------------------------------------------------------- entry points

int32_t find_wpath(Game& g, std::vector<PathData>& stack, int32_t x, int32_t y, int32_t who, int32_t o) {
	PathFinder& pf = g.pathfinder;
	const World& w = g.world;
	set_pathing_unit(pf, who, o);
	Unit& u = pathing_unit(g);
	const Type& ut = utype(g, u);
	pf.scouting = 0;
	pf.sx = tcoord_of(x);
	pf.sy = tcoord_of(y);
	pf.offx = pf.offy = 0;
	const int32_t uwx = wcoord_of(x), uwy = wcoord_of(y);

	PathData gl = pop(stack);
	int32_t gwx = wcoord_of(gl.x), gwy = wcoord_of(gl.y);
	if (gwx < 0 || gwy < 0 || gwx >= w.xs || gwy >= w.ys) {
		stack.clear();
		return -1;
	}
	if ((uwx == gwx && uwy == gwy) || (ut.unit_flags & 0x20)) return push(stack, gl);  // same W cell, helicopter

	const bool human = (g.leaders[size_t(who)].flags & leaderflag::kHuman) != 0;
	bool march = true;
	if (human) {
		// Fog branch (leader flag 4; traced from the decompile @688fc0, not yet in the spec): drop goals whose W cell
		// is in another tregion or unexplored, down to a final entry; then march only toward explored goals (or for
		// ships), else search straight to the unexplored goal.
		if (!unit_can_transport(u)) {
			const int32_t ur = w.get_tregion(uwx * 4 + 2, uwy * 4 + 2);
			for (;;) {
				if (w.get_tregion(gwx * 4 + 2, gwy * 4 + 2) == ur && was_seen(g, gwx * 2 + 1, gwy * 2 + 1, who)) break;
				if ((gl.flags & pathflag::kFinal) || stack.empty()) break;
				gl = pop(stack);
				gwx = wcoord_of(gl.x);
				gwy = wcoord_of(gl.y);
				if (gwx < 0 || gwy < 0 || gwx >= w.xs || gwy >= w.ys) {
					stack.clear();
					return -1;
				}
			}
		}
		march = was_seen(g, gwx * 2 + 1, gwy * 2 + 1, who) || ut.domain == 1;
	}
	if (march && ut.domain < 2 && !unit_can_transport(u)) {
		// March the goal toward the unit while it is in another tregion (ships: or not a valid naval tile).
		const int32_t ur = w.get_tregion(tcoord_of(x), tcoord_of(y));
		for (;;) {
			const int32_t gtx = tcoord_of(gl.x), gty = tcoord_of(gl.y);
			if (w.get_tregion(gtx, gty) == ur &&
			    (ut.domain != 1 || unit_invalid_loc(g, u, gtx, gty, false, true, true, true, false) == 0))
				break;
			const int32_t adx = iabs(x - gl.x), ady = iabs(y - gl.y);
			const int32_t s = adx + ady > 0x2ff ? 0x180 : 0x30;
			if (adx < s && ady < s) return push(stack, gl);
			const uint32_t a = find_angle(x - gl.x, y - gl.y);
			gl.x += sin_a(a, s);
			gl.y -= cos_a(a, s);
			gwx = wcoord_of(gl.x);
			gwy = wcoord_of(gl.y);
			if (gwx == uwx && gwy == uwy) return push(stack, gl);
		}
	}

	push(stack, gl);
	if (iabs(uwy - gwy) + iabs(uwx - gwx) < 3) return int32_t(stack.size());

	if ((ut.role & 0x10) && u.order_type() == kExploreTo) {
		const Order* m = update_order(u);
		if (m && (!(m->flags & orderflag::kExplicit) || (u.unit_masks & 0x40100)) && m->type == kExploreTo)
			pf.scouting = 1;
	}
	push(stack, PathData{gwx * 0x300 + 0x180, gwy * 0x300 + 0x180, 0x180, 0});
	push(stack, PathData{uwx * 0x300 + 0x180, uwy * 0x300 + 0x180, 0, 0});
	if (!human) {
		// army: fighters (attack, or UnitData::is_supply) that are not workers, not attacking, not on half water.
		// TODO(movement-spec 6.1): is_supply is virtual (vtable +0xcc); only the UnitData default is modelled.
		if (ut.attack != 0 || (ut.unit_flags2 & 0x40)) {
			if (!is_worker(g, u) && !is_attacking(u) && !(w.w(wcoord_of(u.x), wcoord_of(u.y)).flags & wflag::kHalfLand))
				pf.army = 1;
		}
		if (is_worker(g, u)) pf.worker = 1;
	}
	int32_t r = astar_path(g, stack, 0x300, false);
	if (r == 0) {
		const PathData t = pop(stack);
		r = (t.flags & pathflag::kFinal) ? -1 : 0;
	} else {
		r = int32_t(stack.size());
	}
	pf.army = 0;
	pf.worker = 0;
	pf.iroquois = 0;
	kill_lists(pf);
	return r;
}

int32_t find_wpath(Game& g, std::vector<PathData>& stack, int32_t who, int32_t o) {
	const Unit& u = *g.unit(who, o);
	return find_wpath(g, stack, u.x, u.y, who, o);
}

int32_t find_wpath_army(Game& g, std::vector<PathData>& stack, int32_t x, int32_t y, int32_t who, int32_t o) {
	g.pathfinder.army = 1;
	const int32_t r = find_wpath(g, stack, x, y, who, o);
	g.pathfinder.army = 0;
	return r;
}

int32_t find_tpath(Game& g, std::vector<PathData>& stack, int32_t x, int32_t y, int32_t who, int32_t o) {
	PathFinder& pf = g.pathfinder;
	const World& w = g.world;
	pf.scouting = 0;
	pf.sx = tcoord_of(x);
	pf.sy = tcoord_of(y);
	pf.offx = pf.offy = 0;
	set_pathing_unit(pf, who, o);
	Unit& u = pathing_unit(g);
	const Type& ut = utype(g, u);

	if (pf.saving == 0) {
		const int32_t utx = tcoord_of(x), uty = tcoord_of(y);
		PathData gl = pop(stack);
		int32_t gtx = tcoord_of(gl.x), gty = tcoord_of(gl.y);
		if (gtx < 0 || gty < 0 || gtx >= w.tile_xs || gty >= w.tile_ys) {
			stack.clear();
			return -1;
		}
		if ((utx == gtx && uty == gty) || (ut.unit_flags & 0x20)) {
			gl.tol = 0;
			return push(stack, gl);
		}
		if (ut.domain < 2 && !unit_can_transport(u)) {
			for (;;) {
				if (w.get_tregion(gtx, gty) == w.get_tregion(utx, uty) && valid_tcoord(g, u, gtx, gty)) break;
				if (iabs(x - gl.x) < 0x60 && iabs(y - gl.y) < 0x60) {
					if (!(gl.flags & pathflag::kFinal)) return 0;
					return push(stack, gl);
				}
				const uint32_t a = find_angle(x - gl.x, y - gl.y);
				gl.x += sin_a(a, 0x60);
				gl.y -= cos_a(a, 0x60);
				gtx = tcoord_of(gl.x);
				gty = tcoord_of(gl.y);
				if (utx == gtx && uty == gty) return push(stack, gl);
			}
		}
		if (iabs(uty - gty) + iabs(utx - gtx) < 2) {
			gl.tol = 0;
			return push(stack, gl);
		}
		push(stack, gl);
		push(stack, PathData{gtx * 0xc0 + 0x60, gty * 0xc0 + 0x60, gl.tol == 0x180 ? 0x180 : 0x60, 0});
		push(stack, PathData{utx * 0xc0 + 0x60, uty * 0xc0 + 0x60, 0, 0});
	}

	// offx/offy (unused by astar_path, which reads the order itself; kept as PathFinder state).
	if (const Order* m = update_order(u); m && order_is_move(m->type)) {
		pf.offx = int32_t(m->off_x) % 0xc0 - 0x60;
		pf.offy = int32_t(m->off_y) % 0xc0 - 0x60;
	}
	int32_t r = astar_path(g, stack, 0xc0, false);
	if (r < 1) {
		if (pf.saving == 0) {
			r = 0;
			if (!stack.empty() && !(stack.back().flags & pathflag::kFinal)) stack.pop_back();
		}
	} else {
		r = int32_t(stack.size());
	}
	pf.iroquois = 0;
	kill_lists(pf);
	return r;
}

int32_t find_tpath(Game& g, std::vector<PathData>& stack, int32_t who, int32_t o) {
	const Unit& u = *g.unit(who, o);
	return find_tpath(g, stack, u.x, u.y, who, o);
}

int32_t find_upath(Game& g, std::vector<PathData>& stack, int32_t who, int32_t o, bool fast) {
	PathFinder& pf = g.pathfinder;
	pf.limit = search_limit(g, who, 500);
	if (fast) pf.limit /= 2;
	const Unit& u = *g.unit(who, o);
	const int32_t r = find_upath_body(g, stack, u.x, u.y, who, o, fast);
	pf.saving = 0;
	return r;
}

int32_t find_upath_restore(Game& g, std::vector<PathData>& stack, int32_t who, int32_t o) {
	PathFinder& pf = g.pathfinder;
	pf.saving = 1;
	pf.limit = search_limit(g, who, 300);
	const Unit& u = *g.unit(who, o);
	const int32_t r = find_upath_body(g, stack, u.x, u.y, who, o, false);
	pf.saving = 0;
	return r;
}

}  // namespace ron::sim
