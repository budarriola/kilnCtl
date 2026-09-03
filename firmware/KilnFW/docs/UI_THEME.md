# UI theme -- shared palette (LCD + web)

TODO.md section 10.2. Source of truth: `App/drivers/ui_theme.h` (LVGL, for the
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
| `UI_THEME_ACCENT_3` | `#3ec6c6` | Accent 3 -- teal/cyan. Also the web UI's dominant/primary-action accent since WEB_UI_RESPONSIVE_PLAN.md sec 7 (2026-09-03) -- `theme.css`'s `button:focus-visible` ring. Same value, web-only usage change; if `kiln_ui.c`'s placeholder screen grows real button chrome, its primary/confirm buttons should reach for this accent too for parity. |
| `UI_THEME_ACCENT_4` | `#5cc06e` | Accent 4 -- green. Matches the reference's nav-icon underline color. |
| `UI_THEME_ACCENT_5` | `#d6555f` | Accent 5 -- red/amber. Held back for "attention" use (fault/alarm/stop) rather than a fifth ordinary zone color; the one accent here inferred rather than directly seen in the reference screenshots, so double-check it hardest once real hardware is available. |

## Spacing / sizing

| Name | Value | Purpose |
|---|---|---|
| `UI_THEME_MIN_TOUCH_TARGET_PX` | 72 px | Minimum touch target edge length. Budgeted against the 480x320 landscape panel (`DISPLAY_WIDTH`/`DISPLAY_HEIGHT`, `App/drivers/settings.h`) -- a round number sized to fit a small grid of large buttons on that resolution, not a measured fingertip size. |
| `UI_THEME_CORNER_RADIUS_PX` | 10 px | Standard corner radius for buttons/cards. |
| `UI_THEME_PADDING_PX` | 8 px | Standard padding/gap between grouped elements. |
| `UI_THEME_STATUS_BAR_HEIGHT_PX` | 32 px | Persistent top status bar height. |

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
}
```

This document, together with `App/drivers/ui_theme.h`, is the single source
of truth both front ends should read from -- the LCD (LVGL, `ui_theme.h`) and
the web dashboard (CSS, not yet built) should not pick their own palettes
independently, per the 10.5 web/LCD parity rule. That CSS block itself is not
built by this pass; this section only records the intent so 10.6 doesn't
reinvent the palette when it gets built.
