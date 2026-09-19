/* Node-only test for zones_page.html's section visibility gates (ROADMAP.md
 * "Visually simplify the Thermocouples & Zones web page", decision (a)):
 * seven sections render only when their feature is enabled/present on at
 * least one zone (or, for Measure Zone Normal Current, when the safety
 * processor's ct_installed says CTs are fitted); otherwise the section is
 * hidden (via the `hidden` DOM property, never inline display) and a single
 * muted hint line (class "hint") takes its place.
 *
 * The five gate functions are extracted VERBATIM from the page, same
 * discipline as test_zones_group_frames.js/test_zones_inheritance.js, so
 * this cannot pass against a copy that has drifted from what ships.
 *
 * Run: node firmware/KilnFW/App/test/test_zones_page_visibility_gates.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'zones_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');

function extractRange(startMarker, endMarker) {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l) === startMarker);
  if (startIdx === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const endIdx = LINES.findIndex((l, i) => i >= startIdx && raw(l) === endMarker);
  if (endIdx === -1) throw new Error('end marker not found after start: ' + JSON.stringify(endMarker));
  return LINES.slice(startIdx, endIdx + 1).join('\n');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

const CODE =
  extractRange('function gateMeasureNormalCurrentSection(installed) {', '}') + '\n' +
  extractRange('function gatePidAutotuneSection(zones) {', '}') + '\n' +
  extractRange('function gateTuningQualitySection(zones) {', '}') + '\n' +
  extractRange('function gateCouplingSection(measuredRows) {', '}') + '\n' +
  extractRange('function gateContinuousTuningSection(zones) {', '}');

assert(/\.hidden\s*=/.test(CODE), 'the gates set the `hidden` property');
assert(!/\.style\.display/.test(CODE), 'the gates never write an inline display style');

// ---------------------------------------------------------------------------
// Minimal DOM stub: one fake element per id, tracking .hidden.
// ---------------------------------------------------------------------------
function makeDom(ids) {
  const els = {};
  ids.forEach((id) => { els[id] = { hidden: false }; });
  return {
    document: { getElementById: (id) => els[id] || (els[id] = { hidden: false }) },
    els,
  };
}

function run(fnName, ids, callArg) {
  const { document, els } = makeDom(ids);
  const sandbox = { document };
  vm.createContext(sandbox);
  vm.runInContext(CODE + '\nthis.__fn = ' + fnName + ';', sandbox);
  sandbox.__fn(callArg);
  return els;
}

// --- Measure Zone Normal Current: gated on ct_installed (boolean) ---------
let els = run('gateMeasureNormalCurrentSection', ['measureNormalCurrentSection', 'measureNormalCurrentHint'], true);
assert(els.measureNormalCurrentSection.hidden === false, 'ct_installed=true: section shown');
assert(els.measureNormalCurrentHint.hidden === true, 'ct_installed=true: hint hidden');

els = run('gateMeasureNormalCurrentSection', ['measureNormalCurrentSection', 'measureNormalCurrentHint'], false);
assert(els.measureNormalCurrentSection.hidden === true, 'ct_installed=false: section hidden (negative case)');
assert(els.measureNormalCurrentHint.hidden === false, 'ct_installed=false: hint shown (negative case)');

// --- PID Autotune: gated on any zone control_mode 2 (PID) or 3 (fuzzy) ---
els = run('gatePidAutotuneSection', ['pidAutotuneSection', 'pidAutotuneHint'],
  [{ control_mode: 0 }, { control_mode: 2 }]);
assert(els.pidAutotuneSection.hidden === false, 'a PID zone (mode 2): section shown');
assert(els.pidAutotuneHint.hidden === true, 'a PID zone (mode 2): hint hidden');

els = run('gatePidAutotuneSection', ['pidAutotuneSection', 'pidAutotuneHint'],
  [{ control_mode: 0 }, { control_mode: 1 }]);
assert(els.pidAutotuneSection.hidden === true, 'no PID/fuzzy zone: section hidden (negative case)');
assert(els.pidAutotuneHint.hidden === false, 'no PID/fuzzy zone: hint shown (negative case)');

els = run('gatePidAutotuneSection', ['pidAutotuneSection', 'pidAutotuneHint'], [{ control_mode: 3 }]);
assert(els.pidAutotuneSection.hidden === false, 'a fuzzy zone (mode 3) also shows the section');

// --- Tuning quality: gated on any zone tuning_valid ------------------------
els = run('gateTuningQualitySection', ['tuningQualitySection', 'tuningQualityHint'],
  [{ tuning_valid: false }, { tuning_valid: true }]);
assert(els.tuningQualitySection.hidden === false, 'a zone with tuning_valid: section shown');
assert(els.tuningQualityHint.hidden === true, 'a zone with tuning_valid: hint hidden');

els = run('gateTuningQualitySection', ['tuningQualitySection', 'tuningQualityHint'],
  [{ tuning_valid: false }, { tuning_valid: false }]);
assert(els.tuningQualitySection.hidden === true, 'no zone tuning_valid: section hidden (negative case)');
assert(els.tuningQualityHint.hidden === false, 'no zone tuning_valid: hint shown (negative case)');

// --- Coupling matrix + RGA: gated on measuredRows > 0 ----------------------
els = run('gateCouplingSection', ['couplingSection', 'couplingSectionHint'], 2);
assert(els.couplingSection.hidden === false, 'measuredRows=2: section shown');
assert(els.couplingSectionHint.hidden === true, 'measuredRows=2: hint hidden');

els = run('gateCouplingSection', ['couplingSection', 'couplingSectionHint'], 0);
assert(els.couplingSection.hidden === true, 'measuredRows=0: section hidden (negative case)');
assert(els.couplingSectionHint.hidden === false, 'measuredRows=0: hint shown (negative case)');

// --- Continuous Tuning: gated on any zone .enabled -------------------------
els = run('gateContinuousTuningSection', ['continuousTuningSection', 'continuousTuningHint'],
  [{ enabled: false }, { enabled: true }]);
assert(els.continuousTuningSection.hidden === false, 'a zone with continuous tuning enabled: section shown');
assert(els.continuousTuningHint.hidden === true, 'a zone with continuous tuning enabled: hint hidden');

els = run('gateContinuousTuningSection', ['continuousTuningSection', 'continuousTuningHint'],
  [{ enabled: false }, { enabled: false }]);
assert(els.continuousTuningSection.hidden === true, 'no zone opted into continuous tuning: section hidden (negative case)');
assert(els.continuousTuningHint.hidden === false, 'no zone opted into continuous tuning: hint shown (negative case)');

// ---------------------------------------------------------------------------
// The hint elements themselves carry class="hint" in the page markup, and
// each gated section has exactly one wrapper + one paired hint id.
// ---------------------------------------------------------------------------
const PAIRS = [
  ['measureNormalCurrentSection', 'measureNormalCurrentHint'],
  ['pidAutotuneSection', 'pidAutotuneHint'],
  ['tuningQualitySection', 'tuningQualityHint'],
  ['couplingSection', 'couplingSectionHint'],
  ['continuousTuningSection', 'continuousTuningHint'],
];
PAIRS.forEach(([sectionId, hintId]) => {
  const sectionRe = new RegExp('id="' + sectionId + '"');
  const hintRe = new RegExp('id="' + hintId + '"[^>]*class="hint"[^>]*hidden|id="' + hintId + '"[^>]*hidden[^>]*class="hint"');
  assert(sectionRe.test(SRC), 'markup: #' + sectionId + ' wrapper exists');
  assert(hintRe.test(SRC), 'markup: #' + hintId + ' is a hidden-by-default class="hint" paragraph');
});

console.log('\n' + passed + ' passed, ' + failed + ' failed.');
if (failed > 0) {
  console.log('Failures: ' + failures.join('; '));
  process.exit(1);
}
