// Unit collisions (docs/movement-spec.md section 10): the CollCheck queries over the per-wcoord CollBlock bitmaps
// (collide_here / fill_slots, move_unit), the corner predicates, Unit::detect_unit_collision,
// Unit::resolve_unit_collision, Unit::resolve_block and the pusher entry of Unit::detect_boat_collision.
//
// Collision blocks: World::coll_blocks / WData.block (sim/world.hpp). Snapshots for path searches: the PathFinder's
// blocklist (sim/pathfinder.hpp), copied on first touch exactly like CollCheck::fill_slots does.
#include <algorithm>

#include "sim/combat.hpp"
#include "sim/game.hpp"
#include "sim/guys.hpp"
#include "sim/movement.hpp"
#include "sim/orders.hpp"
#include "sim/pathfinder.hpp"
#include "sim/spells.hpp"
#include "sim/trig.hpp"

namespace ron::sim {

namespace {

// corner_box_x/y @add2a0/@add2b0: the four wcoords a footprint box can touch (slot = 2*xhi + yhi).
constexpr int32_t kCornerBoxX[4] = {0, 0, 1, 1}, kCornerBoxY[4] = {0, 1, 0, 1};

// The global `collcheck`: per slot, whether the box reaches it (get) and its block (locals).
struct Slots {
	bool get[4] = {false, false, false, false};
	CollBlock* locals[4] = {nullptr, nullptr, nullptr, nullptr};
};

// GameDaemon::repaths is indexed by the player byte; gaia (9) would index past the 8 entries in the original.
int32_t& repaths_of(Game& g, int32_t who) {
	static int32_t scratch = 0;  // TODO(frame-spec): what the original reads for who >= 8 (memory after repaths)
	if (who >= 0 && who < int32_t(g.daemon.repaths.size())) return g.daemon.repaths[size_t(who)];
	scratch = 0;
	return scratch;
}

bool is_move_type(OrderType t) {
	return t == kMoveTo || t == kAttackTo || t == kExploreTo || t == kFleeTo || t == kChangeForm || t == kGroupMove ||
	       t == kGroupAttackTo;
}

// UnitData::is_moving @610af0: the current order's is_move (vtable +0x14; true for every MoveOrder class).
bool is_moving(const Unit& u) { return !u.orders.empty() && order_is_move(u.orders.front().type); }

// The new_block_radius footprint test of UnitData::will_be_corner @609fa0 / GuyData::is_corner @5de270: the diagonal
// corner code of h relative to a footprint centre at Chebyshev distance exactly r on both axes.
int32_t corner_code(int32_t d1, int32_t d2, int32_t r) {
	if (iabs(d1) == r && iabs(d2) == r) {
		if (d1 < 0 && d2 < 0) return 1;
		if (d1 > 0) {
			if (d2 < 0) return 3;
			if (d2 > 0) return 5;
		}
		if (d1 < 0 && d2 > 0) return 7;
	}
	return 0;
}

// CollCheck::fill_slots @6820e0.
void fill_slots(Game& g, Slots& s, int32_t ux, int32_t uy, int32_t r, bool pathing) {
	World& w = g.world;
	const int32_t bx = (ux - r) >> 4, by = (uy - r) >> 4;
	bool done[4] = {false, false, false, false};
	s.get[0] = true;
	s.get[2] = s.get[3] = ((ux + r) >> 4) != bx;
	const bool same_y = ((uy + r) >> 4) == by;
	if (same_y) s.get[3] = false;
	s.get[1] = !same_y;
	for (int i = 0; i < 4; i++) {
		s.locals[i] = nullptr;
		const int32_t cx = bx + kCornerBoxX[i], cy = by + kCornerBoxY[i];
		if (!s.get[i] || !w.in_w(cx, cy)) {
			done[i] = true;
			s.get[i] = false;
		}
	}
	// The probe's own region (get_tregion of its tcoord); a negative region filters nothing. (The original reads
	// whatever WData the index hits for an off-map probe; callers never pass one.)
	const int32_t region = w.in_u(ux, uy) ? w.get_tregion(ux >> 2, uy >> 2) : -1;

	BlockSnapshot* snap = pathing ? g.pathfinder.blocklist : nullptr;
	if (snap) {
		for (int i = 0; i < 4; i++) {
			if (!s.get[i]) continue;
			const int32_t key = (bx + kCornerBoxX[i]) + w.xs * (by + kCornerBoxY[i]);
			auto it = snap->find(key);
			if (it != snap->end()) {
				// A null block was recorded as an empty copy: both read as an empty slot.
				s.locals[i] = it->second.present ? &it->second.block : nullptr;
				done[i] = true;
			}
		}
	}
	for (int i = 0; i < 4; i++) {
		if (done[i]) continue;
		const WData& d = w.w(bx + kCornerBoxX[i], by + kCornerBoxY[i]);
		s.locals[i] = ((region < 0 || d.region == region) && d.block >= 0) ? &w.coll_blocks[size_t(d.block)] : nullptr;
	}
	if (snap) {
		// Copy on first touch. This call keeps reading the live block; later calls of the search read the copy.
		for (int i = 0; i < 4; i++) {
			if (!s.get[i] || done[i]) continue;
			const int32_t key = (bx + kCornerBoxX[i]) + w.xs * (by + kCornerBoxY[i]);
			SnapBlock sb;
			if (s.locals[i]) {
				sb.present = true;
				sb.block = *s.locals[i];
			}
			(*snap)[key] = sb;
		}
	}
}

}  // namespace

// ---------------------------------------------------------------- CollCheck

// CollCheck::collide_here @682540.
int32_t collide_here(Game& g, const Unit& u, int32_t ux, int32_t uy, int32_t r, int32_t* hx, int32_t* hy, bool pathing) {
	if (r == 0) return 0;
	r = std::min(r, 10);  // the compass spiral has radius 10 (stock new_block_radius is 0..7)
	Slots s;
	fill_slots(g, s, ux, uy, r, pathing);
	// BitMask<768>::empty refreshes the block's cached flags (a read with a side effect, also on live blocks).
	bool empty[4];
	for (int i = 0; i < 4; i++) {
		empty[i] = !s.get[i];
		if (!empty[i]) {
			empty[i] = s.locals[i] == nullptr;
			if (!empty[i]) empty[i] = s.locals[i]->empty();
		}
	}
	bool only0 = false;
	if (empty[1] && empty[2] && empty[3]) {
		only0 = true;
		if (empty[0]) return 0;
	}
	const int32_t bx = (ux - r) >> 4, by = (uy - r) >> 4;
	// -1: the slot is empty (no test ran), 0: clear, 1: set.
	auto probe = [&](int32_t X, int32_t Y) -> int {
		if (!only0) {
			const int slot = ((X >> 4) <= bx ? 0 : 2) + ((Y >> 4) <= by ? 0 : 1);
			if (empty[slot]) return -1;
			return s.locals[slot]->get(X, Y) ? 1 : 0;
		}
		if ((X >> 4) <= bx && (Y >> 4) <= by) return s.locals[0]->get(X, Y) ? 1 : 0;
		return -1;
	};
	auto hit = [&](int32_t X, int32_t Y) {
		if (hx) *hx = X;
		if (hy) *hy = Y;
		return 1;
	};

	if (!pathing) {
		// Fast path: a probe one ucoord orthogonally from the unit's ucoord only tests the leading edge, every other
		// cell. An even step advances only when a test ran and was clear: an empty slot stalls it (asm-verified).
		const int32_t dux = ux - ucoord_of(u.x), duy = uy - ucoord_of(u.y);
		if (dux == 0 || duy == 0) {
			const int32_t n = 2 * r + 1;
			if (iabs(dux) == 1) {
				const int32_t X = ux + dux * r;
				int32_t Y = uy - r;
				for (int32_t i = 0; i < n; i++) {
					if (i & 1) {
						Y++;
						continue;
					}
					const int p = probe(X, Y);
					if (p == 1) return hit(X, Y);
					if (p == 0) Y++;
				}
				return 0;
			}
			if (iabs(duy) == 1) {
				const int32_t Y = uy + duy * r;
				int32_t X = ux - r;
				for (int32_t i = 0; i < n; i++) {
					if (i & 1) {
						X++;
						continue;
					}
					const int p = probe(X, Y);
					if (p == 1) return hit(X, Y);
					if (p == 0) X++;
				}
				return 0;
			}
		}
	}

	// General path: the 2-ucoord lattice of the spiral (edges included), skipping cells within r of the unit's own
	// ucoord while it is on the map (its own footprint).
	const int32_t oux = ucoord_of(u.x), ouy = ucoord_of(u.y);
	const bool on_map = u.inside_up < 0;  // UnitData::is_on_map
	const int32_t n = kMoveRadius[r];
	for (int32_t i = 0; i < n; i++) {
		if (((move_x[i] + r) & 1) || ((move_y[i] + r) & 1)) continue;
		const int32_t X = ux + move_x[i], Y = uy + move_y[i];
		if (on_map && iabs(X - oux) <= r && iabs(Y - ouy) <= r) continue;
		if (probe(X, Y) == 1) return hit(X, Y);
	}
	return 0;
}

// CollCheck::move_unit @682ad0.
void coll_move_unit(Game& g, int32_t old_ux, int32_t old_uy, int32_t new_ux, int32_t new_uy, int32_t r) {
	if (r == 0) return;
	r = std::min(r, 10);
	World& w = g.world;
	CollBlock* fast = nullptr;
	int32_t region = r;  // (the original's region slot holds r until it is computed; never read before that)
	// Fast path: both squares inside one valid wcoord whose block (for the old tile's tregion) exists. A null block
	// or a region mismatch writes nothing at all.
	if (iabs(old_ux - new_ux) + 2 * r < 16 && iabs(old_uy - new_uy) + 2 * r < 16) {
		const int32_t bx = (std::min(old_ux, new_ux) - r) >> 4, by = (std::min(old_uy, new_uy) - r) >> 4;
		if (((std::max(old_ux, new_ux) + r) >> 4) == bx && ((std::max(old_uy, new_uy) + r) >> 4) == by &&
		    w.in_w(bx, by)) {
			region = w.get_tregion(old_ux >> 2, old_uy >> 2);
			bool mismatch = false;
			CollBlock* b = w.coll_block(bx, by, region, &mismatch);
			if (mismatch || !b) return;
			fast = b;
		}
	}
	const int32_t n = kMoveRadius[r];
	// Clear the old square's cells that are not within r of the new centre.
	if (w.in_u(old_ux, old_uy)) {
		if (!fast) region = w.get_tregion(old_ux >> 2, old_uy >> 2);
		for (int32_t i = 0; i < n; i++) {
			const int32_t X = old_ux + move_x[i], Y = old_uy + move_y[i];
			if (iabs(X - new_ux) <= r && iabs(Y - new_uy) <= r) continue;
			if (fast) {
				fast->set(X, Y, false);
				continue;
			}
			if (!w.in_u(X, Y)) continue;
			const WData& d = w.w(X >> 4, Y >> 4);
			if (region >= 0 && d.region != region) continue;
			if (d.block >= 0) w.coll_blocks[size_t(d.block)].set(X, Y, false);
		}
	}
	// Set the new square's cells that are not within r of the old centre (creating blocks on the slow path).
	if (!fast) region = w.in_u(new_ux, new_uy) ? w.get_tregion(new_ux >> 2, new_uy >> 2) : -1;
	for (int32_t i = 0; i < n; i++) {
		const int32_t X = new_ux + move_x[i], Y = new_uy + move_y[i];
		if (iabs(X - old_ux) <= r && iabs(Y - old_uy) <= r) continue;
		if (fast) {
			fast->set(X, Y, true);
			continue;
		}
		if (!w.in_u(X, Y)) continue;
		const int32_t wx = X >> 4, wy = Y >> 4;
		const WData& d = w.w(wx, wy);
		if (region >= 0 && d.region != region) continue;
		if (d.block >= 0) w.coll_blocks[size_t(d.block)].set(X, Y, true);
		else w.new_coll_block(wx, wy).set(X, Y, true);
	}
}

// ---------------------------------------------------------------- corner predicates

// UnitData::will_be_corner @609fa0 (radius of the unit's own type).
int32_t will_be_corner(const Game& g, const Unit& u, int32_t hx, int32_t hy, int32_t ux, int32_t uy) {
	(void)g;
	return corner_code(hx - ux, hy - uy, u.ptype->new_block_radius);
}

// UnitData::is_corner @60a040: the first squad guy [0, guy_mark) whose footprint has h as a diagonal corner.
int32_t is_corner(const Game& g, const Unit& u, int32_t hx, int32_t hy) {
	(void)g;
	if (!(u.flags & objflag::kValid) || !u.on_map()) return 0;  // SubObjectData::is_active, is_on_map
	const int32_t r = u.ptype->new_block_radius;
	for (int32_t i = 0; i < int32_t(u.guy_mark) && i < int32_t(u.guys.size()); i++) {
		const Guy& gy = u.guys[size_t(i)];
		const int32_t c = corner_code(hx - ucoord_of(gy.x), hy - ucoord_of(gy.y), r);
		if (c != 0) return c;
	}
	return 0;
}

// UnitData::is_here @60a0c0: h within the unit's (not its guys') footprint square.
bool is_here(const Game& g, const Unit& u, int32_t hx, int32_t hy) {
	(void)g;
	const int32_t r = u.ptype->new_block_radius;
	if (r == 0) return false;
	if (iabs(hx - ucoord_of(u.x)) > r) return false;
	return iabs(hy - ucoord_of(u.y)) <= r;
}

// ---------------------------------------------------------------- Unit::detect_unit_collision @617060

int32_t detect_unit_collision(Game& g, Unit& u, int32_t x, int32_t y, bool test_only, bool allow_push, int32_t p5,
                              bool pathing, bool no_push) {
	(void)p5;  // unused by the original
	const Type& t = *u.ptype;
	auto reset = [&]() -> int32_t {
		u.collide_who = -1;
		u.collide_o = -1;
		if (u.collide_frame < g.frame - 5) u.collide = 0;
		u.unit_masks &= ~umask::kWaiting;
		return 0;
	};
	if (t.domain == 2) return test_only ? 0 : reset();
	if (!no_push) {
		// Pushers: ships, siege (UnitTypeData::is_siege = unit_flags 0x20000), heroes and supply (unit_flags2 0x20 /
		// 0x40). They shove others through detect_boat_collision instead of colliding, except while pathing.
		const bool pusher = t.domain == 1 || (t.unit_flags & 0x20000) || (t.unit_flags2 & 0x20) || (t.unit_flags2 & 0x40);
		if (pusher && !pathing) {
			if (test_only || !allow_push) return 0;
			if (detect_boat_collision(g, u, x, y, true) != 0) return 0;
		}
	}
	// A go_around_building midpoint on top, or the post-failure grace counter: ignore units.
	if (!u.path.empty() && (u.path.back().flags & pathflag::kAround)) return test_only ? 0 : reset();
	if (u.safe != 0) return test_only ? 0 : reset();

	const int32_t ux = ucoord_of(x), uy = ucoord_of(y);
	int32_t hx = 0, hy = 0;
	if ((ux == ucoord_of(u.x) && uy == ucoord_of(u.y)) ||
	    collide_here(g, u, ux, uy, t.new_block_radius, &hx, &hy, pathing) == 0)
		return test_only ? 0 : reset();
	const int32_t corner = will_be_corner(g, u, hx, hy, ux, uy);
	if (test_only) return 1;

	// The owner of the hit bit: units in the object lists of the 9 wcoords around (x, y), in spiral order.
	const int32_t wx = wcoord_of(x), wy = wcoord_of(y);
	const OrderType my_act = action_type(u);
	const bool attacking = my_act == kAttack;
	int32_t beyond = 0;  // how far (x, y) is beyond my attack range from my target
	if (attacking) {
		const Order* aa = get_action(u);
		if (aa && aa->ox >= 0 && aa->whom >= 0 && g.object(aa->whom, aa->ox))
			beyond = std::max(0, attack_dist(g, u, aa->ox, aa->whom, x, y) - obj_max_range(g, u) * 0xc0);
	}
	bool soft = false;
	for (int k = 0; k < 9; k++) {
		const int32_t cx = wx + move_x[k], cy = wy + move_y[k];
		if (!g.world.in_w(cx, cy)) continue;
		const WData& d = g.world.w(cx, cy);
		int32_t o = d.down, who = d.down_who;
		while (o >= 0) {
			const Object* ob = g.object(who, o);
			if (!ob) break;
			const int32_t next_o = ob->down, next_who = ob->down_who;  // read before any test, like the original
			const int32_t cur_o = o, cur_who = who;
			o = next_o;
			who = next_who;
			if (cur_o == u.o && cur_who == u.who) continue;
			if (!ob->is_unit) continue;  // vtable +0x18
			Unit* b = g.unit(cur_who, cur_o);
			if (!b || b->ptype->domain == 2) continue;
			if (!b->on_map() || !is_here(g, *b, hx, hy)) continue;

			// Soft owners: noted (move_step halves the next step) and skipped.
			const OrderType b_act = action_type(*b);
			if (my_act == kTradeRoute && b_act == kTradeRoute) {
				if (is_moving(u) && is_moving(*b)) {
					soft = true;
					continue;
				}
			} else if (b_act == kGuard) {
				const Order* ga = update_action(g, *b);
				if (ga && ga->ox == u.o && ga->whom == u.who) {  // it guards me
					soft = true;
					continue;
				}
			} else if (b_act == kAttack && attacking && cur_who == u.who && t.new_block_radius == 1 &&
			           b->ptype->new_block_radius == 1) {
				if (is_moving(u) && is_moving(*b) && beyond > 0x300) {
					soft = true;
					continue;
				}
			}
			if (u.group == b->group && u.group != -1 && !attacking && !b->openlist) {
				if (b->orders.empty()) {
					soft = true;
					continue;
				}
				const OrderType bt = b->orders.front().type;
				if (bt == kCastSpell) {
					// Soft when the cast order's spell type is a pack spell (0x28b/0x28d/0x28f/0x291: TypeData::is_pack).
					// (TRANSPORT, 0x28a, is not: the mover waits for it below instead.)
					if (spell_is_pack(b->orders.front().spell)) {
						soft = true;
						continue;
					}
				} else if (bt == kOrderNone || bt == kGuard || (is_move_type(bt) && b_act != kAttack)) {
					soft = true;
					continue;
				}
			}
			// Footprints that only touch at opposite diagonal corners pass each other.
			if (corner != 0 && iabs(corner - is_corner(g, *b, hx, hy)) == 4) continue;
			u.collide_o = int16_t(cur_o);
			u.collide_who = int8_t(cur_who);
			u.collide_guy = 0;
			if (pathing) return 1;
			Order* cur = u.front();
			if (!cur || !order_is_move(cur->type)) return 1;
			cur->coll_x = x;
			cur->coll_y = y;
			return 1;
		}
	}
	if (soft) u.unit_masks |= umask::kSlowDown;
	return reset();
}

// ---------------------------------------------------------------- Unit::resolve_unit_collision @5f9d30

void resolve_unit_collision(Game& g, Unit& u, int32_t x, int32_t y) {
	(void)x;
	(void)y;
	// (The leading vtable +0x30 test is a folded `return 0` for units and animals.)
	const int32_t bo = u.collide_o, bw = u.collide_who;
	if (u.ptype->unit_flags & 0x2000) {
		// Units that die attacking (fire ships ramming).
		if (valid_target(g, u, bo, bw, nullptr)) {
			set_attack(g, u, bo, bw);
			fire_ammo(g, u, bo, bw);
			object_die(g, u, kDeathSploded, -1, 0.0f);
			return;
		}
	}
	Order* act = update_action(g, u);
	const OrderType act_t = act ? act->type : kOrderNone;
	if (bw == u.who) {
		// TODO(gather): a GATHER action whose target is an active wall-type building covering my tile ->
		// kill_current_order, return.
	} else if (act) {
		if (order_is_targeted(act->type)) {
			if (act->ox == bo && act->whom == bw) {
				// B is my target. vtable +0x2c of the current order: true for GroupMoveOrder, false for MoveOrder.
				Order* cur = update_order(u);
				if (cur && order_is_group(cur->type)) {
					u.collide = 1;
					return;
				}
				u.collide_o = -1;
				u.collide_who = -1;
				kill_current_order(g, u);
				return;
			}
			// @5f9f2b: my target already in range -> done; an enemy collider B (non-mandatory, not a group order):
			// followers copy their captain's attack when it is in range, captains retarget (find_new_target while
			// the leader's retarget budget is below 10, else attack B).
			if (act->ox >= 0 && act->whom >= 0 && g.object(act->whom, act->ox) &&
			    g.object(act->whom, act->ox)->alive() && is_in_range(g, u, act->ox, act->whom, 0)) {
				kill_current_order(g, u);
				return;
			}
			Order* cur = update_order(u);
			// (the byte at order +0x1c: AttackOrder::mandatory, but CastOrder::paid for a CAST_SPELL action: a paid cast is not
			// dropped for a colliding enemy)
			const int32_t mand_byte = act->type == kCastSpell ? act->paid : act->mandatory;
			if (mand_byte == 0 && !(cur && order_is_group(cur->type)) && is_enemy(g, u.who, bw)) {
				if (u.o_up >= 0) {
					Unit* cap = g.unit(u.who, obj_captain(g, u));
					if (cap && action_type(*cap) == kAttack) {
						const Order* ca = update_action(g, *cap);
						if (ca && ca->ox >= 0 && ca->whom >= 0 && g.object(ca->whom, ca->ox) &&
						    is_in_range(g, u, ca->ox, ca->whom, 0)) {
							const int32_t cox = ca->ox, cw = ca->whom;
							repath(g, u);
							kill_current_order(g, u);
							add_attack_order(g, u, cox, cw, kQueueFirst, 0, false);
							return;
						}
					}
				} else {
					if (g.leaders[size_t(u.who)].retarget_budget < 10) {
						find_new_target(g, u, nullptr, true);
						return;
					}
					if (!(u.ptype->unit_flags & 0x20000) && valid_target(g, u, bo, bw, nullptr)) {
						repath(g, u);
						kill_current_order(g, u);
						add_attack_order(g, u, bo, bw, kQueueFirst, 0, false);
						return;
					}
				}
			}
		} else {
			// TODO(gather): the GATHER wall check (only reachable for targeted orders; nothing to do here).
		}
	}

	Unit* b = g.unit(bw, bo);
	const OrderType bt = b ? b->order_type() : kOrderNone;

	// Sidestep: a moving collider and a land unit not walking a fine (U-level) waypoint.
	if (is_move_type(bt) && u.ptype->domain == 0) {
		const uint32_t tflags = u.path.empty() ? 0 : u.path.back().flags;
		Order* cur = u.front();  // get_order()->get_move_order
		if (!(tflags & pathflag::kFine) && cur && order_is_move(cur->type)) {
			const int32_t cx = ucoord_of(cur->coll_x), cy = ucoord_of(cur->coll_y);
			const int32_t dx = cx - ucoord_of(u.x), dy = cy - ucoord_of(u.y);
			// (dx == dy == 0 raises the debug report "Collided in my space?")
			auto free_cell = [&](int32_t X, int32_t Y) {
				if (unit_invalid_loc(g, u, X >> 2, Y >> 2, false, false, false, false, false) != 0) return false;
				return detect_unit_collision(g, u, X * 48 + 24, Y * 48 + 24, true, true, 0, false, false) == 0;
			};
			int32_t X = 0, Y = 0;
			bool ok = false;
			if (iabs(dx) == iabs(dy)) {
				if (free_cell(cx, cy - dy)) {
					X = cx, Y = cy - dy, ok = true;
				} else if (free_cell(cx - dx, cy)) {
					X = cx - dx, Y = cy, ok = true;
				}
			} else {
				if (free_cell(cx - dy, cy - dx)) {
					X = cx - dy, Y = cy - dx, ok = true;
				} else if (free_cell(cx + dy, cy + dx)) {
					X = cx + dy, Y = cy + dx, ok = true;
				}
			}
			if (ok) {
				u.path.push_back(PathData{X * 48 + 24, Y * 48 + 24, 0, pathflag::kFine});
				Order* m = update_order(u);
				if (m && order_is_move(m->type)) {
					m->dest_x = X * 48 + 24;
					m->dest_y = Y * 48 + 24;
				}
				return;
			}
		}
	}

	u.collide++;
	u.collide_frame = g.frame;

	// Wait for a friendly mover that is not itself waiting on someone (or waits on someone else).
	bool wait_case = false;
	if (bt != kFleeTo) {
		if (bt == kMoveTo || bt == kAttackTo || bt == kExploreTo || bt == kChangeForm || bt == kGroupMove ||
		    bt == kGroupAttackTo)
			wait_case = true;
		else if (bt == kCastSpell) {
			wait_case = b && b->orders.front().spell == spell::kTransport;  // B is turning into a barge (transport-spec 6)
		}
	}
	if (wait_case && b) {
		const int16_t lim = repaths_of(g, u.who) > 3 ? 0x80 : 0x20;
		bool b_free = !(b->unit_masks & umask::kWaiting);
		if (!b_free && b->collide_o >= 0) {
			const Unit* bb = g.unit(b->collide_who, b->collide_o);
			b_free = bb && !(bb->unit_masks & umask::kWaiting);
		}
		if (b->collide < lim && u.collide < lim && !is_enemy(g, u.who, bw) &&
		    (b->collide_o != u.o || b->collide_who != u.who) && b_free) {
			u.unit_masks |= umask::kWaiting;
			return;
		}
	}

	// Repath around units, throttled by the player's repath counter.
	if (u.path.empty()) return;
	int32_t& rp = repaths_of(g, u.who);
	const int32_t rpv = rp;
	int32_t c;
	if (rpv < 4) {
		c = u.collide;
	} else {
		if (((u.o + u.collide) & 3) != 0) return;
		c = u.collide / 4;
	}
	if (!(rpv < 16 && (rpv < 8 || ((u.o + c) & 15) == 0))) return;
	rp = rpv + 1;
	// Drop waypoints up to the first final one, or a coarse (tol >= 0x60), non-fine one that is not blocked.
	PathData p;
	for (;;) {
		p = u.path.back();
		u.path.pop_back();
		bool blocked = false;
		if (p.tol > 0x5f || !(p.flags & pathflag::kFine)) {
			const int32_t tx = tcoord_of(p.x), ty = tcoord_of(p.y);
			blocked = g.world.in_t(tx, ty) && (g.world.t(tx, ty) & 0x6000) != 0;
			if (!blocked)
				blocked = collide_here(g, u, ucoord_of(p.x), ucoord_of(p.y), u.ptype->new_block_radius, nullptr, nullptr,
				                       true) != 0;
		}
		if ((p.flags & pathflag::kFinal) || !(p.tol < 0x60 || (p.flags & pathflag::kFine) || blocked)) break;
		if (u.path.empty()) break;  // the original would re-read the stale bottom entry forever
	}
	u.path.push_back(p);
	unit_set_new_location(g, u, ucoord_of(u.x) * 48 + 24, ucoord_of(u.y) * 48 + 24, true, false);
	const bool fast = act_t == kAttack && b && action_type(*b) == kAttack;
	const int32_t r = find_upath(g, u.path, u.who, u.o, fast);
	// The current order may have changed (find_upath kills it on failure); the original writes through
	// update_order()->get_move_order() regardless.
	Order* cur = update_order(u);
	if (cur && order_is_move(cur->type)) cur->dest = 0;
	if (r != 0 && b && b->collide_o == u.o && b->collide_who == u.who && !(b->unit_masks & umask::kWaiting)) {
		Order* cur2 = update_order(u);
		const int32_t v = g.rnd_draw(9);  // @5fa86e: the only game_random draw of the collision code
		if (cur2 && order_is_move(cur2->type)) cur2->pause = v + 1;
	}
}

// ---------------------------------------------------------------- Unit::resolve_block @5fccc0

int32_t resolve_block(Game& g, Unit& u) {
	const PathData top = u.path.empty() ? PathData{} : u.path.back();
	const int32_t tx = tcoord_of(top.x), ty = tcoord_of(top.y);
	if (unit_invalid_loc(g, u, tx, ty, false, false, false, false, false) == 0) return 0;
	if (unit_invalid_loc(g, u, tx, ty, false, false, true, false, false) != 0) return 0;
	// TODO(buildings): b = ObjectsData::find_any_building_at(tx, ty, who, FILTER_ALL) (owner in objects+0x200).
	//   own building: unless its type's vtable +0x90 (wall-like) is set, Build +0x60 |= 1; return 0.
	//   not an enemy: leader diplomacy entry [owner] |= 2; return 0.
	//   enemy: add_attack_order(b, owner, QUEUE_FIRST); return 1.
	// No building lookup exists yet, which is the original's "nothing found" (return 0).
	return 0;
}

// ---------------------------------------------------------------- Unit::detect_boat_collision @5fa8b0

int32_t detect_boat_collision(Game& g, Unit& u, int32_t x, int32_t y, bool mutual) {
	(void)g;
	(void)x;
	(void)y;
	(void)mutual;
	if (u.who >= 8 || u.ptype->push_size == 0) return 1;
	// TODO(pushers): Objects::find_units candidates, push_circles overlap, push or block (docs/movement-spec.md
	// section 10.5; spells-spec 5: a blocker that is a packer is pushed only while PACKED and never while
	// cast_is_unpacking, nor when entrenched or cargo). Until then a pusher with a push_size is never blocked by the push test and falls through to
	// the ordinary footprint check.
	return 0;
}

}  // namespace ron::sim
