/* Node-only test for live_profile_page.html (docs/LIVE_PROFILE_EDIT_PLAN.md,
 * sections 2, 8 and 10): the client-side editability rule that mirrors the
 * executor's `editable_from_segment` boundary, and the form serialisation
 * that must round-trip a frozen/read-only segment unchanged rather than
 * dropping it from the submitted array.
 *
 * segmentEditableState(), segFieldsHtml(), buildSegmentRow() and
 * collectSegmentParams() are extracted VERBATIM from the page (same
 * discipline test_profiles_page_filter.js and test_zones_page_visibility_gates.js
 * use), so this cannot pass against a copy that has drifted from what
 * ships.
 *
 * Checks:
 *   1. segmentEditableState(): index < editable_from_segment -> 'frozen';
 *      index === editable_from_segment -> 'current'; index >
 *      editable_from_segment -> 'future'; a missing/negative
 *      editable_from_segment (nothing running yet) makes every index
 *      'future'.
 *   2. segFieldsHtml(): target/ramp/dwell inputs are `disabled` for a
 *      non-editable segment and NOT disabled for an editable one.
 *   3. buildSegmentRow(): a relay/IO segment (seg_kind truthy) never emits
 *      numeric input fields at all, regardless of editable state -- this
 *      page's minimal editor cannot express IO fields, so it must not
 *      offer disabled-looking inputs that imply it can.
 *   4. collectSegmentParams(): a frozen row (no rendered target/ramp/dwell
 *      inputs) round-trips its ORIGINAL values from the fetched working
 *      copy rather than vanishing from the submitted seg_count/fields; an
 *      editable row's live input values are read back instead of the
 *      original ones.
 *
 * Run: node firmware/KilnFW/App/test/test_live_profile_page.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'live_profile_page.html');
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

const CODE =
  extractRange('function escapeHtml(s) {', '}') + '\n' +
  extractRange('function segmentEditableState(index, editableFromSegment) {', '}') + '\n' +
  extractRange('function segFieldsHtml(seg, editable) {', '}') + '\n' +
  extractRange('function buildSegmentRow(seg, index, editableFromSegment) {', '}') + '\n' +
  extractRange('function collectSegmentParams(originalSegments) {', '}');

assert(CODE.indexOf('function segmentEditableState(') !== -1,
  'sanity: extracted range includes segmentEditableState');
assert(CODE.indexOf('function collectSegmentParams(') !== -1,
  'sanity: extracted range includes collectSegmentParams');

// ---------------------------------------------------------------------------
// Minimal fake DOM -- enough for buildSegmentRow() (createElement, className,
// dataset, innerHTML-as-opaque-string) and for collectSegmentParams()'s
// getElementById('segments').querySelectorAll('.seg') lookup, which this
// test drives with hand-built mock rows rather than parsing HTML strings,
// since collectSegmentParams() never inspects its own rendered markup --
// only .dataset and .querySelector() on rows the test controls directly.
// ---------------------------------------------------------------------------
function makeFakeElement(tag) {
  return {
    tagName: String(tag).toUpperCase(),
    _className: '',
    dataset: {},
    children: [],
    get className() { return this._className; },
    set className(v) { this._className = v; },
    _html: '',
    get innerHTML() { return this._html; },
    set innerHTML(v) { this._html = v; },
    appendChild(child) { this.children.push(child); return child; },
  };
}

function makeContext(rows) {
  const segmentsEl = makeFakeElement('div');
  segmentsEl.querySelectorAll = function (sel) {
    if (sel === '.seg') return rows || [];
    return [];
  };
  const document = {
    createElement: (tag) => makeFakeElement(tag),
    getElementById: (id) => (id === 'segments' ? segmentsEl : null),
  };
  const context = {
    document: document,
    window: { kcEscapeHtml: (s) => String(s) },
    console: console,
  };
  vm.createContext(context);
  vm.runInContext(CODE, context);
  return context;
}

function mkRamp(target, ramp, dwell) {
  return { seg_kind: 0, target_c: target, ramp_c_per_hr: ramp, dwell_min: dwell };
}
function mkIo(target, state, blocking, leave, dwell) {
  return {
    seg_kind: 1, io_target: target, io_state: state, io_blocking: blocking,
    io_leave_on_at_end: leave, dwell_min: dwell,
  };
}

// ---------------------------------------------------------------------------
// Part 1: segmentEditableState() boundaries.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext([]);
  assert(ctx.segmentEditableState(0, 2) === 'frozen', 'index 0 < editable_from_segment 2 -> frozen');
  assert(ctx.segmentEditableState(1, 2) === 'frozen', 'index 1 < editable_from_segment 2 -> frozen');
  assert(ctx.segmentEditableState(2, 2) === 'current', 'index === editable_from_segment -> current');
  assert(ctx.segmentEditableState(3, 2) === 'future', 'index > editable_from_segment -> future');
  assert(ctx.segmentEditableState(0, -1) === 'future', 'negative editable_from_segment -> every index future');
  assert(ctx.segmentEditableState(0, undefined) === 'future', 'missing editable_from_segment -> every index future');
  assert(ctx.segmentEditableState(0, 0) === 'current', 'index 0 === editable_from_segment 0 -> current (segment 1 running)');
}

// ---------------------------------------------------------------------------
// Part 2: segFieldsHtml() disables inputs exactly when told to.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext([]);
  const editableHtml = ctx.segFieldsHtml(mkRamp(900, 120, 30), true);
  const frozenHtml = ctx.segFieldsHtml(mkRamp(900, 120, 30), false);
  assert(editableHtml.indexOf('disabled') === -1, 'editable segment fields carry no disabled attribute');
  assert((frozenHtml.match(/disabled/g) || []).length === 3,
    'non-editable segment fields are ALL THREE disabled (target, ramp, dwell)');
  assert(editableHtml.indexOf('value="900"') !== -1, 'target value is rendered into the input');
}

// ---------------------------------------------------------------------------
// Part 3: buildSegmentRow() never renders numeric fields for a relay/IO
// segment, regardless of editable state -- it is always presented read-only.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext([]);
  const futureIoRow = ctx.buildSegmentRow(mkIo(1, 1, 1, 0, 10), 5, 2); // future (index 5 > 2)
  const currentIoRow = ctx.buildSegmentRow(mkIo(1, 1, 1, 0, 10), 2, 2); // current (index === 2)
  assert(futureIoRow.innerHTML.indexOf('s-target') === -1,
    'a future relay/IO segment row still has no editable target input');
  assert(currentIoRow.innerHTML.indexOf('s-target') === -1,
    'a current relay/IO segment row still has no editable target input');
  assert(futureIoRow.dataset.kind === '1', 'row dataset records seg_kind 1 for a relay/IO segment');
  assert(futureIoRow.innerHTML.indexOf('Not started yet') !== -1,
    'a future segment badge reads "fully editable" wording');
  assert(currentIoRow.innerHTML.indexOf('Running now') !== -1,
    'a current segment badge reads the "running now" wording');

  const frozenRampRow = ctx.buildSegmentRow(mkRamp(900, 0, 10), 0, 2);
  assert(frozenRampRow.className.indexOf('frozen') !== -1, 'a frozen row carries the frozen CSS class');
  assert(frozenRampRow.innerHTML.indexOf('Already run') !== -1,
    'a frozen segment badge reads the "already run" wording');
}

// ---------------------------------------------------------------------------
// Part 4: collectSegmentParams() -- a frozen row with no rendered inputs
// round-trips its ORIGINAL values; an editable row reads its live inputs.
// ---------------------------------------------------------------------------
{
  // Row 0: frozen ramp segment, no .s-target/.s-ramp/.s-dwell present (mirrors
  // what buildSegmentRow() actually renders for a frozen row: real DOM
  // inputs would exist but be disabled -- querySelector still finds them
  // here to prove the "read the disabled input's value back" path works
  // for a ramp segment sitting at/behind the running index).
  const frozenRow = {
    dataset: { index: '0', kind: '0' },
    querySelector: function (sel) {
      if (sel === '.s-target') return { value: '900' }; // disabled input, unedited value
      if (sel === '.s-ramp') return { value: '0' };
      if (sel === '.s-dwell') return { value: '10' };
      return null;
    },
  };
  // Row 1: relay/IO segment -- no numeric inputs exist at all (buildSegmentRow
  // renders only the read-only paragraph for these), so collectSegmentParams
  // must fall back to the original fetched segment's IO fields.
  const ioRow = {
    dataset: { index: '1', kind: '1' },
    querySelector: function () { return null; },
  };
  // Row 2: future ramp segment, operator has typed a new target.
  const editedRow = {
    dataset: { index: '2', kind: '0' },
    querySelector: function (sel) {
      if (sel === '.s-target') return { value: '1200' };
      if (sel === '.s-ramp') return { value: '60' };
      if (sel === '.s-dwell') return { value: '0' };
      return null;
    },
  };
  const originalSegments = [
    mkRamp(900, 0, 10),
    mkIo(3, 1, 0, 1, 15),
    mkRamp(1000, 50, 0), // pre-edit value -- must NOT appear in the params, 1200 must
  ];
  const ctx = makeContext([frozenRow, ioRow, editedRow]);
  const params = ctx.collectSegmentParams(originalSegments).join('&');

  assert(params.indexOf('seg_count=3') !== -1, 'seg_count reflects all three rows, including the frozen one');
  assert(params.indexOf('seg0_target=900') !== -1, 'frozen row round-trips its original target');
  assert(params.indexOf('seg0_dwell=10') !== -1, 'frozen row round-trips its original dwell');
  assert(params.indexOf('seg1_kind=1') !== -1, 'relay/IO row is submitted as kind 1');
  assert(params.indexOf('seg1_io_target=3') !== -1, 'relay/IO row round-trips its original io_target from the fetched copy');
  assert(params.indexOf('seg1_io_leave_on=1') !== -1, 'relay/IO row round-trips its original io_leave_on_at_end');
  assert(params.indexOf('seg2_target=1200') !== -1, 'editable row submits the operator-edited target, not the original 1000');
  assert(params.indexOf('seg2_ramp=60') !== -1, 'editable row submits the operator-edited ramp');
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join('; '));
  process.exit(1);
}
process.exit(0);
