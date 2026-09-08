/* Node-only test harness for main_page.html's Safety TC card
 * (renderSafetyCard()): the K4 relay pill.
 *
 * Owner request 2026-09-08: the dashboard pill was shortened from
 * "Safety relay K4: <state>" to "K4: <state>" to save horizontal space --
 * superseding the 2026-08-29 "named in full" decision recorded in the same
 * function's comments. This is dashboard-only: diagnostics_page.html and
 * zones_page.html keep their own longer "Safety relay (K4)..." labels and
 * are not touched by this test.
 *
 * Same by-marker-line extraction as test_current_display.js/test_lag_banner.js
 * -- runs the real renderSafetyCard() source in a Node vm context with a
 * small document stub, rather than reimplementing the rendering logic here.
 *
 * Run: node firmware/KilnFW/App/test/test_safety_relay_pill.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// Extracts a top-level function's source by its exact `function NAME(` line,
// ending at the first column-0 '}' after that line.
function extractFunction(source, name) {
  const lines = source.split('\n');
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = lines.findIndex((l) => raw(l).indexOf('function ' + name + '(') === 0);
  if (startIdx === -1) throw new Error('function not found: ' + name);
  let endIdx = -1;
  for (let i = startIdx + 1; i < lines.length; i++) {
    if (raw(lines[i]) === '}') { endIdx = i; break; }
  }
  if (endIdx === -1) throw new Error('end of function not found: ' + name);
  return lines.slice(startIdx, endIdx + 1).join('\n');
}

const RENDER_SAFETY_CARD_SRC = extractFunction(SRC, 'renderSafetyCard');
assert(RENDER_SAFETY_CARD_SRC.indexOf('function renderSafetyCard(') !== -1,
  'sanity: extracted range includes renderSafetyCard');

function makeContext(fnSrc) {
  const ctx = {
    document: {
      createElement: function () { return { className: '', innerHTML: '' }; },
    },
    window: {
      // Real card content isn't under test here -- always report "separate
      // sensor" so the reading branch renders, and stub kcUnit.fmt like the
      // real app.js does (a plain numeric+unit string).
      kcSafetyTcIsSeparate: function () { return true; },
      kcUnit: { fmt: function (c) { return c + ' °C'; } },
    },
    console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(fnSrc, ctx);
  return ctx;
}

function statusWith(relayEnergized) {
  return {
    safety_ready: true,
    safety_temp_c: 28.7,
    safety_tc_is_separate_sensor: true,
    safety_relay_energized: relayEnergized,
  };
}

// ---------------------------------------------------------------------------
// Group 1: the short form renders, the old long form does not, across all
// three relay states this pill can show.
// ---------------------------------------------------------------------------
{
  const cases = [
    { energized: false, want: 'K4: off' },
    { energized: true, want: 'K4: ON' },
    { energized: null, want: 'K4: unknown' },
  ];
  for (const c of cases) {
    const ctx = makeContext(RENDER_SAFETY_CARD_SRC);
    const data = statusWith(c.energized);
    const div = vm.runInContext('renderSafetyCard(data)', Object.assign(ctx, { data }));
    const html = div.innerHTML;
    assert(html.indexOf(c.want) !== -1,
      'dashboard pill reads "' + c.want + '" for safety_relay_energized=' + c.energized + ' -- got: ' + html);
    assert(html.indexOf('Safety relay K4') === -1,
      'dashboard pill must NOT contain the old long form "Safety relay K4" -- got: ' + html);
  }
}

// ---------------------------------------------------------------------------
// NEGATIVE TEST (feedback_negative_test_every_check): prove this suite can
// actually catch a regression back to the long label. Mutates the EXTRACTED
// production source (a scratch copy, never the file on disk) by restoring
// the old "Safety relay K4: " prefix, then re-runs the short-form assertion
// and requires it to go RED.
// ---------------------------------------------------------------------------
{
  const MUTATED_SRC = RENDER_SAFETY_CARD_SRC.replace("'K4: ' + relayTxt", "'Safety relay K4: ' + relayTxt");
  assert(MUTATED_SRC !== RENDER_SAFETY_CARD_SRC, 'sanity: the mutation string was actually found and replaced');
  const ctx = makeContext(MUTATED_SRC);
  const data = statusWith(false);
  const div = vm.runInContext('renderSafetyCard(data)', Object.assign(ctx, { data }));
  const html = div.innerHTML;
  const mutationCaughtRed = html.indexOf('Safety relay K4') !== -1;
  assert(mutationCaughtRed,
    'MUTATION: restoring the long "Safety relay K4" prefix must make the short-form check go red -- ' +
    'proves the check above is real, not vacuous. got: ' + html);
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
