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
function zone(name, maxTempC, maxRampCPerHr) {
  return { name: name, max_temp_c: maxTempC, max_ramp_c_per_hr: maxRampCPerHr };
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
// Group 6: uncommissioned zone (max_temp_c 0) is skipped, not reported as a
// false "exceeds" against a phantom 0C ceiling.
// ---------------------------------------------------------------------------
{
  const zones = [zone('Zone 0', 0, 0)];
  const { ctx } = makeContext(zones);
  const data = { zone_mask: 0x1, segments: [zoneRampSeg(50, 100)] };
  const summary = vm.runInContext('computeProfileLimitWarning(zonesCache.zones, data)', Object.assign(ctx, { data }));
  assert(summary === null, 'uncommissioned zone: no warning against an unset max_temp_c');
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
