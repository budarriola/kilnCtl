/* Regression tests for WEB_UI_XSS_AUDIT_2026-10-09 F1/F2/F3/F6.
 * Run: node firmware/KilnFW/App/test/test_web_xss_fixes.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const dir = resolveDriversDir(__dirname);
const read = (n) => fs.readFileSync(resolveDriverFile(dir, n), 'utf8');
let failed = 0;
function assert(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }

// F6: loginReturnPath, extracted from login_page.html and run against a real URL.
const login = read('login_page.html');
const m = /function loginReturnPath\(\) \{[\s\S]*?\r?\n\}\r?\n/.exec(login);
assert(!!m, 'loginReturnPath extracted');
function run(search) {
  const ctx = { URLSearchParams, URL, window: { location: { search, origin: 'http://board.local' } } };
  vm.createContext(ctx);
  vm.runInContext(m[0], ctx);
  return vm.runInContext('loginReturnPath()', ctx);
}
const q = (v) => '?return=' + encodeURIComponent(v);
assert(run(q('/settings?a=1#x')) === '/settings?a=1#x', 'plain path kept');
assert(run(q('/\t/evil.example')) === '/', 'tab bypass rejected');
assert(run(q('/\n/evil.example')) === '/', 'LF bypass rejected');
assert(run(q('/\r/evil.example')) === '/', 'CR bypass rejected');
assert(run(q('/ x')) === '/', 'space rejected');
assert(run(q('//evil.example')) === '/', '// rejected');
assert(run(q('/\\evil.example')) === '/', 'backslash rejected');
assert(run(q('https://evil.example/')) === '/', 'absolute rejected');
assert(run('') === '/', 'missing -> /');

// F1/F2/F3: the cited sinks must route names through an escaper.
const main = read('main_page.html');
assert(!/notice \S+ ' \+ name \+/.test(main) && !/Start "' \+ name/.test(main), 'F1 main_page names escaped');
assert(/Feasibility notice [^']*' \+ window\.kcEscapeHtml\(name\)/.test(main), 'F1 escaped-name case present');
const zones = read('zones_page.html');
assert(!/\? ' \(' \+ current\.zones\[i\]\.name \+ '\)' : ''\;/.test(zones) && zones.indexOf('kgEsc(current.zones[i].name)') !== -1, 'F2 zone name escaped');
assert(!/var caption = rec\.profile_name/.test(zones), 'F3 caption escaped');
process.exit(failed ? 1 : 0);
