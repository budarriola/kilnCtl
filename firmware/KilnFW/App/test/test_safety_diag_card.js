/* Node-only test harness for safety_page.html's poll()/render()/
 * renderDiagCard() -- specifically, what the "Diagnostics (Frame B)" card
 * shows when the best-effort `GET /api/status?diag=1` document is missing,
 * malformed or slow.
 *
 * Why this exists. adff1595 moved diag_boot_reason and the three safety-link
 * frame counters off the default /api/status document (which had overflowed
 * DASHBOARD_JSON_STATUS_BUF_SIZE and so answered 500 for EVERY consumer) into
 * a small ?diag=1 document, and made this page fetch both. The producer side
 * was reviewed and is correct; the consumer side shipped three defects, all
 * of which this file pins:
 *
 *   1. A failed diag fetch rendered as healthy-looking text. The old merge
 *      copied the diag document's keys onto the main status object; when the
 *      fetch rejected there was nothing to copy, the keys were simply ABSENT,
 *      and the card rendered `undefined + ' / ' + undefined` -- the literal
 *      string "undefined / undefined" -- for the frame counters, plus an
 *      EMPTY value for "TX frames dropped" (window.kcEscapeHtml(undefined)
 *      returns '', app.js). Neither reads as an error. The page's own
 *      staleness banner never fired either, because the outer .catch only saw
 *      the MAIN fetch. A diagnostics card that lies about its own freshness
 *      is worse than one that is plainly blank.
 *   2. Promise.all waited for BOTH fetches, so a slow ?diag=1 stalled the
 *      whole poll -- including the live temperature rows. The ESP's httpd
 *      serves one request at a time under load, so this is the normal case,
 *      not the pathological one.
 *   3. diag_boot_reason was fetched and rendered nowhere.
 *
 * It extracts the REAL source from safety_page.html and app.js by marker line
 * (same approach as test_lag_banner.js / test_safety_tc_diagnostics.js) and
 * runs it in a Node vm with DOM/fetch stubs. A test against a paraphrase of
 * this logic would prove nothing about the page -- this repo has shipped that
 * mistake before.
 *
 * Run: node firmware/KilnFW/App/test/test_safety_diag_card.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const DRIVERS = resolveDriversDir(__dirname);
const PAGE_PATH = resolveDriverFile(DRIVERS, 'safety_page.html');
const APP_JS_PATH = resolveDriverFile(DRIVERS, 'app.js');
// The C header this page's BOOT_REASON_WORDS table mirrors by hand.
const DIAG_H_PATH = path.resolve(
  __dirname, '..', '..', '..', 'CommonFW', 'include', 'kilnlink', 'kilnlink_diag.h');

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

const raw = (l) => l.replace(/\r$/, '');

function linesOf(p) { return fs.readFileSync(p, 'utf8').split('\n'); }

/* Extract [startMarker .. the first `closeMarker` line at/after innerMarker].
 * The close of a function in this file is a bare indented '}' and so is not
 * unique on its own; anchoring it to a unique marker line INSIDE the range
 * keeps the extraction honest across unrelated reflow above it. */
function extractRange(lines, startMarker, innerMarker, closeMarker) {
  const startIdx = lines.findIndex((l) => raw(l) === startMarker);
  if (startIdx === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const innerIdx = lines.findIndex((l, i) => i >= startIdx && raw(l) === innerMarker);
  if (innerIdx === -1) throw new Error('inner marker not found after start: ' + JSON.stringify(innerMarker));
  const endIdx = lines.findIndex((l, i) => i >= innerIdx && raw(l) === closeMarker);
  if (endIdx === -1) throw new Error('close marker not found after inner: ' + JSON.stringify(closeMarker));
  return lines.slice(startIdx, endIdx + 1).join('\n');
}

const PAGE_LINES = linesOf(PAGE_PATH);
// One contiguous slice of the page's IIFE: set()/fmtC() through
// pollDiagDetail(). Everything under test, and its real collaborators, come
// from this one range -- nothing is reimplemented here.
const PAGE_SRC = extractRange(
  PAGE_LINES,
  '  function set(id, text, cls) {',
  '  function pollDiagDetail() {',
  '  }');
const APP_SRC = extractRange(
  linesOf(APP_JS_PATH),
  '  window.kcEscapeHtml = function (s) {',
  '  window.kcEscapeHtml = function (s) {',
  '  };');

for (const [needle, what] of [
  ['function render(st) {', 'render'],
  ['function renderDiagCard(st) {', 'renderDiagCard'],
  ['function poll() {', 'poll'],
  ['function pollDiagDetail() {', 'pollDiagDetail'],
  ['function setStale(', 'setStale'],
  ['var BOOT_REASON_WORDS = {', 'BOOT_REASON_WORDS'],
]) {
  assert(PAGE_SRC.indexOf(needle) !== -1, 'sanity: extracted page range includes ' + what);
}
assert(APP_SRC.indexOf('replace(/&/g') !== -1, 'sanity: extracted the real kcEscapeHtml from app.js');
// The defect-2 fix is structural: if a combinator that waits for BOTH
// fetches comes back, every timing assertion below silently becomes vacuous.
// Scanned with line comments stripped -- the fix's own comment discusses
// Promise.all/allSettled by name, and matching that prose would make this
// assertion fire on correct code. (The strip is naive about '//' inside a
// string literal; there is none in this range, and a false FAIL here is
// loud and harmless, unlike a false pass.)
// The \r must come off FIRST: these files are CRLF, and in a non-multiline
// regex '$' does not match before a trailing \r, so /\/\/.*$/ silently
// matched nothing at all and this assertion read the prose it was meant to
// ignore. Caught because it failed on correct code -- the safe direction.
const PAGE_CODE = PAGE_SRC.split('\n')
  .map((l) => raw(l).replace(/\/\/.*/, '')).join('\n');
assert(PAGE_CODE.indexOf('Promise.all') === -1,
  'poll() does not gate the main render on the optional diag fetch (no Promise.all/allSettled)');

// --------------------------------------------------------------------------
// DOM / fetch stubs.
// --------------------------------------------------------------------------
function makeEl(id) {
  const classes = new Set();
  return {
    id: id,
    className: '',
    textContent: '',
    innerHTML: '',
    disabled: false,
    style: {},
    classList: {
      add: (c) => classes.add(c),
      remove: (c) => classes.delete(c),
      contains: (c) => classes.has(c),
      toggle: (c, on) => { if (on === undefined) { classes.has(c) ? classes.delete(c) : classes.add(c); } else if (on) { classes.add(c); } else { classes.delete(c); } },
    },
    addEventListener: function () {},
  };
}

/* fetchPlan: { main: <behaviour>, diag: <behaviour> } where a behaviour is
 *   {resolve: <json>} | {reject: true} | {hang: true}
 * "hang" returns a promise that never settles -- the slow-?diag=1 case. */
function makeContext(fetchPlan) {
  const els = {};
  const getEl = (id) => (els[id] || (els[id] = makeEl(id)));
  const calls = [];
  const ctx = {
    console: console,
    Date: Date,
    Promise: Promise,
    Number: Number,
    Object: Object,
    Array: Array,
    Math: Math,
    isNaN: isNaN,
    String: String,
    setTimeout: setTimeout,
    clearTimeout: clearTimeout,
    document: {
      getElementById: getEl,
      addEventListener: function () {},
      visibilityState: 'visible',
    },
    window: {
      kcUnit: { fmt: (c) => ((c === null || c === undefined || isNaN(c)) ? 'n/a' : c.toFixed(1) + 'C') },
      addEventListener: function () {},
    },
    fetch: function (url) {
      calls.push(url);
      const which = url.indexOf('diag=1') !== -1 ? 'diag' : 'main';
      const b = fetchPlan[which] || { reject: true };
      if (b.hang) return new Promise(function () {});
      if (b.reject) return Promise.reject(new Error('stubbed network failure'));
      return Promise.resolve({ ok: true, json: function () { return Promise.resolve(b.resolve); } });
    },
  };
  vm.createContext(ctx);
  vm.runInContext(APP_SRC, ctx);
  vm.runInContext(PAGE_SRC, ctx);
  return { ctx, els: getEl, calls };
}

// A realistic default /api/status payload: the safety link HAS spoken, so the
// diag card renders its detail branch -- the precondition under which the
// defects were reachable at all.
function mainDoc(over) {
  return Object.assign({
    ok: true,
    safety_ready: true,
    link_version_known: true, link_version_compatible: true,
    self_protocol_version: 12, peer_protocol_version: 12,
    safety_temp_c: 24.5, enclosure_temp_c: 26.0, power_w: 0,
    safety_build_known: false,
    diag_ever_received: true,
    diag_state: 2, diag_trip_reason: 0,
    diag_trip_reason_words: 'none', diag_trip_reason_cause: '-', diag_trip_reason_remedy: '-',
    diag_warn_mask: 0, diag_trip_mask: 0, diag_context_age_100ms: 3,
    trip_event_ever_received: false,
  }, over || {});
}
const DIAG_DOC = {
  ok: true,
  diag_ever_received: true,
  diag_boot_reason: 0x01,
  diag_context_frames_ok: 4211,
  diag_context_frames_bad: 2,
  diag_tx_frames_dropped: 0,
};

// Let the stubbed fetches' promise chains drain. Each tick of the chain is a
// microtask; a few macrotask turns is far more than enough and never waits on
// a real timer.
function flush() {
  return new Promise((r) => setTimeout(r, 0))
    .then(() => new Promise((r) => setTimeout(r, 0)))
    .then(() => new Promise((r) => setTimeout(r, 0)));
}

async function run(fetchPlan) {
  const h = makeContext(fetchPlan);
  vm.runInContext('poll()', h.ctx);
  await flush();
  return h;
}

(async function main() {
  // ------------------------------------------------------------------------
  // Group 1: both documents arrive. The happy path still works, and
  // diag_boot_reason -- fetched but rendered NOWHERE before this pass
  // (defect 3) -- is now on the card as a name, not only a raw integer.
  // ------------------------------------------------------------------------
  {
    const h = await run({ main: { resolve: mainDoc() }, diag: { resolve: DIAG_DOC } });
    const card = h.els('diagCard').innerHTML;
    assert(card.indexOf('4211 / 2') !== -1, 'both ok: frame counters render from the diag document');
    assert(card.indexOf('TX frames dropped') !== -1, 'both ok: TX drop counter row present');
    assert(card.indexOf('power-on') !== -1, 'both ok: boot reason rendered as a name (defect 3)');
    assert(card.indexOf('0x01') !== -1, 'both ok: boot reason also shows the raw value');
    assert(card.indexOf('UNAVAILABLE') === -1, 'both ok: no unavailable marker');
    assert(card.indexOf('undefined') === -1, 'both ok: no undefined anywhere on the card');
    assert(h.els('staleBanner').classList.contains('show') === false, 'both ok: no staleness banner');
    assert(h.els('safetyReady').textContent === 'linked', 'both ok: main rows rendered');
  }

  // ------------------------------------------------------------------------
  // Group 2: DEFECT 1 -- the diag fetch fails. This is the case that
  // previously rendered "undefined / undefined" inside an otherwise healthy-
  // looking card, with no staleness signal at all.
  // ------------------------------------------------------------------------
  {
    const h = await run({ main: { resolve: mainDoc() }, diag: { reject: true } });
    const card = h.els('diagCard').innerHTML;
    assert(card.indexOf('undefined') === -1,
      'diag fails: the card contains no "undefined" text (defect 1)');
    assert(card.indexOf('UNAVAILABLE') !== -1,
      'diag fails: the card says the diagnostic detail is UNAVAILABLE');
    assert(card.indexOf('request failed') !== -1,
      'diag fails: the reason given is a failed request, not a pending one');
    assert(card.indexOf('value kc-live-value warn') !== -1,
      'diag fails: the unavailable row is marked with the existing .warn idiom, visibly unlike a healthy row');
    assert(card.indexOf('Context frames ok/bad') === -1 && card.indexOf('TX frames dropped') === -1,
      'diag fails: no counter row is rendered at all, rather than one with a missing/blank value');
    // The rows that come from the DEFAULT document are still live and must
    // stay -- the point is to lose only what was actually lost.
    assert(card.indexOf('Trip mask') !== -1 && card.indexOf('Context age') !== -1,
      'diag fails: rows sourced from the default document still render');
    assert(h.els('safetyReady').textContent === 'linked',
      'diag fails: the live safety rows still render');
    assert(h.els('staleBanner').classList.contains('show') === false,
      'diag fails: no false staleness banner -- the main document is fresh');
  }

  // ------------------------------------------------------------------------
  // Group 3: DEFECT 2 -- the diag fetch is SLOW (never settles this tick).
  // The main render must already have happened.
  // ------------------------------------------------------------------------
  {
    const h = await run({ main: { resolve: mainDoc() }, diag: { hang: true } });
    assert(h.els('safetyReady').textContent === 'linked',
      'diag slow: the main rows render without waiting for the diag fetch (defect 2)');
    assert(h.els('safetyTemp').textContent.indexOf('24.5') !== -1,
      'diag slow: the live temperature row is populated, not left at its placeholder');
    const card = h.els('diagCard').innerHTML;
    assert(card.indexOf('Trip mask') !== -1,
      'diag slow: the diag card renders its default-document rows immediately');
    assert(card.indexOf('undefined') === -1, 'diag slow: no undefined text while the fetch is outstanding');
    // An outstanding request must not be reported as a failed one -- the
    // first tick of every page load is exactly this state.
    assert(card.indexOf('has not answered yet') !== -1 && card.indexOf('request failed') === -1,
      'diag slow: the card says the request has not answered yet, NOT that it failed');
    assert(h.els('staleBanner').classList.contains('show') === false,
      'diag slow: an outstanding diag fetch does not make the page claim to be stale');
  }

  // ------------------------------------------------------------------------
  // Group 4: the diag fetch returns 200 but WITHOUT the detail keys -- what
  // the firmware actually sends when diag_ever_received is false
  // (dashboard_status_http.c emits the counters only inside that branch).
  // A successful fetch is not the same as usable data.
  // ------------------------------------------------------------------------
  {
    const h = await run({
      main: { resolve: mainDoc() },
      diag: { resolve: { ok: true, diag_ever_received: false } },
    });
    const card = h.els('diagCard').innerHTML;
    assert(card.indexOf('undefined') === -1, 'diag 200 without keys: no undefined text');
    assert(card.indexOf('UNAVAILABLE') !== -1, 'diag 200 without keys: reported as unavailable');
    assert(card.indexOf('did not carry them') !== -1,
      'diag 200 without keys: the reason names the document, not a failed or pending request');
  }

  // ------------------------------------------------------------------------
  // Group 5: the MAIN document fails. The staleness banner is still the main
  // document's job, and a working diag fetch must not paper over it.
  // ------------------------------------------------------------------------
  {
    const h = makeContext({ main: { resolve: mainDoc() }, diag: { resolve: DIAG_DOC } });
    vm.runInContext('poll()', h.ctx);         // one good poll, to set lastGoodAt
    await flush();
    // Now fail the main document on the next poll.
    vm.runInContext('__failMain = true', h.ctx);
    h.ctx.fetch = function (url) {
      if (url.indexOf('diag=1') !== -1) {
        return Promise.resolve({ ok: true, json: () => Promise.resolve(DIAG_DOC) });
      }
      return Promise.reject(new Error('stubbed main failure'));
    };
    vm.runInContext('poll()', h.ctx);
    await flush();
    assert(h.els('staleBanner').classList.contains('show') === true,
      'main fails: the staleness banner shows even though the diag fetch succeeded');
  }

  // ------------------------------------------------------------------------
  // Group 6: boot-reason bits compose (KILNLINK_DIAG_BOOT_WATCHDOG is set
  // alongside whichever fatal-fault bit caused it -- kilnlink_diag.h), and an
  // unrecognised bit is called out rather than silently dropped.
  // ------------------------------------------------------------------------
  {
    const h = await run({
      main: { resolve: mainDoc() },
      diag: { resolve: Object.assign({}, DIAG_DOC, { diag_boot_reason: 0x02 | 0x20 }) },
    });
    const card = h.els('diagCard').innerHTML;
    assert(card.indexOf('watchdog') !== -1 && card.indexOf('assert failed') !== -1,
      'boot reason: composite watchdog+assert decodes both bits');
  }
  {
    const h = await run({
      main: { resolve: mainDoc() },
      diag: { resolve: Object.assign({}, DIAG_DOC, { diag_boot_reason: 0x40 }) },
    });
    assert(h.els('diagCard').innerHTML.indexOf('unrecognised') !== -1,
      'boot reason: an undefined bit is reported as unrecognised, not dropped');
  }
  {
    const h = await run({
      main: { resolve: mainDoc() },
      diag: { resolve: Object.assign({}, DIAG_DOC, { diag_boot_reason: 0 }) },
    });
    assert(h.els('diagCard').innerHTML.indexOf('none reported') !== -1,
      'boot reason: zero renders as "none reported", not as a missing value');
  }

  // ------------------------------------------------------------------------
  // Group 7: mirror drift. BOOT_REASON_WORDS is a hand-maintained JS copy of
  // kilnlink_diag.h's kilnlink_diag_boot_flag_t; this repo's standing lesson
  // is that such pairs drift silently. Compare against the header itself.
  // ------------------------------------------------------------------------
  {
    const hdr = fs.readFileSync(DIAG_H_PATH, 'utf8');
    const hdrBits = {};
    const re = /KILNLINK_DIAG_BOOT_([A-Z_]+)\s*=\s*0x([0-9a-fA-F]+)u/g;
    let m;
    while ((m = re.exec(hdr)) !== null) hdrBits[parseInt(m[2], 16)] = m[1];
    assert(Object.keys(hdrBits).length >= 6,
      'mirror: parsed the boot-flag enum out of kilnlink_diag.h (got ' +
      Object.keys(hdrBits).length + ' bits)');
    const h = makeContext({ main: { reject: true }, diag: { reject: true } });
    const jsBits = vm.runInContext('BOOT_REASON_WORDS', h.ctx);
    const jsKeys = Object.keys(jsBits).map(Number).sort((a, b) => a - b);
    const hdrKeys = Object.keys(hdrBits).map(Number).sort((a, b) => a - b);
    assert(JSON.stringify(jsKeys) === JSON.stringify(hdrKeys),
      'mirror: BOOT_REASON_WORDS covers exactly the header\'s bits (js ' +
      JSON.stringify(jsKeys) + ' vs header ' + JSON.stringify(hdrKeys) + ')');
    // Each label must plausibly correspond to its C name, so a copy-paste that
    // pairs the right bit with the wrong words is caught too.
    // Compared on letters only, so the JS table stays free to be readable
    // ("power-on" for POWERON) without weakening the pairing check.
    const letters = (s) => String(s).toLowerCase().replace(/[^a-z]/g, '');
    let mismatched = [];
    for (const bit of hdrKeys) {
      if (letters(jsBits[bit]).indexOf(letters(hdrBits[bit])) === -1) {
        mismatched.push('0x' + bit.toString(16) + ': ' + hdrBits[bit] + ' vs ' + jsBits[bit]);
      }
    }
    assert(mismatched.length === 0,
      'mirror: each label matches its C flag name (' + (mismatched.join('; ') || 'all match') + ')');
  }

  console.log('');
  console.log(passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
})().catch((e) => {
  console.log('HARNESS ERROR: ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
