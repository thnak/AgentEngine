// devsim/compare-snippet.js
// Asserts that the program documented on api/first-app.html is byte-identical to the program that
// was actually compiled and run (devsim/first-program/main.cpp). Run after editing either one.
const fs = require('fs');

const tsx = fs.readFileSync('web/marketing/src/components/ApiFirstAppReference.tsx', 'utf8');
const BT = String.fromCharCode(96); // backtick

const start = tsx.indexOf('const SNIPPET_PROGRAM = ' + BT);
if (start < 0) { console.log('FAIL: SNIPPET_PROGRAM not found'); process.exit(1); }
const from = start + ('const SNIPPET_PROGRAM = ' + BT).length;
const end = tsx.indexOf(BT + ';', from);
if (end < 0) { console.log('FAIL: SNIPPET_PROGRAM terminator not found'); process.exit(1); }

// Inside a TS template literal a literal backslash-n is written as two chars: \ and n, escaped as \\
const doc = tsx.slice(from, end).split('\\\\n').join('\\n');
const cpp = fs.readFileSync('devsim/first-program/main.cpp', 'utf8');

const norm = (s) => s.replace(/\r\n/g, '\n').replace(/\n+$/, '');
const a = norm(doc).split('\n');
const b = norm(cpp).split('\n');

console.log('documented lines: ' + a.length + '   compiled lines: ' + b.length);

let diffs = 0;
for (let i = 0; i < Math.max(a.length, b.length); i++) {
    if (a[i] !== b[i]) {
        diffs++;
        if (diffs <= 10) {
            console.log('line ' + (i + 1));
            console.log('   doc: ' + JSON.stringify(a[i]));
            console.log('   cpp: ' + JSON.stringify(b[i]));
        }
    }
}

if (diffs === 0) {
    console.log('IDENTICAL -- the documented program is exactly the compiled one.');
    process.exit(0);
}
console.log('DIFFERS in ' + diffs + ' line(s).');
process.exit(1);
