/* Node-only regression test: a request that goes through
 * kcFetchWithSafetyAck and hits a 401 login prompt must keep working.
 *
 * ROADMAP.md used to list "retrying through the safety-ack wrapper" as open.
 * Reading app.js shows it is not a defect: the auth wrapper (window.fetch) is
 * the innermost layer, retryOnce() replays the caller's init unchanged, and
 * the retry's response resolves back into kcFetchWithSafetyAck, which then
 * handles a 428 itself. This test pins that so it stays true:
 *   Case 1: 401 -> login ok -> 428 -> confirm ok -> 200. Exactly one login
 *           modal, exactly one confirm, and the final request carries
 *           X-Ota-Ack-No-Safety: 1.
 *   Case 2: 428 -> confirm ok -> 401 (session dropped) -> login ok -> 200.
 *           The login retry must carry the ack header, with no second confirm.
 *
 * Same extraction-by-marker approach as test_login_auth_wrapper.js; the login
 * modal and kcConfirm are stubbed. Picked up automatically by
 * tools/check_page_js_tests.ps1 (it runs every test_*.js in this directory).
 *
 * Run: node firmware/KilnFW/App/test/test_fetch_auth_ack.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const SRC = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'app.js'), 'utf8');
const LINES = SRC.split('\n');

function extractRange(startMarker, endMarkerExclusive) {
  const raw = (l) => l.replace(/\r$/, '');
  const s = LINES.findIndex((l) => raw(l) === startMarker);
  if (s === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const e = LINES.findIndex((l, i) => i > s && raw(l) === endMarkerExclusive);
  if (e === -1) throw new Error('end marker not found: ' + JSON.stringify(endMarkerExclusive));
  return LINES.slice(s, e).join('\n');
}

const RANGE_A = extractRange(
  '  var nativeFetch = window.fetch.bind(window);',
  '  var loginModalEl = null, loginTitleEl = null, loginUserEl = null, loginPassEl = null,'
);
// pendingLogin/ensureAdminLogin, the window.fetch wrapper, and
// kcFetchWithSafetyAck (everything up to the OTA-family section).
const RANGE_B = extractRange(
  '  var pendingLogin = null;',
  '  // ---- OTA-family fetch -------------------------------------------------'
);
const SRC_UNDER_TEST = RANGE_A + '\n' + RANGE_B;
if (SRC_UNDER_TEST.indexOf('window.kcFetchWithSafetyAck = function') === -1 ||
    SRC_UNDER_TEST.indexOf('window.fetch = function') === -1) {
  throw new Error('sanity: extracted range is missing the wrapper or kcFetchWithSafetyAck');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

function makeContext(responses) {
  const queue = responses.slice();
  const nativeCalls = [];
  const modalCalls = [];
  const confirmCalls = [];
  const resp = (status, text) => ({
    ok: status >= 200 && status < 300,
    status: status,
    headers: { get: function () { return null; } },
    text: function () { return Promise.resolve(text || ''); },
  });
  const ctx = {
    kcRequestIsUserInitiated: function () { return true; },
    kcPageIsDashboard: function () { return false; },
    Request: undefined,
    openLoginModal: function (t) { modalCalls.push(t); return Promise.resolve(true); },
    console: console,
    window: {
      fetch: function (input, init) {
        nativeCalls.push({ input: input, init: init });
        const next = queue.shift();
        if (!next) return Promise.reject(new Error('no response queued'));
        return Promise.resolve(resp(next[0], next[1]));
      },
      kcConfirm: function (msg) { confirmCalls.push(msg); return Promise.resolve(true); },
    },
  };
  // kcFetchWithSafetyAck calls the bare global `fetch`, which in a page is
  // window.fetch (the wrapper installed by the extracted code).
  ctx.fetch = function (u, i) { return ctx.window.fetch(u, i); };
  vm.createContext(ctx);
  vm.runInContext(SRC_UNDER_TEST, ctx);
  return { ctx, nativeCalls, modalCalls, confirmCalls };
}

function ackHeader(call) {
  return call.init && call.init.headers && call.init.headers['X-Ota-Ack-No-Safety'];
}

(async () => {
  // Case 1: 401 -> login -> 428 -> confirm -> 200.
  {
    const t = makeContext([[401], [428, 'safety link down'], [200]]);
    const r = await t.ctx.window.kcFetchWithSafetyAck('/api/ota/esp/push', { method: 'POST', body: 'x' });
    assert(r.status === 200, 'case 1: final response is the 200');
    assert(t.modalCalls.length === 1, 'case 1: exactly one login modal');
    assert(t.confirmCalls.length === 1, 'case 1: exactly one safety confirm (no double prompt)');
    assert(t.nativeCalls.length === 3, 'case 1: three requests sent (401, login retry, ack retry)');
    assert(!ackHeader(t.nativeCalls[0]) && !ackHeader(t.nativeCalls[1]),
      'case 1: no ack header before the user confirmed');
    assert(ackHeader(t.nativeCalls[2]) === '1', 'case 1: final request carries X-Ota-Ack-No-Safety: 1');
  }

  // Case 2: 428 -> confirm -> 401 (session dropped) -> login -> 200.
  {
    const t = makeContext([[428, 'safety link down'], [401], [200]]);
    const r = await t.ctx.window.kcFetchWithSafetyAck('/api/ota/esp/push', { method: 'POST', body: 'x' });
    assert(r.status === 200, 'case 2: final response is the 200');
    assert(t.modalCalls.length === 1, 'case 2: exactly one login modal');
    assert(t.confirmCalls.length === 1, 'case 2: exactly one safety confirm');
    assert(t.nativeCalls.length === 3, 'case 2: three requests sent');
    assert(ackHeader(t.nativeCalls[1]) === '1', 'case 2: the ack request carries the header');
    assert(ackHeader(t.nativeCalls[2]) === '1',
      'case 2: the post-login retry still carries X-Ota-Ack-No-Safety: 1');
  }

  console.log('\n' + passed + ' passed, ' + failed + ' failed');
  if (failed > 0) { console.log('Failures: ' + failures.join(', ')); process.exit(1); }
})();
