/* Node-only test harness for login_page.html's bootstrap-password form
 * (added 2026-09-20 as this page's new client for
 * POST /api/auth/bootstrap_password, found fully built, registered
 * (ROUTE_TIER_ADMIN_BOOTSTRAP) and completely unreachable by any human
 * during the "no client" audit that day).
 *
 * Extracts the whole inline <script> block (the file has exactly one, and
 * it is small/self-contained -- unlike the multi-thousand-line drivers this
 * repo's other test_*.js files pull single functions out of, marker-line
 * extraction would be overkill here) and runs it in a Node vm context with
 * a small document/fetch/window stub.
 *
 * Run: node firmware/KilnFW/App/test/test_login_bootstrap_form.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'login_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');

const scriptStart = SRC.indexOf('<script>');
const scriptEnd = SRC.indexOf('</script>');
if (scriptStart === -1 || scriptEnd === -1) {
  throw new Error('sanity: could not find a <script>...</script> block in login_page.html');
}
const FULL_SRC = SRC.slice(scriptStart + '<script>'.length, scriptEnd);

if (FULL_SRC.indexOf('function submitBootstrap(') === -1) {
  throw new Error('sanity: extracted range does not include submitBootstrap');
}
if (FULL_SRC.indexOf('bootstrap_needed') === -1) {
  throw new Error('sanity: extracted range does not include the bootstrap_needed gate check');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

function makeInput(value) {
  return { value: value };
}

function makeContext(opts) {
  opts = opts || {};
  const els = {
    'error': { textContent: '' },
    'bootstrap-error': { textContent: '' },
    'username': makeInput(opts.username || ''),
    'password': makeInput(opts.password || ''),
    'bootstrap-username': makeInput(opts.bootstrapUsername || 'admin'),
    'bootstrap-password': makeInput(opts.bootstrapPassword !== undefined ? opts.bootstrapPassword : 'newpw'),
    'bootstrap-password-confirm': makeInput(opts.bootstrapPasswordConfirm !== undefined ? opts.bootstrapPasswordConfirm : 'newpw'),
    'login-form': { hidden: false, addEventListener: function () {} },
    'bootstrap-form': { hidden: false, addEventListener: function () {} },
  };
  const fetchCalls = [];
  const fetchQueue = opts.fetchQueue || [];
  let reloaded = false;
  let navigatedTo = null;
  const ctx = {
    document: {
      getElementById: function (id) {
        if (!(id in els)) throw new Error('unexpected getElementById(' + id + ')');
        return els[id];
      },
    },
    window: {
      location: {
        get href() { return navigatedTo; },
        set href(v) { navigatedTo = v; },
        reload: function () { reloaded = true; },
        search: opts.search || '',
      },
    },
    fetch: function (url, init) {
      fetchCalls.push({ url: url, init: init });
      const next = fetchQueue.shift();
      if (!next) return Promise.reject(new Error('no queued response for ' + url));
      if (next.reject) return Promise.reject(next.reject);
      return Promise.resolve({
        ok: next.ok !== false,
        status: next.status || (next.ok !== false ? 200 : 400),
        text: function () { return Promise.resolve(next.text || ''); },
        json: function () { return Promise.resolve(next.json || {}); },
      });
    },
    encodeURIComponent: encodeURIComponent,
    console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(FULL_SRC, ctx);
  return { ctx: ctx, els: els, fetchCalls: fetchCalls, getReloaded: function () { return reloaded; },
           getNavigatedTo: function () { return navigatedTo; } };
}

function fakeEvt() { return { preventDefault: function () {} }; }

// submitBootstrap's non-ok branch is resp.then(text => ...) two promise
// hops deep (the outer fetch().then, then a nested resp.text().then) --
// same shape test_recovery_banner.js's flush() exists for; one setImmediate
// clears both microtask hops reliably, a bare Promise.resolve().then()
// does not.
function flush() {
  return new Promise((resolve) => setImmediate(resolve));
}

// ---------------------------------------------------------------------------
// Group 1: submitBootstrap client-side validation and success/failure paths.
// ---------------------------------------------------------------------------
// The page's own load-time fetch('/api/auth/session') fires as soon as
// FULL_SRC runs inside makeContext (no queued response -- it rejects and is
// caught, same as a real network error), so every assertion below filters
// fetchCalls down to the bootstrap endpoint specifically rather than
// counting all calls.
function bootstrapCalls(fetchCalls) {
  return fetchCalls.filter(function (c) { return c.url === '/api/auth/bootstrap_password'; });
}

{
  const { ctx, els, fetchCalls } = makeContext({ bootstrapPassword: 'abc', bootstrapPasswordConfirm: 'xyz' });
  vm.runInContext('submitBootstrap', ctx)(fakeEvt());
  assert(bootstrapCalls(fetchCalls).length === 0, 'mismatched password/confirm never POSTs');
  assert(els['bootstrap-error'].textContent.indexOf('match') !== -1,
    'mismatched password/confirm shows an error');
}

const asyncChecks = [];

asyncChecks.push((function () {
  // A bootstrap reached via app.js's gate redirect (/login?return=<page>)
  // must keep return= on the reload to the login form, or the operator
  // lands on the Dashboard instead of the page they were gated from.
  const { ctx, getNavigatedTo } = makeContext({
    search: '?return=%2Fsettings',
    fetchQueue: [{ reject: new Error('page-load session check, not under test') }, { ok: true }],
  });
  vm.runInContext('submitBootstrap', ctx)(fakeEvt());
  return flush().then(function () {
    assert(getNavigatedTo() === '/login?return=%2Fsettings',
      'bootstrap success keeps ?return= on the reload to the login form');
  });
})());

asyncChecks.push((function () {
  const { ctx, getNavigatedTo } = makeContext({
    fetchQueue: [{ reject: new Error('page-load session check, not under test') }, { ok: true }],
  });
  vm.runInContext('submitBootstrap', ctx)(fakeEvt());
  return flush().then(function () {
    assert(getNavigatedTo() === '/login', 'bootstrap success with no query string reloads plain /login');
  });
})());

asyncChecks.push((function () {
  // fetchQueue order: page-load session check (rejects, no entry queued for
  // it -- handled by the page's own .catch), then the bootstrap POST itself.
  const { ctx, fetchCalls } = makeContext({
    fetchQueue: [{ reject: new Error('page-load session check, not under test') }, { ok: true }],
  });
  vm.runInContext('submitBootstrap', ctx)(fakeEvt());
  // submitBootstrap's fetch chain resolves on a microtask -- flush before asserting.
  return flush().then(function () {
    const calls = bootstrapCalls(fetchCalls);
    assert(calls.length === 1 && calls[0].init.method === 'POST',
      'a matching password posts to /api/auth/bootstrap_password');
  });
})());

asyncChecks.push((function () {
  // 409 = someone else configured the admin credential in the meantime --
  // must reload, not show a stale bootstrap form or a raw error string.
  const { ctx, getReloaded } = makeContext({
    fetchQueue: [{ reject: new Error('page-load session check, not under test') },
                 { ok: false, status: 409, text: 'conflict' }],
  });
  vm.runInContext('submitBootstrap', ctx)(fakeEvt());
  return flush().then(function () {
    assert(getReloaded(), 'a 409 (race: credential already set) reloads the page rather than showing an error');
  });
})());

// ---------------------------------------------------------------------------
// Group 3: the page-load toggle between the two forms follows
// GET /api/auth/session's bootstrap_needed exactly -- this is the actual
// "gate" a locked-out owner depends on to ever see the bootstrap form at
// all, so it gets its own coverage separate from submitBootstrap's own
// validation/POST behavior above.
// ---------------------------------------------------------------------------
asyncChecks.push((function () {
  const { els } = makeContext({ fetchQueue: [{ ok: true, json: { bootstrap_needed: true } }] });
  return flush().then(function () {
    assert(els['bootstrap-form'].hidden === false && els['login-form'].hidden === true,
      'bootstrap_needed:true shows the bootstrap form and hides the login form');
  });
})());

asyncChecks.push((function () {
  const { els } = makeContext({ fetchQueue: [{ ok: true, json: { bootstrap_needed: false } }] });
  return flush().then(function () {
    assert(els['bootstrap-form'].hidden === true && els['login-form'].hidden === false,
      'bootstrap_needed:false shows the login form and hides the bootstrap form');
  });
})());

asyncChecks.push((function () {
  // A failed/unreachable session check must fail CLOSED to the ordinary
  // login form, never fail open into exposing the bootstrap form to
  // whoever happens to load the page.
  const { els } = makeContext({ fetchQueue: [{ reject: new Error('network error') }] });
  return flush().then(function () {
    assert(els['bootstrap-form'].hidden === true && els['login-form'].hidden === false,
      'a failed session check fails closed to the ordinary login form');
  });
})());

// ---------------------------------------------------------------------------
// Group 4: markup defaults. The element stubs above start every form at
// hidden:false, so they cannot see the real page's initial state -- assert
// it against the HTML source directly. A session check that never resolves
// (board accepts the connection then stalls: neither .then nor .catch runs)
// leaves whatever the markup says, so the login form must NOT be hidden
// there and the bootstrap form MUST be.
// ---------------------------------------------------------------------------
{
  const loginTag = SRC.match(/<form id="login-form"[^>]*>/);
  const bootstrapTag = SRC.match(/<form id="bootstrap-form"[^>]*>/);
  assert(!!loginTag && loginTag[0].indexOf('hidden') === -1,
    'login form is NOT hidden in the markup (fail-closed default if the session check never resolves)');
  assert(!!bootstrapTag && bootstrapTag[0].indexOf('hidden') !== -1,
    'bootstrap form IS hidden in the markup until the board says bootstrap_needed');
}

Promise.all(asyncChecks).then(finish);

function finish() {
  console.log('');
  console.log('login bootstrap form: ' + passed + '/' + (passed + failed) + ' passed');
  if (failed > 0) {
    console.log('FAILURES:');
    failures.forEach(function (f) { console.log('  ' + f); });
    process.exitCode = 1;
  }
}
