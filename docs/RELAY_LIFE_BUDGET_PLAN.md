# Relay type and contact-life budget — plan

Status: opened 2026-09-06 from an owner request. Survey facts reference
`05087f0`.

## Request

Each relay gets a **type**: `ssr | contactor | mercury`. Heater relays are
chosen in the zones section and default `ssr`; the safety relay is chosen in
the thermocouples/safety section, defaults `contactor`, and **does not offer
`ssr`**. The type sets a rated contact life; the firmware tracks cycles and
shows budget used. Above 80 %: persistent WARNING icon on the LCD home page
and the web dashboard. Above 90 %: ERROR icon. Firing and every other
operation stay allowed — indication only. Diagnostics offers a per-relay
count reset that requires confirmation.

## What exists

- `persist/relay_cycles.c/.h` already counts lifetime transitions for the
  four ESP heater relays, versioned blob `relay_cyc` in `kiln_nvs`, persisted
  at most every 600 s and flushed at firing stop / autotune end, PSRAM-stack
  guarded. Exposed as raw `relay_cycles[]` in the dashboard JSON
  (`dashboard_http.c:171`, `dashboard_status_http.c:180`). No budget math,
  no LCD surface, no MCP tool.
- The safety relay K4 is driven by the Pico (`relay_owner.c`) and **not
  counted anywhere**; `firmware/SaftyFW/docs/RELAY_WEAR_ANALYSIS.md` decided
  that on purpose (~10 transitions per firing). There is no link field for
  a K4 count.
- Zones schema `ZONES_CFG_VERSION 19`; `control_mode` (`uint8_t`) is the
  enum precedent, rendered as a `<select class="mode">` in `zones_page.html`
  (~line 805). Migration pattern: frozen `zone_cfg_vN_t`, loader fills new
  fields with 0, `_Static_assert` on offsets.
- Icon precedents: LCD `s_ui_home_lag_notice` (built once, hidden = zero
  height) and the topbar icon proxy (`ui_topbar.h`, FLOATING, raised after
  content build); web `#profileFeasIcon` with `.feas-icon-warn/-unknown`
  and the `.banner.*` classes (`main_page.html:224-307`). Web confirm is
  `window.kcConfirm()` (`app.js:101`); the LCD diagnostics page has no
  confirm widget yet.
- Rated life: the repo's own assumption for the EE2-12NUH heater relays is
  10^5 operations (`relay_cycles.h` header). No contactor or mercury
  datasheet is in the tree — the table below is industry-typical and must
  be presented as editable defaults, not facts.

## Design

**Rated-life table** (`relay_cycles.h`): `ssr` = no budget (icon never
shown, percent reported as null); `contactor` = 100 000; `mercury` =
1 000 000. Per relay an optional `rated_override` (0 = use table) so a
real datasheet number can be typed in.

**Per-zone type**: `relay_type` `uint8_t` appended to `zone_cfg_t`,
`ZONES_CFG_VERSION` 19→20, default 0 = `ssr`. Since `relay_mask` lets a zone
drive several relays, the type is per zone and applies to each relay in the
mask; the counter stays per relay.

**Safety relay type and count on the ESP, not the Pico.** The ESP observes
K4 state on every safety-status frame, so it can count K4 transitions
itself: a fifth slot in `relay_cycles` (`RELAY_CYCLES_SAFETY_INDEX`),
incremented on an observed off→on or on→off edge, persisted with the
others. Misses transitions that happen while the ESP is rebooting — fine
for an indication with a 10^5 budget and ~10 transitions per firing.
`RELAY_WEAR_ANALYSIS.md` stays true (no Pico counter, no new link field);
add one paragraph pointing here. The type lives in the ESP safety settings
store next to the other commissioning answers, default `contactor`, options
`contactor | mercury` only.

**Budget state** (`relay_cycles_budget(relay)` → percent, tier
`none|warn|error`): computed on read, never stored. Tier is the maximum
over all relays with a budget; the dashboard JSON gains
`relay_life: [{type, cycles, rated, percent, tier}]` and `relay_life_tier`.

**Indication**: LCD — a topbar icon proxy (`LV_SYMBOL_WARNING`) using the
existing warn/bad colours, hidden below 80 %, shown persistently above. The
persistence is deliberate and contradicts the page's "a warning always
present is a warning nobody reads" note (`main_page.html:823`) — say so in a
comment so a later pass does not "fix" it. Web — an icon next to
`#profileFeasIcon` with a tooltip naming the relay and percent, and a row
in the diagnostics section. No new colours (contrast memory).

**Reset**: `POST /api/relay_cycles/reset` `{relay: N}` gated by
`kcConfirm()` on the web diagnostics page; the LCD diagnostics page gets a
two-tap confirm (press "Reset", button turns into "Confirm?" for 5 s) since
no dialog widget exists. Reset writes through the flash worker like the
periodic persist. Log an INFO event with the old count.

## Steps

1. `relay_cycles`: fifth slot, type table, `relay_cycles_budget()`, blob
   version 1→2 migration, host test with quantized counts crossing 79/80/89/90 %.
2. Zones schema 19→20 (`relay_type`), `zones_http.c` round trip, select in
   `zones_page.html`, migration test.
3. Safety relay type in the ESP safety settings store + commissioning page
   select without `ssr`; K4 edge counting in the safety-link status consumer.
4. Dashboard JSON fields; web icon; diagnostics row and confirmed reset.
5. LCD topbar icon; LCD diagnostics reset with two-tap confirm; 480x320,
   no scroll; verify with `capture_lcd.ps1` numeric sampling.
6. `HARDWARE.md` / `RELAY_WEAR_ANALYSIS.md` notes; `check_*` re-run; build
   `build_kilnfw`, flash after an opus FLASH-SAFE review.

Risks: NVS keys ≤ 15 chars; persist paths only from the flash worker (no
re-entrant dispatch); autotune's relay identification switches faster than
the 60 s PWM window, so a tuning campaign eats budget faster than a firing
— mention in the tooltip text.
