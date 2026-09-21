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
(crop is `X=296 Y=58 W=853 H=578`, i.e. an 853x578 window, not 480x320 — the
capture is a photograph of the panel, magnified, not a framebuffer dump).

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
profiles --[builtin section row]--> profiles_builtin_list --[row tap]--> profile_detail
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

### 11. `profiles_builtin_list` — READ-ONLY
- Source: `ui_page_profiles_builtin_list.c`. Purpose: built-in profile list.
- Nav: reached from `profiles`' builtin section (row tap in
  `ui_page_profiles.c`, exact callback not traced this session — grep found
  the destination call in `ui_page_profiles_builtin_list.c:111` going the
  other way, not the entry button). Entry point pixel geometry: **UNKNOWN**.
- Verification region: **UNKNOWN**.
- Backend read-back: `profiles_list()`'s builtin subset.

### 12. `profile_detail` — READ-ONLY
- Source: `ui_page_profile_detail.c`. Purpose: view one profile.
- Nav: `profile_picker` --[row tap]--> `profile_detail`
  (`ui_page_profile_picker.c:247`); also `profiles_builtin_list` --[row
  tap]--> `profile_detail` (`ui_page_profiles_builtin_list.c:111`). Pixel
  geometry: **UNKNOWN** (row position depends on list scroll/index).
- Verification region: **UNKNOWN**.
- Backend read-back: `profiles_get()` (`GET /api/profile`, A10) for the same
  profile id — compare segment count/zone targets shown.

### 13. `profile_segments` — READ-ONLY
- Source: `ui_page_profile_segments.c`. Purpose: segment list.
- Nav: `profile_detail` --[Segments button, `ui_page_profile_detail.c:443`]--> `profile_segments`.
  Pixel geometry: **UNKNOWN**.
- Backend read-back: `profiles_get()`'s segment array.

### 14. `profile_builder_zones` — WRITE flow entry, owner-gated for actual
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

### 15. `profile_builder_segment` — page-load READ-ONLY, same caveat as #14
- Source: `ui_page_profile_builder_segment.c`.
- Nav: `profile_builder_zones` --[Next, `ui_page_profile_builder_zones.c:100`]--> `profile_builder_segment`.
  Pixel geometry: **UNKNOWN**.

### 16. `profile_builder_review` — page-load READ-ONLY, same caveat as #14
- Source: `ui_page_profile_builder_review.c`.
- Nav: `profile_builder_segment` --[Next,
  `ui_page_profile_builder_segment.c:176`]--> `profile_builder_review`.
  Pixel geometry: **UNKNOWN**. Its own Save button
  (`ui_page_profile_builder_review.c:124`) is the WRITE action noted in #14
  — do not tap it in this class.

---

## Summary by class

| Class | Rows |
|---|---|
| Read-only (page-load navigate + numeric-sample + backend read-back) | `home`, `config`, `temperature`, `network` (load only), `diagnostics`, `profiles`, `profile_picker` (load only), `profiles_builtin_list`, `profile_detail`, `profile_segments`, `profile_builder_zones` (load only), `profile_builder_segment` (load only), `profile_builder_review` (load only) — 13 |
| Write (owner-gated, not part of this read-only pass) | `network_manage` connect/forget actions; `profile_picker` pick-and-return; the builder chain's terminal Save (`profile_builder_review`) — 3 flagged sub-actions on top of the 13 rows above |
| N/A on this bench hardware | `touch_cal`, `touch_test` — 2 |

16 registered LCD pages total, matching `docs/COMMISSIONING_TEST_MATRIX.md`'s
count.

## Rows whose tap geometry could not be derived from source

All of them except the two noted with a prior live measurement
(`config` hub's Diagnostics cell, and that page's own topbar icons) — i.e.
**14 of 16 pages'** entry-tap coordinates are UNKNOWN from static analysis
alone, for the structural reason in "What this document is" above (flex/grid
layout, not fixed positions; topbar icon sets vary per page). Both of the
two exceptions carry an unresolved coordinate-space caveat (capture-crop
pixels vs. native LVGL screen pixels) that also needs live confirmation.
**A single, isolated, board-owner-supervised `touch_log_tap_targets()` call
per distinct page** (once its panic fix is confirmed flashed and separately
verified safe) is what would resolve all of these at once, per that tool's
own docstring — this was explicitly out of scope for this read-only,
camera-only pass.

## Restore / re-run notes

Every read-only row ends by navigating back toward `home` via topbar Back/
Home (position UNKNOWN per page, same caveat) — no explicit "restore step"
is needed since nothing is written. For the three flagged write-capable
sub-actions, do not run them as part of the mechanical sweep; they need
owner authorization the same way the backend runbook's Class B/C rows do.
