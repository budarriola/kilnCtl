/* Node-only test for profiles_page.html's per-row action-icon layout
 * (2026-09-19 regression, owner screenshot at ~1216px): the four per-row
 * icons (favorite/export/edit/delete) were laid out with `.pi-acts { float:
 * right; }` and NO clearfix anywhere on the page. A <details>/<summary>
 * row's own box does not grow to contain a floated child unless something
 * clears it, so each row stayed as short as its text line while the
 * floated icon group hung off the bottom-right into the NEXT row, and each
 * row's own float then laid out beside whatever of the previous row's
 * leftover float area intruded into its line box -- shifting the icon
 * group progressively further left, row over row, exactly the "staircase"
 * the owner described (buttons at a different x offset on every row,
 * cycling back after several rows once the intrusion pattern repeated).
 * This was fixed by replacing the float with one real flex row (.pi-row,
 * a child of <summary>) so the label takes the flexible share
 * (flex:1 1 auto + min-width:0, so it ellipsizes rather than wrapping
 * under the icons -- the "cpl_z1" three-line wrap in the same screenshot)
 * and the actions group is simply the row's last flex item, landing flush
 * at the row's true right edge on every row with no float involved at all.
 *
 * This test extracts buildProfileItem()/profileActionsGroup()/
 * favToggleBtn()/iconBtn()/selectionCheckbox() VERBATIM from the page (same
 * discipline as test_zones_page_visibility_gates.js/test_zones_type_toggle.js)
 * and runs them against a tiny fake DOM, so it cannot pass against a copy
 * that has drifted from what ships. It checks:
 *   1. A row's real content is ONE child of <summary> (the .pi-row span),
 *      not several children appended straight to <summary> -- the shape a
 *      float-based layout needs and a flex-row layout does not.
 *   2. .pi-acts is the LAST child of .pi-row (so it is the flex item that
 *      lands at the row's right edge) and .pi-label is flex-first with
 *      min-width:0 + ellipsis, so a long name truncates instead of pushing
 *      icons around or wrapping under them.
 *   3. Two rows built back to back are DOM SIBLINGS under a shared parent,
 *      not nested one inside the other (ruling out the alternative
 *      "accumulating nesting" hypothesis for the staircase).
 *   4. The shipped page's own CSS never re-introduces `float` on `.pi-acts`,
 *      and `.pi-row` is declared as a real flex container.
 *
 * Run: node firmware/KilnFW/App/test/test_profiles_page_row_layout.js
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
// Part 1: the raw CSS never re-introduces the uncleared float.
// ---------------------------------------------------------------------------
const CSS_START = SRC.indexOf('<style>');
const CSS_END = SRC.indexOf('</style>');
if (CSS_START === -1 || CSS_END === -1) throw new Error('could not find <style> block');
const CSS = SRC.slice(CSS_START, CSS_END);

const PI_ACTS_RULE = /\.pi-acts\s*\{[^}]*\}/.exec(CSS);
assert(!!PI_ACTS_RULE, 'sanity: .pi-acts rule found in CSS');
assert(PI_ACTS_RULE && !/float\s*:/.test(PI_ACTS_RULE[0]),
  '.pi-acts no longer uses float (the uncleared-float staircase bug)');

const PI_ROW_RULE = /\.pi-row\s*\{[^}]*\}/.exec(CSS);
assert(!!PI_ROW_RULE, '.pi-row rule exists');
assert(PI_ROW_RULE && /display:\s*inline-flex/.test(PI_ROW_RULE[0]),
  '.pi-row is inline-flex (2026-09-19: plain `display: flex` is a block box, which ' +
  'gets its own anonymous line above <summary>\'s disclosure-marker line -- the ' +
  '48px-per-row summary-height regression; inline-flex shares the marker\'s line ' +
  'instead, so summary height matches row height)');

const PI_LABEL_RULE = /\.pi-label\s*\{[^}]*\}/.exec(CSS);
assert(!!PI_LABEL_RULE, '.pi-label rule exists');
assert(PI_LABEL_RULE && /min-width:\s*0/.test(PI_LABEL_RULE[0]),
  '.pi-label has min-width:0 (required for ellipsis to actually apply in a flex row)');
assert(PI_LABEL_RULE && /text-overflow:\s*ellipsis/.test(PI_LABEL_RULE[0]),
  '.pi-label ellipsizes long names instead of wrapping under the icons');

// ---------------------------------------------------------------------------
// Part 2: extract the real row-building functions, verbatim, and run them.
// ---------------------------------------------------------------------------
const CODE =
  extractRange('function iconBtn(glyph, name, extraClass) {', '}') + '\n' +
  extractRange('function favToggleBtn(p) {', '}') + '\n' +
  extractRange('function profileActionsGroup(p) {', '}') + '\n' +
  extractRange('function selectionCheckbox(p) {', '}') + '\n' +
  extractRange('function buildProfileItem(p, selectable) {', '}');

assert(CODE.indexOf("g.className = 'pi-acts';") !== -1,
  'sanity: extracted range includes the .pi-acts action group');
assert(CODE.indexOf("row.className = 'pi-row';") !== -1,
  'sanity: extracted range includes the .pi-row wrapper');

// ---------------------------------------------------------------------------
// Minimal fake DOM: just enough of the Element API for these functions to
// run to completion without touching a real browser or jsdom (no jsdom
// dependency in this repo -- same approach as test_zones_type_toggle.js).
// ---------------------------------------------------------------------------
function makeFakeDom() {
  class FakeClassList {
    constructor(el) { this.el = el; }
    add(c) { const s = new Set(this.el._className.split(/\s+/).filter(Boolean)); s.add(c); this.el._className = Array.from(s).join(' '); }
  }
  class FakeElement {
    constructor(tag) {
      this.tagName = String(tag).toUpperCase();
      this._className = '';
      this.children = [];
      this.parentNode = null;
      this._text = '';
      this._html = '';
      this._attrs = {};
      this._listeners = {};
    }
    get className() { return this._className; }
    set className(v) { this._className = v; }
    get classList() { return new FakeClassList(this); }
    appendChild(child) { child.parentNode = this; this.children.push(child); return child; }
    setAttribute(k, v) { this._attrs[k] = v; }
    getAttribute(k) { return this._attrs[k]; }
    addEventListener(type, fn) { (this._listeners[type] = this._listeners[type] || []).push(fn); }
    set textContent(v) { this._text = v; this.children = []; }
    get textContent() { return this._text; }
    set innerHTML(v) { this._html = v; this.children = []; this._text = ''; }
    get innerHTML() { return this._html; }
  }
  const document = { createElement: (tag) => new FakeElement(tag) };
  return { document, FakeElement };
}

const { document } = makeFakeDom();

const context = {
  document: document,
  console: console,
  window: { kcEscapeHtml: (s) => String(s) },
  // Globals buildProfileItem()'s call graph reads at CONSTRUCTION time
  // (never at click time, so no fetch/kcConfirm/etc. stub is needed):
  selectMode: null,           // selectionCheckbox() short-circuits to null
  selectedIds: {},
  favoriteIds: [],            // isFav() below always reports false
  isFav: function (id) { return false; },
  isMarked: function (v) { return v === 'too_fast' || v === 'unreachable'; },
  fzMark: function () {},
  FZ_LABEL: { too_fast: 'too fast', unreachable: 'unreachable' },
  segmentTableHtml: function () { return '<table></table>'; },
};
vm.createContext(context);
vm.runInContext(CODE, context);

const profile = {
  id: 42, name: 'A Very Long Profile Name That Should Ellipsize Not Wrap',
  builtin: false, zone_mask: 0x7, segment_count: 3, feasibility: 'unknown', segments: null,
};

const item1 = context.buildProfileItem(profile, true);
const item2 = context.buildProfileItem(Object.assign({}, profile, { id: 43, name: 'cpl_z1' }), true);

// ---- Check 1: one real content child of <summary>, not several floats ----
assert(item1.tagName === 'DETAILS', 'buildProfileItem returns a <details>');
const summary = item1.children.find((c) => c.tagName === 'SUMMARY');
assert(!!summary, 'row has a <summary>');
assert(summary.children.length === 1, 'summary has exactly one real child (the .pi-row wrapper), not several float-laid-out siblings');
const row = summary.children[0];
assert(row.className === 'pi-row', "summary's one child is the .pi-row flex wrapper");

// ---- Check 2: label is flex-first (ellipsizes), actions is flex-last (lands at the right edge) ----
const label = row.children.find((c) => c.className === 'pi-label');
assert(!!label, 'row contains a .pi-label element');
assert(label && label.textContent.indexOf(profile.name) !== -1, 'label carries the real profile name');
const actsIdx = row.children.findIndex((c) => c.className === 'pi-acts');
const labelIdx = row.children.indexOf(label);
assert(actsIdx !== -1, 'row contains the .pi-acts action group');
assert(actsIdx === row.children.length - 1, '.pi-acts is the LAST flex item in the row (right edge on every row, not floated/staggered)');
assert(labelIdx < actsIdx, '.pi-label precedes .pi-acts (grows to fill space, pushing icons flush right)');

// ---- Check 3: two rows are siblings, never nested ----
const container = document.createElement('div');
container.appendChild(item1);
container.appendChild(item2);
assert(container.children.length === 2, 'two rows appended to a shared container both land as its direct children');
assert(item1.children.indexOf(item2) === -1 && item2.children.indexOf(item1) === -1,
  'the two rows are siblings, not nested one inside the other');
assert(item1.parentNode === container && item2.parentNode === container,
  'both rows share the same parent (ruling out the accumulating-nesting hypothesis for the staircase)');

// ---- Check 4: selectMode 'export' puts the checkbox wrapper first in .pi-row ----
context.selectMode = 'export';
const item3 = context.buildProfileItem(Object.assign({}, profile, { id: 44, name: 'exp_row' }), true);
const summary3 = item3.children.find((c) => c.tagName === 'SUMMARY');
const row3 = summary3.children[0];
assert(row3.children.length > 0 && row3.children[0].className === 'sel-cb-wrap',
  "with selectMode 'export', .sel-cb-wrap is children[0] of .pi-row (checkbox leads the row)");
context.selectMode = null;

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join('; '));
  process.exit(1);
}
process.exit(0);
