// The DataWalk interpreter (decode, checksum, field access, CHECK_SUMS parts, verify block): see datawalk.hpp and
// docs/state-spec.md. The semantics of every step mirror re/tools/savdump.mjs (decode) and the CheckSum walker of the original.
#include "sim/datawalk.hpp"

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace ron::sim::datawalk {

namespace {

// ------------------------------------------------------------------------------------------------ generated table

struct Expr { int32_t k, v; };  // k = 0: constant v; k = 1: bytecode at kCode[v]
struct Step { uint8_t op, kind, etype; int32_t a, b, c, d, e, f, g, h, i; };
struct Arg { int32_t name, expr; };
struct Disp { int32_t type, fn, cls; };
struct FnDesc { int32_t name, cls, params0, nparams, body0, bodyn, addr; };
struct GlobalDesc { int32_t name, cls; };
struct ClassDesc { int32_t name, start, n; };
struct FieldDesc { int32_t name, off, size, tc, esize, type; };

#include "sim/datawalk_desc.inc"

enum Opc : int32_t {
	OP_END, OP_CONST, OP_VAR, OP_LOCAL, OP_RD, OP_UPRD, OP_GRD, OP_LEN, OP_RT, OP_NEG, OP_NOT, OP_BNOT,
	OP_MUL, OP_ADD, OP_SUB, OP_SHL, OP_SHR, OP_LT, OP_LE, OP_GT, OP_GE, OP_EQ, OP_NE,
	OP_AND, OP_XOR, OP_OR, OP_LAND, OP_LOR, OP_TERN, OP_INPUT, OP_CHECKSUM, OP_FLAGS, OP_SAVEVER, OP_DIV, OP_MOD,
};

constexpr size_t kStrCount = sizeof(kStr) / sizeof(kStr[0]);
constexpr int kFnCount = int(sizeof(kFns) / sizeof(kFns[0]));
constexpr int kGlobalCount = int(sizeof(kGlobals) / sizeof(kGlobals[0]));
constexpr int kClassCount = int(sizeof(kClasses) / sizeof(kClasses[0]));
constexpr int kStepCount = int(sizeof(kSteps) / sizeof(kSteps[0]));

const char* str(int32_t i) { return i >= 0 && size_t(i) < kStrCount ? kStr[i] : ""; }

struct WalkError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void fail(const char* fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	throw WalkError(buf);
}

int string_index(const char* s) {
	for (size_t i = 0; i < kStrCount; ++i)
		if (std::strcmp(kStr[i], s) == 0) return int(i);
	return -1;
}

template <class T, class Get>
int bsearch_name(int n, const char* name, Get get) {
	int lo = 0, hi = n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = std::strcmp(get(mid), name);
		if (c == 0) return mid;
		if (c < 0) lo = mid + 1; else hi = mid - 1;
	}
	return -1;
}

uint64_t mix_key(int32_t step, int64_t base, int64_t index) {
	uint64_t h = uint64_t(uint32_t(step)) * 0x9E3779B97F4A7C15ull;
	h ^= (uint64_t(base) + 0x7F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
	h = (h << 29) | (h >> 35);
	h ^= (uint64_t(index) + 0x1234567ull) * 0x165667B19E3779F9ull;
	return h;
}

}  // namespace

uint32_t adler32_bytes(const uint8_t* p, size_t n, uint32_t seed) {
	uint32_t a = seed & 0xffff, b = seed >> 16;
	while (n) {
		size_t k = n < 5552 ? n : 5552;
		n -= k;
		while (k--) { a += *p++; b += a; }
		a %= 65521;
		b %= 65521;
	}
	return (b << 16) | a;
}

// ------------------------------------------------------------------------------------------------ Description

const Description& Description::get() {
	static const Description d;
	return d;
}
int Description::function_count() const { return kFnCount; }
const char* Description::function_name(int fn) const { return str(kFns[fn].name); }
const char* Description::function_class(int fn) const { return str(kFns[fn].cls); }
int Description::find_function(const char* name) const {
	return bsearch_name<FnDesc>(kFnCount, name, [](int i) { return str(kFns[i].name); });
}
int Description::global_count() const { return kGlobalCount; }
const char* Description::global_name(int gid) const { return str(kGlobals[gid].name); }
const char* Description::global_class(int gid) const { return str(kGlobals[gid].cls); }
int Description::find_global(const char* name) const {
	return bsearch_name<GlobalDesc>(kGlobalCount, name, [](int i) { return str(kGlobals[i].name); });
}
int Description::step_count() const { return kStepCount; }
int Description::step_function(int step) const { return kStepFn[step]; }
StepKind Description::step_kind(int step) const { return StepKind(kSteps[step].op); }
ContainerKind Description::container_kind(int step) const { return ContainerKind(kSteps[step].kind); }
const char* Description::local_name(int step) const { return kSteps[step].op == uint8_t(StepKind::Local) ? str(kSteps[step].a) : ""; }
const char* Description::local_writes(int step) const { return kSteps[step].op == uint8_t(StepKind::Local) ? str(kSteps[step].c) : ""; }
const char* Description::step_class(int step) const {
	const Step& s = kSteps[step];
	if (s.op == uint8_t(StepKind::Deref)) return str(s.d);
	if (s.op == uint8_t(StepKind::Container)) return str(s.a);
	return "";
}
int64_t Description::step_const(int step, int slot) const {
	const Step& s = kSteps[step];
	int e = slot == 0 ? s.a : s.b;
	if (e < 0 || kExpr[e].k != 0) return INT64_MIN;
	return kExpr[e].v;
}
int Description::find_step(int fn, StepKind kind, int64_t a_const, int nth) const {
	for (int i = 0; i < kStepCount; ++i) {
		if (kStepFn[i] != fn || kSteps[i].op != uint8_t(kind)) continue;
		if (a_const >= 0 && step_const(i, 0) != a_const) continue;
		if (nth-- == 0) return i;
	}
	return -1;
}
int Description::dispatch_function(int step, int32_t data_type) const {
	const Step& s = kSteps[step];
	for (int k = 0; k < s.g; ++k)
		if (kDisp[s.f + k].type == data_type) return kDisp[s.f + k].fn;
	return -1;
}
int Description::field_count(const char* cls) const {
	int ci = bsearch_name<ClassDesc>(kClassCount, cls, [](int i) { return str(kClasses[i].name); });
	return ci < 0 ? 0 : kClasses[ci].n;
}
static FieldInfo make_field(const FieldDesc& f) {
	return FieldInfo{str(f.name), f.off, f.size, FieldType(f.tc), f.esize, str(f.type)};
}
const FieldInfo* Description::field_at(const char* cls, int i) const {
	static thread_local FieldInfo tmp;
	int ci = bsearch_name<ClassDesc>(kClassCount, cls, [](int k) { return str(kClasses[k].name); });
	if (ci < 0 || i < 0 || i >= kClasses[ci].n) return nullptr;
	tmp = make_field(kFields[kClasses[ci].start + i]);
	return &tmp;
}
const FieldInfo* Description::find_field(const char* cls, const char* name) const {
	static thread_local FieldInfo tmp;
	int ci = bsearch_name<ClassDesc>(kClassCount, cls, [](int k) { return str(kClasses[k].name); });
	if (ci < 0) return nullptr;
	for (int k = 0; k < kClasses[ci].n; ++k) {
		const FieldDesc& f = kFields[kClasses[ci].start + k];
		if (std::strcmp(str(f.name), name) == 0) { tmp = make_field(f); return &tmp; }
	}
	return nullptr;
}

// ------------------------------------------------------------------------------------------------ Rec / Tree

void Rec::write(int64_t off, const uint8_t* p, size_t n) {
	if (!n) return;
	if (off + origin < 0) {
		size_t add = size_t(-(off + origin));
		bytes.insert(bytes.begin(), add, 0);
		origin += int64_t(add);
	}
	size_t hi = size_t(off + origin) + n;
	if (hi > bytes.size()) bytes.resize(hi, 0);
	std::memcpy(bytes.data() + (off + origin), p, n);
}
void Rec::read(int64_t off, uint8_t* out, size_t n) const {
	for (size_t i = 0; i < n; ++i) {
		int64_t o = off + int64_t(i) + origin;
		out[i] = o >= 0 && size_t(o) < bytes.size() ? bytes[size_t(o)] : 0;
	}
}
int64_t Rec::read_int(int64_t off, int n, bool is_signed) const {
	uint8_t b[8] = {};
	read(off, b, size_t(n));
	uint64_t u = 0;
	for (int i = 0; i < n; ++i) u |= uint64_t(b[i]) << (8 * i);
	if (is_signed && n < 8 && (u >> (8 * n - 1)) & 1) u |= ~uint64_t(0) << (8 * n);
	return int64_t(u);
}

Rec* Tree::global(int gid) {
	auto it = globals_.find(gid);
	if (it != globals_.end()) return it->second;
	Rec* r = new_rec(d_->global_class(gid));
	r->gid = gid;
	globals_[gid] = r;
	return r;
}
const Rec* Tree::find_global(int gid) const {
	auto it = globals_.find(gid);
	return it == globals_.end() ? nullptr : it->second;
}
Rec* Tree::new_rec(const char* cls) {
	recs_.emplace_back();
	recs_.back().cls = cls ? cls : "";
	return &recs_.back();
}
ContainerData* Tree::new_container() {
	cds_.emplace_back();
	return &cds_.back();
}
size_t Tree::event_count() const {
	size_t n = 0;
	for (const Rec& r : recs_) n += r.ev.size();
	return n;
}

// ------------------------------------------------------------------------------------------------ the walker

namespace {

struct Vars {
	int n = 0;
	int32_t id[10];
	int64_t val[10];
	bool get(int32_t k, int64_t& out) const {
		for (int i = n - 1; i >= 0; --i) if (id[i] == k) { out = val[i]; return true; }
		return false;
	}
	void set(int32_t k, int64_t v) {
		for (int i = 0; i < n; ++i) if (id[i] == k) { val[i] = v; return; }
		if (n >= 10) fail("too many loop variables");
		id[n] = k; val[n] = v; ++n;
	}
};

struct Locals {
	std::vector<std::pair<int32_t, int64_t>> v;
	bool get(int32_t k, int64_t& out) const {
		for (const auto& p : v) if (p.first == k) { out = p.second; return true; }
		return false;
	}
	void set(int32_t k, int64_t x) {
		for (auto& p : v) if (p.first == k) { p.second = x; return; }
		v.emplace_back(k, x);
	}
};

struct Ctx {
	Obj obj = nullptr;
	int64_t base = 0;
	int gid = -1;
	Locals* L = nullptr;
	Vars params, vars;
	const Ctx* parent = nullptr;
};

class Walker {
public:
	Walker(const Description& d, Tree* tree, Provider* prov, const uint8_t* data, size_t size, size_t pos, int32_t save_version)
		: d_(d), tree_(tree), prov_(prov), data_(data), size_(size), pos_(pos), save_version_(save_version), decode_(prov == nullptr) {
		var_i_ = string_index("i");
	}

	bool decode_mode() const { return decode_; }
	size_t pos() const { return pos_; }
	uint64_t tags() const { return tags_; }
	void set_locals_log(std::vector<LocalValue>* l) { log_ = l; }
	void set_record(bool r) { record_ = r; }
	void reset_hash() { ha_ = 1; hb_ = 0; }
	uint32_t hash_value() const { return (hb_ << 16) | ha_; }
	void set_hook(std::function<void(int, const uint8_t*, size_t)> h) { hook_ = std::move(h); }
	void hash_bytes(const uint8_t* p, size_t n) {
		if (hook_) hook_(cur_step_, p, n);
		while (n) {
			size_t k = n < 5552 ? n : 5552;
			n -= k;
			while (k--) { ha_ += *p++; hb_ += ha_; }
			ha_ %= 65521;
			hb_ %= 65521;
		}
	}
	void hash_le(int64_t v, int n) {
		uint8_t b[8];
		for (int i = 0; i < n; ++i) b[i] = uint8_t(uint64_t(v) >> (8 * i));
		hash_bytes(b, size_t(n));
	}
	Provider* provider() const { return prov_; }
	void clear_lengths() { lengths_.clear(); }

	Obj global_obj(int gid) { return decode_ ? static_cast<Obj>(tree_->global(gid)) : prov_->global(gid); }

	// Runs function `fn` on `obj` as a top-level walk.
	void run_function(int fn, Obj obj, int gid, int64_t base, const Vars& params) {
		if (prov_) prov_->begin_walk();
		Locals L;
		Ctx c;
		c.obj = obj; c.base = base; c.gid = gid; c.L = &L; c.params = params;
		fn_stack_.clear();
		invoke_body(fn, c);
	}

private:
	const Description& d_;
	Tree* tree_;
	Provider* prov_;
	const uint8_t* data_;
	size_t size_, pos_;
	int32_t save_version_;
	bool decode_;
	bool record_ = true;
	uint64_t tags_ = 0;
	uint32_t ha_ = 1, hb_ = 0;
	int var_i_ = -1;
	std::vector<LocalValue>* log_ = nullptr;
	std::map<int, int64_t> lengths_;
	std::vector<int> fn_stack_;
	std::function<void(int, const uint8_t*, size_t)> hook_;
	int cur_step_ = -1;

	[[noreturn]] void err(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
		char buf[400];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		char out[640];
		std::snprintf(out, sizeof out, "%s (in %s)", buf, fn_stack_.empty() ? "?" : d_.function_name(fn_stack_.back()));
		throw WalkError(out);
	}

	const uint8_t* take(size_t n, const char* what) {
		if (pos_ + n > size_) err("end of data at %zu reading %zu bytes for %s", pos_, n, what);
		const uint8_t* p = data_ + pos_;
		pos_ += n;
		return p;
	}
	static int64_t le(const uint8_t* p, int n, bool sgn) {
		uint64_t u = 0;
		for (int i = 0; i < n; ++i) u |= uint64_t(p[i]) << (8 * i);
		if (sgn && n < 8 && (u >> (8 * n - 1)) & 1) u |= ~uint64_t(0) << (8 * n);
		return int64_t(u);
	}

	// -------------------------------------------------------------------------------------------- expressions
	int64_t read_obj(Obj o, int64_t off, int n, bool sgn) {
		if (decode_) return static_cast<Rec*>(o)->read_int(off, n, sgn);
		const uint8_t* p = prov_->image(o, off, uint32_t(n));
		return le(p, n, sgn);
	}
	int64_t eval(const Ctx& c, int e) {
		const Expr& x = kExpr[e];
		if (x.k == 0) return x.v;
		const int32_t* pc = kCode + x.v;
		int64_t st[40];
		int sp = 0;
		for (;;) {
			switch (*pc++) {
				case OP_END: return sp > 0 ? st[sp - 1] : 0;
				case OP_CONST: st[sp++] = *pc++; break;
				case OP_VAR: {
					int32_t id = *pc++;
					int64_t v;
					if (c.vars.get(id, v) || c.params.get(id, v)) st[sp++] = v;
					else err("unbound variable %s", str(id));
					break;
				}
				case OP_LOCAL: {
					int64_t v = 0;
					c.L->get(*pc++, v);
					st[sp++] = v;
					break;
				}
				case OP_RD: {
					int t = *pc++;
					int64_t off = st[--sp];
					static const int w[6] = {1, 1, 2, 2, 4, 4};
					st[sp++] = read_obj(c.obj, c.base + off, w[t], (t & 1) == 0);
					break;
				}
				case OP_UPRD: {
					int t = *pc++;
					int64_t off = st[--sp];
					if (!c.parent) err("up.* without a parent context");
					static const int w[6] = {1, 1, 2, 2, 4, 4};
					st[sp++] = read_obj(c.parent->obj, c.parent->base + off, w[t], (t & 1) == 0);
					break;
				}
				case OP_GRD: {
					int gid = *pc++;
					int t = *pc++;
					int64_t off = st[--sp];
					static const int w[6] = {1, 1, 2, 2, 4, 4};
					st[sp++] = read_obj(global_obj(gid), off, w[t], (t & 1) == 0);
					break;
				}
				case OP_LEN: {
					int gid = *pc++;
					auto it = lengths_.find(gid);
					st[sp++] = it == lengths_.end() ? 0 : it->second;
					break;
				}
				case OP_RT: {
					const char* n = str(*pc++);
					if (std::strcmp(n, "GraphicEvents.events.length") == 0) st[sp++] = 60415;
					else err("runtime value needed by the loader: %s", n);
					break;
				}
				case OP_NEG: st[sp - 1] = -st[sp - 1]; break;
				case OP_NOT: st[sp - 1] = !st[sp - 1]; break;
				case OP_BNOT: st[sp - 1] = ~st[sp - 1]; break;
				case OP_TERN: { int64_t f = st[--sp], t = st[--sp], cnd = st[--sp]; st[sp++] = cnd ? t : f; break; }
				case OP_INPUT: st[sp++] = decode_ ? 1 : 0; break;
				case OP_CHECKSUM: st[sp++] = decode_ ? 0 : 1; break;
				case OP_FLAGS: st[sp++] = -1; break;
				case OP_SAVEVER: st[sp++] = save_version_; break;
				default: {
					int op = pc[-1];
					int64_t b = st[--sp], a = st[--sp], r = 0;
					switch (op) {
						case OP_MUL: r = a * b; break;
						case OP_ADD: r = a + b; break;
						case OP_SUB: r = a - b; break;
						case OP_SHL: r = int64_t(uint64_t(a) << (b & 63)); break;
						case OP_SHR: r = a >> (b & 63); break;
						case OP_LT: r = a < b; break;
						case OP_LE: r = a <= b; break;
						case OP_GT: r = a > b; break;
						case OP_GE: r = a >= b; break;
						case OP_EQ: r = a == b; break;
						case OP_NE: r = a != b; break;
						case OP_AND: r = a & b; break;
						case OP_XOR: r = a ^ b; break;
						case OP_OR: r = a | b; break;
						case OP_LAND: r = a && b; break;
						case OP_LOR: r = a || b; break;
						case OP_DIV: if (!b) err("division by zero"); r = a / b; break;
						case OP_MOD: if (!b) err("division by zero"); r = a % b; break;
						default: err("bad bytecode %d", op);
					}
					st[sp++] = r;
				}
			}
		}
	}

	// -------------------------------------------------------------------------------------------- execution
	void invoke_body(int fn, const Ctx& c) {
		const FnDesc& f = kFns[fn];
		if (fn_stack_.size() > 200) err("walk recursion too deep");
		fn_stack_.push_back(fn);
		run_block(f.body0, f.bodyn, c);
		fn_stack_.pop_back();
	}
	void run_block(int start, int n, const Ctx& c) {
		for (int i = 0; i < n; ++i) exec(start + i, c);
	}
	void add_event(Obj o, uint64_t key, int32_t step, uint8_t type, int64_t value, Rec* child, ContainerData* cd) {
		if (!record_) return;
		Event e;
		e.key = key; e.step = step; e.type = type; e.value = value; e.child = child; e.cd = cd;
		static_cast<Rec*>(o)->ev.push_back(e);
	}
	uint32_t take_u32(const char* what) { return uint32_t(le(take(4, what), 4, false)); }

	void write_rec(const Ctx& c, int64_t off, const uint8_t* p, size_t n) { static_cast<Rec*>(c.obj)->write(off, p, n); }

	Ctx switched(const Ctx& c, int gid) {
		if (gid < 0) return c;
		Ctx r = c;
		r.obj = global_obj(gid);
		r.base = 0;
		r.gid = gid;
		return r;
	}

	void exec(int id, const Ctx& c0) {
		const Step& s = kSteps[id];
		cur_step_ = id;
		switch (StepKind(s.op)) {
			case StepKind::Tag: {
				if (!decode_) return;
				uint8_t b = *take(1, "tag");
				++tags_;
				if (b != uint8_t(s.b)) err("tag \"%s\" mismatch at %zu: 0x%x != 0x%x", str(s.a), pos_ - 1, unsigned(b), unsigned(s.b & 0xff));
				return;
			}
			case StepKind::Raw: {
				Ctx c = switched(c0, s.c);
				int64_t at = eval(c, s.a), end = eval(c, s.b), n = end - at;
				if (n < 0 || n > (1 << 28)) err("bad raw range [%lld, %lld)", (long long)at, (long long)end);
				if (decode_) {
					const uint8_t* p = take(size_t(n), "raw range");
					write_rec(c, c.base + at, p, size_t(n));
				} else if (s.kind == 1) {
					// Hashed as zeros by the original (see HASH_AS_ZERO in re/tools/datawalk-gen.mjs).
					static const uint8_t zeros[64] = {};
					if (n > 64) err("zero-hashed raw range too long");
					hash_bytes(zeros, size_t(n));
				} else {
					hash_bytes(prov_->image(c.obj, c.base + at, uint32_t(n)), size_t(n));
				}
				return;
			}
			case StepKind::Local: exec_local(id, s, c0); return;
			case StepKind::String: {
				Ctx c = switched(c0, s.c);
				if (decode_) {
					uint32_t n = take_u32("string length");
					if (n > 0x100000) err("implausible string length %u at %zu", n, pos_ - 4);
					const uint8_t* p = take(2ull * n, "string");
					if (record_) {
						ContainerData* cd = tree_->new_container();
						cd->length = int32_t(n);
						cd->data.assign(p, p + 2ull * n);
						add_event(c.obj, mix_key(id, c.base, 0), id, 2, 0, nullptr, cd);
					}
				} else {
					// String::walk_data under the CheckSum walker: u32 curr_len, then the characters. (Callers that exclude a
					// string from the checksum guard it with `checksum == 0`.)
					uint32_t n = 0;
					const uint8_t* p = prov_->string(c.obj, Site{id, c.base}, n);
					if (!p && n) err("provider has no string for step %d", id);
					hash_le(n, 4);
					hash_bytes(p, 2ull * n);
				}
				return;
			}
			case StepKind::BitMask: {
				Ctx c = switched(c0, s.c);
				int64_t at = eval(c, s.a);
				if (decode_) {
					const uint8_t* h = take(8, "bitmask header");
					write_rec(c, c.base + at, h, 8);
					int32_t size = int32_t(le(h + 4, 4, true));
					if (size < 0 || size > (1 << 20)) err("implausible bitmask size %d", size);
					const uint8_t* b = take(size_t(size), "bitmask bits");
					write_rec(c, c.base + at + 0xc, b, size_t(size));
				} else {
					uint8_t h[8];
					std::memcpy(h, prov_->image(c.obj, c.base + at, 8), 8);
					hash_bytes(h, 8);
					int32_t size = int32_t(le(h + 4, 4, true));
					if (size < 0 || size > (1 << 20)) err("implausible bitmask size %d", size);
					hash_bytes(prov_->image(c.obj, c.base + at + 0xc, uint32_t(size)), size_t(size));
				}
				return;
			}
			case StepKind::Buf: {
				int64_t len = eval(c0, s.b);
				if (len < 0 || len > (1ll << 28)) err("bad buffer length %lld", (long long)len);
				if (decode_) {
					const uint8_t* p = take(size_t(len), "buffer");
					ContainerData* cd = tree_->new_container();
					if (record_) cd->data.assign(p, p + len);
					add_event(c0.obj, mix_key(id, c0.base, 0), id, 2, 0, nullptr, cd);
				} else {
					const uint8_t* p = prov_->bytes(c0.obj, Site{id, c0.base}, uint64_t(len));
					if (!p && len) err("provider has no buffer for step %d", id);
					hash_bytes(p, size_t(len));
				}
				return;
			}
			case StepKind::Call: {
				Ctx c = switched(c0, s.c);
				int64_t at = s.b >= 0 ? eval(c, s.b) : 0;
				Ctx ch;
				ch.obj = c.obj; ch.base = c.base + at; ch.gid = c.gid; ch.parent = c.parent;
				for (int k = 0; k < s.e; ++k) ch.params.set(kArgs[s.d + k].name, eval(c, kArgs[s.d + k].expr));
				Locals L;
				ch.L = &L;
				invoke_body(s.a, ch);
				for (const auto& p : L.v) c.L->set(p.first, p.second);
				return;
			}
			case StepKind::If: {
				int64_t cnd = eval(c0, s.a);
				if (cnd) run_block(s.b, s.c, c0); else run_block(s.d, s.e, c0);
				return;
			}
			case StepKind::Loop: {
				int64_t n = eval(c0, s.b);
				if (n < 0 || n > (1ll << 28)) err("bad loop count %lld", (long long)n);
				for (int64_t i = 0; i < n; ++i) {
					Ctx c = c0;
					c.vars.set(s.a, i);
					run_block(s.c, s.d, c);
				}
				return;
			}
			case StepKind::Deref: {
				Ctx c = switched(c0, s.c);
				int64_t idx = s.b >= 0 ? eval(c, s.b) : 0;
				Obj child;
				if (decode_) {
					Rec* r = tree_->new_rec(str(s.d));
					add_event(c.obj, mix_key(id, c.base, idx), id, 0, idx, r, nullptr);
					child = r;
				} else {
					child = prov_->child(c.obj, Site{id, c.base}, idx);
					if (!child) err("provider has no object for deref step %d index %lld", id, (long long)idx);
				}
				Ctx e;
				e.obj = child; e.base = 0; e.gid = -1; e.L = c.L; e.params = c.params; e.vars = c.vars; e.parent = &c;
				run_block(s.e, s.f, e);
				return;
			}
			case StepKind::Container: exec_container(id, s, c0); return;
		}
		err("unknown step op %d", s.op);
	}

	void exec_local(int id, const Step& s, const Ctx& c) {
		static const int width[7] = {1, 1, 2, 2, 4, 4, 4};
		int n = width[s.etype];
		int64_t v;
		if (decode_) {
			const uint8_t* p = take(size_t(n), "local");
			bool sgn = s.etype == 1 || s.etype == 3 || s.etype == 5;
			v = le(p, n, sgn);
			add_event(c.obj, mix_key(id, c.base, 0), id, 1, v, nullptr, nullptr);
			if (log_) log_->push_back(LocalValue{str(s.a), v});
		} else {
			if (!prov_->local(c.obj, Site{id, c.base}, v)) err("provider has no value for local %s (step %d)", str(s.a), id);
			hash_le(v, n);
		}
		c.L->set(s.a, v);
	}

	int32_t read_i32(const char* what) { return int32_t(le(take(4, what), 4, true)); }

	void exec_container(int id, const Step& s, const Ctx& c) {
		const ContainerKind kind = ContainerKind(s.kind);
		const Site site{id, c.base};
		ContainerData* cd = nullptr;
		ContainerHeader h;
		if (decode_) {
			cd = tree_->new_container();
			add_event(c.obj, mix_key(id, c.base, 0), id, 2, 0, nullptr, cd);
		} else if (!prov_->container(c.obj, site, h)) {
			err("provider has no container for step %d", id);
		}
		auto set_length = [&](int64_t len) { if (c.gid >= 0 && c.base == 0) lengths_[c.gid] = len; };
		// The header of the Array families: i32 length; if non-zero i32 size, i16 increment, u8 flags.
		auto header = [&]() -> int32_t {
			if (decode_) {
				int32_t length = read_i32("container length");
				cd->length = length;
				set_length(length);
				if (!length) return 0;
				cd->size = read_i32("container size");
				cd->increment = int16_t(le(take(2, "increment"), 2, true));
				cd->flags = *take(1, "flags");
				if (cd->size < length || length < 0 || length > 10000000) err("implausible container length %d size %d at %zu", length, cd->size, pos_);
				return length;
			}
			hash_le(h.length, 4);
			set_length(h.length);
			if (!h.length) return 0;
			hash_le(h.size, 4);
			hash_le(h.increment, 2);
			hash_le(h.flags, 1);
			return h.length;
		};
		auto element_ctx = [&](Obj r, Ctx& e, Locals& L, bool share_locals, int64_t index) {
			e.obj = r; e.base = 0; e.gid = -1;
			e.L = share_locals ? c.L : &L;
			e.params = share_locals ? c.params : Vars();
			e.vars = c.vars;
			if (share_locals && var_i_ >= 0) e.vars.set(var_i_, index);
			e.parent = &c;
		};
		const char* ecls = str(s.a);
		switch (kind) {
			case ContainerKind::Array: case ContainerKind::ObjectArray: case ContainerKind::NamedObjectArray: {
				int32_t n = header();
				for (int32_t i = 0; i < n; ++i) {
					Obj eo;
					if (decode_) { Rec* r = tree_->new_rec(ecls); cd->elems.push_back(r); eo = r; }
					else { eo = prov_->element(c.obj, site, i); if (!eo) err("provider has no element %d of step %d", i, id); }
					Ctx e; Locals L;
					element_ctx(eo, e, L, false, i);
					run_block(s.c, s.d, e);
				}
				break;
			}
			case ContainerKind::SimpleArray: {
				int32_t n = header();
				if (!n) break;
				int64_t stride = s.b >= 0 ? eval(c, s.b) : 0;
				int64_t len = stride * n;
				if (len < 0 || len > (1ll << 28)) err("bad SimpleArray block %lld", (long long)len);
				if (decode_) {
					const uint8_t* p = take(size_t(len), "SimpleArray elements");
					if (record_) cd->data.assign(p, p + len);
				} else {
					const uint8_t* p = prov_->bytes(c.obj, site, uint64_t(len));
					if (!p && len) err("provider has no block for step %d", id);
					hash_bytes(p, size_t(len));
				}
				break;
			}
			case ContainerKind::PtrArray: case ContainerKind::MultiPtrArray: {
				int32_t n = header();
				if (!n) break;
				const bool multi = kind == ContainerKind::MultiPtrArray;
				std::vector<uint8_t> present(static_cast<size_t>(n), 0);
				std::vector<int32_t> types(static_cast<size_t>(n), 0);
				if (decode_) {
					const uint8_t* p = take(size_t(n), "presence bytes");
					std::memcpy(present.data(), p, size_t(n));
					if (multi) for (int32_t i = 0; i < n; ++i) if (present[i]) types[i] = read_i32("data type");
					const uint8_t* t = take(6, "size+increment");
					std::memcpy(cd->tail, t, 6);
					cd->present = present;
					cd->types = types;
					cd->elems.assign(size_t(n), nullptr);
				} else {
					for (int32_t i = 0; i < n; ++i) { present[i] = prov_->present(c.obj, site, i) ? 1 : 0; hash_bytes(&present[i], 1); }
					if (multi) for (int32_t i = 0; i < n; ++i) if (present[i]) { types[i] = prov_->data_type(c.obj, site, i); hash_le(types[i], 4); }
					hash_bytes(h.tail, 6);
				}
				for (int32_t i = 0; i < n; ++i) {
					if (!present[i]) continue;
					const char* cls = ecls;
					int fn = -1;
					if (multi) {
						bool found = false;
						for (int k = 0; k < s.g; ++k)
							if (kDisp[s.f + k].type == types[i]) { fn = kDisp[s.f + k].fn; cls = str(kDisp[s.f + k].cls); found = true; break; }
						if (!found) err("no dispatch for data type %d", types[i]);
					}
					Obj eo;
					if (decode_) { Rec* r = tree_->new_rec(cls); cd->elems[i] = r; eo = r; }
					else { eo = prov_->element(c.obj, site, i); if (!eo) err("provider has no element %d of step %d", i, id); }
					if (fn >= 0) {
						Ctx e; Locals L;
						e.obj = eo; e.base = 0; e.gid = -1; e.L = &L; e.parent = &c;
						invoke_body(fn, e);
					} else {
						Ctx e; Locals L;
						element_ctx(eo, e, L, false, i);
						run_block(s.c, s.d, e);
					}
				}
				break;
			}
			case ContainerKind::LinkList: case ContainerKind::PtrLinkListAbstract: case ContainerKind::OrderList: {
				int64_t n;
				if (s.e >= 0) {
					int64_t tmp;
					if (s.i >= 0 && !c.L->get(s.i, tmp)) {  // the count is a local nobody has walked: it is the i32 read here
						int32_t v;
						if (decode_) v = read_i32("list count"); else { v = h.length; hash_le(v, 4); }
						c.L->set(s.i, v);
					}
					n = eval(c, s.e);
				} else if (decode_) {
					n = read_i32("list count");
				} else {
					n = h.length;
					hash_le(n, 4);
				}
				if (n < 0 || n > 10000000) err("implausible list count %lld at %zu", (long long)n, pos_);
				if (decode_) cd->length = int32_t(n);
				for (int64_t i = 0; i < n; ++i) {
					Obj eo;
					if (decode_) { Rec* r = tree_->new_rec(ecls); cd->elems.push_back(r); eo = r; }
					else { eo = prov_->element(c.obj, site, i); if (!eo) err("provider has no node %lld of step %d", (long long)i, id); }
					Ctx e; Locals L;
					element_ctx(eo, e, L, true, i);
					run_block(s.c, s.d, e);
				}
				break;
			}
		}
	}
};

Vars make_params(const Description& d, const std::vector<std::pair<const char*, int64_t>>& params) {
	(void)d;
	Vars v;
	for (const auto& p : params) {
		int id = string_index(p.first);
		if (id < 0) fail("unknown parameter name %s", p.first);
		v.set(id, p.second);
	}
	return v;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ decode API

size_t read_string(const uint8_t* data, size_t size, size_t pos, std::string& out) {
	out.clear();
	if (pos + 4 > size) return std::string::npos;
	uint32_t n = uint32_t(data[pos]) | uint32_t(data[pos + 1]) << 8 | uint32_t(data[pos + 2]) << 16 | uint32_t(data[pos + 3]) << 24;
	if (n > 0x100000 || pos + 4 + 2ull * n > size) return std::string::npos;
	for (uint32_t i = 0; i < n; ++i) {
		uint16_t u = uint16_t(data[pos + 4 + 2 * i] | data[pos + 5 + 2 * i] << 8);
		out.push_back(u < 0x80 ? char(u) : '?');
	}
	return pos + 4 + 2ull * n;
}

static DecodeResult decode_sequence(Tree& tree, const uint8_t* data, size_t size, size_t pos, int32_t version,
		const std::vector<std::pair<std::string, std::string>>& list, std::vector<LocalValue>* locals_for_last) {
	DecodeResult r;
	const Description& d = tree.description();
	Walker w(d, &tree, nullptr, data, size, pos, version);
	try {
		for (size_t k = 0; k < list.size(); ++k) {
			int fn = d.find_function(list[k].first.c_str());
			if (fn < 0) fail("function %s is not described", list[k].first.c_str());
			int gid = -1;
			Obj obj;
			if (!list[k].second.empty()) {
				gid = d.find_global(list[k].second.c_str());
				if (gid < 0) fail("global %s is not declared", list[k].second.c_str());
				obj = tree.global(gid);
			} else {
				obj = tree.new_rec(d.function_class(fn));
			}
			bool last = k + 1 == list.size();
			w.set_locals_log(last ? locals_for_last : nullptr);
			w.set_record(!(last && locals_for_last));
			w.run_function(fn, obj, gid, 0, Vars());
		}
		r.ok = true;
	} catch (const WalkError& e) {
		r.error = e.what();
	}
	r.end = w.pos();
	r.tags = w.tags();
	return r;
}

DecodeResult decode_function(Tree& tree, const uint8_t* data, size_t size, size_t pos, const char* function, const char* global,
		int32_t save_version, std::vector<LocalValue>* locals) {
	return decode_sequence(tree, data, size, pos, save_version, {{function, global ? global : ""}}, locals);
}

SaveDecode decode_save(Tree& tree, const uint8_t* data, size_t size, size_t pos) {
	SaveDecode s;
	size_t p = read_string(data, size, pos, s.magic);
	if (p == std::string::npos || p + 4 > size) { s.result.error = "not a save: no magic"; return s; }
	s.version = uint32_t(data[p]) | uint32_t(data[p + 1]) << 8 | uint32_t(data[p + 2]) << 16 | uint32_t(data[p + 3]) << 24;
	p += 4;
	s.walk_begin = p;
	s.result = decode_sequence(tree, data, size, p, int32_t(s.version),
		{{"WalkDataGame::walk_data", ""}, {"SaveGame::verify_save", ""}}, &s.verify);
	s.end = s.result.end;
	return s;
}

// ------------------------------------------------------------------------------------------------ field access

static bool split_path(const std::string& path, std::vector<std::pair<std::string, int>>& out, std::string& error) {
	size_t i = 0;
	while (i < path.size()) {
		size_t j = path.find_first_of(".[", i);
		std::string name = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
		int idx = -1;
		if (j != std::string::npos && path[j] == '[') {
			size_t k = path.find(']', j);
			if (k == std::string::npos) { error = "unterminated [ in path"; return false; }
			idx = std::atoi(path.substr(j + 1, k - j - 1).c_str());
			j = k + 1;
		}
		if (name.empty()) { error = "empty name in path"; return false; }
		out.emplace_back(name, idx);
		if (j == std::string::npos || j >= path.size()) break;
		if (path[j] == '.') ++j;
		i = j;
	}
	return !out.empty();
}

static std::string hex_bytes(const Rec& rec, int64_t off, int32_t n) {
	std::string s;
	char b[4];
	for (int32_t k = 0; k < n && k < 64; ++k) {
		uint8_t v;
		rec.read(off + k, &v, 1);
		std::snprintf(b, sizeof b, "%02x", v);
		s += b;
	}
	if (n > 64) s += "...";
	return s;
}

static bool read_scalar(const Rec& rec, int64_t off, FieldType t, FieldValue& out) {
	switch (t) {
		case FieldType::I8: out.i = rec.read_int(off, 1, true); break;
		case FieldType::U8: out.i = rec.read_int(off, 1, false); break;
		case FieldType::I16: out.i = rec.read_int(off, 2, true); break;
		case FieldType::U16: out.i = rec.read_int(off, 2, false); break;
		case FieldType::I32: out.i = rec.read_int(off, 4, true); break;
		case FieldType::U32: case FieldType::Ptr: out.i = rec.read_int(off, 4, false); break;
		case FieldType::F32: {
			uint32_t u = uint32_t(rec.read_int(off, 4, false));
			float f;
			std::memcpy(&f, &u, 4);
			out.is_float = true; out.f = f;
			break;
		}
		case FieldType::F64: {
			uint64_t u = uint64_t(rec.read_int(off, 4, false)) | uint64_t(rec.read_int(off + 4, 4, false)) << 32;
			double f;
			std::memcpy(&f, &u, 8);
			out.is_float = true; out.f = f;
			break;
		}
		default: return false;
	}
	return true;
}

bool read_field(const Rec& rec, const char* cls, const std::string& path, FieldValue& out, std::string& error) {
	const Description& d = Description::get();
	std::vector<std::pair<std::string, int>> toks;
	if (!split_path(path, toks, error)) return false;
	int64_t off = 0;
	std::string cur = cls;
	const FieldInfo* f = nullptr;
	for (size_t k = 0; k < toks.size(); ++k) {
		f = d.find_field(cur.c_str(), toks[k].first.c_str());
		if (!f) { error = "no member '" + toks[k].first + "' in class '" + cur + "'"; return false; }
		off += f->offset;
		int idx = toks[k].second;
		if (idx >= 0) {
			if (idx >= f->count()) { error = "index out of range for " + toks[k].first; return false; }
			off += int64_t(idx) * f->elem_size;
		}
		bool last = k + 1 == toks.size();
		if (!last) {
			if (f->type != FieldType::Struct) { error = "'" + toks[k].first + "' is not a struct"; return false; }
			cur = f->struct_class;
		}
	}
	out = FieldValue();
	out.offset = off;
	out.size = toks.back().second >= 0 ? f->elem_size : f->size;
	char buf[64];
	if (f->type != FieldType::Struct && f->type != FieldType::Bytes && (toks.back().second >= 0 || f->count() == 1)) {
		read_scalar(rec, off, f->type, out);
		if (out.is_float) std::snprintf(buf, sizeof buf, "%.9g", out.f);
		else std::snprintf(buf, sizeof buf, "%lld", (long long)out.i);
		out.text = buf;
	} else if (f->type != FieldType::Struct && f->type != FieldType::Bytes) {
		out.text = "[";
		for (int k = 0; k < f->count() && k < 32; ++k) {
			FieldValue e;
			read_scalar(rec, off + int64_t(k) * f->elem_size, f->type, e);
			if (e.is_float) std::snprintf(buf, sizeof buf, "%.9g", e.f);
			else std::snprintf(buf, sizeof buf, "%lld", (long long)e.i);
			out.text += (k ? "," : "") + std::string(buf);
		}
		if (f->count() > 32) out.text += ",...";
		out.text += "]";
	} else {
		out.text = hex_bytes(rec, off, out.size);
	}
	return true;
}

bool read_field(const Tree& tree, const std::string& path, FieldValue& out, std::string& error) {
	const Description& d = tree.description();
	size_t dot = path.find('.');
	std::string g = path.substr(0, dot);
	int gid = d.find_global(g.c_str());
	if (gid < 0) { error = "unknown global '" + g + "'"; return false; }
	const Rec* r = tree.find_global(gid);
	if (!r) { error = "global '" + g + "' was not decoded"; return false; }
	if (dot == std::string::npos) { error = "path needs a member after the global"; return false; }
	return read_field(*r, d.global_class(gid), path.substr(dot + 1), out, error);
}

// ------------------------------------------------------------------------------------------------ TreeProvider

const uint8_t* TreeProvider::image(Obj o, int64_t off, uint32_t n) {
	scratch_.resize(std::max<size_t>(n, 8));
	static_cast<Rec*>(o)->read(off, scratch_.data(), n);
	return scratch_.data();
}

Event* TreeProvider::find(Rec* r, const Site& site, int64_t index, uint8_t type) {
	if (r->pass != pass_) { r->pass = pass_; r->cursor = 0; }
	const uint64_t key = mix_key(site.step, site.base, index);
	for (size_t k = r->cursor; k < r->ev.size(); ++k) {
		Event& e = r->ev[k];
		if (e.key == key && e.type == type && e.step == site.step) { r->cursor = uint32_t(k + 1); return &e; }
	}
	return nullptr;
}

ContainerData* TreeProvider::last_container(Rec* r, const Site& site) {
	const uint64_t key = mix_key(site.step, site.base, 0);
	for (size_t k = r->cursor; k-- > 0;) {
		Event& e = r->ev[k];
		if (e.key == key && e.type == 2 && e.step == site.step) return e.cd;
	}
	return nullptr;
}

Obj TreeProvider::child(Obj o, const Site& site, int64_t index) {
	Event* e = find(static_cast<Rec*>(o), site, index, 0);
	return e ? e->child : nullptr;
}
bool TreeProvider::local(Obj o, const Site& site, int64_t& value) {
	Rec* r = static_cast<Rec*>(o);
	Event* e = find(r, site, 0, 1);
	if (e) { value = e->value; return true; }
	// GameInfo::walk_data hashes `flags & ~1` through a temporary that only exists in checksum mode (nothing in the stream).
	if (std::strcmp(Description::get().local_name(site.step), "flags_masked") == 0) {
		value = r->read_int(site.base + 0x14, 4, false) & 0xfffffffeull;
		return true;
	}
	return false;
}
bool TreeProvider::container(Obj o, const Site& site, ContainerHeader& hdr) {
	Event* e = find(static_cast<Rec*>(o), site, 0, 2);
	if (!e || !e->cd) return false;
	const ContainerData& cd = *e->cd;
	hdr.length = cd.length; hdr.size = cd.size; hdr.increment = cd.increment; hdr.flags = cd.flags;
	std::memcpy(hdr.tail, cd.tail, 6);
	return true;
}
bool TreeProvider::present(Obj o, const Site& site, int64_t i) {
	ContainerData* cd = last_container(static_cast<Rec*>(o), site);
	return cd && i >= 0 && size_t(i) < cd->present.size() && cd->present[size_t(i)];
}
int32_t TreeProvider::data_type(Obj o, const Site& site, int64_t i) {
	ContainerData* cd = last_container(static_cast<Rec*>(o), site);
	return cd && i >= 0 && size_t(i) < cd->types.size() ? cd->types[size_t(i)] : 0;
}
Obj TreeProvider::element(Obj o, const Site& site, int64_t i) {
	ContainerData* cd = last_container(static_cast<Rec*>(o), site);
	return cd && i >= 0 && size_t(i) < cd->elems.size() ? cd->elems[size_t(i)] : nullptr;
}
const uint8_t* TreeProvider::bytes(Obj o, const Site& site, uint64_t n) {
	Rec* r = static_cast<Rec*>(o);
	ContainerData* cd = nullptr;
	if (Description::get().step_kind(site.step) == StepKind::Container) {
		cd = last_container(r, site);   // a SimpleArray block: the header event was just consumed
	} else {
		Event* e = find(r, site, 0, 2);   // a Buf step: its own event, in order
		cd = e ? e->cd : nullptr;
	}
	if (!cd || cd->data.size() != n) return nullptr;
	return cd->data.data();
}

const uint8_t* TreeProvider::string(Obj o, const Site& site, uint32_t& len) {
	Event* e = find(static_cast<Rec*>(o), site, 0, 2);
	if (!e || !e->cd) { len = 0; return nullptr; }
	len = uint32_t(e->cd->length);
	return e->cd->data.data();
}

// ------------------------------------------------------------------------------------------------ ChecksumWalker

struct ChecksumWalker::Impl {
	const Description& d;
	Provider& p;
	Walker w;
	Impl(const Description& dd, Provider& pp) : d(dd), p(pp), w(dd, nullptr, &pp, nullptr, 0, 0, 0x10) {}
};

ChecksumWalker::ChecksumWalker(const Description& d, Provider& p) : impl_(new Impl(d, p)) {}
ChecksumWalker::~ChecksumWalker() { delete impl_; }
void ChecksumWalker::reset() { impl_->w.reset_hash(); impl_->w.clear_lengths(); impl_->p.begin_walk(); }
uint32_t ChecksumWalker::value() const { return impl_->w.hash_value(); }
void ChecksumWalker::add(const uint8_t* p, size_t n) { impl_->w.hash_bytes(p, n); }
void ChecksumWalker::set_hook(std::function<void(int, const uint8_t*, size_t)> hook) { impl_->w.set_hook(std::move(hook)); }
void ChecksumWalker::add_image(Obj o, int64_t off, uint32_t n) { impl_->w.hash_bytes(impl_->p.image(o, off, n), n); }
Provider& ChecksumWalker::provider() { return impl_->p; }
const Description& ChecksumWalker::description() const { return impl_->d; }
void ChecksumWalker::walk(int fn, Obj o, int gid, int64_t base, const std::vector<std::pair<const char*, int64_t>>& params) {
	impl_->w.run_function(fn, o, gid, base, make_params(impl_->d, params));
}
void ChecksumWalker::walk(const char* function, Obj o, int gid, int64_t base, const std::vector<std::pair<const char*, int64_t>>& params) {
	int fn = impl_->d.find_function(function);
	if (fn < 0) fail("function %s is not described", function);
	walk(fn, o, gid, base, params);
}

// ------------------------------------------------------------------------------------------------ CHECK_SUMS parts

const char* const kCheckPartNames[kCheckPartCount] = {"units", "builds", "walls", "ammo", "deaths", "groups", "guys", "leaders", "cities",
	"items", "goods", "world", "rules", "scenario_data", "script_run_time"};

namespace {

// Everything the drivers need to know about the layout, resolved by name from the generated tables.
struct Layout {
	const Description& d;
	Provider& p;
	ChecksumWalker w;
	Obj objects, leaders, groups, world, obj_base;
	int g_objects, g_leaders, g_groups, g_world;
	int off_valid, off_unit_mark, off_build_mark, off_wall_mark;
	int fn_multi_ptr;
	int step_lists;
	Layout(const Description& dd, Provider& pp) : d(dd), p(pp), w(dd, pp) {
		g_objects = gid("objects"); g_leaders = gid("leaders"); g_groups = gid("groups"); g_world = gid("world");
		objects = p.global(g_objects); leaders = p.global(g_leaders); groups = p.global(g_groups); world = p.global(g_world);
		obj_base = p.global(gid("obj_base"));
		off_valid = field("Objects", "valid"); off_unit_mark = field("Objects", "unit_mark");
		off_build_mark = field("Objects", "build_mark"); off_wall_mark = field("Objects", "wall_mark");
		fn_multi_ptr = fn("MultiPtrArray<Object>::walk_data");
		step_lists = d.find_step(fn_multi_ptr, StepKind::Container);
	}
	int gid(const char* n) const { int g = d.find_global(n); if (g < 0) fail("global %s missing", n); return g; }
	int fn(const char* n) const { int f = d.find_function(n); if (f < 0) fail("function %s missing", n); return f; }
	int field(const char* c, const char* n) const { const FieldInfo* f = d.find_field(c, n); if (!f) fail("field %s::%s missing", c, n); return f->offset; }
	int32_t i32(Obj o, int64_t off) { const uint8_t* b = p.image(o, off, 4); return int32_t(b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24); }
	uint8_t u8(Obj o, int64_t off) { return *p.image(o, off, 1); }
	uint16_t u16(Obj o, int64_t off) { const uint8_t* b = p.image(o, off, 2); return uint16_t(b[0] | b[1] << 8); }
	bool leader_active(int who) { return (i32(leaders, int64_t(who) * 0x6eec) & 1) != 0; }
	// The container of a function's first Container step, on the object `o` at call offset `base`.
	Site site_of(const char* function, int64_t base, int nth = 0) {
		int f = fn(function);
		int s = d.find_step(f, StepKind::Container, -1, nth);
		if (s < 0) fail("%s has no container", function);
		return Site{s, base};
	}
	// Calls f(element, data_type) for each present element of the object list `who` whose index is in [lo, hi) and which is alive.
	template <class F> void each_object(int who, int64_t lo, int64_t hi, F f) {
		Site site{step_lists, int64_t(who) * 0x1c + 4};
		ContainerHeader h;
		if (!p.container(objects, site, h)) fail("objects list %d missing", who);
		if (hi > h.length) hi = h.length;
		for (int64_t o = lo; o < hi; ++o) {
			if (!p.present(objects, site, o)) continue;
			Obj e = p.element(objects, site, o);
			if (!e) continue;
			if (!(u8(e, 8) & 1)) continue;   // SubObject.flags bit 0 = alive
			f(e, p.data_type(objects, site, o));
		}
	}
	void walk_object(Obj e, int32_t dt) {
		int f = d.dispatch_function(step_lists, dt);
		if (f < 0) fail("no walk function for object data type %d", dt);
		w.walk(f, e, -1, 0);
	}
	// Generic PtrArray / Array elements of container `site` on `o`.
	template <class F> void each_element(Obj o, const Site& site, F f) {
		ContainerHeader h;
		if (!p.container(o, site, h)) fail("container at step %d missing", site.step);
		for (int64_t i = 0; i < h.length; ++i) {
			Site s2 = site;
			// `present` is only meaningful for PtrArray kinds; Array kinds have all elements.
			ContainerKind k = d.container_kind(site.step);
			bool ptr = k == ContainerKind::PtrArray || k == ContainerKind::MultiPtrArray;
			if (ptr && !p.present(o, s2, i)) continue;
			Obj e = p.element(o, s2, i);
			if (e) f(e, i);
		}
	}
};

uint32_t part_units(Layout& L, bool guys) {
	L.w.reset();
	if (L.i32(L.objects, L.off_valid)) {
		int32_t lo = L.i32(L.obj_base, 0);
		int fn_guys = L.fn("PtrArray<Guy>::walk_data");
		for (int who = 0; who < 9; ++who) {
			if (!L.leader_active(who)) continue;
			int32_t hi = L.i32(L.objects, L.off_unit_mark + 4 * who);
			L.each_object(who, lo, hi, [&](Obj e, int32_t dt) {
				if (guys) L.w.walk(fn_guys, e, -1, 0xe4);
				else L.walk_object(e, dt);
			});
		}
	}
	return L.w.value();
}

uint32_t part_objects(Layout& L, int kind) {  // 1 builds, 2 walls
	L.w.reset();
	if (L.i32(L.objects, L.off_valid)) {
		int32_t lo = L.i32(L.obj_base, 4 * kind);
		int off = kind == 1 ? L.off_build_mark : L.off_wall_mark;
		for (int who = 0; who < 8; ++who) {
			if (!L.leader_active(who)) continue;
			int32_t hi = L.i32(L.objects, off + 4 * who);
			L.each_object(who, lo, hi, [&](Obj e, int32_t dt) { L.walk_object(e, dt); });
		}
	}
	return L.w.value();
}

uint32_t part_ammo(Layout& L) {
	L.w.reset();
	if (L.i32(L.objects, L.off_valid)) {
		int fn = L.fn("AmmoData::walk_data");
		Site site = L.site_of("PtrArray<Ammo>::walk_data", L.field("Objects", "ammo_objs"));
		L.each_element(L.objects, site, [&](Obj e, int64_t) { if (L.u8(e, 4) & 3) L.w.walk(fn, e, -1, 0); });
	}
	return L.w.value();
}

uint32_t part_deaths(Layout& L) {
	L.w.reset();
	if (L.i32(L.objects, L.off_valid)) {
		int fn = L.fn("DeathObjData::walk_data");
		Site site = L.site_of("ObjectArray<DeathObj>::walk_data", L.field("Objects", "death_objs"));
		L.each_element(L.objects, site, [&](Obj e, int64_t) { if (L.i32(e, 0)) L.w.walk(fn, e, -1, 0); });
	}
	return L.w.value();
}

uint32_t part_groups(Layout& L) {
	L.w.reset();
	int fn = L.fn("Group::walk_data");
	Site site = L.site_of("Array<Group>::walk_data", 0);
	L.each_element(L.groups, site, [&](Obj e, int64_t) { L.w.walk(fn, e, -1, 0); });
	L.w.add_image(L.groups, 28, 32);   // last_group[0..7]
	return L.w.value();
}

uint32_t part_leaders(Layout& L) {
	L.w.reset();
	int fn = L.fn("LeaderData::walk_data");
	for (int who = 0; who < 8; ++who) L.w.walk(fn, L.leaders, L.g_leaders, int64_t(who) * 0x6eec);
	return L.w.value();
}

uint32_t part_cities(Layout& L) {
	L.w.reset();
	Obj cities = L.p.global(L.gid("cities"));
	int fn_city = L.fn("City::walk_data");
	int f_arr = L.fn("PtrArray<City>::walk_data");
	int step = L.d.find_step(f_arr, StepKind::Container);
	for (int who = 0; who < 8; ++who) {
		if (!L.leader_active(who)) continue;
		Site site{step, int64_t(who) * 0x1c};
		L.each_element(cities, site, [&](Obj e, int64_t) { if (L.u16(e, 4) & 1) L.w.walk(fn_city, e, -1, 0); });
	}
	return L.w.value();
}

uint32_t part_sub_objects(Layout& L, const char* global, const char* array_fn, const char* elem_fn) {  // items, goods
	L.w.reset();
	Obj o = L.p.global(L.gid(global));
	int fn = L.fn(elem_fn);
	Site site = L.site_of(array_fn, 0);
	L.each_element(o, site, [&](Obj e, int64_t) { if (L.u8(e, 8) & 1) L.w.walk(fn, e, -1, 0); });
	return L.w.value();
}

uint32_t part_world(Layout& L) {
	L.w.reset();
	if (L.i32(L.world, 8) > 0) L.w.walk("World::walk_data", L.world, L.g_world, 0, {{"part", -1}});
	return L.w.value();
}

uint32_t part_global(Layout& L, const char* fn, const char* global) {
	L.w.reset();
	int g = L.gid(global);
	L.w.walk(fn, L.p.global(g), g, 0);
	return L.w.value();
}

}  // namespace

CheckSumParts compute_check_sums(Provider& p) {
	CheckSumParts out;
	try {
		Layout L(Description::get(), p);
		out.v[0] = part_units(L, false);
		out.v[1] = part_objects(L, 1);
		out.v[2] = part_objects(L, 2);
		out.v[3] = part_ammo(L);
		out.v[4] = part_deaths(L);
		out.v[5] = part_groups(L);
		out.v[6] = part_units(L, true);
		out.v[7] = part_leaders(L);
		out.v[8] = part_cities(L);
		out.v[9] = part_sub_objects(L, "items", "PtrArray<Item>::walk_data", "Item::walk_data");
		out.v[10] = part_sub_objects(L, "goods", "PtrArray<Good>::walk_data", "Good::walk_data");
		out.v[11] = part_world(L);
		out.v[12] = part_global(L, "Game::walk_rules_data", "game");
		out.v[13] = part_global(L, "ScenarioData::walk_data", "scenario_data");
		out.v[14] = part_global(L, "RunTimeEnv::walk_data", "run_time_env");
	} catch (const std::exception& e) {
		out.ok = false;
		out.error = e.what();
	}
	return out;
}

// ------------------------------------------------------------------------------------------------ the verify block

std::vector<VerifyEntry> compute_verify(Provider& p, const std::vector<LocalValue>& stored) {
	std::vector<VerifyEntry> out;
	out.reserve(stored.size());
	std::unique_ptr<Layout> L;
	std::string setup_error;
	try { L.reset(new Layout(Description::get(), p)); } catch (const std::exception& e) { setup_error = e.what(); }
	std::map<std::string, int> counters;
	const Description& d = Description::get();
	const uint8_t* tdata = nullptr;
	const uint8_t* seen[3] = {nullptr, nullptr, nullptr};
	for (const LocalValue& lv : stored) {
		VerifyEntry e;
		e.name = lv.name;
		e.index = counters[lv.name]++;
		e.stored = uint32_t(lv.value);
		try {
			if (!L) fail("%s", setup_error.c_str());
			Layout& l = *L;
			const std::string n = lv.name;
			auto global_fn = [&](const char* fn, const char* g, int64_t base = 0, std::vector<std::pair<const char*, int64_t>> params = {}) {
				l.w.reset();
				int gid = l.gid(g);
				l.w.walk(fn, p.global(gid), gid, base, params);
				return l.w.value();
			};
			uint32_t v = 1;
			if (n == "game") v = global_fn("Game::walk_data", "game");
			else if (n == "tileset") v = global_fn("TileSet::walk_data", "tileset");
			else if (n == "mountains") v = global_fn("Mountains::walk_data", "mountains");
			else if (n == "empty_1" || n == "empty_2") v = 1;
			else if (n == "constants") v = global_fn("Constants::walk_data", "constants");
			else if (n == "game_daemon") { l.w.reset(); l.w.add_image(p.global(l.gid("game_daemon")), 0, 0x28); v = l.w.value(); }
			else if (n == "armies") v = global_fn("Armies::walk_data", "armies");
			else if (n == "cities") v = global_fn("Cities::walk_data", "cities");
			else if (n == "forms") v = global_fn("ObjectArray<Form>::walk_data", "forms");
			else if (n == "good") {
				l.w.reset();
				Obj goods = p.global(l.gid("goods"));
				Site site = l.site_of("PtrArray<Good>::walk_data", 0);
				ContainerHeader h;
				if (!p.container(goods, site, h)) fail("goods container missing");
				if (e.index < h.length && p.present(goods, site, e.index)) l.w.walk("Good::walk_data", p.element(goods, site, e.index), -1, 0);
				v = l.w.value();
			}
			else if (n == "goods_array") v = global_fn("PtrArray<Good>::walk_data", "goods");
			else if (n == "items_array") v = global_fn("PtrArray<Item>::walk_data", "items");
			else if (n == "regions") v = global_fn("Regions::walk_data", "regions");
			else if (n == "heroes") v = global_fn("Heroes::walk_data", "heroes");
			else if (n == "herds") v = global_fn("PtrArray<Herd>::walk_data", "herds");
			else if (n == "specials") v = global_fn("Specials::walk_data", "specials");
			else if (n == "wonders") v = global_fn("Wonders::walk_data", "wonders");
			else if (n == "forts") v = global_fn("Forts::walk_data", "forts");
			else if (n == "docks") v = global_fn("Docks::walk_data", "docks");
			else if (n == "oil_wells") v = global_fn("OilWells::walk_data", "oil_wells");
			else if (n == "supplies") v = global_fn("Supplies::walk_data", "supplies");
			else if (n == "caravans") v = global_fn("Caravans::walk_data", "caravans");
			else if (n == "lands") v = global_fn("ObjectArray<Land>::walk_data", "lands");
			else if (n == "leader") v = global_fn("LeaderData::walk_data", "leaders", int64_t(e.index) * 0x6eec);
			else if (n == "leaders_all") v = global_fn("Leaders::walk_data", "leaders");
			else if (n == "techtypes") v = global_fn("Types::walk_data", "types");
			else if (n == "leader_options") v = global_fn("LeaderOptions::walk_data", "leader_options");
			else if (n == "tribes") v = global_fn("ObjectArray<Tribe>::walk_data", "tribes", 4);
			else if (n == "option_info") v = global_fn("OptionInfo::walk_data", "option_info");
			else if (n == "pathfinder") { l.w.reset(); l.w.add_image(p.global(l.gid("pathfinder")), 0x58, 0xc4 - 0x58); v = l.w.value(); }
			else if (n == "groups") v = global_fn("Groups::walk_data", "groups");
			else if (n == "hotkey_groups") v = global_fn("Array<HotKeyGroup>::walk_data", "hotkey_groups");
			else if (n == "object_list") v = global_fn("MultiPtrArray<Object>::walk_data", "objects", int64_t(e.index) * 0x1c + 4);
			else if (n == "objects") v = global_fn("Objects::walk_data", "objects");
			else if (n == "wdata_cell") {
				l.w.reset();
				int f = l.fn("World::walk_data");
				int step = d.find_step(f, StepKind::Deref, 308, 0);
				Obj cell = p.child(l.world, Site{step, 0}, e.index);
				if (!cell) fail("no WData cell %d", e.index);
				l.w.add_image(cell, 0, 0x15);
				v = l.w.value();
			}
			else if (n == "tdata_tile" || n == "seen_cell") {
				l.w.reset();
				if (!tdata) {   // the World buffers are fetched once (each Buf step is one event)
					int f = l.fn("World::walk_data");
					p.begin_walk();
					auto buf = [&](int64_t ptr, uint64_t len) {
						const uint8_t* b = p.bytes(l.world, Site{d.find_step(f, StepKind::Buf, ptr, 0), 0}, len);
						if (!b) fail("no World buffer at +0x%llx", (unsigned long long)ptr);
						return b;
					};
					tdata = buf(312, uint64_t(l.i32(l.world, 0x20)) * 2);
					for (int k = 0; k < 3; ++k) seen[k] = buf(348 + 4 * k, uint64_t(l.i32(l.world, 0x14)));
				}
				if (n == "tdata_tile") {
					l.w.add(tdata + 2 * e.index, 2);
				} else {
					uint8_t three[3] = {seen[0][e.index], seen[1][e.index], seen[2][e.index]};
					l.w.add(three, 3);
				}
				v = l.w.value();
			}
			else if (n == "world") v = global_fn("World::walk_data", "world", 0, {{"part", -1}});
			else if (n == "roads") v = global_fn("Terrain::walk_roads", "terrain");
			else if (n == "graphic_events") v = global_fn("GraphicEvents::walk_data", "graphic_events");
			else if (n == "scene") v = global_fn("Scene::walk_data", "scene");
			else if (n == "message_win") v = global_fn("MessageWin::walk_data", "message_win");
			else if (n == "doober") v = global_fn("Doober::walk_data", "doobers");
			else if (n == "farms") v = global_fn("Farms::walk_data", "farms");
			else if (n == "unbuilt_wonders") v = global_fn("UnbuiltWonders::walk_data", "unbuilt_wonders");
			else if (n == "unbuilt_cities") v = global_fn("UnbuiltCities::walk_data", "unbuilt_cities");
			else if (n == "unbuilt_forts") v = global_fn("UnbuiltForts::walk_data", "unbuilt_forts");
			else if (n == "turn_control") v = global_fn("TurnControl::walk_data", "turn_control");
			else if (n == "units") v = part_units(l, false);
			else if (n == "builds") v = part_objects(l, 1);
			else if (n == "walls") v = part_objects(l, 2);
			else if (n == "guys") v = part_units(l, true);
			else if (n == "ammo") v = part_ammo(l);
			else if (n == "check_groups") v = part_groups(l);
			else if (n == "check_leaders") v = part_leaders(l);
			else if (n == "check_world") v = part_world(l);
			else if (n == "check_cities") v = part_cities(l);
			else if (n == "check_goods") v = part_sub_objects(l, "goods", "PtrArray<Good>::walk_data", "Good::walk_data");
			else if (n == "check_items") v = part_sub_objects(l, "items", "PtrArray<Item>::walk_data", "Item::walk_data");
			else if (n == "script_run_time") v = part_global(l, "RunTimeEnv::walk_data", "run_time_env");
			else fail("unknown verify entry %s", lv.name);
			e.computed = v;
			e.match = v == e.stored;
		} catch (const std::exception& ex) {
			e.error = ex.what();
		}
		out.push_back(std::move(e));
	}
	return out;
}

}  // namespace ron::sim::datawalk
