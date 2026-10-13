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
// omit-PRESERVES these keys AND xzone, tctype, zonetype, failsafe, settings_source, relaytype and name
// (parsed out of zones_http_post_parse.c by firmware_omit_preserved_keys() in
// tools/PcTools/tests/test_zone_omit_preserved_drift.py). The page deliberately drops a blank only for
// FW_DROPPABLE (= the page's ZONE_OPTIONAL_KEY_RE set, a subset) and KEEPS every other blank
// (ct_mask/thermo_mask are omit-DEFAULTED, the rest required). A regex key listed in neither array fails
// the coverage assert below, so drift is loud.
const FW_DROPPABLE = ['wrongdirwindow', 'wrongdirrate', 'offsettle', 'runawayrate', 'runawaymargin', 'driftperiod',
  'debounce', 'frozenwindow', 'fuzzy_strength', 'coupling_diag_k_dc', 'easeoffmult', 'approachratecap', 'errorband',
  'rateband', 'progressband', 'coilpower', 'hystc', 'minons', 'minoffs', 'k', 'tau', 'deadtime',
  'coupling_c0', 'coupling_c1', 'coupling_c2'];
const MUST_KEEP = ['ct_mask', 'thermo_mask', 'kp', 'ki', 'kd', 'name', 'relay_mask', 'xzone', 'maxtemp', 'mintemp',
  'cal', 'ramp', 'sanity', 'mode', 'window', 'minon', 'minoff', 'timingprofile', 'zonetype', 'tctype', 'failsafe',
  'settings_source', 'driftperiodx', 'kk', 'coupling_c', 'coupling_cx'];
{
  const rk = (/\^z\\d\+_\(([^)]*)\)/.exec(m[0]) || [])[1];
  assert(!!rk, 'parsed the key set out of ZONE_OPTIONAL_KEY_RE');
  (rk || '').split('|').forEach(function (k) {
    if (k === 'coupling_c\\d+') k = 'coupling_c0'; // generic form covers coupling_c0..2 (listed individually)
    assert(FW_DROPPABLE.indexOf(k) >= 0 || MUST_KEEP.indexOf(k) >= 0, 'ZONE_OPTIONAL_KEY_RE key is in FW_DROPPABLE or MUST_KEEP: ' + k);
  });
}
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
let tail = null;
const mockFetch = function (url, opts) { posted = { url: url, opts: opts }; return {}; };
try {
  tail = new Function('params', 'current', 'saveBtnEl', 'zonesEditSeq', 'msg', 'fetch', 'omitBlankOptionalParams',
    ZONES.slice(sIdx, eIdx) + '});');
} catch (e) { assert(false, 'Save handler fetch tail parses: ' + e.message); }
const saveParams = [];
for (let z = 0; z < 3; z++) saveParams.push('z' + z + '_thermo_mask=', 'z' + z + '_ct_mask=', 'z' + z + '_kp=1', 'z' + z + '_driftperiod=');
if (tail) tail(saveParams.slice(),{ generation: 7 }, { disabled: false }, 0, {}, mockFetch, omit);
const bodyParts = posted ? posted.opts.body.split('&') : [];
assert(!!posted && posted.url === '/api/zones' && posted.opts.method === 'POST', 'Save posts to /api/zones');
for (let z = 0; z < 3; z++) {
  assert(bodyParts.indexOf('z' + z + '_ct_mask=') >= 0 && bodyParts.indexOf('z' + z + '_thermo_mask=') >= 0,
    'POST body keeps blank ct_mask/thermo_mask for zone ' + z);
  assert(bodyParts.indexOf('z' + z + '_driftperiod=') < 0 && bodyParts.indexOf('z' + z + '_kp=1') >= 0,
    'POST body drops blank optional, keeps set value for zone ' + z);
}
// BENCHFX11 LOW-2: the mask pushes in the zone loop must be unconditional so every zone always sends
// thermo_mask and ct_mask (even 0): the firmware omit-DEFAULTS an absent mask.
['thermo', 'ct'].forEach(function (w) {
  const stmt = "params.push('z' + i + '_" + w + "_mask=' + " + w + "Mask);";
  const lines = ZONES.split(/\r?\n/);
  const at = [];
  lines.forEach(function (l, n) { if (l.indexOf(stmt) >= 0) at.push(n); });
  assert(at.length === 1, 'exactly one push of ' + w + '_mask (found ' + at.length + ')');
  if (at.length === 1) {
    assert(lines[at[0]].trim() === stmt, w + '_mask push is a bare statement, not conditional: ' + lines[at[0]].trim());
    assert(/[;{}]\s*$/.test(lines[at[0] - 1]), w + '_mask push follows a complete statement, not a bare if/else');
  }
});
assert(bodyParts[bodyParts.length - 1] === 'expected_generation=7', 'POST body carries expected_generation');
assert(/saveBtnEl\.disabled = true;[\s\S]*?omitBlankOptionalParams\(params\)[\s\S]*?fetch\('\/api\/zones'/.test(ZONES),
  'Save disables the button and filters params before the POST');
assert(/\.then\(function \(r\) \{\s*if \(saveTimer\) clearTimeout\(saveTimer\);\s*saveBtnEl\.disabled = false;/.test(ZONES), 'button re-enabled on response');
assert(/\.catch\(function \(e\) \{\s*if \(saveTimer\) clearTimeout\(saveTimer\);\s*saveBtnEl\.disabled = false;/.test(ZONES), 'button re-enabled on failure');
assert(/new AbortController\(\)[\s\S]*?setTimeout\(function \(\) \{ ac\.abort\(\); \}, 30000\)[\s\S]*?signal: armSaveTimer\(\)/.test(ZONES), 'save has an AbortController timeout');
const g1 = ZONES.match(/var ZONE_GUARD_BLANK_RE = [^\n]*\n/), g2 = ZONES.match(/function blankGuardKeys\(params\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!g1 && !!g2, 'found blankGuardKeys');
const g3 = ZONES.match(/var ZONE_GUARD_LABELS = \{[\s\S]*?\};\r?\n/);
const g4 = ZONES.match(/function blankGuardLabels\(params, isHidden\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!g3 && !!g4, 'found blankGuardLabels');
const bg = new Function(g1[0] + g2[0] + '; return blankGuardKeys;')();
// BENCHFX11 LOW-1: the Save handler hands blankGuardLabels/blankGuardKeys the LIVE params array, so they
// must be pure: run both on a frozen array (any splice/push/sort throws) and check it is unchanged.
if (g3 && g4) {
  const live = ['z0_ct_mask=', 'z0_thermo_mask=', 'z0_driftperiod=', 'z1_xzone=', 'z0_kp=1'];
  const frozen = Object.freeze(live.slice());
  let threw = null, labels = null;
  try {
    const f = new Function('"use strict";' + g1[0] + g2[0] + g3[0] + g4[0] + '; return [blankGuardKeys, blankGuardLabels];')();
    f[0](frozen); labels = f[1](frozen, null);
  } catch (e) { threw = e; }
  assert(!threw, 'blankGuardKeys/blankGuardLabels do not mutate their params argument: ' + (threw && threw.message));
  assert(frozen.join(',') === live.join(','), 'params unchanged after blankGuard helpers');
  assert(!threw && labels.join(',') === 'Zone 1 Drift period,Zone 2 Cross-zone limit', 'blankGuardLabels result');
  // BENCHFX12 INFO-1: also a frozen, realistic array with NO blank guard (the successful-save path).
  const live2 = ['z0_ct_mask=0', 'z0_thermo_mask=1', 'z0_driftperiod=30', 'z0_xzone=50', 'z1_xzone=40', 'z0_kp=1', 'z0_debounce=5'];
  const frozen2 = Object.freeze(live2.slice());
  let threw2 = null, labels2 = null, keys2 = null;
  try {
    const f2 = new Function('"use strict";' + g1[0] + g2[0] + g3[0] + g4[0] + '; return [blankGuardKeys, blankGuardLabels];')();
    keys2 = f2[0](frozen2); labels2 = f2[1](frozen2, null);
  } catch (e) { threw2 = e; }
  assert(!threw2, 'helpers run on a frozen no-blank array: ' + (threw2 && threw2.message));
  assert(frozen2.join(',') === live2.join(','), 'no-blank params unchanged after blankGuard helpers');
  assert(!threw2 && labels2.length === 0 && keys2.length === 0, 'no-blank array yields no blank guards');
}
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
