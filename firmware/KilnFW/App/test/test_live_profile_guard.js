/* live_profile_page.html beforeunload guard (D7 + REVIEW_WEBFIX L3/L5): clears after
 * a successful save and decision; clears when the editor goes away; a poll with a
 * changed working copy never silently overwrites dirty edits; edits typed while a
 * save is in flight stay dirty.
 * Run: node firmware/KilnFW/App/test/test_live_profile_guard.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const { runPageScript, flush } = require('./_page_vm.js');
const HTML = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'live_profile_page.html'), 'utf8');

let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }
const J = (o, status) => ({ ok: (status || 200) < 300, status: status || 200, text: () => Promise.resolve(JSON.stringify(o)), json: () => Promise.resolve(o) });

function boot(state) {
  const listeners = {};
  const gate = { resolvePost: null };
  const r = runPageScript(HTML, {
    elements: ['themeBtn', 'forkBtn', 'saveBtn', 'reloadBtn', 'saveAsBtn', 'saveAsName', 'overwriteBtn',
      'overwriteConfirm', 'discardBtn', 'segments', 'forkMsg', 'saveMsg', 'decideMsg', 'statusText', 'refusalBanner'],
    extra: {
      setInterval: () => 0,
      kcConfirm: () => Promise.resolve(true),
      addEventListener: (ev, fn) => { listeners[ev] = fn; },
      escapeHtml: (s) => String(s),
      kcIsAuthCancelled: () => false,
      fetch: (url, init) => {
        if (init && init.method === 'POST') {
          if (state.holdPost) return new Promise((res) => { gate.resolvePost = () => res(J({ ok: true })); });
          return Promise.resolve(J({ ok: true }));
        }
        if (url.indexOf('content=1') >= 0) return Promise.resolve(J({ name: 'w', zone_mask: 1, segments: [] }));
        return Promise.resolve(J(state.live));
      },
    },
  });
  r.gate = gate;
  r.prompts = () => { let p = false; listeners.beforeunload({ preventDefault() { p = true; } }); return p; };
  return r;
}
const ACTIVE = (id) => ({ active: true, working_id: id, editable_from_segment: 0, pending_decision: false });

(async () => {
  let st = { live: ACTIVE(5) };
  let r = boot(st); await flush();
  r.els.segments.fire('input');
  ok(r.prompts(), 'edit arms the guard');
  r.els.saveBtn.fire('click'); await flush();
  ok(!r.prompts(), 'L5: guard clears after a successful save');

  st = { live: { active: false, working_id: 5, editable_from_segment: 0, pending_decision: true, origin_is_builtin: false } };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  r.els.discardBtn.fire('click'); await flush();
  ok(!r.prompts(), 'L5: guard clears after a successful decision');

  st = { live: ACTIVE(5) };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  st.live = { active: false, working_id: -1, pending_decision: false };
  await r.sandbox.refreshLive();
  ok(!r.prompts(), 'L3: guard clears when the firing ends / editor goes away');

  st = { live: ACTIVE(5) };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  st.live = { active: true, working_id: -1, pending_decision: false };
  await r.sandbox.refreshLive();
  ok(!r.prompts(), 'L3: guard clears when the working copy disappears');

  st = { live: ACTIVE(5) };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  st.live = ACTIVE(6);
  await r.sandbox.refreshLive(); await flush();
  ok(r.prompts(), 'L3: a changed working copy does not clobber dirty edits (guard stays armed)');
  ok(/NOT reloaded/.test(r.els.saveMsg.innerHTML), 'L3: the operator is told the form was not reloaded');
  r.els.reloadBtn.fire('click'); await flush();
  ok(!r.prompts(), 'L3: explicit Reload discards the edits and clears the guard');

  st = { live: ACTIVE(5), holdPost: true };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  r.els.saveBtn.fire('click'); await flush();
  r.els.segments.fire('input'); // typed while the save is in flight
  r.gate.resolvePost(); await flush();
  ok(r.prompts(), 'L3: edits made while a save is in flight stay dirty');

  process.exit(failed ? 1 : 0);
})();
