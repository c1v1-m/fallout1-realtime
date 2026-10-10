// SET PIECES: things the game only PAINTED -- Junktown's gate is 37 wall slices with a container's
// walls, two cars, barrels and a hose drawn into them -- swapped for a real 3D model in first
// person (build.js --doom).
//
// A set piece is found by its art names on a map. Its place comes from the game's own data: the
// ground lines under its wall slices, the two outermost runs along its long axis being its long
// walls. Its model is build/paper3d/replace/<id>.glb: the first build writes a PLACEHOLDER there
// (a box of the right size) and a <id>.blend of it to edit. Edit the .blend, Ctrl+S, rebuild: a
// .blend newer than the .glb is exported to it through Blender (headless). A .glb saved straight
// from Blender works too. Neither file is ever overwritten by a placeholder once it exists.
//
// The model's own axes: +X along the set piece's length, +Y up, +Z across it; the origin on the
// ground at its middle. Units: 1 = one hex (about 0.9 m).

'use strict';

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const { makeGltf, srgbToLinear, pngRGBA } = require('./gltf.js');

const REPLACE_DIR = path.resolve(__dirname, '../../../build/paper3d/replace');
const S = 32, KZ = S * Math.sqrt(3) / 4, KY = S * Math.sqrt(13) / 4;
const SLOPES = [0, -0.25, 0.75]; // build.js: card, axis a, axis b -- screen slope of a ground line

// mode 'textured' (the Doom build's way for a corridor): a clean box -- two walls and a ceiling --
//   covered with a TEXTURE cut from its own cleanest wall, straightened face-on and tiled, the way
//   Doom textured its walls. Every pixel is still Interplay's; none of the cars painted over the
//   other wall. The rest of its slices (cars, barrels at the ends) stand up as billboards.
// mode 'paint': its long walls stand as they were painted (thin, cut at `height` px), the map's
//   roof tiles its roof -- the cars painted into a wall smear from eye level.
// mode 'model': replaced by build/paper3d/replace/<id>.glb (below) -- real 3D, made by hand.
const SET_PIECES = [
    // innerWidth: the corridor inside, in units. The game's passage is 4 hexes (3.5 units) between
    // its walls -- "moc široký" in first person; a real container is 2.35 m inside (2.65 units).
    // The whole container narrows to it (thin walls; beside it the map is open ground anyway):
    // thick walls to keep the game's outer size ended in a flat blue slab at the entrance.
    { id: 'junktown_gate_container', map: 'JUNKENT', match: /^CARGT\d+\.FRM$/i, kind: 'container', height: 96, mode: 'textured', innerWidth: 2.65 },
];

const setPieceOf = (mapName, artPath) => {
    const file = String(artPath).split(/[\\/]/).pop();
    return SET_PIECES.find((s) => s.map === mapName.toUpperCase() && s.match.test(file)) || null;
};

// relative screen px on the ground -> world (geometry.js's W at height 0)
const ground = (sx, sy) => [sx / S, sy / KZ];

// Its long walls: the runs of wall along the axis most of its ground line follows, one run per
// hex line -- a hex COLUMN for axis b, a ROW for axis a, as snapWallRuns keys them. (Not by the
// ground line's offset: before snapping, one wall's slices spread over 50 px of it.)
const lineOf = (p, axis) => (axis === 2 ? p.tile % 200 : Math.floor(p.tile / 200));
function longRuns(parts) {
    const segs = parts.flatMap((p) => (p.segs || []).filter((s) => s.axis !== 0).map((s) => ({ ...s, p })));
    const byAxis = [1, 2].map((axis) => ({ axis, len: segs.filter((s) => s.axis === axis).reduce((n, s) => n + s.x1 - s.x0, 0) }));
    const axis = byAxis.sort((a, b) => b.len - a.len)[0].axis;
    const byLine = new Map();
    for (const s of segs.filter((q) => q.axis === axis)) {
        const key = lineOf(s.p, axis);
        if (!byLine.has(key)) byLine.set(key, { line: key, ks: [], x0: Infinity, x1: -Infinity });
        const run = byLine.get(key);
        run.ks.push(s.k);
        run.x0 = Math.min(run.x0, s.x0); run.x1 = Math.max(run.x1, s.x1);
    }
    const runs = [...byLine.values()].filter((r) => r.x1 - r.x0 >= 48); // a wall, not a stray post
    for (const r of runs) r.k = r.ks.sort((a, b) => a - b)[r.ks.length >> 1]; // the median line
    runs.sort((a, b) => a.line - b.line);
    return { axis, runs };
}

// 'paint': which slices are its long walls (standing on one of the two outermost hex lines, every
// segment along the long axis) and the rest, gathered into lumps of touching sprites (each end's
// cars and barrels)
function splitSetPiece(parts) {
    const { axis, runs } = longRuns(parts);
    const outer = new Set([runs[0].line, runs[runs.length - 1].line]);
    const onWall = (p) => p.segs && p.segs.length && p.segs.every((s) => s.axis === axis) && outer.has(lineOf(p, axis));
    const walls = parts.filter(onWall), rest = parts.filter((p) => !onWall(p));
    if (process.env.SETPIECE_DEBUG) { // SETPIECE_DEBUG=1: how each slice was read
        console.log('runs on axis', axis, runs.map((r) => `k ${r.k.toFixed(1)} x ${r.x0.toFixed(0)}..${r.x1.toFixed(0)}`).join(' | '));
        for (const p of parts) console.log(' ', String(p.name).split(/[\\/]/).pop(), onWall(p) ? 'WALL' : 'rest', (p.segs || []).map((s) => `a${s.axis} k${s.k.toFixed(1)} x${s.x0.toFixed(0)}..${s.x1.toFixed(0)}`).join(' '));
    }
    const lumps = [];
    for (const p of rest) {
        const near = lumps.filter((g) => g.some((q) => p.ulx <= q.ulx + q.w + 2 && q.ulx <= p.ulx + p.w + 2 && p.uly <= q.uly + q.h + 2 && q.uly <= p.uly + p.h + 2));
        const merged = [p, ...near.flat()];
        for (const g of near) lumps.splice(lumps.indexOf(g), 1);
        lumps.push(merged);
    }
    return { walls, lumps };
}

// Where it stands: the two outermost runs of wall along its long axis are its long walls.
function placeSetPiece(def, parts, origin) {
    const { axis, runs } = longRuns(parts), m = SLOPES[axis];
    const ends = (r) => [ground(r.x0 - origin[0], r.k + m * r.x0 - origin[1]), ground(r.x1 - origin[0], r.k + m * r.x1 - origin[1])];
    const [A0, A1] = ends(runs[0]), [B0] = ends(runs[runs.length - 1]);
    let dx = A1[0] - A0[0], dz = A1[1] - A0[1];
    const len0 = Math.hypot(dx, dz);
    dx /= len0; dz /= len0;
    const nx = -dz, nz = dx; // across
    const along = (p) => (p[0] - A0[0]) * dx + (p[1] - A0[1]) * dz;
    const allEnds = runs.flatMap(ends);
    const lo = Math.min(...allEnds.map(along)), hi = Math.max(...allEnds.map(along));
    const width = Math.abs((B0[0] - A0[0]) * nx + (B0[1] - A0[1]) * nz) || 1;
    const sideB = (B0[0] - A0[0]) * nx + (B0[1] - A0[1]) * nz >= 0 ? 1 : -1;
    const mid = (lo + hi) / 2;
    const at = [A0[0] + dx * mid + nx * sideB * width / 2, A0[1] + dz * mid + nz * sideB * width / 2];
    // (centred on the passage either way; innerWidth + two 0.15 walls replaces the game's width)
    const outer = def.innerWidth ? def.innerWidth + 0.3 : width;
    return { id: def.id, at: at.map((v) => +v.toFixed(3)), angle: +Math.atan2(dz, dx).toFixed(5), dims: [hi - lo, outer, def.height / KY].map((v) => +v.toFixed(3)), gameWidth: +width.toFixed(3), runs: runs.length };
}

// ---------------------------------------------------------------- 'textured': a clean corridor

// A run of wall straightened: for every column along it, the `height` px above its ground line,
// bottom row on the ground -- the wall face-on, still the game's pixels (RGBA; clear where empty).
function straighten(run, parts, axis, height, frameOf, pal) {
    const m = SLOPES[axis];
    const slices = parts.filter((p) => lineOf(p, axis) === run.line);
    const x0 = Math.min(...slices.map((p) => p.ulx)), x1 = Math.max(...slices.map((p) => p.ulx + p.w));
    const W = x1 - x0, H = height, rgba = Buffer.alloc(W * H * 4);
    for (const p of slices) {
        const fr = frameOf(p);
        for (let c = 0; c < fr.w; c++) {
            const x = p.ulx + c, g = Math.round(run.k + m * x);
            for (let t = 0; t < H; t++) {
                const r = g - 1 - t - p.uly;
                if (r < 0 || r >= fr.h) continue;
                const ci = fr.data[r * fr.w + c];
                if (!ci) continue;
                const o = ((H - 1 - t) * W + (x - x0)) * 4;
                rgba[o] = pal[ci * 3] * 4; rgba[o + 1] = pal[ci * 3 + 1] * 4; rgba[o + 2] = pal[ci * 3 + 2] * 4; rgba[o + 3] = 255;
            }
        }
    }
    return { W, H, rgba };
}

// The stretch of a straightened wall that repeats best: fully painted columns, and its last column
// as like its first as can be found -- one rib to the next -- so tiled it shows no seam.
function repeatingTile({ W, H, rgba }, minW = 64, maxW = 128) {
    const full = [];
    for (let x = 0; x < W; x++) {
        let n = 0;
        for (let y = 0; y < H; y++) if (rgba[(y * W + x) * 4 + 3]) n++;
        full.push(n >= H * 0.97);
    }
    const colDiff = (a, b) => {
        let d = 0;
        for (let y = 0; y < H; y++) for (let q = 0; q < 3; q++) d += Math.abs(rgba[(y * W + a) * 4 + q] - rgba[(y * W + b) * 4 + q]);
        return d / (H * 3);
    };
    let best = null;
    for (let s = 0; s < W; s++) {
        for (let w = minW; w <= maxW && s + w < W; w++) {
            let ok = true;
            for (let x = s; x <= s + w && ok; x++) ok = full[x];
            if (!ok) break; // a gap: no longer tile from this start
            const cost = colDiff(s, s + w);
            if (!best || cost < best.cost) best = { s, w, cost };
        }
    }
    if (!best) return null;
    const out = Buffer.alloc(best.w * H * 4);
    for (let y = 0; y < H; y++) rgba.copy(out, y * best.w * 4, (y * W + best.s) * 4, (y * W + best.s + best.w) * 4);
    for (let p = 3; p < out.length; p += 4) out[p] = 255; // the odd hole takes whatever colour it had
    return { W: best.w, H, rgba: out, from: best.s, seam: +best.cost.toFixed(1) };
}

// The corridor: two walls (both faces and a top edge), a ceiling just under the map's own roof
// tiles, frames round both open ends in the colour of the wall's blue foot. Model axes as above.
function texturedContainer(def, parts, placement, frameOf, pal) {
    const { axis, runs } = longRuns(parts);
    const bands = [runs[0], runs[runs.length - 1]].map((r) => straighten(r, parts, axis, def.height, frameOf, pal));
    const coverage = (b) => { let n = 0; for (let p = 3; p < b.rgba.length; p += 4) if (b.rgba[p]) n++; return n / (b.W * b.H); };
    const clean = bands.sort((a, b) => coverage(b) - coverage(a))[0]; // the wall nothing is painted over
    const tile = repeatingTile(clean);
    if (!tile) return null;
    // the frames' colour: the wall's own blue (its clearly blue pixels, averaged); the dark foot
    // of the wall read almost black
    const blues = [];
    for (let p = 0; p < tile.rgba.length; p += 4) {
        const [r, g, b] = [tile.rgba[p], tile.rgba[p + 1], tile.rgba[p + 2]];
        if (b > r + 25 && b > g + 10) blues.push([r, g, b]);
    }
    const foot = blues.length ? [0, 1, 2].map((q) => blues.reduce((s, c) => s + c[q], 0) / blues.length) : [40, 60, 110];

    const [L, Wd, H] = placement.dims, hx = L / 2, hz = Wd / 2, t = 0.15;
    const hzi = hz - t; // inner faces
    // The sprite shows this wall at a slant: 16 px per unit along it, 29 px per unit up. Laid at
    // its true length its pixels came out twice as wide as tall ("moc roztažené") -- tiled at 32
    // px per unit they are square, the ribs twice as close (still wider apart than a real one's).
    const tileLen = tile.W / 32;
    const prims = { wall: [], ceiling: [], frame: [] };
    const quad = (list, corners, n, uvs) => list.push({ corners, n, uvs });
    const wallUv = (x, y) => [(x + hx) / tileLen, 1 - y / H];
    for (const side of [-1, 1]) {
        const zo = side * hz, zi = side * hzi;
        // outer face (normal away from the corridor) and inner face (towards it)
        const face = (z, n, flip) => {
            const c = [[-hx, 0, z], [hx, 0, z], [hx, H, z], [-hx, H, z]];
            const ordered = flip ? [c[1], c[0], c[3], c[2]] : c;
            quad(prims.wall, ordered, n, ordered.map(([x, y]) => wallUv(x, y)));
        };
        face(zo, [0, 0, side], side < 0);
        face(zi, [0, 0, -side], side > 0);
        quad(prims.frame, [[-hx, H, zi], [hx, H, zi], [hx, H, zo], [-hx, H, zo]].map((p, i, a) => (side < 0 ? a[3 - i] : p)), [0, 1, 0], null);
    }
    // the ceiling: its underside carries the ribs across, its top hides under the roof tiles
    const yc = H - 0.12;
    // (the texture once across its width: stretched further, its blue foot smeared over the ceiling)
    quad(prims.ceiling, [[-hx, yc, -hzi], [hx, yc, -hzi], [hx, yc, hzi], [-hx, yc, hzi]], [0, -1, 0], [[0, 0], [L / tileLen, 0], [L / tileLen, 1], [0, 1]]);
    quad(prims.frame, [[-hx, yc + 0.1, hz], [hx, yc + 0.1, hz], [hx, yc + 0.1, -hz], [-hx, yc + 0.1, -hz]], [0, 1, 0], null);
    // frames: a post at each wall end and a lintel across each open end
    const box = ([x0, x1], [y0, y1], [z0, z1]) => {
        const F = [
            [[1, 0, 0], [[x1, y0, z0], [x1, y1, z0], [x1, y1, z1], [x1, y0, z1]]], [[-1, 0, 0], [[x0, y0, z1], [x0, y1, z1], [x0, y1, z0], [x0, y0, z0]]],
            [[0, 1, 0], [[x0, y1, z0], [x0, y1, z1], [x1, y1, z1], [x1, y1, z0]]], [[0, -1, 0], [[x0, y0, z1], [x0, y0, z0], [x1, y0, z0], [x1, y0, z1]]],
            [[0, 0, 1], [[x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]]], [[0, 0, -1], [[x1, y0, z0], [x0, y0, z0], [x0, y1, z0], [x1, y1, z0]]],
        ];
        for (const [n, c] of F) quad(prims.frame, c, n, null);
    };
    // (a post covers the wall's whole thickness: its end between the inner and outer face)
    for (const x of [-hx, hx - 0.2]) {
        box([x, x + 0.2], [0, H], [-hz - 0.05, -hzi + 0.05]);
        box([x, x + 0.2], [0, H], [hzi - 0.05, hz + 0.05]);
        box([x, x + 0.2], [H - 0.35, H], [-hz, hz]);
    }

    const gl = makeGltf('fallout1-ce paper3d setpieces.js (textured from the original art)');
    const J = gl.json;
    J.images.push({ bufferView: gl.view(pngRGBA(tile.W, tile.H, tile.rgba)), mimeType: 'image/png', name: def.id + '_wall' });
    J.samplers.push({ magFilter: 9728, minFilter: 9728, wrapS: 10497, wrapT: 33071 }); // nearest, repeat along
    J.textures.push({ source: 0, sampler: 0 });
    const unlit = { KHR_materials_unlit: {} };
    J.materials.push({ name: 'wall', pbrMetallicRoughness: { baseColorTexture: { index: 0 }, metallicFactor: 0, roughnessFactor: 1 }, doubleSided: true, extensions: unlit });
    J.materials.push({ name: 'ceiling', pbrMetallicRoughness: { baseColorTexture: { index: 0 }, baseColorFactor: [0.55, 0.55, 0.6, 1], metallicFactor: 0, roughnessFactor: 1 }, doubleSided: true, extensions: unlit });
    J.materials.push({ name: 'frame', pbrMetallicRoughness: { baseColorFactor: [...foot.map(srgbToLinear), 1], metallicFactor: 0, roughnessFactor: 1 }, doubleSided: true, extensions: unlit });
    const root = gl.node({ name: def.id, children: [] });
    ['wall', 'ceiling', 'frame'].forEach((name, mi) => {
        const pos = [], nrm = [], uv = [], idx = [];
        for (const q of prims[name]) {
            const b = pos.length / 3;
            q.corners.forEach((c, i) => { pos.push(...c); nrm.push(...q.n); uv.push(...(q.uvs ? q.uvs[i] : [0, 0])); });
            idx.push(b, b + 1, b + 2, b, b + 2, b + 3);
        }
        const attributes = { POSITION: gl.positions(pos), NORMAL: gl.accessor(new Float32Array(nrm), 'VEC3', 5126, 34962), TEXCOORD_0: gl.accessor(new Float32Array(uv), 'VEC2', 5126, 34962) };
        J.meshes.push({ name, primitives: [{ attributes, indices: gl.indices(idx, pos.length / 3), material: mi }] });
        gl.node({ name, mesh: J.meshes.length - 1 }, root);
    });
    return { glb: gl.finish(), tile };
}

// The map's own roof tiles over a set piece -- its ceiling replaces them: they were the painted
// container's roof, laid out on the square grid, and over the rebuilt box they hung askew.
// Records as build.js keeps them: [tx, ty, id], relative screen px of an 80x36 tile lifted 96 px.
// (margin 2: the painted roof overhung the rebuilt box by up to 1.75 each way at the Junktown gate)
function roofOver(place, roofs, margin = 2) {
    const [L, Wd] = place.dims, c = Math.cos(place.angle), s = Math.sin(place.angle);
    return roofs.filter(([tx, ty]) => {
        const X = (tx + 40) / S - place.at[0], Z = (ty + 18 + 96) / KZ - place.at[1];
        const lx = X * c + Z * s, lz = -X * s + Z * c;
        return Math.abs(lx) < L / 2 + margin && Math.abs(lz) < Wd / 2 + margin;
    });
}

// ---------------------------------------------------------------- the placeholder model

// A box's six faces, each shaded by where it faces (top lightest) -- unlit, so it reads the same
// in the viewer, which has no lights, and in Blender.
function boxInto(mesh, [x0, x1], [y0, y1], [z0, z1], rgb) {
    const FACES = [
        [[1, 0, 0], [[x1, y0, z0], [x1, y1, z0], [x1, y1, z1], [x1, y0, z1]], 0.72],
        [[-1, 0, 0], [[x0, y0, z1], [x0, y1, z1], [x0, y1, z0], [x0, y0, z0]], 0.72],
        [[0, 1, 0], [[x0, y1, z0], [x0, y1, z1], [x1, y1, z1], [x1, y1, z0]], 1.0],
        [[0, -1, 0], [[x0, y0, z1], [x0, y0, z0], [x1, y0, z0], [x1, y0, z1]], 0.5],
        [[0, 0, 1], [[x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]], 0.85],
        [[0, 0, -1], [[x1, y0, z0], [x0, y0, z0], [x0, y1, z0], [x1, y1, z0]], 0.85],
    ];
    for (const [n, corners, shade] of FACES) {
        const base = mesh.pos.length / 3;
        for (const c of corners) {
            mesh.pos.push(...c);
            mesh.nrm.push(...n);
            mesh.col.push(...rgb.map((v) => srgbToLinear(v) * shade));
        }
        mesh.idx.push(base, base + 1, base + 2, base, base + 2, base + 3);
    }
}

function containerPlaceholder([L, Wd, H]) {
    const parts = { Wall_left: [], Wall_right: [], Roof: [], Frames: [] };
    const mesh = () => ({ pos: [], nrm: [], col: [], idx: [] });
    const m = Object.fromEntries(Object.keys(parts).map((k) => [k, mesh()]));
    const t = 0.12, hx = L / 2, hz = Wd / 2;
    const BLUE = [51, 85, 127], RIB = [62, 101, 148], ROOF = [45, 76, 114], FRAME = [38, 58, 86];
    for (const [side, key] of [[-1, 'Wall_left'], [1, 'Wall_right']]) {
        const z = side * (hz - t / 2);
        boxInto(m[key], [-hx, hx], [0, H - t], [z - t / 2, z + t / 2], BLUE);
        // the corrugation: a rib every 0.4 on the outside
        for (let x = -hx + 0.3; x < hx - 0.2; x += 0.4) {
            const zo = side * hz;
            boxInto(m[key], [x - 0.07, x + 0.07], [0.05, H - t - 0.05], side < 0 ? [zo - 0.05, zo] : [zo, zo + 0.05], RIB);
        }
    }
    boxInto(m.Roof, [-hx, hx], [H - t, H], [-hz, hz], ROOF);
    // door frames round both open ends
    for (const x of [-hx, hx]) {
        const x0 = x < 0 ? x : x - 0.18, x1 = x0 + 0.18;
        for (const z of [-hz, hz - 0.2]) boxInto(m.Frames, [x0, x1], [0, H], [z, z + 0.2], FRAME);
        boxInto(m.Frames, [x0, x1], [H - 0.3, H + 0.02], [-hz, hz], FRAME);
    }
    return m;
}

function placeholderGlb(def, dims) {
    const gl = makeGltf('fallout1-ce paper3d setpieces.js (placeholder)');
    const J = gl.json;
    J.materials.push({ name: 'placeholder', pbrMetallicRoughness: { baseColorFactor: [1, 1, 1, 1], metallicFactor: 0, roughnessFactor: 1 }, extensions: { KHR_materials_unlit: {} } });
    const root = gl.node({ name: def.id, children: [] });
    const meshes = containerPlaceholder(dims);
    for (const [name, mm] of Object.entries(meshes)) {
        if (!mm.idx.length) continue;
        J.meshes.push({
            name,
            primitives: [{
                attributes: {
                    POSITION: gl.positions(mm.pos),
                    NORMAL: gl.accessor(new Float32Array(mm.nrm), 'VEC3', 5126, 34962),
                    COLOR_0: gl.accessor(new Float32Array(mm.col), 'VEC3', 5126, 34962),
                },
                indices: gl.indices(mm.idx, mm.pos.length / 3),
                material: 0,
            }],
        });
        gl.node({ name, mesh: J.meshes.length - 1 }, root);
    }
    return gl.finish();
}

const BLENDER = process.env.BLENDER || 'C:/Program Files (x86)/Steam/steamapps/common/Blender/blender.exe';
function blender(args) {
    if (!fs.existsSync(BLENDER)) { console.log(`set pieces: no Blender at ${BLENDER} (set BLENDER=...) -- skipped`); return false; }
    execFileSync(BLENDER, ['-b', ...args], { stdio: 'ignore', timeout: 120000 });
    return true;
}

// The model for a set piece: the file in build/paper3d/replace, written as a placeholder the
// first time (with a .blend of it to edit). Returns its bytes, base64, for the page to carry.
function modelFor(def, dims) {
    const glb = path.join(REPLACE_DIR, def.id + '.glb'), blend = path.join(REPLACE_DIR, def.id + '.blend');
    if (!fs.existsSync(glb)) {
        fs.mkdirSync(REPLACE_DIR, { recursive: true });
        fs.writeFileSync(glb, placeholderGlb(def, dims));
        console.log(`set piece ${def.id}: wrote a placeholder, ${dims.map((v) => v.toFixed(2)).join(' x ')} (length x width x height) -> ${glb}`);
    }
    if (!fs.existsSync(blend)) {
        if (blender(['--factory-startup', '--python', path.join(__dirname, 'blender_setpiece.py'), '--', glb, blend])) {
            const now = new Date();
            fs.utimesSync(glb, now, now); // the fresh .blend isn't an edit: nothing to export yet
            console.log(`set piece ${def.id}: ${blend} to edit`);
        }
    } else if (fs.statSync(blend).mtimeMs > fs.statSync(glb).mtimeMs) {
        const py = `import bpy; bpy.ops.export_scene.gltf(filepath=r'${glb}', export_format='GLB')`;
        if (blender([blend, '--python-expr', py])) console.log(`set piece ${def.id}: exported your edited ${path.basename(blend)} -> ${path.basename(glb)}`);
    }
    return { file: glb, glb: fs.readFileSync(glb).toString('base64') };
}

module.exports = { SET_PIECES, setPieceOf, splitSetPiece, placeSetPiece, texturedContainer, roofOver, modelFor, REPLACE_DIR };
