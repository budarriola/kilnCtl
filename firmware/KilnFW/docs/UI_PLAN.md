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
