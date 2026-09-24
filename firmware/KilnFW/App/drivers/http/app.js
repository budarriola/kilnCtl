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

  // Owner report, 2026-09-24: Cancel on the sign-in modal must reject the
  // pending action cleanly with "no error toast, or a quiet 'cancelled'
  // note" -- never the same "could not reach the board" wording a real
  // network/server failure gets, since the board was reached fine and
  // simply refused without a session. window.fetch's own 401/403 handling
  // below throws an Error named 'AuthCancelled' for exactly this case;
  // every page-local .catch() that renders visible error text for a failed
  // /api/ fetch should check this first and skip (or show a quiet note)
  // instead of the "could not reach"/"failed" wording meant for a genuine
  // failure. Exported the same way kcConfirm/kcEscapeHtml are so every page
  // checks the SAME name rather than each re-deriving `err.name ===
  // 'AuthCancelled'` by hand.
  window.kcIsAuthCancelled = function (err) {
    return !!(err && err.name === 'AuthCancelled');
  };

  // ---- Login-escalation modal + global 401/403 handling -----------------
  //
  // Owner report, 2026-09-21: "When I click something on the site that
  // requires a password it should ask me for one, not just show an
  // authentication required page" -- refined 2026-09-24 to: no page ever
  // shows a login form or redirect on load, ever, including a deep link to
  // an admin-tier page (http_auth_http.c's is_page_shell_get() now serves
  // every page shell directly regardless of session/role); the FIRST time
  // an operator is asked to sign in is when an actual fetch needs it,
  // via ONE shared, cancelable, theme-styled modal covering BOTH refusal
  // shapes -- no session at all (401) and a real session with the wrong
  // role (403 + X-Kiln-Auth-Reason: insufficient_role) -- with the same
  // contract either way: success retries the one action once; Cancel (or
  // Escape, or a backdrop click) rejects it with a distinguishable
  // `AuthCancelled` error (see window.kcIsAuthCancelled below) rather than
  // surfacing a raw 401/403 or navigating the tab away.
  //
  // The 401-vs-403-insufficient-role distinction is exactly what the
  // firmware's two decisions already send: HTTP_AUTH_DECISION_DENY_NO_SESSION
  // is always 401, and HTTP_AUTH_DECISION_DENY_INSUFFICIENT is always 403
  // WITH an X-Kiln-Auth-Reason: insufficient_role header (http_auth_http.c)
  // -- that header exists because a bare 403 is NOT unique to this gate
  // (ota_http.c's verify failure, ota_http_recovery.c's "not in recovery
  // mode", diagnostics_http.c's kiln_auth-namespace refusal,
  // profiles_live_http.c's builtin-origin refusal all also answer 403), so
  // this file checks the header, never the status code alone, before
  // treating a 403 as "wrong role -- offer to elevate".
  var nativeFetch = window.fetch.bind(window);

  // Routes this file must never intercept: the login/bootstrap POSTs
  // themselves (so a wrong password typed INTO the modal, or into
  // login_page.html's own form, reports back to that same form instead of
  // opening a second modal on top of itself) and the passive session
  // routes (ROUTE_TIER_OPEN, never 401/403 -- listed defensively only).
  // /api/auth/logout too (2026-09-24 review): it is USER tier, so logging
  // out on an already-expired session answers 401 -- asking the operator to
  // sign in so they can sign out is exactly backwards.
  function isAuthExemptUrl(url) {
    var path = String(url).replace(/^[a-zA-Z][\w+.-]*:\/\/[^/]+/, '').split('?')[0];
    return path === '/api/auth/login' || path === '/api/auth/bootstrap_password' ||
           path === '/api/auth/session' || path === '/api/auth/session/extend' ||
           path === '/api/auth/logout' ||
           // TOTP password-reset routes (docs/TOTP_PASSWORD_RESET_PLAN.md
           // section 6a): both are ROUTE_TIER_OPEN by design -- a locked-out
           // operator with no session must be able to call them -- so, same
           // reasoning as /api/auth/login above, they must never trigger
           // this wrapper's own login-escalation modal on a 4xx.
           path === '/api/auth/forgot' || path === '/api/auth/reset';
  }

  var loginModalEl = null, loginTitleEl = null, loginUserEl = null, loginPassEl = null,
      loginErrorEl = null, loginFormEl = null, loginCancelEl = null, loginSubmitEl = null,
      loginForgotEl = null;
  // Control handle for the login promise currently on screen (set inside
  // openLoginModal(), cleared by its finish()); null when no login modal is
  // pending. The forgot-password flow uses it to hide/restore that modal
  // without settling or duplicating its promise.
  var loginActiveCtl = null;

  function buildLoginModal() {
    var overlay = document.createElement('div');
    overlay.className = 'kc-login-overlay';
    overlay.setAttribute('hidden', '');
    var panel = document.createElement('form');
    panel.className = 'kc-login-panel';
    // Accessibility (opus review, 2026-09-21): role="dialog" + aria-modal
    // tell a screen reader this is a modal, not decoration; aria-labelledby
    // points at the <h2> so its text is announced as the dialog's name.
    // Focus handling (open/Escape/Tab-trap/restore) is wired up in
    // openLoginModal() below, which is the single place the modal is shown.
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-modal', 'true');
    panel.setAttribute('aria-labelledby', 'kc-login-title');
    panel.innerHTML =
      '<h2 class="kc-login-title" id="kc-login-title">Administrator login required</h2>' +
      '<label for="kc-login-username">Username</label>' +
      '<input id="kc-login-username" type="text" autocomplete="username" required>' +
      '<label for="kc-login-password">Password</label>' +
      '<input id="kc-login-password" type="password" autocomplete="current-password" required>' +
      '<div class="kc-login-error" id="kc-login-error"></div>' +
      '<div class="kc-login-actions">' +
      '<button type="button" class="kc-login-cancel">Cancel</button>' +
      '<button type="submit">Log in</button>' +
      '</div>' +
      '<div class="kc-login-forgot">' +
      '<button type="button" class="kc-login-forgot-link">Forgot password?</button>' +
      '</div>';
    overlay.appendChild(panel);
    document.body.appendChild(overlay);
    loginModalEl = overlay;
    loginFormEl = panel;
    loginTitleEl = panel.querySelector('.kc-login-title');
    loginUserEl = panel.querySelector('#kc-login-username');
    loginPassEl = panel.querySelector('#kc-login-password');
    loginErrorEl = panel.querySelector('#kc-login-error');
    loginCancelEl = panel.querySelector('.kc-login-cancel');
    loginSubmitEl = panel.querySelector('button[type="submit"]');
    loginForgotEl = panel.querySelector('.kc-login-forgot-link');
    loginForgotEl.addEventListener('click', function (evt) {
      evt.preventDefault();
      // Same themed overlay/panel classes as the login modal (owner
      // requirement: the popup follows the color theme) but its own
      // element -- the pending login modal is SUSPENDED (hidden, its
      // keyboard handling paused, its promise still pending) rather than
      // stacked or abandoned. Closing the reset modal -- Cancel or a
      // successful reset -- resumes it, so the operator can then log in
      // (and the original request retries) or Cancel it as usual. Refused
      // while a login POST is in flight (same A2 rule as Cancel/Escape).
      if (!loginActiveCtl || !loginActiveCtl.suspend()) return;
      openForgotPasswordModal();
    });
    return overlay;
  }

  // Shows the modal and resolves `true` (logged in) or `false` (cancelled)
  // -- this never rejects, so a caller's `.then()` never needs its own
  // `.catch()` just to handle a decline; window.fetch's wrapper below is
  // what turns a `false` into the AuthCancelled rejection callers see.
  // `titleText` distinguishes the no-session vs wrong-role wording without
  // needing two separate modals.
  function openLoginModal(titleText) {
    if (!loginModalEl) buildLoginModal();
    loginTitleEl.textContent = titleText;
    loginErrorEl.textContent = '';
    loginUserEl.value = '';
    loginPassEl.value = '';
    // Focus restoration (opus review, 2026-09-21): whatever had focus
    // before this modal opened -- the button/link that triggered the
    // denied action -- gets it back on close, so keyboard/screen-reader
    // users land where they were rather than at the top of the page.
    var previouslyFocused = document.activeElement;
    loginModalEl.removeAttribute('hidden');
    loginUserEl.focus();

    return new Promise(function (resolve) {
      var settled = false;
      // A2 (opus review, 2026-09-21): once the login POST is dispatched,
      // Escape/Cancel are ignored until it settles (success, failure, or
      // network error) -- otherwise a slow request racing an impatient
      // Escape press can finish(false) the modal a moment before the POST's
      // own .then() would have finish(true)'d it, so a login that actually
      // SUCCEEDED never retries the original action. There is nothing to
      // cancel once the request is already in flight; the operator can
      // still close the modal after it resolves, same as before.
      var submitting = false;
      // Set while the "Forgot password?" reset modal has this panel hidden
      // in its place: this promise stays pending (so a login after a
      // successful reset still retries the original request), but its
      // document-level keydown handler must not act on keys meant for the
      // other modal (Escape would settle this promise underneath it, and
      // the Tab trap would pull focus into these hidden fields).
      var suspended = false;
      function focusableEls() {
        // Order matches the DOM/tab order inside the panel: username,
        // password, cancel, submit, forgot-password link.
        return [loginUserEl, loginPassEl, loginCancelEl,
                loginFormEl.querySelector('button[type="submit"]'),
                loginForgotEl].filter(Boolean);
      }
      loginActiveCtl = {
        // Refused (false) while the login POST is in flight -- same A2 rule
        // as Escape/Cancel: nothing may hide the modal under a request
        // whose outcome would otherwise be lost.
        suspend: function () {
          if (submitting || settled) return false;
          suspended = true;
          loginModalEl.setAttribute('hidden', '');
          return true;
        },
        resume: function (noticeText) {
          if (settled) return;
          suspended = false;
          loginUserEl.value = '';
          loginPassEl.value = '';
          loginErrorEl.textContent = noticeText || '';
          loginModalEl.removeAttribute('hidden');
          loginUserEl.focus();
        }
      };
      function onKeydown(evt) {
        if (evt.key === 'Escape' || evt.keyCode === 27) {
          if (submitting) return;
          if (suspended) return; // the reset modal owns Escape right now
          // Same outcome as the Cancel button.
          evt.preventDefault();
          finish(false);
          return;
        }
        if (suspended) return;
        if (evt.key !== 'Tab' && evt.keyCode !== 9) return;
        // Focus trap: Tab/Shift+Tab cycles within the panel instead of
        // escaping to the page behind the overlay.
        var els = focusableEls();
        if (!els.length) return;
        var first = els[0], last = els[els.length - 1];
        if (evt.shiftKey) {
          if (document.activeElement === first || !panelContains(document.activeElement)) {
            evt.preventDefault();
            last.focus();
          }
        } else {
          if (document.activeElement === last || !panelContains(document.activeElement)) {
            evt.preventDefault();
            first.focus();
          }
        }
      }
      function panelContains(el) {
        return !!el && loginFormEl.contains(el);
      }
      function finish(ok) {
        if (settled) return;
        settled = true;
        suspended = false;
        loginActiveCtl = null;
        // The modal's DOM is built ONCE (openLoginModal reuses loginModalEl),
        // so the disabled/"Signing in..." state set by setSubmitting(true)
        // outlives this promise unless it is cleared here. finish(true) --
        // the success path -- returns without any other settle step, so
        // without this the NEXT time the modal opens both buttons are still
        // disabled: Cancel is unclickable and a disabled default submit
        // button also blocks implicit (Enter-key) form submission, leaving
        // the operator unable to log in again without reloading the page.
        setSubmitting(false);
        loginFormEl.removeEventListener('submit', onSubmit);
        loginCancelEl.removeEventListener('click', onCancel);
        loginModalEl.removeEventListener('click', onBackdrop);
        document.removeEventListener('keydown', onKeydown, true);
        loginModalEl.setAttribute('hidden', '');
        if (previouslyFocused && typeof previouslyFocused.focus === 'function') {
          previouslyFocused.focus();
        }
        resolve(ok);
      }
      function onCancel(evt) {
        if (submitting) return;
        evt.preventDefault();
        finish(false);
      }
      // Backdrop cancel (2026-09-24 review): a click that lands on the
      // overlay itself -- outside the panel -- is the same as Cancel. A
      // click inside the panel bubbles up with a different target and is
      // ignored here. Ignored while the login POST is in flight, same A2
      // rule as Escape/Cancel above.
      function onBackdrop(evt) {
        if (evt.target !== loginModalEl) return;
        if (submitting) return;
        evt.preventDefault();
        finish(false);
      }
      function setSubmitting(on) {
        submitting = on;
        if (loginCancelEl) loginCancelEl.disabled = on;
        if (loginSubmitEl) {
          loginSubmitEl.disabled = on;
          loginSubmitEl.textContent = on ? 'Signing in…' : 'Log in';
        }
      }
      function onSubmit(evt) {
        evt.preventDefault();
        if (submitting) return;
        setSubmitting(true);
        loginErrorEl.textContent = '';
        var body = 'username=' + encodeURIComponent(loginUserEl.value) +
                   '&password=' + encodeURIComponent(loginPassEl.value);
        // Login-POST timeout (opus review, 2026-09-21): measured ~0.9s on
        // hardware, but a stalled board would otherwise leave the modal
        // stuck (Escape/Cancel ignored while `submitting`) until the
        // browser's own timeout. 15s covers a 429 lockout ladder's
        // Retry-After without racing it.
        var controller = new AbortController();
        var timedOut = false;
        var timer = setTimeout(function () {
          timedOut = true;
          controller.abort();
        }, 15000);
        // nativeFetch, not window.fetch -- this request must never itself
        // be re-intercepted by the wrapper installed below (it already IS
        // the login attempt).
        nativeFetch('/api/auth/login', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body,
          signal: controller.signal
        }).then(function (resp) {
          clearTimeout(timer);
          if (resp.ok) {
            finish(true);
            return;
          }
          setSubmitting(false);
          if (resp.status === 429) {
            // Login lockout ladder: never auto-retry, just surface the
            // server's own Retry-After seconds (same wording as
            // login_page.html's own copy of this).
            var retryAfter = resp.headers.get('Retry-After');
            loginErrorEl.textContent = retryAfter ?
              ('Too many attempts -- try again in ' + retryAfter + 's.') :
              'Too many attempts -- try again later.';
            return;
          }
          return resp.text().then(function (text) {
            loginErrorEl.textContent = text || 'Login failed.';
          });
        }).catch(function () {
          clearTimeout(timer);
          setSubmitting(false);
          loginErrorEl.textContent = timedOut ? 'Login timed out.' : 'Network error.';
        });
      }
      loginFormEl.addEventListener('submit', onSubmit);
      loginCancelEl.addEventListener('click', onCancel);
      loginModalEl.addEventListener('click', onBackdrop);
      // Review fix, 2026-09-21: this listener is on `document` (capture
      // phase), not on the panel. A click on the overlay backdrop moves
      // focus to <body>, and a keydown bound to the panel would then never
      // fire -- so both Escape and the Tab trap silently stopped working
      // after one stray click, which is exactly the case a trap exists
      // for. finish() removes it (same capture flag, or removal is a
      // no-op), so no listener outlives the modal.
      document.addEventListener('keydown', onKeydown, true);
    });
  }

  // ---- Forgot-password reset flow (docs/TOTP_PASSWORD_RESET_PLAN.md) -----
  //
  // Same themed overlay/panel idiom as the login modal above (owner
  // requirement: the popup follows the color theme), reached only from the
  // "Forgot password?" link inside it. Two steps in one panel, swapped by
  // toggling `hidden` on two <div> sections rather than two modals:
  //   1. username + 6-digit authenticator code -> POST /api/auth/forgot.
  //      Always 202 with {"reset_token"} per section 6a UNLESS the board's
  //      clock is not SNTP-synced (503) or the shared login-ladder rate
  //      limit is tripped (429, PLAIN TEXT body, not JSON -- never call
  //      resp.json() on that path, only resp.text() if displayed at all).
  //   2. new password (twice, must match) -> POST /api/auth/reset with the
  //      token from step 1. 200 {"ok":true} success; any other status is
  //      the SAME generic failure text as an invalid code, on purpose --
  //      this flow must never reveal whether TOTP is enrolled for the
  //      typed username (enumeration channel).
  // The code is never logged (not even to console on error) and the reset
  // token is held only in a closure variable -- never localStorage/
  // sessionStorage. The token is single-use on this side too: it is nulled
  // the moment the /reset POST is built, and on every close.
  var forgotModalEl = null, forgotPanelEl = null, forgotStep1El = null, forgotStep2El = null,
      forgotUserEl = null, forgotCodeEl = null, forgotErrorEl = null,
      forgotNewPassEl = null, forgotNewPass2El = null, forgotErrorEl2 = null,
      forgotSubmit1El = null, forgotSubmit2El = null;
  var forgotResetToken = null;
  var forgotListenersWired = false;

  function buildForgotModal() {
    var overlay = document.createElement('div');
    overlay.className = 'kc-login-overlay';
    overlay.setAttribute('hidden', '');
    var panel = document.createElement('div');
    panel.className = 'kc-login-panel';
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-modal', 'true');
    panel.setAttribute('aria-labelledby', 'kc-forgot-title');
    panel.innerHTML =
      '<h2 class="kc-login-title" id="kc-forgot-title">Reset password</h2>' +
      '<form class="kc-forgot-step" id="kc-forgot-step1">' +
      '<label for="kc-forgot-username">Username</label>' +
      '<input id="kc-forgot-username" type="text" autocomplete="username" required>' +
      '<label for="kc-forgot-code">6-digit authenticator code</label>' +
      '<input id="kc-forgot-code" type="text" inputmode="numeric" pattern="[0-9]{6}" ' +
      'maxlength="6" autocomplete="one-time-code" required>' +
      '<div class="kc-login-error" id="kc-forgot-error"></div>' +
      '<div class="kc-login-actions">' +
      '<button type="button" class="kc-login-cancel" id="kc-forgot-cancel1">Cancel</button>' +
      '<button type="submit" id="kc-forgot-submit1">Continue</button>' +
      '</div>' +
      '</form>' +
      '<form class="kc-forgot-step" id="kc-forgot-step2" hidden>' +
      '<label for="kc-forgot-newpass">New password</label>' +
      '<input id="kc-forgot-newpass" type="password" autocomplete="new-password" required>' +
      '<label for="kc-forgot-newpass2">New password (again)</label>' +
      '<input id="kc-forgot-newpass2" type="password" autocomplete="new-password" required>' +
      '<div class="kc-login-error" id="kc-forgot-error2"></div>' +
      '<div class="kc-login-actions">' +
      '<button type="button" class="kc-login-cancel" id="kc-forgot-cancel2">Cancel</button>' +
      '<button type="submit" id="kc-forgot-submit2">Reset password</button>' +
      '</div>' +
      '</form>';
    overlay.appendChild(panel);
    document.body.appendChild(overlay);
    forgotModalEl = overlay;
    forgotPanelEl = panel;
    forgotStep1El = panel.querySelector('#kc-forgot-step1');
    forgotStep2El = panel.querySelector('#kc-forgot-step2');
    forgotUserEl = panel.querySelector('#kc-forgot-username');
    forgotCodeEl = panel.querySelector('#kc-forgot-code');
    forgotErrorEl = panel.querySelector('#kc-forgot-error');
    forgotNewPassEl = panel.querySelector('#kc-forgot-newpass');
    forgotNewPass2El = panel.querySelector('#kc-forgot-newpass2');
    forgotErrorEl2 = panel.querySelector('#kc-forgot-error2');
    forgotSubmit1El = panel.querySelector('#kc-forgot-submit1');
    forgotSubmit2El = panel.querySelector('#kc-forgot-submit2');
    return overlay;
  }

  // Generic, enumeration-safe failure text -- used for every non-success
  // status on both routes except the two the plan calls out by name (503
  // clock-not-synced, 429 rate limit), which get their own, more useful
  // wording since neither one reveals anything about a specific account.
  var KC_FORGOT_GENERIC_FAIL = 'Could not reset the password. Check the code and try again.';

  function kcForgotStatusMessage(status, bodyText) {
    if (status === 503) return 'Board clock is not synced yet -- codes will not validate. Try again shortly.';
    if (status === 429) {
      // Plain-text ladder body (section 6a review correction) -- never
      // parsed as JSON. Shown verbatim if present (it already carries a
      // human-readable "try again in Ns" message), else a generic fallback.
      return (bodyText && String(bodyText).trim()) || 'Too many attempts -- try again later.';
    }
    return KC_FORGOT_GENERIC_FAIL;
  }

  // Bumped on every open and close: a /forgot or /reset response that
  // lands after the operator cancelled (or reopened) the modal belongs to a
  // dead attempt and must not advance the step, store a token, or reopen
  // the login panel.
  var forgotGeneration = 0;

  function forgotModalVisible() {
    return !!forgotModalEl && !forgotModalEl.hasAttribute('hidden');
  }

  // Clears every typed value and the token. `noticeText`, if given, is
  // shown on the login panel this returns to (see loginActiveCtl above).
  function closeForgotModal(noticeText) {
    if (!forgotModalEl) return;
    forgotGeneration++;
    forgotModalEl.setAttribute('hidden', '');
    forgotResetToken = null; // never persisted anywhere else -- discard now
    if (forgotUserEl) forgotUserEl.value = '';
    if (forgotCodeEl) forgotCodeEl.value = '';
    if (forgotNewPassEl) forgotNewPassEl.value = '';
    if (forgotNewPass2El) forgotNewPass2El.value = '';
    if (forgotErrorEl) forgotErrorEl.textContent = '';
    if (forgotErrorEl2) forgotErrorEl2.textContent = '';
    if (forgotSubmit1El) forgotSubmit1El.disabled = false;
    if (forgotSubmit2El) forgotSubmit2El.disabled = false;
    forgotStep1El.hidden = false;
    forgotStep2El.hidden = true;
    if (loginActiveCtl) loginActiveCtl.resume(noticeText);
  }

  // Back to step 1 with a message -- used when a /reset attempt fails,
  // since the token it carried was already spent (single-use).
  function forgotBackToStep1(message) {
    forgotResetToken = null;
    forgotNewPassEl.value = '';
    forgotNewPass2El.value = '';
    forgotErrorEl2.textContent = '';
    forgotCodeEl.value = '';
    forgotStep2El.hidden = true;
    forgotStep1El.hidden = false;
    forgotErrorEl.textContent = message;
    forgotCodeEl.focus();
  }

  function openForgotPasswordModal() {
    if (!forgotModalEl) buildForgotModal();
    forgotGeneration++;
    forgotStep1El.hidden = false;
    forgotStep2El.hidden = true;
    forgotErrorEl.textContent = '';
    forgotErrorEl2.textContent = '';
    forgotUserEl.value = '';
    forgotCodeEl.value = '';
    forgotNewPassEl.value = '';
    forgotNewPass2El.value = '';
    forgotSubmit1El.disabled = false;
    forgotSubmit2El.disabled = false;
    forgotResetToken = null;
    forgotModalEl.removeAttribute('hidden');
    forgotUserEl.focus();
    if (forgotListenersWired) return;
    forgotListenersWired = true;

    function onCancel(evt) {
      evt.preventDefault();
      closeForgotModal();
    }
    function onBackdrop(evt) {
      if (evt.target !== forgotModalEl) return;
      closeForgotModal();
    }
    function visibleFocusables() {
      var step = forgotStep1El.hidden ? forgotStep2El : forgotStep1El;
      return Array.prototype.slice.call(step.querySelectorAll('input, button'))
        .filter(function (el) { return !el.disabled; });
    }
    // Registered once for the page's lifetime, so it must stay inert while
    // the modal is hidden -- otherwise every Escape anywhere on the page
    // would be swallowed (preventDefault) after the first open.
    function onKeydown(evt) {
      if (!forgotModalVisible()) return;
      if (evt.key === 'Escape' || evt.keyCode === 27) {
        evt.preventDefault();
        // This listener is registered once, during the first login modal;
        // every later login modal's own capture listener on `document` runs
        // AFTER it. closeForgotModal() resumes that login synchronously, so
        // without this the same Escape would fall through and cancel it.
        if (typeof evt.stopImmediatePropagation === 'function') evt.stopImmediatePropagation();
        closeForgotModal();
        return;
      }
      if (evt.key !== 'Tab' && evt.keyCode !== 9) return;
      // Focus trap, same shape as the login modal's.
      var els = visibleFocusables();
      if (!els.length) return;
      var first = els[0], last = els[els.length - 1];
      var inside = !!document.activeElement && forgotPanelEl.contains(document.activeElement);
      if (evt.shiftKey) {
        if (document.activeElement === first || !inside) { evt.preventDefault(); last.focus(); }
      } else {
        if (document.activeElement === last || !inside) { evt.preventDefault(); first.focus(); }
      }
    }
    forgotModalEl.querySelector('#kc-forgot-cancel1').addEventListener('click', onCancel);
    forgotModalEl.querySelector('#kc-forgot-cancel2').addEventListener('click', onCancel);
    forgotModalEl.addEventListener('click', onBackdrop);
    document.addEventListener('keydown', onKeydown, true);

    function onStep1Submit(evt) {
      evt.preventDefault();
      if (forgotSubmit1El.disabled) return;
      var gen = forgotGeneration;
      forgotErrorEl.textContent = '';
      forgotSubmit1El.disabled = true;
      var body = 'username=' + encodeURIComponent(forgotUserEl.value) +
                 '&code=' + encodeURIComponent(forgotCodeEl.value);
      nativeFetch('/api/auth/forgot', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body
      }).then(function (resp) {
        if (gen !== forgotGeneration) return;
        if (resp.status === 202) {
          return resp.json().then(function (data) {
            if (gen !== forgotGeneration) return;
            forgotResetToken = (data && typeof data.reset_token === 'string' && data.reset_token) || null;
            if (!forgotResetToken) {
              forgotErrorEl.textContent = KC_FORGOT_GENERIC_FAIL;
              return;
            }
            // The code has done its job -- do not leave it in the DOM.
            forgotCodeEl.value = '';
            forgotStep1El.hidden = true;
            forgotStep2El.hidden = false;
            forgotNewPassEl.value = '';
            forgotNewPass2El.value = '';
            forgotNewPassEl.focus();
          });
        }
        if (resp.status === 429) {
          return resp.text().then(function (text) {
            if (gen !== forgotGeneration) return;
            forgotErrorEl.textContent = kcForgotStatusMessage(429, text);
          });
        }
        forgotErrorEl.textContent = kcForgotStatusMessage(resp.status, null);
      }).catch(function () {
        if (gen !== forgotGeneration) return;
        forgotErrorEl.textContent = 'Network error.';
      }).then(function () {
        if (gen !== forgotGeneration) return;
        forgotSubmit1El.disabled = false;
      });
    }

    function onStep2Submit(evt) {
      evt.preventDefault();
      if (forgotSubmit2El.disabled) return;
      forgotErrorEl2.textContent = '';
      if (forgotNewPassEl.value !== forgotNewPass2El.value) {
        forgotErrorEl2.textContent = 'Passwords do not match.';
        return;
      }
      if (!forgotResetToken) {
        // Token was discarded or already spent -- restart at step 1 rather
        // than POSTing a reset with nothing to authorize it.
        forgotBackToStep1(KC_FORGOT_GENERIC_FAIL);
        return;
      }
      var gen = forgotGeneration;
      // Single use: the token leaves this closure exactly once, in this
      // request's body. Any retry has to go back through step 1.
      var token = forgotResetToken;
      forgotResetToken = null;
      forgotSubmit2El.disabled = true;
      var body = 'username=' + encodeURIComponent(forgotUserEl.value) +
                 '&reset_token=' + encodeURIComponent(token) +
                 '&new_password=' + encodeURIComponent(forgotNewPassEl.value);
      token = null;
      nativeFetch('/api/auth/reset', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body
      }).then(function (resp) {
        if (gen !== forgotGeneration) return;
        if (resp.status === 200) {
          return resp.json().then(function (data) {
            if (gen !== forgotGeneration) return;
            if (data && data.ok === true) {
              // Back to the (still pending) login modal so the operator
              // signs in with the password just set; that login then
              // retries whatever request originally raised the modal.
              closeForgotModal('Password reset. Log in with the new password.');
              return;
            }
            forgotBackToStep1(KC_FORGOT_GENERIC_FAIL);
          });
        }
        if (resp.status === 429) {
          return resp.text().then(function (text) {
            if (gen !== forgotGeneration) return;
            forgotBackToStep1(kcForgotStatusMessage(429, text));
          });
        }
        forgotBackToStep1(kcForgotStatusMessage(resp.status, null));
      }).catch(function () {
        if (gen !== forgotGeneration) return;
        forgotBackToStep1('Network error.');
      }).then(function () {
        if (gen !== forgotGeneration) return;
        forgotSubmit2El.disabled = false;
      });
    }

    forgotStep1El.addEventListener('submit', onStep1Submit);
    forgotStep2El.addEventListener('submit', onStep2Submit);
  }

  // ---- Which refusals may raise the modal (2026-09-24 review) ----------
  //
  // The owner's rule is "don't show the login until I do something that
  // would require it". The dashboard ("/") loads and polls a few gated
  // routes in the background (/api/zones, /api/autotune every 10 s, the
  // profile list), so "any 401 opens the modal" meant the modal popped on
  // page load and again every 10 s after each Cancel. A refusal may open
  // the modal only when:
  //   - the request was made while a user gesture (click/submit/change/
  //     keydown) was being dispatched, or it is a write (non-GET/HEAD)
  //     issued shortly after one (a click handler that first reads, then
  //     POSTs); or
  //   - the tab is on a gated page the operator deliberately navigated to
  //     (anything but the dashboard) and has not already declined the
  //     modal on this page load; or
  //   - a modal is already open (the request just joins that login); or
  //   - the caller passed `__kcUserAction: true` in init: a read the
  //     operator's own action issues outside the gesture's synchronous
  //     dispatch (the dashboard's long-press PID popup fires from a
  //     setTimeout, so no click/keydown is on the stack when it fetches).
  // Every other refusal rejects quietly with AuthCancelled (prompted:
  // false), so the page's own kcIsAuthCancelled() check keeps it silent.
  var kcGestureActive = false, kcLastGestureAt = 0;
  var KC_WRITE_AFTER_GESTURE_MS = 3000;
  function kcNoteGesture(evt) {
    // Typing/clicking inside the modal itself is not a new page action.
    if (loginModalEl && evt && evt.target && loginModalEl.contains(evt.target)) return;
    if (forgotModalEl && evt && evt.target && forgotModalEl.contains(evt.target)) return;
    kcGestureActive = true;
    kcLastGestureAt = Date.now();
    setTimeout(function () { kcGestureActive = false; }, 0);
  }
  ['click', 'submit', 'change', 'keydown'].forEach(function (type) {
    document.addEventListener(type, kcNoteGesture, true);
  });
  // Answering a native confirm() is a user gesture too, but it dispatches
  // no DOM event (2026-09-24 second review). The dashboard's Run button
  // reads the feasibility plan, then asks "Start firing ... now?", then
  // POSTs /api/profile_exec/start: an operator who took over 3 s on that
  // dialog had the POST refused quietly -- no modal, and the start catch
  // stays silent on AuthCancelled -- so Start did nothing at all. The clock
  // restarts when the dialog returns; kcConfirm resolves window.confirm at
  // call time, so it is covered as well.
  var kcNativeConfirm = (typeof window.confirm === 'function') ? window.confirm.bind(window) : null;
  if (kcNativeConfirm) {
    window.confirm = function (message) {
      var answer = kcNativeConfirm(message);
      kcLastGestureAt = Date.now();
      return answer;
    };
  }
  function kcRequestIsUserInitiated(method) {
    if (kcGestureActive) return true;
    var m = String(method || 'GET').toUpperCase();
    return m !== 'GET' && m !== 'HEAD' && (Date.now() - kcLastGestureAt) < KC_WRITE_AFTER_GESTURE_MS;
  }
  function kcPageIsDashboard() {
    return window.location.pathname === '/';
  }

  // Only one modal in flight at a time -- if a second 403 arrives while the
  // operator is still typing into the first prompt, it waits on the SAME
  // login instead of stacking a second overlay on top.
  var pendingLogin = null;
  // Set once the operator declines the modal on this page load (Cancel,
  // Escape, backdrop): from then on only an explicit action can raise it
  // again, so a background poll never re-nags. See kcRequestIsUserInitiated.
  var authPromptDeclined = false;
  function authPromptAllowed(userInitiated) {
    if (pendingLogin) return true;
    if (userInitiated) return true;
    return !authPromptDeclined && !kcPageIsDashboard();
  }
  function ensureAdminLogin(titleText) {
    if (!pendingLogin) {
      pendingLogin = openLoginModal(titleText).then(function (ok) {
        pendingLogin = null;
        return ok;
      });
    }
    return pendingLogin;
  }

  window.fetch = function (input, init) {
    var url = (typeof input === 'string') ? input : (input && input.url) || '';
    if (isAuthExemptUrl(url)) {
      return nativeFetch(input, init);
    }
    // Review fix, 2026-09-21 (b): a Request object's body is a one-shot
    // stream -- reading it (sending the request) consumes it, so a retry
    // built from the same Request would send an empty body. clone() it
    // before the first send so the retry-after-login below still has one.
    // When `input` is a plain URL string, the body lives in `init.body`
    // instead, which nativeFetch does not consume (only the underlying
    // stream implementation would, and a plain string/FormData/URLSearchParams
    // body is not a stream), so no cloning is needed for that shape.
    var inputForRetry = input;
    if (typeof Request !== 'undefined' && input instanceof Request) {
      inputForRetry = input.clone();
    }
    // One-shot guard (b): a request built by re-entering window.fetch below
    // carries this marker so a SECOND insufficient_role 403 (e.g. the
    // freshly-logged-in account still lacks the role, or the session
    // dropped again immediately) is handed back raw instead of opening a
    // second modal and looping.
    var alreadyRetried = !!(init && init.__kcAuthRetried);
    // Opt-out (2026-09-21, opus review A1): kcOtaAuthedFetch signs its
    // request with a single-use nonce (X-Ota-Mac), so replaying the SAME
    // signed request after this wrapper's own login modal -- as the retry
    // below does for every other caller -- fails the board's nonce check
    // once OTA_AUTH_NONCE_EXPIRY_MS (30 s) has passed, which a human typing
    // a password routinely exceeds. kcOtaAuthedFetch instead owns its own
    // login+re-sign retry (fetches a fresh challenge and re-derives the MAC
    // before resending) and sets this marker on its first send so this
    // generic wrapper stands down and hands the 403 straight back to it,
    // rather than both layers opening a modal for the same failure.
    var callerHandlesAuth = !!(init && init.__kcCallerHandlesAuth);
    // Decided NOW, at call time: the gesture flag only holds during the
    // event's synchronous dispatch, long gone by the time a response lands.
    var reqMethod = (init && init.method) || (input && typeof input === 'object' && input.method) || 'GET';
    var userInitiated = !!(init && init.__kcUserAction) || kcRequestIsUserInitiated(reqMethod);
    function retryOnce() {
      var retryInit = {};
      var src = init || {};
      for (var k in src) {
        if (Object.prototype.hasOwnProperty.call(src, k)) retryInit[k] = src[k];
      }
      retryInit.__kcAuthRetried = true;
      return window.fetch(inputForRetry, retryInit);
    }
    function authCancelled(prompted) {
      // Owner report, 2026-09-24: Cancel on the modal must reject the
      // pending action cleanly, distinguishably, so a caller can tell
      // "the operator declined to sign in" apart from any other failure
      // and stay quiet about it (no error toast) rather than surface the
      // raw 401/403 as if the server itself refused the request. Callers
      // that want a quiet UI already have somewhere to catch this: see
      // kc-auth-cancelled handling added alongside this change.
      var e = new Error('sign-in cancelled');
      e.name = 'AuthCancelled';
      // false: a background request refused without ever showing the modal
      // (see authPromptAllowed) -- same quiet handling for callers.
      e.prompted = prompted !== false;
      throw e;
    }
    function promptThenRetry(titleText) {
      if (!authPromptAllowed(userInitiated)) return authCancelled(false);
      return ensureAdminLogin(titleText).then(function (ok) {
        if (!ok) {
          authPromptDeclined = true;
          return authCancelled(true);
        }
        return retryOnce();
      });
    }
    return nativeFetch(input, init).then(function (resp) {
      if (!alreadyRetried && !callerHandlesAuth && resp.status === 401) {
        // Owner report, 2026-09-24 (see is_page_shell_get() in
        // http_auth_http.c): no session at all no longer navigates the tab
        // away. The page itself never shows a login form or redirect on
        // load -- this is the FIRST time an unauthenticated caller learns
        // anything needs a session, and it happens only when an actual
        // action/data fetch needs one, via the SAME shared, cancelable
        // modal the insufficient-role case below uses (one modal, one
        // retry queue, `pendingLogin` singleton -- concurrent 401s and 403s
        // share it). Success retries this exact request once; Cancel
        // rejects with AuthCancelled instead of resolving to the raw 401,
        // so a caller's own .then() never mistakes "declined to sign in"
        // for a real response body to parse.
        return promptThenRetry('Sign in required');
      }
      if (!alreadyRetried && !callerHandlesAuth && resp.status === 403 &&
          resp.headers.get('X-Kiln-Auth-Reason') === 'insufficient_role') {
        // Signed in, wrong role -- offer to elevate in place and retry the
        // SAME request once on success. Cancel rejects with AuthCancelled
        // (unified with the 401 case above, 2026-09-24) rather than
        // resolving with the original 403, so both refusal shapes give
        // callers one consistent contract to catch.
        //
        // Review fix, 2026-09-21 (b), CORRECTED by review the same day:
        // the retry goes back through window.fetch (this same wrapper)
        // rather than nativeFetch so that a 401 on the retry -- the session
        // dropping again between the login and the resend -- still gets
        // the same modal treatment above instead of surfacing as a raw 401
        // body.
        //
        // What re-entry does NOT do, despite an earlier comment here saying
        // it did: kcFetchWithSafetyAck and kcOtaAuthedFetch sit ABOVE this
        // wrapper (they CALL fetch; this wrapper is the innermost layer),
        // so re-entering window.fetch cannot re-run either of them. The
        // retried request carries whatever headers the original init had --
        // for an ordinary (non-OTA) caller that is fine, it is the same
        // request, unsigned, and the board only cares about the session
        // cookie. An OTA-family request signed by kcOtaAuthedFetch never
        // reaches this branch at all any more (opus review A1, 2026-09-21):
        // it sets __kcCallerHandlesAuth on its own first send, which the
        // `callerHandlesAuth` check above hands straight back to it instead
        // of retrying here, because replaying that same signed request
        // would carry an already-sent X-Ota-Mac -- accepted by the board
        // only because http_auth_http.c denies an insufficient_role request
        // BEFORE the route handler runs (so ota_http_verify_request() never
        // consumed the nonce), and only within OTA_AUTH_NONCE_EXPIRY_MS
        // (30 s), which a human typing into this modal easily exceeds.
        // kcOtaAuthedFetch's own retry (below, in this file) fetches a
        // fresh challenge and re-derives the MAC instead of replaying.
        // The 428/no-safety-ack path is unaffected either way:
        // kcFetchWithSafetyAck inspects whatever response this wrapper's
        // promise resolves to, retry included.
        return promptThenRetry('Administrator login required');
      }
      return resp;
    });
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

  // ---- OTA challenge/response handshake --------------------------------
  //
  // Owner report, 2026-09-18: the Settings page's "Reboot both processors"
  // button sent a bare, unauthenticated POST /api/sw_reset and always failed
  // with "missing or malformed X-Ota-Mac header" -- the server route
  // (sw_reset_http.c's sw_reset_post_handler()) requires the same
  // challenge/HMAC handshake every OTA-family mutating route requires
  // (ota_http.c's ota_http_authenticate_request()). The same defect affected
  // the factory-reset buttons on that same page. Root cause: two pages
  // needed this handshake and only one (net/ota_page.html) had it -- as an
  // inline, page-local copy, so the other page silently shipped a dead
  // button instead of sharing the working code.
  //
  // These functions are that handshake, moved here (out of ota_page.html's
  // own inline <script>) so every page that needs it -- today ota_page.html
  // and settings_page.html, potentially more later -- calls ONE
  // implementation instead of risking a second copy drifting out of sync
  // with CommonFW/docs/UPDATE_PROTOCOL.md section 2 step 2:
  //   key = HMAC-SHA256(ap_password, "kilnctl-ota-v1")
  //   mac = HMAC-SHA256(key, nonce_bytes || context)
  // ota_http.c's ota_http_verify_request() (see its own ctx_str switch) and
  // PcTools' ota_http_client.py's derive_mac() both implement the same
  // derivation; PcTools is a separate, legitimately independent
  // implementation in a different language and is NOT part of this
  // de-duplication.
  //
  // Valid context strings, read directly off ota_http.c's ctx_str switch
  // (ota_http_verify_request()) rather than guessed or copied from an older
  // comment that can go stale: "esp", "pico", "esp-rollback",
  // "pico-rollback", "recovery" (exit-recovery-mode), "factory-reset",
  // "sw-reset", and "boot-guard-reset" (PcTools-only today -- see
  // tools/PcTools/src/kilnctrl/ota_http_client.py -- no web page sends this
  // context yet, but the C side accepts it and a future page-side caller
  // should just pass "boot-guard-reset" through kcOtaAuthedFetch below, not
  // reimplement anything).
  //
  // Pure-JS SHA-256 / HMAC-SHA256: this page set is served over plain HTTP
  // (the board has no TLS cert -- reachable from its own fallback AP at
  // 192.168.4.1 during first-boot provisioning, and from the home LAN
  // afterward, neither of which is a "secure context" per the Secure
  // Contexts spec), so window.crypto.subtle (only exposed in a secure
  // context) is not available here. Standard FIPS 180-4 SHA-256 / RFC 2104
  // HMAC construction, byte arrays only (no external libs, no eval, no
  // network fetch of code).
  function sha256(bytes) {
    var K = [
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    ];
    var H = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];

    var bitLen = bytes.length * 8;
    var withOne = bytes.concat([0x80]);
    while (withOne.length % 64 !== 56) withOne.push(0);
    for (var s = 56; s >= 0; s -= 8) withOne.push(Math.floor(bitLen / Math.pow(2, s)) & 0xff);

    function rotr(x, n) { return (x >>> n) | (x << (32 - n)); }

    for (var chunk = 0; chunk < withOne.length; chunk += 64) {
      var w = new Array(64);
      for (var i = 0; i < 16; i++) {
        var o = chunk + i * 4;
        w[i] = ((withOne[o] << 24) | (withOne[o + 1] << 16) | (withOne[o + 2] << 8) | withOne[o + 3]) >>> 0;
      }
      for (i = 16; i < 64; i++) {
        var s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
        var s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
      }
      var a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
      for (i = 0; i < 64; i++) {
        var S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        var ch = (e & f) ^ (~e & g);
        var temp1 = (h + S1 + ch + K[i] + w[i]) >>> 0;
        var S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        var maj = (a & b) ^ (a & c) ^ (b & c);
        var temp2 = (S0 + maj) >>> 0;
        h = g; g = f; f = e; e = (d + temp1) >>> 0;
        d = c; c = b; b = a; a = (temp1 + temp2) >>> 0;
      }
      H[0] = (H[0] + a) >>> 0; H[1] = (H[1] + b) >>> 0; H[2] = (H[2] + c) >>> 0; H[3] = (H[3] + d) >>> 0;
      H[4] = (H[4] + e) >>> 0; H[5] = (H[5] + f) >>> 0; H[6] = (H[6] + g) >>> 0; H[7] = (H[7] + h) >>> 0;
    }

    var out = [];
    for (i = 0; i < 8; i++) {
      out.push((H[i] >>> 24) & 0xff, (H[i] >>> 16) & 0xff, (H[i] >>> 8) & 0xff, H[i] & 0xff);
    }
    return out;
  }

  function hmacSha256(keyBytes, msgBytes) {
    var blockSize = 64;
    if (keyBytes.length > blockSize) keyBytes = sha256(keyBytes);
    var padded = keyBytes.concat(new Array(blockSize - keyBytes.length).fill(0));
    var ipad = padded.map(function (b) { return b ^ 0x36; });
    var opad = padded.map(function (b) { return b ^ 0x5c; });
    var inner = sha256(ipad.concat(msgBytes));
    return sha256(opad.concat(inner));
  }

  function strToBytes(s) {
    var out = [];
    for (var i = 0; i < s.length; i++) {
      var c = s.charCodeAt(i);
      if (c < 0x80) out.push(c);
      else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
      else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
    }
    return out;
  }

  function bytesToHex(bytes) {
    return bytes.map(function (b) { return ('0' + b.toString(16)).slice(-2); }).join('');
  }

  function hexToBytes(hex) {
    var out = [];
    for (var i = 0; i < hex.length; i += 2) out.push(parseInt(hex.substr(i, 2), 16));
    return out;
  }

  // Namespaced (not bare globals) per the de-duplication plan, so a page
  // that happens to define its own "sha256" or "hmacSha256" identifier
  // (unlikely, but this is exactly the kind of collision a shared file must
  // not risk) can never collide with these.
  window.kcOtaCrypto = {
    sha256: sha256,
    hmacSha256: hmacSha256,
    strToBytes: strToBytes,
    bytesToHex: bytesToHex,
    hexToBytes: hexToBytes,
  };

  var OTA_KDF_CONTEXT = 'kilnctl-ota-v1';

  // GET /api/ota/challenge -> the nonce, decoded to a byte array. Every
  // authenticated OTA-family request fetches a fresh one; the server enforces
  // single-use/expiry server-side (ota_http.c), this is just the client half.
  window.kcOtaGetChallenge = function () {
    return fetch('/api/ota/challenge').then(function (r) {
      if (!r.ok) throw new Error('challenge request failed: HTTP ' + r.status);
      return r.json();
    }).then(function (obj) {
      return hexToBytes(obj.nonce);
    });
  };

  window.kcOtaDeriveMac = function (apPassword, nonceBytes, context) {
    var key = hmacSha256(strToBytes(apPassword), strToBytes(OTA_KDF_CONTEXT));
    var msg = nonceBytes.concat(strToBytes(context));
    return hmacSha256(key, msg);
  };

  // The one call every page should use: fetches a fresh challenge, derives
  // the MAC for `context`, sets X-Ota-Mac, and dispatches through the
  // EXISTING window.kcFetchWithSafetyAck so the 428-retry/no-safety-ack
  // behaviour stays exactly what it always was -- this function adds
  // authentication, it does not change what happens once the request is
  // authenticated. Merges (never clobbers) any caller-supplied `init.headers`
  // -- a caller setting Content-Type (pushImage's octet-stream upload,
  // settings_page.html's form-encoded factory-reset body) keeps it.
  //
  // Own insufficient_role retry (opus review A1, 2026-09-21): a signed OTA
  // request that comes back 403/insufficient_role cannot be handled by the
  // generic window.fetch wrapper's replay-after-login the way every other
  // caller's request is -- the prehandler denies before the route consumes
  // the nonce, so a replayed X-Ota-Mac is only accepted within the 30 s
  // OTA_AUTH_NONCE_EXPIRY_MS window, which a human typing a password into
  // the login modal routinely exceeds (a stale-nonce 403 after a successful
  // login, reported as a mysterious second failure). So this function marks
  // its own request `__kcCallerHandlesAuth` (the wrapper stands down for
  // that marker -- see window.fetch's own comment) and, on that specific
  // 403, does its own single login-then-retry: `ensureAdminLogin` is the
  // same modal/promise the wrapper uses (defined earlier in this closure),
  // then a brand-new challenge is fetched and the MAC re-derived before
  // resending -- a re-signing, not a replay. Exactly one retry: `attempt()`
  // is called at most twice total, and a second insufficient_role 403 (the
  // freshly-logged-in account still lacking the role) is handed back as-is.
  window.kcOtaAuthedFetch = function (url, context, apPassword, init) {
    init = init || {};
    function attempt() {
      return window.kcOtaGetChallenge().then(function (nonce) {
        var mac = window.kcOtaDeriveMac(apPassword, nonce, context);
        var merged = {};
        for (var k in init) { if (Object.prototype.hasOwnProperty.call(init, k)) merged[k] = init[k]; }
        merged.headers = {};
        var src = init.headers || {};
        // Same Headers-instance-or-plain-object normalisation as
        // kcFetchWithSafetyAck's own retry path above.
        if (typeof src.forEach === 'function' && !(src instanceof Array)) {
          src.forEach(function (v, k2) { merged.headers[k2] = v; });
        } else {
          for (var k3 in src) {
            if (Object.prototype.hasOwnProperty.call(src, k3)) merged.headers[k3] = src[k3];
          }
        }
        merged.headers['X-Ota-Mac'] = bytesToHex(mac);
        merged.__kcCallerHandlesAuth = true;
        return window.kcFetchWithSafetyAck(url, merged);
      });
    }
    return attempt().then(function (resp) {
      if (resp.status === 403 &&
          resp.headers.get('X-Kiln-Auth-Reason') === 'insufficient_role') {
        return ensureAdminLogin('Administrator login required').then(function (ok) {
          if (!ok) return resp;
          return attempt();
        });
      }
      return resp;
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
  // SETUP_WIZARD.md: "OFFER the wizard rather than forcing a redirect"
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

  // ---- Session inactivity lock (WEB_AUTH_PLAN.md section 8) --------------
  //
  // Convenience only -- the SERVER is what actually expires the session
  // (http_session_iface.c's http_auth_session_status()/_touch(), which
  // re-validates on every call so an expired session can never be revived
  // just because a client keeps calling in). This widget polls the passive
  // GET /api/auth/session status route (deliberately ROUTE_TIER_OPEN on the
  // server so the poll itself never counts as activity -- see
  // http_auth_decision_counts_as_activity()), shows a small non-blocking
  // "stay unlocked" prompt in the final window the server reports, and
  // returns to the Dashboard ("/") -- not /login, per the plan's own
  // wording -- once the server reports the session is actually gone. A
  // client that ignores this prompt, or has JS disabled entirely, is still
  // refused by the server on its next USER/ADMIN request regardless.
  var SESSION_POLL_MS = 5000;
  var sessionPollTimer = null;
  var lockPromptEl = null;
  var lastKnownRole = null; // null until the first successful poll

  function buildLockPrompt() {
    var el = document.createElement('div');
    el.className = 'kc-lock-prompt';
    el.setAttribute('hidden', '');
    el.setAttribute('role', 'alert');
    var msg = document.createElement('span');
    msg.textContent = 'Your session is about to lock from inactivity.';
    var btn = document.createElement('button');
    btn.type = 'button';
    btn.textContent = 'Stay unlocked';
    btn.addEventListener('click', function () {
      fetch('/api/auth/session/extend', { method: 'POST' })
        .then(function () {
          el.setAttribute('hidden', '');
          pollSession(); // re-poll immediately so the countdown reflects the extension
        })
        .catch(function () {
          // Extend failed (e.g. the session already expired underneath the
          // click) -- leave the prompt up; the next regular poll reconciles
          // against the server's real state either way.
        });
    });
    el.appendChild(msg);
    el.appendChild(btn);
    document.body.appendChild(el);
    return el;
  }

  function pollSession() {
    if (document.visibilityState === 'hidden') {
      sessionPollTimer = setTimeout(pollSession, SESSION_POLL_MS);
      return;
    }
    fetch('/api/auth/session')
      .then(function (r) {
        if (!r.ok) throw new Error('http ' + r.status);
        return r.json();
      })
      .then(function (st) {
        var role = st && st.role;
        // Transition from a real session to "none" -- the server has
        // actually expired it. Return to the Dashboard, per the plan's
        // wording ("the interface returns to the Dashboard"), not /login.
        if (lastKnownRole && lastKnownRole !== 'none' && role === 'none') {
          if (lockPromptEl) lockPromptEl.setAttribute('hidden', '');
          if (window.location.pathname !== '/') {
            window.location.href = '/';
          }
        }
        lastKnownRole = role;

        // Shows/hides nav.js's Log out control for the role this poll just
        // reported -- the SAME poll response, not a second fetch, so the
        // button can never disagree with lastKnownRole above about whether a
        // session exists. auth_enabled is passed through because role alone
        // cannot distinguish a real admin session from section 11's
        // auth-off collapse (which also reports "admin"); see
        // setAuthState()'s own comment in nav.js.
        if (window.kcNav && window.kcNav.setAuthState) {
          window.kcNav.setAuthState(role, !!(st && st.auth_enabled));
        }

        if (lockPromptEl) {
          if (st && st.prompt) {
            lockPromptEl.removeAttribute('hidden');
          } else {
            lockPromptEl.setAttribute('hidden', '');
          }
        }
        sessionPollTimer = setTimeout(pollSession, SESSION_POLL_MS);
      })
      .catch(function () {
        // A transient failure here is exactly what the heartbeat's own
        // connection-lost banner already covers -- just retry on schedule.
        sessionPollTimer = setTimeout(pollSession, SESSION_POLL_MS);
      });
  }

  function init() {
    bannerEl = buildBanner();
    recoveryBannerEl = buildRecoveryBanner();
    setupBannerEl = buildSetupBanner();
    stopBarEl = buildStopBar();
    observeStopBarHeight(stopBarEl);
    buildUnitBtn();
    lockPromptEl = buildLockPrompt();
    pollHeartbeat();
    pollRecoveryMode();
    pollSetupOffer();
    pollSession();
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
