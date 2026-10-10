/* Node-only test harness for app.js's forgot-password modal flow state
 * machine (TOTP password reset, WT-B, 2026-09-24).
 *
 * Extracts the forgotModalEl/buildForgotModal/openForgotPasswordModal block
 * from app.js by exact marker lines (same approach as
 * test_login_auth_wrapper.js) and runs it against a minimal fake DOM plus a
 * stubbed nativeFetch/loginActiveCtl/document, without a real browser.
 *
 * Covers:
 *   - step1 (username+code) POSTs /api/auth/forgot; a 202 with a
 *     reset_token advances to step2, a 429 shows the plain-text body
 *     verbatim (never parsed as JSON), a 503 shows the clock-not-synced
 *     message, any other status shows the SAME generic failure text (no
 *     enrolled/not-enrolled oracle);
 *   - step2 (new password x2) refuses locally on a mismatch without
 *     calling fetch at all; POSTs /api/auth/reset with the held token;
 *     {ok:true} closes the modal and resumes the (still pending, suspended)
 *     login modal via loginActiveCtl; anything else returns to step1 with
 *     the generic failure text, since the token is single-use -- it is
 *     nulled the moment the /reset POST is built, never re-sent;
 *   - Escape is ignored (no preventDefault) while the modal is hidden --
 *     the document-level keydown listener lives for the page's lifetime;
 *   - a response that lands after Cancel is ignored (no step change, no
 *     token stored, no login resume);
 *   - the code typed in step1 and the reset_token are never handed to
 *     console.log/console.error/console.warn (the modal must not become a
 *     new logging leak for either secret);
 *   - the reset token is never written anywhere resembling localStorage/
 *     sessionStorage (this app.js target has neither global at all -- the
 *     test asserts the code never references them, and that a fresh
 *     context has no such globals for it to reach for);
 *   - Cancel (on either step) and a fresh openForgotPasswordModal() call
 *     both fully reset state: token cleared, fields cleared, back to step1.
 *
 * Run: node firmware/KilnFW/App/test/test_forgot_password_modal.js
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

// Range D: the forgot-password modal subsystem, inserted right after
// openLoginModal() and before the "Which refusals may raise the modal"
// review comment (verified in the implementation session to sit outside
// test_login_auth_wrapper.js's ranges A/B/C).
const RANGE_D = extractRange(
  '  var forgotModalEl = null, forgotPanelEl = null, forgotStep1El = null, forgotStep2El = null,',
  '  // ---- Which refusals may raise the modal (2026-09-24 review) ----------'
);

// Range L: the login modal (buildLoginModal/openLoginModal) that the
// "Forgot password?" link lives in -- used by the suspend/resume group at
// the end, which runs RANGE_L + RANGE_D together so the real
// loginActiveCtl handshake is exercised, not a stub.
const RANGE_L = extractRange(
  '  var loginModalEl = null, loginTitleEl = null, loginUserEl = null, loginPassEl = null,',
  '  // ---- Forgot-password reset flow (docs/TOTP_PASSWORD_RESET_PLAN.md) -----'
);

// Range S: the real kcModalStack/kcFocusFirstEmpty helpers shared by every
// modal (never a hand-copied stand-in, which could drift from app.js).
const RANGE_S = extractRange(
  '  // ---- kcModalStack -------------------------------------------------------',
  '  // ---- kcSafetyTcIsSeparate ----------------------------------------------'
);

// Range C: the themed confirm/alert modal (buildConfirmModal/
// openConfirmModal), used by the stacked-modal Escape group.
const RANGE_C = extractRange(
  '  var confirmModalEl = null, confirmTitleEl = null, confirmTextEl = null,',
  '  // One modal on screen at a time. There is exactly one overlay/panel, so a'
);

const RESET_PW_SRC = SRC.match(/window\.kcResetPasswordProblem = function \(pw, username\) \{[\s\S]*?\r?\n  \};\r?\n/)[0];

if (RANGE_S.indexOf('function kcModalIsTop') === -1 || RANGE_S.indexOf('function kcFocusFirstEmpty') === -1) {
  throw new Error('sanity: extracted range does not include the kcModalStack helpers');
}
if (RANGE_C.indexOf('function openConfirmModal') === -1) {
  throw new Error('sanity: extracted range does not include openConfirmModal');
}
if (RANGE_D.indexOf('function openForgotPasswordModal') === -1) {
  throw new Error('sanity: extracted range does not include openForgotPasswordModal');
}
if (RANGE_D.indexOf('reset_token') === -1) {
  throw new Error('sanity: extracted range does not include reset_token handling');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// ---- minimal fake DOM -----------------------------------------------
// Every element with an id is registered in one flat map; querySelector on
// ANY element resolves '#id' against that same map, which is enough for
// this flat, non-nested modal markup (buildForgotModal never queries by
// tag or class, only by id).
function makeFakeDom() {
  const registry = {};

  function makeElement(tag) {
    const el = {
      tagName: tag,
      children: [],
      attrs: {},
      hidden: false,
      value: '',
      textContent: '',
      disabled: false,
      style: {},
      _listeners: {},
      _innerHTML: '',
      addEventListener(type, fn) {
        (this._listeners[type] = this._listeners[type] || []).push(fn);
      },
      dispatch(type, evt) {
        evt = evt || { preventDefault() {}, target: this };
        (this._listeners[type] || []).forEach((fn) => fn(evt));
      },
      focus() { this._focused = true; },
      hasAttribute(k) { return Object.prototype.hasOwnProperty.call(this.attrs, k); },
      contains() { return false; },
      querySelectorAll() { return []; },
      setAttribute(k, v) {
        this.attrs[k] = v;
        if (k === 'hidden') this.hidden = true;
      },
      removeAttribute(k) {
        delete this.attrs[k];
        if (k === 'hidden') this.hidden = false;
      },
      appendChild(child) { this.children.push(child); },
      querySelector(sel) {
        const m = sel.match(/^#(.+)$/);
        if (m) return registry[m[1]] || null;
        return registry[sel] || null;
      },
      removeEventListener(type, fn) {
        const arr = this._listeners[type] || [];
        const i = arr.indexOf(fn);
        if (i !== -1) arr.splice(i, 1);
      },
      set className(v) { this._className = v; },
      get className() { return this._className; },
      set innerHTML(html) {
        this._innerHTML = html;
        // Register every tag's id="..." (as 'x', for querySelector('#x')),
        // each class (as '.c', first registration wins) and a submit
        // button (as 'button[type="submit"]', first wins) so both modals'
        // lookups resolve. One element per tag, shared across keys.
        const tagRe = /<(\w+)([^>]*)>/g;
        let t;
        while ((t = tagRe.exec(html))) {
          const attrs = t[2];
          const idM = attrs.match(/id="([^"]+)"/);
          const clsM = attrs.match(/class="([^"]+)"/);
          const isSubmit = t[1] === 'button' && /type="submit"/.test(attrs);
          if (!idM && !clsM && !isSubmit) continue;
          let el = (idM && registry[idM[1]]) || makeElement(t[1]);
          if (idM && !registry[idM[1]]) registry[idM[1]] = el;
          if (clsM) clsM[1].split(/\s+/).forEach((c) => { if (!registry['.' + c]) registry['.' + c] = el; });
          if (isSubmit && !registry['button[type="submit"]']) registry['button[type="submit"]'] = el;
        }
      },
      get innerHTML() { return this._innerHTML; },
    };
    return el;
  }

  const documentListeners = {};
  const doc = {
    createElement: (tag) => makeElement(tag),
    body: { appendChild() {} },
    addEventListener(type, fn, capture) {
      (documentListeners[type] = documentListeners[type] || []).push(fn);
    },
    removeEventListener(type, fn) {
      const arr = documentListeners[type] || [];
      const i = arr.indexOf(fn);
      if (i !== -1) arr.splice(i, 1);
    },
    dispatch(type, evt) {
      for (const fn of (documentListeners[type] || []).slice()) {
        fn(evt);
        if (evt && evt._stopped) break;
      }
    },
    activeElement: null,
    _listenerCount(type) { return (documentListeners[type] || []).length; },
  };
  return { registry, document: doc };
}

function fakeResp(spec) {
  return {
    status: spec.status,
    ok: spec.status >= 200 && spec.status < 300,
    headers: { get: () => null },
    json: () => Promise.resolve(spec.json || {}),
    text: () => Promise.resolve(spec.text || ''),
  };
}

function makeContext(opts) {
  opts = opts || {};
  const fetchQueue = (opts.fetchResponses || []).slice();
  const fetchCalls = [];
  const loggedArgs = [];
  const openLoginModalCalls = [];
  const resumeCalls = [];
  const dom = makeFakeDom();

  const fakeConsole = {
    log: (...a) => loggedArgs.push(a),
    error: (...a) => loggedArgs.push(a),
    warn: (...a) => loggedArgs.push(a),
  };

  const ctx = {
    document: dom.document,
    nativeFetch: function (url, init) {
      fetchCalls.push({ url, init });
      const next = fetchQueue.shift();
      if (!next) return Promise.reject(new Error('no fetch fixture queued'));
      return Promise.resolve(fakeResp(next));
    },
    openLoginModal: function (titleText) {
      openLoginModalCalls.push(titleText);
      return Promise.resolve(true);
    },
    // Stand-in for the pending login modal's control handle (app.js's
    // openLoginModal() sets the real one); closing the reset modal must
    // resume it rather than open a second login modal.
    loginActiveCtl: {
      resume: function (noticeText) { resumeCalls.push(noticeText); },
    },
    console: fakeConsole,
    // The real client-side strength rule (web batch W1), extracted from app.js, never a stand-in.
    window: (function () {
      const w = {};
      new Function('window', 'TextEncoder', RESET_PW_SRC)(w, TextEncoder);
      return w;
    })(),
    encodeURIComponent,
    String,
    // Deliberately no localStorage/sessionStorage globals -- the modal
    // code must never reach for them; if it tried, this would throw
    // ReferenceError, which the tests below treat as a failure.
  };
  vm.createContext(ctx);
  // The real shared modal stack goes in first, in every context.
  vm.runInContext(RANGE_S, ctx);
  if (opts.withLogin) {
    // Real login modal: drop the stubs so RANGE_L's own declarations win.
    delete ctx.loginActiveCtl;
    delete ctx.openLoginModal;
    ctx.setTimeout = setTimeout;
    ctx.clearTimeout = clearTimeout;
    ctx.AbortController = AbortController;
    vm.runInContext((opts.withConfirm ? RANGE_C + '\n' : '') + RANGE_L + '\n' + RANGE_D, ctx);
  } else {
    vm.runInContext(RANGE_D, ctx);
  }
  return { ctx, dom, fetchCalls, openLoginModalCalls, resumeCalls, loggedArgs };
}

function flush() {
  return new Promise((r) => setImmediate(r)).then(() => new Promise((r) => setImmediate(r)));
}

(async () => {
  // Group 1: step1 -> 202 with reset_token advances to step2.
  {
    const { ctx, dom, fetchCalls } = makeContext({
      fetchResponses: [{ status: 202, json: { reset_token: 'tok-abc-123' } }],
    });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = '123456';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    assert(fetchCalls.length === 1 && fetchCalls[0].url === '/api/auth/forgot', 'step1 POSTs /api/auth/forgot');
    assert(dom.registry['kc-forgot-step1'].hidden === true, 'step1 hides on success');
    assert(dom.registry['kc-forgot-step2'].hidden === false, 'step2 shows on success');
    assert(dom.registry['kc-forgot-code'].value === '', 'code field cleared once step1 succeeds');
  }

  // Group 2: step1 -> 429 shows the plain-text body verbatim, never JSON-parsed.
  {
    const { ctx, dom } = makeContext({
      fetchResponses: [{ status: 429, text: 'try again in 12s' }],
    });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = '000000';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    assert(dom.registry['kc-forgot-error'].textContent === 'try again in 12s', '429 body shown verbatim');
    assert(dom.registry['kc-forgot-step1'].hidden === false, '429 stays on step1');
  }

  // Group 3: step1 -> 503 shows the clock-not-synced message.
  {
    const { ctx, dom } = makeContext({ fetchResponses: [{ status: 503 }] });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = '000000';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    assert(/clock is not synced/i.test(dom.registry['kc-forgot-error'].textContent), '503 shows clock-not-synced text');
  }

  // Group 4: step1 -> 400 (bad code) shows the SAME generic text as any
  // other failure -- never distinguishes enrolled vs. not-enrolled.
  {
    const { ctx, dom } = makeContext({ fetchResponses: [{ status: 400 }] });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'someone-not-enrolled';
    dom.registry['kc-forgot-code'].value = '000000';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    const generic = dom.registry['kc-forgot-error'].textContent;
    const { ctx: ctx2, dom: dom2 } = makeContext({ fetchResponses: [{ status: 400 }] });
    ctx2.openForgotPasswordModal();
    dom2.registry['kc-forgot-username'].value = 'bench';
    dom2.registry['kc-forgot-code'].value = '111111';
    dom2.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    assert(generic === dom2.registry['kc-forgot-error'].textContent && generic.length > 0,
      '400 gives identical generic text regardless of username (no enumeration oracle)');
  }

  // Group 5: step2 -- local mismatch never calls fetch.
  {
    const { ctx, dom, fetchCalls } = makeContext({});
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-newpass'].value = 'aaaaaaaa';
    dom.registry['kc-forgot-newpass2'].value = 'bbbbbbbb';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    await flush();
    assert(fetchCalls.length === 0, 'mismatched passwords never call fetch');
    assert(dom.registry['kc-forgot-error2'].textContent === 'Passwords do not match.', 'mismatch shows local message');
  }

  // Group 6: step2 -- no held token (e.g. modal reopened) refuses locally and
  // returns to step1, never calling fetch either.
  {
    const { ctx, dom, fetchCalls } = makeContext({});
    ctx.openForgotPasswordModal(); // forgotResetToken is null right after open
    dom.registry['kc-forgot-newpass'].value = 'Samepass-1x9';
    dom.registry['kc-forgot-newpass2'].value = 'Samepass-1x9';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    await flush();
    assert(fetchCalls.length === 0, 'step2 with no held token never calls fetch');
    assert(dom.registry['kc-forgot-step1'].hidden === false, 'step2 with no token falls back to step1');
  }

  // Group 7: full happy path -- step1 202, step2 200/{ok:true} closes the
  // modal (token cleared) and resumes the pending login modal (never opens
  // a second one, which would stack a second set of submit listeners).
  {
    const { ctx, dom, openLoginModalCalls, resumeCalls, fetchCalls } = makeContext({
      fetchResponses: [
        { status: 202, json: { reset_token: 'tok-xyz' } },
        { status: 200, json: { ok: true } },
      ],
    });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = '654321';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    dom.registry['kc-forgot-newpass'].value = 'NewSecret-pw99';
    dom.registry['kc-forgot-newpass2'].value = 'NewSecret-pw99';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    await flush();
    assert(resumeCalls.length === 1 && /reset/i.test(resumeCalls[0] || ''),
      'success resumes the pending login modal with a notice');
    assert(openLoginModalCalls.length === 0, 'success never opens a second login modal');
    assert(ctx.forgotResetToken === null, 'reset token cleared after success (closeForgotModal)');
    assert(fetchCalls.length === 2 && /reset_token=tok-xyz/.test(fetchCalls[1].init.body),
      'reset POST carries the token from step1');
    assert(dom.registry['kc-forgot-username'].value === '', 'close clears the username field');
  }

  // Group 7b: the token is single-use client-side -- a failed /reset
  // returns to step1 with the token already gone, and a second step2
  // submit never re-sends it.
  {
    const { ctx, dom, fetchCalls } = makeContext({
      fetchResponses: [
        { status: 202, json: { reset_token: 'tok-once' } },
        { status: 400, json: { ok: false } },
      ],
    });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = '654321';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    dom.registry['kc-forgot-newpass'].value = 'Sh0rt-but-ok-1';
    dom.registry['kc-forgot-newpass2'].value = 'Sh0rt-but-ok-1';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    assert(ctx.forgotResetToken === null, 'token nulled as soon as the /reset POST is sent');
    await flush();
    assert(dom.registry['kc-forgot-step1'].hidden === false, 'failed reset returns to step1');
    assert(dom.registry['kc-forgot-newpass'].value === '', 'failed reset clears new-password field');
    dom.registry['kc-forgot-newpass'].value = 'Sh0rt-but-ok-1';
    dom.registry['kc-forgot-newpass2'].value = 'Sh0rt-but-ok-1';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    await flush();
    const resets = fetchCalls.filter((c) => c.url === '/api/auth/reset');
    assert(resets.length === 1, 'token never sent twice (' + resets.length + ' reset POSTs)');
  }

  // Group 7a (web batch W1): a weak password is refused locally with the token UNSPENT, so the
  // operator can retype and succeed with the same code/token (the firmware now checks strength
  // before consuming the token, but answers a weak password with the same generic 400).
  {
    const { ctx, dom, fetchCalls } = makeContext({
      fetchResponses: [
        { status: 202, json: { reset_token: 'tok-keep' } },
        { status: 200, json: { ok: true } },
      ],
    });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = '654321';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    dom.registry['kc-forgot-newpass'].value = 'weak';
    dom.registry['kc-forgot-newpass2'].value = 'weak';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    await flush();
    assert(fetchCalls.filter((c) => c.url === '/api/auth/reset').length === 0, 'weak password never POSTs /reset');
    assert(ctx.forgotResetToken === 'tok-keep', 'weak password leaves the reset token unspent');
    assert(/too short/i.test(dom.registry['kc-forgot-error2'].textContent), 'weak password explains why');
    assert(dom.registry['kc-forgot-step2'].hidden === false, 'weak password stays on step 2');
    dom.registry['kc-forgot-newpass'].value = 'Str0ng-enough-pw';
    dom.registry['kc-forgot-newpass2'].value = 'Str0ng-enough-pw';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    await flush();
    const rs = fetchCalls.filter((c) => c.url === '/api/auth/reset');
    assert(rs.length === 1 && /reset_token=tok-keep/.test(rs[0].init.body), 'retry with a strong password sends the SAME token');
  }

  // Group 7c: the page-lifetime keydown listener is inert while hidden.
  {
    const { ctx, dom, resumeCalls } = makeContext({});
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-cancel1'].dispatch('click');
    const before = resumeCalls.length;
    let prevented = false;
    dom.document.dispatch('keydown', { key: 'Escape', preventDefault() { prevented = true; } });
    assert(!prevented, 'Escape not swallowed while the modal is hidden');
    assert(resumeCalls.length === before, 'hidden-modal Escape does not resume the login modal again');
    ctx.openForgotPasswordModal();
    prevented = false;
    dom.document.dispatch('keydown', { key: 'Escape', preventDefault() { prevented = true; } });
    assert(prevented && ctx.forgotModalEl.hidden === true, 'Escape closes the modal while visible');
  }

  // Group 7d: a /forgot response landing after Cancel is ignored.
  {
    let release;
    const { ctx, dom, resumeCalls } = makeContext({});
    ctx.nativeFetch = function () {
      return new Promise((r) => { release = r; });
    };
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = '123123';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    dom.registry['kc-forgot-cancel1'].dispatch('click');
    release(fakeResp({ status: 202, json: { reset_token: 'late-token' } }));
    await flush();
    assert(ctx.forgotResetToken === null, 'late /forgot response after Cancel stores no token');
    assert(dom.registry['kc-forgot-step2'].hidden === true, 'late /forgot response after Cancel does not advance');
    assert(resumeCalls.length === 1, 'Cancel resumed the login modal exactly once');
  }

  // Group 8: Cancel on step1 clears all state.
  {
    const { ctx, dom } = makeContext({
      fetchResponses: [{ status: 202, json: { reset_token: 'should-be-cleared' } }],
    });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = '222222';
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    // now on step2 with a token held
    assert(ctx.forgotResetToken === 'should-be-cleared', 'sanity: token held before cancel');
    dom.registry['kc-forgot-cancel2'].dispatch('click');
    assert(ctx.forgotResetToken === null, 'Cancel clears the held reset token');
    assert(dom.registry['kc-forgot-newpass'].value === '', 'Cancel clears new-password field');
    assert(dom.registry['kc-forgot-step1'].hidden === false, 'Cancel returns to step1');
  }

  // Group 9: neither the typed code nor the reset token are ever logged.
  {
    const secretCode = '999888';
    const secretToken = 'super-secret-reset-token-value';
    const { ctx, dom, loggedArgs } = makeContext({
      fetchResponses: [{ status: 202, json: { reset_token: secretToken } }],
    });
    ctx.openForgotPasswordModal();
    dom.registry['kc-forgot-username'].value = 'bench';
    dom.registry['kc-forgot-code'].value = secretCode;
    dom.registry['kc-forgot-step1'].dispatch('submit');
    await flush();
    const serialized = JSON.stringify(loggedArgs);
    assert(serialized.indexOf(secretCode) === -1, 'typed code never logged to console');
    assert(serialized.indexOf(secretToken) === -1, 'reset token never logged to console');
  }

  // Group 11: the "Forgot password?" link SUSPENDS the pending login
  // promise (real openLoginModal, not a stub): while suspended its
  // keydown handler ignores Escape/Tab, closing the reset modal resumes the
  // same login modal, and the promise still settles normally afterward.
  {
    const { ctx, dom } = makeContext({ withLogin: true });
    let settledWith;
    const p = ctx.openLoginModal('Administrator login required').then((ok) => { settledWith = ok; });
    const loginOverlay = ctx.loginModalEl;
    const kdCount = dom.document._listenerCount('keydown');
    dom.registry['.kc-login-forgot-link'].dispatch('click');
    assert(loginOverlay.hidden === true, 'forgot link hides the login modal');
    assert(ctx.forgotModalEl.hidden === false, 'forgot link shows the reset modal');
    assert(ctx.loginActiveCtl !== null, 'login promise still pending while reset modal is open');
    dom.document.dispatch('keydown', { key: 'Escape', preventDefault() {} });
    await flush();
    assert(settledWith === undefined, 'Escape in the reset modal does not settle the suspended login');
    assert(ctx.forgotModalEl.hidden === true && loginOverlay.hidden === false,
      'Escape closes the reset modal and resumes the login modal');
    dom.registry['.kc-login-forgot-link'].dispatch('click');
    dom.registry['kc-forgot-cancel1'].dispatch('click');
    assert(loginOverlay.hidden === false, 'reset-modal Cancel resumes the login modal');
    dom.registry['.kc-login-cancel'].dispatch('click', { preventDefault() {}, target: null });
    await p;
    assert(settledWith === false, 'login Cancel after a reset detour still settles the original promise');
    assert(ctx.loginActiveCtl === null, 'finish() clears the control handle');
    assert(dom.document._listenerCount('keydown') === kdCount,
      'login keydown listener removed on finish (only the reset modal\'s page-lifetime one remains)');
    dom.registry['.kc-login-forgot-link'].dispatch('click');
    assert(ctx.forgotModalEl.hidden === true, 'forgot link is inert once no login is pending');
  }

  // Group 11b: a SECOND login modal on the same page load. The reset
  // modal's page-lifetime keydown listener was registered during the first
  // login, so for the second one it now runs BEFORE the login's own
  // capture listener: Escape must close only the reset modal, never also
  // fall through to the just-resumed login and cancel it.
  {
    const { ctx, dom } = makeContext({ withLogin: true });
    const p1 = ctx.openLoginModal('Administrator login required');
    dom.registry['.kc-login-forgot-link'].dispatch('click');
    dom.registry['kc-forgot-cancel1'].dispatch('click');
    dom.registry['.kc-login-cancel'].dispatch('click', { preventDefault() {}, target: null });
    await p1;
    let settled2;
    const p2 = ctx.openLoginModal('Administrator login required').then((ok) => { settled2 = ok; });
    dom.registry['.kc-login-forgot-link'].dispatch('click');
    const evt = { key: 'Escape', preventDefault() {}, stopImmediatePropagation() { this._stopped = true; } };
    // Honour stopImmediatePropagation the way a browser would.
    dom.document.dispatch('keydown', evt);
    await flush();
    assert(settled2 === undefined, 'second login: Escape in the reset modal does not cancel the resumed login');
    assert(ctx.forgotModalEl.hidden === true && ctx.loginModalEl.hidden === false,
      'second login: Escape closes the reset modal and resumes the login modal');
    dom.registry['.kc-login-cancel'].dispatch('click', { preventDefault() {}, target: null });
    await p2;
    assert(settled2 === false, 'second login still settles on its own Cancel');
  }

  // Group 12 (ROADMAP.md modal-accessibility follow-up, gap 1 -- focus
  // defaults): username gets focus while it's empty; once it already has a
  // value (a browser autofill, or an operator who typed it and then
  // cancelled) focus goes to password instead of blindly back to
  // username. Password itself is still cleared on every open regardless.
  {
    const { ctx, dom } = makeContext({ withLogin: true });
    ctx.openLoginModal('Administrator login required');
    assert(dom.registry['kc-login-username']._focused === true, 'empty username: initial focus on username');
    assert(!dom.registry['kc-login-password']._focused, 'empty username: password not focused');
    dom.registry['.kc-login-cancel'].dispatch('click', { preventDefault() {}, target: null });
    dom.registry['kc-login-username']._focused = false;
    dom.registry['kc-login-username'].value = 'bench';
    ctx.openLoginModal('Administrator login required');
    assert(dom.registry['kc-login-password']._focused === true, 'pre-filled username: focus jumps to password');
    assert(!dom.registry['kc-login-username']._focused, 'pre-filled username: username itself not re-focused');
    assert(dom.registry['kc-login-password'].value === '', 'password is still cleared on every open regardless of focus target');
  }

  // Group 13 (same follow-up, gap 1 continued): the reset modal's step 1
  // carries over whatever username the operator already typed into the
  // login modal, and focus goes straight to the TOTP code field once that
  // username is known -- never back to a field already filled in.
  {
    const { ctx, dom } = makeContext({ withLogin: true });
    ctx.openLoginModal('Administrator login required');
    dom.registry['.kc-login-forgot-link'].dispatch('click');
    assert(dom.registry['kc-forgot-username'].value === '', 'no login username typed: reset modal opens with username blank');
    assert(dom.registry['kc-forgot-username']._focused === true, 'no login username typed: focus on username');
    assert(!dom.registry['kc-forgot-code']._focused, 'no login username typed: code field not focused');
    dom.registry['kc-forgot-cancel1'].dispatch('click');
    dom.registry['.kc-login-cancel'].dispatch('click', { preventDefault() {}, target: null });

    ctx.openLoginModal('Administrator login required');
    dom.registry['kc-login-username'].value = 'bench';
    dom.registry['kc-forgot-username']._focused = false;
    dom.registry['.kc-login-forgot-link'].dispatch('click');
    assert(dom.registry['kc-forgot-username'].value === 'bench', 'login username carried over into the reset modal');
    assert(dom.registry['kc-forgot-code']._focused === true, 'reset step: focus goes to the TOTP code field once username is known');
    assert(!dom.registry['kc-forgot-username']._focused, 'reset step: username itself not focused once already filled');
  }

  // Group 14 (ROADMAP.md modal-accessibility follow-up, gap 2 -- Escape
  // with stacked modals): the login modal must react to Escape only while
  // it is the TOPMOST entry on the shared kcModalStack. This simulates
  // another modal (e.g. a themed confirm dialog, per the "login modal over
  // a confirm dialog" case named in the follow-up) opening on top of an
  // already-open login modal by pushing its own id onto the same stack --
  // exactly what openConfirmModal()/openForgotPasswordModal() do for real.
  // A single Escape press must never fall through and cancel the login
  // modal underneath while that other modal is still up.
  {
    const { ctx, dom } = makeContext({ withLogin: true });
    let settled;
    const p = ctx.openLoginModal('Administrator login required').then((ok) => { settled = ok; });
    const foreignId = ctx.kcModalPush();
    dom.document.dispatch('keydown', { key: 'Escape', preventDefault() {} });
    await flush();
    assert(settled === undefined, 'Escape does not cancel the login modal while another modal is stacked on top of it');
    ctx.kcModalPop(foreignId);
    dom.document.dispatch('keydown', { key: 'Escape', preventDefault() {} });
    await p;
    assert(settled === false, 'Escape cancels the login modal once it is topmost again');
  }

  // Group 15 (review of the same follow-up): the REAL confirm modal stacked
  // with the real login/reset modals. One Escape press closes exactly one
  // layer, the topmost; Escape on a confirm never resolves it as confirmed;
  // the stack is empty once every modal has closed (nothing left registered
  // to swallow a later Escape).
  {
    const esc = () => ({ key: 'Escape', preventDefault() {}, stopImmediatePropagation() { this._stopped = true; } });

    // 15a: login escalates on top of an open confirm.
    {
      const { ctx, dom } = makeContext({ withLogin: true, withConfirm: true });
      let c, l;
      ctx.openConfirmModal('Start firing?', {}).then((ok) => { c = ok; });
      ctx.openLoginModal('Administrator login required').then((ok) => { l = ok; });
      dom.document.dispatch('keydown', esc());
      await flush();
      assert(l === false && c === undefined, 'login over confirm: one Escape cancels only the login');
      dom.document.dispatch('keydown', esc());
      await flush();
      assert(c === false, 'login over confirm: second Escape cancels the confirm (never "confirmed")');
      assert(ctx.kcModalStack.length === 0, 'login over confirm: stack empty after both close');
    }

    // 15b: confirm opens on top of a pending login.
    {
      const { ctx, dom } = makeContext({ withLogin: true, withConfirm: true });
      let c, l;
      ctx.openLoginModal('Administrator login required').then((ok) => { l = ok; });
      ctx.openConfirmModal('Start firing?', {}).then((ok) => { c = ok; });
      dom.document.dispatch('keydown', esc());
      await flush();
      assert(c === false && l === undefined, 'confirm over login: one Escape cancels only the confirm');
      dom.document.dispatch('keydown', esc());
      await flush();
      assert(l === false, 'confirm over login: second Escape cancels the login');
      assert(ctx.kcModalStack.length === 0, 'confirm over login: stack empty after both close');
    }

    // 15c: a kcAlert lands on top of the reset modal, then the reset
    // modal closes underneath it (a /reset success arriving late). The
    // resumed login must stay BELOW the alert, and one Escape must not
    // close both the alert and the login.
    {
      const { ctx, dom } = makeContext({ withLogin: true, withConfirm: true });
      let a, l;
      ctx.openLoginModal('Administrator login required').then((ok) => { l = ok; });
      dom.registry['.kc-login-forgot-link'].dispatch('click');
      ctx.openConfirmModal('Board unreachable.', { alertOnly: true }).then((ok) => { a = ok; });
      ctx.closeForgotModal('Password reset. Log in with the new password.');
      dom.document.dispatch('keydown', esc());
      await flush();
      assert(a === true && l === undefined, 'alert over resumed login: one Escape closes only the alert');
      dom.document.dispatch('keydown', esc());
      await flush();
      assert(l === false, 'alert over resumed login: second Escape cancels the login');
      assert(ctx.kcModalStack.length === 0, 'alert over resumed login: stack empty afterwards');
    }

    // 15d: every login exit path pops its id -- success, Cancel, backdrop,
    // and a reset detour -- so no id is ever left behind.
    {
      const { ctx, dom } = makeContext({
        withLogin: true, withConfirm: true,
        fetchResponses: [{ status: 401, text: 'bad' }, { status: 200 }],
      });
      let l;
      ctx.openLoginModal('Administrator login required').then((ok) => { l = ok; });
      dom.registry['.kc-login-forgot-link'].dispatch('click');
      dom.registry['kc-forgot-cancel1'].dispatch('click');
      dom.registry['kc-login-username'].value = 'bench';
      dom.registry['kc-login-password'].value = 'x';
      ctx.loginFormEl.dispatch('submit', { preventDefault() {} });
      await flush();
      assert(l === undefined && ctx.kcModalStack.length === 1, 'failed login POST keeps exactly one stack entry');
      dom.registry['kc-login-password'].value = 'y';
      ctx.loginFormEl.dispatch('submit', { preventDefault() {} });
      await flush();
      assert(l === true && ctx.kcModalStack.length === 0, 'successful login pops its stack entry');
      let l2;
      ctx.openLoginModal('Administrator login required').then((ok) => { l2 = ok; });
      const overlay = ctx.loginModalEl;
      overlay.dispatch('click', { preventDefault() {}, target: overlay });
      await flush();
      assert(l2 === false && ctx.kcModalStack.length === 0, 'backdrop cancel pops its stack entry');
    }
  }

  // Group 10: no localStorage/sessionStorage reference exists in this code
  // at all -- confirm the source text itself never mentions them, which is
  // stronger than "didn't happen to call it in these scenarios".
  {
    assert(RANGE_D.indexOf('localStorage') === -1, 'source never references localStorage');
    assert(RANGE_D.indexOf('sessionStorage') === -1, 'source never references sessionStorage');
  }

  console.log('\n' + passed + ' passed, ' + failed + ' failed');
  if (failed) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
  process.exit(0);
})();
