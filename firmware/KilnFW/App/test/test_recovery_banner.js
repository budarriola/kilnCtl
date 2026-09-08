/* Node-only test harness for app.js's recovery-mode dashboard banner
 * (owner request 2026-09-08: "when in recovery mode it should show a banner
 * on the Dashboard"): buildRecoveryBanner / setRecoveryBanner /
 * pollRecoveryMode, sourced from the ALREADY EXISTING GET /api/ota/esp/status
 * route's `recovery_mode` field (ota_http_esp.c's
 * ota_esp_status_get_handler()) -- no new endpoint, no new JSON field.
 *
 * Extracts the real source (same extraction-by-marker-line approach as
 * test_lag_banner.js/test_firing_chart.js) from app.js -- app.js is loaded
 * on every *_page.html, not just main_page.html, so this banner is shared
 * across pages "for free" -- rather than reimplementing the logic, and runs
 * it in a Node vm context with a small document/fetch stub.
 *
 * Run: node firmware/KilnFW/App/test/test_recovery_banner.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const { resolveDriversDir, resolveDriverFile } = require('./_drivers_layout.js');
const APP_JS_PATH = resolveDriverFile(resolveDriversDir(__dirname), 'app.js');
const SRC = fs.readFileSync(APP_JS_PATH, 'utf8');
const LINES = SRC.split('\n');

function extractRange(startMarker, endMarker) {
  const raw = (l) => l.replace(/\r$/, '');
  const startIdx = LINES.findIndex((l) => raw(l) === startMarker);
  if (startIdx === -1) throw new Error('start marker not found: ' + JSON.stringify(startMarker));
  const endIdx = LINES.findIndex((l, i) => i >= startIdx && raw(l) === endMarker);
  if (endIdx === -1) throw new Error('end marker not found after start: ' + JSON.stringify(endMarker));
  return LINES.slice(startIdx, endIdx + 1).join('\n');
}

const RECOVERY_SRC = extractRange(
  '  var RECOVERY_POLL_MS = 20000;',
  '  window.setRecoveryBanner = setRecoveryBanner;'
);
assert_sanity();
function assert_sanity() {
  if (RECOVERY_SRC.indexOf('function buildRecoveryBanner()') === -1) {
    throw new Error('sanity: extracted range does not include buildRecoveryBanner');
  }
  if (RECOVERY_SRC.indexOf('function pollRecoveryMode()') === -1) {
    throw new Error('sanity: extracted range does not include pollRecoveryMode');
  }
}

let passed = 0, failed = 0;
const failures = [];
function assert(cond, label) {
  if (cond) { passed++; console.log('PASS: ' + label); }
  else { failed++; failures.push(label); console.log('FAIL: ' + label); }
}

// Minimal fake element: enough for the attribute/class/text assertions this
// file needs, and for buildRecoveryBanner()'s insertBefore/appendChild calls
// on its ancestors.
function makeEl() {
  const attrs = {};
  return {
    className: '',
    textContent: '',
    parentNode: null,
    children: [],
    setAttribute: function (k, v) { attrs[k] = v; },
    removeAttribute: function (k) { delete attrs[k]; },
    hasAttribute: function (k) { return Object.prototype.hasOwnProperty.call(attrs, k); },
    insertBefore: function (child, ref) {
      child.parentNode = this;
      const idx = ref ? this.children.indexOf(ref) : -1;
      if (idx === -1) this.children.push(child); else this.children.splice(idx, 0, child);
    },
    appendChild: function (child) { child.parentNode = this; this.children.push(child); },
  };
}

// fetchQueue: an array of {ok, json} (or a rejection) consumed in order by
// successive fetch('/api/ota/esp/status') calls, so each test controls
// exactly what the "board" answers on that tick without a real server.
function makeContext(opts) {
  opts = opts || {};
  const body = makeEl();
  const topbar = makeEl();
  topbar.parentNode = body;
  body.children.push(topbar);
  const fetchCalls = [];
  const fetchQueue = opts.fetchQueue || [];
  const ctx = {
    document: {
      visibilityState: 'visible',
      body: body,
      createElement: function () { return makeEl(); },
      querySelector: function (sel) { return sel === '.kc-topbar' ? topbar : null; },
      // setRecoveryBanner() (d89256fe, 2026-09-08 UI aggregate review) now
      // looks up #runBtn to keep the Start control disabled in lockstep
      // with the banner. This page-level fixture has no #runBtn (same as
      // most *_page.html app.js loads on, per that commit's own comment),
      // so mimic that real "not on this page" case rather than adding an
      // element none of these tests exercise.
      getElementById: function () { return null; },
    },
    window: {},
    bannerEl: opts.bannerEl || null, // simulates the conn-banner built just above this block in real app.js
    fetch: function (url) {
      fetchCalls.push(url);
      const next = fetchQueue.shift();
      if (!next) return Promise.reject(new Error('no fixture queued'));
      if (next.reject) return Promise.reject(new Error(next.reject));
      return Promise.resolve({ ok: next.ok !== false, status: next.status || 200, json: function () { return Promise.resolve(next.json); } });
    },
    console: console,
  };
  vm.createContext(ctx);
  vm.runInContext(RECOVERY_SRC, ctx);
  return { ctx, fetchCalls };
}

function flush() {
  // Let the fetch()/then() microtask chain settle before the test reads
  // the banner's post-poll state.
  return new Promise((resolve) => setImmediate(resolve));
}

(async () => {
  // -------------------------------------------------------------------
  // Group 1: buildRecoveryBanner() shape -- correct class, starts hidden,
  // carries the operator-facing explanation text (not just "RECOVERY MODE").
  // -------------------------------------------------------------------
  {
    const { ctx } = makeContext();
    const el = vm.runInContext('buildRecoveryBanner()', ctx);
    assert(el.className === 'kc-recovery-banner', 'buildRecoveryBanner: correct class name');
    assert(el.hasAttribute('hidden'), 'buildRecoveryBanner: starts hidden');
    assert(el.textContent.indexOf('RECOVERY MODE') !== -1, 'banner text names RECOVERY MODE');
    assert(el.textContent.indexOf('profile executor') !== -1 && el.textContent.indexOf('autotune') !== -1,
      'banner text names the specific subsystems that are OFF (profile executor, autotune)');
    assert(el.textContent.indexOf('config filesystem') !== -1 || el.textContent.indexOf('fallback') !== -1,
      'banner text mentions the config filesystem / fallback-settings state');
    assert(el.textContent.indexOf('Firing is NOT available') !== -1,
      'banner text says firing is unavailable, not just that recovery mode is on');
    assert(el.textContent.indexOf('Exit recovery mode') !== -1 || el.textContent.indexOf('healthy boot') !== -1,
      'banner text says how to get out of recovery mode');
  }

  // -------------------------------------------------------------------
  // Group 2: pollRecoveryMode() with recovery_mode:true shows the banner;
  // this is the exact field/endpoint the owner asked to be verified, not
  // assumed -- GET /api/ota/esp/status, field "recovery_mode".
  // -------------------------------------------------------------------
  {
    const { ctx, fetchCalls } = makeContext({ fetchQueue: [{ json: { recovery_mode: true, phase: 'idle' } }] });
    const bannerEl = vm.runInContext('buildRecoveryBanner()', ctx);
    ctx.recoveryBannerEl = bannerEl;
    vm.runInContext('recoveryBannerEl = bannerEl', Object.assign(ctx, { bannerEl }));
    vm.runInContext('pollRecoveryMode()', ctx);
    await flush();
    assert(fetchCalls[0] === '/api/ota/esp/status', 'pollRecoveryMode fetches GET /api/ota/esp/status');
    assert(!bannerEl.hasAttribute('hidden'), 'recovery_mode:true -- banner is shown');
  }

  // -------------------------------------------------------------------
  // Group 3: recovery_mode:false -- banner stays/becomes absent (hidden).
  // -------------------------------------------------------------------
  {
    const { ctx } = makeContext({ fetchQueue: [{ json: { recovery_mode: false, phase: 'idle' } }] });
    const bannerEl = vm.runInContext('buildRecoveryBanner()', ctx);
    vm.runInContext('recoveryBannerEl = bannerEl', Object.assign(ctx, { bannerEl }));
    vm.runInContext('pollRecoveryMode()', ctx);
    await flush();
    assert(bannerEl.hasAttribute('hidden'), 'recovery_mode:false -- banner is absent (hidden)');
  }

  // -------------------------------------------------------------------
  // Group 4: a failed poll (board unreachable) must NOT clear an
  // already-shown recovery banner -- a network blip is not evidence the
  // board left recovery mode. This is the "must not be dismissible in a
  // way that hides a still-active recovery state" requirement applied to
  // polling, not just to a close button.
  // -------------------------------------------------------------------
  {
    const { ctx } = makeContext({ fetchQueue: [{ json: { recovery_mode: true } }, { reject: 'network down' }] });
    const bannerEl = vm.runInContext('buildRecoveryBanner()', ctx);
    vm.runInContext('recoveryBannerEl = bannerEl', Object.assign(ctx, { bannerEl }));
    vm.runInContext('pollRecoveryMode()', ctx);
    await flush();
    assert(!bannerEl.hasAttribute('hidden'), 'setup: banner shown after first (successful) poll');
    vm.runInContext('pollRecoveryMode()', ctx);
    await flush();
    assert(!bannerEl.hasAttribute('hidden'), 'a failed poll leaves an already-shown recovery banner shown');
  }

  // -------------------------------------------------------------------
  // Group 5: setRecoveryBanner is exported on window, same convention as
  // window.kcConfirm/window.kcEscapeHtml -- ui_responsive_sweep.mjs's
  // 'recovery_shown' fixture depends on this.
  // -------------------------------------------------------------------
  {
    const { ctx } = makeContext();
    const bannerEl = vm.runInContext('buildRecoveryBanner()', ctx);
    vm.runInContext('recoveryBannerEl = bannerEl', Object.assign(ctx, { bannerEl }));
    assert(typeof ctx.window.setRecoveryBanner === 'function', 'window.setRecoveryBanner is exported');
    ctx.window.setRecoveryBanner(true);
    assert(!bannerEl.hasAttribute('hidden'), 'window.setRecoveryBanner(true) shows the banner');
  }

  console.log('');
  console.log(passed + ' passed, ' + failed + ' failed');
  if (failed > 0) {
    console.log('Failures: ' + failures.join(', '));
    process.exit(1);
  }
})();
