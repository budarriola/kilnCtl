# Commissioning LCD Runbook

Prepared for the LCD phase of M18 (`docs/COMMISSIONING_TEST_MATRIX.md`'s
"3. LCD last" ordering), after the backend (`docs/COMMISSIONING_BACKEND_RUNBOOK.md`)
and web-UI phases. This document was produced **read-only, camera-only** —
no `kilnctrl` MCP tool was called, the board was not touched, tapped, or
reset. Another session held the board for other work at the time of writing.

## What this document is, and its central limitation

Every LCD page's layout is built with LVGL flex/grid containers
(`lv_obj_set_flex_flow`, `LV_FLEX_FLOW_ROW_WRAP`, `lv_obj_align`), not fixed
`lv_obj_set_pos` coordinates — confirmed by reading `ui_topbar.c` (topbar
icons are a right-aligned flex row, Back/Home/Prev/Next/Gear built
left-to-right, present-or-absent per page, so a topbar icon's absolute X
position is **context-dependent per page**, not fixed) and
`ui_page_config.c` (the Configuration hub is a `ROW_WRAP` grid whose cell
count and therefore row assignment changes with which cells are offered).
`touch_inject(x, y, pressed)`'s own docstring
(`tools/PcTools/src/kilnctrl/mcp_server_touch.py:88-110`) confirms it takes
raw LVGL **screen-pixel** coordinates (0,0 top-left, 480x320 native), the
same space every `ui_page_*.c` lays widgets out in — a different space from
the webcam capture-crop pixel coordinates used by `sample_lcd_region.ps1`
(crop is `X=158 Y=52 W=886 H=593` as of the 2026-09-24 camera-aim fix
(CLAUDE.md), an 886x593 window, not 480x320 — the capture is a photograph of
the panel, magnified, not a framebuffer dump; the crop verification section
below predates that fix and quotes the superseded 2026-09-19 numbers).

`touch_log_tap_targets()`'s own docstring says plainly: **"this is the only
way to discover where a widget actually is on this panel — there is no
framebuffer readback."** This session was explicitly forbidden from calling
it (it is the tool that previously panicked the board —
`project_touch_log_tap_targets_panics_board`, fix `3f86e899` in source but
not confirmed on this running image). **Consequence: exact `touch_inject`
pixel coordinates for most rows below cannot be derived from source alone
and are not claimed as precise.** Where a value is given it is either (a) a
structural derivation from grid constants that is exact for cell membership
but only approximate for the pixel center (topbar height/padding origin not
fully traced), or (b) the one prior live measurement on record
(`reference_lcd_tap_geometry.md`, 2026-09-19, itself in **capture-crop**
pixel space, not confirmed against native LVGL screen space — flagged below
everywhere it is used). Every row not covered by (a)/(b) is flagged
**UNKNOWN — needs a live `touch_log_tap_targets` dump** before it can be run
mechanically; this is intentional, not an oversight, per the tool's own
docstring above.

**`touch_log_tap_targets` is forbidden for the reason above and must not be
called by whichever session eventually runs this class**, until that fix is
confirmed flashed and re-tested in isolation first.

## Panel and camera facts

- Panel: 480x320, ST7796 driver, FT6336U capacitive touch (self-calibrating
  — `firmware/KilnFW/docs/PROJECT_STATUS.md` "Hardware present on this bench
  unit"). `KILNCTL_TOUCH_CAL_SWAP_XY` is inert on this hardware; the live
  knobs are `KILNCTL_TOUCH_CAP_*`. Pages must not scroll (per
  `feedback_lcd_no_scrolling` and `ui_page_config.c`'s own comment that its
  grid height is sized to never need it).
- Because this bench's FT6336U is self-calibrating, `touch_dev_cal_support()`
  returns not-offerable here, so **`ui_page_config.c`'s "Touch Calibration"
  cell is hidden on this bench build** (`ui_page_config.c:255-262`,
  `touch_cal_support_is_offerable(lvgl_port_touch_cal_support())`). The
  Configuration hub therefore renders **5** cells here, not 6: Profiles,
  Temperature, Network / Wi-Fi, Diagnostics, the units toggle
  (`ui_page_config.c:243-249`), laid out 2+2+1 in a `ROW_WRAP` grid. This
  also means the `touch_cal` LCD page — and therefore `touch_test`, which is
  reachable only via `touch_cal`'s own "Test" button
  (`ui_page_touch_cal.c:187`) — **has no reachable navigation path from the
  home page on this bench's hardware.** Both pages still exist and are
  registered (`kiln_ui.c:219,266`), so they could in principle still be
  driven by a direct `kiln_ui_show()` call from a debug surface, but no such
  MCP tool exists (`kiln_find(query="show ui page")` was not run — no board
  contact this session — but no `ui_show`/`ui_navigate`-shaped tool is named
  anywhere in the backend/web runbooks or `mcp_server_touch.py`). **Rows
  `touch_cal` and `touch_test` are therefore N/A on this bench unit, not
  merely untested**, until this bench's touch controller changes.

## Camera crop verification (today, 2026-09-21)

**Superseded 2026-09-24:** the crop this section verifies (`X=296 Y=58
W=853 H=578`) went stale (LCD-01) and was replaced with `X=158 Y=52 W=886
H=593` — see CLAUDE.md's "Camera aim (2026-09-24)" note. This section is
kept as a historical record of that day's evidence, not current geometry.

`capture_lcd.ps1 -Full` and the default-crop capture both succeeded (no
ffmpeg exit `-5` — no other process was found holding the C920). Full frame:
1280x720, confirmed via `ffprobe`.

Numeric luminance/RGB scans (`ffmpeg ... -f rawvideo -pix_fmt rgb24 - | od`,
even-sized crop windows since the source is 4:2:2 chroma-subsampled and an
odd crop offset made `ffmpeg` refuse with `-22 Invalid argument`) along
horizontal and vertical lines through the documented 2026-09-19 crop
(`X=296 Y=58 W=853 H=578`, corners at left `X=298`(top)-`323`(bottom), right
`X=1145-1147`, top `Y=60`(right)-`86`(left), bottom `Y=617`(right)-`635`(left))
show content-bearing (non-uniform, colored) pixel values changing character
at each of the four documented boundaries and materially different pixel
values immediately outside them in every case checked (e.g. `y=300` scan:
dark ~RGB(40-50,26-32,27-33) inside the left edge from `x=260` to `x=360`,
stepping to brighter/differently-toned values `x>=1140`; `x=700` vertical
scan: bright ~RGB(212-219,168-175,165-174) `y<=65`, dropping sharply at
`y=70-75`, i.e. within a few px of the documented `Y=60-86` top band; bottom
scan shows the same kind of step at `y=635-645`). Because this panel's own
UI is dark-themed and non-uniform (not a solid test pattern), a single-line
luminance scan does not resolve a sub-pixel edge the way it would against a
blank bezel — **this confirms the documented crop still frames the panel
with no evidence of drift beyond the +/-10px tolerance**, but is not a new
independent corner-by-corner re-measurement to the same precision as the
2026-09-19 pass. No re-crop is warranted from this evidence.

Captures saved to the scratchpad only (`C:\Users\budar\AppData\Local\Temp\claude\lcd_full_20260921.jpg`,
`...\lcd_crop_20260921.jpg`), not the repo.

## Camera aim (2026-09-24, round 3): `lcd_sampler.FRAME_CORNERS` re-derived

Round 2's review found `lcd_sampler.py`'s `FRAME_CORNERS` stale: on capture
`logs/bench_test/20260924T162517Z_lcd/captures/lcd01_start_pause.jpg`,
`widget_to_frame(5,5)` mapped to frame `(171,63)`, which samples as bezel
`RGB(6,13,22)`, and `(423,289)` landed on the Start button's own edge instead
of clear background.

Re-derived by numeric luminance/color edge scans (PIL pixel sampling against
that same capture, never by eye): scanning horizontal and vertical lines
looking for the single biggest luminance step (`0.299*R+0.587*G+0.114*B`)
between bezel and panel content. The top-right and bottom-right corners fall
in a region contaminated by an overexposed glare band (near-white RGB ~250+
fading gradually into the theme's dark-blue background around `y=100-140`
for `x=950-1100`), where a direct step-scan is unreliable; those two corners
were instead obtained by a linear fit through the clean part of the same
edge (top edge `x=190-630`; right edge `y=300-550`) extrapolated outward,
cross-checked against the point in the noisy region where raw RGB actually
transitions into the theme's characteristic bluish background tone
(`y~=100-120`) — both methods agreed on `y~=121-123` for the top-right
corner.

Result, in the bench camera's full 1280x720 frame:
- top-left corner: `(180, 69)`
- top-right corner: `(1038, 121)`
- bottom-left corner: `(178, 627)`
- bottom-right corner: `(985, 628)`

Verified against the reported symptom: with these corners,
`widget_to_frame(5,5)` maps to frame `(190,79)`, which samples as
`RGB(109,213,248)` — clearly panel content, not bezel.

Because a stale-corner regression like this fails silently as ordinary color
FAILs rather than loudly, `lcd_sampler.frame_corners_look_stale()` now
samples the four expected-background widget-space corners (`(5,5)` etc.) at
runtime and reports `True` if *any one* of them reads as bezel, compared
against `sample_widget()`'s one fixed bezel reference at frame `(100,100)`
(not a per-corner local sample, and never an absolute threshold, since the
theme's own darkest background is not far from bezel darkness). "Any", not
"all four": on the capture above the superseded corners put only two of the
four points on bezel (`(5,5)` and `(5,315)`), so an all-four rule would have
missed the very incident it exists to catch. A dark/blanked panel trips it
too. `cases_lcd.py`'s `_downgrade_if_corners_stale()` uses this to downgrade
an existing color-judgment `FAIL` to `INCONCLUSIVE` with reason "frame
corners stale or panel dark" (the original reason kept in
`observed["original_fail_reason"]`) — but only a `FAIL`, and only on a
definite `True`, never on `False`
(background reads as background: a real mismatch is real) or `None`
(capture/sample failure: stays whatever it already was).

## Navigation graph (from source, `kiln_ui_show()` call sites)

```
home --[Menu button, ui_home_menu_nav_cb]--> config
home --[profile name / picker, ui_home_profile_btn_cb]--> profile_picker
config --[Profiles cell]--> profiles
config --[Temperature cell]--> temperature
config --[Network/Wi-Fi cell]--> network
config --[Diagnostics cell]--> diagnostics
config --[Touch Calibration cell]--> touch_cal   (cell HIDDEN on this bench, see above)
network --[Manage networks button]--> network_manage
touch_cal --[Test button]--> touch_test          (unreachable on this bench, see above)
touch_cal --[Back]--> config
touch_test --[Back]--> home
profile_picker --[row tap]--> profile_detail
profile_picker --[New]--> profile_builder_zones
profile_detail --[Segments]--> profile_segments
profile_detail --[Edit-as-new]--> profile_builder_zones
profile_detail --[Back]--> home
profile_builder_zones --[Next]--> profile_builder_segment
profile_builder_segment --[Next]--> profile_builder_review
profile_builder_review --[Save]--> profiles
every page's topbar --[Back/Home icon]--> back_page / home (per ui_topbar_cfg_t.back_page)
```
Every arrow above is a direct grep hit (`kiln_ui_show("<name>")`) at the
cited files; none of it is inferred.

---

## Row template

Each row below: **Page** (source file), **Class** (read-only / write /
owner-gated), **Nav from home** (path + best-available tap geometry, or
UNKNOWN), **MCP call**, **verification region** (capture-crop pixel box,
sampled with `sample_lcd_region.ps1` against a bezel reference — bezel
reference for this session's capture: `(100,100)` in the `-Full` frame,
measured `RGB(53-54,33-34,32-33)`), **backend read-back**.

Unless noted, all rows are **read-only page-load / render checks**: navigate
to the page, capture, sample, read the backend state that should match, and
navigate back — no config, profile, network, or credential state is
written.

### 1. `home` — READ-ONLY
- Source: `ui_page_home.c` (+`_actions`, `_chart`, `_graph`, `_rail`,
  `_refresh`). Purpose: live status, start/stop, graph.
- Nav: boot default; also every page's Home topbar icon returns here.
  Topbar Home icon position is **UNKNOWN** (context-dependent, see above) —
  needs a live tap-target dump.
- MCP: none needed to reach it (default screen); `touch_inject` only needed
  to return here from elsewhere.
- Verification region: whole-panel capture inside the crop; sample a
  content cell (e.g. current-temperature readout) once its region is known
  from a live dump. **UNKNOWN pixel region** — flag for live confirmation.
- Backend read-back: `get_board_state()` (`GET /api/status`) — compare
  displayed zone temps/status text against the API's `zones[].temp_c` and
  `state`.

### 2. `config` — READ-ONLY
- Source: `ui_page_config.c`. Purpose: device config menu (nav hub).
- Nav: `home` --[Menu, `ui_home_menu_nav_cb`, `ui_page_home_actions.c:305`]--> `config`.
  Menu button position: **UNKNOWN** (home page's own button layout not
  traced this session).
- MCP: `touch_inject(x=<menu_x>, y=<menu_y>, pressed=True)` then
  `pressed=False`, coordinates UNKNOWN.
- Verification region: the 5-cell grid (Profiles/Temperature/Network/
  Diagnostics/units-toggle, 2+2+1, since Touch Calibration is hidden on this
  bench — see above). Cell geometry: `UI_CONFIG_HUB_GRID_HEIGHT_PX` = 3 x
  `UI_THEME_MIN_TOUCH_TARGET_PX`(72) + 2 x (`UI_THEME_PADDING_PX`/2) —
  concrete row heights (72px each, ~4px gaps) but the grid's absolute
  screen origin (below the topbar + screen padding) was not traced to a
  final pixel this session; **row membership is exact from source, absolute
  pixel origin is approximate.** One live data point exists: the 2026-09-19
  memory (`reference_lcd_tap_geometry.md`) recorded "Config page:
  'Diagnostics' button at (345,156)" — Diagnostics is cell 4 of 5 (row 2,
  right column: row1=Profiles/Temperature, row2=Network/Diagnostics,
  row3=units toggle alone), consistent with this grid's structure, but that
  coordinate is stated as capture-crop space in the memory file, not
  confirmed as native 480x320 LVGL space — **do not feed 345,156 directly
  to `touch_inject` without re-confirming which space it is in.**
- Backend read-back: none directly (nav hub only); confirmed present via
  `GET /api/status`'s `touch_cal_supported: false` field, which should agree
  with the Touch Calibration cell being absent.

### 3. `temperature` — READ-ONLY
- Source: `ui_page_temperature.c` (+`_safety`). Purpose: per-zone
  temperature + safety status.
- Nav: `config` --[Temperature cell, `temperature_nav_cb`,
  `ui_page_config.c:207-211`]--> `temperature`. Cell 2 of 5 (row 1, right
  column). Pixel geometry: **UNKNOWN** absolute origin, same caveat as
  above.
- MCP: `touch_inject` at the Temperature cell (coords UNKNOWN), then
  capture.
- Verification region: **UNKNOWN** — per-zone temp readouts.
- Backend read-back: `safety_get_status()` / `GET /api/status` safety
  fields (matrix row A20) and `thermo_read_faults()` (A22) — compare
  displayed values.

### 4. `network` — READ-ONLY (page load); `network_manage` below is WRITE
- Source: `ui_page_network.c`. Purpose: Wi-Fi status.
- Nav: `config` --[Network/Wi-Fi cell, `network_nav_cb`,
  `ui_page_config.c:213-217`]--> `network`. Cell 3 of 5 (row 2, left
  column). Pixel geometry: **UNKNOWN** absolute origin.
- MCP: `touch_inject` at the Network cell (coords UNKNOWN).
- Verification region: **UNKNOWN** — SSID/status text area.
- Backend read-back: `wifi_get_status()` (`GET /wifi`, `/status`) — note
  `project_peer_ip_is_ipv4_mapped_ipv6` if any IP is displayed, and that
  `wifi_get_status()`/`get_board_state()` now redact the AP password
  (`project_ota_status_route_leaks_build_identity` class fix,
  `7a6d7ed3` in this repo's recent history) — do not expect to see it on
  screen either.

### 5. `network_manage` — WRITE, owner-gated for this class
- Source: `ui_page_network_manage.c`. Purpose: Wi-Fi scan/connect/forget.
- Nav: `network` --[Manage networks button, `manage_btn_cb`,
  `ui_page_network.c:387`]--> `network_manage`. Pixel geometry: **UNKNOWN**.
- This page's own actions (connect/forget) change live Wi-Fi association —
  **do not exercise the connect/forget actions in this class**; page-load
  only (navigate in, capture, navigate back) is read-only and fine. Treat
  the row as READ-ONLY for a load/render check, WRITE/owner-gated for any
  scan-and-connect action.
- Verification region: **UNKNOWN**.
- Backend read-back: `wifi_get_status()` before/after confirms no
  association change if only the page-load was exercised.

### 6. `diagnostics` — READ-ONLY
- Source: `ui_page_diagnostics.c`. Purpose: on-device diagnostics (folds in
  Board Health, Safety Processor, Thermocouple Faults per the 2026-08-27
  owner request recorded in `ui_page_config.c`'s header comment).
- Nav: `config` --[Diagnostics cell, `diagnostics_nav_cb`,
  `ui_page_config.c:218-222`]--> `diagnostics`. Cell 4 of 5 (row 2, right
  column) — **this is the one cell with a prior live measurement on
  record**: `reference_lcd_tap_geometry.md` (2026-09-19) recorded
  "Diagnostics button at (345,156)" on the Config page, consistent with
  this cell's row/column position. As above, confirm which coordinate space
  that number is in before using it with `touch_inject` — it was captured
  from screen photography, and this document could not re-verify the space
  without a live tap-target dump.
- Also has its own topbar Back/Home/Prev/Next set — memory records
  "Diagnostics page topbar: Back (333,20), Home (373,20), Prev (413,20),
  Next (453,20)" from the same 2026-09-19 pass, same space caveat.
- MCP: `touch_inject` at the Diagnostics cell, then at the topbar icons for
  sub-page navigation if the page itself pages internally (not confirmed
  this session).
- Verification region: **UNKNOWN** per sub-section; use
  `sample_lcd_region.ps1` on the populated area once geometry is confirmed
  live.
- Backend read-back: `safety_get_status()`, `thermo_read_faults()`,
  `get_stack_margin()` (ESP side only — A29 notes SaftyFW stack margin has
  no direct MCP tool), `GET /api/crash_report` — this page is the LCD
  surface most likely to show an unacknowledged-crash banner; cross-check
  against `get_heap_status()`'s own crash-report surfacing
  (`project_safety_calls_logging_unchecked_success` class — verify the LCD
  agrees with the API, don't trust either alone).

### 7. `touch_cal` — N/A on this bench (see "Panel and camera facts")
- Source: `ui_page_touch_cal.c`. Not reachable from the home page on this
  bench build (cell hidden, `touch_dev_cal_support()` false for the
  self-calibrating FT6336U). Do not attempt; there is no button to tap.

### 8. `touch_test` — N/A on this bench (unreachable, see above) — and its
own natural exercise tool (`touch_log_tap_targets`) is separately
**FORBIDDEN** (panics the board, fix not confirmed flashed on the running
image). Do not attempt either the page or the tool.

### 9. `profiles` — READ-ONLY
- Source: `ui_page_profiles.c`. Purpose: profile library.
- Nav: `config` --[Profiles cell, `profiles_nav_cb`,
  `ui_page_config.c:196-203`]--> `profiles`. Cell 1 of 5 (row 1, left
  column, moved here 2026-08-27 per that file's header comment). Also
  reached from `profile_builder_review` on Save (write path, see row 15).
  Pixel geometry: **UNKNOWN**.
- MCP: `touch_inject` at the Profiles cell.
- Verification region: **UNKNOWN** — profile list rows.
- Backend read-back: `profiles_list()` (`GET /api/profiles`, A9) — compare
  the on-screen list against the API's list, including favorites.

### 10. `profile_picker` — READ-ONLY (page load); selecting a profile is a
state-changing pick (see below)
- Source: `ui_page_profile_picker.c` (+`_format`). Purpose: choose profile
  to fire.
- Nav: `home` --[profile name / picker slot, `ui_home_profile_btn_cb`,
  `ui_page_home_actions.c:314-323`]--> `profile_picker`. Pixel geometry:
  **UNKNOWN** (home page button layout not traced).
- Picking a profile (`ui_home_profile_picked_cb`) records
  `s_ui_home_picked_id` for the boot and returns to `home` — this is an
  in-RAM UI pick, not a persisted write, but it does change what Start would
  fire next. Treat page-load as read-only; **do not tap a row** unless the
  intent is specifically to exercise the pick-and-return path, and restore
  by picking back to whatever was selected before (or accept it's boot-scoped
  and irrelevant until a Start).
- Verification region: **UNKNOWN**.
- Backend read-back: `profiles_list()`/`profiles_get()` for what should be
  offered.

### 11. `profile_detail` — READ-ONLY
- Source: `ui_page_profile_detail.c`. Purpose: view one profile.
- Nav: `profile_picker` --[row tap]--> `profile_detail`
  (`ui_page_profile_picker.c:247`). ("`profiles_builtin_list` --[row
  tap]--> `profile_detail`" was a second nav path until
  `ui_page_profiles_builtin_list.c` was removed 2026-09-21 as dead code --
  nothing ever called `kiln_ui_show("profiles_builtin_list")`.) Pixel
  geometry: **UNKNOWN** (row position depends on list scroll/index).
- Verification region: **UNKNOWN**.
- Backend read-back: `profiles_get()` (`GET /api/profile`, A10) for the same
  profile id — compare segment count/zone targets shown.

### 12. `profile_segments` — READ-ONLY
- Source: `ui_page_profile_segments.c`. Purpose: segment list.
- Nav: `profile_detail` --[Segments button, `ui_page_profile_detail.c:443`]--> `profile_segments`.
  Pixel geometry: **UNKNOWN**.
- Backend read-back: `profiles_get()`'s segment array.

### 13. `profile_builder_zones` — WRITE flow entry, owner-gated for actual
save; page-load itself is read-only
- Source: `ui_page_profile_builder_zones.c`. Purpose: new-profile zone
  selection.
- Nav: `profile_picker` --[New button, `ui_page_profile_picker.c:377`]-->
  `profile_builder_zones`; also `profile_detail` --[Edit-as-new,
  `ui_page_profile_detail.c:456`]--> same. Pixel geometry: **UNKNOWN**.
- The builder chain (`profile_builder_zones` -> `_segment` -> `_review` ->
  Save) ends in a `POST /api/profile` write. **Do not carry the chain
  through to Save in this class** — page-load-only for zones/segment/review
  is read-only; the Save action is WRITE and belongs with the backend
  runbook's profile-write rows (owner-scheduled), not repeated here.
- Backend read-back: none needed for a load-only check.

### 14. `profile_builder_segment` — page-load READ-ONLY, same caveat as #13
- Source: `ui_page_profile_builder_segment.c`.
- Nav: `profile_builder_zones` --[Next, `ui_page_profile_builder_zones.c:100`]--> `profile_builder_segment`.
  Pixel geometry: **UNKNOWN**.

### 15. `profile_builder_review` — page-load READ-ONLY, same caveat as #13
- Source: `ui_page_profile_builder_review.c`.
- Nav: `profile_builder_segment` --[Next,
  `ui_page_profile_builder_segment.c:176`]--> `profile_builder_review`.
  Pixel geometry: **UNKNOWN**. Its own Save button
  (`ui_page_profile_builder_review.c:124`) is the WRITE action noted in #13
  — do not tap it in this class.

---

## Summary by class

| Class | Rows |
|---|---|
| Read-only (page-load navigate + numeric-sample + backend read-back) | `home`, `config`, `temperature`, `network` (load only), `diagnostics`, `profiles`, `profile_picker` (load only), `profile_detail`, `profile_segments`, `profile_builder_zones` (load only), `profile_builder_segment` (load only), `profile_builder_review` (load only) — 12 |
| Write (owner-gated, not part of this read-only pass) | `network_manage` connect/forget actions; `profile_picker` pick-and-return; the builder chain's terminal Save (`profile_builder_review`) — 3 flagged sub-actions on top of the 12 rows above |
| N/A on this bench hardware | `touch_cal`, `touch_test` — 2 |

15 registered LCD pages total, matching `docs/COMMISSIONING_TEST_MATRIX.md`'s
count (`profiles_builtin_list` removed 2026-09-21 as dead code -- was never
navigated to).

## Rows whose tap geometry could not be derived from source

All of them except the two noted with a prior live measurement
(`config` hub's Diagnostics cell, and that page's own topbar icons) — i.e.
**13 of 15 pages'** entry-tap coordinates are UNKNOWN from static analysis
alone, for the structural reason in "What this document is" above (flex/grid
layout, not fixed positions; topbar icon sets vary per page). Both of the
two exceptions carry an unresolved coordinate-space caveat (capture-crop
pixels vs. native LVGL screen pixels) that also needs live confirmation.
**A single, isolated, board-owner-supervised `touch_log_tap_targets()` call
per distinct page** (once its panic fix is confirmed flashed and separately
verified safe) is what would resolve all of these at once, per that tool's
own docstring — this was explicitly out of scope for this read-only,
camera-only pass.

## Tap geometry derived from source (2026-09-21)

**Everything below is source-derived, not bench-verified.** No board, no MCP
`kilnctrl` tool, and no webcam were touched to produce it — another session
held the bench. All coordinates are in native LVGL screen-pixel space,
0,0 top-left, 480x320 landscape (confirmed: `firmware/KilnFW/App/drivers/hw/settings.h`'s
`CONFIG_KILNCTL_DISPLAY_WIDTH`-derived `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` feed
`UI_THEME_PAGE_CONTENT_BUDGET_PX`'s own derivation comment in `ui_theme.h:238-244`,
which states the panel is landscape 480x320 with the unrotated short edge
(320) becoming the rotated height — the same space `touch_inject(x,y)` takes
per its docstring, already noted above).

### Method and the fix to the "flex/grid means unknowable" premise

The original pass in this document concluded pixel geometry was unrecoverable
because pages use `lv_obj_align`/flex instead of fixed `lv_obj_set_pos`. That
is true of the *page's own internal arithmetic*, but every page here shares
two fully-deterministic containers whose geometry a static read resolves
completely:

1. **The topbar icon row** (`ui_topbar.c`). Icons are built into a
   `LV_FLEX_FLOW_ROW` container with `LV_FLEX_ALIGN_END` (packed to the right,
   no distributed slack), `UI_TOPBAR_ICON_GAP_PX` (4px) gap
   (`ui_topbar.h:80-82`), each icon a fixed `UI_TOPBAR_ICON_W_PX x
   UI_TOPBAR_ICON_H_PX` = 36x26px. The container itself is `FLOATING` and
   `lv_obj_align`ed `LV_ALIGN_TOP_RIGHT` with a `0,0` offset onto the page
   root (`ui_topbar.c:151-163`) — and **every page's root sets
   `lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0)`** (8px, confirmed
   by grep across all fourteen `ui_page_*.c` files below), which
   `lv_obj_align` honors as an inset. So the icon row's real right/top edge is
   `(480-8, 0+8)` = `(472, 8)`, not `(480, 0)` as a naive read of the align
   call alone would suggest — **this 8px inset is the correction that makes
   the derivation match live measurement (below)**; missing it is exactly
   the kind of "approximate, absolute origin not traced" gap the original
   pass flagged and stopped short of closing.
   Icon height 26px, top-aligned within the row (cross-axis `START`) → every
   topbar icon's vertical center is **y=21** (8 + 26/2 = 21), full stop,
   independent of which page or how many icons.
   Horizontal: container left edge = `472 - icons_w`, where
   `icons_w = n*36 + (n-1)*4`. Icons are packed left-to-right in build order
   (Back, Home, Prev, Next, Warning, Add, Gear — `ui_topbar.c:170-201`), and
   because the row is right-flush, **the rightmost icon's center is always
   x=454**, and each icon one slot to its left is exactly **40px less**
   (36 + 4 gap). This is why the table below is just "454 minus 40 times
   position-from-the-right" for every page.

2. **The Configuration hub's grid** (`ui_page_config.c`). Fixed cell height
   `UI_THEME_MIN_TOUCH_TARGET_PX` (72px), `lv_pct(48)` width, `ROW_WRAP`,
   `UI_THEME_PADDING_PX/2` (4px) gap both axes (`ui_page_config.c:139-238`).
   The grid is the content area's first (and only occupied-height) child, so
   its top edge is the page's fixed content-area origin
   (`UI_THEME_STATUS_BAR_HEIGHT_PX` + 2 gaps + top pad = 32+4+8 = 44,
   matching `UI_THEME_PAGE_CONTENT_BUDGET_PX`'s own 268px derivation,
   `ui_theme.h:238-244`). Grid width = scr's inner width (480-16=464);
   `lv_pct(48)` of 464 = 222px per cell (LVGL integer truncation), leaving a
   16px unused strip on the right that the ROW_WRAP's default
   `START`/`START` alignment does not redistribute.

### Calibration check against the two prior live measurements

`reference_lcd_tap_geometry.md` (2026-09-19) recorded two live numbers. Both
are checked against this derivation:

| Prior live measurement | This derivation | Agreement |
|---|---|---|
| Config page "Diagnostics" cell at (345,156) | Diagnostics cell, hub grid col1/row1 → **(345,156)** exactly (see table below) | **Exact match, 0px.** This also settles the coordinate-space doubt the original pass raised (Section "Row template" / row 6 above): the memory value is native LVGL screen-pixel space, not capture-crop space — a 1.78x/1.81x crop-scale mismatch would have shown up as a large, non-constant error, not an exact hit. |
| Diagnostics page topbar Back/Home/Prev/Next at (333,20)/(373,20)/(413,20)/(453,20) | Diagnostics topbar (4 icons: Back,Home,Prev,Next) → **(334,21)/(374,21)/(414,21)/(454,21)** | **Within 1px on every value** — consistent with the live reading being eyeballed/rounded off a webcam photo (per the runbook's own camera-crop caveats) rather than a real disagreement. Trusted: this derivation, since it is exact arithmetic against source constants, not a second photo measurement; the 1px gap is noise, not a conflict. |

Both prior "trust which one" flags in the original document (rows 2 and 6
above) are now resolved: **trust this derivation**, and it is consistent
with, not contradicting, the two live data points on record.

One structural finding this closes: the nav-graph's "home --[Menu button,
`ui_home_menu_nav_cb`]--> config" arrow (line 109) and row 2's "Menu button
position: UNKNOWN, home page's own button layout not traced" (line 168) are
the SAME icon — `ui_home_menu_nav_cb` is wired as the topbar `gear_cb`
(`ui_page_home.c:590`), not a separate home-page widget. Its position is
therefore covered by the topbar derivation, not unknown.

### Table 1 — Topbar icons, every page (y=21 for every icon, universal)

Rightmost icon is always x=454; each position further left is exactly 40px
less. `n` = icon count that page's `ui_topbar_cfg_t` builds.

| Page | Icons (build order, left→right) | n | Centers (x,21) | Source |
|---|---|---|---|---|
| `home` | Warning(indicator, hidden unless triggered), Gear | 2 | Warning (414,21), **Gear (454,21)** | `ui_page_home.c:588-592` |
| `config` | Back | 1 | Back (454,21) | `ui_page_config.c:214-218` |
| `temperature` | Back, Home | 2 | Back (414,21), Home (454,21) | `ui_page_temperature.c:550-554` |
| `network` | Back, Home | 2 | Back (414,21), Home (454,21) | `ui_page_network.c:757-761` |
| `network_manage` | Back, Home | 2 | Back (414,21), Home (454,21) | `ui_page_network_manage.c:557-561` |
| `diagnostics` | Back, Home, Prev, Next | 4 | Back (334,21), Home (374,21), Prev (414,21), Next (454,21) | `ui_page_diagnostics.c:1572-1578` |
| `profiles` (picker, manage mode) | Back, Home, Prev, Next, Add | 5 | Back (294,21), Home (334,21), Prev (374,21), Next (414,21), Add (454,21) | `ui_page_profile_picker.c:469-479` |
| `profile_picker` (non-manage) | Back, Home, Prev, Next | 4 | Back (334,21), Home (374,21), Prev (414,21), Next (454,21) | `ui_page_profile_picker.c:469-476` |
| `profile_detail` | Back, Home | 2 | Back (414,21), Home (454,21) | `ui_page_profile_detail.c:563-567` |
| `profile_segments` | Back, Home, Prev, Next | 4 | Back (334,21), Home (374,21), Prev (414,21), Next (454,21) | `ui_page_profile_segments.c:167-173` |
| `profile_builder_zones` | Back, Home | 2 | Back (414,21), Home (454,21) | `ui_page_profile_builder_zones.c:183-187` |
| `profile_builder_segment` | Back, Home, Prev, Next | 4 | Back (334,21), Home (374,21), Prev (414,21), Next (454,21) | `ui_page_profile_builder_segment.c:358-364` |
| `profile_builder_review` | Back, Home | 2 | Back (414,21), Home (454,21) | `ui_page_profile_builder_review.c:323-327` |
| `touch_test` | Home only (back_page NULL) | 1 | Home (454,21) | `ui_page_touch_test.c:154-158` |
| `touch_cal` | Hand-built "Back" label, NOT via `ui_topbar.c` | — | **UNRESOLVED** — this page predates the topbar module; N/A on this bench anyway (unreachable, see "Panel and camera facts" above) | `ui_page_touch_cal.c:319` |

All icon boxes are 36x26px (`UI_TOPBAR_ICON_W_PX`/`UI_TOPBAR_ICON_H_PX`,
`ui_topbar.h:80-81`), regardless of page.

### Table 2 — `config` page, hub grid (5 cells on this bench; Touch Calibration hidden)

Grid origin (472... wait, left-aligned, not right): col0 x=[8,230] center **119**;
col1 x=[234,456] center **345**. Row0 y=[44,116] center **80**; row1
y=[120,192] center **156**; row2 y=[196,268] center **232**. Cell size
222x72px each.

| Cell (build order) | Grid slot | Center (x,y) | Action |
|---|---|---|---|
| Profiles | row0/col0 | (119,80) | `kiln_ui_show("profiles")` |
| Temperature | row0/col1 | (345,80) | `kiln_ui_show("temperature")` |
| Network / Wi-Fi | row1/col0 | (119,156) | `kiln_ui_show("network")` |
| Diagnostics | row1/col1 | **(345,156)** | `kiln_ui_show("diagnostics")` — matches the 2026-09-19 live measurement exactly, see calibration check above |
| Units toggle | row2/col0 | (119,232) | in-place `unit_pref_set()` toggle, no navigation |

Source: `ui_page_config.c:139-238,247-263` (`build_nav_item`/`build_unit_toggle_item`,
`UI_CONFIG_HUB_GRID_HEIGHT_PX`). Touch Calibration cell would occupy
row2/col1 (232,232) if offered — hidden on this bench build
(`touch_cal_support_is_offerable()` false for the FT6336U).

### Table 3 — `home` page action row (bottom-pinned, fixed heights)

Row y=[276,312], all buttons height 36, center y=**294**. Widths: profile
button flex-grows to fill the leftover space, Pause/Start are fixed 96px.
Content inner width 464px (8..472); fixed total = 96+96+2*4(gaps)=200; profile
button gets 464-200=264px.

| Widget | Center (x,y) | Size (w x h) | Action | Source |
|---|---|---|---|---|
| Profile picker button | (140,294) | 264x36 | `ui_home_profile_btn_cb` → `profile_picker` | `ui_page_home.c:1271-1286` |
| Pause/Resume button | (324,294) | 96x36 | `ui_home_pause_resume_btn_cb` | `ui_page_home.c:1296-1300` |
| Start/Stop button | (424,294) | 96x36 | `ui_home_fire_btn_cb` | `ui_page_home.c:1302-1304` |
| Gear (Menu, topbar) | (454,21) | 36x26 | `ui_home_menu_nav_cb` → `config` (see Table 1) | `ui_page_home.c:588-592` |
| Warning indicator (topbar, relay-life) | (414,21) | 36x26, non-clickable, hidden unless triggered | none — display only | `ui_page_home.c:588-592`, `ui_topbar.c:74-91` |

This resolves row 1's (`home`) two prior UNKNOWNs: the topbar Home icon
question doesn't apply (home has no Home icon, it IS home), and the
"Menu button" reached by the nav graph is the Gear above, not a separate
undiscovered widget.

The gear's tap-target *name*, as `list_tap_targets`/`click_by_name` see it, is
`settings` -- an explicit override (`ui_topbar.c`'s `build_icon_named()`),
not its label text, since an icon-only button's caption is an opaque LVGL
glyph. The old "Menu" button this replaced was removed 2026-08-20.

The topbar's Back and Home icons carry the same kind of override as of
2026-09-24: tap names `back` and `home` (`kUiTopbarBackTapName`/
`kUiTopbarHomeTapName`, `ui_topbar.c`). Before this they had no tap name at
all, so a page like the config hub (`ui_page_config.c`, `show_home=false`)
had no way to navigate back by name -- only by raw coordinates. Both names
are lowercase, matching `settings`'s convention.

### Table 4 — list-page row geometry (fixed row height, page-relative; absolute row identity is data-dependent)

These pages page a live list into fixed-height rows; the *row slot*
geometry is exact, but *which* item lands in which slot depends on runtime
data (profile count/order), so only the slot geometry is source-derived —
matching a specific profile name to a specific y needs a live read of that
list's content.

| Page | Rows/page | Row height | Row centers (x=240, content spans x=[8,472]) | Source |
|---|---|---|---|---|
| `profiles` (picker, manage) / `profile_picker` | 4 | 64px, 4px gap | y: 76, 144, 212, 280 (zero slack — 4*64+3*4=268=`UI_THEME_PAGE_CONTENT_BUDGET_PX` exactly) | `ui_page_profile_picker.h:37-38` |
| `profile_segments` | `ROWS_PER_PAGE` (see file) | 48px (`ROW_HEIGHT_PX`) | Not derived further this pass — list top position after the paging indicator label was not traced | `ui_page_profile_segments.c:30-31,61,181` |

### Unresolved — could not be derived from source this pass

- **`touch_cal` / `touch_test` internal widgets** — N/A regardless (unreachable
  on this bench's FT6336U hardware, per "Panel and camera facts" above);
  `touch_cal`'s topbar is hand-built, not via `ui_topbar.c`, so even its Back
  icon position was not derived.
- **`temperature` page's per-zone rows and relay toggle buttons** — zone rows
  are `LV_SIZE_CONTENT` height (`ui_page_temperature.c:435-436`), so their
  count (`MAX31856_CHANNEL_COUNT`, runtime-visible but not re-confirmed this
  pass) times an unmeasured per-row rendered height determines where the
  fixed-height (40px) relay row starts — not resolved to a pixel.
- **`network` page's "Manage networks" button and mode-toggle buttons'
  vertical position** — `status_card` above them is `LV_SIZE_CONTENT`
  height (wraps live Wi-Fi status text of variable length,
  `ui_page_network.c:772-786`), so `mode_row`/`s_manage_btn`'s y is
  data-dependent; their x-span (full content width, 8..472) and heights
  (44px, 36px respectively) are known, y is not (`ui_page_network.c:820-895`).
- **`network_manage` internal scan/saved-network list** — not inspected this
  pass (out of scope: this row's own MCP action is page-load-only, per the
  runbook's write-gating above).
- **`profile_detail`'s Segments/Edit/Start action row** — `s_info_card` above
  it is `LV_SIZE_CONTENT` (profile name/segment-count text, variable length),
  so the action row's y is data-dependent; not resolved
  (`ui_page_profile_detail.c:570-664`).
- **`profile_builder_zones`/`_segment`/`_review` internal controls** — not
  inspected below the topbar this pass; each page's own zone-selector/
  segment-editor/review-summary widget geometry is unresolved.

### What remains true from the original pass

The two structural facts the original document cited as reasons pixel
geometry "cannot be derived from source alone" (line 367-380) were correct
in general — flex/grid content areas genuinely do vary per-page and
per-data-state — but **understated how much of each page's total tap
surface is actually fixed**: the topbar (every page) and the Configuration
hub grid (the single highest-traffic navigation hub) turn out to be fully
static. The remaining UNRESOLVED items above are the genuinely
data-dependent ones (`LV_SIZE_CONTENT` cards whose height depends on live
text), not a blanket "everything is unknowable" — that blanket claim is now
retracted for the topbar and the config hub specifically, while standing for
the items listed as unresolved above. `touch_log_tap_targets()` remains
forbidden per the original document's own gate (panic fix not confirmed
flashed on the running image) and was not called or considered for this
pass.

## Bench confirmation, 2026-09-21 (M18 LCD class)

The panic fix (`3f86e899`) is confirmed flashed on this image (ESP
`1045e542`), so this pass's coordinator explicitly authorized exactly one
live `touch_log_tap_targets()` call, on the `home` page, to check the
derivation above against the board's own dump. Before this, the panel was
found **screen-blanked** (`touch_get_state()`: idle ~1.94e6 ms) — a plain
wake tap was required before any capture showed page content; this is
ordinary screen-idle behavior, not a defect (see `screen_idle.c`).

**Topbar confirmed, within noise.** The dump's `home`-page Gear icon tap
target was `(436,8)-(471,33)` centre **(453,20)** against this document's
derived **(454,21)** — 1px on each axis, consistent with the exact-arithmetic
derivation and not a real disagreement.

**Home action row (Table 3) needed a correction.** The dump was taken while
the board was idle (no firing), and in that state the Pause/Resume button
is absent from the layout — Table 3 assumed it always present. The actual
tap targets were:

| Widget | Table 3 (derived, assumed 3 buttons) | Bench dump (idle, no Pause/Resume) |
|---|---|---|
| Profile picker button | (140,294), 264x36 | **(189,289)**, region `(8,272)-(371,307)`, 363x35 |
| Start/Stop button | (424,294), 96x36 | **(423,289)**, region `(376,272)-(471,307)`, 95x35 |

Start's x is close (1px) but the profile button's centre is off by 49px on
x and 5px on y — with Pause/Resume absent, the profile-name widget grows to
fill the freed width instead of staying pinned at its 3-button-layout
position, and the whole row sits 5px higher (y=289 vs 294) than derived.
**Table 3 should be read as the layout while a firing is active** (Pause
button present, three elements); the idle-state, two-element layout above
is the one to use for hitting the profile-picker button or Start from
`home` while idle, which covers ordinary bench verification. Whether the
row shifts again once Start is pressed (three elements, still idle-adjacent
during the transition) was not tested this pass.

No other page's tap targets were re-confirmed this pass, in keeping with
this document's "one live call" allowance — the topbar and config-hub
derivations for every other page stand as before, un-re-verified.

## Restore / re-run notes

Every read-only row ends by navigating back toward `home` via topbar Back/
Home (position UNKNOWN per page, same caveat) — no explicit "restore step"
is needed since nothing is written. For the three flagged write-capable
sub-actions, do not run them as part of the mechanical sweep; they need
owner authorization the same way the backend runbook's Class B/C rows do.

## Bench confirmation, 2026-09-21 continuation (M18 LCD class, previously-NOT-RUN pages)

Resolved five of the seven previously-NOT-RUN rows' unresolved geometry above
by measured `touch_log_tap_targets()` dumps (explicitly authorized for this
continuation, same panic-fix precondition as the prior pass). One page
(`profiles_builtin_list`) turned out to be dead code, not a geometry gap —
see the matrix row. Confirmed unreachable (registered in `kiln_ui.c` but
nothing anywhere called `kiln_ui_show("profiles_builtin_list")`) and removed
outright the same day: `ui_page_profiles_builtin_list.c`/`.h` deleted, its
`kiln_ui.c` registration and `CMakeLists.txt` entry dropped, this runbook's
nav map/tables renumbered, and the matrix row updated to "removed, dead
code". One crash defect was found and is documented below and in the matrix
(`profile_builder_segment` row).

**Architecture correction: PICK vs MANAGE mode.** `ui_page_profiles.c` is a
thin alias for `ui_page_profile_picker_build_manage()` — the profile list and
the profile picker are the *same* widget (`ui_page_profile_picker.c`)
differing only by a `manage` flag. Reached from `home` (PICK mode), a row tap
selects that profile and returns to `home` immediately (a WRITE — do not tap
rows here without owner sign-off). Reached from `config` → Profiles cell
(MANAGE mode), a row tap instead navigates read-only to `profile_detail`.
This document's earlier nav map did not distinguish the two paths.

**Topbar icon order is deterministic** (`ui_topbar.c`): left-to-right,
Back, Home, Prev, Next, Warning, Add, Gear — each present only if its
`cfg` field is set. A page's icon x-coordinates can be derived by counting
which flags are set rather than measured, once the icon width/spacing
constants are known; this pass used dumps to confirm rather than derive.

**Measured coordinates this pass:**

| Page | Widget | Coordinates |
|---|---|---|
| `network` | "Manage networks" button | (239,283) |
| `network_manage` | saved network row + Forget button | (407,197) |
| `profiles` (MANAGE mode) | topbar, page 1 (Back/Home/Next/Add, no Prev) | Back (293,20), Home (333,20), Next (413,20), Add (453,20) |
| `profiles` (MANAGE mode) | row tap → `profile_detail` | row centre ~(239,75) for row 1 |
| `profile_detail` | action row | Segments (69,275), Edit (240,275), Start (410,275) |
| `profile_builder_zones` | Next button (nav_row's actual clickable sub-region) | hit-box (384,180)-(471,223), centre **(427,201)** — NOT the nav_row container's overall centre (239,201), which does not register a click |

**Defect found: `profile_builder_zones` → Next crashes the board.** With
zone1 selected (enabling the Next button per its clickable-flag gate), a tap
at the button's true centre (427,201) — not the wider container centre that
was tried first and produced no transition — triggered an `IllegalInstruction`
panic in the `lvgl` task and a `TASK_WDT` reset
(`firmware/KilnFW/App/drivers/ui/ui_page_profile_builder_zones.c`, the
`next_btn_cb` handler wired to `s_next_btn` around line 93/230-237). The
board rebooted cleanly on its own — no manual `debug_reset` or
`safety_clear_trip()` was needed: firmware's own boot-time safety_link logic
detected and cleared the resulting stale S6a trip itself (device log: "boot
was clean but a stale S6a ... trip is still latched from before this boot --
sending clear_trip to release it"). The crash report was left unacknowledged
per this project's read-only rule (`crash_report_ack` never called). This
blocks `profile_builder_segment` and `profile_builder_review` from being
reached without retriggering the crash; both remain NOT RUN pending a fix.
No coredump symbolization (`read_esp_coredump`) was attempted this pass.
