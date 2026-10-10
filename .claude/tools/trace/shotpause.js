// For every DUDE attack on a critter that was WALKING (a walk-seg / chase-move in the 0.6 s before the
// shot): when did it walk again, what happened to it in between, and was it hit.
const fs = require('fs');
const L = fs.readFileSync(process.argv[2], 'utf8').split(/\r?\n/);
const ev = [];
for (const l of L) {
    const m = l.match(/ s=([0-9.]+) \[([A-Z]+\/[a-z0-9-]+)\] (.*)$/);
    if (m) ev.push({ s: +m[1], tag: m[2], msg: m[3], raw: l });
}
const idOf = (e) => { const m = e.msg.match(/(?:^|\s)id=(\d+)/); return m ? +m[1] : null; };
const rows = [];
for (let i = 0; i < ev.length; i++) {
    const e = ev[i];
    if (e.tag !== 'COMBAT/attack' || !e.msg.startsWith('DUDE')) continue;
    const did = +(e.msg.match(/did=(\d+)/) || [])[1];
    // walking before?
    let walkedAt = null;
    for (let j = i - 1; j >= 0 && ev[j].s >= e.s - 0.6; j--) {
        if ((ev[j].tag === 'FLOW/walk-seg' || ev[j].tag === 'FLOW/chase-move') && idOf(ev[j]) === did) { walkedAt = ev[j].s; break; }
    }
    if (walkedAt == null) continue;
    let nextWalk = null, notes = [];
    for (let j = i + 1; j < ev.length && ev[j].s <= e.s + 4; j++) {
        const x = ev[j];
        if (idOf(x) === did || (x.msg.includes('did=' + did) && x.tag !== 'COMBAT/attack')) {
            if (x.tag === 'FLOW/walk-seg' || x.tag === 'FLOW/chase-move') { nextWalk = x.s; break; }
            if (/COMBAT\/(react|react-start|react-refuse|hit|damage)|FLOW\/(chase-why)/.test(x.tag)) notes.push((x.s - e.s).toFixed(2) + ' ' + x.tag.split('/')[1] + ' ' + x.msg.replace(/ t=[0-9.]+$/, '').slice(0, 70));
        }
        if (x.tag === 'COMBAT/resolve' && x.msg.startsWith('DUDE')) notes.push((x.s - e.s).toFixed(2) + ' resolve ' + x.msg.slice(5, 60));
    }
    rows.push({ s: e.s, did, pause: nextWalk ? (nextWalk - e.s).toFixed(2) : '>4', notes });
}
for (const r of rows.slice(0, +(process.argv[3] || 15))) {
    console.log(`s=${r.s} target id=${r.did}  walked again after ${r.pause}s`);
    for (const n of r.notes.slice(0, 8)) console.log('     ' + n);
}
const ps = rows.filter(r => r.pause !== '>4').map(r => +r.pause).sort((a, b) => a - b);
console.log(`\n${rows.length} shots at a walking target; pause before it walked again: median ${ps[Math.floor(ps.length / 2)]}s, ` +
    `p25 ${ps[Math.floor(ps.length / 4)]}s, p75 ${ps[Math.floor(ps.length * 3 / 4)]}s, never within 4 s: ${rows.length - ps.length}`);
