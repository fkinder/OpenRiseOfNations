// Compares the C++ replay reader (engine/sim/replay.*, `ron-net survey --json out.jsonl`) with re/tools/rcxdump.mjs on every .rcx under a
// directory: packages, commands, CHECK_SUMS rows, last frame, start frame, embedded save size, random-info entries, where the command stream
// starts, the truncated tail, the per-verb command counts and a fingerprint (FNV-1a over every record header and every decoded payload byte).
//
//   ./build/ron-net survey "re/oracle/Recorded Games/Multi" --json /tmp/net.jsonl
//   node apps/net/rcxcompare.mjs "re/oracle/Recorded Games/Multi" /tmp/net.jsonl      (the same for the single-player folder)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const { loadReplay, decodeReplay } = await import(pathToFileURL(path.join(here, '../../re/tools/rcxdump.mjs')).href);

const [dir, netFile] = process.argv.slice(2);
if (!dir || !netFile) { console.error('usage: node apps/net/rcxcompare.mjs <dir> <ron-net survey --json file>'); process.exit(2); }

const fnvByte = (h, b) => Math.imul((h ^ (b & 0xff)) >>> 0, 16777619) >>> 0;
const fnv32 = (h, v) => { for (let i = 0; i < 4; i++) h = fnvByte(h, (v >>> (8 * i)) & 0xff); return h; };

function reference(file) {
  const { buf } = loadReplay(file);
  const d = decodeReplay(buf);
  let h = 2166136261, commands = 0, sums = 0;
  const verbs = {};
  for (const r of d.records) {
    for (const v of [r.frame, r.play, r.valid, r.stamp, r.size]) h = fnv32(h, v);
    for (const b of r.plain ?? r.data) h = fnvByte(h, b);
    for (const c of r.cmds) { commands++; verbs[c.name] = (verbs[c.name] || 0) + 1; if (c.op === 57) sums++; }
  }
  return { packages: d.records.length, commands, sums, lastFrame: d.records.length ? d.records.at(-1).frame : 0,
    startFrame: d.save?.frame ?? d.header.frame, headerFrame: d.header.frame, saveSize: d.save?.size ?? 0,
    rgi: d.randomGameInfo ? d.randomGameInfo.entries.length : -1, streamAt: d.streamAt, trunc: d.records.truncated ? d.records.truncated.at : -1, fp: h, verbs };
}

const net = new Map(fs.readFileSync(netFile, 'utf8').trim().split('\n').map((l) => JSON.parse(l)).map((o) => [o.file, o]));
const walk = (d) => fs.readdirSync(d, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name)).flatMap((e) => e.isDirectory() ? walk(path.join(d, e.name)) : /\.rcx$/i.test(e.name) ? [path.join(d, e.name)] : []);
let same = 0, bad = 0;
for (const f of walk(dir)) {
  const name = path.basename(f), n = net.get(name);
  if (!n) { console.log(`MISSING in ron-net: ${name}`); bad++; continue; }
  const r = reference(f), diffs = [];
  for (const k of Object.keys(r)) if (k !== 'verbs' && r[k] !== n[k]) diffs.push(`${k}: rcxdump ${r[k]}, ron-net ${n[k]}`);
  for (const v of new Set([...Object.keys(r.verbs), ...Object.keys(n.verbs)])) if ((r.verbs[v] || 0) !== (n.verbs[v] || 0)) diffs.push(`verb ${v}: rcxdump ${r.verbs[v] || 0}, ron-net ${n.verbs[v] || 0}`);
  if (diffs.length) { bad++; console.log(`DIFF ${name}: ${diffs.join('; ')}`); } else same++;
}
console.log(`${same} identical, ${bad} different`);
process.exit(bad ? 1 : 0);
