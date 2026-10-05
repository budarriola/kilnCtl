/* Node-only test for the spare-relay WP-5 web UI (docs/SPARE_RELAY_ONOFF_PLAN.md):
 *   - zones_page.html's aux-outputs editor: client-side validation
 *     (auxValidate) and the exact POST /api/aux_outputs form body
 *     (auxFormBody), mirroring aux_outputs_http_core.c's field names/limits.
 *   - main_page.html's dashboard manual toggle: hidden for a non-admin
 *     (owner rule: unauthenticated = dashboards only), disabled with the
 *     server's own 409 text while a firing/autotune owns the relays, and
 *     refusing (client-side) an out-of-range relay or a relay that is not an
 *     enabled aux output.
 * All functions are extracted VERBATIM from the shipped pages.
 *
 * Run: node firmware/KilnFW/App/test/test_aux_outputs_ui.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');
const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');

const DIR = resolveDriversDir(__dirname);
function lines(file) {
  return fs.readFileSync(resolveDriverFile(DIR, file), 'utf8').split('\n').map((l) => l.replace(/\r$/, ''));
}
/* A line starting with `prefix`; a multi-line function runs to the next bare "}" at column 0. */
function extract(L, prefix) {
  const i = L.findIndex((l) => l.startsWith(prefix));
  if (i === -1) throw new Error('not found: ' + prefix);
  if (prefix.startsWith('var ')) return L[i];
  const e = L.findIndex((l, k) => k >= i && l === '}');
  if (e === -1) throw new Error('no end for: ' + prefix);
  return L.slice(i, e + 1).join('\n');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// ---------------------------------------------------------------------------
// zones page: validation + wire body
// ---------------------------------------------------------------------------
const Z = lines('zones_page.html');
const ZSRC = ['var AUX_COUNT', 'var AUX_HYST_MIN', 'var AUX_SECS_MIN', 'function auxValidate', 'function auxFormBody',
  'function auxCyclesText', 'function auxRowHtml', 'function auxZoneNames', 'function renderAuxOutputs',
  'function readAuxRow']
  .map((p) => extract(Z, p)).join('\n');
const zsb = { RELAY_NAMES: ['Relay 1', 'Relay 2', 'Relay 3', 'Relay 4'], window: { kcEscapeHtml: (x) => String(x) } };
vm.createContext(zsb);
vm.runInContext(ZSRC, zsb);

const good = { relay: 2, enabled: 1, tc_zone: 1, hyst_c: 2, min_on_s: 30, min_off_s: 30 };
assert(zsb.auxValidate(good, 3) === '', 'a valid aux row passes client validation');
assert(zsb.auxFormBody(good) === 'relay=2&enabled=1&tc_zone=1&hyst_c=2&min_on_s=30&min_off_s=30',
  'POST body uses the exact aux_outputs_http_core.c field names');
assert(zsb.auxFormBody({ relay: 1, enabled: 0, tc_zone: -1, hyst_c: 0.5, min_on_s: 1, min_off_s: 3600 }) ===
  'relay=1&enabled=0&tc_zone=-1&hyst_c=0.5&min_on_s=1&min_off_s=3600', 'none (-1) and boundary values serialize unchanged');

function bad(over) { return zsb.auxValidate(Object.assign({}, good, over), 3) !== ''; }
assert(bad({ relay: 0 }) && bad({ relay: 5 }) && bad({ relay: 1.5 }) && bad({ relay: NaN }),
  'NEGATIVE: relay 0, 5, 1.5 and NaN are rejected client-side');
assert(bad({ enabled: 2 }), 'NEGATIVE: enabled other than 0/1 is rejected');
assert(bad({ tc_zone: 3 }) && bad({ tc_zone: -2 }) && bad({ tc_zone: NaN }),
  'NEGATIVE: tc_zone beyond thermo_count-1, below -1 or NaN is rejected');
assert(zsb.auxValidate(Object.assign({}, good, { tc_zone: 0 }), 0) !== '',
  'NEGATIVE: any thermocouple zone is rejected when there are no thermocouples');
assert(bad({ hyst_c: 0.4 }) && bad({ hyst_c: 25.1 }) && bad({ hyst_c: NaN }), 'NEGATIVE: hysteresis outside 0.5..25 is rejected');
assert(!bad({ hyst_c: 0.5 }) && !bad({ hyst_c: 25 }), 'hysteresis boundaries 0.5 and 25 are accepted');
assert(bad({ min_on_s: 0 }) && bad({ min_on_s: 3601 }) && bad({ min_on_s: 1.5 }), 'NEGATIVE: min_on_s outside 1..3600 or fractional is rejected');
assert(bad({ min_off_s: 0 }) && bad({ min_off_s: 3601 }), 'NEGATIVE: min_off_s outside 1..3600 is rejected');
assert(zsb.auxValidate(null, 3) !== '', 'NEGATIVE: a missing row is rejected');

// ---------------------------------------------------------------------------
// dashboard: toggle visibility / disabled / refusals
// ---------------------------------------------------------------------------
const M = lines('main_page.html');
const MSRC = ['var AUX_BLOCKED_TEXT', 'var auxDash', 'function auxBlockedReason', 'function auxRelayIsOn',
  'function auxManualProblem', 'function renderAuxCard', 'function hideAuxDash', 'function loadAuxDash']
  .map((p) => extract(M, p)).join('\n');

function mkDash(opts) {
  const card = { style: { display: 'none' }, innerHTML: '' };
  const msg = { textContent: '' };
  const sb = {
    window: { kcEscapeHtml: (s) => String(s) },
    isFiringActive: (s) => s === 'running' || s === 'paused',
    isAutotuneActive: (s) => s === 'settling' || s === 'stepping' || s === 'relay_approach' || s === 'relay_cycling',
    lastExecStData: opts.exec || null,
    lastAutotuneStatus: opts.autotune || null,
    lastChannelsData: { relays: opts.relays || [{ relay: 1, on: false }, { relay: 2, on: true }] },
    zonesCache: { relay_names: ['Vent', 'Fan', '', ''] },
    document: { getElementById: (id) => (id === 'auxCard' ? card : id === 'auxCardMsg' ? msg : null) },
    fetch: opts.fetch || (() => Promise.reject(new Error('no fetch stub'))),
  };
  vm.createContext(sb);
  vm.runInContext(MSRC, sb);
  sb.auxDash.admin = opts.admin !== false;
  sb.auxDash.relays = opts.aux || [{ relay: 1, enabled: true }, { relay: 2, enabled: true }];
  sb.renderAuxCard();
  return { sb, card };
}

let d = mkDash({ admin: false });
assert(d.card.style.display === 'none' && d.card.innerHTML === '',
  'NEGATIVE: a non-admin (aux GET refused) sees no card and no write controls');
d = mkDash({ aux: [] });
assert(d.card.style.display === 'none', 'admin with no enabled aux output: card stays hidden');

d = mkDash({});
assert(d.card.style.display === '' && /aux-toggle/.test(d.card.innerHTML), 'admin + enabled aux outputs + idle: toggles shown');
assert(!/disabled/.test(d.card.innerHTML), 'idle: toggles are enabled');
assert(/Vent<\/b>: OFF/.test(d.card.innerHTML) && /Fan<\/b>: ON/.test(d.card.innerHTML),
  'state comes from the /api/status relays array (relay 1 off, relay 2 on)');
assert(/data-relay="1" data-on="1"/.test(d.card.innerHTML) && /data-relay="2" data-on="0"/.test(d.card.innerHTML),
  'an OFF relay posts on=1, an ON relay posts on=0');

d = mkDash({ exec: { state: 'running' } });
assert((d.card.innerHTML.match(/disabled/g) || []).length === 2 && /manual relay control is not available until it ends/.test(d.card.innerHTML),
  'NEGATIVE: mid-firing, every toggle is disabled and the 409 text is shown');
d = mkDash({ exec: { state: 'paused' } });
assert(/disabled/.test(d.card.innerHTML), 'NEGATIVE: a paused firing also disables the toggles');
d = mkDash({ autotune: { state: 'relay_cycling' } });
assert(/disabled/.test(d.card.innerHTML), 'NEGATIVE: mid-autotune disables the toggles');
d = mkDash({ exec: { state: 'done' }, autotune: { state: 'idle' } });
assert(!/disabled/.test(d.card.innerHTML), 'finished/idle states do not disable the toggles');
d = mkDash({ relays: [] });
assert(/disabled/.test(d.card.innerHTML) && /unknown/.test(d.card.innerHTML),
  'NEGATIVE: an unknown relay state disables the toggle rather than guessing');

d = mkDash({});
assert(d.sb.auxManualProblem(1, 1) === '', 'idle manual toggle of an enabled aux relay is allowed');
assert(d.sb.auxManualProblem(0, 1) !== '' && d.sb.auxManualProblem(5, 1) !== '' && d.sb.auxManualProblem(1.5, 1) !== '',
  'NEGATIVE: an invalid relay index is rejected client-side');
assert(d.sb.auxManualProblem(1, 2) !== '', 'NEGATIVE: on other than 0/1 is rejected');
assert(d.sb.auxManualProblem(3, 1) !== '', 'NEGATIVE: a relay that is not an enabled aux output (e.g. a zone relay) is refused');
d = mkDash({ exec: { state: 'running' } });
assert(d.sb.auxManualProblem(1, 1) !== '', 'NEGATIVE: mid-run the toggle is refused before any POST');

// ---------------------------------------------------------------------------
// review fixes
// ---------------------------------------------------------------------------
// (1) The page's blocked text must be system_mode_gate.c's real mid-run 409 reason.
const gateSrc = fs.readFileSync(resolveDriverFile(DIR, 'system_mode_gate.c'), 'utf8').replace(/\r/g, '');
const gm = /snprintf\(reason, reason_cap,\s*((?:"[^"\n]*"\s*)+)\);/.exec(
  gateSrc.slice(gateSrc.indexOf('if (snap->profile_running || snap->autotune_running)')));
const gateText = gm ? gm[1].match(/"([^"\n]*)"/g).map((q) => q.slice(1, -1)).join('') : null;
assert(gateText !== null && gateText.startsWith('refused -- a firing or autotune run is active'),
  'sanity: parsed the mid-run reason out of system_mode_gate.c');
d = mkDash({});
assert(gateText !== null && d.sb.AUX_BLOCKED_TEXT === gateText,
  'the page AUX_BLOCKED_TEXT is verbatim the system_mode_gate.c mid-run 409 reason');

// (3) a stored tc_zone beyond the live thermocouple count stays selectable (not silently none).
const orphanRow = zsb.auxRowHtml(1, { enabled: true, conflicted: false, tc_zone: 2, hyst_c: 2, min_on_s: 30, min_off_s: 30 },
  false, ['A', 'B'], '');
assert(/<option value="2" selected>/.test(orphanRow) && !/<option value="-1" selected>/.test(orphanRow),
  'NEGATIVE: tc_zone 2 with 2 thermocouples keeps a selected orphan option, not "none"');
const okRow = zsb.auxRowHtml(1, { enabled: true, conflicted: false, tc_zone: 1, hyst_c: 2, min_on_s: 30, min_off_s: 30 },
  false, ['A', 'B'], '');
assert(!/no such thermocouple/.test(okRow), 'an in-range tc_zone adds no orphan option');

// (7) re-render with preserve keeps unsaved row edits.
function fakeRow(relay) {
  const f = {
    '.aux-enabled': { checked: false }, '.aux-tc': { value: '-1' }, '.aux-hyst': { value: '2' },
    '.aux-minon': { value: '30' }, '.aux-minoff': { value: '30' }, '.aux-cycles': { textContent: '' },
  };
  return { dataset: { relay: String(relay) }, className: '', set innerHTML(v) {}, querySelector: (q) => f[q] };
}
const auxEl = {
  rows: [], set innerHTML(v) { this.rows = []; }, appendChild(r) { this.rows.push(r); },
  querySelectorAll() { return this.rows; },
};
const rsb = {
  AUX_COUNT: 4, auxState: { loaded: true, quarantined: false, relays: [], enabledMask: 0 },
  window: { kcEscapeHtml: (x) => String(x) }, current: { zones: [], relay_names: [] },
  clientOwnedRelayMask: () => 0, applyAuxZoneGreying: () => {}, auxRowHtml: () => '', auxZoneNames: () => [],
  auxCyclesText: zsb.auxCyclesText, readAuxRow: zsb.readAuxRow, parseInt, parseFloat, isNaN, String, Array,
  document: {
    createElement: () => fakeRow(0),
    getElementById: (id) => (id === 'auxOutputs' ? auxEl : { value: '4' }),
  },
};
vm.createContext(rsb);
vm.runInContext(extract(Z, 'function renderAuxOutputs'), rsb);
// createElement must hand back a distinct row per call; relay is set by dataset afterwards.
rsb.document.createElement = () => fakeRow(0);
rsb.renderAuxOutputs();
const r2 = auxEl.rows[1];
r2.querySelector('.aux-enabled').checked = true;
r2.querySelector('.aux-hyst').value = '7.5';
r2.querySelector('.aux-tc').value = '1';
rsb.renderAuxOutputs(true);
const r2b = auxEl.rows[1];
assert(auxEl.rows.length === 4 && r2b.querySelector('.aux-enabled').checked === true &&
  r2b.querySelector('.aux-hyst').value === 7.5 && r2b.querySelector('.aux-tc').value === '1',
  'a preserving re-render keeps the unsaved enabled/hysteresis/zone edits of row 2');
rsb.renderAuxOutputs();
assert(auxEl.rows[1].querySelector('.aux-enabled').checked === false,
  'NEGATIVE: a plain re-render (after a load) rebuilds from the stored state');

// (4) session loss hides the card; app.js dispatches the event; main_page listens.
d = mkDash({});
assert(d.card.style.display === '', 'precondition: card visible for an admin');
d.sb.hideAuxDash();
assert(d.card.style.display === 'none' && d.card.innerHTML === '' && d.sb.auxDash.admin === false,
  'hideAuxDash() (kc-logout) hides the card and clears the controls');
const appSrc = fs.readFileSync(resolveDriverFile(DIR, 'app.js'), 'utf8').replace(/\r/g, '');
const noneBranch = appSrc.slice(appSrc.indexOf("lastKnownRole !== 'none' && role === 'none'"));
assert(/^[^}]*new CustomEvent\('kc-logout'\)/.test(noneBranch.slice(0, 400)),
  'app.js pollSession dispatches kc-logout when the role goes real -> none');
assert(/window\.addEventListener\('kc-logout', hideAuxDash\);/.test(M.join('\n')),
  'main_page.html listens for kc-logout');

// (6) loadAuxDash with a stubbed 401 (what a logged-out session really returns).
const asyncTests = [];
asyncTests.push((async () => {
  const d401 = mkDash({ fetch: () => Promise.resolve({ ok: false, status: 401, json: () => Promise.resolve({}) }) });
  assert(d401.card.style.display === '', 'precondition: admin card visible before the 401 reload');
  await d401.sb.loadAuxDash();
  assert(d401.card.style.display === 'none' && d401.card.innerHTML === '' && d401.sb.auxDash.admin === false,
    'NEGATIVE: loadAuxDash() against a 401 hides the card and renders no controls');
  const dOk = mkDash({ admin: false, fetch: () => Promise.resolve({ ok: true, status: 200,
    json: () => Promise.resolve({ relays: [{ relay: 1, enabled: true, conflicted: false }, { relay: 2, enabled: true, conflicted: true },
      { relay: 3, enabled: false, conflicted: false }] }) }) });
  await dOk.sb.loadAuxDash();
  assert(dOk.card.style.display === '' && /data-relay="1"/.test(dOk.card.innerHTML) && !/data-relay="2"/.test(dOk.card.innerHTML) &&
    !/data-relay="3"/.test(dOk.card.innerHTML),
    'a 200 shows only enabled, non-conflicted aux relays');
})());

Promise.all(asyncTests).then(() => {
  console.log('');
  console.log(passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
  process.exit(0);
});
