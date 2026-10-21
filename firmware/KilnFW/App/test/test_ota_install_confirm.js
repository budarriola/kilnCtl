/* Node-only test for OC-01/OC-02/OC-04/OC-07: ota_page.html keeps refreshing the last-install line while it
 * is pending_confirm, and the firmware glue wires the retry loop, the otadata derivation and the 409 guard.
 * Run: node firmware/KilnFW/App/test/test_ota_install_confirm.js */
'use strict';
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const fs = require('fs');
const path = require('path');
const DD = resolveDriversDir(__dirname);
const PAGE = fs.readFileSync(resolveDriverFile(DD, 'ota_page.html'), 'utf8');
const HTTP = fs.readFileSync(resolveDriverFile(DD, 'update_install_http.c'), 'utf8');
const NET = fs.readFileSync(path.join(__dirname, '..', 'main_network_http.c'), 'utf8');

let passed = 0, failed = 0;
function assert(c, l) { if (c) { passed++; console.log('PASS: ' + l); } else { failed++; console.log('FAIL: ' + l); } }

const m = PAGE.match(/var lastInstallTimer = null;\s*function loadLastInstall\(\) \{[\s\S]*?\r?\n\}\r?\n/);
assert(!!m, 'found loadLastInstall() in ota_page.html');

function run(state) {
  const calls = { fetches: 0, timers: [], text: '' };
  const el = { set textContent(v) { calls.text = v; } };
  const win = { kcOtaAuthedFetch: function () {
    calls.fetches++;
    return Promise.resolve({ ok: true, text: function () {
      return Promise.resolve(JSON.stringify({ last_install: { state: state, code: 'ok', version: '2.0.0' } })); } });
  } };
  const fn = new Function('window', 'document', 'setTimeout', 'clearTimeout', 'lastInstallText',
    m[0] + '; return loadLastInstall;')(win, { getElementById: function () { return el; } },
    function (f, ms) { calls.timers.push(ms); return 1; }, function () {}, function (li) { return li.state; });
  fn();
  return new Promise(function (res) { setTimeout(function () { res(calls); }, 20); });
}

(async function () {
  let c = await run('pending_confirm');
  assert(c.timers.length === 1 && c.timers[0] === 5000, 'OC-07: pending_confirm re-polls in 5 s');
  c = await run('not_running');
  assert(c.timers.length === 0, 'OF-02: not_running stops polling');
  c = await run('unverified');
  assert(c.timers.length === 0, 'OG-01: unverified stops polling');
  assert(/li\.state === 'unverified'\) return 'Last install'/.test(PAGE), 'OG-01: ota_page words the unverified state');
  c = await run('recorded');
  assert(c.timers.length === 0 && c.text === 'recorded', 'OC-07: final state stops polling');
  assert(/typeof window\.kcOtaAuthedFetch !== 'function'\) return Promise\.resolve\(\)/.test(m[0]), 'loadLastInstall guards a missing kcOtaAuthedFetch');
  assert(/update_install_confirm_retry_delay_ms\(attempt\)/.test(NET) && /vTaskDelay\(pdMS_TO_TICKS\(wait_ms\)\)/.test(NET),
         'OC-01: confirm task retries the mark with backoff');
  assert(/update_install_res_derive_confirmed\(&res, desc, rid_p, running_ota_state\(\) == ESP_OTA_IMG_VALID\)/.test(HTTP),
         'OC-02: status derives confirmed from otadata');
  assert(/\.running_unconfirmed = app_running_unconfirmed/.test(HTTP) && /ESP_OTA_IMG_PENDING_VERIFY/.test(HTTP),
         'OC-04: install guard wired to PENDING_VERIFY');
  assert(/update_install_confirm_result\(&ops, desc, have_id \? rid : NULL\)/.test(HTTP), 'OC-03/OF-01: confirm passes the running version and image id');
  assert(/UPD_INST_CONFIRM_UNVERIFIED/.test(NET), 'OG-01: confirm loop treats unverified as final');
  const mm = HTTP.match(/int update_install_http_mark_confirmed\(void\)\s*\{[\s\S]*?\n\}/);
  assert(!!mm && mm[0].indexOf('record_needs_decision()') >= 0 && mm[0].indexOf('record_needs_decision()') < mm[0].indexOf('ota_http_update_try_begin'),
         'OG-02: id hashed only for a record needing a decision, before the update claim is taken');
  assert(/portENTER_CRITICAL\(&s_id_mux\)/.test(HTTP), 'OG-03: cached id published under a spinlock');
  assert(/UPD_INST_KEY_LEN/.test(HTTP) && /read_hashed_len\(\)/.test(HTTP), 'OG-04: hash covers the install image_length');
  assert(/high-water mark before mark loop/.test(NET), 'OF-04: stack high-water logged before the mark loop');
  console.log(passed + ' passed, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})();
