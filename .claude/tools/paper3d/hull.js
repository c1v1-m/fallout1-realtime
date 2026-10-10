// True 3D critters from Fallout's own renders: a VISUAL HULL.
//
// Interplay rendered every critter from a 3D model, in 6 directions, with the same camera as the
// map (see build.js: 32 px per unit across, looking down at sin(theta) = sqrt(3)/4). A critter
// facing direction r is the same as the camera orbiting it by 60 deg steps -- so every frame of
// every animation is 6 calibrated views of one pose. Carving away every voxel that falls outside
// any of the 6 silhouettes leaves the pose's 3D shape; the surface is then coloured from the view
// that sees each voxel best. No AI, no rig: the original animation, frame by frame, in 3D.
//
// Usage: node hull.js <FRMNAME> [frame|all] [out.png]      e.g. node hull.js HMJMPSAB all
//   prints per-view coverage (how much of each original silhouette the hull reproduces) and
//   writes a contact sheet of the model turned through 12 headings.

'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { openDat } = require('../f1script/dat.js');

const GAME = path.resolve(__dirname, '../../../build/Release');
const BS = String.fromCharCode(92);
const S = 32, SIN = Math.sqrt(3) / 4, COS = Math.sqrt(13) / 4;
const KZ = S * SIN, KY = S * COS;
const VOX = 1 / 32; // voxel edge in units: one sprite pixel across

function readFrm(buf) {
    const fpd = buf.readUInt16BE(8), fps = buf.readUInt16BE(4) || 10;
    const xOff = [], yOff = [], dataOff = [];
    for (let i = 0; i < 6; i++) {
        xOff.push(buf.readInt16BE(10 + i * 2));
        yOff.push(buf.readInt16BE(22 + i * 2));
        dataOff.push(buf.readUInt32BE(34 + i * 4));
    }
    const dirs = [];
    for (let d = 0; d < 6; d++) {
        let p = 62 + dataOff[d];
        const frames = [];
        for (let f = 0; f < fpd; f++) {
            const w = buf.readUInt16BE(p), h = buf.readUInt16BE(p + 2), size = buf.readUInt32BE(p + 4);
            frames.push({ w, h, fx: buf.readInt16BE(p + 8), fy: buf.readInt16BE(p + 10), data: buf.subarray(p + 12, p + 12 + w * h) });
            p += 12 + size;
        }
        dirs.push(frames);
    }
    return { fpd, fps, xOff, yOff, dirs };
}

// The 6 views of one frame: the sprite, where its top-left sits relative to the critter's anchor
// (the hex centre, on the ground), and the heading the critter faces in it.
// A moving animation (walk, run) also shifts the sprite by the running sum of its frames' offsets
// -- that is how the engine moves the critter along -- and each direction rounds that sum to whole
// pixels on its own. With `acc`, every view is placed where the engine would draw it, and the
// body is moved forward by one distance fitted to all six views (least squares), so the leftover
// is only each direction's rounding.
function viewsOf(frm, f, acc = false) {
    const views = frm.dirs.map((frames, r) => {
        const fr = frames[Math.min(f, frames.length - 1)];
        let ax = 0, ay = 0;
        if (acc) for (let i = 0; i <= Math.min(f, frames.length - 1); i++) { ax += frames[i].fx; ay += frames[i].fy; }
        const bx0 = frm.xOff[r] - (fr.w >> 1), by0 = frm.yOff[r] - (fr.h - 1); // around the body
        return { r, fr, ax, ay, bx0, by0, dx0: bx0 + ax, dy0: by0 + ay, a: (r - 1) * Math.PI / 3, fwd: 0 };
    });
    if (acc) {
        // screen px per unit of forward travel in view r: (32 cos a, KZ sin a)
        let num = 0, den = 0;
        for (const v of views) {
            const px = S * Math.cos(v.a), py = KZ * Math.sin(v.a);
            num += v.ax * px + v.ay * py;
            den += px * px + py * py;
        }
        const d = num / den;
        for (const v of views) v.fwd = d;
    }
    return views;
}

// local (x forward, y up, z to the critter's right -- the world axes of a critter facing +x) to
// the sprite pixel of view v; the body stands v.fwd units ahead of the anchor
function project(v, x, y, z) {
    const c = Math.cos(v.a), s = Math.sin(v.a);
    const xl = x + v.fwd;
    const xw = xl * c - z * s, zw = xl * s + z * c;
    return [S * xw - v.dx0, KZ * zw - KY * y - v.dy0];
}

// Every direction of a critter FRM paints the critter's SHADOW on the ground around its feet, and
// carved as body it stood up as the "blocks between the legs". It is always the palette's darkest
// entries -- the standing vault dweller's is one flat 207 (3,3,3); his walk dithers it from 100
// (dark green), 31 (dark red) and 98 -- while the boots are the lighter greys 202/13/14/15 and
// brown 78. One entry is both: 47, a dark blue that is also the trousers' folds; in the shadow it
// is dithered in among the others, so it counts as shadow only next to them. Tried and rendered
// before this: a height rule (the plate stands up to 19 layers), and two ray tests (one cut the
// boots off, the other kept the plate) -- colour, read per animation, is what separates them.
// Only the lower part of the sprite, and only for creatures on their feet (see carve's trim).
let PAL = null;
function setPalette(pal) { PAL = pal; }
function shadowClass(c) { // 0 = body, 1 = shadow, 2 = shadow only among shadow
    if (!PAL) return c === 207 ? 1 : 0;
    const r = PAL[c * 3], g = PAL[c * 3 + 1], b = PAL[c * 3 + 2];
    if (Math.max(r, g, b) > 8) return 0;
    if (r === g && g === b && r >= 7) return 0; // 202, 15: the boots' greys
    if (b >= r + 3 && b >= g) return 2; // 47: dark blue
    return 1;
}
function silhouette(fr, trim = true) {
    if (!trim) {
        if (!fr.mask0) fr.mask0 = fr.data.map((c) => (c ? 1 : 0));
        return fr.mask0;
    }
    if (fr.mask) return fr.mask;
    const { w, h, data } = fr;
    const mask = data.map((c) => (c ? 1 : 0));
    const low = Math.floor(h * 0.65);
    const cls = new Uint8Array(w * h);
    for (let p = low * w; p < w * h; p++) if (data[p]) cls[p] = shadowClass(data[p]);
    // ...and a dark pixel touching the body (a trouser's shaded edge, a boot's outline) stays:
    // dropping those too thinned every leg by a pixel per view, six views over, and the boots
    // went with them
    for (let y = low; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const p = y * w + x;
            if (!cls[p]) continue;
            let shadowNear = 0, bodyNear = false;
            for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
                const xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || xx >= w || yy >= h || (!dx && !dy)) continue;
                const q = yy * w + xx;
                if (cls[q] === 1) shadowNear++;
                else if (data[q] && !cls[q]) bodyNear = true;
            }
            if (bodyNear) continue;
            if (cls[p] === 1 || shadowNear >= 2) mask[p] = 0;
        }
    }
    return (fr.mask = mask);
}
const isShadow = (fr) => silhouette(fr).map((m, p) => (fr.data[p] && !m ? 1 : 0));

function carve(views, { trim = true, shadowLayers = 3 } = {}) {
    // bounds from the sprites around the body (not the walk's travel), snapped to the voxel grid
    // so that voxel centres sit at (index + 0.5) * VOX
    let half = 0, tall = 0;
    for (const v of views) {
        half = Math.max(half, Math.abs(v.bx0), Math.abs(v.bx0 + v.fr.w));
        tall = Math.max(tall, -v.by0);
    }
    const R = Math.ceil((half / S + 0.3) / VOX) * VOX, H = tall / KY + 0.6;
    const nx = Math.round((2 * R) / VOX), ny = Math.ceil((H + 0.25) / VOX), nz = nx;
    const x0 = -R, y0 = -8 * VOX, z0 = -R;
    const occ = new Uint8Array(nx * ny * nz);
    const idx = (i, j, k) => (j * nz + k) * nx + i;
    for (let j = 0; j < ny; j++) {
        const y = y0 + (j + 0.5) * VOX;
        for (let k = 0; k < nz; k++) {
            const z = z0 + (k + 0.5) * VOX;
            for (let i = 0; i < nx; i++) {
                const x = x0 + (i + 0.5) * VOX;
                let inside = true;
                for (const v of views) {
                    const [u, w] = project(v, x, y, z);
                    const pu = Math.floor(u), pv = Math.floor(w);
                    if (pu < 0 || pv < 0 || pu >= v.fr.w || pv >= v.fr.h || !silhouette(v.fr, trim)[pv * v.fr.w + pu]) { inside = false; break; }
                }
                if (inside) occ[idx(i, j, k)] = 1;
            }
        }
    }
    // Leftover crumbs of the shadow: a column that never rises above the lowest layers.
    const jg = Math.round(-y0 / VOX); // the first layer at ground level
    for (let k = 0; k < nz; k++) {
        for (let i = 0; i < nx; i++) {
            let top = -1;
            for (let j = ny - 1; j >= 0; j--) if (occ[idx(i, j, k)]) { top = j; break; }
            if (trim && top >= 0 && top < jg + shadowLayers) for (let j = 0; j <= top; j++) occ[idx(i, j, k)] = 0;
            // and the shadow blob also reaches BELOW the feet line on screen, which carved a plate
            // under the soles -- nothing of a critter is below the ground
            for (let j = 0; j < jg; j++) occ[idx(i, j, k)] = 0;
        }
    }
    return { occ, nx, ny, nz, x0, y0, z0, idx, trim };
}

// A creature on its feet: shadow trimmed, and the loose crumbs it leaves swept up.
function carveStanding(views) {
    return dropCrumbs(carve(views, { trim: true }));
}

// Loose bits the shadow leaves behind: connected lumps of fewer than 12 voxels.
function dropCrumbs(grid) {
    const { occ, nx, ny, nz, idx } = grid;
    const seen = new Uint8Array(occ.length);
    const stack = [];
    for (let start = 0; start < occ.length; start++) {
        if (!occ[start] || seen[start]) continue;
        const lump = [];
        stack.push(start);
        seen[start] = 1;
        while (stack.length) {
            const p = stack.pop();
            lump.push(p);
            const i = p % nx, k = Math.floor(p / nx) % nz, j = Math.floor(p / (nx * nz));
            for (const [di, dj, dk] of [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]]) {
                const ii = i + di, jj = j + dj, kk = k + dk;
                if (ii < 0 || jj < 0 || kk < 0 || ii >= nx || jj >= ny || kk >= nz) continue;
                const q = idx(ii, jj, kk);
                if (occ[q] && !seen[q]) { seen[q] = 1; stack.push(q); }
            }
        }
        if (lump.length < 12) for (const p of lump) occ[p] = 0;
    }
    return grid;
}

// A standing pose is left-right symmetric; the carve is not (six views at 60 deg steps each leave
// their own lumps). Keeping only voxels whose mirror image across the body's middle plane is also
// solid trims those lumps -- the "blob" -- and nothing the real body has. Never for walks or runs.
function symmetrize(grid) {
    const { occ, nx, ny, nz, idx } = grid;
    let sum = 0, n = 0;
    for (let j = 0; j < ny; j++) for (let k = 0; k < nz; k++) for (let i = 0; i < nx; i++) {
        if (occ[idx(i, j, k)]) { sum += k; n++; }
    }
    if (!n) return grid;
    const twice = Math.round((2 * sum) / n); // mirror k -> twice - k, on whole or half voxels
    const keep = new Uint8Array(occ.length);
    for (let j = 0; j < ny; j++) for (let k = 0; k < nz; k++) {
        const m = twice - k;
        if (m < 0 || m >= nz) continue;
        for (let i = 0; i < nx; i++) if (occ[idx(i, j, k)] && occ[idx(i, j, m)]) keep[idx(i, j, k)] = 1;
    }
    grid.occ.set(keep);
    return grid;
}

// Surface voxels, each coloured from the view that sees it most squarely.
function colour(grid, views) {
    const { occ, nx, ny, nz, x0, y0, z0, idx } = grid;
    const at = (i, j, k) => i >= 0 && j >= 0 && k >= 0 && i < nx && j < ny && k < nz && occ[idx(i, j, k)];
    const surf = [];
    for (let j = 0; j < ny; j++) for (let k = 0; k < nz; k++) for (let i = 0; i < nx; i++) {
        if (!occ[idx(i, j, k)]) continue;
        if (at(i + 1, j, k) && at(i - 1, j, k) && at(i, j + 1, k) && at(i, j - 1, k) && at(i, j, k + 1) && at(i, j, k - 1)) continue;
        // outward normal: towards the empty part of the 3x3x3 neighbourhood
        let n = [0, 0, 0];
        for (let dj = -1; dj <= 1; dj++) for (let dk = -1; dk <= 1; dk++) for (let di = -1; di <= 1; di++) {
            if ((di || dj || dk) && !at(i + di, j + dj, k + dk)) { n[0] += di; n[1] += dj; n[2] += dk; }
        }
        const len = Math.hypot(...n) || 1;
        surf.push({ i, j, k, x: x0 + (i + 0.5) * VOX, y: y0 + (j + 0.5) * VOX, z: z0 + (k + 0.5) * VOX, n: n.map((q) => q / len), c: 0, best: -2 });
    }
    // per view: depth buffer of the surface, nearest to the camera wins
    for (const v of views) {
        const cs = Math.cos(v.a), sn = Math.sin(v.a);
        // the camera looks along world (0, -SIN, -COS); towards it in local = rotate back by -a
        const tc = [0 * cs + COS * sn, SIN, -0 * sn + COS * cs]; // R(-a) * (0, SIN, COS)
        const depth = new Float64Array(v.fr.w * v.fr.h).fill(-Infinity);
        const hits = [];
        for (const s of surf) {
            const [u, w] = project(v, s.x, s.y, s.z);
            const pu = Math.floor(u), pv = Math.floor(w);
            if (pu < 0 || pv < 0 || pu >= v.fr.w || pv >= v.fr.h) { hits.push(-1); continue; }
            const p = pv * v.fr.w + pu;
            const d = s.x * tc[0] + s.y * tc[1] + s.z * tc[2];
            if (d > depth[p]) depth[p] = d;
            hits.push(p);
        }
        surf.forEach((s, n) => {
            const p = hits[n];
            if (p < 0) return;
            const d = s.x * tc[0] + s.y * tc[1] + s.z * tc[2];
            if (d < depth[p] - 1.5 * VOX) return; // hidden behind nearer surface
            // Down the middle of the chest and the back two views tie (they flank it at +-30 deg)
            // and the pick flipped between them voxel by voxel -- that is what bent the zipper
            // stripe. Each half of the body now keeps to the views on its own side.
            const facing = s.n[0] * tc[0] + s.n[1] * tc[1] + s.n[2] * tc[2] + 0.08 * Math.sign(s.z) * Math.sign(tc[2]);
            if (facing > s.best && silhouette(v.fr, grid.trim !== false)[p]) { s.best = facing; s.c = v.fr.data[p]; }
        });
    }
    // anything no view saw takes a coloured neighbour's colour
    const key = (s) => (s.j * nz + s.k) * nx + s.i;
    const byKey = new Map(surf.map((s) => [key(s), s]));
    for (let pass = 0; pass < 6; pass++) {
        let left = 0;
        for (const s of surf) {
            if (s.c) continue;
            left++;
            for (const [di, dj, dk] of [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]]) {
                const o = byKey.get(((s.j + dj) * nz + (s.k + dk)) * nx + (s.i + di));
                if (o && o.c) { s.c = o.c; break; }
            }
        }
        if (!left) break;
    }
    return surf;
}

// How much of each original silhouette the hull reproduces when projected back.
function coverage(grid, views) {
    const { occ, nx, ny, nz, x0, y0, z0, idx } = grid;
    return views.map((v) => {
        const hit = new Uint8Array(v.fr.w * v.fr.h);
        for (let j = 0; j < ny; j++) for (let k = 0; k < nz; k++) for (let i = 0; i < nx; i++) {
            if (!occ[idx(i, j, k)]) continue;
            const [u, w] = project(v, x0 + (i + 0.5) * VOX, y0 + (j + 0.5) * VOX, z0 + (k + 0.5) * VOX);
            const pu = Math.floor(u), pv = Math.floor(w);
            if (pu >= 0 && pv >= 0 && pu < v.fr.w && pv < v.fr.h) hit[pv * v.fr.w + pu] = 1;
        }
        let sil = 0, got = 0;
        const m = silhouette(v.fr, grid.trim !== false);
        for (let p = 0; p < hit.length; p++) if (m[p]) { sil++; if (hit[p]) got++; }
        return got / (sil || 1);
    });
}

// ---------------------------------------------------------------- preview sheet
function crc32(buf) {
    let c, crc = 0xffffffff;
    for (let n = 0; n < buf.length; n++) {
        c = (crc ^ buf[n]) & 0xff;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        crc = c ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
}
function writePng(file, w, h, px, channels = 3) { // channels 3 = RGB, 4 = RGBA
    const chunk = (type, data) => {
        const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
        const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
        const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body));
        return Buffer.concat([len, body, crc]);
    };
    const stride = w * channels;
    const raw = Buffer.alloc((stride + 1) * h);
    for (let y = 0; y < h; y++) { raw[y * (stride + 1)] = 0; px.copy(raw, y * (stride + 1) + 1, y * stride, (y + 1) * stride); }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = channels === 4 ? 6 : 2;
    fs.writeFileSync(file, Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
}

// Render voxels orthographically: heading h (the camera orbits), elevation e, SC px per unit.
function renderInto(img, W, ox, oy, surf, pal, heading, elev, SC) {
    const ch = Math.cos(heading), sh = Math.sin(heading), se = Math.sin(elev), ce = Math.cos(elev);
    const zb = new Map();
    const r = Math.max(1, Math.round(SC * VOX * 1.05));
    for (const s of surf) {
        const xw = s.x * ch - s.z * sh, zw = s.x * sh + s.z * ch;
        const px = Math.round(ox + SC * xw), py = Math.round(oy + SC * (se * zw - ce * s.y));
        const d = zw * ce + s.y * se;
        for (let dy = 0; dy < r; dy++) for (let dx = 0; dx < r; dx++) {
            const k = (py + dy) * W + (px + dx);
            const prev = zb.get(k);
            if (prev !== undefined && prev >= d) continue;
            zb.set(k, d);
            img[k * 3] = pal[s.c * 3] * 4; img[k * 3 + 1] = pal[s.c * 3 + 1] * 4; img[k * 3 + 2] = pal[s.c * 3 + 2] * 4;
        }
    }
}

if (require.main === module) {
    const name = (process.argv[2] || 'HMJMPSAA').toUpperCase();
    const which = process.argv[3] || '0';
    const out = process.argv[4] || path.join(path.resolve(__dirname, '../../../build/paper3d'), 'hull_' + name + '.png');
    const critter = openDat(path.join(GAME, 'CRITTER.DAT'));
    const master = openDat(path.join(GAME, 'MASTER.DAT'));
    const pal = master.extract('COLOR.PAL');
    const frm = readFrm(critter.extract(['ART', 'CRITTERS', name + '.FRM'].join(BS)));
    const frames = which === 'all' ? [...Array(frm.fpd).keys()] : [+which];

    const SC = 96; // px per unit in the sheet
    const cellW = 200, cellH = 300;
    const headings = [0, 30, 60, 90, 120, 150, 180, 210, 240, 270, 300, 330];
    const rows = frames.length * 2; // original elevation + low
    const W = cellW * headings.length, H = cellH * rows;
    const img = Buffer.alloc(W * H * 3, 40);
    const acc = process.env.ACC === '1';
    frames.forEach((f, fi) => {
        const views = viewsOf(frm, f, acc);
        const t = Date.now();
        const grid = carve(views);
        const cov = coverage(grid, views);
        const surf = colour(grid, views);
        console.log(`${name} frame ${f}: grid ${grid.nx}x${grid.ny}x${grid.nz}, ${surf.length} surface voxels, coverage per view ${cov.map((c) => (c * 100).toFixed(0) + '%').join(' ')} (${Date.now() - t} ms)`);
        headings.forEach((hd, hi) => {
            renderInto(img, W, hi * cellW + cellW / 2, (fi * 2) * cellH + cellH - 30, surf, pal, (hd * Math.PI) / 180, Math.asin(SIN), SC);
            renderInto(img, W, hi * cellW + cellW / 2, (fi * 2 + 1) * cellH + cellH - 30, surf, pal, (hd * Math.PI) / 180, (8 * Math.PI) / 180, SC);
        });
    });
    fs.mkdirSync(path.dirname(out), { recursive: true });
    writePng(out, W, H, img);
    console.log('wrote', out);
}

module.exports = { readFrm, viewsOf, project, carve, colour, coverage, renderInto, writePng, VOX, isShadow, symmetrize, carveStanding, setPalette, silhouette };
