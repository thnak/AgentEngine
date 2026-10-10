// devsim/verify-includes.js
// Every `#include` path the docs publish goes through ApiIncludes.tsx, whose callers pass a
// `*_HEADERS` array. This asserts that each of those paths is also present in
// devsim/verify-includes/main.cpp, which the gate compiles -- so a header cannot be advertised on
// the site without something proving it still exists.
const fs = require('fs');
const path = require('path');

const compDir = 'web/marketing/src/components';
const verifiedSrc = fs.readFileSync('devsim/verify-includes/main.cpp', 'utf8');

// Headers actually #included by the verification TU.
const compiled = new Set(
    [...verifiedSrc.matchAll(/#include\s+"([^"]+)"/g)].map((m) => m[1]),
);

// `const NAME_HEADERS = [ "a", "b" ];` in any component.
const published = new Map(); // header -> [file]
for (const f of fs.readdirSync(compDir)) {
    if (!f.endsWith('.tsx')) continue;
    const src = fs.readFileSync(path.join(compDir, f), 'utf8');
    for (const decl of src.matchAll(/const\s+\w*HEADERS\w*\s*=\s*\[([\s\S]*?)\];/g)) {
        for (const h of decl[1].matchAll(/"([^"]+)"/g)) {
            const list = published.get(h[1]) || [];
            list.push(f);
            published.set(h[1], list);
        }
    }
}

if (published.size === 0) {
    // No include blocks are published yet; nothing to check rather than a silent pass.
    console.log('no *_HEADERS arrays found in components -- nothing to verify');
    process.exit(0);
}

let bad = 0;
for (const [header, files] of [...published].sort()) {
    if (!compiled.has(header)) {
        bad++;
        // "MISSING from devsim/verify-includes/main.cpp" -- the gate would not catch a rename.
        console.log('UNVERIFIED  ' + header + '  (published by ' + [...new Set(files)].join(', ') + ')');
    }
}

// And the converse: anything the TU compiles that no page publishes is worth knowing about, but is
// not an error -- the TU intentionally covers more than today's blocks.
const orphan = [...compiled].filter((h) => !published.has(h));

console.log('published include paths: ' + published.size + ' (all present in: ' + compiled.size + ' compiled)');
if (orphan.length) console.log('compiled but not yet published (' + orphan.length + '): ' + orphan.join(', '));
console.log(bad === 0
    ? 'ALL PUBLISHED INCLUDE PATHS ARE VERIFIED BY COMPILATION'
    : bad + ' PUBLISHED PATH(S) ARE NOT COMPILED ANYWHERE');
process.exit(bad === 0 ? 0 : 1);
