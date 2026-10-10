// Paper 3D: rebuilds one Fallout map as "paper" geometry and writes a self-contained three.js
// viewer for it -- an experiment to see what the game would look like with a free camera,
// without touching the engine.
//
// Usage:  node build.js <MAPNAME> [out.html]        e.g.  node build.js SHADYE
//
// Everything is read from the player's own game data (build/Release: MASTER.DAT, CRITTER.DAT and
// the data/ patch folder, in the engine's order). The output embeds the map's art, so it belongs
// in build/ (git-ignored) and must never be committed or published.
//
// THE GEOMETRY RESTS ON ONE FACT: the engine's screen projection (tile_coord / square_coord in
// src/game/tile.cc) is an exact orthographic view of a REGULAR hex grid. Read in world units
// where one hex step is 1, all six neighbours come out at distance 1 when the camera looks down
// at sin(theta) = sqrt(3)/4 (~25.7 deg) with 32 px per unit across the screen. So:
//
//     screen x = 32 * x
//     screen y = KZ * z - KY * height      KZ = 32 sin(theta) = 13.86,  KY = 32 cos(theta) = 28.84
//
// The viewer rebuilds that camera and PROJECTS every sprite back onto a proxy (the ground, the
// roof plane, a vertical panel, a billboard), so the original view comes out pixel for pixel and
// every other view shows the paper. The roof's 96 px lift (square_coord_roof) is 96/KY = 3.33
// units -- the only height the engine itself knows.

'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const { openDat } = require('../f1script/dat.js');
const hull = require('./hull.js');
const { dressVaultSuit, isJumpsuit } = require('./dress.js');
const setPieces = require('./setpieces.js');

const ROOT = path.resolve(__dirname, '../../..');
const GAME = path.join(ROOT, 'build', 'Release');
const BS = String.fromCharCode(92);

// ---------------------------------------------------------------- game file system
// Patch folder first, then the DATs -- the engine's master_patches / critter_patches order.
function openGameFs() {
    const master = openDat(path.join(GAME, 'MASTER.DAT'));
    const critter = openDat(path.join(GAME, 'CRITTER.DAT'));
    const patch = new Map();
    (function walk(dir, rel) {
        if (!fs.existsSync(dir)) return;
        for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
            const r = rel ? rel + BS + e.name : e.name;
            if (e.isDirectory()) walk(path.join(dir, e.name), r);
            else patch.set(r.toUpperCase(), path.join(dir, e.name));
        }
    })(path.join(GAME, 'data'), '');

    const cache = new Map();
    return function read(name) {
        const key = name.toUpperCase().split('/').join(BS);
        if (cache.has(key)) return cache.get(key);
        let buf = null;
        if (patch.has(key)) buf = fs.readFileSync(patch.get(key));
        else buf = master.extract(key) || critter.extract(key);
        cache.set(key, buf);
        return buf;
    };
}

// art_read_lst (art.cc): one entry per line, cut at the first of " ,;\r\t\n", max 12 chars.
function readLst(read, name) {
    const buf = read(name);
    if (!buf) throw new Error('missing ' + name);
    const lines = buf.toString('latin1').split('\n');
    if (lines.length && lines[lines.length - 1] === '') lines.pop();
    return lines.map((l) => l.split(/[ ,;\r\t\n]/)[0].slice(0, 12));
}

// ---------------------------------------------------------------- art
const ART_DIRS = ['items', 'critters', 'scenery', 'walls', 'tiles', 'misc'];
const OBJ_ITEM = 0, OBJ_CRITTER = 1, OBJ_SCENERY = 2, OBJ_WALL = 3, OBJ_TILE = 4, OBJ_MISC = 5;

// art_get_code (art.cc), for the animations a map can hold.
function artCode(anim, weapon) {
    if (weapon < 0 || weapon > 15) return null;
    if (anim >= 38 && anim <= 47) { // ANIM_TAKE_OUT .. ANIM_FIRE_CONTINUOUS
        if (weapon === 0) return null;
        return [String.fromCharCode(100 + weapon - 1), String.fromCharCode(99 + anim - 38)];
    }
    if (anim === 36) return ['c', 'h'];
    if (anim === 37) return ['c', 'j'];
    if (anim === 64) return ['n', 'a'];
    if (anim >= 48) return ['r', String.fromCharCode(97 + anim - 48)];
    if (anim >= 20) return ['b', String.fromCharCode(97 + anim - 20)];
    if (anim === 18) {
        if (weapon === 1) return ['d', 'm'];
        if (weapon === 3) return ['g', 'm'];
        return ['a', 's'];
    }
    if (anim === 13) return weapon <= 0 ? ['a', 'n'] : [String.fromCharCode(100 + weapon - 1), 'e'];
    const c2 = String.fromCharCode(97 + anim);
    if (anim <= 1 && weapon > 0) return [String.fromCharCode(100 + weapon - 1), c2];
    return ['a', c2];
}

function makeArtNamer(read) {
    const lsts = ART_DIRS.map((d) => readLst(read, 'ART' + BS + d.toUpperCase() + BS + d.toUpperCase() + '.LST'));
    return function artName(fid) {
        const type = (fid & 0xF000000) >> 24;
        const index = fid & 0xFFF;
        if (type > OBJ_MISC || index >= lsts[type].length) return null;
        if (type === OBJ_CRITTER) {
            const code = artCode((fid & 0xFF0000) >> 16, (fid & 0xF000) >> 12);
            if (!code) return null;
            const v1 = (fid & 0x70000000) >> 28;
            const base = 'ART' + BS + 'CRITTERS' + BS + lsts[type][index] + code[0] + code[1];
            return (v1 ? base + '.FR' + String.fromCharCode(v1 + 47) : base + '.FRM').toUpperCase();
        }
        return ('ART' + BS + ART_DIRS[type] + BS + lsts[type][index]).toUpperCase();
    };
}

function parseFrm(buf) {
    const fpd = buf.readUInt16BE(8);
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
            frames.push({ w, h, data: buf.subarray(p + 12, p + 12 + w * h) });
            p += 12 + size;
        }
        dirs.push(frames);
    }
    return { fpd, xOff, yOff, dirs };
}

// ---------------------------------------------------------------- protos
function makeProtos(read) {
    const lsts = {};
    const cache = new Map();
    const dirOf = { 0: 'ITEMS', 2: 'SCENERY', 3: 'WALLS' };
    return function proto(pid) {
        if (cache.has(pid)) return cache.get(pid);
        const type = pid >> 24;
        const dir = dirOf[type];
        let out = null;
        if (dir) {
            if (!lsts[dir]) lsts[dir] = readLst(read, 'PROTO' + BS + dir + BS + dir + '.LST');
            const name = lsts[dir][(pid & 0xFFFFFF) - 1];
            const buf = name && read('PROTO' + BS + dir + BS + name);
            if (buf) {
                out = { flags: buf.readInt32BE(20), ext: buf.readInt32BE(24) };
                if (type !== OBJ_WALL) out.subtype = buf.readInt32BE(32); // item.type / scenery.type
            }
        }
        cache.set(pid, out);
        return out;
    };
}

// ---------------------------------------------------------------- map (map_load_file, map.cc)
function parseMap(buf, proto) {
    let p = 0;
    const i32 = () => { const v = buf.readInt32BE(p); p += 4; return v; };
    const hdr = {};
    hdr.version = i32();
    hdr.name = buf.toString('latin1', p, p + 16).replace(/\0[\s\S]*$/, ''); p += 16;
    hdr.enteringTile = i32();
    hdr.enteringElevation = i32();
    hdr.enteringRotation = i32();
    const localVars = i32();
    hdr.scriptIndex = i32();
    hdr.flags = i32();
    hdr.darkness = i32();
    const globalVars = i32();
    i32(); i32(); // map index, last visit
    p += 44 * 4;
    if (hdr.version !== 19) throw new Error('map version ' + hdr.version);

    p += 4 * Math.max(0, globalVars) + 4 * Math.max(0, localVars);

    // square_load: one int per square, floor in the low 16 bits, roof in the high 16.
    const squares = [null, null, null];
    for (let e = 0; e < 3; e++) {
        if ((hdr.flags & [2, 4, 8][e]) !== 0) continue;
        const arr = new Int32Array(10000);
        for (let t = 0; t < 10000; t++) arr[t] = i32();
        squares[e] = arr;
    }

    // scr_load: 5 lists, each in extents of 16 records whose size depends on the record's own sid.
    for (let type = 0; type < 5; type++) {
        const count = i32();
        if (count === 0) continue;
        const extents = Math.ceil(count / 16);
        for (let x = 0; x < extents; x++) {
            for (let s = 0; s < 16; s++) {
                const sid = i32();
                i32(); // next
                const st = sid >> 24;
                if (st === 1) p += 8; // spatial: built_tile, radius
                else if (st === 2) p += 4; // timed: time
                p += 14 * 4;
            }
            i32(); i32(); // length, next
        }
    }

    // obj_load / obj_read_obj / proto_read_protoUpdateData
    let unknownProto = 0;
    function readObj() {
        const o = {};
        o.id = i32(); o.tile = i32(); o.x = i32(); o.y = i32(); i32(); i32(); // sx, sy (stale screen coords)
        o.frame = i32(); o.rotation = i32(); o.fid = i32(); o.flags = i32(); i32(); // elevation
        o.pid = i32(); i32(); // cid
        i32(); i32(); i32(); o.sid = i32(); i32(); // light dist/int, outline, sid, script index
        o.invLength = i32(); i32(); i32(); // inventory length, capacity, pointer
        const type = o.pid >> 24;
        if (type === OBJ_CRITTER) {
            p += 4 * 11; // field_0, 7 combat ints, hp, radiation, poison
        } else {
            i32(); // updated flags
            if (type === OBJ_ITEM) {
                const pr = proto(o.pid);
                if (!pr) unknownProto++;
                o.subtype = pr ? pr.subtype : -1;
                if (o.subtype === 3) p += 8; // weapon: ammo quantity + type
                else if (o.subtype === 4 || o.subtype === 5 || o.subtype === 6) p += 4; // ammo, misc, key
            } else if (type === OBJ_SCENERY) {
                const pr = proto(o.pid);
                if (!pr) unknownProto++;
                o.subtype = pr ? pr.subtype : -1;
                o.ext = pr ? pr.ext : 0;
                if (o.subtype === 0 || o.subtype === 3 || o.subtype === 4) p += 4; // door, ladders
                else if (o.subtype === 1 || o.subtype === 2) p += 8; // stairs, elevator
            } else if (type === OBJ_WALL) {
                const pr = proto(o.pid);
                o.ext = pr ? pr.ext : 0;
            } else if (type === OBJ_MISC) {
                if (o.pid >= 0x5000010 && o.pid <= 0x5000017) p += 16; // exit grid
            }
        }
        return o;
    }
    function readInventory(o) {
        for (let k = 0; k < o.invLength; k++) {
            i32(); // quantity
            readInventory(readObj());
        }
    }

    i32(); // total object count
    const objects = [];
    for (let e = 0; e < 3; e++) {
        const n = i32();
        for (let k = 0; k < n; k++) {
            const o = readObj();
            o.elevation = e;
            objects.push(o);
            readInventory(o);
        }
    }
    if (p !== buf.length) console.warn(`note: ${buf.length - p} bytes left after the objects`);
    if (unknownProto) console.warn(`note: ${unknownProto} objects without a readable proto`);
    return { hdr, squares, objects };
}

// ---------------------------------------------------------------- screen coordinates
// tile_coord / square_coord with the view centred on tile 0 (tile_x = tile_y = 0, offsets 0).
function hexTopLeft(tile) {
    const col = 199 - (tile % 200), row = Math.floor(tile / 200);
    const half = col >> 1;
    return [48 * half + (col & 1 ? 32 : 0) + 16 * row, -12 * half + 12 * row];
}
// The six hexes around a tile (columns are staggered: odd ones sit half a row lower).
function hexNeighbours(tile) {
    const col = 199 - (tile % 200), row = Math.floor(tile / 200);
    const odd = col & 1;
    const steps = odd
        ? [[1, 1], [1, 0], [0, -1], [-1, 0], [-1, 1], [0, 1]]
        : [[1, 0], [1, -1], [0, -1], [-1, -1], [-1, 0], [0, 1]];
    return steps
        .map(([dc, dr]) => [col + dc, row + dr])
        .filter(([c, r]) => c >= 0 && c < 200 && r >= 0 && r < 200)
        .map(([c, r]) => r * 200 + (199 - c));
}

function squareTopLeft(sq) {
    const col = 99 - (sq % 100), row = Math.floor(sq / 100);
    return [-16 + 48 * col + 32 * row, -2 - 12 * col + 24 * row];
}

// ---------------------------------------------------------------- atlas
// Shelf packer over palette-index images; pages are single-channel, 1 px of transparent gutter.
const PAGE_W = 2048, PAGE_H = 2048;
function makeAtlas() {
    const images = []; // { w, h, data }
    const byKey = new Map();
    function add(key, frame) {
        if (byKey.has(key)) return byKey.get(key);
        const id = images.length;
        images.push({ w: frame.w, h: frame.h, data: frame.data });
        byKey.set(key, id);
        return id;
    }
    function pack() {
        const order = images.map((_, i) => i).sort((a, b) => images[b].h - images[a].h);
        const pages = [];
        let page = null, x = 0, y = 0, shelf = 0;
        const place = new Array(images.length);
        function newPage() {
            page = { data: Buffer.alloc(PAGE_W * PAGE_H), used: 0 };
            pages.push(page);
            x = 1; y = 1; shelf = 0;
        }
        newPage();
        for (const i of order) {
            const im = images[i];
            if (im.w + 2 > PAGE_W || im.h + 2 > PAGE_H) throw new Error('image too big for a page');
            if (x + im.w + 1 > PAGE_W) { x = 1; y += shelf + 1; shelf = 0; }
            if (y + im.h + 1 > PAGE_H) newPage();
            for (let r = 0; r < im.h; r++) im.data.copy(page.data, (y + r) * PAGE_W + x, r * im.w, (r + 1) * im.w);
            place[i] = [pages.length - 1, x, y, im.w, im.h];
            page.used = Math.max(page.used, y + im.h + 1);
            x += im.w + 1;
            shelf = Math.max(shelf, im.h);
        }
        // trim each page to what it uses (rows only; the width stays a power of two)
        const out = pages.map((pg) => {
            const h = Math.max(4, Math.ceil(pg.used / 4) * 4);
            return { w: PAGE_W, h, z: zlib.deflateSync(pg.data.subarray(0, PAGE_W * h), { level: 9 }).toString('base64') };
        });
        return { pages: out, place };
    }
    return { add, pack, images };
}

// ---------------------------------------------------------------- scene
// The bottom edge of every column of a sprite, in absolute screen y (NaN = an empty column).
function bottomEdge(frame, uly) {
    const bottom = new Float64Array(frame.w).fill(NaN);
    for (let c = 0; c < frame.w; c++) {
        for (let r = frame.h - 1; r >= 0; r--) {
            if (frame.data[r * frame.w + c] !== 0) { bottom[c] = uly + r + 1; break; }
        }
    }
    return bottom;
}

// A standing sprite rests on a GROUND LINE, y = k + m*x in screen px, drawn under it so that every
// opaque pixel stays above it (a pixel below the ground would be hidden by the floor, and the
// original view would lose it). m is one of three slopes: a card facing the original camera, or
// one of the two wall axes. Walls and corners get one or two segments ("V") of such lines.
const SLOPES = [0, -0.25, 0.75]; // card, axis a = (48,-12), axis b = (32,24)
const AXIS_A = 1, AXIS_B = 2;
const V_PENALTY = 1.0; // px of average gap a corner split must win by

// Per slope: v_c = bottom_c - m*x_c for the opaque columns, with prefix/suffix maxima and prefix
// sums, so the lowest valid line over any column range -- and its gap -- is O(1).
function slopeTable(bottom, ulx, m) {
    const w = bottom.length;
    const pmax = new Float64Array(w + 1).fill(-Infinity), smax = new Float64Array(w + 1).fill(-Infinity);
    const psum = new Float64Array(w + 1), pn = new Int32Array(w + 1);
    const v = (c) => bottom[c] - m * (ulx + c + 0.5);
    const ok = (c) => !Number.isNaN(bottom[c]);
    for (let c = 0; c < w; c++) {
        pmax[c + 1] = ok(c) ? Math.max(pmax[c], v(c)) : pmax[c];
        psum[c + 1] = psum[c] + (ok(c) ? v(c) : 0);
        pn[c + 1] = pn[c] + (ok(c) ? 1 : 0);
    }
    for (let c = w - 1; c >= 0; c--) smax[c] = ok(c) ? Math.max(smax[c + 1], v(c)) : smax[c + 1];
    return {
        left: (s) => ({ k: pmax[s], n: pn[s], sum: psum[s] }), // columns [0, s)
        right: (s) => ({ k: smax[s], n: pn[w] - pn[s], sum: psum[w] - psum[s] }), // columns [s, w)
    };
}
const gapOf = (r, k) => r.n * k - r.sum;

// Best ground for one sprite: a single line on one of `axes`, or (allowV) a corner of two lines
// meeting at a vertex. Returns { segs: [{ axis, k, x0, x1 }] } in absolute screen coordinates.
function fitGround(bottom, ulx, axes, allowV, prefer) {
    const w = bottom.length;
    const tables = SLOPES.map((m) => slopeTable(bottom, ulx, m));
    const cols = tables[0].left(w).n || 1;
    let best = null;
    const consider = (cost, segs) => { if (!best || cost < best.cost) best = { cost, segs }; };
    for (const axis of axes) {
        const r = tables[axis].left(w);
        consider(gapOf(r, r.k) / cols - (axis === prefer ? 0.75 : 0), [{ axis, k: r.k, x0: ulx, x1: ulx + w }]);
    }
    if (allowV) {
        for (const [a1, a2] of [[AXIS_A, AXIS_B], [AXIS_B, AXIS_A]]) {
            const m1 = SLOPES[a1], m2 = SLOPES[a2];
            for (let s = 6; s <= w - 6; s++) {
                const L = tables[a1].left(s), R = tables[a2].right(s);
                if (!L.n || !R.n) continue;
                const xs = ulx + s;
                const yv = Math.max(L.k + m1 * xs, R.k + m2 * xs); // meet at the lower of the two
                const k1 = yv - m1 * xs, k2 = yv - m2 * xs;
                consider((gapOf(L, k1) + gapOf(R, k2)) / cols + V_PENALTY,
                    [{ axis: a1, k: k1, x0: ulx, x1: xs }, { axis: a2, k: k2, x0: xs, x1: ulx + w }]);
            }
        }
    }
    return best;
}

// Big scenery is often several sprites on neighbouring hexes (Junktown's gate car is three), and
// fitted one by one each piece stood at the depth of its own bottom edge: the car came out sliced
// into strips with gaps between them. Pieces whose rectangles overlap on screen and stand on
// nearly the same line are one object; fit ONE ground under their combined bottom edge and hand
// every piece its stretch of it.
// px: the deepest a group may reach from its front-most to its back-most bottom. 96 flattened
// Junktown's whole gate container into one slab and shut the passage; 40 keeps the car whole.
const GROUP_SPAN = 40;

function groupScenery(pieces) {
    const parent = pieces.map((_, i) => i);
    const lo = pieces.map((p) => p.lowest), hi = pieces.map((p) => p.lowest);
    const find = (i) => (parent[i] === i ? i : (parent[i] = find(parent[i])));
    for (let i = 0; i < pieces.length; i++) {
        for (let j = i + 1; j < pieces.length; j++) {
            const a = pieces[i], b = pieces[j];
            // overlapping or abutting (fence pieces only touch)
            if (a.ulx > b.ulx + b.w + 2 || b.ulx > a.ulx + a.w + 2 || a.uly > b.uly + b.h + 2 || b.uly > a.uly + a.h + 2) continue;
            const ri = find(i), rj = find(j);
            if (ri === rj) continue;
            const l = Math.min(lo[ri], lo[rj]), h = Math.max(hi[ri], hi[rj]);
            if (h - l > GROUP_SPAN) continue;
            parent[ri] = rj;
            lo[rj] = l;
            hi[rj] = h;
        }
    }
    const groups = new Map();
    pieces.forEach((p, i) => { const r = find(i); if (!groups.has(r)) groups.set(r, []); groups.get(r).push(p); });

    for (const g of groups.values()) {
        for (const p of g) { p.groupSize = g.length; p.group = g; }
        const gx0 = Math.min(...g.map((p) => p.ulx)), gx1 = Math.max(...g.map((p) => p.ulx + p.w));
        const comb = new Float64Array(gx1 - gx0).fill(NaN);
        for (const p of g) {
            for (let c = 0; c < p.w; c++) {
                const b = p.bottom[c];
                if (Number.isNaN(b)) continue;
                const x = p.ulx + c - gx0;
                comb[x] = Number.isNaN(comb[x]) ? b : Math.max(comb[x], b);
            }
        }
        // a card is the safe default for a lump; an axis (or a corner) must beat it clearly
        let fit = fitGround(comb, gx0, [0], false, 0);
        if (gx1 - gx0 >= 40) {
            const shaped = fitGround(comb, gx0, [AXIS_A, AXIS_B], true, 0);
            if (shaped.cost <= fit.cost * 0.5) fit = shaped;
        }
        for (const p of g) {
            p.segs = [];
            for (const s of fit.segs) {
                const x0 = Math.max(s.x0, p.ulx), x1 = Math.min(s.x1, p.ulx + p.w);
                if (x1 - x0 > 0.01) p.segs.push({ axis: s.axis, k: s.k, x0, x1 });
            }
            if (!p.segs.length) p.segs = [{ ...fit.segs[0], x0: p.ulx, x1: p.ulx + p.w }];
        }
    }
    return groups.size;
}

// Pieces of one straight wall each fit their own line, and a pillar or a joint piece pushes its
// line forward -- up to ~0.8 units -- which opened gaps between the pieces the moment the camera
// left the original angle. Two pieces with touching stretches on the same axis are one wall when
//  - they stand on the same hex line: axis a runs along a hex ROW (consecutive columns step
//    (32,0),(16,-12) -- the zig-zag -- inside one row), axis b along a hex COLUMN. This catches a
//    rail floating between two posts, which has no pixel on the ground, so its own line sits a
//    hex behind theirs (the brahmin pen); or
//  - their lines nearly agree (SNAP_TOL). This catches walls whose pieces alternate between two
//    neighbouring rows (the pen's other side), which the hex rule alone split apart.
// The whole chain then gets its FRONT-most line: still under every pixel, and now one plane.
const SNAP_TOL = 12.5; // px: one wall strays up to ~11 px; parallel walls a hex apart are 16 (a) / 24 (b)

function snapWallRuns(panels) {
    const segs = [];
    for (const p of panels) {
        for (const s of p.segs) {
            if (!s.axis) continue;
            s.line = s.axis === AXIS_A ? 'a' + Math.floor(p.tile / 200) : 'b' + (p.tile % 200);
            segs.push(s);
        }
    }
    const parent = segs.map((_, i) => i);
    const find = (i) => (parent[i] === i ? i : (parent[i] = find(parent[i])));
    const touch = (a, b) => a.x0 <= b.x1 + 2 && b.x0 <= a.x1 + 2;
    const byLine = new Map();
    segs.forEach((s, i) => { if (!byLine.has(s.line)) byLine.set(s.line, []); byLine.get(s.line).push(i); });
    for (const ids of byLine.values()) {
        ids.sort((i, j) => segs[i].x0 - segs[j].x0);
        for (let n = 1; n < ids.length; n++) {
            // chain along the line: anything touching an earlier piece of the current stretch
            for (let m = n - 1; m >= 0 && segs[ids[m]].x1 + 2 >= segs[ids[n]].x0 - 64; m--) {
                if (touch(segs[ids[m]], segs[ids[n]])) { parent[find(ids[n])] = find(ids[m]); break; }
            }
        }
    }
    const order = segs.map((_, i) => i).sort((i, j) => segs[i].axis - segs[j].axis || segs[i].k - segs[j].k);
    for (let oi = 0; oi < order.length; oi++) {
        const a = segs[order[oi]];
        for (let oj = oi + 1; oj < order.length; oj++) {
            const b = segs[order[oj]];
            if (b.axis !== a.axis || b.k - a.k > SNAP_TOL) break;
            if (touch(a, b)) parent[find(order[oi])] = find(order[oj]);
        }
    }
    const front = new Map();
    segs.forEach((s, i) => { const r = find(i); front.set(r, Math.max(front.get(r) ?? -Infinity, s.k)); });
    segs.forEach((s, i) => { s.k = front.get(find(i)); });

    // A corner's two lines moved independently: put the vertex back where they cross, and push a
    // line down again wherever a column that changed sides would otherwise poke under it.
    for (const p of panels) {
        if (p.segs.length !== 2) continue;
        const [s1, s2] = p.segs;
        const m1 = SLOPES[s1.axis], m2 = SLOPES[s2.axis];
        for (let iter = 0; iter < 3; iter++) {
            let xv = (s2.k - s1.k) / (m1 - m2);
            xv = Math.min(Math.max(xv, s1.x0 + 1), s2.x1 - 1);
            s1.x1 = s2.x0 = xv;
            const split = Math.round(xv - p.ulx);
            const need1 = p.tables[s1.axis].left(Math.max(0, Math.min(p.w, split))).k;
            const need2 = p.tables[s2.axis].right(Math.max(0, Math.min(p.w, split))).k;
            if (need1 <= s1.k + 1e-6 && need2 <= s2.k + 1e-6) break;
            s1.k = Math.max(s1.k, need1);
            s2.k = Math.max(s2.k, need2);
        }
    }
}

function build(mapName, opts = {}) {
    const read = openGameFs();
    const artName = makeArtNamer(read);
    const proto = makeProtos(read);
    const mapBuf = read('MAPS' + BS + mapName.toUpperCase() + '.MAP');
    if (!mapBuf) throw new Error('no map ' + mapName);
    const map = parseMap(mapBuf, proto);
    const palette = read('COLOR.PAL');
    hull.setPalette(palette); // the carve reads shadow colours off it
    // A critter sprite without the shadow painted on the ground under it: a billboard stands
    // upright, and the shadow would stand up with it.
    const noShadow = (df) => {
        const keep = hull.silhouette(df);
        return { ...df, data: df.data.map((c, p) => (keep[p] ? c : 0)) };
    };

    const frmCache = new Map();
    function frm(name) {
        if (!name) return null;
        if (frmCache.has(name)) return frmCache.get(name);
        const buf = read(name);
        const f = buf ? parseFrm(buf) : null;
        frmCache.set(name, f);
        return f;
    }

    const atlas = makeAtlas();
    const stats = { missingArt: 0, hidden: 0, misc: 0, objects: 0, voxels: 0, corpses: 0, revolved: 0, ground: { 0: 0, 1: 0, 2: 0, corners: 0, capped: 0 } };

    const origin = hexTopLeft(map.hdr.enteringTile); // keep numbers small around the entrance
    const rel = (x, y) => [x - origin[0], y - origin[1]];
    const label = (name) => (name || '?').split(BS).pop().replace(/.FR[M0-5]$/i, '').toLowerCase();

    // Voxel bodies (hull.js), one per FRM + frame, shared by every critter wearing that art.
    // Stored as deflated little-endian records: int16 x, y, z voxel index (centre at
    // (index + 0.5) / 32 units from the anchor on the ground) + one palette index.
    const models = [];
    const modelIds = new Map();
    const critFrm = new Map();
    // standing: shadow trimmed (hull.carveStanding) -- off for a corpse, which lies low all over
    function voxelModel(name, f, acc, standing = true) {
        const key = name + ':' + f + ':' + (acc ? 1 : 0) + ':' + (standing ? 1 : 0);
        if (modelIds.has(key)) return modelIds.get(key);
        let id = -1;
        if (!critFrm.has(name)) {
            const buf = read(name);
            critFrm.set(name, buf ? hull.readFrm(buf) : null);
        }
        const frm = critFrm.get(name);
        if (frm) id = packModel(hull.viewsOf(frm, f, acc), 200, { standing, name });
        modelIds.set(key, id);
        return id;
    }
    function packModel(views, minVoxels, { standing = false, name = '' } = {}) {
        const grid = standing ? hull.carveStanding(views) : hull.carve(views, { trim: false });
        let surf = hull.colour(grid, views).filter((s) => s.c);
        if (isJumpsuit(name)) surf = dressVaultSuit(surf, palette); // the straight zipper, a crisp "13"
        if (surf.length < minVoxels) return -1;
        const oi = Math.round(-grid.x0 * 32), oj = Math.round(-grid.y0 * 32), ok = Math.round(-grid.z0 * 32);
        const out = Buffer.alloc(surf.length * 7);
        surf.forEach((s, n) => {
            out.writeInt16LE(s.i - oi, n * 7);
            out.writeInt16LE(s.j - oj, n * 7 + 2);
            out.writeInt16LE(s.k - ok, n * 7 + 4);
            out[n * 7 + 6] = s.c;
        });
        models.push({ n: surf.length, z: zlib.deflateSync(out, { level: 9 }).toString('base64') });
        stats.voxels += surf.length;
        return models.length - 1;
    }
    // Scenery has ONE direction (all 920 FRMs), so there is nothing to carve against -- except that
    // a ROUND thing looks the same from every side. Its one silhouette used as all six views
    // carves a solid of revolution around its hex: right for barrels, tyres, rocks, bushes, trees,
    // the well; wrong for anything long, which is why only lone, compact cards are offered here.
    function revolveModel(name, fr, rot, dx, dy) {
        const key = 'rev:' + name + ':' + fr + ':' + rot + ':' + dx + ':' + dy;
        if (modelIds.has(key)) return modelIds.get(key);
        let id = -1;
        const buf = read(name);
        if (buf) {
            const frm = hull.readFrm(buf);
            const f = frm.dirs[rot][Math.min(fr, frm.fpd - 1)];
            const bx0 = frm.xOff[rot] + dx - (f.w >> 1), by0 = frm.yOff[rot] + dy - (f.h - 1);
            const views = [0, 1, 2, 3, 4, 5].map((r) => ({ r, fr: f, ax: 0, ay: 0, bx0, by0, dx0: bx0, dy0: by0, a: (r - 1) * Math.PI / 3, fwd: 0 }));
            id = packModel(views, 60);
        }
        modelIds.set(key, id);
        return id;
    }
    // one animation as voxel frames: [model ids], fps, and walking speed in units/s (the whole
    // cycle's forward travel, fitted over the six directions, over the cycle's duration)
    function voxelAnim(name, acc) {
        if (!critFrm.has(name)) voxelModel(name, 0, acc);
        const frm = critFrm.get(name);
        if (!frm) return null;
        const frames = [];
        for (let f = 0; f < frm.fpd; f++) frames.push(voxelModel(name, f, acc));
        if (frames.some((id) => id < 0)) return null;
        const travel = acc ? hull.viewsOf(frm, frm.fpd - 1, true)[0].fwd : 0;
        return { frames, fps: frm.fps, speed: +(travel / (frm.fpd / frm.fps)).toFixed(3) };
    }

    const levels = [];
    const replacements = {}; // set piece id -> its model (.glb, base64)
    const debugOut = [];
    for (let e = 0; e < 3; e++) {
        const sq = map.squares[e];
        const objs = map.objects.filter((o) => o.elevation === e);
        if (!sq && !objs.length) { levels.push(null); continue; }
        const L = { floor: [], roof: [], decal: [], panel: [], trans: [], bill: [], board: [], vox: [], setpieces: [], blocked: new Set() };
        let standing = [];
        const setAside = {}; // set piece id -> its slices, which a model replaces (--doom)

        if (sq) {
            for (let s = 0; s < 10000; s++) {
                const v = sq[s];
                const floor = v & 0xFFFF, roof = (v >>> 16) & 0xFFFF;
                const [tx, ty] = squareTopLeft(s);
                if (((floor >> 12) & 1) === 0 && (floor & 0xFFF) !== 1) {
                    const f = frm(artName((OBJ_TILE << 24) | (floor & 0xFFF)));
                    if (f) L.floor.push([...rel(tx, ty), atlas.add('T' + (floor & 0xFFF), f.dirs[0][0])]);
                }
                if ((roof & 0xFFF) !== 1 && (roof & 0xFFF) !== 0) {
                    const f = frm(artName((OBJ_TILE << 24) | (roof & 0xFFF)));
                    if (f) L.roof.push([...rel(tx, ty - 96), atlas.add('T' + (roof & 0xFFF), f.dirs[0][0])]);
                }
            }
        }

        let decalOrder = 0;
        for (const o of objs) {
            const type = o.pid >> 24;
            if (o.flags & 0x01) { stats.hidden++; continue; } // OBJECT_HIDDEN
            if (type === OBJ_MISC) { stats.misc++; continue; }
            // Walking blocks where the engine's obj_blocking_at would: anything without
            // OBJECT_NO_BLOCK (0x10) that stands (walls, scenery, critters, containers), plus the
            // ring around an OBJECT_MULTIHEX one. Doors are left open, so buildings can be entered.
            const standsInWay = type === OBJ_WALL || type === OBJ_CRITTER
                || (type === OBJ_SCENERY && o.subtype !== 0) || (type === OBJ_ITEM && o.subtype === 1);
            if (standsInWay && !(o.flags & 0x10) && !(o.flags & 0x08)) {
                L.blocked.add(o.tile);
                if (opts.debug) (L.blockedBy ||= {})[o.tile] = (L.blockedBy[o.tile] || '') + ' ' + (artName(o.fid) || '?').split(BS).pop() + '/t' + type + '/f' + (o.flags >>> 0).toString(16);
                if (o.flags & 0x800) for (const t of hexNeighbours(o.tile)) L.blocked.add(t);
            }
            const name = artName(o.fid);
            const f = frm(name);
            if (!f) { stats.missingArt++; continue; }
            const rot = Math.min(Math.max(o.rotation, 0), 5);
            const fr = Math.min(Math.max(o.frame, 0), f.fpd - 1);
            const frame = f.dirs[rot][fr];
            if (!frame || !frame.w || !frame.h) continue;
            const [hx, hy] = hexTopLeft(o.tile);
            // obj_render_object: anchor = hex + (16, 8) + art offset + accumulated frame offset
            const ax = hx + 16 + f.xOff[rot] + o.x, ay = hy + 8 + f.yOff[rot] + o.y;
            const ulx = ax - (frame.w >> 1), uly = ay - (frame.h - 1);
            const [rx, ry] = rel(ulx, uly);
            const img = () => atlas.add(name + ':' + rot + ':' + fr, frame);
            const trans = (o.flags & 0xFC000) !== 0 && (o.flags & 0x8000) === 0; // TRANS_* except NONE

            const anim = (o.fid & 0xFF0000) >> 16;
            const lying = (anim >= 20 && anim <= 35) || (anim >= 48 && anim <= 63);
            const flat = (o.flags & 0x08) !== 0
                || (type === OBJ_ITEM && o.subtype !== 1) // items lie on the ground; containers stand
                || (type === OBJ_CRITTER && lying);

            if (type === OBJ_CRITTER && lying && !opts.billboards) {
                // a body on the ground: its death animation has six directions too, so it carves
                const model = voxelModel(name, fr, false, false);
                if (model >= 0) {
                    L.vox.push([...rel(hx + 16, hy + 8), +((rot - 1) * 60).toFixed(1), model, 'corpse_' + label(name) + '_' + o.tile]);
                    stats.corpses++;
                    (stats.corpseAt ||= []).push([+((hx + 16 - origin[0]) / 32).toFixed(1), +((hy + 8 - origin[1]) / (8 * Math.sqrt(3))).toFixed(1), e]);
                    continue;
                }
            }
            if (flat) {
                L.decal.push([rx, ry, img(), decalOrder++]);
                continue;
            }

            if (type === OBJ_CRITTER) {
                // a true 3D body carved from its six directions (hull.js), turned to its heading;
                // the billboard below stays only as the fallback for art the carve can't hold
                const [cx, cy] = rel(hx + 16, hy + 8);
                const model = opts.billboards ? -1 : voxelModel(name, fr, false);
                if (model >= 0) {
                    L.vox.push([cx, cy, +((rot - 1) * 60).toFixed(1), model, 'critter_' + label(name) + '_' + o.tile]);
                    continue;
                }
                const dirs = [];
                for (let r = 0; r < 6; r++) {
                    const df = f.dirs[r][Math.min(fr, f.fpd - 1)];
                    const dx = f.xOff[r] + (r === rot ? o.x : 0), dy = f.yOff[r] + (r === rot ? o.y : 0);
                    dirs.push([atlas.add(name + ':' + r + ':' + fr, opts.billboards ? noShadow(df) : df), dx - (df.w >> 1), dy - (df.h - 1)]);
                }
                L.bill.push([cx, cy, rot, dirs]);
                continue;
            }

            // Standing sprite on vertical panels over a fitted ground line (see fitGround).
            //  - walls and doors: one of the two wall axes, or a corner of both. The proto's
            //    light-type bits (flags_ext >> 27, used for the "egg" in obj_render_object) name
            //    the axis of a straight wall -- none = NORTH_SOUTH = axis b, 0x08000000 =
            //    EAST_WEST = axis a, agreeing with the sprite's own bottom edge ~97% of the time --
            //    so they only tip the balance; corners are recognised by shape.
            //  - other scenery and containers: grouped into whole objects first (groupScenery),
            //    then a card facing the original camera unless the object's bottom edge clearly
            //    runs along an axis (fences, junk walls -- a row of cards looked like blinds).
            const bottom = bottomEdge(frame, uly);
            const wallLike = type === OBJ_WALL || (type === OBJ_SCENERY && o.subtype === 0);
            const piece = {
                trans, rx, ry, img: img(), segs: null, ulx, uly, w: frame.w, h: frame.h, bottom, tile: o.tile,
                name, type, anchorX: hx + 16, anchorY: hy + 8, rot, fr, ox: o.x, oy: o.y,
                door: type === OBJ_SCENERY && o.subtype === 0,
                lowest: Math.max(...bottom.filter((b) => !Number.isNaN(b))),
                tables: SLOPES.map((m) => slopeTable(bottom, ulx, m)),
            };
            if (!Number.isFinite(piece.lowest)) continue; // a frame with no opaque pixel
            if (wallLike) {
                // A straight wall's proto says its axis, and that is final: a 16 px piece (a post,
                // a joint) has no bottom edge long enough to show a slope, and letting its shape
                // vote turned every other post of the brahmin pen across the fence. Only a clearly
                // better corner fit may override it. Corner and door protos don't say: shape decides.
                const light = (o.ext >>> 27) & 0x1F;
                const flagged = type === OBJ_WALL && (light === 0 || light === 1) ? (light === 0 ? AXIS_B : AXIS_A) : 0;
                if (flagged) {
                    const straight = fitGround(bottom, ulx, [flagged], false, 0);
                    const corner = frame.w >= 24 ? fitGround(bottom, ulx, [AXIS_A, AXIS_B], true, 0) : null;
                    piece.segs = corner && corner.segs.length === 2 && corner.cost < straight.cost * 0.3 ? corner.segs : straight.segs;
                } else {
                    piece.segs = fitGround(bottom, ulx, [AXIS_A, AXIS_B], true, 0).segs;
                }
            }
            // a set piece (setpieces.js): its slices stand aside, a real model takes their place
            const sp = opts.billboards && setPieces.setPieceOf(mapName, name);
            if (sp) { (setAside[sp.id] ||= { def: sp, parts: [] }).parts.push(piece); continue; }
            standing.push(piece);
        }

        // A billboard of a lump of sprites (Doom's way with decorations): its pieces where the game
        // drew them, back to front, the lump's lowest pixel on the ground.
        const boardOf = (g) => {
            const ax = (Math.min(...g.map((q) => q.ulx)) + Math.max(...g.map((q) => q.ulx + q.w))) / 2;
            const ay = Math.max(...g.map((q) => q.lowest)) + 1;
            const back = [...g].sort((a, b) => a.anchorY - b.anchorY);
            return [...rel(ax, ay), back.map((q) => [q.img, q.ulx - ax, q.uly - ay])];
        };
        // set pieces kept in their own art (--doom, setpieces.js mode 'paint'): the long walls
        // stand again, cut at the face height with no lid -- the map's roof tiles are the roof --
        // and the rest (the gate's cars and barrels at its two ends) become billboards
        for (const [id, { def, parts }] of Object.entries(setAside)) {
            if (def.mode === 'textured') {
                // a clean box textured with its own cleanest wall, tiled (setpieces.js)
                const { lumps } = setPieces.splitSetPiece(parts);
                const place = setPieces.placeSetPiece(def, parts, origin);
                const made = setPieces.texturedContainer(def, parts, place, (p) => frm(p.name).dirs[p.rot][p.fr], palette);
                if (made) {
                    for (const g of lumps) L.board.push(boardOf(g));
                    // its own painted roof, trimmed to the box (geometry.js clips a tile with a 4th field)
                    const over = new Set(setPieces.roofOver(place, L.roof));
                    const clip = [place.at[0], place.at[1], place.angle, place.dims[0] / 2, place.dims[1] / 2];
                    L.roof = L.roof.map((r) => (over.has(r) ? [r[0], r[1], r[2], clip] : r));
                    stats.roofsUnderSetPieces = (stats.roofsUnderSetPieces || 0) + over.size;
                    if (def.innerWidth) {
                        // narrower than the game's passage: the hexes under its new walls block, and
                        // the hexes its old painted walls stood on are free ground again
                        const c = Math.cos(place.angle), s = Math.sin(place.angle), hz = place.dims[1] / 2;
                        const oldWalls = new Set(setPieces.splitSetPiece(parts).walls.map((p) => p.tile));
                        const rows = parts.map((p) => Math.floor(p.tile / 200)), cols = parts.map((p) => p.tile % 200);
                        let added = 0, freed = 0;
                        for (let row = Math.min(...rows) - 2; row <= Math.max(...rows) + 2; row++) {
                            for (let col = Math.min(...cols) - 2; col <= Math.max(...cols) + 2; col++) {
                                const tile = row * 200 + col, [hx, hy] = hexTopLeft(tile), [x, y] = rel(hx + 16, hy + 8);
                                const X = x / 32 - place.at[0], Z = y / (8 * Math.sqrt(3)) - place.at[1];
                                const lx = Math.abs(X * c + Z * s), lz = Math.abs(-X * s + Z * c);
                                const underWall = lx <= place.dims[0] / 2 && lz > hz - 0.5 && lz < hz + 0.4;
                                if (underWall && !L.blocked.has(tile)) { L.blocked.add(tile); added++; }
                                else if (!underWall && oldWalls.has(tile) && L.blocked.delete(tile)) freed++;
                            }
                        }
                        stats.setPieceHexes = `${added} blocked under its walls, ${freed} freed where the painted walls stood`;
                    }
                    L.setpieces.push(place);
                    replacements[def.id] = made.glb.toString('base64');
                    stats.setPieces = (stats.setPieces || []).concat(`${id}: textured corridor ${place.dims.join(' x ')} at ${place.at.join(', ')}, tile ${made.tile.W}x${made.tile.H} px from column ${made.tile.from} (seam ${made.tile.seam}), ${lumps.length} billboards`);
                    delete setAside[id];
                    continue;
                }
            }
            if (def.mode !== 'paint' && def.mode !== 'textured') continue; // 'model': swapped at the end
            const { walls, lumps } = setPieces.splitSetPiece(parts);
            for (const p of walls) { p.cut = true; standing.push(p); }
            for (const g of lumps) L.board.push(boardOf(g));
            stats.setPieces = (stats.setPieces || []).concat(`${id}: kept in its own art -- ${walls.length} wall slices, ${lumps.length} billboards`);
            delete setAside[id];
        }

        stats.objects += groupScenery(standing.filter((p) => !p.segs && !p.trans));
        for (const p of standing) if (!p.segs) p.segs = fitGround(p.bottom, p.ulx, [0], false, 0).segs;

        if (opts.billboards) {
            // Doom stood its decorations up as sprites turned to the eye -- barrels, lamps, trees.
            // Scenery standing as a card (facing the original camera) smeared into streaks from eye
            // level; a billboard stays the picture it was drawn as. An object of several sprites
            // (the gate car) is ONE billboard, its pieces where the game drew them, its lowest
            // pixel on the ground. Anything standing along a wall axis (fences, junk walls) stays.
            const done = new Set();
            for (const p of standing) {
                const g = p.group;
                if (!g || p.door || p.trans || done.has(g)) continue;
                if (!g.every((q) => q.segs.length === 1 && q.segs[0].axis === 0)) continue;
                done.add(g);
                L.board.push(boardOf(g));
                for (const q of g) q.boarded = true;
                stats.boards = (stats.boards || 0) + 1;
            }
            standing = standing.filter((p) => !p.boarded);
        }

        // lone, compact scenery standing as a card -> a solid of revolution (revolveModel)
        for (const p of standing) {
            if (p.type !== OBJ_SCENERY || p.door || p.trans || p.groupSize !== 1 || p.w > 80) continue;
            if (p.segs.length !== 1 || p.segs[0].axis !== 0) continue;
            const model = revolveModel(p.name, p.fr, p.rot, p.ox, p.oy);
            if (model < 0) continue;
            L.vox.push([...rel(p.anchorX, p.anchorY), 0, model, 'object_' + label(p.name) + '_' + p.tile]);
            p.revolved = true;
            stats.revolved++;
        }
        standing = standing.filter((p) => !p.revolved);
        for (const p of standing) {
            for (const s of p.segs) stats.ground[s.axis]++;
            if (p.segs.length === 2) stats.ground.corners++;
        }

        snapWallRuns(standing.filter((p) => !p.trans));

        // A wall under a roof is 96 px tall -- the height square_coord_roof lifts the roof by --
        // and whatever its sprite has above that is its TOP, seen from above. Standing that part
        // up made building walls poke through their own roofs, so the viewer lays it flat at
        // roof height ("cap"). Walls with no roof around them (a town wall with round pillar
        // heads) keep standing whole.
        const roofAt = (px, py) => {
            if (!sq) return false;
            // continuous square coordinates of a ground point (inverse of squareTopLeft)
            const dx = px + 16, dy = py - 10;
            const u = (24 * dx - 32 * dy) / 1536, v = (12 * dx + 48 * dy) / 1536;
            for (let du = -1; du <= 1; du++) {
                for (let dv = -1; dv <= 1; dv++) {
                    const col = Math.floor(u) + du, row = Math.floor(v) + dv;
                    if (col < 0 || col > 99 || row < 0 || row > 99) continue;
                    const roof = (sq[row * 100 + (99 - col)] >>> 16) & 0xFFF;
                    if (roof !== 0 && roof !== 1) return true;
                }
            }
            return false;
        };
        for (const p of standing) {
            for (const s of p.segs) {
                // relative coordinates: y - oy = (k - oy + m*ox) + m*(x - ox)
                const m = SLOPES[s.axis];
                const k = +(s.k - origin[1] + m * origin[0]).toFixed(3);
                const xm = (s.x0 + s.x1) / 2;
                // --doom caps EVERY wall: from eye level the top of a free-standing wall smeared
                // into streaks; flat at 96 px, on a box as deep as it shows (the viewer's
                // deepBoxes), it is the top of the wall again -- the gate container a crate
                // cap 2: a set piece's wall kept in its own art -- cut there, no lid (see above)
                const cap = s.axis === 0 ? 0 : p.cut ? 2 : opts.billboards || roofAt(xm, s.k + m * xm) ? 1 : 0;
                if (cap) stats.ground.capped++;
                (p.trans ? L.trans : L.panel).push([p.rx, p.ry, p.img, s.axis, k,
                    +(s.x0 - origin[0]).toFixed(2), +(s.x1 - origin[0]).toFixed(2), cap]);
            }
        }
        for (const { def, parts } of Object.values(setAside)) {
            const place = setPieces.placeSetPiece(def, parts, origin);
            L.setpieces.push(place);
            replacements[def.id] = setPieces.modelFor(def, place.dims).glb;
            stats.setPieces = (stats.setPieces || []).concat(`${def.id}: ${parts.length} slices -> ${place.dims.join(' x ')} at ${place.at.join(', ')}`);
        }
        if (opts.debug) (debugOut[e] = standing);
        L.blocked = [...L.blocked];
        levels.push(L);
    }

    // the dude, standing at the entrance (he is not part of the map file)
    const dudeFrm = frm('ART' + BS + 'CRITTERS' + BS + 'HMJMPSAA.FRM');
    let dude = null;
    if (dudeFrm) {
        const [hx, hy] = hexTopLeft(map.hdr.enteringTile);
        const [cx, cy] = rel(hx + 16, hy + 8);
        const dirs = [];
        for (let r = 0; r < 6; r++) {
            const df = dudeFrm.dirs[r][0];
            dirs.push([atlas.add('DUDE:' + r, opts.billboards ? noShadow(df) : df), dudeFrm.xOff[r] - (df.w >> 1), dudeFrm.yOff[r] - (df.h - 1)]);
        }
        const rot = Math.min(Math.max(map.hdr.enteringRotation, 0), 5);
        dude = { elevation: map.hdr.enteringElevation, b: [cx, cy, rot], facing: (rot - 1) * 60, dirs };
        if (!opts.billboards) {
            // the same man in 3D: standing, walking, running -- carved from his own animations
            const crit = (code) => 'ART' + BS + 'CRITTERS' + BS + 'HMJMPS' + code + '.FRM';
            const still = voxelModel(crit('AA'), 0, false); // the idle fidget isn't a loop: frame 0 only
            dude.stand = still >= 0 ? { frames: [still], fps: 1, speed: 0 } : null;
            dude.walk = voxelAnim(crit('AB'), true);
            dude.run = voxelAnim(crit('AT'), true);
        }
    }

    const { pages, place } = atlas.pack();

    const palBuf = read('COLOR.PAL');
    const pal = Buffer.alloc(768);
    for (let i = 0; i < 256; i++) {
        const r = palBuf[i * 3], g = palBuf[i * 3 + 1], b = palBuf[i * 3 + 2];
        const ok = r <= 63 && g <= 63 && b <= 63; // color.cc: out-of-range entries load as black
        pal[i * 3] = ok ? r * 4 : 0; pal[i * 3 + 1] = ok ? g * 4 : 0; pal[i * 3 + 2] = ok ? b * 4 : 0;
    }

    return {
        scene: {
            map: mapName.toUpperCase(),
            name: map.hdr.name,
            entering: { elevation: map.hdr.enteringElevation },
            palette: pal.toString('base64'),
            pages,
            images: place,
            levels,
            dude,
            origin,
            models,
            doom: !!opts.billboards, // opens in first person, critters as billboards
            replacements, // set pieces' models (setpieces.js)
        },
        debug: opts.debug ? { origin, levels: debugOut } : undefined,
        stats: {
            ...stats,
            images: atlas.images.length,
            pages: pages.length,
            levels: levels.map((L, e) => L && `elev ${e}: floor ${L.floor.length}, roof ${L.roof.length}, decal ${L.decal.length}, panel ${L.panel.length}, trans ${L.trans.length}, critters ${L.bill.length}`).filter(Boolean),
        },
    };
}

// ---------------------------------------------------------------- main
if (require.main === module) {
    // --doom: first person, as Doom drew it -- every critter a billboard of its own six sprites
    const args = process.argv.slice(2), doom = args.includes('--doom');
    const [mapName, outArg] = args.filter((a) => a !== '--doom');
    if (!mapName) {
        console.error('usage: node build.js <MAPNAME> [out.html] [--doom]');
        process.exit(1);
    }
    const out = outArg || path.join(ROOT, 'build', 'paper3d', mapName.toUpperCase() + (doom ? '_doom' : '') + '.html');
    const { scene, stats } = build(mapName, { billboards: doom });
    const tpl = fs.readFileSync(path.join(__dirname, 'viewer.html'), 'utf8');
    const html = tpl.replace('/*SCENE*/null', () => JSON.stringify(scene))
        .replace('/*GEOMETRY*/', () => fs.readFileSync(path.join(__dirname, 'geometry.js'), 'utf8'));
    fs.mkdirSync(path.dirname(out), { recursive: true });
    fs.writeFileSync(out, html);
    console.log(JSON.stringify(stats, null, 2));
    console.log(`wrote ${out} (${(html.length / 1024 / 1024).toFixed(2)} MB)`);
}

module.exports = { build };
