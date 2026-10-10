/* Node-only test for the F4/MED-1 refusal UX fixes (host-allow-list review):
 * app.js kcHostRefusalText + its fetch-wrapper hook, zones_page.html blank-optional
 * omission and Save-button disable, setup_wizard_page.html real read-back text.
 * Extracts PRODUCTION text from the pages.
 * Run: node firmware/KilnFW/App/test/test_host_refusal_pages.js */
'use strict';
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const fs = require('fs');
const dir = resolveDriversDir(__dirname);
const rd = function (f) { return fs.readFileSync(resolveDriverFile(dir, f), 'utf8'); };
const APP = rd('app.js'), ZONES = rd('zones_page.html'), WIZ = rd('setup_wizard_page.html');

let passed = 0, failed = 0;
function assert(c, l) { if (c) { passed++; console.log('PASS: ' + l); } else { failed++; console.log('FAIL: ' + l); } }

// ---- app.js ----
let m = APP.match(/window\.kcHostRefusalText = function \(body\) \{[\s\S]*?\r?\n  \};/);
assert(!!m, 'found kcHostRefusalText in app.js');
const win = {};
new Function('window', m[0])(win);
const t1 = win.kcHostRefusalText({ error: 'bad_host' });
assert(/IP address or <name>\.local/.test(t1), 'bad_host maps to the open-by-IP message');
assert(win.kcHostRefusalText({ error: 'cross_origin' }) === t1, 'cross_origin maps to the same message');
assert(win.kcHostRefusalText({ error: 'insufficient_role' }) === null && win.kcHostRefusalText(null) === null,
  'other bodies are not mapped');
assert(/resp\.status === 403[\s\S]{0,200}kcHostRefusalText/.test(APP), 'fetch wrapper calls the mapper on a 403');
assert(!/bench/i.test(t1), 'no bench text in the message');

// ---- zones_page.html ----
m = ZONES.match(/var ZONE_OPTIONAL_KEY_RE = [^\n]*\n/);
const m2 = ZONES.match(/function omitBlankOptionalParams\(params\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!m && !!m2, 'found omitBlankOptionalParams in zones_page.html');
const omit = new Function(m[0] + m2[0] + '; return omitBlankOptionalParams;')();
const out = omit(['z0_driftperiod=', 'z0_kp=', 'z1_debounce=  ', 'z0_driftperiod=30', 'z2_coupling_c1=', 'z0_name=',
  'z0_k=', 'thermo_count=']);
assert(out.join(',') === 'z0_kp=,z0_driftperiod=30,z0_name=,thermo_count=',
  'blank optional fields dropped, required blanks and set values kept: ' + out.join(','));
assert(/saveBtnEl\.disabled = true;[\s\S]*?omitBlankOptionalParams\(params\)[\s\S]*?fetch\('\/api\/zones'/.test(ZONES),
  'Save disables the button and filters params before the POST');
assert(/\.then\(function \(r\) \{\s*saveBtnEl\.disabled = false;/.test(ZONES), 'button re-enabled on response');
assert(/\.catch\(function \(e\) \{\s*saveBtnEl\.disabled = false;/.test(ZONES), 'button re-enabled on failure');

// ---- setup_wizard_page.html ----
m = WIZ.match(/function wizardLoginReadBackText\(wantEnable, cfg\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!m, 'found wizardLoginReadBackText');
const rb = new Function(m[0] + '; return wizardLoginReadBackText;')();
assert(/is now ON/.test(rb(true, { web_enabled: true })), 'read back ON');
assert(/is now off\./.test(rb(false, { web_enabled: false })), 'read back off');
assert(/differs/.test(rb(true, { web_enabled: false })), 'mismatch is called out');
assert(/could not be read back/.test(rb(true, null)) && !/is now/.test(rb(true, null)), 'unreadable: only sent claimed');
assert(/fetch\('\/api\/auth\/config'\)\.then\(function \(rb\)/.test(WIZ), 'wizard performs a real GET /api/auth/config after the POST');

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
