/* Node-only test for ota_page.html pollEspStatus(): /api/ota/esp/status is ADMIN
 * (46616a4e), so a 401 after a post-OTA reboot must be handled explicitly
 * (board answered = up, ask for login) and a non-ok body must never reach
 * renderEspInfo(). Extracts the PRODUCTION function text from the page.
 * Run: node firmware/KilnFW/App/test/test_ota_page_poll_auth.js */
'use strict';
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const fs = require('fs');
const SRC = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'ota_page.html'), 'utf8');

let passed = 0, failed = 0;
function assert(c, l) { if (c) { passed++; console.log('PASS: ' + l); } else { failed++; console.log('FAIL: ' + l); } }

const m = SRC.match(/function pollEspStatus\(\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!m, 'found pollEspStatus() in ota_page.html');

function run(resp) {
  const calls = { render: 0, timers: 0, fetches: 0, text: '' };
  const el = { set textContent(v) { calls.text = v; } };
  const sandbox = {
    fetch: function () { calls.fetches++; return Promise.resolve(resp); },
    renderEspInfo: function () { calls.render++; return 'idle'; },
    setTimeout: function () { calls.timers++; },
    document: { getElementById: function () { return el; } },
    window: {},
  };
  const fn = new Function('fetch', 'renderEspInfo', 'setTimeout', 'document', 'window',
    m[0] + '; return pollEspStatus;')(sandbox.fetch, sandbox.renderEspInfo, sandbox.setTimeout, sandbox.document, sandbox.window);
  fn();
  return new Promise(function (res) { setTimeout(function () { res(calls); }, 20); });
}

(async function () {
  let c = await run({ ok: false, status: 401, json: function () { return Promise.resolve({ phase: 'writing' }); } });
  assert(c.render === 0 && c.timers === 0 && c.fetches === 1, '401: nothing rendered, no re-poll');
  assert(/log in again/.test(c.text), '401: tells the operator the board is up and to log in');
  c = await run({ ok: false, status: 500, json: function () { return Promise.resolve({ phase: 'writing' }); } });
  assert(c.render === 0 && c.timers === 0 && /Could not load/.test(c.text), '500: generic error, no render, no loop');
  c = await run({ ok: true, status: 200, json: function () { return Promise.resolve({ phase: 'writing' }); } });
  assert(c.render === 1, 'ok: rendered');
  console.log(passed + ' passed, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})();
