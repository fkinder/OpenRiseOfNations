// Example asset edit: hue-shift a building sprite and write it back as an uncompressed 32-bit TGA.
// The alpha channel is a CODE in RoN sprites (0 empty, 145 shadow, 248 team-colour mask, 255 opaque), so it is
// copied through untouched; only RGB of opaque texels changes.
// Usage: node re/tools/mods/tga-recolor.mjs <in.tga> <out.tga> <hue-degrees>
import fs from 'node:fs';
import { parseTGA } from '../ronassets.mjs';

const [inp, out, degStr] = process.argv.slice(2);
const deg = Number(degStr ?? 180);
const { width: w, height: h, rgba } = parseTGA(fs.readFileSync(inp));

function shiftHue(r, g, b, d) {
  const t = d * Math.PI / 180, c = Math.cos(t), s = Math.sin(t);
  // Hue rotation in RGB via the YIQ-like rotation matrix.
  const m = [
    0.213 + c * 0.787 - s * 0.213, 0.715 - c * 0.715 - s * 0.715, 0.072 - c * 0.072 + s * 0.928,
    0.213 - c * 0.213 + s * 0.143, 0.715 + c * 0.285 + s * 0.140, 0.072 - c * 0.072 - s * 0.283,
    0.213 - c * 0.213 - s * 0.787, 0.715 - c * 0.715 + s * 0.715, 0.072 + c * 0.928 + s * 0.072];
  const cl = v => Math.max(0, Math.min(255, Math.round(v)));
  return [cl(m[0] * r + m[1] * g + m[2] * b), cl(m[3] * r + m[4] * g + m[5] * b), cl(m[6] * r + m[7] * g + m[8] * b)];
}

let changed = 0;
for (let i = 0; i < w * h; i++) {
  const a = rgba[i * 4 + 3];
  if (a !== 255) continue;                 // keep shadow / team-mask / empty texels as they are
  const [r, g, b] = shiftHue(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], deg);
  rgba[i * 4] = r; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b; changed++;
}

// TGA type 2 (raw), 32 bpp, top-left origin (descriptor bit 5), BGRA byte order.
const hdr = Buffer.alloc(18); hdr[2] = 2; hdr.writeUInt16LE(w, 12); hdr.writeUInt16LE(h, 14); hdr[16] = 32; hdr[17] = 0x28;
const px = Buffer.alloc(w * h * 4);
for (let i = 0; i < w * h; i++) { px[i * 4] = rgba[i * 4 + 2]; px[i * 4 + 1] = rgba[i * 4 + 1]; px[i * 4 + 2] = rgba[i * 4]; px[i * 4 + 3] = rgba[i * 4 + 3]; }
fs.writeFileSync(out, Buffer.concat([hdr, px]));
console.log(`${inp} -> ${out}: ${w}x${h}, ${changed} opaque texels hue-shifted by ${deg} deg`);
