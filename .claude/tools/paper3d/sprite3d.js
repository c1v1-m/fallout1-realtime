// SPRITE3D -- our own program for turning Fallout's 2D sprites into 3D. Module 1: SHAPE.
//
// Interplay rendered every critter from a 3D model in 6 directions with the map's own camera, so
// each animation frame is 6 calibrated orthographic views of one pose (hull.js has the camera).
// hull.js carved them into VOXELS: blocky, and the six silhouettes' flat sides met in hard ridges.
// This carves the same six views as a smooth FIELD and gives back a mesh:
//   1. each silhouette (ground shadow already trimmed by hull.silhouette) -> a signed distance
//      field: the mask upscaled x4 with Scale2x (the staircase becomes slopes), then an exact
//      Euclidean distance transform, so the outline runs between pixels instead of along them
//   2. the 3D field = the largest of the six at each point: the visual hull, as a field
//   3. surface nets on that field -> a closed mesh, no cubes
//   4. Taubin smoothing (a Laplacian step, then a slightly larger step back: it rounds without
//      shrinking) -- six views leave hexagonal cross-sections; a leg is round
//   5. every vertex coloured from the views that actually see it (depth-tested), weighted by how
//      squarely each one does
//
//   node sprite3d.js <FRM> [frame] [outDir]          e.g.  node sprite3d.js HMJMPSAA
// writes <outDir>/<frm>_<frame>_smooth.glb, and _voxel.glb (the old carve, to compare), and
// prints how much of each original silhouette the mesh reproduces.
//
// Planned modules: RIG (one skeleton for all the humanoids), MOTION (fit that skeleton's pose to
// each frame's six silhouettes -> real, editable animations), and scenery from its single view.

'use strict';

const fs = require('fs');
const path = require('path');
const { openDat } = require('../f1script/dat.js');
const hull = require('./hull.js');
const { dressVaultSuit, isJumpsuit } = require('./dress.js');
const { makeGltf, srgbToLinear } = require('./gltf.js');

const GAME = path.resolve(__dirname, '../../../build/Release');
const BS = String.fromCharCode(92);
const S = 32, SIN = Math.sqrt(3) / 4, COS = Math.sqrt(13) / 4, KY = S * COS;
const U = 4, PAD = 8; // silhouette upscale (two Scale2x passes), margin around a sprite in px
let PAL = null; // the game palette (6-bit), set by smoothModel: novelView reads sprite colours through it

// ---------------------------------------------------------------- 1. silhouette -> distance field

// Scale2x (EPX) on a mask: every pixel becomes four, a corner takes its neighbours' value where
// two of them agree -- a staircase edge becomes a slope
function scale2x({ w, h, m }) {
    const out = new Uint8Array(w * h * 4);
    const at = (x, y) => m[Math.min(h - 1, Math.max(0, y)) * w + Math.min(w - 1, Math.max(0, x))];
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const P = at(x, y), A = at(x, y - 1), B = at(x + 1, y), C = at(x - 1, y), D = at(x, y + 1);
            const o = y * 4 * w + x * 2;
            out[o] = C === A && C !== D && A !== B ? A : P;
            out[o + 1] = A === B && A !== C && B !== D ? B : P;
            out[o + 2 * w] = D === C && D !== B && C !== A ? C : P;
            out[o + 2 * w + 1] = B === D && B !== A && D !== C ? D : P;
        }
    }
    return { w: w * 2, h: h * 2, m: out };
}

// Squared Euclidean distance transform (Felzenszwalb & Huttenlocher): for every pixel, the squared
// distance to the nearest pixel where `feature` is set.
const INF = 1e20;
function dt1(f, n, d, v, z) {
    let k = 0;
    v[0] = 0; z[0] = -INF; z[1] = INF;
    for (let q = 1; q < n; q++) {
        let s = (f[q] + q * q - (f[v[k]] + v[k] * v[k])) / (2 * q - 2 * v[k]);
        while (s <= z[k]) {
            k--;
            s = (f[q] + q * q - (f[v[k]] + v[k] * v[k])) / (2 * q - 2 * v[k]);
        }
        k++;
        v[k] = q; z[k] = s; z[k + 1] = INF;
    }
    k = 0;
    for (let q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        d[q] = (q - v[k]) * (q - v[k]) + f[v[k]];
    }
}
function edt(feature, W, H) {
    const n = Math.max(W, H);
    const f = new Float64Array(n), d = new Float64Array(n), v = new Int32Array(n), z = new Float64Array(n + 1);
    const out = new Float64Array(W * H);
    for (let p = 0; p < W * H; p++) out[p] = feature[p] ? 0 : INF;
    for (let x = 0; x < W; x++) {
        for (let y = 0; y < H; y++) f[y] = out[y * W + x];
        dt1(f, H, d, v, z);
        for (let y = 0; y < H; y++) out[y * W + x] = d[y];
    }
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) f[x] = out[y * W + x];
        dt1(f, W, d, v, z);
        for (let x = 0; x < W; x++) out[y * W + x] = d[x];
    }
    return out;
}

// The signed distance to a sprite's outline, in sprite px, negative inside; cached on the frame.
function silhouetteField(fr, trim = true) {
    const key = trim ? 'sdf' : 'sdf0';
    if (fr[key]) return fr[key];
    const mask = hull.silhouette(fr, trim);
    let g = { w: fr.w + 2 * PAD, h: fr.h + 2 * PAD, m: null };
    g.m = new Uint8Array(g.w * g.h);
    for (let y = 0; y < fr.h; y++) for (let x = 0; x < fr.w; x++) g.m[(y + PAD) * g.w + x + PAD] = mask[y * fr.w + x] ? 1 : 0;
    for (let s = 1; s < U; s *= 2) g = scale2x(g);
    const W = g.w, H = g.h;
    const toBody = edt(g.m, W, H);
    const toEmpty = edt(g.m.map((q) => (q ? 0 : 1)), W, H);
    const sdf = new Float32Array(W * H);
    // the outline lies halfway between a body pixel's centre and an empty one's
    for (let p = 0; p < W * H; p++) sdf[p] = (g.m[p] ? -(Math.sqrt(toEmpty[p]) - 0.5) : Math.sqrt(toBody[p]) - 0.5) / U;
    return (fr[key] = { W, H, sdf });
}

// the field at sprite position (u, v) -- pixel (pu, pv) covers [pu, pu + 1) -- bilinear
function sampleField(sf, u, v) {
    const a = (u + PAD) * U - 0.5, b = (v + PAD) * U - 0.5;
    if (a < 0 || b < 0 || a >= sf.W - 1 || b >= sf.H - 1) return PAD;
    const x0 = Math.floor(a), y0 = Math.floor(b), tx = a - x0, ty = b - y0, W = sf.W, s = sf.sdf;
    const p = y0 * W + x0;
    return (s[p] * (1 - tx) + s[p + 1] * tx) * (1 - ty) + (s[p + W] * (1 - tx) + s[p + W + 1] * tx) * ty;
}

// ---------------------------------------------------------------- 2. the hull as a 3D field

// Sampled on a grid of spacing h (units) around the body; negative = inside every silhouette.
function hullField(views, { h = 1 / 64, trim = true } = {}) {
    let half = 0, tall = 0;
    for (const v of views) {
        half = Math.max(half, Math.abs(v.bx0), Math.abs(v.bx0 + v.fr.w));
        tall = Math.max(tall, -v.by0);
    }
    const R = half / S + 0.3, top = tall / KY + 0.6;
    const nx = Math.ceil((2 * R) / h) + 1, ny = Math.ceil((top + 3 * h) / h) + 1, nz = nx;
    const x0 = -R, y0 = -3 * h, z0 = -R;
    const fields = views.map((v) => silhouetteField(v.fr, trim));
    const F = new Float32Array(nx * ny * nz);
    for (let k = 0; k < nz; k++) {
        const z = z0 + k * h;
        for (let j = 0; j < ny; j++) {
            const y = y0 + j * h;
            for (let i = 0; i < nx; i++) {
                const x = x0 + i * h;
                let f = -y * S; // nothing of a critter is below the ground
                for (let n = 0; n < views.length && f < PAD; n++) {
                    const [u, w] = hull.project(views[n], x, y, z);
                    const s = sampleField(fields[n], u, w);
                    if (s > f) f = s;
                }
                F[i + nx * (j + ny * k)] = f;
            }
        }
    }
    return { F, nx, ny, nz, x0, y0, z0, h };
}

// ---------------------------------------------------------------- 3. surface nets

// One vertex per grid cell the surface passes through (the mean of its edge crossings), one quad
// per grid edge it crosses, wound so the outside sees it counter-clockwise.
function surfaceNets({ F, nx, ny, nz, x0, y0, z0, h }) {
    const cx = nx - 1, cy = ny - 1, cz = nz - 1;
    const cell = new Int32Array(cx * cy * cz).fill(-1);
    const cid = (i, j, k) => i + cx * (j + cy * k);
    const val = (i, j, k) => F[i + nx * (j + ny * k)];
    const CORNER = [[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0], [0, 0, 1], [1, 0, 1], [0, 1, 1], [1, 1, 1]];
    const EDGE = [[0, 1], [2, 3], [4, 5], [6, 7], [0, 2], [1, 3], [4, 6], [5, 7], [0, 4], [1, 5], [2, 6], [3, 7]];
    const pos = [], c = new Float64Array(8);
    for (let k = 0; k < cz; k++) {
        for (let j = 0; j < cy; j++) {
            for (let i = 0; i < cx; i++) {
                let inside = 0;
                for (let q = 0; q < 8; q++) {
                    c[q] = val(i + CORNER[q][0], j + CORNER[q][1], k + CORNER[q][2]);
                    if (c[q] < 0) inside++;
                }
                if (inside === 0 || inside === 8) continue;
                let sx = 0, sy = 0, sz = 0, n = 0;
                for (const [a, b] of EDGE) {
                    if ((c[a] < 0) === (c[b] < 0)) continue;
                    const t = c[a] / (c[a] - c[b]);
                    sx += CORNER[a][0] + t * (CORNER[b][0] - CORNER[a][0]);
                    sy += CORNER[a][1] + t * (CORNER[b][1] - CORNER[a][1]);
                    sz += CORNER[a][2] + t * (CORNER[b][2] - CORNER[a][2]);
                    n++;
                }
                cell[cid(i, j, k)] = pos.length / 3;
                pos.push(x0 + (i + sx / n) * h, y0 + (j + sy / n) * h, z0 + (k + sz / n) * h);
            }
        }
    }
    const tris = [];
    // around an edge along axis a, the cells in the order (b-1,c-1) (b,c-1) (b,c) (b-1,c) with
    // (a,b,c) cyclic are counter-clockwise seen from +a: kept when the inside is at the low end
    const quad = (p0, p1, p2, p3, flip) => {
        if (flip) tris.push(p0, p3, p2, p0, p2, p1);
        else tris.push(p0, p1, p2, p0, p2, p3);
    };
    for (let k = 0; k < nz; k++) {
        for (let j = 0; j < ny; j++) {
            for (let i = 0; i < nx; i++) {
                const inHere = val(i, j, k) < 0;
                if (i < cx && j > 0 && j < cy && k > 0 && k < cz && inHere !== val(i + 1, j, k) < 0)
                    quad(cell[cid(i, j - 1, k - 1)], cell[cid(i, j, k - 1)], cell[cid(i, j, k)], cell[cid(i, j - 1, k)], !inHere);
                if (j < cy && i > 0 && i < cx && k > 0 && k < cz && inHere !== val(i, j + 1, k) < 0)
                    quad(cell[cid(i - 1, j, k - 1)], cell[cid(i - 1, j, k)], cell[cid(i, j, k)], cell[cid(i, j, k - 1)], !inHere);
                if (k < cz && i > 0 && i < cx && j > 0 && j < cy && inHere !== val(i, j, k + 1) < 0)
                    quad(cell[cid(i - 1, j - 1, k)], cell[cid(i, j - 1, k)], cell[cid(i, j, k)], cell[cid(i - 1, j, k)], !inHere);
            }
        }
    }
    return { pos: new Float32Array(pos), tris: new Uint32Array(tris) };
}

// ---------------------------------------------------------------- 4. mesh clean-up and smoothing

function adjacency(nv, tris) {
    const sets = Array.from({ length: nv }, () => new Set());
    for (let t = 0; t < tris.length; t += 3) {
        const a = tris[t], b = tris[t + 1], c = tris[t + 2];
        sets[a].add(b); sets[a].add(c); sets[b].add(a); sets[b].add(c); sets[c].add(a); sets[c].add(b);
    }
    return sets.map((s) => Int32Array.from(s));
}

// Only the body: the lumps the ground shadow can still leave are dropped (under `share` of the
// biggest part).
function keepMainParts({ pos, tris }, share = 0.05) {
    const nv = pos.length / 3, up = new Int32Array(nv).map((_, i) => i);
    const root = (a) => { while (up[a] !== a) a = up[a] = up[up[a]]; return a; };
    for (let t = 0; t < tris.length; t += 3) {
        const a = root(tris[t]), b = root(tris[t + 1]), c = root(tris[t + 2]);
        up[b] = a; up[root(c)] = a;
    }
    const size = new Map();
    for (let i = 0; i < nv; i++) size.set(root(i), (size.get(root(i)) || 0) + 1);
    const biggest = Math.max(...size.values());
    const remap = new Int32Array(nv).fill(-1), out = [];
    for (let i = 0; i < nv; i++) {
        if (size.get(root(i)) < share * biggest) continue;
        remap[i] = out.length / 3;
        out.push(pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]);
    }
    const kept = [];
    for (let t = 0; t < tris.length; t += 3) if (remap[tris[t]] >= 0) kept.push(remap[tris[t]], remap[tris[t + 1]], remap[tris[t + 2]]);
    return { pos: new Float32Array(out), tris: new Uint32Array(kept), parts: size.size, keptParts: [...size.values()].filter((n) => n >= share * biggest).length };
}

// Taubin's lambda|mu smoothing: a Laplacian step in, a slightly larger step out -- rounds the
// corners without shrinking the body the way plain Laplacian smoothing does
function taubin(pos, adj, iterations = 12, lambda = 0.5, mu = -0.53) {
    const nv = adj.length, next = new Float32Array(pos.length);
    for (let it = 0; it < iterations; it++) {
        for (const f of [lambda, mu]) {
            for (let v = 0; v < nv; v++) {
                const nb = adj[v];
                let sx = 0, sy = 0, sz = 0;
                for (const u of nb) { sx += pos[3 * u]; sy += pos[3 * u + 1]; sz += pos[3 * u + 2]; }
                const m = nb.length || 1;
                next[3 * v] = pos[3 * v] + f * (nb.length ? sx / m - pos[3 * v] : 0);
                next[3 * v + 1] = pos[3 * v + 1] + f * (nb.length ? sy / m - pos[3 * v + 1] : 0);
                next[3 * v + 2] = pos[3 * v + 2] + f * (nb.length ? sz / m - pos[3 * v + 2] : 0);
            }
            pos.set(next);
        }
    }
}

function vertexNormals(pos, tris) {
    const n = new Float32Array(pos.length);
    for (let t = 0; t < tris.length; t += 3) {
        const a = 3 * tris[t], b = 3 * tris[t + 1], c = 3 * tris[t + 2];
        const ux = pos[b] - pos[a], uy = pos[b + 1] - pos[a + 1], uz = pos[b + 2] - pos[a + 2];
        const vx = pos[c] - pos[a], vy = pos[c + 1] - pos[a + 1], vz = pos[c + 2] - pos[a + 2];
        const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx; // area-weighted
        for (const p of [a, b, c]) { n[p] += nx; n[p + 1] += ny; n[p + 2] += nz; }
    }
    for (let p = 0; p < n.length; p += 3) {
        const l = Math.hypot(n[p], n[p + 1], n[p + 2]) || 1;
        n[p] /= l; n[p + 1] /= l; n[p + 2] /= l;
    }
    return n;
}

// signed volume: positive when every triangle is wound counter-clockwise from outside
function volume(pos, tris) {
    let v = 0;
    for (let t = 0; t < tris.length; t += 3) {
        const a = 3 * tris[t], b = 3 * tris[t + 1], c = 3 * tris[t + 2];
        v += pos[a] * (pos[b + 1] * pos[c + 2] - pos[b + 2] * pos[c + 1])
            - pos[a + 1] * (pos[b] * pos[c + 2] - pos[b + 2] * pos[c])
            + pos[a + 2] * (pos[b] * pos[c + 1] - pos[b + 1] * pos[c]);
    }
    return v / 6;
}

// ---------------------------------------------------------------- 5. colour

// A view's rasterised mesh at D x the sprite's resolution: nearest depth per pixel (towards the
// camera is larger), or just coverage.
function rasterise(mesh, v, D) {
    const { pos, tris } = mesh, nv = pos.length / 3;
    const W = v.fr.w * D, H = v.fr.h * D;
    const tc = [COS * Math.sin(v.a), SIN, COS * Math.cos(v.a)]; // towards the camera, local axes
    const px = new Float32Array(nv * 2), dep = new Float32Array(nv);
    for (let i = 0; i < nv; i++) {
        const [u, w] = hull.project(v, pos[3 * i], pos[3 * i + 1], pos[3 * i + 2]);
        px[2 * i] = u * D; px[2 * i + 1] = w * D;
        dep[i] = pos[3 * i] * tc[0] + pos[3 * i + 1] * tc[1] + pos[3 * i + 2] * tc[2];
    }
    const zb = new Float32Array(W * H).fill(-Infinity);
    for (let t = 0; t < tris.length; t += 3) {
        const a = tris[t], b = tris[t + 1], c = tris[t + 2];
        const ax = px[2 * a], ay = px[2 * a + 1], bx = px[2 * b], by = px[2 * b + 1], cx = px[2 * c], cy = px[2 * c + 1];
        const area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (Math.abs(area) < 1e-9) continue;
        const x0 = Math.max(0, Math.floor(Math.min(ax, bx, cx))), x1 = Math.min(W - 1, Math.ceil(Math.max(ax, bx, cx)));
        const y0 = Math.max(0, Math.floor(Math.min(ay, by, cy))), y1 = Math.min(H - 1, Math.ceil(Math.max(ay, by, cy)));
        for (let y = y0; y <= y1; y++) {
            const qy = y + 0.5;
            for (let x = x0; x <= x1; x++) {
                const qx = x + 0.5;
                const w0 = ((bx - qx) * (cy - qy) - (by - qy) * (cx - qx)) / area;
                const w1 = ((cx - qx) * (ay - qy) - (cy - qy) * (ax - qx)) / area;
                const w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const d = w0 * dep[a] + w1 * dep[b] + w2 * dep[c];
                if (d > zb[y * W + x]) zb[y * W + x] = d;
            }
        }
    }
    return { W, H, zb, px, dep, tc };
}

// Every vertex takes the sprite colour under it in each view that sees it, weighted by
// (facing)^power -- mostly the most squarely facing view, blended where two tie.
function colourMesh(mesh, views, pal, { trim = true, power = 4, eps = 0.03 } = {}) {
    const { pos, nrm } = mesh, nv = pos.length / 3, D = 4;
    const acc = new Float64Array(nv * 3), wsum = new Float64Array(nv);
    for (const v of views) {
        const { W, H, zb, px, dep, tc } = rasterise(mesh, v, D);
        const keep = hull.silhouette(v.fr, trim);
        for (let i = 0; i < nv; i++) {
            const facing = nrm[3 * i] * tc[0] + nrm[3 * i + 1] * tc[1] + nrm[3 * i + 2] * tc[2];
            if (facing <= 0) continue;
            const bx = Math.floor(px[2 * i]), by = Math.floor(px[2 * i + 1]);
            if (bx < 0 || by < 0 || bx >= W || by >= H || dep[i] < zb[by * W + bx] - eps) continue; // hidden
            const pu = Math.floor(px[2 * i] / D), pv = Math.floor(px[2 * i + 1] / D), p = pv * v.fr.w + pu;
            if (!keep[p]) continue;
            const c = v.fr.data[p], wgt = Math.pow(facing, power);
            acc[3 * i] += wgt * pal[c * 3] * 4; acc[3 * i + 1] += wgt * pal[c * 3 + 1] * 4; acc[3 * i + 2] += wgt * pal[c * 3 + 2] * 4;
            wsum[i] += wgt;
        }
    }
    const col = new Float32Array(nv * 3), done = new Uint8Array(nv);
    for (let i = 0; i < nv; i++) {
        if (!wsum[i]) continue;
        for (let q = 0; q < 3; q++) col[3 * i + q] = acc[3 * i + q] / wsum[i];
        done[i] = 1;
    }
    // whatever no view saw (under the arms, between the legs) takes its neighbours' colour
    const adj = mesh.adj;
    for (let pass = 0; pass < 50; pass++) {
        let left = 0;
        const now = [];
        for (let i = 0; i < nv; i++) {
            if (done[i]) continue;
            let n = 0, r = 0, g = 0, b = 0;
            for (const u of adj[i]) if (done[u]) { r += col[3 * u]; g += col[3 * u + 1]; b += col[3 * u + 2]; n++; }
            if (n) now.push([i, r / n, g / n, b / n]); else left++;
        }
        for (const [i, r, g, b] of now) { col[3 * i] = r; col[3 * i + 1] = g; col[3 * i + 2] = b; done[i] = 1; }
        if (!left) break;
    }
    return col;
}

// How the mesh's outline matches each sprite: the share of the silhouette it covers, and the
// share of its own outline that falls outside.
function fidelity(mesh, views, trim = true) {
    return views.map((v) => {
        const { zb } = rasterise(mesh, v, 1);
        const m = hull.silhouette(v.fr, trim);
        let sil = 0, got = 0, drawn = 0, spill = 0;
        for (let p = 0; p < m.length; p++) {
            const on = zb[p] > -Infinity;
            if (m[p]) { sil++; if (on) got++; }
            if (on) { drawn++; if (!m[p]) spill++; }
        }
        return { cover: got / (sil || 1), spill: spill / (drawn || 1) };
    });
}

// ---------------------------------------------------------------- 6. new views, drawn with the ORIGINAL pixels

// A sprite the game never drew: the critter turned to `azimuth` (0 = facing east, pi/2 = facing
// the camera, the direction between SE and SW) and seen `elevation` above the horizon (the game's
// 25.7 deg, or 0 for eye level, as Doom did). The mesh only says WHERE each pixel's point of the
// body is; its colour is read from the original sprites that see that point, the nearer their
// camera to this one the more -- so it stays the original art, pixel for pixel, and turns.
function novelView(mesh, views, { azimuth = Math.PI / 2, elevation = Math.asin(SIN), scale = S, trim = true, near = 8, face = 4, eps = 0.03 } = {}) {
    const { pos, nrm, tris, col } = mesh, nv = pos.length / 3;
    const ca = Math.cos(azimuth), sa = Math.sin(azimuth), se = Math.sin(elevation), ce = Math.cos(elevation);
    const sx = new Float32Array(nv), sy = new Float32Array(nv), sd = new Float32Array(nv);
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    for (let i = 0; i < nv; i++) {
        const x = pos[3 * i], y = pos[3 * i + 1], z = pos[3 * i + 2];
        const xw = x * ca - z * sa, zw = x * sa + z * ca;
        sx[i] = scale * xw; sy[i] = scale * (se * zw - ce * y); sd[i] = zw * ce + y * se;
        minX = Math.min(minX, sx[i]); maxX = Math.max(maxX, sx[i]); minY = Math.min(minY, sy[i]); maxY = Math.max(maxY, sy[i]);
    }
    const ox = Math.floor(minX) - 1, oy = Math.floor(minY) - 1;
    const W = Math.ceil(maxX) - ox + 2, H = Math.ceil(maxY) - oy + 2;
    // per pixel: the nearest surface point -- its place, normal and the mesh's own colour there
    const zb = new Float32Array(W * H).fill(-Infinity), at = new Float32Array(W * H * 9);
    for (let t = 0; t < tris.length; t += 3) {
        const a = tris[t], b = tris[t + 1], c = tris[t + 2];
        const ax = sx[a] - ox, ay = sy[a] - oy, bx = sx[b] - ox, by = sy[b] - oy, cx = sx[c] - ox, cy = sy[c] - oy;
        const area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (Math.abs(area) < 1e-9) continue;
        const x0 = Math.max(0, Math.floor(Math.min(ax, bx, cx))), x1 = Math.min(W - 1, Math.ceil(Math.max(ax, bx, cx)));
        const y0 = Math.max(0, Math.floor(Math.min(ay, by, cy))), y1 = Math.min(H - 1, Math.ceil(Math.max(ay, by, cy)));
        for (let y = y0; y <= y1; y++) {
            for (let x = x0; x <= x1; x++) {
                const qx = x + 0.5, qy = y + 0.5;
                const w0 = ((bx - qx) * (cy - qy) - (by - qy) * (cx - qx)) / area;
                const w1 = ((cx - qx) * (ay - qy) - (cy - qy) * (ax - qx)) / area;
                const w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const d = w0 * sd[a] + w1 * sd[b] + w2 * sd[c], p = y * W + x;
                if (d <= zb[p]) continue;
                zb[p] = d;
                for (let q = 0; q < 3; q++) {
                    at[p * 9 + q] = w0 * pos[3 * a + q] + w1 * pos[3 * b + q] + w2 * pos[3 * c + q];
                    at[p * 9 + 3 + q] = w0 * nrm[3 * a + q] + w1 * nrm[3 * b + q] + w2 * nrm[3 * c + q];
                    at[p * 9 + 6 + q] = w0 * col[3 * a + q] + w1 * col[3 * b + q] + w2 * col[3 * c + q];
                }
            }
        }
    }
    const tcT = [ce * sa, se, ce * ca]; // towards this camera, in the critter's axes
    const src = views.map((v) => ({ v, ...rasterise(mesh, v, 4), keep: hull.silhouette(v.fr, trim) }));
    const rgba = Buffer.alloc(W * H * 4);
    let fromSprites = 0, filled = 0;
    for (let p = 0; p < W * H; p++) {
        if (zb[p] === -Infinity) continue;
        filled++;
        const P = [at[p * 9], at[p * 9 + 1], at[p * 9 + 2]], N = [at[p * 9 + 3], at[p * 9 + 4], at[p * 9 + 5]];
        const nl = Math.hypot(...N) || 1;
        let r = 0, g = 0, b = 0, ws = 0;
        for (const s of src) {
            const facing = (N[0] * s.tc[0] + N[1] * s.tc[1] + N[2] * s.tc[2]) / nl;
            if (facing <= 0) continue;
            const [u, w] = hull.project(s.v, P[0], P[1], P[2]);
            const bx = Math.floor(u * 4), by = Math.floor(w * 4);
            if (bx < 0 || by < 0 || bx >= s.W || by >= s.H) continue;
            if (P[0] * s.tc[0] + P[1] * s.tc[1] + P[2] * s.tc[2] < s.zb[by * s.W + bx] - eps) continue; // hidden there
            const q = Math.floor(w) * s.v.fr.w + Math.floor(u);
            if (!s.keep[q]) continue;
            const closeness = Math.max(0, tcT[0] * s.tc[0] + tcT[1] * s.tc[1] + tcT[2] * s.tc[2]);
            const wgt = Math.pow(closeness, near) * Math.pow(facing, face);
            const c = s.v.fr.data[q];
            r += wgt * PAL[c * 3] * 4; g += wgt * PAL[c * 3 + 1] * 4; b += wgt * PAL[c * 3 + 2] * 4;
            ws += wgt;
        }
        const o = p * 4;
        if (ws > 1e-6) {
            rgba[o] = Math.round(r / ws); rgba[o + 1] = Math.round(g / ws); rgba[o + 2] = Math.round(b / ws);
            fromSprites++;
        } else { // no sprite sees it (under the chin, from eye level): the mesh's own colour
            rgba[o] = Math.round(at[p * 9 + 6]); rgba[o + 1] = Math.round(at[p * 9 + 7]); rgba[o + 2] = Math.round(at[p * 9 + 8]);
        }
        rgba[o + 3] = 255;
    }
    // the anchor (the hex centre, on the ground) sits at (-ox, -oy) in the image
    return { W, H, rgba, ax: -ox, ay: -oy, fromSprites: fromSprites / (filled || 1) };
}

// ---------------------------------------------------------------- the old voxel carve, as a mesh

function voxelMesh(grid, surf, pal) {
    const { occ, nx, ny, nz, idx } = grid, h = hull.VOX / 2;
    const at = (i, j, k) => i >= 0 && j >= 0 && k >= 0 && i < nx && j < ny && k < nz && occ[idx(i, j, k)];
    const FACES = [ // neighbour offset, normal, corners (counter-clockwise from outside)
        [[1, 0, 0], [[1, -1, -1], [1, 1, -1], [1, 1, 1], [1, -1, 1]]],
        [[-1, 0, 0], [[-1, -1, 1], [-1, 1, 1], [-1, 1, -1], [-1, -1, -1]]],
        [[0, 1, 0], [[-1, 1, -1], [-1, 1, 1], [1, 1, 1], [1, 1, -1]]],
        [[0, -1, 0], [[1, -1, -1], [1, -1, 1], [-1, -1, 1], [-1, -1, -1]]],
        [[0, 0, 1], [[-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1]]],
        [[0, 0, -1], [[-1, 1, -1], [1, 1, -1], [1, -1, -1], [-1, -1, -1]]],
    ];
    const pos = [], nrm = [], col = [], tris = [];
    for (const s of surf) {
        for (const [d, corners] of FACES) {
            if (at(s.i + d[0], s.j + d[1], s.k + d[2])) continue;
            const base = pos.length / 3;
            for (const c of corners) {
                pos.push(s.x + c[0] * h, s.y + c[1] * h, s.z + c[2] * h);
                nrm.push(...d);
                col.push(pal[s.c * 3] * 4, pal[s.c * 3 + 1] * 4, pal[s.c * 3 + 2] * 4);
            }
            tris.push(base, base + 1, base + 2, base, base + 2, base + 3);
        }
    }
    return { pos: new Float32Array(pos), nrm: new Float32Array(nrm), col: new Float32Array(col), tris: new Uint32Array(tris) };
}

// ---------------------------------------------------------------- glTF

function writeModel(file, name, { pos, nrm, col, tris }) {
    const gl = makeGltf('fallout1-ce paper3d sprite3d.js');
    const J = gl.json;
    J.materials.push({ name: 'sprite_colour', pbrMetallicRoughness: { baseColorFactor: [1, 1, 1, 1], metallicFactor: 0, roughnessFactor: 1 } });
    const lin = new Float32Array(col.length);
    for (let i = 0; i < col.length; i++) lin[i] = srgbToLinear(col[i]);
    const attributes = {
        POSITION: gl.positions(pos),
        NORMAL: gl.accessor(new Float32Array(nrm), 'VEC3', 5126, 34962),
        COLOR_0: gl.accessor(lin, 'VEC3', 5126, 34962),
    };
    J.meshes.push({ name, primitives: [{ attributes, indices: gl.indices(tris, pos.length / 3), material: 0 }] });
    gl.node({ name, mesh: 0 });
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, gl.finish());
}

// ---------------------------------------------------------------- the whole shape pass

function smoothModel(views, pal, { h = 1 / 64, smooth = 12, trim = true } = {}) {
    PAL = pal;
    const field = hullField(views, { h, trim });
    let mesh = keepMainParts(surfaceNets(field));
    mesh.adj = adjacency(mesh.pos.length / 3, mesh.tris);
    taubin(mesh.pos, mesh.adj, smooth);
    for (let i = 1; i < mesh.pos.length; i += 3) if (mesh.pos[i] < 0) mesh.pos[i] = 0; // soles on the ground
    mesh.nrm = vertexNormals(mesh.pos, mesh.tris);
    mesh.col = colourMesh(mesh, views, pal, { trim });
    return mesh;
}

module.exports = { silhouetteField, hullField, surfaceNets, keepMainParts, adjacency, taubin, vertexNormals, colourMesh, fidelity, smoothModel, novelView, voxelMesh, writeModel };

if (require.main === module) {
    const name = (process.argv[2] || 'HMJMPSAA').toUpperCase();
    const frame = +(process.argv[3] || 0);
    const outDir = process.argv[4] || path.resolve(__dirname, '../../../build/paper3d/sprite3d');
    const pal = openDat(path.join(GAME, 'MASTER.DAT')).extract('COLOR.PAL');
    hull.setPalette(pal);
    const frm = hull.readFrm(openDat(path.join(GAME, 'CRITTER.DAT')).extract(['ART', 'CRITTERS', name + '.FRM'].join(BS)));
    const views = hull.viewsOf(frm, frame, false);
    const stem = path.join(outDir, `${name.toLowerCase()}_${frame}`);

    let t = Date.now();
    const mesh = smoothModel(views, pal);
    const fid = fidelity(mesh, views);
    console.log(`smooth: ${mesh.pos.length / 3} vertices, ${mesh.tris.length / 3} triangles, volume ${volume(mesh.pos, mesh.tris).toFixed(4)} (positive = wound outward), parts ${mesh.parts} -> ${mesh.keptParts}, ${Date.now() - t} ms`);
    console.log('  outline covered per view:', fid.map((f) => (f.cover * 100).toFixed(0) + '%').join(' '), '| spilling outside:', fid.map((f) => (f.spill * 100).toFixed(0) + '%').join(' '));
    writeModel(stem + '_smooth.glb', `${name}_smooth`, mesh);

    t = Date.now();
    const grid = hull.carveStanding(views);
    let surf = hull.colour(grid, views).filter((s) => s.c);
    if (isJumpsuit(name)) surf = dressVaultSuit(surf, pal);
    const vox = voxelMesh(grid, surf, pal);
    const vfid = fidelity(vox, views);
    console.log(`voxel:  ${surf.length} surface voxels, ${vox.tris.length / 3} triangles, ${Date.now() - t} ms`);
    console.log('  outline covered per view:', vfid.map((f) => (f.cover * 100).toFixed(0) + '%').join(' '), '| spilling outside:', vfid.map((f) => (f.spill * 100).toFixed(0) + '%').join(' '));
    writeModel(stem + '_voxel.glb', `${name}_voxel`, vox);
    console.log('wrote', stem + '_smooth.glb', 'and _voxel.glb');
}
