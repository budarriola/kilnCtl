/* D7 unsaved-edit guard (docs/audits/WEB_JS_COVERAGE_GAPS_2026-10-10.md): runs the
 * real inline script of each editable-form page in a node vm (fake DOM, _page_vm.js)
 * and checks the beforeunload guard: no edit -> no prompt; a real user edit event
 * arms it; a successful save (or re-render from board data) clears it.
 * Run: node firmware/KilnFW/App/test/test_unsaved_guard_pages.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const { runPageScript, flush, fakeResponse } = require('./_page_vm.js');

let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }

function scriptContaining(html, marker) {
  const re = /<script>([\s\S]*?)<\/script>/g;
  let m;
  while ((m = re.exec(html)) !== null) if (m[1].includes(marker)) return '<script>' + m[1] + '</script>';
  throw new Error('no inline script contains ' + marker);
}

function jsonResp(obj, status) {
  const s = status || 200;
  return { ok: s < 300, status: s, json: () => Promise.resolve(obj), text: () => Promise.resolve(JSON.stringify(obj)) };
}

/* routes: {urlPrefix: (init) => response-object}; unmatched fetches never settle. */
function boot(file, marker, routes, extraOpts) {
  const html = scriptContaining(
    fs.readFileSync(resolveDriverFile(resolveDriversDir(__dirname), file), 'utf8'), marker);
  const listeners = {};
  const errors = [];
  const extra = Object.assign({
    addEventListener(ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); },
    fetch(url, init) {
      for (const k of Object.keys(routes)) {
        if (url.indexOf(k) === 0) return Promise.resolve(routes[k](init || {}, url));
      }
      return new Promise(() => {});
    },
    kcFetchWithSafetyAck(url, init) { return extra.fetch(url, init); },
    setInterval() { return 0; }, clearInterval() {},
    kcIsAuthCancelled: () => false,
    kcConfirm: () => Promise.resolve(true),
    kcEscapeHtml: (s) => String(s),
    matchMedia: () => ({ matches: false }),
    window: undefined,
  }, extraOpts && extraOpts.extra);
  delete extra.window;
  const r = runPageScript(html, Object.assign({}, extraOpts, { extra }));
  r.sandbox.window = r.sandbox;
  r.prompts = () => {
    let prevented = false;
    const e = { preventDefault() { prevented = true; }, returnValue: undefined };
    let ret;
    (listeners.beforeunload || []).forEach((f) => { ret = f(e); });
    return prevented;
  };
  r.hasGuard = () => (listeners.beforeunload || []).length === 1;
  return r;
}

(async () => {
  /* ---- safety_config ---- */
  {
    const zonesJson = { thermo_count: 0, relay_count: 0, zones: [], timing_profiles: [], relay_names: [], pc_link_abort_silence_ms: 0 };
    const r = boot('safety_config_page.html', "getElementById('save')", {
      '/api/zones': (init) => (init.method === 'POST' ? jsonResp({ ok: true }) : jsonResp(zonesJson)),
    });
    await flush();
    ok(r.hasGuard(), 'safety_config: exactly one beforeunload guard registered');
    ok(!r.prompts(), 'safety_config: no edit -> no prompt');
    r.els.pcLink.fire('input');
    ok(r.prompts(), 'safety_config: edit arms the guard');
    r.els.save.fire('click');
    await flush();
    ok(!r.prompts(), 'safety_config: successful save clears the guard');
  }

  /* ---- settings_display ---- */
  {
    const r = boot('settings_display_page.html', 'kcDpBrightness', {
      '/api/settings/display_power': (init) => jsonResp(init.method === 'POST' ? { ok: true } :
        { brightness_percent: 50, timeout_setting: 0, keep_on_while_firing: false, display_on_error: false }),
    });
    await flush();
    ok(r.hasGuard(), 'settings_display: exactly one beforeunload guard registered');
    ok(!r.prompts(), 'settings_display: no edit (and board load) -> no prompt');
    r.els.kcDpTimeout.fire('change');
    ok(r.prompts(), 'settings_display: edit arms the guard');
    r.els.kcDpSave.fire('click');
    await flush();
    ok(!r.prompts(), 'settings_display: successful save clears the guard');
  }

  /* ---- kiln_configs ---- */
  {
    const r = boot('kiln_configs_page.html', 'kcSaveNewName', {
      '/api/kiln_configs/save': () => jsonResp({ ok: true }),
      '/api/kiln_configs': () => jsonResp({ active_id: null, configs: [], max_count: 8 }),
    });
    await flush();
    ok(r.hasGuard(), 'kiln_configs: exactly one beforeunload guard registered');
    ok(!r.prompts(), 'kiln_configs: no edit -> no prompt');
    r.els.kcSaveNewName.value = 'My setup';
    r.els.kcSaveNewName.fire('input');
    ok(r.prompts(), 'kiln_configs: typing a name arms the guard');
    r.els.kcSaveNewBtn.fire('click');
    await flush();
    ok(!r.prompts(), 'kiln_configs: successful save (box emptied) clears the guard');
  }

  /* ---- zones ---- */
  {
    const r = boot('zones_page.html', 'zonesFormDirty', {
      '/api/zones': (init) => jsonResp(init.method === 'POST' ? { ok: true } :
        { thermo_count: 0, relay_count: 0, max_simultaneous_relays: 0, zones: [], timing_profiles: [] }),
    });
    await flush();
    ok(r.hasGuard(), 'zones: exactly one beforeunload guard registered');
    ok(!r.prompts(), 'zones: no edit -> no prompt');
    r.els.thermoCount.fire('change');
    ok(r.prompts(), 'zones: edit arms the guard');
    r.els.saveBtn.fire('click');
    await flush();
    ok(!r.prompts(), 'zones: successful save clears the guard');
  }

  /* ---- profiles ---- */
  {
    const r = boot('profiles_page.html', 'function resetEditor', {
      '/api/profile': (init) => jsonResp({ ok: true, id: 3 }),
    }, { groups: { '.pzone-cb': [{ checked: true, value: '0' }] } });
    await flush();
    const rowEl = r.getEl('row');
    r.els.segments.querySelectorAll = (sel) => (sel === '.seg' ? [rowEl] : []);
    ok(r.hasGuard(), 'profiles: exactly one beforeunload guard registered');
    ok(!r.prompts(), 'profiles: no edit -> no prompt');
    r.els.pname.fire('input');
    ok(r.prompts(), 'profiles: edit arms the guard');
    r.els.saveBtn.fire('click');
    await flush();
    ok(!r.prompts(), 'profiles: successful save clears the guard');
  }

  /* ---- setup_wizard ---- */
  {
    const r = boot('setup_wizard_page.html', 'function postStepState(', {
      '/api/setup/progress': () => jsonResp({ ok: true }),
    });
    await flush();
    ok(r.hasGuard(), 'setup_wizard: exactly one beforeunload guard registered');
    ok(!r.prompts(), 'setup_wizard: no edit -> no prompt');
    r.els.stepBody1.fire('input');
    ok(r.prompts(), 'setup_wizard: edit arms the guard');
    await r.sandbox.postStepState(1, 'done');
    ok(!r.prompts(), 'setup_wizard: successful step save clears the guard');
    r.els.stepBody1.fire('change');
    await r.sandbox.postStepState(1, 'done').catch(() => {});
  }

  process.exit(failed ? 1 : 0);
})().catch((e) => { console.log('FAIL: exception ' + (e && e.stack || e)); process.exit(1); });
