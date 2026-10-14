// devsim/verify-encoding.js
// Guards against the exact corruption that bit this session: a PowerShell Get-Content/Set-Content
// round-trip that turned a file's UTF-8 into mojibake and added a BOM. Walks every marketing source
// file and fails if any has a BOM, invalid UTF-8, or the double-encode signature.
const fs = require('fs');
const path = require('path');

const roots = ['web/marketing/src', 'docs/planning', 'devsim'];
const exts = new Set(['.ts', '.tsx', '.css', '.md', '.ps1', '.js']);

// Simulation scratch and build output are NOT files this project maintains: sub-agent reports and
// generated trees are written by other processes, and one of them writing a BOM is not a defect in
// the docs. Only the doc sources and this directory's own tooling are guarded.
const skipDir = /^(out|p\d+|rerun\d+|consumer-cmake.*)$/;

// The signature: a UTF-8 decode of already-UTF-8-decoded bytes. Vietnamese "Một" becomes "Má»™t".
const signatures = ['M\u00e1\u00bb', '\u00c3\u00a1\u00bb', '\u00c4\u2018\u00c3', '\u00c2\u00a7'];

function walk(dir, out) {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, e.name);
        if (e.isDirectory()) {
            if (e.name === 'node_modules' || e.name === 'dist' || e.name === 'build') continue;
            if (skipDir.test(e.name)) continue;
            walk(p, out);
        } else if (exts.has(path.extname(e.name))) {
            // This detector necessarily contains the signature it looks for.
            if (e.name === 'verify-encoding.js') continue;
            out.push(p);
        }
    }
}

const files = [];
for (const r of roots) if (fs.existsSync(r)) walk(r, files);

let bad = 0;
for (const f of files) {
    const buf = fs.readFileSync(f);
    const bom = buf.length >= 3 && buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf;
    let invalid = false;
    try {
        new TextDecoder('utf-8', { fatal: true }).decode(buf);
    } catch {
        invalid = true;
    }
    const text = buf.toString('utf8');
    const moji = signatures.filter((s) => text.includes(s));
    if (bom || invalid || moji.length) {
        bad++;
        console.log('PROBLEM ' + f + '  BOM=' + bom + ' invalidUtf8=' + invalid + ' mojibake=' + JSON.stringify(moji));
    }
}

console.log('checked ' + files.length + ' file(s); ' + (bad === 0 ? 'all clean' : bad + ' problem(s)'));
process.exit(bad === 0 ? 0 : 1);
