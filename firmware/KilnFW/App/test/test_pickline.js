/* Node-only test for _drivers_layout.js pickLine (REVIEW_JSFX1 LOW-1/LOW-2).
 * Run: node firmware/KilnFW/App/test/test_pickline.js
 */
'use strict';
const { pickLine } = require('./_drivers_layout.js');

let failed = 0;
function check(ok, label) {
  if (ok) console.log('PASS: ' + label);
  else { failed++; console.log('FAIL: ' + label); }
}
function throwsWith(fn, needle, label) {
  try { fn(); check(false, label + ' (did not throw)'); }
  catch (e) { check(String(e.message).indexOf(needle) >= 0, label + ' [' + e.message + ']'); }
}

const lines = [
  'function kgOk(x) { return x; }\r',
  '  function kgIndented(x) { return x; }',
  'function kgSplit(x) {',
  '  return x;',
  '}',
  'function kgNoBrace(x) => x;',
];
check(pickLine(lines, 'function kgOk(') === 'function kgOk(x) { return x; }', 'complete one-line helper returned, CR stripped');
throwsWith(() => pickLine(lines, 'function kgMissing('), 'function kgMissing(', 'missing helper names itself');
throwsWith(() => pickLine(lines, 'function kgIndented('), 'function kgIndented(', 'indented helper is not found at column 0');
throwsWith(() => pickLine(lines, 'function kgSplit('), 'no longer a one-line function', 'multi-line helper names the problem');
throwsWith(() => pickLine(lines, 'function kgNoBrace('), 'no longer a one-line function', 'brace-less line rejected');

console.log((failed ? 'FAILED: ' : 'ALL PASS: ') + failed + ' failed');
process.exit(failed ? 1 : 0);
