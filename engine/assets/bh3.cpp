#include "assets/bh3.hpp"

#include <cmath>

namespace ron {

namespace {

// Chunk payload "u32 n; then n * width floats", keeping the first `keep` components of each element.
std::vector<float> read_float_array(const Bytes& b, const Chunk& c, int width, int keep) {
	const uint32_t n = read_u32(b, c.start);
	if (c.start + 4 + size_t(n) * width * 4 > c.end) throw FormatError("float array overruns chunk");
	std::vector<float> out;
	out.reserve(size_t(n) * keep);
	for (uint32_t i = 0; i < n; i++) {
		for (int k = 0; k < keep; k++) out.push_back(read_f32(b, c.start + 4 + (size_t(i) * width + k) * 4));
	}
	return out;
}

// Node record, chunk id 7: u32 first, u32 count, u32 name_len, char name[name_len],
// f32 quat[4] (file convention: conjugated), f32 trans[3], f32 unused.
void read_node_record(const Bytes& b, const Chunk& c, BH3Node& node) {
	size_t p = c.start;
	node.first_vertex = read_u32(b, p);
	node.vertex_count = read_u32(b, p + 4);
	const uint32_t name_len = read_u32(b, p + 8);
	p += 12;
	if (p + name_len + 28 > c.end) throw FormatError("node record overruns chunk");
	node.name.assign(reinterpret_cast<const char*>(b.data() + p), name_len);
	node.name = std::string(node.name.c_str());  // drop the NUL terminator and anything after it
	p += name_len;
	node.rest.q = {-read_f32(b, p), -read_f32(b, p + 4), -read_f32(b, p + 8), read_f32(b, p + 12)};
	node.rest.t = {read_f32(b, p + 16), read_f32(b, p + 20), read_f32(b, p + 24)};
}

void read_hierarchy(const Bytes& b, const Chunk& c, BH3Node& node, int depth) {
	if (depth > 256) throw FormatError("hierarchy too deep");
	for (const Chunk& k : read_chunks(b, c.start, c.end)) {
		if (k.id == 7) {
			read_node_record(b, k, node);
		} else if (k.id == 6) {
			node.children.emplace_back();
			read_hierarchy(b, k, node.children.back(), depth + 1);
		}
	}
}

}  // namespace

BH3Model parse_bh3(const Bytes& b) {
	BH3Model model;
	for (const Chunk& c : read_top_level(b)) {
		if (c.id == 1) {
			BH3Mesh mesh;
			for (const Chunk& k : read_chunks(b, c.start, c.end)) {
				switch (k.id) {
					case 2: mesh.positions = read_float_array(b, k, 4, 3); break;
					case 3: mesh.normals = read_float_array(b, k, 4, 3); break;
					case 4: mesh.uvs = read_float_array(b, k, 2, 2); break;
					case 5: {
						const uint32_t n = read_u32(b, k.start);
						if (k.start + 4 + size_t(n) * 2 > k.end) throw FormatError("index array overruns chunk");
						mesh.indices.resize(n);
						for (uint32_t i = 0; i < n; i++) mesh.indices[i] = read_u16(b, k.start + 4 + size_t(i) * 2);
						break;
					}
					default: break;
				}
			}
			const size_t n = mesh.vertex_count();
			if (mesh.normals.size() != n * 3) mesh.normals.assign(n * 3, 0.0f);
			// Shipped meshes carry unnormalised normals and, in the tail vertices of many (lightcav.bh3, napoleon.bh3,
			// banner_0n.BH3...), uninitialised ones: denormals, zeros and NaNs. Normalising those gives NaN lighting,
			// and one NaN pixel blacks the whole frame through the exposure average (renderer.mm luminance chain).
			// Unit length here; a degenerate normal points up.
			for (size_t i = 0; i < n; i++) {
				float* nv = &mesh.normals[i * 3];
				const float len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
				if (std::isfinite(len) && len > 1e-6f) nv[0] /= len, nv[1] /= len, nv[2] /= len;
				else nv[0] = 0, nv[1] = 0, nv[2] = 1;
			}
			if (mesh.uvs.size() != n * 2) mesh.uvs.assign(n * 2, 0.0f);
			for (uint16_t i : mesh.indices) {
				if (i >= n) throw FormatError("triangle index out of range");
			}
			model.meshes.push_back(std::move(mesh));
		} else if (c.id == 6) {
			read_hierarchy(b, c, model.root, 0);
		}
	}
	if (model.meshes.empty()) throw FormatError("no mesh in BH3");
	return model;
}

}  // namespace ron
