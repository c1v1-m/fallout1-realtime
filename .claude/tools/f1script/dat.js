// Fallout 1 DAT1 reader as a module. Same format handling as dat1.js (the CLI), which stays
// as it is -- this is the importable half, for tools that need several files out of a DAT.
const fs = require('fs');

// LZSS, ported from src/plib/db/lzss.cc
function lzssDecode(src, srcOff, compressedLen, outLen, out, o0) {
    const ring = Buffer.alloc(4116, 0x20);
    let ringIdx = 4078;
    if (!out) { out = Buffer.alloc(outLen); o0 = 0; }
    let o = o0, i = srcOff;
    let left = compressedLen;
    while (left > 0 && o < outLen) {
        const flag = src[i++]; left--;
        for (let b = 0; b < 8 && left > 0 && o < outLen; b++) {
            if (flag & (1 << b)) {
                const c = src[i++]; left--;
                out[o++] = c;
                ring[ringIdx] = c; ringIdx = (ringIdx + 1) & 0xFFF;
            } else {
                const lo = src[i++], hi = src[i++]; left -= 2;
                const dictOff = lo | ((hi & 0xF0) << 4);
                const runLen = (hi & 0x0F) + 3;
                for (let k = 0; k < runLen; k++) {
                    const c = ring[(dictOff + k) & 0xFFF];
                    out[o++] = c;
                    ring[ringIdx] = c; ringIdx = (ringIdx + 1) & 0xFFF;
                }
            }
        }
    }
    return { out, o, i };
}

function openDat(path) {
    const buf = fs.readFileSync(path);
    let p = 0;
    const rdLong = () => { const v = buf.readInt32BE(p); p += 4; return v; };
    const rdName = () => { const n = buf[p]; p += 1; const s = buf.toString('latin1', p, p + n); p += n; return s; };

    const rSize = rdLong(); rdLong(); rdLong(); rdLong();
    const dirs = [];
    for (let i = 0; i < rSize; i++) dirs.push(rdName());

    const files = new Map();
    for (const d of dirs) {
        const size = rdLong(); rdLong(); rdLong(); rdLong();
        for (let i = 0; i < size; i++) {
            const name = rdName();
            const flags = rdLong(), offset = rdLong(), length = rdLong(), packed = rdLong();
            files.set((d === '.' ? '' : d + String.fromCharCode(92)) + name, { flags, offset, length, packed });
        }
    }

    function extract(key) {
        const e = files.get(key);
        if (!e) return null;
        const f = e.flags === 0 ? 16 : e.flags;
        const kind = f & 0xF0;
        if (kind === 0x10) return lzssDecode(buf, e.offset, e.packed, e.length).out;
        if (kind === 0x20) return buf.subarray(e.offset, e.offset + e.length);
        if (kind === 0x40) {
            const out = Buffer.alloc(e.length);
            let o = 0, i = e.offset;
            while (o < e.length) {
                const v = buf.readUInt16BE(i); i += 2;
                if (v & 0x8000) {
                    const n = v & 0x7FFF;
                    buf.copy(out, o, i, i + n); o += n; i += n;
                } else {
                    const r = lzssDecode(buf, i, v, e.length, out, o);
                    o = r.o; i += v;
                }
            }
            return out;
        }
        return buf.subarray(e.offset, e.offset + e.length);
    }

    return { files, extract };
}

module.exports = { openDat };
