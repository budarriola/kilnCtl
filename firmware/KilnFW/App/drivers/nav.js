// nav.js -- shared navigation chrome for every kilnCtl web page (UI_PLAN.md,
// Web "page structure rework" section 4 item 1: "Shared nav, not six copies").
//
// Loaded via `<script defer src="/nav.js">` from every *_page.html. `defer`
// matters here: each page's own inline <script> (no defer/async) at the
// bottom of <body> runs synchronously the instant the HTML parser reaches
// it, which is BEFORE any deferred script -- deferred scripts run after
// parsing finishes but before DOMContentLoaded. So this file (and app.js,
// loaded the same way) is NOT available to a page's inline script's
// top-level statements. It IS available inside any event-handler callback
// registered by that inline script, because a click can only happen after
// the page has finished loading. Every page in this codebase only touches
// window.kcApp / window.kcConfirm (see app.js) from inside such callbacks,
// so this ordering is safe -- documented here once rather than re-derived
// per page.
//
// Served from GET /nav.js, registered in wifi_provision_http.c alongside
// /theme.css (same module: it owns the one httpd_handle_t every other
// *_http.c file's routes share, so this is reachable in AP-only
// provisioning mode too, not just once the board is on the home network --
// see that file's comment on theme_css_uri for the original reasoning this
// reuses verbatim).
//
// 2026-08-21 rework (owner's "web-GUI rework" spec): the bottom nav bar is
// gone (see buildBottomNav's removal note below) and the menu no longer
// merely points at the /settings hub -- it now carries the hub's own
// "board configuration" link list directly, sourced by hand from
// settings_page.html's <h2>Board configuration</h2> section as it read on
// 2026-08-21 (readiness/zones/relays/manual/profiles/wifi/ota/diagnostics/
// safety, same hrefs and same order that page used), because that section
// is being deleted from settings_page.html in this same pass -- its links
// have to live somewhere, and the drop-down is that somewhere now. Two
// entries were added on top of that list: /diagnostics/thermo (the
// thermo-faults page has its own route, registered in diagnostics_http.c,
// but was never linked from the settings hub at all -- an existing gap,
// not something this pass broke) and a "Reset" item pointing at
// /settings#danger, since /settings itself now shows nothing BUT the
// danger zone (see settings_page.html) and needs a way in from here.
// /settings has no entry of its own anymore for the same reason: the plain
// hub page it used to point at doesn't exist any more, only the danger
// zone does, and that's what "Reset" already reaches.
(function () {
  'use strict';

  var NAV_LINKS = [
    { href: '/readiness', label: 'Ready to fire? (checklist)' },
    { href: '/settings/zones', label: 'Thermocouples & zones' },
    { href: '/settings/relays', label: 'Relays & rules' },
    { href: '/settings/manual', label: 'Manual relay control' },
    { href: '/profiles', label: 'Firing profiles' },
    { href: '/wifi', label: 'Network settings' },
    { href: '/ota', label: 'Firmware update' },
    { href: '/diagnostics', label: 'Diagnostics' },
    { href: '/diagnostics/thermo', label: 'Thermocouple faults' },
    { href: '/safety', label: 'Safety processor' },
    { href: '/settings/backup', label: 'Backup & restore' },
    // 2026-08-21: /settings gained a Display section (theme + °C/°F, moved
    // off the topbar at the owner's request), so it needs a way in that is
    // not the Reset entry below -- an operator looking for the units toggle
    // should not have to guess it lives behind a link labelled "Reset".
    // Both entries point at the same page, each at its own anchor.
    { href: '/settings#display', label: 'Display (theme & units)' },
    { href: '/settings#danger', label: 'Reset' },
  ];

  function currentPath() {
    // Compare by pathname only -- query strings / hashes never appear on
    // these routes today, but stripping them costs nothing and avoids a
    // near-miss on "active" highlighting if one ever does.
    return window.location.pathname;
  }

  function buildMenuOverlay() {
    var overlay = document.createElement('div');
    overlay.className = 'kc-menu-overlay';
    overlay.setAttribute('hidden', '');

    var panel = document.createElement('div');
    panel.className = 'kc-menu-panel';

    var here = currentPath();
    NAV_LINKS.forEach(function (link) {
      var a = document.createElement('a');
      a.href = link.href;
      a.textContent = link.label;
      if (link.href === here) {
        a.className = 'kc-menu-active';
      }
      panel.appendChild(a);
    });

    overlay.appendChild(panel);
    // Tapping the dimmed backdrop (not the panel itself) closes the menu --
    // standard bottom-sheet convention, and cheap: only the overlay element
    // itself needs the listener, not each link.
    overlay.addEventListener('click', function (ev) {
      if (ev.target === overlay) {
        overlay.setAttribute('hidden', '');
      }
    });
    document.body.appendChild(overlay);
    return overlay;
  }

  // Page name for the topbar (item 5 of the rework spec: the brand
  // "kilnCtl" that used to sit in the topbar, with the page name repeated
  // as an <h1> underneath, is replaced by the page name ALONE in the
  // topbar -- the <h1> is deleted from every page). Every *_page.html's
  // <title> is "kilnCtl - X" (verified 2026-08-21 via `grep -n "<title>"
  // *.html` over all twelve pages) with two hand-written exceptions:
  // main_page.html's is bare "kilnCtl" (no page name at all -- there was
  // never a second word to reuse, so this maps it to "Dashboard", matching
  // the Home button below and the old bottom-nav's own "Dashboard" label)
  // and wifi_provision_page.html's is "kilnCtl Wi-Fi Setup" (a space, not
  // "kilnCtl - ", predating the "kilnCtl - X" convention -- this page is
  // also reachable stand-alone during AP-only first-boot provisioning, a
  // different code path than the rest, which likely explains the drift).
  // One regex strips "kilnCtl" and an optional leading " - " / " " in
  // either order, covering both forms without needing a per-page marker
  // element added to twelve files just to name a five-word bar.
  function pageTitle() {
    var t = document.title.replace(/^kilnCtl\s*-?\s*/, '').trim();
    return t || 'Dashboard';
  }

  function buildTopbar(menuOverlay) {
    var bar = document.createElement('div');
    bar.className = 'kc-topbar';

    var title = document.createElement('span');
    title.className = 'kc-page-title';
    title.textContent = pageTitle();

    // Item 4: a Home button next to Menu, both grouped on the right so a
    // thumb reaching for either lands in the same corner regardless of
    // which one it meant to hit -- the topbar no longer has a brand link
    // to double as "go home" (that was the ONLY way home before the bottom
    // nav's own Dashboard button existed; that button was arguably
    // redundant with the brand, and now the brand is gone, this replaces
    // both former "go home" paths with one explicit control).
    var actions = document.createElement('div');
    actions.className = 'kc-topbar-actions';

    // Owner's report (2026-08-21, same day as the rework above): on the
    // dashboard itself the Home button is a no-op -- it navigates to `/`,
    // which is already the page you're looking at -- so it's suppressed
    // there. Reusing pageTitle()'s own "am I the dashboard" signal rather
    // than inventing a second one: pageTitle() already special-cases
    // main_page.html's bare "kilnCtl" <title> (no second word to strip) to
    // the literal string 'Dashboard', specifically so this button and the
    // old bottom-nav's "Dashboard" label would agree (see that function's
    // comment). A URL-path check was considered instead (e.g.
    // window.location.pathname === '/') but pageTitle() is the ONE place
    // that already decides "which page is this" for the topbar -- a path
    // check would be a second, parallel notion of the same fact, and would
    // still have to cover both '/' and any '/index.html'-style alias this
    // server might answer to (not verified either way here) plus guard
    // against matching a sub-path like '/settings/zones'. Comparing
    // against the title string sidesteps all of that.
    var isDashboard = title.textContent === 'Dashboard';

    var menuBtn = document.createElement('button');
    menuBtn.type = 'button';
    menuBtn.className = 'kc-menu-btn';
    menuBtn.textContent = 'Menu';
    menuBtn.addEventListener('click', function () {
      menuOverlay.removeAttribute('hidden');
    });

    // Theme toggle placement, 2026-08-21 (second pass, owner request "move
    // the Fahrenheit/Celsius and theme selection to the settings pages").
    //
    // History, because the two passes look contradictory otherwise: every
    // page's <button id="themeBtn"> markup (real HTML, not script-created)
    // originally stayed wherever the page put it in <body> and relied on
    // `.theme-btn { position: fixed; top/right }` to land in the same corner
    // as this topbar's Home/Menu group -- on a phone-width viewport there
    // wasn't room for both, so the fixed button landed ON TOP of Home/Menu.
    // The first fix reparented it into .kc-topbar-actions so it became a
    // normal flex child that could not overlap anything. This pass goes
    // further: the theme and °C/°F controls are display preferences, not
    // per-page chrome, so they now live on the Settings page only and the
    // topbar carries just Home/Menu.
    //
    // Kept as a MOVE (or a hide), never a removal: each page's own inline
    // <script> looks up #themeBtn by id and attaches the theme listener to
    // it, and would throw on null. So on the one page that offers the
    // control (settings_page.html, which provides the #kcDisplayPrefs
    // container) the real element is moved into that container; everywhere
    // else it stays in the DOM, functional but hidden, and the page script
    // is none the wiser.
    //
    // Ordering: this runs from nav.js, guaranteed to execute before app.js
    // on every page (`defer` scripts run in document order; nav.js's
    // <script> tag is listed first on every *_page.html). app.js's
    // buildUnitBtn() then inserts the °C/°F toggle next to #themeBtn, so it
    // follows wherever this put it. #themeBtn itself is real markup parsed
    // before any deferred script runs, so it already exists here -- no race
    // on that side either.
    var themeBtn = document.getElementById('themeBtn');
    var displayPrefs = document.getElementById('kcDisplayPrefs');
    if (themeBtn) {
      if (displayPrefs) {
        themeBtn.classList.remove('theme-btn');
        themeBtn.classList.add('kc-pref-btn');
        displayPrefs.appendChild(themeBtn);
      } else {
        themeBtn.hidden = true;
      }
    }

    if (!isDashboard) {
      var homeBtn = document.createElement('a');
      homeBtn.href = '/';
      homeBtn.className = 'kc-home-btn';
      homeBtn.textContent = 'Home';
      actions.appendChild(homeBtn);
    }
    actions.appendChild(menuBtn);
    bar.appendChild(title);
    bar.appendChild(actions);
    // Inserted as the very first child of <body> -- ahead of anything left
    // in <body> (on pages that still have a stray <h1> etc; see this pass's
    // page-by-page edits) -- so it reads as the same top-of-page chrome
    // everywhere, matching UI_PLAN.md's "consistent header" wording. The
    // theme/unit toggles are no longer among the things left behind in
    // <body> -- they were just moved into `bar` above.
    document.body.insertBefore(bar, document.body.firstChild);
    return bar;
  }

  // Bottom nav bar deleted outright (2026-08-21 rework spec item 1): its two
  // links (Dashboard, Profiles) are now covered by the topbar's Home button
  // and the drop-down's own "Firing profiles" entry, and "Menu" duplicated
  // the topbar's menu button one-for-one. Nothing replaces this function --
  // it simply no longer exists, and neither does .kc-bottom-nav* in
  // theme.css (see that file's own edit).

  // The fixed Stop bar (app.js, shown only while a firing is running) would
  // otherwise cover the last inch of every page's real content, same as the
  // deleted bottom nav used to. Body padding has to track the Stop bar's
  // *current* height, not a guessed constant, because it attaches/detaches
  // at runtime as a firing starts/stops. app.js calls
  // kcNav.updateBodyPadding() itself whenever it changes the Stop bar's
  // presence; nav.js also calls it once here so a page with no app.js
  // involvement yet (there is none today, but nothing stops a future page
  // skipping app.js) still gets correct padding.
  //
  // Previously this also added the bottom nav's own offsetHeight -- removed
  // along with the bottom nav itself. The Stop bar's CSS `bottom` offset
  // used to be a hardcoded 56px so it would stack visually just above the
  // bottom nav (see theme.css's old .kc-stop-bar comment); with the bottom
  // nav gone, that offset is now 0 (theme.css), and this function's job is
  // unchanged -- it only ever measured the Stop bar's own height for body
  // padding, never the 56px stacking offset, so removing the bottom nav
  // term here is the only change this function needed.
  function updateBodyPadding() {
    var total = 0;
    var stopBar = document.querySelector('.kc-stop-bar');
    if (stopBar && !stopBar.hasAttribute('hidden')) total += stopBar.offsetHeight;
    document.body.style.paddingBottom = total + 'px';
  }

  function init() {
    var menuOverlay = buildMenuOverlay();
    buildTopbar(menuOverlay);
    updateBodyPadding();
    window.addEventListener('resize', updateBodyPadding);
  }

  // window.kcNav is the seam app.js's sticky-Stop bar hooks into (attach/
  // detach the bar, then call updateBodyPadding so content reflows around
  // it) without nav.js and app.js needing to agree on load order beyond
  // "both are deferred, both run before DOMContentLoaded".
  window.kcNav = { updateBodyPadding: updateBodyPadding };

  if (document.body) {
    init();
  } else {
    // Belt-and-suspenders: with `defer`, document.body always exists by the
    // time this runs, but nothing about that guarantee is enforced by the
    // language, only by browsers' shared implementation choice -- fall back
    // to DOMContentLoaded rather than assume.
    document.addEventListener('DOMContentLoaded', init);
  }
})();
