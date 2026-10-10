/* Executes settings_page.html's real destructive-action handlers (factory
 * reset scopes, software reset, cfg-fs format) in a vm with a fake DOM and
 * pins: nothing is sent without confirm, the request body carries the right
 * scope, each HTTP refusal shows "Failed: <server text>" (never a success
 * line), a success shows the server text, and the catch arm wording.
 * Run: node firmware/KilnFW/App/test/test_settings_reset_flows.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const { runPageScript, flush, fakeResponse, makeEl } = require('./_page_vm.js');
const HTML = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'settings_page.html'), 'utf8');

let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }

// kind: 'reset' (danger-btn scope), 'sw' (swResetBtn), 'format' (cfgFsFormatConfirmBtn)
async function scenario(o) {
  const calls = [];
  const confirms = [];
  const btn = makeEl('danger');
  btn.setAttribute('data-scope', o.scope || 'wifi');
  const { els } = runPageScript(HTML, {
    elements: ['themeBtn', 'swResetBtn', 'swResetStatus', 'resetStatus', 'cfgFsFormatBanner',
      'cfgFsFormatConfirmBtn', 'cfgFsFormatStatus', 'cfgFsFormatReason'],
    groups: { '.danger-btn': [btn] },
    extra: {
      fetch: () => Promise.resolve(fakeResponse(200, '{}')),
      kcConfirm: (m) => { confirms.push(m); return Promise.resolve(o.confirm !== false); },
      kcIsAuthCancelled: (e) => !!(e && e.authCancelled),
      kcOtaAuthedFetch: (url, init) => {
        calls.push({ url, init: init || {} });
        if (o.reject === 'authcancel') return Promise.reject({ authCancelled: true });
        if (o.reject === 'net') return Promise.reject(Object.assign(new Error('Failed to fetch'), { name: 'TypeError' }));
        if (o.reject === 'other') return Promise.reject(new Error('boom-real-error'));
        return Promise.resolve(fakeResponse(o.status, o.text));
      },
    },
  });
  const trigger = { reset: btn, sw: els.swResetBtn, format: els.cfgFsFormatConfirmBtn }[o.kind];
  const statusEl = { reset: els.resetStatus, sw: els.swResetStatus, format: els.cfgFsFormatStatus }[o.kind];
  trigger.fire('click');
  await flush();
  return { status: statusEl.textContent, calls, confirms, banner: els.cfgFsFormatBanner };
}

(async () => {
  for (const kind of ['reset', 'sw', 'format']) {
    let s = await scenario({ kind, confirm: false, status: 200, text: 'x' });
    ok(s.calls.length === 0 && s.status === '', kind + ': declined confirm sends nothing and leaves status untouched');

    for (const code of [401, 403, 409, 423, 500]) {
      s = await scenario({ kind, status: code, text: 'refusal-' + code });
      ok(s.status === 'Failed: refusal-' + code, kind + ': HTTP ' + code + ' shows "Failed: <server text>" (got "' + s.status + '")');
    }
    s = await scenario({ kind, status: 200, text: 'did-it' });
    ok(s.status.indexOf('did-it') === 0 && !/Failed/.test(s.status), kind + ': 200 shows the server text, no failure wording');

    s = await scenario({ kind, reject: 'net' });
    ok(/Request sent|Request failed/.test(s.status), kind + ': transport error ends in a settled message, not the in-progress one');
    if (kind !== 'format') {
      ok(/Request sent -- the board may already be rebooting/.test(s.status), kind + ': a dropped connection (TypeError) after the send reads as a reboot in progress');
      s = await scenario({ kind, reject: 'other' });
      ok(/boom-real-error/.test(s.status) && !/Request sent/.test(s.status), kind + ': any other failure shows its real error, not "Request sent" (got "' + s.status + '")');
    }

    s = await scenario({ kind, reject: 'authcancel' });
    ok(s.status === 'Sign-in cancelled', kind + ': sign-in cancel shows "Sign-in cancelled" and clears the in-progress status (got "' + s.status + '")');
  }

  const w = await scenario({ kind: 'reset', scope: 'profiles', status: 200, text: 'ok' });
  ok(w.calls[0].url === '/api/factory_reset' && w.calls[0].init.body === 'scope=profiles', 'factory reset POSTs scope=profiles');
  ok(/permanently erases/.test(w.confirms[0]) && /profiles/i.test(w.confirms[0]), 'confirm text names the destructive scope');
  const f = await scenario({ kind: 'format', status: 200, text: 'formatted' });
  ok(f.banner.hidden === true, 'format success hides the pending-format banner');
  const g = await scenario({ kind: 'format', status: 409, text: 'busy' });
  ok(g.banner.hidden === false, 'format failure leaves the banner visible');

  console.log(failed ? failed + ' FAILED' : 'all passed');
  process.exit(failed ? 1 : 0);
})();
