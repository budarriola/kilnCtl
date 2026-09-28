/* Node-only test for main_page.html's "last relevant profile" persistence
 * (owner report, 2026-09-27): "i perodicly see a profile flash on the graph
 * even though there is nothing running. i would like to see the last
 * profile there untill i start or select another."
 *
 * Extracts getPersistedLastProfile/setPersistedLastProfileId/
 * newestFiredProfile/pickInitialProfileId VERBATIM from the page and checks
 * the "last run or last explicit selection, whichever is later" ordering,
 * including across a reload where a run was started from the LCD/UART while
 * the page was closed (ordered by GET /api/profiles' per-profile
 * last_run_started_unix_s against the selection's stored wall-clock time),
 * the legacy bare-id storage format, a deleted persisted id, and a negative
 * control that removes the timestamp comparison.
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
  'function pickInitialProfileId(sel, execSt, profiles)',
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


function makeCtx(code) {
  const ctx = { window: { localStorage: makeFakeLocalStorage() }, console, Date };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  return ctx;
}
const SEL = { value: '7' };
const LIST = [
  { id: 1, last_run_started_unix_s: 1000 },       // started at t = 1 000 000 ms
  { id: 3, last_run_started_unix_s: 5000 },       // started at t = 5 000 000 ms (newest)
  { id: 7, last_run_started_unix_s: 0 },          // never fired / no SNTP time
  { id: 9, last_run_started_unix_s: 0 },
];

// Group 1: nothing persisted, no list, no last_run -- falls back to sel.value.
{
  const ctx = makeCtx(CODE);
  assert(ctx.pickInitialProfileId(SEL, null, []) === '7',
    'nothing known: falls back to the select\'s current value');
}

// Group 2: nothing persisted, no list (no session), boot-record last_run present.
{
  const ctx = makeCtx(CODE);
  assert(ctx.pickInitialProfileId(SEL, { last_run: { present: true, profile_id: 3 } }, []) === '3',
    'no persisted id and no list: the boot-record last_run is the fallback');
}

// Group 3: an explicit selection made AFTER the newest recorded run wins.
{
  const ctx = makeCtx(CODE);
  ctx.setPersistedLastProfileId('9', 6000 * 1000);
  assert(ctx.pickInitialProfileId(SEL, null, LIST) === '9',
    'a selection newer than every recorded run start wins');
}

// Group 4: the reload case -- a run started (LCD/UART) AFTER the persisted
// selection, while the page was closed, must win over that selection.
{
  const ctx = makeCtx(CODE);
  ctx.setPersistedLastProfileId('9', 2000 * 1000);
  assert(ctx.pickInitialProfileId(SEL, null, LIST) === '3',
    'a run started after the persisted selection (page closed) wins over it');
}

// Group 5: a legacy bare-string value reads as "some time ago" -- any
// recorded run beats it, and with no list it is still used.
{
  const ctx = makeCtx(CODE);
  ctx.window.localStorage.setItem('kcLastPreviewedProfileId', '9');
  const got = ctx.getPersistedLastProfile();
  assert(got.id === '9' && got.t === 0, 'legacy bare id parses as {id, t: 0}');
  assert(ctx.pickInitialProfileId(SEL, null, LIST) === '3', 'legacy bare id loses to a recorded run');
  assert(ctx.pickInitialProfileId(SEL, null, []) === '9', 'legacy bare id is used when there is no list');
}

// Group 6: a persisted id that is no longer in the list (deleted) is ignored.
{
  const ctx = makeCtx(CODE);
  ctx.setPersistedLastProfileId('42', 9999 * 1000);
  assert(ctx.pickInitialProfileId(SEL, null, LIST) === '3',
    'a persisted id missing from the loaded list is ignored');
}

// Group 7: clearing stores nothing, not the string "undefined".
{
  const ctx = makeCtx(CODE);
  ctx.setPersistedLastProfileId('4', 1);
  assert(ctx.getPersistedLastProfile().id === '4', 'sanity: persisted id round-trips');
  ctx.setPersistedLastProfileId(undefined);
  assert(ctx.getPersistedLastProfile() === undefined,
    'setPersistedLastProfileId(undefined) clears the stored value');
}

// Group 8: negative control -- with the timestamp comparison removed (the
// persisted selection always wins, as in this change's first cut), group
// 4's scenario picks the stale selection.
{
  const needle = 'return (fired.t > persisted.t) ? fired.id : persisted.id;';
  if (CODE.indexOf(needle) === -1) throw new Error('sanity: negative-test needle not found');
  const ctx = makeCtx(CODE.replace(needle, 'return persisted.id;'));
  ctx.setPersistedLastProfileId('9', 2000 * 1000);
  assert(ctx.pickInitialProfileId(SEL, null, LIST) !== '3',
    'negative control: without the timestamp comparison the stale selection wins -- group 4 is real');
}

console.log('\n' + passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
process.exit(0);
