// Serves build/paper3d on http://localhost:8765 -- only needed where a page can't be opened
// straight from disk (a normal browser opens the .html files directly).
const http = require('http');
const fs = require('fs');
const path = require('path');

const DIR = path.resolve(__dirname, '../../../build/paper3d');
const PORT = +process.argv[2] || 8765;

http.createServer((req, res) => {
    const name = decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || '';
    const file = path.join(DIR, name);
    if (!file.startsWith(DIR)) { res.writeHead(403).end(); return; }
    if (name === '') {
        const list = fs.readdirSync(DIR).filter((f) => f.endsWith('.html'));
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        res.end(`<!doctype html><title>Paper Fallout maps</title><body style="background:#0a0a09;color:#ddd6c4;font:14px monospace;padding:16px">`
            + list.map((f) => `<p><a style="color:#9ad26f" href="${f}">${f.replace('.html', '')}</a></p>`).join('') + '</body>');
        return;
    }
    fs.readFile(file, (err, data) => {
        if (err) { res.writeHead(404).end('not found'); return; }
        res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
        res.end(data);
    });
}).listen(PORT, () => console.log(`paper3d: http://localhost:${PORT}/`));
