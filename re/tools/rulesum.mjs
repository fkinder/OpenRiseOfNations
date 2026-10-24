// Reproduce the CHECK_SUMS `rules` field from a replay's own rules block (Game::walk_rules_data under the checksum walker)
// and compare it with the recorded first CHECK_SUMS of every player. First proof that the checksum model of
// docs/state-spec.md §6 and docs/replay-spec.md §11 matches real data.
//
//   node re/tools/rulesum.mjs <replay.rcx> [more.rcx ...]
//
// Model (confirmed on the corpus, docs/oracle-corpus.md §4): accum = adler32 over the raw ranges of the rules block in file
// order, section tags and String fields skipped, and each SimpleArray<u16> hashed as `u32 n` followed, when n != 0, by
// `u32 alloc, u16, u8 flags` and the n u16 values. Only replays that start from scratch (no embedded save) carry the block.
import { loadReplay, Reader, readHeader, readRandomGameInfo, rgiCount } from './rcxdump.mjs';
import { pathToFileURL } from 'node:url';

const RANGES = [[0, 50, 'G'], [50, 414, 'U'], [414, 543, 'B'], [543, 544, 'I'], [544, 629, 'T'], [629, 684, 'S'], [684, 806, 'N']];

export function adler32(parts, a = 1, b = 0) {
  for (const p of parts) for (const x of p) { a = (a + x) % 65521; b = (b + a) % 65521; }
  return ((b << 16) | a) >>> 0;
}

// Returns the checksum of the rules block of a decoded replay buffer, or null if the replay embeds a save instead.
export function rulesChecksum(buf) {
  const h = readHeader(new Reader(buf));
  if (h.saveName) return null;
  const r = new Reader(buf);
  readHeader(r);
  readRandomGameInfo(r, h, rgiCount(buf, r.p, h));
  r.tag('Game Rules', 0x92);
  const parts = [];
  const arr = () => {
    const n = r.u32(); const nb = Buffer.alloc(4); nb.writeUInt32LE(n); parts.push(nb);
    if (n) parts.push(r.bytes(4), r.bytes(2), r.bytes(1), r.bytes(2 * n));
  };
  for (const [lo, hi, c] of RANGES) for (let i = lo; i < hi; i++) {
    parts.push(r.bytes(0x5a)); r.str();
    if ('GUBI'.includes(c)) {
      parts.push(r.bytes(0x98)); arr(); arr();
      if (c === 'G') parts.push(r.bytes(0x44)); if (c === 'U') parts.push(r.bytes(0x18 + 8 + 4 + 0x2f4)); if (c === 'B') parts.push(r.bytes(0x31));
    } else if (c === 'T') { parts.push(r.bytes(0x1b)); for (let k = 0; k < 8; k++) r.str(); }
    else if (c === 'S') parts.push(r.bytes(0x30));
  }
  parts.push(r.bytes(0xd40), r.bytes(4), r.bytes(0x1ed * 0x1ed * 2));
  for (let t = 0; t < 24; t++) { r.u8(); parts.push(r.bytes(0x18), r.bytes(0x580)); }
  return adler32(parts);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  for (const f of process.argv.slice(2)) {
    const { buf } = loadReplay(f);
    const c = rulesChecksum(buf);
    console.log(`${f.split('/').pop()}: rules block adler32 = ${c}`);
  }
}
