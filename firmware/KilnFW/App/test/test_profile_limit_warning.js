/* Node-only test harness for main_page.html's configured-limit headroom
 * warning: classifyMargin / computeProfileLimitWarning / limitBodyHtml, the
 * client-side-only feature added 2026-09-08 per owner policy ("profiles are
 * never clamped -- warn instead, guided by the dashboard warnings").
 *
 * Extracts the real source (same extraction-by-marker-line approach as
 * test_lag_banner.js) rather than reimplementing the logic, and runs it in a
 * Node vm context with small DOM/window stubs.
 *
 * Run: node firmware/KilnFW/App/test/test_profile_limit_warning.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
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

const pending = [];   // async assertions (the icon path resolves a Promise)
let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

const ZONE_NAME_SRC = extractRange('function zoneName(zi) {', '}');
// The block below ends at updateProfileLimitIcon()'s closing brace -- find it
// by walking forward from the LIMIT_HEADROOM_WARN_C line to the matching
// top-level '}' after 'function updateProfileLimitIcon(' starts, same
// content-based approach test_lag_banner.js uses, so this stays correct
// across unrelated reflow inside the block.
function extractLimitSection() {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l).indexOf('var LIMIT_HEADROOM_WARN_C = 5;') === 0);
  if (startIdx === -1) throw new Error('limit warning start marker not found');
  const fnStart = LINES.findIndex((l, i) => i >= startIdx && raw(l).indexOf('function updateProfileLimitIcon(') === 0);
  if (fnStart === -1) throw new Error('updateProfileLimitIcon start not found');
  let endIdx = -1;
  for (let i = fnStart + 1; i < LINES.length; i++) {
    if (raw(LINES[i]) === '}') { endIdx = i; break; }
  }
  if (endIdx === -1) throw new Error('updateProfileLimitIcon end not found');
  return LINES.slice(startIdx, endIdx + 1).join('\n');
}
const LIMIT_SECTION_SRC = extractLimitSection();
assert(LIMIT_SECTION_SRC.indexOf('function computeProfileLimitWarning(') !== -1,
  'sanity: extracted range includes computeProfileLimitWarning');
assert(LIMIT_SECTION_SRC.indexOf('function classifyMargin(') !== -1,
  'sanity: extracted range includes classifyMargin');
assert(LIMIT_SECTION_SRC.indexOf('function limitBodyHtml(') !== -1,
  'sanity: extracted range includes limitBodyHtml');

function makeContext(zones) {
  const elements = {};
  function makeEl() { return { innerHTML: '', hidden: true, textContent: '', title: '',
    classList: { list: [], add: function (c) { if (this.list.indexOf(c) === -1) this.list.push(c); },
                 remove: function () { this.list = []; }, toggle: function (c, on) {
                   if (on) this.add(c); else this.list = this.list.filter(function (x) { return x !== c; });
                 } } }; }
  elements.profileLimitIcon = makeEl();
  elements.profileSelect = { value: '1' };
  const ctx = {
    document: { getElementById: function (id) { return elements[id] || null; } },
    window: { kcEscapeHtml: function (s) { return String(s); } },
    zonesCache: { zones: zones },
    profileFeasCache: {},
    profileTitleById: {},
    console: console,
    Math: Math,
  };
  vm.createContext(ctx);
  vm.runInContext(ZONE_NAME_SRC, ctx);
  vm.runInContext(LIMIT_SECTION_SRC, ctx);
  return { ctx, elements };
}

// Bench-realistic zones: max_temp_c=80 (both ESP and Pico's abs_max_temp_c
// are 80C on this bench, per CLAUDE.md), max_ramp_c_per_hr=900.
// control_mode mirrors zone_control_mode_t: 0 = OFF, 1 = BANGBANG/PID etc.
// Defaults to 1 ("this zone can heat"), which is what makes the max_temp_c==0
// case below a REFUSAL rather than a skip -- see profile_executor_start.c's
// ceiling precheck, which this mirrors exactly.
function zone(name, maxTempC, maxRampCPerHr, controlMode) {
  return { name: name, max_temp_c: maxTempC, max_ramp_c_per_hr: maxRampCPerHr,
           control_mode: controlMode === undefined ? 1 : controlMode };
}
const BENCH_ZONES = [zone('Zone 0', 80, 900), zone('Zone 1', 80, 900), zone('Zone 2', 80, 900)];

function zoneRampSeg(targetC, rampCPerHr) {
  return { seg_kind: 0, target_c: targetC, ramp_c_per_hr: rampCPerHr };
}

// ---------------------------------------------------------------------------
// Group 1: exceeds -- target above the ceiling outright.
// ---------------------------------------------------------------------------
{
  const { ctx } = makeContext(BENCH_ZONES);
  const data = { zone_mask: 0x1, segments: [zoneRampSeg(85, 100)] };
  const summary = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)', Object.assign(ctx, { data }));
  assert(summary !== null, 'exceeds: summary is non-null');
  assert(summary.level === 'exceeds', 'exceeds: level is exceeds (got ' + (summary && summary.level) + ')');
}

// ---------------------------------------------------------------------------
// Group 2: at_limit -- target exactly at the ceiling (the owner's specific
// bench case: 80C target against an 80C max_temp_c). This is the state the
// owner explicitly raised, and the one negative-tested below.
// ---------------------------------------------------------------------------
{
  const { ctx } = makeContext(BENCH_ZONES);
  const data = { zone_mask: 0x1, segments: [zoneRampSeg(80, 100)] };
  const summary = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)', Object.assign(ctx, { data }));
  assert(summary !== null, 'at_limit: summary is non-null');
  assert(summary.level === 'at_limit', 'at_limit: level is at_limit (got ' + (summary && summary.level) + ')');
  const html = vm.runInContext('limitBodyHtml(summary)', Object.assign(ctx, { summary }));
  assert(html.indexOf('dwell entry') !== -1, 'at_limit: body mentions dwell-entry overshoot in these terms');
  assert(html.indexOf('predictable trip') !== -1, 'at_limit: body calls it a predictable trip, not generic red text');
}

// ---------------------------------------------------------------------------
// Group 3: near_limit -- within the 5C headroom band but not at the limit.
// ---------------------------------------------------------------------------
{
  const { ctx } = makeContext(BENCH_ZONES);
  const data = { zone_mask: 0x1, segments: [zoneRampSeg(77, 100)] };
  const summary = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)', Object.assign(ctx, { data }));
  assert(summary !== null, 'near_limit: summary is non-null');
  assert(summary.level === 'near_limit', 'near_limit: level is near_limit (got ' + (summary && summary.level) + ')');
}

// ---------------------------------------------------------------------------
// Group 4: comfortable -- well inside the ceiling -- no warning at all.
// ---------------------------------------------------------------------------
{
  const { ctx } = makeContext(BENCH_ZONES);
  const data = { zone_mask: 0x1, segments: [zoneRampSeg(50, 100)] };
  const summary = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)', Object.assign(ctx, { data }));
  assert(summary === null, 'comfortable: no summary reported (well inside ceiling)');
}

// ---------------------------------------------------------------------------
// Group 5: ramp-rate ceiling, independently of temperature.
// ---------------------------------------------------------------------------
{
  const { ctx } = makeContext(BENCH_ZONES);
  const dataExceeds = { zone_mask: 0x1, segments: [zoneRampSeg(50, 950)] }; // > 900 ceiling
  const s1 = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)',
    Object.assign(ctx, { data: dataExceeds }));
  assert(s1 !== null && s1.level === 'exceeds', 'ramp exceeds: 950C/hr against a 900C/hr ceiling reports exceeds');

  const dataNear = { zone_mask: 0x1, segments: [zoneRampSeg(50, 860)] }; // 90%..100% band
  const s2 = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)',
    Object.assign(ctx, { data: dataNear }));
  assert(s2 !== null && s2.level === 'near_limit', 'ramp near_limit: 860C/hr against a 900C/hr ceiling reports near_limit');
}

// ---------------------------------------------------------------------------
// Group 6: max_temp_c == 0 on an ACTIVE zone.
//
// 0 is NOT the "use the firmware default" sentinel for this field -- that
// convention belongs to the zone BAND fields (progress_band_c and friends).
// zones_config_accessors.h states outright that there is no repo-established
// safe absolute-temperature default to substitute here, and
// profile_executor_start.c's ceiling precheck therefore REFUSES the firing
// for any zone in the mask that is not OFF and has max_temp_c <= 0.
//
// This group previously asserted the opposite (that such a zone is silently
// skipped), which meant a default-configured board got no warning at all for
// a run the firmware was always going to refuse.
// ---------------------------------------------------------------------------
{
  const zones = [zone('Zone 0', 0, 0)];
  const { ctx } = makeContext(zones);
  const data = { zone_mask: 0x1, segments: [zoneRampSeg(50, 100)] };
  const summary = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)', Object.assign(ctx, { data }));
  assert(summary !== null, 'unset ceiling: an active zone with max_temp_c 0 is reported, not skipped');
  assert(summary && summary.level === 'exceeds',
    'unset ceiling: reported at exceeds level -- the firing WILL be refused at start (got ' +
    (summary && summary.level) + ')');
  assert(summary && summary.items.some(function (it) { return it.kind === 'unset_ceiling'; }),
    'unset ceiling: the item names the real reason, not a phantom 0C ceiling comparison');
  const html6 = vm.runInContext('limitBodyHtml(summary)', Object.assign(ctx, { summary: summary }));
  assert(html6.indexOf('no maximum temperature') !== -1,
    'unset ceiling: the popup body says WHY, rather than rendering "0.0C vs limit 0.0C"');
  assert(html6.indexOf('NaN') === -1 && html6.indexOf('undefined') === -1,
    'unset ceiling: the body has no NaN/undefined from the temp/ramp item formatter');
}

// ---------------------------------------------------------------------------
// Group 6b: the same zone switched OFF is skipped -- profile_executor_start.c
// skips OFF zones in exactly the same precheck, so warning about one would be
// a warning about a refusal that never happens.
// ---------------------------------------------------------------------------
{
  const zones = [zone('Zone 0', 0, 0, 0 /* ZONE_CONTROL_MODE_OFF */)];
  const { ctx } = makeContext(zones);
  const data = { zone_mask: 0x1, segments: [zoneRampSeg(50, 100)] };
  const summary = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)', Object.assign(ctx, { data }));
  assert(summary === null, 'unset ceiling on an OFF zone: no warning -- that zone is not driven');
}

// ---------------------------------------------------------------------------
// Group 7: a BUILTIN catalogue profile, driven through the REAL fetch/render
// path rather than a hand-written summary object.
//
// The regression this pins: builtins used to arrive with "zone_mask":0 and no
// seg_kind field at all, so computeProfileLimitWarning() checked no zones AND
// skipped every segment on `undefined !== 0`. On this bench (80C ceilings) the
// shipped BQ1000 schedule peaks above 1000C and showed NO icon, while an
// identical user profile showed the blocked one.
//
// The JSON below is the shape profiles_catalog_http.c's send_builtin_full()
// now emits -- that emitter is separately pinned, against the production C
// function itself, by test_profiles_http.c's
// test_builtin_json_emits_seg_kind_and_resolved_zone_mask(). Together the two
// cover the producer and the consumer of the same contract.
//
// This group drives updateProfileLimitIcon(), the real entry point the page
// calls, so the assertion is on the ICON the operator actually sees.
// ---------------------------------------------------------------------------
{
  const { ctx, elements } = makeContext(BENCH_ZONES);
  const builtinJson = {
    id: 128, builtin: true, read_only: true, name: 'BQ1000', code: 'BQ1000',
    title: 'Bench Bisque 1000', hidden: false,
    zone_mask: 0x7,            // resolved by builtin_effective_zone_mask()
    segment_count: 2,
    feasibility: 'unknown',
    segments: [
      { seg_kind: 0, target_c: 600, ramp_c_per_hr: 100, dwell_min: 0, feasibility: 'unknown' },
      { seg_kind: 0, target_c: 1000, ramp_c_per_hr: 60, dwell_min: 0, feasibility: 'unknown' }
    ]
  };
  const summary = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)',
    Object.assign(ctx, { data: builtinJson }));
  assert(summary !== null, 'builtin: a 1000C schedule against 80C zones is reported at all');
  assert(summary && summary.level === 'exceeds',
    'builtin: reported at exceeds level (got ' + (summary && summary.level) + ')');
  assert(summary && summary.items.length >= 6,
    'builtin: BOTH over-ceiling segments are reported against all three zones in the resolved ' +
    'mask (expected >= 6 items, got ' + (summary && summary.items.length) + ')');

  // Now the real icon path. fetchProfileFeas() lives outside the extracted
  // block, so it is stubbed here -- everything downstream of it
  // (computeProfileLimitWarning, the class/textContent/title decisions) is the
  // page's own production source.
  ctx.fetchProfileFeas = function () { return Promise.resolve(builtinJson); };
  ctx.Promise = Promise;
  elements.profileSelect.value = '128';
  const icon = elements.profileLimitIcon;
  vm.runInContext('updateProfileLimitIcon()', ctx);
  pending.push(Promise.resolve().then(function () {
    assert(icon.hidden === false, 'builtin icon: shown, not hidden (this is the exact bug: no icon at all)');
    assert(icon.textContent === '\u26D4', 'builtin icon: the blocked glyph');
    assert(icon.classList.list.indexOf('limit-icon-danger') !== -1, 'builtin icon: danger styling');
    assert(icon.title.indexOf('refused at start') !== -1, 'builtin icon: tooltip says it will be refused at start');
  }));
}

// The icon-path group above asserts inside a resolved Promise, so the summary
// must wait for it -- otherwise a failure there would be counted after this
// script had already decided its exit code, i.e. reported green.
Promise.all(pending).then(function () {
  console.log('');
  console.log(passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
}).catch(function (e) {
  console.log('FAIL: async assertion group threw: ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
