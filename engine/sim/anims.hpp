// The art data the simulation reads (docs/anim-spec.md): which graphics piece a guy uses and, through that piece's
// AnimationPacket and the global animation manager (animmgr), whether it has an animation, its length in frames,
// its length in milliseconds and whether it loops. The guy animation clock (Guy::set_anim / Guy::inc_time) depends
// on these, so they are part of the lockstep state's inputs; nothing here depends on graphics settings.
//
// The interface keeps engine/sim free of asset parsing: sim/anim_assets.hpp builds the concrete source from the
// install (Data/unit_graphics.xml, Data/anim_graphics.xml, the .bha files).
#pragma once

#include <cstdint>
#include <vector>

namespace ron::sim {

// GraphicPieces ammo flags (the <AMMO> attributes of effects_graphics.xml; combat-spec 4.1).
namespace ammoflag {
constexpr uint32_t kPuncture = 0x1, kPuff = 0x2, kExplode = 0x4, kMissile = 0x8, kFlame = 0x10, kPulsewave = 0x20,
                   kFlak = 0x40, kDoNoDamage = 0x80;
}

// One <RELEASEEVENT> of a unit graphics piece as GraphicEvents::init_unit_events @8e2520 stores it: the animation
// (UnitAnim), the game frame it fires at (max(1, (u16)starttime / 67)), the ammo piece (index into the AMMO list of
// effects_graphics.xml, i.e. gpiece - first_ammo_piece) and the release node.
struct ReleaseEvent {
	int32_t anim = 0, frame = 1, ammo = 0, node = 0;
	bool resolved = true;  // the ammo name was found (an unknown name reports an error and uses piece 0)
};

// One <PLANERELEASE> of a carrier piece (GraphicEvents event type 5): the animation, the game frame it fires at (same
// conversion as the release events), the deck node and the release angle in degrees (attribute `angle`, 0 when absent).
struct PlaneRelease {
	int32_t anim = 0, frame = 1, node = 0, angle = 0;
};

// GraphicPieces::init_piece_ranges @8f70e0: the unit piece index space. A unit piece is
//   (type - 50) + style * kPerStyle + age_bucket * kPerAge + gender * kPerGender + crew * kPerCrew
// (style 0..5 = UNIT_CONTINENT, age bucket 0..2 = AGE0/AGE3/AGE5, gender 1 = -FEMALE or -PACKED, crew 0..3 = guy
// slot, -CREWn for n > 0). The six pieces [kTotalUnit - 6, kTotalUnit) are the merchant build-style specials;
// birds/gaia animals (types 402..413) use three pieces each from kFirstBird.
namespace gpiece {
constexpr int32_t kFirstUnit = 0, kPerStyle = 0x160, kPerAge = 0x840, kPerGender = 0x18c0, kPerCrew = 0x3180;
constexpr int32_t kTotalUnit = 0xc606, kFirstBuild = 0xc606, kFirstBird = 0xea8d, kNumBird = 0x24;
}  // namespace gpiece

struct AnimSource {
	virtual ~AnimSource() = default;
	// GraphicPieces data_pieces[piece] != 0 after verify_load: the piece's <UNIT> exists and its model loads.
	virtual bool piece_exists(int32_t piece) const = 0;
	// The piece has an AnimationPacket (RData packets[0x13]).
	virtual bool has_packet(int32_t piece) const = 0;
	// AnimationPacket::get_animobj(anim) != 0: the action maps to an animmgr entry whose .bha loaded.
	virtual bool has_anim(int32_t piece, int32_t anim) const = 0;
	// animmgr.frames of the action's entry (Guy::set_anim end_time, AnimationPacket::get_game_frames); 3 when the
	// action has no entry.
	virtual int32_t frames(int32_t piece, int32_t anim) const = 0;
	// animmgr.times in ms (AnimationPacket::get_anim_time); 200 when the action has no entry.
	virtual uint32_t time_ms(int32_t piece, int32_t anim) const = 0;
	// animmgr.loopings (AnimationPacket::is_looping); false when the action has no entry.
	virtual bool loops(int32_t piece, int32_t anim) const = 0;
	// The piece's RData scale (attribute scale, < 0.1 -> 1) and its unit_data track offsets (trackoffsetx/y, read
	// only for crew pieces of unit types; 0 otherwise). False when the piece did not load (no unit_data).
	virtual bool track(int32_t piece, float* scale, float* offset_x, float* offset_y) const = 0;

	// ---- combat data (combat-spec 4.2, 4.17, 6.2-6.3); the defaults describe art without any of it.
	// The piece's RELEASEEVENTs in file order (null: none).
	virtual const std::vector<ReleaseEvent>* release_events(int32_t piece) const {
		(void)piece;
		return nullptr;
	}
	// The piece's PLANERELEASE events in file order (null: none; aircraft-spec 8, GraphicEvents::execute_game_events).
	virtual const std::vector<PlaneRelease>* plane_releases(int32_t piece) const {
		(void)piece;
		return nullptr;
	}
	// <INSTANTHIT ammo=..> (ammo index, -1 none) and <NORELEASE> of the piece.
	virtual int32_t instant_hit(int32_t piece) const {
		(void)piece;
		return -1;
	}
	virtual bool no_release(int32_t piece) const {
		(void)piece;
		return false;
	}
	// UnitRDataStruct skeleton / scorch (the attributes resolve to a <SKELETON> / <SCORCH>): a skeleton keeps the
	// corpse 627 frames past its death animation, a scorch piece gives flame deaths their burn animations.
	virtual bool has_skeleton(int32_t piece) const {
		(void)piece;
		return false;
	}
	virtual bool has_scorch(int32_t piece) const {
		(void)piece;
		return false;
	}
	// The AMMO pieces: count and flags (ammoflag::).
	virtual int32_t num_ammo() const { return 0; }
	virtual uint32_t ammo_flags(int32_t ammo) const {
		(void)ammo;
		return 0;
	}
};

// guy_scale @c06244 (4.8): world units per model unit; Guy::update_gpiece scales the crew track offsets by it.
constexpr float kGuyScale = 4.8f;

}  // namespace ron::sim
