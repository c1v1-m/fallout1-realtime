// What every shot's target did in the 2 s before and after the DUDE's attack.
const fs = require('fs');
const L = fs.readFileSync(process.argv[2], 'utf8').split(/\r?\n/);
const ev = [];
for (const l of L) {
    const m = l.match(/ s=([0-9.]+) \[([A-Z]+\/[a-z0-9-]+)\] (.*)$/);
    if (m) ev.push({ s: +m[1], tag: m[2], msg: m[3] });
}
const idOf = (e) => { const m = e.msg.match(/(?:^|\s)id=(\d+)/); return m ? +m[1] : null; };
const skip = new Set((process.argv[4] || '174').split(',').map(Number));
let n = 0;
for (let i = 0; i < ev.length && n < +(process.argv[3] || 10); i++) {
    const e = ev[i];
    if (e.tag !== 'COMBAT/attack' || !e.msg.startsWith('DUDE')) continue;
    const did = +(e.msg.match(/did=(\d+)/) || [])[1];
    if (skip.has(did)) continue;
    n++;
    const mine = (x) => (idOf(x) === did || x.msg.includes('aid=' + did + ' ') || x.msg.includes('did=' + did + ' ')) && x.tag !== 'STATE/critter';
    const before = [], after = [];
    for (let j = i - 1; j >= 0 && ev[j].s >= e.s - 2; j--) if (mine(ev[j])) before.unshift(ev[j]);
    for (let j = i + 1; j < ev.length && ev[j].s <= e.s + 2.5; j++) if (mine(ev[j])) after.push(ev[j]);
    const fmt = (x) => ((x.s - e.s) >= 0 ? '+' : '') + (x.s - e.s).toFixed(2) + ' ' + x.tag.split('/')[1] + ' ' + x.msg.replace(/ t=[0-9.]+$/, '').slice(0, 90);
    console.log('== shot s=' + e.s + ' at id=' + did);
    before.slice(-5).forEach(b => console.log('   ' + fmt(b)));
    console.log('   ---- SHOT');
    after.slice(0, 7).forEach(b => console.log('   ' + fmt(b)));
}
