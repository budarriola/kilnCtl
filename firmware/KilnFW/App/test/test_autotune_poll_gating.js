/* AG-01: main_page.html pollAutotune() polls the ADMIN /api/autotune route only for a signed-in
 * admin and tags it X-Kiln-Background; app.js publishes window.kcSessionRole; the server skips
 * the activity touch for that header. Run: node firmware/KilnFW/App/test/test_autotune_poll_gating.js */
'use strict';
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const fs = require('fs');
const dir = resolveDriversDir(__dirname);
const PAGE = fs.readFileSync(resolveDriverFile(dir, 'main_page.html'), 'utf8');
const APP = fs.readFileSync(resolveDriverFile(dir, 'app.js'), 'utf8');
const AUTH = fs.readFileSync(resolveDriverFile(dir, 'http_auth_http.c'), 'utf8');
let failed = 0;
function assert(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }

const m = PAGE.match(/function pollAutotune\(\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!m, 'found pollAutotune()');
function run(role) {
  const calls = [];
  const win = { kcSessionRole: role };
  const fetchFn = function (url, init) { calls.push({ url, init }); return new Promise(function () {}); };
  new Function('fetch', 'window', m[0] + '; pollAutotune();')(fetchFn, win);
  return calls;
}
assert(run('none').length === 0, 'signed out: no fetch');
assert(run(undefined).length === 0, 'role unknown: no fetch');
assert(run('user').length === 0, 'user role: no fetch');
const c = run('admin');
assert(c.length === 1 && c[0].url === '/api/autotune', 'admin: one /api/autotune fetch');
assert(c.length === 1 && c[0].init && c[0].init.headers['X-Kiln-Background'] === '1', 'admin poll sends X-Kiln-Background');
assert(/window\.kcSessionRole\s*=\s*role/.test(APP), 'app.js publishes window.kcSessionRole');
assert(/X-Kiln-Background"\) == 0/.test(AUTH), 'server skips the activity touch for background polls');
process.exit(failed ? 1 : 0);
