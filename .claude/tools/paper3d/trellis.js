// Image -> textured 3D model through Microsoft's TRELLIS.2 (MIT licence) on its official Hugging
// Face demo -- no local GPU: the demo runs on Hugging Face's shared GPUs (ZeroGPU), with a DAILY
// quota: 2 min unauthenticated -- too little, the generation fits but the GLB export after it
// doesn't -- 5 min with a free account. The account's token is read from HF_TOKEN or from
// ~/.cache/huggingface/token (where hf_token.js and Hugging Face's own login put it), is sent only
// to the demo, and is never printed.
//
//   node trellis.js <image.png> [outDir] [--seed N] [--res 512|1024|1536] [--faces N] [--tex N]
//   node trellis.js <image.png> --session <hash> --extract-only   (finish an earlier generation:
//                                        the asset waits in that session's state on the demo)
//
// Writes <outDir>/<name>.glb, <name>_seen.<ext> (the image after the demo's own crop -- what the
// model actually saw) and <name>.json (settings and timings). Give it an RGBA image with a
// transparent background (sprite_views.js cutout/) and the demo uses that outline as-is instead of
// guessing the background away.
//
// The demo is a Gradio 6 app; this speaks the same queue protocol its web page does (upload, then
// queue/join per step and read the session's event stream from queue/data). ONE session hash
// throughout: image_to_3d leaves the generated asset in the session's state (a hidden gr.State)
// and extract_glb takes it from there. The simpler /call/<endpoint> API can't do that -- give it
// a session_hash and it answers "404: Not Found", because it reads results back by event id.
// The demo's start_session isn't called either: all it does is make the session folder, which
// extract_glb makes itself. Function numbers are from the demo's /config (Gradio 6.1, sse_v3).

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');

const API = 'https://microsoft-trellis-2.hf.space/gradio_api';

const args = process.argv.slice(2);
const opt = (name, def) => {
    const i = args.indexOf('--' + name);
    return i >= 0 ? args.splice(i, 2)[1] : def;
};
const flag = (name) => {
    const i = args.indexOf('--' + name);
    return i >= 0 && !!args.splice(i, 1);
};
const seed = +opt('seed', 0), res = opt('res', '1024'), faces = +opt('faces', 300000), tex = +opt('tex', 2048);
const session = opt('session', null) || crypto.randomBytes(6).toString('hex');
const extractOnly = flag('extract-only');
const [input, outArg] = args;
if (!input) {
    console.error('usage: node trellis.js <image.png> [outDir] [--seed N] [--res 512|1024|1536] [--faces N] [--tex N] [--session H --extract-only]');
    process.exit(1);
}
const outDir = outArg || path.resolve(__dirname, '../../../build/paper3d/ai_output');
const name = path.basename(input, path.extname(input));
const t0 = Date.now();
const log = (...a) => console.log(`[${String(Math.round((Date.now() - t0) / 1000)).padStart(4)} s]`, ...a);

function hfToken() {
    if (process.env.HF_TOKEN) return { token: process.env.HF_TOKEN.trim(), from: 'HF_TOKEN' };
    const file = path.join(os.homedir(), '.cache', 'huggingface', 'token');
    if (fs.existsSync(file)) return { token: fs.readFileSync(file, 'utf8').trim(), from: file };
    return { token: null, from: null };
}
const HF = hfToken();
const AUTH = HF.token ? { Authorization: `Bearer ${HF.token}` } : {};

const FN = { preprocess_image: 4, image_to_3d: 7, extract_glb: 9 };

async function call(endpoint, data) {
    const r = await fetch(`${API}/queue/join`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...AUTH },
        body: JSON.stringify({ data, event_data: null, fn_index: FN[endpoint], trigger_id: null, session_hash: session }),
    });
    if (!r.ok) throw new Error(`${endpoint}: HTTP ${r.status} ${await r.text()}`);
    const { event_id } = await r.json();
    // the session's event stream: "data: <json>" blocks -- queue estimates, progress, and finally
    // process_completed for our event
    const s = await fetch(`${API}/queue/data?session_hash=${session}`, { headers: AUTH });
    if (!s.ok) throw new Error(`${endpoint}: stream HTTP ${s.status}`);
    const reader = s.body.getReader(), dec = new TextDecoder();
    let buf = '', said = 0;
    for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        for (let k; (k = buf.indexOf('\n\n')) >= 0; ) {
            const line = buf.slice(0, k).split('\n').find((l) => l.startsWith('data: '));
            buf = buf.slice(k + 2);
            if (!line) continue;
            const m = JSON.parse(line.slice(6));
            if (m.event_id && m.event_id !== event_id) continue;
            if (m.msg === 'estimation' && m.rank != null && m.rank > 0) log(`${endpoint}: queue position ${m.rank}`);
            else if (m.msg === 'process_starts') log(`${endpoint}: running`);
            else if (m.msg === 'progress' && m.progress_data && Date.now() - said > 15000) {
                said = Date.now();
                const p = m.progress_data[0];
                log(`${endpoint}: ${p.desc || ''} ${p.index != null && p.length ? `${p.index}/${p.length}` : ''}`.trim());
            } else if (m.msg === 'unexpected_error') {
                throw new Error(`${endpoint}: ${m.message}`);
            } else if (m.msg === 'process_completed') {
                reader.cancel().catch(() => {});
                if (!m.success) throw new Error(`${endpoint}: ${(m.output && m.output.error) || JSON.stringify(m.output)}`);
                return m.output.data;
            }
        }
    }
    throw new Error(`${endpoint}: the stream ended without a result`);
}

async function upload(file) {
    const form = new FormData();
    form.append('files', new Blob([fs.readFileSync(file)], { type: 'image/png' }), path.basename(file));
    const r = await fetch(`${API}/upload`, { method: 'POST', body: form, headers: AUTH });
    if (!r.ok) throw new Error(`upload: HTTP ${r.status} ${await r.text()}`);
    const [p] = await r.json();
    return { path: p, orig_name: path.basename(file), mime_type: 'image/png', meta: { _type: 'gradio.FileData' } };
}

async function download(fd, file) {
    const r = await fetch(fd.url || `${API}/file=${fd.path}`, { headers: AUTH });
    if (!r.ok) throw new Error(`download ${fd.path}: HTTP ${r.status}`);
    fs.writeFileSync(file, Buffer.from(await r.arrayBuffer()));
    return fs.statSync(file).size;
}

(async () => {
    fs.mkdirSync(outDir, { recursive: true });
    const note = (stage) => fs.writeFileSync(path.join(outDir, `${name}.json`),
        JSON.stringify({ input, session, stage, seed, res, faces, tex, seconds: (Date.now() - t0) / 1000 }, null, 2));
    log('session', session, '| Hugging Face token:', HF.token ? `yes (${HF.from})` : 'no -- the unauthenticated quota');
    if (!extractOnly) {
        const img = await upload(input);
        log('uploaded', path.basename(input));
        const [seen] = await call('preprocess_image', [img]);
        await download(seen, path.join(outDir, `${name}_seen${path.extname(seen.path) || '.png'}`));
        log('preprocessed');
        // resolution, then guidance strength / rescale / steps / rescale_t for the sparse
        // structure, the shape and the texture stages -- the demo's own defaults
        const [, html] = await call('image_to_3d', [seen, seed, res, 7.5, 0.7, 12, 5, 7.5, 0.5, 12, 3, 1, 0, 12, 3]);
        note('generated'); // from here --session <this> --extract-only can finish it
        // the demo's preview is a stack of inline JPEGs, 8 views around the model (s0-7) in each
        // shading mode (m0 normals, m1 clay = the bare shape, m2 base colour = the texture, m3-5
        // lit by HDRIs) -- kept, so the result can be judged even when the export is refused
        const dir = path.join(outDir, `${name}_preview`);
        fs.mkdirSync(dir, { recursive: true });
        let n = 0;
        for (const [, m, s, b64] of String(html).matchAll(/id="view-m(\d+)-s(\d+)"[^>]*?src="data:image\/jpeg;base64,([^"]+)"/g)) {
            fs.writeFileSync(path.join(dir, `m${m}_s${s}.jpg`), Buffer.from(b64, 'base64'));
            n++;
        }
        log('generated;', n, 'preview renders in', path.basename(dir));
    }
    const [glb] = await call('extract_glb', [null, faces, tex]); // null: the state, filled in by the session
    const size = await download(glb, path.join(outDir, `${name}.glb`));
    note('done');
    log(`${name}.glb`, (size / 1e6).toFixed(1), 'MB');
})().catch((e) => {
    console.error('FAILED:', e.message);
    process.exit(1);
});
