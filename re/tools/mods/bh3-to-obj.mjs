// Export a unit's BH3 mesh in its rest pose as Wavefront OBJ + MTL, for texture painting in Blender or any 3D tool.
// Positions are stored per node in local space; each node's vertex range is moved to world space with the rest
// transforms (same walk as render-preview.mjs). The game is +z up, which Blender also uses, so axes are kept.
// OBJ texture origin is bottom-left, BH3's is top-left: v is flipped.
// Usage: node re/tools/mods/bh3-to-obj.mjs <UNIT-GRAPHIC-NAME|path.bh3> <out.obj> [--texture path.tga]
import fs from 'node:fs'; import path from 'node:path';
import { parseBH3, quatMul, quatRotate } from '../ronassets.mjs';

const G = (process.env.RON_GAME ?? process.env.HOME + '/Games/RiseOfNations') + '/';
const [src, out] = process.argv.slice(2);
let texArg = process.argv[process.argv.indexOf('--texture') + 1]; if (process.argv.indexOf('--texture') < 0) texArg = null;
const files = new Map(fs.readdirSync(G + 'art').map(f => [f.toLowerCase(), G + 'art/' + f]));
const art = p => files.get(p.replace(/^\.[\\/]/, '').replace(/\\/g, '/').replace(/^art\//i, '').toLowerCase());

let modelPath = src, texPath = texArg;
if (!/\.bh3$/i.test(src)) {
  const tag = fs.readFileSync(G + 'Data/unit_graphics.xml', 'latin1').match(new RegExp(`<UNIT name="${src}"[^>]*>`, 'i'))?.[0];
  if (!tag) throw new Error('unit graphic not found: ' + src);
  const attr = a => tag.match(new RegExp(`\\b${a}="([^"]*)"`, 'i'))?.[1];
  modelPath = art(attr('model')); texPath ??= art(attr('texture'));
}
const model = parseBH3(fs.readFileSync(modelPath));
const mesh = model.meshes[0], n = mesh.positions.length / 4;

const compose = (A, B) => ({ q: quatMul(A.q, B.q), t: quatRotate(A.q, B.t).map((v, i) => v + A.t[i]) });
const ID = { q: [0, 0, 0, 1], t: [0, 0, 0] };
const P = new Float32Array(n * 3), N = new Float32Array(n * 3); const groups = [];
(function walk(node, parentWorld) {
  const world = compose(parentWorld, { q: node.quat, t: node.trans });
  if (node.count) groups.push({ name: node.name || 'root', first: node.first, count: node.count });
  for (let v = node.first; v < node.first + node.count && v < n; v++) {
    const p = quatRotate(world.q, [mesh.positions[v * 4], mesh.positions[v * 4 + 1], mesh.positions[v * 4 + 2]]);
    const nn = quatRotate(world.q, [mesh.normals[v * 4], mesh.normals[v * 4 + 1], mesh.normals[v * 4 + 2]]);
    for (let k = 0; k < 3; k++) { P[v * 3 + k] = p[k] + world.t[k]; N[v * 3 + k] = nn[k]; }
  }
  node.children.forEach(c => walk(c, world));
})(model.hierarchy, ID);

const base = path.basename(out, '.obj'), mtl = base + '.mtl';
const f = v => Number(v.toFixed(5));
let obj = `# ${path.basename(modelPath)} exported by re/tools/mods/bh3-to-obj.mjs\nmtllib ${mtl}\no ${base}\n`;
for (let i = 0; i < n; i++) obj += `v ${f(P[i * 3])} ${f(P[i * 3 + 1])} ${f(P[i * 3 + 2])}\n`;
for (let i = 0; i < n; i++) obj += `vt ${f(mesh.uvs[i * 2])} ${f(1 - mesh.uvs[i * 2 + 1])}\n`;
for (let i = 0; i < n; i++) obj += `vn ${f(N[i * 3])} ${f(N[i * 3 + 1])} ${f(N[i * 3 + 2])}\n`;
obj += `usemtl ${base}\n`;
// Faces grouped by the node that owns their first vertex, so Blender shows the body parts as vertex groups.
const owner = new Int32Array(n); groups.forEach((g, gi) => owner.fill(gi, g.first, g.first + g.count));
const byGroup = new Map();
for (let t = 0; t + 2 < mesh.indices.length; t += 3) {
  const a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
  const k = owner[a]; if (!byGroup.has(k)) byGroup.set(k, []);
  byGroup.get(k).push(`f ${a + 1}/${a + 1}/${a + 1} ${b + 1}/${b + 1}/${b + 1} ${c + 1}/${c + 1}/${c + 1}`);
}
for (const [k, faces] of byGroup) obj += `g ${groups[k].name.replace(/\s+/g, '_')}\n${faces.join('\n')}\n`;
fs.writeFileSync(out, obj);
const texRel = texPath ? path.relative(path.dirname(path.resolve(out)), path.resolve(texPath)) : '';
fs.writeFileSync(path.join(path.dirname(out), mtl), `newmtl ${base}\nKd 1 1 1\nKa 0 0 0\nKs 0 0 0\nd 1\nillum 1\n${texPath ? 'map_Kd ' + texRel + '\n' : ''}`);
console.log(`${modelPath} -> ${out}: ${n} vertices, ${mesh.indices.length / 3} triangles, ${groups.length} nodes${texPath ? ', texture ' + texRel : ''}`);
