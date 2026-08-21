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
// UI_PLAN.md's route map (section 1) named a `/settings` hub page that did
// not exist when this file was first written -- the menu below listed
// every settings-ish route individually as a stand-in (zones, relays,
// wifi, ota, readiness) with a comment to collapse them once /settings
// landed. It has now landed (settings_page.html, this same "web page
// structure" pass): that page already collects zones/relays/manual/wifi/
// ota/profiles/readiness/diagnostics/safety in one place, so this menu
// links there once instead of repeating the same list in a second spot --
// exactly the "update NAV_LINKS in one place, not two" the old comment
// asked for. Dashboard/profiles/readiness stay direct links since they are
// this app's other most-visited destinations, not settings.
(function () {
  'use strict';

  var NAV_LINKS = [
    { href: '/', label: 'Dashboard' },
    { href: '/profiles', label: 'Firing profiles' },
    { href: '/readiness', label: 'Ready to fire?' },
    { href: '/settings', label: 'Settings' },
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

  function buildTopbar(menuOverlay) {
    var bar = document.createElement('div');
    bar.className = 'kc-topbar';

    var brand = document.createElement('a');
    brand.className = 'kc-brand';
    brand.href = '/';
    brand.textContent = 'kilnCtl';

    var menuBtn = document.createElement('button');
    menuBtn.type = 'button';
    menuBtn.className = 'kc-menu-btn';
    menuBtn.textContent = 'Menu';
    menuBtn.addEventListener('click', function () {
      menuOverlay.removeAttribute('hidden');
    });

    bar.appendChild(brand);
    bar.appendChild(menuBtn);
    // Inserted as the very first child of <body> -- ahead of each page's own
    // theme-toggle button and <h1> -- so it reads as the same top-of-page
    // chrome everywhere, matching UI_PLAN.md's "consistent header" wording.
    document.body.insertBefore(bar, document.body.firstChild);
    return bar;
  }

  function buildBottomNav(menuOverlay) {
    var bar = document.createElement('div');
    bar.className = 'kc-bottom-nav';

    var here = currentPath();
    function navBtn(href, label, isMenu) {
      var el = document.createElement(isMenu ? 'button' : 'a');
      if (isMenu) {
        el.type = 'button';
      } else {
        el.href = href;
      }
      el.className = 'kc-bottom-nav-item';
      if (!isMenu && href === here) {
        el.className += ' kc-bottom-nav-active';
      }
      el.textContent = label;
      if (isMenu) {
        el.addEventListener('click', function () {
          menuOverlay.removeAttribute('hidden');
        });
      }
      return el;
    }

    bar.appendChild(navBtn('/', 'Dashboard', false));
    bar.appendChild(navBtn('/profiles', 'Profiles', false));
    bar.appendChild(navBtn(null, 'Menu', true));

    document.body.appendChild(bar);
    return bar;
  }

  // The fixed bottom nav (and, when a firing is running, app.js's sticky
  // Stop bar stacked above it -- see app.js's KC_BOTTOM_STACK contract)
  // would otherwise cover the last inch of every page's real content. Body
  // padding has to track the *current* stack height, not a guessed
  // constant, because the Stop bar attaches/detaches at runtime as a firing
  // starts/stops. app.js calls kcNav.updateBodyPadding() itself whenever it
  // changes the Stop bar's presence; nav.js also calls it once here so a
  // page with no app.js involvement yet (there is none today, but nothing
  // stops a future page skipping app.js) still gets correct padding for the
  // nav bar alone.
  function updateBodyPadding() {
    var total = 0;
    var bottomNav = document.querySelector('.kc-bottom-nav');
    var stopBar = document.querySelector('.kc-stop-bar');
    if (bottomNav) total += bottomNav.offsetHeight;
    if (stopBar && !stopBar.hasAttribute('hidden')) total += stopBar.offsetHeight;
    document.body.style.paddingBottom = total + 'px';
  }

  function init() {
    var menuOverlay = buildMenuOverlay();
    buildTopbar(menuOverlay);
    buildBottomNav(menuOverlay);
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
