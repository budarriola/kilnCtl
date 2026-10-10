/* Executes app.js's real window.kcUnit IIFE and main_page.html's fmtDeltaC()
 * and pins the C/F conversion rules: absolute readings get the +32 offset,
 * deltas must not, null/NaN pass through, a garbage or missing temp_unit
 * leaves Celsius, set() posts the right form body.
 * Run: node firmware/KilnFW/App/test/test_kcunit_conversion.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const dir = resolveDriversDir(__dirname);
const APP = fs.readFileSync(resolveDriverFile(dir, 'app.js'), 'utf8');
const MAIN = fs.readFileSync(resolveDriverFile(dir, 'main_page.html'), 'utf8');

let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }
function near(a, b) { return Math.abs(a - b) < 1e-9; }

const start = APP.indexOf('window.kcUnit = (function () {');
const end = APP.indexOf('\n  })();', start);
ok(start > 0 && end > start, 'sanity: found the kcUnit IIFE in app.js');

const posts = [];
const events = [];
let fetchRejects = false;
const sb = {
  Event: function (t) { this.type = t; },
  fetch: (u, init) => { posts.push({ u, init }); return fetchRejects ? Promise.reject(new Error('net')) : Promise.resolve({}); },
  console: { warn() {}, log() {} },
};
sb.window = sb;
sb.dispatchEvent = (e) => events.push(e.type);
vm.createContext(sb);
vm.runInContext(APP.slice(start, end + '\n  })();'.length), sb);
const u = sb.kcUnit;

ok(u.get() === 'c', 'default unit is Celsius');
ok(u.toDisplay(100) === 100 && u.fmt(100) === '100.0 °C', 'Celsius: identity and "100.0 °C"');
ok(u.toDisplay(null) === null && u.toDisplay(undefined) === null && u.toDisplay(NaN) === null, 'toDisplay passes null/undefined/NaN through as null');
ok(u.fmt(null) === 'n/a' && u.fmt(NaN) === 'n/a', 'fmt of a missing reading is "n/a", never "NaN" or 32');

u.updateFromStatus({ temp_unit: 'F' });
ok(u.get() === 'f' && events.indexOf('kcunitchange') !== -1, 'status temp_unit "F" switches to Fahrenheit (case-insensitive) and fires kcunitchange');
ok(near(u.toDisplay(100), 212) && near(u.toDisplay(0), 32) && near(u.toDisplay(-40), -40) && near(u.toDisplay(1060), 1940),
  'Fahrenheit: 100C=212F, 0C=32F, -40C=-40F, 1060C=1940F (absolute offset applied)');
ok(u.fmt(1000, 0) === '1832 °F', 'fmt(1000C, 0 decimals) = "1832 °F" (got "' + u.fmt(1000, 0) + '")');
ok(u.toDisplay(0) !== 0 * 9 / 5, 'an absolute 0 C is not shown as 0 F');

const n = events.length;
u.updateFromStatus({ temp_unit: 'F' });
ok(events.length === n, 'unchanged unit does not re-fire kcunitchange');
u.updateFromStatus({});
u.updateFromStatus(null);
u.updateFromStatus({ temp_unit: 7 });
ok(u.get() === 'f', 'status without a string temp_unit leaves the cached unit alone');
u.updateFromStatus({ temp_unit: 'K' });
ok(u.get() === 'c', 'an unknown unit string falls back to Celsius, not Kelvin or a throw');

fetchRejects = true;
u.set('f');
ok(u.get() === 'f' && events[events.length - 1] === 'kcunitchange', 'set("f") updates the cache optimistically and fires the event');
ok(posts[0].u === '/api/unit_pref' && posts[0].init.body === 'unit=F', 'set("f") POSTs unit=F');
u.set('garbage');
ok(u.get() === 'c' && posts[1].init.body === 'unit=C', 'set(garbage) coerces to Celsius and POSTs unit=C');

// main_page.html fmtDeltaC: a delta must scale by 9/5 with no +32.
const m = MAIN.match(/function fmtDeltaC\(c\) \{[\s\S]*?\n\}/);
ok(!!m, 'sanity: found fmtDeltaC in main_page.html');
const dsb = { window: { kcUnit: u } };
vm.createContext(dsb);
vm.runInContext(m[0] + '\nthis.fmtDeltaC = fmtDeltaC;', dsb);
u.set('f');
ok(dsb.fmtDeltaC(10) === '+18.0 °F', 'delta of +10 C shows +18.0 F, not +50 (got "' + dsb.fmtDeltaC(10) + '")');
ok(dsb.fmtDeltaC(-5) === '-9.0 °F', 'delta of -5 C shows -9.0 F with a minus sign');
ok(dsb.fmtDeltaC(0) === '+0.0 °F', 'zero delta shows +0.0 F, not 32');
u.set('c');
ok(dsb.fmtDeltaC(10) === '+10.0 °C', 'Celsius delta is unscaled');
ok(dsb.fmtDeltaC(NaN) === 'n/a', 'NaN delta is n/a');

console.log(failed ? failed + ' FAILED' : 'all passed');
process.exit(failed ? 1 : 0);
