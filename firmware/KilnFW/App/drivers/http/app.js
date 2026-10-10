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

  // ---- kcModalStack -------------------------------------------------------
  //
  // Accessibility follow-up (ROADMAP.md "Owner requests 2026-09-22" item 1):
  // the confirm/alert modal, the admin-login modal, and the forgot-password
  // reset modal each register their own `document`-level capture keydown
  // listener, added/removed independently. Multiple listeners on the same
  // target/type/capture triple fire in REGISTRATION order, not stacking
  // (visual) order -- so if a login modal opens on top of an already-open
  // confirm dialog (confirm's listener registered first), a single Escape
  // press used to run the confirm dialog's own unconditional
  // `finish(...)`, closing and cancelling the BOTTOM modal while the login
  // modal -- the one actually on screen and focused -- stayed open. This
  // tiny stack is the shared fix: every modal pushes an id when it opens
  // and pops it when it closes, and each modal's own Escape/Tab handler
  // bails out immediately unless its id is the current top -- so only the
  // topmost modal ever reacts to a key, regardless of listener
  // registration order or which pair of modals is stacked. A login modal
  // "suspended while forgot-password is up" KEEPS its slot (the reset
  // modal simply sits above it) rather than popping and re-pushing: a
  // re-push on resume would jump the login above anything opened on top of
  // the reset modal in the meantime (e.g. a kcAlert landing while the
  // /reset POST is in flight). Every Escape handler that acts also calls
  // stopImmediatePropagation(), so the modal uncovered by that close can
  // never see the same keypress and close as well (two layers per press).
  var kcModalStack = [];
  var kcModalIdSeq = 0;
  function kcModalPush() {
    var id = ++kcModalIdSeq;
    kcModalStack.push(id);
    return id;
  }
  function kcModalPop(id) {
    var i = kcModalStack.lastIndexOf(id);
    if (i !== -1) kcModalStack.splice(i, 1);
  }
  function kcModalIsTop(id) {
    return kcModalStack.length > 0 && kcModalStack[kcModalStack.length - 1] === id;
  }

  // Shared focus-default helper (ROADMAP.md same follow-up item, gap 1):
  // given a modal's required inputs in tab order, focus the first one that
  // is still empty (e.g. username, or the TOTP code field on the reset
  // step), falling back to the last field when every field already has a
  // value (e.g. a browser autofilled both, or the operator re-opens a
  // modal after typing into it and cancelling) -- never blindly the first
  // field regardless of its content.
  function kcFocusFirstEmpty(fields) {
    for (var i = 0; i < fields.length; i++) {
      var el = fields[i];
      if (el && !el.value) { el.focus(); return; }
    }
    var last = fields[fields.length - 1];
    if (last) last.focus();
  }

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

  // ---- kcConfirm / kcAlert --------------------------------------------
  //
  // A single named seam for "ask before doing something destructive"
  // (UI_PLAN.md item 5: delete a profile, force a relay on, forget a
  // network, factory-reset) and for reporting a failure that used to be a
  // native alert(). kcConfirm used to be a thin wrapper over
  // window.confirm() -- every call site in every page already went
  // through this name instead of calling confirm()/alert() directly, so
  // the owner's "every confirmation popup must be an in-page, cancellable
  // modal that follows the color theme" rule (the login modal above is
  // the reference; ota_page.html's confirmForceProtocol() was the first
  // page-local themed confirm, built before this shared version existed)
  // lands here as a one-file change instead of a grep-and-replace across
  // every page. Both return a Promise now, not a bool, so every existing
  // caller has to become a .then()/await -- same shape kcOtaAuthedFetch
  // callers already use.
  //
  // Reuses the same .kc-login-overlay/.kc-login-panel/.kc-login-actions/
  // .kc-login-cancel tokens the login modal above and confirmForceProtocol()
  // used, plus a wider .kc-confirm-panel rule in theme.css (generalized
  // from ota_page.html's page-local .ota-confirm-panel). `message` may
  // contain literal "\n\n" to split into separate paragraphs; each is
  // rendered via textContent, never innerHTML, so an operator- or server-
  // supplied string spliced into a confirm/alert message can't inject
  // markup. `opts` is optional: {title, okLabel}. Only call from inside an
  // event-handler callback, same rule kcConfirm always had (this file's own
  // header comment).
  var confirmModalEl = null, confirmTitleEl = null, confirmTextEl = null,
      confirmCancelEl = null, confirmOkEl = null;

  function buildConfirmModal() {
    var overlay = document.createElement('div');
    overlay.className = 'kc-login-overlay';
    overlay.setAttribute('hidden', '');
    var panel = document.createElement('div');
    panel.className = 'kc-login-panel kc-confirm-panel';
    panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-modal', 'true');
    panel.setAttribute('aria-labelledby', 'kc-confirm-title');
    panel.setAttribute('aria-describedby', 'kc-confirm-text');
    panel.innerHTML =
      '<h2 class="kc-login-title" id="kc-confirm-title"></h2>' +
      '<div id="kc-confirm-text"></div>' +
      '<div class="kc-login-actions">' +
      '<button type="button" class="kc-login-cancel">Cancel</button>' +
      '<button type="button" class="kc-confirm-ok"></button>' +
      '</div>';
    overlay.appendChild(panel);
    document.body.appendChild(overlay);
    confirmModalEl = overlay;
    confirmTitleEl = panel.querySelector('#kc-confirm-title');
    confirmTextEl = panel.querySelector('#kc-confirm-text');
    confirmCancelEl = panel.querySelector('.kc-login-cancel');
    confirmOkEl = panel.querySelector('.kc-confirm-ok');
  }

  // Resolves true on OK, false on Cancel/Escape/backdrop -- except in
  // `opts.alertOnly` mode (kcAlert below), which hides the Cancel button
  // and treats Escape/backdrop the same as OK, always resolving true (an
  // alert has nothing to decline). Never rejects. Focus starts on Cancel
  // (the safe choice) when present, else OK; Tab stays inside the panel;
  // focus returns to whatever had it before -- same behaviour as the login
  // modal and confirmForceProtocol() above/below.
  function openConfirmModal(message, opts) {
    if (!confirmModalEl) buildConfirmModal();
    confirmTitleEl.textContent = opts.title || (opts.alertOnly ? 'Notice' : 'Confirm');
    confirmTextEl.textContent = '';
    String(message === null || message === undefined ? '' : message).split('\n\n').forEach(function (para) {
      var p = document.createElement('p');
      p.textContent = para;
      confirmTextEl.appendChild(p);
    });
    confirmOkEl.textContent = opts.okLabel || 'OK';
    confirmCancelEl.style.display = opts.alertOnly ? 'none' : '';
    var previouslyFocused = document.activeElement;
    confirmModalEl.removeAttribute('hidden');
    var kcModalId = kcModalPush();
    (opts.alertOnly ? confirmOkEl : confirmCancelEl).focus();
    return new Promise(function (resolve) {
      function finish(ok) {
        kcModalPop(kcModalId);
        confirmOkEl.removeEventListener('click', onOk);
        confirmCancelEl.removeEventListener('click', onCancel);
        confirmModalEl.removeEventListener('click', onBackdrop);
        document.removeEventListener('keydown', onKeydown, true);
        confirmModalEl.setAttribute('hidden', '');
        if (previouslyFocused && typeof previouslyFocused.focus === 'function') previouslyFocused.focus();
        resolve(ok);
      }
      function onOk(evt) { evt.preventDefault(); finish(true); }
      function onCancel(evt) { evt.preventDefault(); finish(false); }
      function onBackdrop(evt) { if (evt.target === confirmModalEl) finish(!!opts.alertOnly); }
      function onKeydown(evt) {
        // Stacking guard (kcModalStack above): another modal opened on top
        // of this one (e.g. an admin-login modal escalating out of an
        // in-flight confirm) owns the keyboard until it closes.
        if (!kcModalIsTop(kcModalId)) return;
        if (evt.key === 'Escape' || evt.keyCode === 27) {
          evt.preventDefault();
          // One Escape closes one layer: a listener registered after this
          // one must not see the modal uncovered by this close as topmost.
          if (typeof evt.stopImmediatePropagation === 'function') evt.stopImmediatePropagation();
          finish(!!opts.alertOnly);
          return;
        }
        if (evt.key !== 'Tab' && evt.keyCode !== 9) return;
        evt.preventDefault();
        if (opts.alertOnly) { confirmOkEl.focus(); return; }
        (document.activeElement === confirmCancelEl ? confirmOkEl : confirmCancelEl).focus();
      }
      confirmOkEl.addEventListener('click', onOk);
      confirmCancelEl.addEventListener('click', onCancel);
      confirmModalEl.addEventListener('click', onBackdrop);
      document.addEventListener('keydown', onKeydown, true);
    });
  }

  // One modal on screen at a time. There is exactly one overlay/panel, so a
  // second openConfirmModal() while the first is still up would overwrite
  // its text and attach a second set of button listeners -- one click would
  // then answer BOTH callers, the first one with a question it was never
  // shown (e.g. a late kcAlert() from a failed fetch landing on top of an
  // open "Start firing?" confirm). A request that arrives while one is open
  // waits for it to close, then opens in turn; the common, uncontended case
  // still opens synchronously inside the caller's click handler.
  var confirmTail = null;
  function enqueueConfirmModal(message, opts) {
    var run = function () { return openConfirmModal(message, opts); };
    var p = confirmTail ? confirmTail.then(run) : run();
    var tail = p.then(function () {}, function () {});
    confirmTail = tail;
    tail.then(function () { if (confirmTail === tail) confirmTail = null; });
    return p;
  }

  window.kcConfirm = function (message, opts) {
    return enqueueConfirmModal(message, opts || {});
  };

  // Themed replacement for window.alert() -- OK-only, resolves (never
  // rejects) once dismissed by OK, Escape, or a backdrop click.
  window.kcAlert = function (message, opts) {
    var o = { alertOnly: true };
    if (opts && opts.title) o.title = opts.title;
    if (opts && opts.okLabel) o.okLabel = opts.okLabel;
    return enqueueConfirmModal(message, o).then(function () {});
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
    // Password is always cleared -- never leave a typed password sitting in
    // the DOM across opens. Username is NOT force-cleared: a browser that
    // autofills the (autocomplete="username") field, or an operator who
    // typed a username and then cancelled, should not have to retype it,
    // and the focus-default rule right below only makes sense if the field
    // can genuinely already be non-empty.
    loginPassEl.value = '';
    // Focus restoration (opus review, 2026-09-21): whatever had focus
    // before this modal opened -- the button/link that triggered the
    // denied action -- gets it back on close, so keyboard/screen-reader
    // users land where they were rather than at the top of the page.
    var previouslyFocused = document.activeElement;
    loginModalEl.removeAttribute('hidden');
    var kcModalId = kcModalPush();
    // Focus default (ROADMAP.md modal-accessibility follow-up): username
    // if it's empty, else password -- never blindly the username field
    // when it's already filled in.
    kcFocusFirstEmpty([loginUserEl, loginPassEl]);

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
          // `suspended` too: a second suspend would let a second
          // openForgotPasswordModal() push a second stack id.
          if (submitting || settled || suspended) return false;
          suspended = true;
          loginModalEl.setAttribute('hidden', '');
          // kcModalId stays on the stack: the reset modal pushes above it.
          return true;
        },
        resume: function (noticeText) {
          if (settled) return;
          suspended = false;
          loginPassEl.value = '';
          loginErrorEl.textContent = noticeText || '';
          loginModalEl.removeAttribute('hidden');
          // No re-push: kcModalId kept its slot while suspended (see
          // kcModalStack's header comment).
          kcFocusFirstEmpty([loginUserEl, loginPassEl]);
        }
      };
      function onKeydown(evt) {
        // Stacking guard: bail unless this modal is visually on top --
        // covers both "suspended under forgot-password" (below) and a
        // login modal that escalated on top of an already-open confirm
        // dialog, or vice versa.
        if (!kcModalIsTop(kcModalId)) return;
        if (evt.key === 'Escape' || evt.keyCode === 27) {
          if (submitting) return;
          if (suspended) return; // the reset modal owns Escape right now
          // Same outcome as the Cancel button.
          evt.preventDefault();
          if (typeof evt.stopImmediatePropagation === 'function') evt.stopImmediatePropagation();
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
        kcModalPop(kcModalId);
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
        if (ok && typeof window !== 'undefined' && typeof window.dispatchEvent === 'function' &&
            typeof Event !== 'undefined') {
          // Owner ask, 2026-09-27: a page-local loader that fetched once at
          // load time and swallowed a background 401/403 as AuthCancelled
          // (e.g. the dashboard's #profileSelect picker) has no other way to
          // learn a login later succeeded -- this is the single choke point
          // every successful login passes through (401 retry, 403 role
          // elevation, or a direct openLoginModal() call), so dispatching
          // here covers all of them without any caller special-casing which
          // action triggered the login. Not fired on cancel/decline: nothing
          // changed for a page to re-fetch. Guarded: the node-only test
          // harnesses (test_login_auth_wrapper.js, test_forgot_password_modal.js)
          // run this same code against a minimal fake `window`/`document`
          // with no dispatchEvent/Event, same discipline as this file's other
          // `typeof X !== 'undefined'` feature checks.
          window.dispatchEvent(new Event('kc-login'));
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
            loginErrorEl.textContent = window.kcHostRefusalFromText(text) || text || 'Login failed.';
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
  // This modal's own kcModalStack id (module-scope, not per-open, since its
  // listener is wired once and reused -- see forgotListenersWired). Pushed
  // in openForgotPasswordModal, popped in closeForgotModal.
  var forgotModalKcId = null;

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
    if (forgotModalKcId !== null) { kcModalPop(forgotModalKcId); forgotModalKcId = null; }
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
    // Carry over whatever username the operator already typed into the
    // login modal this reset was opened from -- they've already told us
    // who they are once, and it lets the focus-default rule below send
    // focus straight to the TOTP code field instead of making them retype
    // a username they just entered a moment ago.
    forgotUserEl.value = (typeof loginUserEl !== 'undefined' && loginUserEl && loginUserEl.value) || '';
    forgotCodeEl.value = '';
    forgotNewPassEl.value = '';
    forgotNewPass2El.value = '';
    forgotSubmit1El.disabled = false;
    forgotSubmit2El.disabled = false;
    forgotResetToken = null;
    forgotModalEl.removeAttribute('hidden');
    // Never leak an id if this is somehow re-opened while still open.
    if (forgotModalKcId !== null) kcModalPop(forgotModalKcId);
    forgotModalKcId = kcModalPush();
    // Focus default (ROADMAP.md modal-accessibility follow-up): username if
    // empty, else the 6-digit TOTP code field on this reset step.
    kcFocusFirstEmpty([forgotUserEl, forgotCodeEl]);
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
      if (!kcModalIsTop(forgotModalKcId)) return;
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
        if (resp.status === 403) {
          return resp.text().then(function (text) {
            if (gen !== forgotGeneration) return;
            forgotErrorEl.textContent = window.kcHostRefusalFromText(text) || kcForgotStatusMessage(403, null);
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
      // Weak password: refuse here, token still unspent, so the operator just retypes.
      var pwProblem = window.kcResetPasswordProblem(forgotNewPassEl.value, forgotUserEl.value);
      if (pwProblem) {
        forgotErrorEl2.textContent = pwProblem;
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
      var retryToken = token; // restored only when the board refuses a weak password (it does not spend the token then)
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
        if (resp.status === 400) {
          // F2: the firmware checks strength (including the AP SSID / AP password rule the
          // client cannot see) BEFORE spending the token and says so with reason=weak_password.
          return resp.text().then(function (text) {
            if (gen !== forgotGeneration) return;
            var weak = false;
            try { weak = JSON.parse(text).reason === 'weak_password'; } catch (e) { weak = false; }
            if (weak && retryToken) {
              forgotResetToken = retryToken;
              forgotErrorEl2.textContent = 'The board refused that password (too weak, or it matches the Wi-Fi name or password). ' +
                'Choose a different one; wait a few seconds if it says to try again later.';
              return;
            }
            forgotBackToStep1(kcForgotStatusMessage(400, null));
          });
        }
        if (resp.status === 429) {
          return resp.text().then(function (text) {
            if (gen !== forgotGeneration) return;
            // LOW-6: the 429 is refused before the token is looked up, so the token is still
            // unspent -- keep it and stay on step 2 so the user can retry after the backoff.
            if (retryToken) {
              forgotResetToken = retryToken;
              forgotErrorEl2.textContent = kcForgotStatusMessage(429, text);
              return;
            }
            forgotBackToStep1(kcForgotStatusMessage(429, text));
          });
        }
        if (resp.status === 403) {
          return resp.text().then(function (text) {
            if (gen !== forgotGeneration) return;
            forgotBackToStep1(window.kcHostRefusalFromText(text) || kcForgotStatusMessage(403, null));
          });
        }
        forgotBackToStep1(kcForgotStatusMessage(resp.status, null));
      }).catch(function () {
        if (gen !== forgotGeneration) return;
        forgotBackToStep1('Network error.');
      }).then(function () {
        retryToken = null;
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
  // Answering a confirmation is a user gesture too (2026-09-24 second
  // review): the dashboard's Run button asks "Start firing ... now?", then
  // POSTs /api/profile_exec/start, and an operator who took over 3 s on
  // that question must not have the POST refused quietly. This used to
  // need a window.confirm() hook, since a native dialog dispatches no DOM
  // event. kcConfirm()'s themed modal is answered by a real click (or an
  // Enter/Space keydown on its focused button), which kcNoteGesture() above
  // already records -- the modal is not loginModalEl/forgotModalEl -- so no
  // hook is needed, and none may be added: no native dialog is ever raised
  // (tools/check_no_native_dialogs_in_ui.ps1).
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
  // Mirrors GET /api/auth/session's bootstrap_needed (web auth on, no
  // administrator credential configured yet) -- refreshed by every
  // pollSession() tick, same lifetime/reliability as lastKnownRole. Starts
  // false (fail-safe: an unknown state never redirects away from whatever
  // the operator is doing).
  //
  // Bug fixed here (2026-09-28, post d25d5ccf): buildLoginModal()'s popup
  // has only username/password fields, the same POST /api/auth/login the
  // board refuses outright with no credential configured -- it can never
  // succeed while bootstrap_needed is true. Before the page-gate/dashboard-
  // only change, an unauthenticated full navigation to a gated page was
  // redirected server-side to /login?return=..., and THAT page (login_page.html)
  // already knows how to show the bootstrap form instead of the login form.
  // Losing that redirect (replaced by this popup) silently closed off
  // first-run/lockout-recovery bootstrap on every page but '/' and '/login'
  // themselves. ensureAdminLogin() is the single choke point every login
  // prompt (page gate + the fetch wrapper's 401/403 retry) goes through, so
  // fixing it here covers both without touching route_tier_table.h (no
  // route gets looser) or the popup's own DOM.
  var lastKnownBootstrapNeeded = false;
  function authPromptAllowed(userInitiated) {
    if (pendingLogin) return true;
    if (userInitiated) return true;
    return !authPromptDeclined && !kcPageIsDashboard();
  }
  // Sends the operator to the one page that can actually complete
  // first-run/lockout-recovery bootstrap, carrying the page they were on so
  // login_page.html's own loginReturnPath() can send them back after either
  // form succeeds. location.pathname always starts with "/", but can itself
  // start with "//" (http://board//x) -- loginReturnPath() is the one
  // open-redirect guard and rejects that shape back to "/", so nothing is
  // re-validated here. One-shot: a second caller never re-navigates.
  var kcBootstrapRedirected = false;
  function kcRedirectToBootstrap() {
    if (kcBootstrapRedirected) return;
    kcBootstrapRedirected = true;
    var ret = (window.location.pathname || '/') + (window.location.search || '');
    window.location.href = '/login?return=' + encodeURIComponent(ret);
  }
  // First-load race (review fix, 2026-09-28): lastKnownBootstrapNeeded
  // starts false until the first pollSession() answer lands, and a page's
  // own first data fetch usually 401s before that -- so the login-only
  // popup is already open (pendingLogin set) by the time the poll learns
  // bootstrap is needed, and maybeGateThisPageGated() then merely JOINS
  // that dead popup. pollSession() calls this after every refresh of the
  // flag: a login prompt still pending while bootstrap is needed can never
  // succeed, so leave for the bootstrap form instead.
  function kcBootstrapRedirectIfPromptOpen() {
    if (lastKnownBootstrapNeeded && pendingLogin) kcRedirectToBootstrap();
  }
  function ensureAdminLogin(titleText) {
    if (!pendingLogin) {
      if (lastKnownBootstrapNeeded) {
        // The popup cannot bootstrap a credential; redirect instead of
        // opening it. The page is about to navigate away, so this promise
        // is never meant to resolve -- nothing left on this page depends on
        // its outcome.
        kcRedirectToBootstrap();
        pendingLogin = new Promise(function () {});
        return pendingLogin;
      }
      pendingLogin = openLoginModal(titleText).then(function (ok) {
        pendingLogin = null;
        return ok;
      });
    }
    return pendingLogin;
  }

  var hostRefusalShown = false;
  // Maps the firmware's 403 {"error":"cross_origin"|"bad_host"} bodies to one message
  // (http_auth_http.c); null for any other body. Pure; unit-tested.
  window.kcHostRefusalText = function (body) {
    if (body && (body.error === 'cross_origin' || body.error === 'bad_host')) {
      return 'The controller refused this request because of the address it was opened ' +
        'from. Open the controller by its IP address or <name>.local and try again.';
    }
    return null;
  };

  // Same guidance for a refusal body that arrives as raw text (login/bootstrap/forgot/reset
  // use nativeFetch and read text); null when it is not a host/origin refusal.
  window.kcHostRefusalFromText = function (text) {
    try { return window.kcHostRefusalText(JSON.parse(text)); } catch (e) { return null; }
  };

  // Client-side mirror of web_auth_password_check() (persist/web_auth_store.c) for the
  // forgot-password flow: /api/auth/reset spends its one-time token on a request that passes
  // the strength rule, and answers a weak password with the same generic 400 as a bad token,
  // so the page must refuse a weak password BEFORE the POST to keep the token usable (web
  // batch W1). Returns a message, or null when the password passes. The AP SSID/passphrase
  // equality rule is board-side only. Length is UTF-8 bytes, like the firmware's strlen.
  window.kcResetPasswordProblem = function (pw, username) {
    var bytes = (typeof TextEncoder !== 'undefined') ? new TextEncoder().encode(pw).length : pw.length;
    if (bytes < 10) return 'Password too short: use at least 10 characters.';
    if (bytes > 64) return 'Password too long: use at most 64 characters.';
    if (/^[a-z]+$/.test(pw)) return 'Password cannot be all lowercase letters: add a capital, digit or symbol.';
    var low = pw.toLowerCase();
    if (low.indexOf('password') >= 0 || low.indexOf('kiln') >= 0) {
      return 'Password cannot contain "password" or "kiln".';
    }
    if (username && pw === username) return 'Password cannot be the same as the username.';
    return null;
  };

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
    // The AP-password HMAC scheme (X-Ota-Mac, single-use nonce) this used to
    // special-case for kcOtaAuthedFetch was retired 2026-09-29 (WEB_AUTH_PLAN.md
    // item 2b) -- an OTA-family request is now an ordinary ADMIN-tier request
    // with no nonce to go stale, so it takes the same retry-on-403 path as
    // everything else and no longer needs to opt out of it.
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
      // The caller's own timeout (zones Save, 30 s) must not run while the login modal is
      // open, and its original signal may already be aborted: a caller that supplies
      // __kcAuthSignal() gets a freshly armed signal for the retry (dev web review LOW-1).
      if (typeof src.__kcAuthSignal === 'function') retryInit.signal = src.__kcAuthSignal();
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
      if (init && typeof init.__kcOnAuthPrompt === 'function') init.__kcOnAuthPrompt();
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
        // the board only cares about the session cookie, so replaying the
        // same request unchanged is fine. Since the AP-password HMAC scheme
        // was retired 2026-09-29, kcOtaAuthedFetch no longer sets
        // __kcCallerHandlesAuth and an OTA-family request takes this same
        // retry path as everything else.
        // The 428/no-safety-ack path is unaffected either way:
        // kcFetchWithSafetyAck inspects whatever response this wrapper's
        // promise resolves to, retry included.
        return promptThenRetry('Administrator login required');
      }
      // F4/MED-1 refusals (cross_origin, bad_host): a 403 JSON body, no auth-reason
      // header. Tell the operator once what to do instead of a silent failed save.
      if (resp.status === 403 && !alreadyRetried && typeof resp.clone === 'function') {
        try {
          resp.clone().json().then(function (b) {
            var msg = window.kcHostRefusalText(b);
            if (msg && !hostRefusalShown) {
              hostRefusalShown = true;
              window.kcAlert(msg, { title: 'Address not accepted' });
            }
          }, function () {});
        } catch (e) { /* body unreadable: leave the raw 403 to the caller */ }
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
        return window.kcConfirm(msg).then(function (ok) {
          if (!ok) {
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
    });
  };

  // ---- OTA-family fetch -------------------------------------------------
  //
  // Every OTA-family mutating route (push, rollback, recovery-exit,
  // boot_guard_reset, factory_reset, cfgfs_format_confirm, sw_reset) used to
  // also require an AP-password HMAC challenge/response handshake on top of
  // ROUTE_TIER_ADMIN (CommonFW/docs/UPDATE_PROTOCOL.md section 2). That
  // scheme was retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b, owner decision
  // "Retire; open when login off") -- route_tier_table.h's ADMIN tier is now
  // the only gate, the same as every other admin route, so signing a request
  // client-side no longer does anything the session cookie doesn't already
  // do. window.kcOtaAuthedFetch is kept as a thin, named call site (rather
  // than deleting it and inlining kcFetchWithSafetyAck everywhere) so a
  // future auth requirement for this route family has exactly one place to
  // land again, and so ota_page.html/settings_page.html's existing call
  // sites did not all need to change shape in the same pass that retired the
  // handshake.
  window.kcOtaAuthedFetch = function (url, init) {
    return window.kcFetchWithSafetyAck(url, init || {});
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
  // GET /api/status (dashboard_status_http.c, boot_guard_is_recovery_mode());
  // /api/ota/esp/status is ADMIN-tier since 2026-10-09 (route tier review LOW-3).
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
    // server-side enforcement is system_mode_gate.c's recovery-mode rule
    // (docs/SYSTEM_MODE_GATE.md slice 2, formerly recovery_start_
    // refusal.h). Tracked via a
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
    fetch('/api/status')
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
  // Owner request: a button next to Start/Stop to adjust the CURRENTLY
  // RUNNING firing's dwell times/targets/ramp rates, reusing the existing
  // /live_profile page (docs/LIVE_PROFILE_EDIT.md) rather than a new
  // editor. That plan's section 10/decision 4 allows editing in RUNNING,
  // PAUSED, *and* FAULTED alike (a fault freezes heat, not the schedule) --
  // deliberately not DONE, where there is no live run left to adjust.
  var EDITABLE_STATES = { running: true, paused: true, faulted: true };

  var stopBarEl = null;
  var pauseResumeBtnEl = null;
  var ackBtnEl = null;
  var editFiringBtnEl = null;
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
      kcConfirm('Stop this firing now? This aborts the run in progress and cannot be resumed.').then(function (ok) {
        if (!ok) return;
        btn.disabled = true;
        fetch('/api/profile_exec/stop', { method: 'POST' })
          .then(function () { btn.disabled = false; })
          .catch(function () { btn.disabled = false; });
      });
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
      kcConfirm(msg).then(function (ok) {
        if (!ok) return;
        ackBtn.disabled = true;
        fetch('/api/profile_exec/stop', { method: 'POST' })
          .then(function () { ackBtn.disabled = false; })
          .catch(function () { ackBtn.disabled = false; });
      });
    });
    ackBtnEl = ackBtn;

    // "Edit firing" -- next to Start/Stop per the owner's own wording. Opens
    // the existing ADMIN-tier /live_profile editor, but signs in FIRST, in
    // place, before leaving the current page: the click issues the editor's
    // own read-only status GET (/api/profile/live, ADMIN) through the
    // global fetch wrapper, which raises the shared cancelable login modal
    // on a 401/403 and retries once on success. Only then does the tab
    // navigate. Cancel leaves the operator where they were (no navigation,
    // no error text). Deliberately not a bare navigation: once every
    // non-dashboard page shell is gated behind login (owner decision
    // 2026-09-28), an unauthenticated GET /live_profile no longer gets the
    // shell -- and even while the shell is still served open, a Cancel
    // there strands the operator on an empty editor instead of the page
    // they came from. The gate itself stays the firmware's (the same /api
    // tier check the editor's own fetches meet); this only orders "sign
    // in" before "leave the page".
    var editBtn = document.createElement('button');
    editBtn.type = 'button';
    editBtn.className = 'kc-edit-firing-btn';
    editBtn.textContent = 'Edit firing';
    editBtn.setAttribute('hidden', '');
    editBtn.addEventListener('click', function () {
      editBtn.disabled = true;
      fetch('/api/profile/live', { __kcUserAction: true })
        .then(function () {
          window.location.href = '/live_profile';
        })
        .catch(function () {
          // AuthCancelled (the operator declined to sign in) stays quiet;
          // a network failure is already covered by the connection-lost
          // banner, and navigating would only fail the same way.
          editBtn.disabled = false;
        });
    });
    editFiringBtnEl = editBtn;

    el.appendChild(pauseBtn);
    el.appendChild(btn);
    el.appendChild(editBtn);
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

  // Independent of setStopOrAckState()'s Stop/Ack toggle -- EDITABLE_STATES
  // (running/paused/faulted) overlaps both STOPPABLE_STATES and the faulted
  // half of ACK_STATES, so this button can be visible alongside either of
  // the other two, never alone. 'done' hides it (the bar stays up with only
  // the ACKNOWLEDGE button: no live run is left to edit), as does 'idle'.
  // Also hidden on /live_profile itself, where it would only reload the
  // editor and discard any field the operator had not submitted yet.
  function setEditFiringVisible(state) {
    if (!editFiringBtnEl) return;
    if (EDITABLE_STATES[state] && window.location.pathname !== '/live_profile') {
      editFiringBtnEl.removeAttribute('hidden');
    }
    else editFiringBtnEl.setAttribute('hidden', '');
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
        setEditFiringVisible(lastExecState);
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

  // Owner decision, 2026-09-28: an unauthenticated web user may only VIEW
  // the dashboard(s) -- every other page requires login, via the existing
  // cancelable login pop-up, never a separate "authentication required"
  // page. The server still serves every page's static shell unauthenticated
  // (is_page_shell_get() in http_auth_http.c -- harmless, since the shell
  // carries no per-caller data and every real data/action route is gated at
  // the /api/ layer), so this is a client-side proactive gate reusing that
  // same login-modal machinery rather than a new server-rendered page.
  // Tested by test_page_login_gate.js (extracted from the next line through
  // pollSession()).
  // Paths reachable with no login at all: the dashboard itself, /login, and
  // the Wi-Fi provisioning/AP-setup flow's own pages (a device with no
  // credential yet, or on the AP network, must be able to get online before
  // any session can exist).
  var OPEN_WITHOUT_LOGIN_PATHS = {
    '/': true,
    '/login': true,
    '/status': true
  };
  // Owner decision 2026-09-28: /wifi (and its own page's JSON fetches,
  // /scan and /networks, though those are never top-level navigations this
  // pathname check would see) stay open with no login ONLY while the board
  // is unprovisioned -- matching the server-side ROUTE_TIER_WIFI_SETUP gate
  // (route_tier_table.h). Checked separately from OPEN_WITHOUT_LOGIN_PATHS,
  // which is unconditional, since this one depends on live board state read
  // from GET /status (ROUTE_TIER_OPEN, unconditionally reachable, and the
  // one route that already reports wifi_prov_get_state() as its "state"
  // field) rather than the path alone.
  var WIFI_SETUP_PATHS = { '/wifi': true, '/scan': true, '/networks': true };
  var pageGateChecked = false;
  function maybeGateThisPage(role) {
    if (pageGateChecked) return;
    pageGateChecked = true;
    if (OPEN_WITHOUT_LOGIN_PATHS[window.location.pathname]) return;
    if (WIFI_SETUP_PATHS[window.location.pathname]) {
      pageGateChecked = false; // let the async /status read decide, below
      fetch('/status')
        .then(function (r) { return r.ok ? r.json() : null; })
        .then(function (st) {
          if (pageGateChecked) return; // a second poll already resolved this
          pageGateChecked = true;
          if (st && st.state === 'unprovisioned') return; // captive-portal setup: no gate
          maybeGateThisPageGated(role);
        })
        .catch(function () {
          // Could not confirm unprovisioned -- fail closed, same as any
          // other network error here: gate the page rather than assume the
          // open case.
          if (pageGateChecked) return;
          pageGateChecked = true;
          maybeGateThisPageGated(role);
        });
      return;
    }
    maybeGateThisPageGated(role);
  }

  function maybeGateThisPageGated(role) {
    // role === 'admin' covers both a real admin session and section 11's
    // auth-off collapse (role always reports 'admin' when web auth is
    // disabled) -- in either case the page needs no gate. A 'user' session
    // is also allowed through: it's still a real login, and any action the
    // page itself cannot perform is refused by the /api/ tier check as
    // usual.
    if (role === 'admin' || role === 'user') return;
    // The page's own first data fetch has usually already 401'd and raised
    // the modal through the fetch wrapper by the time this poll answers.
    // Join THAT login (ensureAdminLogin()'s pendingLogin singleton) rather
    // than calling openLoginModal() a second time: two concurrent opens of
    // the one shared modal DOM each attach their own submit handler, so a
    // single Log in click would POST /api/auth/login twice (doubling the
    // lockout ladder's failure count on a wrong password). If the operator
    // already declined that modal, go straight back to the dashboard.
    if (authPromptDeclined) {
      window.location.href = '/';
      return;
    }
    ensureAdminLogin('Sign in required').then(function (ok) {
      if (!ok) {
        authPromptDeclined = true;
        window.location.href = '/';
      }
    });
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
        // Refreshed on every tick, independent of maybeGateThisPage()'s
        // once-per-load pageGateChecked latch -- ensureAdminLogin() (the
        // fetch wrapper's 401/403 retry, not only the page gate) needs the
        // current answer whenever it next runs, not just at page load.
        lastKnownBootstrapNeeded = !!(st && st.bootstrap_needed);
        kcBootstrapRedirectIfPromptOpen();
        maybeGateThisPage(role);
        // Transition from a real session to "none" -- the server has
        // actually expired it. Return to the Dashboard, per the plan's
        // wording ("the interface returns to the Dashboard"), not /login.
        if (lastKnownRole && lastKnownRole !== 'none' && role === 'none') {
          // Tell page scripts (e.g. the dashboard aux card) the session is gone
          // so they can drop admin-only controls without waiting for a reload.
          try { window.dispatchEvent(new CustomEvent('kc-logout')); } catch (e) { /* old browser */ }
          if (lockPromptEl) lockPromptEl.setAttribute('hidden', '');
          // Not over a bootstrap redirect already issued above this tick.
          if (window.location.pathname !== '/' && !kcBootstrapRedirected) {
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
