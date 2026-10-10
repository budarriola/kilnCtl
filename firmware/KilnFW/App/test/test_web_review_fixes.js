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
  const abortBranch = ZONES.slice(ZONES.indexOf("e.name === 'AbortError'"));
  const abortBody = abortBranch.slice(0, abortBranch.indexOf('return;'));
  assert(/showSaveTimeoutNotice\(msg\)/.test(abortBody) && !/loadCurrent\(/.test(abortBody),
    'AbortError branch shows the notice and does NOT auto-reload');

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
