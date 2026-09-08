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

Promise.resolve().then(() => {
  console.log('');
  console.log((passed + failed) + ' assertions, ' + passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('FAILED: ' + failures.join(', '));
    process.exitCode = 1;
  }
});
