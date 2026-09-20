/* Node-only test for main_page.html's PROFILE_SLOTS_100_PLAN.md section 7
 * task 8 addition: <optgroup> grouping (Favorites / Recently fired / All)
 * on #profileSelect, replacing the old flat favorites-first partition.
 *
 * Extracts groupProfilesForPicker() and appendGroup() VERBATIM from the page
 * (same discipline as test_profile_favorites_order.js) so the test cannot
 * pass against a copy that has drifted from what ships. Checks:
 *   1. groupProfilesForPicker() partitions into favorites / recent / all
 *      with no item repeated across groups and no item dropped.
 *   2. .recent is sorted newest-fired-first, excludes favorites, and never
 *      includes a never-fired (0) profile.
 *   3. End to end: appendGroup(), called in Favorites/Recent/All order (the
 *      order loadProfileList() calls it in), appends <optgroup> elements to
 *      #profileSelect in that same order, and an EMPTY group produces no
 *      <optgroup> at all (no empty "Recently fired" heading when nothing
 *      has ever fired).
 *   4. Holds at both 8 real slots and a simulated 100 rows (fake test data
 *      only -- PROFILES_MAX_COUNT is not touched).
 *
 * Run: node firmware/KilnFW/App/test/test_profile_picker_optgroups.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
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

const GROUP_FN_SRC = extractRange('function groupProfilesForPicker(list, favIds, recentLimit) {', '}');
const APPEND_FN_SRC = extractRange('    function appendGroup(label, items, isFav) {', '    }');
const DISPLAY_NAME_SRC = extractRange('function profileDisplayName(p) {', '}');
const ZONE_LIST_SRC = extractRange('function zoneListText(mask) {', '}');
const OPTION_LABEL_SRC = extractRange('function profileOptionLabel(p, isFav) {', '}');
assert(GROUP_FN_SRC.indexOf('function groupProfilesForPicker(') !== -1,
  'sanity: extracted range includes groupProfilesForPicker');
assert(APPEND_FN_SRC.indexOf('optgroup') !== -1,
  'sanity: extracted appendGroup() creates an <optgroup>');

const CODE = DISPLAY_NAME_SRC + '\n' + ZONE_LIST_SRC + '\n' + OPTION_LABEL_SRC + '\n' +
  GROUP_FN_SRC + '\n' + APPEND_FN_SRC;

function makeFakeDom() {
  class FakeElement {
    constructor(tag) {
      this.tagName = String(tag).toUpperCase();
      this.children = [];
      this.value = '';
      this.textContent = '';
      this.label = '';
    }
    appendChild(child) { this.children.push(child); return child; }
  }
  const registry = { profileSelect: new FakeElement('select') };
  const document = {
    createElement: (tag) => new FakeElement(tag),
    getElementById: (id) => registry[id],
  };
  return { document, registry };
}

function makeContext(dom) {
  const context = {
    console: console,
    document: dom.document,
    // FAVORITE_STAR/zoneName are page globals this test doesn't otherwise
    // touch; zoneName is stubbed (its own behavior is out of scope here).
    FAVORITE_STAR: '★',
    zoneName: function (b) { return 'Zone ' + (b + 1); },
  };
  vm.createContext(context);
  vm.runInContext(CODE, context);
  return context;
}

function mkProfile(id, name, opts) {
  opts = opts || {};
  return Object.assign({ id: id, name: name, zone_mask: 0x1, last_run_started_unix_s: 0 }, opts);
}

// ---------------------------------------------------------------------------
// Part 1: groupProfilesForPicker() -- pure, no DOM.
// ---------------------------------------------------------------------------
function runGroupingCheck(scaleLabel, items, favCount) {
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  const favIds = items.slice(0, favCount).map((p) => p.id);
  const groups = ctx.groupProfilesForPicker(items, favIds);

  assert(groups.favorites.length === favCount,
    `[${scaleLabel}] exactly the favorited items land in .favorites`);

  const allIdsOut = groups.favorites.concat(groups.recent).concat(groups.all)
    .map((p) => p.id).sort((a, b) => a - b);
  const allIdsIn = items.map((p) => p.id).sort((a, b) => a - b);
  assert(JSON.stringify(allIdsOut) === JSON.stringify(allIdsIn),
    `[${scaleLabel}] every item appears in exactly one group (none dropped, none duplicated)`);

  const seen = {};
  let noOverlap = true;
  groups.favorites.concat(groups.recent).concat(groups.all).forEach((p) => {
    if (seen[p.id]) noOverlap = false;
    seen[p.id] = true;
  });
  assert(noOverlap, `[${scaleLabel}] no item repeats across favorites/recent/all`);

  const recentSorted = groups.recent.slice()
    .sort((a, b) => b.last_run_started_unix_s - a.last_run_started_unix_s);
  assert(JSON.stringify(groups.recent.map((p) => p.id)) === JSON.stringify(recentSorted.map((p) => p.id)),
    `[${scaleLabel}] .recent is sorted newest-fired-first`);
  assert(groups.recent.every((p) => favIds.indexOf(p.id) === -1),
    `[${scaleLabel}] .recent never repeats a favorite`);
  assert(groups.recent.every((p) => p.last_run_started_unix_s > 0),
    `[${scaleLabel}] .recent never includes a never-fired (0) profile`);
}

runGroupingCheck('8 slots', [1, 2, 3, 4, 5, 6, 7, 8].map((i) =>
  mkProfile(i, 'Slot ' + i, { last_run_started_unix_s: i % 2 === 0 ? 1000 + i : 0 })), 2);

{
  const items = [];
  for (let i = 1; i <= 100; i++) {
    items.push(mkProfile(i, 'Profile ' + i, { last_run_started_unix_s: i % 3 === 0 ? 100000 + i : 0 }));
  }
  runGroupingCheck('100 simulated slots', items, 3);
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  const groups = ctx.groupProfilesForPicker(items, [1, 2, 3], 5);
  assert(groups.recent.length === 5,
    'at 100 simulated rows, .recent still caps at the default limit (5), not every fired profile');
}

// ---------------------------------------------------------------------------
// Part 2: end-to-end <optgroup> order and empty-group suppression, driving
// the page's own appendGroup() directly (not reimplemented).
// ---------------------------------------------------------------------------
function runOptgroupOrderCheck(scaleLabel, items, favCount) {
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  const favIds = items.slice(0, favCount).map((p) => p.id);
  const groups = ctx.groupProfilesForPicker(items, favIds);
  const sel = dom.registry.profileSelect;
  ctx.sel = sel; // appendGroup() closes over `sel` as a free variable in the page

  ctx.appendGroup('Favorites', groups.favorites, true);
  ctx.appendGroup('Recently fired', groups.recent, false);
  ctx.appendGroup('All', groups.all, false);

  const optgroups = sel.children.filter((c) => c.tagName === 'OPTGROUP');
  const nonEmptyGroupCount = [groups.favorites, groups.recent, groups.all]
    .filter((g) => g.length > 0).length;
  assert(optgroups.length === nonEmptyGroupCount,
    `[${scaleLabel}] one <optgroup> per non-empty group, no <optgroup> for an empty one`);

  const labels = optgroups.map((og) => og.label);
  const expectedOrder = ['Favorites', 'Recently fired', 'All']
    .filter((label, i) => [groups.favorites, groups.recent, groups.all][i].length > 0);
  assert(JSON.stringify(labels) === JSON.stringify(expectedOrder),
    `[${scaleLabel}] <optgroup> order is Favorites, then Recently fired, then All (got: ${labels.join(', ')})`);

  const favOg = optgroups.find((og) => og.label === 'Favorites');
  if (favOg && groups.favorites.length > 0) {
    assert(favOg.children.length === groups.favorites.length,
      `[${scaleLabel}] Favorites <optgroup> holds exactly the favorited <option>s`);
    assert(String(favOg.children[0].value) === String(groups.favorites[0].id),
      `[${scaleLabel}] an <option>'s value is still the raw profile id, unchanged by grouping`);
  }
}

runOptgroupOrderCheck('8 slots', [1, 2, 3, 4, 5, 6, 7, 8].map((i) =>
  mkProfile(i, 'Slot ' + i, { last_run_started_unix_s: i % 2 === 0 ? 1000 + i : 0 })), 2);

{
  const items = [];
  for (let i = 1; i <= 100; i++) {
    items.push(mkProfile(i, 'Profile ' + i, { last_run_started_unix_s: i % 3 === 0 ? 100000 + i : 0 }));
  }
  runOptgroupOrderCheck('100 simulated slots', items, 3);
}

// No profile has ever fired -- "Recently fired" must not appear at all.
{
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  const items = [mkProfile(1, 'A'), mkProfile(2, 'B'), mkProfile(3, 'C')];
  const groups = ctx.groupProfilesForPicker(items, [1]);
  ctx.sel = dom.registry.profileSelect;
  ctx.appendGroup('Favorites', groups.favorites, true);
  ctx.appendGroup('Recently fired', groups.recent, false);
  ctx.appendGroup('All', groups.all, false);
  const labels = dom.registry.profileSelect.children.map((og) => og.label);
  assert(labels.indexOf('Recently fired') === -1,
    'an empty Recently-fired group is never rendered as an <optgroup>');
  assert(JSON.stringify(labels) === JSON.stringify(['Favorites', 'All']),
    'with nothing ever fired, only Favorites and All appear, in that order');
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join('; '));
  process.exit(1);
}
process.exit(0);
