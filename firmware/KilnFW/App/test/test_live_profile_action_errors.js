/* Executes live_profile_page.html's real fork/save/decide click handlers (vm,
 * fake DOM) and pins the operator-visible text for each HTTP refusal: a 400
 * bound violation / 409 window violation / 403 builtin-overwrite shows the
 * server's own message verbatim and never "Saved."/"Done."; the overwrite
 * confirmation box and save-as name are enforced before anything is sent.
 * KNOWN-DEFECT lines document behaviour that is wrong today (see
 * docs/audits/WEB_JS_COVERAGE_GAPS_2026-10-10.md) without failing the suite.
 * Run: node firmware/KilnFW/App/test/test_live_profile_action_errors.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const { runPageScript, flush } = require('./_page_vm.js');
const HTML = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'live_profile_page.html'), 'utf8');

let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }
function known(c, l) { console.log((c ? 'KNOWN-DEFECT (present): ' : 'KNOWN-DEFECT FIXED (update audit): ') + l); }

// resp: {status, json} | {status, html} (body that is not JSON) | 'authcancel'
async function scenario(btnId, resp, setup) {
  const posts = [];
  const { els } = runPageScript(HTML, {
    elements: ['themeBtn', 'forkBtn', 'saveBtn', 'reloadBtn', 'saveAsBtn', 'saveAsName', 'overwriteBtn',
      'overwriteConfirm', 'discardBtn', 'forkMsg', 'saveMsg', 'decideMsg', 'statusText', 'refusalBanner'],
    extra: {
      setInterval: () => 0,
      escapeHtml: (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;'),
      kcIsAuthCancelled: (e) => !!(e && e.authCancelled),
      fetch: (url, init) => {
        if (init && init.method === 'POST') {
          posts.push({ url, body: init.body });
          if (resp === 'authcancel') return Promise.reject({ authCancelled: true });
          return Promise.resolve({
            ok: resp.status >= 200 && resp.status < 300, status: resp.status,
            json: () => (resp.json ? Promise.resolve(resp.json) : Promise.reject(new SyntaxError('Unexpected token <'))),
          });
        }
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ active: false }) });
      },
    },
  });
  await flush(5);
  if (setup) setup(els);
  els[btnId].fire('click');
  await flush();
  return { els, posts };
}
const txt = (el) => (el.innerHTML || '') + (el.textContent || '');

(async () => {
  let r = await scenario('saveBtn', { status: 400, json: { ok: false, error: 'segment 2: target 1400 C exceeds zone limit 1300 C' } });
  ok(/segment 2: target 1400 C exceeds zone limit 1300 C/.test(txt(r.els.saveMsg)) && !/Saved\./.test(txt(r.els.saveMsg)),
    'save 400 shows the server bound-violation text verbatim, not "Saved."');
  r = await scenario('saveBtn', { status: 409, json: { ok: false, error: 'segment 0 is already running' } });
  ok(/segment 0 is already running/.test(txt(r.els.saveMsg)), 'save 409 shows the server window-violation text verbatim');
  r = await scenario('saveBtn', { status: 400, json: {} });
  ok(/save failed/.test(txt(r.els.saveMsg)), 'save refusal with no error field falls back to "save failed", never success');
  r = await scenario('saveBtn', { status: 200, json: { ok: true } });
  ok(txt(r.els.saveMsg) === 'Saved.' && r.posts.length === 1 && /^id=/.test(r.posts[0].body), 'save 200 -> "Saved." and one form POST');
  r = await scenario('saveBtn', { status: 200, json: { ok: false, error: 'rev conflict' } });
  ok(/rev conflict/.test(txt(r.els.saveMsg)) && !/Saved\./.test(txt(r.els.saveMsg)), 'HTTP 200 with ok:false is still a failure');

  r = await scenario('saveBtn', { status: 403, html: '<html>forbidden</html>' });
  known(/could not reach the board/.test(txt(r.els.saveMsg)),
    'save refused 403 with a non-JSON body reports "could not reach the board" (got "' + txt(r.els.saveMsg) + '")');
  r = await scenario('saveBtn', 'authcancel');
  known(/Saving/.test(txt(r.els.saveMsg)), 'sign-in cancel on save leaves the stale "Saving..." text (got "' + txt(r.els.saveMsg) + '")');

  r = await scenario('forkBtn', { status: 409, json: { ok: false, error: 'a working copy already exists' } });
  ok(/a working copy already exists/.test(txt(r.els.forkMsg)), 'fork 409 shows the server text');

  r = await scenario('overwriteBtn', { status: 200, json: { ok: true } }, (e) => { e.overwriteConfirm.checked = false; });
  ok(r.posts.length === 0 && /confirmation box/.test(txt(r.els.decideMsg)), 'overwrite without the checkbox sends nothing');
  r = await scenario('overwriteBtn', { status: 200, json: { ok: true } }, (e) => { e.overwriteConfirm.checked = true; });
  ok(r.posts.length === 1 && r.posts[0].body === 'action=overwrite&confirm=1' && txt(r.els.decideMsg) === 'Done.', 'overwrite with the checkbox posts action=overwrite&confirm=1');
  r = await scenario('overwriteBtn', { status: 403, json: { ok: false, error: 'origin is a built-in schedule' } }, (e) => { e.overwriteConfirm.checked = true; });
  ok(/built-in schedule/.test(txt(r.els.decideMsg)) && !/Done\./.test(txt(r.els.decideMsg)), 'overwrite 403 (builtin origin) shows the server text, not "Done."');

  r = await scenario('saveAsBtn', { status: 200, json: { ok: true } }, (e) => { e.saveAsName.value = ''; });
  ok(r.posts.length === 0 && /Enter a name/.test(txt(r.els.decideMsg)), 'save-as with an empty name sends nothing');
  r = await scenario('saveAsBtn', { status: 200, json: { ok: true } }, (e) => { e.saveAsName.value = 'a&b c'; });
  ok(r.posts.length === 1 && r.posts[0].body === 'action=save_as&name=a%26b%20c', 'save-as URL-encodes the name');

  r = await scenario('discardBtn', { status: 200, json: { ok: true } });
  known(r.posts.length === 1, 'Discard working copy posts immediately with no confirm dialog (unsaved edits lost on one click)');

  console.log(failed ? failed + ' FAILED' : 'all passed');
  process.exit(failed ? 1 : 0);
})();
