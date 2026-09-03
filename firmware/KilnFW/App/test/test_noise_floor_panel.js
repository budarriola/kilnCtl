/* Node-only test harness for zones_page.html's measured noise-floor panel
 * (noiseFloorUsable / noiseFloorMetricRow / renderNoiseFloorHtml).
 *
 * REAL MEASUREMENT 2026-09-03: a six-firing repeat campaign on the bench
 * kiln gave a genuine measured noise floor per metric (see
 * tools/PcTools/config_presets/noise_floor.json). The panel under test folds
 * that into tuning_recommendations.json's optional `noise_floor` field and
 * must (a) never claim a difference is real when the metric's own floor is
 * too large/unstable to support that, (b) always show provenance/scope
 * caveats (bench rig, not like-for-like, range-grows-with-n, does not
 * transfer to the user's own kiln), and (c) degrade to "no data" rather than
 * guessing when the field is absent (older artifact / fallback placeholder)
 * or a requested metric simply has no entry.
 *
 * Same extraction convention as test_tuning_recommendation.js: pull the
 * marked NOISE_FLOOR_START/END range out of zones_page.html by exact line
 * match and vm-execute it, so a real edit to the page can't silently drift
 * away from what this file tests.
 *
 * Run: node firmware/KilnFW/App/test/test_noise_floor_panel.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const PAGE_PATH = path.join(__dirname, '..', 'drivers', 'zones_page.html');
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

const NOISE_FLOOR_SRC = extractRange('// NOISE_FLOOR_START', '// NOISE_FLOOR_END');
assert(NOISE_FLOOR_SRC.indexOf('function noiseFloorUsable') !== -1,
  'sanity: extracted range defines noiseFloorUsable');
assert(NOISE_FLOOR_SRC.indexOf('function noiseFloorMetricRow') !== -1,
  'sanity: extracted range defines noiseFloorMetricRow');
assert(NOISE_FLOOR_SRC.indexOf('function renderNoiseFloorHtml') !== -1,
  'sanity: extracted range defines renderNoiseFloorHtml');

function loadFns(src) {
  const ctx = {
    window: { kcEscapeHtml: function (s) { return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); } },
  };
  vm.createContext(ctx);
  new vm.Script(src + '\nthis.noiseFloorUsable = noiseFloorUsable;' +
    '\nthis.noiseFloorMetricRow = noiseFloorMetricRow;' +
    '\nthis.renderNoiseFloorHtml = renderNoiseFloorHtml;',
    { filename: 'zones_page.html (noise floor panel slice)' }).runInContext(ctx);
  return ctx;
}

// Representative payload: floor PRESENT, matching the real artifact shape
// (tools/PcTools/config_presets/tuning_recommendations.json's noise_floor
// field, as produced from the real six-firing campaign).
function floorPresentArtifact() {
  return {
    schema_version: 1,
    recommendations: [],
    noise_floor: {
      rig: 'bench fixture (~60C peak firing)',
      n_repeats: 6,
      like_for_like: false,
      like_for_like_threshold_c: 1.0,
      start_temp_c_range: 1.29,
      warning: 'start temperatures span 1.29C, exceeding the 1.00C like-for-like threshold',
      stat_caveat: 'Each floor is a max-minus-min RANGE across 6 runs; it grows with sample size.',
      transfer_caveat: 'Measured on the reference bench rig only -- does not transfer to your own kiln.',
      metrics: {
        iae_normalized_whole_c: { reliable: true, min_c: 0.112, max_c: 0.167, unit: 'C', note: 'stable across zones' },
        ramp_worst_error_c: { reliable: false, min_c: 0.230, max_c: 27.110, unit: 'C', note: '117.9x spread' },
      },
    },
  };
}

// Representative payload: floor ABSENT (older schema / pre-measurement
// artifact, e.g. the current tuning_recommendations_fallback.json shape).
function floorAbsentArtifact() {
  return { schema_version: 1, recommendations: [] };
}

// ---------------------------------------------------------------------------
// Group 1: usability / degradation.
// ---------------------------------------------------------------------------
(function testUsableWithRealShapedArtifact() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  assert(ctx.noiseFloorUsable(floorPresentArtifact()) === true,
    'a real-shaped artifact with noise_floor.metrics is usable');
})();

(function testUnusableWhenFieldAbsent() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  assert(ctx.noiseFloorUsable(floorAbsentArtifact()) === false,
    'an artifact with no noise_floor field at all (older schema) is not usable');
})();

(function testUnusableWhenNullOrUndefinedArtifact() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  assert(ctx.noiseFloorUsable(null) === false, 'null artifact is not usable');
  assert(ctx.noiseFloorUsable(undefined) === false, 'undefined artifact is not usable');
})();

(function testUnusableWhenMetricsMalformed() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const art = floorPresentArtifact();
  art.noise_floor.metrics = 'not-an-object';
  assert(ctx.noiseFloorUsable(art) === false, 'a non-object metrics field is refused, not guessed at');
})();

// ---------------------------------------------------------------------------
// Group 2: per-metric lookup, including "a metric with no floor entry".
// ---------------------------------------------------------------------------
(function testMetricRowReturnedForKnownMetric() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const row = ctx.noiseFloorMetricRow(floorPresentArtifact(), 'iae_normalized_whole_c');
  assert(row && row.reliable === true && row.min_c === 0.112 && row.max_c === 0.167,
    'a known metric returns its real row -- got ' + JSON.stringify(row));
})();

(function testMetricRowNullForUnmeasuredMetric() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const row = ctx.noiseFloorMetricRow(floorPresentArtifact(), 'some_metric_the_campaign_never_measured');
  assert(row === null,
    'a metric with no floor entry returns null, not a fabricated row -- got ' + JSON.stringify(row));
})();

(function testMetricRowNullWhenArtifactUnusable() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  assert(ctx.noiseFloorMetricRow(floorAbsentArtifact(), 'iae_normalized_whole_c') === null,
    'an unusable artifact yields null for any metric lookup, not a throw');
})();

// ---------------------------------------------------------------------------
// Group 3: rendered HTML -- the actual strings shown to the user. Prove the
// honesty requirements land in the emitted markup, not just in the data.
// ---------------------------------------------------------------------------
(function testRenderNoDataWhenAbsent() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const html = ctx.renderNoiseFloorHtml(floorAbsentArtifact());
  assert(html.indexOf('No measured noise-floor data') !== -1,
    'rendering an artifact with no noise_floor field says so plainly -- got: ' + html);
})();

(function testRenderNoDataWhenNull() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const html = ctx.renderNoiseFloorHtml(null);
  assert(html.indexOf('No measured noise-floor data') !== -1,
    'rendering a null artifact (fetch failed) degrades safely -- got: ' + html);
})();

(function testRenderShowsProvenanceAndScope() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const html = ctx.renderNoiseFloorHtml(floorPresentArtifact());
  assert(html.indexOf('bench fixture') !== -1, 'rendered panel names the bench rig -- got: ' + html);
  assert(html.indexOf('does not transfer to your own kiln') !== -1,
    'rendered panel states the no-transfer caveat -- got: ' + html);
  assert(html.indexOf('exceeding the 1.00C like-for-like threshold') !== -1,
    'rendered panel surfaces the like-for-like failure warning -- got: ' + html);
  assert(html.indexOf('grows with sample size') !== -1,
    'rendered panel states the range-grows-with-n caveat -- got: ' + html);
})();

(function testReliableMetricSaysDifferenceLikelyReal() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const html = ctx.renderNoiseFloorHtml(floorPresentArtifact());
  const idx = html.indexOf('iae_normalized_whole_c');
  assert(idx !== -1, 'reliable metric name appears in the rendered panel');
  const around = html.slice(idx, idx + 200);
  assert(around.indexOf('is likely real') !== -1,
    'a reliable metric is told apart as trustworthy in its own rendered line -- got: ' + around);
  assert(html.indexOf('[RESOLVES]') !== -1, 'reliable metric carries the RESOLVES badge -- got: ' + html);
})();

(function testUnreliableMetricSaysDoNotTrustSmallDifference() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const html = ctx.renderNoiseFloorHtml(floorPresentArtifact());
  const idx = html.indexOf('ramp_worst_error_c');
  assert(idx !== -1, 'unreliable metric name appears in the rendered panel');
  const around = html.slice(idx, idx + 250);
  assert(around.indexOf('do not read a small difference') !== -1,
    'an unreliable metric explicitly warns against reading a small difference as real -- got: ' + around);
  assert(html.indexOf('[UNRELIABLE]') !== -1, 'unreliable metric carries the UNRELIABLE badge -- got: ' + html);
})();

(function testReliableAndUnreliableNeverShareABadge() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const html = ctx.renderNoiseFloorHtml(floorPresentArtifact());
  assert(html.indexOf('var(--ok)') !== -1 && html.indexOf('var(--bad)') !== -1,
    'reliable and unreliable metrics render with different status colors (--ok vs --bad) -- got: ' + html);
})();

(function testMalformedMetricRowSkippedNotThrown() {
  const ctx = loadFns(NOISE_FLOOR_SRC);
  const art = floorPresentArtifact();
  art.noise_floor.metrics.broken_metric = { reliable: true }; // missing min_c/max_c
  const html = ctx.renderNoiseFloorHtml(art);
  assert(html.indexOf('broken_metric') === -1,
    'a metric row missing min_c/max_c is skipped rather than rendering a broken line -- got: ' + html);
})();

// ---------------------------------------------------------------------------
console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exitCode = 1;
}
