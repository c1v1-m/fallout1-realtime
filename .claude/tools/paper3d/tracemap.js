// tracemap: renders one map region the way the engine draws it (floor squares, walls, scenery) and
// critters where a TRACE says they stood, so "where is that critter really" can be LOOKED at.
// usage: node tracemap.js MAP trace.log centerTile out.png [W H] [time]
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const Module = require('module');
const file = require('path').join(__dirname, 'build.js');
const src = fs.readFileSync(file, 'utf8').replace('module.exports = { build };',
  'module.exports = { build, openGameFs, makeProtos, parseMap, hexTopLeft, squareTopLeft, makeArtNamer, parseFrm };');
const m = new Module(file, module);
m.filename = file; m.paths = Module._nodeModulePaths(path.dirname(file));
m._compile(src, file);
const { openGameFs, makeProtos, parseMap, hexTopLeft, squareTopLeft, makeArtNamer, parseFrm } = m.exports;
const read = openGameFs();
const proto = makeProtos(read);
const artName = makeArtNamer(read);
const BS = String.fromCharCode(92);

const [mapName, tracePath, centerArg, outPath, wArg, hArg, timeArg] = process.argv.slice(2);
const map = parseMap(read('MAPS' + BS + mapName.toUpperCase() + '.MAP'), proto);
const pal = read('COLOR.PAL');
const W = +(wArg || 1400), H = +(hArg || 1000);
const elev = 0;
const center = +centerArg;
const [cx0, cy0] = hexTopLeft(center);
const ox = cx0 + 16 - W / 2, oy = cy0 + 8 - H / 2;
const img = Buffer.alloc(W * H * 3); // black
const frmCache = new Map();
const frm = (name) => { if (!name) return null; if (frmCache.has(name)) return frmCache.get(name); const b = read(name); const f = b ? parseFrm(b) : null; frmCache.set(name, f); return f; };
function blit(fr, left, top, tint) {
  for (let y = 0; y < fr.h; y++) {
    const py = Math.round(top + y - oy); if (py < 0 || py >= H) continue;
    for (let x = 0; x < fr.w; x++) {
      const c = fr.data[y * fr.w + x]; if (!c) continue;
      const px = Math.round(left + x - ox); if (px < 0 || px >= W) continue;
      const o = (py * W + px) * 3;
      let r = pal[c * 3] * 4, g = pal[c * 3 + 1] * 4, b = pal[c * 3 + 2] * 4;
      if (tint) { r = (r + tint[0]) >> 1; g = (g + tint[1]) >> 1; b = (b + tint[2]) >> 1; }
      img[o] = r; img[o + 1] = g; img[o + 2] = b;
    }
  }
}
function dot(sx, sy, rad, col) {
  for (let y = -rad; y <= rad; y++) for (let x = -rad; x <= rad; x++) {
    if (x * x + y * y > rad * rad) continue;
    const px = Math.round(sx + x - ox), py = Math.round(sy + y - oy);
    if (px < 0 || px >= W || py < 0 || py >= H) continue;
    const o = (py * W + px) * 3; img[o] = col[0]; img[o + 1] = col[1]; img[o + 2] = col[2];
  }
}
// floor
const sqs = map.squares[elev];
for (let s = 0; s < 10000; s++) {
  const f = sqs[s] & 0xFFF; if (f === 1 || ((sqs[s] >> 12) & 1)) continue;
  const fr = frm(artName((4 << 24) | f)); if (!fr) continue;
  const [tx, ty] = squareTopLeft(s);
  blit(fr.dirs[0][0], tx, ty);
}
// objects (walls + scenery + items), painter's order by tile
function drawObj(fid, tile, offx, offy, frameNo, rot, tint) {
  const f = frm(artName(fid)); if (!f) return;
  const r = Math.min(rot || 0, f.dirs.length - 1);
  const dirFrames = f.dirs[r] || f.dirs[0];
  const fr = dirFrames[Math.min(frameNo || 0, dirFrames.length - 1)];
  const [hx, hy] = hexTopLeft(tile);
  const ax = hx + 16 + (f.xOff[r] || 0) + offx, ay = hy + 8 + (f.yOff[r] || 0) + offy;
  blit(fr, ax - (fr.w >> 1), ay - (fr.h - 1), tint);
}
const objs = map.objects.filter(o => o.elevation === elev && [0, 2, 3].includes(o.pid >> 24) && !(o.flags & 0x1));
objs.sort((a, b) => { const fa = (a.flags & 0x8) ? 0 : 1, fb = (b.flags & 0x8) ? 0 : 1; if (fa !== fb) return fa - fb; return hexTopLeft(a.tile)[1] - hexTopLeft(b.tile)[1] || a.tile - b.tile; });
// critters from the trace: the snapshot nearest to `time` (default: last)
const T = timeArg ? +timeArg : Infinity;
const crit = new Map();
for (const L of fs.readFileSync(tracePath, 'utf8').split('\n')) {
  const mm = L.match(/s=([\d.]+) \[STATE\/critter\] id=(\d+) pid=(\d+) tile=(-?\d+) x=(-?\d+) y=(-?\d+) fid=([0-9a-f]+) anim=(\d+) frame=(\d+).*hid=(\d) dead=(\d).*host=(\d)/);
  if (!mm) continue;
  if (+mm[1] > T) continue;
  crit.set(mm[2], { pid: +mm[3], tile: +mm[4], x: +mm[5], y: +mm[6], fid: parseInt(mm[7], 16), frame: +mm[9], hid: mm[10] === '1', dead: mm[11] === '1', host: mm[12] === '1' });
}
const all = objs.map(o => ({ kind: 'o', o, y: hexTopLeft(o.tile)[1], flat: !!(o.flags & 0x8) }))
  .concat([...crit.entries()].filter(([, c]) => !c.hid).map(([id, c]) => ({ kind: 'c', id, c, y: hexTopLeft(c.tile)[1], flat: c.dead })));
all.sort((a, b) => (a.flat === b.flat ? a.y - b.y : (a.flat ? -1 : 1)));
for (const e of all) {
  if (e.kind === 'o') drawObj(e.o.fid, e.o.tile, e.o.x, e.o.y, e.o.frame, e.o.rotation);
  else drawObj(e.c.fid, e.c.tile, e.c.x, e.c.y, e.c.frame, 2, e.c.host ? [255, 40, 40] : null);
}
// markers at the critters' real hexes
for (const [id, c] of crit) {
  if (c.hid) continue;
  const [hx, hy] = hexTopLeft(c.tile);
  dot(hx + 16, hy + 8, 3, c.pid === 16777216 ? [0, 255, 0] : c.host ? [255, 0, 0] : [255, 255, 0]);
}
// PNG
const crcTable = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc32 = (buf) => { let c = -1; for (const b of buf) c = crcTable[(c ^ b) & 0xFF] ^ (c >>> 8); return (c ^ -1) >>> 0; };
const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const td = Buffer.concat([Buffer.from(type), data]); const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td)); return Buffer.concat([len, td, crc]); };
const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 2;
const raw = Buffer.alloc((W * 3 + 1) * H); for (let y = 0; y < H; y++) img.copy(raw, y * (W * 3 + 1) + 1, y * W * 3, (y + 1) * W * 3);
fs.writeFileSync(outPath, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
console.log('wrote', outPath, W + 'x' + H, 'critters', crit.size);
