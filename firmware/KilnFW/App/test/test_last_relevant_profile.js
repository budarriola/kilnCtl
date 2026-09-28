/* Node-only test for main_page.html's "last relevant profile" persistence
 * (owner report, 2026-09-27): "i perodicly see a profile flash on the graph
 * even though there is nothing running. i would like to see the last
 * profile there untill i start or select another."
 *
 * Root cause (see the long comment above pollExec() in main_page.html,
 * marked "Owner report, 2026-09-27"): poll()/pollExec() used to react to a
 * run ending by calling `setActivePlanFor(sel.value)` -- sel.value being
 * whatever the <select> incidentally showed, not the profile that had just
 * run. This test extracts getPersistedLastProfileId/setPersistedLastProfileId/
 * pickInitialProfileId VERBATIM from the page and checks:
 *
 *   1. With nothing persisted and no last_run, pickInitialProfileId() falls
 *      back to the select's own current value (old behavior, unchanged as
 *      a last resort).
 *   2. With nothing persisted but the board reporting a present last_run,
 *      pickInitialProfileId() prefers that firmware-exposed id over the
 *      select's incidental value -- the preferred, no-new-route path
 *      (CLAUDE.md: URI handler headroom is tight).
 *   3. A persisted (localStorage) id outranks both -- it represents
 *      whichever of "last run" or "last explicitly selected" happened
 *      later, since both paths write through setPersistedLastProfileId().
 *   4. setPersistedLastProfileId(undefined) clears the persisted value
 *      (so an id of `undefined` doesn't get coerced into the string
 *      "undefined").
 *   5. Negative-test control: a build of pickInitialProfileId() with the
 *      persisted-id check removed (always reads last_run first) fails
 *      assertion 3 -- proving that assertion actually exercises the
 *      priority order rather than passing regardless.
 *
 * Run: node firmware/KilnFW/App/test/test_last_relevant_profile.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');

function extractThrough(startMarker, mustContain, stopMarker) {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l) === startMarker);
  if (startIdx === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const stopIdx = LINES.findIndex((l, i) => i > startIdx && raw(l) === stopMarker);
  if (stopIdx === -1) throw new Error('stop marker not found after start: ' + JSON.stringify(stopMarker));
  const text = LINES.slice(startIdx, stopIdx).join('\n');
  if (text.indexOf(mustContain) === -1) throw new Error('sanity: range missing ' + JSON.stringify(mustContain));
  return text;
}

const CODE = extractThrough(
  "var LAST_PROFILE_STORAGE_KEY = 'kcLastPreviewedProfileId';",
  'function pickInitialProfileId(sel)',
  '// Tracks the profile id the running/paused/etc. plan curve was last fetched'
);
if (CODE.indexOf('function pickInitialProfileId') === -1) {
  throw new Error('sanity: extracted range does not include pickInitialProfileId');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// Minimal in-memory localStorage, same shape Node lacks natively.
function makeFakeLocalStorage() {
  const store = {};
  return {
    getItem(k) { return Object.prototype.hasOwnProperty.call(store, k) ? store[k] : null; },
    setItem(k, v) { store[k] = String(v); },
    removeItem(k) { delete store[k]; },
  };
}

function makeContext(code, lastExecStatus) {
  const ctx = {
    window: { localStorage: makeFakeLocalStorage() },
    lastExecStatus: lastExecStatus,
    console,
  };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  return ctx;
}

// Group 1: nothing persisted, no last_run -- falls back to sel.value.
{
  const ctx = makeContext(CODE, null);
  const sel = { value: '7' };
  assert(ctx.pickInitialProfileId(sel) === '7',
    'no persisted id and no last_run: falls back to the select\'s current value');
}

// Group 2: nothing persisted, last_run present -- prefers firmware data.
{
  const ctx = makeContext(CODE, { last_run: { present: true, profile_id: 3 } });
  const sel = { value: '7' };
  assert(ctx.pickInitialProfileId(sel) === 3,
    'no persisted id, last_run present: prefers the firmware-exposed last_run.profile_id');
}

// Group 3: a persisted id outranks both last_run and the select's value.
{
  const ctx = makeContext(CODE, { last_run: { present: true, profile_id: 3 } });
  ctx.window.localStorage.setItem('kcLastPreviewedProfileId', '9');
  const sel = { value: '7' };
  assert(ctx.pickInitialProfileId(sel) === '9',
    'a persisted id (a later explicit selection or run) outranks last_run and the select value');
}

// Group 4: setPersistedLastProfileId(undefined) clears rather than storing "undefined".
{
  const ctx = makeContext(CODE, null);
  ctx.setPersistedLastProfileId('4');
  assert(ctx.getPersistedLastProfileId() === '4', 'sanity: persisted id round-trips');
  ctx.setPersistedLastProfileId(undefined);
  assert(ctx.getPersistedLastProfileId() === undefined,
    'setPersistedLastProfileId(undefined) clears the stored value instead of storing "undefined"');
}

// Group 5: negative-test control -- with the persisted-id check removed,
// group 3's distinguishing assertion must fail.
{
  const patchRe = /function pickInitialProfileId\(sel\) \{\r?\n  var id = getPersistedLastProfileId\(\);\r?\n/;
  if (!patchRe.test(CODE)) throw new Error('sanity: negative-test patch did not match pickInitialProfileId source');
  const BROKEN_CODE = CODE.replace(
    patchRe,
    'function pickInitialProfileId(sel) {\n  var id = undefined; // negative-test: persisted id never consulted\n'
  );
  const ctx = makeContext(BROKEN_CODE, { last_run: { present: true, profile_id: 3 } });
  ctx.window.localStorage.setItem('kcLastPreviewedProfileId', '9');
  const sel = { value: '7' };
  const wouldHavePassed = ctx.pickInitialProfileId(sel) === '9';
  assert(!wouldHavePassed,
    'negative control: with the persisted-id check removed, last_run wins instead -- proving group 3 exercises real priority logic');
}

console.log('\n' + passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
process.exit(0);
