// The game's random number generator (class Random, riseofnations.exe): one 32-bit LCG per instance. The
// simulation's instance is game_random (seeded from GameInfo.seed); see docs/replay-spec.md "Random".
#pragma once

#include <cstdint>
#include <cstring>

namespace ron::sim {

#ifdef RON_RNG_PROFILE_HOOK
// Profiling build only (apps/sav RON_RNG_PROFILE=1, docs/original-save.md section 6): called on every draw.
inline void (*g_rng_hook)() = nullptr;
#endif

class Random {
public:
	explicit Random(uint32_t seed = 0) : seed_(seed) {}

	// Random::reseed @00a39d30: returns the old seed.
	uint32_t reseed(uint32_t s) {
		const uint32_t old = seed_;
		seed_ = s;
		return old;
	}
	uint32_t seed() const { return seed_; }

	uint32_t next() {
#ifdef RON_RNG_PROFILE_HOOK
		if (g_rng_hook) g_rng_hook();
#endif
		seed_ = seed_ * 0x19660Du + 0x3C6EF35Fu;
		return seed_;
	}

	// Random::get(lo, hi) @00a39d70: [lo, hi) scaled by the low 16 bits; no draw when lo == hi; swapped when lo > hi.
	// The game asserts lo, hi <= 0xFFFF.
	int32_t get(int32_t lo, int32_t hi) {
		if (lo == hi) return lo;
		if (lo > hi) {
			const int32_t t = lo;
			lo = hi;
			hi = t;
		}
		next();
		return lo + int32_t((int64_t(seed_ & 0xFFFFu) * int64_t(hi - lo)) >> 16);
	}

	// Random::get() @00a39cf0: [0, 1) from the low 23 bits as a float mantissa, minus 1 in double, rounded to float.
	float get_float() {
		next();
		const uint32_t bits = 0x3F800000u | (seed_ & 0x7FFFFFu);
		float f;
		std::memcpy(&f, &bits, sizeof f);
		return float(double(f) - 1.0);
	}

private:
	uint32_t seed_;
};

}  // namespace ron::sim
