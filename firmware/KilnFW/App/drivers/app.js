// app.js -- shared cross-page behaviours (UI_PLAN.md, Web "page structure
// rework" section 4 items 2/4/5: sticky Stop, connection-lost banner,
// confirm-before-destructive-action). Loaded via `<script defer src="/app.js">`
// from every *_page.html, same ordering caveat as nav.js (see that file's
// header comment) -- only call window.kcConfirm from inside an event-handler
// callback, never from a page's inline <script> top level.
//
// Deliberately does NOT implement UI_PLAN.md section 4 item 3's "one shared
// poller" replacing every page's own setTimeout/fetch chain -- that is a
// materially larger refactor of each page's rendering code (main_page.html's
// poll()/pollExec(), ota_page.html's progress polling, etc.) and is left for
// that follow-on pass. What this file *does* poll (see pollHeartbeat below)
// is its own minimal loop against /api/profile_exec, used only to drive the
// banner and the sticky Stop bar -- an intentional, small duplication of
// whatever request main_page.html already makes to the same endpoint, traded
// for not touching five other pages' rendering logic in this pass. Noted so
// a future consolidation pass knows this is the redundant request to remove.
(function () {
  'use strict';

  // ---- kcConfirm -----------------------------------------------------
  //
  // A single named seam for "ask before doing something destructive"
  // (UI_PLAN.md item 5: delete a profile, force a relay on, forget a
  // network, factory-reset). Today this is exactly window.confirm() --
  // no custom modal, no new CSS, nothing to get wrong -- but every call
  // site in every page goes through this name instead of calling
  // confirm() directly, so a future replacement (a themed dialog that
  // matches dark mode, say) is a one-file change instead of a grep-and-
  // replace across seven pages.
  window.kcConfirm = function (message) {
    return window.confirm(message);
  };

  // ---- Unit preference (°C/°F) --------------------------------------
  //
  // UI_PLAN.md item 7 ("Unit parity", "4 item 7" in the old numbering): the
  // web should honour the same °F/°C setting the LCD uses. Checked before
  // writing anything here -- grepped every ui_page_*.c, board_temps.{c,h},
  // zones_http.c, and every NVS key string in firmware/KilnFW for unit/
  // fahrenheit/celsius (case-insensitive): there is no such setting anywhere.
  // No NVS namespace stores a unit preference, no ui_page_*.c reads or writes
  // one, and /api/status (and every other JSON endpoint) returns plain
  // Celsius floats with no accompanying unit field. So there is nothing on
  // the board side to "honour" yet -- the LCD itself has no °F mode today,
  // it always shows Celsius. Building a shared settings subsystem (NVS key +
  // API field + LCD toggle) to honour would be inventing scope no one asked
  // for yet, and touching ui_page_*.c is out of bounds for this pass anyway.
  // So this is deliberately just a client-side, localStorage-persisted
  // display toggle: it does NOT share state with the LCD, and switching it
  // here has zero effect on, and zero awareness of, what the panel shows.
  // Making the two surfaces actually agree would need the LCD side to grow
  // its own stored preference and an API field carrying it to the browser --
  // that is the follow-on work, not this pass.
  //
  // Conversion is display-only by construction: kcUnit.fmt()/toDisplay() take
  // a Celsius number and hand back a STRING or NUMBER for reading, never
  // something written into a form field's `value` or a POST body. Every
  // input across every page (zones_page.html's maxtemp/mintemp, profiles
  // segment targets, autotune setpoints, ...) still holds and sends whatever
  // raw °C number it always did -- a setpoint posted in the wrong unit is a
  // real hazard on a kiln, so this toggle touches rendered text only.
  var KC_UNIT_KEY = 'kilnctl-unit';
  window.kcUnit = (function () {
    function get() {
      return localStorage.getItem(KC_UNIT_KEY) === 'f' ? 'f' : 'c';
    }
    function set(u) {
      localStorage.setItem(KC_UNIT_KEY, u === 'f' ? 'f' : 'c');
      // Pages that cache their last-rendered API payload listen for this to
      // re-render in the new unit without waiting for the next poll tick.
      window.dispatchEvent(new Event('kcunitchange'));
    }
    // Celsius in, a plain number in the selected unit out. null/undefined/NaN
    // pass through as null so callers can keep their own "n/a" handling.
    function toDisplay(c) {
      if (c === null || c === undefined || isNaN(c)) return null;
      return get() === 'f' ? (c * 9 / 5 + 32) : c;
    }
    function label() { return get() === 'f' ? '°F' : '°C'; }
    // Format a Celsius value as a display string, e.g. "212.0 °F". Mirrors
    // the ".toFixed(1) + ' °C'" pattern every page used inline before this,
    // so call sites are a near-mechanical swap.
    function fmt(c, decimals) {
      if (c === null || c === undefined || isNaN(c)) return 'n/a';
      if (decimals === undefined) decimals = 1;
      return toDisplay(c).toFixed(decimals) + ' ' + label();
    }
    return { get: get, set: set, toDisplay: toDisplay, label: label, fmt: fmt };
  })();

  // Small toggle button, inserted next to each page's own #themeBtn (every
  // *_page.html has one, same fixed-position top-right convention) rather
  // than requiring a markup change in twelve files -- same "app.js injects
  // its own chrome" pattern already used for the banner and stop bar below.
  function buildUnitBtn() {
    var el = document.createElement('button');
    el.type = 'button';
    el.className = 'kc-unit-btn theme-btn';
    el.title = 'Toggle °C/°F display on this device only -- does not change what the LCD shows (see app.js)';
    function refresh() { el.textContent = window.kcUnit.label(); }
    refresh();
    el.addEventListener('click', function () {
      window.kcUnit.set(window.kcUnit.get() === 'f' ? 'c' : 'f');
      refresh();
    });
    var themeBtn = document.getElementById('themeBtn');
    if (themeBtn && themeBtn.parentNode) {
      themeBtn.parentNode.insertBefore(el, themeBtn);
    } else {
      document.body.insertBefore(el, document.body.firstChild);
    }
    return el;
  }

  // ---- Connection-lost banner -----------------------------------------
  //
  // UI_PLAN.md item 4: "a failed fetch() leaves the last good numbers on
  // screen indefinitely -- a stale temperature that looks live is a
  // safety-relevant lie, not a cosmetic bug." This banner is the fix:
  // independent of whatever a given page's own polling does, this file
  // runs its own tiny heartbeat against /api/profile_exec (chosen because
  // it is small, already served on every page's server, and doubles as
  // the sticky-Stop data source below) and puts up an explicit banner the
  // moment that heartbeat starts failing.
  var HEARTBEAT_MIN_MS = 3000;
  var HEARTBEAT_MAX_MS = 30000; // backoff ceiling
  var FAILURES_BEFORE_BANNER = 2; // one dropped packet on a flaky Wi-Fi
                                   // link shouldn't flash the banner --
                                   // two consecutive failures is a real
                                   // outage, not noise.

  var consecutiveFailures = 0;
  var lastGoodMs = null; // Date.now() of the last successful heartbeat
  var heartbeatTimer = null;
  var bannerTickTimer = null;
  var lastExecState = null; // 'idle' | 'running' | 'paused' | 'faulted' | 'done' | null (unknown)

  var bannerEl = null;
  function buildBanner() {
    var el = document.createElement('div');
    el.className = 'kc-conn-banner';
    el.setAttribute('hidden', '');
    el.textContent = 'Disconnected from kilnCtl';
    // Inserted right after the topbar (nav.js runs first -- both are
    // `defer` scripts and this file is listed after nav.js in every page's
    // <head>, so document order guarantees nav.js's init() has already run).
    // Falls back to prepending to <body> if nav.js somehow isn't present,
    // rather than throwing and killing the rest of app.js.
    var topbar = document.querySelector('.kc-topbar');
    if (topbar && topbar.parentNode) {
      topbar.parentNode.insertBefore(el, topbar.nextSibling);
    } else {
      document.body.insertBefore(el, document.body.firstChild);
    }
    return el;
  }

  function bannerText() {
    if (lastGoodMs === null) {
      return 'Disconnected from kilnCtl -- no successful update yet';
    }
    var ageS = Math.max(0, Math.round((Date.now() - lastGoodMs) / 1000));
    return 'Disconnected from kilnCtl -- last update ' + ageS + 's ago. ' +
           'Values on this page may be stale.';
  }

  function showBanner() {
    if (!bannerEl) return;
    bannerEl.textContent = bannerText();
    bannerEl.removeAttribute('hidden');
    document.body.classList.add('kc-stale');
    if (!bannerTickTimer) {
      // Ticks once a second purely to keep the "Ns ago" text live while
      // disconnected -- independent of the (much slower, backed-off)
      // heartbeat interval itself.
      bannerTickTimer = setInterval(function () {
        if (bannerEl && !bannerEl.hasAttribute('hidden')) {
          bannerEl.textContent = bannerText();
        }
      }, 1000);
    }
  }

  function hideBanner() {
    if (!bannerEl) return;
    bannerEl.setAttribute('hidden', '');
    document.body.classList.remove('kc-stale');
    if (bannerTickTimer) {
      clearInterval(bannerTickTimer);
      bannerTickTimer = null;
    }
  }

  // ---- Sticky Stop -----------------------------------------------------
  //
  // UI_PLAN.md item 2: a fixed Stop control on every page, not just the
  // dashboard, active only while a profile is running -- "a user deep in
  // the zones page today must navigate home first." /api/profile_exec's
  // `state` field is exactly main_page.html's own trigger for showing its
  // in-page Stop button, reused here unchanged.
  //
  // Confirm-on-Stop: per the owner's 2026-08-20 instruction, Start and
  // Stop both require confirmation today, on both surfaces -- the LCD
  // (see App/drivers/ui_confirm.c, which gained confirm dialogs for Start
  // and Stop the same day) and the web (main_page.html's stopBtn handler,
  // and this sticky bar, both confirm before stopping).
  var STOPPABLE_STATES = { running: true, paused: true, faulted: true, done: true };

  var stopBarEl = null;
  function buildStopBar() {
    var el = document.createElement('div');
    el.className = 'kc-stop-bar';
    el.setAttribute('hidden', '');

    var btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'kc-stop-btn';
    btn.textContent = 'STOP FIRING';
    btn.addEventListener('click', function () {
      if (!kcConfirm('Stop this firing now? This aborts the run in progress and cannot be resumed.')) {
        return;
      }
      btn.disabled = true;
      fetch('/api/profile_exec/stop', { method: 'POST' })
        .then(function () { btn.disabled = false; })
        .catch(function () { btn.disabled = false; });
    });

    el.appendChild(btn);
    document.body.appendChild(el);
    return el;
  }

  function setStopBarVisible(visible) {
    if (!stopBarEl) return;
    var wasHidden = stopBarEl.hasAttribute('hidden');
    if (visible) {
      stopBarEl.removeAttribute('hidden');
    } else {
      stopBarEl.setAttribute('hidden', '');
    }
    if (wasHidden !== !visible && window.kcNav) {
      window.kcNav.updateBodyPadding();
    }
  }

  // ---- Heartbeat / poll loop --------------------------------------------

  function scheduleNext(delayMs) {
    if (heartbeatTimer) clearTimeout(heartbeatTimer);
    heartbeatTimer = setTimeout(pollHeartbeat, delayMs);
  }

  function currentDelay() {
    // Linear backoff capped at HEARTBEAT_MAX_MS -- doubling would reach the
    // cap in 4 failures either way at these values, but linear is easier to
    // reason about for a human reading the failure count in the future, and
    // this isn't a hot enough path for the difference to matter.
    var delay = HEARTBEAT_MIN_MS + Math.max(0, consecutiveFailures - FAILURES_BEFORE_BANNER) * 3000;
    return Math.min(delay, HEARTBEAT_MAX_MS);
  }

  function pollHeartbeat() {
    // UI_PLAN.md item 3's "polling pauses on document.visibilityState ===
    // 'hidden'" applies here too, even though this isn't the consolidated
    // poller item 3 describes -- a phone left on a bench with the screen
    // off shouldn't keep this heartbeat running either. Re-checked on the
    // visibilitychange listener below so a backgrounded tab doesn't have to
    // wait out its current timer to stop.
    if (document.visibilityState === 'hidden') {
      scheduleNext(HEARTBEAT_MIN_MS);
      return;
    }
    fetch('/api/profile_exec')
      .then(function (r) {
        if (!r.ok) throw new Error('http ' + r.status);
        return r.json();
      })
      .then(function (st) {
        consecutiveFailures = 0;
        lastGoodMs = Date.now();
        hideBanner();
        lastExecState = st && st.state;
        setStopBarVisible(!!(lastExecState && STOPPABLE_STATES[lastExecState]));
        scheduleNext(HEARTBEAT_MIN_MS);
      })
      .catch(function () {
        consecutiveFailures++;
        if (consecutiveFailures >= FAILURES_BEFORE_BANNER) {
          showBanner();
        }
        scheduleNext(currentDelay());
      });
  }

  function init() {
    bannerEl = buildBanner();
    stopBarEl = buildStopBar();
    buildUnitBtn();
    pollHeartbeat();
    document.addEventListener('visibilitychange', function () {
      if (document.visibilityState === 'visible') {
        // Coming back into view: re-poll immediately instead of waiting out
        // whatever backoff delay was in flight when it was hidden, so the
        // banner/stop-bar state can't lag a real reconnect by up to 30s.
        scheduleNext(0);
      }
    });
  }

  if (document.body) {
    init();
  } else {
    document.addEventListener('DOMContentLoaded', init);
  }
})();
