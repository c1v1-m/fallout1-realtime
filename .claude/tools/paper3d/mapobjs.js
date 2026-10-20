// mapobjs: lists a map's scripted objects -- tile, elevation, pid, and the script (scripts.lst name) --
// so a self-played scenario can put the dude next to the door, the elevator or the computer it is about.
// usage: node mapobjs.js MAP [name-regex | --nav]
//   --nav: the ways between floors and maps -- spatial scripts (a tile and radius that fire when
//   stepped into: Mariposa's elevators are these), elevator / stair / ladder scenery, exit grids.
const fs = require('fs');
const path = require('path');
const Module = require('module');
const file = path.join(__dirname, 'build.js');
const src = fs.readFileSync(file, 'utf8')
    .replace('module.exports = { build };', 'module.exports = { openGameFs, makeProtos, parseMap, SIDX, SPATIAL };')
    .replace('function parseMap(buf, proto) {', 'const SIDX = new Map();\nconst SPATIAL = [];\nfunction parseMap(buf, proto) {')
    .replace('if (st === 1) p += 8; // spatial: built_tile, radius',
        'if (st === 1) { SPATIAL.push({ sid, built: buf.readInt32BE(p), radius: buf.readInt32BE(p + 4) }); p += 8; }')
    .replace('                p += 14 * 4;', '                SIDX.set(sid, buf.readInt32BE(p + 4)); p += 14 * 4;');
const m = new Module(file, module);
m.filename = file;
m.paths = Module._nodeModulePaths(path.dirname(file));
m._compile(src, file);
const { openGameFs, makeProtos, parseMap, SIDX, SPATIAL } = m.exports;
const read = openGameFs();
const proto = makeProtos(read);
const BS = String.fromCharCode(92);

const [mapName, filter] = process.argv.slice(2);
const lst = read('SCRIPTS' + BS + 'SCRIPTS.LST').toString('latin1').split(/\r?\n/);
const map = parseMap(read('MAPS' + BS + mapName.toUpperCase() + '.MAP'), proto);
const nav = filter === '--nav';
const pat = new RegExp(nav ? '.' : (filter || '.'), 'i');

function scriptOf(sid) {
    const idx = SIDX.get(sid);
    const line = idx !== undefined && lst[idx] ? lst[idx] : '?';
    return { name: line.split(';')[0].trim(), what: (line.split(';')[1] || '').split('#')[0].trim() };
}

console.log(`map ${map.hdr.name} enter tile=${map.hdr.enteringTile} elev=${map.hdr.enteringElevation} script=${map.hdr.scriptIndex}`);
if (nav) {
    for (const sp of SPATIAL) {
        const s = scriptOf(sp.sid);
        console.log(`elev=${sp.built >>> 29} tile=${sp.built & 0x3ffffff} spatial r=${sp.radius} script=${s.name} (${s.what})`);
    }
}
const SUB = ['door', 'stairs', 'elevator', 'ladder-up', 'ladder-down', 'generic'];
for (const o of map.objects) {
    const type = o.pid >> 24;
    if (nav) {
        if (type === 3 && (o.subtype === 1 || o.subtype === 2 || o.subtype === 3 || o.subtype === 4)) {
            console.log(`elev=${o.elevation} tile=${o.tile} ${SUB[o.subtype]} pid=0x${(o.pid >>> 0).toString(16)}`);
        } else if (type === 5 && o.pid >= 0x5000010 && o.pid <= 0x5000017) {
            console.log(`elev=${o.elevation} tile=${o.tile} exit-grid pid=0x${(o.pid >>> 0).toString(16)}`);
        }
        continue;
    }
    if (o.sid === -1 || o.sid === undefined) {
        continue;
    }
    const s = scriptOf(o.sid);
    if (!pat.test(`${s.name} ${s.what}`)) {
        continue;
    }
    console.log(`elev=${o.elevation} tile=${o.tile} row=${Math.floor(o.tile / 200)} col=${o.tile % 200} pid=0x${(o.pid >>> 0).toString(16)} type=${type} script=${s.name} (${s.what})`);
}
