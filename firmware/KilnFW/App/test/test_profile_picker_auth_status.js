/* Node-only test for main_page.html's #profileSelect load-status handling
 * (owner report, 2026-09-27): loadProfileList() used to swallow a background
 * 401 from GET /api/profiles with an empty `.catch(function () {})`, leaving
 * the picker silently empty forever with no way to notice or retry. This
 * test extracts loadProfileList() and its status-line helpers VERBATIM from
 * the page (same discipline as test_profile_picker_optgroups.js) and checks:
 *
 *   1. A successful load hides #profilePickerStatus (clears any stale
 *      status text/class from a previous failed attempt).
 *   2. A rejection that window.kcIsAuthCancelled() recognizes (the
 *      AuthCancelled shape app.js's fetch wrapper throws on a declined/
 *      unraised login) shows a clickable "log in" prompt, NOT a bare error --
 *      and clicking it calls loadProfileList() again.
 *   3. Any other rejection (a genuine network/server failure) shows a
 *      visible "Could not load profiles." error, distinguishable by class
 *      from the login prompt.
 *   4. Negative-test control: a build of loadProfileList() with the
 *      catch's kcIsAuthCancelled() branch removed (both cases fall through
 *      to showProfilePickerError()) fails assertion 2 -- proving the assertion
 *      actually exercises the distinction rather than passing regardless.
 *      The mutation is applied to the extracted page source itself.
 *   5. A reload of the list keeps the operator's current pick (the select
 *      is rebuilt from scratch), with its own source-mutation negative control.
 *
 * Run: node firmware/KilnFW/App/test/test_profile_picker_auth_status.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const PAGE_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'main_page.html');
const SRC = fs.readFileSync(PAGE_PATH, 'utf8');
const LINES = SRC.split('\n');

// Covers profileTitleById, profileDisplayName, zoneListText, FAVORITE_STAR,
// orderProfilesByFavorite, isFavoriteProfile, profileOptionLabel,
// groupProfilesForPicker, loadFavoriteIds, hideProfilePickerStatus,
// showProfilePickerLoginPrompt, showProfilePickerError and loadProfileList
// itself, in one contiguous block -- exactly what ships, not a hand copy.
// A bare-'}' end marker (as extractRange() above uses) would stop at
// profileDisplayName's own closing brace, the first such line after the
// start marker -- so this extracts through an explicit stop line instead:
// the "-- Kiln config indicator" comment that immediately follows
// loadProfileList()'s closing brace in the page.
function extractThrough(startMarker, mustContain, stopMarker) {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l) === startMarker);
  if (startIdx === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const stopIdx = LINES.findIndex((l, i) => i > startIdx && raw(l) === stopMarker);
  if (stopIdx === -1) throw new Error('stop marker not found after start: ' + JSON.stringify(stopMarker));
  const text = LINES.slice(startIdx, stopIdx).join('\n');
  if (text.indexOf(mustContain) === -1) throw new Error('sanity: range missing ' + JSON.stringify(mustContain));
  return text;
}
const FULL_RANGE = extractThrough(
  'var profileTitleById = {};',
  'function loadProfileList()',
  '// -- Kiln config indicator (read-only) -----------------------------------'
);
if (FULL_RANGE.indexOf('function showProfilePickerLoginPrompt') === -1 ||
    FULL_RANGE.indexOf('function showProfilePickerError') === -1 ||
    FULL_RANGE.indexOf('function hideProfilePickerStatus') === -1) {
  throw new Error('sanity: extracted range does not include the status helpers');
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// ---- minimal fake DOM: one flat id registry, enough for this page's flat
// getElementById/createElement usage (no querySelector needed here). ----
function makeFakeDom() {
  const registry = {};
  function makeElement(tag) {
    return {
      tagName: tag,
      children: [],
      dataset: {},
      hidden: false,
      value: '',
      textContent: '',
      disabled: false,
      _innerHTML: '',
      appendChild(child) { this.children.push(child); },
      addEventListener(type, fn) { (this._listeners = this._listeners || {})[type] = (this._listeners[type] || []).concat(fn); },
      dispatch(type, evt) { ((this._listeners || {})[type] || []).forEach((fn) => fn(evt)); },
      set innerHTML(v) { this._innerHTML = v; this.children = []; },
      get innerHTML() { return this._innerHTML; },
      set className(v) { this._className = v; },
      get className() { return this._className; },
    };
  }
  function getOrMake(id) {
    if (!registry[id]) registry[id] = makeElement('div');
    return registry[id];
  }
  const doc = {
    createElement: (tag) => makeElement(tag),
    getElementById: (id) => getOrMake(id),
  };
  return { registry, document: doc };
}

function makeContext(kcIsAuthCancelledImpl, code) {
  const dom = makeFakeDom();
  // Pre-seed the two elements loadProfileList()/its helpers touch by id.
  dom.registry['profileSelect'] = dom.document.createElement('select');
  dom.registry['profilePickerStatus'] = dom.document.createElement('div');
  dom.registry['profilePickerStatus'].hidden = true;
  dom.registry['runBtn'] = dom.document.createElement('button');

  const ctx = {
    document: dom.document,
    window: { kcIsAuthCancelled: kcIsAuthCancelledImpl },
    console,
  };
  vm.createContext(ctx);
  vm.runInContext(code || FULL_RANGE, ctx);
  return { ctx, dom };
}

(async () => {
  // Group 1: a successful load hides the status line.
  {
    const { ctx, dom } = makeContext(() => false);
    dom.registry['profilePickerStatus'].hidden = false;
    dom.registry['profilePickerStatus'].className = 'status-fault';
    dom.registry['profilePickerStatus'].textContent = 'stale error text';
    ctx.fetch = function (url) {
      if (url === '/api/profiles') return Promise.resolve({ json: () => Promise.resolve([{ id: 1, name: 'A', zone_mask: 0 }]) });
      if (url === '/api/profiles/favorites') return Promise.resolve({ ok: true, json: () => Promise.resolve({ ids: [] }) });
      return Promise.reject(new Error('unexpected url ' + url));
    };
    await ctx.loadProfileList();
    const status = dom.registry['profilePickerStatus'];
    assert(status.hidden === true, 'successful load hides the status line');
    assert(status.textContent === '', 'successful load clears stale status text');
  }

  // Group 2: an AuthCancelled-shaped rejection shows the login prompt, not
  // the generic error -- and clicking it re-calls loadProfileList().
  {
    let calls = 0;
    const { ctx, dom } = makeContext((err) => !!(err && err.name === 'AuthCancelled'));
    ctx.fetch = function (url) {
      // loadFavoriteIds() fetches '/api/profiles/favorites' too and degrades
      // to an empty list on its own -- only '/api/profiles' itself is
      // counted here, so this tracks retries of the profile list specifically.
      if (url === '/api/profiles') calls++;
      const e = new Error('sign-in cancelled');
      e.name = 'AuthCancelled';
      return Promise.reject(e);
    };
    await ctx.loadProfileList();
    const status = dom.registry['profilePickerStatus'];
    assert(status.hidden === false, 'AuthCancelled rejection shows the status line');
    assert(status.className !== 'status-fault', 'AuthCancelled rejection is not the fault class');
    assert(status.children.length === 1 && status.children[0].tagName === 'button',
      'AuthCancelled rejection appends a login button, not plain error text');
    assert(calls === 1, 'sanity: one fetch call so far');
    status.children[0].dispatch('click');
    await Promise.resolve();
    assert(calls === 2, 'clicking the login prompt re-calls loadProfileList()');
  }

  // Group 3: any other rejection shows the visible error note.
  {
    const { ctx, dom } = makeContext(() => false);
    ctx.fetch = function () { return Promise.reject(new Error('network down')); };
    await ctx.loadProfileList();
    const status = dom.registry['profilePickerStatus'];
    assert(status.hidden === false, 'a real failure shows the status line');
    assert(status.className === 'status-fault', 'a real failure uses the fault class');
    assert(status.textContent === 'Could not load profiles.', 'a real failure names itself plainly');
  }

  // Group 4: negative-test control -- mutate the SHIPPED source so the
  // catch's AuthCancelled branch is dead (if (false)), keeping the same
  // recognizing kcIsAuthCancelled stub group 2 uses. Group 2's
  // distinguishing assertion must then fail, proving group 2 exercises that
  // branch in the page source rather than passing regardless of it.
  {
    const needle = 'if (window.kcIsAuthCancelled(err)) {';
    if (FULL_RANGE.indexOf(needle) === -1) throw new Error('sanity: negative-test needle not found');
    const mutated = FULL_RANGE.replace(needle, 'if (false) {');
    const { ctx, dom } = makeContext((err) => !!(err && err.name === 'AuthCancelled'), mutated);
    ctx.fetch = function () {
      const e = new Error('sign-in cancelled');
      e.name = 'AuthCancelled';
      return Promise.reject(e);
    };
    await ctx.loadProfileList();
    const status = dom.registry['profilePickerStatus'];
    assert(status.className === 'status-fault',
      'negative control: with the AuthCancelled branch removed from the source, the fault path runs -- group 2 is real');
  }

  // Group 5: a reload (e.g. the kc-login-driven one) keeps the operator's
  // current pick. The fake select models a real <select>: clearing
  // innerHTML resets value, and the first appended option becomes selected.
  {
    const run = async (code) => {
      const { ctx, dom } = makeContext(() => false, code);
      const sel = dom.registry['profileSelect'];
      Object.defineProperty(sel, 'innerHTML', {
        set(v) { this._innerHTML = v; this.children = []; this.value = ''; },
        get() { return this._innerHTML; },
      });
      sel.appendChild = function (child) {
        this.children.push(child);
        if (this.value === '') {
          const first = child.tagName === 'optgroup' ? child.children[0] : child;
          if (first && first.value !== undefined) this.value = String(first.value);
        }
      };
      sel.value = '2';
      ctx.fetch = function (url) {
        if (url === '/api/profiles') return Promise.resolve({ json: () => Promise.resolve([
          { id: 1, name: 'A', zone_mask: 0 }, { id: 2, name: 'B', zone_mask: 0 }]) });
        if (url === '/api/profiles/favorites') return Promise.resolve({ ok: true, json: () => Promise.resolve({ ids: [] }) });
        return Promise.reject(new Error('unexpected url ' + url));
      };
      await ctx.loadProfileList();
      return sel.value;
    };
    assert(await run() === '2', 'reloading the list keeps the operator\'s current pick');
    const needle = 'sel.value = prevValue;';
    if (FULL_RANGE.indexOf(needle) === -1) throw new Error('sanity: restore needle not found');
    assert(await run(FULL_RANGE.replace(needle, '')) !== '2',
      'negative control: without the restore the pick resets -- the positive case is real');
  }

  console.log('\n' + passed + ' passed, ' + failed + ' failed');
  if (failed) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
  process.exit(0);
})();
