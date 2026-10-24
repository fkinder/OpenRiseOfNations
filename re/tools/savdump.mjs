#!/usr/bin/env node
// Generic DataWalk decoder for Rise of Nations saves, the embedded save in .rcx replays, and any other DataWalk stream
// (replay header, rules block). It interprets the machine-readable walk description re/iface/datawalk.json (built from
// the per-batch fragments in re/iface/datawalk/*.json) instead of hard-coding the format. Spec: docs/state-spec.md.
//
// Usage:
//   node re/tools/savdump.mjs --build                          merge re/iface/datawalk/*.json → re/iface/datawalk.json, validate
//   node re/tools/savdump.mjs --check                          validate the description only (refs, labels, expressions)
//   node re/tools/savdump.mjs --doc docs/state-walk.md         readable listing with PDB field names
//   node re/tools/savdump.mjs --synth | --synth-fill [--mode checksum]   run WalkDataGame over a synthetic stream (consistency)
//   node re/tools/savdump.mjs <file.sav>                       decode a save (gzip or plain): magic, version, WalkDataGame
//   node re/tools/savdump.mjs <file.sav> --summary            decode a save and print its header, world, leaders, object counts and
//                                                              world-grid completeness (no player names)
//   node re/tools/savdump.mjs <file.rcx|.bin> --replay         decode a replay's header walk; if it embeds a save, the save too;
//                                                              otherwise also the rules block (Game::walk_rules_data)
//   node re/tools/savdump.mjs <file> --fn "Game::walk_data" --at N [--obj game]   decode one function at byte offset N
// Options: --json out.json (full decoded tree)  --trace (log every step)  --max-depth N (tree print depth, default 3)
//          --mode load|checksum (expression mode; default load)  --version V (save_version for --fn, default 0x10)
//
// Decoding follows the loader's view (input=1, checksum=0, flags=-1). Tag bytes are verified against the labels, which is
// the main structural check: a wrong field size anywhere surfaces as a tag mismatch at the next section.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { loadPdbTypes } from './pdbtypes.mjs';
import { resolveLabel } from './walklabel.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const FRAG_DIR = path.join(ROOT, 're/iface/datawalk');
const MERGED = path.join(ROOT, 're/iface/datawalk.json');

// ------------------------------------------------------------------------------------------------ description

const num = v => (typeof v === 'number' ? v : typeof v === 'string' && /^-?(0x[0-9a-f]+|\d+)$/i.test(v.trim()) ? Number(v.trim()) : null);
const hex = (n, w = 0) => (n < 0 ? '-0x' + (-n).toString(16) : '0x' + n.toString(16).padStart(w, '0'));

function* walkSteps(steps, path = []) {
	for (let i = 0; i < (steps || []).length; i++) {
		const s = steps[i];
		yield [s, [...path, i]];
		for (const k of ['then', 'else', 'body', 'elem'])
			if (Array.isArray(s[k])) yield* walkSteps(s[k], [...path, i, k]);
		if (s.dispatch && typeof s.dispatch === 'object')
			for (const [t, d] of Object.entries(s.dispatch)) if (Array.isArray(d)) yield* walkSteps(d, [...path, i, 'dispatch', t]);
	}
}

export function buildDescription({ write = true, quiet = false } = {}) {
	const files = fs.readdirSync(FRAG_DIR).filter(f => f.endsWith('.json')).sort();
	const out = { format: 'ron-datawalk/1', generated: 'node re/tools/savdump.mjs --build', sources: files, walkers: WALKERS, containers: CONTAINER_DOC,
		functions: {}, globals: {} };
	const problems = [];
	for (const f of files) {
		let j;
		try { j = JSON.parse(fs.readFileSync(path.join(FRAG_DIR, f), 'utf8')); } catch (e) { problems.push(`${f}: JSON parse error: ${e.message}`); continue; }
		for (const [k, v] of Object.entries(j.functions || {})) {
			if (out.functions[k]) problems.push(`${f}: duplicate function ${k} (also in ${out.functions[k].source})`);
			out.functions[k] = { ...v, source: f };
		}
		if (j.checksums) out.checksums = j.checksums;
		if (j.container_kinds) Object.assign(out.containers, j.container_kinds);
		for (const [k, v] of Object.entries(j.globals || {})) {
			if (!out.globals[k]) out.globals[k] = { ...v, source: f };
		}
	}
	// Canonicalise function references: Ghidra spells template names with underscores ("unsigned_char", "Vector<float>_"),
	// translators sometimes with spaces. Rewrite every reference to the key that exists.
	const canon = n => n.replace(/[\s_]/g, '');
	const byCanon = new Map(Object.keys(out.functions).map(k => [canon(k), k]));
	for (const fn of Object.values(out.functions))
		for (const [s] of walkSteps(fn.body)) {
			if (s.op === 'call' && s.fn && !out.functions[s.fn] && byCanon.has(canon(s.fn))) s.fn = byCanon.get(canon(s.fn));
			if (s.dispatch && typeof s.dispatch === 'object')
				for (const [t, d] of Object.entries(s.dispatch)) {
					if (typeof d === 'string' && !out.functions[d] && byCanon.has(canon(d))) s.dispatch[t] = byCanon.get(canon(d));
					else if (d && typeof d === 'object' && d.fn && !out.functions[d.fn] && byCanon.has(canon(d.fn))) d.fn = byCanon.get(canon(d.fn));
				}
		}
	// Resolve labels, check references and expressions.
	let P = null;
	try { P = loadPdbTypes(path.join(ROOT, 're/bin/rise.pdb')); } catch { /* layouts optional */ }
	const labels = {};
	for (const [name, fn] of Object.entries(out.functions)) {
		if (fn.this && P && P.find(fn.this) == null) problems.push(`${name}: unknown class ${fn.this}`);
		for (const [s, where] of walkSteps(fn.body)) {
			const at = `${name} @${where.join('.')}`;
			if (!s.op) { problems.push(`${at}: step without op`); continue; }
			if (s.op === 'tag') {
				try {
					const lab = typeof s.label === 'object' && s.label ? s.label.literal : s.label;
					const r = resolveLabel(lab);
					s.text = r.label; s.byte = r.byte;
					labels[r.label] = r.byte;
				} catch (e) { problems.push(`${at}: ${e.message}`); }
			}
			if (s.op === 'call' && s.fn && !out.functions[s.fn]) problems.push(`${at}: call to undescribed ${s.fn}`);
			if (s.obj && !out.globals[s.obj]) problems.push(`${at}: undeclared global ${s.obj}`);
			if (s.op === 'container' && s.dispatch && typeof s.dispatch === 'object')
				for (const d of Object.values(s.dispatch)) if (typeof d === 'string' && !out.functions[d]) problems.push(`${at}: dispatch to undescribed ${d}`);
			for (const k of ['cond', 'count', 'len'])
				if (typeof s[k] === 'string') { try { compile(s[k]); } catch (e) { problems.push(`${at}: bad ${k} expression ${JSON.stringify(s[k])}: ${e.message}`); } }
		}
	}
	out.labels = labels;
	out.problems = problems;
	out.coverage = coverage(out);
	if (write) fs.writeFileSync(MERGED, JSON.stringify(out, null, 1));
	if (!quiet) {
		console.log(`${Object.keys(out.functions).length} functions, ${Object.keys(out.globals).length} globals from ${files.join(', ')}`);
		console.log(`coverage: ${out.coverage.described}/${out.coverage.total} walk functions described; missing ${out.coverage.missing.length}: ${out.coverage.missing.slice(0, 30).join(', ')}${out.coverage.missing.length > 30 ? ', …' : ''}`);
		if (problems.length) console.log(`${problems.length} problems:\n  ` + problems.join('\n  '));
		if (write) console.log(`wrote ${path.relative(ROOT, MERGED)}`);
	}
	return out;
}

function coverage(desc) {
	// Every walk function in the symbol index (walk_data*, walk_rules_data, walk_* helpers) except the visitors themselves.
	// Identical-COMDAT folding gives several names one address (e.g. the trivial *Order::walk_data bodies), so count addresses.
	const tsv = fs.readFileSync(path.join(ROOT, 're/map/functions.tsv'), 'utf8').split('\n');
	const byAddr = new Map();
	for (const line of tsv) {
		const c = line.split('\t');
		if (c.length < 5 || !/^walk_|walk_data/.test(c[3]) || /unwind|ehhandler/.test(c[4])) continue;
		if (/^(SaveGame|LoadGame|CheckSum|StripBuilder)$/.test(c[2])) continue;
		const k = c[0].toLowerCase();
		if (!byAddr.has(k)) byAddr.set(k, []);
		byAddr.get(k).push(c[2] + '::' + c[3]);
	}
	const addrs = new Set(Object.values(desc.functions).map(f => (f.addr || '').toLowerCase().replace(/^0x/, '').padStart(8, '0')));
	const missing = [];
	let described = 0;
	for (const [k, names] of byAddr) { if (addrs.has(k)) described++; else missing.push(names[0] + '@' + k); }
	return { total: byAddr.size, described, missing, names: Object.keys(desc.functions).length };
}

export const WALKERS = {
	SaveGame: { input: 0, checksum: 0, flags: -1, walk_function: 'fwrite/gzwrite [a,b)', walk_test: 'writes low byte of label hash_value_insensitive' },
	LoadGame: { input: 1, checksum: 0, flags: -1, walk_function: 'fread/gzread into [a,b)', walk_test: 'reads 1 byte, error "Error loading section" on mismatch' },
	CheckSum: { input: 0, checksum: 1, flags: -1, walk_function: 'accum = adler32(accum, a, b-a)', walk_test: 'no-op' },
};
const CONTAINER_DOC = {
	Array: 'i32 length; if length: i32 size, u16 increment (raw +0xc..+0xe), u8 flags&~0x40; then elem steps for each element (stride)',
	ObjectArray: 'same as Array', NamedObjectArray: 'same as Array (names are walked by the element steps if at all)',
	SimpleArray: 'Array header; then one raw block of length*stride bytes',
	PtrArray: 'Array header (length; if length: size, increment, flags); length x u8 present; raw +0x8..+0xe (size, increment); elem steps for each present element (heap object)',
	MultiPtrArray: 'PtrArray header + presence bytes; for each present element i32 data type; raw +0x8..+0xe; each present element walk_data by type (dispatch)',
	Stack: 'raw size/length/increment header as described; then length x raw elements (stride)',
};

// ------------------------------------------------------------------------------------------------ expressions

const exprCache = new Map();
function compile(src) {
	if (exprCache.has(src)) return exprCache.get(src);
	// eslint-disable-next-line no-new-func
	const f = new Function('env', `with (env) { return (${src}); }`);
	exprCache.set(src, f);
	return f;
}

class Stop extends Error {}

// Values the loader takes from game data or memory rather than from the stream (the `rt("...")` expressions of the description).
// They are constants of one game build: validated by decoding every save (docs/state-spec.md section 8).
export const RUNTIME_VALUES = {
	// GraphicEvents.events.length = graphic_pieces->first_ammo_piece + ammo_names.length (GraphicEvents::init @8e5390), the
	// number of EventGroup slots; not in the stream, one presence byte per slot. Found by decoding: 60415 in every save.
	'GraphicEvents.events.length': 60415,
};

// ------------------------------------------------------------------------------------------------ records

// A record is one object's memory image, filled by raw walks at their offsets (like the loader's memory).
class Rec {
	constructor(cls, size, label) {
		this.cls = cls; this.label = label;
		this.origin = 0;     // index in bytes[] of offset 0 (grows when a thunk walks a subobject's enclosing object)
		this.bytes = Buffer.alloc(Math.max(size || 0, 0));
		this.mask = new Uint8Array(this.bytes.length);
		this.kids = [];
		this.raws = [];
	}
	ensure(lo, hi) {
		if (lo + this.origin < 0) {
			const add = -(lo + this.origin);
			const b = Buffer.alloc(this.bytes.length + add); this.bytes.copy(b, add); this.bytes = b;
			const m = new Uint8Array(b.length); m.set(this.mask, add); this.mask = m;
			this.origin += add;
		}
		if (hi + this.origin > this.bytes.length) {
			const n = hi + this.origin;
			const b = Buffer.alloc(n); this.bytes.copy(b); this.bytes = b;
			const m = new Uint8Array(n); m.set(this.mask); this.mask = m;
		}
	}
	write(off, buf) { this.ensure(off, off + buf.length); buf.copy(this.bytes, off + this.origin); this.mask.fill(1, off + this.origin, off + this.origin + buf.length); }
	read(off, n, signed, float) {
		const o = off + this.origin;
		if (o + n > this.bytes.length || o < 0) throw new Error(`read ${n}@${hex(off)} outside ${this.cls || '?'} record (${hex(this.bytes.length)})`);
		if (float) return this.bytes.readFloatLE(o);
		if (n === 1) return signed ? this.bytes.readInt8(o) : this.bytes[o];
		if (n === 2) return signed ? this.bytes.readInt16LE(o) : this.bytes.readUInt16LE(o);
		return signed ? this.bytes.readInt32LE(o) : this.bytes.readUInt32LE(o);
	}
	slice(off, n) { return this.bytes.subarray(off + this.origin, off + this.origin + n); }
}

// ------------------------------------------------------------------------------------------------ decoder

export class Decoder {
	constructor(desc, buf, pos = 0, opts = {}) {
		this.d = desc; this.b = buf; this.p = pos; this.opts = opts;
		this.walker = opts.mode === 'checksum' ? { input: 0, checksum: 1, flags: -1 } : { input: 1, checksum: 0, flags: -1 };
		this.globals = {};
		this.P = opts.pdb || null;
		this.layoutCache = new Map();
		this.depth = 0;
		this.lengths = {};   // container length by global name (the last container walked on that global), for LEN("name")
		this.saveVersion = opts.saveVersion ?? 0x10;
	}
	layout(cls) {
		if (!cls || !this.P) return null;
		if (!this.layoutCache.has(cls)) {
			const L = this.P.layout(cls);
			this.layoutCache.set(cls, L ? { size: L.size, flat: this.P.flat(L) } : null);
		}
		return this.layoutCache.get(cls);
	}
	sizeOf(cls, fallback = 0) { const L = this.layout(cls); return L ? L.size : fallback; }
	global(name) {
		if (!this.globals[name]) {
			const g = this.d.globals[name];
			if (!g) throw new Error(`undeclared global ${name}`);
			const size = num(g.size) ?? this.sizeOf(g.class, 0);
			this.globals[name] = new Rec(g.class, size, name);
		}
		return this.globals[name];
	}
	take(n, what) {
		if (this.opts.synth) {   // synthetic stream: zeros everywhere (tags are supplied by the tag step)
			if (n < 0 || !Number.isFinite(n) || n > 1e8) throw new Error(`bad length ${n} for ${what}`);
			this.p += n; return Buffer.alloc(n);
		}
		if (n < 0 || !Number.isFinite(n)) throw new Error(`bad length ${n} for ${what}`);
		if (this.p + n > this.b.length) throw new Stop(`end of data at ${this.p} reading ${n} bytes for ${what}`);
		const v = this.b.subarray(this.p, this.p + n); this.p += n; return v;
	}
	// --synth-fill: report 1 for counts, presence flags and locals (bounded by depth) so nested walks get exercised.
	fill(n) {
		const b = Buffer.alloc(n);
		if (this.opts.synthFill && this.depth < (this.opts.fillDepth ?? 14)) b[0] = 1;
		return b;
	}
	trace(ctx, msg) { if (this.opts.trace) console.log(`${String(this.p).padStart(9)} ${'  '.repeat(this.depth)}${msg}`); }

	env(ctx) {
		const rd = (signed, n, float) => o => ctx.rec.read(ctx.base + this.ev(ctx, o), n, signed, float);
		const acc = c => ({
			i8: o => c.rec.read(c.base + o, 1, true), u8: o => c.rec.read(c.base + o, 1, false),
			i16: o => c.rec.read(c.base + o, 2, true), u16: o => c.rec.read(c.base + o, 2, false),
			i32: o => c.rec.read(c.base + o, 4, true), u32: o => c.rec.read(c.base + o, 4, false),
			f32: o => c.rec.read(c.base + o, 4, false, true),
		});
		const e = {
			...this.walker, L: ctx.L, P: ctx.params, ...ctx.params, ...ctx.vars,
			i8: rd(true, 1), u8: rd(false, 1), i16: rd(true, 2), u16: rd(false, 2), i32: rd(true, 4), u32: rd(false, 4), f32: rd(false, 4, true),
			up: ctx.parent ? acc(ctx.parent) : null,
			G: name => acc({ rec: this.global(name), base: 0 }),
			save_version: this.saveVersion,
			LEN: name => this.lengths[name] ?? 0,
			rt: t => {
				if (this.opts.rtValues && t in this.opts.rtValues) return this.opts.rtValues[t];
				if (t in RUNTIME_VALUES) return this.opts.synth ? 1 : RUNTIME_VALUES[t];
				throw new Error(`runtime value needed by the loader: ${t}`);
			},
		};
		return e;
	}
	ev(ctx, v) {
		const n = num(v);
		if (n !== null) return n;
		if (typeof v !== 'string') throw new Error(`bad value ${JSON.stringify(v)}`);
		const r = compile(v)(this.env(ctx));
		return typeof r === 'boolean' ? (r ? 1 : 0) : r;
	}
	stepCtx(ctx, s) {
		if (s.obj) return { rec: this.global(s.obj), base: 0, cls: this.d.globals[s.obj].class, L: ctx.L, params: ctx.params, vars: ctx.vars, parent: ctx.parent };
		return ctx;
	}

	run(steps, ctx, node) {
		for (const s of steps || []) this.step(s, ctx, node);
	}
	step(s, ctx0, node) {
		const ctx = this.stepCtx(ctx0, s);
		switch (s.op) {
			case 'tag': {
				if (this.walker.checksum) { node.items.push({ tag: s.text, hashed: false }); return; }   // CheckSum::walk_test is a no-op
				const b = this.opts.synth ? (this.p++, s.byte) : this.take(1, 'tag')[0];
				const ok = s.byte === undefined || b === s.byte;
				this.trace(ctx, `tag ${s.text} = 0x${b.toString(16)}${ok ? '' : ' MISMATCH, expected 0x' + s.byte.toString(16)}`);
				node.items.push({ tag: s.text, at: this.p - 1, ok });
				if (!ok) throw new Stop(`tag "${s.text}" mismatch at ${this.p - 1}: 0x${b.toString(16)} != 0x${s.byte.toString(16)} (${node.fn})`);
				return;
			}
			case 'raw': {
				const at = this.ev(ctx, s.at ?? 0), end = this.ev(ctx, s.end);
				const n = end - at;
				const data = this.take(n, `raw ${hex(at)}..${hex(end)} in ${node.fn}`);
				ctx.rec.write(ctx.base + at, data);
				node.items.push({ raw: [ctx.base + at, n], obj: s.obj, pos: this.p - n });
				this.trace(ctx, `raw ${s.obj ? s.obj + '+' : ''}${hex(at)}..${hex(end)} (${n})`);
				return;
			}
			case 'local': {
				const n = { u8: 1, i8: 1, u16: 2, i16: 2, u32: 4, i32: 4, f32: 4 }[s.type || 'u32'];
				const b = this.opts.synth ? (this.p += n, this.fill(n)) : this.take(n, `local ${s.name}`);
				const t = s.type || 'u32';
				const v = t === 'f32' ? b.readFloatLE(0) : n === 1 ? (t === 'i8' ? b.readInt8(0) : b[0]) : n === 2 ? (t === 'i16' ? b.readInt16LE(0) : b.readUInt16LE(0)) : (t === 'i32' ? b.readInt32LE(0) : b.readUInt32LE(0));
				ctx.L[s.name] = v;
				node.items.push({ local: s.name, value: v });
				this.trace(ctx, `local ${s.name} = ${v}`);
				return;
			}
			case 'string': {
				const n = this.take(4, 'string length').readUInt32LE(0);
				if (n > 0x100000) throw new Stop(`implausible string length ${n} at ${this.p - 4} in ${node.fn}`);
				const str = this.take(2 * n, 'string').toString('utf16le');
				node.items.push({ string: str, at: ctx.base + this.ev(ctx, s.at ?? 0) });
				this.trace(ctx, `string ${JSON.stringify(str)}`);
				return;
			}
			case 'bitmask': {
				const at = this.ev(ctx, s.at ?? 0);
				const hdr = this.take(8, 'bitmask header');
				ctx.rec.write(ctx.base + at, hdr);
				const size = hdr.readInt32LE(4);
				const bits = this.take(size, 'bitmask bits');
				ctx.rec.write(ctx.base + at + 0xc, bits);
				node.items.push({ bitmask: ctx.base + at, bits: hdr.readInt32LE(0), size, hex: bits.toString('hex') });
				this.trace(ctx, `bitmask ${hex(at)} bits=${hdr.readInt32LE(0)} size=${size}`);
				return;
			}
			case 'buf': {
				const len = this.ev(ctx, s.len);
				const data = this.take(len, `buf in ${node.fn}`);
				node.items.push({ buf: s.ptr, elem: s.elem, len, pos: this.p - len, value: fmtBuf(data, s.elem) });
				this.trace(ctx, `buf *${s.ptr} (${len})`);
				return;
			}
			case 'call': {
				const fn = this.d.functions[s.fn];
				if (!fn) throw new Error(`call to undescribed ${s.fn}`);
				const at = this.ev(ctx, s.at ?? 0);
				const params = {};
				for (const [k, v] of Object.entries(s.args || {})) params[k] = this.ev(ctx, v);
				const child = this.invoke(s.fn, { rec: ctx.rec, base: ctx.base + at, cls: fn.this, parent: ctx.parent }, params);
				for (const [k, v] of Object.entries(child.L)) ctx.L[k] = v;
				node.items.push({ call: child.node });
				return;
			}
			case 'if': {
				const c = this.ev(ctx, s.cond);
				this.trace(ctx, `if ${s.cond} → ${!!c}`);
				this.run(c ? s.then : s.else, ctx, node);
				return;
			}
			case 'loop': {
				const n = this.ev(ctx, s.count);
				const v = s.var || 'i';
				for (let i = 0; i < n; i++) {
					const vars = { ...ctx.vars, [v]: i };
					this.run(s.body, { ...ctx, vars }, node);
				}
				return;
			}
			case 'deref': {
				const idx = this.ev(ctx, s.index ?? 0);
				const stride = this.ev(ctx, s.stride ?? 0);
				const rec = new Rec(s.class, this.sizeOf(s.class, stride), `*${s.ptr}[${idx}]`);
				const sub = { fn: `*(${hex(this.ev(ctx, s.ptr))})[${idx}] ${s.class || ''}`, cls: s.class, items: [], rec };
				node.items.push({ deref: sub });
				this.depth++;
				this.run(s.body, { rec, base: 0, cls: s.class, L: ctx.L, params: ctx.params, vars: ctx.vars, parent: ctx }, sub);
				this.depth--;
				return;
			}
			case 'container': return this.container(s, ctx, node);
			case 'note': return;
			default: throw new Error(`unknown op ${s.op} in ${node.fn}`);
		}
	}
	invoke(name, ctx, params = {}) {
		const fn = this.d.functions[name];
		const node = { fn: name, cls: fn.this, base: ctx.base, items: [], rec: ctx.rec };
		const c = { rec: ctx.rec, base: ctx.base, cls: fn.this, L: {}, params, vars: {}, parent: ctx.parent };
		this.trace(c, `${name}`);
		this.depth++;
		try { this.run(fn.body, c, node); } finally { this.depth--; }
		return { node, L: c.L };
	}
	elemRec(cls, stride, label) { return new Rec(cls, this.sizeOf(cls, stride) || stride, label); }
	runElem(s, rec, ctx, sub, typeFn) {
		const ectx = { rec, base: 0, cls: s.class, L: {}, params: {}, vars: ctx.vars, parent: ctx };
		if (typeFn) {
			const r = this.invoke(typeFn, ectx);
			sub.items.push({ call: r.node });
		} else this.run(s.elem, ectx, sub);
	}
	container(s, ctx, node) {
		const at = this.ev(ctx, s.at ?? 0);
		const base = ctx.base + at;
		const kind = s.kind;
		const stride = this.ev(ctx, s.stride ?? 0);
		const out = { container: kind, class: s.class, elements: [] };
		node.items.push(out);
		const i32 = what => (this.opts.synth && /length|size|count/.test(what) ? (this.p += 4, this.fill(4)) : this.take(4, what)).readInt32LE(0);
		const header = () => {
			const length = i32(`${kind} length`);
			out.length = length;
			if (ctx.rec.label && !ctx.base) this.lengths[ctx.rec.label] = length;
			if (!length) return 0;
			out.size = i32(`${kind} size`);
			out.increment = this.take(2, 'increment').readInt16LE(0);
			out.flags = this.take(1, 'flags')[0];
			if (out.size < length || length < 0 || length > 1e7) throw new Stop(`implausible ${kind} length ${length} size ${out.size} at ${this.p} (${node.fn})`);
			return length;
		};
		this.trace(ctx, `container ${kind}<${s.class || s.elemType || ''}> @${hex(base)}`);
		this.depth++;
		try {
			switch (kind) {
				case 'Array': case 'ObjectArray': case 'NamedObjectArray': {
					const n = header();
					for (let i = 0; i < n; i++) {
						const rec = this.elemRec(s.class, stride, `[${i}]`);
						const sub = { fn: `[${i}]`, cls: s.class, items: [], rec };
						this.runElem(s, rec, ctx, sub, null);
						out.elements.push(sub);
					}
					break;
				}
				case 'SimpleArray': {
					const n = header();
					if (n) {
						const data = this.take(n * stride, 'SimpleArray elements');
						out.values = fmtBuf(data, s.elemType || (stride === 4 ? 'i32' : stride === 2 ? 'u16' : stride === 1 ? 'u8' : 'bytes'), stride);
					}
					break;
				}
				case 'PtrArray': case 'MultiPtrArray': {
					const n = header();
					if (!n) break;
					const present = this.opts.synth ? (this.p += n, [...this.fill(n)].map(() => (this.opts.synthFill && this.depth < (this.opts.fillDepth ?? 14) ? 1 : 0))) : [...this.take(n, 'presence bytes')];
					const types = [];
					if (kind === 'MultiPtrArray') for (let i = 0; i < n; i++) types.push(present[i] ? i32('data type') : null);
					const tail = this.take(6, 'size+increment');
					out.size2 = tail.readInt32LE(0);
					for (let i = 0; i < n; i++) {
						if (!present[i]) { out.elements.push(null); continue; }
						let fnName = null, cls = s.class;
						if (kind === 'MultiPtrArray') {
							const d = s.dispatch && (s.dispatch[types[i]] ?? s.dispatch.default);
							if (!d) throw new Stop(`no dispatch for data type ${types[i]} (${node.fn})`);
							if (typeof d === 'string') { fnName = d; cls = this.d.functions[d]?.this; }
							else if (d.fn) { fnName = d.fn; cls = d.class || this.d.functions[d.fn]?.this; }
						}
						const rec = this.elemRec(cls, stride, `[${i}]`);
						const sub = { fn: `[${i}]${types[i] != null ? ' type ' + types[i] : ''}`, cls, items: [], rec };
						if (fnName) { const r = this.invoke(fnName, { rec, base: 0, parent: ctx }); sub.items.push({ call: r.node }); }
						else this.runElem({ ...s, class: cls }, rec, ctx, sub, null);
						out.elements.push(sub);
					}
					break;
				}
				case 'LinkList': case 'PtrLinkListAbstract': case 'OrderList': {
					// Linked lists: a count (from `header` steps, a `count` expression, or else an i32 read here), then per
					// node the `node` steps (if any) and the `elem` steps. PtrLinkList/OrderList nodes are written tail first
					// and re-inserted at the head on load, so file order = reverse list order for those kinds.
					const L = ctx.L;
					if (s.header) this.run(s.header, { ...ctx, base }, node);
					let n;
					const m = typeof s.count === 'string' && s.count.match(/^L\.(\w+)$/);
					if (m && L[m[1]] === undefined) { L[m[1]] = i32(`${kind} count`); }
					if (s.count !== undefined) n = this.ev({ ...ctx, base }, s.count);
					else if (s.header) n = L.count ?? L.length;
					else n = i32(`${kind} count`);
					out.length = n;
					if (n < 0 || n > 1e7) throw new Stop(`implausible ${kind} count ${n} at ${this.p} (${node.fn})`);
					for (let i = 0; i < n; i++) {
						const rec = new Rec(s.class, Math.max(this.sizeOf(s.class, 0x40), 0x40), `node ${i}`);
						const sub = { fn: `node ${i}`, cls: s.class, items: [], rec };
						const ectx = { rec, base: 0, cls: s.class, L, params: ctx.params, vars: { ...ctx.vars, i }, parent: ctx };
						if (s.node) this.run(s.node, ectx, sub);
						this.run(s.elem, ectx, sub);
						out.elements.push(sub);
					}
					break;
				}
				default: {
					// Families without a fixed shape (linked lists, stacks) must spell out their bytes in `elem`/`body`.
					if (s.body) { this.run(s.body, { ...ctx, base }, node); break; }
					throw new Error(`container kind ${kind} not implemented in savdump (${node.fn}); spell it out with steps`);
				}
			}
		} finally { this.depth--; }
	}
}

function fmtBuf(data, elem, stride) {
	const n = data.length;
	const take = (k, f) => { const a = []; for (let o = 0; o + k <= n; o += k) a.push(f(o)); return a; };
	switch (elem) {
		case 'u8': return [...data];
		case 'i8': return take(1, o => data.readInt8(o));
		case 'u16': return n <= 4096 ? take(2, o => data.readUInt16LE(o)) : `${n / 2} x u16`;
		case 'i16': return n <= 4096 ? take(2, o => data.readInt16LE(o)) : `${n / 2} x i16`;
		case 'u32': return n <= 8192 ? take(4, o => data.readUInt32LE(o)) : `${n / 4} x u32`;
		case 'i32': case 'int': case 'Coord': case 'WCoord': return n <= 8192 ? take(4, o => data.readInt32LE(o)) : `${n / 4} x i32`;
		case 'f32': case 'float': return n <= 8192 ? take(4, o => data.readFloatLE(o)) : `${n / 4} x f32`;
		default: return n <= 256 ? data.toString('hex') : `${n} bytes`;
	}
}

// Name the bytes of a record using the PDB layout of its class.
function nameFields(dec, rec, base, cls, ranges) {
	const L = dec.layout(cls);
	const out = {};
	if (!L) { for (const [o, n] of ranges) out[hex(o - base)] = rec.slice(o, n).toString('hex'); return out; }
	for (const [o, n] of ranges) {
		const lo = o - base, hi = lo + n;
		let covered = lo;
		for (const f of L.flat) {
			if (f.offset < lo || f.offset >= hi || f.type === 'vfptr') continue;
			const fo = base + f.offset, size = Math.min(f.size, hi - f.offset);
			out[f.name] = fieldValue(rec, fo, size, f.type);
			covered = Math.max(covered, f.offset + f.size);
		}
		if (!Object.keys(out).length) out[hex(lo)] = rec.slice(o, n).toString('hex');
	}
	return out;
}
function fieldValue(rec, o, size, type) {
	const b = rec.bytes;
	o += rec.origin;
	if (o + size > b.length) return null;
	const t = type.replace(/^const /, '');
	if (/\*$/.test(t)) return size === 4 ? hex(b.readUInt32LE(o)) : b.subarray(o, o + size).toString('hex');
	if (t === 'float' && size === 4) return +b.readFloatLE(o).toPrecision(7);
	if (t === 'double' && size === 8) return b.readDoubleLE(o);
	if (/^(int|long|Coord|WCoord)$/.test(t) && size === 4) return b.readInt32LE(o);
	if (/^(uint|ulong)$/.test(t) && size === 4) return b.readUInt32LE(o);
	if (t === 'short' && size === 2) return b.readInt16LE(o);
	if ((t === 'ushort' || t === 'wchar_t') && size === 2) return b.readUInt16LE(o);
	if ((t === 'char' || t === 'int8') && size === 1) return b.readInt8(o);
	if ((t === 'uchar' || t === 'bool' || t === 'uint8') && size === 1) return b[o];
	const arr = t.match(/^(int|uint|short|ushort|char|uchar|float|ulong|long|Coord)\[(\d+)\]$/);
	if (arr) {
		const es = { int: 4, uint: 4, ulong: 4, long: 4, Coord: 4, float: 4, short: 2, ushort: 2, char: 1, uchar: 1 }[arr[1]];
		const a = [];
		for (let k = 0; k + es <= size; k += es) a.push(fieldValue(rec, o + k - rec.origin, es, arr[1]));
		return a;
	}
	if (size === 4) return b.readInt32LE(o);    // enums, small structs
	return b.subarray(o, o + size).toString('hex');
}

// Convert the decode tree into plain JSON with named fields.
function render(dec, node, depthLeft = 1e9) {
	const out = { fn: node.fn };
	if (node.cls) out.class = node.cls;
	const items = [];
	for (const it of node.items) {
		if (it.raw) {
			const rec = it.obj ? dec.globals[it.obj] : node.rec;
			const cls = it.obj ? dec.d.globals[it.obj].class : node.cls;
			const base = it.obj ? 0 : (node.base || 0);
			items.push({ fields: nameFields(dec, rec, base, cls, [it.raw]), ...(it.obj ? { obj: it.obj } : {}), pos: it.pos });
		} else if (it.call) items.push(depthLeft > 0 ? render(dec, it.call, depthLeft - 1) : { fn: it.call.fn });
		else if (it.deref) items.push(render(dec, it.deref, depthLeft - 1));
		else if (it.container) items.push({ container: it.container, class: it.class, length: it.length, ...(it.values ? { values: it.values } : {}),
			elements: depthLeft > 0 ? it.elements.map(e => (e ? render(dec, e, depthLeft - 1) : null)) : `${it.elements.length} elements` });
		else items.push(it);
	}
	out.items = items;
	return out;
}

function printTree(t, indent, maxDepth, depth = 0) {
	const pad = '  '.repeat(indent);
	if (depth > maxDepth) return;
	console.log(`${pad}${t.fn}${t.class ? ' <' + t.class + '>' : ''}`);
	for (const it of t.items || []) {
		if (it.fields) {
			const s = Object.entries(it.fields).map(([k, v]) => `${k}=${Array.isArray(v) ? '[' + v.slice(0, 8).join(',') + (v.length > 8 ? ',…' : '') + ']' : v}`).join(' ');
			console.log(`${pad}  ${it.obj ? it.obj + ': ' : ''}${s.length > 200 ? s.slice(0, 200) + '…' : s}`);
		} else if (it.tag) console.log(`${pad}  [${it.tag}]`);
		else if (it.string !== undefined) console.log(`${pad}  "${it.string}"`);
		else if (it.local) console.log(`${pad}  ${it.local}=${it.value}`);
		else if (it.container) {
			console.log(`${pad}  ${it.container}<${it.class || ''}> length=${it.length ?? 0}${it.values ? ' ' + JSON.stringify(it.values).slice(0, 120) : ''}`);
			if (Array.isArray(it.elements) && depth < maxDepth) it.elements.slice(0, 3).forEach(e => e && printTree(e, indent + 2, maxDepth, depth + 1));
		} else if (it.fn) printTree(it, indent + 1, maxDepth, depth + 1);
		else if (it.bitmask !== undefined) console.log(`${pad}  bitmask size=${it.size} ${it.hex.slice(0, 64)}`);
		else if (it.buf !== undefined) console.log(`${pad}  buf ${it.len} bytes ${JSON.stringify(it.value).slice(0, 100)}`);
	}
}


// ------------------------------------------------------------------------------------------------ readable listing

// Render the description as indented pseudo-code with PDB field names (used for docs/state-walk.md).
export function docgen(outFile) {
	if (!fs.existsSync(MERGED)) buildDescription({ quiet: true });
	const desc = JSON.parse(fs.readFileSync(MERGED, 'utf8'));
	const P = loadPdbTypes(path.join(ROOT, 're/bin/rise.pdb'));
	const flatCache = new Map();
	const flat = cls => {
		if (!cls) return null;
		if (!flatCache.has(cls)) { const L = P.layout(cls); flatCache.set(cls, L ? P.flat(L) : null); }
		return flatCache.get(cls);
	};
	const fieldsIn = (cls, lo, hi) => {
		const F = flat(cls);
		if (!F || lo === null || hi === null) return '';
		const names = [];
		for (const x of F) if (x.offset >= lo && x.offset < hi && x.type !== 'vfptr') names.push(x.name);
		const uniq = [...new Set(names)];
		return uniq.length ? (uniq.length > 12 ? uniq.slice(0, 5).join(', ') + ` … (${uniq.length} fields) … ` + uniq.slice(-3).join(', ') : uniq.join(', ')) : '';
	};
	const fieldAt = (cls, o) => { const F = flat(cls); const x = F && F.find(f => f.offset === o && f.type !== 'vfptr'); return x ? x.name : null; };
	const lines = [];
	const v = x => (typeof x === 'number' ? hex(x) : String(x));
	const emit = (steps, cls, ind) => {
		for (const s of steps || []) {
			const pad = '  '.repeat(ind);
			const ccls = s.obj ? desc.globals[s.obj]?.class : cls;
			const on = s.obj ? s.obj + '.' : '';
			const why = s.why ? `   // ${s.why}` : '';
			switch (s.op) {
				case 'tag': lines.push(`${pad}tag "${s.text}" (0x${(s.byte ?? 0).toString(16).padStart(2, '0')})`); break;
				case 'raw': {
					const lo = num(s.at ?? 0), hi = num(s.end);
					const n = lo !== null && hi !== null ? hi - lo : null;
					const fl = fieldsIn(ccls, lo, hi);
					lines.push(`${pad}raw ${on}[${v(s.at ?? 0)}, ${v(s.end)})${n !== null ? ' ' + n + ' bytes' : ''}${fl ? ': ' + fl : ''}${why}`);
					break;
				}
				case 'local': lines.push(`${pad}${s.type || 'u32'} ${s.name}   (writer stores: ${s.save})${why}`); break;
				case 'string': lines.push(`${pad}String ${on}${fieldAt(ccls, num(s.at ?? 0)) || v(s.at ?? 0)}${why}`); break;
				case 'bitmask': lines.push(`${pad}BitMask ${on}${fieldAt(ccls, num(s.at ?? 0)) || v(s.at ?? 0)}: i32 bits, i32 size, size bytes${why}`); break;
				case 'buf': lines.push(`${pad}buffer *${on}${fieldAt(ccls, num(s.ptr)) || v(s.ptr)}: ${s.len} bytes${s.elem ? ' of ' + s.elem : ''}${why}`); break;
				case 'call': {
					const at = num(s.at ?? 0);
					const fname = at === null ? '(' + s.at + ')' : at ? (fieldAt(ccls, at) || v(s.at)) : '';
					const args = s.args ? '(' + Object.entries(s.args).map(([k, x]) => k + '=' + x).join(', ') + ')' : '';
					const target = s.obj ? (fname ? s.obj + '.' + fname : s.obj) : (fname ? 'this.' + fname : 'this');
					lines.push(`${pad}${s.fn}${args} on ${target}${why}`);
					break;
				}
				case 'if': lines.push(`${pad}if (${s.cond}):${why}`); emit(s.then, cls, ind + 1); if (s.else && s.else.length) { lines.push(`${pad}else:`); emit(s.else, cls, ind + 1); } break;
				case 'loop': lines.push(`${pad}for ${s.var || 'i'} in 0..(${s.count})-1:${why}`); emit(s.body, cls, ind + 1); break;
				case 'deref': lines.push(`${pad}*${on}${fieldAt(ccls, num(s.ptr)) || v(s.ptr)}${s.index ? '[' + s.index + ']' : ''} (${s.class || '?'}):${why}`); emit(s.body, s.class, ind + 1); break;
				case 'container': {
					lines.push(`${pad}${s.kind}<${s.class || s.elemType || ''}>${s.stride ? ' stride ' + v(s.stride) : ''}${s.alloc ? ', load allocates ' + s.alloc : ''}${why}`);
					if (s.count !== undefined) lines.push(`${pad}  count: ${s.count}`);
					if (s.header) { lines.push(`${pad}  header:`); emit(s.header, s.class, ind + 2); }
					if (s.node) { lines.push(`${pad}  node:`); emit(s.node, s.class, ind + 2); }
					if (s.elem) { lines.push(`${pad}  element:`); emit(s.elem, s.class, ind + 2); }
					if (s.body) emit(s.body, cls, ind + 1);
					if (s.dispatch) for (const [t, d] of Object.entries(s.dispatch)) lines.push(`${pad}  type ${t} → ${typeof d === 'string' ? d : JSON.stringify(d)}`);
					break;
				}
				case 'note': lines.push(`${pad}// ${s.text}`); break;
				default: lines.push(`${pad}?? ${JSON.stringify(s)}`);
			}
		}
	};
	const names = Object.keys(desc.functions).sort((a, b) => a.localeCompare(b));
	lines.push('# DataWalk listing (generated)', '', 'Generated by `node re/tools/savdump.mjs --doc docs/state-walk.md` from re/iface/datawalk.json. Field names from rise.pdb.',
		'Offsets are relative to the function\'s `this` class unless prefixed by a global name. Conditions are in the loader\'s view (docs/state-spec.md §2).', '');
	for (const n of names) {
		const fn = desc.functions[n];
		lines.push(`## ${n}`, '', `@${fn.addr || '?'} · this: ${fn.this || '(globals)'}${fn.params && fn.params.length ? ' · params: ' + fn.params.join(', ') : ''} · confidence: ${fn.confidence || '?'} · source: ${fn.source}`, '');
		if (fn.note) lines.push(fn.note, '');
		if (fn.issues) lines.push('Open: ' + fn.issues, '');
		lines.push('```');
		emit(fn.body, fn.this, 0);
		lines.push('```', '');
	}
	const text = lines.join('\n');
	if (outFile) { fs.writeFileSync(outFile, text); console.log(`wrote ${outFile} (${names.length} functions)`); } else console.log(text);
}


// ------------------------------------------------------------------------------------------------ save summary

// Facts about a decoded save, read from the decoded records with PDB field names (no player names are printed).
export function summarizeSave(dec, node, verifyNode, buf) {
	const P = dec.P;
	const offs = cls => { const m = {}; for (const f of P.flat(P.layout(cls))) if (!(f.name in m) && f.type !== 'vfptr') m[f.name] = { o: f.offset, s: f.size }; return m; };
	const GI = offs('GameInfo'), G = offs('Game'), W = offs('World'), LD = offs('LeaderData'), PL = offs('Player');
	const rd = (rec, base, F, name) => { const f = F[name]; const b = rec.bytes, o = base + f.o + rec.origin; return f.s >= 4 ? b.readInt32LE(o) : f.s >= 2 ? b.readUInt16LE(o) : b[o]; };
	const find = (n, name, out = []) => {
		for (const it of n.items) {
			if (it.call) { if (it.call.fn === name) out.push(it.call); find(it.call, name, out); }
			else if (it.deref) find(it.deref, name, out);
			else if (it.container) for (const e of it.elements) if (e) find(e, name, out);
		}
		return out;
	};
	const game = dec.globals.game, world = dec.globals.world, leaders = dec.globals.leaders;
	const out = {};
	const gi = find(node, 'GameInfo::walk_data')[0];
	out.gameVersion = (gi.items.find(i => i.string !== undefined) || {}).string;
	out.setup = {};
	for (const n of ['version', 'seed', 'checksum_deep', 'checksum_window_size', 'checksum_failure_threshold', 'flags', 'team_style', 'map_style', 'map_size', 'players', 'max_observers', 'game_speed', 'game_rules', 'difficulty', 'starting_town', 'starting_resources', 'starting_resources2', 'tech_cost', 'reveal_map', 'pop_limit', 'rush_rules', 'cannon_times', 'starting_technology', 'starting_technology2', 'ending_technology', 'elimination', 'victory', 'wonderwin', 'score_goal', 'popwin', 'time_limit', 'chairs', 'econwin', 'scenario_type', 'script_type', 'mods'])
		out.setup[n] = rd(game, G.info.o, GI, n);
	out.game = {};
	for (const n of ['frame', 'tick', 'market_tick', 'num_players', 'num_nations', 'num_sides', 'num_goods', 'num_items', 'num_objects', 'total_units', 'world_pop', 'world_cities', 'world_villages', 'musical_chairs']) out.game[n] = rd(game, 0, G, n);
	out.game.semaphore = game.slice(G.semaphore.o, 16).toString('hex');
	out.game.random_seed = dec.globals.game_random.slice(0, 4).readUInt32LE(0);
	out.world = {};
	for (const n of ['xs', 'ys', 'size', 'fog_size', 'tile_size', 'reg_xs', 'reg_ys', 'reg_size', 'map', 'sea_map', 'forest_size', 'mountain_size', 'rock_size', 'total_metal', 'total_oil', 'goodies', 'land_resources', 'sea_resources', 'land_size', 'seed'])
		out.world[n] = n === 'xs' ? world.slice(0, 4).readInt32LE(0) : n === 'ys' ? world.slice(4, 4).readInt32LE(0) : rd(world, 0, W, n);
	// players (names are never printed)
	out.players = [];
	for (let k = 0; k < 8; k++) {
		const base = G.info.o + GI.player.o + k * 0x8c;
		const fl = rd(game, base, PL, 'flags');
		if (fl & 1) out.players.push({ slot: k, flags: '0x' + fl.toString(16), tribe: rd(game, base, PL, 'tribe'), who: rd(game, base, PL, 'who'), team: rd(game, base, PL, 'team'), play: rd(game, base, PL, 'play'), diff: rd(game, base, PL, 'diff') });
	}
	out.leaders = [];
	for (let k = 0; k < 9; k++) {
		const base = k * 0x6eec;
		if (!(leaders.slice(base, 4).readInt32LE(0) & 1)) continue;
		out.leaders.push({ who: k, tribe: rd(leaders, base, LD, 'tribe'), gov: rd(leaders, base, LD, 'gov'), score: rd(leaders, base, LD, 'score'), cities: rd(leaders, base, LD, 'city_num'), villages: rd(leaders, base, LD, 'village_num') });
	}
	// objects: per list (player slot) alive and dead slots by class
	const on = find(node, 'Objects::walk_data')[0];
	const names = ['units', 'builds', 'walls', 'animals'];
	out.objects = {};
	on.items.filter(i => i.call).forEach((l, who) => {
		const c = l.call.items.find(i => i.container);
		const cnt = { units: 0, builds: 0, walls: 0, animals: 0, empty_or_dead_slots: 0 };
		if (c) for (const e of c.elements) {
			if (!e) { cnt.empty_or_dead_slots++; continue; }
			const t = +(/type (\d+)/.exec(e.fn) || [])[1];
			let alive = null;
			(function dfs(n) { for (const it of n.items) { if (alive !== null) return; if (it.call) { if (it.call.fn === 'SubObject::walk_data') { const r = it.call.items.find(x => x.raw && x.raw[1] === 1); if (r) alive = buf[r.pos] & 1; } else dfs(it.call); } } })(e);
			if (alive) cnt[names[t]]++; else cnt.empty_or_dead_slots++;
		}
		if (cnt.units + cnt.builds + cnt.walls + cnt.animals) out.objects[who === 8 ? 'gaia(8)' : 'player ' + who] = cnt;
	});
	// world grid completeness
	const wn = find(node, 'World::walk_data')[0];
	const cells = [];
	(function coll(n) { for (const it of n.items) { if (it.deref) { const d = it.deref; if (d.fn.includes('WData')) { const r = d.items.find(x => x.raw && x.raw[1] === 0x15); if (r) cells.push(r.pos); } } else if (it.call) coll(it.call); } })(wn);
	const g = { wdata_cells: cells.length, size: out.world.size, land_values: {}, cells_without_region: 0 };
	const regs = new Map();
	let shore = 0;
	for (const p of cells) {
		const land = buf.readInt8(p + 2), reg = buf.readInt16LE(p + 4);
		g.land_values[land] = (g.land_values[land] || 0) + 1;
		if (reg < 0) g.cells_without_region++; else regs.set(reg, (regs.get(reg) || 0) + 1);
		if (buf.readUInt16LE(p) & 0x100) shore++;
	}
	g.land_size_check = `land==0 cells ${g.land_values[0] || 0} + flag 0x100 cells ${shore} = ${(g.land_values[0] || 0) + shore} (World.land_size ${out.world.land_size})`;
	const tdata = wn.items.find(x => x.buf !== undefined && x.len === out.world.tile_size * 2);
	g.tdata_tiles = tdata ? tdata.len / 2 : 0;
	if (tdata) { let nz = 0; for (let i = 0; i < tdata.len; i += 2) if (buf.readUInt16LE(tdata.pos + i)) nz++; g.tdata_nonzero = nz; }
	let rl = null;
	(function f2(n) { for (const it of n.items) { if (it.container && it.class === 'Region') rl = it; else if (it.call) f2(it.call); } })(node);
	if (rl) {
		const used = [];
		rl.elements.forEach((e, i) => { if (e && regs.has(i)) used.push([i, e.rec.bytes.readInt32LE(0x14), regs.get(i)]); });
		g.region_list = rl.length;
		g.regions_used = used.length;
		g.region_size_equals_cell_count = used.every(u => u[1] === u[2]);
		g.regions_sea_land = [dec.globals.regions.slice(0x18, 4).readInt32LE(0), dec.globals.regions.slice(0x1c, 4).readInt32LE(0)];
	}
	out.grid = g;
	if (verifyNode) {
		const vals = [];
		(function w(n) { for (const it of n.items) { if (it.local !== undefined) vals.push(it.value); else if (it.call) w(it.call); } })(verifyNode);
		out.verify_entries = vals.length;
	}
	return out;
}

// ------------------------------------------------------------------------------------------------ files

function loadBytes(file) {
	let b = fs.readFileSync(file);
	if (b[0] === 0x1f && b[1] === 0x8b) b = zlib.gunzipSync(b);
	return b;
}
function readStr(b, p) { const n = b.readUInt32LE(p); return [b.toString('utf16le', p + 4, p + 4 + 2 * n), p + 4 + 2 * n]; }

function decodeAt(desc, buf, pos, fn, obj, opts) {
	const dec = new Decoder(desc, buf, pos, opts);
	const ctxRec = obj ? dec.global(obj) : new Rec(desc.functions[fn]?.this, dec.sizeOf(desc.functions[fn]?.this, 0), fn);
	let result, error = null;
	try {
		result = dec.invoke(fn, { rec: ctxRec, base: 0, parent: null }).node;
		// Further functions on the same decoder (a save is followed by SaveGame::verify_save's checksum list).
		for (const next of opts.then || []) {
			if (!desc.functions[next]) continue;
			const n2 = dec.invoke(next, { rec: new Rec(desc.functions[next].this, 0, next), base: 0, parent: null }).node;
			result = { fn: result.fn + ' + ' + next, cls: result.cls, base: 0, items: [{ call: result }, { call: n2 }], rec: result.rec };
		}
	} catch (e) {
		if (!(e instanceof Stop) && opts.strict) throw e;
		error = e.message;
	}
	return { dec, node: result, end: dec.p, error };
}

function main(argv) {
	const a = argv.slice(2);
	const opt = { json: null, trace: false, maxDepth: 3, mode: 'load', fn: null, at: 0, obj: null, replay: false, version: 0x10 };
	const files = [];
	for (let i = 0; i < a.length; i++) {
		const k = a[i];
		if (k === '--build') return void buildDescription();
		if (k === '--doc') return void docgen(a[i + 1]);
		if (k === '--check') { const d = buildDescription({ write: false }); process.exitCode = d.problems.length ? 1 : 0; return; }
		if (k === '--json') opt.json = a[++i];
		else if (k === '--trace') opt.trace = true;
		else if (k === '--max-depth') opt.maxDepth = +a[++i];
		else if (k === '--mode') opt.mode = a[++i];
		else if (k === '--fn') opt.fn = a[++i];
		else if (k === '--at') opt.at = Number(a[++i]);
		else if (k === '--obj') opt.obj = a[++i];
		else if (k === '--replay') opt.replay = true;
			else if (k === '--summary') opt.summary = true;
		else if (k === '--synth') opt.synth = true;
		else if (k === '--synth-fill') { opt.synth = true; opt.synthFill = true; }
		else if (k === '--version') opt.version = Number(a[++i]);
		else files.push(k);
	}
	if (opt.synth) {
		// Consistency check of the description: walk WalkDataGame over an all-zero stream (empty containers, absent
		// optionals, tags correct). Every reached expression must evaluate and every call must resolve.
		if (!fs.existsSync(MERGED)) buildDescription({ quiet: true });
		const desc = JSON.parse(fs.readFileSync(MERGED, 'utf8'));
		const pdb = loadPdbTypes(path.join(ROOT, 're/bin/rise.pdb'));
		const fn = opt.fn || 'WalkDataGame::walk_data';
		const r = decodeAt(desc, Buffer.alloc(0), 0, fn, opt.obj, { trace: opt.trace, mode: opt.mode, pdb, synth: true, synthFill: opt.synthFill, strict: false });
		console.log(`synthetic ${fn}: ${r.end} bytes${r.error ? ', STOPPED: ' + r.error : ', no errors'}`);
		process.exitCode = r.error ? 1 : 0;
		return;
	}
	if (!files.length) { console.log(fs.readFileSync(fileURLToPath(import.meta.url), 'utf8').split('\n').slice(1, 16).join('\n')); return; }
	if (!fs.existsSync(MERGED)) buildDescription({ quiet: true });
	const desc = JSON.parse(fs.readFileSync(MERGED, 'utf8'));
	const pdb = loadPdbTypes(path.join(ROOT, 're/bin/rise.pdb'));
	const buf = loadBytes(files[0]);
	const common = { trace: opt.trace, mode: opt.mode, pdb, saveVersion: opt.version };
	const results = [];
	const report = (title, r, start) => {
		console.log(`\n== ${title}: bytes ${start}..${r.end} (${r.end - start})${r.error ? '  STOPPED: ' + r.error : ''}`);
		if (r.node) printTree(render(r.dec, r.node), 0, opt.maxDepth);
		results.push({ title, start, end: r.end, error: r.error, tree: r.node ? render(r.dec, r.node) : null });
	};
	if (opt.fn) {
		const r = decodeAt(desc, buf, opt.at, opt.fn, opt.obj, common);
		report(opt.fn, r, opt.at);
	} else if (opt.replay || /\.rcx$/i.test(files[0]) || /replay/i.test(files[0])) {
		// Replay: Game::walk_data header + String save_name (RecordGame::write_header), then the embedded save or rules.
		const r = decodeAt(desc, buf, 0, 'Game::walk_data', 'game', { ...common, saveVersion: 0x10 });
		report('replay header (Game::walk_data)', r, 0);
		if (!r.error) {
			const [saveName, p] = readStr(buf, r.end);
			console.log(`save_name = ${JSON.stringify(saveName)} (header ends @${p})`);
			const sem = r.dec.globals.game.bytes;
			const ctw = sem.length > 0x814 + 0xe && (sem[0x814 + 0xc + 2] & 2);
			if (ctw) console.log('Conquer the World header follows (ConquestGame::walk_data) — decoding');
			let pos = p;
			if (ctw) { const c = decodeAt(desc, buf, pos, 'ConquestGame::walk_data', 'conquest_game', common); report('CTW header', c, pos); pos = c.end; }
			if (saveName) {
				const [magic, q] = readStr(buf, pos);
				const ver = buf.readUInt32LE(q);
				console.log(`embedded save @${pos}: magic=${JSON.stringify(magic)} version=${hex(ver)}`);
				const s = decodeAt(desc, buf, q + 4, 'WalkDataGame::walk_data', null, { ...common, saveVersion: ver, then: ['SaveGame::verify_save'] });
				report('embedded save (WalkDataGame::walk_data + SaveGame::verify_save)', s, q + 4);
			} else {
				// Random game info: 5 bytes per active player (flags&1, who<8) + 4; then the rules block.
				// Count the active players from the decoded header (Game.info.player[k].flags / .who).
				let active = 0;
				const info = r.dec.globals.game;
				for (let k = 0; k < 8; k++) {
					const base = 0xc + playerOffset(pdb) + k * 0x8c;
					const flags = info.bytes.readUInt16LE(base + 0x30), who = info.bytes[base + 0x33];
					if ((flags & 1) && who < 8) active++;
				}
				// The block can have more entries than the header has players (lobby / scenario games): probe for the rules tag.
				let nEntries = active;
				for (const n of [active, ...[...Array(9).keys()].filter(n => n !== active)]) if (pos + 5 * n + 4 < buf.length && buf[pos + 5 * n + 4] === 0x92) { nEntries = n; break; }
				const rpos = pos + 5 * nEntries + 4;
				console.log(`random game info: ${nEntries} entries (${active} header players) → rules block @${rpos}`);
				const s = decodeAt(desc, buf, rpos, 'Game::walk_rules_data', 'game', common);
				report('rules (Game::walk_rules_data)', s, rpos);
			}
		}
	} else {
		const [magic, q] = readStr(buf, 0);
		const ver = buf.readUInt32LE(q);
		console.log(`save: magic=${JSON.stringify(magic)} version=${hex(ver)}`);
		if (!/^RoN(Multi|CTW)?Save$/.test(magic)) console.log('warning: unexpected magic');
		const s = decodeAt(desc, buf, q + 4, 'WalkDataGame::walk_data', null, { ...common, saveVersion: ver, then: ['SaveGame::verify_save'] });
		report('save (WalkDataGame::walk_data + SaveGame::verify_save)', s, q + 4);
		if (opt.summary && !s.error) {
			const sum = summarizeSave(s.dec, s.node, s.node.items[1].call, buf);
			console.log('\nsummary:\n' + JSON.stringify(sum, null, 1));
			results.push({ title: 'summary', summary: sum });
		}
		if (!s.error && s.end !== buf.length) console.log(`note: ${buf.length - s.end} bytes after the walk`);
	}
	if (opt.json) { fs.writeFileSync(opt.json, JSON.stringify(results, null, 1)); console.log(`wrote ${opt.json}`); }
}

function playerOffset(P) {
	const L = P.layout('GameInfo');
	const f = L && P.flat(L).find(x => x.name === 'player');
	return f ? f.offset : 0x38;
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) main(process.argv);
