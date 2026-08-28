/* Node-only test harness for safety_commissioning_page.html's guided
 * four-question commissioning flow (gGoto, gValidateScreen1/2, gCommit and
 * friends).
 *
 * Why this exists: an opus review BLOCKed the guided flow's commit path
 * because POST /api/safety/commissioning answers a REJECTED commit with HTTP
 * 200 and body {"ok":false,"reason":"..."} (safety_cfg_http.c:703-723), but
 * gCommit() used to branch on the HTTP status alone (`res.ok`). Every
 * rejection path -- an ARMED refusal, a RANGE/CONTRADICTION rejection, a
 * confirm_commit_landed() read-back failure -- rendered the SAME "Committed
 * and confirmed by read-back." success message. A previous fix attempt
 * claimed to have proven this with negative tests ("broke two validations,
 * saw RED"), but never actually drove gCommit() against a fake fetch
 * response -- it tested something adjacent and called it covered.
 *
 * This harness extracts the page's real inline <script> (the same one the
 * browser runs, via the same non-src regex lint_pages.js uses) and executes
 * it inside a Node vm context with a minimal DOM/fetch stub, so the
 * functions under test (gCommit, gBuildAnswers, checkTcMaxContradiction,
 * gStartWriteWindowRetry, ...) are the ACTUAL page functions, not a
 * reimplementation of what they are supposed to do.
 *
 * Run: node firmware/KilnFW/App/test/test_guided_flow.js
 * Exit code 0 on all-pass, 1 otherwise (also prints a summary line).
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const PAGE_PATH = path.join(__dirname, '..', 'drivers', 'safety_commissioning_page.html');

function extractInlineScript(html) {
  // Same rule lint_pages.js uses: an inline <script> with no src=, comments
  // blanked first so a literal "<script>" inside an HTML comment can't be
  // mistaken for a real tag.
  const noComments = html.replace(/<!--[\s\S]*?-->/g, (m) => m.replace(/[^\n]/g, ' '));
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  let m;
  let best = null;
  while ((m = re.exec(noComments))) {
    // The page has two script-without-src blocks: a tiny early-theme IIFE
    // (line 7) and the big guided-flow one. Take the longest -- that's the
    // real one, and staying content-based (not a hardcoded line number)
    // means this harness does not silently start testing the wrong block
    // after an unrelated edit shifts line numbers.
    if (!best || m[1].length > best.length) best = m[1];
  }
  if (!best) throw new Error('could not find the inline <script> block in ' + PAGE_PATH);
  return best;
}

// ---- Minimal fake DOM ---------------------------------------------------
// Auto-vivifying: any id asked for via getElementById gets a generic element
// if one hasn't been created yet, so the harness doesn't have to enumerate
// every id the page happens to touch during its (considerable) startup
// sequence. querySelector/querySelectorAll are handled for the small,
// specific set of selectors the guided-flow code actually uses; anything
// else returns null / an empty list, matching "field not rendered" behaviour
// the real functions already tolerate (see checkTcMaxContradiction()).

function makeElement(id) {
  const listeners = {};
  return {
    id: id || '',
    value: '',
    textContent: '',
    innerHTML: '',
    className: '',
    hidden: false,
    disabled: false,
    checked: false,
    title: '',
    dataset: {},
    style: {},
    classList: {
      add() {}, remove() {}, toggle() {}, contains() { return false; },
    },
    children: [],
    addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
    _fire(type) { (listeners[type] || []).forEach((fn) => fn.call(this)); },
    appendChild(child) { this.children.push(child); return child; },
    removeChild() {},
    querySelector() { return null; },
    querySelectorAll() { return []; },
    closest() { return null; },
    focus() {},
    remove() {},
    setAttribute() {}, getAttribute() { return null; }, removeAttribute() {},
  };
}

function makeDocument() {
  const byId = new Map();
  const doc = {
    _byId: byId,
    getElementById(id) {
      if (!byId.has(id)) byId.set(id, makeElement(id));
      return byId.get(id);
    },
    createElement(tag) { return makeElement(); },
    querySelector(sel) {
      // 'input[name="gq2"]:checked' -- used by gValidateScreen2(). The
      // harness drives that path by setting a specific radio element's
      // .checked directly (see setCheckedRadio below) and registering it
      // here.
      if (sel === 'input[name="gq2"]:checked') {
        return doc._checkedRadio || null;
      }
      // 'select[data-id="257"]' / 'select[data-id="259"]' /
      // 'select[data-id="261"]' / '[data-id="260"]' -- Advanced-view fields,
      // not rendered by the guided flow at all in this harness, so
      // checkTcMaxContradiction() correctly sees them as absent (returns
      // null, "field not rendered -- nothing to check") unless a test
      // explicitly wants the contradiction path, in which case it registers
      // them via doc._dataIdSelects.
      const m = /^(?:select)?\[?data-id="(\d+)"\]?$|data-id="(\d+)"/.exec(sel);
      if (m && doc._dataIdSelects) {
        const wantedId = m[1] || m[2];
        return doc._dataIdSelects[wantedId] || null;
      }
      return null;
    },
    querySelectorAll() { return []; },
    body: makeElement('body'),
    documentElement: makeElement('documentElement'),
  };
  return doc;
}

function makeFetch(responder) {
  // responder(url, opts) -> { status, ok, json: () => Promise }
  return function fetch(url, opts) {
    const r = responder(url, opts);
    return Promise.resolve({
      ok: r.ok,
      status: r.status,
      json: () => Promise.resolve(r.body),
    });
  };
}

function buildContext(fetchImpl) {
  const document = makeDocument();
  const localStorageData = {};
  const sandbox = {
    document,
    window: {
      kcEscapeHtml: (s) => String(s),
    },
    fetch: fetchImpl,
    localStorage: {
      getItem: (k) => (k in localStorageData ? localStorageData[k] : null),
      setItem: (k, v) => { localStorageData[k] = v; },
    },
    matchMedia: () => ({ matches: false }),
    console,
    // Real timers, but unref'd -- the page's own setInterval(loadCurrent,
    // 5000) and gStartWriteWindowRetry()'s retry timer must not keep this
    // test process alive past its own assertions. A test can still assert
    // the synchronous state gCommit()/gStartWriteWindowRetry() leave behind
    // before any timer fires, which is all the required negative tests need.
    setInterval: (fn, ms) => { const t = setInterval(fn, ms); if (t.unref) t.unref(); return t; },
    clearInterval,
    setTimeout: (fn, ms) => { const t = setTimeout(fn, ms); if (t.unref) t.unref(); return t; },
    clearTimeout,
  };
  sandbox.window.document = document;
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  return sandbox;
}

function loadPageScript(fetchImpl) {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  const code = extractInlineScript(html);
  const ctx = buildContext(fetchImpl);
  // Running the whole script exercises the same startup sequence the real
  // page runs (gGoto(0), loadCurrent(), the theme IIFE) -- if the harness's
  // stub DOM is too thin for that to complete, this throws, and per the
  // task's own instruction ("If your test cannot reach gCommit(), say so
  // plainly") that failure is left uncaught so it is visible, not swallowed.
  new vm.Script(code, { filename: 'safety_commissioning_page.html (inline script)' }).runInContext(ctx);
  return ctx;
}

// ---- Test scaffolding -----------------------------------------------------
let passed = 0, failed = 0;
const failures = [];

function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

function assertIncludes(haystack, needle, label) {
  assert(typeof haystack === 'string' && haystack.indexOf(needle) !== -1,
    label + ' (got: ' + JSON.stringify(haystack) + ')');
}

function assertNotIncludes(haystack, needle, label) {
  assert(!(typeof haystack === 'string' && haystack.indexOf(needle) !== -1),
    label + ' (got: ' + JSON.stringify(haystack) + ')');
}

// ---- Test 1: gCommit() against a REJECTED commit answered with HTTP 200 --
// This is the exact case from the task: {"ok":false,"reason":"commit
// rejected: relay is ARMED -- config writes are refused while ARMED"}.
// Before the fix, gCommit() branched on res.ok (the HTTP status, which is
// true/200 here) and rendered success. After the fix it must read the
// parsed body's own ok field and report failure.
(function testArmedRejectionOver200() {
  const armedReason = 'commit rejected: relay is ARMED -- config writes are refused while ARMED';
  const ctx = loadPageScript(makeFetch((url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') {
      return { ok: true, status: 200, body: { ok: false, reason: armedReason } };
    }
    // The GET at page load and the periodic refetch: answer with an empty
    // params list so renderGroups()/gPrefill() have something to chew on.
    return { ok: true, status: 200, body: { params: [], link_up: true, commissioned: false } };
  }));

  if (typeof ctx.gCommit !== 'function') {
    console.log('FAIL: gCommit() is not reachable from the extracted script -- cannot run this test.');
    failed++; failures.push('gCommit unreachable (ARMED-over-200 test)');
    return;
  }

  // Answer Q1 so gBuildAnswers() has something to stage (mirrors what
  // gValidateScreen1() requires before this screen is ever reachable).
  ctx.document.getElementById('gAbsMax').value = '1300';
  ctx.document.getElementById('gMainsSelect').value = 'unset';

  ctx.gCommit();
  // gCommit()'s fetch chain is a real Promise chain (queued via
  // Promise.resolve() above); microtasks() drains it via a synchronous spin.
  return runMicrotasks().then(() => {
    const msg = ctx.document.getElementById('guidedMsg').textContent;
    const banner2 = ctx.document.getElementById('gWriteWindow2').textContent;
    assertNotIncludes(msg, 'Committed and confirmed by read-back.',
      'ARMED-over-200: guidedMsg must NOT claim success');
    assertIncludes(msg, 'ARMED', 'ARMED-over-200: guidedMsg surfaces the ARMED reason');
    assertNotIncludes(banner2, 'saved', 'ARMED-over-200: write-window banner must not claim "saved"');
    if (ctx.gRetryTimer) clearTimeout(ctx.gRetryTimer);
  });
})();

// ---- Test 2: gCommit() against a read-back-failure rejection over HTTP 200
(function testReadbackFailureOver200() {
  const readbackReason = 'commit accepted but read-back did not confirm every value';
  const ctx = loadPageScript(makeFetch((url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') {
      return { ok: true, status: 200, body: { ok: false, reason: readbackReason } };
    }
    return { ok: true, status: 200, body: { params: [], link_up: true, commissioned: false } };
  }));

  if (typeof ctx.gCommit !== 'function') {
    console.log('FAIL: gCommit() is not reachable from the extracted script -- cannot run this test.');
    failed++; failures.push('gCommit unreachable (read-back-failure test)');
    return;
  }

  ctx.document.getElementById('gAbsMax').value = '1300';
  ctx.document.getElementById('gMainsSelect').value = 'unset';

  ctx.gCommit();
  return runMicrotasks().then(() => {
    const msg = ctx.document.getElementById('guidedMsg').textContent;
    assertNotIncludes(msg, 'Committed and confirmed by read-back.',
      'read-back-failure-over-200: guidedMsg must NOT claim success');
    assertIncludes(msg, 'Rejected:', 'read-back-failure-over-200: guidedMsg reports a rejection');
    assertIncludes(msg, readbackReason, 'read-back-failure-over-200: guidedMsg surfaces the server reason verbatim');
  });
})();

// ---- Test 3: a genuine success (HTTP 200, {"ok":true}) still reports success
(function testRealSuccess() {
  const ctx = loadPageScript(makeFetch((url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') {
      return { ok: true, status: 200, body: { ok: true } };
    }
    return { ok: true, status: 200, body: { params: [], link_up: true, commissioned: false } };
  }));
  if (typeof ctx.gCommit !== 'function') {
    console.log('FAIL: gCommit() is not reachable -- cannot run the real-success control test.');
    failed++; failures.push('gCommit unreachable (real-success control)');
    return;
  }
  ctx.document.getElementById('gAbsMax').value = '1300';
  ctx.document.getElementById('gMainsSelect').value = 'unset';
  ctx.gCommit();
  return runMicrotasks().then(() => {
    const msg = ctx.document.getElementById('guidedMsg').textContent;
    assertIncludes(msg, 'Committed and confirmed by read-back.',
      'real success (ok:true): guidedMsg reports success');
  });
})();

// Drains the microtask queue by yielding a handful of times -- enough for
// gCommit()'s fetch().then().then().then() chain (three links) to settle
// with the synchronous fetch stub above.
function runMicrotasks() {
  let p = Promise.resolve();
  for (let i = 0; i < 8; i++) p = p.then(() => {});
  return p;
}

// The three IIFEs above return promises (or undefined, if the early-exit
// "gCommit unreachable" path fired); wait for all of them before printing
// the summary and setting the exit code.
Promise.resolve()
  .then(() => new Promise((resolve) => setTimeout(resolve, 50)))
  .then(() => {
    console.log('');
    console.log(passed + ' passed, ' + failed + ' failed');
    if (failed) {
      console.log('FAILURES:');
      failures.forEach((f) => console.log('  - ' + f));
      process.exitCode = 1;
    }
  });
