// Draw a unit's UV layout (BH3 triangle edges) over its texture, scaled up, so a painter can see which part of the
// atlas maps to which part of the model. Usage: node re/tools/mods/uv-layout.mjs <UNIT-GRAPHIC-NAME> <out.png> [--scale 4]
import fs from 'node:fs'; import zlib from 'node:zlib';
import { parseBH3, parseTGA } from '../ronassets.mjs';
const G = (process.env.RON_GAME ?? process.env.HOME + '/Games/RiseOfNations') + '/';
const [name, out] = process.argv.slice(2); const S = Number(process.argv[process.argv.indexOf('--scale') + 1] || 4);
const files = new Map(fs.readdirSync(G + 'art').map(f => [f.toLowerCase(), G + 'art/' + f]));
const art = p => files.get(p.replace(/^\.[\\/]/, '').replace(/\\/g, '/').replace(/^art\//i, '').toLowerCase());
const tag = fs.readFileSync(G + 'Data/unit_graphics.xml', 'latin1').match(new RegExp(`<UNIT name="${name}"[^>]*>`, 'i'))?.[0];
if (!tag) throw new Error('unit graphic not found: ' + name);
const attr = a => tag.match(new RegExp(`\\b${a}="([^"]*)"`, 'i'))[1];
const mesh = parseBH3(fs.readFileSync(art(attr('model')))), tex = parseTGA(fs.readFileSync(art(attr('texture'))));
const W = tex.width * S, H = tex.height * S, px = new Uint8Array(W * H * 4);
for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) { const s = ((y / S | 0) * tex.width + (x / S | 0)) * 4, d = (y * W + x) * 4;
  const a = tex.rgba[s + 3]; px[d] = tex.rgba[s]; px[d + 1] = tex.rgba[s + 1]; px[d + 2] = tex.rgba[s + 2]; px[d + 3] = 255;
  if (a < 200) { px[d] = px[d] * 0.5 | 0; px[d + 1] = px[d + 1] * 0.5 | 0; px[d + 2] = Math.min(255, px[d + 2] * 0.5 + 100); } }  // team-colour mask tinted blue
const line = (x0, y0, x1, y1) => { const n = Math.ceil(Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0))) || 1;
  for (let i = 0; i <= n; i++) { const x = Math.round(x0 + (x1 - x0) * i / n), y = Math.round(y0 + (y1 - y0) * i / n);
    if (x >= 0 && y >= 0 && x < W && y < H) { const d = (y * W + x) * 4; px[d] = 255; px[d + 1] = 255; px[d + 2] = 0; } } };
let tris = 0;
for (const m of mesh.meshes) { const uv = m.uvs, idx = m.indices; if (!uv || !idx) continue;
  const P = i => [((uv[i * 2] % 1) + 1) % 1 * W, ((uv[i * 2 + 1] % 1) + 1) % 1 * H];
  for (let t = 0; t + 2 < idx.length; t += 3, tris++) { const a = P(idx[t]), b = P(idx[t + 1]), c = P(idx[t + 2]);
    line(...a, ...b); line(...b, ...c); line(...c, ...a); } }
const raw = Buffer.alloc((W * 4 + 1) * H); for (let y = 0; y < H; y++) { raw[y * (W * 4 + 1)] = 0; Buffer.from(px.buffer, y * W * 4, W * 4).copy(raw, y * (W * 4 + 1) + 1); }
const crc = b => zlib.crc32 ? zlib.crc32(b) : 0; const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const c = Buffer.alloc(4); c.writeUInt32BE(crc(Buffer.concat([Buffer.from(t), d]))); return Buffer.concat([l, Buffer.from(t), d, c]); };
const ih = Buffer.alloc(13); ih.writeUInt32BE(W, 0); ih.writeUInt32BE(H, 4); ih[8] = 8; ih[9] = 6;
fs.writeFileSync(out, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
console.log(`${name}: ${attr('texture')} ${tex.width}x${tex.height}, ${tris} triangles -> ${out} (${W}x${H})`);
