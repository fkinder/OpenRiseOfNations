// Resource placement (docs/randommap-resources.md): Map::place_resources with the BONUSES / FISH / GOODIES passes, the
// ResourceDivvyPool, place_player_resource, place_region_resource, Objects::init_good / init_item effects and the herds.
#include <array>

#include "sim/mapgen_internal.hpp"

namespace ron::sim::mapgen {

int good_key(const Rules& rules, const std::string& name);  // mapgen_data.cpp

namespace {

constexpr int kGoodyType = 543;  // item type 0x21f
constexpr int kFish = 6, kWhales = 31;

struct HerdTab {
	int good, unit;
};
const HerdTab kHerdTab[] = {{6, 411}, {14, 407}, {21, 410}, {23, 409}, {25, 413}, {31, 412}, {33, 408}};

}  // namespace

// ---------------------------------------------------------------- the pool (ResourceDivvyPool)

int pool_age(const Rules& rules, int g) {
	const Type& t = rules.types[size_t(g)];
	const int p = t.preq[0];
	if (p < 0 || p >= kNumTypes) return 0;
	const Type& q = rules.types[size_t(p)];
	return q.age + ((p >= kAges && p < kEndAges) ? 1 : 0);
}

void Gen::pool_add(int g) {
	const auto& water = lands_rare[2];
	if (std::find(water.begin(), water.end(), g) != water.end()) {
		pool_water.push_back(g);
		if (g == kFish) {
			for (int i = 0; i < 9; i++) pool_water.push_back(g);
		}
		return;
	}
	if (pool_age(rules, g) < 3) pool_early.push_back(g);
	else pool_late.push_back(g);
}

void Gen::pool_done() {
	bits_early.assign(pool_early.size(), 0);
	bits_late.assign(pool_late.size(), 0);
	bits_water.assign(pool_water.size(), 0);
}

int Gen::pool_get(int which) {
	std::vector<int>& list = which == 0 ? pool_early : which == 1 ? pool_late : pool_water;
	std::vector<uint8_t>& bits = which == 0 ? bits_early : which == 1 ? bits_late : bits_water;
	if (list.empty()) return -1;
	int result = list[0];
	for (int guard = 0; guard < 100000; guard++) {
		const int i = (int(list.size()) - 1 > 0) ? r.rnd(int(list.size())) : 0;
		if (!bits[size_t(i)]) {
			bits[size_t(i)] = 1;
			result = list[size_t(i)];
			break;
		}
	}
	bool all = true;
	for (uint8_t b : bits) {
		if (!b) all = false;
	}
	if (all) std::fill(bits.begin(), bits.end(), 0);
	return result;
}

// ---------------------------------------------------------------- XML

std::vector<BonusEntry> Gen::read_bonuses(const char16_t* name) {
	std::vector<BonusEntry> out;
	const XNode* el = section(name);
	if (!el) return out;
	for (const XNode* e : xml::children(el, u"BONUS")) {
		BonusEntry b;
		b.type = to_utf8(xml::get_attrib(e, u"type"));
		const std::string lt = lc(b.type);
		const int gk = b.type.empty() ? -1 : good_key(rules, b.type);
		if (gk >= 0) {
			b.kind = 0;
			b.good = gk;
			b.resolved = true;
		} else if (lt == "random") {
			b.kind = 1;
			b.resolved = true;
		} else if (lt.size() >= 5 && lt.compare(0, 5, "goody") == 0) {
			b.kind = 0;
			b.good = kGoodyType;
			b.resolved = true;
		} else if (lt == "early") {
			b.kind = 2;
			b.resolved = true;
		} else if (lt == "late") {
			b.kind = 3;
			b.resolved = true;
		}
		b.chance = xml::get_attrib_num(e, u"chance", -1);
		b.group = xml::get_attrib_num(e, u"group", -1);
		b.numrare = scale_number(e, u"numrare");
		const std::string pat = lc(to_utf8(xml::get_attrib(e, u"pattern")));
		b.pattern = pat == "continent" ? 1 : pat == "world" ? 2 : pat == "nonplayer" ? 3 : pat == "corner" ? 4 : 0;
		b.saturate = std::max(0, xml::get_attrib_num(e, u"saturate", 0));
		b.pka = xml::get_attrib_num(e, u"player_keep_away", -1);
		b.psn = scale_number(e, u"player_stay_near");
		if (b.pattern == 0 && b.psn < 0) b.psn = 0;
		b.sp = std::max(1, scale_number(e, u"spacing"));
		b.gsp = std::max(1, scale_number(e, u"group_spacing"));
		b.ck = scale_number(e, u"center_keep_away");
		b.cs = scale_number(e, u"center_stay_near");
		b.ek = scale_number(e, u"edge_keep_away");
		b.es = scale_number(e, u"edge_stay_near");
		b.cok = scale_number(e, u"corner_keep_away");
		b.cos = scale_number(e, u"corner_stay_near");
		out.push_back(b);
	}
	return out;
}

// ---------------------------------------------------------------- shared predicates

int Gen::land_class(int x, int y) {
	const WData& d = W(x, y);
	const uint16_t f = d.flags;
	if (f & 4) return 3;
	if (f & 0x20) return 4;
	if (f & 0x50) return 5;
	if (f & 8) return (f & 0x800) ? 7 : 6;
	if (f & 0x100) return d.land;
	if ((d.land == 1 || d.land == 2) && (f & 0x800)) return 7;
	return d.land;
}

bool Gen::rare_ok(int u, int good) const {
	if (u < 0 || u >= 9) return false;
	const auto& l = lands_rare[size_t(u)];
	return std::find(l.begin(), l.end(), good) != l.end();
}

bool Gen::has_south_forest(int x, int y) {
	static const int dx[3] = {1, 0, 1}, dy[3] = {1, 1, 0};
	for (int i = 0; i < 3; i++) {
		const int nx = x + dx[i], ny = y + dy[i];
		if (inb(nx, ny) && (W(nx, ny).flags & 0x20)) return true;
	}
	return false;
}

// sp test: every valid object of the kind must be at least `sp` away.
bool Gen::object_spacing_ok(int x, int y, int sp, bool water, bool goody) const {
	if (goody) {
		for (const auto& it : placed_items) {
			if (vd(x - it[0], y - it[1]) < sp) return false;
		}
		return true;
	}
	for (const auto& gd : placed_goods) {
		const int type = gd[2];
		if (type == kOilGood) continue;
		const bool wg = type == kFish || type == kWhales;
		if (wg != water) continue;
		if (vd(x - gd[0], y - gd[1]) < sp) return false;
	}
	return true;
}

void Gen::put_good(int good, int x, int y) {
	const Type& gt = rules.types[size_t(good)];
	const int cx = x * 768 + ((gt.x_size & 1) ? 0x120 : 0x180);
	const int cy = y * 768 + ((gt.y_size & 1) ? 0x120 : 0x180);
	GoodObj go;
	go.type = good;
	go.x = cx;
	go.y = cy;
	go.flags = objflag::kValid;
	int slot = int(c.goods.size());  // Objects::init_good: the first free slot, else a new one
	for (size_t i = 0; i < c.goods.size(); i++) {
		if (!c.goods[i].alive()) {
			slot = int(i);
			break;
		}
	}
	if (size_t(slot) == c.goods.size()) c.goods.push_back(go);
	else c.goods[size_t(slot)] = go;
	good_init(w, good, gt, cx, cy);
	if (good != kOilGood) {
		W(x, y).down = -2;
		W(x, y).down_who = int16_t(slot);
	}
	placed_goods.push_back({x, y, good});
	res.num_goods++;
	for (const HerdTab& h : kHerdTab) {
		if (h.good == good) {
			HerdRec hr;
			hr.id = int(herds.size());
			hr.wx = x;
			hr.wy = y;
			hr.unit_type = h.unit;
			hr.good_slot = slot;
			herds.push_back(hr);
		}
	}
}

void Gen::put_goody(int x, int y) {
	const int cx = x * 768 + 0x180, cy = y * 768 + 0x180;
	GoodObj it;
	it.type = kGoodyType;
	it.x = cx;
	it.y = cy;
	it.flags = objflag::kValid;
	int slot = int(c.items.size());  // Objects::init_item: the first free slot, else a new one
	for (size_t i = 0; i < c.items.size(); i++) {
		if (!c.items[i].alive()) {
			slot = int(i);
			break;
		}
	}
	if (size_t(slot) == c.items.size()) c.items.push_back(it);
	else c.items[size_t(slot)] = it;
	item_init(w, cx, cy);
	W(x, y).down = -3;
	W(x, y).down_who = int16_t(slot);
	placed_items.push_back({x, y});
	res.num_goodies++;
}

// ---------------------------------------------------------------- place_player_resource @691f70

int Gen::place_player_resource(int good, int count, const BonusEntry& b0, int kind, bool water_class) {
	BonusEntry b = b0;
	const Circles& C = circles();
	const int S = int(starts.size());
	std::vector<WCoord> placed;
	int total = 0;
	const bool goody = good == kGoodyType;
	for (int i = 0; i < nations; i++) {
		if (i >= S) continue;
		const WCoord st = starts[size_t(i)];
		if (!inb(st.x, st.y)) continue;
		for (int cn = 0; cn < count; cn++) {
			if (kind != 0) good = (kind == 2) ? pool_get(0) : pool_get(1);
			if (good < 0) continue;
			b.psn = (b.psn > 64 || b.psn == 0) ? 64 : b.psn;
			b.pka = std::min(b.pka, b.psn);
			const int pka = std::max(0, b.pka);
			const int wd = C.radius[size_t(b.psn)] - C.radius[size_t(pka)];
			if (wd <= 0) continue;
			const int off = r.rnd(wd + 1);
			for (int j = 0; j < wd; j++) {
				const int k = (off + j) % wd + C.radius[size_t(pka)];
				const int x = st.x + C.x[size_t(k)], y = st.y + C.y[size_t(k)];
				if (!inb(x, y)) continue;
				const WData& d = W(x, y);
				if (start_city_wcoord(x, y)) continue;  // "not in the forest bit set": +0xf8 is the start-city cell set
				if (d.flags & 0x1000) continue;
				if ((d.flags & 0x1878) || d.blocked != 0) continue;
				bool ok = true;
				if (b.pka > 0) {
					for (const WCoord& s : starts) {
						if (vd(x - s.x, y - s.y) < b.pka) ok = false;
					}
					if (!ok) continue;
				}
				{
					const int own = vd(x - st.x, y - st.y);
					if (own > b.psn) continue;
					for (int q = 0; q < S && ok; q++) {
						if (q != i && vd(x - starts[size_t(q)].x, y - starts[size_t(q)].y) <= own) ok = false;
					}
					if (!ok) continue;
				}
				if (b.ck != 0 && center_d(x, y) < b.ck) continue;
				if (b.cs > 0 && center_d(x, y) > b.cs) continue;
				if (b.ek > 0 && edge_d(x, y) < b.ek) continue;
				if (b.es > 0 && edge_d(x, y) > b.es) continue;
				if (b.cok > 0 && corner_d(x, y, -1) < b.cok) continue;
				if (b.cos > 0 && corner_d(x, y, -1) > b.cos) continue;
				if (has_south_forest(x, y)) continue;
				if (b.gsp > 0) {
					for (const WCoord& p : placed) {
						if (vd(x - p.x, y - p.y) < b.gsp) ok = false;
					}
					if (!ok) continue;
					if (x == 0 || x == xs - 1 || y == 0 || y == xs - 1) continue;  // sic: y is compared with xs - 1
				}
				if (b.sp > 0 && !object_spacing_ok(x, y, b.sp, water_class, goody)) continue;
				if (!goody) {
					for (int o = 1; o <= 4; o++) {
						const int nx = x + orthog_x[o], ny = y + orthog_y[o];
						if (inb(nx, ny) && (W(nx, ny).flags & 4)) ok = false;
					}
					if (!ok || has_mountain_tcoords(x, y)) continue;
					if (!rare_ok(land_class(x, y), good)) continue;
				} else {
					if ((!(d.flags & 0x100) && (d.land == 1 || d.land == 2)) || (d.flags & 0x70)) continue;
					if (has_mountain_tcoords(x, y)) continue;
				}
				if (d.down < -1) continue;
				if (goody) put_goody(x, y);
				else put_good(good, x, y);
				placed.push_back({x, y});
				total++;
				break;
			}
		}
	}
	return total;
}

// ---------------------------------------------------------------- place_region_resource @690480

int Gen::place_region_resource(int good, const BonusEntry& b, int kind) {
	const bool water = good >= 0 && good != kGoodyType && rare_ok(2, good);
	const bool goody = good == kGoodyType;
	int cur = 0;
	const std::vector<int> list = find_avail_regions(water, b.pattern, &cur);
	if (list.empty()) return 0;
	const int len = int(list.size());
	const int count = b.numrare;
	const int INF_ = 1 << 30;
	int th = INF_;
	if (count > 2 && b.saturate == 0 && ((b.pattern != 1 && b.pattern != 3) || len == 1)) th = count >> 1;
	const int rounds = b.pattern == 1 ? len : (b.pattern == 4 ? 4 : 1);
	std::array<int, 8> score{};
	int worst = 0, total = 0;
	std::vector<WCoord> placed;
	int last_region = cur;
	const int S = int(starts.size());
	for (int rr = 0; rr < rounds; rr++) {
		int k = 0, guard = 0;
		while (k < count && ++guard < count * (len + 2) + 64) {
			if (kind != 0) good = water ? pool_get(2) : (kind == 2 ? pool_get(0) : pool_get(1));
			if (good < 0) break;
			const int R = list[size_t(cur)];
			const GenRegion& G = reg[size_t(R)];
			const int m = int(G.cells.size());
			bool did = false;
			if (m > 0) {
				const uint32_t start = m > 1 ? uint32_t(r.rnd(m) + 1) : 1u;
				uint32_t xi = start;
				do {
					if (xi <= uint32_t(m)) {
						const WCoord cell = G.cells[size_t(xi - 1)];
						if (region_accept(cell.x, cell.y, good, b, water, goody, rr, k, th, worst, placed)) {
							if (goody) put_goody(cell.x, cell.y);
							else put_good(good, cell.x, cell.y);
							for (int p = 0; p < S && p < 8; p++) {
								score[size_t(p)] += vd(cell.x - starts[size_t(p)].x, cell.y - starts[size_t(p)].y);
							}
							int best = 0, bv = 0;
							for (int p = 0; p < S && p < 8; p++) {
								if (score[size_t(p)] > bv) {
									bv = score[size_t(p)];
									best = p;
								}
							}
							worst = best;
							placed.push_back(cell);
							total++;
							did = true;
							break;
						}
					}
					xi = (xi >> 1) ^ ((xi & 1) ? 0xB400u : 0u);
					if (xi == 0) break;
				} while (xi != start);
			}
			if (b.pattern == 2 || b.pattern == 3) {
				const int prev = cur;
				cur = (cur + 1) % len;
				if (!did) {
					if (cur != last_region) k--;
				} else {
					last_region = prev;
				}
			}
			k++;
		}
		if (b.pattern == 1) cur = (cur + 1) % len;
	}
	return total;
}

// accept(x, y) of place_region_resource (§6.3)
bool Gen::region_accept(int x, int y, int good, const BonusEntry& b, bool water, bool goody, int round, int k, int th,
                        int worst, const std::vector<WCoord>& placed) {
	if (!inb(x, y)) return false;
	const WData& d = W(x, y);
	if (d.flags & 0x1000) return false;
	if (b.pka > 0) {
		for (const WCoord& s : starts) {
			if (vd(x - s.x, y - s.y) < b.pka) return false;
		}
	}
	if ((d.flags & 0x1878) || d.blocked != 0) return false;
	if (b.psn > 0) {
		for (const WCoord& s : starts) {
			if (vd(x - s.x, y - s.y) > b.psn) return false;
		}
	}
	if (b.ck > 0 && center_d(x, y) < b.ck) return false;
	if (b.cs > 0 && center_d(x, y) > b.cs) return false;
	if (b.ek > 0 && edge_d(x, y) < b.ek) return false;
	if (b.es > 0 && edge_d(x, y) > b.es) return false;
	const int cd = corner_d(x, y, b.pattern == 4 ? round : -1);
	if (b.cok > 0 && cd < b.cok) return false;
	if (b.cos > 0 && cd > b.cos) return false;
	if (b.gsp > 0) {
		for (const WCoord& p : placed) {
			if (vd(x - p.x, y - p.y) < b.gsp) return false;
		}
	}
	if (b.sp > 0 && !object_spacing_ok(x, y, b.sp, water, goody)) return false;
	if (!water) {
		for (int o = 1; o <= 8; o++) {
			const int nx = x + move_x[o], ny = y + move_y[o];
			if (!inb(nx, ny)) continue;
			const WData& n = W(nx, ny);
			if (!(n.flags & 0x100) && (n.land == 1 || n.land == 2)) return false;
		}
	} else {
		for (int o = 1; o <= 24; o++) {
			const int nx = x + move_x[o], ny = y + move_y[o];
			if (inb(nx, ny) && W(nx, ny).land == 0) return false;
		}
	}
	if (has_mountain_tcoords(x, y)) return false;
	if (x == 0 || x == xs - 1 || y == 0 || y == xs - 1) return false;  // sic: y is compared with xs - 1
	if (has_south_forest(x, y)) return false;
	if (goody) {
		if (d.flags & 0x70) return false;
	} else {
		const int u = land_class(x, y);
		if (!rare_ok(u, good)) return false;
		if (water && (d.flags & 4)) return false;
	}
	if (d.down < -1) return false;
	if (k >= th) {
		int best = 0, bd = 1 << 30;
		for (size_t q = 0; q < starts.size(); q++) {
			const int dd = vd(x - starts[q].x, y - starts[q].y);
			if (dd < bd) {
				bd = dd;
				best = int(q);
			}
		}
		if (best != worst) return false;
	}
	return true;
}

// ---------------------------------------------------------------- Map::place_resources @68f4f0

void Gen::place_resources() {
	pool_early.clear();
	pool_late.clear();
	pool_water.clear();
	for (int g = 6; g < kEndGoods; g++) {
		const Type& t = rules.types[size_t(g)];
		if (t.random_rare_drop != 0 && !(P.ctw && t.exclude_ctw != 0)) pool_add(g);
	}
	pool_done();
	int prev_group = -1, roll = 0, hit = 0;
	static const char16_t* passes[3] = {u"BONUSES", u"FISH", u"GOODIES"};
	for (int pass = 0; pass < 3; pass++) {
		const std::vector<BonusEntry> entries = read_bonuses(passes[pass]);
		for (const BonusEntry& b : entries) {
			if (!b.resolved) continue;
			if (b.group != prev_group || b.group == 0) {
				prev_group = b.group;
				roll = r.rnd(100);
				hit = 0;
			}
			if (roll < 0) {
				if (!(b.chance == 0 && hit)) {
					hit = 0;
					continue;
				}
			} else {
				roll -= b.chance;
				if (roll < 0) {
					hit = 1;
				} else {
					hit = 0;
					continue;
				}
			}
			const int count = b.numrare;
			if (count <= 0) continue;
			if (b.pattern == 0) {
				const bool wc = b.good == kFish || b.good == kWhales;
				BonusEntry bb = b;
				bb.numrare = count;
				place_player_resource(b.good, count, bb, b.kind, wc);
			} else {
				place_region_resource(b.good, b, b.kind);
			}
		}
	}
	res.num_goods = int(c.goods.size());
	res.num_goodies = int(c.items.size());
}

// Herd::create_units @7417f0 for every herd in slot order (Setup::build_game step 10).
void Gen::create_herd_units() {
	for (const HerdRec& h : herds) {
		if (h.unit_type < 0) continue;  // a herd closed by Terrain::move_goody
		const int units = h.unit_type == 412 ? 1 : h.unit_type == 411 ? 3 : 4;
		for (int u = 0; u < units; u++) {
			HerdAnimal a;
			a.herd = h.id;
			a.unit_type = h.unit_type;
			a.x = h.wx * 768 + 384;
			a.y = h.wy * 768 + 384;
			if (h.unit_type == 411) a.angle = 0;
			else if (h.unit_type == 412) a.angle = 0x80000000u;
			else a.angle = uint32_t(degrees_to_angle(r.rnd(360)));
			res.animals.push_back(a);
		}
	}
	res.herds = herds;
}

}  // namespace ron::sim::mapgen
