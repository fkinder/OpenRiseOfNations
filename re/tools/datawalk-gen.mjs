#!/usr/bin/env node
// Generate engine/sim/datawalk_desc.inc, the compact C++ table of the DataWalk description (re/iface/datawalk.json),
// interpreted by engine/sim/datawalk.cpp. Spec: docs/state-spec.md section 3.
//
//   node re/tools/statewalk-to-json.mjs && node re/tools/savdump.mjs --build      # re/iface is gitignored: rebuild it first
//   node re/tools/datawalk-gen.mjs                                                  # writes engine/sim/datawalk_desc.inc
//
// What is emitted (all `static const`, included inside namespace ron::sim::datawalk):
//   kStr      interned strings (function, class, field, label names; runtime names)
//   kCode     expression bytecode (a postfix stack machine over int64, see Op in datawalk.cpp); `save:` expressions are
//             writer-side descriptions and are kept as text only (a `local` step carries the text of what the writer stores)
//   kExpr     expression table: {0, constant} or {1, offset into kCode}
//   kSteps    every step of every walk function (nested blocks are contiguous ranges of this array)
//   kArgs     call arguments {name, expr}
//   kDisp     container dispatch entries {data type, function, class}
//   kFns      walk functions sorted by name {name, this class, params, body range}
//   kGlobals  the globals walked by name {name, class}, sorted
//   kClasses/kFields  PDB layouts (field name, offset, size, type) of every class the walks touch, for field access by name
// Output is deterministic: sorted names, no timestamps.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadPdbTypes } from './pdbtypes.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const SRC = path.join(ROOT, 're/iface/datawalk.json');
const OUT = path.join(ROOT, 'engine/sim/datawalk_desc.inc');

const desc = JSON.parse(fs.readFileSync(SRC, 'utf8'));
let P = null;
try { P = loadPdbTypes(path.join(ROOT, 're/bin/rise.pdb')); } catch { console.error('warning: rise.pdb not found, no field names'); }

// ------------------------------------------------------------------------------------------------ strings
const strs = [];
const strIdx = new Map();
const S = s => {
	if (s === undefined || s === null) return -1;
	s = String(s);
	if (!strIdx.has(s)) { strIdx.set(s, strs.length); strs.push(s); }
	return strIdx.get(s);
};

// ------------------------------------------------------------------------------------------------ expressions
// Ops (must match datawalk.cpp).
const OP = {
	END: 0, CONST: 1, VAR: 2, LOCAL: 3, RD: 4, UPRD: 5, GRD: 6, LEN: 7, RT: 8, NEG: 9, NOT: 10, BNOT: 11,
	MUL: 12, ADD: 13, SUB: 14, SHL: 15, SHR: 16, LT: 17, LE: 18, GT: 19, GE: 20, EQ: 21, NE: 22,
	AND: 23, XOR: 24, OR: 25, LAND: 26, LOR: 27, TERN: 28, INPUT: 29, CHECKSUM: 30, FLAGS: 31, SAVEVER: 32, DIV: 33, MOD: 34,
};
const RD_TYPE = { i8: 0, u8: 1, i16: 2, u16: 3, i32: 4, u32: 5 };

const code = [];
const exprs = [];
const exprKey = new Map();

function tokenize(src) {
	const t = [];
	const re = /\s*(?:(0x[0-9a-fA-F]+|\d+)|([A-Za-z_]\w*)|("[^"]*"|'[^']*')|(===|!==|==|!=|<=|>=|<<|>>|&&|\|\||[-+*\/%<>&^|!~?:().,]))/y;
	let m;
	re.lastIndex = 0;
	while (re.lastIndex < src.length) {
		const before = re.lastIndex;
		m = re.exec(src);
		if (!m) { if (/^\s*$/.test(src.slice(before))) break; throw new Error(`bad token at ${before} in ${JSON.stringify(src)}`); }
		if (m[1] !== undefined) t.push({ k: 'num', v: Number(m[1]) });
		else if (m[2] !== undefined) t.push({ k: 'id', v: m[2] });
		else if (m[3] !== undefined) t.push({ k: 'str', v: m[3].slice(1, -1) });
		else t.push({ k: 'op', v: m[4] });
	}
	return t;
}

function compileExpr(src) {
	const toks = tokenize(src);
	let p = 0;
	const out = [];
	const peek = () => toks[p];
	const isOp = v => toks[p] && toks[p].k === 'op' && toks[p].v === v;
	const expectOp = v => { if (!isOp(v)) throw new Error(`expected ${v} in ${JSON.stringify(src)}`); p++; };
	const emit = (...x) => out.push(...x);
	const binary = (next, table) => () => {
		next();
		while (toks[p] && toks[p].k === 'op' && table[toks[p].v] !== undefined) { const o = table[toks[p].v]; p++; next(); emit(o); }
	};
	function primary() {
		const t = toks[p++];
		if (!t) throw new Error(`unexpected end in ${JSON.stringify(src)}`);
		if (t.k === 'num') { emit(OP.CONST, t.v | 0); if (t.v > 0x7fffffff) throw new Error(`constant too large in ${src}`); return; }
		if (t.k === 'op' && t.v === '(') { ternary(); expectOp(')'); return; }
		if (t.k !== 'id') throw new Error(`unexpected ${t.v} in ${JSON.stringify(src)}`);
		const id = t.v;
		if (RD_TYPE[id] !== undefined && isOp('(')) { p++; ternary(); expectOp(')'); emit(OP.RD, RD_TYPE[id]); return; }
		if (id === 'up' && isOp('.')) { p++; const f = toks[p++].v; expectOp('('); ternary(); expectOp(')'); emit(OP.UPRD, RD_TYPE[f]); return; }
		if (id === 'G' && isOp('(')) {
			p++; const g = toks[p++].v; expectOp(')'); expectOp('.'); const f = toks[p++].v; expectOp('('); ternary(); expectOp(')');
			const gi = globalIndex.get(g);
			if (gi === undefined) throw new Error(`unknown global ${g}`);
			emit(OP.GRD, gi, RD_TYPE[f]); return;
		}
		if (id === 'LEN' && isOp('(')) {
			p++; const g = toks[p++].v; expectOp(')');
			const gi = globalIndex.get(g); if (gi === undefined) throw new Error(`unknown global ${g}`);
			emit(OP.LEN, gi); return;
		}
		if (id === 'rt' && isOp('(')) { p++; const n = toks[p++].v; expectOp(')'); emit(OP.RT, S(n)); return; }
		if (id === 'L' && isOp('.')) { p++; emit(OP.LOCAL, S(toks[p++].v)); return; }
		if (id === 'input') return void emit(OP.INPUT);
		if (id === 'checksum') return void emit(OP.CHECKSUM);
		if (id === 'flags') return void emit(OP.FLAGS);
		if (id === 'save_version') return void emit(OP.SAVEVER);
		if (id === 'true') return void emit(OP.CONST, 1);
		if (id === 'false') return void emit(OP.CONST, 0);
		emit(OP.VAR, S(id));
	}
	function unary() {
		if (isOp('-')) { p++; unary(); emit(OP.NEG); return; }
		if (isOp('!')) { p++; unary(); emit(OP.NOT); return; }
		if (isOp('~')) { p++; unary(); emit(OP.BNOT); return; }
		primary();
	}
	const mul = binary(unary, { '*': OP.MUL, '/': OP.DIV, '%': OP.MOD });
	const add = binary(mul, { '+': OP.ADD, '-': OP.SUB });
	const shift = binary(add, { '<<': OP.SHL, '>>': OP.SHR });
	const rel = binary(shift, { '<': OP.LT, '<=': OP.LE, '>': OP.GT, '>=': OP.GE });
	const eq = binary(rel, { '==': OP.EQ, '===': OP.EQ, '!=': OP.NE, '!==': OP.NE });
	const band = binary(eq, { '&': OP.AND });
	const bxor = binary(band, { '^': OP.XOR });
	const bor = binary(bxor, { '|': OP.OR });
	const land = binary(bor, { '&&': OP.LAND });
	const lor = binary(land, { '||': OP.LOR });
	function ternary() {
		lor();
		if (isOp('?')) { p++; ternary(); expectOp(':'); ternary(); emit(OP.TERN); }
	}
	ternary();
	if (p !== toks.length) throw new Error(`trailing tokens in ${JSON.stringify(src)}`);
	return out;
}

const num = v => (typeof v === 'number' ? v : typeof v === 'string' && /^-?(0x[0-9a-f]+|\d+)$/i.test(v.trim()) ? Number(v.trim()) : null);

// Returns an index into kExpr, or -1 when the value is absent.
function E(v) {
	if (v === undefined || v === null) return -1;
	const n = num(v);
	let key, ent;
	if (n !== null) { key = 'c' + n; ent = [0, n]; }
	else {
		if (typeof v !== 'string') throw new Error(`bad expression ${JSON.stringify(v)}`);
		key = 'e' + v;
		if (!exprKey.has(key)) { const c = compileExpr(v); ent = [1, code.length]; code.push(...c, OP.END); }
	}
	if (!exprKey.has(key)) { exprKey.set(key, exprs.length); exprs.push(ent); }
	return exprKey.get(key);
}

// ------------------------------------------------------------------------------------------------ tables
const fnNames = Object.keys(desc.functions).sort();
const fnIndex = new Map(fnNames.map((n, i) => [n, i]));
const globalNames = Object.keys(desc.globals).sort();
const globalIndex = new Map(globalNames.map((n, i) => [n, i]));

const OPS = { tag: 0, raw: 1, local: 2, string: 3, bitmask: 4, buf: 5, call: 6, if: 7, loop: 8, deref: 9, container: 10 };
const KINDS = { Array: 0, ObjectArray: 1, NamedObjectArray: 2, SimpleArray: 3, PtrArray: 4, MultiPtrArray: 5, LinkList: 6, PtrLinkListAbstract: 7, OrderList: 8 };
const LTYPES = { u8: 0, i8: 1, u16: 2, i16: 3, u32: 4, i32: 5, f32: 6 };

// Raw ranges whose bytes the CheckSum walker hashes as zeros although the save stream carries other values (Step.kind = 1 on a
// Raw step). Found by recomputing SaveGame::verify_save from the decoded saves (docs/state-spec.md section 8): the metric byte of
// every GatherPoint list node (walk_function(node+0xc, node+0xd) in PtrLinkListAbstract<GatherPoint,unsigned char>::walk_data
// @4708a0) is 0 in the verify hash while the stream holds 0..3; with it hashed as 0 all 10 saves verify completely, with the
// stream value every list that has gather points fails (a single-byte Adler delta at exactly that byte).
const HASH_AS_ZERO = [{ fn: 'PtrLinkListAbstract<GatherPoint,unsigned_char>::walk_data', at: 12 }];

const steps = [];
const stepFn = [];      // step id -> function index
const args = [];
const disp = [];
let curFn = 0;
let curFnName = '';

function block(list) {
	const live = (list || []).filter(s => s.op !== 'note');
	const start = steps.length;
	for (let i = 0; i < live.length; i++) { steps.push(null); stepFn.push(curFn); }
	for (let i = 0; i < live.length; i++) steps[start + i] = encode(live[i]);
	return [start, live.length];
}
const G = o => (o ? (globalIndex.has(o) ? globalIndex.get(o) : (() => { throw new Error(`undeclared global ${o}`); })()) : -1);

function encode(s) {
	const st = { op: OPS[s.op], kind: 0, etype: 0, a: -1, b: -1, c: -1, d: -1, e: -1, f: -1, g: -1, h: -1, i: -1 };
	if (st.op === undefined) throw new Error(`unknown op ${s.op}`);
	switch (s.op) {
		case 'tag': st.a = S(s.text); st.b = s.byte; break;
		case 'raw': st.a = E(s.at ?? 0); st.b = E(s.end); st.c = G(s.obj);
			if (HASH_AS_ZERO.some(q => q.fn === curFnName && q.at === num(s.at))) st.kind = 1;
			break;
		case 'local': st.a = S(s.name); st.etype = LTYPES[s.type || 'u32']; st.c = S(s.save); break;
		case 'string': st.a = E(s.at ?? 0); st.c = G(s.obj); break;
		case 'bitmask': st.a = E(s.at ?? 0); st.c = G(s.obj); break;
		case 'buf': st.a = E(s.ptr); st.b = E(s.len); st.c = S(s.elem); break;
		case 'call': {
			if (!fnIndex.has(s.fn)) throw new Error(`call to undescribed ${s.fn}`);
			st.a = fnIndex.get(s.fn); st.b = E(s.at); st.c = G(s.obj);
			const a = Object.entries(s.args || {});
			st.d = args.length; st.e = a.length;
			for (const [k, v] of a) args.push([S(k), E(v)]);
			break;
		}
		case 'if': {
			st.a = E(s.cond);
			const [t0, tn] = block(s.then); const [e0, en] = block(s.else);
			st.b = t0; st.c = tn; st.d = e0; st.e = en;
			break;
		}
		case 'loop': {
			st.a = S(s.var || 'i'); st.b = E(s.count);
			const [b0, bn] = block(s.body); st.c = b0; st.d = bn;
			break;
		}
		case 'deref': {
			st.a = E(s.ptr); st.b = E(s.index); st.c = G(s.obj); st.d = S(s.class);
			const [b0, bn] = block(s.body); st.e = b0; st.f = bn;
			break;
		}
		case 'container': {
			if (KINDS[s.kind] === undefined) throw new Error(`unknown container kind ${s.kind}`);
			st.kind = KINDS[s.kind]; st.a = S(s.class); st.b = E(s.stride); st.e = E(s.count); st.h = S(s.elemType);
			{ const m = typeof s.count === 'string' && s.count.match(/^L\.(\w+)$/); if (m) st.i = S(m[1]); }   // count taken from a local that is read here if no earlier step set it
			const [b0, bn] = block(s.elem); st.c = b0; st.d = bn;
			st.f = disp.length; st.g = 0;
			if (s.dispatch && typeof s.dispatch === 'object') {
				for (const [t, d] of Object.entries(s.dispatch).sort((x, y) => Number(x[0]) - Number(y[0]))) {
					const fn = typeof d === 'string' ? d : d.fn;
					const cls = typeof d === 'string' ? desc.functions[d]?.this : (d.class || desc.functions[d.fn]?.this);
					if (!fnIndex.has(fn)) throw new Error(`dispatch to undescribed ${fn}`);
					disp.push([Number(t), fnIndex.get(fn), S(cls)]);
					st.g++;
				}
			}
			break;
		}
		default: throw new Error(`unhandled op ${s.op}`);
	}
	return st;
}

const fnRows = [];
const paramList = [];
for (const name of fnNames) {
	const fn = desc.functions[name];
	curFn = fnIndex.get(name);
	curFnName = name;
	const [b0, bn] = block(fn.body);
	fnRows.push([S(name), S(fn.this), paramList.length, (fn.params || []).length, b0, bn, parseInt((fn.addr || '0').replace(/^0x/, ''), 16) || 0]);
	for (const p of fn.params || []) paramList.push(S(p));
}
const globalRows = globalNames.map(n => [S(n), S(desc.globals[n].class)]);

// ------------------------------------------------------------------------------------------------ classes and fields
const PRIM = new Set(['int', 'uint', 'long', 'ulong', 'short', 'ushort', 'char', 'uchar', 'float', 'double', 'bool', 'wchar_t', 'int8', 'uint8', 'int64', 'uint64', 'void', 'bool16', 'bool32', 'HRESULT', 'long double']);
const TC = { bytes: 0, i8: 1, u8: 2, i16: 3, u16: 4, i32: 5, u32: 6, f32: 7, f64: 8, ptr: 9, struct: 10 };
const SCALAR = { int: 'i32', long: 'i32', Coord: 'i32', WCoord: 'i32', uint: 'u32', ulong: 'u32', short: 'i16', ushort: 'u16', wchar_t: 'u16', char: 'i8', int8: 'i8',
	uchar: 'u8', bool: 'u8', uint8: 'u8', float: 'f32', double: 'f64' };
const ESIZE = { i8: 1, u8: 1, i16: 2, u16: 2, i32: 4, u32: 4, f32: 4, f64: 8 };

function classify(f) {
	const t = f.type.replace(/^const /, '');
	if (/\*$/.test(t)) return { tc: 'ptr', es: f.size, count: 1 };
	const arr = t.match(/^(.*)\[(\d+)\]$/);
	const base = arr ? arr[1] : t;
	const count = arr ? Number(arr[2]) : 1;
	if (SCALAR[base]) { const tc = SCALAR[base]; return { tc, es: ESIZE[tc], count: f.size / ESIZE[tc] }; }
	if (!PRIM.has(base) && P && P.layout(base)) { const L = P.layout(base); const es = L.size > 0 ? L.size : f.size;
		return { tc: 'struct', es, count: Math.max(1, Math.round(f.size / es)) || count, cls: base }; }
	if (f.size === 4 && !arr) return { tc: 'i32', es: 4, count: 1 };
	return { tc: 'bytes', es: 1, count: f.size };
}

const classRows = [];     // [nameStr, fieldStart, nfields]
const fieldRows = [];     // [nameStr, offset, size, tc, esize, typeStr]
const classSeen = new Set();
function addClass(cls, depth) {
	if (!P || !cls || classSeen.has(cls)) return;
	let L;
	try { L = P.layout(cls); } catch { return; }
	if (!L) return;
	classSeen.add(cls);
	const flat = P.flat(L).filter(f => f.type !== 'vfptr' && f.name);
	const start = fieldRows.length;
	const nested = [];
	for (const f of flat) {
		const c = classify(f);
		fieldRows.push([S(f.name), f.offset, f.size, TC[c.tc], c.es, c.cls ? S(c.cls) : -1]);
		if (c.cls && depth < 2) nested.push(c.cls);
	}
	classRows.push([S(cls), start, fieldRows.length - start]);
	for (const n of nested) addClass(n, depth + 1);
}
{
	const roots = new Set();
	for (const n of fnNames) if (desc.functions[n].this) roots.add(desc.functions[n].this);
	for (const g of globalNames) if (desc.globals[g].class) roots.add(desc.globals[g].class);
	const scan = list => { for (const s of list || []) { if (s.class) roots.add(s.class); for (const k of ['then', 'else', 'body', 'elem']) if (Array.isArray(s[k])) scan(s[k]); if (s.dispatch && typeof s.dispatch === 'object') for (const d of Object.values(s.dispatch)) if (d && d.class) roots.add(d.class); } };
	for (const n of fnNames) scan(desc.functions[n].body);
	for (const c of [...roots].sort()) addClass(c, 0);
	// classes the accessors and parts drivers name directly
	for (const c of ['Leader', 'Game', 'GameInfo', 'Player', 'World', 'Objects', 'Unit', 'Build', 'Wall', 'Animal', 'Good', 'Item', 'City', 'Group', 'Ammo', 'DeathObj', 'LeaderData', 'Console', 'Constants', 'WData']) addClass(c, 0);
}
const classOrder = classRows.map((r, i) => i).sort((a, b) => (strs[classRows[a][0]] < strs[classRows[b][0]] ? -1 : 1));
const classSorted = classOrder.map(i => classRows[i]);

// ------------------------------------------------------------------------------------------------ emit
const lines = [];
const arr = (decl, rows, fmt, perLine = 1) => {
	lines.push(`${decl} = {`);
	for (let i = 0; i < rows.length; i += perLine) lines.push('\t' + rows.slice(i, i + perLine).map(fmt).join(' ') );
	lines.push('};', '');
};
const cstr = s => '"' + s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/[^\x20-\x7e]/g, c => '\\x' + c.charCodeAt(0).toString(16).padStart(2, '0') + '""') + '"';

lines.push(
	'// GENERATED FILE: do not edit. Regenerate with:',
	'//   node re/tools/statewalk-to-json.mjs && node re/tools/savdump.mjs --build && node re/tools/datawalk-gen.mjs',
	'// Source: re/iface/datawalk.json (the machine-readable DataWalk description, docs/state-spec.md section 3), built from docs/state-walk.md.',
	`// ${fnNames.length} walk functions, ${steps.length} steps, ${exprs.length} expressions (${code.length} bytecode words), ${globalNames.length} globals,`,
	`// ${classSorted.length} classes with ${fieldRows.length} fields (rise.pdb layouts), ${strs.length} strings. Included inside namespace ron::sim::datawalk by datawalk.cpp.`,
	'');
lines.push(`static const char* const kStr[${strs.length}] = {`);
{
	let cur = '\t';
	for (const s of strs) { const t = cstr(s) + ','; if (cur.length + t.length > 118) { lines.push(cur); cur = '\t'; } cur += t + ' '; }
	lines.push(cur.trimEnd(), '};', '');
}
{
	lines.push(`static const int32_t kCode[${code.length}] = {`);
	let cur = '\t';
	for (const v of code) { const t = v + ','; if (cur.length + t.length > 118) { lines.push(cur.trimEnd()); cur = '\t'; } cur += t; }
	lines.push(cur.trimEnd(), '};', '');
}
arr(`static const Expr kExpr[${exprs.length}]`, exprs, e => `{${e[0]},${e[1]}},`, 12);
arr(`static const Step kSteps[${steps.length}]`, steps, s => `{${s.op},${s.kind},${s.etype},${s.a},${s.b},${s.c},${s.d},${s.e},${s.f},${s.g},${s.h},${s.i}},`, 3);
arr(`static const uint16_t kStepFn[${stepFn.length}]`, stepFn, v => `${v},`, 24);
arr(`static const Arg kArgs[${Math.max(args.length, 1)}]`, args.length ? args : [[0, 0]], a => `{${a[0]},${a[1]}},`, 8);
arr(`static const Disp kDisp[${Math.max(disp.length, 1)}]`, disp.length ? disp : [[0, 0, 0]], d => `{${d[0]},${d[1]},${d[2]}},`, 6);
arr(`static const int32_t kParams[${Math.max(paramList.length, 1)}]`, paramList.length ? paramList : [0], v => `${v},`, 16);
arr(`static const FnDesc kFns[${fnRows.length}]`, fnRows, r => `{${r.join(',')}},`, 3);
arr(`static const GlobalDesc kGlobals[${globalRows.length}]`, globalRows, r => `{${r.join(',')}},`, 6);
arr(`static const ClassDesc kClasses[${classSorted.length}]`, classSorted, r => `{${r.join(',')}},`, 6);
arr(`static const FieldDesc kFields[${Math.max(fieldRows.length, 1)}]`, fieldRows.length ? fieldRows : [[0, 0, 0, 0, 0, -1]], r => `{${r.join(',')}},`, 4);
fs.writeFileSync(OUT, lines.join('\n') + '\n');
const bytes = fs.statSync(OUT).size;
console.log(`wrote ${path.relative(ROOT, OUT)}: ${fnNames.length} functions, ${steps.length} steps, ${exprs.length} exprs, ${code.length} code words, ${globalNames.length} globals, ${classSorted.length} classes, ${fieldRows.length} fields, ${strs.length} strings, ${(bytes / 1024).toFixed(0)} KB`);
