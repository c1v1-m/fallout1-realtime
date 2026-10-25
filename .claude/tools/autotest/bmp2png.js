// 8/24-bit BMP -> PNG, optionally cropped and scaled up by an integer. usage:
// node bmp2png.js in.bmp out.png [x y w h] [scale]
const fs = require('fs');
const zlib = require('zlib');
const [inFile, outFile, ...rest] = process.argv.slice(2);
const b = fs.readFileSync(inFile);
const off = b.readUInt32LE(10);
const W = b.readInt32LE(18);
let H = b.readInt32LE(22);
const bpp = b.readUInt16LE(28);
const topDown = H < 0;
H = Math.abs(H);
const hdr = b.readUInt32LE(14);
const pal = [];
if (bpp === 8) {
    const n = b.readUInt32LE(46) || 256;
    for (let i = 0; i < n; i++) {
        const p = 14 + hdr + i * 4;
        pal.push([b[p + 2], b[p + 1], b[p]]);
    }
}
// Fallout 2's screenshots leave the rows unpadded: take the row length from the pixel data's size.
const padded = Math.floor((W * bpp + 31) / 32) * 4;
const stride = b.length - off === W * (bpp / 8) * H ? W * (bpp / 8) : padded;
function px(x, y) {
    const row = topDown ? y : H - 1 - y;
    const p = off + row * stride;
    if (bpp === 8) return pal[b[p + x]] || [255, 0, 255];
    return [b[p + x * 3 + 2], b[p + x * 3 + 1], b[p + x * 3]];
}
let [cx, cy, cw, ch] = rest.length >= 4 ? rest.slice(0, 4).map(Number) : [0, 0, W, H];
const scale = rest.length === 5 ? Number(rest[4]) : rest.length === 1 ? Number(rest[0]) : 1;
const ow = cw * scale;
const oh = ch * scale;
const raw = Buffer.alloc((ow * 3 + 1) * oh);
for (let y = 0; y < oh; y++) {
    raw[y * (ow * 3 + 1)] = 0;
    for (let x = 0; x < ow; x++) {
        const c = px(cx + Math.floor(x / scale), cy + Math.floor(y / scale));
        const p = y * (ow * 3 + 1) + 1 + x * 3;
        raw[p] = c[0];
        raw[p + 1] = c[1];
        raw[p + 2] = c[2];
    }
}
function crc32(buf) {
    let c, crc = 0xffffffff;
    for (let n = 0; n < buf.length; n++) {
        c = (crc ^ buf[n]) & 0xff;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        crc = (crc >>> 8) ^ c;
    }
    return (crc ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
    const len = Buffer.alloc(4);
    len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
}
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(ow, 0);
ihdr.writeUInt32BE(oh, 4);
ihdr[8] = 8;
ihdr[9] = 2;
const png = Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
fs.writeFileSync(outFile, png);
console.log(outFile, W + 'x' + H, '->', ow + 'x' + oh);
