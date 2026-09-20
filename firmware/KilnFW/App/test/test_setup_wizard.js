/* Node-only test harness for setup_wizard_page.html's stepper state machine
 * (SETUP_WIZARD.md, implementation step 3): computeStepState(),
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

function buildContext(fetchImpl, hash, documentOverride) {
  const document = documentOverride || makeDocument();
  const localStorageData = {};
  // hashListeners is captured on the sandbox so a test can simulate the
  // browser actually dispatching 'hashchange' (real gGoto() no longer
  // renders synchronously when the hash changes -- see gRenderNavCount
  // below -- it relies on this event to trigger the one real render).
  const hashListeners = [];
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
  sandbox.window.addEventListener = (type, fn) => { if (type === 'hashchange') hashListeners.push(fn); };
  sandbox.globalThis = sandbox;
  sandbox.__hashListeners = hashListeners;
  vm.createContext(sandbox);
  return sandbox;
}

function loadPageScript(fetchImpl, hash, documentOverride) {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  const code = extractInlineScript(html);
  const ctx = buildContext(fetchImpl, hash, documentOverride);
  new vm.Script(code, { filename: 'setup_wizard_page.html (inline script)' }).runInContext(ctx);
  return ctx;
}

// Simulates the browser actually firing 'hashchange' after window.location.hash
// was assigned a NEW value -- gGoto() (setup_wizard_page.html) intentionally
// does not render synchronously in that case any more (§1 fix), it defers to
// this event, so a test driving real navigation must fire it by hand exactly
// once per hash assignment, same as a real browser would.
function fireHashChange(ctx) {
  ctx.__hashListeners.forEach((fn) => fn());
}

// Minimal scoped-selector fake DOM for testStep2SelectorScoped* below: models
// just enough of document.querySelector('#containerId .zonecard[data-zone="N"]')
// vs the unscoped '.zonecard[data-zone="N"]' to prove a scoped selector picks
// the right container's card even when another container has a same-indexed
// decoy earlier in document order (the exact §3 defect: an unscoped selector
// only worked by DOM ordering).
function makeScopedZoneDocument(cards) {
  function matches(card, sel) {
    const scoped = /^#(\S+)\s+(.*)$/.exec(sel);
    let containerReq = null, rest = sel;
    if (scoped) { containerReq = scoped[1]; rest = scoped[2]; }
    const zm = /\.zonecard\[data-zone="(\d+)"\]/.exec(rest);
    if (!zm) return false;
    if (containerReq && card.container !== containerReq) return false;
    return card.zone === parseInt(zm[1], 10);
  }
  function makeCardEl(card) {
    return {
      dataset: { zone: String(card.zone) },
      querySelectorAll(sel) {
        if (sel === '.s2chan:checked') {
          return (card.checkedChannels || []).map((ch) => ({ dataset: { ch: String(ch) } }));
        }
        return [];
      },
      querySelector(sel) {
        if (sel === '.s2name') return { value: card.name || '' };
        return null;
      },
    };
  }
  return {
    getElementById: () => makeElement(),
    querySelector(sel) {
      const found = cards.filter((c) => matches(c, sel));
      return found.length ? makeCardEl(found[0]) : null;
    },
    querySelectorAll: () => [],
    body: makeElement('body'), documentElement: makeElement('documentElement'),
  };
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
  // authentication) must NOT block completion by itself. (Former step 11,
  // "Coupling matrix (optional)", was this test's original example, but was
  // removed from WIZARD_STEPS 2026-09-19 -- its info is now folded into step
  // 10's own render -- so it can no longer stand in here: mergeAllSteps()
  // only iterates the live WIZARD_STEPS table, and a stray progress.steps['11']
  // is simply never read, which would make this assertion vacuous. Step 12
  // (authentication, renumbered from 13 on the same 2026-09-19 change) is
  // the same shape -- safety:false, no readinessKeys.)
  const nonSafetySkippedProgress = JSON.parse(JSON.stringify(allDoneProgress));
  nonSafetySkippedProgress.steps['12'] = { state: 'skipped', note: 'not needed' };
  const mergedNonSafetySkipped = ctx.mergeAllSteps(nonSafetySkippedProgress, allOkReadiness);
  const gateNonSafetyOk = ctx.computeCompleteness(mergedNonSafetySkipped, allOkReadiness);
  assert(gateNonSafetyOk.complete === true, 'skipping a non-safety step (12: authentication) does not block completion');
})();

// ---- Owner decision 2026-09-09: /api/readiness is now a REAL firing gate
// (readiness_gate.c/.h), blocking on safety_trip, recovery_mode, crash_report
// and estop_verified with NO override. The wizard must consume that SAME
// predicate -- computeCompleteness() already iterates every item in the raw
// /api/readiness response, not just the keys any WIZARD_STEPS entry declares,
// so a whole-board item none of the 13 steps track (crash_report,
// recovery_mode, safety_trip, estop_verified, safety_context, cfg_fs) still
// blocks "Setup complete" the instant readiness_http.c reports it not_done --
// there is no second, wizard-owned copy of "is this board allowed to fire" to
// drift from readiness_gate's. This is not hypothetical: the bench board's
// live /api/readiness right now reports crash_report=not_done (an
// unacknowledged panic from today's profile_executor task) and
// estop_verified=not_done, neither of which any wizard step maps to.
(function testGateBlocksOnUnmappedFiringGateItem() {
  const ctx = loadPageScript(noopFetch);
  const allDoneProgress = {
    version: 1,
    steps: Object.fromEntries(ctx.WIZARD_STEPS.map((s) => [String(s.id), { state: 'done' }])),
  };
  // Every step-tracked key is 'ok', but readiness ALSO reports the four real
  // firing-gate items (readiness_gate.h's READINESS_GATE_KEY_* strings) plus
  // two other whole-board items no step maps to -- exactly the bench shape.
  const stepKeyItems = ctx.WIZARD_STEPS.reduce((acc, s) => acc.concat(s.readinessKeys.map((k) => item(k, 'ok'))), []);
  const readinessWithCrash = readinessOf(stepKeyItems.concat([
    item('recovery_mode', 'ok'),
    item('safety_trip', 'ok'),
    item('crash_report', 'not_done', 'unacknowledged crash on record (IllegalInstruction, task profile_executo)'),
    item('estop_verified', 'not_done', 'never confirmed'),
  ]));
  const merged = ctx.mergeAllSteps(allDoneProgress, readinessWithCrash);
  const gate = ctx.computeCompleteness(merged, readinessWithCrash);
  assert(gate.complete === false,
    'an unmapped not_done firing-gate item (crash_report) blocks "Setup complete" even with all 13 steps done');
  assert(gate.reasons.some((r) => /crash_report|Unacknowledged crash report/.test(r)),
    'the refusal names the crash_report item, not a generic message');
  assert(gate.reasons.some((r) => /estop_verified|E-stop interlock verified/.test(r)),
    'the refusal also names estop_verified independently -- both real gate blockers surface, not just the first');

  // And the inverse: once every item (step-tracked AND whole-board) reads
  // ok/deliberately_off, completion is granted -- proves the block above was
  // this test's fixture, not some unrelated always-false path.
  const readinessAllClear = readinessOf(stepKeyItems.concat([
    item('recovery_mode', 'ok'), item('safety_trip', 'ok'),
    item('crash_report', 'ok'), item('estop_verified', 'ok'),
  ]));
  const mergedClear = ctx.mergeAllSteps(allDoneProgress, readinessAllClear);
  const gateClear = ctx.computeCompleteness(mergedClear, readinessAllClear);
  assert(gateClear.complete === true, 'once the firing-gate items also clear, completion is granted');
})();

// ---- Review 2026-09-08 (docs/audits/setup_wizard_review_2026-09-08.md) §2a/
// §2c: an unreachable or truncated /api/readiness must render UNKNOWN and
// must never be able to read as "Setup complete". Before this fix,
// loadAll()'s { items: [] } fallback iterated to zero reasons in
// computeCompleteness() and the gate painted green on a kiln that could not
// even be asked. See the standalone negative-test proof further down for
// the "make it fail on purpose" half of this requirement.
(function testUnreachableReadinessNeverReadsComplete() {
  const ctx = loadPageScript(noopFetch);
  const allDoneProgress = {
    version: 1,
    steps: Object.fromEntries(ctx.WIZARD_STEPS.map((s) => [String(s.id), { state: 'done' }])),
  };
  const unreachable = readinessOf([]); // exactly loadAll()'s fetchJsonOr fallback shape
  const merged = ctx.mergeAllSteps(allDoneProgress, unreachable);
  const gate = ctx.computeCompleteness(merged, unreachable);
  assert(gate.complete === false, 'unreachable readiness (empty item list) never reads as complete, ' +
    'even with every step stored done');
  assert(gate.reasons.some((r) => /could not be read/.test(r)),
    'unreachable readiness: the gate names the checklist as unreadable, not as "nothing outstanding"');
})();

(function testTruncatedChecklistRendersUnknownNotStoredState() {
  const ctx = loadPageScript(noopFetch);
  // A step that declares a readiness key, but that key never made it into
  // this readiness response (readiness_http.c's append_item() dropped it) --
  // simulates a truncated checklist rather than a total fetch failure.
  const step = { id: 6, title: 'Zone limits', readinessKeys: ['guard_max_temp'], safety: true };
  const truncated = ctx.computeStepState(step, { state: 'done' }, {}); // key absent from byKey entirely
  assert(truncated.state === 'unknown',
    'a step whose readiness key never resolved renders unknown, not the stored "done"');

  // Same fixture through the full merge/gate path: an unknown step must
  // also block "setup complete", not just render an odd pill.
  const merged = ctx.mergeAllSteps(
    { version: 1, steps: { '6': { state: 'done' } } },
    readinessOf([]) // guard_max_temp and guard_cross_zone both missing
  );
  const gate = ctx.computeCompleteness(merged, readinessOf([]));
  assert(gate.complete === false, 'a merged step in the unknown state blocks completion');
})();

// §2b: an item that has gone cannot_yet (a prerequisite this step depended
// on was lost) is a regression exactly like not_done -- computeStepState()
// previously tested only anyNotDone, so a stored "done" survived a
// cannot_yet readiness answer and rendered DONE.
(function testCannotYetRegressesStoredDone() {
  const ctx = loadPageScript(noopFetch);
  const step = { id: 6, title: 'Zone limits', readinessKeys: ['guard_max_temp'], safety: true };
  const cannotYetByKey = { guard_max_temp: item('guard_max_temp', 'cannot_yet', 'thermo_count now 0') };
  const result = ctx.computeStepState(step, { state: 'done' }, cannotYetByKey);
  assert(result.state === 'regressed',
    'stored done + readiness now cannot_yet -> REGRESSED, matching the not_done case');
  assert(/guard_max_temp/.test(result.detail), 'cannot_yet regression detail names the offending item');
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

// ---- Identification-round-trip regression (2026-09-08 hazard report):
// zones_http_post_parse.c treats z%u_k/z%u_tau/z%u_deadtime as
// OMIT-DELETES -- a submission that doesn't name them zeroes the measured
// plant model. Steps 2 and 3 originally hand-built a short params list
// containing only the field(s) each screen edits and POSTed that alone,
// which would have wiped model identification (and tripped required-field
// 400s on relay_mask/kp/ki/kd/etc) on a real board. The fix routes both
// steps through the same submitZonesConfig()/zoneToPostParams() helper
// steps 4-6 already used; these tests assert the POST body actually carries
// the identification fields through when only an UNRELATED field changes,
// by driving submitZonesConfig() directly (the same call step 2/3's commit
// handlers now make) and by simulating each handler's own merge logic. ----
(function testSubmitZonesConfigEchoesIdentification() {
  let capturedBody = null;
  const ctx = loadPageScript(makeFetch((url, opts) => {
    if (opts && opts.body) capturedBody = opts.body;
    return { ok: true, status: 200, body: {} };
  }));
  const data = {
    thermo_count: 1, relay_count: 1, max_simultaneous_relays: 0, continue_on_zone_trip: false,
    timing_profiles: [{ name: 'Profile 0' }],
    zones: [{
      name: 'Zone 1', relay_mask: 1, thermo_mask: 1, tc_type: 3, cal_offset_c: 0,
      pid_kp: 10, pid_ki: 1, pid_kd: 0, control_mode: 0, max_ramp_c_per_hr: 100,
      sanity_rate_c_per_min: 5, max_temp_c: 1200, min_temp_c: -20,
      heater_window_ms: 0, heater_min_on_ms: 0, heater_min_off_ms: 0, timing_profile: 0,
      model_k_dc: 42.5, model_tau_s: 600, model_dead_time_s: 30,
    }],
  };
  ctx.submitZonesConfig(data, data.zones);
  assert(capturedBody !== null, 'submitZonesConfig posts a body');
  assert(/z0_k=42\.5/.test(capturedBody), 'submitZonesConfig echoes the measured model K');
  assert(/z0_tau=600/.test(capturedBody), 'submitZonesConfig echoes the measured model tau');
  assert(/z0_deadtime=30/.test(capturedBody), 'submitZonesConfig echoes the measured model dead time');
})();

// Step 2's own merge logic (name/thermo_mask edited, everything else --
// including the model fields -- must survive from the GET-returned zone).
(function testStep2MergePreservesIdentification() {
  let capturedBody = null;
  const ctx = loadPageScript(makeFetch((url, opts) => {
    if (opts && opts.body) capturedBody = opts.body;
    return { ok: true, status: 200, body: {} };
  }));
  const data = {
    thermo_count: 1, relay_count: 1, timing_profiles: [{ name: 'Profile 0' }],
    zones: [{
      name: 'Old Name', relay_mask: 1, thermo_mask: 1, tc_type: 3, cal_offset_c: 0,
      pid_kp: 10, pid_ki: 1, pid_kd: 0, control_mode: 0, max_ramp_c_per_hr: 100,
      sanity_rate_c_per_min: 5, max_temp_c: 1200, min_temp_c: -20,
      heater_window_ms: 0, heater_min_on_ms: 0, heater_min_off_ms: 0, timing_profile: 0,
      model_k_dc: 42.5, model_tau_s: 600, model_dead_time_s: 30,
    }],
  };
  const zones = data.zones;
  const liveZones = [{ name: 'New Name', thermo_mask: 2 }]; // step2ReadCurrentZones()'s own shape
  const thermoCountNow = 1;
  const dataCopy = Object.assign({}, data);
  dataCopy.thermo_count = thermoCountNow;
  const mergedZones = [];
  for (let mi = 0; mi < thermoCountNow; mi++) {
    const copy = Object.assign({}, zones[mi] || {});
    copy.name = liveZones[mi].name;
    copy.thermo_mask = liveZones[mi].thermo_mask;
    mergedZones.push(copy);
  }
  ctx.submitZonesConfig(dataCopy, mergedZones);
  assert(/z0_name=New\+Name/.test(capturedBody) || /z0_name=New%20Name/.test(capturedBody),
    'step2 merge: the edited field (name) is applied');
  assert(/z0_thermo_mask=2/.test(capturedBody), 'step2 merge: the edited field (thermo_mask) is applied');
  assert(/z0_k=42\.5/.test(capturedBody), 'step2 merge: model K survives an unrelated (name/thermo_mask) step commit');
  assert(/z0_tau=600/.test(capturedBody), 'step2 merge: model tau survives an unrelated step commit');
  assert(/z0_deadtime=30/.test(capturedBody), 'step2 merge: model dead time survives an unrelated step commit');
})();

// ---- Step 8: CT mapping validation (this pass) ------------------------
(function testValidateStep8() {
  const ctx = loadPageScript(noopFetch);
  const notInstalled = ctx.validateStep8(0, null, 0, []);
  assert(notInstalled.valid, 'step8: ct_installed=0 needs no mapping validation at all');

  const noTopology = ctx.validateStep8(1, null, 0, []);
  assert(!noTopology.valid, 'step8: ct_installed=1 with no topology set is refused');

  const perZoneOk = ctx.validateStep8(1, 0, 0x3, [0, 1]); // CT0->zone0, CT1->zone1
  assert(perZoneOk.valid, 'step8: per-zone topology, distinct zones per CT is valid');

  const perZoneClash = ctx.validateStep8(1, 0, 0x3, [0, 0]); // both CTs -> zone0
  assert(!perZoneClash.valid, 'step8: per-zone topology, two CTs mapped to the same zone is refused');
  assert(/both map to zone 1/.test(perZoneClash.errors[0]), 'step8: the clash names the zone');

  const summedOk = ctx.validateStep8(1, 1, 0x3, [0, 0]); // summed: same zone id on multiple CTs is fine
  assert(summedOk.valid, 'step8: summed topology tolerates the same zone id on multiple CT channels');
})();

// ---- Step 9: heat-required marking is not silently walkable past -------
(function testStep9HeatWarningPresent() {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  assert(/APPLIES HEAT/.test(html), 'step9: the page source names the heat warning verbatim');
  assert(/step9Ack/.test(html), 'step9: an explicit presence/safety acknowledgement checkbox gates the start button');
  assert(/step9SkipLater/.test(html), 'step9: the step offers an explicit skip-for-later control');
  assert(/does NOT stop it/.test(html), 'step9: closing the tab not stopping a running sweep is stated, not implied');
})();

// Step 9 used to print "Verification complete." and post state 'done' for
// ANY sweep that reached state 'done', whether or not it had resolved a
// single CT channel -- a confident false statement about a safety-relevant
// commissioning step. On this bench every zone is expected to come out
// INCONCLUSIVE (a ~4 W fixture draws far too little current to attribute),
// so the all-inconclusive case below is the NORMAL case here, not an edge.
(function testStep9VerdictRefusesGreenWhenNothingResolved() {
  const ctx = loadPageScript(noopFetch);

  const nothing = ctx.step9SweepVerdict({ state: 'done', zones_total: 3, ct_map_derived_mask: 0,
    ct_map_reason: 'no channel above the respond floor' });
  assert(nothing.resolved === false, 'step9 verdict: a done sweep that resolved NO channel is not a pass');
  assert(/NOT resolved/.test(nothing.text), 'step9 verdict: the inconclusive text says so plainly');
  assert(!/^Verification complete/.test(nothing.text),
    'step9 verdict: an inconclusive run never claims verification is complete');
  assert(/no channel above the respond floor/.test(nothing.text),
    'step9 verdict: the firmware reason is carried through, not swallowed');

  const partial = ctx.step9SweepVerdict({ state: 'done', zones_total: 3, ct_map_derived_mask: 0x3 });
  assert(partial.resolved === false, 'step9 verdict: 2 of 3 zones resolved is still not a pass');

  const all = ctx.step9SweepVerdict({ state: 'done', zones_total: 3, ct_map_derived_mask: 0x7 });
  assert(all.resolved === true, 'step9 verdict: a channel resolved for every zone is a pass');

  const noZones = ctx.step9SweepVerdict({ state: 'done', zones_total: 0, ct_map_derived_mask: 0 });
  assert(noZones.resolved === false, 'step9 verdict: a sweep over zero zones proves nothing');

  const missing = ctx.step9SweepVerdict({ state: 'done', zones_total: 3 });
  assert(missing.resolved === false,
    'step9 verdict: an absent ct_map_derived_mask is inconclusive, never a pass');

  const running = ctx.step9SweepVerdict({ state: 'running', zones_total: 3, ct_map_derived_mask: 0x7 });
  assert(running.resolved === false, 'step9 verdict: only a finished sweep can be a pass');
})();

(function testStep9InconclusiveDoesNotPersistDone() {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  assert(!/document\.getElementById\('step9Ok'\)\.innerHTML = '<p class="wok">Verification complete\.<\/p>'/.test(html),
    'step9: the unconditional green verdict on state=done is gone');
  assert(/if \(!v\.resolved\)[\s\S]{0,800}?postStepState\(9, 'done'\)/.test(html),
    'step9: postStepState(9, done) is reached only after the resolved check');
  // The only legitimate 'skipped' post for step 9 is the operator's own
  // explicit "Skip for now" button. An inconclusive SWEEP must leave the step
  // PENDING instead: computeCompleteness() treats a skipped safety step as an
  // outstanding reason forever, so posting skipped from the result path would
  // trade one wrong verdict for another.
  const skipped = html.match(/postStepState\(9, 'skipped'/g) || [];
  assert(skipped.length === 1, 'step9: exactly one skipped post exists (the explicit operator button)');
  assert(/postStepState\(9, 'skipped', 'deferred/.test(html),
    'step9: that one skipped post is the deferred/Skip-for-now path, not a sweep result');
})();

// The state strings below are the ones the firmware actually emits --
// exec_state_name() (dashboard_exec_http.c) and autotune_state_name()
// (dashboard_json.c) are both lowercase. The original version of this test
// fed 'RUNNING'/'IDLE', which the firmware never sends, so it passed against
// a step9CheckBusy() that could not refuse a real firing (idealized-test-
// input class). Do not "fix" a failure here by re-uppercasing the fixture.
(function testStep9CheckBusy() {
  const ctx = loadPageScript(makeFetch((url) => {
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'running' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    return { ok: true, status: 200, body: {} };
  }));
  return ctx.step9CheckBusy().then((reason) => {
    assert(typeof reason === 'string' && /firing is currently running/.test(reason),
      'step9CheckBusy: a running firing refuses the CT verification sweep');
  });
})();

(function testStep9CheckBusyPausedFiring() {
  const ctx = loadPageScript(makeFetch((url) => {
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'paused' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'idle' } };
    return { ok: true, status: 200, body: {} };
  }));
  return ctx.step9CheckBusy().then((reason) => {
    assert(typeof reason === 'string' && /firing is currently running/.test(reason),
      'step9CheckBusy: a PAUSED firing still refuses the sweep (relays are still owned)');
  });
})();

(function testStep9CheckBusyRunningAutotune() {
  const ctx = loadPageScript(makeFetch((url) => {
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'idle' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'stepping' } };
    return { ok: true, status: 200, body: {} };
  }));
  return ctx.step9CheckBusy().then((reason) => {
    assert(typeof reason === 'string' && /autotune is currently running/.test(reason),
      'step9CheckBusy: a running autotune refuses the sweep');
  });
})();

(function testStep9CheckBusyFinishedRunsDoNotBlock() {
  const ctx = loadPageScript(makeFetch((url) => {
    if (url === '/api/profile_exec') return { ok: true, status: 200, body: { state: 'done' } };
    if (url === '/api/autotune') return { ok: true, status: 200, body: { state: 'done' } };
    return { ok: true, status: 200, body: {} };
  }));
  return ctx.step9CheckBusy().then((reason) => {
    assert(reason === null,
      'step9CheckBusy: a FINISHED firing/autotune (state "done") does not block the sweep forever');
  });
})();

(function testStep9CheckBusyNeverBlocksOnFailedFetch() {
  const ctx = loadPageScript(function () { return Promise.reject(new Error('network down')); });
  return ctx.step9CheckBusy().then((reason) => {
    assert(reason === null, 'step9CheckBusy: a failed fetch never blocks the start (matches ' +
      'checkFiringOrAutotuneRunning()\'s own rule)');
  });
})();

// §4: a CT-less kiln (this bench's normal state -- project_no_cts_fitted_
// guard_coverage) must be able to reach "setup complete". Step 9's own copy
// posted 'skipped' for ct_installed=0 while WIZARD_STEPS marks step 9
// safety:true unconditionally, so computeCompleteness() blocked forever.
// The fix posts 'done' instead -- same precedent step 8 already sets for
// ct_installed=0 (:869) and the same treatment `deliberately_off` gets.
(function testCtSkipPostsDoneNotSkipped() {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  assert(/postStepState\(9, 'done', 'ct_installed=0, nothing to verify'\)/.test(html),
    'step9: the no-CT exit posts done (a complete answer), not skipped (which blocks forever)');
  assert(!/postStepState\(9, 'skipped', 'ct_installed=0, nothing to verify'\)/.test(html),
    'step9: the old skipped-forever call is gone');
})();

(function testCtSkipDoneAllowsCompletion() {
  const ctx = loadPageScript(noopFetch);
  const allOkReadiness = readinessOf(
    ctx.WIZARD_STEPS.reduce((acc, s) => acc.concat(s.readinessKeys.map((k) => item(k, 'ok'))), [])
  );
  const allDoneProgress = {
    version: 1,
    steps: Object.fromEntries(ctx.WIZARD_STEPS.map((s) => [String(s.id), { state: 'done' }])),
  };
  // Step 9 stored 'done' via the CT-less exit, exactly like the fixed code
  // now posts -- must NOT read as a blocking skip.
  allDoneProgress.steps['9'] = { state: 'done', note: 'ct_installed=0, nothing to verify' };
  const merged = ctx.mergeAllSteps(allDoneProgress, allOkReadiness);
  const gate = ctx.computeCompleteness(merged, allOkReadiness);
  assert(gate.complete === true,
    'a CT-less kiln that finished step 9 via the no-CT exit (state done) can reach setup complete');
})();

// §5: step 7's abs-max comparison must slice to thermo_count, matching step
// 6 (:2043) -- zones_http_get.c always emits all 5 MAX31856_CHANNEL_COUNT
// slots and zones_http_post_parse.c deliberately preserves stale values at
// slots >= thermo_count when the zone count is lowered, so comparing the
// whole array lets an off-screen stale slot block abs_max_temp_c with a
// number that appears on no screen in the wizard.
(function testStep7SourceSlicesToThermoCount() {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  assert(/thermoCountS7[\s\S]{0,400}slice\(0, thermoCountS7\)/.test(html),
    'step7: the initial render slices zones to thermo_count before computing the ceiling comparison');
  assert(/freshThermoCount[\s\S]{0,400}slice\(0, freshThermoCount\)/.test(html),
    'step7: the commit-time re-check slices the freshly-fetched zones to thermo_count too');
})();

// ---- Step 10: hand-entered gains vs. autotune branch -------------------
(function testAutotunePrecheck() {
  const ctx = loadPageScript(noopFetch);
  const noTc = ctx.autotunePrecheck({ thermo_mask: 0, relay_mask: 1, zone_type: 0 });
  assert(/no thermocouple channel assigned/.test(noTc), 'step10: no-thermocouple refusal matches autotune_engine.c verbatim');

  const onOff = ctx.autotunePrecheck({ thermo_mask: 1, relay_mask: 1, zone_type: 1 });
  assert(/on\/off device, not a heater/.test(onOff), 'step10: on/off-zone refusal matches autotune_engine.c verbatim');

  const noRelay = ctx.autotunePrecheck({ thermo_mask: 1, relay_mask: 0, zone_type: 0 });
  assert(/no relay mask configured/.test(noRelay), 'step10: no-relay refusal matches autotune_engine.c verbatim');

  const ok = ctx.autotunePrecheck({ thermo_mask: 1, relay_mask: 1, zone_type: 0 });
  assert(ok === null, 'step10: a normal heater zone with sensor+relay has no client-side autotune precheck refusal');
})();

(function testStep10HandEnteredCountsAsComplete() {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  assert(/Hand-entered gains complete this step just as fully as autotune/.test(html),
    'step10: the UI states hand-entered gains explicitly complete the step, not a silent loophole');
  assert(/autotune is not mandatory|complete this step just as fully/.test(html),
    'step10: the deliberate-choice framing is present in the page text');
})();

// ---- Completion gate: NEGATIVE TEST (task requirement) -----------------
// Prove computeCompleteness() can actually fail this test if it regresses --
// simulate the exact bug class this repo has shipped before ("a check that
// cannot fail proves nothing"): make the readiness-not_done branch a no-op
// and confirm the test goes RED, then restore by hand.
(function testCompletenessGateCatchesOutstandingItem() {
  const ctx = loadPageScript(noopFetch);
  const readiness = readinessOf([item('guard_max_temp', 'not_done', 'zone 1 has no ceiling')]);
  const merged = ctx.mergeAllSteps({ version: 1, steps: {} }, readiness);
  const gate = ctx.computeCompleteness(merged, readiness);
  assert(gate.complete === false, 'completeness gate: a not_done readiness item blocks "complete"');
  assert(gate.reasons.some((r) => /guard_max_temp is not_done/.test(r)),
    'completeness gate: the outstanding item is named in the reasons list');
})();

// ---- Step 7: safety processor commissioning (embedded per owner decision) --
// validateStep7() and the enum-label helper it shares with the confirm
// dialog. Fields: tc_type (261), tc_offset_c (266), abs_max_temp_c (260),
// ct_installed (265), ct_topology (799) -- same ids/codes
// safety_commissioning_page.html's own GROUPS table uses.
(function testStep7Validate() {
  const ctx = loadPageScript(noopFetch);
  const okFields = { tcType: 3, tcOffsetC: 0, absMaxTempC: 1300, ctInstalled: 1, ctTopology: 1 };
  assert(ctx.validateStep7(okFields, [1000, 1200]).valid === true,
    'step7: a real tc_type + positive abs_max_temp_c above every zone ceiling + CT answers validates');

  const noType = Object.assign({}, okFields, { tcType: -1 });
  assert(ctx.validateStep7(noType, []).valid === false, 'step7: tc_type must be chosen (no default)');

  const badType = Object.assign({}, okFields, { tcType: 8 });
  assert(ctx.validateStep7(badType, []).valid === false, 'step7: an out-of-range tc_type code is refused');

  const zeroMax = Object.assign({}, okFields, { absMaxTempC: 0 });
  assert(ctx.validateStep7(zeroMax, []).valid === false,
    'step7: abs_max_temp_c of 0 is refused -- no "unlimited" value');

  const nanOffset = Object.assign({}, okFields, { tcOffsetC: NaN });
  assert(ctx.validateStep7(nanOffset, []).valid === false, 'step7: a non-finite tc_offset_c is refused');

  const noCtAnswer = Object.assign({}, okFields, { ctInstalled: -1 });
  assert(ctx.validateStep7(noCtAnswer, []).valid === false,
    'step7: ct_installed must be answered explicitly -- no default');

  const ctYesNoTopology = Object.assign({}, okFields, { ctTopology: -1 });
  assert(ctx.validateStep7(ctYesNoTopology, []).valid === false,
    'step7: ct_installed=yes requires a chosen ct_topology');

  const ctNoTopologyOptional = Object.assign({}, okFields, { ctInstalled: 0, ctTopology: -1 });
  assert(ctx.validateStep7(ctNoTopologyOptional, []).valid === true,
    'step7: ct_installed=no does not require a topology answer (deliberately_off is a legitimate finish)');
})();

// Requirement 2, the abs-max relationship (must never be tighter than the
// ESP's) -- REFUSED, not clamped, mirroring step 6's own check in the
// opposite direction.
(function testStep7AbsMaxNeverTighterThanEsp() {
  const ctx = loadPageScript(noopFetch);
  const base = { tcType: 3, tcOffsetC: 0, ctInstalled: 0, ctTopology: -1 };

  const atCeiling = Object.assign({}, base, { absMaxTempC: 1200 });
  assert(ctx.validateStep7(atCeiling, [1200]).valid === true,
    'step7: abs_max_temp_c exactly equal to the highest zone max_temp_c is accepted');

  const aboveCeiling = Object.assign({}, base, { absMaxTempC: 1300 });
  assert(ctx.validateStep7(aboveCeiling, [1200]).valid === true,
    'step7: abs_max_temp_c above every zone ceiling is accepted');

  const belowCeiling = Object.assign({}, base, { absMaxTempC: 1000 });
  const v = ctx.validateStep7(belowCeiling, [1200]);
  assert(v.valid === false, 'step7: abs_max_temp_c BELOW a zone max_temp_c is refused, not clamped');
  assert(v.errors.some((e) => /never be tighter than the ESP/.test(e)),
    'step7: the refusal names the second-set-of-eyes rule, matching step 6\'s own wording');

  const zeroZones = Object.assign({}, base, { absMaxTempC: 1000 });
  assert(ctx.validateStep7(zeroZones, [0, 0]).valid === true,
    'step7: zones with max_temp_c still 0 (uncommissioned) are excluded from the ceiling comparison');
})();

// step7EnumLabelFor: the confirm dialog must name "Type K -> Type T", not
// "3 -> 7" -- same human-label rule safety_commissioning_page.html's
// findCriticalChanges() already applies to tc_type.
(function testStep7EnumLabelFor() {
  const ctx = loadPageScript(noopFetch);
  assert(ctx.step7EnumLabelFor(261, '3') === 'Type K', 'step7: tc_type code 3 labels as Type K');
  assert(ctx.step7EnumLabelFor(265, '1') === 'Yes -- CTs fitted', 'step7: ct_installed=1 labels as fitted');
  assert(ctx.step7EnumLabelFor(799, '1') === 'Summed', 'step7: ct_topology=1 labels as Summed');
  assert(ctx.step7EnumLabelFor(999, '1') === null, 'step7: an unknown field id has no enum label');
})();

// CR1-verify gap wording (requirement 3): the page must say plainly that a
// successful commit confirms the CONFIG RECORD, not the physical chip.
(function testStep7Cr1GapWordingHonest() {
  const html = fs.readFileSync(PAGE_PATH, 'utf8');
  assert(/CONFIG RECORD now holds this type, NOT that the MAX31856 chip/.test(html),
    'step7: the CR1-verify gap is stated explicitly, not implied as a stronger confirmation');
})();

// ---- Step 4/6 confirmation drift fix (docs/audits/setup_wizard_review_
// 2026-09-08.md): step4ZoneTypeConfirmLines()/step6LimitConfirmLines() are
// the pure line-builders the click handlers feed into the shared
// kcConfirmConsequentialChange(). Testing these top-level functions
// directly (same technique as validateStep4/validateStep6) is what proves
// "requires confirmation" / "routine steps do not prompt" without needing
// to drive the stub DOM's querySelector (which this harness's makeDocument
// does not implement -- it always returns null/[]).
(function testStep4ZoneTypeConfirmLinesFiresOnNewOnOff() {
  const ctx = loadPageScript(noopFetch);
  const prior = [{ zone_type: 0, name: 'Bisque' }];
  const live = [{ zone_type: 1 }];
  const lines = ctx.step4ZoneTypeConfirmLines(prior, live, 1);
  assert(lines.length === 1, 'step4: switching a zone to ON_OFF_DEVICE produces exactly one confirm line');
  assert(/Zone 1 \(Bisque\)/.test(lines[0]), 'step4: the confirm line names the zone (number + stored name)');
  assert(/Heater -> On\/off device/.test(lines[0]), 'step4: the confirm line states the direction of the change');
  assert(lines[0].indexOf(ctx.ZONE_TYPE_CONSEQUENCE_TEXT) !== -1,
    'step4: the confirm line reuses ZONE_TYPE_CONSEQUENCE_TEXT verbatim, not a third wording');
})();

(function testStep4ZoneTypeConfirmLinesRoutineCasesDoNotPrompt() {
  const ctx = loadPageScript(noopFetch);
  // No change at all.
  assert(ctx.step4ZoneTypeConfirmLines([{ zone_type: 0 }], [{ zone_type: 0 }], 1).length === 0,
    'step4: an unchanged heater zone produces no confirm line');
  // Already on/off, staying on/off (only its hyst/min-on/min-off fields
  // changed) -- not a NEW disablement, so no line.
  assert(ctx.step4ZoneTypeConfirmLines([{ zone_type: 1 }], [{ zone_type: 1 }], 1).length === 0,
    'step4: a zone already on/off staying on/off produces no confirm line');
  // Switching BACK to Heater re-enables guards -- safety-increasing, not
  // confirmed (over-confirming a safe direction trains click-through).
  assert(ctx.step4ZoneTypeConfirmLines([{ zone_type: 1 }], [{ zone_type: 0 }], 1).length === 0,
    'step4: switching a zone back to Heater produces no confirm line');
})();

(function testStep6LimitConfirmLinesFiresOnChangedCeiling() {
  const ctx = loadPageScript(noopFetch);
  const prior = [{ name: 'Bisque', max_temp_c: 1200, max_ramp_c_per_hr: 200, min_temp_c: -20 }];
  const liveTempChanged = [{ max_temp_c: 1300, max_ramp_c_per_hr: 200 }];
  const lines = ctx.step6LimitConfirmLines(prior, liveTempChanged, 1);
  assert(lines.length === 1, 'step6: a changed max_temp_c produces exactly one confirm line');
  assert(/Zone 1 \(Bisque\) max temp: 1200°C -> 1300°C/.test(lines[0]),
    'step6: the confirm line names the zone and old->new max temp');

  const liveRampChanged = [{ max_temp_c: 1200, max_ramp_c_per_hr: 250 }];
  const rampLines = ctx.step6LimitConfirmLines(prior, liveRampChanged, 1);
  assert(rampLines.length === 1 && /max ramp: 200°C\/hr -> 250°C\/hr/.test(rampLines[0]),
    'step6: a changed max_ramp_c_per_hr produces a confirm line naming old->new');
})();

(function testStep6LimitConfirmLinesRoutineCasesDoNotPrompt() {
  const ctx = loadPageScript(noopFetch);
  const prior = [{ max_temp_c: 1200, max_ramp_c_per_hr: 200, min_temp_c: -20 }];
  // Nothing changed at all.
  assert(ctx.step6LimitConfirmLines(prior, [{ max_temp_c: 1200, max_ramp_c_per_hr: 200 }], 1).length === 0,
    'step6: unchanged max_temp_c/max_ramp_c_per_hr produce no confirm line');
  // min_temp_c is NOT one of the two consequence-bearing fields (it narrows
  // the broken-sensor floor, it does not raise a ceiling) -- confirming it
  // here would be over-confirming a field this step's own UI does not even
  // treat as consequence-bearing.
  const liveMinTempOnly = [{ max_temp_c: 1200, max_ramp_c_per_hr: 200, min_temp_c: 5 }];
  assert(ctx.step6LimitConfirmLines(prior, liveMinTempOnly, 1).length === 0,
    'step6: a changed min_temp_c alone produces no confirm line');
})();

// ---- commissioning_shared.js: the read-back verification itself (Non-
// negotiable 1: "a post-write read-back that fails loudly on disagreement").
// Loads the real shared file (not a reimplementation) into its own vm
// context with a stub fetch: POST reports {ok:true} (as a real board would
// for an accepted commit) but the follow-up GET reports a DIFFERENT value
// for the critical field than what was just sent -- kcCommissioningCommitAndVerify
// must resolve {ok:false} and name the field, never trust the POST's ok:true alone.
const COMMISSIONING_SHARED_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'commissioning_shared.js');

function loadCommissioningShared(fetchImpl, confirmImpl) {
  const code = fs.readFileSync(COMMISSIONING_SHARED_PATH, 'utf8');
  const sandbox = { fetch: fetchImpl, confirm: confirmImpl || (() => true), console };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  new vm.Script(code, { filename: 'commissioning_shared.js' }).runInContext(sandbox);
  return sandbox;
}

// ---- kcConfirmConsequentialChange() (commissioning_shared.js): the generic
// sibling steps 4/6 use in place of a bespoke confirm implementation.
(function testConfirmConsequentialChangeNoLinesNeverPrompts() {
  let confirmCalled = false;
  const ctx = loadCommissioningShared(() => Promise.resolve({ ok: true, json: () => Promise.resolve({}) }),
    () => { confirmCalled = true; return true; });
  return ctx.kcConfirmConsequentialChange([], {}).then((result) => {
    assert(result === true, 'kcConfirmConsequentialChange: an empty change list resolves true');
    assert(confirmCalled === false, 'kcConfirmConsequentialChange: a routine (no-op) call never shows a dialog');
  });
})();

(function testConfirmConsequentialChangeConfirmedProceeds() {
  const ctx = loadCommissioningShared(
    () => Promise.resolve({ ok: true, json: () => Promise.resolve({ state: 'idle' }) }),
    () => true
  );
  return ctx.kcConfirmConsequentialChange(['Zone 1: Heater -> On/off device.'], {}).then((result) => {
    assert(result === true, 'kcConfirmConsequentialChange: confirming a real change resolves true');
  });
})();

(function testConfirmConsequentialChangeDeclinedRefuses() {
  const ctx = loadCommissioningShared(
    () => Promise.resolve({ ok: true, json: () => Promise.resolve({ state: 'idle' }) }),
    () => false
  );
  return ctx.kcConfirmConsequentialChange(['Zone 1: Heater -> On/off device.'], {}).then((result) => {
    assert(result === false, 'kcConfirmConsequentialChange: declining resolves false (caller must revert, not just skip the save)');
  });
})();

(function testConfirmConsequentialChangeRefusedWhenBusy() {
  let confirmCalled = false;
  let busyMsg = null;
  const ctx = loadCommissioningShared(
    (url) => Promise.resolve({ ok: true, json: () => Promise.resolve(url.indexOf('profile_exec') !== -1 ? { state: 'running' } : { state: 'idle' }) }),
    () => { confirmCalled = true; return true; }
  );
  return ctx.kcConfirmConsequentialChange(['Zone 1: Heater -> On/off device.'], {
    onBusy: (msg) => { busyMsg = msg; },
  }).then((result) => {
    assert(result === false, 'kcConfirmConsequentialChange: a running firing refuses without ever asking to confirm');
    assert(confirmCalled === false, 'kcConfirmConsequentialChange: busy refusal short-circuits before the confirm dialog');
    assert(typeof busyMsg === 'string' && /profile is currently firing/.test(busyMsg),
      'kcConfirmConsequentialChange: the busy refusal is reported via onBusy, naming the reason');
  });
})();

(function testReadbackMismatchFailsLoudly() {
  const boardValue = { 261: '3' }; // board's own tc_type after the "successful" commit
  const fetchImpl = (url, opts) => {
    if (opts && opts.method === 'POST') {
      return Promise.resolve({ ok: true, json: () => Promise.resolve({ ok: true }) });
    }
    // GET read-back: reports a DIFFERENT value (7, Type T) than what was
    // sent (3, Type K) -- the board silently did not apply the write.
    return Promise.resolve({
      ok: true,
      json: () => Promise.resolve({ params: [{ id: 261, name: 'tc_type', value: boardValue[261], set: true }] }),
    });
  };
  const ctx = loadCommissioningShared(fetchImpl);
  const bodyPairs = [{ id: 261, name: 'tc_type', value: '7' }]; // what we tried to write
  const critical = [{ id: 261, name: 'tc_type', oldDisplay: 'Type K', newDisplay: 'Type T' }];
  return ctx.kcCommissioningCommitAndVerify(bodyPairs, critical, { setMsg: () => {} }).then((res) => {
    assert(res.ok === false, 'read-back mismatch: overall result is NOT ok, even though POST reported ok:true');
    assert(/does NOT match what was just written/.test(res.message),
      'read-back mismatch: message fails loudly and names the disagreement');
    assert(/tc_type/.test(res.message), 'read-back mismatch: the offending field is named');
  });
})();

(function testReadbackMatchSucceeds() {
  const fetchImpl = (url, opts) => {
    if (opts && opts.method === 'POST') {
      return Promise.resolve({ ok: true, json: () => Promise.resolve({ ok: true }) });
    }
    return Promise.resolve({
      ok: true,
      json: () => Promise.resolve({ params: [{ id: 261, name: 'tc_type', value: '7', set: true }] }),
    });
  };
  const ctx = loadCommissioningShared(fetchImpl);
  const bodyPairs = [{ id: 261, name: 'tc_type', value: '7' }];
  const critical = [{ id: 261, name: 'tc_type', oldDisplay: 'Type K', newDisplay: 'Type T' }];
  return ctx.kcCommissioningCommitAndVerify(bodyPairs, critical, { setMsg: () => {} }).then((res) => {
    assert(res.ok === true, 'read-back match: agreeing read-back reports ok:true');
    assert(/confirmed by read-back/.test(res.message), 'read-back match: message says confirmed');
  });
})();

// ---- JS hygiene pass (docs/audits/setup_wizard_review_2026-09-08.md) -----

// §1: gGoto() + the 'hashchange' listener used to BOTH render on a real
// navigation (gGoto() rendered directly, unconditionally, on top of setting
// window.location.hash -- which then also fired 'hashchange', which rendered
// again), so one click rendered twice and re-fetched that step's data twice.
// gRenderNavCount (exposed for exactly this purpose) counts every actual
// render; a real navigation must produce exactly one.
(function testOneRenderPerNavigation() {
  const ctx = loadPageScript(noopFetch, '');
  return runMicrotasks().then(() => {
    const before = ctx.gRenderNavCount;
    ctx.gGoto(2); // hash '' -> '#step=2': a real change, so gGoto() must NOT render synchronously.
    assert(ctx.gRenderNavCount === before,
      'gGoto() to a new hash defers rendering to the hashchange event instead of rendering directly');
    fireHashChange(ctx); // simulates the browser's one resulting event
    assert(ctx.gRenderNavCount === before + 1,
      'exactly one render happens per navigation once hashchange fires');
    // A second, distinct navigation must also be exactly one render, not an
    // accumulating double per click.
    ctx.gGoto(3);
    fireHashChange(ctx);
    assert(ctx.gRenderNavCount === before + 2,
      'a second navigation adds exactly one more render, not two');
  });
})();

// gGoto() to a step that is ALREADY open (hash unchanged, e.g. re-clicking
// the same "Open" button) must still render once directly -- no hashchange
// event fires in that case, so skipping the direct render would render
// nothing at all.
(function testGGotoRendersDirectlyWhenHashAlreadyCorrect() {
  const ctx = loadPageScript(noopFetch, '#step=5');
  return runMicrotasks().then(() => {
    const before = ctx.gRenderNavCount;
    ctx.gGoto(5); // hash is already '#step=5' -- no hashchange will fire
    assert(ctx.gRenderNavCount === before + 1,
      'gGoto() to the step already open renders directly exactly once (no hashchange would fire otherwise)');
  });
})();

// NEGATIVE TEST for §1 (task requirement): proof the counter can actually
// catch a regression. Performed by hand against the real file, not simulated
// here -- see the report for the exact steps (reintroduce the old
// unconditional render + hash-set in gGoto(), re-run this file, quote the
// failing line, revert, confirm `git diff` empty). Left as a comment (like
// the commissioning_shared.js negative test below) so the procedure is
// preserved without permanently shipping a broken gGoto().

// §2: a save confirmation used to be written directly into the DOM and then
// immediately raced by loadAll()'s re-render of that same step, which wiped
// it. armStepMessage()/consumeArmedStepMessage() are the fix -- a message
// armed for step N is applied once, after step N's own next render actually
// finishes, so it survives the refresh instead of losing the race.
(function testArmedStepMessageSurvivesRerender() {
  const ctx = loadPageScript(noopFetch);
  let applied = 0;
  ctx.armStepMessage(1, () => { applied++; });
  // A render of a DIFFERENT step must not consume or apply step 1's armed message.
  ctx.consumeArmedStepMessage(2);
  assert(applied === 0, 'an armed message for step 1 is not applied when a different step (2) renders');
  // The matching step's render applies it exactly once.
  ctx.consumeArmedStepMessage(1);
  assert(applied === 1, 'an armed message for step 1 is applied when step 1 itself renders (survives the refresh)');
  // It must not fire a second time on a later, unrelated render of the same step.
  ctx.consumeArmedStepMessage(1);
  assert(applied === 1, 'a consumed armed message is not re-applied on a later render of the same step');
})();

// §3: document.querySelector('.zonecard[data-zone="N"]') in
// step2ReadCurrentZones() used to be unscoped, so it worked only because step
// 2 happened to be the only visible screen with a .zonecard at the time --
// step 3 uses the identical class/attribute shape. Prove the fixed, scoped
// selector (#stepBody2 .zonecard[...]) resolves the step-2 card even with a
// same-indexed decoy elsewhere in the DOM ahead of it in document order.
(function testStep2SelectorScopedNotFooledByDuplicateElsewhere() {
  const cards = [
    { container: 'stepBody3', zone: 0, name: 'DECOY (step 3 zone card)', checkedChannels: [] },
    { container: 'stepBody2', zone: 0, name: 'Zone One', checkedChannels: [0] },
  ];
  const ctx = loadPageScript(noopFetch, '', makeScopedZoneDocument(cards));
  const zones = ctx.step2ReadCurrentZones(1, [{}]);
  assert(zones[0].name === 'Zone One',
    'step2ReadCurrentZones() resolves the #stepBody2-scoped card, not a same-zone-index decoy elsewhere in the DOM');
})();

// ---- WEB_AUTH_PLAN.md section 11: "the setup wizard should recommend
// enabling it. It presents an 'enable authentication' step that is
// skippable, defaults the checkbox to off ... and states in one line what is
// at stake." This is a shape assertion on the real WIZARD_STEPS table (not a
// second, hand-maintained copy of it) so a future edit that renames/removes
// this step without updating the plan's own acceptance line is caught here,
// not just by eye. Step id renumbered 13 -> 12 on 2026-09-19 when former
// step 11 ("Coupling matrix (optional)") was removed and steps after it
// shifted down by one -- this test follows that renumbering. It
// intentionally does NOT drive the DOM-only renderStep12() (fetch/document-
// heavy, same class as the other untested optional-step renderers 8-11) --
// see the file header's extraction rationale for why this suite only
// reaches the page's pure/state functions.
(function testStep12AuthWizardStepExistsAndIsOptional() {
  const ctx = loadPageScript(noopFetch);
  const step12 = ctx.WIZARD_STEPS.filter((s) => s.id === 12)[0];
  assert(!!step12, 'WIZARD_STEPS declares a step 12 (section 11\'s authentication step)');
  assert(/auth/i.test(step12.title), 'step 12 is titled about authentication, not a generic placeholder');
  assert(step12.safety === false, 'step 12 is not safety-relevant (skipping it must never block the gate)');
  assert(step12.readinessKeys.length === 0,
    'step 12 declares no readiness keys -- it is purely optional/recommend-only, per plan section 11');

  // Confirm the "never blocks completion" half end-to-end through the real
  // gate function, the same way testGateBlocksOnUnmappedFiringGateItem()
  // above proves the opposite direction for a real blocking item.
  const allDoneProgress = {
    version: 1,
    steps: Object.fromEntries(ctx.WIZARD_STEPS.filter((s) => s.id !== 12).map((s) => [String(s.id), { state: 'done' }])),
  };
  allDoneProgress.steps['12'] = { state: 'skipped', note: 'left off' };
  const stepKeyItems = ctx.WIZARD_STEPS.reduce((acc, s) => acc.concat(s.readinessKeys.map((k) => item(k, 'ok'))), []);
  const readiness = readinessOf(stepKeyItems);
  const merged = ctx.mergeAllSteps(allDoneProgress, readiness);
  const gate = ctx.computeCompleteness(merged, readiness);
  assert(gate.complete === true,
    'skipping step 12 (authentication, non-safety) does not block "Setup complete" -- same precedent as ' +
    'testGateAllowsSkippedNonSafetyStep\'s non-safety-skip assertion above');
})();

// ---- NEGATIVE TEST (task requirement): prove testReadbackMismatchFailsLoudly
// can actually fail, not just always pass. See report for the exact steps
// taken by hand: (1) in commissioning_shared.js's commitAndVerify(), the
// mismatch line
//   return !p || !p.set || String(p.value) !== String(pair.value);
// was changed to `return false;` (bug: mismatch never detected), (2) this
// test file was re-run, (3) testReadbackMismatchFailsLoudly went RED on its
// first assertion ("read-back mismatch: overall result is NOT ok, even
// though POST reported ok:true") because res.ok came back true, (4) the
// edit was reversed by hand, (5) `git diff` on commissioning_shared.js
// confirmed empty before this pass committed.

Promise.resolve().then(() => {
  console.log('');
  console.log((passed + failed) + ' assertions, ' + passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('FAILED: ' + failures.join(', '));
    process.exitCode = 1;
  }
});
