// For every NPC hit that rolled NO stagger: was it walking right before, and did it keep walking
// (a walk-seg/chase-move continuing within 0.6 s, and no flinch react in between)?
const fs = require('fs');
const L = fs.readFileSync(process.argv[2], 'utf8').split(/\r?\n/);
const ev = [];
for (const l of L) {
    const m = l.match(/ s=([0-9.]+) \[([A-Z]+\/[a-z0-9-]+)\] (.*)$/);
    if (m) ev.push({ s: +m[1], tag: m[2], msg: m[3] });
}
const idOf = (e) => { const m = e.msg.match(/(?:^|\s)id=(\d+)/); return m ? +m[1] : null; };
let walking = 0, kept = 0, stopped = 0, standing = 0;
const samples = [];
for (let i = 0; i < ev.length; i++) {
    const e = ev[i];
    if (e.tag !== 'COMBAT/stagger-roll' || !/staggers=0/.test(e.msg)) continue;
    const id = idOf(e);
    if (id === 18000) continue;
    let walkedBefore = false;
    for (let j = i - 1; j >= 0 && ev[j].s >= e.s - 1.0; j--) {
        if ((ev[j].tag === 'FLOW/walk-seg' || ev[j].tag === 'FLOW/chase-move') && idOf(ev[j]) === id) { walkedBefore = true; break; }
    }
    if (!walkedBefore) { standing++; continue; }
    walking++;
    let flinched = false, walkedAfter = null;
    for (let j = i + 1; j < ev.length && ev[j].s <= e.s + 1.0; j++) {
        const x = ev[j];
        if (idOf(x) !== id) continue;
        if (x.tag === 'COMBAT/react' && /flinch/.test(x.msg)) flinched = true;
        if ((x.tag === 'FLOW/walk-seg' || x.tag === 'FLOW/chase-move') && walkedAfter == null) walkedAfter = x.s - e.s;
    }
    if (!flinched) kept++; else stopped++;
    if (samples.length < 6) samples.push(`s=${e.s} id=${id} ${e.msg.replace(/ t=.*$/, '')} flinched=${flinched} nextWalkSeg=${walkedAfter == null ? '-' : '+' + walkedAfter.toFixed(2)}`);
}
console.log(`NPC hits with no stagger: walking ${walking} (kept going without a flinch: ${kept}, flinched anyway: ${stopped}), standing ${standing}`);
samples.forEach(s => console.log('  ' + s));
