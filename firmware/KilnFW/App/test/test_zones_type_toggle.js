/* Node-only test for zones_page.html's ON_OFF_ZONE_PLAN.md step 6 UI gating:
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
function renderZoneFragment(zoneType) {
  // zoneBlock() itself does a lot (relay bitmaps, thermo bitmaps, CT probes,
  // inheritance dropdown) unrelated to this gating and requires more of the
  // page's surrounding state than this test needs. The gating logic this
  // bug lives in is entirely inside the settingsStackHtml/zoneTypeHtml
  // template literals build in zoneBlock() -- extract THOSE ranges verbatim
  // and evaluate them with the minimal locals they close over, rather than
  // re-deriving the gating structure by hand (which would just re-assert
  // this test author's belief about the markup, not the real markup).
  const TEMPLATE_SRC = extractRange(
    '  var pidPanelHtml =',
    "    '</details>';"
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

function buildZoneDiv(zoneType) {
  const html = renderZoneFragment(zoneType);
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

console.log('\n' + passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures:\n  ' + failures.join('\n  '));
  process.exit(1);
}
process.exit(0);
