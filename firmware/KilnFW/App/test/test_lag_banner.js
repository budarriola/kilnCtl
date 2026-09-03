/* Node-only test harness for main_page.html's ramp-lag banner logic:
 * deriveLagZones / updateLagBannerVisibility / formatRichLagDetail /
 * renderLagBanner, wired (commit 1e03448 follow-up) to the real per-zone
 * ramp_lag_sustained/ramp_lag_held_s/ramp_lag_commanded_rate_c_per_hr/
 * ramp_lag_achieved_rate_c_per_hr fields and the run-level
 * ramp_stretch_segment_s/ramp_stretch_total_s fields, with a fallback to the
 * older ramp_lock_held/ramp_lock_lagging_mask shape for a board that
 * predates that commit.
 *
 * Extracts the real source (same extraction-by-marker-line approach as
 * test_firing_chart.js) rather than reimplementing the logic, and runs it in
 * a Node vm context with small DOM/window stubs.
 *
 * Run: node firmware/KilnFW/App/test/test_lag_banner.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const PAGE_PATH = path.join(__dirname, '..', 'drivers', 'main_page.html');
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

const FMT_DURATION_SRC = extractRange(
  'function fmtDuration(s) {',
  '}'
);

// LAG_DEBOUNCE_TICKS..renderLagBanner: find the real end of renderLagBanner
// (its own top-level closing brace) by content rather than guessing an exact
// line, so this stays correct across unrelated reflow inside the block.
function extractLagBannerSection() {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l) === 'var LAG_DEBOUNCE_TICKS = 3;');
  if (startIdx === -1) throw new Error('lag banner start marker not found');
  const fnStart = LINES.findIndex((l, i) => i >= startIdx && raw(l).indexOf('function renderLagBanner(') === 0);
  if (fnStart === -1) throw new Error('renderLagBanner start not found');
  // renderLagBanner ends at the first top-level '}' (column 0) after fnStart.
  let endIdx = -1;
  for (let i = fnStart + 1; i < LINES.length; i++) {
    if (raw(LINES[i]) === '}') { endIdx = i; break; }
  }
  if (endIdx === -1) throw new Error('renderLagBanner end not found');
  return LINES.slice(startIdx, endIdx + 1).join('\n');
}
const LAG_SECTION_SRC = extractLagBannerSection();
assert(LAG_SECTION_SRC.indexOf('function renderLagBanner(') !== -1,
  'sanity: extracted range includes renderLagBanner');
assert(LAG_SECTION_SRC.indexOf('function deriveLagZones(') !== -1,
  'sanity: extracted range includes deriveLagZones');
assert(LAG_SECTION_SRC.indexOf('function updateLagBannerVisibility(') !== -1,
  'sanity: extracted range includes updateLagBannerVisibility');

function makeContext() {
  const elements = {};
  function makeEl() { return { innerHTML: '' }; }
  const lagBannerEl = makeEl();
  elements.lagBanner = lagBannerEl;
  const ctx = {
    document: {
      getElementById: function (id) { return elements[id] || null; },
    },
    window: {
      kcEscapeHtml: function (s) { return String(s); },
      kcUnit: { fmt: function (c) { return c.toFixed(1) + 'C'; } },
    },
    zonesCache: { zones: [] },
    console: console,
    Math: Math,
  };
  vm.createContext(ctx);
  vm.runInContext(FMT_DURATION_SRC, ctx);
  vm.runInContext('function zoneName(zi) { var raw = (zonesCache.zones[zi] && zonesCache.zones[zi].name) || ("Zone " + zi); return window.kcEscapeHtml(raw); }', ctx);
  vm.runInContext(LAG_SECTION_SRC, ctx);
  return { ctx, lagBannerEl };
}

// ---------------------------------------------------------------------------
// Representative payloads
// ---------------------------------------------------------------------------
function richZone(zi, sustained, opts) {
  opts = opts || {};
  return {
    zone: zi,
    actual_c: opts.actualC != null ? opts.actualC : 500,
    actual_valid: true,
    ramp_lag_sustained: sustained,
    ramp_lag_held_s: sustained ? (opts.heldS != null ? opts.heldS : 145) : 0,
    ramp_lag_commanded_rate_c_per_hr: sustained ? (opts.commanded != null ? opts.commanded : 60) : 0,
    ramp_lag_achieved_rate_c_per_hr: sustained ? (opts.achieved != null ? opts.achieved : 22) : 0,
  };
}

const NO_LAG = {
  state: 'running', target_c: 500, ramp_lock_held: false, ramp_lock_lagging_mask: 0,
  ramp_stretch_segment_s: 0, ramp_stretch_total_s: 0,
  zones: [richZone(0, false), richZone(1, false), richZone(2, false)],
};

const ONE_ZONE_LAG = {
  state: 'running', target_c: 500, ramp_lock_held: true, ramp_lock_lagging_mask: 0x2,
  ramp_stretch_segment_s: 42, ramp_stretch_total_s: 145,
  zones: [richZone(0, false), richZone(1, true), richZone(2, false)],
};

const ALL_THREE_LAG = {
  state: 'running', target_c: 500, ramp_lock_held: true, ramp_lock_lagging_mask: 0x7,
  ramp_stretch_segment_s: 90, ramp_stretch_total_s: 300,
  zones: [
    richZone(0, true, { heldS: 60, commanded: 60, achieved: 30 }),
    richZone(1, true, { heldS: 90, commanded: 60, achieved: 22 }),
    richZone(2, true, { heldS: 200, commanded: 55, achieved: 40 }),
  ],
};

// Old-firmware shape (predates commit 1e03448): none of the new fields
// exist anywhere in the payload -- this is what the bench board currently
// serves until reflashed.
const OLD_FIRMWARE_SHAPE = {
  state: 'running', target_c: 500, ramp_lock_held: true, ramp_lock_lagging_mask: 0x2,
  zones: [
    { zone: 0, actual_c: 498, actual_valid: true },
    { zone: 1, actual_c: 470, actual_valid: true },
    { zone: 2, actual_c: 501, actual_valid: true },
  ],
};

const ASSIST_DISABLED = {
  state: 'running', target_c: 500, ramp_lock_held: true, ramp_lock_lagging_mask: 0x1,
  ramp_stretch_segment_s: 0, ramp_stretch_total_s: 0,
  zones: [richZone(0, true, { heldS: 55, commanded: 60, achieved: 40 }), richZone(1, false), richZone(2, false)],
};

const STATUS_ASSIST_ON = { ramp_assist_enabled: true };
const STATUS_ASSIST_OFF = { ramp_assist_enabled: false };

// ---------------------------------------------------------------------------
// Group 1: rich path shows immediately (no 3-tick debounce) and reports the
// real message with rates/duration.
// ---------------------------------------------------------------------------
{
  const { ctx, lagBannerEl } = makeContext();
  vm.runInContext('renderLagBanner(ONE_ZONE_LAG, STATUS_ASSIST_ON)', Object.assign(ctx, { ONE_ZONE_LAG, STATUS_ASSIST_ON }));
  const html = lagBannerEl.innerHTML;
  assert(html.indexOf('falling behind schedule') !== -1, 'one-zone rich lag: banner shows on FIRST tick (no extra debounce)');
  assert(html.indexOf('60') !== -1 && html.indexOf('22') !== -1, 'one-zone rich lag: shows commanded (60) and achieved (22) rates');
  assert(html.indexOf('2m 25s') !== -1, 'one-zone rich lag: shows held duration (145s -> 2m 25s)');
  assert(html.indexOf('stretched') !== -1, 'one-zone rich lag: mentions stretch time when ramp_assist_enabled is true');
  console.log('  emitted: ' + html.replace(/\n/g, ''));
}

// ---------------------------------------------------------------------------
// Group 2: all three zones lagging.
// ---------------------------------------------------------------------------
{
  const { ctx, lagBannerEl } = makeContext();
  vm.runInContext('renderLagBanner(ALL_THREE_LAG, STATUS_ASSIST_ON)', Object.assign(ctx, { ALL_THREE_LAG, STATUS_ASSIST_ON }));
  const html = lagBannerEl.innerHTML;
  const zoneMentions = (html.match(/Zone \d/g) || []).length;
  assert(zoneMentions === 3, 'all-three-lag: mentions all 3 zones (got ' + zoneMentions + ')');
  assert(html.indexOf('these zones') !== -1, 'all-three-lag: plural wording used for >1 lagging zone');
  console.log('  emitted: ' + html.replace(/\n/g, ''));
}

// ---------------------------------------------------------------------------
// Group 3: no lag -> banner empty.
// ---------------------------------------------------------------------------
{
  const { ctx, lagBannerEl } = makeContext();
  vm.runInContext('renderLagBanner(NO_LAG, STATUS_ASSIST_ON)', Object.assign(ctx, { NO_LAG, STATUS_ASSIST_ON }));
  assert(lagBannerEl.innerHTML === '', 'no lag: banner stays empty');
}

// ---------------------------------------------------------------------------
// Group 4: old-firmware shape (fallback path) -- 6s (3-tick) debounce still
// applies, since this instantaneous field has no firmware-side debounce.
// ---------------------------------------------------------------------------
{
  const { ctx, lagBannerEl } = makeContext();
  vm.runInContext('a = renderLagBanner(OLD_FIRMWARE_SHAPE, null)', Object.assign(ctx, { OLD_FIRMWARE_SHAPE }));
  assert(lagBannerEl.innerHTML === '', 'fallback shape: tick 1 does not show yet (debounce)');
  vm.runInContext('renderLagBanner(OLD_FIRMWARE_SHAPE, null)', ctx);
  assert(lagBannerEl.innerHTML === '', 'fallback shape: tick 2 does not show yet (debounce)');
  vm.runInContext('renderLagBanner(OLD_FIRMWARE_SHAPE, null)', ctx);
  const html = lagBannerEl.innerHTML;
  assert(html.indexOf('falling behind schedule') !== -1, 'fallback shape: tick 3 shows (debounce satisfied)');
  assert(html.indexOf('vs target') !== -1, 'fallback shape: uses actual-vs-target wording, not rate wording');
  assert(html.indexOf('has been stretched') === -1, 'fallback shape: no stretch-time claim (ramp_stretch_* not in this payload)');
  console.log('  emitted: ' + html.replace(/\n/g, ''));

  // A single blip (one lagging tick, then clear) must NOT show the banner.
  const { ctx: ctx2, lagBannerEl: el2 } = makeContext();
  vm.runInContext('renderLagBanner(OLD_FIRMWARE_SHAPE, null)', Object.assign(ctx2, { OLD_FIRMWARE_SHAPE, NO_LAG }));
  vm.runInContext('renderLagBanner(NO_LAG, null)', ctx2);
  assert(el2.innerHTML === '', 'fallback shape: single blip tick does not show (debounce filters it)');
}

// ---------------------------------------------------------------------------
// Group 5: ramp_assist disabled -- stretch values are 0 but that must read
// as "not measured", never as "0s stretched" (a false claim of measurement).
// ---------------------------------------------------------------------------
{
  const { ctx, lagBannerEl } = makeContext();
  vm.runInContext('renderLagBanner(ASSIST_DISABLED, STATUS_ASSIST_OFF)', Object.assign(ctx, { ASSIST_DISABLED, STATUS_ASSIST_OFF }));
  const html = lagBannerEl.innerHTML;
  assert(html.indexOf('falling behind schedule') !== -1, 'assist disabled: banner still shows (lag detection is independent of assist)');
  assert(html.indexOf('has been stretched') === -1, 'assist disabled: does NOT claim a stretch duration (0 is unmeasured, not "0s")');
  assert(html.indexOf('0s') === -1, 'assist disabled: does not print a bare "0s" as if it were a measurement');
  console.log('  emitted: ' + html.replace(/\n/g, ''));
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
