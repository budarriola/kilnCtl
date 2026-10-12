/* Node-only test (review web7 test gap): zones_page.html must send expected_generation and
 * profiles_page.html expected_rev/expected_name on save, taken from the GET it loaded, or the
 * firmware's 409 lost-update guards (zones_config_stale / profile_changed) never fire from the UI.
 * Also pins zones_page.html's adaptive-tune Enable/Revert refreshing the generation (LOW-3).
 * Source-level (regex) like test_zones_type_toggle.js; no DOM.
 * Run: node firmware/KilnFW/App/test/test_page_lost_update_tokens.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const dir = resolveDriversDir(__dirname);
const zones = fs.readFileSync(resolveDriverFile(dir, 'zones_page.html'), 'utf8');
const profiles = fs.readFileSync(resolveDriverFile(dir, 'profiles_page.html'), 'utf8');

let failed = 0;
function check(cond, label) {
  console.log((cond ? 'PASS: ' : 'FAIL: ') + label);
  if (!cond) failed++;
}

check(/current\.generation\s*!==\s*undefined[^{]*\{\s*params\.push\('expected_generation='\s*\+\s*current\.generation\)/.test(zones),
  'zones save body pushes expected_generation from current.generation');
check(/^\s+current = data;/m.test(zones),
  'zones page assigns current from the GET');
check(/editingRev\s*!==\s*null\)\s*\{\s*params\.push\('expected_rev='\s*\+\s*editingRev\);\s*if \(editingName !== null\) params\.push\('expected_name='/.test(profiles),
  'profiles save body pushes expected_rev and expected_name');
check(/editingRev\s*=\s*\(typeof p\.slot_rev === 'number'\)/.test(profiles),
  'profiles editor loads editingRev from GET /api/profile slot_rev');
check(/editingRev\s*=\s*\(typeof result\.data\.slot_rev === 'number'\)/.test(profiles),
  'profiles save refreshes editingRev from the save response');

const en = zones.indexOf("'/api/adaptive_tune/enable'");
const rv = zones.indexOf("'/api/adaptive_tune/revert'");
const enSeg = zones.slice(en, rv);
const rvSeg = zones.slice(rv, rv + 2500);
check(en > 0 && /reloadUnlessDirty\(/.test(enSeg), 'adaptive-tune Enable refreshes via reloadUnlessDirty');
check(rv > 0 && /reloadUnlessDirty\(/.test(rvSeg), 'adaptive-tune Revert refreshes via reloadUnlessDirty');

console.log(failed ? 'FAILED: ' + failed : 'ALL PASSED');
process.exit(failed ? 1 : 0);
