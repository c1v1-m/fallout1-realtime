// Linear disassembler for a compiled Fallout .INT.
// Layout per src/int/intrpret.cc:457-460 ; big-endian per fetchWord/fetchLong.
const fs = require('fs');

const ops = new Map();
for (const line of fs.readFileSync(__dirname + '/opcodes.txt', 'utf8').split(/\r?\n/)) {
    const m = line.trim().match(/^(0x[0-9A-Fa-f]+)\s+(\S+)$/);
    if (m) ops.set(parseInt(m[1], 16) & 0x3FF, m[2].replace(/^OPCODE_/, '').toLowerCase());
}

const b = fs.readFileSync(process.argv[2]);
const L = (o) => b.readInt32BE(o);
const procBase = 42;
const procCount = L(procBase);
const identBase = procBase + 4 + 24 * procCount;
const strBase = identBase + L(identBase) + 4;
const sLen = L(strBase); const codeBase = strBase + 4 + (sLen === -1 ? 0 : sLen) + 4;

const name = (off) => {
    let e = identBase + off;
    let s = e; while (b[e]) e++;
    return b.toString('latin1', s, e);
};
const str = (off) => {
    let s = strBase + 4 + off, e = s; while (b[e]) e++;
    return b.toString('latin1', s, e);
};

const procs = [];
for (let i = 0; i < procCount; i++) {
    const o = procBase + 4 + 24 * i;
    procs.push({ name: name(L(o)), flags: L(o + 4), delay: L(o + 8), cond: L(o + 12), body: L(o + 16), argc: L(o + 20) });
}
console.log('# procCount=' + procCount + ' ident@' + identBase + ' str@' + strBase + ' code@' + codeBase + ' size=' + b.length);
procs.forEach((p, i) => console.log('# proc[' + i + '] ' + p.name + ' body=' + p.body + ' cond=' + p.cond + ' argc=' + p.argc + ' flags=0x' + p.flags.toString(16)));

const labels = new Map();
procs.forEach(p => { if (p.body) labels.set(p.body, p.name); });

const want = process.argv[3] ? procs.find(p => p.name.toLowerCase() === process.argv[3].toLowerCase()) : null;
let ip = want ? want.body : codeBase;
const end = want ? (procs.filter(p => p.body > want.body).reduce((m, p) => Math.min(m, p.body), b.length)) : b.length;

const out = [];
while (ip + 1 < end) {
    const at = ip;
    const w = b.readUInt16BE(ip); ip += 2;
    const idx = w & 0x3FF;
    let txt = ops.get(idx) || ('op_' + w.toString(16));
    if (idx === 0x001) { // push: 4-byte operand, type in high bits
        const v = L(ip); ip += 4;
        if (w === 0xC001) txt = 'push ' + v;
        else if (w === 0x9001) txt = 'push_str "' + str(v) + '"';
        else if (w === 0x9801) txt = 'push_dstr @' + v;
        else if (w === 0xE001) txt = 'push_ptr ' + v + (labels.has(v) ? ' (' + labels.get(v) + ')' : '');
        else txt = 'push[' + w.toString(16) + '] ' + v;
    }
    out.push((labels.has(at) ? '\n' + labels.get(at) + ':\n' : '') + '  ' + at + ': ' + txt);
}
console.log(out.join('\n'));
