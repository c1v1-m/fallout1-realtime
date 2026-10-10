// For every LT lock-on in a trace: where the dude stood, which way he last steered, and every live
// critter with its distance, side (angle off the facing) and hostility -- to see what the pick chose.
const fs = require('fs');
const lines = fs.readFileSync(process.argv[2], 'utf8').split('\n');
const tS = (l) => { const m = l.match(/ s=([0-9.]+) /); return m ? parseFloat(m[1]) : null; };
const scr = (tile) => { const v3 = 199 - (tile % 200), v4 = Math.floor(tile / 200); return [24 * v3 + 16 * v4, -6 * v3 + 12 * v4]; };
const names = { 16777266: 'MASTER', 16777260: 'mutant(260)', 16777293: 'nightkin?(293)', 16777409: '409', 16777241: '241', 16777513: '513', 16777408: '408' };

let dudeTile = -1, stickAng = null, stickAt = 0, snap = [], snapS = -1, cur = [], curS = -1;
for (const l of lines) {
    const s = tS(l);
    let m;
    if ((m = l.match(/\[FLOW\/dude-tile\] from=-?\d+ to=(\d+)/))) dudeTile = +m[1];
    if ((m = l.match(/\[INPUT\/stick\] ang=(\d+) dir=(\d)/))) { stickAng = +m[1]; stickAt = s; }
    if (l.includes('[STATE/critter]')) {
        if (s !== curS) { if (cur.length) { snap = cur; snapS = curS; } cur = []; curS = s; }
        const g = (k) => { const r = l.match(new RegExp(' ' + k + '=(-?\\d+)')); return r ? +r[1] : null; };
        if (g('dead') === 0) cur.push({ id: g('id'), pid: g('pid'), tile: g('tile'), host: g('host') });
    }
    if ((m = l.match(/\[ACTION\/lock-on\] target=(\d+)/))) {
        const list = (curS === s || cur.length > snap.length) ? cur : snap;
        const [dx0, dy0] = scr(dudeTile);
        const fa = stickAng;
        console.log(`\n== s=${s} LOCK -> ${names[m[1]] || m[1]}   dude tile=${dudeTile}  last stick ang=${fa} (${(s - stickAt).toFixed(1)}s ago)`);
        const rows = list.map(c => {
            const [x, y] = scr(c.tile);
            const ex = x - dx0, ey = -(y - dy0); // screen up = +
            const dist = Math.round(Math.hypot(ex, ey) / 28);
            let ang = Math.round(Math.atan2(ey, ex) * 180 / Math.PI); if (ang < 0) ang += 360;
            let off = fa == null ? null : Math.abs(((ang - fa + 540) % 360) - 180);
            return { ...c, dist, ang, off };
        }).filter(r => r.dist <= 25).sort((a, b) => a.dist - b.dist);
        for (const r of rows) console.log(`   id=${String(r.id).padEnd(5)} ${(names[r.pid] || r.pid).toString().padEnd(15)} dist~${String(r.dist).padEnd(3)} off=${String(r.off).padEnd(4)}° host=${r.host}`);
    }
}
