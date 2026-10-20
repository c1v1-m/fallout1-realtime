// Runs one or more self-played scenarios and reports on each (game/autotest.cc + trace/airep.js).
//
//   node run.js <scenario.txt> [runs] [--visible] [--exe <path>] [--show <regex>]
//
// Plays the dev build (build/Release/fallout-ce.exe) in a hidden window from build/Release, waits
// for it to quit, picks up the trace it wrote and prints the AI report. With runs > 1 the same
// scenario is played that many times (seed + i) and a summary line per run closes the output.
// The traces stay in build/Release/autotest_traces/<scenario>_<n>.log.
//
// A SCREEN scenario (one with pad / tap / axis / open / note timeline lines) has no fight to report:
// it prints the timeline and what the screens traced instead (FLOW/autotest, INPUT/ce-*, btn, key,
// inject, WARN, ERR -- plus --show <regex>). Any scenario can carry checks, read here and ignored by
// the game:
//
//   expect <regex>                 some trace line matches
//   expect-not <regex>             no trace line matches
//   expect-count [>=|<=]<n> <regex>   that many lines match (exactly n without a sign)
//
// A failed check makes the exit code 1.
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const args = process.argv.slice(2);
const scenario = args[0];
if (!scenario) {
    console.error('usage: node run.js <scenario.txt> [runs] [--visible] [--exe <path>] [--show <regex>]');
    process.exit(1);
}
const runs = Number(args[1]) > 0 ? Number(args[1]) : 1;
const visible = args.includes('--visible');
const exeIdx = args.indexOf('--exe');
const exe = exeIdx >= 0 ? path.resolve(args[exeIdx + 1]) : path.join(ROOT, 'build', 'Release', 'fallout-ce.exe');
const showIdx = args.indexOf('--show');
const show = showIdx >= 0 ? new RegExp(args[showIdx + 1]) : null;
const cwd = path.dirname(exe);
const outDir = path.join(cwd, 'autotest_traces');
fs.mkdirSync(outDir, { recursive: true });

const scenPath = path.resolve(scenario);
const scenText = fs.readFileSync(scenPath, 'utf8');
const durMatch = /^duration\s+(\d+)/m.exec(scenText);
const duration = durMatch ? Number(durMatch[1]) : 60;
const seedMatch = /^seed\s+(\d+)/m.exec(scenText);
const baseSeed = seedMatch ? Number(seedMatch[1]) : 1;
const name = path.basename(scenPath, path.extname(scenPath));
const screen = /^\s*(pad|tap|axis|open|note)\s/m.test(scenText);

// The checks. `#` starts a comment, as in the game's own parser.
const expects = [];
for (const raw of scenText.split(/\r?\n/)) {
    const line = raw.replace(/#.*$/, '').trim();
    const m = /^expect(-not|-count)?\s+(.*)$/.exec(line);
    if (!m) continue;
    if (m[1] === '-count') {
        const c = /^(>=|<=)?(\d+)\s+(.*)$/.exec(m[2]);
        if (!c) {
            console.log(`bad check (expect-count <n> <regex>): ${line}`);
            continue;
        }
        expects.push({ kind: 'count', op: c[1] || '=', n: Number(c[2]), re: new RegExp(c[3]), src: line });
    } else {
        expects.push({ kind: m[1] === '-not' ? 'not' : 'any', re: new RegExp(m[2]), src: line });
    }
}

// What a screen run prints of its trace.
const DIGEST = /\[(FLOW\/autotest|INPUT\/ce-|INPUT\/btn\]|INPUT\/key\]|INPUT\/inject\]|WARN\/|ERR\/)/;

function checkExpects(lines) {
    let passed = 0;
    for (const e of expects) {
        const hits = lines.filter(l => e.re.test(l)).length;
        let ok;
        if (e.kind === 'any') ok = hits > 0;
        else if (e.kind === 'not') ok = hits === 0;
        else ok = e.op === '>=' ? hits >= e.n : e.op === '<=' ? hits <= e.n : hits === e.n;
        if (ok) passed++;
        console.log(`   ${ok ? 'PASS' : 'FAIL'}  ${e.src}   (${hits} line${hits === 1 ? '' : 's'})`);
    }
    return passed;
}

const traces = () => new Set(fs.readdirSync(cwd).filter(f => /^trace_.*\.log$/.test(f)));
const summaries = [];
let failed = false;

for (let i = 0; i < runs; i++) {
    // A per-run copy with its own seed, so repeated runs are not the same dice.
    let runScen = scenPath;
    if (runs > 1) {
        runScen = path.join(outDir, `${name}_${i + 1}.scenario.txt`);
        const text = /^seed\s+\d+/m.test(scenText)
            ? scenText.replace(/^seed\s+\d+/m, `seed ${baseSeed + i}`)
            : `seed ${baseSeed + i}\n` + scenText;
        fs.writeFileSync(runScen, text);
    }
    const before = traces();
    const t0 = Date.now();
    const exeArgs = ['--autotest', runScen];
    if (visible) exeArgs.push('--autotest-visible');
    const r = spawnSync(exe, exeArgs, { cwd, timeout: (duration + 90) * 1000, stdio: 'ignore' });
    const secs = ((Date.now() - t0) / 1000).toFixed(0);
    const fresh = [...traces()].filter(f => !before.has(f)).sort();
    if (fresh.length === 0) {
        console.log(`run ${i + 1}: no trace written (exit ${r.status}, signal ${r.signal}, ${secs} s) -- another fallout-ce running? (one instance at a time)`);
        failed = true;
        continue;
    }
    const dest = path.join(outDir, `${name}_${i + 1}.log`);
    fs.copyFileSync(path.join(cwd, fresh[fresh.length - 1]), dest);
    const text = fs.readFileSync(dest, 'latin1');
    const lines = text.split(/\r?\n/);
    const end = /\[FLOW\/autotest\] end why=(\S+) elapsed=([\d.]+) dudeHp=(-?\d+)/.exec(text);
    const started = /\[FLOW\/autotest\] start/.test(text);
    console.log(`\n######## run ${i + 1}/${runs}  ${name}  exit=${r.status}  ${secs} s wall  ${started ? '' : 'NEVER STARTED  '}${end ? `end=${end[1]} after ${end[2]} s, dude HP ${end[3]}` : 'NO END LINE'}`);
    for (const w of lines.filter(l => /\[(WARN|ERR)\/autotest\]/.test(l))) console.log('   SCENARIO PROBLEM: ' + w.replace(/^\d+ /, ''));

    if (screen) {
        for (const l of lines) {
            if (DIGEST.test(l) || (show && show.test(l))) console.log('   ' + l.replace(/^\d+ /, ''));
        }
    } else {
        const rep = spawnSync('node', [path.join(ROOT, '.claude', 'tools', 'trace', 'airep.js'), dest], { encoding: 'utf8' });
        console.log(rep.stdout);
        if (show) for (const l of lines.filter(x => show.test(x))) console.log('   ' + l.replace(/^\d+ /, ''));
    }

    let checks = '';
    if (expects.length > 0) {
        console.log(`\n   checks (${dest}):`);
        const passed = checkExpects(lines);
        checks = ` checks ${passed}/${expects.length}`;
        if (passed < expects.length) failed = true;
    }

    if (screen) {
        summaries.push(`run ${i + 1}: end=${end ? end[1] : '?'} after ${end ? end[2] : '?'} s${checks}`);
        continue;
    }
    const json = spawnSync('node', [path.join(ROOT, '.claude', 'tools', 'trace', 'airep.js'), dest, '--json'], { encoding: 'utf8' });
    try {
        const j = JSON.parse(json.stdout);
        const f = j.fights;
        const bad = j.issues.filter(x => x.sev >= 2).length;
        summaries.push(`run ${i + 1}: end=${end ? end[1] : '?'} hp=${end ? end[3] : '?'} fights=${f.length} issues=${j.issues.length} (bad ${bad}) good=${j.goods.length}${checks}`);
    } catch (e) {
        summaries.push(`run ${i + 1}: report failed${checks}`);
    }
}
if (runs > 1 || expects.length > 0) {
    console.log('\n######## summary');
    for (const s of summaries) console.log(s);
}
if (failed) process.exitCode = 1;
