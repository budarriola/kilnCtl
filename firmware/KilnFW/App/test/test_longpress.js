/* Node-only test harness for main_page.html's press-and-hold PID gesture
 * (attachLongPress(), ~line 609-643).
 *
 * Why this exists: an audit found this gesture (owner request 2026-09-01:
 * "hold a zone on the dashboard and create a popup that allows them to
 * adjust the pid peramiters while it is running") had zero test coverage.
 * The backend it drives (POST /api/zones/pid) is well tested; the client
 * gesture recognizer -- threshold timing, move-cancel, double-fire/leak
 * guards -- was not.
 *
 * Follows test_firing_chart.js's extraction pattern: pull the real source
 * out of main_page.html by exact-line markers and run it in a Node vm, so
 * this exercises the actual production function, not a reimplementation.
 * Uses a fake clock (queued setTimeout/clearTimeout) and a minimal
 * EventTarget-like element stub to drive synthetic Pointer Events -- no
 * real-time sleeping anywhere.
 *
 * Run: node firmware/KilnFW/App/test/test_longpress.js
 * Exit code 0 on all-pass, 1 otherwise.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const PAGE_PATH = path.join(__dirname, '..', 'drivers', 'main_page.html');
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

const LONGPRESS_SRC = extractRange(
  "var LONGPRESS_MS = 550;",
  '}'
);
assert(LONGPRESS_SRC.indexOf('function attachLongPress') !== -1,
  'sanity: extracted range is attachLongPress');
assert(LONGPRESS_SRC.indexOf('LONGPRESS_MOVE_THRESHOLD_PX') !== -1,
  'sanity: extracted range defines the move-cancel threshold');

// ---------------------------------------------------------------------------
// Fake clock: a tiny setTimeout/clearTimeout replacement so tests advance
// virtual time deterministically instead of sleeping in real time.
// ---------------------------------------------------------------------------
function makeFakeClock() {
  let now = 0;
  let nextId = 1;
  const pending = new Map(); // id -> { at, fn }
  function setTimeout_(fn, ms) {
    const id = nextId++;
    pending.set(id, { at: now + ms, fn: fn });
    return id;
  }
  function clearTimeout_(id) {
    pending.delete(id);
  }
  // Advance virtual time by ms, firing any timers whose deadline falls
  // at-or-before the new "now", in deadline order.
  function tick(ms) {
    now += ms;
    for (;;) {
      let dueId = null, due = null;
      for (const [id, t] of pending) {
        if (t.at <= now && (due === null || t.at < due.at)) { dueId = id; due = t; }
      }
      if (dueId === null) break;
      pending.delete(dueId);
      due.fn();
    }
  }
  return { setTimeout: setTimeout_, clearTimeout: clearTimeout_, tick, pendingCount: () => pending.size };
}

// ---------------------------------------------------------------------------
// Minimal EventTarget-like element stub -- just enough for
// pointerdown/pointermove/pointerup/pointercancel/pointerleave and the
// synthetic 'click' suppression attachLongPress installs on success.
// ---------------------------------------------------------------------------
function makeFakeElement() {
  const listeners = {}; // type -> [{fn, opts}]
  return {
    style: {},
    addEventListener(type, fn, opts) {
      (listeners[type] = listeners[type] || []).push({ fn, opts: opts || {} });
    },
    dispatchEvent(type, evt) {
      const handlers = (listeners[type] || []).slice();
      for (const h of handlers) {
        if (h.opts.once) {
          const idx = listeners[type].indexOf(h);
          if (idx !== -1) listeners[type].splice(idx, 1);
        }
        h.fn(evt);
      }
    },
    listenerCount(type) { return (listeners[type] || []).length; },
  };
}

function pointerEvent(x, y, button) {
  let defaultPrevented = false, propagationStopped = false;
  return {
    clientX: x, clientY: y, button: button === undefined ? 0 : button,
    preventDefault() { defaultPrevented = true; },
    stopPropagation() { propagationStopped = true; },
    get defaultPrevented() { return defaultPrevented; },
    get propagationStopped() { return propagationStopped; },
  };
}

// Load attachLongPress into a fresh vm context wired to a fake clock, and
// return { el, fire } helpers to drive it.
function loadLongPress() {
  const clock = makeFakeClock();
  const ctx = vm.createContext({
    console,
    setTimeout: clock.setTimeout,
    clearTimeout: clock.clearTimeout,
    Math,
  });
  new vm.Script(LONGPRESS_SRC, { filename: 'main_page.html (longpress slice)' }).runInContext(ctx);
  const el = makeFakeElement();
  let longPressCount = 0;
  const longPressCalls = [];
  ctx.attachLongPress(el, function (e) { longPressCount++; longPressCalls.push(e); });
  return {
    el, clock,
    down: (x, y, btn) => { const e = pointerEvent(x, y, btn); el.dispatchEvent('pointerdown', e); return e; },
    move: (x, y) => { const e = pointerEvent(x, y); el.dispatchEvent('pointermove', e); return e; },
    up: (x, y) => { const e = pointerEvent(x, y); el.dispatchEvent('pointerup', e); return e; },
    cancel: () => el.dispatchEvent('pointercancel', {}),
    leave: () => el.dispatchEvent('pointerleave', {}),
    click: () => { const e = pointerEvent(0, 0); el.dispatchEvent('click', e); return e; },
    longPressCount: () => longPressCount,
  };
}

// ---------------------------------------------------------------------------
// A hold past the threshold fires.
// ---------------------------------------------------------------------------
(function testHoldPastThresholdFires() {
  const h = loadLongPress();
  h.down(100, 100);
  assert(h.longPressCount() === 0, 'hold-past-threshold: nothing fires before the timer elapses');
  h.clock.tick(551);
  assert(h.longPressCount() === 1, 'hold-past-threshold: fires once the 550ms threshold is crossed');
})();

// ---------------------------------------------------------------------------
// A short tap (pointerup before the threshold) does not fire.
// ---------------------------------------------------------------------------
(function testShortTapDoesNotFire() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(100);
  h.up(100, 100);
  h.clock.tick(1000); // give the (cleared) timer every chance to fire anyway
  assert(h.longPressCount() === 0, 'short tap released well before threshold never fires');
})();

// ---------------------------------------------------------------------------
// Pointer-up before the threshold aborts even at the very last instant.
// ---------------------------------------------------------------------------
(function testPointerUpJustBeforeThresholdAborts() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(549);
  h.up(100, 100);
  h.clock.tick(10); // past where the timer would have fired had it survived
  assert(h.longPressCount() === 0, 'pointerup 1ms before threshold still aborts the gesture');
})();

// ---------------------------------------------------------------------------
// Movement beyond the cancel distance aborts, even if held long enough
// afterward for the (now-cleared) timer to have fired.
// ---------------------------------------------------------------------------
(function testMovementBeyondThresholdAborts() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(100);
  h.move(111, 100); // dx=11 > 10px threshold
  h.clock.tick(1000);
  assert(h.longPressCount() === 0, 'movement past the 10px cancel distance aborts the hold');
})();

// Small movement WITHIN the threshold must not cancel -- proves the cancel
// is distance-gated, not any-movement-gated.
(function testSmallMovementWithinThresholdStillFires() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(100);
  h.move(105, 100); // dx=5 < 10px threshold
  h.clock.tick(500);
  assert(h.longPressCount() === 1, 'movement within the 10px cancel distance does not abort the hold');
})();

// ---------------------------------------------------------------------------
// The click-suppression path: a successful long-press swallows the very
// next click on the element (so no stray click handler double-fires), but
// only that one -- a second click after that goes through untouched.
// ---------------------------------------------------------------------------
(function testSuccessfulHoldSuppressesNextClickOnlyOnce() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(551);
  assert(h.longPressCount() === 1, 'sanity: hold fired');
  h.up(100, 100); // real gesture: finger/mouse lifts after the hold fires, which is what installs the suppressor
  const c1 = h.click();
  assert(c1.defaultPrevented === true, 'the click immediately following a fired long-press is suppressed');
  const c2 = h.click();
  assert(c2.defaultPrevented === false, 'a second, later click is NOT suppressed (once:true, not sticky)');
})();

// A short tap's click must never be suppressed -- fired stays false.
(function testShortTapClickNotSuppressed() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(100);
  h.up(100, 100);
  const c = h.click();
  assert(c.defaultPrevented === false, 'an ordinary short tap does not suppress its own click');
})();

// ---------------------------------------------------------------------------
// Timer does not leak or double-fire if a second press starts before the
// first one resolves: pointerdown always clears any prior pending timer
// first, so only ONE longpress fires for the second, completed hold, and
// there is never more than one timer pending at once.
// ---------------------------------------------------------------------------
(function testSecondPressBeforeFirstResolvesDoesNotDoubleFire() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(200); // first hold in flight, not yet fired
  assert(h.clock.pendingCount() === 1, 'sanity: exactly one timer pending after first pointerdown');
  h.down(300, 300); // second press starts; must supersede, not stack, the first timer
  assert(h.clock.pendingCount() === 1, 'a second pointerdown before the first resolves does not leak a second timer');
  h.clock.tick(551);
  assert(h.longPressCount() === 1, 'only the second (superseding) hold fires -- no double-fire from the abandoned first timer');
  assert(h.clock.pendingCount() === 0, 'no timer left pending after the superseding hold fires');
})();

// Non-primary button (e.g. right-click, button !== 0) must never start the
// gesture at all.
(function testNonPrimaryButtonIgnored() {
  const h = loadLongPress();
  h.down(100, 100, 2); // secondary button
  h.clock.tick(1000);
  assert(h.longPressCount() === 0, 'a non-primary-button pointerdown never starts the hold timer');
  assert(h.clock.pendingCount() === 0, 'no timer is queued for a non-primary-button pointerdown');
})();

// pointercancel/pointerleave abort an in-flight hold just like an early
// pointerup does.
(function testPointerCancelAborts() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(100);
  h.cancel();
  h.clock.tick(1000);
  assert(h.longPressCount() === 0, 'pointercancel aborts an in-flight hold');
})();

(function testPointerLeaveAborts() {
  const h = loadLongPress();
  h.down(100, 100);
  h.clock.tick(100);
  h.leave();
  h.clock.tick(1000);
  assert(h.longPressCount() === 0, 'pointerleave aborts an in-flight hold');
})();

// ---------------------------------------------------------------------------
console.log('');
console.log(passed + ' passed, ' + failed + ' failed');
if (failed) {
  console.log('FAILURES:');
  failures.forEach((f) => console.log('  - ' + f));
  process.exitCode = 1;
}
