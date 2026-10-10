// A glTF 2.0 binary (.glb) builder, shared by export.js (whole maps) and sprite3d.js (models):
// buffers, accessors, nodes, and the packing of it all into one file.

'use strict';

// ---------------------------------------------------------------- glTF assembly
function makeGltf(generator = 'fallout1-ce paper3d') {
    const json = {
        asset: { version: '2.0', generator },
        extensionsUsed: ['KHR_materials_unlit'],
        scene: 0, scenes: [{ name: 'Scene', nodes: [] }],
        nodes: [], meshes: [], materials: [], textures: [], images: [], samplers: [],
        accessors: [], bufferViews: [], buffers: [], cameras: [],
    };
    const chunks = [];
    let offset = 0;
    function view(buf, target) {
        const pad = (4 - (offset % 4)) % 4;
        if (pad) { chunks.push(Buffer.alloc(pad)); offset += pad; }
        json.bufferViews.push({ buffer: 0, byteOffset: offset, byteLength: buf.length, ...(target ? { target } : {}) });
        chunks.push(buf);
        offset += buf.length;
        return json.bufferViews.length - 1;
    }
    function accessor(typed, type, componentType, target, extra = {}) {
        const buf = Buffer.from(typed.buffer, typed.byteOffset, typed.byteLength);
        const n = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 }[type];
        json.accessors.push({ bufferView: view(buf, target), componentType, count: typed.length / n, type, ...extra });
        return json.accessors.length - 1;
    }
    function positions(arr) {
        const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
        for (let i = 0; i < arr.length; i += 3) for (let q = 0; q < 3; q++) {
            min[q] = Math.min(min[q], arr[i + q]); max[q] = Math.max(max[q], arr[i + q]);
        }
        return accessor(new Float32Array(arr), 'VEC3', 5126, 34962, { min, max });
    }
    function indices(arr, vertexCount) {
        return vertexCount < 65536
            ? accessor(new Uint16Array(arr), 'SCALAR', 5123, 34963)
            : accessor(new Uint32Array(arr), 'SCALAR', 5125, 34963);
    }
    function node(n, parent) {
        json.nodes.push(n);
        const id = json.nodes.length - 1;
        if (parent === undefined) json.scenes[0].nodes.push(id);
        else (json.nodes[parent].children ||= []).push(id);
        return id;
    }
    function finish() {
        const bin = Buffer.concat(chunks);
        const binPadded = Buffer.concat([bin, Buffer.alloc((4 - (bin.length % 4)) % 4)]);
        json.buffers.push({ byteLength: binPadded.length });
        for (const k of ['cameras', 'textures', 'images', 'samplers', 'materials']) if (!json[k].length) delete json[k];
        let text = Buffer.from(JSON.stringify(json), 'utf8');
        text = Buffer.concat([text, Buffer.alloc((4 - (text.length % 4)) % 4, 0x20)]);
        const header = Buffer.alloc(12);
        header.writeUInt32LE(0x46546c67, 0); header.writeUInt32LE(2, 4);
        header.writeUInt32LE(12 + 8 + text.length + 8 + binPadded.length, 8);
        const jh = Buffer.alloc(8); jh.writeUInt32LE(text.length, 0); jh.writeUInt32LE(0x4e4f534a, 4);
        const bh = Buffer.alloc(8); bh.writeUInt32LE(binPadded.length, 0); bh.writeUInt32LE(0x004e4942, 4);
        return Buffer.concat([header, jh, text, bh, binPadded]);
    }
    return { json, view, accessor, positions, indices, node, finish };
}


const srgbToLinear = (c) => { c /= 255; return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };

// ---------------------------------------------------------------- PNG (RGBA), for embedded textures
const zlib = require('zlib');
function crc32(buf) {
    let c, crc = 0xffffffff;
    for (let n = 0; n < buf.length; n++) {
        c = (crc ^ buf[n]) & 0xff;
        for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
        crc = c ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
}
function pngRGBA(w, h, rgba) {
    const chunk = (type, data) => {
        const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
        const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
        const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(body));
        return Buffer.concat([len, body, crc]);
    };
    const raw = Buffer.alloc((w * 4 + 1) * h);
    for (let y = 0; y < h; y++) rgba.copy(raw, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4);
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
    return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}

module.exports = { makeGltf, srgbToLinear, pngRGBA };
