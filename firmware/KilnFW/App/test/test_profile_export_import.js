/* Node-only test harness for profiles_page.html's single-profile export/
 * import feature (UI_PLAN.md "Open: settings import/export (partial) and
 * profile import/export", item 2): the pure client-side helpers
 * profileExportUrl() and validateImportJson(), extracted verbatim (same
 * pattern as test_zones_inheritance.js).
 *
 * The server-side handlers (profiles_export_http.c) are exercised
 * separately -- there is no host-test harness for httpd handlers in this
 * split (matches the note in TODO.md 0.5 that no host-test harness reaches
 * backup_http.c/wifi_prov.c either); this file covers exactly the part that
 * would otherwise ship with zero test coverage, the client-side file
 * validation that decides whether an upload is even attempted.
 *
 * Run: node firmware/KilnFW/App/test/test_profile_export_import.js
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

const HELPERS_SRC =
  extractRange('function profileExportUrl(id) {', '}') + '\n' +
  extractRange('function validateImportJson(text) {', '}');
assert(HELPERS_SRC.indexOf('function validateImportJson') !== -1,
  'sanity: extracted range includes validateImportJson');
assert(HELPERS_SRC.indexOf('JSON.parse') !== -1,
  'sanity: extracted range includes the JSON.parse call');

function loadHelpers() {
  const ctx = vm.createContext({ console, JSON, Array });
  new vm.Script(HELPERS_SRC, { filename: 'profiles_page.html (export/import helpers slice)' }).runInContext(ctx);
  return ctx;
}

// ---------------------------------------------------------------------------
// profileExportUrl()
// ---------------------------------------------------------------------------
(function testExportUrlBuildsIdQuery() {
  const ctx = loadHelpers();
  assert(ctx.profileExportUrl(3) === '/api/profile/export?id=3',
    'profileExportUrl(3) builds the expected query string');
  assert(ctx.profileExportUrl(0) === '/api/profile/export?id=0',
    'profileExportUrl(0) does not drop the falsy-but-valid id 0');
})();

// ---------------------------------------------------------------------------
// validateImportJson() -- the gate between a file-picker selection and the
// board round trip. Every rejection path must be reachable and distinct
// (an idealized-input bug would only ever exercise the success path).
// ---------------------------------------------------------------------------
(function testValidateRejectsMalformedJson() {
  const ctx = loadHelpers();
  const result = ctx.validateImportJson('{not json');
  assert(result.ok === false, 'malformed JSON is rejected');
  assert(typeof result.error === 'string' && result.error.length > 0,
    'malformed JSON rejection carries a human-readable error');
})();

(function testValidateRejectsNonObjectJson() {
  const ctx = loadHelpers();
  assert(ctx.validateImportJson('42').ok === false, 'a bare number is rejected');
  assert(ctx.validateImportJson('null').ok === false, 'JSON null is rejected');
  assert(ctx.validateImportJson('[1,2,3]').ok === false, 'a JSON array is rejected (not treated as an object)');
})();

(function testValidateRejectsMissingNameOrSegments() {
  const ctx = loadHelpers();
  const noName = ctx.validateImportJson(JSON.stringify({ segments: [] }));
  assert(noName.ok === false, 'a profile object with no "name" is rejected');
  const noSegments = ctx.validateImportJson(JSON.stringify({ name: 'Cone 6' }));
  assert(noSegments.ok === false, 'a profile object with no "segments" array is rejected');
  const segmentsNotArray = ctx.validateImportJson(JSON.stringify({ name: 'Cone 6', segments: 'oops' }));
  assert(segmentsNotArray.ok === false, '"segments" that is not an array is rejected');
})();

(function testValidateAcceptsWellFormedExport() {
  const ctx = loadHelpers();
  const exported = {
    kind: 'kilnctl_profile', version: 1, name: 'Bisque', zone_mask: 3,
    segments: [
      { seg_kind: 0, target_c: 999, ramp_c_per_hr: 60, dwell_min: 30,
        io_target: 0, io_state: 0, io_blocking: 0, io_leave_on_at_end: 0 },
    ],
  };
  const result = ctx.validateImportJson(JSON.stringify(exported));
  assert(result.ok === true, 'a well-formed export round-trips through validateImportJson as ok');
  assert(result.data && result.data.name === 'Bisque', 'the parsed data carries the original name through');
  assert(result.data.segments.length === 1, 'the parsed data carries the segment array through');
})();

// A profile with zero segments is still structurally valid JSON-wise here --
// the REAL "at least one segment" rule is enforced server-side by
// profiles_http_save() (single source of truth, same as every other range
// check this editor defers to the board for). This client-side gate only
// screens out obviously-wrong files, not every field-level rule.
(function testValidateAcceptsEmptySegmentsArray() {
  const ctx = loadHelpers();
  const result = ctx.validateImportJson(JSON.stringify({ name: 'Empty', segments: [] }));
  assert(result.ok === true,
    'an empty segments array is left for the server to refuse, not rejected client-side');
})();

// ---------------------------------------------------------------------------
console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exitCode = 1;
}
