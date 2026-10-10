// One Fallout map as a glTF 2.0 binary (.glb): the whole map in 3D, to open in Blender
// (File > Import > glTF 2.0) and replace piece by piece with real models, textures and animation.
//
// Usage:  node export.js <MAPNAME> [out.glb]        e.g.  node export.js JUNKENT
//
// Built by the SAME code as the viewer (build.js for the scene, geometry.js for the shapes), so
// the file is exactly what you walk through. What's inside:
//   <MAP> / Elevation_N / Floor, Roof, Walls, Scenery, Decals, Translucent
//       sprite geometry; its projective texturing baked into UVs (exact: the original camera
//       is orthographic), atlas pages as PNG with transparency, NEAREST filtering, unlit
//   <MAP> / Elevation_N / Critters, Objects, Corpses
//       one node per thing, named kind_art_hex (critter_hmlthraa_13506); a voxel body per art,
//       shared by every node that wears it -- replace the mesh once, every guard changes
//   <MAP> / Elevation_N / Dude          the vault dweller, standing at the entrance
//   <MAP> / Original_Camera             orthographic, the game's own view
//   Reference_Dude_Animation            his walk and run, frame by frame, in a row beside the map
// Units: 1 = one hex (about a metre); +Y up (Blender's importer turns it to +Z). Elevations of a
// multi-floor map are stacked 8 units apart.
//
// The file embeds the map's art: it stays in build/ (git-ignored) and is never to be shared.

'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { build } = require('./build.js');
const G = require('./geometry.js');
const { makeGltf, srgbToLinear, pngRGBA } = require('./gltf.js');

const ROOT = path.resolve(__dirname, '../../..');
const ELEVATION_GAP = 8;

const yRot = (deg) => { const a = (-deg * Math.PI) / 180 / 2; return [0, Math.sin(a), 0, Math.cos(a)]; };

function exportMap(mapName, out) {
    const { scene } = build(mapName);
    const gl = makeGltf('fallout1-ce paper3d export.js');
    const J = gl.json;
    const images = scene.images;
    const pal = Buffer.from(scene.palette, 'base64');

    // ---- atlas pages -> RGBA PNG textures, one unlit material per page (+ a translucent twin)
    J.samplers.push({ magFilter: 9728, minFilter: 9728, wrapS: 33071, wrapT: 33071 }); // NEAREST: pixel art
    const pages = scene.pages.map((pg, i) => {
        const idx = zlib.inflateSync(Buffer.from(pg.z, 'base64'));
        const rgba = Buffer.alloc(pg.w * pg.h * 4);
        for (let p = 0; p < pg.w * pg.h; p++) {
            const c = idx[p];
            if (!c) continue;
            rgba[p * 4] = pal[c * 3]; rgba[p * 4 + 1] = pal[c * 3 + 1]; rgba[p * 4 + 2] = pal[c * 3 + 2]; rgba[p * 4 + 3] = 255;
        }
        J.images.push({ name: `atlas_${i}`, mimeType: 'image/png', bufferView: gl.view(pngRGBA(pg.w, pg.h, rgba)) });
        J.textures.push({ sampler: 0, source: i });
        const base = { pbrMetallicRoughness: { baseColorTexture: { index: i }, metallicFactor: 0, roughnessFactor: 1 }, doubleSided: true, extensions: { KHR_materials_unlit: {} } };
        J.materials.push({ name: `atlas_${i}`, ...base, alphaMode: 'MASK', alphaCutoff: 0.5 });
        J.materials.push({ name: `atlas_${i}_translucent`, ...base, alphaMode: 'BLEND', pbrMetallicRoughness: { ...base.pbrMetallicRoughness, baseColorFactor: [1, 1, 1, 0.5] } });
        return { w: pg.w, h: pg.h, opaque: J.materials.length - 2, translucent: J.materials.length - 1 };
    });
    J.materials.push({ name: 'voxel', pbrMetallicRoughness: { baseColorFactor: [1, 1, 1, 1], metallicFactor: 0, roughnessFactor: 1 }, doubleSided: true, extensions: { KHR_materials_unlit: {} } });
    const voxelMaterial = J.materials.length - 1;

    // ---- voxel bodies: one mesh per model, vertices shared where position and colour agree
    const voxelMesh = new Map();
    function voxMesh(id, name) {
        if (voxelMesh.has(id)) return voxelMesh.get(id);
        const m = scene.models[id];
        const bytes = zlib.inflateSync(Buffer.from(m.z, 'base64'));
        const f = G.voxelFaces(new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength), m.n);
        const pos = [], col = [], index = [], seen = new Map();
        for (let v = 0; v < f.colour.length; v++) {
            const c = f.colour[v], sh = f.shade[v];
            const rgb = [0, 1, 2].map((q) => Math.round(255 * srgbToLinear(pal[c * 3 + q]) * sh));
            const key = `${f.pos[v * 3]},${f.pos[v * 3 + 1]},${f.pos[v * 3 + 2]},${rgb}`;
            let at = seen.get(key);
            if (at === undefined) {
                at = pos.length / 3;
                seen.set(key, at);
                pos.push(f.pos[v * 3], f.pos[v * 3 + 1], f.pos[v * 3 + 2]);
                col.push(rgb[0], rgb[1], rgb[2], 255);
            }
            f.remap = f.remap || [];
            f.remap[v] = at;
        }
        for (const i of f.index) index.push(f.remap[i]);
        J.meshes.push({
            name,
            primitives: [{
                attributes: { POSITION: gl.positions(pos), COLOR_0: gl.accessor(new Uint8Array(col), 'VEC4', 5121, 34962, { normalized: true }) },
                indices: gl.indices(index, pos.length / 3), material: voxelMaterial,
            }],
        });
        voxelMesh.set(id, J.meshes.length - 1);
        return J.meshes.length - 1;
    }

    // ---- sprite geometry of one elevation: per kind and atlas page, clipped quads with baked UVs
    function spriteMeshes(L, parent, yOffset) {
        const groups = new Map(); // kind:page -> { pos, uv, index }
        G.levelQuads(L, images, (kind, quad) => {
            const [pg, ax, ay, w, h] = images[quad.id];
            const poly = G.clipToSprite(quad, w, h);
            if (poly.length < 3) return;
            const key = kind + ':' + pg;
            if (!groups.has(key)) groups.set(key, { kind, pg, pos: [], uv: [], index: [] });
            const g = groups.get(key);
            const base = g.pos.length / 3;
            for (const v of poly) {
                g.pos.push(v.p[0], v.p[1] + yOffset, v.p[2]);
                g.uv.push((ax + v.l[0]) / pages[pg].w, (ay + v.l[1]) / pages[pg].h);
            }
            const flip = G.frontOrder(quad.corners)[1] !== 1; // keep the painted side as the front
            for (let i = 1; i + 1 < poly.length; i++) {
                if (flip) g.index.push(base, base + i + 1, base + i);
                else g.index.push(base, base + i, base + i + 1);
            }
        });
        const NAMES = { floor: 'Floor', roof: 'Roof', panel: 'Walls', card: 'Scenery', decal: 'Decals', trans: 'Translucent' };
        const byKind = new Map();
        for (const g of groups.values()) {
            if (!byKind.has(g.kind)) byKind.set(g.kind, []);
            byKind.get(g.kind).push({
                attributes: { POSITION: gl.positions(g.pos), TEXCOORD_0: gl.accessor(new Float32Array(g.uv), 'VEC2', 5126, 34962) },
                indices: gl.indices(g.index, g.pos.length / 3),
                material: g.kind === 'trans' ? pages[g.pg].translucent : pages[g.pg].opaque,
            });
        }
        for (const [kind, primitives] of byKind) {
            J.meshes.push({ name: NAMES[kind], primitives });
            gl.node({ name: NAMES[kind], mesh: J.meshes.length - 1 }, parent);
        }
    }

    // ---- the map
    const mapRoot = gl.node({ name: scene.map });
    let lowestX = Infinity;
    scene.levels.forEach((L, e) => {
        if (!L) return;
        const yOffset = e * ELEVATION_GAP;
        const level = gl.node({ name: `Elevation_${e + 1}` }, mapRoot);
        spriteMeshes(L, level, yOffset);
        for (const [tx] of L.floor) lowestX = Math.min(lowestX, tx / G.S);
        const groupOf = {};
        for (const [cx, cy, heading, model, label = 'thing'] of L.vox) {
            const kind = label.startsWith('critter') ? 'Critters' : label.startsWith('corpse') ? 'Corpses' : 'Objects';
            if (groupOf[kind] === undefined) groupOf[kind] = gl.node({ name: kind }, level);
            const p = G.W(cx, cy, 0);
            gl.node({ name: label, mesh: voxMesh(model, label.replace(/_\d+$/, '')), translation: [p[0], yOffset, p[2]], rotation: yRot(heading) }, groupOf[kind]);
        }
        if (scene.dude && scene.dude.elevation === e && scene.dude.stand) {
            const p = G.W(scene.dude.b[0], scene.dude.b[1], 0);
            gl.node({ name: 'Dude', mesh: voxMesh(scene.dude.stand.frames[0], 'dude_stand'), translation: [p[0], yOffset, p[2]], rotation: yRot(scene.dude.facing) }, level);
        }
    });

    // ---- the game's own camera: orthographic, looking down sin(theta) = sqrt(3)/4 at the entrance
    const theta = Math.asin(G.SIN);
    J.cameras.push({ name: 'Original', type: 'orthographic', orthographic: { xmag: 14, ymag: 8, znear: 0.1, zfar: 1000 } });
    gl.node({ name: 'Original_Camera', camera: 0, translation: G.TO_CAMERA.map((q) => q * 200), rotation: [Math.sin(-theta / 2), 0, 0, Math.cos(-theta / 2)] }, mapRoot);

    // ---- the dude's animation, frame by frame, laid out beside the map as reference
    if (scene.dude) {
        const ref = gl.node({ name: 'Reference_Dude_Animation', translation: [Math.min(0, lowestX) - 25, 0, 0] });
        ['walk', 'run'].forEach((kind, row) => {
            const a = scene.dude[kind];
            if (!a) return;
            a.frames.forEach((id, i) => {
                gl.node({ name: `dude_${kind}_${String(i).padStart(2, '0')}`, mesh: voxMesh(id, `dude_${kind}_${String(i).padStart(2, '0')}`), translation: [i * 2.2, 0, row * 5], rotation: yRot(90) }, ref);
            });
        });
    }

    const glb = gl.finish();
    fs.mkdirSync(path.dirname(out), { recursive: true });
    fs.writeFileSync(out, glb);
    return { bytes: glb.length, meshes: J.meshes.length, nodes: J.nodes.length, textures: J.textures.length };
}

if (require.main === module) {
    const mapName = process.argv[2];
    if (!mapName) { console.error('usage: node export.js <MAPNAME> [out.glb]'); process.exit(1); }
    const out = process.argv[3] || path.join(ROOT, 'build', 'paper3d', mapName.toUpperCase() + '.glb');
    const r = exportMap(mapName, out);
    console.log(`wrote ${out}: ${(r.bytes / 1024 / 1024).toFixed(1)} MB, ${r.meshes} meshes, ${r.nodes} nodes, ${r.textures} textures`);
}

module.exports = { exportMap };
