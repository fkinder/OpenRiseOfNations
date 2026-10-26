#include "sim/terrain_land.hpp"

#include <cstdlib>

#include "sim/compass.hpp"
#include "sim/game.hpp"
#include "sim/original_save.hpp"
#include "sim/rules_xml.hpp"

namespace ron::sim {

namespace {

bool iequal(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	return true;
}

std::string to_ascii(const ustr& s) {
	std::string r;
	for (char16_t c : s) r += c < 128 ? char(c) : '?';
	return r;
}

// TileSet::is_combo_acceptable @87abe0.
int32_t combo_acceptable(const TileLandInfo& ti, int32_t p1, int32_t p2, int32_t p3, int32_t p4) {
	// num_tiles is read at [land] for any land index; only 0..2 (BASELAND, COASTAL, OCEAN) are known: the other entries read 0.
	auto num = [&](int32_t l) { return l >= 0 && l < 3 ? ti.num_tiles[l] : 0; };
	if (p1 == p3) {
		if (std::abs(p4 - p2) < 2) return (p4 <= p2) + 1;
	} else {
		if (p1 == 0) {
			if (p3 == 1) return p2 == ti.coast_base ? 1 : 0;
		} else if (p1 == 1 && p3 == 0) {
			return p4 == ti.coast_base ? 2 : 0;
		}
		if (p3 == 2) return 1;
		if (p3 < p1) {
			if (p4 == num(p3) - 1) return p2 != 0 ? 0 : 2;
		} else if (p4 == 0) {
			return p2 == num(p1) - 1 ? 1 : 0;
		}
	}
	return 0;
}

struct Land {
	Game& g;
	const TileLandInfo& ti;
	World& w;
	int32_t depth = 0;

	bool inb(int32_t x, int32_t y) const { return x >= 0 && y >= 0 && x < w.xs && y < w.ys; }

	// Terrain::verify_land @84d8a0 (the presentation calls of the original - rebuild_terrain_lists, add_update_wcoord, invalidate_wcoord - left out).
	void verify_land(int32_t x, int32_t y, int32_t* pl, int32_t* ps) {
		depth++;
		if (depth > 500) return;  // (the original returns without leaving the level)
		bool changed[8] = {};
		for (int32_t k = 1; k <= 8; k++) {
			const int32_t nx = move_x[k] + x, ny = move_y[k] + y;
			if (!inb(nx, ny)) continue;
			WData& n = w.w(nx, ny);
			const int32_t nl = n.land, ns = n.land_sub;
			if (combo_acceptable(ti, *pl, *ps, nl, ns) != 0) continue;
			if (nl == 2 || nl == 1) {
				// The centre wcoord itself becomes the coast base of plain land and is verified again.
				WData& c = w.w(x, y);
				if (ti.coast_base >= 0) c.land_sub = uint8_t(ti.coast_base);
				c.land = 0;
				int32_t l = 0, s = ti.coast_base;
				verify_land(x, y, &l, &s);
				*pl = l;
				*ps = s;
			} else {
				int32_t tgt = ns;
				if (std::abs(*ps - ns) > 1) tgt = *ps <= ns ? ns - 1 : ns + 1;
				if (*pl == 0) {
					if (ns != tgt && ns != *ps) {
						if (tgt >= 0) n.land_sub = uint8_t(tgt);
						if (nl >= 0) n.land = int8_t(nl);
						changed[k - 1] = true;
					}
				} else if (*pl == 1 || *pl == 2) {
					if (ti.coast_base >= 0) n.land_sub = uint8_t(ti.coast_base);
					n.land = 0;
					changed[k - 1] = true;
				}
			}
		}
		for (int32_t k = 1; k <= 8; k++) {
			const int32_t nx = move_x[k] + x, ny = move_y[k] + y;
			if (!inb(nx, ny) || !changed[k - 1]) continue;
			int32_t l = w.w(nx, ny).land, s = w.w(nx, ny).land_sub;
			verify_land(nx, ny, &l, &s);
		}
		depth--;
	}

	// Terrain::spot_update_land @84fa50.
	void spot_update_land(int32_t x, int32_t y, int32_t nl, int32_t ns) {
		if (!inb(x, y)) return;
		WData& d = w.w(x, y);
		const int32_t ol = d.land, os = d.land_sub;
		if (ol == nl && os == ns) return;
		if (ns >= 0) d.land_sub = uint8_t(ns);
		if (nl >= 0) d.land = int8_t(nl);
		int32_t l = nl, s = ns;
		verify_land(x, y, &l, &s);
	}
};

}  // namespace

TileLandInfo load_tile_land_info(const GameFiles& files, const std::string& tileset) {
	TileLandInfo ti;
	const XDoc ts = read_game_xml(files, "tilesets.xml", "", false);
	if (!ts.root) return ti;
	for (const XNode* t : xml::children(ts.root, u"TILESET")) {
		if (!iequal(to_ascii(xml::get_attrib(t, u"name")), tileset)) continue;
		ti.known = true;
		ti.weatherbase = xml::get_attrib_num(t, u"weatherbase", 0);
		static const char16_t* names[3] = {u"BASELAND", u"COASTAL", u"OCEAN"};
		for (int li = 0; li < 3; li++)
			if (const XNode* land = xml::child(t, names[li])) ti.num_tiles[li] = int32_t(xml::children(land, u"BASE").size());
		if (const XNode* gr = xml::child(t, u"TERRAINGROUP")) ti.coast_base = xml::get_attrib_num(xml::child(gr, u"COAST_BASE"), u"value", -1);
		return ti;
	}
	return ti;
}

void terrain_object_placed(Game& g, int32_t tile_x, int32_t tile_y, int32_t xsize, int32_t ysize) {
	if (!g.original || !g.original->tile_land.known) return;
	Land L{g, g.original->tile_land, g.world};
	World& w = g.world;
	for (int32_t y = tile_y >> 2; y < ((ysize + tile_y) >> 2) + 1; y++) {
		for (int32_t x = tile_x >> 2; x < ((tile_x + xsize) >> 2) + 1; x++) {
			if (x < 0 || y < 0 || x >= w.xs || y >= w.ys) continue;
			if (w.w(x, y).land == 0) L.spot_update_land(x, y, 0, g.original->tile_land.weatherbase);
		}
	}
}

}  // namespace ron::sim
