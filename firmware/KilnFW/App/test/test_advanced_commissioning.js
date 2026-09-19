/* Node-only test harness for safety_commissioning_page.html's ADVANCED-view
 * save path (the #saveBtn click handler around line 1588 as of the
 * commissioning_shared.js retrofit): findCriticalChanges(), the busy check
 * (checkFiringOrAutotuneRunning() pre-retrofit / kcCommissioningCheckBusy()
 * post-retrofit), the confirm dialog, and the page's own independent
 * read-back verification.
 *
 * Why this exists: a 2026-09-08 review (5d03f8c2) found this exact contract
 * duplicated between commissioning_shared.js (used only by
 * setup_wizard_page.html) and this page's own copy (findCriticalChanges()
 * :1417, checkFiringOrAutotuneRunning() :1453, the #saveBtn handler
 * :1588-1725) -- and this page had NO test covering that path at all before
 * this file. Per the task's own instruction ("if it has no tests covering
 * the confirm/read-back path, add them BEFORE the retrofit"), this file was
 * written and run GREEN against the pre-retrofit duplicate implementation
 * first, then left UNMODIFIED while the page was retrofitted onto
 * commissioning_shared.js -- an identical pass afterward is the proof of
 * equivalence.
 *
 * Same extraction technique as test_guided_flow.js (real inline <script>,
 * vm context, minimal DOM/fetch stub) plus commissioning_shared.js loaded
 * into the SAME context first, so once the retrofit lands `window.kc*`
 * refers to the real shared functions, not a reimplementation.
 *
 * Run: node firmware/KilnFW/App/test/test_advanced_commissioning.js
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const DRIVERS_DIR = resolveDriversDir(__dirname);
const PAGE_PATH = resolveDriverFile(DRIVERS_DIR, 'safety_commissioning_page.html');
const SHARED_PATH = resolveDriverFile(DRIVERS_DIR, 'commissioning_shared.js');

function extractInlineScript(html) {
  const noComments = html.replace(/<!--[\s\S]*?-->/g, (m) => m.replace(/[^\n]/g, ' '));
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  let m; let best = null;
  while ((m = re.exec(noComments))) {
    if (!best || m[1].length > best.length) best = m[1];
  }
  if (!best) throw new Error('could not find the inline <script> block in ' + PAGE_PATH);
  return best;
}

function makeElement(id) {
  const listeners = {};
  return {
    id: id || '', value: '', textContent: '', innerHTML: '', className: '',
    hidden: false, disabled: false, checked: false, title: '', dataset: {}, style: {},
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    children: [],
    addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
    _fire(type) { (listeners[type] || []).forEach((fn) => fn.call(this)); },
    appendChild(child) { this.children.push(child); return child; },
    removeChild() {}, querySelector() { return null; }, querySelectorAll() { return []; },
    closest() { return null; }, focus() {}, remove() {},
    setAttribute() {}, getAttribute() { return null; }, removeAttribute() {},
  };
}

// The two critical fields (tc_type=261, tc_offset_c=266) as staged
// #fields inputs/selects, per the page's own CRITICAL_CONFIRM_FIELD_IDS.
function makeDocument(fields) {
  const byId = new Map();
  const doc = {
    _byId: byId,
    _fields: fields || [],
    getElementById(id) {
      if (!byId.has(id)) byId.set(id, makeElement(id));
      return byId.get(id);
    },
    createElement() { return makeElement(); },
    querySelector() { return null; }, // contradiction-check/tc-max fields: not rendered in this harness
    querySelectorAll(sel) {
      if (sel === '#fields input[data-id], #fields select[data-id]') return doc._fields;
      return [];
    },
    body: makeElement('body'),
    documentElement: makeElement('documentElement'),
  };
  return doc;
}

function makeFieldEl(id, type, value) {
  return { dataset: { id: String(id), type: type }, value: String(value), checked: false };
}

function makeFetch(responder) {
  return function fetch(url, opts) {
    const r = responder(url, opts);
    return Promise.resolve({ ok: r.ok, status: r.status, json: () => Promise.resolve(r.body) });
  };
}

function loadContext({ fetchImpl, confirmImpl, fields }) {
  const document = makeDocument(fields);
  const sandbox = {
    document,
    fetch: fetchImpl,
    console,
    setInterval: (fn, ms) => { const t = setInterval(fn, ms); if (t.unref) t.unref(); return t; },
    clearInterval,
    setTimeout: (fn, ms) => { const t = setTimeout(fn, ms); if (t.unref) t.unref(); return t; },
    clearTimeout,
    matchMedia: () => ({ matches: false }),
    localStorage: { getItem: () => null, setItem: () => {} },
  };
  sandbox.window = sandbox;
  sandbox.window.document = document;
  sandbox.window.kcEscapeHtml = (s) => String(s);
  sandbox.window.confirm = confirmImpl || (() => true);
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);

  // commissioning_shared.js FIRST, real code -- once the page is retrofitted
  // onto it, window.kcCommissioning* below are these functions, not stubs.
  const sharedCode = fs.readFileSync(SHARED_PATH, 'utf8');
  new vm.Script(sharedCode, { filename: 'commissioning_shared.js' }).runInContext(sandbox);

  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  const pageCode = extractInlineScript(html);
  new vm.Script(pageCode, { filename: 'safety_commissioning_page.html (inline script)' }).runInContext(sandbox);
  return sandbox;
}

function runMicrotasks() {
  let p = Promise.resolve();
  for (let i = 0; i < 8; i++) p = p.then(() => {});
  return p;
}

// loadCurrent()'s startup chain (ct_channel_map -> zone names -> commissioning
// GET, each a real Promise hop) is deeper than a single runMicrotasks()
// drain -- wait across a couple of macrotask boundaries too so `latest` is
// reliably populated before a test drives #saveBtn.
function waitForPageLoad() {
  return runMicrotasks()
    .then(() => new Promise((resolve) => setTimeout(resolve, 0)))
    .then(() => runMicrotasks())
    .then(() => new Promise((resolve) => setTimeout(resolve, 0)))
    .then(() => runMicrotasks());
}

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

// Baseline GET body: tc_type currently Type K (3), tc_offset_c currently 0.
const BASE_PARAMS = [
  { id: 261, name: 'tc_type', value: '3', set: true },
  { id: 266, name: 'tc_offset_c', value: '0', set: true },
];

function clickSave(ctx) {
  ctx.document.getElementById('saveBtn')._fire('click');
  return runMicrotasks();
}

// ---- Test 1: no critical change (tc_type/tc_offset_c resubmitted unchanged)
// -- must go straight to commit, no confirm, no busy check, and report a
// plain "Committed." (NOT a false claim of read-back verification, since
// none was needed or performed).
(function testNoCriticalChangeCommitsDirectly() {
  let confirmCalled = false;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') {
      return { ok: true, status: 200, body: { ok: true } };
    }
    if (url === '/api/safety/commissioning') return { ok: true, status: 200, body: { params: BASE_PARAMS } };
    if (url === '/api/zones/ct_channel_map') return { ok: true, status: 200, body: {} };
    if (url === '/api/zones') return { ok: true, status: 200, body: { zones: [] } };
    return { ok: true, status: 200, body: {} };
  };
  const fields = [makeFieldEl(261, 'enum', '3'), makeFieldEl(266, 'f32', '0')];
  const ctx = loadContext({
    fetchImpl: makeFetch(fetchImpl),
    confirmImpl: () => { confirmCalled = true; return true; },
    fields,
  });
  return waitForPageLoad().then(() => clickSave(ctx)).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => {
    const msg = ctx.document.getElementById('msg').textContent;
    assert(confirmCalled === false, 'no-critical-change: never shows a confirm dialog');
    assertIncludes(msg, 'Committed.', 'no-critical-change: reports a plain commit');
    assertNotIncludes(msg, 'confirmed by read-back', 'no-critical-change: does NOT falsely claim a read-back verification that never ran');
  });
})();

// ---- Test 2: a critical change (tc_type K->T), not busy, confirmed -> POST
// ok, read-back agrees -> named success message.
(function testCriticalChangeConfirmedAndVerified() {
  let getCount = 0;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') {
      return { ok: true, status: 200, body: { ok: true } };
    }
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/safety/commissioning') {
      getCount++;
      // First GET (page load): OLD value (Type K/3). Every GET after the
      // commit (this page's own read-back): NEW value (Type T/7) -- the
      // board actually applied the write.
      return { ok: true, status: 200, body: { params: getCount === 1 ? BASE_PARAMS : [{ id: 261, name: 'tc_type', value: '7', set: true }, { id: 266, name: 'tc_offset_c', value: '0', set: true }] } };
    }
    return { ok: true, status: 200, body: {} };
  };
  const fields = [makeFieldEl(261, 'enum', '7'), makeFieldEl(266, 'f32', '0')];
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => true, fields });
  return waitForPageLoad().then(() => clickSave(ctx)).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => {
    const msg = ctx.document.getElementById('msg').textContent;
    assertIncludes(msg, 'Committed and confirmed by read-back:', 'critical-change/confirmed/verified: reports named success');
    assertIncludes(msg, 'tc_type', 'critical-change/confirmed/verified: names the field');
  });
})();

// ---- Test 3: a critical change while a profile is firing -> refused before
// ever asking to confirm, and nothing is POSTed.
(function testCriticalChangeRefusedWhenBusy() {
  let postCalled = false;
  let confirmCalled = false;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') { postCalled = true; return { ok: true, status: 200, body: { ok: true } }; }
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'running' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/safety/commissioning') return { ok: true, status: 200, body: { params: BASE_PARAMS } };
    return { ok: true, status: 200, body: {} };
  };
  const fields = [makeFieldEl(261, 'enum', '7'), makeFieldEl(266, 'f32', '0')];
  const ctx = loadContext({
    fetchImpl: makeFetch(fetchImpl),
    confirmImpl: () => { confirmCalled = true; return true; },
    fields,
  });
  return waitForPageLoad().then(() => clickSave(ctx)).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => {
    const msg = ctx.document.getElementById('msg').textContent;
    assertIncludes(msg, 'Refused:', 'critical-change/busy: refuses');
    assertIncludes(msg, 'a profile is currently firing', 'critical-change/busy: names the reason');
    assert(confirmCalled === false, 'critical-change/busy: never reaches the confirm dialog');
    assert(postCalled === false, 'critical-change/busy: never POSTs');
  });
})();

// ---- Test 4: a critical change, operator cancels the confirm -> nothing sent.
(function testCriticalChangeCancelled() {
  let postCalled = false;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') { postCalled = true; return { ok: true, status: 200, body: { ok: true } }; }
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/safety/commissioning') return { ok: true, status: 200, body: { params: BASE_PARAMS } };
    return { ok: true, status: 200, body: {} };
  };
  const fields = [makeFieldEl(261, 'enum', '7'), makeFieldEl(266, 'f32', '0')];
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => false, fields });
  return waitForPageLoad().then(() => clickSave(ctx)).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => {
    const msg = ctx.document.getElementById('msg').textContent;
    assertIncludes(msg, 'Cancelled -- nothing was written.', 'critical-change/cancelled: reports cancellation');
    assert(postCalled === false, 'critical-change/cancelled: never POSTs');
  });
})();

// ---- Test 5: POST reports ok:true but this page's OWN fresh read-back
// disagrees -> FAILS LOUDLY, naming the field, even though the server said ok.
(function testCriticalChangeReadbackMismatchFailsLoudly() {
  let getCount = 0;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') {
      return { ok: true, status: 200, body: { ok: true } };
    }
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/safety/commissioning') {
      getCount++;
      // First GET (page load): old value. Second GET (post-commit
      // read-back): still the OLD value -- the board silently did not apply it.
      return { ok: true, status: 200, body: { params: BASE_PARAMS } };
    }
    return { ok: true, status: 200, body: {} };
  };
  const fields = [makeFieldEl(261, 'enum', '7'), makeFieldEl(266, 'f32', '0')];
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => true, fields });
  return waitForPageLoad().then(() => clickSave(ctx)).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => {
    const msg = ctx.document.getElementById('msg').textContent;
    assertIncludes(msg, 'FAILED:', 'critical-change/readback-mismatch: fails loudly');
    assertIncludes(msg, 'does NOT match what was just written', 'critical-change/readback-mismatch: names the disagreement');
    assertIncludes(msg, 'tc_type', 'critical-change/readback-mismatch: names the field');
  });
})();

// ---- Test 6: the per-channel gain field is operator-editable, and an
// out-of-range entry is REFUSED at the client before any request is built --
// never silently replaced by a default. gain sits in the denominator of the
// Pico's amps formula and of the presence threshold, so a 0 entry would
// otherwise be accepted here and then quietly ignored by cs_counts_to_amps()'s
// CS_DEFAULT_GAIN fallback, leaving the operator reading a number the board
// is not using.
(function testGainZeroIsRefusedClientSide() {
  let postCalled = false;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') { postCalled = true; return { ok: true, status: 200, body: { ok: true } }; }
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/safety/commissioning') return { ok: true, status: 200, body: { params: BASE_PARAMS } };
    return { ok: true, status: 200, body: {} };
  };
  const fields = [makeFieldEl(779, 'f32', '0')];
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => true, fields });
  return waitForPageLoad().then(() => clickSave(ctx)).then(() => runMicrotasks()).then(() => {
    const msg = ctx.document.getElementById('msg').textContent;
    assertIncludes(msg, 'Rejected:', 'gain/zero: refused loudly');
    assertIncludes(msg, 'gain[0]', 'gain/zero: names the field');
    assertIncludes(msg, '0.05', 'gain/zero: names the accepted range');
    assert(postCalled === false, 'gain/zero: never POSTs');
  });
})();

// ---- Test 7: a gain above the accepted ceiling is refused the same way.
(function testGainAboveMaxIsRefusedClientSide() {
  let postCalled = false;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') { postCalled = true; return { ok: true, status: 200, body: { ok: true } }; }
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/safety/commissioning') return { ok: true, status: 200, body: { params: BASE_PARAMS } };
    return { ok: true, status: 200, body: {} };
  };
  const fields = [makeFieldEl(781, 'f32', '25')];
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => true, fields });
  return waitForPageLoad().then(() => clickSave(ctx)).then(() => runMicrotasks()).then(() => {
    const msg = ctx.document.getElementById('msg').textContent;
    assertIncludes(msg, 'Rejected:', 'gain/above-max: refused loudly');
    assertIncludes(msg, 'gain[2]', 'gain/above-max: names the field');
    assert(postCalled === false, 'gain/above-max: never POSTs');
  });
})();

// ---- Test 8: an in-range gain trim is accepted and actually sent. Without
// this, Tests 6 and 7 would still pass if the field were simply un-editable
// or every entry were refused.
(function testInRangeGainIsAccepted() {
  let postBody = null;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning' && opts && opts.method === 'POST') { postBody = opts.body; return { ok: true, status: 200, body: { ok: true } }; }
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/safety/commissioning') return { ok: true, status: 200, body: { params: BASE_PARAMS } };
    return { ok: true, status: 200, body: {} };
  };
  const fields = [makeFieldEl(779, 'f32', '0.73')];
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => true, fields });
  return waitForPageLoad().then(() => clickSave(ctx)).then(() => runMicrotasks()).then(() => runMicrotasks()).then(() => {
    assert(postBody !== null, 'gain/in-range: the trim is actually POSTed');
    assertIncludes(String(postBody), 'id=779', 'gain/in-range: carries the gain param id');
    assertIncludes(String(postBody), '0.73', 'gain/in-range: carries the entered value');
  });
})();

// ---- Tests 9-12: the operator-entered CT scale trim
// (docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md owner decision 3). These drive
// ctTrimRowHtml()/wireCtTrimButtons() directly rather than through a full
// render, because this harness's stub DOM has no real element tree for the
// ct_cal rows to be rendered into.

const TRIM_FETCH_BASE = (url, opts) => {
  if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'idle' } };
  if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
  if (url === '/api/safety/commissioning') return { ok: true, status: 200, body: { params: BASE_PARAMS } };
  return { ok: true, status: 200, body: {} };
};

// A stand-in for one rendered .ct-trim-row, carrying the two inputs and the
// result span wireCtTrimButtons() reaches through querySelector().
function makeTrimRow(ch, offValue, gainValue) {
  const parts = {
    '.ct-trim-offset': { value: String(offValue) },
    '.ct-trim-gain': { value: String(gainValue) },
    '.ct-trim-apply': { disabled: false, onclick: null },
    '.ct-trim-result': { textContent: '', className: '' },
  };
  return {
    _parts: parts,
    getAttribute(name) { return name === 'data-ct-trim-ch' ? String(ch) : null; },
    querySelector(sel) { return parts[sel] || null; },
  };
}

function wireTrimRow(ctx, row) {
  ctx.document.querySelectorAll = (sel) => (sel === '.ct-trim-row' ? [row] : []);
  ctx.wireCtTrimButtons();
}

// ---- Test 9: the row shows what is actually STORED -- the round-trip the
// whole field exists for. A field that posts but renders a blank or a
// hardcoded default would pass every POST test below and still be useless.
(function testTrimRowRendersStoredValues() {
  const ctx = loadContext({ fetchImpl: makeFetch(TRIM_FETCH_BASE), confirmImpl: () => true, fields: [] });
  return waitForPageLoad().then(() => {
    const html = ctx.ctTrimRowHtml(1, { has_value: false, trim_offset_a: -0.4, trim_gain: 1.04 });
    assertIncludes(html, 'value="-0.4"', 'trim/render: shows the stored offset trim');
    assertIncludes(html, 'value="1.04"', 'trim/render: shows the stored gain trim');
    assertIncludes(html, 'data-ct-trim-ch="1"', 'trim/render: carries its channel');
    // has_value:false above is deliberate: the trim is NOT gated on a channel
    // having been commissioned, so an uncommissioned channel must still show
    // its real stored trim rather than an empty box.
    assertNotIncludes(html, 'data-id', 'trim/render: no data-id, so the generic "Save all" pass cannot sweep it up');
  });
})();

// ---- Test 10: identity fallback when the firmware's JSON predates these
// fields -- 0 A / 1.0x, never a blank or a NaN.
(function testTrimRowFallsBackToIdentity() {
  const ctx = loadContext({ fetchImpl: makeFetch(TRIM_FETCH_BASE), confirmImpl: () => true, fields: [] });
  return waitForPageLoad().then(() => {
    const html = ctx.ctTrimRowHtml(0, {});
    assertIncludes(html, 'value="0"', 'trim/fallback: offset falls back to the identity 0 A');
    assertIncludes(html, 'value="1"', 'trim/fallback: gain falls back to the identity 1.0x');
  });
})();

// ---- Test 11: Apply actually POSTs the entered pair to the trim endpoint,
// and a persisted success re-reads the page (the read-back).
(function testTrimApplyPostsAndReloads() {
  let postUrl = null, postBody = null, commissioningGets = 0;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning/ct_trim') {
      postUrl = url; postBody = opts.body;
      return { ok: true, status: 200, body: { ok: true, persisted: true, trim_offset_a: -0.4, trim_gain: 1.04 } };
    }
    if (url === '/api/safety/commissioning' && !(opts && opts.method === 'POST')) commissioningGets++;
    return TRIM_FETCH_BASE(url, opts);
  };
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => true, fields: [] });
  return waitForPageLoad().then(() => {
    const row = makeTrimRow(2, '-0.4', '1.04');
    wireTrimRow(ctx, row);
    const getsBefore = commissioningGets;
    row._parts['.ct-trim-apply'].onclick();
    return runMicrotasks().then(() => {
      assert(postUrl === '/api/safety/commissioning/ct_trim', 'trim/apply: POSTs to the trim endpoint');
      assertIncludes(String(postBody), 'ch=2', 'trim/apply: carries the channel');
      assertIncludes(String(postBody), 'trim_offset_a=-0.4', 'trim/apply: carries the offset trim');
      assertIncludes(String(postBody), 'trim_gain=1.04', 'trim/apply: carries the gain trim');
      assert(commissioningGets > getsBefore, 'trim/apply: re-reads the page so the row shows what was stored');
      assertNotIncludes(row._parts['.ct-trim-result'].className, 'crc-mismatch', 'trim/apply: success is not styled as an error');
    });
  });
})();

// ---- Test 12: ok:true with persisted:false means "applied but will not
// survive a reboot". Reporting that as a plain success is the failure mode
// this asserts against.
(function testTrimUnpersistedIsReportedAsFailure() {
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning/ct_trim') {
      return { ok: true, status: 200, body: { ok: true, persisted: false, err: 'ESP_ERR_NVS_NOT_ENOUGH_SPACE' } };
    }
    return TRIM_FETCH_BASE(url, opts);
  };
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => true, fields: [] });
  return waitForPageLoad().then(() => {
    const row = makeTrimRow(0, '1', '1.02');
    wireTrimRow(ctx, row);
    row._parts['.ct-trim-apply'].onclick();
    return runMicrotasks().then(() => {
      const res = row._parts['.ct-trim-result'];
      assertIncludes(res.textContent, 'NOT SAVED', 'trim/unpersisted: says it was not saved');
      assertIncludes(res.className, 'crc-mismatch', 'trim/unpersisted: styled as an error');
    });
  });
})();

// ---- Test 13: an empty box is refused before any request is built, so a
// half-filled row cannot post a partial pair.
(function testTrimEmptyFieldIsRefusedClientSide() {
  let posted = false;
  const fetchImpl = (url, opts) => {
    if (url === '/api/safety/commissioning/ct_trim') { posted = true; return { ok: true, status: 200, body: { ok: true, persisted: true } }; }
    return TRIM_FETCH_BASE(url, opts);
  };
  const ctx = loadContext({ fetchImpl: makeFetch(fetchImpl), confirmImpl: () => true, fields: [] });
  return waitForPageLoad().then(() => {
    const row = makeTrimRow(0, '', '1.04');
    wireTrimRow(ctx, row);
    row._parts['.ct-trim-apply'].onclick();
    return runMicrotasks().then(() => {
      assert(posted === false, 'trim/empty: never POSTs a partial pair');
      assertIncludes(row._parts['.ct-trim-result'].className, 'crc-mismatch', 'trim/empty: refused visibly');
    });
  });
})();

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
