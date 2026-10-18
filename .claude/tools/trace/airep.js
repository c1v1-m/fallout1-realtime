// AI REPORT: what the enemies did well and badly in a trace, without anyone having to describe it
// (2026-10-18, "potřebuji abys to pak dokázal zjistit sám, co je na tom špatně i dobře, při testu").
//
//   node airep.js <trace.log>            the report
//   node airep.js <trace.log> --json     the same numbers as JSON (for before/after comparisons)
//
// Reads: combat-enter/exit, STATE/critter snapshots (once a second), STATE/hb (dude HP),
// COMBAT/attack, COMBAT/hit (2026-10-18+), FLOW/intent (2026-10-18+), and the AI's own event lines
// (disengage, search-*, unreachable, oscillate, defend-hold, squad-hold, ...). Older traces without
// hit/intent lines still get a report; those columns read "n/a".
// Traces are CRLF; lines are split on /\r?\n/.
const fs = require('fs');

const file = process.argv[2];
const asJson = process.argv.includes('--json');
if (!file) {
    console.error('usage: node airep.js <trace.log> [--json]');
    process.exit(1);
}

const DUDE = 18000;
const W = 200; // hex grid width

// Fallout's hex grid is "even-q": even columns sit half a hex lower (tile.cc dir_tile).
function cube(tile) {
    const x = tile % W, y = Math.floor(tile / W);
    return [x, y - (x + (x & 1)) / 2];
}
function tileDist(a, b) {
    if (a < 0 || b < 0) return -1;
    const [q1, r1] = cube(a), [q2, r2] = cube(b);
    const dq = q2 - q1, dr = r2 - r1;
    return (Math.abs(dq) + Math.abs(dr) + Math.abs(dq + dr)) / 2;
}
function kv(rest) {
    const o = {};
    for (const m of rest.matchAll(/(\w+)=(\S+)/g)) o[m[1]] = m[2];
    return o;
}
const num = v => (v === undefined ? NaN : Number(v));
const fmt = (v, d = 1) => (Number.isFinite(v) ? v.toFixed(d) : 'n/a');
const pct = (a, b) => (b > 0 ? Math.round((100 * a) / b) + '%' : 'n/a');

// ---------------------------------------------------------------- parse
const head = /^\d+ s=([\d.]+) \[(\w+)\/([\w-]+)\] ?(.*)$/;
const ev = [];          // every relevant line: {s, cat, sub, rest, f}
const names = new Map(); // pid -> name (from the balance rows)
const snaps = [];       // {s, dudeTile, npcs: Map(id -> fields)}
const hb = [];          // {s, hp}
let cur = null;
let hasHit = false, hasIntent = false;
let lastS = 0;

for (const line of fs.readFileSync(file, 'latin1').split(/\r?\n/)) {
    const m = head.exec(line);
    if (!m) continue;
    const s = Number(m[1]);
    lastS = s;
    const cat = m[2], sub = m[3], rest = m[4];
    if (cat === 'STATE' && sub === 'critter') {
        if (!cur || cur.s !== s) {
            cur = { s, dudeTile: -1, npcs: new Map() };
            snaps.push(cur);
        }
        const f = kv(rest);
        if (rest.startsWith('DUDE')) cur.dudeTile = num(f.tile);
        else cur.npcs.set(num(f.id), f);
        continue;
    }
    if (cat === 'STATE' && sub === 'hb') {
        hb.push({ s, hp: num(kv(rest).hp) });
        continue;
    }
    if (cat === 'COMBAT' && sub === 'balance') {
        const b = /NPC pid=(\d+) (.+?) str=/.exec(rest);
        if (b) names.set(Number(b[1]), b[2]);
        continue;
    }
    if (cat === 'COMBAT' && sub === 'hit') hasHit = true;
    if (cat === 'FLOW' && sub === 'intent') hasIntent = true;
    ev.push({ s, cat, sub, rest, f: kv(rest) });
}

// ---------------------------------------------------------------- engagements
// A fight = combat-enter .. combat-exit. Exits followed by an enter within FLICKER_S are one
// engagement (the flicker itself is reported).
const FLICKER_S = 1.5;
const windows = [];
let open = null;
const flickers = [];
for (const e of ev) {
    if (e.cat !== 'FLOW') continue;
    if (e.sub === 'combat-enter') {
        if (open === null) open = e.s;
    } else if (e.sub === 'combat-exit' && open !== null) {
        windows.push([open, e.s]);
        open = null;
    }
}
if (open !== null) windows.push([open, lastS]);
const fights = [];
for (const w of windows) {
    const prev = fights[fights.length - 1];
    if (prev && w[0] - prev.end < FLICKER_S) {
        flickers.push({ s: prev.end, gap: w[0] - prev.end });
        prev.end = w[1];
        prev.flickers++;
    } else {
        fights.push({ start: w[0], end: w[1], flickers: 0 });
    }
}

const inFight = s => fights.find(f => s >= f.start - 0.05 && s <= f.end + 0.05);
const issues = [];
const goods = [];
function issue(sev, s, id, what) { issues.push({ sev, s, id, what }); }
function good(s, id, what) { goods.push({ s, id, what }); }
const label = id => {
    for (const sn of snaps) {
        const f = sn.npcs.get(id);
        if (f) {
            const pid = num(f.pid);
            return `${id}(${names.get(pid) || 'pid ' + pid})`;
        }
    }
    return String(id);
};

// ---------------------------------------------------------------- per fight
const BUSY_INTENTS = new Set(['hold', 'reload', 'heal', 'script', 'down', 'dodge', 'dead', 'flee', 'reposition']);
const report = { trace: file, hasHit, hasIntent, fights: [] };

for (const fight of fights) {
    const fs_ = fight.start, fe = fight.end;
    const within = e => e.s >= fs_ - 0.05 && e.s <= fe + 0.05;
    const fev = ev.filter(within);
    const fsnaps = snaps.filter(sn => sn.s >= fs_ && sn.s <= fe);
    const npcs = new Map();
    const get = id => {
        if (!npcs.has(id)) {
            npcs.set(id, {
                id, joinAt: Infinity, firstAttack: Infinity, attacks: 0, hits: 0, shots: 0, dmg: 0,
                ff: 0, died: null, st: [0, 0, 0], intents: {}, intentFrom: null, intentAt: 0,
                atkDist: [], frozenRun: 0, idleNearRun: 0,
            });
        }
        return npcs.get(id);
    };

    // participants from snapshots
    for (const sn of fsnaps) {
        for (const [id, f] of sn.npcs) {
            const st = num(f.st);
            const dead = f.dead === '1';
            if (st !== 0 && !dead) {
                const n = get(id);
                n.joinAt = Math.min(n.joinAt, sn.s);
            }
        }
    }
    const dude = { attacks: 0, hits: 0, shots: 0, dmg: 0, kills: 0, taken: 0 };
    for (const e of fev) {
        const f = e.f;
        if (e.cat === 'COMBAT' && e.sub === 'attack') {
            if (e.rest.startsWith('NPC')) {
                const n = get(num(f.aid));
                n.attacks++;
                n.firstAttack = Math.min(n.firstAttack, e.s);
                n.joinAt = Math.min(n.joinAt, e.s);
            } else if (e.rest.startsWith('DUDE')) {
                dude.attacks++;
                const did = num(f.did);
                if (did !== DUDE) get(did);
            }
        } else if (e.cat === 'COMBAT' && e.sub === 'hit') {
            const att = num(f.att), def = num(f.def), hit = f.hit === '1', dmg = num(f.dmg) || 0;
            if (att === DUDE) {
                dude.shots++;
                if (hit) { dude.hits++; dude.dmg += dmg; }
            } else if (att >= 0) {
                const n = get(att);
                if (def === DUDE) {
                    n.shots++;
                    if (hit) { n.hits++; n.dmg += dmg; dude.taken += dmg; }
                    n.atkDist.push(num(f.dist));
                } else if (hit && dmg > 0) {
                    // a hostile hurting someone on its own side
                    const lastSnap = fsnaps.find(sn => sn.s >= e.s) || fsnaps[fsnaps.length - 1];
                    const vf = lastSnap ? lastSnap.npcs.get(def) : null;
                    const af = lastSnap ? lastSnap.npcs.get(att) : null;
                    // Same team when the trace says (team= since 2026-10-18), else both hostile to the dude.
                    const sameSide = vf && af && (vf.team !== undefined && af.team !== undefined
                        ? vf.team === af.team : vf.host === '1' && af.host === '1');
                    if (sameSide) {
                        n.ff++;
                        issue(2, e.s, att, `friendly fire: hit its ally ${label(def)} for ${dmg}`);
                    }
                }
            }
        } else if (e.cat === 'COMBAT' && e.sub === 'react' && /[a-z]+->dying/.test(e.rest)) {
            const id = num(f.id);
            if (id === DUDE) {
                if (fight.dudeDied === undefined) fight.dudeDied = e.s;
                continue;
            }
            const n = get(id);
            if (n.died === null) { n.died = e.s; dude.kills++; }
        } else if (e.cat === 'FLOW' && e.sub === 'guard-join') {
            const n = get(num(f.id));
            n.joinAt = Math.min(n.joinAt, e.s);
        }
    }

    // intents (time-weighted, clipped to the fight)
    if (hasIntent) {
        const all = ev.filter(e => e.cat === 'FLOW' && e.sub === 'intent');
        const state = new Map();
        for (const e of all) {
            const im = /(\w+)->(\w+) why=(\S*)/.exec(e.rest);
            if (!im) continue;
            const id = num(e.f.id);
            const prev = state.get(id);
            if (prev && npcs.has(id)) {
                const a = Math.max(prev.s, fs_), b = Math.min(e.s, fe);
                if (b > a) {
                    const n = npcs.get(id);
                    n.intents[prev.to] = (n.intents[prev.to] || 0) + (b - a);
                }
            }
            state.set(id, { s: e.s, to: im[2], why: im[3] });
        }
        for (const [id, p] of state) {
            if (!npcs.has(id)) continue;
            const a = Math.max(p.s, fs_), b = fe;
            if (b > a) {
                const n = npcs.get(id);
                n.intents[p.to] = (n.intents[p.to] || 0) + (b - a);
            }
        }
    }

    // snapshot detectors
    // FROZEN: a chaser that has not moved for 3 s, or an attacker that has not attacked for 6 s (a
    // pistol raider's normal cadence is one shot every 4-5 s), neither of them deliberately busy.
    const FROZEN_CHASE_S = 3, FROZEN_ATTACK_S = 6, IDLE_NEAR_TILES = 8, IDLE_NEAR_S = 3;
    let prevSnap = null;
    for (const sn of fsnaps) {
        for (const [id, f] of sn.npcs) {
            const n = npcs.get(id);
            const st = num(f.st), dead = f.dead === '1', prone = f.prone === '1';
            if (n && !dead) n.st[st] = (n.st[st] || 0) + 1;
            // FROZEN: engaged, not moving, not attacking, not deliberately busy
            // A change of state starts the count again (attacking from a spot, then chasing from it,
            // is not three seconds of standing about).
            if (n && n.lastSt !== st) {
                n.frozenRun = 0;
                n.lastSt = st;
            }
            if (n && !dead && !prone && (st === 1 || st === 2) && prevSnap) {
                const pf = prevSnap.npcs.get(id);
                const still = st === 2 || (pf && pf.tile === f.tile);
                const attacked = ev.some(e => e.cat === 'COMBAT' && e.sub === 'attack' && e.f.aid == id
                    && e.s > prevSnap.s && e.s <= sn.s);
                const busy = f.int && BUSY_INTENTS.has(f.int);
                if (still && !attacked && !busy) {
                    n.frozenRun += sn.s - prevSnap.s;
                    if (n.frozenRun >= (st === 2 ? FROZEN_ATTACK_S : FROZEN_CHASE_S) && !n.frozenReported) {
                        n.frozenReported = true;
                        issue(3, sn.s, id, st === 2
                            ? `in range ${fmt(n.frozenRun)} s without attacking (dist=${f.dist}${f.int ? ' int=' + f.int : ''})`
                            : `chased ${fmt(n.frozenRun)} s without moving (dist=${f.dist}${f.int ? ' int=' + f.int : ''})`);
                    }
                } else {
                    n.frozenRun = 0;
                }
            }
            // IDLE NEAR: a hostile doing nothing a few tiles from the dude while the fight is on
            if (!dead && f.host === '1' && f.frnd !== '1' && st === 0 && sn.dudeTile >= 0) {
                const d = tileDist(num(f.tile), sn.dudeTile);
                const nn = get(id);
                if (d >= 0 && d <= IDLE_NEAR_TILES && prevSnap) {
                    nn.idleNearRun += sn.s - prevSnap.s;
                    if (nn.idleNearRun >= IDLE_NEAR_S && !nn.idleNearReported) {
                        nn.idleNearReported = true;
                        issue(2, sn.s, id, `hostile idle ${d} tiles from the dude for ${fmt(nn.idleNearRun)} s during the fight${f.int ? ' (int=' + f.int + ')' : ''}`);
                    }
                } else {
                    nn.idleNearRun = 0;
                }
            }
        }
        prevSnap = sn;
    }

    // dude HP
    const fhb = hb.filter(h => h.s >= fs_ && h.s <= fe);
    const hpStart = fhb.length ? fhb[0].hp : NaN;
    const hpMin = fhb.length ? Math.min(...fhb.map(h => h.hp)) : NaN;
    const hpEnd = fhb.length ? fhb[fhb.length - 1].hp : NaN;

    // arrival spread
    const firsts = [...npcs.values()].map(n => n.firstAttack).filter(Number.isFinite).sort((a, b) => a - b);
    const spread = firsts.length >= 2 ? firsts[firsts.length - 1] - firsts[0] : NaN;
    if (firsts.length >= 2 && spread <= 3) good(fs_, -1, `${firsts.length} attackers opened within ${fmt(spread)} s of each other (arrived together)`);
    if (firsts.length >= 3 && spread > 10) issue(1, fs_, -1, `attackers trickled in: first attacks spread over ${fmt(spread)} s`);
    if (fight.flickers > 0) issue(2, fs_, -1, `fight ended and restarted ${fight.flickers}x within ${FLICKER_S} s (combat flicker)`);

    const dur = fe - fs_;
    const rows = [];
    for (const n of npcs.values()) {
        if (!Number.isFinite(n.joinAt) && n.attacks === 0) continue;
        const life = (n.died ?? fe) - Math.max(n.joinAt, fs_);
        const tta = Number.isFinite(n.firstAttack) && Number.isFinite(n.joinAt) ? n.firstAttack - n.joinAt : NaN;
        const intentTotal = Object.values(n.intents).reduce((a, b) => a + b, 0);
        const intentStr = intentTotal > 0
            ? Object.entries(n.intents).filter(([k]) => k !== 'dead').sort((a, b) => b[1] - a[1])
                .map(([k, v]) => `${k} ${pct(v, intentTotal)}`).join(' ')
            : '';
        const stTotal = n.st.reduce((a, b) => a + b, 0);
        const stStr = stTotal > 0 ? `idle ${pct(n.st[0], stTotal)} chase ${pct(n.st[1], stTotal)} attack ${pct(n.st[2], stTotal)}` : '';
        const avgDist = n.atkDist.length ? n.atkDist.reduce((a, b) => a + b, 0) / n.atkDist.length : NaN;
        rows.push({
            id: n.id, name: label(n.id), life, tta, attacks: n.attacks, apm: life > 1 ? (60 * n.attacks) / life : NaN,
            hits: n.hits, shots: n.shots, dmg: n.dmg, ff: n.ff, died: n.died, intentStr, stStr, avgDist,
        });
        // per-NPC verdicts
        if (life >= 8 && n.attacks === 0 && n.died === null) issue(3, Math.max(n.joinAt, fs_), n.id, `in the fight ${fmt(life)} s and never attacked`);
        if (Number.isFinite(tta) && tta > 8) issue(1, n.firstAttack, n.id, `took ${fmt(tta)} s from joining to its first attack`);
        const appr = n.intents.approach || 0;
        if (intentTotal >= 8 && appr / intentTotal >= 0.75) issue(2, fs_, n.id, `spent ${pct(appr, intentTotal)} of its fight approaching`);
    }
    rows.sort((a, b) => b.attacks - a.attacks);
    report.fights.push({
        start: fs_, end: fe, dur, flickers: fight.flickers, hpStart, hpMin, hpEnd, spread, dude, npcs: rows,
        dudeDied: fight.dudeDied ?? null,
    });
}

// ---------------------------------------------------------------- whole-trace detectors
const byId = sub => ev.filter(e => e.cat === 'FLOW' && e.sub === sub);
const defenders = new Set(byId('defend-hold').map(e => num(e.f.id)));
for (const d of byId('disengage')) {
    const id = num(d.f.id);
    const searched = ev.some(e => e.cat === 'FLOW' && e.sub === 'search-start' && num(e.f.id) === id && Math.abs(e.s - d.s) < 0.3);
    // A runner is right not to look for him (rt_flee_step / combatai_flee_reason).
    const fleeing = ev.some(e => e.cat === 'FLOW' && num(e.f.id) === id && e.s <= d.s && d.s - e.s < 15
        && (/^flee/.test(e.sub) || (e.sub === 'intent' && /->flee /.test(e.rest))));
    if (!searched && !defenders.has(id) && !fleeing) issue(2, d.s, id, `lost the dude (disengage at ${d.f.dist} tiles) and did not search`);
}
for (const st of byId('search-start')) {
    const id = num(st.f.id);
    const end = ev.find(e => e.cat === 'FLOW' && e.sub === 'search-end' && num(e.f.id) === id && e.s > st.s);
    const until = end ? end.s : st.s + 11;
    const found = ev.find(e => e.s > st.s && e.s <= until && num(e.f.id) === id
        && ((e.sub === 'intent' && /->(approach|attack)/.test(e.rest)) || e.sub === 'chase-why'))
        || ev.find(e => e.s > st.s && e.s <= until && e.cat === 'COMBAT' && e.sub === 'attack' && num(e.f.aid) === id);
    if (found) good(found.s, id, `search found the dude after ${fmt(found.s - st.s)} s`);
    else issue(1, st.s, id, `searched and found nothing`);
}
const counts = {};
for (const e of ev) {
    const k = e.cat + '/' + e.sub;
    counts[k] = (counts[k] || 0) + 1;
}
const perId = (sub, min, sev, text) => {
    const m = new Map();
    for (const e of byId(sub)) m.set(num(e.f.id), (m.get(num(e.f.id)) || []).concat(e.s));
    for (const [id, list] of m) if (list.length >= min) issue(sev, list[0], id, `${text} x${list.length}`);
};
perId('unreachable', 3, 2, 'could not reach its target');
perId('engage-unreachable', 3, 1, 'saw the dude but had no way to him');
perId('oscillate', 1, 2, 'walked back and forth between two tiles');
for (const e of byId('squad-hold')) if (/end=/.test(e.rest)) good(e.s, num(e.f.id), `waited for its squad (${e.rest.match(/end=\w+/)[0]})`);
for (const e of ev.filter(e => e.sub === 'grenade-dodge')) good(e.s, num(e.f.id), 'jumped clear of a grenade');
for (const e of byId('reload-cover')) if (e.f.moved === '1') good(e.s, num(e.f.id), 'reloaded from cover');
const warnCount = k => counts[k] || 0;
if (warnCount('WARN/unstick-move') > 0) issue(1, 0, -1, `unstick-move fired ${warnCount('WARN/unstick-move')}x (a mover judged stuck)`);
if (warnCount('COMBAT/attack-fail') > 0) issue(2, 0, -1, `attack-fail ${warnCount('COMBAT/attack-fail')}x (attacks the engine refused)`);
if (warnCount('COMBAT/react-refuse') > 0) issue(1, 0, -1, `react-refuse ${warnCount('COMBAT/react-refuse')}x (a reaction could not play)`);
if (hasIntent) {
    // flapping: going BACK to the intent it left, 3 times inside 2 s (A->B->A->B->A). An opening run
    // of different intents (watch, approach, hold, attack) is a decision, not a flap.
    const m = new Map();
    for (const e of byId('intent')) {
        const im = /(\w+)->(\w+) why=(\S*)/.exec(e.rest);
        if (im) m.set(num(e.f.id), (m.get(num(e.f.id)) || []).concat({ s: e.s, from: im[1], to: im[2], why: im[3] }));
    }
    for (const [id, list] of m) {
        const back = [];
        for (let i = 1; i < list.length; i++) {
            if (list[i].to === list[i - 1].from) back.push(list[i]);
        }
        for (let i = 0; i + 2 < back.length; i++) {
            if (back[i + 2].s - back[i].s <= 2) {
                issue(2, back[i].s, id, `intent flapped ${back[i].from}<->${back[i].to} (${back[i].why}) 3x in ${fmt(back[i + 2].s - back[i].s)} s`);
                break;
            }
        }
    }
    // long holds
    for (const e of byId('intent')) {
        const im = /hold->(\w+) why=\S* was=(\S*) held=([\d.]+)/.exec(e.rest);
        if (im && Number(im[3]) > 6) issue(1, e.s, num(e.f.id), `held (${im[2]}) for ${im[3]} s`);
    }
}

// ---------------------------------------------------------------- output
report.issues = issues.sort((a, b) => b.sev - a.sev || a.s - b.s);
report.goods = goods.sort((a, b) => a.s - b.s);
if (asJson) {
    console.log(JSON.stringify(report, null, 1));
    process.exit(0);
}

const out = [];
out.push(`AI REPORT  ${file.split(/[\\/]/).pop()}   hit lines: ${hasHit ? 'yes' : 'NO (older build)'}   intent lines: ${hasIntent ? 'yes' : 'NO (older build)'}`);
out.push(`${report.fights.length} fight(s), ${flickers.length} flicker(s)`);
for (const f of report.fights) {
    out.push('');
    out.push(`== fight s=${fmt(f.start)}..${fmt(f.end)}  (${fmt(f.dur)} s)  dude HP ${f.hpStart}->${f.hpEnd} (min ${f.hpMin})  kills ${f.dude.kills}${f.dudeDied !== null ? '  DUDE DIED s=' + fmt(f.dudeDied) : ''}`);
    out.push(`   dude: ${f.dude.attacks} attacks` + (hasHit ? `, hit ${f.dude.hits}/${f.dude.shots} (${pct(f.dude.hits, f.dude.shots)}), dealt ${f.dude.dmg}, took ${f.dude.taken}` : ''));
    if (Number.isFinite(f.spread)) out.push(`   first-attack spread: ${fmt(f.spread)} s`);
    for (const r of f.npcs) {
        let line = `   ${r.name.padEnd(30)} in ${fmt(r.life).padStart(5)} s  atk ${String(r.attacks).padStart(2)}`;
        line += `  1st after ${fmt(r.tta).padStart(4)} s`;
        if (hasHit) line += `  hit ${r.hits}/${r.shots} dmg ${r.dmg}${Number.isFinite(r.avgDist) ? ' @' + fmt(r.avgDist) + 't' : ''}`;
        if (r.died !== null) line += `  died s=${fmt(r.died)}`;
        out.push(line);
        out.push(`      ${r.intentStr || r.stStr}`);
    }
}
const sevName = ['', 'minor', 'BAD', 'WORST'];
out.push('');
out.push(`== problems (${issues.length})`);
for (const i of report.issues) out.push(`   [${sevName[i.sev].padEnd(5)}] s=${fmt(i.s).padStart(6)}  ${i.id >= 0 ? label(i.id) + ': ' : ''}${i.what}`);
out.push('');
out.push(`== good (${goods.length})`);
for (const g of report.goods) out.push(`   s=${fmt(g.s).padStart(6)}  ${g.id >= 0 ? label(g.id) + ': ' : ''}${g.what}`);
console.log(out.join('\n'));
