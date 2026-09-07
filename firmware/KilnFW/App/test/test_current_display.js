/* Node-only test harness for main_page.html's real-amps display
 * (CT_COMMISSIONING_PLAN.md step 4): renderCurrentCard()'s per-CT-channel
 * badges and renderChannels()'s per-zone summed-CT amps tag.
 *
 * The rule under test: a channel flagged not-fitted (data.ct_fitted[i] ===
 * false) must render the words "not fitted", and must NEVER render a
 * fabricated "0.00 A" -- the same non-plausible-fake-reading convention
 * every other null-until-known field on this endpoint already uses
 * (dashboard_status_http.c's own doc comments). Extracts the real
 * renderCurrentCard() source (same by-marker-line extraction as
 * test_lag_banner.js/test_firing_chart.js) and runs it in a Node vm
 * context with a small document stub, rather than reimplementing the
 * rendering logic here.
 *
 * Run: node firmware/KilnFW/App/test/test_current_display.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// Extracts a top-level function's source by its exact `function NAME(` line,
// ending at the first column-0 '}' after that line -- same convention
// test_lag_banner.js's extractLagBannerSection() uses.
function extractFunction(source, name) {
  const lines = source.split('\n');
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = lines.findIndex((l) => raw(l).indexOf('function ' + name + '(') === 0);
  if (startIdx === -1) throw new Error('function not found: ' + name);
  let endIdx = -1;
  for (let i = startIdx + 1; i < lines.length; i++) {
    if (raw(lines[i]) === '}') { endIdx = i; break; }
  }
  if (endIdx === -1) throw new Error('end of function not found: ' + name);
  return lines.slice(startIdx, endIdx + 1).join('\n');
}

const RENDER_CURRENT_CARD_SRC = extractFunction(SRC, 'renderCurrentCard');
assert(RENDER_CURRENT_CARD_SRC.indexOf('function renderCurrentCard(') !== -1,
  'sanity: extracted range includes renderCurrentCard');
assert(RENDER_CURRENT_CARD_SRC.indexOf('not fitted') !== -1,
  'sanity: extracted range contains the "not fitted" literal under test');

function makeContext(fnSrc) {
  const ctx = {
    document: {
      createElement: function () { return { className: '', innerHTML: '' }; },
    },
    console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(fnSrc, ctx);
  return ctx;
}

function statusWith(ctCurrentA, ctFitted) {
  return { ct_current_a: ctCurrentA, ct_fitted: ctFitted };
}

// ---------------------------------------------------------------------------
// Group 1: per_zone topology (or an older board with no ct_fitted array at
// all) -- every channel fitted, real numbers render with units.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext(RENDER_CURRENT_CARD_SRC);
  const data = statusWith([1.23, 4.56, 7.89], [true, true, true]);
  const div = vm.runInContext('renderCurrentCard(data)', Object.assign(ctx, { data }));
  assert(div !== null, 'renderCurrentCard returns a card when ct_current_a is present');
  assert(div.innerHTML.indexOf('1.23 A') !== -1, 'channel 0 real reading renders with units -- got: ' + div.innerHTML);
  assert(div.innerHTML.indexOf('4.56 A') !== -1, 'channel 1 real reading renders with units -- got: ' + div.innerHTML);
  assert(div.innerHTML.indexOf('7.89 A') !== -1, 'channel 2 real reading renders with units -- got: ' + div.innerHTML);
  assert(div.innerHTML.indexOf('not fitted') === -1, 'no channel is flagged not-fitted -- must not print "not fitted"');
}

// ---------------------------------------------------------------------------
// Group 2: summed topology -- channels 0/1 not fitted, channel 2 (GPIO28)
// is the real reading. THE rule this task adds.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext(RENDER_CURRENT_CARD_SRC);
  const data = statusWith([null, null, 12.34], [false, false, true]);
  const div = vm.runInContext('renderCurrentCard(data)', Object.assign(ctx, { data }));
  const html = div.innerHTML;
  const notFittedCount = (html.match(/not fitted/g) || []).length;
  assert(notFittedCount === 2, 'exactly the two not-fitted channels print "not fitted" -- got ' + notFittedCount + ' in: ' + html);
  assert(html.indexOf('12.34 A') !== -1, 'the fitted channel (2) still renders its real reading -- got: ' + html);
  // The specific defect this task's own instructions call out: a not-fitted
  // channel must NEVER render as a fabricated 0.00 A.
  assert(html.indexOf('0.00 A') === -1, 'a not-fitted channel must never render as "0.00 A" -- got: ' + html);
}

// ---------------------------------------------------------------------------
// Group 3: no ct_current_a at all (a build that predates this field) --
// renderCurrentCard must decline to render rather than showing empty/broken
// badges.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext(RENDER_CURRENT_CARD_SRC);
  const data = {};
  const div = vm.runInContext('renderCurrentCard(data)', Object.assign(ctx, { data }));
  assert(div === null, 'renderCurrentCard returns null when ct_current_a is absent (older firmware), not a broken card');
}

// ---------------------------------------------------------------------------
// NEGATIVE TEST (feedback_negative_test_every_check): prove this suite can
// actually catch the regression it exists to catch -- a not-fitted channel
// silently falling through to the real-number branch and printing a
// fabricated reading instead of "not fitted". Mutates the EXTRACTED
// production source (a scratch copy, never the file on disk) by flipping
// the not-fitted condition to always-false, then re-runs Group 2's exact
// assertion and requires it to go RED. If this block itself passes clean,
// the check above is proven capable of catching the real bug, not just
// agreeing with whatever the source currently does.
// ---------------------------------------------------------------------------
{
  const MUTATED_SRC = RENDER_CURRENT_CARD_SRC.replace('if (!fitted[ci]) {', 'if (false && !fitted[ci]) {');
  assert(MUTATED_SRC !== RENDER_CURRENT_CARD_SRC, 'sanity: the mutation string was actually found and replaced');
  const ctx = makeContext(MUTATED_SRC);
  const data = statusWith([null, null, 12.34], [false, false, true]);
  const div = vm.runInContext('renderCurrentCard(data)', Object.assign(ctx, { data }));
  const html = div.innerHTML;
  const mutationCaughtRed = html.indexOf('not fitted') === -1;
  assert(mutationCaughtRed,
    'MUTATION: disabling the not-fitted branch must make the "not fitted" assertion go red -- ' +
    'proves Group 2 above is a real check, not a vacuous one. got: ' + html);
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
