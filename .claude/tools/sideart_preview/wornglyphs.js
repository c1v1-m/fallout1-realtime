// The worn interface letters, CUT from the vanilla word art pixel for pixel (shape AND colour).
// art = intrface.lst index, x/w = columns, y/h = rows of the source FRM, skip = pixels in the cut
// that belong to the neighbouring letter. Y exists in no word on the bar: its shape is drawn in the
// same 3-px hand and every row of every stroke takes the colours of I at that row (left edge /
// middle / right edge), which is how the real strokes are shaded.
module.exports.GLYPHS = {
    S: { art: 43, x: 0, w: 8, y: 0, h: 11 },
    I: { art: 43, x: 9, w: 3, y: 0, h: 11 },
    N: { art: 43, x: 13, w: 11, y: 0, h: 11 },
    G: { art: 43, x: 25, w: 11, y: 0, h: 11 },
    L: { art: 43, x: 36, w: 8, y: 0, h: 11 },
    E: { art: 43, x: 44, w: 8, y: 0, h: 11 },
    M: { art: 287, x: 39, w: 9, y: 0, h: 11 },
    D: { art: 287, x: 59, w: 9, y: 0, h: 11 },
    A: { art: 289, x: 0, w: 8, y: 1, h: 11 },
    H: { art: 45, x: 10, w: 11, y: 0, h: 11 },
    T: { art: 45, x: 0, w: 9, y: 0, h: 11 },
    R: { art: 40, x: 22, w: 9, y: 0, h: 11, skip: [[30, 9]] },
    O: { art: 117, x: 30, w: 11, y: 0, h: 12, skip: [[40, 1]] },
    Y: { shade: { art: 43, x: 9, y: 0 }, h: 11, rows: ['###...###', '###...###', '###...###', '.###.###.', '..#####..', '...###...', '...###...', '...###...', '...###...', '...###...', '...###...'] },
};
