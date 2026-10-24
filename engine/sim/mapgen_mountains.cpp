// TerrainOut::adjust_for_mountains(0) @8703c0 as far as it touches the height grid (docs/randommap-spec.md "Fifth pass").
//
// Terrain::init ends with adjust_for_mountains(0) once the rivers are done. For every mountain placed by Mountains::add_mountain
// (in placement order) it walks the range's face wcoords and, for each of the 5 x 5 grid vertices of such a wcoord, looks for the
// template vertex that lands on it (TerrainOut::fill_mountain_data @869380: |vertex + stamp location - 192 * (tx, ty)| < 0.01 in x
// and y, each template vertex matched at most once per mountain) and ADDS the template z to the stored height. Overlapping
// mountains therefore add their z one after the other. The template's z is MountainRange::init @8998b0:
// ((float)blue + 0.0) * (height * 3.0) / 255.0 on the 4-pixel lattice (x = ox + 4 i, y = oy + 4 j, alpha != 0).
#pragma clang fp contract(off)
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>

#include "assets/tga.hpp"
#include "assets/xml.hpp"
#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

namespace {

struct TVert {
	float x, y, z;
};

struct Template {
	std::vector<TVert> verts;
	bool ok = false;
};

// MOUNTAIN elements of the first <MOUNTAINS> section of effects_graphics.xml: (height, TEMPLATE_TEX path) per range index.
struct TemplateSource {
	std::vector<std::pair<float, std::string>> list;
};

std::mutex g_tpl_mutex;
std::map<std::string, std::map<int, Template>> g_tpl;  // per install root

TemplateSource read_sources(const GameFiles& files) {
	TemplateSource s;
	bool done = false;
	ron::for_each_element(ron::parse_xml(files.read_text("Data/effects_graphics.xml")), "MOUNTAINS", [&](const XmlElement& sec) {
		if (done) return;
		done = true;
		for (const XmlElement& m : sec.children) {
			if (m.name != "MOUNTAIN") continue;
			const XmlElement* t = m.child("TEMPLATE_TEX");
			s.list.push_back({std::strtof(m.get("height").c_str(), nullptr), t ? normalize_game_path(t->get("file")) : std::string()});
		}
	});
	return s;
}

Template build_template(const GameFiles& files, float height, const std::string& rel) {
	Template t;
	const std::string path = files.resolve(rel);
	if (path.empty()) return t;
	const Image img = parse_tga(read_file(path));
	const int W = img.width, H = img.height;
	const int ox = (W / 2) % 16, oy = (H / 2) % 16;
	const float h3 = height * 3.0f;
	for (int y = oy; y < H; y += 4) {
		for (int x = ox; x < W; x += 4) {
			const uint8_t* px = &img.rgba[(size_t(y) * size_t(W) + size_t(x)) * 4];
			if (px[3] == 0) continue;
			TVert v;
			v.x = float(x - W / 2) * 192.0f * 0.25f;
			v.y = float(y - H / 2) * 192.0f * 0.25f;
			v.z = ((float(px[2]) + 0.0f) * h3) / 255.0f;  // the blue byte (the texture is grey)
			t.verts.push_back(v);
		}
	}
	t.ok = true;
	return t;
}

}  // namespace

void Gen::mountain_heights() {
	if (res.heights.empty() || placed_mountains.empty()) return;
	std::lock_guard<std::mutex> lock(g_tpl_mutex);
	auto& cache = g_tpl[c.files.root()];
	static std::map<std::string, TemplateSource> sources;
	auto sit = sources.find(c.files.root());
	if (sit == sources.end()) sit = sources.emplace(c.files.root(), read_sources(c.files)).first;
	const TemplateSource& src = sit->second;
	const int vxs = res.height_vxs, vys = res.height_vys;
	auto fdiv = [](int v) { return v >= 0 ? v / 4 : -((-v + 3) / 4); };
	for (const PlacedMountain& pm : placed_mountains) {
		auto it = cache.find(pm.range);
		if (it == cache.end()) {
			Template t;
			if (pm.range >= 0 && size_t(pm.range) < src.list.size()) t = build_template(c.files, src.list[size_t(pm.range)].first, src.list[size_t(pm.range)].second);
			it = cache.emplace(pm.range, std::move(t)).first;
		}
		const Template& tp = it->second;
		if (!tp.ok) continue;
		// the range's face wcoords (the distinct wcoords of its complete cells, in face order)
		std::vector<Offset> faces;
		for (const Offset& t : pm.tcoords) {
			const Offset f{fdiv(t[0]), fdiv(t[1])};
			if (std::find(faces.begin(), faces.end(), f) == faces.end()) faces.push_back(f);
		}
		const float lx = float(pm.wx * 0x300), ly = float(pm.wy * 0x300);
		std::vector<uint8_t> emitted(tp.verts.size(), 0);
		for (const Offset& f : faces) {
			const int fx = pm.wx + f[0], fy = pm.wy + f[1];
			for (int j = 0; j < 5; j++) {
				for (int i = 0; i < 5; i++) {
					const int tx = fx * 4 + i, ty = fy * 4 + j;
					const float gx = float(tx) * 192.0f, gy = float(ty) * 192.0f;
					for (size_t k = 0; k < tp.verts.size(); k++) {
						if (emitted[k]) continue;
						const float px = tp.verts[k].x + lx, py = tp.verts[k].y + ly;
						if (std::fabs(px - gx) < 0.01f && std::fabs(py - gy) < 0.01f) {
							emitted[k] = 1;
							if (tx >= 0 && ty >= 0 && tx < vxs && ty < vys) {
								float& h = res.heights[size_t(ty) * size_t(vxs) + size_t(tx)];
								h = h + tp.verts[k].z;
							}
						}
					}
				}
			}
		}
	}
}

}  // namespace ron::sim::mapgen
