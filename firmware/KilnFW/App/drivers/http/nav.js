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

  // 2026-08-27 rework (owner's four verbatim requests, applied in one pass):
  //
  //   (a) "the board health page should be folded into the diagnostics page.
  //        most of the information is there anyway" -- /board_temps is gone
  //        (diagnostics_http.c/board_temps.c); its content lives in
  //        diagnostics_page.html's existing "Board health" card, which
  //        already read the identical GET /api/board_temps data this menu
  //        item pointed at, so no menu entry for it survives either.
  //   (b) "safty timeings, safty commitioning and safty pages should be sub
  //        menu items under a heading of safty. the menu should just show
  //        them as sub items when the main safty item is opened not a
  //        seprate page to hold the 3 page links" -- an EXPANDING group, not
  //        a landing page: see the `children` array below and
  //        buildMenuOverlay()'s handling of it (a <details>/<summary>
  //        disclosure, expanding in place inside the drop-down itself).
  //   (c) "fireing profiles should be the top menu item" -- /profiles is now
  //        NAV_LINKS[0].
  //   (d) "Thermocouple Faults should also be combined into the diagnostic
  //        page" -- /diagnostics/thermo is gone (diagnostics_http.c); its
  //        content is diagnostics_page.html's new "Thermocouple faults"
  //        section, reading the same GET /api/thermo/faults this menu item
  //        used to send an operator to a second page for.
  //
  // A plain entry is `{ href, label }`, same shape as before. A group entry
  // is `{ label, children: [...] }` -- no href of its own, since (b) is
  // explicit that opening a group must never navigate anywhere, only reveal
  // its children in place. That rule is an owner instruction, not an
  // implementation convenience: it survives the 2026-09-18 reorganisation
  // below unchanged, and now governs three groups instead of one.
  //
  // 2026-09-18 reorganisation (the owner's chosen structure, implemented as
  // given rather than re-derived). The list is no longer a flat run of
  // entries with a single "Safety" group at the end: all fifteen
  // destinations now sit under exactly one of three top-level groups --
  // Firing, Kiln setup, System -- and none of those three navigates
  // anywhere itself. The former "Safety" group is DISSOLVED, its three
  // pages moved into "Kiln setup" at full depth keeping their existing
  // labels, because this menu supports exactly ONE level of nesting: a
  // group nested inside a group has nowhere to render. Every destination
  // that existed before the move still exists after it; nothing was
  // dropped, and no entry is hidden by role or state -- this is a static
  // array and stays one.
  //
  // With every page now nested, buildMenuOverlay()'s auto-expand (a group
  // opens when the current page is one of its children) stops being a
  // nicety and becomes load-bearing: without it the drop-down would open
  // fully collapsed to three words and tell the operator nothing about
  // where they are.
  var NAV_LINKS = [
    {
      label: 'Firing',
      children: [
        // Owner, 2026-08-27: "fireing profiles should be the top menu item".
        // Still the first thing in the menu -- now the first child of the
        // first group, which is the closest the nested structure allows.
        { href: '/profiles', label: 'Firing profiles' },
        // docs/LIVE_PROFILE_EDIT_PLAN.md -- edit the profile actually firing
        // right now, sits right beside the profile catalogue it forks from.
        { href: '/live_profile', label: 'Edit running firing' },
        { href: '/readiness', label: 'Ready to fire? (checklist)' },
        { href: '/diagnostics', label: 'Diagnostics' },
      ],
    },
    {
      label: 'Kiln setup',
      children: [
        { href: '/setup', label: 'Setup wizard' },
        { href: '/settings/zones', label: 'Thermocouples & zones' },
        // Kiln-config selector + management (save/clone/rename/delete/export/
        // import/apply), split out of main_page.html's own dashboard
        // disclosure 2026-09-18 into its own page (kiln_configs_page.html,
        // kiln_cfg_http.c).
        { href: '/settings/kiln_configs', label: 'Kiln configs' },
        // The three entries below are the whole of the former top-level
        // "Safety" group (owner request (b), 2026-08-27), moved here intact
        // 2026-09-18. They keep their labels deliberately: "Safety timings"
        // and "Safety processor" still have to be told apart from each
        // other now that the word "Safety" is no longer supplied by a
        // heading standing above them.
        { href: '/settings/safety', label: 'Safety timings' },
        { href: '/safety', label: 'Safety processor' },
        { href: '/safety/commissioning', label: 'Safety commissioning' },
      ],
    },
    {
      label: 'System',
      children: [
        { href: '/wifi', label: 'Network settings' },
        { href: '/ota', label: 'Firmware update' },
        { href: '/settings/backup', label: 'Backup & restore' },
        // 2026-08-21: /settings gained a Display section (theme + °C/°F,
        // moved off the topbar at the owner's request), so it needs a way
        // in that is not the reset entry below -- an operator looking for
        // the units toggle should not have to guess it lives behind a link
        // labelled "Reset".
        //
        // 2026-08-22: that Display section moved again, this time to its
        // own page (/settings/display, settings_display_page.html) -- owner
        // report: "both display theme and reset menu items take me to the
        // same page." The two entries used to be /settings#display and
        // /settings#danger, two anchors on the ONE settings page, so picking
        // either menu item visibly landed on the same document. They now
        // point at genuinely different routes.
        { href: '/settings/display', label: 'Display (theme & units)' },
        // WEB_AUTH_PLAN.md section 6: the admin password/settings page.
        { href: '/settings/security', label: 'Security (passwords & PINs)' },
        // Relabelled from "Reset" 2026-09-18. /settings carries a Reboot
        // section (settings_page.html's <h2 id="sw-reset">Reboot</h2>) as
        // well as the Danger zone below it, and an entry labelled just
        // "Reset" hid the reboot control behind a word that does not name
        // it. The #danger fragment stays: that is still the part of the
        // page this entry has always been for. See isActive() below for why
        // an href carrying a fragment needs `activeFor` to highlight at all.
        { href: '/settings#danger', label: 'Reboot & reset', activeFor: '/settings' },
      ],
    },
    // Two destinations removed in earlier passes, recorded here so they are
    // not re-added by someone reading the route table and finding a gap.
    // 'Relays & rules' (/settings/relays) removed 2026-08-27: the rule
    // engine was deleted -- relay/IO control is now a firing profile
    // segment (see profile_executor.c's io_seg_* machinery) per the
    // owner's "instead of the relays and rules section I want them to be
    // part of the profile" request.
    // 'Manual relay control' (/settings/manual) removed 2026-08-27: the
    // owner's call once the kiln's heating elements were actually wired to
    // this board -- "the danger zone in the diagnostics page covers it
    // fine." Diagnostics' Danger Zone (diagnostics_page.html, POST
    // /api/diagnostics/danger/relay) is the one sanctioned place left to
    // move a relay by hand; it carries its own explicit accept-the-risk
    // gate and auto-exit timer, which the removed page did not.
  ];

  function currentPath() {
    // Compare by pathname only -- query strings / hashes never appear on
    // these routes today, but stripping them costs nothing and avoids a
    // near-miss on "active" highlighting if one ever does.
    return window.location.pathname;
  }

  // Is this entry the page we are standing on? An entry normally matches on
  // its own href, compared exactly. `activeFor` is an explicit per-entry
  // opt-in override, and it exists for one reason: an href carrying a
  // fragment can never equal a pathname. '/settings#danger' is compared
  // against window.location.pathname, which on that page is '/settings' --
  // so until 2026-09-18 the "Reboot & reset" entry was the one item in this
  // menu that could never highlight as active, no matter where you stood.
  //
  // Deliberately an EXACT comparison against a field the entry opts into,
  // and deliberately NOT a prefix match on href. A prefix match would close
  // that one false negative by opening six false positives: '/settings' is
  // a prefix of '/settings/zones', '/settings/kiln_configs',
  // '/settings/safety', '/settings/backup', '/settings/display' and
  // '/settings/security', every one of which is its own entry in this menu,
  // so standing on any of them would light up "Reboot & reset" as well --
  // strictly worse than the bug being fixed. With an exact comparison those
  // six match only their own hrefs, and '/settings' matches only the one
  // entry that names it. Exactly one entry carries `activeFor` today.
  function isActive(entry, here) {
    return (entry.activeFor || entry.href) === here;
  }

  function buildMenuOverlay() {
    var overlay = document.createElement('div');
    overlay.className = 'kc-menu-overlay';
    overlay.setAttribute('hidden', '');

    var panel = document.createElement('div');
    panel.className = 'kc-menu-panel';

    var here = currentPath();
    NAV_LINKS.forEach(function (link) {
      if (link.children) {
        // Expanding group (owner's (b): "the menu should just show them as
        // sub items when the main safty item is opened not a seprate page
        // to hold the 3 page links"). <details>/<summary> is the native
        // disclosure widget -- no click-handler JS needed to expand/collapse
        // it, no new page, and it never navigates on its own: exactly the
        // "reveal in place" shape the request asks for, and it costs no
        // extra flash-budget asset (web_encoding.h) the way a hand-rolled
        // JS toggle plus icon would.
        var details = document.createElement('details');
        details.className = 'kc-menu-group';
        // Open by default when the current page is one of this group's
        // children, so landing on e.g. /safety/commissioning from a
        // bookmark or a refresh shows the group already expanded around the
        // active item instead of hiding it behind a collapsed heading.
        //
        // Since the 2026-09-18 reorganisation every page is a child of some
        // group, which makes this the only thing standing between the
        // operator and a menu that opens as three collapsed words. It is
        // load-bearing behaviour now, not a nicety -- do not drop it.
        var hasActiveChild = link.children.some(function (c) { return isActive(c, here); });
        if (hasActiveChild) {
          details.setAttribute('open', '');
        }
        var summary = document.createElement('summary');
        summary.textContent = link.label;
        details.appendChild(summary);
        link.children.forEach(function (child) {
          var a = document.createElement('a');
          a.href = child.href;
          a.textContent = child.label;
          if (isActive(child, here)) {
            a.className = 'kc-menu-active';
          }
          details.appendChild(a);
        });
        panel.appendChild(details);
        return;
      }
      var a = document.createElement('a');
      a.href = link.href;
      a.textContent = link.label;
      if (isActive(link, here)) {
        a.className = 'kc-menu-active';
      }
      panel.appendChild(a);
    });

    overlay.appendChild(panel);
    // Tapping the dimmed backdrop (not the panel itself) closes the menu --
    // standard bottom-sheet convention, and cheap: only the overlay element
    // itself needs the listener, not each link.
    //
    // 2026-08-22: the panel's own click handler below is NOT redundant with
    // this. Owner report: "clicking the reset menu button leaves the menu
    // open and does not allow the page to be seen." Every other entry
    // navigates to a different document, which tears the overlay down as a
    // side effect of the page unloading -- so nothing ever had to close it
    // explicitly. "Reset" points at /settings#danger, and from /settings
    // that is a SAME-PAGE fragment jump: no navigation, no unload, so the
    // overlay just sat there covering the anchor it had scrolled to.
    // Closing on any link click fixes it for that entry and for the
    // "Display (theme & units)" entry, which has the same shape, without
    // depending on which page you happen to be standing on.
    panel.addEventListener('click', function (ev) {
      if (ev.target && ev.target.tagName === 'A') {
        overlay.setAttribute('hidden', '');
      }
    });
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

    /* 2026-08-22 owner request: "use a gear for menu and a home icon for
       home". Unicode glyphs, not an icon font or SVG sprite: these pages are
       served from flash on an ESP32 and every byte of asset is a byte of
       flash budget (see web_encoding.h), so a two-character label beats a
       new asset for two icons. The text label stays alongside the glyph
       rather than being replaced by it -- an unlabelled gear is guessable,
       but the label costs nothing here and the topbar has room at phone
       width (the title ellipsises first; see .kc-page-title's min-width: 0
       note in theme.css). aria-label carries the same word for a screen
       reader, which a bare glyph would not. */
    var menuBtn = document.createElement('button');
    menuBtn.type = 'button';
    menuBtn.className = 'kc-menu-btn';
    menuBtn.setAttribute('aria-label', 'Menu');
    menuBtn.textContent = '⚙ Menu';
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
    // control (settings_display_page.html, which provides the
    // #kcDisplayPrefs container -- moved there from settings_page.html
    // 2026-08-22, see that file's own header comment) the real element is
    // moved into that container; everywhere else it stays in the DOM,
    // functional but hidden, and the page script is none the wiser.
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
      homeBtn.setAttribute('aria-label', 'Home');
      homeBtn.textContent = '⌂ Home';
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
