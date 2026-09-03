/* Node-only test harness for zones_page.html's tuning-method recommendation
 * lookup (pickTuningRecommendation / tuningRecArtifactUsable /
 * tuningRecStatusClass).
 *
 * OWNER REQUEST 2026-09-02 part 2/2: "use all this info to create
 * recomendations in the web gui for what to use for tuneing pid" -- the
 * owner's explicit requirement was that CONFIDENCE MUST BE VISIBLE AND
 * HONEST: a recommendation derived past the simulator's extrapolation
 * boundary must never render like one backed by measurement, and two
 * statistically indistinguishable methods must say so instead of picking a
 * winner. These tests exist to prove that logic, not just that a row comes
 * back.
 *
 * Same extraction convention as test_firing_chart.js: pull the marked
 * TUNING_REC_LOOKUP_START/END range out of zones_page.html by exact line
 * match and vm-execute it, rather than re-implementing the logic here where
 * a real edit to the page could drift away from what this file tests.
 *
 * Run: node firmware/KilnFW/App/test/test_tuning_recommendation.js
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

const LOOKUP_SRC = extractRange('// TUNING_REC_LOOKUP_START', '// TUNING_REC_LOOKUP_END');
assert(LOOKUP_SRC.indexOf('function pickTuningRecommendation') !== -1,
  'sanity: extracted range defines pickTuningRecommendation');
assert(LOOKUP_SRC.indexOf('function tuningRecArtifactUsable') !== -1,
  'sanity: extracted range defines tuningRecArtifactUsable');
assert(LOOKUP_SRC.indexOf('function tuningRecStatusClass') !== -1,
  'sanity: extracted range defines tuningRecStatusClass');

function loadFns(src) {
  const ctx = { exports: {} };
  vm.createContext(ctx);
  new vm.Script(src + '\nthis.pickTuningRecommendation = pickTuningRecommendation;' +
    '\nthis.tuningRecArtifactUsable = tuningRecArtifactUsable;' +
    '\nthis.tuningRecStatusClass = tuningRecStatusClass;',
    { filename: 'zones_page.html (tuning rec lookup slice)' }).runInContext(ctx);
  return ctx;
}

// A representative artifact matching the schema in the task brief.
function sampleArtifact() {
  return {
    schema_version: 1,
    generated_from: 'deadbeef',
    sim_confidence: { held_out_rms_c: [1.05, 0.61, 0.67], discrimination_threshold_c: [2.1, 1.2, 1.3], extrapolation_boundary_c: 80 },
    recommendations: [
      { peak_temp_c_max: 60, load: 'any', method: 'relay', rule: 'ziegler-nichols',
        confidence: 'measured', why: 'Relay converges fastest at low peaks.', runner_up: null, margin_c: null },
      { peak_temp_c_max: 80, load: 'any', method: 'step', rule: 'cohen-coon',
        confidence: 'indistinguishable', why: 'Step and relay are statistically tied here.',
        runner_up: 'relay', margin_c: 0.3 },
      // Deliberately overlaps the full-load row's coverage at peak 150 with
      // a lower-ceiling 'any' row (150 < 200) -- this is the case that
      // actually distinguishes "prefer an exact load match" from "just take
      // the lowest covering ceiling regardless of load": without the
      // exact-match priority, a naive ceiling-only sort would return THIS
      // any-load row for a load='full' query at peak 150, not the full-load
      // row below.
      { peak_temp_c_max: 150, load: 'any', method: 'pid_direct', rule: 'lambda',
        confidence: 'measured', why: 'Generic any-load fallback, should lose to an exact full-load match.',
        runner_up: null, margin_c: null },
      { peak_temp_c_max: 200, load: 'full', method: 'step', rule: 'cohen-coon',
        confidence: 'extrapolated', why: 'Beyond the checked range; assumed physics only.',
        runner_up: null, margin_c: null },
      { peak_temp_c_max: 200, load: 'empty', method: 'relay', rule: 'ziegler-nichols',
        confidence: 'extrapolated', why: 'Beyond the checked range; assumed physics only.',
        runner_up: null, margin_c: null },
    ],
    caveats: ['Tuned empty, fired loaded -- gains may not survive loading.'],
  };
}

// ---------------------------------------------------------------------------
// Group 1: correct row chosen by peak temp + load.
// ---------------------------------------------------------------------------
(function testPicksLowestCoveringCeilingWithinAny() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation(sampleArtifact(), 45, 'any');
  assert(r.ok && r.row.peak_temp_c_max === 60 && r.row.method === 'relay',
    'peak 45C/any picks the 60C-ceiling row (measured), not the 80C one -- got ' + JSON.stringify(r));
})();

(function testExactLoadPreferredOverAnyAtSamePeak() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation(sampleArtifact(), 150, 'full');
  assert(r.ok && r.row.load === 'full' && r.row.confidence === 'extrapolated',
    'peak 150C/full picks the load=full row over any competing any-load row -- got ' + JSON.stringify(r));
  const r2 = ctx.pickTuningRecommendation(sampleArtifact(), 150, 'empty');
  assert(r2.ok && r2.row.load === 'empty' && r2.row.method === 'relay',
    'peak 150C/empty picks the load=empty row, a DIFFERENT method than load=full at the same peak -- got ' + JSON.stringify(r2));
})();

(function testUnknownLoadFallsBackToAnyRows() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation(sampleArtifact(), 70, 'any');
  assert(r.ok && r.row.peak_temp_c_max === 80,
    'peak 70C/any with no exact-load rows falls through to the any-load 80C row -- got ' + JSON.stringify(r));
})();

(function testPeakAbovePeakEverything() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation(sampleArtifact(), 5000, 'full');
  assert(r.ok && r.row.peak_temp_c_max === 200,
    'a peak above every row ceiling still returns the highest-ceiling row rather than "no data" -- got ' + JSON.stringify(r));
})();

// ---------------------------------------------------------------------------
// Group 2: confidence rendering is honest -- the owner's headline requirement.
// ---------------------------------------------------------------------------
(function testIndistinguishableRowCarriesBothMethods() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation(sampleArtifact(), 80, 'any');
  assert(r.ok && r.row.confidence === 'indistinguishable' && r.row.runner_up === 'relay' && r.row.margin_c === 0.3,
    'an indistinguishable row keeps its runner_up and margin_c instead of collapsing to one winner -- got ' + JSON.stringify(r.row));
})();

(function testStatusClassMapping() {
  const ctx = loadFns(LOOKUP_SRC);
  assert(ctx.tuningRecStatusClass('measured') === 'ok', 'measured maps to ok (green)');
  assert(ctx.tuningRecStatusClass('extrapolated') === 'warn', 'extrapolated maps to warn (amber), never ok');
  assert(ctx.tuningRecStatusClass('indistinguishable') === 'warn', 'indistinguishable maps to warn (amber), never ok');
  assert(ctx.tuningRecStatusClass('bogus-future-value') === 'bad',
    'an unrecognised confidence value is treated as the LEAST trusted case, never ok/green');
})();

(function testExtrapolatedNeverMapsToOk() {
  const ctx = loadFns(LOOKUP_SRC);
  // Direct assertion of the owner's exact requirement: an extrapolated
  // recommendation must not render identically to a measured one.
  assert(ctx.tuningRecStatusClass('extrapolated') !== ctx.tuningRecStatusClass('measured'),
    'extrapolated and measured must render with different status classes');
})();

// ---------------------------------------------------------------------------
// Group 3: schema/degradation safety -- an unknown schema or missing
// artifact must degrade to "no data", never guess at a shape it has not
// seen, and never crash the page.
// ---------------------------------------------------------------------------
(function testUnknownSchemaVersionDegradesSafely() {
  const ctx = loadFns(LOOKUP_SRC);
  const artifact = sampleArtifact();
  artifact.schema_version = 2; // a future bump this page predates
  const r = ctx.pickTuningRecommendation(artifact, 50, 'any');
  assert(r.ok === false && r.reason === 'unavailable',
    'schema_version 2 (unknown to this page) is refused, not guessed at -- got ' + JSON.stringify(r));
  assert(ctx.tuningRecArtifactUsable(artifact) === false, 'tuningRecArtifactUsable rejects an unknown schema_version');
})();

(function testMissingArtifactDegradesSafely() {
  const ctx = loadFns(LOOKUP_SRC);
  assert(ctx.pickTuningRecommendation(null, 50, 'any').ok === false,
    'a null artifact (fetch failed / not yet loaded) does not throw and reports not-ok');
  assert(ctx.pickTuningRecommendation(undefined, 50, 'any').reason === 'unavailable',
    'an undefined artifact reports "unavailable", not a crash');
  assert(ctx.tuningRecArtifactUsable(null) === false, 'tuningRecArtifactUsable(null) is false');
})();

(function testMalformedRecommendationsFieldDegradesSafely() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation({ schema_version: 1, recommendations: 'not-an-array' }, 50, 'any');
  assert(r.ok === false && r.reason === 'unavailable',
    'recommendations not being an array is refused rather than throwing -- got ' + JSON.stringify(r));
})();

(function testEmptyRecommendationsListIsNoDataNotCrash() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation({ schema_version: 1, recommendations: [] }, 50, 'any');
  assert(r.ok === false && r.reason === 'no_data',
    'an empty recommendations list (fallback fixture before the campaign artifact lands) reports no_data -- got ' + JSON.stringify(r));
})();

(function testInvalidPeakInputDegradesSafely() {
  const ctx = loadFns(LOOKUP_SRC);
  assert(ctx.pickTuningRecommendation(sampleArtifact(), NaN, 'any').reason === 'invalid_input', 'NaN peak is rejected');
  assert(ctx.pickTuningRecommendation(sampleArtifact(), -5, 'any').reason === 'invalid_input', 'negative peak is rejected');
  assert(ctx.pickTuningRecommendation(sampleArtifact(), '100', 'any').reason === 'invalid_input',
    'a string peak (not a number) is rejected rather than coerced');
})();

// ---------------------------------------------------------------------------
// Group 4: REVIEW 2026-09-02 (Opus round 4). A peak that exceeds EVERY row's
// ceiling is, by definition, outside the range the campaign tested -- it is
// answered with the highest-ceiling row (testPeakAbovePeakEverything above),
// which is the right row to answer with, but it must NOT inherit that row's
// 'measured' confidence. The panel's own header comment states the rule:
// "a recommendation for a peak above the simulator's own checked range rests
// on ASSUMED physics ... It must never look as certain as one that was."
// Before this group, pickTuningRecommendation returned the row verbatim and
// renderTuningRecommendation coloured it var(--ok) green with a MEASURED
// badge and no caveat -- a green, measurement-badged recommendation for a
// peak the campaign never simulated.
// ---------------------------------------------------------------------------
function measuredOnlyArtifact() {
  return {
    schema_version: 1,
    sim_confidence: { extrapolation_boundary_c: 80 },
    recommendations: [
      { peak_temp_c_max: 1000, load: 'any', method: 'ziegler_nichols', rule: 'zn',
        confidence: 'measured', why: 'Checked against held-out runs up to 1000C.' },
    ],
  };
}

(function testPeakWithinTopCeilingStaysMeasured() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation(measuredOnlyArtifact(), 900, 'any');
  assert(r.ok && r.confidence === 'measured',
    'a peak INSIDE the top row ceiling keeps its measured confidence (the downgrade must not fire here) -- got ' + JSON.stringify(r.confidence));
  assert(r.beyond_tested_range === false,
    'a covered peak is not flagged beyond_tested_range -- got ' + JSON.stringify(r.beyond_tested_range));
})();

(function testPeakAboveEveryCeilingIsNotRenderedAsMeasured() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation(measuredOnlyArtifact(), 1300, 'any');
  assert(r.ok && r.row.peak_temp_c_max === 1000,
    'a peak above every ceiling still answers with the highest-ceiling row -- got ' + JSON.stringify(r.row));
  assert(r.beyond_tested_range === true,
    'a peak above every row ceiling is flagged beyond_tested_range -- got ' + JSON.stringify(r.beyond_tested_range));
  assert(r.confidence === 'extrapolated',
    'a peak above every row ceiling is downgraded from measured to extrapolated -- got ' + JSON.stringify(r.confidence));
  assert(ctx.tuningRecStatusClass(r.confidence) !== 'ok',
    'the effective confidence for an out-of-range peak must not render green -- got ' + ctx.tuningRecStatusClass(r.confidence));
})();

(function testDowngradeNeverUpgrades() {
  const ctx = loadFns(LOOKUP_SRC);
  // An already-indistinguishable row stays indistinguishable when the peak
  // runs past its ceiling: the downgrade must never overwrite a MORE
  // cautious verdict with a less cautious one.
  const art = measuredOnlyArtifact();
  art.recommendations[0].confidence = 'indistinguishable';
  art.recommendations[0].runner_up = 'step';
  const r = ctx.pickTuningRecommendation(art, 1300, 'any');
  assert(r.confidence === 'indistinguishable',
    'an indistinguishable row is not rewritten to extrapolated by the out-of-range downgrade -- got ' + JSON.stringify(r.confidence));
  assert(r.beyond_tested_range === true, 'it is still flagged beyond_tested_range');
})();

(function testMissingConfidenceFieldDoesNotCrash() {
  const ctx = loadFns(LOOKUP_SRC);
  const art = measuredOnlyArtifact();
  delete art.recommendations[0].confidence;
  const r = ctx.pickTuningRecommendation(art, 900, 'any');
  assert(r.ok && typeof r.confidence === 'string',
    'a row with no confidence field yields a STRING effective confidence (renderTuningRecommendation calls .toUpperCase() on it) -- got ' + JSON.stringify(r.confidence));
  assert(ctx.tuningRecStatusClass(r.confidence) === 'bad',
    'a row with no confidence field is treated as the least-trusted case -- got ' + ctx.tuningRecStatusClass(r.confidence));
})();

// ---------------------------------------------------------------------------
// Group 5: REVIEW 2026-09-02 (Opus round 4, finding 4). wantLoad === 'any'
// ("not sure" operator) must never silently hand back a load-specific row
// with no indication -- either a genuine any-load row covers the peak, or
// the caller is told which load the returned row assumes.
// ---------------------------------------------------------------------------
function loadSpecificOnlyArtifact() {
  return {
    schema_version: 1,
    sim_confidence: { extrapolation_boundary_c: 80 },
    recommendations: [
      { peak_temp_c_max: 200, load: 'full', method: 'step', rule: 'cohen-coon',
        confidence: 'measured', why: 'Full-load only coverage at this peak.' },
      { peak_temp_c_max: 500, load: 'empty', method: 'relay', rule: 'ziegler-nichols',
        confidence: 'measured', why: 'Empty-load only coverage at this peak.' },
    ],
  };
}

function anyRowHasHigherCeilingArtifact() {
  return {
    schema_version: 1,
    sim_confidence: { extrapolation_boundary_c: 80 },
    recommendations: [
      // A ceiling-only sort (no load priority) would pick this LOWER-
      // ceiling full-load row over the any-load row below, even though
      // the any-load row also covers peak 90 -- this is exactly the case
      // that distinguishes "prefer a genuine any row" from "lowest
      // covering ceiling regardless of load".
      { peak_temp_c_max: 100, load: 'full', method: 'step', rule: 'cohen-coon',
        confidence: 'measured', why: 'Full-load, lower ceiling.' },
      { peak_temp_c_max: 300, load: 'any', method: 'relay', rule: 'ziegler-nichols',
        confidence: 'measured', why: 'Any-load, higher ceiling, still covers 90.' },
    ],
  };
}

(function testAnyPrefersGenuineAnyRowOverLoadSpecific() {
  const ctx = loadFns(LOOKUP_SRC);
  const r = ctx.pickTuningRecommendation(anyRowHasHigherCeilingArtifact(), 90, 'any');
  assert(r.ok && r.row.load === 'any',
    'an "any" request prefers a genuine any-load row when one covers the peak, even at a higher ' +
    'ceiling than a covering load-specific row -- got ' + JSON.stringify(r.row));
  assert(r.assumed_load === null || r.assumed_load === undefined,
    'a genuine any-load match carries no assumed_load flag -- got ' + JSON.stringify(r.assumed_load));
})();

(function testAnyFallsBackToLoadSpecificAndFlagsAssumedLoad() {
  const ctx = loadFns(LOOKUP_SRC);
  // No any-load row exists in this artifact at all, so any request for
  // 'any' MUST fall through to a load-specific row and say which load it
  // assumed -- this is the vacuous case the finding was about: silently
  // returning row.load === 'full' under an unqualified banner.
  const r = ctx.pickTuningRecommendation(loadSpecificOnlyArtifact(), 150, 'any');
  assert(r.ok && r.row.load === 'full',
    'falls back to the lowest-covering load-specific row -- got ' + JSON.stringify(r.row));
  assert(r.assumed_load === 'full',
    'the caller is told this recommendation assumes full load -- got ' + JSON.stringify(r.assumed_load));
})();

(function testExactLoadRequestNeverFlagsAssumedLoad() {
  const ctx = loadFns(LOOKUP_SRC);
  // assumed_load only applies to the wantLoad === 'any' case -- an
  // operator who explicitly picked 'full' asked for exactly this row, so
  // there is nothing to caveat.
  const r = ctx.pickTuningRecommendation(sampleArtifact(), 150, 'full');
  assert(r.ok && r.row.load === 'full' && !r.assumed_load,
    'an explicit load request never sets assumed_load -- got ' + JSON.stringify(r));
})();

// ---------------------------------------------------------------------------
console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exitCode = 1;
}
