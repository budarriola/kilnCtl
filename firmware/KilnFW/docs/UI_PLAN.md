# UI_PLAN.md — LCD + web usability/cleanup plan

**Verification: build-clean only (`idf.py -C firmware/KilnFW build`, `-Werror`),
not visually confirmed on hardware or a real phone browser.** The 2026-08-19
implementation pass below DOES touch `.c`/`.h`/`.html`/`.css` (all 10 section-3
work-queue items) -- the original "docs/planning only" line below described
the state before that pass and is no longer accurate; kept struck through
rather than deleted, per this doc's own honesty-header convention.

~~Docs/planning only. Nothing in this pass touches `.c`/`.h`/`.html`/`.css`.~~
Driven by the explicit user requirement: "the user interface is in need of
usability and cleanup fixes... the webpage should be optimized for a phone
or tablet. and the lcd should not require scrolling."

## Honesty header

- **Verified**: the six web pages (`main_page.html`, `rules_page.html`,
  `profiles_page.html`, `readiness_page.html`, `zones_page.html`,
  `wifi_provision_page.html`) each carry `<meta name="viewport"
  content="width=device-width, initial-scale=1">` and a `max-width`-bounded
  centered body — read directly, not inferred. `theme.css`'s `--ui-touch:
  72px` variable and `button { min-height: var(--ui-touch); }` rules were
  read directly in `rules_page.html`; the LCD's `UI_THEME_MIN_TOUCH_TARGET_PX
  72px` / `UI_THEME_PADDING_PX 8px` / `UI_THEME_STATUS_BAR_HEIGHT_PX 32px`
  constants were read directly from `ui_theme.h`. The 2026-08-19 back-button
  freeze fix and the mutex-ordering fix in `ui_page_network.c` were read
  directly in that file's header comment.
- **Computed** (arithmetic against the constants above, not measured on a
  physical panel): every LCD page's "content height vs ~264px budget" row
  below is the same style of arithmetic `ui_page_home.c`'s and
  `ui_page_network.c`'s own header comments already do — reused here, not
  re-derived independently line-by-line for every page. `ROADMAP.md`'s M6
  entry and each page's own header comment are the source for these numbers;
  where a page's comment gives a number (network ~268px, temperature
  "depends on runtime relay count"), that number is cited, not recomputed.
- **Guessed / not verified**: nothing about actual rendered appearance on
  the ILI9488 panel (colors, exact pixel fit, touch responsiveness) — no
  hardware visual check happened for this plan. Web page rendering on an
  actual phone/tablet browser is also unverified — see the bench-limitation
  note in the work queue.

## 1. LCD audit

Budget: ~264px of content height under the 480x320-landscape status bar,
per `ui_page_home.c`'s header-comment derivation (320 − 2×`UI_THEME_PADDING_PX`
(8px) − `UI_THEME_STATUS_BAR_HEIGHT_PX` (32px) − bar-to-content gap ≈ 264px),
reused by every other page's own comments rather than re-derived per file.

| Page | Computed content height vs ~264px | Scroll-rule status | Known usability issues |
|---|---|---|---|
| `ui_page_home.c` | Fits — zones (compact rows, pad trimmed to `UI_THEME_PADDING_PX/4`) + run-state summary + Start/Stop/Menu, deliberately slimmed by moving the AP-QR card, profile picker, safety card, and chart off-page (2026-08-18 rewrite, see file header + `ROADMAP.md` M6). | Compliant — `LV_OBJ_FLAG_SCROLLABLE` explicitly cleared. | Multiple nav hops now needed to reach safety/history/network from home (see "navigation depth" below); zone name still shows generic "Zone N" (no name getter exposed by `zones_http.h`). |
| `ui_page_config.c` | Fits — nav hub, switched to a compact 2-column grid to hold 7 destinations (grew from 4) inside budget. | Compliant. | 7 destinations in a 2-col grid on a 480px-wide panel means small grid cells; worth checking cell size against `UI_THEME_MIN_TOUCH_TARGET_PX` (72px) once on hardware — the file's own comment doesn't cite a per-cell px number. |
| `ui_page_temperature.c` | **At risk, not bounded at compile time.** Card layout (relay buttons shrunk 72px→36px, `ui_theme_apply_touch_area(..., true)` restores effective hit area toward 72px) fits the common case, but height scales with `zones_config_get_relay_mask()` — a zone with several relays wraps to a second button row, growing past the single-row estimate. Named explicitly in the file's own header comment and `ROADMAP.md` M6 as the one *not* silently claimed solved. | At risk of violating the no-scroll rule for high-relay-count configs — the file does not currently cap relays-per-zone or fall back to an internally-scrollable list when the wrap would overflow. | Touch targets shrunk to 36px drawn height (relies entirely on the invisible extended click area to stay usable — a visual affordance a user can't see is a real usability smell, not just a hitbox technicality). |
| `ui_page_network.c` | **~268px against ~264px, computed not measured** — over budget on paper per the file's own 2026-08-18 follow-up comment and `ROADMAP.md` M6. Back button was moved into the fixed title bar specifically because content's own arithmetic never had margin (2026-08-19 fix), which brought content's own worst case down to ~228px, but the *page total* including the bar is still the ~268px figure on record. | Non-compliant on paper; internal `lv_list` scroll for Scan/Saved lists is sanctioned and intentional, not the violation — the violation risk is the page-level worst-case sum, not list-internal scrolling. | Scan/Saved toggle is a real usability improvement (mutually exclusive, avoids double-stacking) but adds a navigation step to reach the Saved list from Scan's default view; mode toggle and Scan button shrunk to 44px (below the 72px minimum, same "invisible extended hit area" pattern as temperature's relay buttons). |
| `ui_page_safety.c` | Fits — "Last trip" line was deliberately chosen over the fuller DIAG detail specifically because it was "the only field that fit the page's documented ~264px no-scroll budget" (own comment, also cited in `TODO.md`). | Compliant. | DIAG's warn/trip masks and context-health counters are HTTP-exposed but deliberately left off the LCD — an information-priority tradeoff worth revisiting once the page has more room (larger panel, or a details-on-tap affordance) rather than a bug, but flagged here since "usability" review should note it. |
| `ui_page_board_health.c` | Fits — pad_all trimmed from 8px, own comment cites the ~264px budget directly. | Compliant. | Not reviewed further; no flagged gap in its own comments. |
| `ui_page_history.c` | Fits — own comment states "leave a comfortable margin against ~264px." | Compliant. | Chart-only page moved off `ui_page_home.c`; adds one more nav hop from home (config hub → history) versus its old inline position on the home page. |
| `ui_page_touch_cal.c` / `ui_page_touch_test.c` | Not budget-annotated in either file — these are calibration/diagnostic utility pages, not part of the normal operator flow, and don't carry the ~264px comment other pages do. | Compliant (both clear `LV_OBJ_FLAG_SCROLLABLE`; see LCD work-queue item 4). | `ui_page_touch_cal.c`'s cancel/back path (LCD work-queue item 6, 2026-08-19) is now built. |
| `ui_page_diagnostics.c` (new, 2026-08-20) | Fits — 7 stat rows in a fixed-height (180px), internally scrollable list, same sanctioned pattern as `ui_page_config.c`'s nav grid. | Compliant — outer containers clear `LV_OBJ_FLAG_SCROLLABLE`; the inner list is the sanctioned internally-scrollable exception, not page-level scroll. | None flagged — new page, TODO.md's "Diagnostics / System info page" ESP-only half. |
| `ui_page_thermo_faults.c` (new, 2026-08-20, explicit user request) | Fits — 3 fixed-height (`UI_THEME_MIN_TOUCH_TARGET_PX`, 72px) channel rows in a fixed-height (180px), internally scrollable list, same pattern as `ui_page_diagnostics.c`. Worst case (all 8 fault bits set) reasoned to wrap to ≤2 text lines within the 72px row — not measured on real font metrics/hardware, see the page's own header comment. | Compliant on paper, same caveat as every other page this session: not visually confirmed on the physical panel. | Per-channel MAX31856 fault status (`fault_status` SR bits: OPEN/OVUV/TCLOW/TCHIGH/CJLOW/CJHIGH/TCRANGE/CJRANGE), `~FAULT` pin state, SPI-transfer health — split into its own page from `ui_page_diagnostics.c` per explicit user request ("diagnostics should be broken up into multiple pages"). No "clear faults" action — deliberately out of scope, visibility only. |

**Two known-at-risk pages, concrete proposed fixes:**

1. **`ui_page_temperature.c` (relay-count-dependent fit).** Proposed fix:
   cap the visible relay-button grid at a fixed row count (e.g. 2 rows ×
   however many fit `UI_THEME_MIN_TOUCH_TARGET_PX`-scaled buttons per zone
   card) and convert the per-zone relay row into a small internally-scrollable
   `lv_obj` (same sanctioned pattern as `ui_page_network.c`'s `s_scan_list`/
   `s_saved_list`) once a zone's relay count exceeds that cap, rather than
   letting `LV_FLEX_FLOW_ROW_WRAP` grow the card's drawn height unbounded.
   This makes the page's worst case boundable at compile time instead of
   config-dependent.
2. **`ui_page_network.c` (~268px vs ~264px).** Proposed fix: shrink the
   status card at the top (currently full-width `status_label` +
   `detail_label` row, ~40px per the file's own arithmetic) by combining the
   mode name and detail (RSSI/client count) onto a single label line instead
   of two, saving one label's line height (~20px, montserrat_14 default);
   combined with the existing ~228px content-only figure (post the 2026-08-19
   Back-button move), this should bring the full page (bar + content) back
   under 264px without touching the Scan/Saved/QR behavior that was just
   fixed. Verify with the same "sum the constants" arithmetic this file's
   own comments already use before flashing, then confirm on hardware since
   this file already has two rounds of "computed not measured" debt.

## 2. Web audit

All six pages share `theme.css` (served once at `GET /theme.css`, `TODO.md`
10.6/10.6a) for the `--ui-*` palette and `.theme-btn`, but each page keeps
its own `<style>` block for everything else (`--bg`/`--fg`/light-dark
variable mapping, layout, tables, buttons) — confirmed duplicated per the
`theme.css` header comment ("Only what was byte-identical across all six
pages' old inline `<style>` blocks was pulled out").

| Page | Viewport meta | Layout | Touch targets | Phone-width (~390px) risk |
|---|---|---|---|---|
| `main_page.html` | Present | `max-width: 480px`, centered, responsive | `button { min-height: var(--ui-touch) }` (72px) present | Low risk — narrow max-width already phone-scaled. |
| `wifi_provision_page.html` | Present | `max-width: 420px` | Uses shared `.theme-btn`; page-specific buttons not fully inspected this pass | Low risk — narrowest max-width of all six pages. |
| `profiles_page.html` | Present | `max-width: 560px` | `#cycleTable` present, per-relay cycle table — no `overflow-x: auto` wrapper found around it | **Medium risk**: a multi-column table inside a 560px container on a 390px viewport can force horizontal page scroll if column count/content is wide, since nothing constrains the table itself, only the page body. |
| `readiness_page.html` | Present | `max-width: 560px` | Not fully inspected this pass | Not fully assessed — same `max-width: 560px` family as profiles/rules, recommend the same table check. |
| `rules_page.html` | Present | `max-width: 560px` | `button { min-height: var(--ui-touch) }` present; `.code`/pre block has `overflow-x: auto` (good — scrolls internally rather than pushing page width) | Low-medium — the `.code` block is already handled correctly; the plain `#cycleTable` (Relay/Cycles) is not wrapped the same way `.code` is. |
| `zones_page.html` | Present | `max-width: 480px` | Coupling matrix (`table.coupling`) rendered dynamically via JS (`buildCouplingTable()`-style code), `font-size: 0.85em`, no `overflow-x` wrapper found around `#rgaMatrix` | **Medium-high risk**: an N×N coupling matrix (one row/col per zone) is the classic "breaks on a phone" table shape — small font mitigates but does not bound the risk; more than ~4 zones will likely force overflow on a 390px screen. |

**Phone/tablet-first remediation plan** (shared strategy in `theme.css`
rather than per-page hacks, since the plan is meant to avoid the
duplication the 10.6/10.6a pass already flagged as real):

1. Add one shared rule to `theme.css` — a `.table-scroll` wrapper class
   (`overflow-x: auto; -webkit-overflow-scrolling: touch;`) — and wrap each
   page's data table (`#cycleTable` in `rules_page.html`/`profiles_page.html`,
   `#rgaMatrix`'s generated `<table class="coupling">` in `zones_page.html`)
   in a `<div class="table-scroll">`. One class, defined once, applied at
   three call sites — the antithesis of the six-copies-of-one-`<style>`-block
   problem `theme.css`'s own header comment already documents as the mistake
   to avoid repeating.
2. Audit `wifi_provision_page.html` and `readiness_page.html`'s own button
   markup for the same `min-height: var(--ui-touch)` rule the other four
   pages use — if either defines page-local buttons outside that shared
   rule, promote the rule itself into `theme.css` (it is presently
   `rules_page.html`/`main_page.html`-local per what this pass actually
   read) so every page gets 72px touch targets from one shared declaration
   instead of six independent copies of the same number.
3. Flash-size implication: `App/drivers/CMakeLists.txt`'s gzip step
   (`TODO.md` 10.6a) compresses every embedded page and `theme.css` at build
   time before `EMBED_TXTFILES` packs them in, so a few added CSS rules in
   one shared file cost close to nothing in flash after gzip — no need to
   ration bytes here the way un-gzipped embedded assets would require.

## 3. Prioritized work queue

Each item is sized for one agent pass. "Verifiable" notes reflect the
current bench state: the ILI9488 **is** attached and flashable via
JTAG/OpenOCD, so LCD items get a real visual check; the PC↔ESP UART is
currently dead, so Wi-Fi/network status is unobservable live — web items
are build+inspection-verified only until that's fixed, noted per item.

### LCD

1. **DONE (2026-08-19).** Fixed `ui_page_network.c`'s ~268px overrun:
   `s_status_label`/`s_detail_label` (two label rows) combined into one
   `s_status_label` line (`"%s -- %s"` of the wifi-status text and the
   RSSI/client-count detail, built in `refresh_cb()`'s `combined_buf`).
   Re-summed the arithmetic in a new header-comment paragraph ("2026-08-19
   budget-fix pass"): status_card ~40px -> ~20px, bringing the page's
   content-only worst case from ~228px to ~207px against the ~264px budget.
   Build-clean only — NOT flashed/visually confirmed (no ILI9488 panel
   attached to this environment this pass).
2. **DONE (2026-08-19).** Bounded `ui_page_temperature.c`'s relay-count
   risk: `relay_row` changed from `LV_SIZE_CONTENT` (unbounded growth) to a
   fixed `UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX` (80px, room for 2 rows of
   the 36px buttons) and left internally scrollable (`LV_DIR_VER`,
   `LV_SCROLLBAR_MODE_AUTO`) — same sanctioned pattern as
   `ui_page_network.c`'s `s_scan_list`/`s_saved_list` (fixed height,
   `LV_OBJ_FLAG_SCROLLABLE` left set, not cleared). Each zone card's
   worst-case height is now fixed at compile time regardless of runtime
   relay count. Build-clean only — the bench's actual relay-per-zone count
   wasn't exercised against the >2-row case this pass (no hardware access).
3. **DONE (2026-08-19).** Added a visual affordance (1px border,
   `UI_THEME_COLOR_TEXT_SECONDARY` at `LV_OPA_40`, no theme-color change) to
   every shrunk-target button this doc named: `ui_page_network.c`'s
   `s_mode_home_btn`/`s_mode_ap_btn` (44px), `s_scan_toggle_btn`/
   `s_saved_toggle_btn` (36px), `s_scan_btn` (44px), and
   `ui_page_temperature.c`'s per-relay toggle buttons (36px). Purely visual;
   `ui_theme_apply_touch_area()`'s invisible hit-area extension is
   unchanged. Build-clean only — not visually confirmed on hardware.
4. **DONE (2026-08-19).** Audited `ui_page_touch_cal.c`/
   `ui_page_touch_test.c`: both already clear `LV_OBJ_FLAG_SCROLLABLE` on
   `scr`, and neither is sized against the fixed ~264px budget the "main"
   pages use — `ui_page_touch_cal.c` places every element from the real
   `lv_display_get_*_resolution()` at build time (`compute_targets()`);
   `ui_page_touch_test.c` derives `s_canvas_h` directly from the real
   resolution minus the status bar/button row/padding, so content sums to
   exactly the available height by construction. Both were previously
   unreviewed and un-commented per this doc's own finding; added a header
   comment to each documenting this check (no code fix needed — neither was
   actually broken). Build-clean only.
5. **DONE (2026-08-19).** Checked `ui_page_config.c`'s grid cells against
   `UI_THEME_MIN_TOUCH_TARGET_PX` (72px), reading `ui_theme.c`'s real
   extension code rather than assuming: width was already fine (~223px,
   `lv_pct(48)` of the ~464px grid), but the old 44px-tall cells only got
   `compact_layout=true`'s small capped extension (`UI_THEME_PADDING_PX/2` =
   4px/side, NOT the "extend to 72px" behavior that's `compact_layout=false`
   -only), so effective height was ~52px — genuinely short, and raising
   every cell's drawn height enough to close that gap would have blown the
   ~264px budget. Fixed by making `grid` a fixed-height (180px), internally
   scrollable container (same pattern as items 2/3 above) holding real
   72px-tall cells, rather than shrinking cells to fit all 8 on-screen at
   once. Build-clean only.
6. **DONE (2026-08-19).** Added a "Cancel" button to `ui_page_touch_cal.c`:
   a 64x28px `lv_button_create()` in the top-left status-bar strip
   (`compute_targets()` already reserves `UI_THEME_STATUS_BAR_HEIGHT_PX`
   there before placing any grid target, so this can't overlap a
   calibration point or shrink the usable grid area), created *after*
   `overlay` so it wins z-order hit-testing over it, extended toward
   `UI_THEME_MIN_TOUCH_TARGET_PX` via `ui_theme_apply_touch_area(...,
   false)` same as other shrunk buttons this doc already names. Calls
   `kiln_ui_show("config")` without ever calling `touch_cal_store_save()`
   — a cancelled run never persists a partial/garbage calibration.
   **Deliberately hidden and non-clickable on a forced first-run boot**
   (`touch_cal_store_is_calibrated() == false`, decided fresh in
   `on_screen_loaded()` every time the page is shown): `kiln_ui.c`'s own
   comment on why an uncalibrated board boots straight into this page
   applies equally here — cancelling to "config" would strand the user on
   a screen whose other buttons don't reliably work yet, since no working
   touch mapping exists. Cancel only appears for a deliberate
   re-calibration from the config nav hub, where a working calibration
   (and therefore a working "config" screen to land on) already exists.
   Build-clean (`idf.py -C firmware/KilnFW build` / ninja on the existing
   configured build dir) — not flashed/visually confirmed, no ILI9488
   panel attached in this environment this pass.

### Web

1. **DONE (2026-08-19).** Added `.table-scroll` (`overflow-x: auto;
   -webkit-overflow-scrolling: touch;`) to `theme.css`. Wrapped
   `rules_page.html`'s `#cycleTable` in it. `zones_page.html`'s two
   generated tables (`#couplingMatrix` and `#rgaMatrix`, built by JS that
   replaces each div's `innerHTML` with a `<table class="coupling">`) got
   the wrapper on their static container divs, which picks up the scroll
   behavior regardless of how the JS renders. **Correction to this doc's own
   audit**: `profiles_page.html` has no `#cycleTable` or any `<table>`
   element at all (checked directly, grep confirms zero matches) — the
   audit's claim that it has one was wrong; nothing to wrap there. Verified
   by inspection/build only (UART dead, board not reachable live).
2. **DONE (2026-08-19).** Added `button { min-height: var(--ui-touch); }`
   to `theme.css`. Removed the now-redundant `min-height: var(--ui-touch)`
   from `profiles_page.html`, `zones_page.html`, `rules_page.html`,
   `wifi_provision_page.html`'s plain `button {}` rules and from
   `main_page.html`'s `button.relay-toggle`/`button.run-btn`/
   `button.danger-btn` rules (`main_page.html`'s `a.button` keeps its own —
   it's an `<a>`, not a `<button>`, so the shared bare-tag rule doesn't
   match it). Deliberate overrides (`button.small { min-height: 0; }`,
   `.forget-btn`, `.mode-toggle button { min-height: 2.6em; }`) were left
   alone. `readiness_page.html` has no real `<button>` needing this (only
   `.theme-btn`, a small icon toggle) — nothing to change there. Verified by
   inspection/build only.
3. **NOT DONE this pass.** `wifi_provision_page.html`'s AP-mode QR/form flow
   on an actual phone viewport still needs a live check — this item was
   explicitly scoped as inspection-only pending the bench UART fix in the
   original queue, and that dependency hasn't changed; no code change was
   made or needed for this item specifically (its CSS was already
   `max-width: 420px`-responsive per the original audit).
4. **DONE (2026-08-19).** Decided in favor of "scroll wrapper alone is
   sufficient," documented in a new comment above `zones_page.html`'s
   `table.coupling` CSS rule: `MAX31856_CHANNEL_COUNT` is 3, so this
   board physically never has more than 3 thermocouple zones — the
   coupling matrix is at most 4x4 (header row/col + up to 3 zones),
   nowhere near the ">4-zone cramped" case the original audit worried
   about in the abstract. No responsive collapse was built; it would
   solve a problem this hardware configuration can't produce.
5. **DONE (2026-08-19).** Swept all six pages for fixed-pixel-width
   elements beyond the original audit: found `main_page.html`'s
   `<canvas id="historyChart" width="600" height="220">` (HTML attribute)
   and `wifi_provision_page.html`'s `<canvas id="apQrCanvas" width="200"
   height="200">`. Neither needed a fix: `#historyChart` already has a CSS
   `width: 100%` rule that overrides the HTML attribute for layout, and its
   own JS (`drawHistoryChart()`) re-derives the actual backing-buffer size
   from `canvas.clientWidth` at draw time (DPR-aware) — already responsive.
   `#apQrCanvas`'s 200px is well inside `wifi_provision_page.html`'s 420px
   `max-width` container and is a deliberately fixed physical QR-code
   resolution, not a layout-overflow risk. No other fixed-width elements
   found. Verified by inspection/build only.
6. **DONE (2026-08-20), explicit user request** ("on the web gui i want the
   ability to choose between dhcp and static ip for the home network
   connection. keep the lcd network settings page simple" — the second
   sentence is why this is web-only, `ui_page_network.c` was deliberately
   left untouched). New "IP Address" section on `wifi_provision_page.html`
   under Networks: DHCP/Static toggle buttons + three text inputs (ip/
   netmask/gateway), shown only for Static, prefilled from `GET /status`'s
   new `ip_mode`/`static_ip`/`static_netmask`/`static_gateway` fields,
   submitted to a new `POST /ip_config` (`wifi_provision_http.c`). Backend
   in `wifi_prov.c`/`.h`: new `wifi_prov_ip_mode_t`, persisted to the
   existing `wifi_cfg` NVS namespace, applied via
   `esp_netif_dhcpc_stop()`/`esp_netif_set_ip_info()` inside
   `apply_sta_config()` (the one function every STA join path already
   funnels through) before `esp_wifi_connect()`. Uses the same theme.css
   classes/touch-target sizing every other page here already follows — no
   new design language introduced. **Known gap, flagged by the build
   itself, not yet fixed**: a wrong-but-parseable static IP (bad gateway/
   subnet) still reaches `WIFI_PROV_STATE_CONNECTED` at the L2 layer, so
   `ap_fallback_timer`'s normal DHCP-timeout-triggered recovery does not
   self-heal it — an operator can still switch back to DHCP/AP mode via
   the same API, or power-cycle, so this isn't a full lockout, but it is a
   real reachability edge case worth a closer look before this feature is
   trusted on a remote/unattended kiln. Build-clean (`ninja`), flashed via
   OpenOCD/JTAG, confirmed booting clean on the bench board — the actual
   DHCP/static toggle behavior has NOT been exercised against a real
   router this pass (no live network to test against in this environment).

### LCD — planned: profile creation page (2026-08-19, not started)

New `ui_page_profile_edit.c` (name tentative), reachable from
`ui_page_config.c`'s nav hub grid (item 5's fixed-height scrollable grid has
room for one more 72px cell). Scope, planning-only — no code written yet:

1. **Graph view.** Reuse `ui_page_history.c`'s chart widget/drawing approach
   (already on the nav hub, budget-fit) to render the profile curve
   (time on X, temp on Y) built from the point list below, redrawn on every
   point edit.
2. **Point selection.** Left/Right buttons (existing 72px touch-target
   pattern) step a "selected point" cursor across the profile's point list;
   selected point highlighted on the graph (marker color/size change, no
   new widget type needed).
3. **Per-point fields**, edited via +/- stepper buttons (same pattern as
   existing numeric-adjust UI elsewhere) once a point is selected:
   - Hold temp (°F/°C per existing unit setting)
   - Hold time (minutes)
   - Ramp rate to reach this point from the previous one (°/hr)
4. **Persistence.** Save writes through the existing profile storage path
   (`profiles_save`/`profiles_get` MCP tools imply an existing profile
   store/schema on the firmware side — reuse it rather than inventing a
   parallel format; needs confirming against the actual struct before
   implementation starts).
5. **Budget risk, flagged up front:** graph + point list + 3 stepper fields +
   Left/Right nav + Save/Back is a lot for the ~264px no-scroll budget in one
   screen. Likely needs the graph and the point-editor to be two sub-views
   (tab or Left/Right-reachable panes) rather than one stacked layout —
   decide during implementation, not guessed here.
6. **Not yet done:** no `.c`/`.h` file created, no nav-hub entry wired, no
   estimate of actual pixel heights against budget. This entry exists so the
   feature isn't lost, not as a claim of progress.

### Web — planned: settings import/export and profile import/export (2026-08-19, not started)

Two separate import/export features, kept independent (different data,
different failure modes if merged into one blob):

1. **Settings import/export.** Covers board/system config — Wi-Fi networks
   (`wifi_add_network`/`wifi_get_networks` imply an existing store), zone
   PID/model config (`control_set_zone_pid`/`control_set_zone_model`),
   thermocouple channel config, safety thresholds. Export as a downloadable
   JSON file from a new control on `rules_page.html` or a new dedicated
   settings page; import via file-picker + upload, server-side validated
   before applying (never apply un-validated fields directly over live
   config).
2. **Profile import/export.** Covers kiln firing profiles only
   (`profiles_save`/`profiles_get`/`profiles_list` imply existing schema) —
   export a single profile or all profiles as JSON from `profiles_page.html`,
   import via file-picker + upload. Should reuse whatever point-list schema
   the new LCD profile-creation page (above) ends up writing, so a profile
   authored on the LCD round-trips through web export/import unchanged.
3. **Shared mechanics, not shared data:** both likely want the same
   file-picker + `POST` upload + JSON-parse-and-validate pattern on the
   ESP32 HTTP server side, so implementation can share a helper, but the two
   export files/endpoints stay separate (`/api/settings/export`,
   `/api/profiles/export` style) — a settings file should never accidentally
   double as a profile file or vice versa.
4. **Not yet done:** no endpoints, no HTML controls, no schema audit against
   the real firmware structs. Needs confirming actual field lists in
   firmware source before implementation starts.

### Web — planned: page structure rework (2026-08-20, explicit user request, not started)

Requested verbatim: *"rework the webpage structure. be sure to include
everything that is included in the lcd that makes sense and more. i dont want
everyhting mashed into the main page there should be a seprate page for
settings. and the main page should be reminicient of the lcd main page but be
allowed to scroll."*

Today `main_page.html` (589 lines) is the whole product: Thermocouples,
Relays, Firing profiles, History, a "Settings" link block and a "Danger zone"
all stacked on one page, with the only other routes being `/profiles`,
`/settings/zones`, `/settings/relays`, `/readiness`, `/wifi` and `/ota`. That
is the "everything mashed into the main page" problem. Meanwhile the LCD has
grown eleven pages (home, config hub, temperature, network, safety, board
health, history, diagnostics, thermocouple faults, touch cal/test) and four
of those have **no web equivalent at all** — safety, board health,
diagnostics, thermocouple faults — even though `GET /api/status` already
serves nearly every field they display (see "Data already on the wire"
below). Section 10.5's Web/LCD parity rule is the standing obligation this
section discharges in the web direction.

#### 1. Target route map

| Route | Page | Content |
|---|---|---|
| `GET /` | `main_page.html` (rewritten) | Dashboard only — LCD-home-shaped, scrollable. No settings blocks, no danger zone. |
| `GET /settings` | `settings_page.html` (**new**) | Nav hub, the web twin of `ui_page_config.c`. Links only, no live data. |
| `GET /settings/zones` | `zones_page.html` | unchanged route/content |
| `GET /settings/relays` | `rules_page.html` | unchanged route/content |
| `GET /settings/manual` | `manual_page.html` (**new**) | Manual relay toggles, moved off the dashboard — decided 2026-08-20, see §2 item 6 |
| `GET /wifi` | `wifi_provision_page.html` | unchanged; reached from `/settings`, not from `/` |
| `GET /profiles` | `profiles_page.html` | unchanged route; reachable from both `/` (operator flow) and `/settings` |
| `GET /readiness` | `readiness_page.html` | unchanged route; summarized as a card on `/` |
| `GET /ota` | `ota_page.html` | unchanged; reached from `/settings` |
| `GET /safety` | `safety_page.html` (**new**) | Web twin of `ui_page_safety.c`, plus the detail the LCD's ~264px budget forced off it |
| `GET /diagnostics` | `diagnostics_page.html` (**new**) | Web twin of `ui_page_diagnostics.c` + `ui_page_board_health.c` |
| `GET /diagnostics/thermo` | `thermo_faults_page.html` (**new**) | Web twin of `ui_page_thermo_faults.c` |

Five new pages, five new routes plus their embedded-file symbols.
`max_uri_handlers` is **56** in `wifi_provision_http.c` today (WEB_UI.md's
"40" is stale) against roughly 32 registered, so there is real headroom — but
the 2026-08-11 lesson stands: `httpd_register_uri_handler` failure is logged
and non-fatal, so a page that overflows the cap silently 404s. Re-count and
re-check the cap as part of this work rather than assuming.

#### 2. The main page, LCD-shaped and scrollable

`ui_page_home.c` after its 2026-08-18 slimming is: per-zone compact rows,
run-state summary, and Start/Stop/Menu. The web dashboard should read as the
same page — same card order, same `theme.css` palette and per-zone rotating
accent it already uses — but is explicitly **allowed to scroll**, so it can
carry the cards the LCD had to move off-page. Proposed order, top-down, with
safety-relevant controls first per section 0.5's "fewest taps to Stop" rule:

1. **Run state / profile summary.** Profile name, state (`state`, `phase`),
   segment index of count, elapsed, dwell remaining, target vs actual.
   Start / Pause / Resume / Stop. Stop styled with the accent-5 alarm color
   and never below the fold — see the sticky-Stop item in §4.
2. **Per-zone rows.** Zone name, current temp, setpoint, duty, relay state,
   stale/fault marker — the LCD home row content, one row per zone.
3. **Safety strip.** Armed / tripped / degraded plus the latest trip reason,
   linking to `/safety`. New on the web; the LCD has had it since M6.
4. **Readiness summary.** Pass/fail count from `GET /api/readiness` with a
   link to the full `/readiness` checklist, replacing today's bare link.
5. **History chart.** `#historyChart` moves down the page rather than off it
   — this is exactly the card the LCD had to relocate to `ui_page_history.c`
   for budget, and the scroll allowance is what lets the web keep it inline.
6. ~~**Quick relay toggles.**~~ **Moved off the dashboard entirely
   (decided 2026-08-20).** They get their own page, `/settings/manual`,
   reached from the settings hub. The dashboard becomes pure monitoring
   plus profile run controls — nothing on `/` can energize an element
   outside a running profile. This is a deliberate divergence from the LCD,
   where `ui_page_temperature.c` carries per-zone manual toggles: a phone
   in a pocket can brush a screen in a way a panel mounted on a kiln
   cannot, so the two surfaces get different answers to the same question.
   The extra taps are the point, and the page still keeps §4.5's confirm
   step on top of the added distance.
7. **Footer link to `/settings`.** One link, replacing the current
   five-link "Settings" block and the "Danger zone" section, both of which
   move to the settings hub.

#### 3. What moves off the main page

- The `Settings` `<h2>` block (`/readiness`, `/settings/zones`,
  `/settings/relays`, `/wifi`, `/ota` buttons) becomes the body of
  `/settings`.
- The `Danger zone` block moves to `/settings` under its own heading, keeps
  its distinct accent-5 styling, and gains the confirm step in §4. Nothing
  destructive stays one tap from the dashboard.
- The manual relay toggles move to `/settings/manual` (§2 item 6). They keep
  reading `relays[]` from `/api/status` and posting to the existing
  `POST /api/relay` — no new endpoint, only a new page that owns the
  control.

#### 4. Cross-cutting improvements worth doing in the same pass

These are the "and more" half of the request. Each is independent of the
route split and can be dropped without breaking it.

1. **Shared nav, not six copies.** `theme.css` (10.6/10.6a) already proved
   the shared-asset pattern — one gzipped `EMBED_TXTFILES` file, one route,
   served from `wifi_provision_http.c`. Add `/nav.js` the same way: it
   injects a consistent header (product name + hub link) and a
   thumb-reachable bottom nav bar (Dashboard / Profiles / Settings) on every
   page, retiring the ad-hoc `<a href="/">` back-links each settings page
   hand-rolls today. **This supersedes 10.6's "duplicated `<style>` block, no
   shared route" conclusion** — that reasoning was correct when no hub file
   served all six pages, and `theme.css` has since made it obsolete.
2. **Sticky Stop.** A fixed-position Stop control on *every* page, not just
   the dashboard, active only while a profile is running
   (`/api/profile_exec` already reports this). Section 0.5's own framing:
   "this is a mobile control surface for hitting Stop quickly, not a desktop
   admin panel." A user deep in the zones page today must navigate home first.
3. **One shared poller.** Every page currently owns its own `setTimeout`
   chain against a different endpoint at a different period. Move to a shared
   `/app.js` poller: a single `/api/status` fetch, subscribers register for
   the fields they render, polling **pauses on `document.visibilityState ===
   "hidden"`** and backs off after consecutive failures. Fewer sockets and
   less ESP CPU burned by a phone left on a bench with the page open.
4. **Connection-lost banner.** Today a failed `fetch()` leaves the last good
   numbers on screen indefinitely — a stale temperature that looks live is a
   safety-relevant lie, not a cosmetic bug. Any page showing live data must
   dim its values and show an explicit "disconnected, last update Ns ago"
   banner once a poll fails.
5. **Confirm step for destructive actions.** Forcing a relay on, clearing a
   latched trip, deleting a profile, and everything in the danger zone get an
   explicit confirm. Stop deliberately does **not** — stopping a firing must
   stay one tap.
6. **Auth on writes, with a session token. Scope DECIDED by the user
   (2026-08-20): writes only, plus an encrypted/authenticated session token
   so the password is entered once per session, and more than one user may
   be logged in at the same time.** WEB_UI.md states the current state
   plainly: "there is no other navigation and no authentication of any kind
   — anyone who can reach the board's IP can switch a relay."

   - **Open, no auth:** every page (`GET /`, `/settings`, `/safety`,
     `/diagnostics`, …) and every read endpoint (`GET /api/status`,
     `/api/profile_exec`, `/api/readiness`, `/api/history.csv`,
     `/api/zones`, `/api/rules`, `/api/profiles`, `/status`, `/scan`).
     Monitoring a firing from a phone stays one tap, and §4.3's shared
     poller never carries a credential.
   - **Gated:** every state-changing POST — `POST /api/relay`,
     `/api/profile_exec/{start,stop,pause,resume,ack_last_run}`,
     `/api/profile`, `/api/profile/delete`, `/api/safety/clear_trip`,
     `/api/zones`, `/api/rules`, `/api/control`, `/api/autotune/*`,
     `/provision`, `/forget`, `/ip_config`, and the danger zone. OTA's
     routes keep their own existing per-request challenge (below) — this
     session layer sits alongside it, it does not replace it.

   **Mechanism — reuse `App/drivers/ota_auth.{c,h}`, don't invent a second
   scheme.** That module already implements exactly the primitives needed
   and is proven on this board: `ota_auth_nonce_issue()`/`_check()`
   (16 random bytes, single use, 30 s expiry, IP-bound),
   `hmac_sha256()` over the nonce keyed by the AP password (so the password
   itself is never sent over the wire), `ota_auth_constant_time_equal()`,
   and `ota_auth_lockout_*()` for brute-force backoff. The login flow is
   OTA's challenge/verify flow verbatim: `GET /api/auth/challenge` →
   client HMACs the nonce with the AP password → `POST /api/auth/login`.
   The only new part is what happens on success.

   **Session tokens — server-side table, not a stateless signed cookie.**
   On successful verify, the server mints a session and returns it:

   - **Token**: 32 bytes from `esp_fill_random()`, hex-encoded. Random, not
     derived — nothing about the password or the session is recoverable
     from it, so there is no key to leak and no format to forge.
   - **Storage**: a fixed-size RAM table (start at 8 slots) of
     `{token_hash, client_ip, issued_ms, last_seen_ms}`, guarded by the
     same mutex pattern `ota_http.c`'s `s_ota_lock` already uses. Store the
     **SHA-256 of the token**, not the token — a RAM dump or a stray log
     line then leaks nothing usable. Compare with
     `ota_auth_constant_time_equal()`.
   - **Multi-user**: the table is why this design was picked over a
     stateless HMAC cookie. N independent sessions coexist naturally, each
     with its own expiry, and any one can be revoked (logout, password
     change, admin "sign out all") — a stateless signed token cannot be
     revoked before it expires. Full table = evict the least-recently-seen
     slot, same `lru_purge_enable` philosophy the HTTP server itself uses:
     a stale session must never lock a real operator out.
   - **Expiry**: sliding idle timeout (~30 min, refreshed on each
     authenticated request) plus a hard absolute cap (~12 h). Both cleared
     on reboot, since the table is RAM-only — deliberate, a power cycle
     ends every session.
   - **Transport**: `Set-Cookie: kiln_sid=…; HttpOnly; SameSite=Strict;
     Path=/`. `HttpOnly` keeps it out of reach of page JS; `SameSite=Strict`
     is the CSRF defense — without it any page in the browser can POST to
     the board's IP and ride the cookie.
   - **IP binding**: bind the session to the client IP that logged in, same
     as OTA's nonce already does (`httpd_req_to_sockfd()` +
     `getpeername()`). Cheap, and it kills a stolen-cookie replay from
     another device on the LAN. Accept the cost: a phone that switches from
     the fallback AP to the home network must log in again.

   **Honest limitation, to state in the UI rather than paper over:** this
   server is plain HTTP with no TLS, so while the *password* is never
   transmitted (the HMAC challenge is the whole point), the *session token*
   does travel in clear text on every gated request and can be sniffed by
   anyone already on the same LAN. IP binding and the idle timeout narrow
   that window; they do not close it. This raises the bar from "anyone who
   can reach the IP can switch a relay" to "anyone who can passively sniff
   the LAN can", which is the right increment for a device on a home
   network — but it is not a substitute for keeping the kiln off an
   untrusted network. **This caveat is temporary: TLS is now planned (§6,
   requested 2026-08-20) and closes exactly this gap.** The session layer
   is still designed to be correct without it, and is brought up over plain
   HTTP first so a handshake bug and an auth bug stay distinguishable.

   **Open sub-question, not blocking:** whether the AP password is the
   right credential long-term, or whether a separate "web password" should
   be settable. Reusing the AP password is what ships first — it is
   already provisioned, already the OTA credential, and adds no new stored
   secret.
7. **Unit parity.** The web renders °C from the API; the LCD honors a °F/°C
   setting. The same value should read the same on both surfaces — pick up
   the board's unit setting rather than adding an independent web-only toggle.
8. **Trim `main_page.html` as it splits.** It is 589 lines carrying five
   concerns; after §3 it should be materially smaller, and the shared
   `/nav.js` + `/app.js` extraction should shrink the other five pages too.
   Net embedded-flash cost of four new pages is expected to be roughly
   neutral after gzip because of it — worth measuring, not assuming.

#### 5. Data already on the wire

`GET /api/status` already serves almost everything the four new pages
display, which is why this is mostly a front-end restructure rather than a
firmware feature:

- **Safety page**: `diag_state`, `diag_trip_mask`, `diag_warn_mask`,
  `diag_trip_reason`, `diag_ever_received`, `diag_context_frames_ok`/`_bad`,
  `diag_tx_frames_dropped`, `trip_reason`, `trip_safety_tc_c`,
  `trip_deciding_threshold`, `trip_event_age_ms`, `trip_event_ever_received`,
  `safety_ready`, `safety_temp_c`, `link_version_compatible`,
  `peer_protocol_version`, `self_protocol_version`. Clear-trip already has a
  route: `POST /api/safety/clear_trip`. **This page is the surface that
  action has never had** — TODO 0.5 records the send path as built
  2026-08-19, while `ui_page_safety.c`'s ~264px budget had no room for the
  button.
- **Thermocouple faults page**: `fault_status` (all eight SR bits),
  `fault_guard`, `spi_failed`, `stale`, `valid`, `cj_c`, per channel.
  Visibility only, no "clear faults" action — same scope call as the LCD page.
- **Diagnostics / board health page**: `enclosure_temp_c`, `nvs_sections`,
  `io_ready`, `io_read_failed`, `thermo_ready`, `zones_config_valid`,
  `mounted`, `uptime_at_write_s`, `power_w`. Firmware version/build strings
  and the heap numbers `ui_page_diagnostics.c` shows are **not** in
  `/api/status` today — that is the one genuinely new field set this section
  needs, and it belongs in `/api/status` (or a small `/api/diag`) rather than
  being obtained some other way.

Deliberately **not** mirrored from the LCD: touch calibration and touch test
(`ui_page_touch_cal.c` / `ui_page_touch_test.c`) — panel-hardware utilities
with no meaning in a browser. That is the "that makes sense" qualifier in the
request.

#### 6. TLS for the web UI *and* OTA (decided 2026-08-20 — **plan only, do not build yet**)

Requested explicitly alongside the build-order answer: *"i also want tls for
both ota and this. for now plan only."* This is the piece that turns §4.6's
session token from "sniffable on the LAN" into a real credential, and it
applies to OTA's existing challenge/upload path too — an unencrypted firmware
upload is a bigger exposure than an unencrypted status poll. Nothing in this
subsection is authorized for implementation yet; it exists so the design
decisions are recorded before the front-end work locks assumptions in.

**Server**: swap `httpd_start()` in `wifi_provision_http.c` for
`httpd_ssl_start()` (`esp_https_server`, already in ESP-IDF, no new
dependency). Every other module reaches the server through
`wifi_provision_http_get_server()`, so **no other `.c` file's registration
code changes** — this is the same single-hub property that made `theme.css`
cheap. `httpd_ssl_config_t` wraps the existing `httpd_config_t`, so the three
deliberate deviations already documented in WEB_UI.md (`lru_purge_enable`,
`max_uri_handlers`, `stack_size`) carry over unchanged.

**Certificate — self-signed, generated on the device at first boot.** There
is no CA that will issue for a LAN device with no public name, and a cert
shipped in the firmware image would be identical on every board and
extractable from the binary — worse than self-signed.

- **ECDSA P-256, not RSA-2048.** Roughly an order of magnitude faster to
  handshake on an ESP32-S3, far smaller key/cert, and a much smaller mbedTLS
  footprint. RSA's only advantage here is ancient-client compatibility, which
  a phone browser does not need.
- Generated once on first boot, persisted in NVS (its own namespace,
  alongside the existing `wifi_cfg`/`kiln_nvs` sections — see section 8.1's
  one-partition-per-concern rule and 8.2's boot-time compatibility check,
  which the cert section needs to participate in like every other section).
- CN/SAN covering the mDNS name (`kiln.local`) **and** the current IP. IP
  SANs go stale on a DHCP lease change — plan for regeneration on IP change,
  or accept name-only access and make mDNS the supported path. Decide during
  implementation; it is the one genuinely fiddly part.
- **Show the certificate fingerprint on the LCD.** `ui_page_diagnostics.c`
  is the natural home. This is what makes self-signed defensible rather than
  a shrug: the user can compare the browser's "this certificate is untrusted"
  fingerprint against the physical panel in front of them and know they are
  talking to *their* kiln and not something else answering on that IP. A
  browser warning nobody can verify is theater; one you can check against
  the device is real authentication.

**Port and redirect layout.**

- HTTPS on 443. A plain-HTTP listener stays on 80 that does nothing but
  `301` to the HTTPS URL, so a bookmarked/typed `http://kiln.local` still
  lands.
- **AP-mode provisioning stays plain HTTP.** Phone captive-portal detection
  breaks on TLS, and a fresh board's AP has no name and no trusted cert
  anyway, so a TLS handshake there buys nothing and costs the setup flow.
  `wifi_provision_page.html` served over the fallback AP is the one
  deliberate exception; every STA-mode route is TLS. Worth stating in the UI
  so it is not mistaken for an oversight.

**Cost, and the constraint that decides it: PSRAM is off** (section 9.1a,
"decided: stays off"). Each concurrent TLS session costs mbedTLS handshake
and record buffers out of internal SRAM, and this firmware has already hit
internal-SRAM exhaustion once (ROADMAP.md's 2026-08-19 crash-loop entry).
That makes the following non-optional rather than tuning:

- Cap concurrent TLS sessions well below the plain-HTTP connection count,
  and lean on `lru_purge_enable` — which is already set for exactly this
  "a stuck client must not lock a phone out" reason.
- Reduce `MBEDTLS_SSL_IN_CONTENT_LEN`/`OUT_CONTENT_LEN` from the 16 KB
  default; this UI's largest body is `zones_post_handler`'s ~3.2 KB, so
  4 KB is generous. This is the single biggest RAM lever available.
- Enable TLS session resumption/tickets — §4.3's shared poller reconnects
  regularly, and a full ECDHE handshake every 2 s per client is the one
  workload that would actually hurt.
- Measure `esp_get_minimum_free_heap_size()` before and after with two or
  three phones connected. `ui_page_diagnostics.c` already displays exactly
  that worst-case-ever number — the instrument for this is built.

**OTA over TLS.** `ota_http.c`'s routes ride the same server, so they inherit
TLS with no per-route change; the challenge/HMAC scheme stays exactly as it
is (defense in depth, and it is what proves knowledge of the AP password
without sending it). What TLS adds for OTA specifically is confidentiality
and integrity of the *image transfer* — today a firmware upload crosses the
LAN in clear. Note it does **not** replace image signing: TLS protects the
wire, secure boot / signed images protect against a malicious image
delivered over a perfectly good TLS connection. Those are separate, and
signing is not in this plan.

**Sequencing.** TLS lands *after* the §4.6 session layer works over plain
HTTP, not before — otherwise a handshake bug and an auth bug are
indistinguishable during bring-up. Once TLS is on, revisit §4.6's honest
limitation paragraph: the "token is sniffable on the LAN" caveat is exactly
what this closes.

#### 7. Not yet done

No new `.html` file, no new route, no `CMakeLists.txt` `EMBED_TXTFILES`
entry, no handler registered, no shared `/nav.js` or `/app.js`, no field
added to `/api/status`, no session table and no `/api/auth/*` route. This
section is a plan entry so the request isn't lost, not a claim of progress.
§4.6's auth scope is no longer open — the user settled it 2026-08-20 (writes
gated, reads open, server-side session tokens, multi-user); the only
sub-question left there is whether a separate web password eventually
replaces the AP password, which does not block starting.

**Decisions taken 2026-08-20, recorded so implementation doesn't re-litigate
them:**

- **Scope of the first implementation pass: everything, auth included** —
  the route split (§1–3), the shared `/nav.js` + `/app.js`, sticky Stop, the
  connection-lost banner, the confirm steps, and §4.6's session layer land
  together rather than as separate passes. Consequence to plan around: this
  is one large diff mixing a front-end restructure with new firmware
  security code, so the auth layer wants its own commit inside that pass and
  its own review attention — the failure mode of a bug there is not a
  cosmetic one.
- **Manual relay toggles: their own page** (`/settings/manual`), off the
  dashboard. §2 item 6 and §3.
- **Diagnostics: two web pages, not three** — `/diagnostics` merges the
  LCD's diagnostics and board-health content (the web can scroll, so the
  ~264px split that forced them apart on the panel doesn't apply), and
  `/diagnostics/thermo` stays separate.
- **Live updates: polling stays** — §4.3's single shared, visibility-aware,
  backing-off `/api/status` poll. No SSE, no WebSocket, no
  `CONFIG_HTTPD_WS_SUPPORT`. TODO.md section 2's push question stays open
  pending evidence from someone actually watching a real firing.
- **TLS for both the web UI and OTA: planned, not authorized** — §6 is
  design only by explicit instruction ("for now plan only"). Nothing in it
  gets built in the pass above.

### LCD navigation (found during item 6's work, 2026-08-19)

While wiring `ui_page_touch_cal.c`'s new Cancel button, swept every LCD
page's Back button target for the same bug class (going to the wrong
screen). `ui_page_config.c` is the nav hub reached from `ui_page_home.c`'s
Menu button; every page reached *from* that hub
(`ui_page_board_health.c`, `ui_page_history.c`, `ui_page_network.c`,
`ui_page_safety.c`) correctly calls `kiln_ui_show("config")` from its
`back_btn_cb()`. **`ui_page_temperature.c` alone called
`kiln_ui_show("home")`** — same hub-reached page, wrong target, skipping
the menu it was actually opened from and dropping the user straight to
the dashboard instead of one level back. **Fixed (2026-08-19)**: changed
to `kiln_ui_show("config")`, matching every sibling page. Build-clean —
not flashed/visually confirmed. No other page had this bug: `ui_config.c`
itself correctly goes to `"home"` (it *is* the top level below home), and
`ui_page_touch_test.c`'s "Done" button correctly goes to `"home"` per
`ui_page_touch_cal.c`'s own comment on that page's deliberate flow
(calibrate → verify → home, not calibrate → verify → config).
