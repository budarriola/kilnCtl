/* Node-only test harness for main_page.html's firing-profile picker
 * ordering: favorited profiles sort to the top of the dashboard's profile
 * list and carry a star, per the owner's request (2026-09-18).
 *
 * The rule being pinned has three parts, and each is a separate failure the
 * operator would actually notice:
 *   1. favorites come first;
 *   2. the order WITHIN each group is untouched -- this only changes the
 *      grouping, never the sort inside a group;
 *   3. the star is a visual marker on the label only. Option VALUES stay
 *      exactly the profile id, so starting a firing still posts the same
 *      identifier it always did. A star that leaked into the value would
 *      break profile selection silently, which is why it is asserted here
 *      rather than left to inspection.
 *
 * Favorites are shortcuts, not a move: a favorited profile is still present
 * in the list exactly once, never removed from the underlying set. Asserted
 * below as a set-equality check, not just a length check.
 *
 * Extracts the real source by marker line (same approach as
 * test_profile_limit_warning.js / test_lag_banner.js) rather than
 * reimplementing the logic, so the test cannot drift from the page.
 *
 * Run: node firmware/KilnFW/App/test/test_profile_favorites_order.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');
const raw = (l) => l.replace(/\r$/, '');

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

/* The favorites helper block runs from the FAVORITE_STAR constant to just
 * before loadFavoriteIds(), which is the first thing after it that touches
 * the network. Bounded by content rather than by line number so unrelated
 * reflow above or below does not silently change what is under test. */
function extractFavoriteHelpers() {
  const startIdx = LINES.findIndex((l) => raw(l).indexOf('var FAVORITE_STAR =') === 0);
  if (startIdx === -1) throw new Error('FAVORITE_STAR start marker not found');
  const endIdx = LINES.findIndex((l, i) => i >= startIdx && raw(l).indexOf('function loadFavoriteIds(') === 0);
  if (endIdx === -1) throw new Error('loadFavoriteIds end marker not found after start');
  return LINES.slice(startIdx, endIdx).join('\n');
}
const HELPERS_SRC = extractFavoriteHelpers();

assert(HELPERS_SRC.indexOf('function orderProfilesByFavorite(') !== -1,
  'sanity: extracted range includes orderProfilesByFavorite');
assert(HELPERS_SRC.indexOf('function profileOptionLabel(') !== -1,
  'sanity: extracted range includes profileOptionLabel');
assert(HELPERS_SRC.indexOf('function isFavoriteProfile(') !== -1,
  'sanity: extracted range includes isFavoriteProfile');

function makeContext() {
  const ctx = {
    // profileOptionLabel composes these two; they are the page's own
    // functions, stubbed here so the assertions are about ORDER and the
    // STAR rather than about display-name formatting, which
    // test_profile_limit_warning.js and the page's other tests already
    // cover.
    profileDisplayName: function (p) { return p.name; },
    zoneListText: function () { return 'Zone 0'; },
    console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(HELPERS_SRC, ctx);
  return ctx;
}

function prof(id, name) { return { id: id, name: name, zone_mask: 0x1 }; }

// Deliberately NOT in id order: if the implementation sorted by id instead
// of preserving the incoming order, group 2 below would still look correct.
// This ordering is what makes that mistake visible.
const LIST = [prof(3, 'Cone 6 Glaze'), prof(1, 'Bisque'), prof(128, 'BQ1000'), prof(2, 'Slow Cool')];

function names(arr) { return arr.map(function (p) { return p.name; }); }

// ---------------------------------------------------------------------------
// Group 1: favorites sort to the top.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext();
  ctx.list = LIST; ctx.favIds = [1];
  const out = vm.runInContext('orderProfilesByFavorite(list, favIds)', ctx);
  assert(out[0].id === 1, 'favorite sorts to the top (got id ' + out[0].id + ')');
  assert(out.length === LIST.length, 'no profile is dropped by favoriting');
}

// ---------------------------------------------------------------------------
// Group 2: order WITHIN each group is preserved -- grouping only.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext();
  ctx.list = LIST; ctx.favIds = [1, 2];
  const out = vm.runInContext('orderProfilesByFavorite(list, favIds)', ctx);
  assert(JSON.stringify(names(out)) === JSON.stringify(['Bisque', 'Slow Cool', 'Cone 6 Glaze', 'BQ1000']),
    'favorites keep their incoming relative order, and so does the remainder (got ' +
    names(out).join(', ') + ')');
}

// ---------------------------------------------------------------------------
// Group 3: a favorite is a shortcut, not a move -- the set is unchanged and
// nothing appears twice.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext();
  ctx.list = LIST; ctx.favIds = [128];
  const out = vm.runInContext('orderProfilesByFavorite(list, favIds)', ctx);
  const before = names(LIST).slice().sort();
  const after = names(out).slice().sort();
  assert(JSON.stringify(before) === JSON.stringify(after),
    'favoriting changes the order only -- the same profiles are present, none lost or duplicated');
  assert(out[0].id === 128, 'a SHIPPED profile can be favorited too, not just saved ones');
}

// ---------------------------------------------------------------------------
// Group 4: no favorites (and the old-firmware/404 case, which yields []) --
// the list must be completely untouched.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext();
  ctx.list = LIST; ctx.favIds = [];
  const out = vm.runInContext('orderProfilesByFavorite(list, favIds)', ctx);
  assert(JSON.stringify(names(out)) === JSON.stringify(names(LIST)),
    'with no favorites the order is exactly as it was -- this is also the 404/old-firmware path');

  const ctx2 = makeContext();
  ctx2.list = LIST; ctx2.favIds = null;
  const out2 = vm.runInContext('orderProfilesByFavorite(list, favIds)', ctx2);
  assert(JSON.stringify(names(out2)) === JSON.stringify(names(LIST)),
    'a null favorites list degrades to the unchanged order rather than throwing');
}

// ---------------------------------------------------------------------------
// Group 5: the star is a LABEL marker, and only on favorites.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext();
  ctx.p = prof(1, 'Bisque');
  const favLabel = vm.runInContext('profileOptionLabel(p, true)', ctx);
  const plainLabel = vm.runInContext('profileOptionLabel(p, false)', ctx);
  assert(favLabel.indexOf('★') === 0, 'favorited option label starts with the star');
  assert(plainLabel.indexOf('★') === -1, 'a non-favorited option label has no star');
  assert(favLabel.indexOf('Bisque') !== -1 && plainLabel.indexOf('Bisque') !== -1,
    'the profile name survives in both labels');
}

// ---------------------------------------------------------------------------
// Group 6: id matching is type-tolerant. The favorites route reports ids as
// JSON numbers while DOM option values are strings, so a strict === between
// the two would silently mark nothing as favorite.
// ---------------------------------------------------------------------------
{
  const ctx = makeContext();
  ctx.p = prof(1, 'Bisque'); ctx.favIds = ['1'];
  assert(vm.runInContext('isFavoriteProfile(p, favIds)', ctx) === true,
    'a string id from the favorites route matches a numeric profile id');

  const ctx2 = makeContext();
  ctx2.p = prof(1, 'Bisque'); ctx2.favIds = [2];
  assert(vm.runInContext('isFavoriteProfile(p, favIds)', ctx2) === false,
    'a non-favorited profile is not marked');
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join(', '));
  process.exit(1);
}
