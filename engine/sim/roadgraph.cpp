// The road graph (Roads / RoadsOut / TerrainOut road data of the original); see sim/roadgraph.hpp for what is and is not ported.
// Every function carries its address; the decompile reads were checked against the disassembly where the decompiler lost an
// argument (set_diags: the corner tile is (x + corner_x[k], y), the horizontal neighbour; scan_and_kill_stray_roads: an
// out-of-map tile clears the *wrong* cache array, see scan_stray).
#include "sim/roadgraph.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "sim/compass.hpp"
#include "sim/world.hpp"

namespace ron::sim {
namespace {

// road_compass_flags @af2570: the arm bit of cache direction k (1 NW, 2 N, 3 NE, 4 E, 5 SE, 6 S, 7 SW, 8 W).
constexpr uint32_t kArm[9] = {0, 0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u};
constexpr uint32_t kN = 0x40000000u, kE = 0x10000000u, kS = 0x04000000u, kW = 0x01000000u;
constexpr uint32_t kNW = 0x80000000u, kNE = 0x20000000u, kSE = 0x08000000u, kSW = 0x02000000u;
// corner_x / corner_y @adc3e0 / @adc3c0 for k = 1..4 (NW, NE, SE, SW).
constexpr int32_t kCornerX[5] = {0, -1, 1, 1, -1};
constexpr int32_t kCornerY[5] = {0, -1, -1, 1, 1};
// The diagonal cache index of corner k, and its two orthogonal cache indices (set_diags' local tables).
constexpr int32_t kDiagIdx[5] = {0, 1, 3, 5, 7};
constexpr int32_t kOrth1[5] = {0, 8, 2, 4, 6};  // A[k + 30]
constexpr int32_t kOrth2[5] = {0, 2, 4, 6, 8};  // A[k + 25]
constexpr uint32_t kSetOwn[5] = {0, 0x80000000u, 0x20000000u, 0x08000000u, 0x02000000u};     // A[k + 10]: the corner's arm on this tile
constexpr uint32_t kSetNeighbour[5] = {0, 0x08000000u, 0x02000000u, 0x80000000u, 0x20000000u};  // A[k + 5]: the opposite arm on the neighbour
constexpr uint32_t kBlock1[5] = {0, 0x02000000u, 0x08000000u, 0x20000000u, 0x80000000u};  // A[k + 20]
constexpr uint32_t kBlock2[5] = {0, 0x20000000u, 0x80000000u, 0x02000000u, 0x08000000u};  // A[k + 15]

// "C4" of Data/roads.xml (RoadsPieces::element_C4): the element number of the diagonal-piece candidates. Read from the saves: every candidate
// with no arms, a rotation 0..3 and one reference carries 55 (0x37) in element_num.
constexpr int32_t kElementC4 = 55;

using Words = uint32_t*;

inline uint8_t* b2(Words c) { return reinterpret_cast<uint8_t*>(c + 2); }  // [0] rotation, [1] unknown, [2] ref_a, [3] ref_b
inline uint8_t* b3(Words c) { return reinterpret_cast<uint8_t*>(c + 3); }  // [0..1] support_codes (LE), [2] unknown, [3] is_terrain_creation
inline uint8_t& rot(Words c) { return b2(c)[0]; }
inline uint8_t& ref_a(Words c) { return b2(c)[2]; }
inline uint8_t& ref_b(Words c) { return b2(c)[3]; }
inline uint8_t& terrain_creation(Words c) { return b3(c)[3]; }
inline uint32_t& flags(Words c) { return c[1]; }
inline int32_t element_num(Words c) { int32_t v; std::memcpy(&v, c, 4); return v; }
inline void set_element_num(Words c, int32_t v) { std::memcpy(c, &v, 4); }
inline uint16_t support(Words c) { return uint16_t(b3(c)[0] | b3(c)[1] << 8); }
inline void set_support(Words c, uint16_t v) { b3(c)[0] = uint8_t(v); b3(c)[1] = uint8_t(v >> 8); }

struct Mod {
	int32_t index = 0;  // x + tile_xs * y (the original's x + world.xs * y * 4)
	uint8_t added = 0, valid = 1;
};

class Roads {
public:
	explicit Roads(World& w) : w_(w) {
		if (w_.roads.slot.size() != size_t(w_.size)) w_.roads.slot.assign(size_t(w_.size), -1);
	}

	// ---- Roads::road_added @8954d0 / road_cleared @8955d0
	void road_added(int32_t tx, int32_t ty, bool run) {
		mods_.push_back({tx + w_.tile_xs * ty, 1, 1});
		if (run) add_roads(0);
	}
	void road_cleared(int32_t tx, int32_t ty) {
		mods_.push_back({tx + w_.tile_xs * ty, 0, 1});
		add_roads(0);
	}

	// ---- Roads::scan_and_kill_stray_roads @8956a0
	void scan_stray();

private:
	World& w_;
	std::vector<Mod> mods_;
	std::vector<int32_t> list_a_, list_b_;  // Roads +0x150 / +0x16c: tile indices whose arms must be redone
	int32_t cx_ = 0, cy_ = 0;               // Roads +0x120 / +0x124: the tile being processed
	std::array<int32_t, 9> blk_{}, road_{};  // RoadsOut +0xfc / +0x128 (fill_cache)
	// The scan's own caches (Roads +0x604 / +0x628 / +0x64c / +0x670); the road cache persists between tiles (an off-map tile leaves it stale).
	std::array<int32_t, 9> s_bld_{}, s_arm_{}, s_wat_{};
	std::array<int32_t, 9>& s_road_ = w_.roads.scan_road;  // Roads +0x604: persists between tiles, frames and (here) saves

	// ---------------------------------------------------------------- candidate storage (TerrainOut::get_road_data / road_changed)
	int32_t& slot_of(int32_t wx, int32_t wy) { return w_.roads.slot[w_.wi(wx, wy)]; }
	Words cell_words(int32_t wx, int32_t wy) {
		const int32_t s = slot_of(wx, wy);
		return s < 0 ? nullptr : &w_.roads.words[size_t(s) * RoadStore::kWords];
	}
	static Words cand_of(Words cell, int32_t idx) { return cell + 1 + 4 * idx; }
	static int32_t idx_of(int32_t tx, int32_t ty) { return (tx & 3) + 4 * (ty & 3); }

	Words alloc_cell(int32_t wx, int32_t wy) {
		RoadStore& r = w_.roads;
		int32_t s;
		if (!r.free_slots.empty()) {
			s = r.free_slots.back();
			r.free_slots.pop_back();
		} else {
			s = int32_t(r.words.size() / RoadStore::kWords);
			r.words.resize(r.words.size() + RoadStore::kWords);
		}
		Words c = &r.words[size_t(s) * RoadStore::kWords];
		c[0] = 0;  // num_roads
		for (int i = 0; i < 16; i++) {  // RoadElementCandidate::RoadElementCandidate @49bd30
			Words k = cand_of(c, i);
			k[0] = 0xffffffffu;
			k[1] = 0;
			k[2] = 10;
			k[3] = 0;
		}
		r.slot[w_.wi(wx, wy)] = s;
		return c;
	}
	void free_cell(int32_t wx, int32_t wy) {
		RoadStore& r = w_.roads;
		const int32_t s = r.slot[w_.wi(wx, wy)];
		if (s < 0) return;
		std::fill_n(r.words.begin() + ptrdiff_t(s) * RoadStore::kWords, RoadStore::kWords, 0u);
		r.free_slots.push_back(s);
		r.slot[w_.wi(wx, wy)] = -1;
	}

	// TerrainOut::get_road_data @8744d0
	Words get_road_data(int32_t tx, int32_t ty, int alloc, int p4, int p5) {
		if (!w_.in_t(tx, ty)) return nullptr;  // (the original indexes blindly; every caller checks the map edge first)
		Words cell = cell_words(tx >> 2, ty >> 2);
		const int32_t idx = idx_of(tx, ty);
		if (cell && cell[0] != 0) {
			Words c = cand_of(cell, idx);
			if (rot(c) != 10) {
				if (p5 != 0) {
					if (p4 == 0) {
						ref_a(c)++;
						return c;
					}
					ref_b(c)++;
				}
				return c;
			}
		}
		if (alloc == 0) return nullptr;
		road_changed(tx, ty, 1, p4, 0);
		return cand_of(cell_words(tx >> 2, ty >> 2), idx);
	}

	// TerrainOut::road_changed @866a70
	int road_changed(int32_t tx, int32_t ty, int add, int p4, int p5) {
		const int32_t wx = tx >> 2, wy = ty >> 2, idx = idx_of(tx, ty);
		Words cell = cell_words(wx, wy);
		if (add != 0) {
			if (!cell) cell = alloc_cell(wx, wy);
			Words c = cand_of(cell, idx);
			if (rot(c) == 10) {
				flags(c) = 0;
				rot(c) = 9;
				cell[0]++;
				if (p4 == 0) {
					if (p5 == 0) ref_a(c)++;
				} else if (p5 == 0) {
					ref_b(c)++;
				}
			} else if (p5 == 0) {
				ref_a(c)++;
			}
			return int(ref_a(c)) + int(ref_b(c));
		}
		if (!cell || cell[0] == 0) return 0;
		Words c = cand_of(cell, idx);
		if (rot(c) == 10) return 0;
		if (ref_b(c) == 0) ref_a(c)--;
		else ref_b(c)--;
		const int ra = ref_a(c), rb = ref_b(c);
		if (ra == 0 && rb == 0) {
			set_element_num(c, 0);
			rot(c) = 10;
			set_support(c, 0);
			terrain_creation(c) = 0;
			cell[0]--;
		}
		if (cell[0] == 0) free_cell(wx, wy);
		return ra + rb;
	}

	// ---------------------------------------------------------------- RoadsOut
	// RoadsOut::fill_cache @893b30
	void fill_cache(int32_t x, int32_t y) {
		for (int k = 0; k < 9; k++) {
			const int32_t nx = x + move_x[k], ny = y + move_y[k];
			if (nx < 0 || ny < 0 || nx >= w_.tile_xs || ny >= w_.tile_ys) {
				blk_[size_t(k)] = 0;
				road_[size_t(k)] = 0;
				continue;
			}
			const uint16_t m = w_.t(nx, ny);
			const int32_t rd = (m & 0x30) == 0x10;
			road_[size_t(k)] = rd;
			blk_[size_t(k)] = ((m & 3) == 3 && rd == 0) ? 1 : 0;
		}
	}
	// RoadsOut::get_orthog_connects @893560
	int get_orthog_connects(int32_t x, int32_t y) {
		Words c = get_road_data(x, y, 0, 0, 0);
		if (!c) return 0;
		const uint32_t f = flags(c);
		return int((f >> 30) & 1) + ((f & kS) ? 1 : 0) + ((f & kE) ? 1 : 0) + ((f & kW) ? 1 : 0);
	}
	std::vector<int32_t>& list_of(int sel) { return sel == 1 ? list_a_ : list_b_; }
	void add_unique(int sel, int32_t tx, int32_t ty) {
		std::vector<int32_t>& l = list_of(sel);
		const int32_t v = tx + w_.tile_xs * ty;
		if (std::find(l.begin(), l.end(), v) == l.end()) l.push_back(v);
	}
	// RoadsOut::set_neighbor @890c30
	int set_neighbor(int32_t x, int32_t y, uint32_t bits, int alloc, uint32_t clear) {
		Words c = get_road_data(x, y, alloc, 0, 0);
		if (c && (bits & flags(c)) == 0) {
			flags(c) |= bits;
			if (clear != 0xffffffffu) flags(c) &= ~clear;
			return 1;
		}
		return 0;
	}
	// "if the neighbour has no `arm` yet, set it and queue the neighbour" (the pattern of mark_and_trim_directions / mark_splits)
	void arm_neighbour(int32_t x, int32_t y, uint32_t arm, int p1, Words nb) {
		if (nb && (flags(nb) & arm) == 0) {
			flags(nb) |= arm;
			if (p1 != 0) add_unique(p1, x, y);
		}
	}

	void mark_and_trim_directions(int p1);
	void mark_splits(int p1);
	void leech_codes();
	int set_diags(int p1, int p2);
	int clear_support(int32_t x, int32_t y, int p3);
	void clear_roads(Mod& m);
	void redo_changed_roads(std::vector<int32_t>& list, int phase, int p3);
	void add_roads(int p1);
	// The candidate of the tile being processed (RoadsOut +0x14c). The original dereferences it unchecked; a scratch record keeps a
	// missing one (a state the original never reaches) from crashing the port.
	Words cur() {
		if (Words c = get_road_data(cx_, cy_, 0, 0, 0)) return c;
		scratch_ = {0xffffffffu, 0, 10, 0};
		return scratch_.data();
	}
	std::array<uint32_t, 4> scratch_{};

	// ---- the scan
	void scan_bad(int32_t tx, int32_t ty, Words c);
	void scan_straggled(int32_t tx, int32_t ty);
};

// RoadsOut::mark_and_trim_directions @8935c0
void Roads::mark_and_trim_directions(int p1) {
	int ns = 0, ew = 0;
	if (blk_[2] != 0 || blk_[6] != 0) ns = 1;
	if (blk_[4] != 0 || blk_[8] != 0) ew = 1;
	bool block_a = true;  // the N / S arms
	if (ns + ew == 0) ns = 1;
	else if (ew == 0) block_a = false;
	if (block_a) {
		if (blk_[2] == 0) {
			flags(cur()) |= kN;
			if (road_[2] == 0) {
				flags(cur()) &= ~kN;
			} else {
				Words nb = get_road_data(cx_, cy_ - 1, 1, 0, 0);
				arm_neighbour(cx_, cy_ - 1, kS, p1, nb);
			}
		}
		if (blk_[6] == 0) {
			flags(cur()) |= kS;
			if (road_[6] == 0) {
				flags(cur()) &= ~kS;
			} else {
				Words nb = get_road_data(cx_, cy_ + 1, 1, 0, 0);
				arm_neighbour(cx_, cy_ + 1, kN, p1, nb);
			}
		}
	}
	if (ns != 0) {
		if (blk_[4] == 0) {
			flags(cur()) |= kE;
			if (road_[4] == 0) {
				flags(cur()) &= ~kE;
			} else {
				Words nb = get_road_data(cx_ + 1, cy_, 1, 0, 0);
				arm_neighbour(cx_ + 1, cy_, kW, p1, nb);
			}
		}
		if (blk_[8] == 0) {
			flags(cur()) |= kW;
			if (road_[8] == 0) {
				flags(cur()) &= ~kW;
			} else {
				Words nb = get_road_data(cx_ - 1, cy_, 1, 0, 0);
				arm_neighbour(cx_ - 1, cy_, kE, p1, nb);
			}
		}
	}
	const uint32_t f = flags(cur());
	const int arms = int((f >> 30) & 1) + ((f & kE) ? 1 : 0) + ((f & kS) ? 1 : 0) + ((f & kW) ? 1 : 0);
	if (arms == 1) {  // a dead end: join every road neighbour
		if (road_[2] != 0) {
			flags(cur()) |= kN;
			arm_neighbour(cx_, cy_ - 1, kS, p1, get_road_data(cx_, cy_ - 1, 1, 0, 0));
		}
		if (road_[6] != 0) {
			flags(cur()) |= kS;
			arm_neighbour(cx_, cy_ + 1, kN, p1, get_road_data(cx_, cy_ + 1, 1, 0, 0));
		}
		if (road_[4] != 0) {
			flags(cur()) |= kE;
			arm_neighbour(cx_ + 1, cy_, kW, p1, get_road_data(cx_ + 1, cy_, 1, 0, 0));
		}
		if (road_[8] != 0) {
			flags(cur()) |= kW;
			arm_neighbour(cx_ - 1, cy_, kE, p1, get_road_data(cx_ - 1, cy_, 1, 0, 0));
		}
	}
}

// RoadsOut::mark_splits @891d40: a T junction keeps the arm of the side it has a road on. The low byte of the neighbour candidate
// (its intersection flags, presentation) is cleared where the original does; that byte is not modelled, the clears are kept anyway.
void Roads::mark_splits(int p1) {
	auto low_clear = [&](Words c) {
		if (c) flags(c) &= ~0xffu;
	};
	auto drop = [&](uint32_t arm) {  // LAB_00891e1d / LAB_00891ef5 / ...: the neighbour candidate is missing
		if (Words c = cur()) flags(c) &= ~arm;
	};
	// One "extend towards side X" step. Returns false when the original returns from mark_splits.
	// arm_self: the arm of this tile; arm_nb: the matching arm of the neighbour; road: the side's road cache index.
	auto extend = [&](uint32_t arm_self, uint32_t arm_nb, int road_idx, int32_t nx, int32_t ny, bool use_set_neighbor) -> int {
		// 1 = continue, 0 = return
		if (road_[size_t(road_idx)] == 0) return 2;  // side not taken (the caller decides what a missing road means)
		flags(cur()) |= arm_self;
		if (use_set_neighbor) {
			const int r = set_neighbor(nx, ny, arm_nb, 0, 0xffffffffu);
			if (r != 0 && p1 != 0) add_unique(p1, nx, ny);
		} else {
			arm_neighbour(nx, ny, arm_nb, p1, get_road_data(nx, ny, 0, 0, 0));
		}
		Words nb = get_road_data(nx, ny, 0, 0, 0);
		if (!nb) {
			drop(arm_self);
			return 0;
		}
		low_clear(nb);
		return 1;
	};
	Words c0 = cur();
	const uint32_t f = flags(c0);
	if ((f & 0x41000000u) == 0x41000000u) {  // N and W arms: take S and E
		if (road_[6] != 0) {
			if (extend(kS, kN, 6, cx_, cy_ + 1, false) == 0) return;
		}
		if (road_[4] == 0) return;
		extend(kE, kW, 4, cx_ + 1, cy_, false);
		return;
	}
	if ((f & 0x50000000u) == 0x50000000u) {  // N and E arms: take S and W
		if (road_[6] != 0) {
			if (extend(kS, kN, 6, cx_, cy_ + 1, false) == 0) return;
		}
		if (road_[8] == 0) return;
		extend(kW, kE, 8, cx_ - 1, cy_, false);
		return;
	}
	if ((f & kS) != 0 && (f & kW) != 0) {  // S and W arms: take N and E
		if (road_[2] != 0) {
			if (extend(kN, kS, 2, cx_, cy_ - 1, false) == 0) return;
		}
		if (road_[4] != 0) {
			if (extend(kE, kW, 4, cx_ + 1, cy_, true) == 0) return;
		}
	}
	const uint32_t f2 = flags(cur());
	if ((f2 & kS) == 0 || (f2 & kE) == 0) return;  // S and E arms: take N and W
	if (road_[2] != 0) {
		if (extend(kN, kS, 2, cx_, cy_ - 1, false) == 0) return;
	}
	if (road_[8] == 0) return;
	extend(kW, kE, 8, cx_ - 1, cy_, true);
}

// RoadsOut::leech_codes @890da0 (copies arms of the four road neighbours; the line bits come along)
void Roads::leech_codes() {
	struct Side {
		int road_idx;
		int32_t dx, dy;
		uint32_t arm_self, arm_nb;       // 'nb has its arm back' -> this tile gets its arm
		uint32_t map[4][2];              // {neighbour bit, own bit}
	};
	static const Side sides[4] = {
	    {2, 0, -1, kN, kS, {{0x400, 0x8000}, {0x800, 0x4000}, {0x40000, 0x800000}, {0x80000, 0x400000}}},
	    {6, 0, 1, kS, kN, {{0x8000, 0x400}, {0x4000, 0x800}, {0x800000, 0x40000}, {0x400000, 0x80000}}},
	    {4, 1, 0, kE, kW, {{0x100, 0x2000}, {0x200, 0x1000}, {0x10000, 0x200000}, {0x20000, 0x100000}}},
	    {8, -1, 0, kW, kE, {{0x2000, 0x100}, {0x1000, 0x200}, {0x200000, 0x10000}, {0x100000, 0x20000}}},
	};
	for (const Side& s : sides) {
		if (road_[size_t(s.road_idx)] == 0) continue;
		Words nb = get_road_data(cx_ + s.dx, cy_ + s.dy, 0, 0, 0);
		if (!nb) {
			if (Words c = cur()) flags(c) &= ~s.arm_self;
			return;
		}
		const uint32_t nf = flags(nb);
		Words c = cur();
		if (nf & s.arm_nb) flags(c) |= s.arm_self;
		for (const auto& m : s.map)
			if (nf & m[0]) flags(c) |= m[1];
	}
}

// Roads::set_diags @88e9d0. Returns RoadsOut's local_8 (1 when a diagonal step was joined).
int Roads::set_diags(int p1, int p2) {
	const int orth = get_orthog_connects(cx_, cy_);
	if (orth > 1) return 0;
	if (!get_road_data(cx_, cy_, 0, 0, 0)) return 0;
	int result = 0;
	for (int k = 1; k <= 4; k++) {
		const int32_t nx = cx_ + kCornerX[k], ny = cy_ + kCornerY[k];
		if (nx < 0 || ny < 0 || nx >= w_.tile_xs || ny >= w_.tile_ys) continue;
		const uint32_t f = flags(cur());
		if ((f & kN) && (k == 1 || k == 2)) continue;
		if ((f & kE) && (k == 2 || k == 3)) continue;
		if ((f & kS) && (k == 3 || k == 4)) continue;
		if ((f & kW) && (k == 4 || k == 1)) continue;
		if (road_[size_t(kDiagIdx[k])] == 0) continue;
		const int n2 = get_orthog_connects(nx, ny);
		bool queue_neighbour = false;
		if (n2 < 2) {
			result = 1;
			const uint32_t fl = flags(cur());
			if ((kBlock1[k] & fl) == 0 && (kBlock2[k] & fl) == 0) {
				flags(cur()) = kSetOwn[k] | fl;
				const int r = set_neighbor(nx, ny, kSetNeighbour[k], 1, 0xffffffffu);
				if (r != 0 && p1 != 0) queue_neighbour = true;
			}
		} else if (road_[size_t(kOrth1[k])] == 0 && blk_[size_t(kOrth1[k])] == 0 && road_[size_t(kOrth2[k])] == 0 && blk_[size_t(kOrth2[k])] == 0) {
			const int32_t kx = cx_ + kCornerX[k];
			w_.set_road_at(kx, cy_, true, true);
			road_added(kx, cy_, false);
			road_changed(kx, cy_, 1, 0, 0);
			if (p1 != 0) add_unique(p1, kx, cy_);
			Words corner = get_road_data(kx, cy_, 0, 0, 0);
			if (!corner) return 0;
			terrain_creation(corner) = 1;
			Words own = cur();
			set_support(own, uint16_t(support(own) | 0x2000));
			if (kCornerX[k] == 1) set_support(own, uint16_t(support(own) | 0x80));
			if (p1 != 0) {
				add_unique(p1, kx, cy_);
				add_unique(p1, cx_, cy_);
				queue_neighbour = true;
			}
		}
		if (queue_neighbour) add_unique(p1, nx, ny);
	}
	if (result == 0) return 0;
	// The diagonal pieces: candidates (element "C4") on the two orthogonal tiles next to each diagonal arm. get_road_data(.., 1, p2, 1) takes a
	// reference on the candidate (creating it when it is missing); the first of each pair is null-checked (return 0), the second is not.
	auto gp = [&](int32_t px, int32_t py, int rotation) -> bool {
		Words c = get_road_data(px, py, 1, p2, 1);
		if (!c) return false;
		set_element_num(c, kElementC4);
		rot(c) = uint8_t(rotation);
		return true;
	};
	auto sup = [&](uint16_t bits) {
		Words own = cur();
		set_support(own, uint16_t(support(own) | bits));
	};
	const int32_t x = cx_, y = cy_;
	uint32_t f = flags(cur());
	if (f & kW) {
		if (f & kSE) {
			if (!gp(x, y + 1, 0)) return 0;
			sup(0xc000);
			sup(0x1000);
			gp(x + 1, y, 2);
			sup(0x200);
		}
		if (flags(cur()) & kNE) {
			if (!gp(x, y - 1, 1)) return 0;
			sup(0xc000);
			sup(0x1800);
			gp(x + 1, y, 3);
			sup(0x200);
		}
	}
	f = flags(cur());
	if (f & kN) {
		if (f & kSW) {
			if (!gp(x - 1, y, 1)) return 0;
			sup(0xc000);
			gp(x, y + 1, 3);
			sup(0x400);
		}
		if (flags(cur()) & kSE) {
			if (!gp(x + 1, y, 2)) return 0;
			sup(0xc000);
			sup(0x800);
			gp(x, y + 1, 0);
			sup(0x400);
		}
	}
	f = flags(cur());
	if (f & kE) {
		if (f & kNW) {
			if (!gp(x, y - 1, 2)) return 0;
			sup(0xc000);
			sup(0x1800);
			gp(x - 1, y, 0);
		}
		if (flags(cur()) & kSW) {
			if (!gp(x, y + 1, 3)) return 0;
			sup(0xc000);
			sup(0x1000);
			gp(x - 1, y, 1);
		}
	}
	f = flags(cur());
	if (f & kS) {
		if (f & kNE) {
			if (!gp(x + 1, y, 3)) return 0;
			sup(0xc000);
			sup(0x800);
			gp(x, y - 1, 1);
			sup(0x600);
		}
		if (flags(cur()) & kNW) {
			if (!gp(x - 1, y, 0)) return 0;
			sup(0xc000);
			gp(x, y - 1, 2);
			sup(0x600);
		}
	}
	if ((flags(cur()) & 0x88000000u) == 0x88000000u) {
		if (!gp(x - 1, y, 0)) return 0;
		sup(0xc000);
		gp(x + 1, y, 2);
		sup(0x200);
	}
	if ((flags(cur()) & 0x22000000u) == 0x22000000u) {
		if (!gp(x - 1, y, 1)) return 0;
		sup(0xc000);
		gp(x + 1, y, 3);
		sup(0x200);
	}
	return result;
}

// Roads::clear_support @88e3f0
int Roads::clear_support(int32_t x, int32_t y, int p3) {
	Words c = get_road_data(x, y, 0, 0, 0);
	if (!c) return 0;
	const uint16_t sc = support(c);
	const int term = terrain_creation(c) != 0;
	if ((sc & 0xe000) == 0) {
		set_support(c, 0);
		return term;
	}
	auto drop_support = [&](int32_t sx, int32_t sy) {
		Words p = get_road_data(sx, sy, 0, 0, 0);
		if (!p) return;
		const int t = terrain_creation(p) != 0;
		const int n = road_changed(sx, sy, 0, 0, 1);
		if (n == 0 && t != 0) w_.set_road_at(sx, sy, false, true);
	};
	if (sc & 0x8000) {
		int32_t dx = -1, dy;
		if ((sc & 0x1000) == 0) {
			dy = 0;
			if (sc & 0x800) dx = 1;
		} else {
			dx = 0;
			dy = (sc & 0x800) ? -1 : 1;
		}
		drop_support(x + dx, y + dy);
	}
	if (sc & 0x4000) {
		int32_t dx = -1, dy;
		if ((sc & 0x400) == 0) {
			dy = 0;
			if (sc & 0x200) dx = 1;
		} else {
			dx = 0;
			dy = (sc & 0x200) ? -1 : 1;
		}
		if (Words own = get_road_data(x, y, 0, 0, 0)) set_support(own, 0);
		drop_support(x + dx, y + dy);
	}
	if (p3 == 0 && (sc & 0x2000)) {
		int32_t dx = -1, dy;
		if ((sc & 0x100) == 0) {
			dy = 0;
			if (sc & 0x80) dx = 1;
		} else {
			dx = 0;
			dy = (sc & 0x80) ? -1 : 1;
		}
		if (Words own = get_road_data(x, y, 0, 0, 0)) set_support(own, 0);
		drop_support(x + dx, y + dy);
	}
	return term;
}

// Roads::clear_roads @88fef0
void Roads::clear_roads(Mod& m) {
	cx_ = m.index % w_.tile_xs;
	cy_ = m.index / w_.tile_xs;
	const int sup = clear_support(cx_, cy_, 0);
	fill_cache(cx_, cy_);
	const int n = road_changed(cx_, cy_, 0, 0, 1);
	if (sup != 0 && n == 0) w_.set_road_at(cx_, cy_, false, true);
	if (road_[2] != 0) add_unique(1, cx_, cy_ - 1);
	if (road_[6] != 0) add_unique(1, cx_, cy_ + 1);
	if (road_[4] != 0) add_unique(1, cx_ + 1, cy_);
	if (road_[8] != 0) add_unique(1, cx_ - 1, cy_);
	m.valid = 0;
}

// Roads::redo_changed_roads @88e670
void Roads::redo_changed_roads(std::vector<int32_t>& list, int phase, int p3) {
	const std::vector<int32_t> items = list;  // the lists are only appended to through the other selector (phase)
	for (int32_t v : items) clear_support(v % w_.tile_xs, v / w_.tile_xs, 1);
	for (int32_t v : items) {
		cx_ = v % w_.tile_xs;
		cy_ = v / w_.tile_xs;
		Words c = get_road_data(cx_, cy_, 0, 0, 0);
		if (!c) continue;
		flags(c) = 0;
		fill_cache(cx_, cy_);
		mark_and_trim_directions(phase);
	}
	for (int32_t v : items) {
		cx_ = v % w_.tile_xs;
		cy_ = v / w_.tile_xs;
		if (!get_road_data(cx_, cy_, 0, 0, 0)) continue;
		fill_cache(cx_, cy_);
		leech_codes();
		if (set_diags(phase, p3) == 0) {
			fill_cache(cx_, cy_);
			mark_splits(phase);
		}
	}
	// (the fourth pass is the presentation: fixup_lines, delete_straglers, find_piece)
}

// Roads::add_roads @88f4b0
void Roads::add_roads(int p1) {
	std::vector<Mod> remove_list;
	// ---- pass 1: removals and creations of candidates
	for (size_t i = 0; i < mods_.size(); i++) {
		Mod m = mods_[i];
		cx_ = m.index % w_.tile_xs;
		cy_ = m.index / w_.tile_xs;
		if (!m.valid) continue;
		if (m.added == 0) {
			clear_roads(m);
			for (Mod& e : mods_) {
				if (e.index == m.index && e.valid == 1) {
					e.valid = 0;
					break;
				}
			}
		} else {
			fill_cache(cx_, cy_);
			const int n = road_changed(cx_, cy_, 1, 0, p1 != 0);
			if (n < 2) {
				if (Words c = get_road_data(cx_, cy_, 0, 0, 0)) {
					(void)c;
					mark_and_trim_directions(1);
				}
			} else {
				mods_.push_back({cx_ + w_.tile_xs * cy_, 0, 1});
				remove_list.push_back(m);
			}
		}
	}
	for (const Mod& r : remove_list) {
		for (size_t j = 0; j < mods_.size(); j++) {
			if (mods_[j].index == r.index && mods_[j].added == r.added) {
				mods_.erase(mods_.begin() + ptrdiff_t(j));
				break;
			}
		}
	}
	// ---- pass 2: arms, diagonal steps, splits
	for (size_t i = 0; i < mods_.size(); i++) {
		const Mod m = mods_[i];
		if (!m.valid) continue;
		cx_ = m.index % w_.tile_xs;
		cy_ = m.index / w_.tile_xs;
		if (!get_road_data(cx_, cy_, 0, 0, 0)) continue;
		if (m.added == 0) mark_and_trim_directions(1);
		fill_cache(cx_, cy_);
		if (set_diags(1, p1) == 0) {
			fill_cache(cx_, cy_);
			mark_splits(1);
		}
	}
	// (pass 3 is the presentation: fixup_lines, the line-bit trims, find_piece)
	// ---- the changed lists, up to 11 rounds
	bool toggle = false;
	std::vector<int32_t>* list = &list_a_;
	int phase = 2;
	int rounds = 0;
	while (!list->empty()) {
		rounds++;
		std::vector<int32_t> snapshot = *list;
		redo_changed_roads(snapshot, phase, p1);
		list->clear();
		toggle = !toggle;
		if (toggle) {
			phase = 1;
			list = &list_b_;
		} else {
			phase = 2;
			list = &list_a_;
		}
		if (rounds > 10) break;
	}
	list_a_.clear();
	list_b_.clear();
	mods_.clear();
}

// RoadsOut / Roads: the scan ------------------------------------------------------------------------------------------------

// Roads::scan_and_kill_bad_tcoord @88e100. `c` is the scanned tile's candidate (nullptr: none).
void Roads::scan_bad(int32_t tx, int32_t ty, Words c) {
	auto kill = [&]() {
		road_changed(tx, ty, 0, 0, 1);
		w_.set_road_at(tx, ty, false);
	};
	if (!c || rot(c) == 10) {
		if (s_road_[0] != 0) w_.set_road_at(tx, ty, false);
		return;
	}
	if (terrain_creation(c) != 0 || element_num(c) == kElementC4) {
		for (int k = 0; k < 9; k++)
			if (s_road_[size_t(k)] != 0) return;
		kill();
		return;
	}
	if (s_road_[0] == 0) {
		kill();
		return;
	}
	int count = 0;
	for (int k = 1; k <= 8; k++) {
		if (s_arm_[size_t(k)] == 0) continue;
		count++;
		const int32_t nx = tx + move_x[k], ny = ty + move_y[k];
		if (!w_.in_t(nx, ny) || s_road_[size_t(k)] == 0) {
			kill();
			return;
		}
	}
	if (count == 0) kill();
}

// Roads::scan_and_kill_straggled_tcoord @88e050: a lone arm with no building and no water / river next to it.
void Roads::scan_straggled(int32_t tx, int32_t ty) {
	int arms = 0, water = 0, bld = 0;
	for (int k = 1; k <= 8; k++) {
		if (s_arm_[size_t(k)] != 0 && s_road_[size_t(k)] == 0) {
			road_changed(tx, ty, 0, 0, 1);
			w_.set_road_at(tx, ty, false);
			return;
		}
		if (s_arm_[size_t(k)] != 0) arms++;
		if (s_wat_[size_t(k)] != 0) water++;
		if (s_bld_[size_t(k)] != 0) bld++;
	}
	if (arms == 1 && bld == 0 && water == 0) {
		road_changed(tx, ty, 0, 0, 1);
		w_.set_road_at(tx, ty, false);
	}
}

void Roads::scan_stray() {
	int32_t budget = w_.size / 500;
	if (budget < 1) return;
	RoadStore& r = w_.roads;
	while (budget-- > 0) {
		r.scan_x++;
		if (r.scan_x >= w_.xs) {
			r.scan_x = 0;
			r.scan_y++;
			if (r.scan_y >= w_.ys) r.scan_y = 0;
		}
		const int32_t wx = r.scan_x, wy = r.scan_y;
		for (int i = 0; i < 16; i++) {
			const int32_t tx = (i & 3) + wx * 4, ty = (i >> 2) + wy * 4;
			Words cell = cell_words(wx, wy);
			Words cand = cell ? cand_of(cell, i) : nullptr;
			Words own = (cell && rot(cand) != 10) ? cand : nullptr;
			for (int k = 0; k < 9; k++) {
				const int32_t nx = tx + move_x[k], ny = ty + move_y[k];
				if (nx < 0 || ny < 0 || nx >= w_.tile_xs || ny >= w_.tile_ys) {
					// @895875: the original zeroes Roads +0x128 + 4k (the fill_cache road cache) here, not +0x604 + 4k: the scan's road
					// cache keeps the previous tile's value for an off-map neighbour.
					road_[size_t(k)] = 0;
					s_bld_[size_t(k)] = 0;
					s_wat_[size_t(k)] = 0;
					s_arm_[size_t(k)] = 0;
					continue;
				}
				const uint16_t m = w_.t(nx, ny);
				s_road_[size_t(k)] = (m & 0x30) == 0x10;
				s_bld_[size_t(k)] = (m & 3) == 3;
				s_wat_[size_t(k)] = (m & 0x800) != 0 || (m & 0x30) == 0x20;
				if (s_road_[size_t(k)] == 0 || !cell || !own) s_arm_[size_t(k)] = 0;
				else s_arm_[size_t(k)] = (kArm[k] & flags(own)) != 0;
			}
			scan_bad(tx, ty, own);
			if (s_road_[0] != 0) scan_straggled(tx, ty);
		}
	}
}

}  // namespace

void roads_road_added(World& w, int32_t tx, int32_t ty) {
	Roads r(w);
	r.road_added(tx, ty, true);
}

void roads_road_cleared(World& w, int32_t tx, int32_t ty) {
	Roads r(w);
	r.road_cleared(tx, ty);
}

void roads_scan_and_kill_stray(World& w) {
	Roads r(w);
	r.scan_stray();
}

}  // namespace ron::sim
