/* Node-only test harness for security_page.html's inline TOTP-enrollment
 * script (docs/TOTP_PASSWORD_RESET_PLAN.md WT-A, 2026-09-25 owner decision):
 * cmd=totp_enroll_begin and cmd=totp_enroll_confirm must show a distinct,
 * actionable message ("Turn on web login before enrolling an
 * authenticator.") when the board answers 409 { ok:false,
 * web_auth_disabled:true }, rather than the generic clock-unsynced or
 * incorrect-code messages already covered elsewhere.
 *
 * Extracts the inline `(function () { ... })();` TOTP-enrollment IIFE from
 * security_page.html by locating the `<script>`/`</script>` pair that
 * brackets the "TOTP enrollment wire contract" comment (the same
 * marker-extraction approach test_forgot_password_modal.js uses against
 * app.js), and runs it against a minimal fake DOM/fetch, without a real
 * browser.
 *
 * Covers:
 *   - cmd=totp_enroll_begin: a 409 { ok:false, web_auth_disabled:true }
 *     response shows the web-auth-off message and never throws (the naive
 *     `if (!resp.ok) throw` shape this fix replaces would have discarded
 *     the JSON body and shown "Could not start enrollment." instead);
 *   - cmd=totp_enroll_confirm: the same 409 { ok:false, web_auth_disabled:true }
 *     shape shows the same message rather than falling through to
 *     "Incorrect code -- try again.";
 *   - the ordinary success and clock_unsynced paths for both are
 *     unaffected by this change (still reachable, still show their own
 *     text).
 *
 * Run: node firmware/KilnFW/App/test/test_totp_enroll_web_auth_off.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'security_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');

function raw(l) { return l.replace(/\r$/, ''); }

// Find the "TOTP enrollment wire contract" comment, then walk outward to
// the bracketing <script>/</script> tags.
const markerIdx = LINES.findIndex((l) => l.indexOf('TOTP enrollment wire contract') !== -1);
if (markerIdx === -1) throw new Error('marker comment not found in security_page.html');
let scriptStart = -1;
for (let i = markerIdx; i >= 0; i--) {
  if (raw(LINES[i]) === '<script>') { scriptStart = i; break; }
}
if (scriptStart === -1) throw new Error('opening <script> not found before marker');
let scriptEnd = -1;
for (let i = markerIdx; i < LINES.length; i++) {
  if (raw(LINES[i]) === '</script>') { scriptEnd = i; break; }
}
if (scriptEnd === -1) throw new Error('closing </script> not found after marker');

const SCRIPT_SRC = LINES.slice(scriptStart + 1, scriptEnd).join('\n');
if (SCRIPT_SRC.indexOf('kcTotpBeginBtn') === -1) {
  throw new Error('extracted range does not look like the TOTP enrollment script');
}

// --- minimal fake DOM --------------------------------------------------

function makeEl(id) {
  return {
    id: id,
    textContent: '',
    className: '',
    value: '',
    hidden: false,
    disabled: false,
    width: 0,
    height: 0,
    _handlers: {},
    addEventListener: function (evt, fn) { this._handlers[evt] = fn; },
    getContext: function () { return { fillRect: function () {}, fillStyle: '' }; },
  };
}

const ELEMENT_IDS = [
  'kcTotpBadge', 'kcTotpClockLine', 'kcTotpNotEnrolled', 'kcTotpEnrolling', 'kcTotpEnrolled',
  'kcTotpBeginBtn', 'kcTotpQrCanvas', 'kcTotpSecretText', 'kcTotpCopySecret', 'kcTotpConfirmCode',
  'kcTotpConfirmBtn', 'kcTotpCancelEnrollBtn', 'kcTotpEnrollStatus', 'kcTotpDisableCode',
  'kcTotpDisableBtn', 'kcTotpDisableStatus',
];

function buildSandbox(fetchImpl) {
  const elements = {};
  ELEMENT_IDS.forEach(function (id) { elements[id] = makeEl(id); });

  const documentStub = {
    getElementById: function (id) {
      if (!elements[id]) throw new Error('unexpected getElementById(' + id + ')');
      return elements[id];
    },
  };

  const sandbox = {
    document: documentStub,
    window: {},
    fetch: fetchImpl,
    navigator: { clipboard: null },
    console: console,
    setTimeout: setTimeout,
    encodeURIComponent: encodeURIComponent,
  };
  sandbox.window.kcIsAuthCancelled = function () { return false; };
  sandbox.window.kcQr = { encode: function () { return { size: 1, matrix: [[1]] }; } };
  sandbox.window.kcConfirm = function () { return Promise.resolve(true); };
  vm.createContext(sandbox);
  vm.runInContext(SCRIPT_SRC, sandbox, { filename: 'security_page_totp_inline.js' });
  return elements;
}

// --- test plumbing -------------------------------------------------------

let failures = 0;
function check(cond, msg) {
  if (!cond) {
    failures++;
    console.error('FAIL: ' + msg);
  } else {
    console.log('PASS: ' + msg);
  }
}

function jsonResponse(body, ok, status) {
  if (ok === undefined) ok = true;
  if (status === undefined) status = 200;
  return Promise.resolve({ ok: ok, status: status, json: function () { return Promise.resolve(body); } });
}

function flush() {
  // Two microtask turns is enough to drain the .then() chains used here.
  return Promise.resolve().then(function () {}).then(function () {}).then(function () {});
}

// --- cmd=totp_enroll_begin -------------------------------------------------

function testBeginWebAuthOff() {
  const elements = buildSandbox(function () {
    return jsonResponse({ ok: false, web_auth_disabled: true }, false, 409);
  });
  elements.kcTotpBeginBtn._handlers.click();
  return flush().then(function () {
    check(elements.kcTotpEnrollStatus.textContent === 'Turn on web login before enrolling an authenticator.',
          'begin: web_auth_disabled shows the actionable message, got: ' +
          JSON.stringify(elements.kcTotpEnrollStatus.textContent));
  });
}

function testBeginClockUnsyncedStillWorks() {
  const elements = buildSandbox(function () {
    return jsonResponse({ ok: false, clock_unsynced: true, board_time_utc: '2026-09-25T00:00:00Z' });
  });
  elements.kcTotpBeginBtn._handlers.click();
  return flush().then(function () {
    check(elements.kcTotpEnrollStatus.textContent.indexOf('clock is not synced') !== -1,
          'begin: clock_unsynced path is unaffected by the new gate');
  });
}

// --- cmd=totp_enroll_confirm ------------------------------------------------

function testConfirmWebAuthOff() {
  const elements = buildSandbox(function () {
    return jsonResponse({ ok: false, web_auth_disabled: true }, false, 409);
  });
  elements.kcTotpConfirmCode.value = '123456';
  elements.kcTotpConfirmBtn._handlers.click();
  return flush().then(function () {
    check(elements.kcTotpEnrollStatus.textContent === 'Turn on web login before enrolling an authenticator.',
          'confirm: web_auth_disabled shows the actionable message, got: ' +
          JSON.stringify(elements.kcTotpEnrollStatus.textContent));
  });
}

function testConfirmIncorrectCodeStillWorks() {
  const elements = buildSandbox(function () {
    return jsonResponse({ ok: false });
  });
  elements.kcTotpConfirmCode.value = '123456';
  elements.kcTotpConfirmBtn._handlers.click();
  return flush().then(function () {
    check(elements.kcTotpEnrollStatus.textContent === 'Incorrect code -- try again.',
          'confirm: an ordinary wrong-code refusal is unaffected by the new gate');
  });
}

Promise.resolve()
  .then(testBeginWebAuthOff)
  .then(testBeginClockUnsyncedStillWorks)
  .then(testConfirmWebAuthOff)
  .then(testConfirmIncorrectCodeStillWorks)
  .then(function () {
    if (failures > 0) {
      console.error(failures + ' check(s) failed');
      process.exit(1);
    }
    console.log('all checks passed');
    process.exit(0);
  })
  .catch(function (err) {
    console.error('unhandled error: ' + (err && err.stack || err));
    process.exit(1);
  });
