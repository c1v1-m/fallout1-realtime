// What every fighter WANTED, from the [FLOW/intent] lines (combat_rt.cc rt_intent_update, 2026-10-18).
//
//   node intents.js <trace.log>            per critter: share of time in each intent, and the flaps
//   node intents.js <trace.log> <id>       one critter's timeline, line by line
//
// Traces are CRLF; lines are split on /\r?\n/.
const fs = require('fs');

const file = process.argv[2];
const only = process.argv[3] ? Number(process.argv[3]) : null;
if (!file) {
    console.error('usage: node intents.js <trace.log> [id]');
    process.exit(1);
}

const re = /s=([\d.]+) \[FLOW\/intent\] id=(\d+) pid=(\d+) (\w+)->(\w+) why=(\S*) was=(\S*) held=([\d.]+) tgt=(\d+) dist=(-?\d+) ap=(-?\d+)/;
const actors = new Map();
let lastS = 0;

for (const line of fs.readFileSync(file, 'latin1').split(/\r?\n/)) {
    const sm = /\bs=([\d.]+)/.exec(line);
    if (sm) lastS = Number(sm[1]);
    const m = re.exec(line);
    if (!m) continue;
    const [, s, id, pid, from, to, why, was, , tgt, dist, ap] = m;
    const t = Number(s);
    const key = Number(id);
    if (only !== null && key !== only) continue;
    if (only !== null) {
        console.log(`${t.toFixed(2).padStart(8)}  ${from.padEnd(10)} -> ${to.padEnd(10)} why=${why.padEnd(16)} (was ${was || '-'})  tgt=${tgt} dist=${dist} ap=${ap}`);
    }
    let a = actors.get(key);
    if (!a) {
        a = { pid, cur: null, since: t, time: {}, changes: 0, flaps: 0, last: [] };
        actors.set(key, a);
    }
    if (a.cur !== null && a.cur !== to) {
        const label = a.cur;
        a.time[label] = (a.time[label] || 0) + (t - a.since);
        a.since = t;
        a.changes++;
        // a flap: back to the intent it left less than half a second ago
        a.last.push({ t, to });
        if (a.last.length > 2) a.last.shift();
        if (a.last.length === 2 && a.last[0].to !== to && t - a.last[0].t < 0.5) a.flaps++;
    } else if (a.cur === null) {
        a.since = t;
    }
    a.cur = to;
}

if (only !== null) process.exit(0);

for (const [id, a] of actors) {
    if (a.cur !== null) a.time[a.cur] = (a.time[a.cur] || 0) + Math.max(0, lastS - a.since);
    const total = Object.values(a.time).reduce((x, y) => x + y, 0) || 1;
    const parts = Object.entries(a.time)
        .filter(([k]) => k !== 'dead' && k !== 'none')
        .sort((x, y) => y[1] - x[1])
        .map(([k, v]) => `${k} ${(100 * v / total).toFixed(0)}%`)
        .join('  ');
    console.log(`id=${id} pid=${a.pid}  changes=${a.changes} flaps=${a.flaps}  ${parts}`);
}
