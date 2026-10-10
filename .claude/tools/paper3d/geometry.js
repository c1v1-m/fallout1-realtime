// The map's 3D geometry as plain numbers -- ONE source for both the browser viewer (viewer.html,
// which build.js inlines this file into) and the glTF exporter (export.js), so what you walk
// through and what you open in Blender can never drift apart.
//
// Everything that comes from a sprite is a QUAD, textured PROJECTIVELY: a point takes the sprite
// pixel where the ORIGINAL camera sees it (screenOf). The viewer does that per fragment; the
// exporter bakes it into UVs -- exact, since that camera is orthographic, so on any flat quad the
// map from surface to sprite is affine.

(function (root) {
    'use strict';

    // The engine's projection read as a camera (build.js): 32 px per unit across the screen,
    // looking down at sin(theta) = sqrt(3)/4. A point the original camera sees at screen (sx, sy)
    // and that stands h units above the ground lives at W(sx, sy, h).
    const S = 32, SIN = Math.sqrt(3) / 4, COS = Math.sqrt(13) / 4;
    const KZ = S * SIN, KY = S * COS;
    const W = (sx, sy, h = 0) => [sx / S, h, (sy + KY * h) / KZ];
    const screenOf = (p) => [S * p[0], KZ * p[2] - KY * p[1]];
    const H_ROOF = 96 / KY; // square_coord_roof lifts the roof 96 px
    const SLOPES = [0, -0.25, 0.75]; // card, wall axis a (48,-12), wall axis b (32,24)
    // the unit normal of a panel on each axis, on the ground, towards the side the original camera saw
    const NORMAL = [[0, 0, 1], [0.5, 0, Math.sqrt(3) / 2], [-Math.sqrt(3) / 2, 0, 0.5]];
    // The WHOLE 80x36 rectangle, like floor_draw: the art's own transparency does the interlocking.
    const FLOOR_CORNERS = [[0, 0], [80, 0], [80, 36], [0, 36]];
    const TO_CAMERA = [0, SIN, COS];

    const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
    const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
    const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

    // The triangle order that makes a quad face the side the original camera saw (the back is the
    // side that was never painted -- the viewer darkens it).
    // A flat polygon of world points clipped (in x-z) to a rectangle [cx, cz, angle, half-length,
    // half-width] -- a set piece's footprint (Sutherland-Hodgman against its four sides).
    function clipToRect(poly, [cx, cz, a, hx, hz]) {
        const u = [Math.cos(a), Math.sin(a)], v = [-Math.sin(a), Math.cos(a)];
        for (const [n, d] of [[u, hx], [[-u[0], -u[1]], hx], [v, hz], [[-v[0], -v[1]], hz]]) {
            const f = (p) => d - ((p[0] - cx) * n[0] + (p[2] - cz) * n[1]); // >= 0: inside
            const out = [];
            for (let i = 0; i < poly.length; i++) {
                const p = poly[i], q = poly[(i + 1) % poly.length], fp = f(p), fq = f(q);
                if (fp >= 0) out.push(p);
                if ((fp >= 0) !== (fq >= 0)) { const t = fp / (fp - fq); out.push(p.map((c, k) => c + t * (q[k] - c))); }
            }
            poly = out;
            if (!poly.length) break;
        }
        return poly;
    }

    function frontOrder(c) {
        const n = cross(sub(c[1], c[0]), sub(c[2], c[0]));
        return dot(n, TO_CAMERA) >= 0 ? [0, 1, 2, 0, 2, 3] : [0, 2, 1, 0, 3, 2];
    }

    // Every projective quad of one elevation: emit(kind, quad), quad = { id, x, y, corners, shifts }
    // -- id the image, (x, y) the sprite's top-left on the original screen, corners 4 world points,
    // shifts null or 4 world offsets added before asking where the original camera sees a corner
    // (a face behind the front samples the front: the back of a box is its front's mirror image).
    // kinds: floor, roof, decal, panel (walls), card (scenery), trans (translucent).
    // deepBoxes (first person): a capped wall is a box as deep as its own top shows -- a gate
    // container a real crate. Otherwise the depth stops at 0.9, and the top of anything deeper
    // hung out over the back of its box, which only the free camera's height hides.
    function levelQuads(L, images, emit, { boxWalls = true, boxCards = true, deepBoxes = false } = {}) {
        for (const [tx, ty, id] of L.floor) {
            emit('floor', { id, x: tx, y: ty, corners: FLOOR_CORNERS.map(([x, y]) => W(tx + x, ty + y, 0)), shifts: null });
        }
        for (const [tx, ty, id, clip] of L.roof) {
            const corners = FLOOR_CORNERS.map(([x, y]) => W(tx + x, ty + y, H_ROOF));
            if (!clip) { emit('roof', { id, x: tx, y: ty, corners, shifts: null }); continue; }
            // a set piece's own painted roof, trimmed to the box it now tops (build.js --doom):
            // whole, the square-grid tiles overhung it; the projective mapping stays exact
            const poly = clipToRect(corners, clip);
            for (let i = 1; i + 1 < poly.length; i++) emit('roof', { id, x: tx, y: ty, corners: [poly[0], poly[i], poly[i + 1], poly[i + 1]], shifts: null });
        }
        for (const [x, y, id, order] of L.decal) {
            const [, , , w, h] = images[id];
            const lift = 0.01 + order * 0.0002;
            emit('decal', { id, x, y, corners: [W(x, y, lift), W(x + w, y, lift), W(x + w, y + h, lift), W(x, y + h, lift)], shifts: null });
        }
        // A standing sprite is one or two vertical panels (two = a corner), each over a stretch
        // [x0, x1] of its ground line y = k + m*x, reaching up to the sprite's top edge. A capped
        // wall (one under a roof) stands only 96 px -- the roof's height -- and the rest of its
        // sprite, its top seen from above, lies flat at that height, flush with the roof. Cap 2
        // stands 96 px and the rest is cut: a set piece's wall whose roof is the map's roof tiles.
        for (const kind of ['panel', 'trans']) {
            for (const [x, y, id, axis, k, x0, x1, cap] of L[kind]) {
                const m = SLOPES[axis];
                const b0 = k + m * x0, b1 = k + m * x1;
                const out = kind === 'panel' && axis === 0 ? 'card' : kind;
                const t0 = cap ? Math.min(96, b0 - y) : b0 - y, t1 = cap ? Math.min(96, b1 - y) : b1 - y;
                const F = [W(x0, b0, 0), W(x1, b1, 0), W(x1, b1 - t1, t1 / KY), W(x0, b0 - t0, t0 / KY)];
                emit(out, { id, x, y, corners: F, shifts: null });
                let capDepth = 0;
                if (cap === 1 && (b0 - y > 96 || b1 - y > 96)) {
                    const back0 = Math.min(y, b0 - 96), back1 = Math.min(y, b1 - 96); // never in front of the edge
                    emit(out, { id, x, y, corners: [W(x0, b0 - 96, H_ROOF), W(x1, b1 - 96, H_ROOF), W(x1, back1, H_ROOF), W(x0, back0, H_ROOF)], shifts: null });
                    capDepth = ((b0 - 96 - back0) + (b1 - 96 - back1)) / 2 / KZ; // how deep its top reaches, in z
                }
                if (kind !== 'panel') continue;
                // A WALL IS A BOX, not a sheet: as thick as its own top shows (under a roof), else a
                // slab. A scenery CARD gets depth too -- a car or a crate is not paper either -- about
                // half its width. Back, top and ends sample the front through the shifts, so the
                // back is the front mirrored and the ends/top carry its edge pixels; in the original
                // view they are edge-on or hidden, so that view is untouched.
                if (axis !== 0 && !boxWalls) continue;
                if (axis === 0 && !boxCards) continue;
                const nrm = NORMAL[axis];
                const T = axis !== 0
                    ? (capDepth > 0 ? Math.min(deepBoxes ? Infinity : 0.9, Math.max(0.15, capDepth * nrm[2])) : 0.25)
                    : Math.min(1.0, Math.max(0.2, ((x1 - x0) / S) * 0.5));
                const back = F.map((p) => [p[0] - nrm[0] * T, p[1], p[2] - nrm[2] * T]);
                const s = [nrm[0] * T, 0, nrm[2] * T], o = [0, 0, 0];
                emit(out, { id, x, y, corners: back, shifts: [s, s, s, s] });
                emit(out, { id, x, y, corners: [F[3], F[2], back[2], back[3]], shifts: [o, o, s, s] });
                emit(out, { id, x, y, corners: [F[0], F[3], back[3], back[0]], shifts: [o, o, s, s] });
                emit(out, { id, x, y, corners: [F[1], F[2], back[2], back[1]], shifts: [o, o, s, s] });
            }
        }
    }

    // The sprite pixel (lx, ly) a world point shows, relative to the sprite's top-left.
    function spritePixel(quad, i) {
        const c = quad.corners[i], sh = quad.shifts ? quad.shifts[i] : null;
        const [sx, sy] = screenOf(sh ? [c[0] + sh[0], c[1] + sh[1], c[2] + sh[2]] : c);
        return [sx - quad.x, sy - quad.y];
    }

    // For baking into UVs: the quad clipped to its sprite's rectangle (so a corner past the art
    // can't sample the atlas neighbour), as a polygon of { p: world point, l: sprite pixel }.
    // Clipping in sprite space is exact because sprite space is an affine image of the quad.
    function clipToSprite(quad, w, h) {
        let poly = quad.corners.map((p, i) => ({ p, l: spritePixel(quad, i) }));
        const edges = [(v) => v.l[0], (v) => w - v.l[0], (v) => v.l[1], (v) => h - v.l[1]];
        for (const inside of edges) {
            const out = [];
            for (let i = 0; i < poly.length; i++) {
                const a = poly[i], b = poly[(i + 1) % poly.length];
                const da = inside(a), db = inside(b);
                if (da >= 0) out.push(a);
                if ((da >= 0) !== (db >= 0)) {
                    const t = da / (da - db);
                    out.push({
                        p: [0, 1, 2].map((q) => a.p[q] + t * (b.p[q] - a.p[q])),
                        l: [0, 1].map((q) => a.l[q] + t * (b.l[q] - a.l[q])),
                    });
                }
            }
            poly = out;
            if (poly.length < 3) return [];
        }
        return poly;
    }

    // A voxel body (hull.js): records of int16 x, y, z + one palette index; a voxel's centre sits
    // at (index + 0.5) / 32 units in the critter's own frame (x forward, y up, z to its right).
    // Only faces bordering empty space are built. Returns flat arrays per face vertex.
    const VOX = 1 / 32;
    const CUBE_FACES = [
        { d: [1, 0, 0], c: [[1, 0, 0], [1, 1, 0], [1, 1, 1], [1, 0, 1]], shade: 0.9 },
        { d: [-1, 0, 0], c: [[0, 0, 1], [0, 1, 1], [0, 1, 0], [0, 0, 0]], shade: 0.9 },
        { d: [0, 1, 0], c: [[0, 1, 1], [1, 1, 1], [1, 1, 0], [0, 1, 0]], shade: 1.0 },
        { d: [0, -1, 0], c: [[0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1]], shade: 0.6 },
        { d: [0, 0, 1], c: [[1, 0, 1], [1, 1, 1], [0, 1, 1], [0, 0, 1]], shade: 0.8 },
        { d: [0, 0, -1], c: [[0, 0, 0], [0, 1, 0], [1, 1, 0], [1, 0, 0]], shade: 0.8 },
    ];
    function voxelFaces(bytes, n) {
        const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const key = (x, y, z) => ((x + 512) * 1024 + (y + 512)) * 1024 + (z + 512);
        const vox = [];
        const occupied = new Set();
        for (let i = 0; i < n; i++) {
            const x = dv.getInt16(i * 7, true), y = dv.getInt16(i * 7 + 2, true), z = dv.getInt16(i * 7 + 4, true);
            vox.push([x, y, z, bytes[i * 7 + 6]]);
            occupied.add(key(x, y, z));
        }
        const pos = [], colour = [], shade = [], index = [];
        for (const [x, y, z, c] of vox) {
            for (const f of CUBE_FACES) {
                if (occupied.has(key(x + f.d[0], y + f.d[1], z + f.d[2]))) continue;
                const base = pos.length / 3;
                for (const [cx, cy, cz] of f.c) {
                    pos.push((x + cx) * VOX, (y + cy) * VOX, (z + cz) * VOX);
                    colour.push(c);
                    shade.push(f.shade);
                }
                index.push(base, base + 1, base + 2, base, base + 2, base + 3);
            }
        }
        return { pos, colour, shade, index };
    }

    const api = {
        S, SIN, COS, KZ, KY, W, screenOf, H_ROOF, SLOPES, NORMAL, TO_CAMERA, VOX,
        frontOrder, levelQuads, spritePixel, clipToSprite, voxelFaces,
    };
    root.PaperGeometry = api;
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
})(typeof window !== 'undefined' ? window : globalThis);
