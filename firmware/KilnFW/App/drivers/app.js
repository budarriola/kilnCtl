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

  // ---- kcEscapeHtml ----------------------------------------------------
  //
  // Shared escape helper for every page that builds markup via innerHTML
  // string concatenation. Any operator- or server-supplied text (zone
  // names, profile names, Wi-Fi SSIDs, fault reason strings, ...) MUST be
  // run through this before being spliced into an HTML string -- a name
  // containing a double quote or "<"/">" can otherwise break out of an
  // attribute or inject an element/event handler that runs in the next
  // person who opens the page's browser. There is no auth on these APIs,
  // so anyone who can reach the board on the network can plant such a
  // payload via a plain POST.
  //
  // Prefer textContent / setAttribute / createElement at the call site
  // when practical -- they can't have this class of bug at all -- but for
  // the many existing places that build a whole innerHTML string in one
  // shot, wrap every dynamic value with this instead of duplicating an
  // escape function per page (this codebase has been bitten by exactly
  // that duplication before).
  window.kcEscapeHtml = function (s) {
    if (s === null || s === undefined) return '';
    return String(s)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  };

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
  // UI_PLAN.md item 7 ("Unit parity"): the web should honour the same
  // Fahrenheit/Celsius setting the LCD uses. This USED to be a client-side,
  // localStorage-only toggle with an explicit disclaimer that it could not
  // agree with the LCD, because nothing on the board stored a unit
  // preference at all. It now does (2026-08-21): unit_pref.c (App/drivers)
  // persists the choice to NVS, GET /api/status reports it as the additive
  // "temp_unit" field ("C" or "F"), and POST /api/unit_pref (form body
  // "unit=C"/"unit=F") is the write side -- the exact same board-is-the-
  // source-of-truth model the LCD's Configuration-hub toggle uses
  // (ui_page_config.c). This module replaces the old localStorage-only
  // mechanism entirely: the device is now the one place this preference
  // lives, and a page here reflects it rather than keeping its own
  // independent copy.
  //
  // Conversion is display-only by construction: kcUnit.fmt()/toDisplay() take
  // a Celsius number and hand back a STRING or NUMBER for reading, never
  // something written into a form field's `value` or a POST body. Every
  // input across every page (zones_page.html's maxtemp/mintemp, profiles
  // segment targets, autotune setpoints, ...) still holds and sends whatever
  // raw °C number it always did -- a setpoint posted in the wrong unit is a
  // real hazard on a kiln, so this toggle touches rendered text only.
  window.kcUnit = (function () {
    // In-memory cache of the device's last-known answer -- 'c' until the
    // first successful read, matching unit_pref.c's own shipped-default
    // fallback on a fresh board. updateFromStatus() below is how a page
    // that already polls GET /api/status for its own data (main_page.html's
    // poll()) keeps this in sync for free, with no second network round trip
    // spent just on this one field.
    var cached = 'c';

    function get() {
      return cached;
    }
    // Called with the *decoded* GET /api/status JSON body (not raw text) by
    // any page that already fetches it -- see main_page.html's poll(). A
    // response with no "temp_unit" key (a firmware predating this feature)
    // leaves `cached` exactly where it was: the additive-field contract this
    // field's own comment on the firmware side documents means an old/new
    // client/firmware pairing degrades to "assume Celsius", not a throw.
    function updateFromStatus(status) {
      if (!status || typeof status.temp_unit !== 'string') return;
      var next = status.temp_unit.toLowerCase() === 'f' ? 'f' : 'c';
      if (next !== cached) {
        cached = next;
        window.dispatchEvent(new Event('kcunitchange'));
      }
    }
    // Posts the new preference to the device. Updates the in-memory cache
    // (and fires kcunitchange) optimistically -- same "assume the write
    // succeeds" convention every other settings control on these pages
    // uses -- rather than waiting on the network round trip before the UI
    // reflects the tap. A failed POST is only logged; the next
    // GET /api/status poll (via updateFromStatus above) is what would
    // correct `cached` back if the device actually rejected or failed to
    // persist it.
    function set(u) {
      var next = u === 'f' ? 'f' : 'c';
      cached = next;
      window.dispatchEvent(new Event('kcunitchange'));
      fetch('/api/unit_pref', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'unit=' + (next === 'f' ? 'F' : 'C'),
      }).catch(function (err) {
        console.warn('unit preference POST failed (will retry on next status poll):', err);
      });
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
    return {
      get: get, set: set, toDisplay: toDisplay, label: label, fmt: fmt,
      updateFromStatus: updateFromStatus,
    };
  })();

  // The °C/°F toggle. Injected rather than written into twelve *_page.html
  // files -- same "app.js injects its own chrome" pattern used for the
  // banner and stop bar below.
  //
  // 2026-08-21, second pass (owner request "move the Fahrenheit/Celsius and
  // theme selection to the settings pages"): this button used to be built on
  // every page and inserted into the topbar next to #themeBtn. It is a
  // display preference, not per-page chrome, so it is now built ONLY on a
  // page that offers a #kcDisplayPrefs container -- settings_display_page.html
  // (moved there off settings_page.html 2026-08-22, see that page's header
  // comment: the two nav-menu entries "Display" and "Reset" used to be two
  // anchors on one settings page, which made them look like the same
  // destination). Returning null elsewhere is not a loss of function: kcUnit itself still
  // loads on every page and every page still RENDERS in the selected unit;
  // only the control that changes it has moved to one place.
  //
  // The unit's source of truth is the device (unit_pref.c/NVS), so unlike
  // the theme -- which is per-browser localStorage -- a change made here is
  // visible on the LCD and in every other browser too.
  function buildUnitBtn() {
    var host = document.getElementById('kcDisplayPrefs');
    if (!host) return null;
    var el = document.createElement('button');
    el.type = 'button';
    el.className = 'kc-unit-btn kc-pref-btn';
    el.title = 'Toggle Celsius/Fahrenheit display -- persisted on the device, same setting the LCD shows (see app.js)';
    // Spelled out, not the bare "°C"/"°F" this showed as a topbar button:
    // with room for words, the settings row says both what the setting IS
    // and what tapping does, matching the LCD hub cell's own wording
    // (ui_page_config.c's units_cell_set_label()).
    function refresh() {
      el.textContent = window.kcUnit.get() === 'f'
        ? 'Units: Fahrenheit (switch to °C)'
        : 'Units: Celsius (switch to °F)';
    }
    refresh();
    // Listens rather than refreshing only on click: updateFromStatus() (from
    // this page's own poll, or from another browser tab's POST landing on
    // the device) can change `cached` without this button being the cause,
    // and the button text must not go stale in that case.
    window.addEventListener('kcunitchange', refresh);
    el.addEventListener('click', function () {
      window.kcUnit.set(window.kcUnit.get() === 'f' ? 'c' : 'f');
    });
    // nav.js has already moved #themeBtn into this same container by the
    // time this runs (deferred scripts execute in document order and nav.js
    // is listed first on every page), so inserting before it puts Units
    // above Theme; if nav.js somehow did not run, appending still lands the
    // control inside the container.
    var themeBtn = document.getElementById('themeBtn');
    if (themeBtn && themeBtn.parentNode === host) {
      host.insertBefore(el, themeBtn);
    } else {
      host.appendChild(el);
    }
    return el;
  }

  // One-time initial read on every page load, independent of whether the
  // page also polls /api/status for its own data -- a page that does not
  // (most of them; only main_page.html's poll() calls updateFromStatus()
  // itself on its own cadence) would otherwise show Celsius indefinitely.
  // Best-effort: a failed fetch here just leaves `cached` at its 'c' default,
  // same as a board with no saved preference yet.
  fetch('/api/status')
    .then(function (r) { return r.json(); })
    .then(function (status) { window.kcUnit.updateFromStatus(status); })
    .catch(function () { /* leave cached at its default -- see comment above */ });

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
