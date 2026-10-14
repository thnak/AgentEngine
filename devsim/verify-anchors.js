// devsim/verify-anchors.js
// Regression guard for the docs site: every right-rail "on this page" entry must point at an id the
// page's own component actually renders, or the TOC silently links nowhere. Resolves each
// api/*DetailApp.tsx to its reference component and its sections (local const or imported from
// apiContent), then checks every section id appears as id="..." in that component.
const fs = require('fs');
const path = require('path');

const apiDir = 'web/marketing/src/api';
const compDir = 'web/marketing/src/components';
const dataFile = 'web/marketing/src/data/apiContent.ts';

const apiContent = fs.readFileSync(dataFile, 'utf8');

// Collect every `export const <name>... = {` ... ids, keyed by const name.
function sectionIdsFromSource(src, constName) {
    const decl = new RegExp('export const ' + constName + '\\b[\\s\\S]*?\\n\\};', 'm').exec(src);
    if (!decl) return null;
    const ids = [...decl[0].matchAll(/\{\s*id:\s*"([^"]+)"/g)].map((m) => m[1]);
    return ids.length ? ids : null;
}

const apps = fs.readdirSync(apiDir).filter((f) => f.endsWith('DetailApp.tsx'));
let problems = 0;
let checked = 0;

for (const app of apps) {
    const appPath = path.join(apiDir, app);
    const appSrc = fs.readFileSync(appPath, 'utf8');

    // The reference component is the Api* import that is NOT the shared layout.
    const compImports = [...appSrc.matchAll(/from "\.\.\/components\/([A-Za-z0-9]+)"/g)].map((m) => m[1]);
    const compName = compImports.find((n) => n !== 'ApiDetailLayout');
    if (!compName) continue;
    const compPath = path.join(compDir, compName + '.tsx');
    if (!fs.existsSync(compPath)) { console.log('MISSING COMPONENT ' + compPath); problems++; continue; }
    // A page may render more than one component (e.g. the Agent page also renders
    // ApiAuthoringSample), so search every component it imports, not just the main one.
    let compSrc = '';
    for (const n of compImports) {
        const p = path.join(compDir, n + '.tsx');
        if (fs.existsSync(p)) compSrc += fs.readFileSync(p, 'utf8');
    }

    // sections: either a local object literal, or an imported const from apiContent
    let ids = null;
    if (/const sections = \{/.test(appSrc)) {
        const local = appSrc.slice(appSrc.indexOf('const sections = {'));
        ids = [...local.matchAll(/\{\s*id:\s*"([^"]+)"/g)].map((m) => m[1]);
    } else {
        const imp = /import \{ ([A-Za-z0-9, ]+) \} from "\.\.\/data\/apiContent"/.exec(appSrc);
        if (imp) {
            for (const name of imp[1].split(',').map((s) => s.trim())) {
                const got = sectionIdsFromSource(apiContent, name);
                if (got) { ids = got; break; }
            }
        }
    }
    if (!ids || !ids.length) continue;
    ids = [...new Set(ids)];  // en and vi both carry the same ids

    checked++;
    const missing = ids.filter((id) => !compSrc.includes('id="' + id + '"'));
    if (missing.length) {
        problems++;
        console.log('PROBLEM ' + app + ' -> ' + compName + ': TOC id(s) with no anchor: ' + missing.join(', '));
    }
}

console.log('checked ' + checked + ' page(s); ' + (problems === 0 ? 'all TOC anchors resolve' : problems + ' problem(s)'));
process.exit(problems === 0 ? 0 : 1);
