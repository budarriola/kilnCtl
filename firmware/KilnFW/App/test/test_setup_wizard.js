/* Node-only test harness for setup_wizard_page.html's stepper state machine
 * (SETUP_WIZARD_PLAN.md, implementation step 3): computeStepState(),
 * mergeAllSteps(), computeCompleteness(), pickResumeStep().
 *
 * Same extraction technique test_guided_flow.js uses for
 * safety_commissioning_page.html: pull the page's real inline <script> out
 * of the HTML and run it in a Node vm context, so the functions under test
 * are the actual page functions, not a reimplementation of what they are
 * supposed to do.
 *
 * Run: node firmware/KilnFW/App/test/test_setup_wizard.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'setup_wizard_page.html');

function extractInlineScript(html) {
  const noComments = html.replace(/<!--[\s\S]*?-->/g, (m) => m.replace(/[^\n]/g, ' '));
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  let m;
  let best = null;
  while ((m = re.exec(noComments))) {
    // Same "take the longest script-without-src block" rule test_guided_flow.js
    // uses -- this page also has a tiny early-theme IIFE ahead of the real one.
    if (!best || m[1].length > best.length) best = m[1];
  }
  if (!best) throw new Error('could not find the inline <script> block in ' + PAGE_PATH);
  return best;
}

function makeElement(id) {
  const listeners = {};
  return {
    id: id || '', value: '', textContent: '', innerHTML: '', className: '', hidden: false,
    style: {}, dataset: {},
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    children: [],
    addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
    appendChild(child) { this.children.push(child); return child; },
    setAttribute() {}, getAttribute() { return null; }, removeAttribute() {},
  };
}

function makeDocument() {
  const byId = new Map();
  return {
    getElementById(id) {
      if (!byId.has(id)) byId.set(id, makeElement(id));
      return byId.get(id);
    },
    createElement() { return makeElement(); },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    body: makeElement('body'),
    documentElement: makeElement('documentElement'),
  };
}

function makeFetch(responder) {
  return function fetch(url, opts) {
    const r = responder(url, opts);
    return Promise.resolve({ ok: r.ok, status: r.status, json: () => Promise.resolve(r.body) });
  };
}

function buildContext(fetchImpl, hash) {
  const document = makeDocument();
  const localStorageData = {};
  const sandbox = {
    document,
    window: { kcEscapeHtml: (s) => String(s), location: { hash: hash || '', pathname: '/' } },
    location: { hash: hash || '', pathname: '/' },
    fetch: fetchImpl,
    localStorage: {
      getItem: (k) => (k in localStorageData ? localStorageData[k] : null),
      setItem: (k, v) => { localStorageData[k] = v; },
    },
    console,
    setInterval: (fn, ms) => { const t = setInterval(fn, ms); if (t.unref) t.unref(); return t; },
    clearInterval,
    setTimeout: (fn, ms) => { const t = setTimeout(fn, ms); if (t.unref) t.unref(); return t; },
    clearTimeout,
  };
  sandbox.window.document = document;
  sandbox.window.addEventListener = () => {};
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  return sandbox;
}

function loadPageScript(fetchImpl, hash) {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  const code = extractInlineScript(html);
  const ctx = buildContext(fetchImpl, hash);
  new vm.Script(code, { filename: 'setup_wizard_page.html (inline script)' }).runInContext(ctx);
  return ctx;
}

function runMicrotasks() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

// ---- Test scaffolding -------------------------------------------------
let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

function readinessOf(items) { return { items }; }
function item(key, status, detail) { return { key: key, label: key, status: status, detail: detail || '' }; }

// A fetch stub that never actually gets called by these tests -- all pure-
// function tests below call computeStepState/mergeAllSteps/computeCompleteness/
// pickResumeStep directly off the loaded context, not through loadAll()'s
// fetch chain.
const noopFetch = makeFetch(() => ({ ok: true, status: 200, body: { items: [] } }));

(function testPureFunctionsReachable() {
  const ctx = loadPageScript(noopFetch);
  ['computeStepState', 'mergeAllSteps', 'computeCompleteness', 'pickResumeStep', 'WIZARD_STEPS']
    .forEach((name) => {
      assert(ctx[name] !== undefined, name + '() is reachable from the extracted script');
    });
})();

// ---- done/pending/skipped/regressed rendering --------------------------
(function testStepStates() {
  const ctx = loadPageScript(noopFetch);
  const step = { id: 6, title: 'Zone limits', readinessKeys: ['guard_max_temp'], safety: true };

  const pendingByKey = { guard_max_temp: item('guard_max_temp', 'not_done', 'zone 1 has no ceiling') };
  const pending = ctx.computeStepState(step, undefined, pendingByKey);
  assert(pending.state === 'pending', 'no stored state + not_done readiness -> pending');

  const doneByKey = { guard_max_temp: item('guard_max_temp', 'ok') };
  const done = ctx.computeStepState(step, { state: 'done' }, doneByKey);
  assert(done.state === 'done', 'stored done + readiness ok -> done');

  const skipped = ctx.computeStepState(step, { state: 'skipped', note: 'deferred' }, doneByKey);
  assert(skipped.state === 'skipped', 'stored skipped stays skipped when readiness is ok');
  assert(skipped.detail === 'deferred', 'skipped detail carries the stored note');

  // ---- THE core anti-drift property: stored done + readiness now not_done
  // must render REGRESSED, never done.
  const regressed = ctx.computeStepState(step, { state: 'done' }, pendingByKey);
  assert(regressed.state === 'regressed', 'stored done + readiness not_done -> REGRESSED, not done');
  assert(/guard_max_temp/.test(regressed.detail), 'regressed detail names the offending readiness item');
})();

// ---- readiness overriding stored state (both directions) --------------
(function testReadinessOverridesStored() {
  const ctx = loadPageScript(noopFetch);
  const step = { id: 1, title: 'Network', readinessKeys: ['network'], safety: false };

  // Never visited in the wizard (no stored entry at all), but the board
  // already reports it configured -- readiness alone should be enough.
  const neverVisitedButOk = ctx.computeStepState(step, undefined, { network: item('network', 'ok') });
  assert(neverVisitedButOk.state === 'done', 'unvisited step with ok readiness renders done');

  // A step with no mapped readiness key at all (e.g. step 0) must never be
  // forced to "done" by the (vacuous) allOkOrOff check.
  const noKeysStep = { id: 0, title: 'Welcome', readinessKeys: [], safety: false };
  const noKeys = ctx.computeStepState(noKeysStep, undefined, {});
  assert(noKeys.state === 'pending', 'a step with no readiness keys and no stored state stays pending');
})();

// ---- resume-after-reload -------------------------------------------------
(function testResumeAfterReload() {
  const ctx = loadPageScript(noopFetch);
  const progress = { version: 1, steps: { '0': { state: 'done' }, '1': { state: 'done' } } };
  const readiness = readinessOf([item('network', 'ok'), item('thermo_count', 'not_done', 'no zones yet')]);
  const merged = ctx.mergeAllSteps(progress, readiness);
  const resumeTarget = ctx.pickResumeStep(merged);
  assert(resumeTarget === 2, 'resume picks the first pending/regressed step (2: thermocouples), got ' + resumeTarget);

  // Simulate "across browser sessions and reboots": a second, independent
  // load with the SAME board state (a fresh progress fetch, no in-memory
  // carryover) must resume at the same place.
  const ctx2 = loadPageScript(noopFetch);
  const merged2 = ctx2.mergeAllSteps(progress, readiness);
  assert(ctx2.pickResumeStep(merged2) === resumeTarget, 'resume target is stable across independent loads');
})();

// ---- completeness gate: refuses while items are outstanding ------------
(function testCompletenessGate() {
  const ctx = loadPageScript(noopFetch);
  const allOkReadiness = readinessOf(
    ctx.WIZARD_STEPS.reduce((acc, s) => acc.concat(s.readinessKeys.map((k) => item(k, 'ok'))), [])
  );
  const allDoneProgress = {
    version: 1,
    steps: Object.fromEntries(ctx.WIZARD_STEPS.map((s) => [String(s.id), { state: 'done' }])),
  };
  const mergedAllDone = ctx.mergeAllSteps(allDoneProgress, allOkReadiness);
  const gateOk = ctx.computeCompleteness(mergedAllDone, allOkReadiness);
  assert(gateOk.complete === true, 'gate reports complete when every readiness item is ok');

  // One outstanding item -> must refuse.
  const oneOutstanding = readinessOf(
    ctx.WIZARD_STEPS.reduce((acc, s) => acc.concat(s.readinessKeys.map((k) => item(k, 'ok'))), [])
  );
  oneOutstanding.items[0] = item(oneOutstanding.items[0].key, 'not_done', 'still outstanding');
  const mergedOutstanding = ctx.mergeAllSteps(allDoneProgress, oneOutstanding);
  const gateBlocked = ctx.computeCompleteness(mergedOutstanding, oneOutstanding);
  assert(gateBlocked.complete === false, 'gate refuses complete with one outstanding not_done item');
  assert(gateBlocked.reasons.length > 0, 'gate names at least one reason when refusing');

  // A skipped SAFETY-relevant step must also block, even with readiness
  // otherwise all-ok.
  const safetySkippedProgress = JSON.parse(JSON.stringify(allDoneProgress));
  safetySkippedProgress.steps['7'] = { state: 'skipped', note: 'deferred' };
  const mergedSafetySkipped = ctx.mergeAllSteps(safetySkippedProgress, allOkReadiness);
  const gateSafetyBlocked = ctx.computeCompleteness(mergedSafetySkipped, allOkReadiness);
  assert(gateSafetyBlocked.complete === false, 'gate refuses complete when a safety-relevant step is skipped');

  // A skipped NON-safety step (deliberately_off-equivalent choice, e.g.
  // coupling matrix) must NOT block completion by itself.
  const nonSafetySkippedProgress = JSON.parse(JSON.stringify(allDoneProgress));
  nonSafetySkippedProgress.steps['11'] = { state: 'skipped', note: 'not needed' };
  const mergedNonSafetySkipped = ctx.mergeAllSteps(nonSafetySkippedProgress, allOkReadiness);
  const gateNonSafetyOk = ctx.computeCompleteness(mergedNonSafetySkipped, allOkReadiness);
  assert(gateNonSafetyOk.complete === true, 'skipping a non-safety step (11: coupling matrix) does not block completion');
})();

// ---- Steps 0-3 content: pure validators (implementation steps 4-5) ------
(function testStep1Validation() {
  const ctx = loadPageScript(noopFetch);
  const ok = ctx.validateStep1({ tz: 'EST5EDT,M3.2.0,M11.1.0', unit: 'C' });
  assert(ok.valid === true, 'step1: a real POSIX TZ rule + a unit validates');

  const empty = ctx.validateStep1({ tz: '', unit: 'C' });
  assert(empty.valid === false, 'step1: empty tz is refused');

  const iana = ctx.validateStep1({ tz: 'America/Chicago', unit: 'C' });
  assert(iana.valid === false, 'step1: an IANA zone name (not POSIX TZ) is refused');

  const noUnit = ctx.validateStep1({ tz: 'EST5EDT,M3.2.0,M11.1.0', unit: '' });
  assert(noUnit.valid === false, 'step1: missing unit choice is refused');
})();

(function testStep2Validation() {
  const ctx = loadPageScript(noopFetch);
  const ok = ctx.validateStep2(2, [{ thermo_mask: 1 }, { thermo_mask: 2 }]);
  assert(ok.valid === true, 'step2: every zone within thermoCount has a channel assigned -> valid');

  const unassigned = ctx.validateStep2(2, [{ thermo_mask: 1 }, { thermo_mask: 0 }]);
  assert(unassigned.valid === false, 'step2: a zone with thermo_mask 0 is refused');
  assert(/Zone 2/.test(unassigned.errors[0]), 'step2: error names the offending zone');

  const badCount = ctx.validateStep2(0, []);
  assert(badCount.valid === false, 'step2: thermoCount 0 is refused (must be >=1 to proceed)');

  const tooMany = ctx.validateStep2(ctx.THERMO_COUNT_MAX + 1, []);
  assert(tooMany.valid === false, 'step2: thermoCount above THERMO_COUNT_MAX is refused');
})();

(function testStep3Validation() {
  const ctx = loadPageScript(noopFetch);
  const ok = ctx.validateStep3(1, [{ tc_type: 3, cal_offset_c: 0 }], -50.0, 50.0);
  assert(ok.valid === true, 'step3: a real tc_type code + an in-range offset validates');

  const badType = ctx.validateStep3(1, [{ tc_type: 99, cal_offset_c: 0 }], -50.0, 50.0);
  assert(badType.valid === false, 'step3: an out-of-range tc_type code is refused');

  const badOffset = ctx.validateStep3(1, [{ tc_type: 3, cal_offset_c: 999 }], -50.0, 50.0);
  assert(badOffset.valid === false, 'step3: an out-of-bound cal_offset_c is refused');

  const nanOffset = ctx.validateStep3(1, [{ tc_type: 3, cal_offset_c: NaN }], -50.0, 50.0);
  assert(nanOffset.valid === false, 'step3: NaN cal_offset_c is refused');
})();

// ---- Task requirement 4 / plan section 2: 0 means UNSET for max_temp_c and
// max_ramp_c_per_hr (settled in 1fc9b1dd), never a valid commissioned limit.
// isZoneCommissioned() is the single place this rule lives for the wizard;
// step 2's zone cards render its badge off this exact function.
(function testZoneLimitsZeroMeansUnset() {
  const ctx = loadPageScript(noopFetch);
  assert(ctx.formatZoneLimitC(0) === 'NOT SET', 'formatZoneLimitC(0) reads as NOT SET, never as "0°C"');
  assert(ctx.formatZoneLimitC(1250) === '1250°C', 'formatZoneLimitC renders a real limit with its value');

  assert(ctx.isZoneCommissioned({ max_temp_c: 1250, max_ramp_c_per_hr: 200 }) === true,
    'a zone with both limits set is commissioned');
  assert(ctx.isZoneCommissioned({ max_temp_c: 0, max_ramp_c_per_hr: 200 }) === false,
    'a zone with max_temp_c still 0 is NOT commissioned, even with a ramp limit set');
  assert(ctx.isZoneCommissioned({ max_temp_c: 1250, max_ramp_c_per_hr: 0 }) === false,
    'a zone with max_ramp_c_per_hr still 0 is NOT commissioned, even with a temp limit set');
})();

// ---- Step 4 (zone type) validation -------------------------------------
(function testStep4Validation() {
  const ctx = loadPageScript(noopFetch);
  const heaterOnly = ctx.validateStep4([{ zone_type: 0 }]);
  assert(heaterOnly.valid === true, 'step4: a heater zone needs no on/off fields at all');

  const validOnOff = ctx.validateStep4([{ zone_type: 1, hyst_c: 2.0, min_on_s: 30, min_off_s: 30 }]);
  assert(validOnOff.valid === true, 'step4: an on/off zone with in-range hyst/min-on/min-off validates');

  const badHyst = ctx.validateStep4([{ zone_type: 1, hyst_c: 0, min_on_s: 30, min_off_s: 30 }]);
  assert(badHyst.valid === false, 'step4: on/off hysteresis below 0.5 is refused');

  const badMinOn = ctx.validateStep4([{ zone_type: 1, hyst_c: 2.0, min_on_s: 0, min_off_s: 30 }]);
  assert(badMinOn.valid === false, 'step4: on/off min_on_s of 0 is refused (1-3600)');

  assert(ctx.ZONE_TYPE_CONSEQUENCE_TEXT.indexOf('guards 1 (stall)') !== -1 &&
    ctx.ZONE_TYPE_CONSEQUENCE_TEXT.indexOf('coupling row/column are zeroed') !== -1,
    'step4: the reused consequence text names the disabled guards and the zeroed coupling row/column');
})();

// ---- Step 5 (relay assignment) validation, including the on/off-vs-cap
// interaction (bf1db47f: on/off zones count toward max_simultaneous_relays
// and are suppressed last) -------------------------------------------------
(function testStep5Validation() {
  const ctx = loadPageScript(noopFetch);
  const ok = ctx.validateStep5(2, [{ zone_type: 0 }, { zone_type: 1 }], 2);
  assert(ok.valid === true, 'step5: one on/off zone within a cap of 2 validates');

  const capExceeded = ctx.validateStep5(2, [{ zone_type: 1 }, { zone_type: 1 }, { zone_type: 1 }], 2);
  assert(capExceeded.valid === false, 'step5: 3 on/off zones alone exceeding a cap of 2 is refused');
  assert(/bf1db47f/.test(capExceeded.errors[0]), 'step5: the refusal cites the commit that made on/off count toward the cap');

  const unlimitedOk = ctx.validateStep5(2, [{ zone_type: 1 }, { zone_type: 1 }, { zone_type: 1 }], 0);
  assert(unlimitedOk.valid === true, 'step5: cap 0 (unlimited) never refuses on the on/off count');

  const badRelayCount = ctx.validateStep5(ctx.RELAY_COUNT_MAX + 1, [], 0);
  assert(badRelayCount.valid === false, 'step5: relay count above RELAY_COUNT_MAX is refused');
})();

// ---- Step 6 (zone commissioning limits): 0-means-unset refusal AND the
// abs-max-vs-Pico-ceiling relationship (requirement 3) -------------------
(function testStep6Validation() {
  const ctx = loadPageScript(noopFetch);
  const ok = ctx.validateStep6([{ zone_type: 0, max_temp_c: 1200, max_ramp_c_per_hr: 200 }], null);
  assert(ok.valid === true, 'step6: a fully-commissioned heater zone with no known Pico ceiling validates');

  const unsetTemp = ctx.validateStep6([{ zone_type: 0, max_temp_c: 0, max_ramp_c_per_hr: 200 }], null);
  assert(unsetTemp.valid === false, 'step6: max_temp_c left at 0 is refused (1fc9b1dd)');

  const unsetRamp = ctx.validateStep6([{ zone_type: 0, max_temp_c: 1200, max_ramp_c_per_hr: 0 }], null);
  assert(unsetRamp.valid === false, 'step6: max_ramp_c_per_hr left at 0 on a heater zone is refused');

  const onOffNoRampNeeded = ctx.validateStep6([{ zone_type: 1, max_temp_c: 800, max_ramp_c_per_hr: 0 }], null);
  assert(onOffNoRampNeeded.valid === true, 'step6: an on/off zone needs no ramp rate (not PID/ramped)');

  const onOffStillNeedsMaxTemp = ctx.validateStep6([{ zone_type: 1, max_temp_c: 0, max_ramp_c_per_hr: 0 }], null);
  assert(onOffStillNeedsMaxTemp.valid === false, 'step6: an on/off zone still needs a real max_temp_c');

  // The abs-max relationship itself: a zone max_temp_c above the Pico's
  // known abs_max_temp_c must be REFUSED (not clamped -- no code path here
  // rewrites the value, only rejects it).
  const aboveCeiling = ctx.validateStep6([{ zone_type: 0, max_temp_c: 1300, max_ramp_c_per_hr: 200 }], 1200);
  assert(aboveCeiling.valid === false, 'step6: a zone max_temp_c above the Pico abs_max_temp_c ceiling is refused');
  assert(/never be tighter/.test(aboveCeiling.errors.join(' ')), 'step6: the refusal states the abs-max relationship');

  const atCeiling = ctx.validateStep6([{ zone_type: 0, max_temp_c: 1200, max_ramp_c_per_hr: 200 }], 1200);
  assert(atCeiling.valid === true, 'step6: a zone max_temp_c exactly at the Pico ceiling is allowed');

  const belowCeiling = ctx.validateStep6([{ zone_type: 0, max_temp_c: 1100, max_ramp_c_per_hr: 200 }], 1200);
  assert(belowCeiling.valid === true, 'step6: a zone max_temp_c below the Pico ceiling is allowed');
})();

// ---- getAbsMaxTempC(): reads the Pico's abs_max_temp_c off GET
// /api/safety/commissioning's params[] (id 260) verbatim -- never a second,
// wizard-owned copy of the number, and never confuses "unset"/"unknown"
// with a real 0. -----------------------------------------------------------
(function testGetAbsMaxTempC() {
  const ctx = loadPageScript(noopFetch);
  const set = ctx.getAbsMaxTempC({ params: [{ id: 260, name: 'abs_max_temp_c', set: true, value: 1250 }] });
  assert(set === 1250, 'getAbsMaxTempC reads a set value straight off params[]');

  const unset = ctx.getAbsMaxTempC({ params: [{ id: 260, name: 'abs_max_temp_c', set: false }] });
  assert(unset === null, 'getAbsMaxTempC returns null (not 0) when the Pico field is unset');

  const missing = ctx.getAbsMaxTempC({ params: [] });
  assert(missing === null, 'getAbsMaxTempC returns null when the field is absent entirely');

  const noCommissioning = ctx.getAbsMaxTempC(null);
  assert(noCommissioning === null, 'getAbsMaxTempC returns null when GET /api/safety/commissioning itself failed');
})();

Promise.resolve().then(() => {
  console.log('');
  console.log((passed + failed) + ' assertions, ' + passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('FAILED: ' + failures.join(', '));
    process.exitCode = 1;
  }
});
