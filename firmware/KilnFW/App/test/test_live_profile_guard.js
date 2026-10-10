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
  const r = { posts: [] };
  Object.assign(r, runPageScript(HTML, {
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
          r.posts.push(url);
          if (state.holdPost) return new Promise((res) => { gate.resolvePost = () => res(J({ ok: true, generation: 9 })); });
          if (state.postStatus) return Promise.resolve(J({ ok: false, error: 'working copy changed elsewhere -- reload' }, state.postStatus));
          return Promise.resolve(J({ ok: true, generation: 9 }));
        }
        if (url.indexOf('content=1') >= 0) {
          if (state.holdContent) return new Promise((res) => { gate.resolveContent = () => res(J({ name: 'w', zone_mask: 1, segments: [] })); });
          return Promise.resolve(J({ name: 'w', zone_mask: 1, segments: [] }));
        }
        return Promise.resolve(J(state.live));
      },
    },
  }));
  r.gate = gate;
  r.prompts = () => { let p = false; listeners.beforeunload({ preventDefault() { p = true; } }); return p; };
  return r;
}
const ACTIVE = (id, gen) => ({ active: true, working_id: id, generation: gen === undefined ? 1 : gen, editable_from_segment: 0, pending_decision: false });

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
  st.live = ACTIVE(5, 2); // same id, newer generation (M1)
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

  // M1: save/decide echo the generation; the board's 409 keeps the guard armed.
  st = { live: ACTIVE(5, 4) };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  st.live = ACTIVE(5, 9); // the board reports the generation our save produces
  r.els.saveBtn.fire('click'); await flush();
  ok(/\?gen=4$/.test(r.posts[0]), 'M1: save sends the loaded generation');
  r.els.discardBtn.fire('click'); await flush();
  ok(/decide\?gen=9$/.test(r.posts[1]), 'M1: decide sends the generation adopted from the save response');

  st = { live: ACTIVE(5, 4), postStatus: 409 };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  r.els.saveBtn.fire('click'); await flush();
  ok(r.prompts(), 'M1: a 409 stale save keeps the guard armed');
  ok(/changed elsewhere/.test(r.els.saveMsg.innerHTML), 'M1: a 409 stale save shows the board text');

  // L1: own save with edits typed in flight: no misleading Reload banner, still dirty.
  st = { live: ACTIVE(5), holdPost: true };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  r.els.saveBtn.fire('click'); await flush();
  r.els.segments.fire('input');
  st.live = ACTIVE(5, 9); // the board now reports the generation our save produced
  r.gate.resolvePost(); await flush();
  await r.sandbox.refreshLive(); await flush();
  ok(!/NOT reloaded/.test(r.els.saveMsg.innerHTML), 'L1: no "press Reload" banner after our own save');
  ok(/not saved yet/.test(r.els.saveMsg.textContent), 'L1: tells the operator the later edits are unsaved');
  ok(r.prompts(), 'L1: later edits keep the guard armed');

  // L2: edits typed while the working copy is being (re)loaded are not overwritten.
  st = { live: ACTIVE(5), holdContent: true };
  r = boot(st); await flush();
  r.els.segments.fire('input');
  r.gate.resolveContent(); await flush();
  ok(r.prompts(), 'L2: edits typed during the content fetch stay dirty');

  process.exit(failed ? 1 : 0);
})();
