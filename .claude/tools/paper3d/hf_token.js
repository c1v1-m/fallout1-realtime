// Asks for a Hugging Face access token (hidden as you type or paste it), checks it with Hugging
// Face, and saves it where Hugging Face's own tools keep it: ~/.cache/huggingface/token. trellis.js
// reads it from there. Run it yourself in a terminal -- the token never passes through anyone else.
//
//   node hf_token.js

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const readline = require('readline');

const file = path.join(os.homedir(), '.cache', 'huggingface', 'token');
const rl = readline.createInterface({ input: process.stdin, output: process.stdout, terminal: true });
let hidden = false;
rl._writeToOutput = (s) => rl.output.write(hidden ? (s.includes('\n') ? '\n' : '*') : s);
rl.question('Hugging Face token (hf_...): ', async (answer) => {
    rl.close();
    const token = answer.trim();
    if (!/^hf_\w{20,}$/.test(token)) {
        console.log('\nThat does not look like a token (it starts with hf_). Nothing saved.');
        process.exit(1);
    }
    const r = await fetch('https://huggingface.co/api/whoami-v2', { headers: { Authorization: `Bearer ${token}` } });
    if (!r.ok) {
        console.log(`\nHugging Face refused it (HTTP ${r.status}). Nothing saved.`);
        process.exit(1);
    }
    const who = await r.json();
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, token);
    console.log(`\nOK -- account "${who.name}", saved to ${file}`);
});
hidden = true;
