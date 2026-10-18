// Rockets at the dude in a trace: hit / outran the blast / missed on the roll.
// usage: node dodgecount.js <trace.log> [...]
const fs = require('fs');
for (const file of process.argv.slice(2)) {
    const lines = fs.readFileSync(file, 'latin1').split(/\r?\n/);
    let fired = 0, hit = 0, cleared = 0, missed = 0, dmg = 0;
    for (const l of lines) {
        if (/\[COMBAT\/attack\] NPC .*did=18000/.test(l)) fired++;
        const h = /\[COMBAT\/hit\] att=(\d+) .* def=18000 .* hit=(\d) crit=\d dmg=(\d+)/.exec(l);
        if (h && h[1] !== '18000') {
            if (h[2] === '1' && +h[3] > 0) { hit++; dmg += +h[3]; } else missed++;
        }
        if (/blast-clear\] who=defender id=18000/.test(l) || /proj-dodged\] id=18000 /.test(l)) cleared++;
    }
    console.log(`${file.split(/[\/]/).pop()}: fired ${fired}  hit ${hit} (dmg ${dmg})  outran ${cleared}  missed-roll ${missed - cleared < 0 ? 0 : missed - cleared}`);
}
