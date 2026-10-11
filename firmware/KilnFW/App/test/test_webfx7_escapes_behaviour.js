/* Behaviour tests for the REVIEW_WEBFX7 LOW-2 escapes: hostile strings fed into numeric fields of
 * profiles_page.html (rule fields, segment table) and zones_page.html (autotune status line) must not
 * create elements. Real page functions run in a vm against a fake DOM.
 * Run: node firmware/KilnFW/App/test/test_webfx7_escapes_behaviour.js */
'use strict';
const fs = require('fs');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const { runPageScript, flush, makeEl } = require('./_page_vm.js');
const dir = resolveDriversDir(__dirname);
const rd = (f) => fs.readFileSync(resolveDriverFile(dir, f), 'utf8');
let failed = 0;
function ok(c, l) { if (c) console.log('PASS: ' + l); else { failed++; console.log('FAIL: ' + l); } }
process.on('unhandledRejection', () => {});

const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const jsonResp = (body) => ({ ok: true, status: 200, json: () => Promise.resolve(body), text: () => Promise.resolve(JSON.stringify(body)) });
const EVIL = '<img src=x onerror=1>';
/* every querySelector on a fake element answers "absent" (null): these tests only call pure render helpers */
const detailsEl = makeEl('details', {});
const anySel = new Proxy({}, { get: (t, k) => (k === 'details' ? detailsEl : null), getOwnPropertyDescriptor: () => ({ value: null, enumerable: true, configurable: true, writable: true }) });
const kcUnit = { toDisplay: (x) => x, label: () => 'C', fmt: (x) => String(x), fromDisplay: (x) => x };

async function profilesPage() {
  const { sandbox, els } = runPageScript(rd('profiles_page.html'), {
    selectors: anySel,
    extra: {
      setInterval: () => 0, kcEscapeHtml: esc, escapeHtml: esc, kcUnit, kcConfirm: () => Promise.resolve(true),
      kcIsAuthCancelled: () => false, fetch: () => Promise.resolve(jsonResp({})),
    },
  });
  await flush(5);
  els.segments.children = [];
  return sandbox;
}

async function zonesPage() {
  const { els, sandbox } = runPageScript(rd('zones_page.html'), {
    selectors: anySel,
    extra: {
      setInterval: () => 0, kcEscapeHtml: esc, escapeHtml: esc, kcUnit, kcConfirm: () => Promise.resolve(true),
      kcIsAuthCancelled: () => false, fetch: () => Promise.resolve(jsonResp({})),
    },
  });
  await flush(5);
  return { els, sandbox };
}

(async () => {
  const p = await profilesPage();

  /* rule fields: temp / time window go into value="..." */
  const rule = { zone: 1, segment: 0, temp_source: 1, temp_cmp: 1, temp_c: '"><img src=x onerror=1>', time_start_s: EVIL, time_stop_s: '"><b>' };
  const html = p.ooRuleFieldsHtml(rule);
  ok(html.indexOf('<img') < 0 && html.indexOf('<b>') < 0, 'W2: ooRuleFieldsHtml escapes tempC/startS/stopS (no element from a hostile string)');
  ok(/class="oo-tempval" value="0"/.test(html) && /class="oo-start" value="0"/.test(html), 'W2: hostile numeric fields collapse to 0');
  const good = p.ooRuleFieldsHtml({ zone: 1, temp_source: 1, temp_cmp: 1, temp_c: 450, time_start_s: 30, time_stop_s: 60 });
  ok(/oo-tempval" value="450"/.test(good) && /oo-start" value="30"/.test(good) && /oo-stop" value="60"/.test(good), 'W2: good numbers still round-trip');

  /* stale zone option */
  const zopts = p.ooZoneOptionsHtml('<img src=x>');
  ok(zopts.indexOf('<img') < 0, 'W2: ooZoneOptionsHtml stale option never emits an element from a hostile selected value');

  /* segment table dwell */
  const tbl = p.segmentTableHtml([{ ramp_c_per_hr: 100, target_c: 500, dwell_min: EVIL }]);
  ok(tbl.indexOf('<img') < 0, 'W2: segmentTableHtml escapes dwell_min');
  ok(/<td>7<\/td><\/tr>/.test(p.segmentTableHtml([{ ramp_c_per_hr: 100, target_c: 500, dwell_min: 7 }])), 'W2: good dwell still shown');

  /* zones autotune status line */
  const z = await zonesPage();
  const s = {
    state: 'done', method: 'relay', relay_valid: true, zone: EVIL, elapsed_s: EVIL, sample_count: EVIL,
    relay_cycles_used: EVIL, rule: EVIL, relay_ku: 1, relay_tu_s: 60, relay_amplitude_c: 1,
    proposed_kp: 1, proposed_ki: 1, proposed_kd: 1, actual_valid: false, model_valid: false,
  };
  z.sandbox.fetch = () => Promise.resolve(jsonResp(s));
  z.sandbox.pollAutotune();
  await flush(10);
  const at = z.els.atStatus.innerHTML;
  ok(at.indexOf('Measured Ku') >= 0, 'W1: the relay-done autotune line rendered (harness reaches the code)');
  ok(at.indexOf('<img') < 0, 'W1: zone/elapsed_s/sample_count/relay_cycles_used/rule cannot inject an element');

  console.log(failed ? failed + ' FAILED' : 'all passed');
  process.exit(failed ? 1 : 0);
})().catch((e) => { console.log('FAIL: exception ' + e.stack); process.exit(1); });
