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
let mt = APP.match(/window\.kcHostRefusalFromText = function \(text\) \{[\s\S]*?\r?\n  \};/);
assert(!!mt, 'found kcHostRefusalFromText');
new Function('window', mt[0])(win);
assert(win.kcHostRefusalFromText('{"error":"bad_host"}') === t1 && win.kcHostRefusalFromText('Bad password') === null, 'text variant maps JSON refusal only');
assert(/kcHostRefusalFromText\(text\) \|\| text \|\| 'Login failed/.test(APP), 'login modal uses guidance');
assert((APP.match(/kcHostRefusalFromText\(text\) \|\| kcForgotStatusMessage\(403/g) || []).length === 2 && /forgotBackToStep1\(window\.kcHostRefusalFromText/.test(APP), 'forgot/reset use guidance');
const LOGIN = rd('login_page.html');
assert(!/<script[^>]+src=/.test(LOGIN), 'login page loads no external script, so it must not depend on app.js helpers');
assert(!/window\.kcHostRefusal/.test(LOGIN), 'login page does not reference the app.js-only helper');
const lm = LOGIN.match(/function hostRefusalFromText\(text\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!lm, 'found inline hostRefusalFromText in login_page.html');
const lhr = new Function(lm[0] + '; return hostRefusalFromText;')();
assert(lhr('{"error":"bad_host"}') === t1 && lhr('{"error":"cross_origin"}') === t1, 'login page helper behaves like app.js (same text)');
assert(lhr('Bad password') === null && lhr('{"error":"x"}') === null && lhr('null') === null, 'login page helper maps only refusals');
assert(/hostRefusalFromText\(text\) \|\| text \|\| 'Login failed/.test(LOGIN) && /hostRefusalFromText\(text\) \|\| text \|\| 'Could not set password/.test(LOGIN), 'login page uses the helper');
assert(/xhr\.status === 403[\s\S]{0,120}kcHostRefusalFromText/.test(rd('ota_page.html')), 'stage XHR uses guidance');
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
assert(omit(['z0_coilpower=','z1_hystc= ','z0_minons=','z0_minoffs=','z0_coilpower=5']).join(',') === 'z0_coilpower=5',
  'blank coilpower/hystc/minons/minoffs dropped (keep stored), set value kept');
// BENCHFX9 LOW-1 / BENCHFX10 LOW-2: exhaustive, independent of ZONE_OPTIONAL_KEY_RE. The firmware
// omit-PRESERVES exactly these keys (zones_http_client.ZONE_OMIT_PRESERVED_*), so a blank for one may be
// dropped; every other blank (ct_mask/thermo_mask are omit-DEFAULTED, the rest required) must be KEPT.
const FW_DROPPABLE = ['wrongdirwindow', 'wrongdirrate', 'offsettle', 'runawayrate', 'runawaymargin', 'driftperiod',
  'debounce', 'frozenwindow', 'fuzzy_strength', 'coupling_diag_k_dc', 'easeoffmult', 'approachratecap', 'errorband',
  'rateband', 'progressband', 'coilpower', 'hystc', 'minons', 'minoffs', 'k', 'tau', 'deadtime',
  'coupling_c0', 'coupling_c1', 'coupling_c2'];
const MUST_KEEP = ['ct_mask', 'thermo_mask', 'kp', 'ki', 'kd', 'name', 'relay_mask', 'xzone', 'maxtemp', 'mintemp',
  'cal', 'ramp', 'sanity', 'mode', 'window', 'minon', 'minoff', 'timingprofile', 'zonetype', 'tctype', 'failsafe',
  'settings_source', 'driftperiodx', 'kk', 'coupling_c', 'coupling_cx'];
const gen = [];
for (let z = 0; z < 3; z++) {
  FW_DROPPABLE.concat(MUST_KEEP).forEach(function (k) {
    ['', '  ', '5', '0'].forEach(function (v) { gen.push('z' + z + '_' + k + '=' + v); });
  });
}
['thermo_count=', 'relay_count=', 'relay1_name=', 'expected_generation=', 'tp0_name='].forEach(function (e) { gen.push(e); });
function indepDrops(p) {
  const mm = /^z\d+_(.*?)=(.*)$/.exec(p);
  return !!mm && FW_DROPPABLE.indexOf(mm[1]) >= 0 && mm[2].trim() === '';
}
assert(omit(gen).join('\n') === gen.filter(function (p) { return !indepDrops(p); }).join('\n'),
  'omit() drops exactly the blank omit-preserved keys, over every zone 0..2 and every key');
for (let z = 0; z < 3; z++) {
  assert(omit(['z' + z + '_ct_mask=', 'z' + z + '_thermo_mask=']).length === 2, 'blank ct_mask/thermo_mask kept for zone ' + z);
}
// BENCHFX10 LOW-1: run the REAL tail of the Save handler (from the in-flight marker through the fetch call)
// against a mocked fetch and check the POST body itself. Edits BEFORE this point are pinned in
// test_zone_omit_preserved_drift.py (every use of `params` must be an allowed form).
const sIdx = ZONES.indexOf('  var savedZonesEditSeq');
const eIdx = ZONES.indexOf('}).then(function (r) {', sIdx);
assert(sIdx > 0 && eIdx > sIdx, 'found the Save handler fetch tail');
let posted = null;
const mockFetch = function (url, opts) { posted = { url: url, opts: opts }; return {}; };
const tail = new Function('params', 'current', 'saveBtnEl', 'zonesEditSeq', 'msg', 'fetch', 'omitBlankOptionalParams',
  ZONES.slice(sIdx, eIdx) + '});');
const saveParams = [];
for (let z = 0; z < 3; z++) saveParams.push('z' + z + '_thermo_mask=', 'z' + z + '_ct_mask=', 'z' + z + '_kp=1', 'z' + z + '_driftperiod=');
tail(saveParams.slice(), { generation: 7 }, { disabled: false }, 0, {}, mockFetch, omit);
const bodyParts = posted ? posted.opts.body.split('&') : [];
assert(!!posted && posted.url === '/api/zones' && posted.opts.method === 'POST', 'Save posts to /api/zones');
for (let z = 0; z < 3; z++) {
  assert(bodyParts.indexOf('z' + z + '_ct_mask=') >= 0 && bodyParts.indexOf('z' + z + '_thermo_mask=') >= 0,
    'POST body keeps blank ct_mask/thermo_mask for zone ' + z);
  assert(bodyParts.indexOf('z' + z + '_driftperiod=') < 0 && bodyParts.indexOf('z' + z + '_kp=1') >= 0,
    'POST body drops blank optional, keeps set value for zone ' + z);
}
assert(bodyParts[bodyParts.length - 1] === 'expected_generation=7', 'POST body carries expected_generation');
assert(/saveBtnEl\.disabled = true;[\s\S]*?omitBlankOptionalParams\(params\)[\s\S]*?fetch\('\/api\/zones'/.test(ZONES),
  'Save disables the button and filters params before the POST');
assert(/\.then\(function \(r\) \{\s*if \(saveTimer\) clearTimeout\(saveTimer\);\s*saveBtnEl\.disabled = false;/.test(ZONES), 'button re-enabled on response');
assert(/\.catch\(function \(e\) \{\s*if \(saveTimer\) clearTimeout\(saveTimer\);\s*saveBtnEl\.disabled = false;/.test(ZONES), 'button re-enabled on failure');
assert(/new AbortController\(\)[\s\S]*?setTimeout\(function \(\) \{ ac\.abort\(\); \}, 30000\)[\s\S]*?signal: armSaveTimer\(\)/.test(ZONES), 'save has an AbortController timeout');
const g1 = ZONES.match(/var ZONE_GUARD_BLANK_RE = [^\n]*\n/), g2 = ZONES.match(/function blankGuardKeys\(params\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!g1 && !!g2, 'found blankGuardKeys');
const bg = new Function(g1[0] + g2[0] + '; return blankGuardKeys;')();
assert(bg(['z0_driftperiod=', 'z1_runawaymargin= ', 'z0_kp=', 'z0_k=', 'z0_debounce=5']).join(',') === 'z0_driftperiod,z1_runawaymargin',
  'blank guard thresholds detected, other blanks and set values not');

assert(bg(['z0_xzone=', 'z1_xzone=5']).join(',') === 'z0_xzone', 'blank xzone (guard 8) refused client-side, set xzone kept');
assert(!/ZONE_OPTIONAL_KEY_RE[^\n]*xzone/.test(ZONES), 'xzone is not in the optional-drop regex');

assert(/blankGuardLabels\(params, guardFieldHidden\);\s*if \(blankGuards\.length\) \{[\s\S]*?return;\s*\}\s*msg\.textContent = 'Saving/.test(ZONES), 'blank guard field refuses before the POST');
assert(!/omit-PRESERVES each one/.test(ZONES), 'wrong omit-PRESERVES comment gone');

// ---- setup_wizard_page.html ----
m = WIZ.match(/function wizardLoginReadBackText\(wantEnable, cfg\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!m, 'found wizardLoginReadBackText');
const rb = new Function(m[0] + '; return wizardLoginReadBackText;')();
assert(/is now ON/.test(rb(true, { web_enabled: true })), 'read back ON');
assert(/is now off\./.test(rb(false, { web_enabled: false })), 'read back off');
assert(/differs/.test(rb(true, { web_enabled: false })), 'mismatch is called out');
assert(/could not be read back/.test(rb(true, null)) && !/is now/.test(rb(true, null)), 'unreadable: only sent claimed');
assert(/NOT marked done/.test(WIZ) && /return postStepState\(11, 'done'\);\s*\}, function/.test(WIZ), 'step 11 done only after a matching read-back');
assert((WIZ.match(/postStepStateOrThrow\(\d, 'done'\)/g) || []).length === 5 && /postStepState\(3, 'done'\)\.then\(function \(r\) \{\s*if \(stepSaveFailed/.test(WIZ) && /postStepState\(7, 'done'\)\.then\(function \(r\) \{\s*if \(stepSaveFailed/.test(WIZ), 'steps 1-7 report a refused progress save');
assert(/fetch\('\/api\/auth\/config'\)\.then\(function \(rb\)/.test(WIZ), 'wizard performs a real GET /api/auth/config after the POST');

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
