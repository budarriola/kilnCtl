/* BENCHFX12 LOW-1: behavioural pin for zones_page.html's Save handler. A zone with every mask
 * checkbox unchecked (masks 0) must still POST z0_thermo_mask=0 and z0_ct_mask=0, because the
 * firmware omit-DEFAULTS an absent mask. Any conditional/early-return form of the mask pushes
 * skips exactly this input. Boots the real inline script via _page_vm.js (like
 * test_unsaved_guard_pages.js) and clicks Save with one stub zone block.
 * Run: node firmware/KilnFW/App/test/test_zones_save_masks.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const { runPageScript, flush, makeEl } = require('./_page_vm.js');

let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }

function jsonResp(obj) {
  return { ok: true, status: 200, json: () => Promise.resolve(obj), text: () => Promise.resolve(JSON.stringify(obj)) };
}

/* Generic stack/field stub: any querySelector yields another stub with value '0'; *src selects
 * read 255 (own settings, no inheritance); mask checkbox lists are empty. */
function stub(value) {
  const e = makeEl('stub');
  e.value = value;
  e.dataset = { index: '0', timingProfile: '0', modelK: '0', modelTau: '0', modelDead: '0', bit: '0' };
  e.querySelector = (q) => stub(/src/.test(q) ? '255' : '0');
  e.querySelectorAll = () => [];
  return e;
}

(async () => {
  const html = fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), 'zones_page.html'), 'utf8');
  const re = /<script>([\s\S]*?)<\/script>/g;
  let m, last = null;
  while ((m = re.exec(html)) !== null) last = m[1];
  const posts = [];
  const zdiv = stub('0');
  const extra = {
    fetch(url, init) {
      if (url.indexOf('/api/zones') === 0) {
        if (init && init.method === 'POST') { posts.push(init.body); return Promise.resolve(jsonResp({ ok: true })); }
        return Promise.resolve(jsonResp({ thermo_count: 1, relay_count: 1, max_simultaneous_relays: 1, zones: [], timing_profiles: [] }));
      }
      return new Promise(() => {});
    },
    kcFetchWithSafetyAck(url, init) { return extra.fetch(url, init); },
    setInterval() { return 0; }, clearInterval() {},
    kcIsAuthCancelled: () => false, kcConfirm: () => Promise.resolve(true),
    kcEscapeHtml: (s) => String(s), matchMedia: () => ({ matches: false }),
    addEventListener() {},
  };
  const sel = new Proxy({}, {
    has: () => true,
    get: (t, q) => (q === 'hasOwnProperty' ? undefined : zdiv),
    getOwnPropertyDescriptor: () => ({ value: zdiv, enumerable: true, configurable: true }),
  });
  const r = runPageScript('<script>' + last + '</script>', { extra, selectors: sel });
  await flush();
  r.els.thermoCount.value = '1'; r.els.relayCount.value = '1'; r.els.maxSimultaneous.value = '1';
  r.els.zones.querySelectorAll = (q) => (q === '.zone' ? [zdiv] : []);
  r.els.saveBtn.fire('click');
  await flush();
  ok(posts.length === 1, 'Save with all-unchecked masks posts to /api/zones (' + posts.length + ' posts, msg: ' + r.els.msg.textContent + ')');
  const body = posts[0] || '';
  const parts = body.split('&');
  ok(parts.indexOf('z0_thermo_mask=0') >= 0, 'POST body carries z0_thermo_mask=0');
  ok(parts.indexOf('z0_ct_mask=0') >= 0, 'POST body carries z0_ct_mask=0');
  ok(parts.indexOf('z0_relay_mask=0') >= 0, 'POST body carries z0_relay_mask=0');
  if (failed) { console.log(failed + ' failed'); process.exit(1); }
  console.log('all passed');
})().catch((e) => { console.log('FAIL: unexpected ' + (e && e.stack || e)); process.exit(1); });
