// Setup::build_game steps 7 and 9 (start order, empires) and Setup::build_empire / build_cities / build_units / place_unit
// (docs/randommap-spec.md sections 1 and 8). The empire is produced as a placement list: the city position comes from
// BuildTypeData::snap_center, the other buildings are found by the AI's site search at run time (Leader::produce_building),
// and the units are placed with Setup::place_unit's exact draw sequence.
#include <array>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

bool valid_leader_slot(const Params& p, int slot) { return slot >= 0 && slot < p.nations && slot < 8; }

bool is_team(const Params& p, int a, int b) {
	if (a == b) return false;
	const int ta = p.team[size_t(a)], tb = p.team[size_t(b)];
	return ta >= 0 && ta < 8 && ta == tb;
}

namespace {

// Setup::get_starting_citizens @5aaf50: jump table at 0x5aafac, read from the exe: {3,0}, {4,0}, {5,2}, {10,5}.
void starting_citizens(int town, int* n, int* n_farm) {
	static const int tab[4][2] = {{3, 0}, {4, 0}, {5, 2}, {10, 5}};
	if (town >= 0 && town < 4) {
		*n = tab[town][0];
		*n_farm = tab[town][1];
	} else {
		*n = 0;
		*n_farm = 0;
	}
}

constexpr int kScoutType = 0x45, kCitizenType = 0x32, kScholarType = 0x34, kMerchantDutch = 62, kLargeCity = 0x19f,
              kWoodcutter = 0x1a2, kTower = 0x1b7, kLibrary = 0x1b3, kMarket = 0x1b4, kTemple = 0x1b5,
              kGranary = 0x1a7, kLumberMill = 0x1a8, kSenate = 0x1b6;

}  // namespace

void Gen::emit_unit(int who, int start, int type, int x, int y, int note) {
	EmpireEntry e;
	e.kind = EmpireEntry::Unit;
	e.who = who;
	e.start = start;
	e.type = type;
	e.x = x;
	e.y = y;
	e.note = note;
	res.empire.push_back(e);
}

// Setup::place_unit @5abca0. (ox, oy): the origin in world units (the city position when there is one).
int Gen::place_unit(int who, bool has_city, int type, int ox, int oy, int ux, int uy) {
	(void)ux;
	(void)uy;
	const Circles& C = circles();
	const int town = P.starting_town;
	const int ring = town ? C.radius[2] : (active_units[size_t(who)] != 0 ? C.radius[24] : C.radius[8]);
	const int attempts = town == 0 ? 500 : 60;
	const int owx = wcoord_of(ox), owy = wcoord_of(oy);
	const int origin_region = inb(owx, owy) ? W(owx, owy).region : 0;
	for (int a = 0; a < attempts; a++) {
		const int k = ring > 1 ? r.rnd(ring) : 0;
		const int cx = owx + C.x[size_t(k)], cy = owy + C.y[size_t(k)];
		if (!inb(cx, cy)) continue;
		const WData& d = W(cx, cy);
		if (d.region != origin_region) continue;
		if (d.flags & 0x30) continue;
		if (!(d.flags & 0x100) && (d.land == 1 || d.land == 2)) continue;
		if (d.flags & 0x100) continue;
		if (d.down >= 0 || occupied[w.wi(cx, cy)]) continue;
		const uint16_t tm = w.t(cx * 4 + 2, cy * 4 + 2);
		if ((tm & tmask::kBlocked) || (tm & tmask::kLowField) == tmask::kBuilding) continue;
		if (blocked_tiles[w.ti(cx * 4 + 2, cy * 4 + 2)]) continue;
		if (d.flags & wflag::kGoody) continue;
		occupied[w.wi(cx, cy)] = 1;
		active_units[size_t(who)]++;
		emit_unit(who, cur_start, type, cx * 768 + 384, cy * 768 + 384, 0);
		return int(res.empire.size()) - 1;
	}
	// after the attempts: Build::train(city, type) with a city, else init_unit at (x, y) + come_out(0)
	active_units[size_t(who)]++;
	emit_unit(who, cur_start, type, has_city ? -1 : ox, has_city ? -1 : oy, has_city ? 3 : 4);
	return int(res.empire.size()) - 1;
}

// Setup::build_empire @5abb80
void Gen::build_empire(int who, int start) {
	if (start < 0 || start >= int(starts.size())) return;
	cur_start = start;
	const int tribe = P.tribe[size_t(who)];
	const int bonus = tribe >= 0 && tribe < int(rules.tribes.size()) ? rules.tribes[size_t(tribe)].tribe : -1;
	const int town = P.starting_town;
	const Constants& K = rules.constants;
	auto konst = [&](const char* name) { return K.at(uint32_t(constant_offset(name))); };
	auto graft = [&](int type) {
		if (tribe >= 0 && tribe < int(rules.tribes.size()) && type >= kUnits && type < kUnits + kNumGraft) {
			const int g = rules.tribes[size_t(tribe)].graft[size_t(type - kUnits)];
			if (g >= 0) return g;
		}
		return type;
	};
	const WCoord st = starts[size_t(start)];
	// ---- build_cities @5ab910
	int city_x = -1, city_y = -1;
	bool has_city = false;
	if (town != 0) {
		const int cx0 = st.x * 768, cy0 = st.y * 768;
		int type = kVillage;
		const Type& bt0 = rules.types[size_t(kVillage)];
		const auto snapped = snap_center(w, bt0, cx0, cy0);
		city_x = snapped[0];
		city_y = snapped[1];
		if (bonus == 14 && (konst("chinese_large_city") || konst("chinese_large_cities"))) type = kLargeCity;
		EmpireEntry e;
		e.kind = EmpireEntry::City;
		e.who = who;
		e.start = start;
		e.type = type;
		e.x = city_x;
		e.y = city_y;
		res.empire.push_back(e);
		has_city = true;
		const Footprint f = footprint(rules.types[size_t(type)], city_x, city_y);
		for (int tx = f.x0; tx < f.x0 + f.w; tx++) {
			for (int ty = f.y0; ty < f.y0 + f.h; ty++) {
				if (w.in_t(tx, ty)) {
					blocked_tiles[w.ti(tx, ty)] = 1;
					occupied[w.w_of_t(tx, ty)] = 1;
				}
			}
		}
		auto building = [&](int t, int n = 1) {
			for (int i = 0; i < n; i++) {
				EmpireEntry b;
				b.kind = EmpireEntry::Building;
				b.who = who;
				b.start = start;
				b.type = t;
				res.empire.push_back(b);
			}
		};
		if (town == 3) {
			building(kWoodcutter);
			if (bonus != 19) building(kFarm, 5);
			building(kTower, 4);
			building(kLibrary);
		} else if (town == 2) {
			building(kWoodcutter);
			if (bonus == 20 && konst("americans_starting_farms") > 0) building(kFarm, konst("americans_starting_farms"));
			else if (bonus != 19) building(kFarm, 3);
			building(kLibrary);
		}
		// build_civ_specific @5ab760 (read once: ❓ randommap-spec §8 notes the decompile calls it twice for town 3)
		if (town >= 2) {
			if (bonus == 4 || bonus == 22) building(kMarket);
			if (bonus == 5 && konst("greek_university_early") > 1) building(kUniversity);
			if (bonus == 16 && konst("korean_temple_upgrades") > 1) building(kTemple);
			if (bonus == 7 && konst("egyptian_granary_early") > 1) building(kGranary);
			if (bonus == 10 && konst("french_lumbermill_early") > 1) building(kLumberMill);
			if (bonus == 18 && konst("iroquois_senate")) building(kSenate);
		}
	}
	// ---- build_units @5aafc0
	int n = 0, n_farm = 0;
	starting_citizens(town, &n, &n_farm);
	const int orig_x = has_city ? city_x : st.x * 768 + 384, orig_y = has_city ? city_y : st.y * 768 + 384;
	if (P.starting_resources == 7) n += 8;    // WINE (TypeIndex 8) used as a number
	if (P.starting_resources == 8) n += 12;   // NUM_GAIATYPES
	if (bonus == 20 && konst("americans_starting_farms") > 3 && town == 2) n = n - 3 + konst("americans_starting_farms");
	if (bonus == 19) {
		if (town == 2) n -= 3;
		else if (town == 3) n -= 5;
	}
	if (town != 0) {
		const int scout = graft(kScoutType);
		place_unit(who, has_city, scout, orig_x, orig_y, 0, 0);
		if (bonus == 9) {
			for (int i = 0; i < konst("spanish_extra_scout"); i++) place_unit(who, has_city, scout, orig_x, orig_y, 0, 0);
			if (P.reveal_map > 1) place_unit(who, has_city, scout, orig_x, orig_y, 0, 0);
		}
		if (bonus == 22) {
			place_unit(who, has_city, kMerchantDutch, orig_x, orig_y, 0, 0);
			place_unit(who, has_city, kMerchantDutch, orig_x, orig_y, 0, 0);
		}
	}
	if (bonus == 5 && konst("greek_start_scholars") != 0 && has_city) {
		for (int i = 0; i < konst("greek_start_scholars"); i++) {
			const int e = place_unit(who, has_city, graft(kScholarType), orig_x, orig_y, 0, 0);
			if (e >= 0) res.empire[size_t(e)].note = 2;  // Unit::go_inside the university
		}
	}
	if (bonus == 16) n += K.at(uint32_t(constant_offset("korean_citizens")));
	for (int i = 0; i < n; i++) {
		const int cit = graft(kCitizenType);
		if (town < 2) {
			place_unit(who, has_city, cit, orig_x, orig_y, 0, 0);
		} else if (i < n_farm) {
			// created by Objects::init_unit at the first farm (slot 2001), come_out(0) and a gather order: no ring draw
			emit_unit(who, start, cit, -1, -1, 1);
			active_units[size_t(who)]++;
		} else {
			// ❓ randommap-spec §8: the later citizens look for a farm with room (not known before the site search); here
			// they are placed near the city
			place_unit(who, has_city, cit, orig_x, orig_y, 0, 0);
		}
	}
}

// Setup::build_game steps 7 and 9
void Gen::build_empires() {
	// step 7: start_order
	std::array<int, 8> order{};
	if (P.randomize_starts) {
		std::array<int, 8> used{};
		for (int i = 0; i < 8; i++) {
			int s;
			int guard = 0;
			do {
				s = r.mask(7);
				if (++guard > 5000) {
					for (s = 0; s < 8 && used[size_t(s)]; s++) {
					}
					break;
				}
			} while (used[size_t(s)]);
			used[size_t(s)] = 1;
			order[size_t(i)] = s;
		}
	} else {
		for (int i = 0; i < 8; i++) order[size_t(i)] = i;
	}
	res.start_order = order;
	phase("start order");
	// step 9: empires
	occupied.assign(size_t(cells), 0);
	blocked_tiles.assign(size_t(w.tile_size), 0);
	active_units.fill(0);
	std::array<int, 10> placed{};
	int next_start = 0, placed_count = 0;
	for (int pass = 0; pass < 10 && placed_count < nations; pass++) {
		for (int i = 0; i < 8; i++) {
			const int who = order[size_t(i)];
			if (!valid_leader_slot(P, who) || placed[size_t(who)]) continue;
			const int start = P.randomize_starts ? next_start : who;
			build_empire(who, start);
			placed[size_t(who)] = 1;
			placed_count++;
			next_start++;
			if (style != kStyleBritishIsles) {
				for (int j = 0; j < 8; j++) {
					if (!valid_leader_slot(P, j) || placed[size_t(j)] || !is_team(P, who, j)) continue;
					build_empire(j, next_start++);
					placed[size_t(j)] = 1;
					placed_count++;
				}
			}
		}
	}
	phase("build_empire");
}

}  // namespace ron::sim::mapgen
