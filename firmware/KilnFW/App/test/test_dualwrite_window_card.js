/* Node-only test harness for diagnostics_page.html's Config Migration Exit
 * Window card (renderDualwriteWindow) -- added 2026-09-20 as this page's new
 * client for GET /api/dualwrite_window and
 * POST /api/dualwrite_window/restore_verified (dualwrite_window_http.c),
 * which were found fully built, registered and completely unreachable by
 * any human (no HTML, no JS, no PcTools/MCP tool) during the "no client"
 * audit that day.
 *
 * Extracts the real source (same extraction-by-marker-line approach as
 * test_safety_tc_diagnostics.js/test_recovery_banner.js) rather than
 * reimplementing the logic, and runs it in a Node vm context with a small
 * document/fetch stub.
 *
 * Run: node firmware/KilnFW/App/test/test_dualwrite_window_card.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'diagnostics_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');

// The naive "extract up to the next lone '  }'" trick (test_safety_tc_
// diagnostics.js's approach) breaks here: renderDualwriteWindow() and
// pollDualwriteWindow() are two adjacent top-level functions sharing this
// file's 2-space indent for their own closing braces, so the FIRST '  }'
// after the start marker is renderDualwriteWindow's own close, not the end
// of this block. Instead, extract up to (but not including) the next
// unrelated comment marker that immediately follows pollDualwriteWindow in
// the real file, which is unambiguous.
const dwwStartIdx = LINES.findIndex((l) => l.replace(/\r$/, '') === '  var dwwSubmitting = false;');
const dwwStopIdx = LINES.findIndex((l, i) => i > dwwStartIdx &&
  l.replace(/\r$/, '') === '  // --- Thermocouple faults (folded in from thermo_faults_page.html) -------');
if (dwwStartIdx === -1) throw new Error('start marker not found for dualwrite window block');
if (dwwStopIdx === -1) throw new Error('stop marker (thermocouple faults comment) not found after dualwrite window block');
const FULL_SRC = LINES.slice(dwwStartIdx, dwwStopIdx).join('\n');

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

assert(FULL_SRC.indexOf('function renderDualwriteWindow(') !== -1,
  'sanity: extracted range includes renderDualwriteWindow');
assert(FULL_SRC.indexOf('dualwrite_window/restore_verified') !== -1,
  'sanity: extracted range includes the restore_verified POST');

// Minimal fake element: innerHTML is captured as plain text (this test never
// re-parses it into a tree), and one button lookup (#dwwRecordRestoreBtn) is
// supported for the click-handler tests below.
function makeCard() {
  return { _html: '', get innerHTML() { return this._html; }, set innerHTML(v) { this._html = v; } };
}

function makeButtonFromHtml(html) {
  // Only asserts presence/absence in this test -- the click handler itself
  // is exercised by calling the exported behavior indirectly via a stub
  // button object below, not by re-parsing this string into a real DOM.
  return html.indexOf('id="dwwRecordRestoreBtn"') !== -1;
}

function makeContext(opts) {
  opts = opts || {};
  const card = makeCard();
  const fetchCalls = [];
  const fetchQueue = opts.fetchQueue || [];
  let clickHandler = null;
  const btnStub = {
    disabled: false,
    addEventListener: function (evt, fn) { if (evt === 'click') clickHandler = fn; },
  };
  const ctx = {
    document: {
      getElementById: function (id) {
        if (id === 'dualwriteWindowCard') return card;
        if (id === 'dwwRecordRestoreBtn') return opts.hasButton ? btnStub : null;
        return null;
      },
    },
    kcConfirm: function () { return opts.confirmReturns !== undefined ? opts.confirmReturns : true; },
    fetch: function (url, init) {
      fetchCalls.push({ url: url, init: init });
      const next = fetchQueue.shift();
      if (!next) return Promise.reject(new Error('no queued response for ' + url));
      if (next.reject) return Promise.reject(next.reject);
      return Promise.resolve({ ok: next.ok !== false, json: function () { return Promise.resolve(next.json || {}); } });
    },
    fetchJsonTimeout: function (url) {
      const next = fetchQueue.shift();
      if (!next) return Promise.reject(new Error('no queued response for ' + url));
      if (next.reject) return Promise.reject(next.reject);
      return Promise.resolve(next.json);
    },
    console: console,
  };
  // The page's catch sites reference window.kcIsAuthCancelled (sign-in
  // cancel stays quiet); in a browser window is the global object.
  ctx.window = ctx;
  vm.createContext(ctx);
  vm.runInContext(FULL_SRC, ctx);
  return { ctx: ctx, card: card, fetchCalls: fetchCalls, getClickHandler: function () { return clickHandler; } };
}

// ---------------------------------------------------------------------------
// Group 1: rendering the three criteria and the headline.
// ---------------------------------------------------------------------------
{
  const { ctx, card } = makeContext();
  vm.runInContext(
    'renderDualwriteWindow({consecutive_clean_boots: 3, clean_boots_target: 20, ' +
    'firing_complete: false, restore_verified: false, window_may_close: false})', ctx);
  assert(card.innerHTML.indexOf('3 of 20') !== -1, 'shows boot progress against target');
  assert(card.innerHTML.indexOf('not yet') !== -1, 'firing/restore not yet done render as "not yet"');
  assert(card.innerHTML.indexOf('YES') === -1, 'window not reported closeable when window_may_close is false');
  assert(makeButtonFromHtml(card.innerHTML), 'record-restore button present while restore_verified is false');
}

{
  const { ctx, card } = makeContext();
  vm.runInContext(
    'renderDualwriteWindow({consecutive_clean_boots: 20, clean_boots_target: 20, ' +
    'firing_complete: true, restore_verified: true, window_may_close: true})', ctx);
  assert(card.innerHTML.indexOf('YES') !== -1, 'all three criteria met renders window_may_close as YES');
  assert(!makeButtonFromHtml(card.innerHTML),
    'record-restore button is hidden once restore_verified is already true -- no re-attestation invited');
}

// ---------------------------------------------------------------------------
// Group 2: the attestation button posts restore_verified only after
// confirmation, and never auto-acts on its own.
// ---------------------------------------------------------------------------
{
  const { ctx, fetchCalls, getClickHandler } = makeContext({
    hasButton: true, confirmReturns: false,
    fetchQueue: [],
  });
  vm.runInContext(
    'renderDualwriteWindow({consecutive_clean_boots: 1, clean_boots_target: 20, ' +
    'firing_complete: false, restore_verified: false, window_may_close: false})', ctx);
  getClickHandler()();
  assert(fetchCalls.length === 0, 'declining the confirm dialog never POSTs restore_verified');
}

{
  const { ctx, fetchCalls, getClickHandler } = makeContext({
    hasButton: true, confirmReturns: true,
    fetchQueue: [
      { ok: true, json: {} }, // the POST itself
      { json: { consecutive_clean_boots: 1, clean_boots_target: 20, firing_complete: false, restore_verified: true, window_may_close: false } }, // the re-poll
    ],
  });
  vm.runInContext(
    'renderDualwriteWindow({consecutive_clean_boots: 1, clean_boots_target: 20, ' +
    'firing_complete: false, restore_verified: false, window_may_close: false})', ctx);
  getClickHandler()();
  assert(fetchCalls.length === 1 && fetchCalls[0].url === '/api/dualwrite_window/restore_verified' &&
         fetchCalls[0].init.method === 'POST',
    'confirming posts to /api/dualwrite_window/restore_verified');
}

// ---------------------------------------------------------------------------
// Group 3: pollDualwriteWindow surfaces a load failure without throwing.
// ---------------------------------------------------------------------------
{
  const { ctx, card } = makeContext({ fetchQueue: [{ reject: new Error('boom') }] });
  vm.runInContext('pollDualwriteWindow()', ctx).then(function () {
    assert(card.innerHTML.indexOf('could not load') !== -1,
      'a failed/timed-out fetch renders a "could not load" message, not a throw');
    finish();
  });
}

function finish() {
  console.log('');
  console.log('dualwrite window card: ' + passed + '/' + (passed + failed) + ' passed');
  if (failed > 0) {
    console.log('FAILURES:');
    failures.forEach(function (f) { console.log('  ' + f); });
    process.exitCode = 1;
  }
}
