/* Node-only test harness for app.js's global fetch auth wrapper -- owner
 * report, 2026-09-24 (verbatim): "When I open the webpage, don't show the
 * login until I do something that would require it ... a pop-up on the
 * screen that allows me to cancel the action."
 *
 * Covers the client-side half of that change:
 *   - a 401 (no session) opens the shared login modal instead of navigating
 *     the tab away, retries the ORIGINAL request once on success, and
 *     rejects with a distinguishable `AuthCancelled` error on Cancel;
 *   - a 403 insufficient_role does the same (unified contract, 2026-09-24 --
 *     it used to resolve with the raw 403 on cancel instead of rejecting);
 *   - concurrent 401/403s share ONE modal (the `pendingLogin` singleton);
 *   - a caller that already handled auth itself (kcOtaAuthedFetch's
 *     __kcCallerHandlesAuth) or is retrying (__kcAuthRetried) is never
 *     re-intercepted, so no infinite loop and no double-signing;
 *   - (2026-09-24 review) a BACKGROUND refusal -- no user gesture, on the
 *     dashboard, or after the operator already declined on this page --
 *     never opens the modal; it rejects quietly (AuthCancelled,
 *     prompted:false). kcRequestIsUserInitiated/kcPageIsDashboard are DOM/
 *     location helpers outside the extracted range and are stubbed here.
 *
 * Same extraction-by-marker-line approach as test_recovery_banner.js. The
 * modal's own DOM (buildLoginModal/openLoginModal -- focus trap, Escape,
 * backdrop, theme CSS classes) is deliberately NOT re-implemented here: it
 * has no board/browser to click-test in this environment either, so this
 * file stubs `openLoginModal` with a scripted queue of outcomes and checks
 * the WRAPPER's contract around it, which is what every page actually
 * depends on. The real modal markup/CSS was reviewed by hand (see the
 * accompanying commit message) against theme.css's `.kc-login-*` rules.
 *
 * Run: node firmware/KilnFW/App/test/test_login_auth_wrapper.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const APP_JS_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'app.js');
const SRC = fs.readFileSync(APP_JS_PATH, 'utf8');
const LINES = SRC.split('\n');

function extractRange(startMarker, endMarkerExclusive) {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l) === startMarker);
  if (startIdx === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const endIdx = LINES.findIndex((l, i) => i > startIdx && raw(l) === endMarkerExclusive);
  if (endIdx === -1) throw new Error('end marker not found after start: ' + JSON.stringify(endMarkerExclusive));
  return LINES.slice(startIdx, endIdx).join('\n');
}

// Range A: nativeFetch + isAuthExemptUrl (pure logic, no DOM).
const RANGE_A = extractRange(
  '  var nativeFetch = window.fetch.bind(window);',
  '  var loginModalEl = null, loginTitleEl = null, loginUserEl = null, loginPassEl = null,'
);
// Range B: pendingLogin/ensureAdminLogin + the window.fetch wrapper itself.
// Deliberately EXCLUDES buildLoginModal/openLoginModal -- those are DOM-heavy
// and stubbed via ctx.openLoginModal below instead (see file header).
const RANGE_B = extractRange(
  '  var pendingLogin = null;',
  '  // ---- kcFetchWithSafetyAck ------------------------------------------'
);
const SRC_UNDER_TEST = RANGE_A + '\n' + RANGE_B;
// Range C (2026-09-24 second review): the REAL gesture bookkeeping that
// ranges A/B stub out -- kcNoteGesture, kcRequestIsUserInitiated, the
// window.confirm hook and kcPageIsDashboard -- run against a fake document,
// clock and timer queue.
const RANGE_C = extractRange(
  '  var kcGestureActive = false, kcLastGestureAt = 0;',
  '  // Only one modal in flight at a time -- if a second 403 arrives while the'
);

function assert_sanity() {
  if (SRC_UNDER_TEST.indexOf('function ensureAdminLogin(') === -1) {
    throw new Error('sanity: extracted range does not include ensureAdminLogin');
  }
  if (SRC_UNDER_TEST.indexOf('window.fetch = function') === -1) {
    throw new Error('sanity: extracted range does not include the window.fetch wrapper');
  }
  if (SRC_UNDER_TEST.indexOf('function buildLoginModal') !== -1) {
    throw new Error('sanity: extraction accidentally pulled in buildLoginModal (DOM-heavy, should be stubbed)');
  }
}
assert_sanity();

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// makeContext: `loginOutcomes` is a queue of true/false consumed in order,
// one per openLoginModal() call -- the stand-in for an operator typing a
// correct password (true) or hitting Cancel/Escape/backdrop (false), all of
// which openLoginModal already collapses to the same `false` resolution
// (see app.js's real onKeydown/onCancel, both calling finish(false)).
// `fetchImpl` answers every nativeFetch() call in sequence.
function makeContext(opts) {
  opts = opts || {};
  const openLoginModalCalls = [];
  const loginOutcomes = (opts.loginOutcomes || []).slice();
  const fetchQueue = (opts.fetchResponses || []).slice();
  const fetchCalls = [];
  function fakeResp(spec) {
    const headers = spec.headers || {};
    return {
      ok: spec.status >= 200 && spec.status < 300,
      status: spec.status,
      headers: { get: function (k) { return Object.prototype.hasOwnProperty.call(headers, k) ? headers[k] : null; } },
      json: function () { return Promise.resolve(spec.json || {}); },
      text: function () { return Promise.resolve(spec.text || ''); },
    };
  }
  // `state` is mutable by a test between calls: userInitiated is what the
  // stubbed kcRequestIsUserInitiated() answers, dashboard what
  // kcPageIsDashboard() answers. Defaults keep the pre-review behaviour
  // (every request user-initiated, not on the dashboard).
  const state = {
    userInitiated: opts.userInitiated === undefined ? true : opts.userInitiated,
    dashboard: !!opts.dashboard,
    methodsSeen: [],
  };
  const ctx = {
    kcRequestIsUserInitiated: function (method) {
      state.methodsSeen.push(method);
      return state.userInitiated;
    },
    kcPageIsDashboard: function () { return state.dashboard; },
    window: {
      fetch: function (input, init) {
        fetchCalls.push({ input: input, init: init });
        const next = fetchQueue.shift();
        if (!next) return Promise.reject(new Error('no fetch fixture queued'));
        if (next.reject) return Promise.reject(new Error(next.reject));
        return Promise.resolve(fakeResp(next));
      },
    },
    Request: undefined, // not exercised by these tests (plain string URLs only)
    openLoginModal: function (titleText) {
      openLoginModalCalls.push(titleText);
      const outcome = loginOutcomes.length ? loginOutcomes.shift() : false;
      return Promise.resolve(outcome);
    },
    console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(SRC_UNDER_TEST, ctx);
  return { ctx, fetchCalls, openLoginModalCalls, state };
}

function flush() {
  return new Promise((resolve) => setImmediate(resolve)).then(() => new Promise((r) => setImmediate(r)));
}

(async () => {
  // -------------------------------------------------------------------
  // Group 1: a 401 opens the modal (never a bare 401 handed back, never a
  // navigation) and retries the SAME request once on a successful login.
  // -------------------------------------------------------------------
  {
    const { ctx, fetchCalls, openLoginModalCalls } = makeContext({
      loginOutcomes: [true],
      fetchResponses: [{ status: 401 }, { status: 200, json: { ok: true } }],
    });
    const result = await ctx.window.fetch('/api/zones');
    assert(openLoginModalCalls.length === 1, '401: opens exactly one login modal');
    assert(fetchCalls.length === 2, '401 + successful login: retries the original request once');
    assert(fetchCalls[1].input === '/api/zones', 'retry re-sends the SAME url');
    assert(fetchCalls[1].init && fetchCalls[1].init.__kcAuthRetried === true,
      'retry is marked __kcAuthRetried so a second failure does not loop');
    assert(result.ok === true && result.status === 200, '401 case resolves to the retried response on success');
  }

  // -------------------------------------------------------------------
  // Group 2: Cancel on a 401's modal rejects with AuthCancelled -- never
  // resolves with the raw 401, never retries.
  // -------------------------------------------------------------------
  {
    const { ctx, fetchCalls } = makeContext({
      loginOutcomes: [false],
      fetchResponses: [{ status: 401 }],
    });
    let caught = null;
    try {
      await ctx.window.fetch('/api/zones');
    } catch (e) {
      caught = e;
    }
    assert(!!caught, '401 + Cancel: the returned promise REJECTS (not resolves)');
    assert(caught && caught.name === 'AuthCancelled', '401 + Cancel: rejection is named AuthCancelled');
    assert(fetchCalls.length === 1, '401 + Cancel: never retries the request');
  }

  // -------------------------------------------------------------------
  // Group 3: 403 insufficient_role -- same modal, same retry-once, and
  // (2026-09-24 unification) Cancel now also rejects AuthCancelled rather
  // than resolving with the raw 403.
  // -------------------------------------------------------------------
  {
    const { ctx, openLoginModalCalls } = makeContext({
      loginOutcomes: [true],
      fetchResponses: [
        { status: 403, headers: { 'X-Kiln-Auth-Reason': 'insufficient_role' } },
        { status: 200, json: { ok: true } },
      ],
    });
    const result = await ctx.window.fetch('/api/settings', { method: 'POST' });
    assert(openLoginModalCalls[0] === 'Administrator login required',
      '403 insufficient_role: modal titled for an admin escalation, distinct from the 401 case');
    assert(result.ok === true, '403 insufficient_role + successful login: retried request resolves');
  }
  {
    const { ctx } = makeContext({
      loginOutcomes: [false],
      fetchResponses: [{ status: 403, headers: { 'X-Kiln-Auth-Reason': 'insufficient_role' } }],
    });
    let caught = null;
    try {
      await ctx.window.fetch('/api/settings', { method: 'POST' });
    } catch (e) {
      caught = e;
    }
    assert(caught && caught.name === 'AuthCancelled',
      '403 insufficient_role + Cancel: rejects AuthCancelled (unified with the 401 contract)');
  }

  // -------------------------------------------------------------------
  // Group 4: a 403 with NO X-Kiln-Auth-Reason header (an unrelated 403 --
  // OTA verify failure, kiln_auth namespace refusal, etc.) is handed back
  // untouched: no modal, no rejection.
  // -------------------------------------------------------------------
  {
    const { ctx, openLoginModalCalls } = makeContext({
      fetchResponses: [{ status: 403 }],
    });
    const result = await ctx.window.fetch('/api/ota/esp/verify');
    assert(openLoginModalCalls.length === 0, 'unrelated 403 (no insufficient_role header): no modal opened');
    assert(result.status === 403, 'unrelated 403: handed back untouched');
  }

  // -------------------------------------------------------------------
  // Group 5: concurrent 401s share ONE modal -- the pendingLogin singleton.
  // Two calls racing in before either resolves must not open two modals.
  // -------------------------------------------------------------------
  {
    const { ctx, openLoginModalCalls } = makeContext({
      loginOutcomes: [true],
      fetchResponses: [
        { status: 401 }, { status: 401 },
        { status: 200, json: {} }, { status: 200, json: {} },
      ],
    });
    const p1 = ctx.window.fetch('/api/a');
    const p2 = ctx.window.fetch('/api/b');
    await Promise.all([p1, p2]);
    assert(openLoginModalCalls.length === 1, 'two concurrent 401s share exactly one login modal');
  }

  // -------------------------------------------------------------------
  // Group 6: __kcCallerHandlesAuth (kcOtaAuthedFetch's opt-out) is never
  // re-intercepted -- the 403 goes straight back so its own signed-retry
  // logic can run instead.
  // -------------------------------------------------------------------
  {
    const { ctx, openLoginModalCalls } = makeContext({
      fetchResponses: [{ status: 403, headers: { 'X-Kiln-Auth-Reason': 'insufficient_role' } }],
    });
    const result = await ctx.window.fetch('/api/ota/esp/flash', { __kcCallerHandlesAuth: true });
    assert(openLoginModalCalls.length === 0, '__kcCallerHandlesAuth: this wrapper stands down, no modal');
    assert(result.status === 403, '__kcCallerHandlesAuth: raw 403 handed back to the caller');
  }

  // -------------------------------------------------------------------
  // Group 7: isAuthExemptUrl -- the login/bootstrap/session routes
  // themselves are never intercepted (so a wrong password reports back to
  // the SAME form, not a second modal on top of it).
  // -------------------------------------------------------------------
  {
    const { ctx, openLoginModalCalls } = makeContext({
      fetchResponses: [{ status: 401 }],
    });
    const result = await ctx.window.fetch('/api/auth/login', { method: 'POST' });
    assert(openLoginModalCalls.length === 0, '/api/auth/login itself: never intercepted, even on 401');
    assert(result.status === 401, '/api/auth/login itself: raw response handed straight back');
  }
  {
    const { ctx, openLoginModalCalls } = makeContext({
      fetchResponses: [{ status: 401 }],
    });
    const result = await ctx.window.fetch('/api/auth/logout', { method: 'POST' });
    assert(openLoginModalCalls.length === 0 && result.status === 401,
      '/api/auth/logout on an expired session: never asks to sign in just to sign out');
  }

  // -------------------------------------------------------------------
  // Group 8 (review, 2026-09-24): retry is ONE-SHOT. A retry that is itself
  // refused (401 again, or a logged-in viewer still lacking the role) hands
  // the raw response back -- no second modal, no third request.
  // -------------------------------------------------------------------
  {
    const { ctx, fetchCalls, openLoginModalCalls } = makeContext({
      loginOutcomes: [true, true],
      fetchResponses: [{ status: 401 }, { status: 401 }, { status: 200 }],
    });
    const result = await ctx.window.fetch('/api/zones');
    assert(openLoginModalCalls.length === 1, 'retry refused again (401): no second modal');
    assert(fetchCalls.length === 2, 'retry refused again (401): exactly one retry, never a second');
    assert(result.status === 401, 'retry refused again (401): raw 401 handed back');
  }
  {
    const denied = { status: 403, headers: { 'X-Kiln-Auth-Reason': 'insufficient_role' } };
    const { ctx, fetchCalls, openLoginModalCalls } = makeContext({
      loginOutcomes: [true, true],
      fetchResponses: [denied, denied, { status: 200 }],
    });
    const result = await ctx.window.fetch('/api/zones', { method: 'POST' });
    assert(openLoginModalCalls.length === 1, 'viewer still insufficient after login: no second modal (no loop)');
    assert(fetchCalls.length === 2, 'viewer still insufficient after login: exactly one retry');
    assert(result.status === 403, 'viewer still insufficient after login: raw 403 handed back');
  }

  // -------------------------------------------------------------------
  // Group 9: background refusal on the dashboard never opens the modal.
  // -------------------------------------------------------------------
  {
    const { ctx, fetchCalls, openLoginModalCalls } = makeContext({
      userInitiated: false, dashboard: true,
      loginOutcomes: [true],
      fetchResponses: [{ status: 401 }, { status: 403, headers: { 'X-Kiln-Auth-Reason': 'insufficient_role' } }],
    });
    let c1 = null, c2 = null;
    try { await ctx.window.fetch('/api/autotune'); } catch (e) { c1 = e; }
    try { await ctx.window.fetch('/api/zones'); } catch (e) { c2 = e; }
    assert(openLoginModalCalls.length === 0, 'dashboard background 401/403: no modal on load or poll');
    assert(c1 && c1.name === 'AuthCancelled' && c1.prompted === false,
      'dashboard background 401: rejects quietly (AuthCancelled, prompted:false)');
    assert(c2 && c2.name === 'AuthCancelled' && c2.prompted === false,
      'dashboard background 403 insufficient_role: rejects quietly');
    assert(fetchCalls.length === 2, 'dashboard background refusal: never retried');
  }
  {
    // ...but a click on the dashboard does prompt.
    const { ctx, openLoginModalCalls } = makeContext({
      userInitiated: true, dashboard: true,
      loginOutcomes: [true],
      fetchResponses: [{ status: 401 }, { status: 200 }],
    });
    const r = await ctx.window.fetch('/api/profile_exec/start', { method: 'POST' });
    assert(openLoginModalCalls.length === 1 && r.status === 200, 'dashboard user action: modal opens, retried on login');
  }

  // -------------------------------------------------------------------
  // Group 10: on a gated page the operator navigated to, a background load
  // may prompt ONCE; after Cancel no background refusal re-prompts, but an
  // explicit action still does.
  // -------------------------------------------------------------------
  {
    const { ctx, openLoginModalCalls, state } = makeContext({
      userInitiated: false, dashboard: false,
      loginOutcomes: [false, true],
      fetchResponses: [{ status: 401 }, { status: 401 }, { status: 401 }, { status: 200 }],
    });
    let c1 = null, c2 = null;
    try { await ctx.window.fetch('/api/zones'); } catch (e) { c1 = e; }
    assert(openLoginModalCalls.length === 1, 'gated page load: background refusal prompts once');
    assert(c1 && c1.name === 'AuthCancelled' && c1.prompted === true, 'gated page load + Cancel: AuthCancelled, prompted:true');
    try { await ctx.window.fetch('/api/zones'); } catch (e) { c2 = e; }
    assert(openLoginModalCalls.length === 1, 'after Cancel: the next background poll does NOT re-prompt');
    assert(c2 && c2.name === 'AuthCancelled' && c2.prompted === false, 'after Cancel: background poll rejects quietly');
    state.userInitiated = true;
    const r = await ctx.window.fetch('/api/zones', { method: 'POST' });
    assert(openLoginModalCalls.length === 2 && r.status === 200, 'after Cancel: an explicit action still prompts');
  }

  // -------------------------------------------------------------------
  // Group 11: a background refusal that lands while a modal is already open
  // joins that login instead of failing, and the method is sampled at call
  // time.
  // -------------------------------------------------------------------
  {
    const { ctx, openLoginModalCalls, state } = makeContext({
      userInitiated: true, dashboard: true,
      loginOutcomes: [true],
      fetchResponses: [{ status: 401 }, { status: 401 }, { status: 200 }, { status: 200 }],
    });
    const p1 = ctx.window.fetch('/api/profile_exec/start', { method: 'POST' });
    state.userInitiated = false;
    const p2 = ctx.window.fetch('/api/autotune');
    const both = await Promise.all([p1, p2]);
    assert(openLoginModalCalls.length === 1 && both[0].status === 200 && both[1].status === 200,
      'background refusal during an open modal joins the same login');
    assert(state.methodsSeen[0] === 'POST' && state.methodsSeen[1] === 'GET',
      'request method is passed to the user-initiated check at call time (default GET)');
  }

  // -------------------------------------------------------------------
  // Group 12: __kcUserAction marks a read the operator's own action issued
  // outside any gesture dispatch (the dashboard long-press PID popup): it
  // prompts on the dashboard where an unmarked background read stays quiet.
  // -------------------------------------------------------------------
  {
    const { ctx, fetchCalls, openLoginModalCalls } = makeContext({
      userInitiated: false, dashboard: true,
      loginOutcomes: [true],
      fetchResponses: [{ status: 401 }, { status: 200 }, { status: 401 }],
    });
    let r = null;
    try { r = await ctx.window.fetch('/api/zones', { __kcUserAction: true }); } catch (e) { r = null; }
    assert(openLoginModalCalls.length === 1 && r && r.status === 200 && fetchCalls.length === 2,
      '__kcUserAction read on the dashboard: modal opens, retried once on login');
    let c = null;
    try { await ctx.window.fetch('/api/zones'); } catch (e) { c = e; }
    assert(openLoginModalCalls.length === 1 && c && c.name === 'AuthCancelled' && c.prompted === false,
      'unmarked background read on the dashboard: still quiet');
  }

  // -------------------------------------------------------------------
  // Group 13: the real gesture rules (range C), not the stub.
  // -------------------------------------------------------------------
  {
    let clock = 1000000;
    const listeners = {};
    const timers = [];
    let nativeConfirmCalls = 0;
    const gctx = {
      Date: { now: function () { return clock; } },
      setTimeout: function (fn) { timers.push(fn); return timers.length; },
      document: { addEventListener: function (type, fn) { (listeners[type] = listeners[type] || []).push(fn); } },
      window: {
        location: { pathname: '/' },
        confirm: function () { nativeConfirmCalls++; return true; },
      },
      String: String,
    };
    vm.createContext(gctx);
    vm.runInContext('var loginModalEl = null;\nvar forgotModalEl = null;\n' + RANGE_C +
      '\nthis.__isUser = kcRequestIsUserInitiated; this.__isDash = kcPageIsDashboard;', gctx);
    const isUser = gctx.__isUser;
    const runTimers = () => { while (timers.length) timers.shift()(); };
    assert(!isUser('GET') && !isUser('POST'), 'gesture rules: nothing is user-initiated before any gesture');
    listeners.click.forEach((fn) => fn({ target: {} }));
    assert(isUser('GET') && isUser('POST'), 'gesture rules: any request during click dispatch is user-initiated');
    runTimers();
    assert(!isUser('GET'), 'gesture rules: a GET after the dispatch ends is not');
    clock += 2999;
    assert(isUser('POST'), 'gesture rules: a POST within 3 s of a click is');
    clock += 2;
    assert(!isUser('POST'), 'gesture rules: a POST more than 3 s after a click is not');
    clock += 60000;
    const answer = gctx.window.confirm('Start firing now?');
    assert(answer === true && nativeConfirmCalls === 1, 'confirm hook: native dialog called, answer passed through');
    clock += 500;
    assert(isUser('POST'), 'confirm hook: a POST right after answering confirm() is user-initiated (dashboard Start)');
    assert(!isUser('GET'), 'confirm hook: a GET after confirm() is still not');
    assert(gctx.__isDash() === true, 'kcPageIsDashboard: "/" is the dashboard');
    gctx.window.location.pathname = '/settings/zones';
    assert(gctx.__isDash() === false, 'kcPageIsDashboard: a gated page is not');
  }

  // -------------------------------------------------------------------
  // Group 14 (second reviewer, 2026-09-24): kcNoteGesture must ignore
  // events inside the forgot-password reset modal too, not just
  // loginModalEl -- typing a TOTP code in that modal is not a new page
  // action and must not restart the 3 s write window.
  // -------------------------------------------------------------------
  {
    let clock2 = 5000000;
    const listeners2 = {};
    const timers2 = [];
    const fakeTarget = {};
    const fakeForgotModal = { contains: function (el) { return el === fakeTarget; } };
    const gctx2 = {
      Date: { now: function () { return clock2; } },
      setTimeout: function (fn) { timers2.push(fn); return timers2.length; },
      document: { addEventListener: function (type, fn) { (listeners2[type] = listeners2[type] || []).push(fn); } },
      window: { location: { pathname: '/' }, confirm: function () { return true; } },
      String: String,
    };
    vm.createContext(gctx2);
    vm.runInContext('var loginModalEl = null;\nvar forgotModalEl = null;\n' + RANGE_C +
      '\nthis.__isUser2 = kcRequestIsUserInitiated;' +
      '\nthis.__setForgotModal = function (m) { forgotModalEl = m; };', gctx2);
    gctx2.__setForgotModal(fakeForgotModal);
    listeners2.keydown.forEach((fn) => fn({ target: fakeTarget }));
    assert(!gctx2.__isUser2('POST'),
      'kcNoteGesture: a keydown inside the reset modal does not restart the write window');
    listeners2.keydown.forEach((fn) => fn({ target: {} }));
    assert(gctx2.__isUser2('POST'),
      'sanity: a keydown outside the reset modal still counts as a gesture');
  }

  console.log('');
  console.log(passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
})();
