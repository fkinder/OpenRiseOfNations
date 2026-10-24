#include "assets/battlefield.hpp"

#include <map>
#include <set>

namespace ron {

Battlefield resolve_battlefield(const GameFiles& files, const Scenario& sc, const std::string& scenario_path, const Rules& rules,
                                const BuildCatalog& builds, const std::vector<UnitGraphic>& unit_names) {
	Battlefield out;
	std::set<std::string> units;
	for (const UnitGraphic& g : unit_names) units.insert(to_lower(g.name));
	auto unit_exists = [&](const std::string& n) { return units.count(to_lower(n)) != 0; };

	// Players: nation -> styles (base tribe list, or the conquest campaign's), age from the age techs.
	std::vector<std::string> tribes = conquest_tribe_files(files, scenario_path);
	if (tribes.empty()) tribes = rules.tribe_files;
	struct PlayerInfo {
		TribeStyle style;
		int age = 0, color = 0;
	};
	std::map<int, PlayerInfo> players;
	std::map<std::string, TribeStyle> style_cache;
	for (const ScenarioPlayer& p : sc.players) {
		PlayerInfo info;
		if (p.tribe >= 0 && size_t(p.tribe) < tribes.size()) {
			const std::string& f = tribes[size_t(p.tribe)];
			auto it = style_cache.find(f);
			info.style = it != style_cache.end() ? it->second : (style_cache[f] = load_tribe_style(files, f));
		}
		info.age = rules.age(p.techs);
		info.color = p.color;
		players[p.who] = info;
	}
	auto player = [&](int who) { return players.count(who) ? players[who] : PlayerInfo{}; };

	for (const ScenarioBuilding& b : sc.buildings) {
		auto g = rules.build_graph.find(b.type);
		if (g == rules.build_graph.end()) {
			out.unresolved.push_back(b.type);
			continue;
		}
		const PlayerInfo p = player(b.who);
		if (g->second == "FARM") {
			// No sprite: FarmsOut::draw and the farm underlay (docs/farms-spec.md).
			out.objects.push_back({PlacedObject::Farm, "FARM", float(b.cx), float(b.cy), 0, p.color, b.who, b.o, false});
			continue;
		}
		const BuildGraphic* graphic = builds.find_variant(g->second, build_style_name(p.style.build_style), p.age,
		                                                     build_style_name(p.style.backup_style));
		if (graphic && graphic->texture.empty() && unit_exists(graphic->name)) {
			// Turret buildings (air defense, SAM sites) have no sprite: they are drawn as the 3D unit graphic of the same name.
			out.objects.push_back({PlacedObject::Unit, graphic->name, float(b.cx), float(b.cy), 180.0f, p.color});
			continue;
		}
		if (!graphic || graphic->texture.empty()) {
			out.unresolved.push_back(b.type + " (" + g->second + ")");
			continue;
		}
		out.objects.push_back({PlacedObject::Building, graphic->name, float(b.cx), float(b.cy), 0, p.color, b.who, b.o, b.city});
	}

	for (const ScenarioUnit& u : sc.units) {
		if ((u.masks & 8) || u.inside >= 0) continue;  // crew slots and garrisoned / carried units are not drawn
		auto g = rules.unit_graph.find(u.type);
		if (g == rules.unit_graph.end()) {
			out.unresolved.push_back(u.type);
			continue;
		}
		const PlayerInfo p = player(u.who);
		// <GRAPH>-<unit style>-AGE{0|3|5}[-FEMALE][-PACKED]; fall back to lower age buckets, then DEFAULT.
		static const int kBucketAge[3] = {0, 3, 5};  // Ancient-Medieval, Gunpowder-Enlightenment, Industrial+
		const int bucket = p.age < 5 ? p.age / 3 : 2;
		const std::string suffix = std::string((u.o & 1) ? "-FEMALE" : "") + ((u.masks & 0x10) ? "-PACKED" : "");
		std::string found;
		for (const std::string& style : {std::string(unit_style_name(p.style.unit_style)), std::string("DEFAULT")}) {
			for (int bk = bucket; bk >= 0 && found.empty(); bk--) {
				const std::string base = g->second + "-" + style + "-AGE" + std::to_string(kBucketAge[bk]);
				for (const std::string& name : {base + suffix, base + ((u.masks & 0x10) ? "-PACKED" : "")}) {
					if (unit_exists(name)) {
						found = name;
						break;
					}
				}
			}
			if (!found.empty()) break;
		}
		if (found.empty()) {
			// Gaia herd animals (Herd Bear, Herd Fish ...) are drawn as <GRAPH>-TYPE0..2, one variant per slot.
			const std::string variant = g->second + "-TYPE" + std::to_string(u.o % 3);
			if (unit_exists(variant)) found = variant;
		}
		if (found.empty()) {
			out.unresolved.push_back(u.type + " (" + g->second + ")");
			continue;
		}
		// Facing: deg = angle * 360 / 2^32; the draw path adds 180 (GuyOut::draw).
		const float deg = float(double(u.angle) * 360.0 / 4294967296.0);
		out.objects.push_back({PlacedObject::Unit, found, float(u.cx), float(u.cy), deg + 180.0f, p.color, u.who, u.o});
	}
	for (const ScenarioGood& g : sc.goods) out.objects.push_back({PlacedObject::Good, g.type, float(g.cx), float(g.cy), 0, 0});
	return out;
}

}  // namespace ron
