/* Node-only test for profiles_page.html's PROFILE_SLOTS_100.md section 7
 * task 8 additions: the client-side name filter and the "recently fired"
 * group. Extracts filterProfilesByName(), recentlyFiredProfiles(),
 * renderList(), renderFavorites() and renderRecent() VERBATIM from the page
 * (same discipline as test_profiles_page_row_layout.js) and runs them
 * against a tiny fake DOM, so it cannot pass against a copy that has
 * drifted from what ships.
 *
 * Checks:
 *   1. filterProfilesByName() matches case-insensitively, by substring, and
 *      an empty query returns every item unchanged (order preserved).
 *   2. recentlyFiredProfiles() sorts newest-first by last_run_started_unix_s,
 *      excludes zero/never-fired, and excludes anything in its exclude set
 *      (favorites -- already shown in their own section).
 *   3. renderList() (the #list section) narrows to the filter query, while
 *      renderFavorites() (#favList) and renderRecent() (#recentSection/
 *      #recentList) are driven by favoriteIds/last_run_started_unix_s only
 *      and never consult filterQuery at all -- i.e. favorites and the
 *      recent group stay pinned and stable while filtering, structurally
 *      (not just by coincidence of test data).
 *   4. All of the above hold at both the current 8-slot scale and a
 *      simulated 100-row scale (PROFILES_MAX_COUNT is NOT bumped for this
 *      -- 100 fake rows are constructed in this test only).
 *
 * Run: node firmware/KilnFW/App/test/test_profiles_page_filter.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'profiles_page.html');
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
  extractRange('function isFav(id) {', '}') + '\n' +
  extractRange('function filterProfilesByName(items, query) {', '}') + '\n' +
  extractRange('function recentlyFiredProfiles(items, excludeIds, limit) {', '}') + '\n' +
  extractRange('function iconBtn(glyph, name, extraClass) {', '}') + '\n' +
  extractRange('function favToggleBtn(p) {', '}') + '\n' +
  extractRange('function profileActionsGroup(p) {', '}') + '\n' +
  extractRange('function selectionCheckbox(p) {', '}') + '\n' +
  extractRange('function buildProfileItem(p, selectable) {', '}') + '\n' +
  extractRange('function renderList() {', '}') + '\n' +
  extractRange('function renderFavorites() {', '}') + '\n' +
  extractRange('function renderRecent() {', '}');

assert(CODE.indexOf('function filterProfilesByName(') !== -1,
  'sanity: extracted range includes filterProfilesByName');
assert(CODE.indexOf('function recentlyFiredProfiles(') !== -1,
  'sanity: extracted range includes recentlyFiredProfiles');
assert(CODE.indexOf("document.getElementById('recentSection')") !== -1,
  'sanity: extracted renderRecent() touches #recentSection');

// ---- Structural check: renderList/renderFavorites/renderRecent's source
// text -- renderFavorites/renderRecent must never reference filterQuery,
// which is what makes "favorites/recent stay pinned and stable while
// filtering" true by construction rather than by test-data coincidence. ----
const RENDER_LIST_SRC = extractRange('function renderList() {', '}');
const RENDER_FAV_SRC = extractRange('function renderFavorites() {', '}');
const RENDER_RECENT_SRC = extractRange('function renderRecent() {', '}');
assert(RENDER_LIST_SRC.indexOf('filterQuery') !== -1,
  'renderList() consults filterQuery (the filtered section)');
assert(RENDER_FAV_SRC.indexOf('filterQuery') === -1,
  'renderFavorites() never references filterQuery -- favorites stay pinned/stable while filtering');
assert(RENDER_RECENT_SRC.indexOf('filterQuery') === -1,
  'renderRecent() never references filterQuery -- the recent group stays pinned/stable while filtering');

// ---------------------------------------------------------------------------
// Minimal fake DOM (same approach as test_profiles_page_row_layout.js),
// extended with style.display and a getElementById registry.
// ---------------------------------------------------------------------------
function makeFakeDom() {
  class FakeClassList {
    constructor(el) { this.el = el; }
    add(c) { const s = new Set(this.el._className.split(/\s+/).filter(Boolean)); s.add(c); this.el._className = Array.from(s).join(' '); }
  }
  class FakeElement {
    constructor(tag) {
      this.tagName = String(tag).toUpperCase();
      this._className = '';
      this.children = [];
      this.parentNode = null;
      this._text = '';
      this._html = '';
      this._attrs = {};
      this._listeners = {};
      this.style = { display: '' };
    }
    get className() { return this._className; }
    set className(v) { this._className = v; }
    get classList() { return new FakeClassList(this); }
    appendChild(child) { child.parentNode = this; this.children.push(child); return child; }
    setAttribute(k, v) { this._attrs[k] = v; }
    getAttribute(k) { return this._attrs[k]; }
    addEventListener(type, fn) { (this._listeners[type] = this._listeners[type] || []).push(fn); }
    set textContent(v) { this._text = v; this.children = []; }
    get textContent() { return this._text; }
    set innerHTML(v) { this._html = v; this.children = []; this._text = ''; }
    get innerHTML() { return this._html; }
  }
  const registry = {};
  ['list', 'favList', 'recentSection', 'recentList', 'catMsg'].forEach((id) => {
    registry[id] = new FakeElement(id === 'recentSection' ? 'div' : 'div');
  });
  const document = {
    createElement: (tag) => new FakeElement(tag),
    getElementById: (id) => registry[id],
  };
  return { document, registry, FakeElement };
}

function makeContext(dom) {
  const context = {
    document: dom.document,
    console: console,
    window: { kcEscapeHtml: (s) => String(s) },
    selectMode: null,
    selectedIds: {},
    favoriteIds: [],
    updateSelectionBar: function () {},
    isMarked: function (v) { return v === 'too_fast' || v === 'unreachable'; },
    fzMark: function () {},
    FZ_LABEL: { too_fast: 'too fast', unreachable: 'unreachable' },
    segmentTableHtml: function () { return '<table></table>'; },
    savedProfiles: [],
    catalogEntries: [],
    filterQuery: '',
  };
  vm.createContext(context);
  vm.runInContext(CODE, context);
  return context;
}

function mkProfile(id, name, opts) {
  opts = opts || {};
  return Object.assign({
    id: id, name: name, builtin: false, zone_mask: 0x1, segment_count: 2,
    feasibility: 'unknown', segments: null, last_run_started_unix_s: 0,
  }, opts);
}

// ---------------------------------------------------------------------------
// Part 1: filterProfilesByName() -- pure function, no DOM.
// ---------------------------------------------------------------------------
{
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  const items = [
    mkProfile(1, 'Bisque Slow'),
    mkProfile(2, 'cone6_glaze'),
    mkProfile(3, 'Bisque Fast'),
  ];
  assert(ctx.filterProfilesByName(items, '').length === 3, 'empty query returns every item');
  assert(ctx.filterProfilesByName(items, '').map((p) => p.id).join(',') === '1,2,3',
    'empty query preserves original order');
  const bisque = ctx.filterProfilesByName(items, 'bisque');
  assert(bisque.length === 2 && bisque[0].id === 1 && bisque[1].id === 3,
    'case-insensitive substring match ("bisque" finds both Bisque profiles)');
  const cone = ctx.filterProfilesByName(items, 'CONE6');
  assert(cone.length === 1 && cone[0].id === 2, 'uppercase query still matches lowercase name');
  assert(ctx.filterProfilesByName(items, 'zzz-not-present').length === 0,
    'a query matching nothing returns an empty list, not an error');
}

// ---------------------------------------------------------------------------
// Part 2: recentlyFiredProfiles() -- newest-first, excludes never-fired and
// excluded (favorite) ids.
// ---------------------------------------------------------------------------
{
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  const items = [
    mkProfile(1, 'Never fired'),                                     // last_run 0
    mkProfile(2, 'Fired long ago', { last_run_started_unix_s: 1000 }),
    mkProfile(3, 'Fired recently', { last_run_started_unix_s: 9000 }),
    mkProfile(4, 'Favorite but recent', { last_run_started_unix_s: 8000 }),
  ];
  const recent = ctx.recentlyFiredProfiles(items, { '4': true }, 5);
  assert(recent.length === 2, 'never-fired (0) and excluded (favorite) ids are dropped');
  assert(recent[0].id === 3 && recent[1].id === 2, 'sorted newest-first by last_run_started_unix_s');
  const limited = ctx.recentlyFiredProfiles(items, {}, 1);
  assert(limited.length === 1 && limited[0].id === 3, 'limit caps the returned count');
}

// ---------------------------------------------------------------------------
// Part 3: renderList() narrows with the filter; renderFavorites()/
// renderRecent() are unaffected by it -- at 8 real slots.
// ---------------------------------------------------------------------------
function runIntegrationCheck(scaleLabel, items) {
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  ctx.savedProfiles = items;
  ctx.catalogEntries = [];
  ctx.favoriteIds = [items[0].id, items[1].id]; // first two are favorites

  // No filter: list has everything, favorites has 2, recent has whoever fired.
  ctx.filterQuery = '';
  ctx.renderList();
  ctx.renderFavorites();
  ctx.renderRecent();
  const fullListCount = dom.registry.list.children.length;
  assert(fullListCount === items.length,
    `[${scaleLabel}] unfiltered #list renders every item (${items.length})`);
  assert(dom.registry.favList.children.length === 2,
    `[${scaleLabel}] #favList always renders exactly the 2 favorites`);
  const recentCountUnfiltered = dom.registry.recentList.children.length;

  // Apply a filter that matches only one item's name, and re-render ONLY
  // #list (exactly what the page's input listener does) -- favorites/recent
  // must stay exactly as they were.
  const target = items[Math.floor(items.length / 2)];
  ctx.filterQuery = target.name.toUpperCase(); // also exercises case-insensitivity end to end
  ctx.renderList();
  assert(dom.registry.list.children.length === 1,
    `[${scaleLabel}] filtering #list narrows to the one matching name`);
  assert(dom.registry.favList.children.length === 2,
    `[${scaleLabel}] #favList is unchanged by a filter that only re-renders #list`);
  assert(dom.registry.recentList.children.length === recentCountUnfiltered,
    `[${scaleLabel}] #recentList is unchanged by a filter that only re-renders #list`);

  // Restore empty filter -- full list comes back.
  ctx.filterQuery = '';
  ctx.renderList();
  assert(dom.registry.list.children.length === items.length,
    `[${scaleLabel}] clearing the filter restores the full #list`);
}

// 8 real slots (today's PROFILES_MAX_COUNT).
{
  const items = [];
  for (let i = 1; i <= 8; i++) {
    items.push(mkProfile(i, 'Slot ' + i, { last_run_started_unix_s: i % 2 === 0 ? 1000 + i : 0 }));
  }
  runIntegrationCheck('8 slots', items);
}

// 100 simulated rows (PROFILE_SLOTS_100.md's target scale -- fake test
// data only, PROFILES_MAX_COUNT itself is NOT touched by this task).
{
  const items = [];
  for (let i = 1; i <= 100; i++) {
    items.push(mkProfile(i, 'Profile ' + i, { last_run_started_unix_s: i % 3 === 0 ? 100000 + i : 0 }));
  }
  runIntegrationCheck('100 simulated slots', items);
  // Recent group is capped (limit 5 in renderRecent()), even though far
  // more than 5 of the 100 fake rows have a nonzero last_run.
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  ctx.savedProfiles = items;
  ctx.catalogEntries = [];
  ctx.favoriteIds = [];
  ctx.filterQuery = '';
  ctx.renderRecent();
  assert(dom.registry.recentList.children.length === 5,
    'at 100 simulated rows, the recent group still caps at 5, not all matching rows');
  assert(dom.registry.recentSection.style.display === '',
    'recent section is shown (display cleared) when it has entries');
}

// Recent section hides itself when nothing has ever fired.
{
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  ctx.savedProfiles = [mkProfile(1, 'Never fired A'), mkProfile(2, 'Never fired B')];
  ctx.catalogEntries = [];
  ctx.favoriteIds = [];
  ctx.renderRecent();
  assert(dom.registry.recentSection.style.display === 'none',
    'recent section hides itself (display:none) when no profile has ever fired');
}

// ---------------------------------------------------------------------------
// Part 4: bulk select mode -- a name filter that hides a checked row must
// drop that id from selectedIds and let updateSelectionBar() re-derive the
// "Delete selected (N)" count from what's left, so an id the operator can
// no longer see is never silently still queued for delete/export.
// ---------------------------------------------------------------------------
{
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  let lastN = null;
  ctx.updateSelectionBar = function () { lastN = Object.keys(ctx.selectedIds).length; };
  ctx.savedProfiles = [
    mkProfile(1, 'Bisque Slow'),
    mkProfile(2, 'cone6_glaze'),
    mkProfile(3, 'Bisque Fast'),
  ];
  ctx.catalogEntries = [];
  ctx.favoriteIds = [];
  ctx.selectMode = 'delete';
  ctx.selectedIds = { '1': true, '2': true, '3': true };
  ctx.filterQuery = '';
  ctx.renderList();
  assert(Object.keys(ctx.selectedIds).length === 3,
    'no filter: nothing hidden, all 3 checked ids survive renderList()');

  ctx.filterQuery = 'bisque';
  ctx.renderList();
  assert(Object.keys(ctx.selectedIds).sort().join(',') === '1,3',
    'filtering to "bisque" drops id 2 (now hidden) from selectedIds, keeps 1 and 3 (still visible)');
  assert(lastN === 2, 'updateSelectionBar() is called after the drop and sees the reduced count');

  ctx.filterQuery = '';
  ctx.renderList();
  assert(Object.keys(ctx.selectedIds).sort().join(',') === '1,3',
    'clearing the filter does not resurrect a previously-dropped selection (2 stays gone)');
}

// ---------------------------------------------------------------------------
// Part 5: filterProfilesByName() normalizes both the query and each name
// with NFC before comparing, so a precomposed name (single codepoint, e.g.
// "\u00e9") matches a decomposed query (base + combining mark, "e\u0301")
// and vice versa -- same visible text, different byte sequence.
// ---------------------------------------------------------------------------
{
  const dom = makeFakeDom();
  const ctx = makeContext(dom);
  const precomposed = 'Bisqu\u00e9';       // "Bisqu\u00e9", single codepoint e-acute
  const decomposed = 'Bisque\u0301';       // "Bisque" + combining acute accent
  const items = [mkProfile(1, precomposed)];
  const viaDecomposedQuery = ctx.filterProfilesByName(items, decomposed);
  assert(viaDecomposedQuery.length === 1,
    'a decomposed query matches a precomposed name of the same visible text (NFC-normalized)');
  const items2 = [mkProfile(2, decomposed)];
  const viaPrecomposedQuery = ctx.filterProfilesByName(items2, precomposed);
  assert(viaPrecomposedQuery.length === 1,
    'a precomposed query matches a decomposed name of the same visible text (NFC-normalized)');
}

console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed > 0) {
  console.log('Failures: ' + failures.join('; '));
  process.exit(1);
}
process.exit(0);
