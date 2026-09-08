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

  // ---- kcSafetyTcIsSeparate ----------------------------------------------
  //
  // ROADMAP.md "Safety TC display audit, 2026-09-05": the single shared
  // predicate every page must call before showing the safety processor's
  // own thermocouple reading/fault as if it were an independent physical
  // sensor. Mirrors safety_tc_is_separate_physical_sensor() (safety_link.h)
  // exactly -- fail-to-shown: hides the reading only when it has been
  // CONFIRMED borrowed (borrowed_known true AND borrowed true). Link down
  // suppresses the display. An older Pico (borrowed_known false) is
  // UNKNOWN, not confirmed borrowed, so it renders as shown, same as a
  // missing tc_is_separate_sensor field on the payload -- fail-to-shown,
  // not fail-to-hidden.
  //
  // Accepts either shape callers already have on hand: zones_http_get.c's
  // safety_wiring object ({link_up, tc_is_separate_sensor}) or a raw
  // safety-link-shaped object ({link_up, borrowed_known, borrowed}) such as
  // dashboard_http.c's /api/status fields once surfaced there.
  window.kcSafetyTcIsSeparate = function (st) {
    if (!st || !st.link_up) return false;
    if (typeof st.tc_is_separate_sensor === 'boolean') return st.tc_is_separate_sensor;
    return !(st.borrowed_known && st.borrowed);
  };

  // ---- thermalGuardWords -----------------------------------------------
  //
  // Single shared decode table for thermal_guard_trip_t (thermal_guard.h),
  // the fault_guard byte that profile_exec_status_t / run_state carry
  // alongside fault_reason. main_page.html and diagnostics_page.html used
  // to each keep their own copy of this table -- exactly the LCD-vs-web
  // drift that safety_trip_words.h was written to stop on the safety side.
  // Keep this table's wording in sync with thermal_guard.h's
  // THERMAL_GUARD_TRIP_* enum comments; add new entries here, not in a page.
  var THERMAL_GUARD_WORDS = {
    0: 'none', 1: 'heating commanded but temperature not rising',
    2: 'temperature moving the wrong direction', 3: 'runaway heating (welded contact / shorted SSR?)',
    4: 'drifted from setpoint after settling', 5: 'over absolute max temperature',
    6: 'under absolute min temperature', 7: 'sensor reading invalid',
    8: 'reading frozen while duty > 0', 9: 'cross-zone plausibility check failed',
    10: 'relay autotune: no oscillation seen (element may be dead)'
  };
  window.thermalGuardWords = function (guard) { return THERMAL_GUARD_WORDS[guard] || ('guard ' + guard); };

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

  // ---- kcFetchWithSafetyAck ------------------------------------------
  //
  // One wrapper for every action the board will perform without a working
  // safety processor, but only if the operator says so: OTA updates and
  // rollbacks, backup restore, and applying a saved kiln config.
  //
  // The board answers such a request with 428 Precondition Required (see
  // ota_interlock.h's OTA_INTERLOCK_REFUSED_NEEDS_ACK) instead of the plain
  // 409 it uses for refusals that cannot be argued with -- a running firing,
  // a hot zone, another update already going. So the rule here is narrow and
  // mechanical: 428 and only 428 gets a warning and a retry. A 409 is passed
  // straight back to the caller to report, exactly as before.
  //
  // The warning text below is duplicated from OTA_INTERLOCK_NO_SAFETY_WARNING
  // in App/drivers/net/ota_interlock.h, which is the LCD's copy of the same
  // sentence. App/test/lint_pages.js compares the two on every run, so an
  // edit to one without the other fails the check rather than shipping two
  // different descriptions of the same risk.
  var NO_SAFETY_WARNING =
    'The safety processor is not answering. Nothing independent is watching this kiln: the ' +
    'heaters may come on, or stay stuck on, with no second processor able to cut them. ' +
    'Continue anyway?';

  window.kcFetchWithSafetyAck = function (url, init) {
    init = init || {};
    return fetch(url, init).then(function (r) {
      if (r.status !== 428) return r;
      // Re-read the board's own reason and show it above the warning: the
      // 428 is always the safety link today, but quoting what the firmware
      // actually said keeps this from going stale if that ever widens.
      return r.text().then(function (reason) {
        var msg = (reason ? reason.trim() + '\n\n' : '') + NO_SAFETY_WARNING;
        if (!window.kcConfirm(msg)) {
          // Hand back the original refusal so the caller's error path runs
          // unchanged -- declining is not a new kind of failure.
          return r;
        }
        var retry = {};
        for (var k in init) { if (Object.prototype.hasOwnProperty.call(init, k)) retry[k] = init[k]; }
        retry.headers = {};
        var src = init.headers || {};
        // init.headers may be a plain object or a Headers instance; normalise
        // to a plain object so adding one key cannot drop the others.
        if (typeof src.forEach === 'function' && !(src instanceof Array)) {
          src.forEach(function (v, k2) { retry.headers[k2] = v; });
        } else {
          for (var k3 in src) {
            if (Object.prototype.hasOwnProperty.call(src, k3)) retry.headers[k3] = src[k3];
          }
        }
        retry.headers['X-Ota-Ack-No-Safety'] = '1';
        return fetch(url, retry);
      });
    });
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

  // ---- Recovery-mode banner ----------------------------------------------
  //
  // Owner report, 2026-09-08 recovery-loop incident: recovery mode was
  // effectively invisible -- the LCD showed a frozen boot banner (the LVGL
  // UI task is one of the things recovery mode skips, see
  // main_bridges_bringup.c), the CONTROL UART was dead, and the config
  // filesystem silently never mounted, with nothing on the dashboard saying
  // so. Hours went into diagnosing symptoms of a state the board already
  // knew it was in. This banner sources `recovery_mode` from the ALREADY
  // EXISTING GET /api/ota/esp/status route (ota_http_esp.c's
  // ota_esp_status_get_handler(), boot_guard_is_recovery_mode()) -- no new
  // endpoint, no new JSON field, so no json_cap headroom is spent.
  //
  // recovery_mode is decided once at boot and never changes within a boot
  // (see that handler's own doc comment), so this does not need the
  // heartbeat's 3s cadence -- polled independently, much slower, on its own
  // timer, and re-checked on visibilitychange like the heartbeat is.
  var RECOVERY_POLL_MS = 20000;
  var recoveryPollTimer = null;
  var recoveryBannerEl = null;

  // Named after what boot_guard.h / main_boot_early.c / main_bridges_bringup.c /
  // main_control_bringup.c actually do in recovery mode, read directly from
  // those files rather than assumed:
  //   - main_boot_early.c: the config filesystem (pref_cfg_fs.c bridge) is
  //     not mounted; settings come from firmware-default fallback storage.
  //   - main_bridges_bringup.c (~line 102): the LVGL/LCD UI task is not
  //     started at all -- this is also why an LCD-side version of this
  //     banner would be unreachable; see this file's own header note.
  //   - main_control_bringup.c (~line 152/160/187): profile_executor and
  //     autotune_engine are not started -- firing and autotune are both
  //     unavailable.
  var RECOVERY_BANNER_TEXT =
    'RECOVERY MODE — this board booted degraded after repeated unhealthy boots. ' +
    'The LCD display, profile executor, and autotune engine are OFF, and the config ' +
    'filesystem did not mount (settings may be running from fallback defaults). ' +
    'Firing is NOT available. A healthy boot clears this automatically on its own, ' +
    'or use "Exit recovery mode & reboot now" on the Firmware update page.';

  function buildRecoveryBanner() {
    var el = document.createElement('div');
    el.className = 'kc-recovery-banner';
    el.setAttribute('hidden', '');
    el.setAttribute('role', 'alert');
    el.textContent = RECOVERY_BANNER_TEXT;
    // No dismiss control anywhere on this element, ever -- owner constraint
    // (item 4): "must not be dismissible in a way that hides a still-active
    // recovery state on reload." A localStorage-backed "dismissed" flag
    // would do exactly that (persist across the very reload that should
    // re-show it), so this banner has no close affordance at all; it only
    // ever tracks live `recovery_mode` state from the board.
    //
    // Inserted directly ABOVE the connection-lost banner (bannerEl, built
    // just above this function) rather than independently off the topbar --
    // a board that is BOTH in recovery mode and currently unreachable
    // should show the more consequential, explanatory state first: recovery
    // mode says WHY firing/settings are degraded, the connection banner
    // only says a poll briefly failed.
    if (bannerEl && bannerEl.parentNode) {
      bannerEl.parentNode.insertBefore(el, bannerEl);
    } else {
      var topbar = document.querySelector('.kc-topbar');
      if (topbar && topbar.parentNode) {
        topbar.parentNode.insertBefore(el, topbar.nextSibling);
      } else {
        document.body.insertBefore(el, document.body.firstChild);
      }
    }
    return el;
  }

  function setRecoveryBanner(active) {
    if (!recoveryBannerEl) return;
    if (active) {
      recoveryBannerEl.removeAttribute('hidden');
    } else {
      recoveryBannerEl.setAttribute('hidden', '');
    }
    // 2026-09-08 UI aggregate review (d89256fe): the banner said "Firing is
    // NOT available" while main_page.html's #runBtn (the Start control)
    // stayed clickable -- a banner asserting a property the page does not
    // enforce is worse than no banner. Disable the control here, in
    // lockstep with the banner, rather than only warning about it; the
    // server-side enforcement is recovery_start_refusal.h. Tracked via a
    // dataset flag rather than driving runBtn.disabled directly so this
    // never fights with loadProfileList()'s own "no saved profiles" disable
    // reason (main_page.html) -- either reason alone must keep it disabled,
    // and #runBtn does not exist on every *_page.html app.js loads on, so
    // this is a no-op there.
    var runBtn = document.getElementById('runBtn');
    if (runBtn) {
      runBtn.dataset.recoveryDisabled = active ? 'true' : 'false';
      runBtn.disabled = active || runBtn.dataset.noProfiles === 'true';
    }
  }

  function pollRecoveryMode() {
    if (document.visibilityState === 'hidden') return;
    fetch('/api/ota/esp/status')
      .then(function (r) {
        if (!r.ok) throw new Error('http ' + r.status);
        return r.json();
      })
      .then(function (st) {
        setRecoveryBanner(!!(st && st.recovery_mode));
      })
      .catch(function () {
        // Best-effort only: a transient fetch failure here is not evidence
        // the board left recovery mode, so this deliberately leaves the
        // banner in whatever state it last confirmed rather than hiding it
        // -- the one banner on this page that must never silently drop
        // because of an unrelated network blip.
      });
  }
  // Exported (same convention as window.kcConfirm/window.kcEscapeHtml above)
  // so ui_responsive_sweep.mjs's 'recovery_shown' fixture and
  // test_recovery_banner.js can drive the SAME function the poll success
  // path calls, rather than a hand-built substitute that could drift from
  // the real show/hide logic.
  window.setRecoveryBanner = setRecoveryBanner;

  // ---- Setup-wizard OFFER banner -----------------------------------------
  //
  // SETUP_WIZARD_PLAN.md: "OFFER the wizard rather than forcing a redirect"
  // -- an owner decision in force for the whole plan. This polls the same
  // GET /api/readiness the /readiness and /setup pages already poll (no new
  // endpoint, no new JSON field -- same "reuse, spend no json_cap headroom"
  // shape as pollRecoveryMode() above) and shows a low-key banner linking to
  // /setup whenever any item reads not_done. cannot_yet is deliberately NOT
  // included here: several items are cannot_yet purely because an earlier
  // item hasn't been answered yet (readiness_http.c's own gating), so
  // counting those too would just restate the same not_done item twice.
  // deliberately_off is excluded on purpose too -- that is a legitimate,
  // already-made choice (e.g. ct_installed = no), not something to nag about.
  //
  // Never on /setup itself -- an operator already on the wizard does not
  // need the wizard advertised to them.
  var SETUP_OFFER_POLL_MS = 20000;
  var setupOfferPollTimer = null;
  var setupBannerEl = null;
  var setupBannerDismissed = false; // session-local only; see theme.css's comment on why

  function buildSetupBanner() {
    var el = document.createElement('div');
    el.className = 'kc-setup-banner';
    el.setAttribute('hidden', '');
    el.setAttribute('role', 'status');
    var text = document.createElement('span');
    text.textContent = 'Setup is not finished yet. ';
    var link = document.createElement('a');
    link.href = '/setup';
    link.textContent = 'Open the setup wizard';
    var dismiss = document.createElement('button');
    dismiss.type = 'button';
    dismiss.textContent = 'Dismiss';
    dismiss.setAttribute('aria-label', 'Dismiss setup reminder for this page load');
    dismiss.addEventListener('click', function () {
      setupBannerDismissed = true;
      setSetupBanner(false);
    });
    el.appendChild(text);
    el.appendChild(link);
    el.appendChild(dismiss);
    // Same insertion precedence rule as buildRecoveryBanner(): more
    // consequential state (recovery mode, then connection-lost) renders
    // above this -- an incomplete setup is real but never urgent enough to
    // outrank either of those, so this always inserts BELOW them (directly
    // above the rest of the page's content) rather than at the very top.
    if (recoveryBannerEl && recoveryBannerEl.parentNode) {
      recoveryBannerEl.parentNode.insertBefore(el, recoveryBannerEl.nextSibling);
    } else if (bannerEl && bannerEl.parentNode) {
      bannerEl.parentNode.insertBefore(el, bannerEl.nextSibling);
    } else {
      var topbar = document.querySelector('.kc-topbar');
      if (topbar && topbar.parentNode) {
        topbar.parentNode.insertBefore(el, topbar.nextSibling);
      } else {
        document.body.insertBefore(el, document.body.firstChild);
      }
    }
    return el;
  }

  function setSetupBanner(active) {
    if (!setupBannerEl) return;
    if (active && !setupBannerDismissed) {
      setupBannerEl.removeAttribute('hidden');
    } else {
      setupBannerEl.setAttribute('hidden', '');
    }
  }

  function pollSetupOffer() {
    if (document.visibilityState === 'hidden') return;
    if (window.location.pathname === '/setup') return; // never advertise the wizard to itself
    fetch('/api/readiness')
      .then(function (r) {
        if (!r.ok) throw new Error('http ' + r.status);
        return r.json();
      })
      .then(function (body) {
        var items = (body && body.items) || [];
        var incomplete = items.some(function (it) { return it && it.status === 'not_done'; });
        setSetupBanner(incomplete);
      })
      .catch(function () {
        // Best-effort, same convention as pollRecoveryMode(): a transient
        // fetch failure never hides a banner that was already correctly
        // shown, and never fabricates one either -- it just leaves the last
        // known state alone.
      });
  }
  // Exported for the same reason window.setRecoveryBanner is: a test can
  // drive the actual show/hide function rather than a hand-built stand-in.
  window.setSetupBanner = setSetupBanner;

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
  // (see App/drivers/ui/ui_confirm.c, which gained confirm dialogs for Start
  // and Stop the same day) and the web (main_page.html's stopBtn handler,
  // and this sticky bar, both confirm before stopping).
  // 2026-09-01 follow-up (owner, confirmed live on the board: "the stop
  // firing button may have stayed red even after the firing was done" --
  // state=3/DONE, all zones duty 0.00/relay off, yet the sticky bar still
  // showed the red STOP FIRING control). Root cause: STOPPABLE_STATES used
  // to include 'faulted'/'done', so this bar rendered the SAME red "STOP
  // FIRING" button (with the SAME "this aborts the run in progress" confirm
  // text) for a run that had already ended with every relay off as for one
  // still actively heating -- an operator glancing at the screen cannot
  // tell a completed run from a live one, and the safe misreading of that
  // screen is "something is still heating," which is wrong.
  //
  // The real executor states (profile_executor.h's profile_exec_state_t,
  // READ-ONLY -- another agent owns that file): IDLE, RUNNING, PAUSED,
  // DONE ("ran to completion; relays off"), FAULTED ("a GLOBAL thermal
  // guard tripped ... stays here until profile_executor_halt() explicitly
  // acknowledges it"). Mapped to exactly two DIFFERENT affordances, never
  // the same button:
  //   RUNNING/PAUSED -- STOPPABLE: heat may genuinely be on right now,
  //     Stop is a real abort-in-progress action, stays red.
  //   DONE/FAULTED   -- ACKNOWLEDGEABLE: relays are already off (DONE's own
  //     doc comment says so outright; FAULTED's guard trip already forced
  //     them off too) -- there is nothing left TO stop. The action is
  //     clearing the finished/faulted run so the picker reappears, worded
  //     and coloured accordingly (never red+"STOP FIRING").
  //   IDLE -- neither; the whole bar hides, as it always did.
  //
  // Both buttons end up calling the SAME /api/profile_exec/stop endpoint
  // (profile_executor_halt() in profile_executor.c, READ-ONLY, confirmed by
  // reading its actual implementation -- not just its header comment, which
  // only names "running/paused/faulted" and doesn't say what it does from
  // DONE): halt() treats every non-IDLE state uniformly (force relays off --
  // a genuine no-op on a DONE run where they're already off -- then
  // s_exec.state = PROFILE_EXEC_IDLE). A separate-looking endpoint,
  // /api/profile_exec/ack_last_run, exists too, but its own handler doc
  // comment says plainly it "does not touch the executor, the relays, or
  // any live firing" -- it dismisses the BOOT-CROSSING previous-run
  // breadcrumb (run_state.h, TODO.md 6A.3), a different concept from this
  // bar's live current-session state. It is NOT a substitute for halt()
  // here; the ack button below still posts to /api/profile_exec/stop, just
  // dressed as what it actually is for DONE/FAULTED: an acknowledgement,
  // not an abort.
  var STOPPABLE_STATES = { running: true, paused: true };
  var ACK_STATES = { faulted: true, done: true };

  var stopBarEl = null;
  var pauseResumeBtnEl = null;
  var ackBtnEl = null;
  function buildStopBar() {
    var el = document.createElement('div');
    el.className = 'kc-stop-bar';
    el.setAttribute('hidden', '');

    // Owner request: Pause/Resume is one toggling button (not two), living
    // on the same sticky bottom bar as Stop rather than main_page.html's own
    // exec-card -- same reasoning as the Stop button's own move to this bar:
    // it must be reachable from every page, not just the dashboard.
    var pauseBtn = document.createElement('button');
    pauseBtn.type = 'button';
    pauseBtn.className = 'kc-pause-btn';
    pauseBtn.setAttribute('hidden', '');
    pauseBtn.addEventListener('click', function () {
      var action = pauseBtn.textContent === 'Resume' ? 'resume' : 'pause';
      pauseBtn.disabled = true;
      fetch('/api/profile_exec/' + action, { method: 'POST' })
        .then(function () { pauseBtn.disabled = false; })
        .catch(function () { pauseBtn.disabled = false; });
    });
    pauseResumeBtnEl = pauseBtn;

    var btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'kc-stop-btn';
    btn.textContent = 'STOP FIRING';
    btn.setAttribute('hidden', '');
    btn.addEventListener('click', function () {
      if (!kcConfirm('Stop this firing now? This aborts the run in progress and cannot be resumed.')) {
        return;
      }
      btn.disabled = true;
      fetch('/api/profile_exec/stop', { method: 'POST' })
        .then(function () { btn.disabled = false; })
        .catch(function () { btn.disabled = false; });
    });

    // The DONE/FAULTED affordance -- reuses .kc-pause-btn's box/touch-target
    // CSS (theme.css) as its base so it doesn't need a new stylesheet rule,
    // but is NEVER red: green (--ui-accent-4, "all clear") for a run that
    // reached its own planned end, amber (--ui-accent-1, "needs a look, but
    // nothing is actively happening") for one a guard aborted -- set per-
    // state in setStopOrAckState() below. Confirm text is state-specific
    // too and never mentions "aborts the run in progress", because by the
    // time this button is visible there is no run in progress left to
    // abort.
    var ackBtn = document.createElement('button');
    ackBtn.type = 'button';
    ackBtn.className = 'kc-pause-btn';
    ackBtn.setAttribute('hidden', '');
    ackBtn.addEventListener('click', function () {
      var faulted = ackBtn.dataset.acking === 'faulted';
      var msg = faulted
        ? 'Clear this fault? Heat is already off; this just returns the board to idle so a new firing can start.'
        : 'Clear this finished firing? It already completed with heat off; this just returns the board to idle so a new firing can start.';
      if (!kcConfirm(msg)) {
        return;
      }
      ackBtn.disabled = true;
      fetch('/api/profile_exec/stop', { method: 'POST' })
        .then(function () { ackBtn.disabled = false; })
        .catch(function () { ackBtn.disabled = false; });
    });
    ackBtnEl = ackBtn;

    el.appendChild(pauseBtn);
    el.appendChild(btn);
    el.appendChild(ackBtn);
    document.body.appendChild(el);
    return el;
  }

  // Shown only for running/paused -- DONE/FAULTED get the ack button
  // instead (setStopOrAckState() below), never this one.
  function setPauseResumeState(state) {
    if (!pauseResumeBtnEl) return;
    if (state === 'running') {
      pauseResumeBtnEl.textContent = 'Pause';
      pauseResumeBtnEl.removeAttribute('hidden');
    } else if (state === 'paused') {
      pauseResumeBtnEl.textContent = 'Resume';
      pauseResumeBtnEl.removeAttribute('hidden');
    } else {
      pauseResumeBtnEl.setAttribute('hidden', '');
    }
  }

  // Toggles the red Stop button vs. the green/amber Acknowledge button --
  // the two affordances this bar now offers are mutually exclusive by
  // construction (STOPPABLE_STATES and ACK_STATES don't overlap), so at
  // most one of stopBtn/ackBtn is ever visible at once, and a completed or
  // faulted run can never be mistaken for a live one at a glance.
  function setStopOrAckState(state) {
    var stopBtn = stopBarEl ? stopBarEl.querySelector('.kc-stop-btn') : null;
    if (stopBtn) {
      if (STOPPABLE_STATES[state]) stopBtn.removeAttribute('hidden');
      else stopBtn.setAttribute('hidden', '');
    }
    if (!ackBtnEl) return;
    if (state === 'done') {
      ackBtnEl.textContent = 'ACKNOWLEDGE (firing complete)';
      ackBtnEl.style.background = 'var(--ui-accent-4, #5cc06e)';
      ackBtnEl.dataset.acking = 'done';
      ackBtnEl.removeAttribute('hidden');
    } else if (state === 'faulted') {
      ackBtnEl.textContent = 'ACKNOWLEDGE (firing FAULTED)';
      ackBtnEl.style.background = 'var(--ui-accent-1, #e8974e)';
      ackBtnEl.dataset.acking = 'faulted';
      ackBtnEl.removeAttribute('hidden');
    } else {
      ackBtnEl.setAttribute('hidden', '');
    }
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

  // The Stop bar is `position: fixed; bottom: 0`, so it covers whatever
  // happens to be at the bottom of the viewport. nav.js's updateBodyPadding()
  // stops it from hiding the END of the document, but it cannot help a
  // control sitting mid-page: at tablet width during a firing, the bar
  // landed squarely on the profile editor's name field, so an operator could
  // be typing into an input they could not see.
  //
  // Rather than reserve space unconditionally (which would leave a dead
  // stripe on every page for the >99% of the time no firing is stoppable),
  // move the one element that matters -- the one the operator just focused
  // -- out from under the bar. Only ever scrolls DOWN-to-UP by the overlap
  // amount, so it cannot fight a user who has deliberately scrolled
  // somewhere, and it does nothing at all when the bar is hidden.
  function keepFocusClearOfStopBar(ev) {
    if (!stopBarEl || stopBarEl.hasAttribute('hidden')) return;
    var el = ev.target;
    if (!el || typeof el.getBoundingClientRect !== 'function') return;
    var barTop = stopBarEl.getBoundingClientRect().top;
    var rect = el.getBoundingClientRect();
    // 8px of breathing room so the control does not end up flush against
    // the bar's border.
    var overlap = rect.bottom + 8 - barTop;
    if (overlap > 0) {
      window.scrollBy({ top: overlap, behavior: 'smooth' });
    }
  }
  document.addEventListener('focusin', keepFocusClearOfStopBar);

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
        setStopBarVisible(!!(lastExecState && (STOPPABLE_STATES[lastExecState] || ACK_STATES[lastExecState])));
        setPauseResumeState(lastExecState);
        setStopOrAckState(lastExecState);
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

  // 2026-09-01, owner report ("pause/firing buttons cover the last page
  // button" on the reset page, phone): setStopBarVisible() above only
  // re-measures the bar when it transitions hidden<->shown, on the
  // assumption that the bar's HEIGHT is otherwise constant. It isn't --
  // setStopOrAckState()/setPauseResumeState() swap which of the three
  // buttons are visible (pause+stop while running, just the ack button
  // once done/faulted) without ever hiding the bar itself, and at a narrow
  // phone width the different button combinations can wrap onto a second
  // line, changing the bar's real offsetHeight out from under a padding
  // value that was computed for the first-line height. A ResizeObserver on
  // the bar itself is the one mechanism that is right by construction for
  // every cause of a height change -- visibility toggling, button swaps,
  // text reflow from a viewport resize, even a future page-local CSS
  // override of --ui-touch -- rather than this file having to remember to
  // call updateBodyPadding() from every place that might change the bar's
  // rendered size. Falls back to doing nothing extra on a browser without
  // ResizeObserver; the explicit calls in setStopBarVisible() still cover
  // the show/hide transition there, just not a same-visibility content
  // change.
  function observeStopBarHeight(bar) {
    if (!window.ResizeObserver || !window.kcNav) return;
    var ro = new ResizeObserver(function () {
      window.kcNav.updateBodyPadding();
    });
    ro.observe(bar);
  }

  function init() {
    bannerEl = buildBanner();
    recoveryBannerEl = buildRecoveryBanner();
    setupBannerEl = buildSetupBanner();
    stopBarEl = buildStopBar();
    observeStopBarHeight(stopBarEl);
    buildUnitBtn();
    pollHeartbeat();
    pollRecoveryMode();
    pollSetupOffer();
    recoveryPollTimer = setInterval(pollRecoveryMode, RECOVERY_POLL_MS);
    setupOfferPollTimer = setInterval(pollSetupOffer, SETUP_OFFER_POLL_MS);
    document.addEventListener('visibilitychange', function () {
      if (document.visibilityState === 'visible') {
        // Coming back into view: re-poll immediately instead of waiting out
        // whatever backoff delay was in flight when it was hidden, so the
        // banner/stop-bar state can't lag a real reconnect by up to 30s.
        scheduleNext(0);
        pollRecoveryMode();
        pollSetupOffer();
      }
    });
  }

  if (document.body) {
    init();
  } else {
    document.addEventListener('DOMContentLoaded', init);
  }
})();
