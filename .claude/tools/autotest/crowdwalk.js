// Walk-in-place finder for a trace (2026-10-30): for every critter, the longest stretch of [STATE/critter]
// samples with the walk or run art (anim 1 or 19, ANIM_RUNNING in both games) on ONE tile. A step takes well under a second, so a
// stretch of 2 s or more is a critter jogging on the spot.
//   node crowdwalk.js <trace.log> [fromSec] [toSec]
const fs = require('fs');
const [file, from, to] = process.argv.slice(2);
const lo = from ? parseFloat(from) : 0, hi = to ? parseFloat(to) : 1e9;
const runs = new Map();
let worst = [];
for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const m = line.match(/^\d+ s=([\d.]+) \[STATE\/critter\] id=(\d+) pid=(\d+) tile=(\d+) .*?anim=(\d+)/);
    if (!m) continue;
    const s = parseFloat(m[1]);
    if (s < lo || s > hi) continue;
    const id = m[2], tile = m[4], anim = +m[5];
    const r = runs.get(id);
    if ((anim === 1 || anim === 19) && r && r.tile === tile) {
        r.last = s;
    } else {
        if (r && r.last - r.first > 0) worst.push({ id, pid: r.pid, tile: r.tile, first: r.first, len: r.last - r.first });
        runs.set(id, (anim === 1 || anim === 19) ? { tile, pid: m[3], first: s, last: s } : null);
    }
}
for (const [id, r] of runs) if (r && r.last - r.first > 0) worst.push({ id, pid: r.pid, tile: r.tile, first: r.first, len: r.last - r.first });
worst.sort((a, b) => b.len - a.len);
const bad = worst.filter(w => w.len >= 2);
console.log(`walk-in-place stretches >= 2 s: ${bad.length}` + (worst.length ? `, longest ${worst[0].len.toFixed(1)} s (id ${worst[0].id} pid ${worst[0].pid} tile ${worst[0].tile} from s=${worst[0].first})` : ''));
for (const w of bad.slice(0, 8)) console.log(`  id ${w.id} pid ${w.pid} tile ${w.tile} s=${w.first.toFixed(1)} for ${w.len.toFixed(1)} s`);
