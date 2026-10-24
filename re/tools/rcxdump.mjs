// Decode Rise of Nations: Extended Edition replays (.rcx): header, embedded save / rules, command stream.
// Layout and evidence: docs/replay-spec.md. Reconstructed from RecordGame::write_header / write_save_game /
// write_random_game_info / write_rules / write_package, CommandManager::process_turn, CommandPackage::process_*
// and CommandManager::issue_* (riseofnations.exe, recordgame.obj / commandmanager.obj / commandpackage.obj).
//
// Usage:
//   node re/tools/rcxdump.mjs <file.rcx|decompressed.bin>             header summary + every command
//   node re/tools/rcxdump.mjs <file> --summary                         header + per-verb counts only
//   node re/tools/rcxdump.mjs <file> --json out.json                   full decode as JSON
//   node re/tools/rcxdump.mjs <file> --hex                             also hex-dump each package payload
//   node re/tools/rcxdump.mjs --survey <dir> [--checksums out.csv] [--tsv out.tsv]   one line per .rcx under <dir>;
//                                                                       --checksums writes every CHECK_SUMS package
//   node re/tools/rcxdump.mjs <file> --checksums out.csv                one file's CHECK_SUMS packages as CSV
//
// .rcx files are gzip (RecordGame::finalize recompresses the plain stream on close); a replay whose game crashed
// is left uncompressed. Both are accepted, as is an already-decompressed dump.
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { pathToFileURL } from 'node:url';

// ---------------------------------------------------------------------------------------------------------------
// Byte reader

export class Reader {
  constructor(buf, pos = 0) { this.b = buf; this.p = pos; }
  need(n) { if (this.p + n > this.b.length) throw new RangeError(`read past end at ${this.p} (+${n}, size ${this.b.length})`); }
  u8() { this.need(1); return this.b[this.p++]; }
  i8() { this.need(1); return this.b.readInt8(this.p++); }
  u16() { this.need(2); const v = this.b.readUInt16LE(this.p); this.p += 2; return v; }
  i16() { this.need(2); const v = this.b.readInt16LE(this.p); this.p += 2; return v; }
  u32() { this.need(4); const v = this.b.readUInt32LE(this.p); this.p += 4; return v; }
  i32() { this.need(4); const v = this.b.readInt32LE(this.p); this.p += 4; return v; }
  f32() { this.need(4); const v = this.b.readFloatLE(this.p); this.p += 4; return v; }
  u64hex() { this.need(8); const v = this.b.subarray(this.p, this.p + 8); this.p += 8; return Buffer.from(v).reverse().toString('hex'); }
  bytes(n) { this.need(n); const v = this.b.subarray(this.p, this.p + n); this.p += n; return v; }
  // String::walk_data / String::read: u32 length in UTF-16 units, then the units (no terminator).
  str() { const n = this.u32(); if (n > 0x100000) throw new RangeError(`implausible string length ${n} at ${this.p - 4}`); return this.bytes(2 * n).toString('utf16le'); }
  // DataWalk::walk_test: one tag byte (low byte of the label's hash). Values observed in EE replays are checked.
  tag(name, expect) {
    const t = this.u8();
    if (expect !== undefined && t !== expect) throw new Error(`section tag "${name}" = 0x${t.toString(16)} at ${this.p - 1}, expected 0x${expect.toString(16)}`);
    return t;
  }
  get left() { return this.b.length - this.p; }
}

// Section tag bytes as written by this build (label strings from int_str_array; see replay-spec.md §2).
const TAG = { Game: 0x16, GameInfo: 0x42, Player: 0x50, GameRules: 0x92, TribeRules: 0x8f };

// ---------------------------------------------------------------------------------------------------------------
// Header: Game::walk_data + String save_name (RecordGame::write_header)

const SETUP_FIELDS = ['team_style', 'map_style', 'map_size', 'players', 'max_observers', 'game_speed', 'game_rules',
  'difficulty', 'starting_town', 'starting_resources', 'starting_resources2', 'tech_cost', 'reveal_map', 'pop_limit',
  'rush_rules', 'cannon_times', 'starting_technology', 'starting_technology2', 'ending_technology', 'elimination',
  'victory', 'wonderwin', 'score_goal', 'popwin', 'time_limit', 'chairs', 'econwin', 'scenario_type', 'script_type', 'mods'];

// Game::semaphore bits (BitMask<256>, byte 0..3) that matter for decoding.
const SEM = { MULTIPLAYER: [0, 0x04], RECORDING: [0, 0x08], PLAYBACK: [0, 0x10], CONQUEST: [2, 0x02] };

export function readHeader(r) {
  r.tag('Game', TAG.Game);
  const h = readGameInfo(r);
  return readHeaderTail(r, h);
}

// GameInfo::walk_data (also the first thing in a save after the magic and version, and again inside Game::walk_data).
export function readGameInfo(r) {
  const h = {};
  r.tag('GameInfo', TAG.GameInfo);
  h.versionString = r.str();                       // "(Version: 00.2024.06.2000)"
  h.version = r.u32();                             // GameInfo.version (packed Version)
  h.seed = r.u32();                                // GameInfo.seed -> game_random, package XOR key, padding RNG
  h.checksumDeep = r.i32();
  h.checksumWindowSize = r.i32();
  h.checksumFailureThreshold = r.i32();
  h.flags = r.u32();
  h.setup = {};
  for (const f of SETUP_FIELDS) h.setup[f] = r.u8();   // 29 bytes in a loop + 'mods'
  h.players = [];
  for (let i = 0; i < 8; i++) {
    r.tag('Player', TAG.Player);
    const flags = r.u16();
    const p = { slot: i, flags };
    if (flags & 1) {
      const raw = r.bytes(0x39);                     // Player bytes 0x00..0x38
      p.synced = [];
      for (let k = 0; k < 12; k++) p.synced.push(raw.readInt32LE(4 * k));
      p.tribe = raw[0x32]; p.who = raw[0x33]; p.team = raw.readInt8(0x34); p.handicap = raw[0x35];
      p.play = raw[0x36]; p.pauses = raw[0x37]; p.diff = raw[0x38];
      p.name = r.str();
    }
    h.players.push(p);
  }
  // sGameSaveVersion >= 0x10 branch (playback forces 0x10).
  h.scenarioWorkshopId = r.u64hex();
  h.scenarioScript = r.str();
  h.scenarioPath = r.str();
  h.scenarioDir = r.str();
  h.modWorkshopId = r.u64hex();
  h.modPath = r.str();
  return h;
}

// The rest of Game::walk_data, then (replay header only) String save_name. A save's Game::walk_data stops after graphic_tick.
export function readHeaderTail(r, h, withSaveName = true) {
  // Game: [frame, balance) = Game+0x550..0x6e4
  const g = r.bytes(0x194);
  h.frame = g.readInt32LE(0x00);
  h.tick = g.readInt32LE(0x10);
  h.numPlayers = g.readInt32LE(0x69c - 0x550);
  // semaphore: BitMask<256> {bits, size} then size bytes
  const bits = r.i32(), size = r.i32();
  if (size < 0 || size > 64) throw new Error(`bad semaphore size ${size}`);
  h.semaphore = Buffer.from(r.bytes(size));
  h.semaphoreBits = bits;
  h.graphicTick = r.i32();
  h.saveName = withSaveName ? r.str() : '';
  const sem = (k) => (h.semaphore[SEM[k][0]] & SEM[k][1]) !== 0;
  h.multiplayer = sem('MULTIPLAYER');
  h.conquest = sem('CONQUEST');
  return h;
}

// ---------------------------------------------------------------------------------------------------------------
// Rules: Game::walk_rules_data (only when the game did not start from a save)

// Types list order (TypeIndex ranges, re/types/typeindex.tsv) and the class whose walk_rules_data runs.
const TYPE_RANGES = [
  [0, 50, 'GoodType'], [50, 414, 'UnitType'], [414, 543, 'BuildType'], [543, 544, 'ItemType'],
  [544, 629, 'TechType'], [629, 684, 'SpellType'], [684, 806, 'BonusType'],
];

function readSimpleArrayU16(r) {          // SimpleArray<unsigned short>::walk_data
  const n = r.u32();
  if (n) { r.u32(); r.u16(); r.u8(); r.bytes(2 * n); }
  return n;
}

export function readRules(r) {
  const out = { start: r.p, types: [] };
  r.tag('Game Rules', TAG.GameRules);
  for (const [lo, hi, cls] of TYPE_RANGES) {
    for (let i = lo; i < hi; i++) {
      r.bytes(0x5a);                                // Type +0x04..0x5e
      const name = r.str();                         // Type +0x74 (name)
      if (cls === 'GoodType' || cls === 'UnitType' || cls === 'BuildType' || cls === 'ItemType') {
        r.bytes(0x98);                              // ObjectType +0x1e4..0x27c
        readSimpleArrayU16(r); readSimpleArrayU16(r);   // +0x27c, +0x298
        if (cls === 'GoodType') r.bytes(0x44);      // +0x2b4..0x2f8
        if (cls === 'UnitType') r.bytes(0x18 + 8 + 4 + 0x2f4);   // +0x2b4..0x2cc, 0x2d4..0x5d4
        if (cls === 'BuildType') r.bytes(0x31);     // +0x2b4..0x2e5
      } else if (cls === 'TechType') {
        r.bytes(0x1b);                              // +0x1c8..0x1e3
        for (let k = 0; k < 8; k++) r.str();        // 8 Strings at +0x1e4
      } else if (cls === 'SpellType') {
        r.bytes(0x30);                              // +0x1c8..0x1f8
      }
      out.types.push({ index: i, cls, name });
    }
  }
  r.bytes(0xd40);                                   // Constants [0, curr_element)
  r.bytes(4);                                       // Constants [mongol_three_mil_cavalry, iroquois_los)
  r.bytes(0x1ed * 0x1ed * 2);                       // Balance table 493 x 493 u16
  for (let t = 0; t < 24; t++) { r.tag('Tribe rules', TAG.TribeRules); r.bytes(0x18); r.bytes(0x580); }
  out.end = r.p;
  return out;
}

// RecordGame::write_random_game_info: per player of the *running* game (team (1 byte) + leader field +0x0c (4 bytes)),
// then World+0x30 (4 bytes). The header's player list is written earlier and is NOT always the same set: in games where
// players are added after the header (lobby/custom AI games) the block has more entries than the header has flags&1
// players (observed: 2 header players, 5 or 8 entries). The count is therefore probed (see rgiCount).
function rgiSize(h) { return 5 * h.players.filter((p) => (p.flags & 1) && p.who < 8).length + 4; }
// Number of 5-byte entries for a block starting at `p` that is followed by the 0x92 rules tag (no embedded save).
export function rgiCount(buf, p, h) {
  const expect = h.players.filter((q) => (q.flags & 1) && q.who < 8).length;
  const order = [expect, ...[...Array(9).keys()].filter((n) => n !== expect)];
  for (const n of order) if (p + 5 * n + 4 < buf.length && buf[p + 5 * n + 4] === TAG.GameRules) return n;
  return expect;
}
export function readRandomGameInfo(r, h, n) {
  if (n === undefined) n = h.players.filter((p) => (p.flags & 1) && p.who < 8).length;
  const entries = [];
  for (let i = 0; i < n; i++) entries.push({ slot: i, team: r.i8(), leaderValue: r.i32() });
  return { entries, world30: r.i32() };
}

// ---------------------------------------------------------------------------------------------------------------
// Commands (CommandTypes enum; layouts = packed *Command structs from rise.pdb; lengths = process_* return values)

const Q = 'queue';   // QueuePos as i32: 0 FIRST, 1 LAST, 2 NEW
const QUEUE = ['FIRST', 'LAST', 'NEW'];
const ORDERS = ['NONE', 'MOVE_TO', 'ATTACK_TO', 'EXPLORE_TO', 'FLEE_TO', 'PATROL', 'BUILD_AT', 'GATHER', 'BOARD_SHIP',
  'AWAIT_BOARD', 'ATTACK', 'FOLLOW', 'GUARD', 'REPAIR', 'CAST_SPELL', 'TRADE_ROUTE', 'STRAFE', 'AIR_PATROL', 'CHANGE_FORM',
  'GROUP_MOVE', 'GROUP_ATTACK', 'GROUP_ATTACK_TO', 'GROUP_PATROL', 'ATTACK_GROUND', 'AIR_ATTACK_GROUND', 'SPECIAL_ANIM',
  'GARRISON', 'THINK'];

// [opcode, name, fields]  field = [name, type]; types: i32 u32 f32 i16 u16 u8 i8 queue, or special handling.
export const VERBS = [
  [0, 'GROUP', 'special'],
  [1, 'BEGIN', []],
  [2, 'STANCE', [['stance', 'i32']]],
  [3, 'FORM', [['form', 'i32'], ['rotate', 'i32'], ['queued', Q]]],
  [4, 'ATTACK', [['ox', 'i32'], ['whom', 'i32'], ['ignore', 'i32'], ['queued', Q]]],
  [5, 'SIEGE_ATTACK', [['ox', 'i32'], ['whom', 'i32'], ['queued', Q]]],
  [6, 'SWARM_AROUND', [['ox', 'i32'], ['whom', 'i32'], ['queued', Q], ['orders', 'i32']]],
  [7, 'MOVE_TO', [['to_x', 'i32'], ['to_y', 'i32'], ['set_angle', 'i32'], ['angle', 'i32'], ['orders', 'i8'], ['queued', 'i8'], ['form', 'i8'], ['width', 'i8'], ['disembark', 'i8']]],
  [8, 'MOVE_NEAR', [['to_x', 'i32'], ['to_y', 'i32'], ['tolerance', 'i32'], ['set_angle', 'i32'], ['angle', 'i32'], ['orders', 'i8'], ['queued', 'i8'], ['form', 'i8'], ['width', 'i8'], ['disembark', 'i8']]],
  [9, 'ATTACK_GROUND', [['to_x', 'i32'], ['to_y', 'i32'], ['queued', 'i8']]],
  [10, 'PATROL', [['to_x', 'i32'], ['to_y', 'i32'], ['queued', 'i8']]],
  [11, 'LAUNCH_PATROL', [['to_x', 'i32'], ['to_y', 'i32'], ['queued', Q], ['shift', 'i32'], ['ctrl', 'i32'], ['alt', 'i32']]],
  [12, 'HALT', []],
  [13, 'TRANSPORT', []],
  [14, 'SET_TRANSPORT', [['flag', 'i32']]],
  [15, 'BOARD_SHIP', [['ox', 'i32'], ['queued', Q]]],
  [16, 'REPAIR', [['ox', 'i32'], ['whom', 'i32'], ['queued', Q]]],
  [17, 'TRADE', [['ox', 'i32'], ['whom', 'i32'], ['oxx', 'i32'], ['whose', 'i32'], ['queued', Q]]],
  [18, 'CITY_GATHER', [['t', 'i32'], ['queued', Q]]],
  [19, 'GATHER', [['ox', 'i32'], ['queued', Q]]],
  [20, 'GARRISON', [['ox', 'i32'], ['whom', 'i32'], ['queued', Q]]],
  [21, 'DISBAND', [['all', 'i32']]],
  [22, 'GATHER_POINT', [['x', 'i32'], ['y', 'i32'], ['action', 'i32'], ['add_to_end', 'i32']]],
  [23, 'SPELL', [['ox', 'i32'], ['whom', 'i32'], ['type', 'i32'], ['x', 'i32'], ['y', 'i32']]],
  [24, 'QUEUE_UP', [['type', 'i32'], ['num', 'i32']]],
  [25, 'BUILD', [['x', 'i32'], ['y', 'i32'], ['x2', 'i32'], ['y2', 'i32'], ['type', 'i32'], ['queued', Q]]],
  [26, 'EJECTALL', [['back_to_work', 'i32'], ['who', 'i32'], ['eject_o', 'i32'], ['eject_who', 'i32']]],
  [27, 'ALARM', []],
  [28, 'FLIGHT', [['ox', 'i32'], ['whom', 'i32'], ['shift', 'i32'], ['ctrl', 'i32'], ['alt', 'i32'], ['orders', 'i32']]],
  [29, 'STOP_SPELL', []],
  [30, 'FOLLOW', [['ox', 'i32'], ['whom', 'i32'], ['queued', Q]]],
  [31, 'GUARD', [['ox', 'i32'], ['whom', 'i32'], ['queued', Q]]],
  [32, 'UNITMASK', [['unitmask', 'i32'], ['set', 'i32']]],
  [33, 'BUILDMASK', [['buildmask', 'i32'], ['set', 'i32']]],
  [34, 'HOTKEY', [['group', 'i32'], ['clear', 'i32'], ['valid', 'i32'], ['x', 'f32'], ['y', 'f32'], ['zoom', 'i32']]],
  [35, 'RECALL', []],
  [36, 'SCRAMBLE', []],
  [37, 'TREATY', [['who', 'i32'], ['whom', 'i32'], ['treaty', 'i32']]],
  [38, 'DECLARE', [['who', 'i32'], ['whom', 'i32'], ['treaty', 'i32']]],
  [39, 'CLEAR_TRIBUTES', [['who', 'i32'], ['whom', 'i32']]],
  [40, 'CLEAR_ALL', [['who', 'i32'], ['whom', 'i32']]],
  [41, 'ACCEPT', [['who', 'i32'], ['whom', 'i32']]],
  [42, 'REJECT', [['who', 'i32'], ['whom', 'i32']]],
  [43, 'TRIBUTE', [['who', 'i32'], ['whom', 'i32'], ['good', 'i32'], ['amount', 'i32']]],
  [44, 'DEMAND_TRIBUTE', [['who', 'i32'], ['whom', 'i32'], ['good', 'i32'], ['amount', 'i32']]],
  [45, 'PROPOSE_ATTACK', [['who', 'i32'], ['whom', 'i32'], ['whose', 'i32'], ['onoff', 'i32']]],
  [46, 'BUY', [['who', 'i32'], ['good', 'i32'], ['flags', 'i32']]],
  [47, 'SELL', [['who', 'i32'], ['good', 'i32'], ['flags', 'i32']]],
  [48, 'UNQUEUE', [['who', 'i32'], ['o', 'i32'], ['type', 'i32'], ['uid', 'i16']]],
  [49, 'COME_OUT', [['who', 'i32'], ['o', 'i32'], ['uid', 'i16']]],
  [50, 'PING', [['x', 'i32'], ['y', 'i32']]],
  [51, 'SPLINE', 'special'],
  [52, 'SPEED_SET', [['speed', 'i32']]],
  [53, 'SPEED_UP', []],
  [54, 'SPEED_DOWN', []],
  [55, 'MP_LOG', []],
  [56, 'CHECK_RANDOM', [['seed', 'u32']]],
  [57, 'CHECK_SUMS', [['units', 'u32'], ['builds', 'u32'], ['walls', 'u32'], ['ammo', 'u32'], ['deaths', 'u32'], ['groups', 'u32'],
    ['guys', 'u32'], ['leaders', 'u32'], ['cities', 'u32'], ['items', 'u32'], ['goods', 'u32'], ['world', 'u32'], ['rules', 'u32'],
    ['scenario_data', 'u32'], ['script_run_time', 'u32'], ['all', 'u32']]],
  [58, 'NEXT_CHECK_SUM', [['checksum_type', 'u8'], ['checksum', 'u32']]],
  [59, 'CHEAT_VIEW_ALL', [['who', 'i32']]],
  [60, 'CHEAT_GIVE_TECHS', [['who', 'i32']]],
  [61, 'CHEAT_ZERO_TECHS', [['who', 'i32']]],
  [62, 'CHEAT_AI_SPEED_INCREASE', []],
  [63, 'CHEAT_AI_SPEED_NORMAL', []],
  [64, 'CHEAT_AI_TOGGLE', []],
  [65, 'CHEAT_INCREASE_BUCKETS', [['who', 'i32']]],
  [66, 'CHEAT_ZERO_BUCKETS', [['who', 'i32']]],
  [67, 'CHEAT_INIT_UNIT', [['who', 'i32'], ['t', 'i32'], ['x', 'i32'], ['y', 'i32']]],
  [68, 'CHAT', 'special'],
  [69, 'CHAT_SET', [['status', 'u8x8']]],
  [70, 'RESIGN', [['play', 'i32']]],
  [71, 'QUIT', [['play', 'i32'], ['replay', 'u8'], ['system_quit', 'u8']]],
  [72, 'CAMERA', [['zoom', 'u8'], ['x_loc', 'i32'], ['y_loc', 'i32']]],
  [73, 'LEADER_OPTIONS', [['who', 'i32'], ['peasants', 'i32'], ['peasants_wait', 'i32'], ['buildings', 'i32'],
    ['flags_bits', 'i32'], ['flags_size', 'i32'], ['flags_flags', 'i32'], ['flags', 'u32']]],
  [74, 'TURN_DATA', [['ping_time', 'u16'], ['frame_average', 'u16'], ['wait_time', 'u16'], ['game_lag', 'u16'], ['forced_loads', 'u16']]],
  [75, 'RENAME_CITY', [['who', 'i32'], ['o', 'i32'], ['name', 'wchar22']]],
  [76, 'PAUSE', [['state', 'u8']]],
  [77, 'CANNON_TIME', [['state', 'u8']]],
  [78, 'CONSOLE_CMD', [['mouse_x', 'i32'], ['mouse_y', 'i32'], ['cmd', 'wchar256']]],
  [79, 'PLAYER_SPEED', [['frames_zoomed_in', 'u8'], ['frames_zoomed_out', 'u8'], ['clicks', 'u8'], ['hotkeys', 'u8'],
    ['minimap_clicks', 'u8'], ['mainmap_clicks', 'u8'], ['control_groups_formed', 'u8'], ['control_groups_activated', 'u8']]],
  [80, 'UNGRACEFUL_PLAYER_DROP', [['play', 'u8'], ['state', 'u8']]],
  [81, 'MARWAN', [['start', 'u8']]],
];
export const VERB_NAME = Object.fromEntries(VERBS.map(([op, n]) => [op, n]));
const TYPE_SIZE = { i32: 4, u32: 4, f32: 4, i16: 2, u16: 2, u8: 1, i8: 1, [Q]: 4, u8x8: 8, wchar22: 44, wchar256: 512 };
export function verbLength(op) {
  const v = VERBS[op];
  if (!v || v[2] === 'special') return null;
  return 1 + v[2].reduce((s, [, t]) => s + TYPE_SIZE[t], 0);
}

function wstr(buf) { let s = buf.toString('utf16le'); const z = s.indexOf('\0'); return z >= 0 ? s.slice(0, z) : s; }

// Decode one command at r.p; returns {op, name, len, args}.
function decodeCommand(r) {
  const at = r.p;
  const op = r.u8();
  const v = VERBS[op];
  if (!v) throw new Error(`unknown command 0x${op.toString(16)} at +${at}`);
  const [, name, fields] = v;
  const args = {};
  if (op === 0) {                                   // GROUP: u8 num, i8 who, num x u16 object index (num 0 = repeat last)
    args.num = r.u8(); args.who = r.i8();
    args.objects = [];
    for (let i = 0; i < args.num; i++) args.objects.push(r.u16());
    if (args.num === 0) args.repeat = true;
  } else if (op === 51) {                           // SPLINE: u8 type, u8 flags, u8 cmd, u16 len, len x {f32 x, f32 y}
    args.spline_type = r.u8(); args.spline_flags = r.u8(); args.spline_cmd = r.u8();
    const n = r.u16(); args.verts = [];
    for (let i = 0; i < n; i++) args.verts.push([r.f32(), r.f32()]);
  } else if (op === 68) {                           // CHAT: i32 bits, i32 taunt, i32 taunt_num, i32 len, wchar[len] + NUL
    args.bits = r.i32(); args.taunt = r.i32(); args.taunt_num = r.i32();
    const n = r.i32(); if (n < 0 || n > 240) throw new Error(`bad chat length ${n}`);
    args.text = wstr(r.bytes(2 * n + 2));
  } else {
    for (const [f, t] of fields) {
      switch (t) {
        case 'i32': args[f] = r.i32(); break;
        case 'u32': args[f] = r.u32(); break;
        case 'f32': args[f] = r.f32(); break;
        case 'i16': args[f] = r.i16(); break;
        case 'u16': args[f] = r.u16(); break;
        case 'u8': args[f] = r.u8(); break;
        case 'i8': args[f] = r.i8(); break;
        case Q: { const q = r.i32(); args[f] = QUEUE[q] ?? q; break; }
        case 'u8x8': args[f] = [...r.bytes(8)]; break;
        case 'wchar22': args[f] = wstr(r.bytes(44)); break;
        case 'wchar256': args[f] = wstr(r.bytes(512)); break;
        default: throw new Error(`type ${t}`);
      }
    }
    if ((op === 7 || op === 8) && typeof args.orders === 'number') args.orders = ORDERS[args.orders] ?? args.orders;
    if (op === 28 && typeof args.orders === 'number') args.orders = ORDERS[args.orders] ?? args.orders;
  }
  return { op, name, len: r.p - at, args };
}

// Same LCG as Random::get(lo, hi) (basic:random.obj).
class Random {
  constructor(seed) { this.s = seed >>> 0; }
  next() { this.s = (Math.imul(this.s, 0x19660d) + 0x3c6ef35f) >>> 0; return this.s; }
  get(lo, hi) { if (lo === hi) return lo; if (hi < lo) [lo, hi] = [hi, lo]; const s = this.next(); return lo + (((s & 0xffff) * (hi - lo)) >>> 16); }
}

// Mirror of CommandPackage::process_all: undo the MP obfuscation, then walk the commands, skipping the per-command
// random padding byte(s).
export function decodePackage(data, h) {
  const mp = h.multiplayer;
  let buf = Buffer.from(data);
  if (mp) {
    // process_all skips the XOR only for MP playback of an UNGRACEFUL_PLAYER_DROP package ('P', size <= 4).
    const isDrop = buf.length !== 0 && buf[0] === 0x50 && buf.length <= 4;
    if (!isDrop) {
      const key = (h.seed >>> 8) & 0xffff;
      for (let i = 0; i + 1 < buf.length; i += 2) buf.writeUInt16LE(buf.readUInt16LE(i) ^ key, i);
    }
  }
  const pad = mp ? new Random(h.seed) : null;
  const r = new Reader(buf);
  const cmds = [];
  while (r.p < buf.length) {
    const c = decodeCommand(r);
    if (pad) { const n = pad.get(0, 2); if (n) c.pad = n; r.p += n; }
    cmds.push(c);
  }
  if (r.p !== buf.length) throw new Error(`package overrun: ${r.p} > ${buf.length}`);
  return { plain: buf, cmds };
}

// RecordGame::write_package record: u32 frame, i32 play, i32 valid, u32 stamp, i16 size, u8 data[size].
function readRecord(r) {
  const at = r.p;
  const frame = r.i32(), play = r.i32(), valid = r.i32(), stamp = r.u32(), size = r.i16();
  if (size < 0 || size > 0x200) throw new Error(`bad package size ${size} at ${at}`);
  const data = r.bytes(size);
  return { at, frame, play, valid, stamp, size, data };
}

// Try to parse the command stream from `pos` to EOF; returns records or throws. A game that crashed leaves a plain
// (uncompressed) file whose last record is cut short; that partial record is reported in `recs.truncated`
// ({at, have, need}) instead of failing.
function readStream(buf, pos, h, strict = true, minFrame = h.frame) {
  const r = new Reader(buf, pos);
  const recs = [];
  let lastFrame = minFrame;   // records are written during play, which starts at the loaded game's frame
  while (r.left > 0) {
    const at = r.p;
    if (r.left < 18) { recs.truncated = { at, have: r.left, need: 18 }; break; }
    const size = buf.readInt16LE(at + 16);
    if (size >= 0 && size <= 0x200 && at + 18 + size > buf.length) { recs.truncated = { at, have: r.left, need: 18 + size }; break; }
    const rec = readRecord(r);
    if (strict) {
      if (rec.play < 0 || rec.play > 7) throw new Error(`bad play ${rec.play} at ${rec.at}`);
      if (rec.size < 1) throw new Error(`empty package at ${rec.at}`);
      if (rec.frame < lastFrame) throw new Error(`frame ${rec.frame} before ${lastFrame} at ${rec.at}`);
    }
    lastFrame = rec.frame;
    const d = decodePackage(rec.data, h);
    rec.cmds = d.cmds;
    if (!h.multiplayer) delete rec.plain; else rec.plain = d.plain;
    recs.push(rec);
  }
  return recs;
}

// Frame of the game stored in a save, read from the start of WalkDataGame::walk_data (GameInfo, 182 raw bytes of Console and
// object ranges, then Game::walk_data whose first raw field is Game.frame). `at` points just after magic + version.
export function embeddedSaveFrame(buf, at) {
  try {
    const r = new Reader(buf, at);
    const gi = readGameInfo(r); void gi;
    r.bytes(182);
    r.tag('Game', TAG.Game);
    return readHeaderTail(r, readGameInfo(r), false).frame;
  } catch { return null; }
}

// Embedded save: String magic ("RoNSave" / "RoNMultiSave" / "RoNCTWSave"), u32 sGameSaveVersion, then
// WalkDataGame::walk_data (not decoded here). The command stream is located by scanning for the earliest offset
// whose records chain exactly to EOF and which is preceded by the random-game-info block whose team bytes match
// the header.
function locateStreamAfterSave(buf, start, h, minFrame) {
  const expect = h.players.filter((p) => (p.flags & 1) && p.who < 8).length;
  const teams = h.players.filter((p) => (p.flags & 1) && p.who < 8).map((p) => p.team);
  const counts = [expect, ...[...Array(9).keys()].filter((n) => n !== expect)];
  for (let o = start; o <= buf.length; o++) {
    // cheap prefilter on the first record header (a record cut short by a crash is accepted by readStream)
    if (o + 18 <= buf.length) {
      const play = buf.readInt32LE(o + 4), size = buf.readInt16LE(o + 16);
      if (play < 0 || play > 7 || size < 1 || size > 0x200) continue;   // size 0 never occurs; rejecting it also stops zero runs in the save from chaining
    }
    for (const n of counts) {
      const rgiAt = o - (5 * n + 4);
      if (rgiAt < start) continue;
      let ok = true;
      if (n === expect) { for (let i = 0; i < teams.length; i++) if (buf.readInt8(rgiAt + 5 * i) !== teams[i]) { ok = false; break; } }
      else for (let i = 0; i < n; i++) { const t = buf.readInt8(rgiAt + 5 * i), l = buf.readInt32LE(rgiAt + 5 * i + 1); if (t < -1 || t > 8 || l < 0 || l > 255) { ok = false; break; } }
      if (!ok) continue;
      try {
        const recs = readStream(buf, o, h, true, minFrame);
        if (recs.length === 0 && !recs.truncated) continue;
        return { pos: o, recs, rgiAt, n };
      } catch { /* keep scanning */ }
    }
  }
  return null;
}

// ---------------------------------------------------------------------------------------------------------------

export function loadReplay(file) {
  let buf = fs.readFileSync(file);
  let compressed = false;
  if (buf[0] === 0x1f && buf[1] === 0x8b) { buf = zlib.gunzipSync(buf); compressed = true; }
  return { buf, compressed };
}

export function decodeReplay(buf) {
  const r = new Reader(buf);
  const h = readHeader(r);
  const out = { header: h, headerEnd: r.p, warnings: [] };
  if (h.conquest) {
    out.warnings.push('Conquer the World replay: ConquestGame::walk_data header follows and is not decoded; stream located by scan');
  }
  const savePresent = h.saveName.length > 0;
  if (savePresent && !h.conquest) {
    const sr = new Reader(buf, r.p);
    out.save = { at: r.p, magic: sr.str(), version: sr.u32(), dataAt: sr.p };
    out.save.frame = embeddedSaveFrame(buf, sr.p);   // frame of the game inside the embedded save (may differ from the header's)
    const loc = locateStreamAfterSave(buf, sr.p, h, out.save.frame ?? h.frame);
    if (!loc) throw new Error('could not locate the command stream after the embedded save');
    out.save.size = loc.rgiAt - out.save.at;
    const rr = new Reader(buf, loc.rgiAt);
    out.randomGameInfo = readRandomGameInfo(rr, h, loc.n);
    out.streamAt = loc.pos; out.records = loc.recs;
    out.located = 'scan';
  } else if (h.conquest) {
    const loc = locateStreamAfterSave(buf, r.p, h, h.frame);
    if (!loc) throw new Error('could not locate the command stream');
    out.streamAt = loc.pos; out.records = loc.recs; out.located = 'scan';
  } else {
    out.randomGameInfo = readRandomGameInfo(r, h, rgiCount(buf, r.p, h));   // present when GameInfo.version > 03.02.03.2905 (all EE builds)
    const rules = readRules(r);
    out.rules = { at: rules.start, size: rules.end - rules.start, types: rules.types.length,
      sample: rules.types.filter((t, i) => [0, 50, 414, 543, 544, 629, 684].includes(i)).map((t) => `${t.index}:${t.cls}:${t.name}`) };
    out.streamAt = r.p;
    out.records = readStream(buf, r.p, h);
    out.located = 'exact';
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Output

function fmtArgs(c) {
  const a = c.args;
  if (c.op === 0) return a.repeat ? `who=${a.who} (repeat last group)` : `who=${a.who} n=${a.num} [${a.objects.join(',')}]`;
  if (c.op === 51) return `type=${a.spline_type} flags=${a.spline_flags} cmd=${a.spline_cmd} verts=${a.verts.length}`;
  return Object.entries(a).map(([k, v]) => `${k}=${typeof v === 'string' ? JSON.stringify(v) : Array.isArray(v) ? `[${v.join(',')}]` : typeof v === 'number' && !Number.isInteger(v) ? v.toFixed(3) : v}`).join(' ');
}

function printSummary(file, d, compressed, opts) {
  const h = d.header;
  const log = console.log;
  log(`${file}${compressed ? ' (gzip)' : ''}`);
  log(`  ${h.versionString}  version=0x${h.version.toString(16)}  seed=${h.seed} (0x${h.seed.toString(16)})`);
  log(`  checksum deep=${h.checksumDeep} window=${h.checksumWindowSize} threshold=${h.checksumFailureThreshold}  info.flags=0x${h.flags.toString(16)}`);
  log(`  mode: ${h.multiplayer ? 'MULTIPLAYER' : 'single player'}${h.conquest ? ' CONQUEST' : ''}  semaphore=${h.semaphore.subarray(0, 4).toString('hex')}  frame=${h.frame} tick=${h.tick}`);
  log(`  setup: ${Object.entries(h.setup).map(([k, v]) => `${k}=${v}`).join(' ')}`);
  for (const p of h.players) {
    if (!(p.flags & 1)) continue;
    log(`  player ${p.slot}: ${JSON.stringify(p.name)} flags=0x${p.flags.toString(16)} tribe=${p.tribe} who=${p.who} team=${p.team} handicap=${p.handicap} play=${p.play} diff=${p.diff}`);
  }
  if (h.scenarioPath || h.scenarioScript || h.modPath) log(`  scenario=${JSON.stringify(h.scenarioPath)} script=${JSON.stringify(h.scenarioScript)} mod=${JSON.stringify(h.modPath)}`);
  log(`  save_name=${JSON.stringify(h.saveName)}  header ends @${d.headerEnd}`);
  if (d.save) log(`  embedded save @${d.save.at}: ${JSON.stringify(d.save.magic)} version=${d.save.version} size=${d.save.size} game frame=${d.save.frame}${d.save.frame !== h.frame ? ` (header says ${h.frame})` : ''}`);
  if (d.records.truncated) log(`  warning: truncated last record @${d.records.truncated.at} (${d.records.truncated.have} of ${d.records.truncated.need} bytes): the game crashed or was killed while recording`);
  if (d.randomGameInfo) log(`  random game info: ${d.randomGameInfo.entries.map((e) => `p${e.slot}:team=${e.team},leader+0c=${e.leaderValue}`).join(' ')} world+30=${d.randomGameInfo.world30}`);
  if (d.rules) log(`  rules @${d.rules.at}: ${d.rules.size} bytes, ${d.rules.types} types (${d.rules.sample.join(', ')})`);
  for (const w of d.warnings) log(`  warning: ${w}`);
  const recs = d.records;
  const counts = {};
  let ncmd = 0;
  for (const rec of recs) for (const c of rec.cmds) { counts[c.name] = (counts[c.name] ?? 0) + 1; ncmd++; }
  const last = recs.length ? recs[recs.length - 1].frame : 0;
  log(`  command stream @${d.streamAt} (${d.located}): ${recs.length} packages, ${ncmd} commands, last frame ${last} (~${(last / 15).toFixed(0)} ticks)`);
  log(`  verbs: ${Object.entries(counts).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}=${v}`).join(' ')}`);
  if (opts.summary) return;
  for (const rec of recs) {
    log(`  @${rec.at} frame=${rec.frame} play=${rec.play} valid=${rec.valid} stamp=${rec.stamp} size=${rec.size}`);
    if (opts.hex) log(`      ${Buffer.from(rec.plain ?? rec.data).toString('hex')}`);
    for (const c of rec.cmds) log(`      ${c.name.padEnd(16)} ${fmtArgs(c)}${c.pad ? `  (+${c.pad} pad)` : ''}`);
  }
}

// One survey row per replay (no player names): see docs/oracle-corpus.md.
export function describeReplay(file) {
  const { buf, compressed } = loadReplay(file);
  const d = decodeReplay(buf);
  const h = d.header, recs = d.records;
  const sums = [];
  for (const rec of recs) for (const c of rec.cmds) if (c.op === 57) sums.push({ frame: rec.frame, play: rec.play, v: Object.values(c.args) });
  const players = h.players.filter((p) => p.flags & 1).length;
  return {
    file: path.basename(file), gz: compressed, bytes: buf.length,
    mode: h.multiplayer ? 'MP' : 'SP', teamStyle: h.setup.team_style, mapStyle: h.setup.map_style, mapSize: h.setup.map_size,
    players, slots: d.randomGameInfo ? d.randomGameInfo.entries.length : players,
    scenarioType: h.setup.scenario_type, scenario: !!h.scenarioPath, script: h.scenarioScript, ctw: h.conquest,
    save: d.save ? d.save.size : 0, startFrame: d.save?.frame ?? h.frame, headerFrame: h.frame, firstFrame: recs.length ? recs[0].frame : null,
    pkgs: recs.length, cmds: recs.reduce((n, r) => n + r.cmds.length, 0),
    nSums: sums.length, firstSum: sums.length ? sums[0].frame : null, lastSum: sums.length ? sums.at(-1).frame : null,
    lastFrame: recs.length ? recs.at(-1).frame : h.frame,
    located: d.located, truncated: recs.truncated ?? null, exact: d.located === 'exact' && !recs.truncated,
    seed: h.seed, sums,
  };
}

function walkRcx(dir) {
  const files = [];
  const walk = (d) => { for (const e of fs.readdirSync(d, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (/\.rcx$/i.test(e.name)) files.push(p); } };
  walk(dir);
  return files;
}

const CSV_HEAD = ['frame', 'play', 'units', 'builds', 'walls', 'ammo', 'deaths', 'groups', 'guys', 'leaders', 'cities', 'items', 'goods', 'world', 'rules', 'scenario_data', 'script_run_time', 'all'];

// opts.csv: write every CHECK_SUMS package (file,frame,play,16 x u32) to this path.
function survey(dir, opts = {}) {
  const files = walkRcx(dir);
  const rows = [];
  const csv = opts.csv ? [['file', ...CSV_HEAD].join(',')] : null;
  let ok = 0, exact = 0, trunc = 0;
  for (const f of files) {
    try {
      const x = describeReplay(f);
      const rel = path.relative(dir, f);
      rows.push({ ...x, rel });
      if (csv) for (const s of x.sums) csv.push([JSON.stringify(rel), s.frame, s.play, ...s.v].join(','));
      delete x.sums;
      const tail = x.truncated ? ` TRUNCATED(last record @${x.truncated.at}: ${x.truncated.have}/${x.truncated.need} bytes)` : '';
      console.log(`OK   ${rel}${x.gz ? '' : ' (plain)'}  ${x.mode} style=${x.mapStyle} size=${x.mapSize} players=${x.players}/${x.slots} scn=${x.scenarioType}${x.scenario ? '+scx' : ''}${x.script ? ` script=${x.script}` : ''}${x.ctw ? ' CTW' : ''} save=${x.save ? x.save : 'no'} pkgs=${x.pkgs} cmds=${x.cmds} sums=${x.nSums}${x.nSums ? `[${x.firstSum}..${x.lastSum}]` : ''} frames=${x.startFrame}..${x.lastFrame} ${x.located}${x.exact ? ' exact' : ''}${tail}`);
      ok++; if (x.exact) exact++; if (x.truncated) trunc++;
    } catch (e) { console.log(`FAIL ${path.relative(dir, f)}: ${e.message}`); }
  }
  console.log(`${ok}/${files.length} replays decoded (${exact} exact end-to-end, ${trunc} with a truncated last record)`);
  if (csv) { fs.writeFileSync(opts.csv, csv.join('\n') + '\n'); console.log(`wrote ${csv.length - 1} CHECK_SUMS rows to ${opts.csv}`); }
  if (opts.tsv) {
    const cols = ['rel', 'mode', 'mapStyle', 'mapSize', 'players', 'slots', 'scenarioType', 'scenario', 'script', 'save', 'startFrame', 'headerFrame', 'firstFrame', 'pkgs', 'nSums', 'firstSum', 'lastSum', 'lastFrame', 'located', 'exact', 'seed'];
    fs.writeFileSync(opts.tsv, [cols.join('\t'), ...rows.map((r) => cols.map((c) => r[c]).join('\t'))].join('\n') + '\n');
  }
}

function main(argv) {
  if (argv[0] === '--survey') {
    const opt = (n) => { const k = argv.indexOf(n); return k >= 0 ? argv[k + 1] : undefined; };
    return survey(argv[1], { csv: opt('--checksums'), tsv: opt('--tsv') });
  }
  const file = argv[0];
  if (!file) { console.error('usage: node re/tools/rcxdump.mjs <file.rcx> [--summary] [--hex] [--json out.json] | --survey <dir>'); process.exit(2); }
  const opts = { summary: argv.includes('--summary'), hex: argv.includes('--hex') };
  const ci = argv.indexOf('--checksums');
  if (ci >= 0) {          // single file: write its CHECK_SUMS packages as CSV
    const x = describeReplay(file);
    fs.writeFileSync(argv[ci + 1], [['file', ...CSV_HEAD].join(','), ...x.sums.map((q) => [JSON.stringify(x.file), q.frame, q.play, ...q.v].join(','))].join('\n') + '\n');
    console.log(`wrote ${x.sums.length} CHECK_SUMS rows to ${argv[ci + 1]}`);
  }
  const ji = argv.indexOf('--json');
  const { buf, compressed } = loadReplay(file);
  const d = decodeReplay(buf);
  if (ji >= 0) {
    const replacer = (k, v) => (v && v.type === 'Buffer' && Array.isArray(v.data) ? Buffer.from(v.data).toString('hex') : v);
    fs.writeFileSync(argv[ji + 1], JSON.stringify(d, replacer, 1));
  }
  printSummary(file, d, compressed, opts);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) main(process.argv.slice(2));
