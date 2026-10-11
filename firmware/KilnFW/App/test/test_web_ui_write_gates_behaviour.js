/* Behaviour tests (real page handlers, vm + fake DOM) for the write-gating fixes from
 * WEB_UI_JS_AUDIT_2026-10-10 that test_web_ui_js_audit_fixes.js only pinned as source text:
 * L-17 (display Save before load), L-16 (wizard step 7 with unreadable zones), L-8 (live
 * profile decide without a generation), L-12 (wizard step 11 password/username pairing),
 * and REVIEW_WEBFX6 LOW-3 (Clear Trip sticky refusal).
 * Run: node firmware/KilnFW/App/test/test_web_ui_write_gates_behaviour.js */
'use strict';
const fs = require('fs');
const vm = require('vm');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const { runPageScript, flush, makeEl } = require('./_page_vm.js');
const dir = resolveDriversDir(__dirname);
const rd = (f) => fs.readFileSync(resolveDriverFile(dir, f), 'utf8');
let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }
process.on('unhandledRejection', () => {});
const jsonResp = (status, body) => ({ ok: status >= 200 && status < 300, status, json: () => Promise.resolve(body), text: () => Promise.resolve(JSON.stringify(body)) });
function nthScript(html, i) {
  const re = /<script>([\s\S]*?)<\/script>/g; let m, k = 0;
  while ((m = re.exec(html))) { if (k++ === i) return '<script>' + m[1] + '</script>'; }
  throw new Error('no script ' + i);
}
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');

/* ---- L-17: display settings Save is refused until the stored settings loaded ---- */
async function display(loadOk) {
  const posts = [];
  const { els } = runPageScript(nthScript(rd('settings_display_page.html'), 1), {
    elements: ['kcDpBrightness', 'kcDpBrightnessVal', 'kcDpTimeout', 'kcDpKeepOnFiring', 'kcDpOnError', 'kcDpSave', 'kcDpStatus', 'kcDpBrightnessNote', 'themeBtn'],
    extra: {
      fetch: (url, init) => {
        if (init && init.method === 'POST') { posts.push(url); return Promise.resolve(jsonResp(200, { ok: true })); }
        return Promise.resolve(loadOk ? jsonResp(200, { brightness_percent: 40, timeout_setting: 2, keep_on_while_firing: true, display_on_error: false })
                                      : jsonResp(500, {}));
      },
    },
  });
  await flush();
  els.kcDpSave.fire('click');
  await flush();
  return { els, posts };
}

/* ---- L-8: live profile decide needs a known generation ---- */
async function liveDecide(withGen, btn) {
  const posts = [];
  const { els, sandbox } = runPageScript(rd('live_profile_page.html'), {
    elements: ['themeBtn', 'forkBtn', 'saveBtn', 'reloadBtn', 'saveAsBtn', 'saveAsName', 'overwriteBtn',
      'overwriteConfirm', 'discardBtn', 'segments', 'forkMsg', 'saveMsg', 'decideMsg', 'statusText', 'refusalBanner'],
    extra: {
      setInterval: () => 0, escapeHtml: esc, kcConfirm: () => Promise.resolve(true),
      kcIsAuthCancelled: () => false,
      fetch: (url, init) => {
        if (init && init.method === 'POST') { posts.push({ url, body: init.body }); return Promise.resolve(jsonResp(200, { ok: true })); }
        return Promise.resolve(jsonResp(200, { active: false }));
      },
    },
  });
  await flush(5);
  sandbox.lastGen = withGen ? 3 : null;
  els.saveAsName.value = 'x'; els.overwriteConfirm.checked = true;
  els[btn].fire('click');
  await flush();
  return { els, posts };
}

/* ---- setup wizard harness (steps 7 and 11) ---- */
function wizard(responder) {
  const byId = new Map();
  const doc = {
    getElementById(id) {
      if (!byId.has(id)) byId.set(id, makeEl(id, {}));
      return byId.get(id);
    },
    createElement: () => makeEl('x', {}), querySelector: () => null, querySelectorAll: () => [],
    body: makeEl('body', {}), documentElement: makeEl('html', {}),
  };
  const calls = [];
  const win = { kcEscapeHtml: esc, location: { hash: '', pathname: '/' }, addEventListener() {}, kcIsAuthCancelled: () => false };
  win.document = doc;
  const sandbox = {
    document: doc, window: win, location: win.location, console: { log() {}, warn() {}, error() {} },
    localStorage: { getItem: () => null, setItem() {} },
    setInterval: () => 0, clearInterval() {}, setTimeout: () => 0, clearTimeout() {},
    fetch: (url, init) => { calls.push({ url, init }); return Promise.resolve(responder(url, init, calls)); },
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  const html = rd('setup_wizard_page.html').replace(/<!--[\s\S]*?-->/g, (m) => m.replace(/[^\n]/g, ' '));
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  let m, best = null;
  while ((m = re.exec(html))) if (!best || m[1].length > best.length) best = m[1];
  new vm.Script(best, { filename: 'wizard' }).runInContext(sandbox);
  return { doc, sandbox, calls };
}

async function wizStep7(zonesSecondOk) {
  let zonesCalls = 0;
  const w = wizard((url) => {
    if (url === '/api/safety/commissioning') return jsonResp(200, { params: [], link_up: true });
    if (url === '/api/zones') { zonesCalls++; return (zonesCalls === 1 || zonesSecondOk) ? jsonResp(200, { thermo_count: 1, zones: [{ max_temp_c: 1000 }] }) : jsonResp(500, {}); }
    return jsonResp(200, {});
  });
  await w.sandbox.renderStep7();
  w.doc.getElementById('s7TcType').value = '1';
  w.doc.getElementById('s7AbsMax').value = '1300';
  w.doc.getElementById('s7CtInstalled').value = '0';
  const before = w.calls.length;
  w.doc.getElementById('step7Save').fire('click');
  await flush();
  return { errHtml: w.doc.getElementById('step7Err').innerHTML, after: w.calls.slice(before).map((c) => c.url) };
}

async function wizStep11(user, pass, adminUser, enable) {
  const w = wizard((url) => {
    if (url === '/api/auth/config') return jsonResp(200, { web_enabled: false, admin_password_set: true, admin_username: adminUser });
    return jsonResp(200, { ok: true });
  });
  await w.sandbox.renderStep11();
  w.doc.getElementById('s11User').value = user;
  w.doc.getElementById('s11Pass').value = pass;
  w.doc.getElementById('step11Enable').checked = !!enable;
  const before = w.calls.length;
  w.doc.getElementById('step11Save').fire('click');
  await flush();
  return { err: w.doc.getElementById('step11Err').innerHTML, posts: w.calls.slice(before).filter((c) => c.init && c.init.method === 'POST') };
}

(async () => {
  let r = await display(false);
  ok(r.posts.length === 0 && /not loaded yet/.test(r.els.kcDpStatus.textContent), 'L-17: Save before a successful load sends nothing');
  r = await display(true);
  ok(r.posts.length === 1, 'L-17: Save after a successful load posts');

  for (const b of ['saveAsBtn', 'overwriteBtn', 'discardBtn']) {
    r = await liveDecide(false, b);
    ok(r.posts.length === 0 && /Still loading/.test(r.els.decideMsg.textContent), 'L-8: ' + b + ' without a generation sends nothing');
  }
  r = await liveDecide(true, 'discardBtn');
  ok(r.posts.length === 1 && /gen=3/.test(r.posts[0].url), 'L-8: discard with a generation posts with ?gen=');

  let s = await wizStep7(false);
  ok(/Could not re-read the zone limits/.test(s.errHtml) && !s.after.includes('/api/safety/commissioning'), 'L-16: unreadable /api/zones refuses step 7 save (calls ' + s.after + ')');
  s = await wizStep7(true);
  ok(!/Could not re-read/.test(s.errHtml) && s.after.includes('/api/safety/commissioning'), 'L-16: readable zones proceeds to the commit path');

  let t = await wizStep11('', 'secret', '', false);
  ok(/username that goes with this password/.test(t.err) && t.posts.length === 0, 'L-12: password without username refuses, nothing POSTed');
  t = await wizStep11('newname', '', 'oldname', false);
  ok(/changed username needs the password/.test(t.err) && t.posts.length === 0, 'L-12: changed username without password refuses');
  t = await wizStep11('admin', 'secret', 'admin', true);
  ok(t.posts.length >= 1 && t.posts[0].init.body.indexOf('cmd=set_web_password') >= 0, 'L-12: username+password proceeds to POST');

  /* REVIEW_WEBFX6 LOW-3: Clear Trip sticky refusal */
  const m = runPageScript(rd('main_page.html'), { elements: ['safetyTripBanner'], extra: { setInterval: () => 0, kcEscapeHtml: esc, fetch: () => Promise.resolve(jsonResp(200, {})) } });
  const sb = m.sandbox;
  vm.runInContext('this.__tripState = SAFETY_LINK_DIAG_STATE_TRIPPED', sb);
  const td = (reason) => ({ diag_ever_received: true, diag_state: sb.__tripState, diag_age_ms: 10, diag_trip_reason: reason });
  let resp = { ok: false, status: 200, body: { ok: false, reason: 'cause still present', error: 'x' } };
  sb.fetch = () => Promise.resolve({ ok: resp.ok, status: resp.status, text: () => Promise.resolve(JSON.stringify(resp.body)) });
  const banner = () => m.els.safetyTripBanner.innerHTML;
  async function click() {
    sb.renderSafetyTrip(td(3));
    const b = m.els.clearTripBtn;
    b.insertAdjacentHTML = () => {};
    b.fire('click');
    await flush();
  }
  await click();
  sb.renderSafetyTrip(td(3));
  ok(/cause still present/.test(banner()), 'LOW-3: refusal shows reason before error and survives a re-render');
  resp = { ok: true, status: 200, body: { ok: true } };
  await click();
  sb.renderSafetyTrip(td(3));
  ok(!/cause still present/.test(banner()), 'LOW-3: a later success clears the sticky refusal');
  resp = { ok: false, status: 200, body: { ok: false, error: 'again' } };
  await click();
  sb.renderSafetyTrip(td(3));
  ok(/again/.test(banner()), 'LOW-3: refusal shown again');
  sb.renderSafetyTrip(td(4));
  ok(!/again/.test(banner()), 'LOW-3: a different trip reason drops the old refusal');

  console.log(failed ? failed + ' FAILED' : 'all passed');
  process.exit(failed ? 1 : 0);
})().catch((e) => { console.log('FAIL: exception ' + e.stack); process.exit(1); });
