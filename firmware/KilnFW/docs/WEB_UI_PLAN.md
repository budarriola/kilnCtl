# Web UI plan — zones page clean-up, Chart.js, display-power Save

Status: opened 2026-09-06 from an owner request; survey facts at `05087f0`.

## 1. Zones page: explanations behind an info control

`zones_page.html` builds every zone card from `zoneBlock()` /
`settingsStackHtml` (lines 769-1041). The explanatory paragraphs and tables
(three relay-timing hints, cross-zone plausibility text, guard-threshold
notes, PID/fuzzy notes) are ~5.5 KB of markup per Custom zone, so three
Custom zones render ~16 KB of prose. Sections stay; the prose moves behind
a disclosure.

- Use the page's existing `<details>/<summary>` idiom (already used for
  `advguards` here and on three other pages) rendered as a small round
  "i" glyph next to each section heading: `<details class="info"><summary
  aria-label="About …">i</summary>…</details>`. Add `.info` styling to
  `theme.css` (glyph in the muted colour, open state shows the prose in a
  `.hint` block). No new colour, no new library, keyboard/touch friendly.
- Every field keeps a one-line label; anything longer than one line moves
  into the `.info` block. Tables (guard thresholds, timing presets) go the
  same way.
- One shared `infoHtml(key)` map so the text exists once in the file; the
  summary/ open state is remembered per section in `localStorage` so an
  operator who wants the prose visible sees it on every zone.

## 2. Zones page: "same as zone N" per group

A whole-stack `settings_source` per zone already exists (PID plan §3.5,
`zones_page.html:1124-1298`, stored as a real versioned field, resolved at
read time, cycle-safe). It mirrors the entire stack — TC type, control mode,
PID, ramp limits, max/min temp, relay timing, guards — or nothing. The
owner wants the choice per item. Extend the same reference model, do not
copy values at save time:

| Group | Fields | Mirrorable |
|---|---|---|
| limits | `max_temp_c`, `min_temp_c`, max ramp rate, sanity/min-rise rate | yes |
| relay timing | window, min-on, min-off | yes |
| control | control mode, Kp/Ki/Kd, fuzzy strength | yes |
| guards | the eight `advguards` thresholds, cross-zone plausibility | yes |
| tc | tc_type, cal offset | tc_type yes, cal offset **no** (per sensor) |
| measured | `model_k_dc/tau_s/dead_time_s`, coupling cells, normals | **never** (identified, not policy) |
| topology | name, `relay_mask`, `thermo_mask`, `ct_mask`, `relay_type` | **never** |

- Schema: replace the single `settings_source` byte with
  `settings_source[SRC_GROUP_COUNT]` (5 bytes, 0xFF = custom), migration
  v20→v21 copying the old byte into every group. `zones_config_accessors.c`
  resolves each group through the existing chain logic.
- UI: each group heading gets the existing `<select class="settingssrc">`
  ("Custom" / "Same as zone K"); the old whole-zone select becomes a
  shortcut that sets all five. Inherited groups collapse to the existing
  read-only `.inheritedSummary`.
- Host test: chain resolution per group, cycle collapse, migration from a
  v20 blob with `settings_source = 1` gives five 1s.

**Status (2026-09-06, Opus review of `5672719`):** landed. Two follow-on
notes from that review:

- **`timing_profile` inheritance narrowed.** Before this pass, a zone's
  effective `timing_profile` came from the whole-zone `settings_source`
  terminal (the zone it ultimately resolves to, following the chain). After
  the per-group split, `timing_profile` is not one of the five groups above
  and is always read as the zone's own stored value, never inherited. A
  board that upgrades from v20 with zone 1 set to "same as zone 0" keeps
  reading zone 0's `timing_profile` for zone 1 until the first post-upgrade
  save of zone 1 — at that point zone 1's own (till-then-inherited, now
  merely stale) stored value takes over, which can be a silent behavior
  change if the two zones' stored `timing_profile` values had drifted apart
  while zone 1 was mirroring zone 0. This is an intentional effect of the
  migration, not a defect, but is easy to miss when reading a v20 board's
  post-upgrade behavior.
- **Test provenance.** The host tests for this section landed in `09769f5a`
  (a concurrent-session sweep unrelated to this feature), not in `5672719`
  itself — so `09769f5a` and its sibling commit `51c084f9` are the ones that
  actually compile the zones host tests for the first time after the
  `settings_source[SRC_GROUP_COUNT]` schema bump; `5672719` alone does not.

## 3. Chart.js — assessed, not adopted

Dashboard graph is hand-rolled Canvas 2D in `main_page.html` (~line 391):
per-zone traces, planned/setpoint trace, duty, guard-trip and freeze
markers, 15 s polling of `/api/history.csv`, legend deliberately in DOM
(2026-08-21) so it scales on phones. It lacks only zoom/pan — which
Chart.js also lacks without `chartjs-plugin-zoom`.

Cost: `chart.umd.min.js` 4.5.x ≈ 200 KB raw / ≈ 60 KB gz, a ~21 % increase
on the 286 KB gz of embedded web assets, served by the same
`esp_http_server` path that has already truncated `/app.js` under internal
DRAM pressure. The LCD graph is LVGL and unrelated. Decision: **no**; if
zoom/pan is wanted, add a ~40-line pointer-drag window to the existing
canvas code instead.

## 4. Display-power Save button — already done

`settings_display_page.html:56-64` records the owner's 2026-09-04 report
and the fix: the card now uses the shared `.card` and plain `<button>`
idiom every other Save button uses; saving POSTs `/api/settings/display_power`
and persists `display_power` in NVS. The fix is in the flashed `05087f0`
image. If it still looks wrong on the bench it is a stale browser cache
or a different page — needs the owner's eyes, nothing open in code.

## Order

§1 first (pure front-end, no schema), then §2 (schema v21, after the
relay-type bump to v20 in `docs/RELAY_LIFE_BUDGET_PLAN.md` so the two
migrations do not collide). Both end with `build_kilnfw` and a bench
check on a phone-width viewport.
