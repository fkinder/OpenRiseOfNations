// Cliffs of the random-map generator (docs/randommap-spec.md "Fifth pass"): the templates of Data/Cliffs.xml
// (CliffsOut::init_templates @8a71a0), the placement the terrain groups call (Cliffs::position_cliff @8a8bc0, verify_placement
// @8a6310, add_cliff @8a6690, verify_defensive_position @8a8680) and the world effect of Cliffs::init @8a8020 (place_cliff @8a7ea0
// raises the heights, assess_wcoord @8a6a40 blocks the steep tcoords). Not ported: the Cliff render pieces and the
// CliffMiningData of Cliff::init, Cliffs::set_edge_flags (TData bit 0x8000 on the mining tcoords): they belong to the cliff
// gathering and the renderer, no random-map output of the generator depends on them. There is no original recording of a map with
// cliffs (the seven saves are styles without cliff groups), so this file is checked by reading and by running every style.
#pragma clang fp contract(off)
#include <cmath>
#include <mutex>

#include "assets/chunk.hpp"
#include "assets/tga.hpp"
#include "assets/xml.hpp"
#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

namespace {

constexpr int kOffTable[5] = {0, -1, -2, -4, -8};  // offtable @af2fb0: the template origin in wcoords
constexpr int kIndexToSize[5] = {4, 8, 16, 32, 64};  // index_to_size @af2fd0: the template side in tcoords

struct CliffTemplates {
	std::vector<CliffTemplate> t[5];
	bool ok = false;
};

std::mutex g_cliff_mutex;
std::map<std::string, CliffTemplates> g_cliff_cache;

CliffTemplates load_templates(const GameFiles& files) {
	CliffTemplates out;
	ron::for_each_element(ron::parse_xml(files.read_text("Data/Cliffs.xml")), "CLIFF", [&](const XmlElement& e) {
		const std::string path = files.resolve(normalize_game_path(e.get("path")));
		if (path.empty()) return;
		const Image img = parse_tga(read_file(path));
		const int D = img.width;
		if (D != img.height || D <= 0) return;
		const int nb = D / 4;
		CliffTemplate tp;
		tp.orientation = e.get_int("orientation", -1);
		bool holes = false;
		std::vector<uint8_t> hole(size_t(nb) * size_t(nb), 0);
		tp.heights.reserve(size_t(D) * size_t(D));
		for (int y = 0; y < D; y++) {
			for (int x = 0; x < D; x++) {
				const uint8_t* px = &img.rgba[(size_t(y) * size_t(D) + size_t(x)) * 4];
				if (px[3] == 0) {
					holes = true;
					hole[size_t(y / 4) * size_t(nb) + size_t(x / 4)] = 1;
				}
				tp.heights.push_back(float(px[2]) * 0.003921569f * 480.0f);  // the blue byte
			}
		}
		for (int by = 0; by < nb; by++) {
			for (int bx = 0; bx < nb; bx++) {
				bool inc;
				if (holes) {
					inc = hole[size_t(by) * size_t(nb) + size_t(bx)] != 0;
				} else {
					float hi = 0.0f, lo = 480.0f;
					for (int r = 0; r < 5; r++) {
						const int y = 4 * by - 1 + r;
						for (int c = 0; c < 6; c++) {
							const int x = 4 * bx - 1 + c;
							const float v = (x < 0 || y < 0 || x >= D || y >= D) ? 0.0f : tp.heights[size_t(y) * size_t(D) + size_t(x)];
							if (hi <= v) hi = v;
							if (v <= lo) lo = v;
						}
					}
					inc = hi - lo > 192.0f;
				}
				if (inc) tp.cells.push_back({bx, by});
			}
		}
		const int idx = D == 64 ? 4 : D == 32 ? 3 : D == 16 ? 2 : D == 8 ? 1 : 0;
		out.t[idx].push_back(std::move(tp));
	});
	out.ok = true;
	return out;
}

}  // namespace

const std::vector<CliffTemplate>* Gen::cliff_templates(int size) {
	if (size < 0 || size > 4) return nullptr;
	if (!cliffs_loaded) {
		std::lock_guard<std::mutex> lock(g_cliff_mutex);
		auto it = g_cliff_cache.find(c.files.root());
		if (it == g_cliff_cache.end()) it = g_cliff_cache.emplace(c.files.root(), load_templates(c.files)).first;
		for (int s = 0; s < 5; s++) cliff_tpl[s] = it->second.t[s];
		cliffs_loaded = true;
	}
	return &cliff_tpl[size];
}

// Cliffs::verify_placement @8a6310. mode != 0: off-map cells are tolerated and the first on-map cell passes.
bool Gen::verify_cliff_placement(int x, int y, int size, int idx, int mode) {
	const std::vector<CliffTemplate>* set = cliff_templates(size);
	if (!set || idx < 0 || size_t(idx) >= set->size()) return false;
	const CliffTemplate& tp = (*set)[size_t(idx)];
	const int off = kOffTable[size];
	for (const auto& cell : tp.cells) {
		const int cx = x + off + cell[0], cy = y + off + cell[1];
		if (!inb(cx, cy)) {
			if (mode == 0) return false;
			continue;
		}
		if (mode != 0) return true;
		for (int k = 1; k < 0x31; k++) {
			const int nx = cx + move_x[k], ny = cy + move_y[k];
			if (!inb(nx, ny)) continue;
			const uint16_t nf = W(nx, ny).flags;
			if (nf & 0x40) return false;
			if (k < 9 && (nf & 0x20)) return false;
		}
		const WData& d = W(cx, cy);
		const uint16_t f = d.flags;
		if ((f & 0x10) || (f & 0x28) || (f & 0x800)) return false;
		if ((f & 0x100) == 0 && (d.land == 2 || d.land == 1)) return false;
		if ((f & 0x1000) || (f & 0x40) || d.blocked != 0) return false;
	}
	return true;
}

// TerrainOut::is_clear_land @874b60 for the masks add_cliff passes (2, or 0x22 with mode != 0): no mountain, forest, rock, oil, open
// water or river, a cliff only when 0x20, and no blocked tcoord unless the mask has one of 0x165.
bool Gen::cliff_clear_land(int x, int y, unsigned mask) {
	const WData& d = W(x, y);
	const uint16_t f = d.flags;
	if ((f & 0x10) && !(mask & 0x40)) return false;
	if ((f & 0x20) && !(mask & 1)) return false;
	if ((f & 0x08) && !(mask & 4)) return false;
	if ((f & 0x800) && !(mask & 0x100)) return false;
	if (!(f & 0x100) && (d.land == 1 || d.land == 2) && !(mask & 8)) return false;
	if ((f & 0x1000) && !(mask & 0x10)) return false;
	if ((f & 0x40) && !(mask & 0x20)) return false;
	if (!(mask & 0x165) && d.blocked != 0) return false;
	return true;  // mask & 2 (always set for cliffs when it matters): the coastal test is skipped
}

// Cliffs::add_cliff @8a6690: 1 placed, -1 when the template does not fit. Marks the clear cells of the template CLIFF (0x40).
int Gen::add_cliff(int x, int y, int size, int idx, int mode) {
	if (!verify_cliff_placement(x, y, size, idx, mode)) return -1;
	placed_cliffs.push_back({x, y, size, idx});
	const CliffTemplate& tp = (*cliff_templates(size))[size_t(idx)];
	const int off = kOffTable[size];
	for (const auto& cell : tp.cells) {
		const int cx = x + off + cell[0], cy = y + off + cell[1];
		if (!inb(cx, cy)) continue;
		if (cliff_clear_land(cx, cy, mode == 0 ? 2u : 0x22u)) W(cx, cy).flags |= 0x40;
	}
	return 1;
}

// Cliffs::position_cliff @8a8bc0 (x, y, size index, the start the cliff defends or -1, mode, facing): the templates of the size
// whose orientation fits (1 NW, 2 NE, 3 SE, 4 SW from the start; FACE_ANY and unknown facings allow only orientation 1) and whose
// cells verify; one draw (when there are several) picks the template.
int Gen::position_cliff(int x, int y, int size, int sx, int sy, int mode, int facing) {
	const std::vector<CliffTemplate>* set = cliff_templates(size);
	int want = -1;
	// the table {1, 2, 4, 3} read at EDX + ECX = -2 (above, left of the start) .. 1 (below, right): NW, NE, SW, SE
	auto pick = [&](int dx, int dy) {
		const int e = (dy < 0 ? -2 : 0) + (dx >= 0 ? 1 : 0);
		return e == -2 ? 1 : e == -1 ? 2 : e == 0 ? 4 : 3;
	};
	if (mode == 0) {
		if (sx != -1) want = pick(x - sx, y - sy);
	} else if (unsigned(facing) < 2u) {
		if (sx != -1) want = pick(xs / 2 - sx, xs / 2 - sy);
	} else {
		want = 1;
	}
	std::vector<int> cands;
	if (set) {
		for (int i = 0; i < int(set->size()); i++) {
			if ((want == -1 || (*set)[size_t(i)].orientation == want) && verify_cliff_placement(x, y, size, i, mode)) cands.push_back(i);
		}
	}
	if (cands.empty()) return 0;
	const int pickn = cands.size() > 1 ? r.rnd_draw(int(cands.size())) : 0;
	return add_cliff(x, y, size, cands[size_t(pickn)], mode);
}

// Cliffs::verify_defensive_position @8a8680: the cliff lies within 22.5 degrees (facing the world centre) or 17.5 degrees (facing the
// middle of the starts) of the direction from the start the cliff defends to that point; FACE_ANY always passes.
bool Gen::verify_defensive_position(int cx, int cy, int sx, int sy, int facing) {
	if (facing == 2) return true;
	float lim;
	float ax, ay;  // the direction the cliff should lie in, relative to the start
	if (facing == 0) {
		ax = float(xs / 2 - sx);
		ay = float(ys / 2 - sy);
		lim = 22.5f;
	} else {
		int tx = 0, ty = 0;
		for (const WCoord& s : starts) {
			tx += s.x;
			ty += s.y;
		}
		const int n = int(starts.size());
		if (n <= 0) return false;
		ax = float(tx / n - sx);
		ay = float(ty / n - sy);
		lim = 17.5f;
	}
	float bx = float(cx - sx), by = float(cy - sy);
	float az = 0.0f, bz = 0.0f;
	float m = ax * ax + ay * ay + 0.0f;
	if (m != 0.0f && m != 1.0f) {
		m = 1.0f / std::sqrt(m);
		ax = ax * m;
		ay = ay * m;
		az = m * 0.0f;
	}
	m = by * by + bx * bx + 0.0f;
	if (m != 0.0f && m != 1.0f) {
		m = 1.0f / std::sqrt(m);
		bx = bx * m;
		by = by * m;
		bz = m * 0.0f;
	}
	const float dot = by * ay + bx * ax + bz * az;
	const float deg = float(std::acos(double(dot))) * 57.29578f;
	return std::fabs(deg) < lim;
}

// Cliffs::init @8a8020 as far as the world is concerned (called at the end of Terrain::init, after adjust_for_mountains).
void Gen::cliffs_init() {
	if (placed_cliffs.empty() || res.heights.empty()) return;
	const int vxs = res.height_vxs;
	// Cliffs::place_cliff: the template heights are added to the tcoord corners of the template, except on mountain, open water,
	// river and coastal wcoords
	for (const PlacedCliff& pc : placed_cliffs) {
		const CliffTemplate& tp = (*cliff_templates(pc.size))[size_t(pc.idx)];
		const int side = kIndexToSize[pc.size];
		const int x0 = (pc.x + kOffTable[pc.size]) * 4, y0 = (pc.y + kOffTable[pc.size]) * 4;
		int hi = 0;
		for (int ty = y0; ty < y0 + side; ty++) {
			for (int tx = x0; tx < x0 + side; tx++, hi++) {
				const int wx = tx >> 2, wy = ty >> 2;
				if (tx < 0 || ty < 0 || !inb(wx, wy)) continue;
				const WData& d = W(wx, wy);
				if (d.flags & 0x10) continue;
				if ((d.flags & 0x100) == 0 && (d.land == 1 || d.land == 2)) continue;
				if ((d.flags & 0x1000) || (coord_flags[size_t(wy) * size_t(xs) + size_t(wx)] & 4)) continue;
				float& h = res.heights[size_t(ty) * size_t(vxs) + size_t(tx)];
				h = tp.heights[size_t(hi)] + h;
			}
		}
	}
	// TerrainOut::has_plus_slope(tx, ty, 25 degrees): the four corner heights of the tcoord differ by more than tan(25) * 192
	const float limit = std::tan(25.0f * 0.017453292f) * 192.0f;
	auto steep = [&](int tx, int ty) {
		const float a = res.heights[size_t(ty) * size_t(vxs) + size_t(tx)], b = res.heights[size_t(ty) * size_t(vxs) + size_t(tx) + 1];
		const float cc = res.heights[size_t(ty + 1) * size_t(vxs) + size_t(tx)], dd = res.heights[size_t(ty + 1) * size_t(vxs) + size_t(tx) + 1];
		float lo = std::min(a, 10000.0f), hi2 = std::max(a, -10000.0f);
		lo = std::min(b, lo);
		hi2 = std::max(b, hi2);
		hi2 = std::max(cc, hi2);
		lo = std::min(cc, lo);
		hi2 = std::max(dd, hi2);
		lo = std::min(dd, lo);
		return hi2 - lo > limit;
	};
	// Cliffs::assess_wcoord for every wcoord, y outer
	for (int y = 0; y < ys; y++) {
		for (int x = 0; x < xs; x++) {
			if (!(W(x, y).flags & 0x40)) continue;
			int count = 0;
			for (int j = 0; j < 4; j++) {
				for (int i = 0; i < 4; i++) {
					const int tx = x * 4 + i, ty = y * 4 + j;
					const WData& d = W(x, y);
					if ((d.flags & 4) || d.land == 2 || d.land == 1) continue;
					if ((w.t(tx, ty) & 3) == 2) continue;
					if (!steep(tx, ty)) continue;
					count++;
					w.set_blocked_at(tx, ty, true);
					w.t(tx, ty) = uint16_t((w.t(tx, ty) & 0xfffd) | 1);
					for (int k = 1; k <= 8; k++) {
						const int nx = tx + move_x[k], ny = ty + move_y[k];
						if (nx < 0 || ny < 0 || nx >= w.tile_xs || ny >= w.tile_ys) continue;
						w.set_blocked_at(nx, ny, true);
						w.t(nx, ny) = uint16_t((w.t(nx, ny) & 0xfffd) | 1);
						move_goody_at(nx >> 2, ny >> 2);
					}
				}
			}
			if (count != 0) move_goody_at(x, y);
			else W(x, y).flags &= 0xffbf;
		}
	}
}

}  // namespace ron::sim::mapgen
