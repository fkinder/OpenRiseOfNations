// The concrete AnimSource built from the user's install (docs/anim-spec.md):
// - Data/anim_graphics.xml: the animmgr entries, in file order of the sections LOOPING (looping) then NONLOOPING,
//   BUILDING, PATH (not looping) (GraphicPieces::init_anims_pool @8fca40, AnimMgr::add @53ac00);
// - Data/unit_graphics.xml: one <UNIT> per graphics piece name, its <ANIM name=UnitAnim file=entry name> children
//   giving the AnimationPacket action ids (GraphicPieces::init_unit_data @8f4290, decipher_animation @8face0);
// - the .bha files: the first node's key times give animmgr.times / frames (AnimMgr::force_load @53ade0,
//   AnimObj::load_hier @54b700). Files are read lazily on first use, as force_load does; the results do not
//   depend on the order of use.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <array>
#include <map>
#include <tuple>

#include "assets/bh3.hpp"
#include "assets/bha.hpp"
#include "assets/files.hpp"
#include "assets/pose.hpp"
#include "sim/anims.hpp"
#include "sim/rules.hpp"

namespace ron::sim {

// AnimObj as AnimObj::load_hier @54b700 builds it (anim_assets.cpp): the muzzle offsets pose the hierarchy from these.
struct OKey {
	float q[4];  // the file order (x, y, z, w) rounded to 1/1000
	float p[3];  // rounded to 1/100
};
struct ONode {
	std::vector<OKey> keys;
	std::vector<uint16_t> times;
	std::vector<uint8_t> flags;
	bool constant = false;
	uint32_t anim_time = 0;
	std::vector<ONode> children;
};

class AssetAnimSource : public AnimSource {
public:
	AssetAnimSource(const GameFiles& files, const Rules& rules);

	bool piece_exists(int32_t piece) const override;
	bool build_piece_exists(int32_t type, int32_t style, int32_t age) const override;
	bool has_packet(int32_t piece) const override;
	bool has_anim(int32_t piece, int32_t anim) const override;
	int32_t frames(int32_t piece, int32_t anim) const override;
	uint32_t time_ms(int32_t piece, int32_t anim) const override;
	bool loops(int32_t piece, int32_t anim) const override;
	bool track(int32_t piece, float* scale, float* offset_x, float* offset_y) const override;
	const std::vector<ReleaseEvent>* release_events(int32_t piece) const override;
	const std::vector<PlaneRelease>* plane_releases(int32_t piece) const override;
	int32_t instant_hit(int32_t piece) const override;
	bool no_release(int32_t piece) const override;
	bool has_skeleton(int32_t piece) const override;
	bool has_scorch(int32_t piece) const override;
	bool attach_position(int32_t piece, int32_t node, int32_t anim, int32_t frame, float out[3]) const override;
	int32_t pivot_restrictions(int32_t piece) const override;
	int32_t pivot_count(int32_t type) const override;
	bool pivot_limits(int32_t type, int32_t node, int32_t* mn, int32_t* mx) const override;
	int32_t num_ammo() const override { return int32_t(ammo_.size()); }
	uint32_t ammo_flags(int32_t ammo) const override;
	// The AMMO piece name (effects_graphics.xml), "" out of range.
	std::string ammo_name(int32_t ammo) const;

	// GraphicPieces::unit_node_from_gpiece @901c30: the <UNIT> name piece `piece` looks up ("" outside the unit and
	// bird ranges).
	std::string piece_name(int32_t piece) const;
	// The animmgr entry name (anim_graphics.xml) and file the action of `piece` maps to, "" if none.
	std::string anim_entry_name(int32_t piece, int32_t anim) const;

	// Data problems seen while loading (unknown action names, unresolved entry names), for diagnostics.
	const std::vector<std::string>& warnings() const { return warnings_; }

private:
	struct UnitArt {
		std::string model;         // normalized relative path
		bool model_ok = false;     // model attribute set and the file exists (file_exist before RData::load)
		float scale = 1.0f;        // RData scale
		float track_x = 0.0f, track_y = 0.0f;  // trackoffsetx / trackoffsety attributes
		std::vector<int32_t> ids;  // AnimationPacket action_ids: UnitAnim -> animmgr entry (-1 none)
		std::vector<ReleaseEvent> releases;  // <RELEASEEVENT> in file order
		std::vector<PlaneRelease> planes;    // <PLANERELEASE> in file order
		int32_t instant_hit = -1;            // <INSTANTHIT ammo>
		bool no_release = false;             // <NORELEASE>
		bool skeleton = false, scorch = false;  // the attributes name a <SKELETON> / <SCORCH>
	};
	struct Ammo {
		std::string name;
		uint32_t flags = 0;
	};
	struct Entry {
		std::string name, file;  // anim_graphics.xml name and normalized file path
		bool looping = false;
		// AnimMgr::force_load results (computed on first use).
		mutable bool loaded = false, obj = false;
		mutable uint32_t times = 0;
		mutable int32_t frames = 0;
	};

	const UnitArt* art(int32_t piece) const;
	const Entry* entry(int32_t piece, int32_t anim) const;  // force-loaded entry of the action, or null
	void force_load(const Entry& e) const;

	GameFiles files_;
	std::vector<std::string> graph_;    // per type index: GRAPH (graph_name), "" for types that were never loaded
	std::unordered_set<std::string> build_pieces_;  // uppercase <BUILD name> of building_graphics.xml whose texture / zbuffer files exist
	std::vector<bool> gender_packed_;   // per type index: gender slot means -PACKED (else -FEMALE)
	std::vector<UnitArt> units_;
	std::unordered_map<std::string, int32_t> unit_by_name_;  // uppercase <UNIT> name -> units_ (first wins)
	std::vector<Entry> entries_;
	std::vector<Ammo> ammo_;
	mutable std::unordered_map<int32_t, int32_t> piece_unit_;  // piece -> units_ index or -1
	struct Restriction {
		int32_t node;
		float min, max;
	};
	std::unordered_map<std::string, std::vector<Restriction>> restrictions_;  // uppercase RESTRICTION name -> its elements in file order (init_pivot_restrictions)
	// attach_position: parsed models (with their attach slots) and animations by file, and the results by (piece, node, anim, frame).
	struct ModelData {
		BH3Model model;
		std::array<int, ron::kAttachSlots> slots;
	};
	mutable std::unordered_map<std::string, std::shared_ptr<ModelData>> models_;
	mutable std::unordered_map<std::string, std::shared_ptr<ONode>> bhas_;
	mutable std::map<std::tuple<int32_t, int32_t, int32_t, int32_t>, std::array<float, 4>> attach_cache_;  // [3] = 1 when an entry exists
	std::vector<std::string> warnings_;
};

std::unique_ptr<AssetAnimSource> load_anim_source(const GameFiles& files, const Rules& rules);

}  // namespace ron::sim
