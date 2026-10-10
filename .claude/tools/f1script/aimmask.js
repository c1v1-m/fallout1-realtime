// Hand-painted body-part masks for the gamepad aimed shot.
//
// The engine works out which pixel belongs to which body part by flooding the silhouette from
// guessed anatomical seeds (gpui_aimbody_partition in gamepad_ui.cc). That is good but not exact,
// so a mask painted by hand wins where one exists. This tool is both halves of that loop:
//
//   export  a critter frame as a big PNG with a palette strip and TWO panels -- paint the left
//           one, the right one stays as a reference
//   import  every painted PNG in a directory into build/Release/data/aimmask.dat, which the
//           engine loads at startup
//
// Usage:
//   node aimmask.js export <CRITTER.DAT> <FRMNAME> <rot|all> <outDir>   e.g. HFJMPSAA 2
//   node aimmask.js import <inDir> <CRITTER.DAT> <out.dat>
//   node aimmask.js check  <PNG> <CRITTER.DAT>
//   node aimmask.js preview <PNG> <CRITTER.DAT> <out.png>
//
// PAINTING RULES (they are what the importer reads):
//   * paint ONLY the left panel, with the flat colours from the strip at the top -- picked with
//     the pencil, no anti-aliasing, no gradients, no new colours
//   * anything you leave in its original colour belongs to no part: that is how the SHADOW (which
//     is part of the FRM, not the critter) and any pixel you are unsure about are marked
//   * limbs are painted BY THE SIDE YOU SEE THEM ON, never by the critter's own anatomy -- the
//     importer flips them for the rotations that look at the critter's back (NE and NW), exactly
//     like gpui_part_on_screen_left does in the engine

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { openDat } = require('./dat.js');

// ---------------------------------------------------------------- layout (matches the shipped
// annotate_*.png templates pixel for pixel; changing any of these invalidates already-painted art)
const SCALE = 8;
const MARGIN = 12;
const TOP = 62;
const SWATCH_X = 5, SWATCH_Y = 5, SWATCH_W = 36, SWATCH_H = 40, SWATCH_STEP = 40;

// HIT_LOCATION_* from src/game/combat_defs.h. -1 = no part (the engine's GPUI_OWNER_NONE).
const HEAD = 0, LEFT_ARM = 1, RIGHT_ARM = 2, TORSO = 3, RIGHT_LEG = 4, LEFT_LEG = 5, EYES = 6, GROIN = 7;
const NONE = -1;

// The strip, left to right. `side` is which side of the SCREEN the limb is seen on.
const SWATCHES = [
    { rgb: [255, 0, 0], name: 'head', part: HEAD },
    { rgb: [255, 255, 0], name: 'eyes', part: EYES },
    { rgb: [255, 128, 0], name: 'arm (screen left)', side: 'left', part: LEFT_ARM },
    { rgb: [0, 0, 255], name: 'arm (screen right)', side: 'right', part: RIGHT_ARM },
    { rgb: [0, 255, 0], name: 'leg (screen left)', side: 'left', part: LEFT_LEG },
    { rgb: [0, 255, 255], name: 'leg (screen right)', side: 'right', part: RIGHT_LEG },
    { rgb: [255, 0, 255], name: 'groin', part: GROIN },
    { rgb: [255, 255, 255], name: 'torso', part: TORSO },
    { rgb: [128, 128, 128], name: 'ignore', part: NONE },
];

// ROTATION_NE (0) and ROTATION_NW (5) are the two that look at a critter's back; there the
// critter's own left limb is the one on the LEFT of the screen. Everywhere else it is the right.
// Same rule as gpui_viewing_back() / gpui_part_on_screen_left().
function partForSide(kind, screenSide, rot) {
    const viewingBack = rot === 0 || rot === 5;
    const ownLeft = viewingBack ? screenSide === 'left' : screenSide === 'right';
    if (kind === 'arm') return ownLeft ? LEFT_ARM : RIGHT_ARM;
    return ownLeft ? LEFT_LEG : RIGHT_LEG;
}

function swatchPart(sw, rot) {
    if (!sw.side) return sw.part;
    const kind = sw.name.startsWith('arm') ? 'arm' : 'leg';
    return partForSide(kind, sw.side, rot);
}

// ---------------------------------------------------------------- FRM
function readFrm(buf) {
    const framesPerDirection = buf.readUInt16BE(8);
    const dataOffset = [];
    for (let i = 0; i < 6; i++) dataOffset.push(buf.readUInt32BE(34 + i * 4));
    const HEADER = 62;
    const dirs = [];
    for (let d = 0; d < 6; d++) {
        const frames = [];
        let p = HEADER + dataOffset[d];
        for (let f = 0; f < framesPerDirection; f++) {
            const w = buf.readUInt16BE(p);
            const h = buf.readUInt16BE(p + 2);
            const size = buf.readUInt32BE(p + 4);
            frames.push({ w, h, data: buf.subarray(p + 12, p + 12 + size) });
            p += 12 + size;
        }
        dirs.push(frames);
    }
    return { framesPerDirection, dirs };
}

function readPal(buf) {
    const pal = [];
    for (let i = 0; i < 256; i++) pal.push([buf[i * 3] * 4, buf[i * 3 + 1] * 4, buf[i * 3 + 2] * 4]);
    return pal;
}

// ---------------------------------------------------------------- PNG
function crc32(buf) {
    let c, crc = 0xffffffff;
    for (let n = 0; n < buf.length; n++) {
        c = (crc ^ buf[n]) & 0xff;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        crc = c ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(body));
    return Buffer.concat([len, body, crc]);
}

function writePng(file, w, h, rgb) {
    const raw = Buffer.alloc((w * 3 + 1) * h);
    let o = 0;
    for (let y = 0; y < h; y++) {
        raw[o++] = 0; // filter: none
        rgb.copy(raw, o, y * w * 3, (y + 1) * w * 3);
        o += w * 3;
    }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0);
    ihdr.writeUInt32BE(h, 4);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    fs.writeFileSync(file, Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk('IHDR', ihdr),
        chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
        chunk('IEND', Buffer.alloc(0)),
    ]));
}

function readPng(file) {
    const buf = fs.readFileSync(file);
    let off = 8, w = 0, h = 0, bitDepth = 0, colorType = 0, palette = null;
    const idat = [];
    while (off < buf.length) {
        const len = buf.readUInt32BE(off);
        const type = buf.toString('ascii', off + 4, off + 8);
        const data = buf.subarray(off + 8, off + 8 + len);
        if (type === 'IHDR') {
            w = data.readUInt32BE(0); h = data.readUInt32BE(4);
            bitDepth = data[8]; colorType = data[9];
            if (data[12] !== 0) throw new Error(file + ': interlaced PNG, re-save without interlace');
        } else if (type === 'PLTE') palette = data;
        else if (type === 'IDAT') idat.push(data);
        else if (type === 'IEND') break;
        off += 12 + len;
    }
    const raw = zlib.inflateSync(Buffer.concat(idat));
    const channels = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[colorType];
    if (channels === undefined) throw new Error(file + ': unsupported PNG colour type ' + colorType);
    if (bitDepth !== 8 && colorType !== 3) throw new Error(file + ': need 8-bit channels');
    const bpp = Math.max(1, (channels * bitDepth) >> 3);
    const rowBytes = Math.ceil(channels * bitDepth * w / 8);
    const out = Buffer.alloc(rowBytes * h);
    let pos = 0;
    for (let y = 0; y < h; y++) {
        const filter = raw[pos++];
        const line = raw.subarray(pos, pos + rowBytes); pos += rowBytes;
        const cur = out.subarray(y * rowBytes, (y + 1) * rowBytes);
        const prev = y > 0 ? out.subarray((y - 1) * rowBytes, y * rowBytes) : Buffer.alloc(rowBytes);
        for (let i = 0; i < rowBytes; i++) {
            const a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            let v = line[i];
            if (filter === 1) v += a;
            else if (filter === 2) v += b;
            else if (filter === 3) v += (a + b) >> 1;
            else if (filter === 4) {
                const p = a + b - c;
                const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
                v += pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
            }
            cur[i] = v & 0xff;
        }
    }
    // One accessor, so the caller never cares how the file was stored.
    return {
        w, h,
        at(x, y) {
            if (colorType === 3) {
                const per = 8 / bitDepth;
                const byte = out[y * rowBytes + Math.floor(x / per)];
                const idx = bitDepth === 8 ? byte : (byte >> (8 - bitDepth * ((x % per) + 1))) & ((1 << bitDepth) - 1);
                return [palette[idx * 3], palette[idx * 3 + 1], palette[idx * 3 + 2]];
            }
            const o = y * rowBytes + x * channels;
            if (colorType === 0 || colorType === 4) return [out[o], out[o], out[o]];
            return [out[o], out[o + 1], out[o + 2]];
        },
    };
}

// ---------------------------------------------------------------- shared
function critterPath(name) {
    return 'ART' + String.fromCharCode(92) + 'CRITTERS' + String.fromCharCode(92) + name.toUpperCase() + '.FRM';
}

function loadFrame(datPath, frmName, rot) {
    const dat = openDat(datPath);
    const raw = dat.extract(critterPath(frmName));
    if (!raw) throw new Error('not in ' + path.basename(datPath) + ': ' + frmName);
    return readFrm(raw).dirs[rot][0];
}

function pngName(frmName, rot) {
    // The trailing animation letter is dropped: every mask is for the STAND frame, and the shipped
    // templates were named this way.
    return 'annotate_' + frmName.toLowerCase().slice(0, 7) + rot + '.png';
}

// ---------------------------------------------------------------- export
function cmdExport(datPath, frmName, rotArg, outDir) {
    const dat = openDat(datPath);
    const raw = dat.extract(critterPath(frmName));
    if (!raw) throw new Error('not in ' + path.basename(datPath) + ': ' + frmName);
    const frm = readFrm(raw);
    const palRaw = openDat(path.join(path.dirname(datPath), 'MASTER.DAT')).extract('COLOR.PAL');
    if (!palRaw) throw new Error('COLOR.PAL not found next to ' + datPath);
    const pal = readPal(palRaw);

    fs.mkdirSync(outDir, { recursive: true });
    const rots = rotArg === 'all' ? [0, 1, 2, 3, 4, 5] : [parseInt(rotArg, 10)];
    for (const rot of rots) {
        const fr = frm.dirs[rot][0];
        const panelW = fr.w * SCALE, panelH = fr.h * SCALE;
        const w = Math.max(MARGIN * 3 + panelW * 2, SWATCH_X * 2 + SWATCHES.length * SWATCH_STEP - 4);
        const h = TOP + panelH + MARGIN;
        const rgb = Buffer.alloc(w * h * 3); // black

        const put = (x, y, c) => {
            const o = (y * w + x) * 3;
            rgb[o] = c[0]; rgb[o + 1] = c[1]; rgb[o + 2] = c[2];
        };
        for (let i = 0; i < SWATCHES.length; i++) {
            for (let y = SWATCH_Y; y < SWATCH_Y + SWATCH_H; y++) {
                for (let x = SWATCH_X + i * SWATCH_STEP; x < SWATCH_X + i * SWATCH_STEP + SWATCH_W; x++) {
                    put(x, y, SWATCHES[i].rgb);
                }
            }
        }
        for (let panel = 0; panel < 2; panel++) {
            const ox = MARGIN + panel * (panelW + MARGIN);
            for (let y = 0; y < fr.h; y++) {
                for (let x = 0; x < fr.w; x++) {
                    const idx = fr.data[y * fr.w + x];
                    if (idx === 0) continue;
                    const c = pal[idx];
                    for (let sy = 0; sy < SCALE; sy++) {
                        for (let sx = 0; sx < SCALE; sx++) put(ox + x * SCALE + sx, TOP + y * SCALE + sy, c);
                    }
                }
            }
        }
        const file = path.join(outDir, pngName(frmName, rot));
        writePng(file, w, h, rgb);
        console.log('wrote ' + file + '  (' + fr.w + 'x' + fr.h + ' sprite, ' + w + 'x' + h + ' canvas)');
    }
    console.log('\nPaint the LEFT panel only, flat swatch colours, pencil (no anti-aliasing).');
    console.log('Leave the shadow and anything you are unsure about in its original colour.');
}

// ---------------------------------------------------------------- import
function liftMask(pngFile, datPath) {
    const base = path.basename(pngFile, '.png');
    const m = /^annotate_([a-z0-9]{6,7})([0-5])$/.exec(base);
    if (!m) throw new Error(base + ': name must be annotate_<frmbase><rot>.png');
    const stem = m[1].toUpperCase();
    const rot = parseInt(m[2], 10);
    // The templates drop the trailing animation letter; stand art is <base><weapon>A.
    const frmName = stem.length === 7 ? stem + 'A' : stem;
    const fr = loadFrame(datPath, frmName, rot);
    const img = readPng(pngFile);

    const panelW = fr.w * SCALE, panelH = fr.h * SCALE;
    if (img.w < MARGIN + panelW || img.h < TOP + panelH) {
        throw new Error(base + ': canvas ' + img.w + 'x' + img.h + ' too small for a '
            + fr.w + 'x' + fr.h + ' sprite -- is the rotation in the name right?');
    }

    const byRgb = new Map();
    for (const sw of SWATCHES) byRgb.set(sw.rgb.join(','), sw);

    const mask = Buffer.alloc(fr.w * fr.h);
    const counts = new Map();
    let painted = 0, unpainted = 0, spill = 0, offColour = 0;
    for (let y = 0; y < fr.h; y++) {
        for (let x = 0; x < fr.w; x++) {
            const opaque = fr.data[y * fr.w + x] !== 0;
            // Sample the middle of the 8x8 block, so a stray pixel on a block edge cannot decide it.
            const c = img.at(MARGIN + x * SCALE + SCALE / 2, TOP + y * SCALE + SCALE / 2);
            const sw = byRgb.get(c.join(','));
            let part = NONE;
            if (sw) {
                part = swatchPart(sw, rot);
                if (opaque) {
                    if (part !== NONE) { painted++; counts.set(sw.name, (counts.get(sw.name) || 0) + 1); }
                } else {
                    spill++;
                }
            } else if (opaque) {
                if (c[0] === 0 && c[1] === 0 && c[2] === 0) unpainted++;
                else offColour++;
            }
            // Only body pixels can own a part; paint that ran onto transparent pixels is dropped
            // rather than trusted, because the engine only ever asks about opaque ones.
            mask[y * fr.w + x] = opaque && part !== NONE ? part : 0xff;
        }
    }
    return {
        frmName, rot, w: fr.w, h: fr.h, mask,
        stats: { painted, unpainted, spill, offColour, counts },
    };
}

// Row-major RLE: [u8 run 1..255][u8 part 0..7 or 0xff].
function rle(mask) {
    const out = [];
    let i = 0;
    while (i < mask.length) {
        const v = mask[i];
        let n = 1;
        while (i + n < mask.length && mask[i + n] === v && n < 255) n++;
        out.push(n, v);
        i += n;
    }
    return Buffer.from(out);
}

function cmdImport(inDir, datPath, outFile, allowPartial) {
    const files = fs.readdirSync(inDir).filter(f => /^annotate_.*\.png$/i.test(f)).sort();
    if (files.length === 0) throw new Error('no annotate_*.png in ' + inDir);

    const entries = [];
    const blobs = [];
    let blobLen = 0;
    for (const f of files) {
        const r = liftMask(path.join(inDir, f), datPath);
        const s = r.stats;
        if (s.painted === 0) {
            console.log('SKIP  ' + f + '  -- nothing painted');
            continue;
        }
        // ⚠️ A HALF-PAINTED MASK IS WORSE THAN NO MASK. It replaces the engine's guessed seeds
        // wholesale, so a part nobody painted gets no pixels at all -- and its mark degrades to a
        // plain ring while its neighbours look exact. Ship it only if it is asked for by name.
        const missing = [];
        for (const need of [[HEAD, 'head'], [EYES, 'eyes'], [TORSO, 'torso'], [GROIN, 'groin'],
            [LEFT_ARM, 'left arm'], [RIGHT_ARM, 'right arm'], [LEFT_LEG, 'left leg'], [RIGHT_LEG, 'right leg']]) {
            if (!r.mask.includes(need[0])) missing.push(need[1]);
        }
        if (missing.length && !allowPartial) {
            console.log('SKIP  ' + f + '  -- unpainted parts: ' + missing.join(', ')
                + '   (pass --partial to include it anyway)');
            continue;
        }
        const packed = rle(r.mask);
        entries.push({ name: r.frmName, rot: r.rot, w: r.w, h: r.h, off: blobLen, len: packed.length });
        blobs.push(packed);
        blobLen += packed.length;
        const parts = [...s.counts.entries()].map(([k, v]) => k.replace(/ .*/, '') + ':' + v).join(' ');
        console.log('ok    ' + f + '  ' + r.frmName + ' rot ' + r.rot + '  ' + r.w + 'x' + r.h
            + '  painted ' + s.painted + '  left plain ' + (s.unpainted + s.offColour)
            + (s.spill ? '  SPILL ' + s.spill : '') + '  [' + parts + ']');
    }
    if (entries.length === 0) throw new Error('nothing to write');

    const HDR = 16, ENT = 28;
    const head = Buffer.alloc(HDR);
    head.write('AIMM', 0, 'ascii');
    head.writeUInt32LE(1, 4);
    head.writeUInt32LE(entries.length, 8);
    head.writeUInt32LE(HDR + ENT * entries.length, 12); // where the blob starts
    const table = Buffer.alloc(ENT * entries.length);
    entries.forEach((e, i) => {
        const o = i * ENT;
        table.write(e.name.toUpperCase().padEnd(16, '\0'), o, 16, 'ascii');
        table.writeUInt8(e.rot, o + 16);
        table.writeUInt8(0, o + 17);
        table.writeUInt16LE(e.w, o + 18);
        table.writeUInt16LE(e.h, o + 20);
        table.writeUInt16LE(0, o + 22);
        table.writeUInt32LE(e.off, o + 24);
    });
    // Lengths live at the end of each entry's slot; recompute rather than trust the loop above.
    entries.forEach((e, i) => table.writeUInt16LE(e.len > 0xffff ? 0 : e.len, i * ENT + 22));
    const long = entries.filter(e => e.len > 0xffff);
    if (long.length) throw new Error('mask too large to index: ' + long[0].name);

    fs.mkdirSync(path.dirname(outFile), { recursive: true });
    fs.writeFileSync(outFile, Buffer.concat([head, table, ...blobs]));
    console.log('\nwrote ' + outFile + '  (' + entries.length + ' masks, '
        + (HDR + ENT * entries.length + blobLen) + ' bytes)');
}

// ---------------------------------------------------------------- check
function cmdCheck(pngFile, datPath) {
    const r = liftMask(pngFile, datPath);
    const s = r.stats;
    console.log(path.basename(pngFile) + ' -> ' + r.frmName + ' rot ' + r.rot + '  ' + r.w + 'x' + r.h);
    console.log('  painted body pixels : ' + s.painted);
    console.log('  left in plain art   : ' + (s.unpainted + s.offColour) + '  (shadow etc.)');
    console.log('  paint off the body  : ' + s.spill + (s.spill ? '   <-- dropped' : ''));
    for (const [k, v] of s.counts) console.log('    ' + k.padEnd(20) + v);
    const letters = { [HEAD]: 'H', [EYES]: 'Y', [LEFT_ARM]: 'l', [RIGHT_ARM]: 'r', [TORSO]: 'T', [GROIN]: 'G', [LEFT_LEG]: 'L', [RIGHT_LEG]: 'R' };
    console.log('\n  (H)ead (Y)eyes (l/r) arm (T)orso (G)roin (L/R) leg  . = no part');
    for (let y = 0; y < r.h; y++) {
        let line = '';
        for (let x = 0; x < r.w; x++) {
            const v = r.mask[y * r.w + x];
            line += v === 0xff ? '.' : letters[v];
        }
        console.log('  ' + line);
    }
}

// ---------------------------------------------------------------- preview
// ⚠️ LOOK AT IT. A mask is painted on ONE frame and the critter plays a dozen, each a different
// size, so the engine projects it onto whichever frame is on screen (gpui_aimmask_project). This
// renders that projection over EVERY frame of the rotation, side by side, as a picture -- because
// three earlier rounds of this feature were tuned against numbers that looked fine and art that
// did not. Grey = the projection claimed nothing and the flood fills it in the game.
function cmdPreview(pngFile, datPath, outFile, overrideFrm) {
    const r = liftMask(pngFile, datPath);
    const dat = openDat(datPath);
    // Optional: project the mask onto a DIFFERENT animation of the same critter, which is what the
    // engine does once the mask is matched on the six-character base rather than the whole name.
    const frm = readFrm(dat.extract(critterPath(overrideFrm || r.frmName)));
    // `rot all` takes the FIRST frame of every rotation instead of every frame of one, which is how
    // you see whether a mask painted for one direction is usable in the others at all.
    const rotArg = process.argv[7];
    const rots = rotArg === 'all' ? [0, 1, 2, 3, 4, 5] : [rotArg !== undefined ? parseInt(rotArg, 10) : r.rot];
    const frames = [];
    for (const rot of rots) {
        const src = rotArg === 'all' ? [frm.dirs[rot][0]] : frm.dirs[rot];
        for (const f of src) frames.push({ w: f.w, h: f.h, data: f.data, rot });
    }

    // A mask stores anatomical sides, decided when it was imported for ITS rotation. Reused on a
    // rotation that looks at the critter from the other side, the critter's left limb is the one on
    // the other side of the screen, so the labels have to swap with it.
    const swapped = new Map();
    const maskFor = (rot) => {
        const back = (rot === 0 || rot === 5);
        if (back === (r.rot === 0 || r.rot === 5)) return r.mask;
        if (!swapped.has(rot)) {
            const m = Buffer.from(r.mask);
            for (let i = 0; i < m.length; i++) {
                if (m[i] === LEFT_ARM) m[i] = RIGHT_ARM;
                else if (m[i] === RIGHT_ARM) m[i] = LEFT_ARM;
                else if (m[i] === LEFT_LEG) m[i] = RIGHT_LEG;
                else if (m[i] === RIGHT_LEG) m[i] = LEFT_LEG;
            }
            swapped.set(rot, m);
        }
        return swapped.get(rot);
    };

    // Painted bounds: what the engine matches to the live silhouette.
    let mLo = r.w, mHi = -1, mTop = r.h, mBottom = -1;
    for (let y = 0; y < r.h; y++) {
        for (let x = 0; x < r.w; x++) {
            if (r.mask[y * r.w + x] === 0xff) continue;
            if (x < mLo) mLo = x;
            if (x > mHi) mHi = x;
            if (y < mTop) mTop = y;
            if (y > mBottom) mBottom = y;
        }
    }
    const maskW = mHi - mLo + 1, maskH = mBottom - mTop + 1;

    // Per part: a point inside the painted region (its centre of mass pulled onto the region) and
    // the point of it furthest from that -- the two ends of an arm. Same two seeds the engine
    // computes when it loads the mask.
    const seedPoints = new Map();
    let headMaskH = maskH;
    for (let part = 0; part < 8; part++) {
        const pts = [];
        for (let y = mTop; y <= mBottom; y++) {
            for (let x = mLo; x <= mHi; x++) if (r.mask[y * r.w + x] === part) pts.push([x, y]);
        }
        if (!pts.length) continue;
        if (part === HEAD) {
            headMaskH = Math.max(...pts.map(p => p[1])) - Math.min(...pts.map(p => p[1])) + 1;
        }
        const cx = pts.reduce((a, p) => a + p[0], 0) / pts.length;
        const cy = pts.reduce((a, p) => a + p[1], 0) / pts.length;
        const d2 = (p) => (p[0] - cx) * (p[0] - cx) + (p[1] - cy) * (p[1] - cy);
        const near = pts.reduce((a, p) => (d2(p) < d2(a) ? p : a), pts[0]);
        const far = pts.reduce((a, p) => (d2(p) > d2(a) ? p : a), pts[0]);
        seedPoints.set(part, d2(far) > 4 ? [near, far] : [near]);
    }

    const colour = {
        [HEAD]: [255, 0, 0], [EYES]: [255, 255, 0], [LEFT_ARM]: [255, 128, 0],
        [RIGHT_ARM]: [0, 0, 255], [TORSO]: [255, 255, 255], [GROIN]: [255, 0, 255],
        [LEFT_LEG]: [0, 255, 0], [RIGHT_LEG]: [0, 255, 255],
    };

    const SC = 4, GAP = 4;
    const cellW = Math.max(...frames.map(f => f.w)) * SC + GAP;
    const cellH = Math.max(...frames.map(f => f.h)) * SC + GAP;
    const cols = Math.min(frames.length, 9);
    const rows = Math.ceil(frames.length / cols);
    const W = cols * cellW, H = rows * cellH;
    const rgb = Buffer.alloc(W * H * 3);

    let claimed = 0, filled = 0;
    const palRaw = openDat(path.join(path.dirname(datPath), 'MASTER.DAT')).extract('COLOR.PAL');
    const pal = readPal(palRaw);
    const lum = (idx) => {
        const c = pal[idx];
        return (c[0] * 30 + c[1] * 59 + c[2] * 11) / 100;
    };

    frames.forEach((fr, fi) => {
        // The ground shadow is baked into the sprite and is not the critter; the engine walls it
        // off before projecting (gpui_aimbody_wall_off_shadow) and this must do the same, or the
        // box being matched grows with the shadow instead of with the body.
        let sTop = fr.h, sBottom = -1;
        for (let y = 0; y < fr.h; y++) {
            for (let x = 0; x < fr.w; x++) {
                if (fr.data[y * fr.w + x] === 0) continue;
                if (y < sTop) sTop = y;
                if (y > sBottom) sBottom = y;
            }
        }
        const shadow = new Uint8Array(fr.w * fr.h);
        const ceiling = sTop + Math.floor((sBottom - sTop + 1) * 55 / 100);
        for (let x = 0; x < fr.w; x++) {
            for (let y = fr.h - 1; y >= ceiling; y--) {
                const idx = fr.data[y * fr.w + x];
                if (idx === 0) continue;
                if (lum(idx) >= 45) break;
                shadow[y * fr.w + x] = 1;
            }
        }

        // Live body bounds of this frame, shadow excluded.
        let lLo = fr.w, lHi = -1, lTop = fr.h, lBottom = -1;
        for (let y = 0; y < fr.h; y++) {
            for (let x = 0; x < fr.w; x++) {
                if (fr.data[y * fr.w + x] === 0 || shadow[y * fr.w + x]) continue;
                if (x < lLo) lLo = x;
                if (x > lHi) lHi = x;
                if (y < lTop) lTop = y;
                if (y > lBottom) lBottom = y;
            }
        }
        const liveW = lHi - lLo + 1, liveH = lBottom - lTop + 1;
        const cx = (fi % cols) * cellW + GAP / 2, cy = Math.floor(fi / cols) * cellH + GAP / 2;

        const owner = new Int8Array(fr.w * fr.h).fill(-1);

        // ROW BY ROW, NOT BOX BY BOX. Matching the painted box to the live box assumes the pose
        // did not change; it does -- the arm comes out, the body leans -- and then everything
        // slides. A row of the body maps onto the matching row of the painted body and stretches to
        // ITS width, so the arm/torso/arm order across a row survives the arm moving.
        const rowSpan = (data, w, y, isShadow) => {
            let lo = -1, hi = -1;
            for (let x = 0; x < w; x++) {
                if (data[y * w + x] === 0xff) continue;
                if (isShadow && isShadow[y * w + x]) continue;
                if (lo < 0) lo = x;
                hi = x;
            }
            return [lo, hi];
        };
        const liveSpan = (y) => {
            let lo = -1, hi = -1;
            for (let x = 0; x < fr.w; x++) {
                if (fr.data[y * fr.w + x] === 0 || shadow[y * fr.w + x]) continue;
                if (lo < 0) lo = x;
                hi = x;
            }
            return [lo, hi];
        };
        const mask = maskFor(fr.rot);
        for (let y = lTop; y <= lBottom; y++) {
            const [lo, hi] = liveSpan(y);
            if (lo < 0) continue;
            const my = maskH > 1 ? mTop + Math.floor(((y - lTop) * (maskH - 1) + (liveH - 1) / 2) / (liveH - 1)) : mTop;
            const [mlo, mhi] = rowSpan(mask, r.w, my);
            if (mlo < 0) continue;
            for (let x = lo; x <= hi; x++) {
                if (fr.data[y * fr.w + x] === 0 || shadow[y * fr.w + x]) continue;
                const mx = hi > lo
                    ? mlo + Math.round((x - lo) * (mhi - mlo) / (hi - lo))
                    : mlo;
                const part = mask[my * r.w + mx];
                if (part < 8) owner[y * fr.w + x] = part;
            }
        }

        const snap = (x, y) => {
            const limit = Math.max(fr.w, fr.h);
            for (let rad = 0; rad <= limit; rad++) {
                for (let dy = -rad; dy <= rad; dy++) {
                    for (let dx = -rad; dx <= rad; dx++) {
                        if (Math.max(Math.abs(dx), Math.abs(dy)) !== rad) continue;
                        const nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= fr.w || ny >= fr.h) continue;
                        if (fr.data[ny * fr.w + nx] !== 0 && !shadow[ny * fr.w + nx]) return ny * fr.w + nx;
                    }
                }
            }
            return -1;
        };
        let front = [];
        for (let p = 0; p < owner.length; p++) if (owner[p] !== -1) front.push(p);
        // The head is the one part the engine leashes (a hunched critter has no neck to stop it).
        const headBudget = Math.max(4, Math.round((headMaskH * liveH / maskH) * 3 / 2));
        let dist = 0;
        while (front.length) {
            const next = [];
            dist++;
            for (const p of front) {
                const o = owner[p];
                if ((o === HEAD || o === EYES) && dist > headBudget) continue;
                const y = Math.floor(p / fr.w), x = p - y * fr.w;
                const nb = [];
                if (x > 0) nb.push(p - 1);
                if (x < fr.w - 1) nb.push(p + 1);
                if (y > 0) nb.push(p - fr.w);
                if (y < fr.h - 1) nb.push(p + fr.w);
                for (const q of nb) {
                    if (fr.data[q] === 0 || shadow[q] || owner[q] !== -1) continue;
                    owner[q] = o;
                    next.push(q);
                }
            }
            front = next;
        }

        for (let y = lTop; y <= lBottom; y++) {
            for (let x = lLo; x <= lHi; x++) {
                if (fr.data[y * fr.w + x] === 0 || shadow[y * fr.w + x]) continue;
                const part = owner[y * fr.w + x];
                const c = colour[part] || [70, 70, 70];
                if (colour[part]) claimed++; else filled++;
                for (let sy = 0; sy < SC; sy++) {
                    for (let sx = 0; sx < SC; sx++) {
                        const px = cx + (x - lLo) * SC + sx, py = cy + (y - lTop) * SC + sy;
                        if (px < 0 || px >= W || py < 0 || py >= H) continue;
                        const o = (py * W + px) * 3;
                        rgb[o] = c[0]; rgb[o + 1] = c[1]; rgb[o + 2] = c[2];
                    }
                }
            }
        }
    });

    writePng(outFile, W, H, rgb);
    console.log((overrideFrm || r.frmName) + ' rot ' + r.rot + ': ' + frames.length + ' frames, sizes '
        + [...new Set(frames.map(f => f.w + 'x' + f.h))].join(' '));
    console.log('projection claimed ' + claimed + ' px, left ' + filled
        + ' px (' + (100 * filled / (claimed + filled)).toFixed(1) + '%) for the flood');
    console.log('wrote ' + outFile);
}

// ----------------------------------------------------------------
const cmd = process.argv[2];
try {
    if (cmd === 'export') cmdExport(process.argv[3], process.argv[4], process.argv[5], process.argv[6]);
    else if (cmd === 'import') cmdImport(process.argv[3], process.argv[4], process.argv[5], process.argv.includes('--partial'));
    else if (cmd === 'check') cmdCheck(process.argv[3], process.argv[4]);
    else if (cmd === 'preview') cmdPreview(process.argv[3], process.argv[4], process.argv[5], process.argv[6]);
    else {
        console.error('usage:');
        console.error('  node aimmask.js export <CRITTER.DAT> <FRMNAME> <rot|all> <outDir>');
        console.error('  node aimmask.js import <inDir> <CRITTER.DAT> <out.dat>');
        console.error('  node aimmask.js check  <PNG> <CRITTER.DAT>');
        console.error('  node aimmask.js preview <PNG> <CRITTER.DAT> <out.png>');
        process.exit(2);
    }
} catch (e) {
    console.error('ERROR: ' + e.message);
    process.exit(1);
}
