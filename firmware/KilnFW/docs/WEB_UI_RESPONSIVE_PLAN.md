# Web UI Responsive & Modernization Plan

Plan doc for making the KilnFW browser UI work across display sizes and look
current. Browser clients only — the on-board ST7796 / LVGL display is out of
scope here (see section 8).

Conventions, matching `PID_EXPANSION_PLAN.md`: **the code is truth, not the
checkboxes.** Nothing is done until a commit is named.

Status at time of writing (2026-09-02): **Phases 0, 1 and 2 (§4, §5, §6) are
built and committed.** Phase 3 (§7) is owner-gated, not started -- §7 below is
now a decision-ready proposal, not applied CSS. §3 items 2 and 4 (previously
recorded as "does not reproduce under the sweep") are now covered; see §4.
Two token-vocabulary questions are open for the owner, made decision-ready in
§5.1. See the status notes at the top of each section below.

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

### 2.1 There WAS a responsive strategy — it was the body cap

**Superseded by §6:** all 13 pages now set
`max-width: var(--kc-shell-max)` (`clamp(480px, 90vw, 1100px)`). The
pre-Phase-2 inventory below is kept only because it explains what the
problem was.

Every substantial page used to set its own `max-width` on `body`:

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

**This was the thing to change, and it was the whole problem in one line.** The
UI is a single fluid column capped at 420–620 px and centred. It is correct on
a phone and correct on a tablet. On a 1280 px or 1920 px display it is a narrow
ribbon in the middle of an empty screen. "Support different display sizes"
means, concretely: earn the horizontal space above the cap.

The three pages with no cap at all were a separate inconsistency, fixed in the
same pass.

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

**§3 items 2 and 4 coverage (2026-09-02):**

- **Item 4** (`diagnostics_page.html:111-114`, buttons landing on top of each
  other at the 320px flex-wrap point) no longer applies to the current
  source: that markup was already rewritten into a stacked `.linklist` block
  (see the file's own 2026-08-22 comment above `.linklist`), which the
  general overlap/undersized-target assertions already exercise at every
  width like any other page content. Nothing to add.
- **Item 2** (`zones_page.html:163,181`, `input.offsettle` overlapping
  `button#saveBtn` at 1280px) *was* a genuine gap, but not for the reason
  first assumed. `#zones` never gets populated under the sweep's static
  server -- `loadCurrent()`'s `fetch('/api/zones')` 404s, so `renderZones()`
  never runs and the page's core content (every zone card, every input in
  it) was never checked at any width. No mock server was needed to fix this:
  `renderZones()` doesn't require a real response -- `current` defaults to
  `{ zones: [] }` and each missing zone index already falls back to a
  hardcoded default object inside the function itself. `ui_responsive_sweep.mjs`
  gained a `PAGE_FIXTURES` map (currently just this one page) that sets
  `thermoCount`/`relayCount` and calls `window.renderZones()` directly,
  before the generic setup step. That generic step was also changed to skip
  `<details class="advguards">` specifically (every other `<details>` is
  still forced open) so the closed state the original bug was reported in is
  the state actually checked, not overwritten.
  - Negative-tested: with the fixture in place, temporarily giving `.zone` a
    2000px `min-width` produced 20 `[clipped]` failures at 1280px -- content
    the old, always-empty `#zones` fixture had no way to ever flag. Reverted
    after confirming red.
  - The specific historic Chromium quirk the 2026-08-22 CSS fix
    (`details.advguards:not([open]) > *:not(summary) { display: none; }`)
    targets -- a closed `<details>`'s non-summary children still getting a
    real, non-zero layout position -- does **not** reproduce on this
    toolchain's installed Chrome/Edge: measured `.offsettle` rects inside a
    closed, unfixed `.advguards` are `0x0` regardless of whether the fix CSS
    is present. That specific regression can no longer be exercised by
    mutation on this machine; recorded here rather than forcing a test that
    can't actually fail. The fixture is still a real, previously-total,
    coverage improvement for this page's content independent of that one
    historic bug, per the negative test above.

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

### 5.1 Two items need an owner decision — still open

**1. `safety_commissioning_page.html`'s `--fault-color` and `--bad`.**
Numerically identical today (both `#c0392b` light, both `var(--ui-accent-5)`
dark — `theme.css:391-394` and `safety_commissioning_page.html:45-69`), but
the page's own comment (`safety_commissioning_page.html:46-53`) is explicit
that they are meant to be different concepts: `--bad` is "this reading is a
warning/error", `--fault-color` is "the data on screen cannot be trusted
right now" (a `body.kc-cfg-linkdown` state, a CRC mismatch banner, an armed
write-window). Checked what the other pages that define `--fault-color` do,
since that's the closest thing to a house convention: `backup_page.html:21-33`
and `settings_page.html:28-40` both alias it straight to their own `--bad`
— the same choice safety_commissioning made. `diagnostics_page.html` is the
one page that looks different (`--fault-color: var(--warn)`,
`diagnostics_page.html:16-25`), but that page also spells its own "this is an
error" red as `--warn` (`--warn: #b00`, no `--bad` defined at all —
`diagnostics_page.html:15`, and the plan's own §2.2 flagged this exact
"some pages spell it `--err`" naming divergence already). So once the naming
inconsistency is set aside, **every page that has a `--fault-color`,
safety_commissioning included, points it at whichever token that page uses
for danger-red** — safety_commissioning is the *majority* pattern, not the
outlier. The numeric identity is not an accident to fix; it is what the rest
of the codebase already does. **Recommendation, pending owner sign-off:**
leave the mapping as-is. If the owner still wants the two concepts to look
visually distinct (danger-red for "this reading is bad" vs. a second color
for "this data cannot be trusted"), the smallest change that does not invent
a new hex is remapping `--fault-color` to a *different existing* accent —
`--ui-accent-1` (`#e8974e`, orange, already used elsewhere as an
attention-but-not-danger color) reads as "caution/uncertain" without
competing with red's "stop/error" meaning. That is still an LCD-parity
question (§7 below), not a web-only tweak, because `--fault-color` ultimately
resolves to one of the shared `--ui-accent-*` tokens either way.

**2. A fourth status token — `main_page.html`'s `--off-color` and
`readiness_page.html`'s `--cannot-yet`.** Both are neutral/grey
"inactive/unknown" states with no honest slot in `--ok`/`--warn`/`--bad`, and
both already resolve to the exact same underlying value:
`--ui-text-secondary` (`#9aa0ae`) in dark mode on both pages, `#888` in light
mode on both pages (`main_page.html:26,32,38`, `readiness_page.html:24,31,38`).
This is not a coincidence needing a decision on *what value* to use — that
part is already settled by two independent pages agreeing — only on whether
to formalize it as a fourth named member of the shared status vocabulary.

*Is `--muted` already this token?* No — checked directly:
`--muted` resolves to `#444` in light mode across all 13 pages
(`theme.css`-consuming pages'  own `:root` blocks, e.g. `main_page.html:25`)
but `--off-color`/`--cannot-yet` resolve to `#888` in light mode on the same
pages. Same dark-mode value, different light-mode value: `--muted` is tuned
for legible de-emphasized *text* (labels, captions), `--off-color`/
`--cannot-yet` for a de-emphasized *status indicator*, a different design
intent that happens to share one of its two mode values. `--muted` cannot
quietly absorb this role without a light-mode contrast regression on
whichever page adopts it.

**Recommendation: add `--neutral`**, parallel in form to `--ok`/`--warn`/
`--bad` (a one-word adjective naming a state, not an element), defined once
in the shared vocabulary as `--neutral: #888` (light) /
`var(--ui-text-secondary)` (dark) — a pure rename of values that already
exist and already agree, not a new color decision. On adoption: `main_page.html`
renames `--off-color` → `--neutral` (the relay/zone "not currently active"
indicators), `readiness_page.html` renames `--cannot-yet` → `--neutral` (the
"can't check yet" commissioning-item marker); no other page currently has an
unmet need for it, but it becomes the shared name any future "no honest
ok/warn/bad answer" state should reach for instead of reinventing its own
grey.

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

**2026-09-02 follow-up on item 2:** `--neutral` has been added to
`main_page.html` and `readiness_page.html`'s three `:root` blocks each
(light, `prefers-color-scheme: dark`, `[data-theme="dark"]`), set to the
exact same values `--off-color`/`--cannot-yet` already hold on those pages.
Nothing was switched over to it — `grep -rn "var(--neutral)"` across
`App/drivers/` returns no hits, so no rendered color changed; the sweep
(78/78) and `run_all_checks.ps1` (26/26) confirm this. This is prep only:
the rename from `--off-color`/`--cannot-yet` to `--neutral` is still the
owner's call, same as item 1.

---

## 6. Phase 2 — earn the width above the cap — DONE

`theme.css` gained `--kc-shell-max: clamp(480px, 90vw, 1100px)` and
container-query rules on `.card`; all 13 pages converted. `main_page.html`
`#channels` and `zones_page.html` `#zones` became
`repeat(auto-fit, minmax(260px,1fr))` grids — a 320px floor was tried first
and caught overflowing a 320px viewport, so 260px is what shipped. New lint
`check_ui_shell_layout.ps1` guards 18 rules and runs from
`run_all_checks.ps1`, which discovers its check list rather than carrying a
count — do not restate the count here, it moves every time a check lands.

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

## 7. Phase 3 — modern look (decision-ready proposal, NOT applied)

Owner-gated per `UI_THEME.md`'s web/LCD parity rule (§3 item 7): any accent
change is an LCD change too. Nothing below has been painted onto any page.
This is the concrete proposal to sign off on or amend.

### 7.1 What is actually there today

`theme.css:23-24` defines five equal-weight accents (`--ui-accent-1`
orange … `--ui-accent-5` red), mirroring `ui_theme.h`'s per-zone/per-metric
coding scheme 1:1. Checked how they're actually *used* in the shared chrome
(`theme.css`, excluding page-local zone-color-coding uses, which are a
different, legitimate use of the same palette):

| Token | Used for |
|---|---|
| `--ui-accent-1` (orange) | `.kc-pause-btn` background; `--warn` on most pages |
| `--ui-accent-3` (teal) | `.kc-menu-panel a.kc-menu-active` only |
| `--ui-accent-5` (red) | `.kc-stop-btn`, `.kc-conn-banner`, the stop-bar top border; `--bad` on most pages |
| `--ui-accent-2`, `--ui-accent-4` | not used in shared chrome at all (page-local zone coding / `--ok` only) |

No single accent currently means "this is the primary action" the way a
one-brand-color design would use one. Ordinary buttons (`theme.css`'s base
`button` rule) use `--button-bg`, a neutral grey — visually flat against
danger/pause, which already have color.

### 7.2 The proposal

**No new hex values.** Every color below already exists in `ui_theme.h` /
`theme.css`; this is a *usage* change (which existing accent means what),
not a palette change:

1. **One dominant accent: `--ui-accent-3` (teal, `#3ec6c6`).** It is
   currently the least-claimed of the five (one rule, the nav active-link
   underline) and does not already carry a safety meaning the way accent-1
   (pause) and accent-5 (stop/danger) do. Proposal: `theme.css`'s base
   `button` rule (and page-local "primary" buttons — Save, Start, Connect —
   which today just inherit the neutral `--button-bg`) get a `--ui-accent-3`
   border/text treatment on focus and on the single "primary" button per
   view, so there is one recognizable "this is the button to press" color
   site-wide instead of every button reading the same grey. Accents 1/2/4/5
   keep their current jobs unchanged (pause, per-zone coding, ok, danger) —
   this does not touch any status-color decision from §5.1.
2. **Spacing scale.** Add `--ui-space-1: 4px` … `--ui-space-5: 32px` to
   `theme.css:21` alongside the existing `--ui-padding: 8px` (which becomes
   an alias, `--ui-padding: var(--ui-space-2)`, so nothing existing breaks).
   Pages currently hardcode `0.3em`/`0.6em`/`0.9em`/`1em` spacing ad hoc;
   this gives new rules a consistent scale to reach for without forcing a
   rewrite of every existing `em` value.
3. **Layered-shadow elevation**, `color-mix()`-based so it works in both
   themes with one rule: `--ui-shadow-1: 0 1px 2px color-mix(in srgb, var(--ui-bg) 60%, black 40%)`
   for resting cards, `--ui-shadow-2` (larger blur/offset) for the topbar and
   any future hover-raised state. Purely additive — `.card` currently has no
   shadow at all, just a border.
4. **`color-mix()` for hover/disabled states**, replacing spots like
   `.kc-pause-btn { background: var(--ui-accent-1, #468); }`'s hardcoded
   `#468` fallback and any other ad hoc hover-darken hex, with e.g.
   `color-mix(in srgb, var(--button-bg) 85%, var(--ui-text-primary) 15%)`.
   No visual palette change, just removing hand-picked one-off hexes that
   don't track the token they're supposed to be a variant of.
5. **`light-dark()`** to collapse each page's paired `:root` /
   `@media (prefers-color-scheme: dark)` / `[data-theme]` blocks (the
   pattern every one of the 13 pages repeats three times per token) into one
   declaration per token, e.g. `--bg: light-dark(#fff, var(--ui-bg));`.
   Baseline-supported since 2023 (same standard §6 already leaned on for
   container queries). This is a mechanical follow-on to §5's consolidation,
   not a new design decision — flagged here because it's naturally done in
   the same pass as the rest of this section's edits.
6. **A tighter type scale** — `--ui-font-sm: 0.85em`, `--ui-font-base: 1em`,
   `--ui-font-lg: 1.15em`, `--ui-font-xl: 1.4em` — replacing the ad hoc
   `0.82em`/`0.85em`/`0.9em`/`0.95em`/`1.05em` values scattered per page
   (`diagnostics_page.html`, `main_page.html`, others) with a shared,
   shorter list.

### 7.3 The corresponding LCD change

Because 7.2 introduces **no new hex**, the LCD-parity change is narrower
than "repaint the panel": it is a semantic annotation, not a color change.
`ui_theme.h`'s `UI_THEME_ACCENT_3` comment and `UI_THEME.md`'s palette table
row for it would gain a line designating it the primary/action accent (e.g.
"also used as the dominant accent for primary buttons/focus, once the LCD's
placeholder screen in `kiln_ui.c` grows real button styling") — the same
`#3ec6c6` value, now with a stated dual role. If and when `kiln_ui.c`'s
still-placeholder screen gets real button chrome, its primary/confirm
buttons should reach for `UI_THEME_ACCENT_3` for the same reason the web
proposal does: consistent with parity, and consistent with 7.1's finding
that accent-3 is otherwise unclaimed. Nothing else in 7.2 (spacing, shadow,
`color-mix()`, `light-dark()`, type scale) has an LCD analog to keep in sync
— those are CSS-syntax/layout concerns LVGL expresses through its own,
already-separate spacing/sizing constants (`UI_THEME_PADDING_PX` etc.,
already deliberately excluded from this pass per §3 item 7).

### 7.4 What this proposal deliberately does not do

No change to `--ui-touch` (§3 item 7, unconditionally out of scope for this
pass). No change to any `--ok`/`--warn`/`--bad`/`--neutral` mapping — that's
§5.1's decision, independent of this one. No new accent hex — if the owner
wants an actual new brand color (a sixth accent, or a paint-over of an
existing one), that is a bigger decision than this proposal makes and would
need its own sign-off with its own LCD-side hex change.

---

## 8. On frameworks — not now, with a stated re-open condition

**No framework in Phases 0–3.** The blocker is token duplication and the body
cap (2.1, 2.2). Neither is a rendering problem, so a renderer does not fix
them. Tailwind would mean rewriting class names across 670 kB of source; Pico
or Bootstrap would fight `theme.css`, which is deliberate and documented.

Flash is not the objection — the app slot has ~1.15 MiB free
(`FLASH_BUDGET_PLAN.md` §4.2, which owns that number) and Preact-with-signals
or Lit is ~5–6 kB gzipped.

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
- **The body cap was never a considered global decision** — three pages had
  none and the rest used four different values. Phase 2 replaced all of it
  with one token.
- **`zones_page.html`'s tuning-recommendation panel (`333dd4e`) checked
  against sec 5/6 conventions — conforms.** It lives inside `<div
  class="card">` (sec 6's container-query pattern) and its status color is
  `var(--ok)` / `var(--warn)` / `var(--bad)`, the token names zones_page
  already uses per sec 5's majority vocabulary — not a new palette. It does
  not touch, override, or duplicate any of `check_ui_shell_layout.ps1`'s
  required rules. The inline `style="color:var(--cls)"` on the result line
  and the `style="margin-left:1em"` on its form controls are not a shell/
  container violation — sec 6 governs page-level width/grid, not inline
  spacing, and ad hoc inline `em`/color styling is the existing convention
  elsewhere on this page and others (sec 7.2 item 6 already notes the
  scattered ad hoc `em` values pending a shared scale). Verified in the
  sweep (78/78) and `check_ui_shell_layout.ps1` (still passing) with the
  panel present.
