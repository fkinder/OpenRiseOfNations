// Formation layout (form.obj, forms.obj; docs/movement-spec.md section 11.2): FormData::type_cat, Form::categorize,
// Form::compute_rows_and_columns, Form::compute_dests, Form::compute and Group::compute_form.
//
// Ported from the disassembly (re/decomp/move_group/asm/Form_*.asm, Group_compute_form@00707c80.asm; the decompiles
// lose the sinx/find_angle register arguments). Integer arithmetic throughout (C division truncates towards zero,
// angles wrap mod 2^32) except one single-precision term in the category stacking (cvttss2si, kept in float with
// contraction disabled).
#include <algorithm>

#include "sim/game.hpp"
#include "sim/groups.hpp"
#include "sim/trig.hpp"

#pragma clang fp contract(off)

namespace ron::sim {

namespace {

constexpr int32_t kCatCommand = 8;

bool valid_on_map_unit(const Object* ob) {
	return ob && ob->is_unit && ob->alive() && static_cast<const Unit*>(ob)->on_map();
}
int32_t wmul(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }

// LeaderData::current_upgrade @006e3140: the leader's current upgrade of a type. TODO(techs): needs the leader's
// researched techs and tribe; without techs no upgrade is available, so the type itself.
int32_t current_upgrade(const Game& g, int32_t who, int32_t t) {
	(void)g;
	(void)who;
	return t;
}

// UnitData::is_caravan (type vtable +0x130). TODO(trade): caravans are not simulated; the merchant role bit is not
// traced, so none.
bool is_caravan(const Unit& u) {
	(void)u;
	return false;
}

// The type Form::categorize lays a captain out as: a transport carrying a unit to land uses the carried unit's type;
// a land unit that can transport itself, sent onto water, the leader's transport barge (merchant fleet for caravans).
int32_t effective_type(const Game& g, const Unit& u, bool water) {
	const Type& t = *u.ptype;
	if (!water) {
		if (t.domain == 1 && (t.unit_flags & 0x10) && u.inside_down >= 0) {
			const Object* c = g.object(u.inside_down_who, u.inside_down);
			if (c && c->ptype) return c->ptype->type;
		}
		return t.type;
	}
	if (t.domain == 0 && (((u.unit_masks & umask::kCanTransport) && !(u.unit_masks2 & umask2::kDisableTransport)) ||
	                      (t.unit_flags & 0x10))) {
		if (is_caravan(u)) {
			const int32_t v = current_upgrade(g, u.who, 0x13e);  // MERCHANTFLEET
			return v < 0 ? 0x13e : v;
		}
		const int32_t v = current_upgrade(g, u.who, 0x140);  // TRANSPORTBARGE
		return v < 0 ? 0x140 : v;
	}
	return t.type;
}

}  // namespace

void FormData::clear_scratch() {
	const int32_t f = form, d = density;
	*this = FormData{};
	form = f;
	density = d;
}

void forms_init(std::array<FormData, kNumForms>& forms) {
	for (int32_t i = 0; i < kNumForms; i++) {
		FormData& F = forms[size_t(i)];
		F = FormData{};
		F.form = i;
		F.density = i < 5 ? 2 : i == 5 ? 0 : 1;
	}
}

int32_t form_type_cat(const Game& g, int32_t type, int32_t who) {
	// FormData::type_cat @72dfc0 (FormCatIndex)
	const Type& t = g.type(type);
	const uint32_t f2 = t.unit_flags2, om = uint32_t(t.obj_masks);
	if (t.attack != 0 && !(f2 & 0x40) && !(f2 & 8) && t.type != 0x3d && t.type != 0x3e && t.type != 400) {
		if (om & 4) return 10;  // CIVILIAN
		if (!(om & 0x20)) {
			if (f2 & 4) return 6;
			if (om & 0x80000000u) return 6;
			if (!(om & 0x1000)) return (om & 0x200000) ? 0 : 6;
			if (t.max_range != 0 && (g.leaders[size_t(who)].flags & leaderflag::kHuman)) return 5;
			return 2;
		}
		return 3 + (t.max_range != 0 ? 1 : 0);
	}
	if (f2 & 0x60) return 6;
	return kCatCommand + ((om & 4) ? 2 : 0);
}

int32_t form_type_cat(const Game& g, const Unit& u) { return form_type_cat(g, u.ptype->type, u.who); }

int32_t form_categorize(Game& g, FormData& F, Group& gr, bool water) {
	// Form::categorize @72e250
	group_sort(g, gr);
	const int32_t n = gr.num, who = gr.who;
	F.num_category.fill(0);
	F.x_spacing.fill(0);
	F.y_spacing.fill(0);
	if (F.guarding != 0) {
		F.num_category[6]++;
		F.x_spacing[6] = 0xc0;
		F.y_spacing[6] = 0x180;
	}
	int32_t leader_o = -1, leader_i = -1, best = kNumFormCat, most = 0, most_cat = kCatCommand;
	auto captain_at = [&](int32_t i) -> Unit* {
		Object* ob = g.object(who, gr.list[size_t(i)]);
		if (!valid_on_map_unit(ob)) return nullptr;
		Unit* u = static_cast<Unit*>(ob);
		return u->o_up < 0 ? u : nullptr;
	};
	auto uber_k = [&](int32_t uber) { return F.form == 8 ? 2 : std::min(uber, 3); };
	// Pass 1: everything except COMMAND units.
	for (int32_t i = 0; i < n; i++) {
		Unit* u = captain_at(i);
		if (!u) continue;
		const int32_t t = effective_type(g, *u, water);
		const Type& T = g.type(t);
		const int32_t xs = T.x_spacing, ys = T.y_spacing;
		const int32_t c = form_type_cat(g, t, who);
		if (c == kCatCommand) continue;
		F.category[size_t(i)] = c;
		F.cat_id[size_t(i)] = F.num_category[size_t(c)];
		F.num_category[size_t(c)]++;
		const int32_t uber = T.uber_size;
		if (uber == 1) {
			F.x_spacing[size_t(c)] = std::max(xs, F.x_spacing[size_t(c)]);
			F.y_spacing[size_t(c)] = std::max(ys, F.y_spacing[size_t(c)]);
		} else {
			const int32_t k = std::max(uber_k(uber), 1);  // (uber 0 would divide by zero in the original)
			F.x_spacing[size_t(c)] = std::max(k * xs, F.x_spacing[size_t(c)]);
			const int32_t v = ((uber - 1) / k + 1) * ys;
			if (unit_is_modern_infantry(g, *u)) F.y_spacing[size_t(c)] = std::max(v + 0x30, F.y_spacing[size_t(c)]);
			else F.y_spacing[size_t(c)] = std::max(v, F.y_spacing[size_t(c)]);
		}
		if (most < F.num_category[size_t(c)]) {
			most_cat = c;
			most = F.num_category[size_t(c)];
		}
		if (leader_o < 0 || c < best) {
			leader_i = i;
			best = c;
			leader_o = gr.list[size_t(i)];
		}
	}
	// Pass 2: COMMAND units join the first populated category after the most populous one (else the one right after
	// it); SQUARE keeps them in COMMAND.
	for (int32_t i = 0; i < n; i++) {
		Unit* u = captain_at(i);
		if (!u) continue;
		const int32_t t = effective_type(g, *u, water);
		const Type& T = g.type(t);
		const int32_t xs = T.x_spacing, ys = T.y_spacing;
		if (form_type_cat(g, t, who) != kCatCommand) continue;
		int32_t c = kCatCommand;
		if (F.form != 6) {
			c = most_cat + 1;
			for (int32_t e = most_cat + 1; e < kNumFormCat; e++)
				if (F.num_category[size_t(e)] != 0) {
					c = e;
					break;
				}
			// most_cat 17 would give 18 and write past the arrays in the original (unreachable: no air formations).
			c = std::min(c, kNumFormCat - 1);
		}
		F.category[size_t(i)] = c;
		F.cat_id[size_t(i)] = F.num_category[size_t(c)];
		F.num_category[size_t(c)]++;
		const int32_t cnt = F.num_category[size_t(c)];
		const int32_t uber = T.uber_size;
		int32_t xsv;
		if (uber == 1) {
			xsv = std::max(xs, F.x_spacing[size_t(c)]);
			F.y_spacing[size_t(c)] = std::max(ys, F.y_spacing[size_t(c)]);
		} else {
			const int32_t k = std::max(uber_k(uber), 1);
			xsv = std::max(k * xs, F.x_spacing[size_t(c)]);
			F.y_spacing[size_t(c)] = std::max(((uber - 1) / k + 1) * ys, F.y_spacing[size_t(c)]);
		}
		F.x_spacing[size_t(c)] = xsv;
		if (most <= cnt) most = cnt;
		if (leader_o < 0 || c < best) {
			leader_i = i;
			best = c;
			leader_o = gr.list[size_t(i)];
		}
	}
	if (leader_i < 0) {
		leader_i = 0;
		leader_o = gr.list[0];
	}
	F.idx = leader_i;
	F.who = who;
	F.o = leader_o;
	return leader_o;
}

void form_compute_rows_and_columns(FormData& F, int32_t* rows, int32_t* cols, int32_t width) {
	// Form::compute_rows_and_columns @72d910
	if (F.form == 6) {
		// SQUARE: built around category 7. (compute_dests then skips every SQUARE captain, see there.)
		int32_t s = 0;
		while (s * s < F.num_category[7]) s++;
		int32_t ms = F.x_spacing[7];
		int32_t W = ms * s;
		F.per[7] = s;
		rows[7] = W;
		for (int32_t c = 6; c >= 0; c--) {
			const int32_t n = F.num_category[size_t(c)];
			if (n == 0) {
				rows[c] = 0;
				F.per[size_t(c)] = 0;
				continue;
			}
			const int32_t xs = F.x_spacing[size_t(c)];
			int32_t v = std::max(wmul(n, xs) / 4, W);
			ms = std::max(ms, xs);
			W = v + 2 * ms;
			F.across = W / xs;
			while (F.across * 4 - 4 < n) {
				W += xs;
				F.across = W / xs;
			}
			rows[c] = W;
			const int32_t per = n / 4;
			ms = xs;
			F.per[size_t(c)] = per;
			const int32_t rem = n % 4;
			const int32_t rv = F.reverse != 0 ? 1 : 0;
			auto side = [&](int32_t more) { return per == 0 ? 0 : W / (per + (rem > more ? 1 : 0)); };
			F.space[size_t(c)][size_t(rv)] = std::max(side(0), ms);
			F.space[size_t(c)][size_t(1 - rv)] = std::max(side(1), ms);
			F.space[size_t(c)][size_t(2 + rv)] = std::max(side(2), ms);
			F.space[size_t(c)][size_t(3 - rv)] = std::max(side(3), ms);
		}
		F.wedge = -1;
		F.across = W / 2;
		return;
	}
	if (F.form == 7) {
		// WEDGE: the wedge category is the first populated of 3, 4, 2, 5, 0, 1, 6, 7, then any.
		static const int32_t order[8] = {3, 4, 2, 5, 0, 1, 6, 7};
		F.wedge = 7;
		bool found = false;
		for (int32_t k : order) {
			F.wedge = k;
			if (F.num_category[size_t(k)] != 0) {
				found = true;
				break;
			}
		}
		if (!found)
			for (int32_t c = 0; c < kNumFormCat; c++)
				if (F.num_category[size_t(c)] != 0) {
					F.wedge = c;
					break;
				}
		for (int32_t c = 0; c < kNumFormCat; c++) {
			const int32_t n = F.num_category[size_t(c)];
			if (c == F.wedge) {
				// Triangular rows: the original seeds the count with rows[c], an uninitialised stack slot of
				// Form::compute (open question, movement-spec 15.4); 0 here.
				if (n > 0) {
					int32_t r = rows[c], sum = 0;
					do {
						r++;
						sum += r;
					} while (sum < n);
					rows[c] = r;
				}
				continue;
			}
			int32_t k = std::max(n / 2, 10);
			cols[c] = k;
			if (F.density == 2) {
				const int32_t m = std::min(k, n);
				if ((m + 1) / 2 < 1) {
					cols[c] = 1;
					rows[c] = (n - 1 + cols[c]) / cols[c];
					continue;
				}
				cols[c] = (std::min(k, n) + 1) / 2;
			}
			rows[c] = (n - 1 + cols[c]) / cols[c];
		}
		return;
	}
	int32_t W = 0;
	for (int32_t c = 0; c < kNumFormCat; c++) {
		const int32_t v = F.x_spacing[size_t(c)] * F.num_category[size_t(c)];
		if (F.num_category[size_t(c)] < 6) W = std::max(W, v);
		else W = std::max(W, v / 2);
	}
	const int32_t K = wmul(W, width) / 50;
	for (int32_t c = 0; c < kNumFormCat; c++) {
		const int32_t n = F.num_category[size_t(c)];
		if (F.form == 8) {
			rows[c] = (n + 2) / 3;  // COLUMN leaves cols[] untouched (uninitialised in the original)
		} else if (n != 0) {
			int32_t k = std::min(K / F.x_spacing[size_t(c)], n);
			if (k < 1) k = 1;
			cols[c] = k;
			rows[c] = (n - 1 + k) / k;
		} else {
			cols[c] = 0;
			rows[c] = 0;
		}
	}
	F.wedge = -1;
}

void form_compute_dests(Game& g, FormData& F, Group& gr, const int32_t* rows, const int32_t* cols, int32_t x,
                        int32_t y, uint32_t A, int32_t reverse) {
	// Form::compute_dests @72cba0
	const int32_t n = gr.num, who = gr.who;
	gr.form_num = n;
	int32_t rank = 0, cap = 0, ring = 5, mob_cnt = 0;
	int32_t density = 1;  // the last captain's density: always 2 (category >= 0), so the depth factor below is 1
	bool first_follower = true;
	uint32_t mob_ang = 0x55555555u;
	int32_t lat0 = 0, dep0 = 0;
	auto place = [&](int32_t i, int32_t lat, int32_t dep) {
		F.to_x[size_t(i)] = sin_a(A, dep) + cos_a(A, lat) + x;
		F.to_y[size_t(i)] = sin_a(A, lat) - cos_a(A, dep) + y;
		F.off_x[size_t(i)] = lat;
		F.off_y[size_t(i)] = dep;
	};
	for (int32_t i = 0; i < n; i++) {
		const int32_t o = gr.list[size_t(i)];
		const Object* ob = g.object(who, o);
		if (!valid_on_map_unit(ob)) continue;
		const Unit& u = *static_cast<const Unit*>(ob);
		if (u.o_up < 0) {
			// ---- captain
			const int32_t c = F.category[size_t(i)];
			rank = F.cat_id[size_t(i)];
			cap = i;
			first_follower = true;
			const int32_t ys = F.y_spacing[size_t(c)], xs = F.x_spacing[size_t(c)];
			density = c >= 0 ? 2 : F.density;
			const int32_t fac = density != 0 ? 1 : 2;  // (2 - (density != 0))
			int32_t lat = 0, dep = 0;
			if (c == F.wedge) {
				int32_t row = 0;
				F.total = 0;
				if (rank > 0) {
					int32_t tot = 0;
					do {
						row++;
						tot += row;
					} while (row + 1 + tot <= rank);
					F.total = tot;
				}
				const int32_t pos = rank - F.total;
				int32_t l = (row & 1) == 0 ? ((pos + 1) >> 1) * xs : (pos >> 1) * xs + xs / 2;
				if (pos & 1) l = -l;
				dep = -fac * xs * row;  // ((density != 0) - 2) * xs * row
				lat = reverse ? -l : l;
				gr.angles[size_t(i)] = 0;
				place(i, lat, dep);
				continue;
			}
			if (F.form == 6) continue;  // SQUARE: captains outside the wedge get no slot (to_* stay 0)
			if (F.form == 8) {
				lat = ((rank + 1) % 3 - 1) * xs;
				dep = -(rank / 3) * ys;
				if (reverse) lat = -lat;
				// COLUMN does not write Group.angles[i] (keeps the previous value).
			} else if (F.form == 9) {
				if (rank != 0) {
					const int32_t r = (xs * ring) / 5;
					const int32_t cv = cos_a(mob_ang, r), sv = sin_a(mob_ang, r);
					mob_cnt++;
					uint32_t inc;
					if (ring & 1) inc = (0xffffffffu / uint32_t(ring)) * 2u;
					else if (ring == 10) inc = 0x4ccccccbu;
					else inc = uint32_t(ring / 10 + 1) * (0xffffffffu / uint32_t(ring));
					mob_ang += inc;
					if (mob_cnt == ring) {
						ring += 5;
						mob_cnt = 0;
						mob_ang += 0xffffffffu / uint32_t(2 * ring);
					}
					lat = reverse ? -cv : cv;
					dep = sv;
				}
			} else {
				const int32_t C = cols[c];
				const int32_t q = rank / C, r = rank % C;
				int32_t h = density == 0 ? ((r + 1) & ~1) : ((r + 1) >> 1);
				h *= xs;
				int32_t l = (r & 1) ? -h : h;
				if (!(C & 1) && F.guarding == 0) l = xs / 2 + l;
				if ((c <= 2 || c == 6 || c >= 12) && (q & 1)) l += xs / 2;
				const int32_t D = -fac * q * ys;
				if (F.form == 1) dep = D - iabs(l);
				else if (F.form == 2) dep = D + iabs(l);
				else if (F.form == 4) dep = reverse ? D - l : D + l;
				else if (F.form == 3) dep = reverse ? D + l : D - l;
				else dep = D;
				lat = reverse ? -l : l;
				int8_t& a = gr.angles[size_t(i)];
				a = 0;
				if (lat < 0) {
					if (dep - D < 0) a = int8_t(0xe0);
					else if (dep - D > 0) a = 0x20;
				} else if (lat > 0) {
					if (dep - D < 0) a = 0x20;
					else if (dep - D > 0) a = int8_t(0xe0);
				} else {
					if (F.form == 4) a = int8_t(0xe0);
					else if (F.form == 3) a = 0x20;
				}
			}
			// Stack the categories front to back.
			if (F.wedge >= 0) dep -= F.x_spacing[size_t(F.wedge)] * rows[F.wedge] * fac;
			int32_t prev = -1;
			for (int32_t k = 0; k <= c; k++) {
				if (k == F.wedge || F.num_category[size_t(k)] == 0) continue;
				if (prev < 0) prev = k;
				else dep -= (fac * F.y_spacing[size_t(k)]) / 2;
				if (k < c) {
					const float v = (float(rows[k]) - 0.5f) * float(F.y_spacing[size_t(k)]) * float(fac);
					dep -= int32_t(v);  // cvttss2si
				}
			}
			place(i, lat, dep);
			if (prev == c && rank == 0) {
				lat0 = lat;
				dep0 = dep;
				if (F.guarding != 0 && c == 6) lat0 = dep0 = 0;
			}
		} else {
			// ---- follower: beside its captain (the first on one side, the others on the other)
			int32_t gs = u.ptype->guy_spacing;
			if (!first_follower) gs = -gs;
			int32_t lat = reverse ? -gs : gs, dep = 0;
			const int32_t cox = F.off_x[size_t(cap)];
			switch (F.form) {
			case 1:
				dep = cox < 0 ? lat : cox > 0 ? -lat : -iabs(lat);
				break;
			case 2:
				dep = cox < 0 ? -lat : cox > 0 ? lat : iabs(lat);
				break;
			case 4:
				dep = lat;
				break;
			case 3:
				dep = -lat;
				break;
			default:
				break;
			}
			int8_t& a = gr.angles[size_t(i)];
			a = 0;
			if (lat < 0) {
				if (dep < 0) a = int8_t(0xe0);
				else if (dep > 0) a = 0x20;
			} else if (lat > 0) {
				if (dep < 0) a = 0x20;
				else if (dep > 0) a = int8_t(0xe0);
			}
			if (unit_is_modern_infantry(g, u)) {
				dep += ((wmul(wmul(x / 0x71, o), i) + rank * 7) / 5 % 3) * 0x30 - 0x30;
				const int32_t gs2 = u.ptype->guy_spacing;
				const int32_t v = ((wmul(wmul(y / 0x61, o), i) + rank * 13) / 7 % 3) * gs2;
				lat += v / 4 - gs2 / 4;
			}
			F.to_x[size_t(i)] = F.to_x[size_t(cap)] + cos_a(A, lat) + sin_a(A, dep);
			F.to_y[size_t(i)] = F.to_y[size_t(cap)] - cos_a(A, dep) + sin_a(A, lat);
			first_follower = false;
			F.off_x[size_t(i)] = F.off_x[size_t(cap)] + lat;
			F.off_y[size_t(i)] = F.off_y[size_t(cap)] + dep;
		}
	}
	// Recentre on rank 0 of the front category. The original passes radius 0 to the lateral cosx/sinx terms, so only
	// the depth part moves the destinations; both parts move the offsets.
	for (int32_t i = 0; i < n; i++) {
		if (!valid_on_map_unit(g.object(who, gr.list[size_t(i)]))) continue;
		F.to_x[size_t(i)] += sin_a(A, -dep0);
		F.to_y[size_t(i)] += 0 - cos_a(A, -dep0);
		F.off_x[size_t(i)] -= lat0;
		F.off_y[size_t(i)] -= dep0;
		gr.off_x[size_t(i)] = ucoord_of(F.off_x[size_t(i)]);
		gr.off_y[size_t(i)] = ucoord_of(F.off_y[size_t(i)]);
	}
	if (F.guarding != 0) {
		const int32_t fac = density != 0 ? 1 : 2;
		int32_t d = 0, prev = -1;
		for (int32_t k = 0; k <= 6; k++) {
			if (k == F.wedge || F.num_category[size_t(k)] == 0) continue;
			if (prev < 0) prev = k;
			else d -= (fac * F.y_spacing[size_t(k)]) / 2;
			if (k < 6) {
				const float v = (float(rows[k]) - 0.5f) * float(F.y_spacing[size_t(k)]) * float(fac);
				d -= int32_t(v);
			}
		}
		for (int32_t i = 0; i < n; i++)
			if (valid_on_map_unit(g.object(who, gr.list[size_t(i)]))) F.off_y[size_t(i)] -= d;
	}
}

void form_compute(Game& g, FormData& F, Group& gr, int32_t x, int32_t y, uint32_t angle, int32_t width,
                  int32_t facing) {
	// Form::compute @72e8e0
	F.reverse = facing;
	int32_t rows[kNumFormCat] = {}, cols[kNumFormCat] = {};
	form_compute_rows_and_columns(F, rows, cols, width);
	form_compute_dests(g, F, gr, rows, cols, x, y, angle, facing);
	const int32_t dx = x - F.to_x[size_t(F.idx)], dy = y - F.to_y[size_t(F.idx)];
	gr.o_angle = find_angle(dx, dy);
	gr.o_dist = vector_dist(dx, dy);
}

FormData* group_compute_form(Game& g, Group& gr, int32_t x, int32_t y, int32_t form, int32_t width, bool set_angle,
                             uint32_t* angle, int32_t loc_x, int32_t loc_y, int32_t guarding) {
	// Group::compute_form @707c80
	if (form == kFormKeep) {
		if (FormData* K = form_compute_keep(g, gr, x, y, set_angle, angle, loc_x, loc_y)) return K;
		// A scattered selection (or none to lay out): the original's LINE instead, and the group moves as LINE.
		form = 0;
		gr.form = 0;
	}
	const int32_t li = group_find_leader(g, gr);  // "No valid group for formation"
	if (li < 0) return nullptr;
	const Unit& L = *g.unit(gr.who, gr.list[size_t(li)]);
	const int32_t dx = x - loc_x, dy = y - loc_y;
	FormData& F = g.forms[size_t(form)];
	F.clear_scratch();
	F.guarding = guarding;
	const World& w = g.world;
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	const bool water = w.in_t(tx, ty) && (w.t(tx, ty) & tmask::kMedium) == tmask::kWater;
	form_categorize(g, F, gr, water);
	F.to_x[0] = x;
	F.to_y[0] = y;
	bool flip = false;
	// The leader's angle less its slot's facing offset (read after categorize: Group::sort may have moved members).
	auto leader_base = [&]() { return L.angle - (uint32_t(int32_t(gr.angles[size_t(li)])) << 24); };
	if (!set_angle) {
		if (dx == 0 && dy == 0) {
			if (loc_x != gr.ox || loc_y != gr.oy) *angle = leader_base();
			else *angle = gr.o_angle;
		} else {
			*angle = find_angle(dx, dy);
		}
	} else {
		const uint32_t d = find_angle(dx, dy) - *angle;
		flip = d >= 0x40000000u && d <= 0xc0000000u;
	}
	const uint32_t u = leader_base() - *angle;
	const bool away = u >= 0x40000000u && u <= 0xc0000000u;
	if (away) gr.facing = gr.facing == 0 ? 1 : 0;
	if (g.editor) {
		gr.facing = 0;
		flip = false;
	}
	form_compute(g, F, gr, x, y, *angle, width, uint8_t(gr.facing));
	if (away) gr.facing = gr.facing == 0 ? 1 : 0;
	if (flip && guarding == 0) {
		for (int32_t i = 0; i < gr.num; i++) {
			F.off_x[size_t(i)] = -F.off_x[size_t(i)];
			F.off_y[size_t(i)] = -F.off_y[size_t(i)];
			gr.off_x[size_t(i)] = -gr.off_x[size_t(i)];
			gr.off_y[size_t(i)] = -gr.off_y[size_t(i)];
		}
	}
	return &F;
}

FormData* form_compute_keep(Game& g, Group& gr, int32_t x, int32_t y, bool set_angle, uint32_t* angle, int32_t loc_x,
                            int32_t loc_y) {
	// KEEP (kFormKeep): integer arithmetic only (sin_a / cos_a tables, 64-bit products), so it is as deterministic as
	// the original's layouts. Uses forms[0] as scratch like LINE would; categorize gives the member order and leader.
	if (group_find_leader(g, gr) < 0) return nullptr;
	FormData& F = g.forms[0];
	F.clear_scratch();
	const World& w = g.world;
	const int32_t tx = tcoord_of(x), ty = tcoord_of(y);
	const bool water = w.in_t(tx, ty) && (w.t(tx, ty) & tmask::kMedium) == tmask::kWater;
	form_categorize(g, F, gr, water);
	const int32_t n = gr.num;
	auto member = [&](int32_t i) -> const Unit* {
		const Object* ob = g.object(gr.who, gr.list[size_t(i)]);
		return valid_on_map_unit(ob) ? static_cast<const Unit*>(ob) : nullptr;
	};
	// The arrangement's centre (captains only: followers stand beside their captain) and its facing.
	int64_t sx = 0, sy = 0;
	int32_t caps = 0, fx = 0, fy = 0;
	constexpr int32_t kR = 0x1000;
	for (int32_t i = 0; i < n; i++) {
		const Unit* u = member(i);
		if (!u || u->o_up >= 0) continue;
		sx += u->x, sy += u->y, caps++;
		fx += sin_a(u->angle, kR);
		fy -= cos_a(u->angle, kR);
	}
	if (caps == 0) return nullptr;
	const int32_t cx = int32_t(sx / caps), cy = int32_t(sy / caps);
	// Units spread over the map are not an arrangement: the caller lays them out as LINE instead.
	constexpr int32_t kMaxExtent = 0x1800;  // 32 tiles from the centre
	for (int32_t i = 0; i < n; i++) {
		const Unit* u = member(i);
		if (u && u->o_up < 0 && vector_dist(u->x - cx, u->y - cy) > kMaxExtent) return nullptr;
	}
	// The travel angle: as compute_form, from the group's location to the destination (or the commanded facing).
	uint32_t A;
	if (set_angle) {
		A = *angle;
	} else {
		const int32_t dx = x - cx, dy = y - cy;
		if (dx == 0 && dy == 0) {
			const int32_t ldx = x - loc_x, ldy = y - loc_y;
			A = (ldx == 0 && ldy == 0) ? gr.o_angle : find_angle(ldx, ldy);
		} else {
			A = find_angle(dx, dy);
		}
		*angle = A;
	}
	// The facing the arrangement is read in: the mean of the captains' facings when they agree (resultant at least
	// half the radius), else the travel angle itself (the arrangement is then translated, not turned).
	const uint32_t facing = vector_dist(fx, fy) >= (kR * caps) / 2 ? find_angle(fx, fy) : A;
	constexpr int32_t kT = 0x4000;  // sin_table's negative-radius branch multiplies in 32 bits: |r| * 0xffff must fit
	const int64_t fc = cos_a(facing, kT), fs = sin_a(facing, kT);
	for (int32_t i = 0; i < n; i++) {
		const Unit* u = member(i);
		const int32_t wx = u ? u->x - cx : 0, wy = u ? u->y - cy : 0;
		// Formation frame: world = (cos(A, lat) + sin(A, dep), sin(A, lat) - cos(A, dep)); its inverse at `facing`.
		const int32_t lat = int32_t((int64_t(wx) * fc + int64_t(wy) * fs) / kT);
		const int32_t dep = int32_t((int64_t(wx) * fs - int64_t(wy) * fc) / kT);
		F.off_x[size_t(i)] = lat;
		F.off_y[size_t(i)] = dep;
		F.to_x[size_t(i)] = x + cos_a(A, lat) + sin_a(A, dep);
		F.to_y[size_t(i)] = y + sin_a(A, lat) - cos_a(A, dep);
		gr.angles[size_t(i)] = 0;
	}
	// Group offsets relative to the leader's slot (what update_positions / do_group_move follow).
	const int32_t lat0 = F.off_x[size_t(F.idx)], dep0 = F.off_y[size_t(F.idx)];
	for (int32_t i = 0; i < n; i++) {
		gr.off_x[size_t(i)] = ucoord_of(F.off_x[size_t(i)] - lat0);
		gr.off_y[size_t(i)] = ucoord_of(F.off_y[size_t(i)] - dep0);
	}
	gr.form_num = n;
	F.reverse = 0;
	gr.facing = 0;
	const int32_t dx = x - F.to_x[size_t(F.idx)], dy = y - F.to_y[size_t(F.idx)];
	gr.o_angle = find_angle(dx, dy);
	gr.o_dist = vector_dist(dx, dy);
	return &F;
}

}  // namespace ron::sim
