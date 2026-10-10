// Renders the weapon slot (188x67 button) the way intface_redraw_items does, with the aimed part
// printed left of the bullseye, so the layout can be LOOKED at before it goes to the Deck.
const fs = require('fs');
const zlib = require('zlib');
const { openDat } = require('C:/Users/MS/Documents/GameDev/fallout1-ce/.claude/tools/f1script/dat.js');

const dat = openDat('C:/Users/MS/Documents/GameDev/fallout1-ce/build/Release/MASTER.DAT');
const BS = String.fromCharCode(92);
const get = (p) => { const b = dat.extract(p.toUpperCase().split('/').join(BS)); if (!b) throw new Error('missing ' + p); return b; };

function lst(p) { return get(p).toString('latin1').split(/\r?\n/).map(l => l.trim().split(/[\s;]/)[0]); }
function frm(buf) {
    const p = 62 + buf.readUInt32BE(34);
    const w = buf.readUInt16BE(p), h = buf.readUInt16BE(p + 2), size = buf.readUInt32BE(p + 4);
    return { w, h, data: buf.subarray(p + 12, p + 12 + size) };
}
const pal = get('color.pal');
const rgbOf = (i) => [pal[i * 3] * 4, pal[i * 3 + 1] * 4, pal[i * 3 + 2] * 4];

const iface = lst('art/intrface/intrface.lst');
const ifrm = (n) => frm(get('art/intrface/' + iface[n]));
const inven = lst('art/inven/inven.lst');

// font1.aaf (text_font(101))
const aaf = get('font1.aaf');
const font = { maxH: aaf.readInt16BE(4), letter: aaf.readInt16BE(6), word: aaf.readInt16BE(8), glyphs: [] };
for (let i = 0; i < 256; i++) {
    const o = 12 + i * 8;
    font.glyphs.push({ w: aaf.readInt16BE(o), h: aaf.readInt16BE(o + 2), off: aaf.readInt32BE(o + 4) });
}
const fdata = aaf.subarray(2060);
const textWidth = (s) => { let w = 0; for (const ch of s) w += (ch === ' ' ? font.word : font.glyphs[ch.charCodeAt(0)].w) + font.letter; return w - font.letter; };

function render(itemName, part, outFile) {
    const W = 188, H = 67;
    const img = []; // rgb per pixel
    const blank = ifrm(32);
    for (let i = 0; i < W * H; i++) img.push(rgbOf(blank.data[i]));
    const blit = (f, x0, y0) => {
        for (let y = 0; y < f.h; y++) for (let x = 0; x < f.w; x++) {
            const v = f.data[y * f.w + x];
            if (v !== 0 && x0 + x >= 0 && x0 + x < W && y0 + y >= 0 && y0 + y < H) img[(y0 + y) * W + x0 + x] = rgbOf(v);
        }
    };
    const text = (s, x0, y0, rgb) => {
        let x = x0;
        for (const ch of s) {
            if (ch === ' ') { x += font.word + font.letter; continue; }
            const g = font.glyphs[ch.charCodeAt(0)];
            const top = y0 + font.maxH - g.h;
            for (let y = 0; y < g.h; y++) for (let xx = 0; xx < g.w; xx++) {
                const v = fdata[g.off + y * g.w + xx];
                if (!v) continue;
                const a = Math.min(1, v / 7);
                const i = (top + y) * W + x + xx;
                img[i] = img[i].map((c, k) => Math.round(c + (rgb[k] - c) * a));
            }
            x += g.w + font.letter;
        }
    };

    // bullseye bottom-right, as intface_redraw_items
    const bull = ifrm(288);
    blit(bull, 181 - bull.w, 60 - bull.h);

    // attack type (single) top-right, AP bottom-left
    const single = ifrm(43);
    blit(single, 181 - single.w, 7);
    const ap = ifrm(289);
    blit(ap, 7, 60 - ap.h);
    const digits = ifrm(290);
    const d = 4;
    for (let y = 0; y < digits.h; y++) for (let x = 0; x < 10; x++) {
        const v = digits.data[y * digits.w + d * 10 + x];
        if (v) img[(60 - digits.h + y) * W + 7 + ap.w + 7 + x] = rgbOf(v);
    }
    // item centered
    const idx = inven.findIndex(n => n.toUpperCase() === itemName.toUpperCase());
    if (idx < 0) throw new Error('no inven art ' + itemName);
    const it = frm(get('art/inven/' + inven[idx]));
    blit(it, Math.floor((W - it.w) / 2), Math.floor((67 - it.h) / 2));
    if (part) {
      if (process.env.SMALL) {
        const { SMALL } = require(__dirname + '/smallglyphs.js');
        const SH = 8, I = ifrm(43); // bevel source: SINGLE's I, cols 9..11
        const lum = (p) => { const c = rgbOf(p); return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; };
        const pix = (n) => { const a = ifrm(n); const out = []; for (const v of a.data) if (v) out.push(v); return out; };
        const A = pix(290).sort((p, q) => lum(p) - lum(q));
        const B = [43, 287, 45, 40, 117].flatMap(pix).map(lum).sort((p, q) => p - q);
        const remap = (v) => { const L0 = lum(v); let lo = 0; while (lo < B.length && B[lo] < L0) lo++; let hi = lo; while (hi < B.length && B[hi] === L0) hi++; return A[Math.min(A.length - 1, Math.floor(((lo + hi) / 2) / B.length * A.length))]; };
        const GAP = 1, SPACE = 3;
        let lw = 0; for (let ci = 0; ci < part.length; ci++) { const ch = part[ci]; lw += (ch === ' ' ? SPACE : SMALL[ch][0].length) + (ci < part.length - 1 ? GAP : 0); }
        const apA = ifrm(289); const x0 = 7 + apA.w + 7 + 10 + 5; const y0 = (60 - apA.h) + Math.floor((apA.h - SH + 1) / 2);
        let x = x0;
        for (let ci = 0; ci < part.length; ci++) { const ch = part[ci];
            if (ch === ' ') { x += SPACE + GAP; continue; }
            const g = SMALL[ch]; const w = g[0].length;
            for (let y = 0; y < SH; y++) { const row = g[y]; const sy = Math.floor(y * 11 / SH);
                for (let gx = 0; gx < w; gx++) { if (row[gx] !== '#') continue;
                    let a = gx; while (a > 0 && row[a - 1] === '#') a--; let b = gx; while (b < w - 1 && row[b + 1] === '#') b++;
                    const run = b - a + 1; let col; if (run === 1) col = 1; else if (run === 2) col = process.env.TWO_MID ? 1 : (gx === a ? 1 : 2); else col = gx === a ? 0 : (gx === b ? 2 : 1);
                    let v; if (process.env.MULTI) { const SRCS = process.env.BRIGHT ? [[43, 44], [45, 10], [45, 18], [40, 0], [40, 22], [43, 36], [43, 13]] : [[43, 9], [43, 13], [43, 21], [43, 36], [43, 44], [45, 10], [45, 18]]; const k = (ci * 3 + a * 2) % SRCS.length; const sa = ifrm(SRCS[k][0]); v = sa.data[sy * sa.w + SRCS[k][1] + col]; if (!v) v = I.data[sy * I.w + 9 + col]; } else v = I.data[sy * I.w + 9 + col]; if (process.env.REMAP) v = remap(v);
                    img[(y0 + y) * W + x + gx] = rgbOf(v); } }
            x += w + GAP; }
        console.log('small', part, 'w', lw, 'x0', x0, 'y0', y0);
      } else {
        // worn label CUT from the vanilla word art (shape and colour), Y shaded like I's strokes
        const { GLYPHS } = require(__dirname + '/wornglyphs.js');
        const art = {}; const A = (n) => art[n] || (art[n] = ifrm(n));
        const LH = 12; // tallest glyph (THROW's O); 11-row glyphs sit on the same bottom row
        const KERN = {}; (process.env.KERN || '').split(',').filter(Boolean).forEach(p => { const [k, v] = p.split(':'); KERN[k] = Number(v); });
        const K = (i) => KERN[part[i] + (part[i + 1] || '')] || 0;
        let lw = 0; for (let ci = 0; ci < part.length; ci++) { const ch = part[ci]; lw += (ch === ' ' ? 4 : (GLYPHS[ch].w || GLYPHS[ch].rows[0].length)) + 1 + (ci < part.length - 1 ? K(ci) : 0); } lw -= 1;
        const lab = new Array(lw * LH).fill(0);
        let x = 0;
        for (let ci = 0; ci < part.length; ci++) { const ch = part[ci];
            if (ch === ' ') { x += 5; continue; }
            const g = GLYPHS[ch];
            const yo = LH - g.h;
            if (g.rows) {
                const src = A(g.shade.art);
                const w = g.rows[0].length;
                for (let y = 0; y < g.h; y++) {
                    const row = g.rows[y];
                    for (let gx = 0; gx < w; gx++) {
                        if (row[gx] !== '#') continue;
                        let a = gx; while (a > 0 && row[a - 1] === '#') a--;
                        let b = gx; while (b < w - 1 && row[b + 1] === '#') b++;
                        const col = gx === a ? 0 : (gx === b ? 2 : 1);
                        lab[(yo + y) * lw + x + gx] = src.data[(g.shade.y + y) * src.w + g.shade.x + col];
                    }
                }
                x += w + 1 + K(ci);
            } else {
                const src = A(g.art);
                const skip = new Set((g.skip || []).map(p => p[0] + ',' + p[1]));
                for (let y = 0; y < g.h; y++) for (let gx = 0; gx < g.w; gx++) {
                    if (skip.has((g.x + gx) + ',' + (g.y + y))) continue;
                    const v = src.data[(g.y + y) * src.w + g.x + gx];
                    if (v) lab[(yo + y) * lw + x + gx] = v;
                }
                x += g.w + 1 + K(ci);
            }
        }
        if (process.env.REMAP) {
            // luminance-rank remap: the word art's colours onto the AP cost's (MVEPNT 289 + digits 290)
            const lum = (p) => { const c = rgbOf(p); return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; };
            const pix = (n) => { const a = ifrm(n); const out = []; for (const v of a.data) if (v) out.push(v); return out; };
            const A = (process.env.REMAP === 'digits' ? pix(290) : [...pix(289), ...pix(290)]).sort((p, q) => lum(p) - lum(q));
            const B = [43, 287, 45, 40, 117].flatMap(pix).map(lum).sort((p, q) => p - q);
            const remap = {};
            for (let k = 0; k < lab.length; k++) { const v = lab[k]; if (!v) continue; if (remap[v] === undefined) {
                const L0 = lum(v); let lo = 0; while (lo < B.length && B[lo] < L0) lo++; let hi = lo; while (hi < B.length && B[hi] === L0) hi++;
                const r = ((lo + hi) / 2) / B.length; remap[v] = A[Math.min(A.length - 1, Math.floor(r * A.length))]; }
                lab[k] = remap[v]; }
            console.log('remap', JSON.stringify(remap));
        }
        const SC = Number(process.env.SCALE || 1), MODE = process.env.MODE || 'box';
        const dw = Math.round(lw * SC), dh = Math.round(LH * SC);
        let x0 = 181 - bull.w - 3 - dw;
        let y0 = process.env.CENTER ? Math.round((60 - bull.h) + bull.h / 2 - dh / 2) : 60 - dh;
        if (process.env.POS === "ap") { const apA = ifrm(289); x0 = 7 + apA.w + 7 + 10 + Number(process.env.GAP || 5); y0 = (60 - apA.h) + Math.floor((apA.h - dh + 1) / 2); console.log("ap", apA.w, apA.h, "x0", x0, "y0", y0); }
        const palRGB = []; for (let p = 0; p < 256; p++) palRGB.push(rgbOf(p));
        const nearest = (c) => { let best = 1, bd = 1e9; for (let p = 1; p < 256; p++) { const q = palRGB[p]; const d = (q[0]-c[0])**2 + (q[1]-c[1])**2 + (q[2]-c[2])**2; if (d < bd) { bd = d; best = p; } } return palRGB[best]; };
        for (let dy = 0; dy < dh; dy++) for (let dx = 0; dx < dw; dx++) {
            if (MODE === 'nn' || SC === 1) { const sx = Math.min(lw - 1, Math.floor((dx + 0.5) / SC)), sy = Math.min(LH - 1, Math.floor((dy + 0.5) / SC)); const v = lab[sy * lw + sx]; if (v) img[(y0 + dy) * W + x0 + dx] = rgbOf(v); continue; }
            const sx0 = dx / SC, sx1 = (dx + 1) / SC, sy0 = dy / SC, sy1 = (dy + 1) / SC;
            let sa = 0, sr = 0, sg = 0, sb = 0;
            for (let sy = Math.floor(sy0); sy < Math.ceil(sy1) && sy < LH; sy++) for (let sx = Math.floor(sx0); sx < Math.ceil(sx1) && sx < lw; sx++) {
                const wx = Math.min(sx + 1, sx1) - Math.max(sx, sx0), wy = Math.min(sy + 1, sy1) - Math.max(sy, sy0); const w = wx * wy; const v = lab[sy * lw + sx]; if (!v) continue; const c = rgbOf(v); sa += w; sr += w * c[0]; sg += w * c[1]; sb += w * c[2]; }
            const area = (sx1 - sx0) * (sy1 - sy0); let a = sa / area; a = Math.min(1, a * Number(process.env.ABOOST || 1)); if (a < (Number(process.env.ATH) || 0.25)) continue;
            const bg = img[(y0 + dy) * W + x0 + dx]; const col = [sr / sa, sg / sa, sb / sa];
            img[(y0 + dy) * W + x0 + dx] = nearest([0, 1, 2].map(k => bg[k] * (1 - a) + col[k] * a));
        }
        console.log('label', part, 'w', lw, 'x0', x0);
      }
    }

    // scale x3 for viewing
    const S = 3, raw = Buffer.alloc((W * S * 3 + 1) * H * S);
    let o = 0;
    for (let y = 0; y < H * S; y++) {
        raw[o++] = 0;
        for (let x = 0; x < W * S; x++) { const c = img[Math.floor(y / S) * W + Math.floor(x / S)]; raw[o++] = c[0]; raw[o++] = c[1]; raw[o++] = c[2]; }
    }
    const crcT = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crcT[n] = c >>> 0; }
    const crc = (b) => { let c = 0xffffffff; for (const x of b) c = crcT[(c ^ x) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
    const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const body = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(body)); return Buffer.concat([l, body, c]); };
    const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W * S, 0); ihdr.writeUInt32BE(H * S, 4); ihdr[8] = 8; ihdr[9] = 2;
    fs.writeFileSync(outFile, Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
    console.log(outFile, 'bull', bull.w + 'x' + bull.h, 'font h', font.maxH, 'text w', part ? textWidth(part) : 0, 'item', it.w + 'x' + it.h);
}

const out = process.argv[2];
const items = inven.filter(n => /PIST|RIFL|HUNT|SNIP|SHOT|DESERT|SMG/i.test(n));
console.log('candidates:', items.slice(0, 20).join(' '));
render(process.argv[3] || items[0], process.argv[5] || 'EYES', out + '/slot_eyes.png');
if (process.argv[4]) render(process.argv[4], process.argv[6] || 'L ARM', out + '/slot_larm.png');
