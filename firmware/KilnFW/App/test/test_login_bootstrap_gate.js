/* Node-only test for app.js's bootstrap-vs-gate interaction -- fixes the gap
 * left by d25d5ccf ("Web auth page gate: join the shared login modal"): the
 * popup login modal (buildLoginModal()) has only username/password fields,
 * which POST /api/auth/login refuses outright while no administrator
 * credential is configured (bootstrap_needed). Before the dashboard-only
 * page gate, an unauthenticated full navigation to a gated page was
 * redirected server-side to /login?return=..., and login_page.html already
 * knows how to show the bootstrap form instead of the login form there --
 * losing that redirect silently closed off first-run/lockout-recovery
 * bootstrap on every page but '/' and '/login'.
 *
 * ensureAdminLogin() is the single choke point every login prompt goes
 * through (the page gate in maybeGateThisPageGated() AND the fetch
 * wrapper's own 401/403 retry), so this test exercises it directly, the
 * same extraction approach as test_page_login_gate.js and
 * test_login_auth_wrapper.js: run the REAL pendingLogin/ensureAdminLogin
 * block (and the REAL gate block, for the page-gate cases) with only
 * openLoginModal() stubbed.
 *
 * Run: node firmware/KilnFW/App/test/test_login_bootstrap_gate.js
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
if (LOGIN_SINGLETON.indexOf('lastKnownBootstrapNeeded') === -1) {
  throw new Error('sanity: singleton range lacks lastKnownBootstrapNeeded -- has ensureAdminLogin moved?');
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
function makeContext(pathname, search) {
  const opens = [];
  const ctx = {
    window: { location: { pathname: pathname, search: search || '', href: pathname } },
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
    '\nthis.__setBootstrapNeeded = function (v) { lastKnownBootstrapNeeded = v; };', ctx);
  return { ctx, opens };
}

function flush() {
  return new Promise((r) => setImmediate(r)).then(() => new Promise((r) => setImmediate(r)));
}

(async () => {
  // Group 1: bootstrap needed -> ensureAdminLogin() redirects to /login
  // instead of ever opening the futile login-only popup.
  {
    const { ctx, opens } = makeContext('/settings');
    ctx.__setBootstrapNeeded(true);
    ctx.__ensure('Sign in required');
    assert(opens.length === 0, 'bootstrap needed: no login-only popup is opened');
    assert(ctx.window.location.href === '/login?return=%2Fsettings',
      'bootstrap needed: redirects to /login with the gated page as return=');
  }

  // Group 2: the return path preserves a query string, still constrained to
  // a same-page-relative "/..." shape login_page.html's loginReturnPath()
  // guard already requires.
  {
    const { ctx, opens } = makeContext('/live_profile', '?id=7');
    ctx.__setBootstrapNeeded(true);
    ctx.__ensure('Sign in required');
    assert(opens.length === 0, 'bootstrap needed with a query string: still no popup');
    assert(ctx.window.location.href === '/login?return=%2Flive_profile%3Fid%3D7',
      'bootstrap needed: return= carries the query string too');
  }

  // Group 3: regression -- once a credential exists (bootstrap not needed),
  // ensureAdminLogin() still opens the ordinary popup exactly as before.
  {
    const { ctx, opens } = makeContext('/settings');
    ctx.__setBootstrapNeeded(false);
    const p = ctx.__ensure('Sign in required');
    assert(opens.length === 1, 'bootstrap not needed: the ordinary login popup still opens');
    opens[0].settle(true);
    const ok = await p;
    assert(ok === true, 'bootstrap not needed: a successful login still resolves true');
  }

  // Group 4: the page gate (maybeGateThisPageGated) goes through the same
  // choke point, so a logged-out viewer on a gated page with bootstrap
  // needed is redirected to /login too, not shown the popup.
  {
    const { ctx, opens } = makeContext('/diagnostics');
    ctx.__setBootstrapNeeded(true);
    ctx.__gate('none');
    await flush();
    assert(opens.length === 0, 'page gate, bootstrap needed: no popup opened');
    assert(ctx.window.location.href === '/login?return=%2Fdiagnostics',
      'page gate, bootstrap needed: redirects to /login, not the dashboard');
  }

  // Group 5: once a second request joins an already-redirecting
  // ensureAdminLogin() call, it must not fire a second redirect (pendingLogin
  // is still set to the never-resolving placeholder from the first call).
  {
    const { ctx, opens } = makeContext('/ota');
    ctx.__setBootstrapNeeded(true);
    ctx.__ensure('Sign in required');
    const hrefAfterFirst = ctx.window.location.href;
    ctx.window.location.href = 'unchanged-marker';
    ctx.__ensure('Sign in required');
    assert(opens.length === 0, 'second concurrent call while bootstrap-redirecting: still no popup');
    assert(ctx.window.location.href === 'unchanged-marker',
      'second concurrent call while bootstrap-redirecting: does not redirect a second time');
    void hrefAfterFirst;
  }

  console.log('');
  console.log(passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
})();
