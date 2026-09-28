/* Node-only test for app.js's maybeGateThisPage() -- owner decision
 * 2026-09-28: an unauthenticated web user may only view the dashboard;
 * every other page raises the shared cancelable login modal, and Cancel
 * returns to '/'.
 *
 * Runs the REAL pendingLogin/authPromptDeclined/ensureAdminLogin block and
 * the REAL gate block from app.js (extracted by marker line, same approach
 * as test_login_auth_wrapper.js), with only openLoginModal() stubbed. The
 * point of running the real ensureAdminLogin() is the review finding this
 * file was written for: the gate used to call openLoginModal() directly, so
 * on a gated page whose own first fetch had already 401'd (raising the
 * modal through the fetch wrapper), a second open of the same modal DOM
 * attached a second submit handler -- one Log in click, two login POSTs.
 *
 * Run: node firmware/KilnFW/App/test/test_page_login_gate.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const APP_JS_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'app.js');
const LINES = fs.readFileSync(APP_JS_PATH, 'utf8').split('\n');

function extractRange(startMarker, endMarkerExclusive) {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l) === startMarker);
  if (startIdx === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const endIdx = LINES.findIndex((l, i) => i > startIdx && raw(l) === endMarkerExclusive);
  if (endIdx === -1) throw new Error('end marker not found after start: ' + JSON.stringify(endMarkerExclusive));
  return LINES.slice(startIdx, endIdx).join('\n');
}

const LOGIN_SINGLETON = extractRange(
  '  var pendingLogin = null;',
  '  window.fetch = function (input, init) {'
);
const GATE = extractRange(
  '  var OPEN_WITHOUT_LOGIN_PATHS = {',
  '  function pollSession() {'
);
if (LOGIN_SINGLETON.indexOf('function ensureAdminLogin(') === -1) {
  throw new Error('sanity: singleton range lacks ensureAdminLogin');
}
if (GATE.indexOf('function maybeGateThisPage(') === -1) {
  throw new Error('sanity: gate range lacks maybeGateThisPage');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// Each openLoginModal() call returns a promise the test settles by hand.
function makeContext(pathname) {
  const opens = [];
  const ctx = {
    window: { location: { pathname: pathname, href: pathname } },
    openLoginModal: function (title) {
      let settle;
      const p = new Promise((resolve) => { settle = resolve; });
      opens.push({ title: title, settle: settle });
      return p;
    },
    kcPageIsDashboard: function () { return pathname === '/'; },
    Promise: Promise,
  };
  vm.createContext(ctx);
  vm.runInContext(LOGIN_SINGLETON + '\n' + GATE +
    '\nthis.__gate = maybeGateThisPage;' +
    '\nthis.__ensure = ensureAdminLogin;' +
    '\nthis.__declined = function () { return authPromptDeclined; };', ctx);
  return { ctx, opens };
}

function flush() {
  return new Promise((r) => setImmediate(r)).then(() => new Promise((r) => setImmediate(r)));
}

(async () => {
  // Group 1: dashboard and the other allowlisted paths never gate.
  for (const p of ['/', '/login', '/wifi']) {
    const { ctx, opens } = makeContext(p);
    ctx.__gate('none');
    assert(opens.length === 0, 'allowlisted path ' + p + ': no modal for a logged-out viewer');
  }

  // Group 2: a real session (or auth off, which reports admin) never gates.
  for (const role of ['admin', 'user']) {
    const { ctx, opens } = makeContext('/settings');
    ctx.__gate(role);
    assert(opens.length === 0, 'gated page, role ' + role + ': no modal');
  }

  // Group 3: logged out on a gated page -> modal; Cancel -> '/'.
  {
    const { ctx, opens } = makeContext('/settings');
    ctx.__gate('none');
    assert(opens.length === 1, 'gated page, logged out: opens the login modal');
    opens[0].settle(false);
    await flush();
    assert(ctx.window.location.href === '/', 'Cancel on the gate modal returns to the dashboard');
    assert(ctx.__declined() === true, 'Cancel marks the prompt declined for this page load');
  }

  // Group 4: successful login stays on the page.
  {
    const { ctx, opens } = makeContext('/profiles');
    ctx.__gate('none');
    opens[0].settle(true);
    await flush();
    assert(ctx.window.location.href === '/profiles', 'successful login stays on the gated page');
  }

  // Group 5 (review finding): the page's own 401 already opened the modal
  // via ensureAdminLogin() -- the gate must JOIN it, not open a second one.
  {
    const { ctx, opens } = makeContext('/diagnostics');
    const fromFetch = ctx.__ensure('Sign in required');
    ctx.__gate('none');
    assert(opens.length === 1, 'modal already open from a 401: the gate does not open a second one');
    opens[0].settle(false);
    const ok = await fromFetch;
    await flush();
    assert(ok === false, 'the fetch-side waiter sees the same Cancel');
    assert(ctx.window.location.href === '/', 'Cancel on the shared modal still returns to the dashboard');
  }

  // Group 6: the operator already declined a fetch-raised modal before the
  // session poll answered -- the gate goes home without asking again.
  {
    const { ctx, opens } = makeContext('/safety');
    const fromFetch = ctx.__ensure('Sign in required');
    opens[0].settle(false);
    await fromFetch;
    // The fetch wrapper sets authPromptDeclined on its own cancel path;
    // emulate that one assignment here.
    vm.runInContext('authPromptDeclined = true;', ctx);
    ctx.__gate('none');
    assert(opens.length === 1, 'already declined: gate opens no further modal');
    assert(ctx.window.location.href === '/', 'already declined: gate returns to the dashboard');
  }

  // Group 7: the gate runs once per page load.
  {
    const { ctx, opens } = makeContext('/ota');
    ctx.__gate('none');
    ctx.__gate('none');
    assert(opens.length === 1, 'repeat session polls do not re-raise the gate');
  }

  console.log('');
  console.log(passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
})();
