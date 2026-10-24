// A deterministic demo player for the network tests: every few turns it trains a citizen at a city centre and sends its citizens walking
// (so there are commands to synchronise). Local to the process that plays the seat; what it does reaches the others only through the
// packages of the lockstep layer.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/lockstep.hpp"
#include "sim/commands.hpp"
#include "sim/game.hpp"
#include "sim/replay.hpp"

namespace ronnet {

namespace sim = ron::sim;
namespace net = ron::net;

inline int32_t find_type_by_name(const sim::Game& g, const std::string& want) {
	auto low = [](std::string v) {
		for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
		return v;
	};
	const std::string w = low(want);
	for (size_t i = 0; i < g.rules->types.size(); ++i) {
		const sim::Type& t = g.rules->types[i];
		if (!t.loaded) continue;
		if (low(sim::to_utf8(t.type_name.empty() ? t.name : t.type_name)) == w) return int32_t(i);
	}
	return -1;
}

// A deterministic player: every few turns it trains a citizen at one of its buildings and sends its citizens walking. Local to the
// process that plays the seat; what it does reaches the others only through the packages.
class DemoBot {
public:
	DemoBot(const sim::Game& g, int seat) : DemoBot([&g]() -> const sim::Game& { return g; }, seat) {}
	// `get` returns the current Game (an OOS recovery replaces the Game object).
	DemoBot(std::function<const sim::Game&()> get, int seat) : get_(std::move(get)), seat_(seat), rng_(0x9e3779b9u * uint32_t(seat + 1)) {
		citizen_ = find_type_by_name(get_(), "Citizen");
	}

	net::Bytes next(uint32_t stamp) {
		net::Bytes out;
		if (stamp < 3) return out;
		const sim::Game& g_ = get_();
		const sim::ObjectList& ol = g_.objects[size_t(seat_)];
		if (stamp % 7 == 0) {  // train a citizen
			std::vector<uint16_t> bld;
			for (size_t i = 0; i < ol.builds.size() && bld.empty(); ++i) {
				const sim::Build* b = ol.builds[i].get();
				if (b && b->alive() && b->type == city_type()) bld.push_back(uint16_t(sim::kBuildBase + int(i)));
			}
			if (!bld.empty()) {
				sim::Command gr;
				gr.verb = sim::Verb::Group;
				gr.who = seat_;
				gr.objects = bld;
				sim::engine_to_wire(gr, out);
				sim::Command q;
				q.verb = sim::Verb::QueueUp;
				q.type = citizen_;
				q.num = 1;
				sim::engine_to_wire(q, out);
			}
		}
		if (stamp % 5 == 0) {  // walk
			std::vector<uint16_t> units;
			int32_t cx = 0, cy = 0;
			for (int pass = 0; pass < 2 && units.empty(); ++pass) {  // citizens first, else whatever the seat has
				for (int o = 0; o < ol.unit_mark && units.size() < 40; ++o) {
					const sim::Unit* u = o < int(ol.units.size()) ? ol.units[size_t(o)].get() : nullptr;
					if (!u || !u->alive() || u->o_up >= 0 || (pass == 0 && u->type != citizen_)) continue;
					units.push_back(uint16_t(o));
					cx = u->x;
					cy = u->y;
				}
			}
			if (!units.empty()) {
				const int32_t span = 768 * 12;
				sim::Command gr;
				gr.verb = sim::Verb::Group;
				gr.who = seat_;
				gr.objects = units;
				sim::engine_to_wire(gr, out);
				sim::Command m;
				m.verb = sim::Verb::MoveTo;
				m.x = std::max<int32_t>(768, std::min<int32_t>(g_.world.xs * 768 - 768, cx + int32_t(rng_.below(2 * span)) - span));
				m.y = std::max<int32_t>(768, std::min<int32_t>(g_.world.ys * 768 - 768, cy + int32_t(rng_.below(2 * span)) - span));
				m.orders = 1;
				m.queued = sim::kQueueNew;
				m.form = -1;
				m.width = -1;
				sim::engine_to_wire(m, out);
			}
		}
		return out;
	}

private:
	struct Lcg {
		uint32_t s;
		explicit Lcg(uint32_t seed) : s(seed) {}
		uint32_t below(uint32_t n) {
			s = s * 1664525u + 1013904223u;
			return n ? (s >> 8) % n : 0;
		}
	};
	int32_t city_type() {
		if (city_ < 0) city_ = find_type_by_name(get_(), "City Center");
		return city_;
	}
	std::function<const sim::Game&()> get_;
	int seat_;
	Lcg rng_;
	int32_t citizen_ = -1, city_ = -1;
};


}  // namespace ronnet
