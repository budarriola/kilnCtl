/* Node-only test harness for backup_page.html's deleteCount derivation
 * (MEDIUM, bkfinish review): the confirm dialog's X-Kiln-Config-Ack-Delete
 * count is computed client-side from the dry-run plan text's own
 * `l.indexOf('delete "') === 0` filter -- this pins that filter against the
 * LITERAL plan-line text backup_import.c's kiln_cfg_plan_add() calls emit
 * (`kiln_cfg_plan_add(plan, "delete \"%s\"", ...)` for a real queued
 * deletion, `kiln_cfg_plan_add(plan, "keep active \"%s\" (active slot is
 * never deleted)", ...)` for the HIGH 2 fix's informational line), so a
 * future edit to either side that breaks the match is caught here rather
 * than only live against a board.
 *
 * The informational "keep active ... (active slot is never deleted)" line
 * is the specific regression this guards: it contains the substring
 * "delete" (from "never deleted"), so a naive `indexOf('delete') !== -1`
 * filter would miscount it as a real deletion -- backup_page.html already
 * avoids this via the anchored `indexOf('delete "') === 0` check; this test
 * proves that choice against literal plan text, not just by reading the
 * source.
 *
 * Run: node firmware/KilnFW/App/test/test_backup_page.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const path = require('path');
const fs = require('fs');

const DRIVERS_DIR = resolveDriversDir(__dirname);
const PAGE_PATH = resolveDriverFile(DRIVERS_DIR, 'backup_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// Extract the exact deleteCount derivation line rather than reimplementing
// it, the same "read the real production line" approach test_lag_banner.js
// and test_profile_limit_warning.js use for their own extracted sections --
// a divergent reimplementation here would test itself, not the page.
const DELETE_COUNT_LINE = SRC.split('\n').find(function (l) {
  return l.indexOf('var deleteCount') !== -1;
});
assert(!!DELETE_COUNT_LINE, 'sanity: found the deleteCount derivation line in backup_page.html');
assert(DELETE_COUNT_LINE.indexOf('delete "') !== -1,
  'sanity: the derivation filters on the literal \'delete "\' plan-line prefix');

// Build the actual filter function from that literal line's own expression,
// isolating "planLines.filter(...)" as a real JS function value -- this runs
// the PRODUCTION expression, not a hand-copied stand-in.
const filterExprMatch = DELETE_COUNT_LINE.match(/planLines\.filter\(([\s\S]*)\)\.length;?\s*$/);
assert(!!filterExprMatch, 'sanity: deleteCount line has the expected planLines.filter(...).length shape');
const filterFn = eval('(' + filterExprMatch[1] + ')'); // eslint-disable-line no-eval

function deleteCountOf(planLines) {
  return planLines.filter(filterFn).length;
}

// ---------------------------------------------------------------------------
// Group 1: a real queued deletion is counted.
// ---------------------------------------------------------------------------
{
  const lines = ['delete "Old Slot"'];
  assert(deleteCountOf(lines) === 1, 'a real "delete \\"name\\"" line is counted');
}

// ---------------------------------------------------------------------------
// Group 2: the HIGH 2 fix's "keep active" informational line is NOT counted,
// even though it contains the substring "delete" (from "never deleted").
// This is the literal text kiln_cfg_plan_add() emits in backup_import.c.
// ---------------------------------------------------------------------------
{
  const lines = ['keep active "Current Firing Profile" (active slot is never deleted)'];
  assert(deleteCountOf(lines) === 0,
    'a "keep active ... (active slot is never deleted)" line is NOT counted as a deletion, ' +
    'despite containing the substring "delete"');
}

// ---------------------------------------------------------------------------
// Group 3: a realistic mixed plan -- two real deletions, one kept-active
// line, one create, one rename -- counts only the real deletions.
// ---------------------------------------------------------------------------
{
  const lines = [
    'delete "Stale A"',
    'keep active "Bisque Fast" (active slot is never deleted)',
    'create "Imported Config"',
    'rename "Old Name" -> "New Name"',
    'delete "Stale B"',
  ];
  assert(deleteCountOf(lines) === 2,
    'mixed plan: exactly the two real "delete \\"...\\"" lines are counted (got ' +
    deleteCountOf(lines) + ')');
}

// ---------------------------------------------------------------------------
// Group 4: no deletions at all (MERGE mode, or a MIRROR restore that matches
// every board slot) -- zero, not a false positive from any other line kind.
// ---------------------------------------------------------------------------
{
  const lines = [
    'create "New Config"',
    '"New Config" was the active kiln config in the backup (informational only -- re-apply ' +
      'from Kiln configs after restore)',
  ];
  assert(deleteCountOf(lines) === 0, 'a plan with no deletions counts zero');
}

// ---------------------------------------------------------------------------
// Group 5: an active slot whose NAME happens to start with "delete " is
// still correctly attributed to its own "keep active" line, not miscounted
// -- proves the anchor is on the LINE's own prefix, not a substring search
// anywhere in the line.
// ---------------------------------------------------------------------------
{
  const lines = ['keep active "delete me not" (active slot is never deleted)'];
  assert(deleteCountOf(lines) === 0,
    'an active slot literally named starting with "delete " is still not counted -- the ' +
    'anchor is the LINE prefix, not any substring');
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
