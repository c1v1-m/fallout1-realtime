// The ORIGINAL game sprites of a critter, blown up for an image-to-3D generator (Hunyuan3D):
// all six directions of one frame, ground shadow removed, on white, 1024 x 1024, in two flavours
// -- crisp (nearest neighbour, the pixels as drawn) and smooth (the OUTLINE through Scale2x three
// times, so its staircase comes out as clean slopes, and the colour inside bilinear). Scale2x on
// the colours themselves was tried and looked like camouflage: these sprites are rendered, every
// pixel a slightly different shade, and a scaler that works on equal neighbours turns that noise
// into blotches. A third flavour, cutout, is smooth with a TRANSPARENT background (RGBA): what a
// generator's API takes as-is, instead of guessing the background away itself (trellis.js).
//
//   node sprite_views.js [FRM=HMJMPSAA] [frame=0] [outDir]
//
// The game drew critters from above (25.7 deg) at 60 deg steps, so there is no true front or back
// view: the files are named after the multi-view slot each fits best --
//   front = SE (three-quarter front)   back = NW (three-quarter back)
//   right = E  (a true profile: facing east he shows the camera his right side)   left = W
//   front_alt = SW, back_alt = NE

'use strict';

const fs = require('fs');
const path = require('path');
const { openDat } = require('../f1script/dat.js');
const hull = require('./hull.js');

const GAME = path.resolve(__dirname, '../../../build/Release');
const BS = String.fromCharCode(92);
const [name = 'HMJMPSAA', frameArg = '0', outArg] = process.argv.slice(2);
const outDir = outArg || path.resolve(__dirname, '../../../build/paper3d/hunyuan_input/sprites');
const SIZE = 1024, FILL = 0.9;
const SLOTS = { 0: 'back_alt', 1: 'right', 2: 'front', 3: 'front_alt', 4: 'left', 5: 'back' };

const pal = openDat(path.join(GAME, 'MASTER.DAT')).extract('COLOR.PAL');
hull.setPalette(pal);
const frm = hull.readFrm(openDat(path.join(GAME, 'CRITTER.DAT')).extract(['ART', 'CRITTERS', name + '.FRM'].join(BS)));
const f = Math.min(+frameArg, frm.fpd - 1);
const rgbOf = (c) => [pal[c * 3] * 4, pal[c * 3 + 1] * 4, pal[c * 3 + 2] * 4];

// one scale for all six, so the views agree on size
const tallest = Math.max(...frm.dirs.map((d) => Math.max(d[f].h, d[f].w)));
const scale = Math.floor((SIZE * FILL) / tallest);

// Scale2x (EPX) on palette indices: every pixel becomes four, and a corner takes its neighbours'
// colour where two of them agree -- a staircase edge turns into a slope, nothing gets blurred.
function scale2x({ w, h, idx }) {
    const out = new Uint8Array(w * h * 4), at = (x, y) => idx[Math.min(h - 1, Math.max(0, y)) * w + Math.min(w - 1, Math.max(0, x))];
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const P = at(x, y), A = at(x, y - 1), B = at(x + 1, y), C = at(x - 1, y), D = at(x, y + 1);
            const o = y * 2 * w * 2 + x * 2;
            out[o] = C === A && C !== D && A !== B ? A : P;
            out[o + 1] = A === B && A !== C && B !== D ? B : P;
            out[o + w * 2] = D === C && D !== B && C !== A ? C : P;
            out[o + w * 2 + 1] = B === D && B !== A && D !== C ? D : P;
        }
    }
    return { w: w * 2, h: h * 2, idx: out };
}

const FLAVOURS = ['crisp', 'smooth', 'cutout'];
for (const flavour of FLAVOURS) fs.mkdirSync(path.join(outDir, flavour), { recursive: true });
frm.dirs.forEach((frames, r) => {
    const fr = frames[f];
    const keep = hull.silhouette(fr); // opaque and not ground shadow
    const base = { w: fr.w, h: fr.h, idx: fr.data.map((c, p) => (keep[p] ? c : 0)) };
    let outline = { w: fr.w, h: fr.h, idx: base.idx.map((c) => (c ? 1 : 0)) };
    for (let i = 0; i < 3; i++) outline = scale2x(outline);
    const k = outline.w / base.w;
    // colour at a pixel of an index grid (white where empty)
    const src = (g, x, y) => {
        if (x < 0 || y < 0 || x >= g.w || y >= g.h) return [255, 255, 255];
        const c = g.idx[y * g.w + x];
        return c ? rgbOf(c) : [255, 255, 255];
    };
    const inside = (x, y) => x >= 0 && y >= 0 && x < base.w && y < base.h && base.idx[y * base.w + x] !== 0;
    const covered = (x, y) => x >= 0 && y >= 0 && x < outline.w && y < outline.h && outline.idx[y * outline.w + x] !== 0;
    // bilinear over the body's own pixels only, so the edge keeps its colour instead of fading to white
    const colourAt = (u, v) => {
        const fx = u - 0.5, fy = v - 0.5, x0 = Math.floor(fx), y0 = Math.floor(fy), tx = fx - x0, ty = fy - y0;
        const acc = [0, 0, 0];
        let sum = 0;
        for (const [x, y, wgt] of [[x0, y0, (1 - tx) * (1 - ty)], [x0 + 1, y0, tx * (1 - ty)], [x0, y0 + 1, (1 - tx) * ty], [x0 + 1, y0 + 1, tx * ty]]) {
            if (!inside(x, y) || wgt <= 0) continue;
            const c = src(base, x, y);
            for (let q = 0; q < 3; q++) acc[q] += c[q] * wgt;
            sum += wgt;
        }
        if (sum > 0) return acc.map((a) => a / sum);
        for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
            if (inside(Math.floor(u) + dx, Math.floor(v) + dy)) return src(base, Math.floor(u) + dx, Math.floor(v) + dy);
        }
        return [255, 255, 255];
    };
    // how much of the output pixel the (Scale2x-smoothed) outline covers, antialiased
    const coverageAt = (u, v) => {
        const fx = u * k - 0.5, fy = v * k - 0.5, x0 = Math.floor(fx), y0 = Math.floor(fy), tx = fx - x0, ty = fy - y0;
        return covered(x0, y0) * (1 - tx) * (1 - ty) + covered(x0 + 1, y0) * tx * (1 - ty) + covered(x0, y0 + 1) * (1 - tx) * ty + covered(x0 + 1, y0 + 1) * tx * ty;
    };
    const ox = Math.round((SIZE - fr.w * scale) / 2), oy = Math.round(SIZE * (1 - (1 - FILL) / 2) - fr.h * scale);
    for (const flavour of FLAVOURS) {
        const ch = flavour === 'cutout' ? 4 : 3;
        const img = Buffer.alloc(SIZE * SIZE * ch, 255);
        if (ch === 4) for (let p = 3; p < img.length; p += 4) img[p] = 0; // transparent
        for (let y = 0; y < SIZE; y++) {
            for (let x = 0; x < SIZE; x++) {
                const u = (x - ox) / scale, v = (y - oy) / scale;
                if (u < -1 || v < -1 || u > fr.w + 1 || v > fr.h + 1) continue;
                let px;
                if (flavour === 'crisp') {
                    px = src(base, Math.floor(u), Math.floor(v));
                } else {
                    const cov = coverageAt(u, v);
                    if (cov <= 0) continue;
                    const c = colourAt(u, v);
                    // smooth sits on white; the cutout keeps the colour and carries coverage as alpha
                    px = ch === 4 ? [...c, cov * 255] : c.map((q) => q * cov + 255 * (1 - cov));
                }
                img.set(px.map(Math.round), (y * SIZE + x) * ch);
            }
        }
        const file = path.join(outDir, flavour, `${name.toLowerCase()}_${SLOTS[r]}.png`);
        hull.writePng(file, SIZE, SIZE, img, ch);
    }
});
console.log(`wrote ${Object.keys(SLOTS).length} views x ${FLAVOURS.length} flavours of ${name} frame ${f} (x${scale}) to ${outDir}`);
