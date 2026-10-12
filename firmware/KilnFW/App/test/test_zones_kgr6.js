/* Node-only test for zones_page.html's kgR6 / kgRawAttrs / kgPost (display
 * rounding to 6 significant digits that must not make an unedited save lossy).
 * Run: node firmware/KilnFW/App/test/test_zones_kgr6.js
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const { resolveDriversDir, resolveDriverFile, pickLine } = require('./_drivers_layout.js');
const SRC = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'zones_page.html'), 'utf8');
const lines = SRC.split('\n').map((l) => l.replace(/\r$/, ''));
const pick = (prefix) => pickLine(lines, prefix);
const ctx = {};
vm.createContext(ctx);
vm.runInContext([pick('function kgR6('), pick('function kgRawAttrs('), pick('function kgPost(')].join('\n'), ctx);

let passed = 0, failed = 0;
function eq(got, want, label) {
  if (got === want) { passed++; console.log('PASS: ' + label); }
  else { failed++; console.log('FAIL: ' + label + ' got ' + JSON.stringify(got) + ' want ' + JSON.stringify(want)); }
}
const el = (attrs, value) => ({ value: value, getAttribute: (k) => (k in attrs ? attrs[k] : null) });

eq(ctx.kgR6(0.100000024), 0.1, 'rounds float noise');
eq(ctx.kgR6(1234.5678), 1234.57, '6 significant digits');
eq(ctx.kgR6(undefined), '', 'undefined renders empty');
eq(ctx.kgR6(null), '', 'null renders empty');
eq(ctx.kgR6(NaN), '', 'NaN renders empty');
eq(ctx.kgR6(0), 0, 'zero stays 0');
eq(ctx.kgRawAttrs(undefined), '', 'no attrs for undefined');
eq(ctx.kgRawAttrs(1234.5678), ' data-raw="1234.5678" data-disp="1234.57"', 'raw+disp attrs');
eq(ctx.kgPost(el({ 'data-raw': '1234.5678', 'data-disp': '1234.57' }, '1234.57')), '1234.5678', 'unedited posts raw');
eq(ctx.kgPost(el({ 'data-raw': '1234.5678', 'data-disp': '1234.57' }, '1500')), '1500', 'edited posts value');
eq(ctx.kgPost(el({}, '7')), '7', 'no data-raw posts value');

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
