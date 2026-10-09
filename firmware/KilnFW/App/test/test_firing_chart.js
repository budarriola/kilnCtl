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

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
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

// chColor() is the one mapping every zone swatch and chart line uses; pin it
// to the real palette, including the modulo wrap for channel >= 6.
(function testChColorMapsChannelsToPalette() {
  const ctx = vm.createContext({ console });
  new vm.Script(COLOR_GUARD_SRC, { filename: 'main_page.html (chColor slice)' }).runInContext(ctx);
  const at = (n) => vm.runInContext('chColor(' + n + ')', ctx);
  assert(at(0) === '#e63' && at(1) === '#3a9' && at(5) === '#0bb',
    'chColor(0/1/5) returns the matching CH_COLORS entries');
  assert(at(6) === '#e63' && at(7) === '#3a9', 'chColor wraps modulo the palette length');
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
  // zone_mask: 0 -- an idle executor cannot report a non-zero zone_mask
  // (profile_executor_status.c only fills zone_mask while state != IDLE);
  // 0x03 here was a fixture the real firmware can never emit (opus review
  // of 3f9b1a9/2a8ff7e), and is exactly why the NaN-duty-while-idle bug
  // this file's mask tests now cover went uncaught.
  ctx.sampleFiringZoneTemps(
    { thermo_ready: true, channels: [{ channel: 1, valid: true, spi_failed: false, temp_c: 50 }] },
    { state: 'idle', zone_mask: 0x00, total_elapsed_s: 5, zones: [] }
  );
  assert(Object.keys(ctx.firingZoneSeries).length === 0, 'no state, no run: nothing is collected');
})();

// ---------------------------------------------------------------------------
// Group 3: per-row zone_mask (2026-09-02, opus review of 3f9b1a9/2a8ff7e).
// parseHistoryCsv must pick up history.csv's trailing zone_mask column, and
// avgMaskedDuty must divide by the zones that actually reported this row,
// not the full mask popcount -- both fixed together, since the old bug was
// exactly "no per-row mask" (forced every row to share lastExecStatus.
// zone_mask, which is 0 while idle) PLUS "divide by the full mask" (a
// single NaN dragged the average toward zero even with a correct mask).
// ---------------------------------------------------------------------------
const MASK_SRC = extractRange(
  'var HISTORY_ZONE_COUNT = 3; // must match firmware\'s MAX31856_CHANNEL_COUNT (profile_executor.h)',
  '}'
) + '\n' + extractRange(
  'function popcount8(mask) {',
  '}'
) + '\n' + extractRange(
  'function avgMaskedDuty(dutyArr, mask, zoneCount) {',
  '}'
);
assert(MASK_SRC.indexOf('function parseHistoryCsv') !== -1,
  'sanity: extracted range includes parseHistoryCsv');
assert(MASK_SRC.indexOf('function avgMaskedDuty') !== -1,
  'sanity: extracted range includes avgMaskedDuty');

function loadMaskFns() {
  const ctx = vm.createContext({ console });
  new vm.Script(MASK_SRC, { filename: 'main_page.html (mask slice)' }).runInContext(ctx);
  return ctx;
}

// history.csv's actual row shape: elapsed_s,desired_c, then one
// z<N>_actual_c,z<N>_duty,z<N>_guard triple per zone (HISTORY_ZONE_COUNT ==
// 3 here), then the trailing zone_mask column dashboard_http.c appends.
(function testParseHistoryCsvReadsTrailingZoneMask() {
  const ctx = loadMaskFns();
  const csv = 'elapsed_s,desired_c,z0_actual_c,z0_duty,z0_guard,z1_actual_c,z1_duty,z1_guard,' +
    'z2_actual_c,z2_duty,z2_guard,zone_mask\n' +
    '30,100.00,101.00,0.500,0,,,0,,,0,1\n'; // only zone 0 active this row (mask 0x01)
  const rows = ctx.parseHistoryCsv(csv);
  assert(rows.length === 1, 'one data row parsed');
  assert(rows[0].zoneMask === 1, 'row.zoneMask reads the trailing column (got ' + rows[0].zoneMask + ')');
})();

// A row from BEFORE this change (no trailing column, older firmware) must
// not throw or silently misparse -- falls back to zoneMask 0 rather than
// crashing on an undefined field.
(function testParseHistoryCsvToleratesMissingZoneMaskColumn() {
  const ctx = loadMaskFns();
  const csv = 'elapsed_s,desired_c,z0_actual_c,z0_duty,z0_guard,z1_actual_c,z1_duty,z1_guard,' +
    'z2_actual_c,z2_duty,z2_guard\n' +
    '30,100.00,101.00,0.500,0,,,0,,,0\n';
  const rows = ctx.parseHistoryCsv(csv);
  assert(rows.length === 1 && rows[0].zoneMask === 0,
    'a pre-change row with no zone_mask column falls back to 0, not NaN/undefined (got ' +
    (rows[0] && rows[0].zoneMask) + ')');
})();

// The regression itself: an idle executor's zone_mask is 0
// (profile_executor_status.c), but a history row recorded DURING a run
// still carries that run's own non-zero mask. Using the row's own mask
// (not a shared "current status" mask) must recover a real average, not
// NaN, for that row.
(function testAvgMaskedDutyUsesRowsOwnMaskNotIdleZero() {
  const ctx = loadMaskFns();
  const rowMask = 0x03; // zones 0,1 were active when THIS row was sampled
  const rowDuty = [0.40, 0.60, NaN]; // zone 2 not in this run -- NaN filler
  const idleStatusMask = 0x00; // executor is idle NOW -- must NOT be used here
  const usingRowMask = ctx.avgMaskedDuty(rowDuty, rowMask, ctx.popcount8(rowMask));
  const usingIdleMask = ctx.avgMaskedDuty(rowDuty, idleStatusMask, ctx.popcount8(idleStatusMask));
  assert(Math.abs(usingRowMask - 0.50) < 1e-9,
    'averaging with the ROW\'s own mask recovers (0.40+0.60)/2 = 0.50, got ' + usingRowMask);
  assert(isNaN(usingIdleMask),
    'averaging with a shared idle-status mask (0) is exactly the old bug -- NaN, proving the row mask matters');
})();

// The divisor fix: a masked-in zone with a missing (NaN) sample this row
// must not drag the average toward zero by being counted in the divisor.
(function testAvgMaskedDutyDividesByReportingZonesNotFullMask() {
  const ctx = loadMaskFns();
  const mask = 0x07; // zones 0,1,2 all in this run's mask
  const duty = [0.90, 0.30, NaN]; // zone 2 is in the mask but this row has no sample
  const avg = ctx.avgMaskedDuty(duty, mask, ctx.popcount8(mask));
  assert(Math.abs(avg - 0.60) < 1e-9,
    'divides by the 2 zones that actually reported ((0.90+0.30)/2 = 0.60), not the full mask of 3 -- got ' + avg);
})();

// ---------------------------------------------------------------------------
// GRAPH_MIN_SPAN_DISP / widenRangeToMinSpan -- owner request 2026-09-28: "the
// graph on the lcd and web should never vertically span less than 5
// degrees." See widenRangeToMinSpan()'s own doc comment in main_page.html.
const MIN_SPAN_SRC = extractRange(
  'var GRAPH_MIN_SPAN_DISP = 5;',
  '}',
) + '\n' + extractRange(
  'function widenRangeToMinSpan(minV, maxV) {',
  '}',
);

assert(MIN_SPAN_SRC.indexOf('function graphMinSpanC') !== -1 &&
  MIN_SPAN_SRC.indexOf('function widenRangeToMinSpan') !== -1,
  'extracted range contains graphMinSpanC and widenRangeToMinSpan (marker drift guard)');

// unit: undefined -> no window at all (Celsius default); 'c'/'f' -> a
// window.kcUnit stub reporting that display unit.
function loadMinSpanFns(unit) {
  const sandbox = { console };
  if (unit) sandbox.window = { kcUnit: { get: () => unit } };
  const ctx = vm.createContext(sandbox);
  vm.runInContext(MIN_SPAN_SRC, ctx);
  return ctx;
}

(function testFahrenheitMinSpanIsFiveDisplayedDegrees() {
  // 5 degrees of the DISPLAYED unit, same as the LCD: in F the Celsius span
  // enforced is 25/9, which is exactly 5 F once toDisplay() scales by 9/5.
  const f = loadMinSpanFns('f');
  const rf = f.widenRangeToMinSpan(20, 20);
  assert(Math.abs((rf[1] - rf[0]) * 9 / 5 - 5) < 1e-9,
    'F display: flat data widens to exactly 5 displayed degrees, got ' + ((rf[1] - rf[0]) * 9 / 5) + ' F');
  const c = loadMinSpanFns('c');
  const rc = c.widenRangeToMinSpan(20, 20);
  assert(rc[1] - rc[0] === 5, 'C display: flat data widens to exactly 5 C, got ' + (rc[1] - rc[0]));
  // 3 C (5.4 F) already meets 5 displayed degrees in F, but not in C.
  const r3f = f.widenRangeToMinSpan(20, 23);
  assert(r3f[0] === 20 && r3f[1] === 23, 'F display: 3 C (5.4 F) range left unchanged');
  const r3c = c.widenRangeToMinSpan(20, 23);
  assert(r3c[1] - r3c[0] === 5, 'C display: 3 C range widened to 5 C');
})();

(function testFlatDataWidensToExactlyMinSpan() {
  const ctx = loadMinSpanFns();
  const r = ctx.widenRangeToMinSpan(20, 20);
  assert(r[1] - r[0] === 5, 'flat data (20==20): widened to exactly GRAPH_MIN_SPAN_C (5), got span ' + (r[1] - r[0]));
  assert(r[0] === 17.5 && r[1] === 22.5, 'flat data (20==20): widened symmetrically around midpoint 20');
})();

(function testOneDegreeRangeWidensToMinSpan() {
  const ctx = loadMinSpanFns();
  const r = ctx.widenRangeToMinSpan(30, 31);
  assert(r[1] - r[0] === 5, '1-degree range (30-31): widened to exactly 5, got ' + (r[1] - r[0]));
  assert(r[0] <= 30 && r[1] >= 31, '1-degree range (30-31): widened range still covers the real data');
  // Symmetric around the data's own midpoint (30.5), not shifted to one side.
  assert(Math.abs((30 - r[0]) - (r[1] - 31)) < 1e-9,
    '1-degree range (30-31): widened symmetrically around the 30.5 midpoint');
})();

(function testRangeAtOrAboveMinSpanLeftUnchanged() {
  const ctx = loadMinSpanFns();
  const r5 = ctx.widenRangeToMinSpan(10, 15); // exactly 5 -- the boundary itself
  assert(r5[0] === 10 && r5[1] === 15, 'range exactly at the 5-degree minimum (10-15): left unchanged');
  const r10 = ctx.widenRangeToMinSpan(40, 50); // well above 5
  assert(r10[0] === 40 && r10[1] === 50, 'range well above the minimum (40-50): left unchanged, no extra widening');
})();

(function testNegativeAndNearZeroTemperatures() {
  const ctx = loadMinSpanFns();
  // Flat sub-zero data: still widens to exactly 5, symmetric around -10.
  const rNeg = ctx.widenRangeToMinSpan(-10, -10);
  assert(rNeg[1] - rNeg[0] === 5, 'flat sub-zero data (-10): widened to exactly 5, got ' + (rNeg[1] - rNeg[0]));
  assert(rNeg[0] === -12.5 && rNeg[1] === -7.5, 'flat sub-zero data (-10): widened symmetrically around -10');
  // Flat at exactly zero (freezing point) -- widening pushes the low edge
  // below zero; this function itself has no freezing-floor opinion (that
  // clamp is a separate, later step in each caller), so it must widen
  // exactly the same way as any other flat value.
  const rZero = ctx.widenRangeToMinSpan(0, 0);
  assert(rZero[1] - rZero[0] === 5, 'flat at-freezing data (0): widened to exactly 5, got ' + (rZero[1] - rZero[0]));
  assert(rZero[0] === -2.5 && rZero[1] === 2.5, 'flat at-freezing data (0): widened symmetrically around 0');
  // A small range straddling zero (-1 to 1.5, span 2.5) -- under 5, must widen.
  const rStraddle = ctx.widenRangeToMinSpan(-1, 1.5);
  assert(rStraddle[1] - rStraddle[0] === 5,
    'small range straddling zero (-1 to 1.5): widened to exactly 5, got ' + (rStraddle[1] - rStraddle[0]));
  assert(rStraddle[0] <= -1 && rStraddle[1] >= 1.5,
    'small range straddling zero (-1 to 1.5): widened range still covers the real data');
})();

// ---------------------------------------------------------------------------
// drawYAxis label uniqueness and placement -- owner fix 2026-09-28:
// GRAPH_MIN_SPAN_DISP can leave a 5-6 displayed-degree span, which the old
// fixed 11-label/.toFixed(0) axis repeated (e.g. 20, 21, 21, 22). Extracts
// niceAxisTickStep() AND drawYAxis() and runs the real drawYAxis() against a
// recording canvas stub and a kcUnit stub, so the tick loop itself (first
// tick, count, F->C mapping, label text) is under test, not a mirror of it.
const NICE_TICK_SRC = extractRange(
  'function niceAxisTickStep(range, targetCount) {',
  '}'
);
assert(NICE_TICK_SRC.indexOf('niceNorm') !== -1,
  'sanity: extracted range is niceAxisTickStep');
const DRAW_Y_AXIS_SRC = extractRange(
  'function drawYAxis(g, yTemp, minV, maxV) {',
  '}'
);
assert(DRAW_Y_AXIS_SRC.indexOf('fillText') !== -1,
  'sanity: extracted range is drawYAxis');

// Returns [{text, y, value}] for every label drawYAxis() draws on a chart
// laid out like makeChartCanvas() (padT 10, plotH 190) over [minC, maxC] C.
function drawYAxisLabels(unit, minC, maxC) {
  const texts = [];
  const g = {
    padL: 42, padT: 10, plotH: 190, border: '#000', muted: '#666',
    ctx: {
      beginPath() {}, moveTo() {}, lineTo() {}, stroke() {},
      fillText(t, x, y) { texts.push({ text: t, y: y - 3 }); },
    },
  };
  const kcUnit = {
    get: () => unit,
    toDisplay: (c) => (unit === 'f' ? c * 9 / 5 + 32 : c),
    label: () => (unit === 'f' ? '\u00b0F' : '\u00b0C'),
  };
  const ctx = vm.createContext({ console, window: { kcUnit } });
  vm.runInContext(NICE_TICK_SRC + '\n' + DRAW_Y_AXIS_SRC, ctx);
  function yTemp(v) { return g.padT + g.plotH - (v - minC) / (maxC - minC) * g.plotH; }
  ctx.drawYAxis(g, yTemp, minC, maxC);
  return texts.map((l) => ({ text: l.text, y: l.y, value: parseFloat(l.text) }));
}

// Spans in displayed degrees (5/5.5/6: the min-span boundary; 50, and the
// full 20-1300 C firing range) in both units. For F the Celsius span is
// chosen so the DISPLAYED span is the listed value.
[
  [20, 25], [20, 25.5], [20, 26], [21.3, 26.3], [-2.2, 2.8], [20, 70], [20, 1300],
].forEach(function (r) {
  ['c', 'f'].forEach(function (unit) {
    const minC = r[0], maxC = unit === 'f' && r[1] - r[0] < 10 ? r[0] + (r[1] - r[0]) * 5 / 9 : r[1];
    const tag = unit.toUpperCase() + ' ' + minC.toFixed(2) + '..' + maxC.toFixed(2) + ' C';
    const labels = drawYAxisLabels(unit, minC, maxC);
    const texts = labels.map((l) => l.text);
    assert(new Set(texts).size === texts.length,
      tag + ': no duplicate Y-axis labels, got [' + texts.join(', ') + ']');
    assert(labels.length >= 4 && labels.length <= 13,
      tag + ': label count 4..13 (got ' + labels.length + ': ' + texts.join(', ') + ')');
    assert(labels.every((l) => l.y >= 10 - 1e-6 && l.y <= 200 + 1e-6),
      tag + ': every tick inside the plot [padT, padT+plotH], got y=[' +
      labels.map((l) => l.y.toFixed(1)).join(', ') + ']');
    // The label text must name the temperature its tick is drawn at.
    const lo = unit === 'f' ? minC * 9 / 5 + 32 : minC, hi = unit === 'f' ? maxC * 9 / 5 + 32 : maxC;
    assert(labels.every((l) => Math.abs((10 + 190 - (l.value - lo) / (hi - lo) * 190) - l.y) < 1e-6),
      tag + ': each label value maps to its own tick y');
    assert(texts.every((t) => t.endsWith(unit === 'f' ? '\u00b0F' : '\u00b0C')),
      tag + ': labels carry the displayed unit');
  });
});

(function testFullFiringRangeDoesNotOvershootTop() {
  // Regression: a Math.round tick count drew 1400 C above a 20..1300 C plot.
  const texts = drawYAxisLabels('c', 20, 1300).map((l) => l.text);
  assert(texts[texts.length - 1] === '1200\u00b0C',
    '20..1300 C: top label is 1200, not past hi, got [' + texts.join(', ') + ']');
})();

// ---------------------------------------------------------------------------
console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exitCode = 1;
}
