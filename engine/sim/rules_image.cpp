// The rules image: Game::walk_rules_data @00589550 (docs/rules-spec.md section 3; port of re/tools/rulesimage.mjs).
// One walk function drives both directions, like the original's DataWalk: a Writer serialises the tables, a Reader
// loads a replay's image over them.
//
//   u8 0x92                              walk_test("Game Rules")
//   806 x type record                    by TypeIndex (Types::walk_rules_data -> vtable slot 0xc4)
//   int32[848]                           Constants [0x000, 0xd40)
//   int32                                Constants [0x804, 0x808) again
//   int16[493][493]                      Balance final_balance_table
//   24 x { u8 0x8f; int32[6] Tribe [0x54, 0x6c); int32[352] graft }
// A String is u32 length + UTF-16LE units; a SimpleArray<u16> is i32 count and, when non-zero, i32 capacity,
// u16 block, u8 flags, count x u16.
#include <cstring>
#include <string>
#include <type_traits>

#include "assets/scenario.hpp"
#include "sim/rules.hpp"

namespace ron::sim {

namespace {

std::string type_where(int t, const Type& o) { return "type " + std::to_string(t) + " (" + to_utf8(o.name) + ")"; }

template <typename W, typename A>
void walk_array(W& w, const char* name, A& a) {
	for (size_t i = 0; i < a.size(); ++i) w.f(name, a[i], int(i));
}

template <typename W, typename R>
void walk_rules(W& w, R& rules) {
	w.marker("marker", kMarkerGameRules);
	for (int t = 0; t < kNumTypes; ++t) {
		auto& o = rules.types[size_t(t)];
		const Kind kind = kind_of(t);
		w.where([&] { return type_where(t, o); });
		// TypeData [0x04, 0x5e)
		w.f("type", o.type);
		w.check_type(t, o.type);
		w.f("job_time", o.job_time);
		w.f("res_time", o.res_time);
		w.f("tribe_mask", o.tribe_mask);
		w.f("cat", o.cat);
		walk_array(w, "costs", o.costs);
		walk_array(w, "preq", o.preq);
		w.f("from", o.from);
		w.f("where", o.where);
		w.f("upgrade", o.upgrade);
		w.f("jump", o.jump);
		w.f("obs", o.obs);
		walk_array(w, "show", o.show);
		w.f("modified", o.modified);
		w.f("grid_x", o.grid_x);
		w.f("grid_y", o.grid_y);
		w.str("display_name", o.display_name);
		if (is_object_kind(kind)) {
			// ObjectTypeData [0x1e4, 0x27c)
			w.f("obj_masks", o.obj_masks);
			w.f("attack", o.attack);
			w.f("to_hit", o.to_hit);
			w.f("attenuate", o.attenuate);
			w.f("recharge", o.recharge);
			w.f("min_range", o.min_range);
			w.f("max_range", o.max_range);
			w.f("splash_area", o.splash_area);
			w.f("splash_percent", o.splash_percent);
			w.f("ammo_per_att", o.ammo_per_att);
			w.f("proj_speed", o.proj_speed);
			w.f("hits", o.hits);
			w.f("armor", o.armor);
			w.f("domain", o.domain);
			w.f("los", o.los);
			w.f("science_los", o.science_los);
			w.f("guy_spacing", o.guy_spacing);
			w.f("x_spacing", o.x_spacing);
			w.f("y_spacing", o.y_spacing);
			w.f("abil", o.abil);
			w.f("x_size", o.x_size);
			w.f("y_size", o.y_size);
			w.f("guy_radius", o.guy_radius);
			w.f("block_radius", o.block_radius);
			w.f("big_radius", o.big_radius);
			w.f("new_block_radius", o.new_block_radius);
			w.f("new_big_radius", o.new_big_radius);
			w.f("fly_high", o.fly_high);
			w.f("fly_low", o.fly_low);
			w.f("block_points", o.block_points);
			w.f("graft", o.graft);
			w.f("special_upgrade", o.special_upgrade);
			w.f("special_upgrade_cost", o.special_upgrade_cost);
			walk_array(w, "support", o.support);
			walk_array(w, "support_cost", o.support_cost);
			w.f("age", o.age);
			w.sa("is_list", o.is_list);
			w.sa("is_strict_list", o.is_strict_list);
			if (kind == Kind::Unit) {
				// UnitTypeData [0x2b4,0x2cc) [0x2d4,0x2e0) [0x2e0,0x5d4): fire_proj and second_max_range skipped
				w.f("unit_flags", o.unit_flags);
				w.f("unit_flags2", o.unit_flags2);
				w.f("mode", o.mode);
				w.f("moves", o.moves);
				w.f("turn_speed", o.turn_speed);
				w.f("role", o.role);
				w.f("carry", o.carry);
				w.f("carry_size", o.carry_size);
				w.f("military_level", o.military_level);
				w.f("research_premium_cost", o.research_premium_cost);
				w.f("research_premium_time", o.research_premium_time);
				w.f("job_extra_time", o.job_extra_time);
				w.f("mana", o.mana);
				w.f("control_cost", o.control_cost);
				w.f("progression", o.progression);
				w.f("push_size", o.push_size);
				w.f("push_circles", o.push_circles);
				w.f("target_size", o.target_size);
				w.f("squad_size", o.squad_size);
				w.f("uber_size", o.uber_size);
				w.f("crew_size", o.crew_size);
				w.f("base_form", o.base_form);
				w.check_size(o.relative_value, kNumGraft);
				walk_array(w, "relative_value", o.relative_value);
			} else if (kind == Kind::Build) {
				// BuildTypeData [0x2b4, 0x2e5)
				w.f("town_hits", o.town_hits);
				w.f("min_city_size", o.min_city_size);
				w.f("misery_rate", o.misery_rate);
				w.f("build_flags", o.build_flags);
				w.f("most_shots", o.most_shots);
				w.f("garrison_max", o.garrison_max);
				w.f("base_arrows", o.base_arrows);
				w.f("wonder_val", o.wonder_val);
				w.f("plunder_value", o.plunder_value);
				w.f("plunder_good", o.plunder_good);
				w.f("behind_height", o.behind_height);
				w.f("to", o.to);
				w.f("civ_graph_mask", o.civ_graph_mask);
			} else if (kind == Kind::Good) {
				// GoodTypeData [0x2b4, 0x2f8)
				w.f("undiscovered_cost_good", o.undiscovered_cost_good);
				w.f("obsolete_cost_good", o.obsolete_cost_good);
				w.f("undiscovered_support_good", o.undiscovered_support_good);
				w.f("obsolete_support_good", o.obsolete_support_good);
				w.f("obsolete_production_good", o.obsolete_production_good);
				w.f("undiscovered_cost_rate", o.undiscovered_cost_rate);
				w.f("obsolete_cost_rate", o.obsolete_cost_rate);
				w.f("undiscovered_support_rate", o.undiscovered_support_rate);
				w.f("obsolete_support_rate", o.obsolete_support_rate);
				w.f("obsolete_production_rate", o.obsolete_production_rate);
				walk_array(w, "bonus_type", o.bonus_type);
				walk_array(w, "bonus_num", o.bonus_num);
				w.f("largest_gather", o.largest_gather);
				w.f("block", o.block);
				w.f("exclude_ctw", o.exclude_ctw);
			}
		} else if (kind == Kind::Tech) {
			// TechTypeData [0x1c8, 0x1e3) + alternate names
			w.f("age", o.age);
			walk_array(w, "ai", o.ai);
			w.f("leader_off", o.leader_off);
			for (size_t i = 0; i < o.alternate_name.size(); ++i) w.str("alternate_name", o.alternate_name[i]);
		} else if (kind == Kind::Spell) {
			// SpellTypeData [0x1c8, 0x1f8)
			w.f("spell_flags", o.spell_flags);
			w.f("spell_range", o.spell_range);
			w.f("mana", o.mana);
			w.f("duration", o.duration);
			w.f("duration_upgrade", o.duration_upgrade);
			w.f("from2", o.from2);
			walk_array(w, "use", o.use);
		}
	}
	w.where([] { return std::string("constants"); });
	walk_array(w, "int", rules.constants.v);
	// mongol_three_mil_cavalry is walked a second time.
	w.dup_constant(rules.constants.v[Constants::kMongolThreeMilCavalry / 4]);
	w.where([] { return std::string("balance"); });
	w.check_size(rules.balance, size_t(kBalanceN) * kBalanceN);
	walk_array(w, "cell", rules.balance);
	for (int i = 0; i < kNumTribes; ++i) {
		auto& tr = rules.tribes[size_t(i)];
		w.where([&] { return "tribe " + std::to_string(i); });
		w.marker("marker", kMarkerTribeRules);
		w.f("tribe", tr.tribe);
		w.f("barbarian", tr.barbarian);
		w.f("people", tr.people);
		w.f("backup_build_continent", tr.backup_build_continent);
		w.f("build_continent", tr.build_continent);
		w.f("unit_continent", tr.unit_continent);
		walk_array(w, "graft", tr.graft);
	}
}

class Writer {
public:
	explicit Writer(std::vector<ImageField>* layout) : layout_(layout) {}
	Bytes out;

	template <typename F>
	void where(F&& f) {
		if (layout_) where_ = f();
	}
	template <typename T>
	void f(const char* name, const T& v, int index = -1) {
		static_assert(std::is_integral<T>::value, "integral fields only");
		record(name, sizeof(T), index);
		using U = typename std::make_unsigned<T>::type;
		const U u = static_cast<U>(v);
		for (size_t i = 0; i < sizeof(T); ++i) out.push_back(uint8_t(u >> (8 * i)));
	}
	void marker(const char* name, uint8_t m) { f(name, m); }
	void check_type(int, int32_t) {}
	template <typename V>
	void check_size(const V& v, size_t n) {
		if (v.size() != n) throw std::logic_error("rules image: table has the wrong size");
	}
	void dup_constant(const int32_t& v) { f("mongol_three_mil_cavalry (again)", v); }
	void str(const char* name, const ustr& s) {
		f(name, uint32_t(s.size()));
		record(name, s.size() * 2, -1);
		for (char16_t c : s) {
			out.push_back(uint8_t(c));
			out.push_back(uint8_t(c >> 8));
		}
	}
	void sa(const char* name, const U16Array& a) {
		if (!a.count) {
			f(name, int32_t(0));
			return;
		}
		f(name, a.count);
		f(name, a.capacity);
		f(name, a.block);
		f(name, a.flags);
		for (size_t i = 0; i < a.v.size(); ++i) f(name, a.v[i], int(i));
	}

private:
	void record(const char* name, size_t size, int index) {
		if (layout_ && size) layout_->push_back({out.size(), size, where_, name, index});
	}
	std::vector<ImageField>* layout_;
	std::string where_;
};

class Reader {
public:
	Reader(const Bytes& b, size_t p) : b_(b), p_(p) {}
	size_t pos() const { return p_; }

	template <typename F>
	void where(F&&) {}
	template <typename T>
	void f(const char*, T& v, int = -1) {
		static_assert(std::is_integral<T>::value, "integral fields only");
		if (p_ + sizeof(T) > b_.size()) throw FormatError("rules image: read past end");
		using U = typename std::make_unsigned<T>::type;
		U u = 0;
		for (size_t i = 0; i < sizeof(T); ++i) u = U(u | (U(b_[p_ + i]) << (8 * i)));
		p_ += sizeof(T);
		v = static_cast<T>(u);
	}
	void marker(const char* name, uint8_t m) {
		uint8_t got = 0;
		f(name, got);
		if (got != m) throw FormatError("rules image: bad marker");
	}
	void check_type(int t, int32_t got) {
		if (got != t) throw FormatError("rules image: type record " + std::to_string(t) + " has type " + std::to_string(got));
	}
	template <typename V>
	void check_size(const V& v, size_t n) {
		if (v.size() != n) throw std::logic_error("rules image: table has the wrong size");
	}
	void dup_constant(const int32_t&) {
		int32_t dup = 0;
		f("dup", dup);
	}
	void str(const char* name, ustr& s) {
		uint32_t n = 0;
		f(name, n);
		if (n > 4096) throw FormatError("rules image: bad string length");
		if (p_ + size_t(n) * 2 > b_.size()) throw FormatError("rules image: read past end");
		s.resize(n);
		for (uint32_t i = 0; i < n; ++i) s[i] = char16_t(b_[p_ + 2 * i] | (b_[p_ + 2 * i + 1] << 8));
		p_ += size_t(n) * 2;
	}
	void sa(const char* name, U16Array& a) {
		a = U16Array();
		f(name, a.count);
		if (a.count < 0 || a.count > 4096) throw FormatError("rules image: bad SimpleArray count");
		if (!a.count) return;
		f(name, a.capacity);
		f(name, a.block);
		f(name, a.flags);
		a.v.resize(size_t(a.count));
		for (auto& x : a.v) f(name, x);
	}

private:
	const Bytes& b_;
	size_t p_;
};

int32_t read_i32(const Bytes& b, size_t p) { return static_cast<int32_t>(read_u32(b, p)); }

}  // namespace

Bytes write_image(const Rules& rules, std::vector<ImageField>* layout) {
	if (rules.types.size() != size_t(kNumTypes) || rules.tribes.size() != size_t(kNumTribes)) {
		throw std::logic_error("write_image: rules are not loaded");
	}
	Writer w(layout);
	w.out.reserve(1100000);
	walk_rules(w, rules);
	return std::move(w.out);
}

size_t read_image(const Bytes& b, size_t offset, Rules& out) {
	out = Rules();
	out.types.reserve(kNumTypes);
	for (int t = 0; t < kNumTypes; ++t) {
		out.types.push_back(make_type(kind_of(t)));
		out.types.back().loaded = true;
	}
	out.balance.assign(size_t(kBalanceN) * kBalanceN, 0);
	out.tribes.assign(kNumTribes, Tribe());
	Reader r(b, offset);
	walk_rules(r, out);
	return r.pos();
}

size_t read_image_into(const Bytes& b, size_t offset, Rules& rules) {
	Reader r(b, offset);
	walk_rules(r, rules);
	return r.pos();
}

bool find_image(const Bytes& b, ReplayRules& out) {
	for (size_t p = 0; p + 200 < b.size(); ++p) {
		// Marker, type 0, and a non-zero tribe_mask in the first record.
		if (b[p] != kMarkerGameRules || read_i32(b, p + 1) != 0 || read_i32(b, p + 1 + 0x0c) == 0) continue;
		try {
			out.end = read_image(b, p, out.rules);
			out.offset = p;
			return true;
		} catch (const std::exception&) {
			// keep scanning
		}
	}
	return false;
}

Bytes read_replay(const std::string& path) {
	Bytes b = read_file(path);
	if (b.size() > 2 && b[0] == 0x1f && b[1] == 0x8b) b = gunzip(b);
	return b;
}

}  // namespace ron::sim
