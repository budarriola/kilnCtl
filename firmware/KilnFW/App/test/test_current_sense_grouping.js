/* Node-only test harness for main_page.html's dashboard current-sense
 * GROUPING (owner request 2026-09-08): "if each zone is assigned its own
 * current sense, put it in the zone that it is assigned to. If there are
 * current sense items assigned to more than one zone, keep those in a
 * separate section. Do not show unassigned current sense items."
 *
 * Covers three functions, extracted from the real page source the same
 * by-marker-line way test_current_display.js already does:
 *   - ctChannelZones(zones, ci)        -- pure zero/one/many-zone counting
 *   - singleZoneCtTag(data, zones, zi) -- the badge on a zone's own row
 *   - renderCurrentCard(data, zones)   -- the shared-only section
 *
 * Run: node firmware/KilnFW/App/test/test_current_sense_grouping.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// Same convention as test_current_display.js's extractFunction(): grabs a
// top-level function's source by its exact `function NAME(` line, ending at
// the first column-0 '}' after that line.
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

// singleZoneCtTag() and renderCurrentCard() both call ctChannelZones(), so
// both are run in a context that also has the real ctChannelZones() source
// loaded, not a reimplementation -- a bug in the real counting function
// must show up in these tests too, not be masked by a hand-rolled stand-in.
const CT_CHANNEL_ZONES_SRC = extractFunction(SRC, 'ctChannelZones');
const SINGLE_ZONE_CT_TAG_SRC = extractFunction(SRC, 'singleZoneCtTag');
const RENDER_CURRENT_CARD_SRC = extractFunction(SRC, 'renderCurrentCard');

function makeContext(extraSrc) {
  const ctx = {
    document: {
      createElement: function () { return { className: '', innerHTML: '' }; },
    },
    console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(CT_CHANNEL_ZONES_SRC, ctx);
  if (extraSrc) vm.runInContext(extraSrc, ctx);
  return ctx;
}

function zone(ctMask) { return { ct_mask: ctMask }; }

// ---------------------------------------------------------------------------
// Group 1: ctChannelZones() -- the pure counting primitive.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext();
  // zone 0 and zone 2 both claim CT channel 0 (bit 0) -- the shared case.
  const zones = [zone(0x01), zone(0x00), zone(0x05)];
  const shared = vm.runInContext('ctChannelZones(zones, 0)', Object.assign(ctx, { zones }));
  assert(JSON.stringify(shared) === JSON.stringify([0, 2]), 'shared CT (bit 0) claimed by zones 0 and 2 -- got ' + JSON.stringify(shared));

  const single = vm.runInContext('ctChannelZones(zones, 2)', Object.assign(ctx, { zones }));
  assert(JSON.stringify(single) === JSON.stringify([2]), 'CT bit 2 claimed by zone 2 only -- got ' + JSON.stringify(single));

  const none = vm.runInContext('ctChannelZones(zones, 1)', Object.assign(ctx, { zones }));
  assert(JSON.stringify(none) === JSON.stringify([]), 'CT bit 1 claimed by no zone -- got ' + JSON.stringify(none));
}

// ---------------------------------------------------------------------------
// Group 2: singleZoneCtTag() -- one-zone assignment renders on that zone's
// own row; multi-zone and unassigned channels render nothing here.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext(SINGLE_ZONE_CT_TAG_SRC);
  // CT0 -> zone 0 only (single); CT1 -> zones 0 and 1 (shared); CT2 -> no zone.
  const zones = [zone(0x01 | 0x02), zone(0x02)];
  const data = { ct_current_a: [3.21, 9.99, 1.0], ct_fitted: [true, true, true] };

  const tagZone0 = vm.runInContext('singleZoneCtTag(data, zones, 0)', Object.assign(ctx, { data, zones }));
  assert(tagZone0.indexOf('CT0') !== -1, 'zone 0 gets its own singly-assigned CT0 badge -- got: ' + tagZone0);
  assert(tagZone0.indexOf('3.21 A') !== -1, 'zone 0 badge shows the real reading -- got: ' + tagZone0);
  assert(tagZone0.indexOf('CT1') === -1, 'zone 0 does NOT get CT1 (shared with zone 1) on its own row -- got: ' + tagZone0);
  assert(tagZone0.indexOf('CT2') === -1, 'zone 0 does NOT get CT2 (unassigned) -- got: ' + tagZone0);

  const tagZone1 = vm.runInContext('singleZoneCtTag(data, zones, 1)', Object.assign(ctx, { data, zones }));
  assert(tagZone1 === '', 'zone 1 has no SINGLY-assigned CT (CT1 is shared, not its own) -- got: ' + tagZone1);

  const tagNoZone = vm.runInContext('singleZoneCtTag(data, zones, null)', Object.assign(ctx, { data, zones }));
  assert(tagNoZone === '', 'a channel row with no zone at all gets no CT tag -- got: ' + tagNoZone);
}

// ---------------------------------------------------------------------------
// Group 3: renderCurrentCard(data, zones) -- shared-only section.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext(RENDER_CURRENT_CARD_SRC);
  // CT0 unassigned, CT1 single-zone (zone 3), CT2 shared (zones 0 and 1).
  const zones = [zone(0x04), zone(0x04), zone(0x00), zone(0x02)];
  const data = { ct_current_a: [1.1, 2.2, 3.3], ct_fitted: [true, true, true] };
  const div = vm.runInContext('renderCurrentCard(data, zones)', Object.assign(ctx, { data, zones }));
  assert(div !== null, 'shared card still renders when a shared CT exists');
  assert(div.innerHTML.indexOf('CT2') !== -1, 'shared CT2 (zones 0 and 1) appears in the shared section -- got: ' + div.innerHTML);
  assert(div.innerHTML.indexOf('3.30 A') !== -1, 'shared CT2 shows its real reading -- got: ' + div.innerHTML);
  assert(div.innerHTML.indexOf('CT0') === -1, 'unassigned CT0 does not appear in the shared section -- got: ' + div.innerHTML);
  assert(div.innerHTML.indexOf('CT1') === -1, 'single-zone CT1 does not appear in the shared section (it is on zone 3 own row) -- got: ' + div.innerHTML);
}

// ---------------------------------------------------------------------------
// Group 4: none installed / none shared -- the whole shared section hides
// rather than rendering an empty card.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext(RENDER_CURRENT_CARD_SRC);
  const zones = [zone(0x01), zone(0x00)]; // CT0 single-zone, nothing shared
  const data = { ct_current_a: [1.1, 2.2, 3.3], ct_fitted: [true, false, false] };
  const div = vm.runInContext('renderCurrentCard(data, zones)', Object.assign(ctx, { data, zones }));
  assert(div === null, 'renderCurrentCard returns null (section hidden) when no channel is shared -- got: ' + JSON.stringify(div));
}

// ---------------------------------------------------------------------------
// NEGATIVE TEST (feedback_negative_test_every_check): prove this suite can
// actually catch the two regressions the owner explicitly called out --
// (a) an unassigned CT rendering somewhere, and (b) a single-zone CT
// landing in the shared section. Mutates EXTRACTED production source (a
// scratch copy, never the file on disk) to remove the assignment-count
// filter from renderCurrentCard, then re-runs Group 3/4's assertions and
// requires them to go RED.
// ---------------------------------------------------------------------------
{
  const MUTATED_SRC = RENDER_CURRENT_CARD_SRC.replace(
    'if (grouped && ctChannelZones(zones, ci).length < 2) continue;',
    'if (false) continue;'
  );
  assert(MUTATED_SRC !== RENDER_CURRENT_CARD_SRC, 'sanity: the mutation string was actually found and replaced');

  const ctx = makeContext(MUTATED_SRC);
  const zones = [zone(0x04), zone(0x04), zone(0x00), zone(0x02)];
  const data = { ct_current_a: [1.1, 2.2, 3.3], ct_fitted: [true, true, true] };
  const div = vm.runInContext('renderCurrentCard(data, zones)', Object.assign(ctx, { data, zones }));
  const html = div.innerHTML;
  const mutationCaughtRed = (html.indexOf('CT0') !== -1) || (html.indexOf('CT1') !== -1);
  assert(mutationCaughtRed,
    'MUTATION: disabling the assignment-count filter must make unassigned CT0 and single-zone CT1 ' +
    'reappear in the shared section, which the Group 3 assertions above must catch -- ' +
    'proves this suite is a real check, not a vacuous one. got: ' + html);
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
