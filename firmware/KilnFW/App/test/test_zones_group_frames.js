/* Node-only test for zones_page.html's per-group setting FRAMES (owner
 * request, 2026-09-18): each `.groupsrc` select owns one bordered
 * `.groupFrame`, and that frame is visible only while the select says
 * "Custom settings for this zone" (settings_source 255). For any shared or
 * inherited option the fields it governs are not editable on that zone, so
 * the frame is hidden.
 *
 * Why this exists as a test rather than an eyeball: the visibility rule has
 * three ways to go quietly wrong, and none of them look broken on a page
 * that renders. It can invert (hiding the Custom case, showing the
 * inherited one); it can hide zone 0, whose frames must never hide because
 * zone 0 has no selects and is always its own source; and it can hide by
 * writing an inline display style, which the request explicitly rules out
 * because a later render would then have to remember to clear it. Each of
 * those is asserted below.
 *
 * The functions under test are extracted VERBATIM from the page (the same
 * discipline test_zones_inheritance.js and test_zones_type_toggle.js use),
 * so this cannot pass against a copy that has drifted from what ships.
 * zoneDiv() is the one thing stubbed, because the point here is the
 * visibility rule, not the zone-lookup it already shares with those tests.
 *
 * Run: node firmware/KilnFW/App/test/test_zones_group_frames.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'zones_page.html');
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

// ---------------------------------------------------------------------------
// Extract the group list, the stored-source reader and the two frame
// functions, verbatim.
// ---------------------------------------------------------------------------
const GROUPS_LINE = LINES.find((l) => l.indexOf("var SRC_GROUPS = [") === 0);
if (!GROUPS_LINE) throw new Error('SRC_GROUPS declaration not found');
const CODE =
  GROUPS_LINE + '\n' +
  extractRange('function groupSettingsSourceOf(k, group) {', '}') + '\n' +
  extractRange('function applyGroupFrames(k) {', '}') + '\n' +
  extractRange('function applyAllGroupFrames() {', '}');
assert(CODE.indexOf('.groupFrame[data-group=') !== -1,
  'sanity: the extracted code addresses frames by data-group');
assert(/frame\.hidden\s*=/.test(CODE),
  'the frame is hidden via the element .hidden property, not an inline display style');
assert(!/frame\.style\.display/.test(CODE),
  'the frame visibility rule never writes an inline display style');

// ---------------------------------------------------------------------------
// Minimal DOM stub. One zone div per zone, each carrying one `.groupsrc`
// select and one `.groupFrame` per group, addressed exactly the way the
// extracted code addresses them.
// ---------------------------------------------------------------------------
const SRC_GROUP_NAMES = ['limits', 'relaytiming', 'control', 'guards', 'tc'];

function makeZone(sources) {
  // sources: { group: number } -- omit a group to give that zone no select
  // for it at all (which is what zone 0 renders).
  const frames = {};
  const selects = {};
  SRC_GROUP_NAMES.forEach((g) => {
    // Start every frame HIDDEN. If these started visible, an assertion that a
    // frame is shown would also pass when the code never touched it at all --
    // which is exactly how the zone-0 case first shipped vacuous here: a
    // sabotage that skipped zone 0 entirely left its frames at their default
    // and the test stayed green.
    frames[g] = { hidden: true, style: {} };
    if (sources && sources[g] !== undefined) {
      selects[g] = { value: String(sources[g]), dataset: {} };
    }
  });
  return {
    frames: frames,
    querySelector(sel) {
      let m = /^\.groupsrc\[data-group="([a-z]+)"\]$/.exec(sel);
      if (m) return selects[m[1]] || null;
      m = /^\.groupFrame\[data-group="([a-z]+)"\]$/.exec(sel);
      if (m) return frames[m[1]] || null;
      return null;
    },
  };
}

function run(zones, thermoCount) {
  const sandbox = {
    zoneDiv: (k) => zones[k] || null,
    document: { getElementById: () => ({ value: String(thermoCount) }) },
    console: console,
  };
  vm.createContext(sandbox);
  vm.runInContext(CODE + '\napplyAllGroupFrames();', sandbox);
}

// ---------------------------------------------------------------------------
// 1. Custom shows, inherited hides -- per group, independently.
// ---------------------------------------------------------------------------
{
  const zones = [
    makeZone(null),                                   // zone 0: no selects
    makeZone({ limits: 255, relaytiming: 0, control: 255, guards: 0, tc: 0 }),
    makeZone({ limits: 0, relaytiming: 255, control: 1, guards: 255, tc: 255 }),
  ];
  run(zones, 3);

  assert(zones[1].frames.limits.hidden === false,
    'zone 1 limits Custom (255) -> frame shown');
  assert(zones[1].frames.control.hidden === false,
    'zone 1 control Custom (255) -> frame shown');
  assert(zones[1].frames.relaytiming.hidden === true,
    'zone 1 relay timing inherited from zone 0 -> frame hidden');
  assert(zones[1].frames.guards.hidden === true,
    'zone 1 guards inherited -> frame hidden');
  assert(zones[1].frames.tc.hidden === true,
    'zone 1 thermocouple type inherited -> frame hidden');

  assert(zones[2].frames.limits.hidden === true,
    'zone 2 limits inherited -> frame hidden');
  assert(zones[2].frames.control.hidden === true,
    'zone 2 control inherited from zone 1 -> frame hidden');
  assert(zones[2].frames.relaytiming.hidden === false,
    'zone 2 relay timing Custom -> frame shown');
  assert(zones[2].frames.guards.hidden === false,
    'zone 2 guards Custom -> frame shown');
  assert(zones[2].frames.tc.hidden === false,
    'zone 2 thermocouple type Custom -> frame shown');
}

// ---------------------------------------------------------------------------
// 2. Zone 0 is always its own source: every frame stays visible, even though
//    it renders no .groupsrc selects at all.
// ---------------------------------------------------------------------------
{
  const zones = [makeZone(null), makeZone({ limits: 0, relaytiming: 0, control: 0, guards: 0, tc: 0 })];
  run(zones, 2);
  SRC_GROUP_NAMES.forEach((g) => {
    assert(zones[0].frames[g].hidden === false,
      'zone 0 ' + g + ' frame is never hidden');
  });
}

// ---------------------------------------------------------------------------
// 3. A zone whose select is present but out of range for the current zone
//    count still reads as stored-invalid, and the stored value -- not the
//    displayed one -- decides the frame, matching groupSettingsSourceOf().
// ---------------------------------------------------------------------------
{
  const zones = [makeZone(null), makeZone({ limits: 255, relaytiming: 255, control: 255, guards: 255, tc: 255 })];
  zones[1].querySelector('.groupsrc[data-group="limits"]').dataset.storedInvalid = '0';
  run(zones, 2);
  assert(zones[1].frames.limits.hidden === true,
    'a stored-but-invalid inherited source hides the frame, matching what the zone actually saves');
  assert(zones[1].frames.control.hidden === false,
    'the other groups are unaffected by that one zone-group being invalid');
}

// ---------------------------------------------------------------------------
// 4. Re-running after a change restores a previously hidden frame -- the
//    rule is idempotent and reversible in both directions.
// ---------------------------------------------------------------------------
{
  const zones = [makeZone(null), makeZone({ limits: 0, relaytiming: 0, control: 0, guards: 0, tc: 0 })];
  run(zones, 2);
  assert(zones[1].frames.limits.hidden === true, 'inherited first -> hidden');
  zones[1].querySelector('.groupsrc[data-group="limits"]').value = '255';
  run(zones, 2);
  assert(zones[1].frames.limits.hidden === false,
    'switching that select back to Custom shows the frame again');
}

console.log('\n' + passed + ' passed, ' + failed + ' failed.');
if (failed) { failures.forEach((f) => console.log('  FAILED: ' + f)); process.exit(1); }
process.exit(0);
