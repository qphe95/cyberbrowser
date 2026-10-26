#!/usr/bin/env node
// A static file server for a Speedometer checkout (JS_PERF_DESIGN.md section
// 4.3), on 127.0.0.1 only, for tests/jit/speedometer.sh.
//   node tools/speedometer_server.js <Speedometer dir> [port=18765]
// Prints "listening <port>" once it is ready.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const root = path.resolve(process.argv[2] || '.');
const port = Number(process.argv[3]) || 18765;
const TYPES = {
    '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript',
    '.css': 'text/css', '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png',
    '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.gif': 'image/gif', '.webp': 'image/webp',
    '.woff': 'font/woff', '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.ico': 'image/x-icon',
    '.wasm': 'application/wasm', '.txt': 'text/plain', '.map': 'application/json',
};

http.createServer((req, res) => {
    let rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    if (rel.endsWith('/')) rel += 'index.html';
    const file = path.join(root, rel);
    if (!file.startsWith(root)) { res.writeHead(403); res.end(); return; }
    fs.stat(file, (err, st) => {
        if (err || !st.isFile()) { res.writeHead(404); res.end('not found'); return; }
        res.writeHead(200, { 'Content-Type': TYPES[path.extname(file).toLowerCase()] || 'application/octet-stream',
                             'Content-Length': st.size, 'Cache-Control': 'no-store' });
        fs.createReadStream(file).pipe(res);
    });
}).listen(port, '127.0.0.1', () => console.log('listening ' + port));
