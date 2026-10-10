// Turns a critter to directions the game never drew -- every 30 deg instead of 60, and at eye
// level, the way Doom drew its monsters -- with sprite3d.js's novelView: the carved body only says
// where each pixel's point is, the ORIGINAL sprites say what colour it is.
//
//   node turn.js <FRM> [frame] [out.png]          e.g.  node turn.js HMJMPSAA
//
// The sheet has 12 columns, the critter turned in 30 deg steps starting face-on:
//   row 1  the game's own sprites (every other column -- it drew only 6 directions)
//   row 2  turned, seen from the game's camera (25.7 deg above)
//   row 3  turned, seen at eye level (Doom)

'use strict';

const path = require('path');
const { openDat } = require('../f1script/dat.js');
const hull = require('./hull.js');
const s3 = require('./sprite3d.js');

const GAME = path.resolve(__dirname, '../../../build/Release');
const BS = String.fromCharCode(92);
const name = (process.argv[2] || 'HMJMPSAA').toUpperCase();
const frame = +(process.argv[3] || 0);
const out = process.argv[4] || path.resolve(__dirname, `../../../build/paper3d/sprite3d/turn_${name.toLowerCase()}_${frame}.png`);

const pal = openDat(path.join(GAME, 'MASTER.DAT')).extract('COLOR.PAL');
hull.setPalette(pal);
const frm = hull.readFrm(openDat(path.join(GAME, 'CRITTER.DAT')).extract(['ART', 'CRITTERS', name + '.FRM'].join(BS)));
const views = hull.viewsOf(frm, frame, false);
const mesh = s3.smoothModel(views, pal);

const COLS = 12, CW = 44, CH = 84, AX = 22, AY = 79, ZOOM = 5; // cell, the anchor in a cell, upscale
const ROW_BG = [[255, 255, 255], [243, 243, 243], [230, 237, 247]];
const GAME_ELEVATION = Math.asin(Math.sqrt(3) / 4);
const W = COLS * CW, H = 3 * CH;
const sheet = Buffer.alloc(W * H * 3);
for (let row = 0; row < 3; row++) {
    for (let y = row * CH; y < (row + 1) * CH; y++) for (let x = 0; x < W; x++) sheet.set(ROW_BG[row], (y * W + x) * 3);
}
const put = (row, col, x, y, rgb) => {
    const X = col * CW + AX + x, Y = row * CH + AY + y;
    if (X >= col * CW && X < (col + 1) * CW && Y >= row * CH && Y < (row + 1) * CH) sheet.set(rgb, (Y * W + X) * 3);
};

for (let col = 0; col < COLS; col++) {
    const deg = 90 - col * 30; // 90 = facing the camera; the game's direction r faces (r - 1) * 60
    const r = [0, 1, 2, 3, 4, 5].find((q) => (((q - 1) * 60 - deg) % 360 + 360) % 360 === 0);
    if (r !== undefined) { // the original, shadow trimmed, anchored on its hex centre
        const v = views[r], fr = v.fr, keep = hull.silhouette(fr);
        for (let y = 0; y < fr.h; y++) for (let x = 0; x < fr.w; x++) {
            const p = y * fr.w + x;
            if (keep[p]) put(0, col, x + v.bx0, y + v.by0, [pal[fr.data[p] * 3] * 4, pal[fr.data[p] * 3 + 1] * 4, pal[fr.data[p] * 3 + 2] * 4]);
        }
    }
    const shares = [];
    [GAME_ELEVATION, 0].forEach((elevation, i) => {
        const img = s3.novelView(mesh, views, { azimuth: (deg * Math.PI) / 180, elevation });
        shares.push(img.fromSprites);
        for (let y = 0; y < img.H; y++) for (let x = 0; x < img.W; x++) {
            const o = (y * img.W + x) * 4;
            if (img.rgba[o + 3]) put(1 + i, col, x - img.ax, y - img.ay, [img.rgba[o], img.rgba[o + 1], img.rgba[o + 2]]);
        }
    });
    console.log(`${String(deg).padStart(4)} deg${r !== undefined ? ` (the game's direction ${r})` : ''}: from sprite pixels ${shares.map((s) => (s * 100).toFixed(0) + '%').join(' / ')}`);
}

const big = Buffer.alloc(W * ZOOM * H * ZOOM * 3);
for (let y = 0; y < H * ZOOM; y++) for (let x = 0; x < W * ZOOM; x++) {
    sheet.copy(big, (y * W * ZOOM + x) * 3, (Math.floor(y / ZOOM) * W + Math.floor(x / ZOOM)) * 3, (Math.floor(y / ZOOM) * W + Math.floor(x / ZOOM)) * 3 + 3);
}
require('fs').mkdirSync(path.dirname(out), { recursive: true });
hull.writePng(out, W * ZOOM, H * ZOOM, big);
console.log('wrote', out);
