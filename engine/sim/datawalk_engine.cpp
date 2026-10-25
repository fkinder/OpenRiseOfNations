#include "sim/datawalk_engine.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ron::sim {

namespace dw = datawalk;

EngineProvider::EngineProvider(Game& g) : g_(g), sh_(*g.original), base_(g.original->tree_provider()) {
	if (!g.original) throw std::logic_error("EngineProvider: the game was not loaded from an original save");
}

void EngineProvider::begin_walk() {
	base_.begin_walk();
	nth_.clear();
	if (!hold_) active_.clear();
}

int EngineProvider::next_nth(const void* rec, int step, int64_t base) {
	uint64_t k = uint64_t(reinterpret_cast<uintptr_t>(rec)) * 1000003ull ^ (uint64_t(uint32_t(step)) << 40) ^ uint64_t(base);
	return nth_[k]++;
}

const uint8_t* EngineProvider::image(dw::Obj o, int64_t off, uint32_t n) {
	const uint8_t* b = base_.image(o, off, n);
	auto it = sh_.segs.find(static_cast<const dw::Rec*>(o));
	if (it == sh_.segs.end()) return b;
	scratch_.assign(b, b + std::max<uint32_t>(n, 8));
	for (const omap::Segment& s : it->second) {
		const int64_t seg_hi = s.len < 0 ? INT64_MAX : s.base + s.len;
		const int64_t lo = std::max<int64_t>(off, s.base), hi = std::min<int64_t>(off + n, seg_hi);
		if (hi <= lo) continue;
		void* obj = s.map->resolve ? s.map->resolve(g_, s.id) : nullptr;
		if (!obj) continue;
		s.map->overlay(obj, *static_cast<const dw::Rec*>(o), s.base, lo - s.base, hi - s.base, scratch_.data() + (lo - off));
	}
	return scratch_.data();
}

bool EngineProvider::local(dw::Obj o, const dw::Site& site, int64_t& value) {
	const dw::Description& d = dw::Description::get();
	const char* name = d.local_name(site.step);
	if (std::strcmp(name, "flags_masked") == 0) {
		const uint8_t* p = image(o, site.base + 0x14, 4);
		value = int64_t(uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24) & 0xfffffffeu);
		return true;
	}
	const bool ok = base_.local(o, site, value);  // advances the replay cursor; the shadow's value unless the engine knows better
	auto it = sh_.segs.find(static_cast<const dw::Rec*>(o));
	if (it == sh_.segs.end()) return ok;
	const int nth = next_nth(o, site.step, site.base);
	for (const omap::Segment& s : it->second) {
		if (!s.map->local_fn) continue;
		if (s.len >= 0 && (site.base < s.base || site.base >= s.base + s.len)) continue;
		int64_t v = 0;
		if (s.map->local_fn(g_, s.id, name, nth, v)) {
			value = v;
			return true;
		}
	}
	// An image the engine created has no recorded locals: writer-side temporaries default to 0 (a freshly allocated, zeroed object).
	if (!ok) {
		value = 0;
		return true;
	}
	return ok;
}

dw::Obj EngineProvider::child(dw::Obj o, const dw::Site& site, int64_t index) {
	dw::Obj c = base_.child(o, site, index);
	auto it = sh_.segs.find(static_cast<const dw::Rec*>(o));
	if (it == sh_.segs.end()) return c;
	for (const omap::Segment& s : it->second) {
		if (!s.map->child_fn) continue;
		if (s.len >= 0 && (site.base < s.base || site.base >= s.base + s.len)) continue;
		omap::ChildSpec cs;
		if (!s.map->child_fn(g_, s.id, site.step, site.base, index, cs)) continue;
		const dw::Description& d = dw::Description::get();
		dw::Rec* r = static_cast<dw::Rec*>(c);
		bool reuse = r != nullptr;
		if (reuse && cs.reuse_local) {
			// the shadow image belongs to another layout unless the parent's recorded Local still says it is this kind
			reuse = false;
			for (const dw::Event& e : static_cast<const dw::Rec*>(o)->ev)
				if (e.type == 1 && std::strcmp(d.local_name(e.step), cs.reuse_local) == 0) { reuse = e.value == cs.reuse_value; break; }
		}
		if (!reuse) r = sh_.fresh_rec(static_cast<const dw::Rec*>(o), site.step, site.base, index, d.step_class(site.step));
		if (cs.map) {
			// A body image has one layout: a cached image reused for another order class must not keep the old class's segment
			// (it would overlay the old members' defaults on the new order's bytes).
			if (cs.reuse_local) {
				std::vector<omap::Segment>& sv = sh_.segs[r];
				sv.erase(std::remove_if(sv.begin(), sv.end(), [&](const omap::Segment& x) { return x.map != cs.map; }), sv.end());
			}
			sh_.rebind(r, *cs.map, cs.id);
		}
		return r;
	}
	return c;
}

bool EngineProvider::container(dw::Obj o, const dw::Site& site, dw::ContainerHeader& hdr) {
	const bool have = base_.container(o, site, hdr);
	if (!have) hdr = dw::ContainerHeader();  // an image the engine created: no recorded container
	auto it = sh_.segs.find(static_cast<const dw::Rec*>(o));
	if (it == sh_.segs.end()) return have;
	for (const omap::Segment& s : it->second) {
		if (!s.map->array_fn) continue;
		if (s.len >= 0 && (site.base < s.base || site.base >= s.base + s.len)) continue;
		Active a;
		if (!s.map->array_fn(g_, s.id, site.step, site.base, a.spec)) continue;
		a.shadow_length = hdr.length;
		const int32_t old_len = hdr.length;
		hdr.length = a.spec.grow_only ? std::max(old_len, a.spec.count) : a.spec.count;
		a.length = hdr.length;
		if (hdr.length > 0 && !a.spec.keep_size) {
			// An Array family keeps its allocation: the capacity grows only when the length outgrows it.
			if (a.spec.simple) {
				if (old_len == 0 && hdr.size == 0) hdr.increment = -1;
				// SimpleArray<int>::increase_size @4220e0 with the default increment -1: 4 slots for the first element, then the size doubles
				// (add() grows when size <= length, so n elements need the smallest 4 * 2^k >= n).
				while (hdr.size < hdr.length) hdr.size = hdr.size ? hdr.size * 2 : 4;
			} else if (old_len == 0) {
				hdr.size = hdr.length;
				hdr.increment = 1;
				hdr.flags = 0;
			} else if (hdr.size < hdr.length) {
				hdr.size = hdr.length;
			}
			std::memcpy(hdr.tail, &hdr.size, 4);
			std::memcpy(hdr.tail + 4, &hdr.increment, 2);
		}
		active_[key_of(o, site.step, site.base)] = a;
		return true;
	}
	return have;
}

bool EngineProvider::present(dw::Obj o, const dw::Site& site, int64_t i) {
	if (const Active* a = active(o, site)) {
		if (i < 0 || i >= a->length) return false;
		if (a->spec.present) return a->spec.present(int32_t(i));
		if (i < a->shadow_length && base_.present(o, site, i)) return true;
		return i < a->shadow_length && !a->spec.extra_present ? false : (a->spec.extra_present ? a->spec.extra_present(int32_t(i)) : true);
	}
	return base_.present(o, site, i);
}

int32_t EngineProvider::data_type(dw::Obj o, const dw::Site& site, int64_t i) {
	if (const Active* a = active(o, site)) {
		if (i < a->shadow_length && base_.present(o, site, i)) return base_.data_type(o, site, i);
		return a->spec.dtype ? a->spec.dtype(int32_t(i)) : 0;
	}
	return base_.data_type(o, site, i);
}

dw::Obj EngineProvider::element(dw::Obj o, const dw::Site& site, int64_t i) {
	const Active* a = active(o, site);
	if (!a) return base_.element(o, site, i);
	dw::Rec* parent = static_cast<dw::Rec*>(o);
	dw::Rec* r = nullptr;
	if (i < a->shadow_length) r = static_cast<dw::Rec*>(base_.element(o, site, i));
	if (!r) r = sh_.fresh_rec(parent, site.step, site.base, i, dw::Description::get().step_class(site.step));
	if (a->spec.bind_child) a->spec.bind_child(sh_, r, int32_t(i));
	else if (a->spec.child && a->spec.child_id && i < a->spec.count) sh_.rebind(r, *a->spec.child, a->spec.child_id(int32_t(i)));
	return r;
}

const uint8_t* EngineProvider::string(dw::Obj o, const dw::Site& site, uint32_t& len) {
	const uint8_t* shadow = base_.string(o, site, len);
	auto it = sh_.segs.find(static_cast<const dw::Rec*>(o));
	if (it == sh_.segs.end()) return shadow;
	for (const omap::Segment& s : it->second) {
		if (!s.map->string_fn) continue;
		if (s.len >= 0 && (site.base < s.base || site.base >= s.base + s.len)) continue;
		std::u16string u;
		if (!s.map->string_fn(g_, s.id, site.step, site.base, u)) continue;
		string_buf_.resize(u.size() * 2);
		for (size_t i = 0; i < u.size(); ++i) { string_buf_[2 * i] = uint8_t(u[i]); string_buf_[2 * i + 1] = uint8_t(u[i] >> 8); }
		len = uint32_t(u.size());
		return string_buf_.data();
	}
	return shadow;
}

const uint8_t* EngineProvider::bytes(dw::Obj o, const dw::Site& site, uint64_t n) {
	const uint8_t* shadow = base_.bytes(o, site, n);
	if (const Active* a = active(o, site)) {
		if (a->spec.block) {
			block_buf_.clear();
			a->spec.block(block_buf_);
			if (block_buf_.size() == n) return block_buf_.data();
		}
	}
	const dw::Description& d = dw::Description::get();
	const dw::Rec* rec = static_cast<const dw::Rec*>(o);
	if (rec->gid >= 0 && d.step_kind(site.step) == dw::StepKind::Buf && std::strcmp(d.global_name(rec->gid), "world") == 0) {
		const World& w = g_.world;
		const int nth = next_nth(o, site.step, site.base);
		const int64_t ptr = d.step_const(site.step, 0);
		auto fit = [&](const void* data, size_t bytes) -> const uint8_t* { return bytes == n ? static_cast<const uint8_t*>(data) : shadow; };
		switch (ptr) {
			case 0x138: return fit(w.tdata.data(), w.tdata.size() * 2);
			case 0x15c: return fit(w.seen.data(), w.seen.size());
			case 0x160: return fit(w.seen2.data(), w.seen2.size());
			case 0x164: return fit(w.seen3.data(), w.seen3.size());
			case 0x168: return fit(w.wcoord_seen.data(), w.wcoord_seen.size());
			default:
				if (ptr == INT64_MIN && nth >= 0 && nth < 8 && w.danger.size() == size_t(8) * size_t(w.reg_size))
					return fit(w.danger.data() + size_t(nth) * size_t(w.reg_size), size_t(w.reg_size) * 4);
		}
	}
	return shadow;
}

}  // namespace ron::sim
