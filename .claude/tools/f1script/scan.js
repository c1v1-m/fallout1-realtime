// Crude scan of a compiled Fallout .INT: which engine opcodes it references + its string table.
const fs = require('fs');
const ops = new Map();
for (const line of fs.readFileSync(__dirname + '/opcodes.txt', 'utf8').split(/\r?\n/)) {
    const m = line.trim().match(/^(0x[0-9A-Fa-f]+)\s+(\S+)$/);
    if (m) ops.set(parseInt(m[1], 16), m[2]);
}

for (const file of process.argv.slice(2)) {
    const b = fs.readFileSync(file);
    const seen = new Map();
    for (let i = 0; i + 1 < b.length; i += 2) {
        const w = b.readUInt16BE(i);
        if (w >= 0x8000 && ops.has(w)) seen.set(ops.get(w), (seen.get(ops.get(w)) || 0) + 1);
    }
    console.log('==== ' + file + ' ====');
    console.log([...seen.entries()].sort((a, b2) => b2[1] - a[1]).map(([k, v]) => k + '(' + v + ')').join('  '));
    const strs = b.toString('latin1').match(/[ -~]{4,}/g) || [];
    console.log('--- strings: ' + strs.join(' | '));
    console.log();
}
