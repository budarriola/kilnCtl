/* Node-only test harness for diagnostics_page.html's safety-processor
 * thermocouple card (renderSafetyTcCard), added 2026-09-08 (owner request:
 * "safety thermocouple faults should show next to the others in
 * diagnostics"). Extracts the real source (same extraction-by-marker-line
 * approach as test_lag_banner.js) rather than reimplementing the logic.
 *
 * The whole point of this feature is distinguishing four states that, on the
 * dashboard's old cached safety_temp_c tile, all looked like a single
 * plausible number or a generic "n/a":
 *   - "ok"             -- TC and CJ both good, no fault bits
 *   - "faulted"         -- a real MAX31856 fault bit (e.g. open circuit) --
 *                          chip alive, probe is the problem
 *   - "not_converting"  -- TC and CJ both NaN, zero fault bits -- the state
 *                          seen the night this was written, indistinguishable
 *                          on the wire from a CR1 type-verify failure
 *   - "no_link"         -- nothing to show at all
 *
 * Run: node firmware/KilnFW/App/test/test_safety_tc_diagnostics.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'diagnostics_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');

function extractRange(startMarker, endMarker) {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l) === startMarker);
  if (startIdx === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const endIdx = LINES.findIndex((l, i) => i >= startIdx && raw(l) === endMarker);
  if (endIdx === -1) throw new Error('end marker not found after start: ' + JSON.stringify(endMarker));
  return LINES.slice(startIdx, endIdx + 1).join('\n');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

const FAULT_BITS_SRC = extractRange(
  "  var FAULT_BITS = [",
  '  }'
);
assert(FAULT_BITS_SRC.indexOf('function faultSummary(') !== -1,
  'sanity: extracted range includes faultSummary');

const CARD_SRC = extractRange(
  '  function renderSafetyTcCard(s) {',
  '  }'
);
assert(CARD_SRC.indexOf('not_converting') !== -1,
  'sanity: extracted range includes the not_converting label');

function makeContext() {
  const ctx = {
    window: {
      kcEscapeHtml: function (s) { return String(s); },
      kcUnit: { fmt: function (c) { return (c === null || c === undefined || isNaN(c)) ? 'n/a' : c.toFixed(1) + 'C'; } },
    },
    console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(FAULT_BITS_SRC, ctx);
  vm.runInContext(CARD_SRC, ctx);
  return ctx;
}

function render(safety) {
  const ctx = makeContext();
  ctx.__safety = safety;
  return vm.runInContext('renderSafetyTcCard(__safety)', ctx);
}

// ---------------------------------------------------------------------------
// Group 1: no data at all -- renders nothing (server never sent a "safety"
// key, e.g. an old firmware build).
// ---------------------------------------------------------------------------
assert(render(undefined) === '', 'missing safety block: renders nothing');
assert(render(null) === '', 'null safety block: renders nothing');

// ---------------------------------------------------------------------------
// Group 2: link down.
// ---------------------------------------------------------------------------
{
  const html = render({ state: 'no_link', link_up: false });
  assert(html.indexOf('LINK DOWN') !== -1, 'no_link: says link down');
  assert(html.indexOf('TC:') === -1, 'no_link: does not print a stale TC/CJ reading');
}

// ---------------------------------------------------------------------------
// Group 3: healthy.
// ---------------------------------------------------------------------------
{
  const html = render({
    state: 'ok', link_up: true, link_age_ms: 400, tc_c: 22.5, cj_c: 23.1,
    fault_status: 0, not_installed: false, injected: false,
  });
  assert(html.indexOf('OK') !== -1, 'ok: fault summary reads OK');
  assert(html.indexOf('22.5') !== -1, 'ok: shows the TC reading');
  assert(html.indexOf('23.1') !== -1, 'ok: shows the CJ reading separately from TC');
  assert(html.indexOf('faulted') === -1, 'ok: not tagged as faulted');
}

// ---------------------------------------------------------------------------
// Group 4: real hardware fault (open circuit) -- chip alive, probe is the
// problem. Cold junction stays a real, separate reading.
// ---------------------------------------------------------------------------
{
  const html = render({
    state: 'faulted', link_up: true, link_age_ms: 400, tc_c: null, cj_c: 24.0,
    fault_status: 0x01, not_installed: false, injected: false,
  });
  assert(html.indexOf('OPEN') !== -1, 'faulted (open circuit): decodes the OPEN fault bit');
  assert(html.indexOf('24.0') !== -1, 'faulted (open circuit): cold junction still shown as a real reading');
  assert(html.indexOf('not_converting') === -1 && html.indexOf('NOT CONVERTING') === -1,
    'faulted (open circuit): NOT rendered as the not_converting state');
}

// ---------------------------------------------------------------------------
// Group 5: THE distinguishing case -- both TC and CJ unavailable with ZERO
// fault bits (chip not converting, or a CR1 type-verify failure -- these
// cannot be told apart with today's wire data, and the label says so).
// This must NOT look like "OK" and must NOT look like "OPEN circuit".
// ---------------------------------------------------------------------------
{
  const html = render({
    state: 'not_converting', link_up: true, link_age_ms: 900, tc_c: null, cj_c: null,
    fault_status: 0, not_installed: false, injected: false,
  });
  assert(html.indexOf('NOT CONVERTING') !== -1, 'not_converting: labelled distinctly');
  assert(html.indexOf('OK') === -1, 'not_converting: does not read as OK');
  assert(html.indexOf('OPEN') === -1, 'not_converting: does not read as an open-circuit fault (no real fault bit set)');
  assert(html.indexOf('n/a') !== -1, 'not_converting: TC/CJ shown as n/a, not a stale plausible number');
}

// ---------------------------------------------------------------------------
// Group 6: declared-not-installed / bench-injected are independent booleans
// shown alongside the state, not folded into it.
// ---------------------------------------------------------------------------
{
  const html = render({
    state: 'ok', link_up: true, link_age_ms: 400, tc_c: 20.0, cj_c: 20.1,
    fault_status: 0, not_installed: true, injected: false,
  });
  assert(html.indexOf('Declared not installed') !== -1, 'not_installed: flagged even though state is ok');
}
{
  const html = render({
    state: 'ok', link_up: true, link_age_ms: 400, tc_c: 20.0, cj_c: 20.1,
    fault_status: 0, not_installed: false, injected: true,
  });
  assert(html.indexOf('synthetic') !== -1, 'injected: flagged as a synthetic bench reading');
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
