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
// "selected"/"checked" attributes, and document.getElementById.
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
  matches(sel) {
    if (sel[0] === '.') return this.classes.includes(sel.slice(1));
    return this.tag === sel;
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
// ---------------------------------------------------------------------------
const RULES_SRC =
  extractRange('function ooPhaseChecksHtml(mask) {', '}') + '\n' +
  extractRange('function ooDirChecksHtml(mask) {', '}') + '\n' +
  extractRange('function ooRuleFieldsHtml(r) {', '}') + '\n' +
  extractRange('function ooRulesToParams() {', '}');

assert(RULES_SRC.indexOf('rule\' + i + \'_temp_source') !== -1,
  'sanity: extracted range includes the temp_source field name');

// Build a fake document that supports exactly what ooRulesToParams() and
// ooRuleFieldsHtml()'s helpers need: getElementById('oorules') and
// querySelectorAll('.oorule') on the root, plus the per-row selectors.
function buildContext(rules, onOffZones, segmentCount) {
  var oorulesHtml = '<div id="oorules">' + rules.map(function (r) {
    return '<div class="oorule">' + '__FIELDS__' + '</div>';
  }).join('') + '</div>';

  const sandbox = {
    onOffZones: onOffZones,
    infoIconHtml: function () { return ''; },
    ooZoneOptionsHtml: function (selected) {
      return onOffZones.map(function (z) {
        return '<option value="' + z.index + '"' + (z.index === selected ? ' selected' : '') + '>' + z.name + '</option>';
      }).join('');
    },
    ooSegmentOptionsHtml: function (selected) {
      var out = '';
      for (var i = 0; i < segmentCount; i++) {
        out += '<option value="' + i + '"' + (i === selected ? ' selected' : '') + '>Step ' + (i + 1) + '</option>';
      }
      return out;
    },
  };
  vm.createContext(sandbox);
  vm.runInContext(RULES_SRC, sandbox);

  // Render each rule's real fields markup with the real function, then
  // splice it into the fake document exactly as ooRuleRow() would via
  // innerHTML (minus the "Remove rule" button, irrelevant here).
  var perRuleHtml = rules.map(function (r) { return sandbox.ooRuleFieldsHtml(r); });
  var html = '<div id="oorules">' + perRuleHtml.map(function (h) {
    return '<div class="oorule">' + h + '</div>';
  }).join('') + '</div>';
  const root = parseHtml(html);
  const oorulesEl = root.getElementById('oorules');
  sandbox.document = {
    getElementById: function (id) {
      if (id === 'oorules') return oorulesEl;
      return root.getElementById(id);
    },
  };
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

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
process.exit(0);
