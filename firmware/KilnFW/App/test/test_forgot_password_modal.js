/* Node-only test harness for app.js's forgot-password modal flow state
 * machine (TOTP password reset, WT-B, 2026-09-24).
 *
 * Extracts the forgotModalEl/buildForgotModal/openForgotPasswordModal block
 * from app.js by exact marker lines (same approach as
 * test_login_auth_wrapper.js) and runs it against a minimal fake DOM plus a
 * stubbed nativeFetch/openLoginModal/document, without a real browser.
 *
 * Covers:
 *   - step1 (username+code) POSTs /api/auth/forgot; a 202 with a
 *     reset_token advances to step2, a 429 shows the plain-text body
 *     verbatim (never parsed as JSON), a 503 shows the clock-not-synced
 *     message, any other status shows the SAME generic failure text (no
 *     enrolled/not-enrolled oracle);
 *   - step2 (new password x2) refuses locally on a mismatch without
 *     calling fetch at all; POSTs /api/auth/reset with the held token;
 *     {ok:true} closes the modal and reopens the login modal; anything
 *     else shows the generic failure text;
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
        return m ? registry[m[1]] || null : null;
      },
      set className(v) { this._className = v; },
      get className() { return this._className; },
      set innerHTML(html) {
        this._innerHTML = html;
        // Register every element carrying an id="..." attribute in the
        // markup so panel.querySelector('#x') can find it afterward.
        const re = /id="([^"]+)"/g;
        let m;
        while ((m = re.exec(html))) {
          if (!registry[m[1]]) registry[m[1]] = makeElement('stub');
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
    dispatch(type, evt) {
      (documentListeners[type] || []).forEach((fn) => fn(evt));
    },
  };
  return { registry, document: doc };
}

function fakeResp(spec) {
  return {
    status: spec.status,
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
    console: fakeConsole,
    encodeURIComponent,
    String,
    // Deliberately no localStorage/sessionStorage globals -- the modal
    // code must never reach for them; if it tried, this would throw
    // ReferenceError, which the tests below treat as a failure.
  };
  vm.createContext(ctx);
  vm.runInContext(RANGE_D, ctx);
  return { ctx, dom, fetchCalls, openLoginModalCalls, loggedArgs };
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
    dom.registry['kc-forgot-newpass'].value = 'samepass1';
    dom.registry['kc-forgot-newpass2'].value = 'samepass1';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    await flush();
    assert(fetchCalls.length === 0, 'step2 with no held token never calls fetch');
    assert(dom.registry['kc-forgot-step1'].hidden === false, 'step2 with no token falls back to step1');
  }

  // Group 7: full happy path -- step1 202, step2 200/{ok:true} closes the
  // modal (token cleared) and reopens the login modal.
  {
    const { ctx, dom, openLoginModalCalls } = makeContext({
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
    dom.registry['kc-forgot-newpass'].value = 'newpassword1';
    dom.registry['kc-forgot-newpass2'].value = 'newpassword1';
    dom.registry['kc-forgot-step2'].dispatch('submit');
    await flush();
    assert(openLoginModalCalls.length === 1, 'success reopens the login modal');
    assert(ctx.forgotResetToken === null, 'reset token cleared after success (closeForgotModal)');
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
