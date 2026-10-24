// Decode just the first header fields of Rise of Nations saves (.SVX): magic, save version, GameInfo (version string,
// seed, setup bytes, player count), and the frame / tick / semaphore of the embedded Game::walk_data block.
// Not a save decoder: the full walk is re/tools/savdump.mjs (docs/state-spec.md §4-§5, docs/state-walk.md).
//
//   node re/tools/svxhead.mjs <file.SVX|dir> [--hex]       one block per save (no player names are printed)
//
// Layout: gzip( String magic, u32 sGameSaveVersion, WalkDataGame::walk_data ) and WalkDataGame starts with
//   GameInfo::walk_data (tag 0x42 ...), 182 raw bytes (Console who..target_*, select_*; obj_base[3]; obj_end[3]),
//   Game::walk_data = tag 0x16, GameInfo::walk_data again, the 0x194-byte frame block, semaphore, graphic_tick.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { pathToFileURL } from 'node:url';
import { Reader, readGameInfo, readHeaderTail } from './rcxdump.mjs';

export function svxHead(file) {
  let buf = fs.readFileSync(file);
  const stored = buf.length;
  const gz = buf[0] === 0x1f && buf[1] === 0x8b;
  if (gz) buf = zlib.gunzipSync(buf);
  const r = new Reader(buf);
  const out = { file: path.basename(file), stored, gz, size: buf.length, first64: buf.subarray(0, 64).toString('hex') };
  out.magic = r.str();
  out.saveVersion = r.u32();
  const gi = readGameInfo(r);                       // WalkDataGame step 1
  out.gameInfoEnd = r.p;
  r.bytes(182);                                     // Console block + obj_base + obj_end
  const gameAt = r.p;
  r.tag('Game', 0x16);
  const gi2 = readGameInfo(r);                      // Game::walk_data repeats GameInfo
  const h = readHeaderTail(r, gi2, false);   // a save's Game::walk_data has no save_name
  out.gameBlockAt = gameAt;
  out.versionString = gi.versionString; out.version = gi.version; out.seed = gi.seed;
  out.seedAgrees = gi.seed === gi2.seed;
  out.setup = gi.setup;
  out.players = gi.players.filter((p) => p.flags & 1).length;
  out.hasScenario = !!(gi.scenarioPath || gi.scenarioScript);
  out.frame = h.frame; out.tick = h.tick; out.multiplayer = h.multiplayer; out.conquest = h.conquest;
  out.semaphore = h.semaphore.toString('hex');
  out.bodyAt = r.p;
  return out;
}

function main(argv) {
  const target = argv[0];
  if (!target) { console.error('usage: node re/tools/svxhead.mjs <file.SVX|dir> [--hex]'); process.exit(2); }
  const files = fs.statSync(target).isDirectory()
    ? fs.readdirSync(target).filter((f) => /\.svx$/i.test(f)).sort().map((f) => path.join(target, f)) : [target];
  for (const f of files) {
    try {
      const o = svxHead(f);
      console.log(`${o.file}: ${o.stored} bytes${o.gz ? ' gzip' : ''} -> ${o.size}; ${JSON.stringify(o.magic)} v${o.saveVersion}; ${o.versionString} ver=0x${o.version.toString(16)}`);
      console.log(`  seed=${o.seed} frame=${o.frame} tick=${o.tick} ${o.multiplayer ? 'MP' : 'SP'}${o.conquest ? ' CTW' : ''} sem=${o.semaphore} players=${o.players} scenario=${o.hasScenario} map_style=${o.setup.map_style} map_size=${o.setup.map_size}; Game block @${o.gameBlockAt}, next section (tag Tribes) @${o.bodyAt}`);
      if (argv.includes('--hex')) console.log(`  ${o.first64}`);
    } catch (e) { console.log(`${path.basename(f)}: FAIL ${e.message}`); }
  }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main(process.argv.slice(2));
