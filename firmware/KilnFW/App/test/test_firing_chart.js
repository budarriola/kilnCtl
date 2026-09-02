/* Node-only test harness for main_page.html's firing-chart colour guard and
 * per-zone client-side sampling (drawHistoryChart / sampleFiringZoneTemps).
 *
 * Why this exists: owner report 2026-09-01 -- "i am still only seeing one
 * zone on the web graph, and for some reason the duty is the same color as
 * zone 1. duty and planed should never be the same color as the zones."
 * Two separate defects: (1) SERIES_COLORS.duty literally equalled
 * CH_COLORS[1], and the guard that was supposed to catch that only ever
 * console.warn'd -- a convention, not a guarantee. (2) the firing chart only
 * ever plotted history.csv's single representative-zone column.
 *
 * This harness does NOT run the whole page (main_page.html's inline script
 * is ~2500 lines with a full poll()/fetch startup sequence that would need a
 * much heavier DOM stub to even reach). Instead it extracts small, marked
 * source ranges by exact line content -- the same functions/consts the
 * production code runs, not a reimplementation -- and executes them in a
 * fresh Node vm context per test. If a future edit renames or moves one of
 * the marker lines, extraction throws immediately (see extractRange) rather
 * than silently testing stale or empty code.
 *
 * Run: node firmware/KilnFW/App/test/test_firing_chart.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const PAGE_PATH = path.join(__dirname, '..', 'drivers', 'main_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');

// Slice LINES[startMarker-line .. endMarker-line] inclusive, both located by
// exact-match search (not line number) so this stays correct across
// unrelated edits elsewhere in the file, and throws loudly if either marker
// has drifted out of the file instead of silently extracting the wrong span.
function extractRange(startMarker, endMarker) {
  // Note: end-marker matching is against the RAW line (CRLF stripped only),
  // not l.trim() -- a top-level "}" closing a function/IIFE has zero
  // indentation in this file, while a nested block's closing brace (e.g. an
  // inner "if (...) { ... }") is indented. Matching on .trim() would strip
  // that indentation away and match the first NESTED close instead of the
  // real end of the function, silently truncating the extraction.
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

// ---------------------------------------------------------------------------
// Group 1: the colour-collision guard actually fires.
// ---------------------------------------------------------------------------
const COLOR_GUARD_SRC = extractRange(
  "const CH_COLORS = ['#e63', '#3a9', '#37f', '#c8a', '#fa3', '#0bb'];",
  '})();'
);
// Sanity: the extraction grabbed the colour-guard IIFE, not some other
// "})();"-terminated block further down the file (2952 is a different one --
// see this file's header comment on why line numbers alone aren't trusted).
assert(COLOR_GUARD_SRC.indexOf('checkSeriesColorsDontCollideWithZoneColors') !== -1,
  'sanity: extracted range is the colour-guard IIFE');
assert(COLOR_GUARD_SRC.indexOf('SERIES_COLORS') !== -1 && COLOR_GUARD_SRC.indexOf('duty') !== -1,
  'sanity: extracted range defines SERIES_COLORS');

function runColorGuard(src) {
  const ctx = vm.createContext({ console });
  new vm.Script(src, { filename: 'main_page.html (colour guard slice)' }).runInContext(ctx);
}

// Baseline: the real, checked-in palette must NOT throw.
(function testRealPaletteDoesNotThrow() {
  let threw = false;
  try { runColorGuard(COLOR_GUARD_SRC); } catch (e) { threw = true; }
  assert(!threw, 'real SERIES_COLORS palette does not collide with CH_COLORS (no throw)');
})();

// Negative test: mutate SERIES_COLORS.duty to literally the bug that was
// reported ('#3a9', CH_COLORS[1]) and prove the guard throws. This is the
// exact defect from the owner's report reproduced as a fixture.
(function testMutatedDutyColorThrows() {
  const mutated = COLOR_GUARD_SRC.replace(
    "const SERIES_COLORS = { duty: '#888', plan: '#96c', guard: '#c22' };",
    "const SERIES_COLORS = { duty: '#3a9', plan: '#96c', guard: '#c22' };"
  );
  assert(mutated !== COLOR_GUARD_SRC, 'sanity: mutation actually changed the source');
  let threw = false, message = '';
  try { runColorGuard(mutated); } catch (e) { threw = true; message = e.message; }
  assert(threw, 'guard throws when SERIES_COLORS.duty is set equal to a CH_COLORS entry');
  assert(message.indexOf('SERIES_COLORS.duty') !== -1 && message.indexOf('collides') !== -1,
    'thrown error names the colliding key');
})();

// Same for plan and guard, so this isn't just special-cased on 'duty'.
(function testMutatedPlanAndGuardColorsThrow() {
  ['plan', 'guard'].forEach(function (key) {
    const re = new RegExp(key + ": '#[0-9a-f]{3}'");
    const collideWith = 'CH_COLORS[2]'; // '#37f'
    const mutated = COLOR_GUARD_SRC.replace(re, key + ": '#37f'");
    let threw = false;
    try { runColorGuard(mutated); } catch (e) { threw = true; }
    assert(threw, 'guard throws when SERIES_COLORS.' + key + ' is set equal to ' + collideWith);
  });
})();

// ---------------------------------------------------------------------------
// Group 2: per-zone client-side sampling (sampleFiringZoneTemps) -- one
// series per enabled zone, disabled/faulted zones omitted, ASYMMETRIC
// fixture so a transposed or off-by-one zone index fails rather than passes.
// ---------------------------------------------------------------------------
const ZONE_SAMPLING_SRC = extractRange(
  'var CLIENT_ZONE_SAMPLE_MIN_INTERVAL_S = 5;', // pulls through pushCappedPoint's closing brace too
  '}'
) + '\n' + extractRange(
  'function isFiringActive(state) {',
  '}'
) + '\n' + extractRange(
  'function zoneMaskLowestBit(mask) {',
  '}'
) + '\n' + extractRange(
  'function popcount8(mask) {',
  '}'
) + '\n' + extractRange(
  'var firingZoneSeries = {};',
  'var wasFiringActive = false;'
) + '\n' + extractRange(
  'function sampleFiringZoneTemps(data, st) {',
  '}'
);
assert(ZONE_SAMPLING_SRC.indexOf('function sampleFiringZoneTemps') !== -1,
  'sanity: extracted range includes sampleFiringZoneTemps');
assert(ZONE_SAMPLING_SRC.indexOf('function pushCappedPoint') !== -1,
  'sanity: extracted range includes pushCappedPoint (pulled in with the CLIENT_* consts)');

function loadZoneSampler() {
  const ctx = vm.createContext({ console });
  new vm.Script(ZONE_SAMPLING_SRC,
    { filename: 'main_page.html (zone sampling slice)' }).runInContext(ctx);
  return ctx;
}

// Three-zone firing, zone_mask = 0b0111 (zones 0,1,2 active) -- zone 0 is the
// representative zone (lowest bit) and is skipped by sampleFiringZoneTemps
// (history.csv already covers it), zones 1 and 2 must each get their OWN
// point with their OWN, DIFFERENT temperature -- an asymmetric fixture so a
// transposed index (zone 1's reading landing in firingZoneSeries[2] or vice
// versa) fails this test instead of passing it. Zone 3 is present in the
// channel list but invalid (disabled/faulted) and must produce no trace at
// all, and zone 4 is a channel that is simply not part of this run's
// zone_mask -- still sampled (client-side sampling does not gate on
// zone_mask membership, only validity; the drawing side labels it "not in
// this run") but its value must be its own, not smeared into zone 1/2.
(function testPerZoneSamplingIsAsymmetricAndSkipsDisabled() {
  const ctx = loadZoneSampler();
  const data = {
    thermo_ready: true,
    channels: [
      { channel: 0, valid: true, spi_failed: false, temp_c: 111.1 },
      { channel: 1, valid: true, spi_failed: false, temp_c: 222.2 },
      { channel: 2, valid: true, spi_failed: false, temp_c: 333.3 },
      { channel: 3, valid: false, spi_failed: false, temp_c: 999.9 }, // disabled -- must be omitted
      { channel: 4, valid: true, spi_failed: true, temp_c: 888.8 },   // SPI fault -- must be omitted
    ],
  };
  const st = {
    state: 'running',
    zone_mask: 0x07, // zones 0,1,2
    total_elapsed_s: 42,
    zones: [
      { zone: 0, duty: 0.55 },
      { zone: 1, duty: 0.10 },
      { zone: 2, duty: 0.20 },
    ],
  };
  ctx.sampleFiringZoneTemps(data, st);

  assert(ctx.firingZoneSeries[0] === undefined,
    'representative zone (0) is NOT client-collected (history.csv already covers it)');
  assert(!!ctx.firingZoneSeries[1] && ctx.firingZoneSeries[1].length === 1,
    'zone 1 got exactly one collected point');
  assert(!!ctx.firingZoneSeries[2] && ctx.firingZoneSeries[2].length === 1,
    'zone 2 got exactly one collected point');
  assert(ctx.firingZoneSeries[1][0].c === 222.2,
    'zone 1 point carries zone 1\'s OWN temperature, not zone 2\'s (222.2, got ' +
    (ctx.firingZoneSeries[1] && ctx.firingZoneSeries[1][0].c) + ')');
  assert(ctx.firingZoneSeries[2][0].c === 333.3,
    'zone 2 point carries zone 2\'s OWN temperature, not zone 1\'s (333.3, got ' +
    (ctx.firingZoneSeries[2] && ctx.firingZoneSeries[2][0].c) + ')');
  assert(ctx.firingZoneSeries[3] === undefined, 'disabled (invalid) channel 3 produces NO trace');
  assert(ctx.firingZoneSeries[4] === undefined, 'SPI-faulted channel 4 produces NO trace');

  // Whole-kiln average duty (2026-09-02, owner: "(duty0+duty1+duty2+duty3)
  // /zones") -- NOT the representative zone's own duty any more. zone_mask
  // is 0x07 (zones 0,1,2), so the divisor is 3 (popcount8), and the sum is
  // every zone's duty that is BOTH active (present in st.zones, per
  // append_zone_status_json()) AND in the mask: (0.55+0.10+0.20)/3.
  const expectedAvgDuty = (0.55 + 0.10 + 0.20) / 3;
  assert(ctx.firingDutySeries.length === 1 &&
    Math.abs(ctx.firingDutySeries[0].duty - expectedAvgDuty) < 1e-9,
    'whole-kiln average duty (' + expectedAvgDuty.toFixed(4) + ') collected, got ' +
    (ctx.firingDutySeries[0] && ctx.firingDutySeries[0].duty));
})();

// A zone_mask bit with NO matching entry in st.zones (e.g. a zone configured
// but not yet reporting) must still divide by the FULL mask popcount, not
// just however many zones happened to report -- otherwise a slow/missing
// zone would inflate the average instead of being absent from it. Negative
// fixture: mask claims 3 zones, only 2 report.
(function testAverageDutyDividesByFullMaskNotJustReportingZones() {
  const ctx = loadZoneSampler();
  const data = { thermo_ready: true, channels: [] };
  const st = {
    state: 'running',
    zone_mask: 0x07, // zones 0,1,2 -- 3 zones
    total_elapsed_s: 10,
    zones: [ // only zone 0 and 1 report this tick
      { zone: 0, duty: 0.90 },
      { zone: 1, duty: 0.30 },
    ],
  };
  ctx.sampleFiringZoneTemps(data, st);
  const expected = (0.90 + 0.30) / 3; // divide by 3, not 2
  assert(ctx.firingDutySeries.length === 1 &&
    Math.abs(ctx.firingDutySeries[0].duty - expected) < 1e-9,
    'divides by full zone_mask popcount (3), not just the 2 zones that reported');
})();

// A repeat call inside the same CLIENT_ZONE_SAMPLE_MIN_INTERVAL_S window must
// NOT add a second point (decimation) -- proves the interval gate is live,
// not a no-op.
(function testDecimationWithinIntervalDoesNotDuplicate() {
  const ctx = loadZoneSampler();
  const mk = (t) => ({
    thermo_ready: true,
    channels: [{ channel: 1, valid: true, spi_failed: false, temp_c: 100 + t }],
  });
  const stAt = (t) => ({ state: 'running', zone_mask: 0x03, total_elapsed_s: t, zones: [{ zone: 0, duty: 0.1 }] });
  ctx.sampleFiringZoneTemps(mk(0), stAt(0));
  ctx.sampleFiringZoneTemps(mk(1), stAt(1)); // 1s later, inside the 5s gate
  assert(ctx.firingZoneSeries[1].length === 1, 'a sample inside the decimation window is dropped');
  ctx.sampleFiringZoneTemps(mk(6), stAt(6)); // 6s later, past the 5s gate
  assert(ctx.firingZoneSeries[1].length === 2, 'a sample past the decimation window is kept');
})();

// Idle/paused-off state: sampleFiringZoneTemps must be a no-op so a finished
// or never-started run never grows a phantom trace.
(function testNoSamplingWhenNotFiring() {
  const ctx = loadZoneSampler();
  ctx.sampleFiringZoneTemps(
    { thermo_ready: true, channels: [{ channel: 1, valid: true, spi_failed: false, temp_c: 50 }] },
    { state: 'idle', zone_mask: 0x03, total_elapsed_s: 5, zones: [] }
  );
  assert(Object.keys(ctx.firingZoneSeries).length === 0, 'no state, no run: nothing is collected');
})();

// ---------------------------------------------------------------------------
console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exitCode = 1;
}
