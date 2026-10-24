// The field maps between the engine's state and the original game's x86 struct images (docs/original-save.md).
//
// A `ClassMap` lists, for one PDB class of the original (offsets come from `rise.pdb` through datawalk::Description), the
// members the engine models: for each, how to read the engine's value out of the original bytes (`load`, used by
// load_original_save) and how to write it back as the original's bytes (`emit`, used by EngineProvider). One declaration
// serves both directions, so a member cannot be loaded without being emitted. Members of a walked range that no entry covers
// stay in the shadow (the decoded original bytes, OriginalShadow::tree).
//
// A `Segment` binds a range of a decoded `Rec` (one object image of the original) to an engine object: `map` says how the
// bytes are laid out, `a / b / c` say which engine object (a unit's who / o, a world cell's index, ...).
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "sim/datawalk.hpp"
#include "sim/game.hpp"

namespace ron::sim::omap {

namespace dw = datawalk;

struct Segment;

// One mapped member (or a raw range): original bytes [off, off + size) of the class image <-> the engine object.
struct Entry {
	int32_t off = 0, size = 0;
	std::string name;
	std::function<void(void* obj, const uint8_t* bytes)> load;
	std::function<void(const void* obj, uint8_t* bytes)> emit;
};

// Where an engine object lives, resolved from a segment's indices at call time (the engine reallocates its vectors).
struct Seg {
	int32_t a = 0, b = 0, c = 0;
};

// What a map says about one container of an object (the Array families and lists of the walk) or one engine-driven array of child
// objects: how many elements the engine has and how each one is bound. The identity of the container is (walk step, call offset).
struct ArraySpec {
	int32_t count = 0;                                 // elements the engine has
	const struct ClassMap* child = nullptr;            // map of each element (null: elements are not bound)
	std::function<Seg(int32_t)> child_id;              // id of element i
	std::function<int32_t(int32_t)> dtype;             // MultiPtrArray: data type of element i (null: 0)
	std::function<bool(int32_t)> present;              // PtrArray families: presence byte of element i (null: all present)
	std::function<bool(int32_t)> extra_present;        // elements the engine has beyond the recorded presence (object lists)
	// Binds element i's image (existing or created) to the engine object, for lists whose elements are of several classes.
	std::function<void(OriginalShadow&, dw::Rec*, int32_t)> bind_child;
	bool keep_size = false;                            // do not touch the container's size / increment (only the length)
	bool simple = false;                               // SimpleArray: the capacity doubles (10, 20, 40, ...) when the length outgrows it
	std::function<void(std::vector<uint8_t>&)> block;  // SimpleArray: the element block (stride * count bytes)
	bool grow_only = false;                            // the length is max(the recorded length, count): records the engine has not created stay as recorded
};

// One engine-driven Deref child: its map, its id, and when the shadow's image of that child may be reused (an order node whose
// original order type differs from the engine's type must start from zeros: its old bytes belong to another layout).
struct ChildSpec {
	const struct ClassMap* map = nullptr;
	Seg id;
	const char* reuse_local = nullptr;  // name of a Local of the parent image that must equal reuse_value for the shadow child to be used
	int64_t reuse_value = 0;
};

struct ClassMap {
	std::string name;  // report label ("Unit")
	std::string cls;   // PDB class the entry names are looked up in
	std::vector<Entry> entries;
	int32_t max_size = 0;  // the largest entry (set by finish())
	std::vector<std::string> walks;  // walk functions whose raw ranges define the "walked" members (for the report)
	// The engine object of a segment; nullptr = absent (the entries are then not applied: the shadow bytes stay).
	std::function<void*(Game&, const Seg&)> resolve;
	// The writer-side temporaries of the walk (Local steps: must_walk, type, has_block, ...) that the engine can compute for the
	// bound object. `nth` counts the calls of the same step on the same image within the current walk (loop index of the
	// walks that store one local per iteration). Returns false to keep the shadow's value.
	std::function<bool(const Game&, const Seg&, const char* name, int nth, int64_t& value)> local_fn;
	// Containers whose contents the engine owns: fills `spec` and returns true when (step, base) is one of them.
	std::function<bool(const Game&, const Seg&, int step, int64_t base, ArraySpec& spec)> array_fn;
	// Deref children (Stack<PathData> elements, queue slots, ...) that the engine owns: the map and id of child `index` of the
	// Deref `step` at call offset `base`, or false when the child is not engine-driven.
	std::function<bool(const Game&, const Seg&, int step, int64_t base, int64_t index, ChildSpec& child)> child_fn;
	// A String step of this object (UTF-16 characters of the engine's value) at call offset `base`; false: keep the shadow's.
	std::function<bool(const Game&, const Seg&, int step, int64_t base, std::u16string& out)> string_fn;
	// Whether the walk covers this entry for this object (an inactive leader walks only 8 bytes); null: always. Audit only.
	std::function<bool(const Game&, const Seg&, const Entry&)> audited;
	void finish();  // sorts the entries by offset
	// Applies every entry that overlaps [lo, hi) of the class image (offsets relative to the segment base `seg_base` of `rec`):
	// `buf` holds the bytes of [lo, hi) and is overwritten. An entry's emit() receives its bytes pre-filled with the shadow's
	// (so a member the engine only partly models can keep the rest).
	void overlay(const void* obj, const dw::Rec& rec, int64_t seg_base, int64_t lo, int64_t hi, uint8_t* buf) const;
	// Loads every entry from the bytes of a decoded image (rec bytes at base + entry offsets).
	void load_from(void* obj, const dw::Rec& rec, int64_t base) const;
};

struct Segment {
	const ClassMap* map = nullptr;
	int64_t base = 0;  // offset of the class image inside the Rec
	int64_t len = -1;  // bytes (-1: to the end)
	Seg id;
};

// ---------------------------------------------------------------------------------------------------- codecs

inline uint64_t get_le(const uint8_t* p, int n) {
	uint64_t u = 0;
	for (int i = 0; i < n; ++i) u |= uint64_t(p[i]) << (8 * i);
	return u;
}
inline void put_le(uint8_t* p, int n, uint64_t v) {
	for (int i = 0; i < n; ++i) p[i] = uint8_t(v >> (8 * i));
}

template <class T> struct is_std_array : std::false_type {};
template <class X, size_t N> struct is_std_array<std::array<X, N>> : std::true_type {};

// One scalar of `size` bytes (signed per the PDB type) <-> an engine member of any arithmetic / enum type.
template <class M> void scalar_load(M& dst, const uint8_t* p, int size, bool is_signed) {
	if constexpr (std::is_same_v<M, float>) {
		float f;
		std::memcpy(&f, p, 4);
		dst = f;
	} else if constexpr (std::is_same_v<M, double>) {
		double f;
		std::memcpy(&f, p, 8);
		dst = f;
	} else {
		uint64_t u = get_le(p, size);
		if (is_signed && size < 8 && ((u >> (8 * size - 1)) & 1)) u |= ~uint64_t(0) << (8 * size);
		dst = static_cast<M>(u);
	}
}
template <class M> void scalar_emit(const M& v, uint8_t* p, int size) {
	if constexpr (std::is_same_v<M, float>) {
		std::memcpy(p, &v, 4);
	} else if constexpr (std::is_same_v<M, double>) {
		std::memcpy(p, &v, 8);
	} else {
		put_le(p, size, uint64_t(int64_t(v)));
	}
}

// A std::array member of integers/floats <-> `esz`-byte elements.
template <class A> void arr_load(A& a, const uint8_t* p, int esz, bool is_signed = true) {
	for (size_t i = 0; i < a.size(); ++i) scalar_load<typename A::value_type>(a[i], p + i * size_t(esz), esz, is_signed);
}
template <class A> void arr_emit(const A& a, uint8_t* p, int esz) {
	for (size_t i = 0; i < a.size(); ++i) scalar_emit<typename A::value_type>(a[i], p + i * size_t(esz), esz);
}

// Builds the entries of one ClassMap against the PDB layout of `cls`. T is the engine type the map resolves to.
template <class T> class Builder {
public:
	Builder(ClassMap& m, const char* name, const char* cls) : m_(m) {
		m_.name = name;
		m_.cls = cls;
	}
	Builder& walks(std::initializer_list<const char*> fns) {
		for (const char* f : fns) m_.walks.push_back(f);
		return *this;
	}
	Builder& locals(std::function<bool(const Game&, const Seg&, const char*, int, int64_t&)> f) {
		m_.local_fn = std::move(f);
		return *this;
	}
	Builder& arrays(std::function<bool(const Game&, const Seg&, int, int64_t, ArraySpec&)> f) {
		m_.array_fn = std::move(f);
		return *this;
	}
	Builder& children(std::function<bool(const Game&, const Seg&, int, int64_t, int64_t, ChildSpec&)> f) {
		m_.child_fn = std::move(f);
		return *this;
	}
	Builder& strings(std::function<bool(const Game&, const Seg&, int, int64_t, std::u16string&)> f) {
		m_.string_fn = std::move(f);
		return *this;
	}
	Builder& resolve(std::function<T*(Game&, const Seg&)> r) {
		m_.resolve = [r](Game& g, const Seg& s) -> void* { return r(g, s); };
		return *this;
	}

	// An engine member (scalar or std::array of scalars) at the PDB member `name`.
	template <class B, class M> Builder& f(const char* name, M B::*mp) {
		const Info in = info(name);
		Entry e;
		e.off = in.off;
		e.size = in.size;
		e.name = name;
		if constexpr (is_std_array<M>::value) {
			using X = typename M::value_type;
			const int esz = in.elem_size > 0 ? in.elem_size : in.size;
			const int count = in.size / esz;
			if (size_t(count) != std::tuple_size<M>::value)
				bad(name, "array length " + std::to_string(count) + " vs engine " + std::to_string(std::tuple_size<M>::value));
			e.load = [=](void* o, const uint8_t* p) {
				M& a = static_cast<T*>(o)->*mp;
				for (int i = 0; i < count; ++i) scalar_load<X>(a[size_t(i)], p + i * esz, esz, in.is_signed);
			};
			e.emit = [=](const void* o, uint8_t* p) {
				const M& a = static_cast<const T*>(o)->*mp;
				for (int i = 0; i < count; ++i) scalar_emit<X>(a[size_t(i)], p + i * esz, esz);
			};
		} else {
			const int sz = in.size;
			e.load = [=](void* o, const uint8_t* p) { scalar_load<M>(static_cast<T*>(o)->*mp, p, sz, in.is_signed); };
			e.emit = [=](const void* o, uint8_t* p) { scalar_emit<M>(static_cast<const T*>(o)->*mp, p, sz); };
		}
		m_.entries.push_back(std::move(e));
		return *this;
	}
	// The same, loading the member as unsigned whatever the PDB type says (an engine uint32 for an original int16 flag word).
	template <class B, class M> Builder& fu(const char* name, M B::*mp) {
		f(name, mp);
		Entry& e = m_.entries.back();
		const Info in = info(name);
		const int sz = in.size;
		e.load = [=](void* o, const uint8_t* p) { scalar_load<M>(static_cast<T*>(o)->*mp, p, sz, false); };
		return *this;
	}
	// The same with a XOR key (the original keeps some members obfuscated in RAM; images hold the XORed value).
	template <class B, class M> Builder& fx(const char* name, M B::*mp, uint32_t key) {
		const Info in = info(name);
		Entry e;
		e.off = in.off;
		e.size = in.size;
		e.name = name;
		const int sz = in.size;
		e.load = [=](void* o, const uint8_t* p) {
			M& v = static_cast<T*>(o)->*mp;
			scalar_load<M>(v, p, sz, in.is_signed);
			v = static_cast<M>(static_cast<int64_t>(v) ^ int64_t(key));
		};
		e.emit = [=](const void* o, uint8_t* p) {
			const M v = static_cast<M>(static_cast<int64_t>(static_cast<const T*>(o)->*mp) ^ int64_t(key));
			scalar_emit<M>(v, p, sz);
		};
		m_.entries.push_back(std::move(e));
		return *this;
	}
	// Anything else: the PDB member `name` (its offset and size) with explicit codecs.
	Builder& c(const char* name, std::function<void(T&, const uint8_t*)> ld, std::function<void(const T&, uint8_t*)> em) {
		const Info in = info(name);
		return raw(in.off, in.size, name, std::move(ld), std::move(em));
	}
	// A raw range by offset.
	Builder& raw(int32_t off, int32_t size, const char* name, std::function<void(T&, const uint8_t*)> ld,
	             std::function<void(const T&, uint8_t*)> em) {
		Entry e;
		e.off = off;
		e.size = size;
		e.name = name;
		e.load = [ld](void* o, const uint8_t* p) { ld(*static_cast<T*>(o), p); };
		e.emit = [em](const void* o, uint8_t* p) { em(*static_cast<const T*>(o), p); };
		m_.entries.push_back(std::move(e));
		return *this;
	}
	// Members named in the PDB but intentionally not modelled are simply left out; `skip` documents a deliberate one.
	Builder& skip(const char*) { return *this; }

private:
	struct Info {
		int32_t off, size, elem_size;
		bool is_signed;
	};
	[[noreturn]] void bad(const char* name, const std::string& why) const {
		throw std::logic_error("original_map: " + m_.cls + "." + name + ": " + why);
	}
	Info info(const char* name) const {
		const dw::FieldInfo* f = dw::Description::get().find_field(m_.cls.c_str(), name);
		if (!f) bad(name, "no such member in the PDB class");
		Info in;
		in.off = f->offset;
		in.size = f->size;
		in.elem_size = f->elem_size;
		in.is_signed = f->type == dw::FieldType::I8 || f->type == dw::FieldType::I16 || f->type == dw::FieldType::I32;
		return in;
	}
	ClassMap& m_;
};

// ---------------------------------------------------------------------------------------------------- the maps

// Each returns a process-wide map built on first use. See the original_map_*.cpp files.
const ClassMap& map_game();            // Game: frame / tick / market / counters (the 0x194 block) and the statics of the Game global
const ClassMap& map_game_info();       // GameInfo (at Game+0xc)
const ClassMap& map_world();           // World scalars (+0x0 xs, ys; +0x8..0x80)
const ClassMap& map_wdata();           // WData cell (0x15 bytes)
const ClassMap& map_wdata_block();     // the part 9 image of a cell: has_block
const ClassMap& map_collblock();       // CollBlock (BitMask<768>)
const ClassMap& map_objects_header();  // Objects marks
const ClassMap& map_unit();            // Object + Unit (0x158)
const ClassMap& map_animal();          // AnimalData extension (+0x150)
const ClassMap& map_build();           // Object + Wall + Build (0xdc)
const ClassMap& map_guy();             // Guy (0xbc)
const ClassMap& map_pathdata();        // PathData (Unit.path.list[i])
const ClassMap& map_queue_item();      // BuildQueue slot (QueueItem)
const ClassMap& map_tcoord();          // MiningList element (TCoordData)
const ClassMap& map_gather_point();    // GatherPointList node
const ClassMap& map_order_node();      // an OrderList node (order type local)
const ClassMap& map_order(int order_type);  // the walked fields of one order class (null for an unknown type)
const ClassMap& map_leader();          // LeaderData (0x6eec)
const ClassMap& map_site();            // Leader.sites[i]
const ClassMap& map_make_object();     // Leader.make_list[i]
const ClassMap& map_ammo();            // AmmoData
const ClassMap& map_death();           // DeathObjData
const ClassMap& map_leader_encrypt();  // LeaderDataEncrypt (the 62 plain u32 locals of an active leader)
const ClassMap& map_good();            // Good
const ClassMap& map_item();            // Item
const ClassMap& map_goods_array();     // the global PtrArray<Good>
const ClassMap& map_items_array();
const ClassMap& map_herd();           // Herd (0x24: cx, cy, wx, wy, t, good_o, herd, herd_flags)
const ClassMap& map_herds_array();    // the global PtrArray<Herd>
const ClassMap& map_city();
const ClassMap& map_caravan_link();
const ClassMap& map_cities_lists();    // Cities: 8 PtrArray<City>
const ClassMap& map_fort();
const ClassMap& map_forts_lists();
const ClassMap& map_wonder();
const ClassMap& map_wonders_lists();
const ClassMap& map_supply();
const ClassMap& map_supplies_lists();
const ClassMap& map_caravan();
const ClassMap& map_caravans_lists();
const ClassMap& map_army();
const ClassMap& map_armies_lists();
const ClassMap& map_group();
const ClassMap& map_groups_global();
const ClassMap& map_wcoord();          // Region.coords[i]
const ClassMap& map_region();
const ClassMap& map_regions_global();
const ClassMap& map_game_random();
const ClassMap& map_ai_speed();
const ClassMap& map_ai_off();
const ClassMap& map_armies_find_dist();
const ClassMap& map_queue_fail();
const ClassMap& map_game_daemon();
const ClassMap& map_constants();
const ClassMap& map_leader_options();

// Walked members of the maps' classes (for docs/original-save.md): raw ranges of the walk functions -> PDB members.
struct FieldCount {
	int walked = 0;  // PDB members of the class that overlap a walked raw range
	int mapped = 0;  // of which an entry covers
	std::vector<std::string> unmapped;
};
FieldCount count_fields(const ClassMap& m);
// Every map, for the report (ron-sav --field-report).
std::vector<const ClassMap*> all_maps();

}  // namespace ron::sim::omap
