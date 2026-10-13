/* Tests for the WEB_UI_JS_AUDIT_2026-10-10 LOW fixes (webfx6). kcExecPost is run for real
 * (extracted from app.js); page-local changes are pinned by source contracts on the
 * production page text (no jsdom, same pattern as test_web_review_fixes.js).
 * Run: node firmware/KilnFW/App/test/test_web_ui_js_audit_fixes.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const dir = resolveDriversDir(__dirname);
const rd = (f) => fs.readFileSync(resolveDriverFile(dir, f), 'utf8').replace(/\r\n/g, '\n');
let passed = 0, failed = 0;
function assert(c, l) { if (c) { passed++; console.log('PASS: ' + l); } else { failed++; console.log('FAIL: ' + l); } }
function has(src, re, l) { assert(re.test(src), l); }

const APP = rd('app.js');
function slice(src, startMarker, endMarker) {
  const s = src.indexOf(startMarker); if (s < 0) throw new Error('missing ' + startMarker);
  const e = src.indexOf(endMarker, s); if (e < 0) throw new Error('missing ' + endMarker);
  return src.slice(s, e);
}

(async () => {
  // L-1: kcExecPost reports failures
  const execSrc = slice(APP, 'function kcExecPost(', 'var stopBarEl = null;');
  function run(fetchImpl) {
    const alerts = [];
    const btn = { disabled: true };
    const ctx = { fetch: fetchImpl, window: { kcAlert: (m) => alerts.push(m), kcIsAuthCancelled: (e) => !!(e && e.name === 'AuthCancelled') } };
    vm.createContext(ctx);
    vm.runInContext(execSrc + '\nthis.kcExecPost = kcExecPost;', ctx);
    return ctx.kcExecPost('/api/profile_exec/stop', btn, 'Stop').then(() => ({ alerts, btn }));
  }
  let r = await run(() => Promise.resolve({ ok: true, status: 200 }));
  assert(r.alerts.length === 0 && r.btn.disabled === false, 'L-1: ok reply quiet, button re-enabled');
  r = await run(() => Promise.resolve({ ok: false, status: 400 }));
  assert(r.alerts.length === 1 && /Stop was NOT sent \(HTTP 400\)/.test(r.alerts[0]) && r.btn.disabled === false, 'L-1: HTTP refusal shown');
  r = await run(() => Promise.reject(new Error('net')));
  assert(r.alerts.length === 1 && /network error/.test(r.alerts[0]), 'L-1: network error shown');
  r = await run(() => { const e = new Error('c'); e.name = 'AuthCancelled'; return Promise.reject(e); });
  assert(r.alerts.length === 0 && r.btn.disabled === false, 'L-1: declined sign-in quiet');
  has(APP, /kcExecPost\('\/api\/profile_exec\/stop', btn, 'Stop'\)/, 'L-1: STOP FIRING uses kcExecPost');

  const MAIN = rd('main_page.html');
  has(MAIN, /id="clearTripMsg"/, 'L-2: refusal row rendered from state');

  const Z = rd('zones_page.html');
  has(Z, /sweepLastRunning/, 'L-3: sweep poll retry state');
  has(Z, /Start failed: /, 'L-4: autotune Start shows refusals');

  const W = rd('setup_wizard_page.html');
  assert(!/still require the/.test(W), 'L-11: retired AP-password claim removed');
  has(W, /password && !username/, 'L-12: password without username refused');
  has(W, /if \(!freshZones\)/, 'L-16: unreadable zones refuses step 7 save');

  has(W, /function loadAll\(\) \{\s*var seqAtStart = kcEditSeq;[\s\S]*?kcEditSeq === seqAtStart\) \{\s*gGoto\(target\)/, 'REVIEW_WEBFX4 LOW-4: loadAll skips the re-render when an edit landed mid-reload');

  const D = rd('diagnostics_page.html');
  has(D, /kcConfirm\('Clear the crash log\?/, 'L-6: Clear asks first');
  has(D, /typeof result\.panic_disabled !== 'boolean'/, 'L-7: watchdog reply validated');

  const LP = rd('live_profile_page.html');
  has(LP, /function decide\(body\) \{\s*var msg = document\.getElementById\('decideMsg'\);\s*if \(lastGen === null/, 'L-8: decide refuses without generation');

  const SP = rd('safety_page.html');
  assert(!/row\('[^']*', window\.kcEscapeHtml/.test(SP), 'L-10: row() callers do not pre-escape');

  const REC = fs.readFileSync(path.join(dir, '..', '..', '..', 'KilnFW_recovery', 'main', 'recovery_page.html'), 'utf8');
  has(REC, /JSON\.parse\(r\.t\)\}catch/, 'I-5: ppoll survives non-JSON reply');

  has(rd('settings_display_page.html'), /if \(!loadedOk\)/, 'L-17: display Save refuses before load');
  assert(!/window\.location\.href = profileExportUrl/.test(rd('profiles_page.html')), 'L-15: profile export no longer navigates');

  for (const f of ['diagnostics_page.html','kiln_configs_page.html','live_profile_page.html','profiles_page.html']) {
    assert(/\.reason \|\| [a-z.]*\.error/.test(rd(f)), 'PERSFX2 LOW-3: ' + f + ' renders reason before error');
  }

  console.log(passed + ' passed, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})().catch((e) => { console.log('FAIL: exception ' + e.stack); process.exit(1); });
