/* Node-only test for profiles_page.html's on/off-rule editor (owner request
 * 2026-09-14/15, ON_OFF_ZONE_PLAN.md plan step 5's web UI): a
 * render(ooRuleFieldsHtml) -> serialize(ooRulesToParams) round trip against
 * the real, extracted page functions, run over a fake but real-markup DOM
 * (same regex parser pattern as test_zones_type_toggle.js -- no jsdom
 * dependency in this repo).
 *
 * What this guards against: profiles_edit_http.c's form parser
 * (rule%u_zone/segment/enable/phase/direction/temp_source/temp_cmp/temp_c/
 * time_start_s/time_stop_s/invert) is the one place that decides what a
 * saved rule means server-side -- a field name or value mismatch here would
 * silently drop or corrupt a rule on save with no client-side symptom (the
 * exact "temp_source was never sent" gap this same pass found and fixed in
 * profiles_edit_http.c). This test locks the wire field names AND the
 * temp_source-follows-temp_cmp derivation (0 when "(none)", 1 otherwise --
 * only 0/1 are wired by profile_resolve_on_off_rule() today).
 *
 * Opus review pass (2026-09-20) added coverage for a second, more serious
 * class of bug found in the same area: ooZoneOptionsHtml()/refreshOoUi() and
 * ooRulesToParams() are ALL extracted verbatim now (no more hand-rolled
 * stand-ins for the zone/segment option renderers) so this test exercises
 * the exact code the real page runs, including the fix for:
 *   - a stored rule whose zone_index is not among onOffZones (either because
 *     onOffZones is empty, or because it names some OTHER zone) used to
 *     either send rule*_zone= empty (dropped silently server-side) or fall
 *     back to the browser's default-selected option (silently retargeted).
 *   - refreshOoUi() used to wipe #oorules' entire innerHTML whenever
 *     onOffZones was empty, destroying any already-loaded rule rows.
 *   - the saveBtn click handler must refuse to submit while any row is
 *     showing the "no longer an on/off zone" orphan option -- tested here
 *     via the extracted ooHasStaleZoneRow() helper.
 *
 * Run: node firmware/KilnFW/App/test/test_on_off_rules_editor.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'profiles_page.html');
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
// Minimal regex-based HTML->DOM parser (adapted from test_zones_type_toggle.js):
// class lists, single-class querySelector(All), .value/.checked honoring
// "selected"/"checked" attributes, .textContent, and .selectedOptions on a
// <select>, plus document.getElementById.
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
    if (this.tag === 'input' || this.tag === 'select') {
      this._value = this.attrs.value !== undefined ? this.attrs.value : '';
    }
    if (this.tag === 'input') this._checked = this.attrs.checked !== undefined;
    this.classList = { contains: (c) => this.classes.includes(c) };
    this.style = {};
  }
  get value() {
    if (this.tag === 'select') {
      const opt = this.children.find((c) => c.tag === 'option' && c.attrs.selected !== undefined);
      return opt ? opt.attrs.value : (this.children[0] ? this.children[0].attrs.value : '');
    }
    return this._value;
  }
  set value(v) { this._value = v; }
  get checked() { return !!this._checked; }
  set checked(v) { this._checked = !!v; }
  get textContent() {
    let out = this._text;
    for (const c of this.children) out += c.textContent;
    return out;
  }
  get selectedOptions() {
    if (this.tag !== 'select') return [];
    const opts = this.children.filter((c) => c.tag === 'option');
    const flagged = opts.filter((o) => o.attrs.selected !== undefined);
    if (flagged.length) return flagged;
    // Real <select> behavior with no explicit "selected" attribute anywhere:
    // the first option is selected by default.
    return opts.length ? [opts[0]] : [];
  }
  matches(sel) {
    if (sel[0] === '.') return this.classes.includes(sel.slice(1));
    return this.tag === sel;
  }
  getAttribute(name) {
    const v = this.attrs[name];
    return v === undefined ? null : v;
  }
  querySelectorAll(sel) {
    const out = [];
    const parts = sel.split(':checked');
    const cls = parts[0];
    const onlyChecked = sel.indexOf(':checked') !== -1;
    (function walk(n) {
      for (const c of n.children) {
        if (c.matches(cls) && (!onlyChecked || c.checked)) out.push(c);
        walk(c);
      }
    })(this);
    return out;
  }
  querySelector(sel) {
    const all = this.querySelectorAll(sel);
    return all.length ? all[0] : null;
  }
  getElementById(id) {
    let found = null;
    (function walk(n) {
      for (const c of n.children) {
        if (found) return;
        if (c.attrs.id === id) { found = c; return; }
        walk(c);
      }
    })(this);
    return found;
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

// ---------------------------------------------------------------------------
// Extract the real functions under test, verbatim, from the real page.
// Opus review fix: ooZoneOptionsHtml/ooSegmentOptionsHtml/refreshOoUi/
// ooHasStaleZoneRow are now extracted too, instead of hand-rolled stand-ins,
// so this test runs the exact code the page ships (defects 1/2/save-refusal
// live entirely inside these functions).
// ---------------------------------------------------------------------------
const RULES_SRC =
  extractRange('function ooZoneOptionsHtml(selected) {', '}') + '\n' +
  extractRange('function ooSegmentOptionsHtml(selected) {', '}') + '\n' +
  extractRange('function refreshOoUi() {', '}') + '\n' +
  extractRange('function ooPhaseChecksHtml(mask) {', '}') + '\n' +
  extractRange('function ooDirChecksHtml(mask) {', '}') + '\n' +
  extractRange('function ooRuleFieldsHtml(r) {', '}') + '\n' +
  extractRange('function ooRulesToParams() {', '}') + '\n' +
  extractRange('function ooHasStaleZoneRow() {', '}');

assert(RULES_SRC.indexOf('rule\' + i + \'_temp_source') !== -1,
  'sanity: extracted range includes the temp_source field name');
assert(RULES_SRC.indexOf('no longer an on/off zone') !== -1,
  'sanity: extracted range includes the real ooZoneOptionsHtml() orphan-option fix');

// Build a fake document that supports exactly what the real functions need:
// getElementById('oorules')/('segments'), querySelectorAll('.oorule') on the
// root, plus the per-row selectors. onOffZones/window.kcEscapeHtml are the
// real page's own globals, provided here as the real page provides them.
function buildContext(rules, onOffZones, segmentCount) {
  const sandbox = {
    onOffZones: onOffZones,
    infoIconHtml: function () { return ''; },
    window: { kcEscapeHtml: function (s) { return String(s); } },
    ooUpdateSummary: function () { /* cosmetic-only, irrelevant to this test's assertions */ },
  };
  vm.createContext(sandbox);
  vm.runInContext(RULES_SRC, sandbox);

  // document must exist BEFORE ooRuleFieldsHtml() runs: ooSegmentOptionsHtml()
  // reads document.getElementById('segments').children.length. Start with
  // the rule rows empty, then fill them in below via the equivalent of
  // ooRuleRow()'s real innerHTML assignment.
  var segmentsHtml = '<div id="segments">' +
    Array(segmentCount).fill('<div class="seg"></div>').join('') + '</div>';
  var ooNoZonesMsgHtml = '<div id="ooNoZonesMsg" style=""></div>';
  var addOoBtnHtml = '<div id="addOoBtn" style=""></div>';
  const root = parseHtml(segmentsHtml + '<div id="oorules"></div>' + ooNoZonesMsgHtml + addOoBtnHtml);
  sandbox.document = {
    getElementById: function (id) { return root.getElementById(id); },
    // Supports the two shapes the real page actually calls on `document`:
    // a bare class selector, and "#id .class" (id-scoped descendant).
    querySelectorAll: function (sel) {
      var m = /^#([\w-]+)\s+(.+)$/.exec(sel);
      if (m) {
        var scopeEl = root.getElementById(m[1]);
        return scopeEl ? scopeEl.querySelectorAll(m[2]) : [];
      }
      return root.querySelectorAll(sel);
    },
  };

  // Render each rule's real fields markup with the real function, then
  // splice it into the fake document exactly as ooRuleRow() would via
  // innerHTML (minus the "Remove rule" button, irrelevant here).
  var perRuleHtml = rules.map(function (r) { return sandbox.ooRuleFieldsHtml(r); });
  var oorulesEl = root.getElementById('oorules');
  var newOorules = parseHtml('<div id="oorules">' + perRuleHtml.map(function (h) {
    return '<div class="oorule">' + h + '</div>';
  }).join('') + '</div>');
  var newOorulesEl = newOorules.getElementById('oorules');
  oorulesEl.children = newOorulesEl.children;
  oorulesEl.children.forEach(function (c) { c.parent = oorulesEl; });
  return sandbox;
}

// ---------------------------------------------------------------------------
// One rule, every axis populated non-default, must round-trip through
// render -> serialize with the exact field names profiles_edit_http.c parses.
// ---------------------------------------------------------------------------
(function testFullRuleRoundTrips() {
  const zones = [{ index: 3, name: 'Damper' }, { index: 5, name: 'Vent' }];
  const rule = {
    zone: 5, segment: 2, enable: 1, phase_mask: 2 /* dwell */,
    direction_mask: 1 /* heating */, temp_source: 1, temp_cmp: 1 /* above */,
    temp_c: 600, time_start_s: 30, time_stop_s: 120, invert: 0,
  };
  const ctx = buildContext([rule], zones, 4);
  const params = ctx.ooRulesToParams();
  const asMap = {};
  params.forEach(function (p) { const i = p.indexOf('='); asMap[p.slice(0, i)] = p.slice(i + 1); });

  assert(asMap.rule0_zone === '5', 'zone round-trips (rule0_zone)');
  assert(asMap.rule0_segment === '2', 'segment round-trips (rule0_segment)');
  assert(asMap.rule0_enable === '1', 'enable round-trips (rule0_enable)');
  assert(asMap.rule0_phase === '2', 'phase mask round-trips (rule0_phase)');
  assert(asMap.rule0_direction === '1', 'direction mask round-trips (rule0_direction)');
  assert(asMap.rule0_temp_source === '1',
    'temp_source is sent as 1 when a comparison is selected -- ' +
    'profile_resolve_on_off_rule() ignores temp_cmp unless temp_source===1');
  assert(asMap.rule0_temp_cmp === '1', 'temp_cmp round-trips (rule0_temp_cmp)');
  assert(asMap.rule0_temp_c === '600', 'threshold round-trips in raw Celsius, no kcUnit conversion');
  assert(asMap.rule0_time_start_s === '30', 'time_start_s round-trips');
  assert(asMap.rule0_time_stop_s === '120', 'time_stop_s round-trips');
  assert(asMap.rule0_invert === '0', 'invert round-trips');
})();

// ---------------------------------------------------------------------------
// "(none)" temperature condition must serialize temp_source=0, not 1 -- this
// is the exact bug profiles_edit_http.c's missing field caused before this
// pass (a stale/previous temp_cmp value would otherwise leak through).
// ---------------------------------------------------------------------------
(function testNoneConditionSendsTempSourceZero() {
  const zones = [{ index: 0, name: 'Damper' }];
  const rule = {
    zone: 0, segment: 0, enable: 1, phase_mask: 0, direction_mask: 0,
    temp_source: 0, temp_cmp: 0, temp_c: 0, time_start_s: 0, time_stop_s: 0, invert: 0,
  };
  const ctx = buildContext([rule], zones, 1);
  const params = ctx.ooRulesToParams();
  const asMap = {};
  params.forEach(function (p) { const i = p.indexOf('='); asMap[p.slice(0, i)] = p.slice(i + 1); });
  assert(asMap.rule0_temp_source === '0', '"(none)" condition serializes temp_source=0');
  assert(asMap.rule0_temp_cmp === '0', '"(none)" condition serializes temp_cmp=0');
})();

// ---------------------------------------------------------------------------
// Multiple rules must each get their own index and not bleed into each
// other's fields (the exact shape a per-row DOM query bug would produce).
// ---------------------------------------------------------------------------
(function testMultipleRulesIndexIndependently() {
  const zones = [{ index: 1, name: 'A' }, { index: 2, name: 'B' }];
  const ruleA = {
    zone: 1, segment: 0, enable: 1, phase_mask: 1, direction_mask: 1,
    temp_source: 0, temp_cmp: 0, temp_c: 0, time_start_s: 0, time_stop_s: 0, invert: 0,
  };
  const ruleB = {
    zone: 2, segment: 3, enable: 0, phase_mask: 2, direction_mask: 2,
    temp_source: 1, temp_cmp: 2, temp_c: 100, time_start_s: 5, time_stop_s: 0, invert: 1,
  };
  const ctx = buildContext([ruleA, ruleB], zones, 5);
  const params = ctx.ooRulesToParams();
  const asMap = {};
  params.forEach(function (p) { const i = p.indexOf('='); asMap[p.slice(0, i)] = p.slice(i + 1); });

  assert(asMap.rule0_zone === '1' && asMap.rule1_zone === '2',
    'each rule gets its own zone at its own index');
  assert(asMap.rule0_enable === '1' && asMap.rule1_enable === '0',
    'each rule gets its own enable at its own index');
  assert(asMap.rule1_temp_cmp === '2' && asMap.rule1_temp_c === '100',
    'second rule\'s temperature condition does not fall back to the first rule\'s');
  assert(asMap.rule1_invert === '1', 'second rule\'s invert is independent of the first');
})();

// ---------------------------------------------------------------------------
// DEFECT 1 (Opus review): a stored rule targeting zone 7, but onOffZones is
// EMPTY (no on/off-typed zone exists on this board at all). Before the fix,
// ooZoneOptionsHtml() rendered only '<option value="">(none)</option>' and
// ooRulesToParams() sent rule0_zone= (empty) -- profiles_edit_http.c's
// `if (len <= 0) break;` then silently dropped the rule with an HTTP 200
// "Saved". The fix must keep the real value 7 round-tripping via a flagged
// orphan option, and refreshOoUi() must not wipe the row out of the DOM.
// ---------------------------------------------------------------------------
(function testEmptyOnOffZonesRoundTripsStoredZone() {
  const rule = {
    zone: 7, segment: 0, enable: 1, phase_mask: 0, direction_mask: 0,
    temp_source: 0, temp_cmp: 0, temp_c: 0, time_start_s: 0, time_stop_s: 0, invert: 0,
  };
  const ctx = buildContext([rule], /* onOffZones */ [], 1);

  const optionsHtml = ctx.ooZoneOptionsHtml(7);
  assert(optionsHtml.indexOf('value="7" selected') !== -1,
    'ooZoneOptionsHtml(7) with an empty onOffZones still emits a selected option for zone 7');
  assert(optionsHtml.indexOf('(none)') === -1,
    'the empty-onOffZones fallback to a valueless "(none)" option must not fire when a stored ' +
    'zone_index exists -- that is exactly the value that used to get silently dropped');

  const params = ctx.ooRulesToParams();
  const asMap = {};
  params.forEach(function (p) { const i = p.indexOf('='); asMap[p.slice(0, i)] = p.slice(i + 1); });
  assert(asMap.rule0_zone === '7',
    'DEFECT 1: rule0_zone must still be 7, not empty, even with zero on/off zones on the board');

  // refreshOoUi() must not have wiped the row: it must still exist and still
  // carry the flagged zone after a re-render pass (the exact "any + Add
  // segment wipes loaded rules" regression named in the review).
  ctx.refreshOoUi();
  const rows = ctx.document.querySelectorAll('#oorules .oorule');
  assert(rows.length === 1,
    'DEFECT 1: refreshOoUi() with zero on/off zones must not clear #oorules -- the row must survive');
  const zsel = rows[0].querySelector('.oo-zone');
  assert(zsel.value === '7',
    'DEFECT 1: the surviving row still shows the real stored zone (7) after refreshOoUi()');
})();

// ---------------------------------------------------------------------------
// DEFECT 2 (Opus review): other on/off zones DO exist, but not the one this
// rule was saved against (a stale zone_index -- retyped or deleted). Before
// the fix this either sent empty (dropped) or silently fell back to the
// browser's first-option default (retargeted to a different, wrong device).
// The fix must flag it as an orphan option (never silently retarget) and the
// save handler must refuse via ooHasStaleZoneRow().
// ---------------------------------------------------------------------------
(function testStaleZoneAmongOthersIsFlaggedAndRefusesSave() {
  const zones = [{ index: 1, name: 'Damper' }, { index: 2, name: 'Vent' }];
  const rule = {
    zone: 9 /* not among zones above */, segment: 0, enable: 1, phase_mask: 0,
    direction_mask: 0, temp_source: 0, temp_cmp: 0, temp_c: 0,
    time_start_s: 0, time_stop_s: 0, invert: 0,
  };
  const ctx = buildContext([rule], zones, 1);

  const optionsHtml = ctx.ooZoneOptionsHtml(9);
  assert(optionsHtml.indexOf('value="9" selected') !== -1,
    'DEFECT 2: a stale zone_index (9) not among onOffZones still gets a selected orphan option');
  assert(/no longer an on\/off zone/.test(optionsHtml),
    'DEFECT 2: the orphan option is visibly flagged so the operator notices, not silently kept');
  assert(/data-oo-stale="1"/.test(optionsHtml),
    'the orphan option also carries data-oo-stale="1" -- the attribute ooHasStaleZoneRow() actually ' +
    'checks (Opus review N6), independent of the human-readable label text');

  const params = ctx.ooRulesToParams();
  const asMap = {};
  params.forEach(function (p) { const i = p.indexOf('='); asMap[p.slice(0, i)] = p.slice(i + 1); });
  assert(asMap.rule0_zone === '9',
    'DEFECT 2: rule0_zone must round-trip as the real stored value (9), never silently retargeted ' +
    'to the first real on/off zone (1) by a browser <select> default');

  assert(ctx.ooHasStaleZoneRow() === true,
    'DEFECT 2: ooHasStaleZoneRow() must detect the flagged row so save is refused client-side');
})();

// ---------------------------------------------------------------------------
// Sanity: a rule whose zone_index IS among onOffZones must never be flagged
// stale (no false positive that would block an ordinary, valid save).
// ---------------------------------------------------------------------------
(function testValidZoneIsNeverFlaggedStale() {
  const zones = [{ index: 1, name: 'Damper' }, { index: 2, name: 'Vent' }];
  const rule = {
    zone: 2, segment: 0, enable: 1, phase_mask: 0, direction_mask: 0,
    temp_source: 0, temp_cmp: 0, temp_c: 0, time_start_s: 0, time_stop_s: 0, invert: 0,
  };
  const ctx = buildContext([rule], zones, 1);
  const optionsHtml = ctx.ooZoneOptionsHtml(2);
  assert(!/no longer an on\/off zone/.test(optionsHtml),
    'a rule targeting a real, still-existing on/off zone is never flagged as an orphan');
  assert(ctx.ooHasStaleZoneRow() === false,
    'ooHasStaleZoneRow() is false when every row targets a real on/off zone -- no false-positive refusal');
})();

// ---------------------------------------------------------------------------
// Opus review N6: ooHasStaleZoneRow() must key off data-oo-stale="1", not the
// orphan option's label text, so a copy-editing change to the label alone
// can never silently disable the save-refusal guard. Simulate exactly that:
// a row whose selected option's label happens to match the old wording but
// carries no data-oo-stale attribute (i.e. a real, non-orphan option that
// coincidentally reused the phrase) must NOT be treated as stale.
// ---------------------------------------------------------------------------
(function testLabelOnlyMatchWithoutAttributeIsNotFlaggedStale() {
  const zones = [{ index: 1, name: 'no longer an on/off zone' }];
  const rule = {
    zone: 1, segment: 0, enable: 1, phase_mask: 0, direction_mask: 0,
    temp_source: 0, temp_cmp: 0, temp_c: 0, time_start_s: 0, time_stop_s: 0, invert: 0,
  };
  const ctx = buildContext([rule], zones, 1);
  const optionsHtml = ctx.ooZoneOptionsHtml(1);
  assert(optionsHtml.indexOf('data-oo-stale="1"') === -1,
    'a real, non-orphan option never carries data-oo-stale="1", even if its label text happens to ' +
    'match the orphan wording');
  assert(ctx.ooHasStaleZoneRow() === false,
    'ooHasStaleZoneRow() must not be fooled by label text alone -- a real zone whose NAME coincidentally ' +
    'reads "no longer an on/off zone" must never trip the stale guard');
})();

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
process.exit(0);
