# UI theme -- shared palette (LCD + web)

TODO.md section 10.2. Source of truth: `App/drivers/ui/ui_theme.h` (LVGL, for the
LCD). This doc mirrors the same values in table form and explains how the web
dashboard (section 10.6) is meant to consume them.

**Provenance / status:** first-pass palette, not colorpicked from a real
screenshot and not verified against the physical ILI9488 panel. It matches a
*description* of KlipperScreen's look (dark navy background, white text, a
small rotating set of saturated accent colors used per-row/zone/nav-icon
rather than one brand color), given to this pass as reference screenshots
that this pass could not itself see. Treat every value here the same way this
codebase already treats `KILNCTL_TOUCH_CAL_SWAP_XY` / `KILNCTL_TOUCH_Z1_MAX_THRESHOLD`
(`App/drivers/Kconfig`): a reasonable guess pending a look at real hardware,
not a measured value. Sanity-check on the physical LCD once one is available,
and update both this table and `ui_theme.h` together if it needs correcting.

## Palette

| Name | Hex | Purpose |
|---|---|---|
| `UI_THEME_COLOR_BG` | `#1a1f2b` | Main screen background -- dark navy, not pure black (distinct from `kiln_ui.c`'s current temporary placeholder screen, which *is* pure black and predates this theme). |
| `UI_THEME_COLOR_CARD` | `#242a3a` | Card/panel background, one step lighter than the main bg -- groups content (stat rows, button clusters) without a hard border. |
| `UI_THEME_COLOR_TEXT_PRIMARY` | `#f0f0f0` | Primary text -- near-white, not pure `#ffffff`, to soften contrast on a small TFT. |
| `UI_THEME_COLOR_TEXT_SECONDARY` | `#9aa0ae` | Secondary/dim text -- labels, units, less-important status text. |
| `UI_THEME_ACCENT_1` | `#e8974e` | Accent 1 -- orange. Per-zone/per-metric color coding, assigned by 10.3, not fixed to a specific zone by this header. |
| `UI_THEME_ACCENT_2` | `#a15fd6` | Accent 2 -- purple/magenta. |
| `UI_THEME_ACCENT_3` | `#3ec6c6` | Accent 3 -- teal/cyan. Also the web UI's dominant/primary-action accent since WEB_UI_RESPONSIVE.md sec 7 (2026-09-03) -- `theme.css`'s `button:focus-visible` ring. Same value, web-only usage change; if `kiln_ui.c`'s placeholder screen grows real button chrome, its primary/confirm buttons should reach for this accent too for parity. |
| `UI_THEME_ACCENT_4` | `#5cc06e` | Accent 4 -- green. Matches the reference's nav-icon underline color. |
| `UI_THEME_ACCENT_5` | `#d6555f` | Accent 5 -- red/amber. Held back for "attention" use (fault/alarm/stop) rather than a fifth ordinary zone color; the one accent here inferred rather than directly seen in the reference screenshots, so double-check it hardest once real hardware is available. |
| `UI_THEME_ACCENT_BLUE` | `#2f6fe4` | Accent blue -- the LCD dashboard's profile-selection button. Added 2026-09-21; none of ACCENT_1..5 above is blue. Keep this table, `ui_theme.h` and `theme.css` in step if it ever changes. |
| `UI_THEME_COLOR_NEUTRAL` | `#9aa0ae` | **Dual-role token**, added Phase 7 (TODO.md 1223-1225). Alias for `UI_THEME_COLOR_TEXT_SECONDARY` -- same value, added so a "neutral status" call site can say that intent by name. Mirrors the web dashboard's `--neutral` (`main_page.html`, `readiness_page.html`, WEB_UI_RESPONSIVE.md sec 5.1 item 2, 2026-09-03), which is itself defined as `var(--ui-text-secondary)` in dark mode -- both front ends already treat "neutral status" and "secondary text" as the same color, this just gives the LCD side a name for that too. |

## Spacing / sizing

| Name | Value | Purpose |
|---|---|---|
| `UI_THEME_MIN_TOUCH_TARGET_PX` | 72 px | Minimum touch target edge length. Budgeted against the 480x320 landscape panel (`DISPLAY_WIDTH`/`DISPLAY_HEIGHT`, `App/drivers/hw/settings.h`) -- a round number sized to fit a small grid of large buttons on that resolution, not a measured fingertip size. |
| `UI_THEME_CORNER_RADIUS_PX` | 10 px | Standard corner radius for buttons/cards. |
| `UI_THEME_PADDING_PX` | 8 px | Standard padding/gap between grouped elements. Phase 7: now an alias for `UI_THEME_SPACE_2` (same value, see the spacing scale below) rather than its own literal. |
| `UI_THEME_STATUS_BAR_HEIGHT_PX` | 32 px | Persistent top status bar height. |

### Spacing scale (Phase 7, TODO.md 1223-1225)

Mirrors `theme.css`'s `--ui-space-1..5` (WEB_UI_RESPONSIVE.md sec 7.2
item 2, 2026-09-03) pixel-for-pixel, so a new LCD layout has the same named
rungs the web side already reaches for:

| Name | Value | Web equivalent |
|---|---|---|
| `UI_THEME_SPACE_1` | 4 px | `--ui-space-1` |
| `UI_THEME_SPACE_2` | 8 px | `--ui-space-2` (= `--ui-padding`, = `UI_THEME_PADDING_PX`) |
| `UI_THEME_SPACE_3` | 12 px | `--ui-space-3` |
| `UI_THEME_SPACE_4` | 20 px | `--ui-space-4` |
| `UI_THEME_SPACE_5` | 32 px | `--ui-space-5` |

Additive only, same as the web side's own adoption note: existing
`lv_obj_set_style_pad_*()` call sites across `ui_page_*.c` were **not** mass-
rewritten to spell their literals as one of these. Most of those numbers (0,
2, 3, 4...) are load-bearing against `UI_THEME_PAGE_CONTENT_BUDGET_PX` --
hand-fit to a specific page's own worst-case arithmetic, documented in that
page's own comment, not spacing debt. A blind find/replace could silently
nudge a page's computed height past its `_Static_assert` (the assert only
checks the constants it was written against, not a renamed call-site
literal) with no compiler error to catch it. New spacing decisions should
reach for the scale; existing ones stay exactly as each page's own budget
comment already justifies them.

### Card shadow (Phase 7, TODO.md 1223-1225)

`ui_theme_apply_card_shadow(lv_obj_t *card, int level)` (`ui_theme.c`)
mirrors `theme.css`'s two-layer translucent-black drop shadows:

```css
--ui-shadow-1: 0 1px 2px rgba(0,0,0,.12), 0 1px 1px rgba(0,0,0,.08);
--ui-shadow-2: 0 2px 6px rgba(0,0,0,.18), 0 1px 2px rgba(0,0,0,.1);
```

LVGL has no multi-layer box-shadow syntax to copy verbatim, so this uses one
`lv_style` shadow layer per level (`UI_THEME_SHADOW_1/2_OPA` +
`_WIDTH_PX`, `ui_theme.h`), picking the stronger alpha of each web pair as
that level's opacity -- level 1 is a subtle resting lift (the common case:
info cards, status cards, the home page's chart background), level 2 a
slightly stronger lift for a raised/modal-style surface. Not a byte-identical
render (not achievable across the two rendering backends), same intent. Pure
paint -- an LVGL shadow draws outside the widget's own box and, like CSS
`box-shadow`, never participates in flex/grid layout sizing, so applying it
to an existing card costs zero page-content-budget pixels. Applied so far to
the "real card" containers restyled in this pass (see this doc's "Phase 7
theme/style pass" note below) -- not to every button, since LVGL buttons
already carry their own press/release visual feedback and a shadow on every
button would read as visual noise rather than hierarchy.

## Web dashboard parity

TODO.md section 10.6 (web dashboard restyle) is meant to pull these exact hex
values into a shared CSS custom-properties block, e.g.:

```css
:root {
  --bg: #1a1f2b;
  --card: #242a3a;
  --text-primary: #f0f0f0;
  --text-secondary: #9aa0ae;
  --accent-1: #e8974e;
  --accent-2: #a15fd6;
  --accent-3: #3ec6c6;
  --accent-4: #5cc06e;
  --accent-5: #d6555f;
  --accent-blue: #2f6fe4;
}
```

This document, together with `App/drivers/ui/ui_theme.h`, is the single source
of truth both front ends should read from -- the LCD (LVGL, `ui_theme.h`) and
the web dashboard (CSS, not yet built) should not pick their own palettes
independently, per the 10.5 web/LCD parity rule. That CSS block itself is not
built by this pass; this section only records the intent so 10.6 doesn't
reinvent the palette when it gets built.

## Phase 7 theme/style pass (DISPLAY_ST7796_PLAN.md Phase 7, TODO.md 1223-1225)

Styling-only pass, no navigation/content restructuring, no status-color
repaint (WEB_UI_RESPONSIVE.md sec 7.5's algebraic-impossibility proof
stands -- see that doc, not reopened here). What actually landed:

- `UI_THEME_COLOR_NEUTRAL`, `UI_THEME_SPACE_1..5`, and
  `ui_theme_apply_card_shadow()` added to `ui_theme.h`/`.c` -- see this
  document's own sections above for each.
- Card shadow applied (pure paint, zero budget cost -- see "Card shadow"
  above) to the real "card" containers restyled this pass:
  `ui_page_home.c`'s `s_chart` (the trend-chart background), and one
  info/status card each on `ui_page_temperature.c`, `ui_page_network.c`
  (`status_card` and `s_ap_section`), `ui_page_profile_detail.c`
  (`s_info_card`), and `ui_page_profile_builder_review.c`
  (`s_summary_card`). Not applied to every `UI_THEME_COLOR_CARD` call site in
  the tree (many are buttons or dense-grid cells, not standalone cards -- see
  the shadow helper's own doc comment for why buttons were left alone).
- Every page carrying a `UI_THEME_PAGE_CONTENT_BUDGET_PX` `_Static_assert`
  (`ui_page_temperature.c`, `ui_page_network.c`, `ui_page_network_manage.c`)
  was left dimensionally unchanged -- shadows add no layout height/width, and
  no padding/gap/font-size literal was edited, so no page's worst-case sum
  moved. See the implementing pass's own report for how each assert was
  re-verified (`build_kilnfw` plus a deliberate-overflow negative test of the
  guard itself).

## Phase 7, second pass (2026-09-03) -- measure-first spacing/typography audit

The first pass above was deliberately conservative: it added the spacing
scale and card shadows but touched no page's padding/gap/font-size literal,
because those numbers are hand-fit per page against
`UI_THEME_PAGE_CONTENT_BUDGET_PX` (268px, see `ui_theme.h`) and a blind mass
substitution risked overflowing a page silently. This pass measured every
`ui_page_*.c`'s worst-case content height against that budget before editing
anything, per DISPLAY_ST7796_PLAN.md Phase 7's own instruction.

**Headroom table** (worst-case height vs. the 268px budget; every number below
is the page's own documented `_Static_assert`/comment arithmetic, not a fresh
guess):

| Page | Worst-case px | Headroom | Note |
|---|---:|---:|---|
| `ui_page_config.c` | 224 | 44 | hub grid |
| `ui_page_diagnostics.c` | 239 (max stat-row page) | 29 | Safety/thermo-fault sub-pages run tighter, variable-height wrapped labels -- treat as tight |
| `ui_page_home.c` | self-fitting | 0 free | chart uses `flex_grow(1)` and absorbs all leftover space by construction; any added fixed px comes straight out of chart height |
| `ui_page_network.c` | 258 (STA and AP both) | **10 -- TIGHT** | do not touch |
| `ui_page_network_manage.c` | 206 | 62 | |
| `ui_page_profile_builder_review.c` | 180 | 88 | |
| `ui_page_profile_builder_segment.c` | 162 | 106 | |
| `ui_page_profile_builder_zones.c` | 208 | 60 | |
| `ui_page_profile_detail.c` | 258 | **10 -- TIGHT** | do not touch |
| `ui_page_profile_picker.c` | 268 | 0 -- zero slack, see the file's own `_Static_assert` | replaced `ui_page_profiles_mine.c`/`ui_page_profiles_family.c`, both deleted; `ui_page_profiles.c` is now a thin alias onto this file's MANAGE mode and has no layout of its own |
| `ui_page_profile_segments.c` | 204 | 64 | |
| `ui_page_temperature.c` | 194 (3 zones today) | 74 | |
| `ui_page_touch_cal.c` | self-fitting | n/a, safe by construction | targets placed from real `lv_display_get_*_resolution()`, never a fixed row stack; on an unsupported controller it builds a centered-notice screen instead (no grid), also self-fitting |
| `ui_page_touch_test.c` | self-fitting | n/a, safe by construction | `s_canvas_h` derived from real resolution minus fixed chrome |

**Spacing migration: nothing was migrated.** Every non-zero
`lv_obj_set_style_pad_*`/`pad_gap` literal left in the tree after grepping all
18 pages is either `0` (semantically "flush", not a scale rung -- `theme.css`
has no `--ui-space-0` either) or a 2px/3px value on `ui_page_home.c`'s
flex-fit chart legend/trip-strip and `ui_page_profile_detail.c`'s plan chart
-- both zero-headroom or 10px-headroom pages. `UI_THEME_SPACE_1` is 4px, so
substituting any scale rung there would either be a no-op-that-lies (call it
`UI_THEME_SPACE_1` while it stays 2px, which nobody wants) or an actual
size increase on exactly the two pages with no room to absorb one. No page
with real headroom (44px+) had any non-macro spacing literal to migrate --
this codebase's ad-hoc-literal population turned out to be entirely
concentrated on its two tightest pages, which is exactly backwards from what
a safe mass-migration needs and confirms the first pass's caution was
correct, not merely prudent-by-default.

**Typography: already harmonised, nothing to change.** Every page uses
`LV_FONT_DEFAULT` (montserrat_14) for all body/label text with exactly one
documented exception -- `ui_page_home.c`'s chart axis labels/legend swatches
use `lv_font_montserrat_10`, a deliberate smaller role for dense
chart-adjacent text (see that file's own comment, `UI_PLAN.md` 5.3). No other
page introduces a third font or a different default-font override, so there
was no inconsistent typography role to reconcile.

**Splitting: not needed.** Every page's measured worst case already fits the
budget with real (if in two cases thin) margin; `ui_page_diagnostics.c`
already reflects the "grow past budget -> add a page" rule from its own
2026-08-27 fold. No page needed splitting this pass.

**Verification:** `build_kilnfw` green; `check_ui_budget_asserts.ps1` passes;
all 21 `App/test/build_host_tests.ps1` executables pass. The guard was
negative-tested by bumping `UI_PAGE_NETWORK_MANAGE_LIST_HEIGHT_PX` from 70 to
700 in `ui_page_network_manage.c` -- `build_kilnfw` failed with a real
compiler `_Static_assert` error naming that exact macro, the edit was
reverted, and a grep for the mutated value confirmed it is gone before the
final green rebuild.
