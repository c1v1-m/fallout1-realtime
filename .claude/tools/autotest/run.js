// Runs one or more self-played fights and reports on each (game/autotest.cc + trace/airep.js).
//
//   node run.js <scenario.txt> [runs] [--visible] [--exe <path>]
//
// Plays the dev build (build/Release/fallout-ce.exe) in a hidden window from build/Release, waits
// for it to quit, picks up the trace it wrote and prints the AI report. With runs > 1 the same
// scenario is played that many times (seed + i) and a summary line per run closes the output.
// The traces stay in build/Release/autotest_traces/<scenario>_<n>.log.
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const args = process.argv.slice(2);
const scenario = args[0];
if (!scenario) {
    console.error('usage: node run.js <scenario.txt> [runs] [--visible] [--exe <path>]');
    process.exit(1);
}
const runs = Number(args[1]) > 0 ? Number(args[1]) : 1;
const visible = args.includes('--visible');
const exeIdx = args.indexOf('--exe');
const exe = exeIdx >= 0 ? path.resolve(args[exeIdx + 1]) : path.join(ROOT, 'build', 'Release', 'fallout-ce.exe');
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

const traces = () => new Set(fs.readdirSync(cwd).filter(f => /^trace_.*\.log$/.test(f)));
const summaries = [];

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
        console.log(`run ${i + 1}: no trace written (exit ${r.status}, signal ${r.signal}, ${secs} s)`);
        continue;
    }
    const dest = path.join(outDir, `${name}_${i + 1}.log`);
    fs.copyFileSync(path.join(cwd, fresh[fresh.length - 1]), dest);
    const text = fs.readFileSync(dest, 'latin1');
    const end = /\[FLOW\/autotest\] end why=(\S+) elapsed=([\d.]+) dudeHp=(-?\d+)/.exec(text);
    const started = /\[FLOW\/autotest\] start/.test(text);
    console.log(`\n######## run ${i + 1}/${runs}  ${name}  exit=${r.status}  ${secs} s wall  ${started ? '' : 'NEVER STARTED  '}${end ? `end=${end[1]} after ${end[2]} s, dude HP ${end[3]}` : 'NO END LINE'}`);
    for (const w of text.split(/\r?\n/).filter(l => /\[(WARN|ERR)\/autotest\]/.test(l))) console.log('   SCENARIO PROBLEM: ' + w.replace(/^\d+ /, ''));
    const rep = spawnSync('node', [path.join(ROOT, '.claude', 'tools', 'trace', 'airep.js'), dest], { encoding: 'utf8' });
    console.log(rep.stdout);
    const json = spawnSync('node', [path.join(ROOT, '.claude', 'tools', 'trace', 'airep.js'), dest, '--json'], { encoding: 'utf8' });
    try {
        const j = JSON.parse(json.stdout);
        const f = j.fights;
        const bad = j.issues.filter(x => x.sev >= 2).length;
        summaries.push(`run ${i + 1}: end=${end ? end[1] : '?'} hp=${end ? end[3] : '?'} fights=${f.length} issues=${j.issues.length} (bad ${bad}) good=${j.goods.length}`);
    } catch (e) {
        summaries.push(`run ${i + 1}: report failed`);
    }
}
if (runs > 1) {
    console.log('\n######## summary');
    for (const s of summaries) console.log(s);
}
