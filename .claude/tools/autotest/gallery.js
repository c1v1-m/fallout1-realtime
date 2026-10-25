// Photograph every projectile in the game: one self-played run per weapon, frames while it flies,
// cropped and laid out in an HTML contact sheet. usage: node gallery.js <exe> <out.html> [label]
//
// Fallout 1 (defaults): node gallery.js build/Release/fallout-ce.exe build/review/projectiles-f1.html
// Fallout 2 (screenshots land next to the exe, the raider cannot hold rifles, 853x480 framing differs):
//   GAL_F2=1 GAL_SHOTS_ROOT=1 GAL_TAG=f2 GAL_ARMOR=17 GAL_TARGET=16777222 GAL_TARGET_TILE=22500
//   GAL_SHOOTER=16777254 GAL_SHOOTER_EXTRA="stat AG 10" GAL_SHOOTER_NAME="a Big Gun Thug"
//   GAL_CROP="350 70 400 280" node gallery.js <fallout2-ce.exe> <out.html> "Fallout 2 Realtime"
// (Big Gun Thug's 5 AP cannot pay the Gatling Laser's 6 -- hence AG 10. A target that shoots back steals
// the frames from rockets and plasma, so the target is a melee radscorpion.)
const fs = require('fs');
const path = require('path');
const { execFileSync, spawnSync } = require('child_process');
const exe = path.resolve(process.argv[2]);
const outHtml = process.argv[3];
const label = process.argv[4] || 'Fallout 1 Realtime';
const DIR = path.join(require('os').tmpdir(), 'projectile-gallery');
fs.mkdirSync(DIR, { recursive: true });
const RUN = path.join(__dirname, 'run.js');
const shotsDir = process.env.GAL_SHOTS_ROOT ? path.dirname(exe) : path.join(path.dirname(exe), 'screenshots');
const TAG = process.env.GAL_TAG || 'f1';

// who: 'dude' = the dude fires at a target 6 tiles off; 'npc' = an NPC fires at the dude (bursts).
const W = [
    ['10mm Pistol', 'dude', 8, 29, 'small_guns'],
    ['9mm Mauser', 'dude', 122, 121, 'small_guns'],
    ['Desert Eagle .44', 'dude', 18, 31, 'small_guns'],
    ['14mm Pistol', 'dude', 22, 33, 'small_guns'],
    ['.223 Pistol', 'dude', 241, 34, 'small_guns'],
    ['Hunting Rifle', 'dude', 10, 34, 'small_guns'],
    ['Sniper Rifle', 'dude', 143, 34, 'small_guns'],
    ['Shotgun', 'dude', 94, 95, 'small_guns'],
    ['Red Ryder BB Gun', 'dude', 161, 163, 'small_guns'],
    ['10mm SMG (burst)', 'npc', 9, 29, 'small_guns'],
    ['Assault Rifle (burst)', 'npc', 23, 35, 'small_guns'],
    ['Combat Shotgun', 'npc', 242, 95, 'small_guns'],
    ['Minigun (burst)', 'npc', 12, 35, 'big_guns'],
    ['Laser Pistol', 'dude', 16, 38, 'energy'],
    ['Laser Rifle', 'dude', 118, 39, 'energy'],
    ['Gatling Laser (burst)', 'npc', 28, 39, 'big_guns'],
    ['Plasma Pistol', 'dude', 24, 38, 'energy'],
    ['Plasma Rifle', 'dude', 15, 39, 'energy'],
    ['Turbo Plasma Rifle', 'dude', 233, 39, 'energy'],
    ['Rocket Launcher', 'dude', 13, 14, 'big_guns'],
    ['Frag Grenade', 'throw', 25, 0, 'throwing'],
    ['Plasma Grenade', 'throw', 26, 0, 'throwing'],
    ['Pulse Grenade', 'throw', 27, 0, 'throwing'],
    ['Throwing Knife', 'throw', 45, 0, 'throwing'],
];
if (process.env.GAL_F2) W.push(['Spear (thrown)', 'throw', 7, 0, 'throwing']);

const TARGET = Number(process.env.GAL_TARGET || 16777227); // F1: a radscorpion, the "potvora" of his report
const SHOOTER = Number(process.env.GAL_SHOOTER || 16777291); // a raider
const TARGET_TILE = Number(process.env.GAL_TARGET_TILE || 21300);
const CROP = (process.env.GAL_CROP || '300 80 320 240').split(' '); // x y w h of the 853x480 screen

function scenario(w) {
    const [name, who, pid, ammo, skill] = w;
    const lines = ['name gallery', 'map DESERT1.MAP', 'seed 2', 'shots 1', 'duration 9', 'dude tile 20100 rot 2', 'dude hp 3000'];
    if (process.env.GAL_ARMOR && who !== 'npc') lines.push('dude armor ' + process.env.GAL_ARMOR);
    if (who === 'dude') {
        lines.push(`dude skill ${skill} 150`, `dude wield ${pid}`, `dude item ${ammo} 30`);
        lines.push(`spawn ${TARGET} ${TARGET_TILE} rot 5 ${process.env.GAL_TARGET_EXTRA || ""} hostile hp 999`, 'at 0 fight');
    } else if (who === 'throw') {
        lines.push(`dude skill throwing 150`, `dude item ${pid} 5`, `dude quickslot rb ${pid}`);
        lines.push(`spawn ${TARGET} ${TARGET_TILE} rot 5 ${process.env.GAL_TARGET_EXTRA || ""} hostile hp 999`, 'at 0 throw');
    } else {
        lines.push(`dude armor 3`);
        lines.push(`spawn ${SHOOTER} 21300 rot 5 wield ${pid} item ${ammo} 60 ${process.env.GAL_SHOOTER_EXTRA || ""} hostile hp 999`, 'at 0 idle');
    }
    return lines.join('\n') + '\n';
}

function pngCrop(bmp, out) {
    execFileSync('node', [path.join(__dirname, 'bmp2png.js'), bmp, out, ...CROP, '2'], { stdio: 'ignore' });
    return fs.readFileSync(out).toString('base64');
}

const sections = [];
W.forEach((w, i) => {
    const scen = path.join(DIR, `gal-${TAG}-${i}.txt`);
    fs.writeFileSync(scen, scenario(w));
    const isShot = f => /^scr\d+\.bmp$/i.test(f);
    const before = new Set(fs.existsSync(shotsDir) ? fs.readdirSync(shotsDir).filter(isShot) : []);
    spawnSync('node', [RUN, scen, '--exe', exe], { stdio: 'ignore', timeout: 90000 });
    const frames = fs.existsSync(shotsDir) ? fs.readdirSync(shotsDir).filter(f => isShot(f) && !before.has(f)).sort() : [];
    const imgs = frames.slice(0, 6).map((f, k) => pngCrop(path.join(shotsDir, f), path.join(DIR, `gal-${TAG}-${i}-${k}.png`)));
    console.log(w[0], frames.length);
    sections.push({ name: w[0], who: w[1], imgs });
});

const esc = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;');
let html = `<!doctype html><meta charset="utf-8"><title>Projectiles</title>
<style>
:root{--bg:#141210;--fg:#e8e2d0;--dim:#9a917c;--card:#1e1b17}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:14px/1.4 system-ui,sans-serif}
h1{font-size:20px;margin:0 0 4px}p{color:var(--dim);margin:0 0 16px}
section{background:var(--card);border-radius:8px;padding:10px 12px;margin:0 0 14px}
h2{font-size:15px;margin:0 0 8px}h2 small{color:var(--dim);font-weight:normal}
.row{display:flex;gap:6px;overflow-x:auto}.row img{width:${CROP[2]}px;height:auto;image-rendering:pixelated;border-radius:4px;flex:none}
.none{color:var(--dim)}
</style>
<h1>${esc(label)}: every projectile</h1>
<p>Self-played: frames taken while the shot is in the air (bullets every frame, rockets, plasma and grenades every 0.12 s). ${process.env.GAL_NOTE || 'The dude shoots a radscorpion 6 tiles away; burst weapons are fired by a raider at the dude.'}</p>
`;
for (const s of sections) {
    const who = s.who === 'npc' ? (process.env.GAL_SHOOTER_NAME || 'a raider') + ' fires at the dude' : s.who === 'throw' ? 'the dude throws (RB)' : 'the dude fires';
    html += `<section><h2>${esc(s.name)} <small>${who}</small></h2>`;
    html += s.imgs.length ? `<div class="row">${s.imgs.map(b => `<img src="data:image/png;base64,${b}">`).join('')}</div>` : `<div class="none">nothing caught in the air</div>`;
    html += `</section>\n`;
}
fs.writeFileSync(outHtml, html);
console.log('wrote', outHtml);
