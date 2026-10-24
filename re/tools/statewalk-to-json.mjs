#!/usr/bin/env node
// Rebuild the machine-readable DataWalk description fragments (re/iface/datawalk/*.json) from the committed readable
// listing docs/state-walk.md. The listing was generated from those fragments by `savdump.mjs --doc`, and `re/iface` is
// gitignored, so on a fresh checkout the listing is the only copy; this tool is its inverse. See docs/state-spec.md §3, §7.
//
//   node re/tools/statewalk-to-json.mjs [--doc docs/state-walk.md] [--out re/iface/datawalk] [--check]
//   node re/tools/savdump.mjs --build            then merges the fragments into re/iface/datawalk.json
//
// What the listing keeps: every function (address, class, params, confidence, source fragment, note, open issues) and every
// step with its expressions, list counts, headers and node steps. What it renders differently and this tool restores:
//   - field names stand for byte offsets (call/string/bitmask/deref/buf `at`, `ptr`): resolved through rise.pdb
//     (the first field at an offset is the one the listing prints, so the inverse is unambiguous);
//   - globals (`obj`) are not declared in the listing: the table GLOBAL_CLASS below declares them (name -> PDB class);
//   - container `at`/`obj` are not kept (the decoder never uses them: containers read the stream and elements get fresh records);
//     a list without `count:` takes the `count`/`length` local walked just before it.
// The listing is lossless for the validated description (the corrections found by decoding real saves are in it, with their
// evidence in docs/state-spec.md §8): `savdump --doc` of the rebuilt description reproduces the listing byte for byte.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadPdbTypes } from './pdbtypes.mjs';
import { resolveLabel } from './walklabel.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

// ---------------------------------------------------------------------------------------------------- globals
// Contexts walked by name. `class` is the PDB class of the object (field names, record size); statics without a class
// are plain records that grow as walked. Names are those the listing uses after `raw`/`on`.
const GLOBAL_CLASS = {
	game: 'Game', console: 'Console', obj_base: null, obj_end: null, tribes: 'Tribes', leaders: 'Leaders', types: 'Types',
	tileset: 'TileSet', mountains: 'Mountains', constants: 'Constants', ai_speed: null, ai_off: null, armies: 'Armies',
	cities: 'Cities', forms: 'Forms', goods: null, items: null, heroes: 'Heroes', herds: null, specials: 'Specials',
	wonders: 'Wonders', forts: 'Forts', docks: 'Docks', oil_wells: 'OilWells', supplies: 'Supplies', caravans: 'Caravans',
	lands: 'Lands', leader_options: 'LeaderOptions', option_info: 'OptionInfo', pathfinder: 'PathFinder', groups: 'Groups',
	objects: 'Objects', hotkey_groups: null, world: 'World', game_daemon: 'GameDaemon', game_random: 'Random',
	graphic_events: 'GraphicEvents', scene: 'Scene', farms: 'Farms', unbuilt_wonders: 'UnbuiltWonders',
	unbuilt_cities: 'UnbuiltCities', unbuilt_forts: 'UnbuiltForts', conquest_game: 'ConquestGame', detail_threshold: null,
	camera: 'Camera', select_groups: 'SelectGroups', options: 'Options', command_manager: 'CommandManager', rivers: 'Rivers',
	terrain: 'Terrain', message_win: 'MessageWin', cliffs: 'Cliffs', doobers: 'Doober', regions: 'Regions',
	sys_command_wait_hold: null, armies_find_dist: null, build_queue_fail: null, achieve: 'Achieve', scenario_data: 'ScenarioData',
	run_time_env: 'RunTimeEnv', turn_control: 'TurnControl', combat_table: 'Balance', conquest_team_colors: 'ConquestTeamColors',
	'ScriptFile::script_files': 'PtrArray<ScriptFile>', techtypes: null,
	'ScenarioData::scenario_name': 'String', 'ScenarioData::temp_save': 'String', 'ScenarioData::victory_message': 'String',
	'ScenarioData::defeat_message': 'String', 'ScenarioData::general_powers_script': 'String',
	'ScenarioData::general_powers_script_file': 'String', 'ScenarioData::msg_color': 'Color', 'ScenarioData::game_msg_color': 'Color',
	'ScenarioData::objective_color': 'Color', 'ScenarioData::user_warnings': 'BitMask<8>', 'ScenarioData::city_lost': 'BitMask<8>',
};

// ---------------------------------------------------------------------------------------------------- helpers

const KINDS = ['Array', 'ObjectArray', 'NamedObjectArray', 'SimpleArray', 'NamedSimpleArray', 'PtrArray', 'MultiPtrArray', 'Stack',
	'LinkList', 'PtrLinkListAbstract', 'OrderList'];
const LOCAL_TYPES = new Set(['u8', 'i8', 'u16', 'i16', 'u32', 'i32', 'f32']);

const numTok = t => (/^-?0x[0-9a-f]+$/i.test(t) ? (t[0] === '-' ? -parseInt(t.slice(1), 16) : parseInt(t, 16)) : t);
const decTok = t => (/^-?\d+$/.test(t) ? Number(t) : t);

// Split at the first top-level ", " (outside brackets and quotes).
function splitTop(s) {
	let d = 0, q = null;
	for (let i = 0; i < s.length; i++) {
		const c = s[i];
		if (q) { if (c === q) q = null; continue; }
		if (c === '"' || c === "'") { q = c; continue; }
		if ('([{'.includes(c)) d++;
		else if (')]}'.includes(c)) d--;
		else if (c === ',' && d === 0 && s[i + 1] === ' ') return [s.slice(0, i), s.slice(i + 2)];
	}
	return null;
}

function balanced(s, open, close, from) {   // index just after the group opened at s[from]
	let d = 0;
	for (let i = from; i < s.length; i++) {
		if (s[i] === open) d++;
		else if (s[i] === close && --d === 0) return i + 1;
	}
	return -1;
}

export function parseListing(text, P, problems) {
	const lines = text.split('\n');
	// 1. sections
	const secs = [];
	for (let i = 0; i < lines.length; i++) {
		if (lines[i].startsWith('## ')) secs.push({ name: lines[i].slice(3), at: i });
	}
	const fnNames = new Set(secs.map(s => s.name));
	const fnSorted = [...fnNames].sort((a, b) => b.length - a.length);
	const globals = Object.keys(GLOBAL_CLASS).sort((a, b) => b.length - a.length);
	// ScenarioData statics are named ScenarioData::<member>; collect them from the listing.
	for (const m of text.matchAll(/ScenarioData::(\w+)/g)) if (!(('ScenarioData::' + m[1]) in GLOBAL_CLASS)) GLOBAL_CLASS['ScenarioData::' + m[1]] = null;
	const allGlobals = Object.keys(GLOBAL_CLASS).sort((a, b) => b.length - a.length);

	const flatCache = new Map();
	const flat = cls => {
		if (!cls || !P) return null;
		if (!flatCache.has(cls)) { const L = P.layout(cls); flatCache.set(cls, L ? P.flat(L).filter(f => f.type !== 'vfptr') : null); }
		return flatCache.get(cls);
	};
	const fieldAt = (cls, o) => { const F = flat(cls); const x = F && F.find(f => f.offset === o); return x ? x.name : null; };
	const offsetOf = (cls, name) => {
		const F = flat(cls);
		if (!F) return null;
		for (const f of F) if (f.name === name && fieldAt(cls, f.offset) === name) return f.offset;
		return null;
	};

	const splitObj = t => {
		for (const g of allGlobals) if (t.startsWith(g + '.')) return [g, t.slice(g.length + 1)];
		return [null, t];
	};
	// A printed offset: hex, field name, or expression. Returns number or string.
	const offsetTok = (tok, cls, where, paren = false) => {
		if (tok === '' || tok === undefined) return 0;
		if (/^-?0x[0-9a-f]+$/i.test(tok)) return numTok(tok);
		if (paren && tok[0] === '(' && tok.endsWith(')') && balanced(tok, '(', ')', 0) === tok.length) return tok.slice(1, -1);
		if (/^[A-Za-z_]\w*$/.test(tok)) {
			// statics without a class (String / BitMask members of ScenarioData, a PtrArray static): the member is at +0
			if (!cls && /^(data|bits|list)$/.test(tok)) return 0;
			const o = offsetOf(cls, tok);
			if (o !== null) return o;
			problems.push(`${where}: cannot resolve field "${tok}" in ${cls || '?'}`);
			return 0;
		}
		return tok;
	};

	const functions = {};
	for (let si = 0; si < secs.length; si++) {
		const sec = secs[si];
		const end = si + 1 < secs.length ? secs[si + 1].at : lines.length;
		let i = sec.at + 1;
		while (i < end && !lines[i].trim()) i++;
		const meta = lines[i++].split(' · ');
		const fn = {};
		const addr = meta[0].replace(/^@/, '');
		if (addr !== '?') fn.addr = addr;
		for (const m of meta.slice(1)) {
			if (m.startsWith('this: ')) { const t = m.slice(6); if (t !== '(globals)') fn.this = t; }
			else if (m.startsWith('params: ')) fn.params = m.slice(8).split(', ');
			else if (m.startsWith('confidence: ')) { const c = m.slice(12); if (c !== '?') fn.confidence = c; }
			else if (m.startsWith('source: ')) fn.source = m.slice(8);
			else problems.push(`${sec.name}: unknown header part ${JSON.stringify(m)}`);
		}
		// note / issues paragraphs until the fence
		const desc = [];
		while (i < end && lines[i] !== '```') desc.push(lines[i++]);
		while (desc.length && !desc[desc.length - 1].trim()) desc.pop();
		while (desc.length && !desc[0].trim()) desc.shift();
		const oi = desc.findIndex(l => l.startsWith('Open: '));
		if (oi >= 0) {
			fn.issues = desc.slice(oi).join('\n').slice(6).trim();
			desc.splice(oi);
			while (desc.length && !desc[desc.length - 1].trim()) desc.pop();
		}
		if (desc.length) fn.note = desc.join('\n');
		i++;   // fence
		const body = [];
		while (i < end && lines[i] !== '```') body.push(lines[i++]);
		const items = body.map(l => ({ ind: (l.length - l.trimStart().length) / 2, t: l.trim() }));
		const ctxName = sec.name;
		let p = 0;

		const parseSteps = (ind, cls) => {
			const out = [];
			while (p < items.length && items[p].ind >= ind) {
				if (items[p].ind > ind) { problems.push(`${ctxName}: unexpected indent at "${items[p].t.slice(0, 60)}"`); p++; continue; }
				const it = items[p];
				if (it.t === 'else:') {
					const prev = out[out.length - 1];
					p++;
					if (!prev || prev.op !== 'if') { problems.push(`${ctxName}: else without if`); continue; }
					prev.else = parseSteps(ind + 1, cls);
					continue;
				}
				p++;
				const s = parseStep(it, ind, cls);
				if (s) out.push(s);
			}
			return out;
		};

		const parseStep = (it, ind, cls) => {
			let t = it.t;
			const where = `${ctxName} "${t.slice(0, 50)}"`;
			if (t.startsWith('// ')) return { op: 'note', text: t.slice(3) };
			let why;
			const wi = t.indexOf('   // ');
			if (wi >= 0) { why = t.slice(wi + 6); t = t.slice(0, wi); }
			const done = s => { if (why !== undefined) s.why = why; return s; };

			let m;
			if ((m = t.match(/^tag "(.*)" \(0x([0-9a-f]+)\)$/))) return done({ op: 'tag', label: m[1], _byte: parseInt(m[2], 16) });
			if (t.startsWith('raw ')) {
				m = t.match(/^raw (?:(\S+?)\.)?\[(.*)$/);
				const rest = m ? m[2] : null;
				let obj = null;
				if (m) { const [o] = splitObj(t.slice(4)); obj = o; }
				if (!m || (m[1] && !obj)) { problems.push(`${where}: bad raw`); return null; }
				let body2 = t.slice(4 + (obj ? obj.length + 1 : 0) + 1);
				let a, b;
				let mm = body2.match(/^(.*?)\) (\d+) bytes(?:: .*)?$/);
				if (mm) [a, b] = splitTop(mm[1]) || []; else { mm = body2.match(/^(.*)\)$/); if (mm) [a, b] = splitTop(mm[1]) || []; }
				if (a === undefined || b === undefined) { problems.push(`${where}: bad raw range`); return null; }
				const s = { op: 'raw', at: numTok(a), end: numTok(b) };
				if (obj) s.obj = obj;
				return done(s);
			}
			if (t.startsWith('String ') || t.startsWith('BitMask ')) {
				const isBm = t.startsWith('BitMask');
				let rest = t.slice(isBm ? 8 : 7);
				if (isBm) rest = rest.replace(/: i32 bits, i32 size, size bytes$/, '');
				const [obj, r2] = splitObj(rest);
				const c2 = obj ? GLOBAL_CLASS[obj] : cls;
				const s = { op: isBm ? 'bitmask' : 'string', at: offsetTok(r2, c2, where) };
				if (obj) s.obj = obj;
				return done(s);
			}
			if (t.startsWith('buffer *')) {
				m = t.slice(8).match(/^(.*?): (.*) bytes(?: of (\w+))?$/);
				if (!m) { problems.push(`${where}: bad buffer`); return null; }
				const [obj, r2] = splitObj(m[1]);
				const s = { op: 'buf', ptr: offsetTok(r2, obj ? GLOBAL_CLASS[obj] : cls, where), len: decTok(m[2]) };
				if (m[3]) s.elem = m[3];
				if (obj) s.obj = obj;
				return done(s);
			}
			if ((m = t.match(/^if \((.*)\):$/))) return done({ op: 'if', cond: m[1], then: parseSteps(ind + 1, cls) });
			if ((m = t.match(/^for (\w+) in 0\.\.\((.*)\)-1:$/))) return done({ op: 'loop', var: m[1], count: decTok(m[2]), body: parseSteps(ind + 1, cls) });
			if (t.startsWith('*')) {
				m = t.match(/^\*(.*) \(([^()]*)\):$/);
				if (!m) { problems.push(`${where}: bad deref`); return null; }
				let head = m[1];
				const cl = m[2] === '?' ? undefined : m[2];
				const [obj, r2] = splitObj(head);
				let name = r2, index;
				const bi = (() => { // trailing [index]
					if (!name.endsWith(']')) return -1;
					let d = 0;
					for (let k = name.length - 1; k >= 0; k--) { if (name[k] === ']') d++; else if (name[k] === '[' && --d === 0) return k; }
					return -1;
				})();
				if (bi >= 0) { index = name.slice(bi + 1, -1); name = name.slice(0, bi); }
				const s = { op: 'deref', ptr: offsetTok(name, obj ? GLOBAL_CLASS[obj] : cls, where) };
				if (index !== undefined) s.index = index === '0' ? '0' : decTok(index);   // the listing prints a falsy 0 only as the string
				if (cl) s.class = cl;
				if (obj) s.obj = obj;
				s.body = parseSteps(ind + 1, cl);
				return done(s);
			}
			// local
			if ((m = t.match(/^(\w+) (\w+)   \(writer stores: ([\s\S]*)\)$/)) && LOCAL_TYPES.has(m[1])) {
				const s = { op: 'local', name: m[2], type: m[1] };
				if (m[3] !== 'undefined') s.save = m[3];
				return done(s);
			}
			// call
			for (const f of fnSorted) {
				if (!t.startsWith(f)) continue;
				const after = t[f.length];
				if (after !== ' ' && after !== '(') continue;
				let k = f.length, args;
				if (t[k] === '(') {
					const e = balanced(t, '(', ')', k);
					if (e < 0) continue;
					args = {};
					const inner = t.slice(k + 1, e - 1);
					for (const part of inner.split(/, (?=\w+=)/)) { const eq = part.indexOf('='); args[part.slice(0, eq)] = decTok(part.slice(eq + 1)); }
					k = e;
				}
				if (!t.startsWith(' on ', k)) continue;
				const target = t.slice(k + 4);
				let obj = null, rest;
				if (target === 'this') rest = '';
				else if (target.startsWith('this.')) rest = target.slice(5);
				else { const [o, r2] = splitObj(target); if (o) { obj = o; rest = r2; } else if (allGlobals.includes(target)) { obj = target; rest = ''; } else { problems.push(`${where}: unknown call target ${target}`); rest = ''; } }
				if (obj === null && target !== 'this' && !target.startsWith('this.')) { /* already reported */ }
				if (obj && allGlobals.includes(target)) rest = '';
				const c2 = obj ? GLOBAL_CLASS[obj] : cls;
				const s = { op: 'call', fn: f };
				const at = rest === '' ? 0 : offsetTok(rest, c2, where, true);
				if (at !== 0) s.at = at;
				if (args) s.args = args;
				if (obj) s.obj = obj;
				return done(s);
			}
			// container
			m = t.match(/^(\w+)</);
			if (m && KINDS.includes(m[1])) {
				const kind = m[1];
				const e = balanced(t, '<', '>', kind.length);
				if (e < 0) { problems.push(`${where}: bad container`); return null; }
				const inner = t.slice(kind.length + 1, e - 1);
				let rest = t.slice(e);
				const s = { op: 'container', kind };
				if (/^(SimpleArray|NamedSimpleArray)$/.test(kind)) { if (inner) s.elemType = inner; } else if (inner) s.class = inner;
				let mm;
				if ((mm = rest.match(/^ stride (.*?)(?:, load allocates (.*))?$/))) {
					s.stride = numTok(mm[1]);
					if (mm[2] !== undefined) s.alloc = mm[2];
				} else if ((mm = rest.match(/^, load allocates (.*)$/))) s.alloc = mm[1];
				else if (rest !== '') problems.push(`${where}: container tail ${JSON.stringify(rest)}`);
				// children: element: / type N → / body
				{
					const body = [];
					while (p < items.length && items[p].ind > ind) {
						const c = items[p];
						if (c.ind !== ind + 1) { problems.push(`${where}: container child indent`); p++; continue; }
						if (c.t === 'element:') { p++; s.elem = parseSteps(ind + 2, inner || cls); continue; }
						if (c.t === 'header:') { p++; s.header = parseSteps(ind + 2, inner || cls); continue; }
						if (c.t === 'node:') { p++; s.node = parseSteps(ind + 2, inner || cls); continue; }
						if (c.t.startsWith('count: ')) { p++; s.count = c.t.slice(7); continue; }
						if ((mm = c.t.match(/^type (\S+) → (.*)$/))) {
							p++;
							s.dispatch ??= {};
							let v = mm[2];
							if (v[0] === '{') { try { v = JSON.parse(v); } catch (er) { problems.push(`${where}: dispatch json ${er.message}`); } }
							s.dispatch[mm[1]] = v;
							continue;
						}
						body.push(parseStep_wrapper(ind + 1, inner || cls));
					}
					if (body.length) s.body = body.flat();
				}
				return done(s);
			}
			if (t.startsWith('??')) { problems.push(`${where}: unknown step in listing`); return null; }
			problems.push(`${where}: unparsed line`);
			return null;
		};
		// one step at the current position (used for container bodies)
		const parseStep_wrapper = (ind, cls) => {
			const out = [];
			const it = items[p++];
			const s = parseStep(it, ind, cls);
			if (s) out.push(s);
			return out;
		};

		fn.body = parseSteps(0, fn.this);
		if (p < items.length) problems.push(`${ctxName}: ${items.length - p} lines left unparsed at "${items[p].t.slice(0, 60)}"`);
		functions[sec.name] = fn;
	}
	return { functions, globalNames: allGlobals };
}

// ---------------------------------------------------------------------------------------------------- post-processing

function* walkSteps(steps) {
	for (const s of steps || []) {
		yield s;
		for (const k of ['then', 'else', 'body', 'elem']) if (Array.isArray(s[k])) yield* walkSteps(s[k]);
	}
}

// Lists read their node count from a `count`/`length` local walked just before them.
function fixLists(fnName, steps, problems) {
	for (let i = 0; i < steps.length; i++) {
		const s = steps[i];
		if (s.op === 'container' && /^(LinkList|PtrLinkListAbstract|OrderList)$/.test(s.kind) && s.count === undefined && !s.header) {
			const prev = steps.slice(0, i).reverse().find(x => x.op === 'local' && /^(count|length)$/.test(x.name));
			if (prev) s.count = 'L.' + prev.name;
		}
		for (const k of ['then', 'else', 'body', 'elem']) if (Array.isArray(s[k])) fixLists(fnName, s[k], problems);
	}
}

export function build(docFile, outDir, { check = false } = {}) {
	const P = loadPdbTypes(path.join(ROOT, 're/bin/rise.pdb'));
	const problems = [];
	const text = fs.readFileSync(docFile, 'utf8');
	const { functions, globalNames } = parseListing(text, P, problems);
	for (const [n, f] of Object.entries(functions)) fixLists(n, f.body, problems);
	// verify the tag bytes against the listing
	for (const [n, f] of Object.entries(functions))
		for (const s of walkSteps(f.body))
			if (s.op === 'tag') {
				try {
					const r = resolveLabel(s.label);
					if (s._byte !== undefined && r.byte !== s._byte) problems.push(`${n}: tag "${s.label}" hashes to 0x${r.byte.toString(16)}, listing says 0x${s._byte.toString(16)}`);
				} catch (e) { problems.push(`${n}: tag "${s.label}": ${e.message}`); }
				delete s._byte;
			}
	// group by source fragment
	const frags = {};
	for (const [n, f] of Object.entries(functions)) {
		const src = f.source || 'misc.json';
		delete f.source;
		(frags[src] ??= { functions: {} }).functions[n] = f;
	}
	const globals = {};
	for (const g of globalNames) {
		const o = {};
		if (GLOBAL_CLASS[g]) o.class = GLOBAL_CLASS[g];
		globals[g] = o;
		if (GLOBAL_CLASS[g] && P.find(GLOBAL_CLASS[g]) == null) problems.push(`global ${g}: unknown class ${GLOBAL_CLASS[g]}`);
	}
	frags['globals.json'] = { globals };
	if (!check) {
		fs.mkdirSync(outDir, { recursive: true });
		for (const f of fs.readdirSync(outDir)) if (f.endsWith('.json') && !(f in frags) && !f.startsWith('fix_')) problems.push(`stale fragment ${f} left in ${outDir}`);
		for (const [f, j] of Object.entries(frags)) fs.writeFileSync(path.join(outDir, f), JSON.stringify(j, null, 1));
	}
	return { functions, frags, problems };
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
	const a = process.argv.slice(2);
	let doc = path.join(ROOT, 'docs/state-walk.md'), out = path.join(ROOT, 're/iface/datawalk'), check = false;
	for (let i = 0; i < a.length; i++) {
		if (a[i] === '--doc') doc = path.resolve(a[++i]);
		else if (a[i] === '--out') out = path.resolve(a[++i]);
		else if (a[i] === '--check') check = true;
	}
	const r = build(doc, out, { check });
	console.log(`${Object.keys(r.functions).length} functions in ${Object.keys(r.frags).length} fragments${check ? ' (not written)' : ' → ' + path.relative(ROOT, out)}`);
	if (r.problems.length) console.log(`${r.problems.length} problems:\n  ` + r.problems.slice(0, 80).join('\n  '));
	process.exitCode = r.problems.length ? 1 : 0;
}
