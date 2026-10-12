/* Node-only test for zones_page.html's ON_OFF_ZONE.md step 6 UI gating:
 * toggleZoneTypeUi() must hide every field that does nothing for the zone
 * type currently selected, and must NOT hide anything that still applies.
 *
 * Why this exists: a UI aggregate review (d89256fe) found the heater PWM
 * relay-timing fields (window/min-on/min-off, all in MILLISECONDS) were
 * missing the .heaterOnly class that 172e3081 uses to hide PID gains,
 * coupling, ramp rate, control mode and guard thresholds for an
 * ON_OFF_DEVICE zone. Left unwrapped, an on/off zone's card showed BOTH the
 * (inert) ms-based heater PWM timing pair and the (live) seconds-based
 * on/off min-on/min-off pair at once -- an operator could edit the wrong
 * one and believe they had configured the device's switching limits.
 *
 * This test extracts toggleZoneTypeUi() and updateOnOffCycles() verbatim
 * from zones_page.html, renders one zone's real bodyHtml/zoneTypeHtml via
 * a tiny regex HTML->DOM parser (no jsdom dependency in this repo), runs
 * the real extracted function against that real DOM, and checks EFFECTIVE
 * visibility (walking up the ancestor chain for a display:none), not just
 * whether the input element itself carries the class -- exactly the check
 * that would have caught the missing class on the wrapping <div>.
 *
 * Run: node firmware/KilnFW/App/test/test_zones_type_toggle.js
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
// Extract the two functions under test, verbatim, from the real page.
// ---------------------------------------------------------------------------
const TOGGLE_SRC =
  extractRange('function toggleZoneTypeUi(div) {', '}') + '\n' +
  extractRange('function updateOnOffCycles(div) {', '}');
assert(TOGGLE_SRC.indexOf('heaterOnly') !== -1,
  'sanity: extracted range includes the .heaterOnly gating');

// ---------------------------------------------------------------------------
// Minimal regex-based HTML->DOM parser: just enough of the DOM API for
// toggleZoneTypeUi/updateOnOffCycles to run against a REAL rendered zone
// fragment (not a hand-built stand-in) -- class lists, single-class
// querySelector(All), .value on inputs/selects (honoring the "selected"
// attribute), .style.display, .textContent, and an isVisible() walk that
// mirrors the browser's inherited display:none.
// ---------------------------------------------------------------------------
const VOID_TAGS = new Set(['input', 'br', 'img', 'hr']);

function parseAttrs(attrStr) {
  const attrs = {};
  const re = /([a-zA-Z_-]+)(?:\s*=\s*"([^"]*)")?/g;
  let m;
  while ((m = re.exec(attrStr))) attrs[m[1]] = m[2] === undefined ? '' : m[2];
  return attrs;
}

class Node2 {
  constructor(tag, attrs) {
    this.tag = tag;
    this.attrs = attrs || {};
    this.classes = (this.attrs.class || '').split(/\s+/).filter(Boolean);
    this.children = [];
    this.parent = null;
    this._text = '';
    this.style = {};
    if (this.tag === 'input' || this.tag === 'select') {
      this._value = this.attrs.value !== undefined ? this.attrs.value : '';
    }
    this.classList = { contains: (c) => this.classes.includes(c) };
  }
  get value() {
    if (this.tag === 'select') {
      const opt = this.children.find((c) => c.tag === 'option' && c.attrs.selected !== undefined);
      return opt ? opt.attrs.value : (this.children[0] ? this.children[0].attrs.value : '');
    }
    return this._value;
  }
  set value(v) { this._value = v; }
  get textContent() {
    if (this._text) return this._text;
    return this.children.map((c) => c.textContent).join('');
  }
  set textContent(v) { this._text = v; this.children = []; }
  matches(sel) {
    if (sel[0] === '.') return this.classes.includes(sel.slice(1));
    return this.tag === sel;
  }
  querySelectorAll(sel) {
    const out = [];
    (function walk(n) {
      for (const c of n.children) {
        if (c.matches(sel)) out.push(c);
        walk(c);
      }
    })(this);
    return out;
  }
  querySelector(sel) {
    const all = this.querySelectorAll(sel);
    return all.length ? all[0] : null;
  }
  closest(sel) {
    let n = this;
    while (n) {
      if (n.matches(sel)) return n;
      n = n.parent;
    }
    return null;
  }
}

function parseHtml(html) {
  const root = new Node2('root', {});
  const stack = [root];
  const re = /<(\/?)([a-zA-Z0-9]+)([^>]*)>|([^<]+)/g;
  let m;
  while ((m = re.exec(html))) {
    if (m[4] !== undefined) {
      const top = stack[stack.length - 1];
      top._text += m[4];
      continue;
    }
    const closing = m[1] === '/';
    const tag = m[2].toLowerCase();
    if (closing) {
      for (let i = stack.length - 1; i >= 1; i--) {
        if (stack[i].tag === tag) { stack.length = i; break; }
      }
      continue;
    }
    const node = new Node2(tag, parseAttrs(m[3] || ''));
    const top = stack[stack.length - 1];
    node.parent = top;
    top.children.push(node);
    if (!VOID_TAGS.has(tag) && !/\/\s*$/.test(m[3] || '')) stack.push(node);
  }
  return root;
}

// Effective visibility: mirrors a browser's inherited display:none --
// exactly what "hidden" must mean for this bug (an ancestor missing
// .heaterOnly means no ancestor ever gets display:none, so the field stays
// effectively visible even though nothing marks the INPUT itself visible
// on purpose).
function isVisible(node) {
  let n = node;
  while (n) {
    if (n.style && n.style.display === 'none') return false;
    n = n.parent;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Render one real zone fragment via the actual page functions (zoneBlock's
// settingsStackHtml/zoneTypeHtml), for zone_type=0 (Heater) and zone_type=1
// (On/off device), then run the real toggleZoneTypeUi() against each.
// ---------------------------------------------------------------------------
function renderZoneFragment(zoneType, overrides) {
  // zoneBlock() itself does a lot (relay bitmaps, thermo bitmaps, CT probes,
  // inheritance dropdown) unrelated to this gating and requires more of the
  // page's surrounding state than this test needs. The gating logic this
  // bug lives in is entirely inside the settingsStackHtml/zoneTypeHtml
  // template literals build in zoneBlock() -- extract THOSE ranges verbatim
  // and evaluate them with the minimal locals they close over, rather than
  // re-deriving the gating structure by hand (which would just re-assert
  // this test author's belief about the markup, not the real markup).
  // Three ranges, not two: settingsStackHtml is extracted from its own
  // declaration rather than being swept up with pidPanelHtml, because the
  // stack no longer ends at the guards <details> -- it ends at the closing
  // </div> of the per-group frame that wraps the guards (the 2026-09-18
  // group-frame change). Each range is terminated by the first matching
  // marker AFTER its own start, so "    '</div>';" resolves to pidPanelHtml's
  // own terminator in the first range and to the stack's in the second.
  const TEMPLATE_SRC = extractRange(
    '  var pidPanelHtml =',
    "    '</div>';"
  ) + '\n' + extractRange(
    '  var settingsStackHtml =',
    "    '</div>';"
  ) + '\n' + extractRange(
    '  var zoneType = z.zone_type || 0;',
    "    '</div>';"
  );

  const sandbox = {
    z: {
      pid_kp: 1, pid_ki: 2, pid_kd: 3, tc_type: 3, cal_offset_c: 0,
      max_ramp_c_per_hr: 100, sanity_rate_c_per_min: 0, max_temp_c: 0,
      min_temp_c: -20, heater_window_ms: 60000, heater_min_on_ms: 10000,
      heater_min_off_ms: 2000, cross_zone_max_delta_c: 0,
      guard_wrong_dir_window_s: 0, guard_wrong_dir_rate_c_per_min: 0,
      guard_off_settle_s: 0, guard_runaway_rate_c_per_min: 0,
      guard_runaway_margin_c: 0, guard_drift_period_s: 0,
      guard_sensor_fault_debounce_ticks: 0, guard_frozen_window_s: 0,
      zone_type: zoneType, failsafe_state: 0, hyst_c: 0, min_on_s: 0,
      min_off_s: 0, model_k_dc: 0, model_tau_s: 0, model_dead_time_s: 0,
      fuzzy_strength_pct: 30,
      coupling_diag_k_dc: 12.5, ease_off_window_mult: 2.5,
      approach_rate_cap_c_per_hr: 300, error_band_c: 15,
      rate_band_c_per_s: 0.25, progress_band_c: 3.5,
      ...(overrides || {}),
    },
    current: {},
    i: 0,
    mode: 0,
    k: 0, tau: 0, dead: 0, haveModel: false,
    couplingHtml: '',
    modelHtml: '',
    pidGainsLabel: 'Kp / Ki / Kd',
    fuzzyStrength: 30,
    MODE_DESC: {},
    modeHtml: '<select class="ctrlmode"></select>',
    tcTypeSelectHtml: () => '<select class="tctype"></select>',
    infoDetailsHtml: () => '',
    result: undefined,
  };
  vm.createContext(sandbox);
  vm.runInContext(
    TEMPLATE_SRC + '\nresult = settingsStackHtml + zoneTypeHtml;',
    sandbox
  );
  return sandbox.result;
}

function buildZoneDiv(zoneType, overrides) {
  const html = renderZoneFragment(zoneType, overrides);
  // Wrap in a zonetype select (real page renders it as a <select>, so
  // simulate the "currently selected" option the same way).
  const wrapped =
    '<div class="zone">' +
    '<select class="zonetype"><option value="' + zoneType + '" selected>x</option></select>' +
    html +
    '</div>';
  const root = parseHtml(wrapped);
  return root.querySelector('.zone');
}

vm.runInThisContext; // no-op keep require(vm) used above readable
const fnHolder = {};
vm.createContext(fnHolder);
vm.runInContext(TOGGLE_SRC, fnHolder);
const toggleZoneTypeUi = fnHolder.toggleZoneTypeUi;
assert(typeof toggleZoneTypeUi === 'function', 'sanity: toggleZoneTypeUi extracted as a function');

// ---------------------------------------------------------------------------
// Case 1: On/off device zone -- heater-only fields (PID gains, ramp rate,
// control mode, guard thresholds, AND the heater PWM relay-timing fields)
// must all be effectively hidden.
// ---------------------------------------------------------------------------
{
  const div = buildZoneDiv(1);
  toggleZoneTypeUi(div);

  const kp = div.querySelector('.kp');
  const ramp = div.querySelector('.ramp');
  const minon = div.querySelector('.minon');
  const minoff = div.querySelector('.minoff');
  const windowEl = div.querySelector('.window');

  assert(kp && !isVisible(kp), 'on/off zone: PID Kp field is hidden');
  assert(ramp && !isVisible(ramp), 'on/off zone: max ramp rate field is hidden');
  assert(minon && !isVisible(minon),
    'on/off zone: heater PWM min-on (ms) field is hidden (the reported defect)');
  assert(minoff && !isVisible(minoff),
    'on/off zone: heater PWM min-off (ms) field is hidden (the reported defect)');
  assert(windowEl && !isVisible(windowEl),
    'on/off zone: heater PWM window (ms) field is hidden (the reported defect)');

  // Converse: the on/off-only fields must be VISIBLE on an on/off zone.
  const hystc = div.querySelector('.hystc');
  const minons = div.querySelector('.minons');
  const minoffs = div.querySelector('.minoffs');
  const failsafestate = div.querySelector('.failsafestate');
  assert(hystc && isVisible(hystc), 'on/off zone: hysteresis (hyst_c) field is visible');
  assert(minons && isVisible(minons), 'on/off zone: minimum ON time (min_on_s) field is visible');
  assert(minoffs && isVisible(minoffs), 'on/off zone: minimum OFF time (min_off_s) field is visible');
  assert(failsafestate && isVisible(failsafestate), 'on/off zone: fail-safe state field is visible');
}

// ---------------------------------------------------------------------------
// Case 2: Heater zone -- the on/off-only fields must be hidden, and every
// heater field (including the PWM relay-timing fields) must be visible.
// ---------------------------------------------------------------------------
{
  const div = buildZoneDiv(0);
  toggleZoneTypeUi(div);

  const kp = div.querySelector('.kp');
  const minon = div.querySelector('.minon');
  const minoff = div.querySelector('.minoff');
  const windowEl = div.querySelector('.window');
  assert(kp && isVisible(kp), 'heater zone: PID Kp field is visible');
  assert(minon && isVisible(minon), 'heater zone: heater PWM min-on (ms) field is visible');
  assert(minoff && isVisible(minoff), 'heater zone: heater PWM min-off (ms) field is visible');
  assert(windowEl && isVisible(windowEl), 'heater zone: heater PWM window (ms) field is visible');

  const hystc = div.querySelector('.hystc');
  const minons = div.querySelector('.minons');
  const minoffs = div.querySelector('.minoffs');
  const failsafestate = div.querySelector('.failsafestate');
  assert(hystc && !isVisible(hystc), 'heater zone: hysteresis (hyst_c) field is hidden');
  assert(minons && !isVisible(minons), 'heater zone: minimum ON time (min_on_s) field is hidden');
  assert(minoffs && !isVisible(minoffs), 'heater zone: minimum OFF time (min_off_s) field is hidden');
  assert(failsafestate && !isVisible(failsafestate), 'heater zone: fail-safe state field is hidden');
}

// ---------------------------------------------------------------------------
// Case 3 (2026-09-22): six always-emitted, POST-round-tripped scalars that
// GET /api/zones and POST /api/zones both handle (zones_http_get.c ~528-574,
// zones_http_post_parse.c ~799-921) had zero renders anywhere on this page --
// coupling_diag_k_dc, ease_off_window_mult, approach_rate_cap_c_per_hr,
// error_band_c, rate_band_c_per_s, progress_band_c. A whole-page save from
// this page could only avoid erasing them because the server's own
// OPTIONAL/omit-preserves fallback happened to cover the gap; an operator
// could never SEE or CHANGE any of them from the web UI at all. This proves
// both directions: the real rendered markup shows the value GET supplied
// (render), and the real save-collection source sends it back under the
// exact wire key POST expects (round trip).
// ---------------------------------------------------------------------------
{
  const div = buildZoneDiv(0);
  const fieldMap = [
    ['.couplingdiag', 'coupling_diag_k_dc', 12.5],
    ['.easeoffmult', 'ease_off_window_mult', 2.5],
    ['.approachratecap', 'approach_rate_cap_c_per_hr', 300],
    ['.errorband', 'error_band_c', 15],
    ['.rateband', 'rate_band_c_per_s', 0.25],
    ['.progressband', 'progress_band_c', 3.5],
  ];
  fieldMap.forEach(([cls, fieldName, expected]) => {
    const el = div.querySelector(cls);
    assert(el !== null, 'render: .' + cls.slice(1) + ' input exists on the page for ' + fieldName);
    assert(el && Number(el.value) === expected,
      'render: .' + cls.slice(1) + ' shows the GET-supplied value for ' + fieldName +
      ' (got ' + (el && el.value) + ', want ' + expected + ')');
  });

  // Round trip: saveAll()'s collection loop must send each field back under
  // the exact wire key zones_http_post_parse.c's snprintf(key, ..., "z%u_...")
  // calls construct, read off ownStack (this zone's own stack, matching the
  // cal-offset field's "no inheritance group" precedent just above it) --
  // not off a group-terminal stack, since none of these six is part of any
  // settings_source inheritance group on the C side. Extracted verbatim out
  // of saveBtn's click handler (it is inline there, not its own named
  // function, same as test_zones_inheritance.js's channel-type fan-out
  // block) and driven against the REAL rendered .customStack from the same
  // `div` used for the render assertions above -- not a hand-built stand-in
  // -- so this exercises the actual collection code, not a regex belief
  // about it.
  const SIX_FIELD_ROUND_TRIP_SRC = extractRange(
    "    params.push('z' + i + '_coupling_diag_k_dc=' + ownStack.querySelector('.couplingdiag').value);",
    "    params.push('z' + i + '_progressband=' + ownStack.querySelector('.progressband').value);"
  );
  assert(SIX_FIELD_ROUND_TRIP_SRC.indexOf('easeoffmult') !== -1,
    'sanity: extracted range is the six-field round-trip block');

  function runSixFieldRoundTrip(i, ownStack) {
    const ctx = vm.createContext({ i, ownStack, params: [] });
    new vm.Script('(function(){\n' + SIX_FIELD_ROUND_TRIP_SRC + '\n})();',
      { filename: 'zones_page.html (six-field round-trip slice)' }).runInContext(ctx);
    return ctx.params;
  }

  const postKeyMap = [
    ['.couplingdiag', 'coupling_diag_k_dc', 'coupling_diag_k_dc'],
    ['.easeoffmult', 'ease_off_window_mult', 'easeoffmult'],
    ['.approachratecap', 'approach_rate_cap_c_per_hr', 'approachratecap'],
    ['.errorband', 'error_band_c', 'errorband'],
    ['.rateband', 'rate_band_c_per_s', 'rateband'],
    ['.progressband', 'progress_band_c', 'progressband'],
  ];
  // The fragment under test (settingsStackHtml + zoneTypeHtml) is rendered
  // without zoneBlock()'s outer bodyHtml wrapper that supplies the literal
  // ".customStack" class (zones_page.html:1598) -- these six fields are
  // querySelector'd from wherever they render either way (there is only one
  // of each per zone), so `div` itself stands in for ownStack here, same as
  // fieldMap's render assertions above already query `div` directly.
  const postedParams = runSixFieldRoundTrip(0, div);
  postKeyMap.forEach(([cls, fieldName, wireKey]) => {
    const expected = fieldMap.find((f) => f[0] === cls)[2];
    const want = 'z0_' + wireKey + '=' + expected;
    assert(postedParams.indexOf(want) !== -1,
      'round trip: saveAll() posts ' + want + ' for ' + fieldName + ' (got: ' + postedParams.join(', ') + ')');
  });

  // Zero is the documented "firmware default"/"uncapped" sentinel for
  // approach_rate_cap_c_per_hr, not "no value entered" -- it must still be
  // POSTED as the literal string "0", never dropped from params entirely
  // (a dropped key would fall through to the server's OPTIONAL-field
  // omit-preserves fallback and silently keep whatever cap was previously
  // stored, defeating an operator's deliberate "uncap this zone" edit).
  const zeroDiv = buildZoneDiv(0, { approach_rate_cap_c_per_hr: 0 });
  const zeroParams = runSixFieldRoundTrip(0, zeroDiv);
  assert(zeroParams.indexOf('z0_approachratecap=0') !== -1,
    'round trip: approach_rate_cap_c_per_hr=0 (uncapped sentinel) posts as z0_approachratecap=0, not dropped ' +
    '(got: ' + zeroParams.join(', ') + ')');
}

console.log('\n' + passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures:\n  ' + failures.join('\n  '));
  process.exit(1);
}
process.exit(0);
