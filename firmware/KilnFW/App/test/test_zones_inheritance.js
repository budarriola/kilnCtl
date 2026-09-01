/* Node-only test harness for zones_page.html's "same as zone N" settings
 * inheritance (PID_EXPANSION_PLAN.md 3.5): resolveTerminal()'s chain walk
 * and the channel-type fan-out saveBtn's click handler runs before POSTing.
 *
 * Why this exists: an audit found settings_source inheritance covered only
 * by migration/range tests (test_zones_http.c's nvs_load_from()/
 * import_blob() cycle-collapse tests) -- nothing exercised the client-side
 * chain resolution itself, or its one real cross-zone invariant: a zone
 * reading MORE THAN ONE physical thermocouple channel (thermo_mask with
 * multiple bits) must write the SAME resolved tc_type to every channel it
 * claims, because zone_cfg_t::tc_type is per-CHANNEL storage, not per-zone
 * (see zones_page.html's saveBtn comment, "zone_cfg_t::tc_type is per
 * CHANNEL, not per zone").
 *
 * Follows test_firing_chart.js's extraction pattern for the three pure
 * functions (zoneDiv/settingsSourceOf/resolveTerminal), and additionally
 * extracts the channel-type fan-out block out of saveBtn's click handler
 * verbatim (it is inline in that handler, not its own named function) into
 * a thin synthesized wrapper -- no logic inside the extracted block is
 * rewritten, only `var params = []` / `return params` bracket it so it can
 * be called in isolation.
 *
 * Run: node firmware/KilnFW/App/test/test_zones_inheritance.js
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

// ---------------------------------------------------------------------------
// Extract the chain-walk trio verbatim.
// ---------------------------------------------------------------------------
const CHAIN_SRC =
  extractRange('function zoneDiv(k) {', '}') + '\n' +
  extractRange('function settingsSourceOf(k) {', '}') + '\n' +
  extractRange('function resolveTerminal(k, thermoCount) {', '}');
assert(CHAIN_SRC.indexOf('function resolveTerminal') !== -1,
  'sanity: extracted range includes resolveTerminal');
assert(CHAIN_SRC.indexOf('forced') !== -1,
  'sanity: extracted range includes the forced-collapse logic');

// The channel-type fan-out block, pulled straight out of saveBtn's handler,
// unmodified -- see this file's header comment for why it needs a wrapper.
const CHANNEL_TYPE_BLOCK = extractRange(
  '  var channelType = {};',
  '  }' // the closing brace of the "for (var c2 ..." loop, zones_page.html:1594
);
assert(CHANNEL_TYPE_BLOCK.indexOf('channelType[c2]') !== -1,
  'sanity: extracted range is the channel-type fan-out block');

// ---------------------------------------------------------------------------
// Minimal DOM stub: zone divs addressed by data-index, each with a
// .settingssrc select (dataset.storedInvalid / .value) and a .customStack
// with a .tctype input -- exactly what zoneDiv/settingsSourceOf/
// resolveTerminal and the fan-out block read.
// ---------------------------------------------------------------------------
function makeZoneDom(zones) {
  // zones: { [k]: { settingsSource: number|undefined (undefined => no select,
  //                  as for zone 0), storedInvalid: number|undefined,
  //                  tcType: string } }
  const divs = {};
  Object.keys(zones).forEach((kStr) => {
    const k = Number(kStr);
    const z = zones[k];
    const sel = z.settingsSource === undefined ? null : {
      dataset: z.storedInvalid !== undefined ? { storedInvalid: String(z.storedInvalid) } : {},
      value: String(z.settingsSource),
    };
    const tctype = { value: z.tcType === undefined ? '3' : String(z.tcType) };
    const customStack = {
      querySelector(sub) { if (sub === '.tctype') return tctype; return null; },
    };
    divs[k] = {
      querySelector(sub) {
        if (sub === '.settingssrc') return sel;
        if (sub === '.customStack') return customStack;
        return null;
      },
    };
  });
  const document = {
    querySelector(sel) {
      const m = /^#zones \.zone\[data-index="(\d+)"\]$/.exec(sel);
      if (!m) return null;
      return divs[Number(m[1])] || null;
    },
  };
  return { document, divs };
}

function loadChain(zones) {
  const dom = makeZoneDom(zones);
  const ctx = vm.createContext({ console, document: dom.document, parseInt, isNaN, window: { kcEscapeHtml: (s) => s } });
  new vm.Script(CHAIN_SRC, { filename: 'zones_page.html (chain-walk slice)' }).runInContext(ctx);
  return ctx;
}

// ---------------------------------------------------------------------------
// GAP 2, test 1: save/reload round-trip -- an inherited zone still resolves
// to its source's values after "persistence" (a fresh chain walk against a
// state assembled purely from what settingsSourceOf() would read back off a
// reloaded page, not anything cached from before the reload). ASYMMETRIC
// fixture: zone 3 inherits from zone 1 (not 0, not a neighbor), thermoCount
// wide enough that a transposed index (e.g. 1 vs 3) resolves to a
// distinguishably different zone.
// ---------------------------------------------------------------------------
(function testRoundTripResolvesToSourceValues() {
  const zones = {
    0: { tcType: '2' }, // zone 0: no dropdown, always its own source
    1: { settingsSource: 255, tcType: '5' }, // zone 1: CUSTOM, its own distinct type
    2: { settingsSource: 0, tcType: '2' },
    3: { settingsSource: 1, tcType: '5' }, // zone 3 "reloaded" already inheriting zone 1
  };
  const ctx = loadChain(zones);
  const result = ctx.resolveTerminal(3, 4);
  assert(result.terminal === 1 && result.forced === false,
    'round-trip: zone 3 (settings_source=1) resolves to terminal zone 1, not zone 0 or itself');
  const terminalDiv = ctx.zoneDiv(result.terminal);
  const terminalType = terminalDiv.querySelector('.customStack').querySelector('.tctype').value;
  assert(terminalType === '5',
    'round-trip: the resolved terminal (zone 1) still reports its OWN stored value (tc_type 5) after reload, ' +
    'not zone 0\'s (2) or zone 3\'s pre-inherit placeholder');
})();

// ---------------------------------------------------------------------------
// GAP 2, test 2: a 2-zone cycle (A inherits from B, B inherits from A)
// collapses to Custom (forced=true) instead of hanging or recursing.
// ASYMMETRIC: zones 2 and 4 (not 0/1), so a transposed index bug (checking
// zone 0/1's chain instead of the real cycling pair) would pass here but
// this test would still fail.
// ---------------------------------------------------------------------------
(function testTwoZoneCycleCollapsesToCustom() {
  const zones = {
    0: { tcType: '3' },
    1: { settingsSource: 255, tcType: '3' },
    2: { settingsSource: 4, tcType: '1' }, // zone 2 -> zone 4
    3: { settingsSource: 255, tcType: '3' },
    4: { settingsSource: 2, tcType: '1' }, // zone 4 -> zone 2: the cycle
  };
  const ctx = loadChain(zones);
  const withTimeout = (fn) => {
    // resolveTerminal has no internal iteration cap of its own (unlike the
    // firmware's hop-capped chain walk) -- it relies on the visited-set to
    // terminate. Guard the test itself against an infinite loop so a
    // regression that removed the visited-set shows up as a timeout/failure
    // rather than hanging this whole test run.
    const start = Date.now();
    const r = fn();
    assert(Date.now() - start < 1000, 'cycle resolution terminates promptly (does not hang)');
    return r;
  };
  const result = withTimeout(() => ctx.resolveTerminal(2, 5));
  assert(result.forced === true, 'a 2-zone cycle (2<->4) is detected and forced to collapse, not resolved as legal');
  const result4 = withTimeout(() => ctx.resolveTerminal(4, 5));
  assert(result4.forced === true, 'the cycle is detected walking from EITHER member (zone 4), not just the one tested first');
})();

// Self-reference (zone points at itself) is also a cycle, caught on the
// first hop -- proven separately from the 2-zone case above so a fix that
// only handles multi-hop cycles is still caught.
(function testSelfReferenceCollapsesToCustom() {
  const zones = {
    0: { tcType: '3' },
    1: { settingsSource: 255, tcType: '3' },
    2: { settingsSource: 2, tcType: '1' }, // zone 2 -> itself
  };
  const ctx = loadChain(zones);
  const result = ctx.resolveTerminal(2, 3);
  assert(result.forced === true, 'a self-referencing settings_source collapses to Custom');
})();

// A longer, legal (acyclic) 3-hop chain resolves cleanly -- proves the walk
// isn't just conservatively refusing every multi-hop chain.
(function testLongerAcyclicChainResolves() {
  const zones = {
    0: { tcType: '4' },
    1: { settingsSource: 0, tcType: '4' },
    2: { settingsSource: 1, tcType: '4' },
    3: { settingsSource: 2, tcType: '4' },
  };
  const ctx = loadChain(zones);
  const result = ctx.resolveTerminal(3, 4);
  assert(result.terminal === 0 && result.forced === false,
    'a legal 3->2->1->0 chain resolves all the way to zone 0, not forced to Custom');
})();

// ---------------------------------------------------------------------------
// GAP 2, test 3: shared-channel-consistency invariant -- a zone claiming
// MORE THAN ONE physical thermocouple channel via thermo_mask, while
// inheriting, must fan its resolved (terminal's) tc_type out to EVERY
// channel bit it claims, not just its own index. ASYMMETRIC fixture: zone 1
// claims channels {0,2} (not its own index 1) while inheriting zone 3's
// type, so a bug that only fans out to `info.i` (its own index) rather than
// walking thermoMask fails this, and a transposed channel index (writing to
// channel 1 instead of 0/2) also fails it.
// ---------------------------------------------------------------------------
function runChannelTypeFanout(zoneInfoInputs, thermoCount, current) {
  // zoneInfoInputs: [{ i, thermoMask, terminal, terminalTcType }]
  const zoneDivStub = (idx) => {
    const info = zoneInfoInputs.find((z) => z.terminal === idx);
    const tcType = info ? info.terminalTcType : (current.zones[idx] && current.zones[idx].tc_type);
    return { querySelector: (sub) => (sub === '.customStack' ? {
      querySelector: (s2) => (s2 === '.tctype' ? { value: String(tcType) } : null),
    } : null) };
  };
  const ctx = vm.createContext({
    console,
    zoneDiv: zoneDivStub,
    zoneInfo: zoneInfoInputs.map((z) => ({ i: z.i, thermoMask: z.thermoMask, terminal: z.terminal })),
    thermoCount,
    current,
    params: [],
  });
  new vm.Script('(function(){\n' + CHANNEL_TYPE_BLOCK + '\n})();',
    { filename: 'zones_page.html (channel-type fan-out slice)' }).runInContext(ctx);
  return ctx.params;
}

(function testSharedChannelFanoutWritesResolvedTypeToEveryClaimedChannel() {
  const current = { zones: [{ tc_type: 9 }, { tc_type: 9 }, { tc_type: 9 }, { tc_type: 7 }] };
  const zoneInfoInputs = [
    // zone 1 is its own terminal's follower: it inherits zone 3 (type 7),
    // and claims channels 0 and 2 via thermo_mask (bits 0 and 2 = 0b101 = 5)
    // -- NEITHER of which is its own index (1). Zone 3 is the terminal,
    // Custom, claiming only its own channel (bit 3).
    { i: 1, thermoMask: 0b0101, terminal: 3, terminalTcType: 7 },
    { i: 3, thermoMask: 0b1000, terminal: 3, terminalTcType: 7 },
  ];
  const params = runChannelTypeFanout(zoneInfoInputs, 4, current);
  const byChannel = {};
  params.forEach((p) => {
    const m = /^z(\d+)_tctype=(.+)$/.exec(p);
    if (m) byChannel[m[1]] = m[2];
  });
  assert(byChannel['0'] === '7', 'channel 0 (claimed by inheriting zone 1) gets the TERMINAL\'s resolved type (7), not zone 0\'s stored 9');
  assert(byChannel['2'] === '7', 'channel 2 (also claimed by zone 1 via thermo_mask) gets the same fanned-out type as channel 0');
  assert(byChannel['3'] === '7', 'channel 3 (zone 3\'s own, Custom) keeps its own type');
  assert(byChannel['1'] === '9', 'channel 1 (claimed by no zone in this fixture) is echoed back unchanged from the last-loaded stored value, not overwritten by the fan-out');
})();

// A zone that is its OWN terminal (Custom, not inheriting) must write its
// type to its OWN channel index only -- never fanned out over thermo_mask,
// even if it happens to read more than one channel. This is the "identity
// case" saveBtn's own comment calls out as the bug 3.5 originally warned
// about (fanning out a Custom zone's edit over every channel it reads would
// silently re-type some OTHER channel).
(function testCustomZoneWritesOnlyItsOwnChannelEvenWithMultiChannelMask() {
  const current = { zones: [{ tc_type: 9 }, { tc_type: 9 }] };
  const zoneInfoInputs = [
    // zone 1 is Custom (its own terminal) but reads BOTH channels 0 and 1.
    { i: 1, thermoMask: 0b11, terminal: 1, terminalTcType: 6 },
  ];
  const params = runChannelTypeFanout(zoneInfoInputs, 2, current);
  const byChannel = {};
  params.forEach((p) => {
    const m = /^z(\d+)_tctype=(.+)$/.exec(p);
    if (m) byChannel[m[1]] = m[2];
  });
  assert(byChannel['1'] === '6', 'a Custom zone writes its own edited type to its own channel index');
  assert(byChannel['0'] === '9', 'a Custom zone reading a SECOND channel via thermo_mask does NOT fan its type onto that channel -- it keeps its last-stored value');
})();

// ---------------------------------------------------------------------------
console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exitCode = 1;
}
