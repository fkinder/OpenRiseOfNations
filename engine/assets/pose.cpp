#include "assets/pose.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace ron {

namespace {

bool iequals(const std::string& a, const std::string& b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	}
	return true;
}

// Parses `name` as prefix + canonical decimal i (what prefix + std::to_string(i) builds), case-insensitively, without allocating
// (pose_node asked this of every node of every unit every frame; it was a quarter of the draw time). Returns i, or -1.
int suffix_index(const std::string& name, const char* prefix) {
	size_t k = 0;
	for (; prefix[k]; k++) {
		if (k >= name.size() || std::tolower(static_cast<unsigned char>(name[k])) != prefix[k]) return -1;
	}
	const size_t digits = name.size() - k;
	if (digits == 0 || digits > 2) return -1;
	if (name[k] < '0' || name[k] > '9') return -1;
	if (digits == 1) return name[k] - '0';
	if (name[k] == '0' || name[k + 1] < '0' || name[k + 1] > '9') return -1;  // "05" is not to_string(5)
	return (name[k] - '0') * 10 + (name[k + 1] - '0');
}

// Slot a node name selects, or -1. The four patterns of the game's order for i = 0..13 (a name matches at most one, so the order is moot).
int slot_for_name(const std::string& name) {
	static const struct {
		const char* prefix;
		int offset;
	} patterns[] = {{"ammo node ", 0}, {"pivot node ", 4}, {"smoke node ", 8}, {"node ", 0}};
	for (const auto& p : patterns) {
		const int i = suffix_index(name, p.prefix);
		if (i >= 0 && i < kAttachSlots) return i + p.offset < kAttachSlots ? i + p.offset : -1;
	}
	return -1;
}

// "pivot node i" with a valid slot (i + 4 < 14): BH3Method::load_hier marks these isDeadNode.
bool is_pivot_node(const std::string& name) {
	const int i = suffix_index(name, "pivot node ");
	return i >= 0 && i + 4 < kAttachSlots;
}

struct PoseContext {
	const BH3Mesh& mesh;
	const Animation* anim;
	uint32_t ms;
	std::vector<PosedVertex>& out;
	std::vector<Xform>* node_world;
};

void pose_node(PoseContext& cx, const BH3Node& node, const AnimNode* anim_node, const Xform& parent_world) {
	Xform local;
	if (anim_node && cx.anim && !is_pivot_node(node.name)) sample(anim_node->keys, cx.ms, cx.anim->duration_ms, local);
	const Xform world = parent_world * node.rest * local;
	if (cx.node_world) cx.node_world->push_back(world);

	const size_t n = cx.mesh.vertex_count();
	const size_t end = std::min<size_t>(size_t(node.first_vertex) + node.vertex_count, n);
	for (size_t v = node.first_vertex; v < end; v++) {
		const Vec3 p = apply(world, {cx.mesh.positions[v * 3], cx.mesh.positions[v * 3 + 1], cx.mesh.positions[v * 3 + 2]});
		const Vec3 nn = rotate(world.q, {cx.mesh.normals[v * 3], cx.mesh.normals[v * 3 + 1], cx.mesh.normals[v * 3 + 2]});
		cx.out[v] = {p.x, p.y, p.z, nn.x, nn.y, nn.z, cx.mesh.uvs[v * 2], cx.mesh.uvs[v * 2 + 1]};
	}
	// Animation nodes pair with mesh nodes by position in the child list (HierObj::set_to_time).
	for (size_t i = 0; i < node.children.size(); i++) {
		const AnimNode* child_anim = anim_node && i < anim_node->children.size() ? &anim_node->children[i] : nullptr;
		pose_node(cx, node.children[i], child_anim, world);
	}
}

}  // namespace

void pose_model(const BH3Model& model, const Animation* anim, uint32_t ms, std::vector<PosedVertex>& out,
                std::vector<Xform>* node_world) {
	const BH3Mesh& mesh = model.meshes[0];
	const size_t n = mesh.vertex_count();
	out.resize(n);
	// Vertices not owned by any node stay in model space.
	for (size_t v = 0; v < n; v++) {
		out[v] = {mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2],
		          mesh.normals[v * 3],   mesh.normals[v * 3 + 1],   mesh.normals[v * 3 + 2],
		          mesh.uvs[v * 2],       mesh.uvs[v * 2 + 1]};
	}
	if (node_world) node_world->clear();
	PoseContext cx{mesh, anim, ms, out, node_world};
	const AnimNode* root_anim = anim && !anim->roots.empty() ? &anim->roots[0] : nullptr;
	pose_node(cx, model.root, root_anim, Xform{});
}

namespace {

void collect_slots(const BH3Node& node, int& index, std::array<int, kAttachSlots>& slots) {
	const int me = index++;
	if (slots[13] < 0) slots[13] = me;
	if (iequals(node.name, "Bip01 Spine")) slots[12] = slots[13] = me;
	const int s = slot_for_name(node.name);
	if (s >= 0) slots[size_t(s)] = me;
	for (const BH3Node& c : node.children) collect_slots(c, index, slots);
}

}  // namespace

std::array<int, kAttachSlots> attach_slots(const BH3Model& model) {
	std::array<int, kAttachSlots> slots;
	slots.fill(-1);
	int index = 0;
	collect_slots(model.root, index, slots);
	return slots;
}

void transform_vertices(const Xform& x, std::vector<PosedVertex>& v) {
	for (PosedVertex& p : v) {
		const Vec3 pos = apply(x, {p.px, p.py, p.pz});
		const Vec3 n = rotate(x.q, {p.nx, p.ny, p.nz});
		p.px = pos.x, p.py = pos.y, p.pz = pos.z;
		p.nx = n.x, p.ny = n.y, p.nz = n.z;
	}
}

void bounding_sphere(const std::vector<PosedVertex>& v, Vec3& center, float& radius) {
	center = {};
	radius = 0;
	if (v.empty()) return;
	Vec3 lo{v[0].px, v[0].py, v[0].pz}, hi = lo;
	for (const auto& p : v) {
		lo = {std::min(lo.x, p.px), std::min(lo.y, p.py), std::min(lo.z, p.pz)};
		hi = {std::max(hi.x, p.px), std::max(hi.y, p.py), std::max(hi.z, p.pz)};
	}
	center = (lo + hi) * 0.5f;
	for (const auto& p : v) radius = std::max(radius, length(Vec3{p.px, p.py, p.pz} - center));
}

}  // namespace ron
