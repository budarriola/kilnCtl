# Web UI Responsive & Modernization Plan

Plan doc for making the KilnFW browser UI work across display sizes and look
current. Browser clients only — the on-board ST7796 / LVGL display is out of
scope here (see section 8).

Conventions, matching `PID_EXPANSION_PLAN.md`: **the code is truth, not the
checkboxes.** Nothing is done until a commit is named.

Status at time of writing (2026-09-02): **Phases 0, 1 and 2 (§4, §5, §6) are
built and committed.** Phase 3 (§7) is owner-gated, not started. See the
status notes at the top of each section below.

Companions: `FLASH_BUDGET_PLAN.md`, `DRAM_PSRAM_PLAN.md`. All three are
independent; none blocks another.

---

## 1. The requirement, and how it has changed

`UI_PLAN.md:44` records the standing owner requirement:

> the webpage should be optimized for a phone or tablet. and the lcd should
> not require scrolling

The current UI satisfies that literally. **The requirement has now been
widened** to supporting different display sizes and a more modern look. That is
a real change of scope, not a bug report: the existing design is not failing at
what it was asked to do, it is being asked to do more. Recorded explicitly so
this plan is not mistaken for a defect list.

---

## 2. What is actually there today (verified inventory)

Several assumptions made before this inventory were wrong. They are corrected
here so they do not get re-adopted later.

### 2.1 There IS a responsive strategy — it is the body cap

Every substantial page sets a `max-width` on `body` with `margin: auto`:

| page | body cap |
|---|---|
| `wifi_provision_page.html` | 420 px |
| `main_page.html`, `zones_page.html` | 480 px |
| `ota_page.html`, `profiles_page.html` | 560 px |
| `safety_commissioning_page.html` | 620 px |
| `readiness_page.html`, `settings_page.html`, `settings_display_page.html` | **none** |

The convention is documented in-source at `backup_page.html:45`,
`diagnostics_page.html:42`, `readiness_page.html:42`, `safety_page.html:29` and
`settings_page.html:43`: "shrinks to fit 320px phones automatically, holds at
640px on anything wider."

**This is the thing to change, and it is the whole problem in one line.** The
UI is a single fluid column capped at 420–620 px and centred. It is correct on
a phone and correct on a tablet. On a 1280 px or 1920 px display it is a narrow
ribbon in the middle of an empty screen. "Support different display sizes"
means, concretely: earn the horizontal space above the cap.

The three pages with no cap at all are a separate inconsistency worth fixing in
the same pass.

### 2.2 The per-page `:root` blocks are NOT identical

An earlier reading of this claimed 13 copies of one block, and that a
copy-paste hoist into `theme.css` would be a pure deletion. **That is wrong.**
Three incompatible status-colour vocabularies coexist:

- `--ok` / `--warn` / `--bad` (some pages spell it `--err`) — diagnostics,
  safety_page, safety_commissioning, zones, ota, profiles
- `--on-color` / `--off-color` / `--fault-color` — `main_page.html` only
- `--cannot-yet` / `--not-done` / `--off` — `readiness_page.html` only, found
  nowhere else

`--fault-color` appears in backup, diagnostics, main, safety_commissioning and
settings, but is **aliased to a different underlying token per page** (in
diagnostics it maps to that page's own `--warn`).

Common core across all 13: `--bg`, `--border`, `--button-bg`, `--card-bg`,
`--fg`, plus `--muted` on all but `wifi_provision_page.html`.

`theme.css:10-19` already documents this honestly — it says only
byte-identical rules were hoisted and the per-page blocks were deliberately
left local. **The consolidation is therefore a rename pass, not a deletion
pass**, and it is the largest single piece of work in this plan. Sizing it as a
deletion would be the main way this plan goes wrong.

### 2.3 Shared layout machinery already exists — build on it

- **`nav.js`** (400 lines) injects `.kc-topbar` as the first child of `<body>`
  on every page, builds the drop-down menu overlay from a central `NAV_LINKS`
  table (`nav.js:72-114`), and exposes `window.kcNav.updateBodyPadding` as the
  seam `app.js` uses. Chrome is already centralized; a responsive shell should
  extend `buildTopbar()`, not re-architect page structure.
- **`app.js`** (689 lines) provides `kcEscapeHtml`, `kcConfirm`,
  `kcFetchWithSafetyAck`, the `kcUnit` °C/°F preference layer, the
  connection-lost banner, and the sticky Stop/Pause bar with a `ResizeObserver`
  keeping body padding correct.
- **`theme.css`** (357 lines) already styles topbar, menu overlay, stop bar,
  connection banner, `.card`, `.table-scroll`, relay tiles and danger buttons.

None of this needs replacing. The gap is layout and scale, not structure.

### 2.4 Layout primitives

`flex` + `flex-wrap` is the dominant mechanism (16 `flex-wrap` rule sites
across 8 files) and is already doing the responsive work below the cap.
`display:grid` appears only twice — `backup_page.html:77` (already
`repeat(auto-fit, minmax(180px,1fr))`, i.e. the target pattern) and
`ota_page.html:67`. Fixed-px layout is *not* widespread: beyond the body caps
there are only isolated `min-width` floors (`profiles_page.html:69`,
`safety_commissioning_page.html:178`). `overflow-x:auto` exists in exactly two
places (`theme.css:84` `.table-scroll`, `diagnostics_page.html:131`).

Good news for the plan: there is little fixed-pixel layout to demolish.

---

## 3. Hard-won constraints this plan must not regress

Each of these is a fixed bug recorded as a source comment. A generic
"modernize the CSS" pass would undo several of them.

1. **`zones_page.html:79-81`** — a headless-Chromium sweep found
   `document.scrollWidth` reaching 474 px inside a 320 px viewport. Horizontal
   overflow on phones is a real, previously-shipped bug here.
2. **`zones_page.html:163,181`** — a 1280 px sweep found `offsettle[2]`
   overlapping `saveBtn`. **Wide-viewport regressions are as real as narrow
   ones**, which matters a great deal for a plan whose whole purpose is to
   start using width above 620 px.
3. **`profiles_page.html:80,257`** — at 390 px, catalogue "Use/Save" targets
   measured 81×19, under the 32 px minimum.
4. **`diagnostics_page.html:111-114`** — at 320 px, buttons landed on top of
   each other at the flex wrap point.
5. **`safety_commissioning_page.html:166,185`** — a bool-field label wrapped
   badly at ~360 px.
6. **`overflow-wrap: anywhere` vs `break-word`** was tuned per column
   (`diagnostics_page.html:82-102`, `safety_page.html:53-77`) so unbreakable
   tokens — fault reasons, IDs — break without wrecking normal word wrap. Easy
   to flatten by accident.
7. **The 72 px touch target is LCD parity, not a mistake.** `theme.css:21-26`
   sets `--ui-touch: 72px` and `button { min-height: var(--ui-touch) }`, sized
   for gloved-hand panel use per `theme.css:120-129` and `UI_THEME.md`. The web
   topbar deliberately uses 40 px instead. An earlier reading called 72 px
   "panel-sized and wrong on desktop" — that was wrong; it is deliberate, and
   `UI_THEME.md`'s web/LCD parity rule depends on it. **Any change here needs
   the owner's decision, not a unilateral clamp.**

---

## 4. Phase 0 — a real test matrix (blocking prerequisite) — DONE

Built as `App/test/ui_responsive_sweep.mjs` + `check_ui_responsive_sweep.ps1`
(`ef101b1`), wired into `run_all_checks.ps1`. Baseline 20/78 (page, width)
failures.

**Correction: most of the baseline red was the sweep's own bug, not page
bugs** (`64e160f`). The viewport height was measured before the sweep's setup
script forced the Stop bar visible, so the fixed-position bar read as
overlapping content it hadn't actually reached; and a closed `<details>` is
hidden via `content-visibility` in Chromium, which the visibility check
misread as on-screen. Both fixed in the sweep itself. The earlier claim that
`6f5c15a`'s stop-bar fix was incomplete was wrong — that fix was fine.

Three genuine page bugs remained and were fixed in the same commit:
`wifi_provision_page.html` `#apSection` had no default-hidden rule, so both
Wi-Fi forms rendered until the first `/status` poll resolved; touch-target
heights under 32px on `safety_config_page.html` `#pcLink`,
`safety_commissioning_page.html` `<summary>`, and `backup_page.html`
`#restoreFile`.

Sweep is now 78/78 green.

Section 3 exists because someone ran headless-browser sweeps and wrote down
what broke. Those sweeps were ad hoc and their results survive only as
comments. This plan will change layout on every page, at widths the UI has
never targeted before. Doing that without a repeatable check will reintroduce
exactly the bugs section 3 lists.

**Build the sweep as a tool before changing any CSS.** Requirements:

- Widths **320, 360, 390, 768, 1280, 1920**. The first five are the widths
  already used in the recorded sweeps; 1920 is new and belongs to this plan's
  own goal.
- Per page, assert: `document.scrollWidth <= viewport width` (no horizontal
  overflow); no overlapping interactive elements; every interactive target
  meets its minimum size; no element clipped out of the viewport.
- Runnable against the real board or a local static serve of the pages.
- Emits a pass/fail table, so "did this break anything" is one command.

**Negative-test it before trusting it.** Point it at a deliberately broken page
and confirm each assertion actually fails. This repo has shipped vacuous checks
before; a green sweep that cannot go red is worse than no sweep, because it
licenses the rest of the plan.

Record the baseline result in this doc.

---

## 5. Phase 1 — token consolidation (the real work) — DONE

All 13 pages converted, one commit each where a page needed a change
(`8e8db7f`, `a1364b4`, `369f593`, `a09bbfb`, `c27b43f`; the rest were already
canonical).

**Two items need an owner decision — still open:**

1. `safety_commissioning_page.html`'s `--fault-color` and `--bad` are
   numerically identical but the page treats them as deliberately distinct
   concepts.
2. `main_page.html`'s `--off-color` and `readiness_page.html`'s
   `--cannot-yet` are neutral/grey "inactive/unknown" states with no honest
   slot in the ok/warn/bad vocabulary — it may be one token short.

Below is the shape the pass followed, kept for reference:

1. Choose one status vocabulary. `--ok` / `--warn` / `--bad` is already the
   majority spelling and should win.
2. In `theme.css`, define the full semantic set once — the common core plus the
   chosen status names plus the input/link tokens — for light, for
   `prefers-color-scheme: dark`, and for `[data-theme]` override, mapped onto
   the existing `--ui-*` palette so `UI_THEME.md`'s parity rule still holds.
3. Migrate pages one at a time, rewriting `--on-color`/`--off-color`/
   `--fault-color`/`--cannot-yet`/`--not-done`/`--err` uses to the chosen
   names, and deleting that page's local `:root` blocks.
4. `main_page.html` and `readiness_page.html` are the two genuinely divergent
   pages and should go last, once the shared block has settled.

One page per commit, sweep after each. Do not batch — this is 13 pages of
find-and-replace across 670 kB of source, and a missed rename fails as an
invisible unstyled colour, not as an error.

Deliverable: `theme.css` becomes the single place a colour or size decision is
made. Everything after this is cheap; nothing before it is.

---

## 6. Phase 2 — earn the width above the cap — DONE

`theme.css` gained `--kc-shell-max: clamp(480px, 90vw, 1100px)` and
container-query rules on `.card`; all 13 pages converted. `main_page.html`
`#channels` and `zones_page.html` `#zones` became
`repeat(auto-fit, minmax(260px,1fr))` grids — a 320px floor was tried first
and caught overflowing a 320px viewport, so 260px is what shipped. New lint
`check_ui_shell_layout.ps1` guards 18 rules; `run_all_checks.ps1` is now 22
checks total.

Notes below kept for reference; only after Phase 1, because every rule here
wants to be written once.

- **Replace the per-page body cap** with a shared shell that widens in stages
  rather than stopping at 480 px. The cap must not simply be removed — full-
  width text lines at 1920 px are worse than the ribbon. Content stays
  readable-width; *layout* gets to use the space.
- **Container queries** (`@container`) on `.card` and the dashboard tiles, so a
  card reflows on its own width and works in a 1-up phone column and a 3-up
  desktop grid from one rule. Baseline-supported since 2023; no JS, no library.
- **`repeat(auto-fit, minmax(…, 1fr))`** for the page shells — column count
  becomes automatic and no breakpoint list is needed.
  `backup_page.html:77` already demonstrates the pattern in this codebase.
- **`clamp()`** on spacing and type tokens for continuous scaling.
  **Excluding `--ui-touch`** — see 3.7; that one is an owner decision.
- Keep `flex-wrap` where it already works. It is not broken and ripping it out
  buys nothing.

`main_page.html` and `zones_page.html` benefit most — they are the dashboards,
and they are the two pages with the tightest 480 px cap.

## 7. Phase 3 — modern look

Cheap once Phase 1 lands, because it is edits to one file: a real spacing
scale, one dominant accent rather than five equal-weight ones, layered-shadow
elevation, a tighter type scale, `color-mix()` for hover/disabled states
instead of hardcoded hexes, and `light-dark()` to collapse the paired
declarations.

Constraint: `UI_THEME.md`'s parity rule says the web mirrors `ui_theme.h`'s hex
values. A palette change is therefore an **LCD change too**, or an explicit,
documented decision to break parity. Settle that before repainting.

---

## 8. On frameworks — not now, with a stated re-open condition

**No framework in Phases 0–3.** The blocker is token duplication and the body
cap (2.1, 2.2). Neither is a rendering problem, so a renderer does not fix
them. Tailwind would mean rewriting class names across 670 kB of source; Pico
or Bootstrap would fight `theme.css`, which is deliberate and documented.

Flash is not the objection — the app slot has 1.22 MB free
(`FLASH_BUDGET_PLAN.md` §1) and Preact-with-signals or Lit is ~5–6 kB gzipped.

**Re-open the question when, and only when,** the dashboards still hurt after
Phase 2. The candidates are `main_page.html` (54,936 B gzipped, 17 `fetch`
sites) and `zones_page.html` (47,767 B gzipped, 18 `fetch` sites) — live-data
views, the shape a reactive renderer genuinely improves. Then: **one page as a
pilot**, measured against the Phase 0 sweep and the flash/heap baselines,
before anything else moves. Never a whole-UI migration on principle.

An esbuild bundling step is worth doing *with* a framework pilot and not before
— on its own it optimizes source that Phase 1 is about to rewrite.

---

## 9. Out of scope

- The on-board ST7796 / LVGL display. Different renderer; none of this CSS
  reaches it, and its no-scroll constraint needs its own layout pass.
- Auth/session and TLS — deferred in `UI_PLAN.md`, explicitly not authorized.
- The shared-poller refactor deferred in `app.js:8-17`. Related, but it is a
  data-flow change, not a layout one.

---

## 10. Incidental findings

- **`WEB_UI.md` is stale on pages and routes.** Its route table (lines 91-98)
  lists 5 pages and states "there is no other navigation"; 13 pages exist and
  `nav.js` drives cross-linking. Its API section remains the source of truth —
  the page/route section should be corrected or deleted rather than left to
  mislead.
- **Four orphaned build artifacts.** `build/esp-idf/drivers/` holds 17
  `*_page.html.gz` against 13 sources: `board_temps_page.html.gz`,
  `manual_page.html.gz`, `rules_page.html.gz` and `thermo_faults_page.html.gz`
  have no corresponding source file. Stale output from deleted pages. Harmless
  — `EMBED_TXTFILES` only embeds what the CMake list names — but worth
  confirming they are genuinely unreferenced rather than silently still linked.
- **Three pages have no body cap** (`readiness`, `settings`,
  `settings_display`) while the other six use four different values
  (420/480/560/620). Phase 2 replaces all of it, but the inconsistency is
  itself evidence the cap was never a considered global decision.
