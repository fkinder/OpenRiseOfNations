// The save archive (docs/save-format.md): one walk function per state struct, driven by a Writer or a Reader so a field
// can never be saved without being loaded (the original does the same with its single `walk` serializer,
// docs/state-spec.md section 1). Internal to sim/save.cpp; nothing else should include this header.
//
// Encoding: little-endian (the host must be little-endian), fixed-width scalars as their bytes (floats bit for bit),
// strings and containers as a u32 count plus the elements. A file is "RONS", a u32 version, then sections:
//   tag (4 ASCII bytes) | u64 payload length | u32 CRC-32 of the payload | payload
// A reader opens a section by tag, checks the CRC, walks it with a bounded Reader and then requires that every byte of
// the payload was consumed, so a corrupt or mismatched section is reported by its tag.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <zlib.h>

namespace ron::sim::save {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the save format is little-endian and written by memcpy");

[[noreturn]] inline void fail(const std::string& where, const std::string& what) {
	throw std::runtime_error("save: " + (where.empty() ? std::string() : "section '" + where + "': ") + what);
}

// Where in a section's payload a field starts: only recorded when a Writer has a trace (describe_game_difference).
struct TraceEntry {
	std::string section;
	size_t off = 0;
	std::string name;
};

class Writer {
public:
	static constexpr bool kReading = false;
	std::vector<uint8_t> buf;
	std::vector<TraceEntry>* trace = nullptr;
	void mark(const std::string& name) {
		if (trace) trace->push_back({sect_, buf.size(), name});
	}
	// The next field name of a "v.a, v.b, ..." list (FIELDS), for the trace.
	void mark_next(const char*& names) {
		if (!trace) return;
		while (*names == ' ' || *names == ',') names++;
		const char* e = names;
		while (*e && *e != ',') e++;
		mark(std::string(names, e));
		names = e;
	}

	void raw(const void* p, size_t n) {
		const uint8_t* b = static_cast<const uint8_t*>(p);
		buf.insert(buf.end(), b, b + n);
	}
	void count(size_t n) {
		if (n > 0xffffffffu) fail(sect_, "container too large");
		const uint32_t c = uint32_t(n);
		raw(&c, 4);
	}
	// Appends a section: the body walks a fresh Writer, then its bytes are framed.
	template <class F>
	void section(const char* tag, F&& body) {
		Writer sub;
		sub.sect_ = tag;
		sub.trace = trace;
		body(sub);
		raw(tag, 4);
		const uint64_t len = sub.buf.size();
		raw(&len, 8);
		const uint32_t crc = uint32_t(::crc32(0, sub.buf.data(), uInt(sub.buf.size())));
		raw(&crc, 4);
		raw(sub.buf.data(), sub.buf.size());
	}
	const std::string& name() const { return sect_; }
	[[noreturn]] void error(const std::string& what) const { fail(sect_, what); }

private:
	std::string sect_;
};

class Reader {
public:
	static constexpr bool kReading = true;
	void mark(const std::string&) {}

	Reader(const uint8_t* begin, size_t n, std::string name) : p_(begin), end_(begin + n), sect_(std::move(name)) {}

	void raw(void* dst, size_t n) {
		need(n);
		std::memcpy(dst, p_, n);
		p_ += n;
	}
	// A container count, validated against the bytes left (every element takes at least min_bytes).
	size_t count(size_t min_bytes = 1) {
		uint32_t c = 0;
		raw(&c, 4);
		if (min_bytes && size_t(c) > remaining() / min_bytes) error("container count " + std::to_string(c) + " exceeds the section");
		return c;
	}
	size_t remaining() const { return size_t(end_ - p_); }
	const uint8_t* pos() const { return p_; }
	void skip(size_t n) {
		need(n);
		p_ += n;
	}

	// Opens the next section, which must have this tag.
	template <class F>
	void section(const char* tag, F&& body) {
		char t[4];
		if (remaining() < 16) error(std::string("missing section '") + tag + "' (file truncated)");
		raw(t, 4);
		if (std::memcmp(t, tag, 4) != 0)
			error(std::string("expected section '") + tag + "', found '" + std::string(t, 4) + "'");
		uint64_t len = 0;
		uint32_t crc = 0;
		raw(&len, 8);
		raw(&crc, 4);
		if (len > remaining()) fail(tag, "payload of " + std::to_string(len) + " bytes runs past the end of the file");
		if (uint32_t(::crc32(0, p_, uInt(len))) != crc) fail(tag, "checksum mismatch (the file is corrupt)");
		Reader sub(p_, size_t(len), tag);
		p_ += len;
		try {
			body(sub);
		} catch (const std::runtime_error& e) {
			if (std::strncmp(e.what(), "save: ", 6) == 0) throw;
			fail(tag, e.what());
		} catch (const std::exception& e) {
			fail(tag, e.what());
		}
		if (sub.remaining() != 0)
			fail(tag, std::to_string(sub.remaining()) + " bytes were not read (the file's layout differs from this build's)");
	}
	// The next section's tag without consuming it ("" at the end).
	std::string peek_tag() const { return remaining() >= 4 ? std::string(reinterpret_cast<const char*>(p_), 4) : std::string(); }
	const std::string& name() const { return sect_; }
	[[noreturn]] void error(const std::string& what) const { fail(sect_, what); }

private:
	void need(size_t n) const {
		if (n > remaining()) fail(sect_, "unexpected end of data (the file is truncated or its layout differs)");
	}
	const uint8_t* p_;
	const uint8_t* end_;
	std::string sect_;
};

// ---------------------------------------------------------------- generic element walkers

template <class T> struct is_std_array : std::false_type {};
template <class T, size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
template <class T> struct is_std_vector : std::false_type {};
template <class T, class A> struct is_std_vector<std::vector<T, A>> : std::true_type {};
template <class T> struct is_std_deque : std::false_type {};
template <class T, class A> struct is_std_deque<std::deque<T, A>> : std::true_type {};
template <class T> struct is_std_map : std::false_type {};
template <class K, class V, class C, class A> struct is_std_map<std::map<K, V, C, A>> : std::true_type {};
template <class T> struct is_std_pair : std::false_type {};
template <class A, class B> struct is_std_pair<std::pair<A, B>> : std::true_type {};
template <class T> struct is_std_string : std::false_type {};
template <class C, class Tr, class A> struct is_std_string<std::basic_string<C, Tr, A>> : std::true_type {};

template <class T> constexpr bool is_plain_v = std::is_arithmetic_v<T> || std::is_enum_v<T>;

template <class Ar, class T> void io(Ar& ar, T& v);
template <class Ar, class... Ts> void fields(Ar& ar, Ts&... vs) {
	(io(ar, vs), ...);
}
// fields() that records each member's name in the Writer's trace. The walks in save_walk.inc use it through FIELDS.
template <class Ar, class... Ts> void fields_named(Ar& ar, const char* names, Ts&... vs) {
	if constexpr (Ar::kReading) {
		(io(ar, vs), ...);
	} else {
		if (!ar.trace) {
			(io(ar, vs), ...);
		} else {
			((ar.mark_next(names), io(ar, vs)), ...);
		}
	}
}
#define FIELDS(ar, ...) ::ron::sim::save::fields_named(ar, #__VA_ARGS__, __VA_ARGS__)

// Writes or reads one value. Structs go through their `walk(ar, v)` overload (save_walk.inc, save.cpp).
template <class Ar, class T>
void io(Ar& ar, T& v) {
	if constexpr (std::is_same_v<T, bool>) {
		if constexpr (Ar::kReading) {
			uint8_t b = 0;
			ar.raw(&b, 1);
			if (b > 1) ar.error("invalid boolean byte " + std::to_string(b));
			v = b != 0;
		} else {
			const uint8_t b = v ? 1 : 0;
			ar.raw(&b, 1);
		}
	} else if constexpr (is_plain_v<T>) {
		ar.raw(&v, sizeof(T));
	} else if constexpr (is_std_string<T>::value) {
		using C = typename T::value_type;
		if constexpr (Ar::kReading) {
			const size_t n = ar.count(sizeof(C));
			v.resize(n);
			if (n) ar.raw(v.data(), n * sizeof(C));
		} else {
			ar.count(v.size());
			if (!v.empty()) ar.raw(v.data(), v.size() * sizeof(C));
		}
	} else if constexpr (is_std_array<T>::value) {
		using E = typename T::value_type;
		if constexpr (is_plain_v<E> && !std::is_same_v<E, bool>) {
			if (!v.empty()) ar.raw(v.data(), v.size() * sizeof(E));
		} else {
			for (auto& e : v) io(ar, e);
		}
	} else if constexpr (is_std_vector<T>::value) {
		using E = typename T::value_type;
		if constexpr (Ar::kReading) {
			const size_t n = ar.count(is_plain_v<E> ? sizeof(E) : 1);
			v.resize(n);
		} else {
			ar.count(v.size());
		}
		if constexpr (is_plain_v<E> && !std::is_same_v<E, bool>) {
			if (!v.empty()) ar.raw(v.data(), v.size() * sizeof(E));
		} else {
			for (auto& e : v) io(ar, e);
		}
	} else if constexpr (is_std_deque<T>::value) {
		if constexpr (Ar::kReading) v.resize(ar.count(1)); else ar.count(v.size());
		for (auto& e : v) io(ar, e);
	} else if constexpr (is_std_pair<T>::value) {
		io(ar, v.first);
		io(ar, v.second);
	} else if constexpr (is_std_map<T>::value) {
		using K = typename T::key_type;
		using V = typename T::mapped_type;
		if constexpr (Ar::kReading) {
			const size_t n = ar.count(1);
			v.clear();
			for (size_t i = 0; i < n; i++) {
				K k{};
				V val{};
				io(ar, k);
				io(ar, val);
				v.emplace(std::move(k), std::move(val));
			}
		} else {
			ar.count(v.size());
			for (auto& kv : v) {
				K k = kv.first;
				io(ar, k);
				io(ar, kv.second);
			}
		}
	} else {
		walk(ar, v);
	}
}

}  // namespace ron::sim::save
