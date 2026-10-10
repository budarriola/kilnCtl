/* Behavioral node test for DEV_WEB_REVIEW_2026-10-09 fixes: the app.js fetch wrapper's
 * caller-timeout hooks (LOW-1), zones blank-guard refusal labels (LOW-2), wizard helpers
 * (LOW-3/4/5). Extracts PRODUCTION text from the pages and runs it.
 * Run: node firmware/KilnFW/App/test/test_web_review_fixes.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const dir = resolveDriversDir(__dirname);
const rd = function (f) { return fs.readFileSync(resolveDriverFile(dir, f), 'utf8'); };
const APP = rd('app.js'), ZONES = rd('zones_page.html'), WIZ = rd('setup_wizard_page.html');

let passed = 0, failed = 0;
function assert(c, l) { if (c) { passed++; console.log('PASS: ' + l); } else { failed++; console.log('FAIL: ' + l); } }

// ---- app.js wrapper (same extraction as test_fetch_auth_ack.js) ----
const LINES = APP.split('\n');
function extractRange(s0, e0) {
  const raw = (l) => l.replace(/\r$/, '');
  const s = LINES.findIndex((l) => raw(l) === s0);
  const e = LINES.findIndex((l, i) => i > s && raw(l) === e0);
  if (s < 0 || e < 0) throw new Error('marker not found: ' + s0 + ' / ' + e0);
  return LINES.slice(s, e).join('\n');
}
const SRC = extractRange('  var nativeFetch = window.fetch.bind(window);',
  '  var loginModalEl = null, loginTitleEl = null, loginUserEl = null, loginPassEl = null,') + '\n' +
  extractRange('  var pendingLogin = null;', '  // ---- OTA-family fetch -------------------------------------------------');

function makeCtx(responses) {
  const queue = responses.slice();
  const calls = [];
  const resp = (status) => ({ ok: status < 300, status: status, headers: { get: () => null },
    text: () => Promise.resolve('') });
  const ctx = {
    kcRequestIsUserInitiated: () => true, kcPageIsDashboard: () => false, Request: undefined,
    openLoginModal: () => Promise.resolve(true), console: console,
    window: {
      fetch: function (input, init) { calls.push(init); return Promise.resolve(resp(queue.shift())); },
      kcConfirm: () => Promise.resolve(true),
    },
  };
  vm.createContext(ctx);
  vm.runInContext(SRC, ctx);
  return { ctx, calls };
}

(async () => {
  const t = makeCtx([401, 200]);
  const events = [];
  const sigA = { aborted: true, tag: 'old' }, sigB = { aborted: false, tag: 'fresh' };
  const r = await t.ctx.window.fetch('/api/zones', {
    method: 'POST', body: 'x', signal: sigA,
    __kcOnAuthPrompt: function () { events.push('pause'); },
    __kcAuthSignal: function () { events.push('rearm'); return sigB; },
  });
  assert(r.status === 200, 'wrapper: login retry resolves with the 200');
  assert(events.join(',') === 'pause,rearm', 'wrapper: caller timer paused for the modal then re-armed for the retry: ' + events.join(','));
  assert(t.calls.length === 2 && t.calls[0].signal === sigA && t.calls[1].signal === sigB,
    'wrapper: the retry carries the fresh signal, not the (possibly aborted) original');
  const t2 = makeCtx([401, 200]);
  await t2.ctx.window.fetch('/api/x', { method: 'POST', signal: sigA });
  assert(t2.calls[1].signal === sigA, 'wrapper: callers without the hooks are unchanged');

  // ---- zones_page.html: blank-guard labels ----
  const g1 = ZONES.match(/var ZONE_GUARD_BLANK_RE = [^\n]*\n/)[0];
  const g2 = ZONES.match(/function blankGuardKeys\(params\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  const g3 = ZONES.match(/var ZONE_GUARD_LABELS = \{[\s\S]*?\};\r?\n/)[0];
  const g4 = ZONES.match(/function blankGuardLabels\(params, isHidden\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  const bgl = new Function(g1 + g2 + g3 + g4 + '; return blankGuardLabels;')();
  const out = bgl(['z0_driftperiod=', 'z1_wrongdirwindow= ', 'z0_kp=', 'z0_debounce=5']);
  assert(out.join('|') === 'Zone 1 Drift period|Zone 2 Wrong-direction window', 'blank guard names UI labels, 1-based zones: ' + out.join('|'));
  assert(!/z\d_|wrongdirwindow|driftperiod/.test(out.join(' ')), 'no wire keys in the refusal text');
  const out2 = bgl(['z0_driftperiod=', 'z1_debounce='], (zi, s) => zi === 0);
  assert(out2.join('|') === 'Zone 2 Sensor debounce', 'a blank guard field the operator cannot see (hidden heater-only row) is not named');
  const ob = new Function(ZONES.match(/var ZONE_OPTIONAL_KEY_RE = [^\n]*\n/)[0] +
    ZONES.match(/function omitBlankOptionalParams\(params\) \{[\s\S]*?\r?\n\}\r?\n/)[0] + '; return omitBlankOptionalParams;')();
  assert(ob(['z0_kp=1', 'z0_driftperiod=', 'z1_coupling_c2= ', 'z0_name=', 'z0_debounce=5']).join('|') === 'z0_kp=1|z0_name=|z0_debounce=5',
    'omitBlankOptionalParams drops blank optional keys only (stored value kept), keeps filled and non-optional blanks');
  assert(/omitBlankOptionalParams\(params\);/.test(ZONES.slice(ZONES.indexOf("msg.textContent = 'Saving"))), 'Save handler runs params through omitBlankOptionalParams');

  // ---- zones save timeout (LOW-5): edits kept, result-unknown text, explicit reload ----
  const tn = ZONES.match(/function showSaveTimeoutNotice\(msgEl\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  let reloads = 0, clickFn = null;
  const kids = [];
  const fakeMsg = { textContent: 'x', appendChild: (c) => kids.push(c) };
  new Function('document', 'loadCurrent', tn + '; showSaveTimeoutNotice(arguments[2]);')(
    { createElement: () => ({ addEventListener: (ev, fn) => { clickFn = fn; } }) }, () => { reloads++; }, fakeMsg);
  assert(/unknown/i.test(fakeMsg.textContent) && /edits are still in the form/.test(fakeMsg.textContent), 'timeout notice says result unknown and edits kept');
  assert(reloads === 0 && kids.length === 1 && /Reload/.test(kids[0].textContent), 'timeout notice does not reload; offers a Reload button');
  clickFn(); assert(reloads === 1, 'Reload button reloads from the board');
  // W4: behavioural -- run the production abort handler and watch for ANY reload, immediate or deferred.
  const ha = ZONES.match(/function handleSaveAbort\(e, msgEl\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  let abortReloads = 0;
  const abortKids = [];
  const abortMsg = { textContent: '', appendChild: (c) => abortKids.push(c) };
  const handleAbort = new Function('showSaveTimeoutNotice', 'loadCurrent', 'document', 'setTimeout',
    ha + '; return handleSaveAbort;')(
    (m) => { m.textContent = 'Save result unknown'; }, () => { abortReloads++; }, {}, setTimeout);
  assert(handleAbort({ name: 'AbortError' }, abortMsg) === true && /unknown/.test(abortMsg.textContent),
    'AbortError is handled with the unknown-result notice');
  assert(handleAbort(new Error('x'), abortMsg) === false, 'non-abort errors fall through to the normal failure path');
  await new Promise((r) => setTimeout(r, 30));
  assert(abortReloads === 0, 'AbortError never reloads the form, immediately or deferred');
  assert(/if \(handleSaveAbort\(e, msg\)\) return;/.test(ZONES), 'Save catch returns right after handleSaveAbort');

  // ---- W5: sweep completion / autotune Accept must not discard unsaved edits ----
  const dz = ZONES.match(/var zonesFormDirty = false;[\s\S]*?function showDirtyReloadNotice\(msgEl, what\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  let dReloads = 0, dClick = null;
  const dKids = [];
  const dMsg = { textContent: '', appendChild: (c) => dKids.push(c) };
  const dz2 = new Function('loadCurrent', 'document', dz + '; return {reloadUnlessDirty, markZonesFormDirty};')(
    () => { dReloads++; },
    { getElementById: () => dMsg, createElement: () => ({ addEventListener: (ev, fn) => { dClick = fn; } }) });
  assert(dz2.reloadUnlessDirty('Done') === true && dReloads === 1, 'clean form: reloads as before');
  dz2.markZonesFormDirty();
  assert(dz2.reloadUnlessDirty('The tuned gains were accepted') === false && dReloads === 1, 'dirty form: NOT reloaded');
  assert(/unsaved edits/.test(dMsg.textContent) && /overwrite/.test(dMsg.textContent) && dKids.length === 1,
    'dirty form: warns, names the stale-overwrite risk, offers a Reload button');
  dClick(); assert(dReloads === 2, 'warning Reload button reloads on request');
  assert(/reloadUnlessDirty\('The current sweep finished'\)/.test(ZONES) && /reloadUnlessDirty\('The tuned gains were accepted'\)/.test(ZONES),
    'sweep completion and autotune Accept both go through reloadUnlessDirty');
  assert(/current = data;\s*zonesFormDirty = false;/.test(ZONES), 'a successful board render clears the dirty flag');

  // ---- F5: relay names/types (#relayNames) live outside #zones and must mark the form dirty ----
  {
    const wire = ZONES.match(/\['zones',[^\]]*\]\.forEach\(function \(id\) \{[\s\S]*?\n\}\);/)[0];
    const els = {};
    const doc = { getElementById: (id) => (els[id] = els[id] || { ls: {}, addEventListener(t, fn) { this.ls[t] = fn; } }) };
    new Function('document', 'markZonesFormDirty', wire)(doc, () => { dReloads += 100; });
    const rn = els['relayNames'];
    assert(rn && typeof rn.ls.input === 'function' && typeof rn.ls.change === 'function',
      '#relayNames gets input and change listeners');
    const before = dReloads; if (rn) { rn.ls.input && rn.ls.input(); rn.ls.change && rn.ls.change(); }
    assert(dReloads === before + 200, '#relayNames edits invoke markZonesFormDirty');
  }

  // ---- guardFieldHidden (DOM half of LOW-2) ----
  const gh = ZONES.match(/var ZONE_GUARD_INPUT_CLASS = \{[\s\S]*?\};\r?\nfunction guardFieldHidden\(zi, suffix\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  const mkDoc = (display, hasWrap) => ({ querySelector: (sel) => /data-index="1"/.test(sel) ? {
    querySelector: (c) => c === '.debounce' ? { closest: (w) => (w === '.heaterOnly' && hasWrap) ? { style: { display: display } } : null } : null } : null });
  const ghf = (d) => new Function('document', gh + '; return guardFieldHidden;')(d);
  assert(ghf(mkDoc('none', true))(1, 'debounce') === true, 'guardFieldHidden: field inside a display:none heater-only row is hidden');
  assert(ghf(mkDoc('', true))(1, 'debounce') === false, 'guardFieldHidden: visible heater-only row is not hidden');
  assert(ghf(mkDoc('none', false))(1, 'debounce') === false, 'guardFieldHidden: field outside a heater-only wrapper is not hidden');
  assert(ghf(mkDoc('none', true))(0, 'debounce') === false, 'guardFieldHidden: missing zone block is not hidden');

  // ---- kcHostRefusalFromText / kcHostRefusalText (app.js) and the forgot-flow password precheck (W1) ----
  const hr = APP.match(/window\.kcHostRefusalText = function \(body\) \{[\s\S]*?\r?\n  \};\r?\n[\s\S]*?window\.kcHostRefusalFromText = function \(text\) \{[\s\S]*?\r?\n  \};\r?\n/)[0];
  const pw = APP.match(/window\.kcResetPasswordProblem = function \(pw, username\) \{[\s\S]*?\r?\n  \};\r?\n/)[0];
  const aw = {};
  new Function('window', 'TextEncoder', hr + pw)(aw, TextEncoder);
  assert(/IP address/.test(aw.kcHostRefusalFromText('{"error":"bad_host"}')) && /IP address/.test(aw.kcHostRefusalFromText('{"error":"cross_origin"}')),
    'kcHostRefusalFromText: host/origin refusal bodies map to guidance');
  assert(aw.kcHostRefusalFromText('{"error":"other"}') === null && aw.kcHostRefusalFromText('not json') === null && aw.kcHostRefusalFromText('') === null,
    'kcHostRefusalFromText: other bodies and non-JSON give null');
  const pp = aw.kcResetPasswordProblem;
  assert(pp('Abcdefgh1', 'u') && !pp('Abcdefgh12', 'u'), 'reset precheck: 10-byte minimum');
  assert(!pp('A' + 'b'.repeat(63), 'u') && pp('A' + 'b'.repeat(64), 'u'), 'reset precheck: 64-byte maximum');
  assert(pp('abcdefghijk', 'u') && !pp('abcdefghij1', 'u'), 'reset precheck: all-lowercase refused');
  assert(pp('MyPassword123', 'u') && pp('xxKILNxx999', 'u'), 'reset precheck: "password"/"kiln" refused, case-insensitive');
  assert(pp('BenchAdmin99', 'BenchAdmin99') && !pp('BenchAdmin99', 'other'), 'reset precheck: equal to username refused');

  // W1: step 2 with a weak password must NOT spend the token or POST; a strong one posts it.
  const s2 = APP.match(/function onStep2Submit\(evt\) \{[\s\S]*?\r?\n    \}\r?\n/)[0];
  const mkStep2 = (pass) => {
    const st = { fetches: 0, back: 0 };
    const el = (v) => ({ value: v, disabled: false, textContent: '' });
    const e2 = el('');
    const env = { forgotSubmit2El: el(''), forgotErrorEl2: e2, forgotNewPassEl: el(pass), forgotNewPass2El: el(pass),
      forgotUserEl: el('BenchAdmin99'), nativeFetch: () => { st.fetches++; return new Promise(() => {}); },
      forgotBackToStep1: () => { st.back++; }, KC_FORGOT_GENERIC_FAIL: 'x', forgotGeneration: 1, window: aw };
    const run = new Function('env', 'with (env) { var forgotResetToken = "TOKEN"; ' + s2 +
      '; return { submit: onStep2Submit, token: function () { return forgotResetToken; } }; }')(env);
    run.submit({ preventDefault() {} });
    return { st, e2, token: run.token() };
  };
  const weak = mkStep2('shortpw');
  assert(weak.st.fetches === 0 && weak.token === 'TOKEN' && /too short/.test(weak.e2.textContent) && weak.st.back === 0,
    'W1: weak password is refused client-side, token kept, no POST, stays on step 2');
  const strong = mkStep2('Str0ngEnough!');
  assert(strong.st.fetches === 1 && strong.token === null, 'W1: strong password POSTs and spends the token');

  // ---- wizard helpers ----
  const w1 = WIZ.match(/function stepRefusalText\(r\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  const w2 = WIZ.match(/function lockSave\(id\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  const w3 = WIZ.match(/function wizardLoginReadBackText\(wantEnable, cfg\) \{[\s\S]*?\r?\n\}\r?\n/)[0];
  const btn = { disabled: false };
  const win = { kcHostRefusalText: (b) => (b && b.error === 'bad_host') ? 'OPEN BY IP' : null };
  const f = new Function('window', 'document', w1 + w2 + w3 + '; return {stepRefusalText, lockSave, wizardLoginReadBackText};')(
    win, { getElementById: () => btn });
  assert(f.stepRefusalText({ body: { error: 'bad_host' } }) === 'OPEN BY IP', 'wizard: bad_host mapped to guidance');
  assert(f.stepRefusalText({ body: { error: 'nope' } }) === 'nope' && f.stepRefusalText({}) === 'the board refused it', 'wizard: other codes unchanged');
  const un = f.lockSave('b');
  assert(typeof un === 'function' && btn.disabled === true, 'lockSave disables the button');
  assert(f.lockSave('b') === null, 'second click while in flight is refused');
  un();
  assert(btn.disabled === false && f.lockSave('b') !== null, 'release re-enables');
  assert(!/^Saved/.test(f.wizardLoginReadBackText(true, { web_enabled: false })), 'mismatch read-back never says "Saved"');
  assert(!/^Saved/.test(f.wizardLoginReadBackText(true, null)), 'unreadable read-back never says "Saved"');
  assert(/^Saved\. Read back: web login is now ON/.test(f.wizardLoginReadBackText(true, { web_enabled: true })), 'matching read-back still says Saved');
  // Every step Save handler locks (and releases) on its own button, inside its own click handler.
  [1, 2, 4, 5, 6, 11].forEach(function (n) {
    const hAt = WIZ.indexOf("getElementById('step" + n + "Save').addEventListener('click'");
    const lock = new RegExp("var unlock" + n + " = lockSave\\('step" + n + "Save'\\);\\s*if \\(!unlock" + n + "\\) return;").exec(WIZ.slice(hAt > 0 ? hAt : 0));
    const nextH = WIZ.indexOf('.addEventListener(\'click\'', hAt + 80);
    assert(hAt > 0 && lock && (nextH < 0 || hAt + lock.index < nextH), 'wizard step ' + n + ' Save handler takes the double-submit lock');
    assert(new RegExp('then\\(unlock' + n + '\\)|unlock' + n + '\\(\\)|, unlock' + n + '\\)').test(WIZ), 'wizard step ' + n + ' releases the lock');
  });
  assert(/return fetch\('\/api\/auth\/config'\)\.then\(function \(rb\)/.test(WIZ), 'step 11 read-back chain is returned');
  assert(/getElementById\('stepStatusLine'\)\.className = 'hint'/.test(WIZ), 'stepStatusLine className reset on render');

  console.log(passed + ' passed, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})();
