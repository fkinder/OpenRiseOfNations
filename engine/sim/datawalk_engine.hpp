// The Provider that produces the original game's byte image of the engine's state (docs/original-save.md, docs/state-spec.md).
//
// The DataWalk (sim/datawalk.hpp) hashes the in-memory images of the original's structs. EngineProvider yields those images for a
// Game loaded from an original save (sim/original_save.hpp): the members the engine models are serialized at their original
// offsets and sizes from the engine's structs (sim/original_map.hpp), padding and every member the engine does not model come
// from the shadow (the decoded save kept in Game::original). compute_check_sums / compute_verify then run on it unchanged, so
// the 15 CHECK_SUMS parts of the engine's state are the same Adler-32 values the original game would send.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sim/datawalk.hpp"
#include "sim/game.hpp"
#include "sim/original_save.hpp"

namespace ron::sim {

class EngineProvider : public datawalk::Provider {
public:
	// `g` must have been produced by load_original_save (g.original set). The provider reads the engine state live: build a
	// new one (or call begin_walk) after the game has advanced. Not thread safe.
	explicit EngineProvider(Game& g);
	void begin_walk() override;
	datawalk::Obj global(int gid) override { return base_.global(gid); }
	const uint8_t* image(datawalk::Obj o, int64_t off, uint32_t n) override;
	datawalk::Obj child(datawalk::Obj o, const datawalk::Site& site, int64_t index) override;
	bool local(datawalk::Obj o, const datawalk::Site& site, int64_t& value) override;
	bool container(datawalk::Obj o, const datawalk::Site& site, datawalk::ContainerHeader& hdr) override;
	bool present(datawalk::Obj o, const datawalk::Site& site, int64_t i) override;
	int32_t data_type(datawalk::Obj o, const datawalk::Site& site, int64_t i) override;
	datawalk::Obj element(datawalk::Obj o, const datawalk::Site& site, int64_t i) override;
	const uint8_t* bytes(datawalk::Obj o, const datawalk::Site& site, uint64_t n) override;
	const uint8_t* string(datawalk::Obj o, const datawalk::Site& site, uint32_t& len) override;

private:
	Game& g_;
	OriginalShadow& sh_;
	datawalk::TreeProvider& base_;
	std::vector<uint8_t> scratch_, block_buf_, string_buf_;
	std::unordered_map<uint64_t, int> nth_;  // calls of one (image, step, base) within the current walk
	int next_nth(const void* rec, int step, int64_t base);
	// Containers the engine owns, as asked in the current walk (see omap::ArraySpec).
	struct Active {
		omap::ArraySpec spec;
		int32_t shadow_length = 0, length = 0;
	};
	std::unordered_map<uint64_t, Active> active_;
	static uint64_t key_of(const void* rec, int step, int64_t base) {
		return (uint64_t(reinterpret_cast<uintptr_t>(rec)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(uint32_t(step)) << 32) ^ uint64_t(base);
	}
	const Active* active(datawalk::Obj o, const datawalk::Site& site) const {
		auto it = active_.find(key_of(o, site.step, site.base));
		return it == active_.end() ? nullptr : &it->second;
	}
	datawalk::Rec* make_child(datawalk::Rec* parent, const datawalk::Site& site, int64_t index, datawalk::Rec* existing, const char* cls);
};

}  // namespace ron::sim
