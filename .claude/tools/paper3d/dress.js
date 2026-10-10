// The Vault 13 jumpsuit, cleaned up on a carved (hull.js) body.
//
// The carve colours each voxel from the view that sees it best, and the suit's two details are
// exactly what that can't carry: the zipper stripe down the chest is seen only by the two front
// three-quarter views (+-30 deg), and the "13" on the back only by the two back ones (+-60 deg),
// each about 7 x 6 px -- so both came out bent and smeared. Here they are painted again, straight,
// in the voxels' own frame (x forward, y up, z to the wearer's right): the stripe down the chest's
// centre line, and a crisp "13" centred on the upper back. Every recolour keeps the voxel's
// brightness (a dark blue voxel becomes a dark yellow one and back), so the baked lighting of the
// original renders survives.
//
// Only for the jumpsuit art (HMJMPS* / HFJMPS*); body shape is not touched.

'use strict';

// Palette shades, darkest to brightest, read off the vault dweller's carve.
const BLUES = [47, 46, 111, 110, 109, 108];
const YELLOWS = [62, 61, 60, 59, 58, 58];

const DIGITS = {
    1: ['.XX..', 'XXX..', '.XX..', '.XX..', '.XX..', '.XX..', 'XXXX.'],
    3: ['XXXX.', '...XX', '...XX', '.XXX.', '...XX', '...XX', 'XXXX.'],
};

function classify(pal) {
    const yellow = (c) => {
        const r = pal[c * 3], g = pal[c * 3 + 1], b = pal[c * 3 + 2];
        return r >= 20 && g >= 0.82 * r && b <= 0.3 * r; // skin is redder (g ~ 0.7 r)
    };
    const blue = (c) => {
        const r = pal[c * 3], g = pal[c * 3 + 1], b = pal[c * 3 + 2];
        return b > r + 3 && b >= g;
    };
    const light = (c) => pal[c * 3] + pal[c * 3 + 1] + pal[c * 3 + 2];
    // shade rank 0..5 of any colour, by brightness against the suit's blues
    const rank = (c) => {
        const l = light(c);
        let best = 0, bestD = Infinity;
        BLUES.forEach((q, n) => { const d = Math.abs(light(q) - l); if (d < bestD) { bestD = d; best = n; } });
        if (yellow(c)) {
            let yb = 0, yd = Infinity;
            YELLOWS.forEach((q, n) => { const d = Math.abs(light(q) - l); if (d < yd) { yd = d; yb = n; } });
            return yb;
        }
        return best;
    };
    return { yellow, blue, rank };
}

const median = (a) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[s.length >> 1] : 0; };

function dressVaultSuit(surf, pal) {
    if (!surf.length) return surf;
    const { yellow, blue, rank } = classify(pal);
    const toBlue = (s) => { s.c = BLUES[rank(s.c)]; };
    const toYellow = (s) => { s.c = YELLOWS[rank(s.c)]; };

    // belt: the layer with the most yellow all round. Collar: the shoulder line, the highest layer
    // below the head that is more than twice the head's width (a fixed "head height" guessed
    // three layers too low and left the old number standing above it).
    // (searched only where a belt can be -- 45-66 % of the height -- because a walking torso
    // twists and the smeared number could out-yellow it)
    const topJ = Math.max(...surf.map((s) => s.j)), botJ = Math.min(...surf.map((s) => s.j));
    const lo = botJ + 0.45 * (topJ - botJ), hi = botJ + 0.66 * (topJ - botJ);
    const yl = {};
    for (const s of surf) if (yellow(s.c) && s.j >= lo && s.j <= hi) yl[s.j] = (yl[s.j] || 0) + 1;
    const beltJ = +Object.entries(yl).sort((a, b) => b[1] - a[1])[0]?.[0];
    const width = (j) => {
        const ks = surf.filter((s) => s.j === j).map((s) => s.k);
        return ks.length ? Math.max(...ks) - Math.min(...ks) + 1 : 0;
    };
    const headW = median([1, 2, 3, 4, 5].map((d) => width(topJ - d)));
    let collarJ = topJ - 13;
    for (let j = topJ - 4; j > beltJ + 10; j--) if (width(j) >= 2.2 * headW) { collarJ = j; break; }
    if (!Number.isFinite(beltJ) || collarJ - beltJ < 10) return surf;
    const torso = (s) => s.j >= beltJ + 2 && s.j <= collarJ;

    // the renders' deepest shading quantised to near-black greys (207, 202) and a dark green (100):
    // on the suit that read as dirt. Keep it dark, but make it the suit's darkest blue.
    const jg = Math.min(...surf.map((s) => s.j));
    for (const s of surf) {
        if (s.j < jg + 8 || s.j > collarJ) continue; // boots and head keep their colours
        const r = pal[s.c * 3], g = pal[s.c * 3 + 1], b = pal[s.c * 3 + 2];
        if (Math.max(r, g, b) <= 9 && !blue(s.c)) s.c = BLUES[0];
    }

    // FRONT: one straight yellow stripe down the centre line, everything else on the chest blue
    const front = surf.filter((s) => torso(s) && s.n[0] >= 0.35);
    const zcF = median(front.filter((s) => s.j >= beltJ + 4 && s.j <= collarJ - 2).map((s) => s.k + 0.5));
    for (const s of front) {
        if (Math.abs(s.k + 0.5 - zcF) <= 1.0) toYellow(s);
        else if (yellow(s.c)) toBlue(s);
    }

    // BACK: wipe the smeared number, then stamp a crisp "13" on the upper back
    for (const s of surf) if (s.j >= beltJ + 3 && s.j <= collarJ + 1 && s.n[0] <= 0.1 && yellow(s.c)) toBlue(s);
    const back = surf.filter((s) => s.j >= beltJ + 3 && s.j <= collarJ - 1 && s.n[0] <= -0.35);
    const midJ = Math.round(beltJ + 0.6 * (collarJ - beltJ));
    const zcB = median(back.filter((s) => Math.abs(s.j - midJ) <= 3).map((s) => s.k + 0.5));
    const scale = 1.35, cols = 11, rows = 7;
    const topText = midJ + (rows * scale) / 2;
    for (const s of back) {
        // seen from behind, the viewer's left is the wearer's left: the text runs towards +z
        const u = Math.floor((s.k + 0.5 - zcB) / scale + cols / 2);
        const v = Math.floor((topText - (s.j + 0.5)) / scale);
        if (u < 0 || u >= cols || v < 0 || v >= rows) continue;
        const digit = u < 5 ? DIGITS[1] : u >= 6 ? DIGITS[3] : null;
        const on = digit && digit[v][u < 5 ? u : u - 6] === 'X';
        if (on) toYellow(s);
        else if (yellow(s.c)) toBlue(s);
    }
    return surf;
}

const isJumpsuit = (name) => /HMJMPS|HFJMPS/i.test(name || '');

module.exports = { dressVaultSuit, isJumpsuit };
