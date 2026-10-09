/* Node-only test for ota_page.html's "Update from GitHub" card (WP10,
 * docs/GITHUB_RELEASE_UPDATE_PLAN.md). Evaluates the page's own pure block
 * (between the UPDATE_CARD_PURE_BEGIN/END markers), not a copy, and pins the
 * text tables against the firmware's literal verdict and error names so a
 * renamed enum or error code cannot silently turn into a raw code on screen.
 *
 * Run: node firmware/KilnFW/App/test/test_update_page.js
 */
'use strict';

const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');

const DRIVERS_DIR = resolveDriversDir(__dirname);
const SRC = fs.readFileSync(resolveDriverFile(DRIVERS_DIR, 'ota_page.html'), 'utf8');
const POLICY_C = fs.readFileSync(resolveDriverFile(DRIVERS_DIR, 'update_policy.c'), 'utf8');
const FETCH_C = fs.readFileSync(resolveDriverFile(DRIVERS_DIR, 'update_fetch.c'), 'utf8');

let passed = 0, failed = 0;
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; console.log('FAIL: ' + label); }
}

const m = SRC.match(/UPDATE_CARD_PURE_BEGIN[\s\S]*?\*\/\r?\n([\s\S]*?)\/\* UPDATE_CARD_PURE_END/);
assert(!!m, 'sanity: found the pure block in ota_page.html');
const api = new Function(m[1] +
  '; return { GH_ERRORS: GH_ERRORS, GH_VERDICTS: GH_VERDICTS, ghErrorText: ghErrorText,' +
  ' ghVerdictText: ghVerdictText, ghDownloadQuery: ghDownloadQuery, ghCheckQuery: ghCheckQuery, ghCanDownload: ghCanDownload,' +
  ' stageInstallable: stageInstallable, ghRefusalText: ghRefusalText, stageWedgeNote: stageWedgeNote };')();

// Every verdict name the firmware can emit has a sentence.
const verdicts = Array.from(POLICY_C.matchAll(/return "([a-z_]+)";/g)).map(x => x[1])
  .filter(v => v !== 'unknown');
assert(verdicts.length === 11, 'firmware has 11 verdict names (found ' + verdicts.length + ')');
verdicts.forEach(v => assert(api.GH_VERDICTS.hasOwnProperty(v), 'verdict text exists for ' + v));

// Every job error the fetch task can set has a sentence (job errors only:
// the two state names and the status words are not errors).
const notErrors = ['idle', 'checking', 'downloading', 'done', 'failed'];
const errs = Array.from(FETCH_C.matchAll(/return "([a-z_0-9]+)";/g)).map(x => x[1])
  .filter(e => notErrors.indexOf(e) < 0);
assert(errs.length >= 15, 'found the fetch error names (' + errs.length + ')');
errs.forEach(e => assert(api.GH_ERRORS.hasOwnProperty(e), 'error text exists for ' + e));
['fetch_busy', 'clock_not_synced', 'task_create_failed'].forEach(e =>
  assert(api.GH_ERRORS.hasOwnProperty(e) && FETCH_C.indexOf('"' + e + '"') >= 0, 'route error ' + e + ' known to both'));

// Unknown names stay readable, never blank.
assert(api.ghErrorText('weird_code') === 'Failed: weird_code', 'unknown error name is shown');
assert(api.ghErrorText('') === '', 'no error is blank');

assert(api.ghCheckQuery({}) === '', 'check: no flag -> no query');
assert(api.ghCheckQuery({ pre: true }) === '?allow_prerelease=1', 'check: prerelease flag reads the list');
assert(api.ghErrorText('no_release').indexOf('Include pre-releases') > 0, 'no_release text');

// Download query: nothing ticked sends nothing; downgrade carries the typed tag.
assert(api.ghDownloadQuery({}) === '', 'no flags -> no query');
assert(api.ghDownloadQuery({ pre: true }) === '?allow_prerelease=1', 'prerelease flag');
assert(api.ghDownloadQuery({ force: true, down: true, typed: 'v1.2.3' }) ===
  '?force=1&allow_downgrade=1&confirm_downgrade=v1.2.3', 'force + downgrade + typed tag');
assert(api.ghDownloadQuery({ down: true, typed: '' }) === '?allow_downgrade=1', 'downgrade without typed tag sends no confirm');
assert(api.ghDownloadQuery({ typed: 'v1.2.3' }) === '', 'a typed tag alone is never sent');
assert(api.ghDownloadQuery({ pre: true, force: true, typed: 'v1.0.0-pre.1' }) ===
  '?allow_prerelease=1&force=1&confirm_downgrade=v1.0.0-pre.1', 'force carries the typed tag (dev build) without a downgrade');

// Download offered only when the policy allows or a tickable flag unlocks it.
const done = (verdict, allowed) => ({ state: 'done', tag: 'v1.0.1', verdict: verdict, allowed: allowed });
assert(api.ghCanDownload(done('allow_upgrade', true), {}) === true, 'upgrade allowed');
assert(api.ghCanDownload(done('up_to_date', false), { force: true }) === false, 'up to date is not downloadable');
assert(api.ghCanDownload(done('allow_reinstall', false), {}) === false, 'reinstall needs force');
assert(api.ghCanDownload(done('allow_reinstall', false), { force: true }) === true, 'reinstall with force');
assert(api.ghCanDownload(done('refuse_downgrade', false), {}) === false, 'downgrade needs the tick');
assert(api.ghCanDownload(done('refuse_downgrade', false), { down: true }) === true, 'downgrade with the tick');
assert(api.ghCanDownload(done('refuse_prerelease', false), { pre: true }) === true, 'prerelease with the tick');
assert(api.ghCanDownload(done('refuse_partitions', false), { force: true, down: true, pre: true }) === false,
  'partition mismatch is never overridable from the page');
assert(api.ghCanDownload(done('refuse_dirty', false), { force: true, down: true, pre: true }) === false, 'dirty never overridable');
assert(api.ghCanDownload({ state: 'checking', tag: 'v1' }, {}) === false, 'not while checking');
assert(api.ghCanDownload(null, {}) === false, 'no status -> no download');

// Install only for a verified, idle stage.
// Review 7 L1: the wedge flag drives the notice, independent of the (busy) reason.
assert(api.stageWedgeNote({ busy: true, reason: 'busy', fetch_writer_wedged: true }).indexOf('restarted') > 0, 'L1: wedged flag shows reboot notice');
assert(api.stageWedgeNote({ reason: 'writer_wedged_reboot_required' }) !== '', 'L1: wedge reason shows reboot notice');
assert(api.stageWedgeNote({ reason: 'blank', fetch_writer_wedged: false }) === '', 'L1: no notice when not wedged');
assert(SRC.indexOf("stageWedgeNote(s)") > 0 && SRC.indexOf("row('Warning', wedge)") > 0, 'L1: renderStageInfo uses the notice');
assert(api.stageInstallable({ staged: true, header: 'ok', state: 'verified', busy: false }) === true, 'verified stage installable');
assert(api.stageInstallable({ staged: true, header: 'ok', state: 'verified', busy: true }) === false, 'not while busy');
assert(api.stageInstallable({ staged: false, header: 'ok', state: 'verified' }) === false, 'not when unstaged');
assert(api.stageInstallable({ staged: true, header: 'bad', state: 'verified' }) === false, 'not with a bad header');
assert(api.stageInstallable({ staged: true, header: 'ok', state: 'applied' }) === false, 'not when state is not verified');

// Refusal text: JSON error name, or the plain-text mode-gate sentence verbatim.
assert(api.ghRefusalText(409, '{"ok":false,"error":"heat_run_active"}') === api.GH_ERRORS.heat_run_active, 'JSON error mapped');
assert(api.ghRefusalText(409, 'refused: a firing is running\n') === 'refused: a firing is running', 'mode-gate text shown as-is');
assert(api.ghRefusalText(428, '') === 'Refused (HTTP 428).', 'empty 428 still explained');
assert(api.ghRefusalText(500, '') === 'HTTP 500', 'empty 500');

// Wiring: every route and element the card needs is in the page, nothing else is invented.
['/api/update/check', '/api/update/download', '/api/update/fetch', '/api/update/fetch/cancel',
 '/api/ota/esp/recovery_boot'].forEach(r => assert(SRC.indexOf("'" + r) >= 0, 'page calls ' + r));
['ghCheckBtn', 'ghDownloadBtn', 'ghCancelBtn', 'ghBusyBox', 'ghInfo', 'ghTyped', 'stageInstallBtn']
  .forEach(id => assert(SRC.indexOf('id="' + id + '"') >= 0, 'element ' + id));
assert(!/id="stageInstallBtn"[^>]*disabled[^>]*title="Waiting/.test(SRC), 'the placeholder install title is gone');
// Every action goes through the admin-gated OTA fetch, never a bare fetch().
const ghPart = SRC.slice(SRC.indexOf('Update from GitHub (WP10'), SRC.indexOf('Update repo setting'));
assert(ghPart.indexOf('kcOtaAuthedFetch(') >= 0 && !/[^a-zA-Z]fetch\(/.test(ghPart.replace(/kcOtaAuthedFetch\(/g, '')),
  'GitHub card uses only kcOtaAuthedFetch');

// Hand-upload downgrade gate (plan section 6): the page can pass the overrides and explains a 409.
['stageForce', 'stageDown', 'stageTyped', 'stageTypedWrap'].forEach(id => assert(SRC.indexOf('id="' + id + '"') >= 0, 'element ' + id));
['X-Stage-Force', 'X-Stage-Allow-Downgrade', 'X-Stage-Confirm'].forEach(h => assert(SRC.indexOf("'" + h + "'") >= 0, 'upload sends ' + h));
assert(SRC.indexOf('stageRefusalText(e)') >= 0, 'upload failure goes through stageRefusalText');
const UH = fs.readFileSync(resolveDriverFile(DRIVERS_DIR, 'update_http.c'), 'utf8');
['X-Stage-Force', 'X-Stage-Allow-Downgrade', 'X-Stage-Confirm'].forEach(h => assert(UH.indexOf('"' + h + '"') >= 0, 'firmware reads ' + h));
assert(/update_stage_set_gate\(&s_stage, policy_gate/.test(UH), 'upload handler installs the policy gate');

assert(SRC.indexOf('espUpdateBtn') < 0 && SRC.indexOf('espRollbackBtn') < 0, 'retired single-slot ESP buttons are gone');
assert(!/pushImage\(\s*'\/api\/ota\/esp'/.test(SRC), 'page has no push call to /api/ota/esp');
assert(SRC.indexOf('/api/ota/esp/rollback') < 0, 'page does not reference /api/ota/esp/rollback');

assert(SRC.indexOf('espFile') < 0 && SRC.indexOf('espPicker') < 0 && SRC.indexOf('updateOrderHint') < 0, 'orphaned ESP picker and order hint are gone');

console.log('\n' + passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
