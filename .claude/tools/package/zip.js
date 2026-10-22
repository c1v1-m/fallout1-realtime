// Writes a release zip with unix permissions, so the Steam Deck script stays executable when it is
// unpacked on Linux (Compress-Archive and Windows tar store no mode bits).
//
//   node zip.js <out.zip> <root folder> [executable suffix ...]
//
// Every file under the root goes in under "<root's name>/...", deflated. Files whose name ends with
// one of the suffixes (default ".sh") get mode 0755, the rest 0644.
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const [out, root, ...execSuffixes] = process.argv.slice(2);
if (!out || !root) {
    console.error('usage: node zip.js <out.zip> <root folder> [executable suffix ...]');
    process.exit(2);
}
const exec = execSuffixes.length ? execSuffixes : ['.sh'];

const crcTable = new Uint32Array(256).map((_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
});
const crc32 = (buf) => {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < buf.length; i++) c = crcTable[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
};

const files = [];
const walk = (dir) => {
    for (const name of fs.readdirSync(dir).sort()) {
        const full = path.join(dir, name);
        if (fs.statSync(full).isDirectory()) walk(full);
        else files.push(full);
    }
};
walk(root);

const base = path.basename(path.resolve(root));
const chunks = [];
const central = [];
let offset = 0;

for (const full of files) {
    const rel = base + '/' + path.relative(root, full).split(path.sep).join('/');
    const data = fs.readFileSync(full);
    const packed = zlib.deflateRawSync(data, { level: 9 });
    const crc = crc32(data);
    const nameBuf = Buffer.from(rel, 'utf8');
    const mtime = fs.statSync(full).mtime;
    const dosTime = (mtime.getHours() << 11) | (mtime.getMinutes() << 5) | (mtime.getSeconds() >> 1);
    const dosDate = ((mtime.getFullYear() - 1980) << 9) | ((mtime.getMonth() + 1) << 5) | mtime.getDate();
    const mode = exec.some((s) => rel.endsWith(s)) ? 0o100755 : 0o100644;

    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6); // UTF-8 names
    local.writeUInt16LE(8, 8);
    local.writeUInt16LE(dosTime, 10);
    local.writeUInt16LE(dosDate, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(packed.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    local.writeUInt16LE(0, 28);
    chunks.push(local, nameBuf, packed);

    const cd = Buffer.alloc(46);
    cd.writeUInt32LE(0x02014b50, 0);
    cd.writeUInt16LE((3 << 8) | 20, 4); // made by unix, so the mode below is honoured
    cd.writeUInt16LE(20, 6);
    cd.writeUInt16LE(0x0800, 8);
    cd.writeUInt16LE(8, 10);
    cd.writeUInt16LE(dosTime, 12);
    cd.writeUInt16LE(dosDate, 14);
    cd.writeUInt32LE(crc, 16);
    cd.writeUInt32LE(packed.length, 20);
    cd.writeUInt32LE(data.length, 24);
    cd.writeUInt16LE(nameBuf.length, 28);
    cd.writeUInt32LE((mode << 16) >>> 0, 38);
    cd.writeUInt32LE(offset, 42);
    central.push(cd, nameBuf);

    offset += local.length + nameBuf.length + packed.length;
    console.log((mode & 0o777).toString(8), data.length, rel);
}

const cdSize = central.reduce((n, b) => n + b.length, 0);
const end = Buffer.alloc(22);
end.writeUInt32LE(0x06054b50, 0);
end.writeUInt16LE(files.length, 8);
end.writeUInt16LE(files.length, 10);
end.writeUInt32LE(cdSize, 12);
end.writeUInt32LE(offset, 16);

fs.writeFileSync(out, Buffer.concat([...chunks, ...central, end]));
console.log('wrote', out, fs.statSync(out).size, 'bytes,', files.length, 'files');
