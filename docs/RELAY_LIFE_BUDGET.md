# Relay type and contact-life budget

Shipped 2026-09-06. Each relay gets a **type** — `ssr | contactor | mercury`
— that sets a rated contact-life budget; firmware tracks cycles and shows
budget used, indication only (firing and every other operation stay
allowed at any budget level).

## Behaviour

- **Heater relays** (K1/K2/K3/K5): type is chosen per zone, in the zones
  page (`relay_type`, `ZONES_CFG_VERSION` 19→20, default 0 = `ssr`). Since
  `relay_mask` lets a zone drive several relays, the type applies to each
  relay in the mask; the cycle counter stays per relay.
- **Safety relay** (K4): type is chosen in the safety commissioning page,
  stored in the ESP's safety settings store, default `contactor`, and the
  select **does not offer `ssr`**.
- **Thresholds**: above 80% of rated life, a persistent WARNING icon shows
  on the LCD home topbar and the web dashboard; above 90%, ERROR. `ssr` has
  no budget (icon never shown, percent reported as null).
- **Rated-life table** (`relay_cycles.h`): `contactor` = 100,000 cycles,
  `mercury` = 1,000,000, editable per relay via an optional
  `rated_override` (0 = use the table). No datasheet for a specific
  contactor or mercury-wetted relay is in this tree — these are
  industry-typical defaults, not measured facts.
- **Reset**: a per-relay cycle-count reset requiring confirmation, **web
  diagnostics page only** (`kcConfirm()`). The LCD's own two-tap Reset
  control was removed 2026-09-19 (UI_PLAN.md section 6.4); the Relay Life
  page is read-only. Reset writes through the flash worker and logs an
  INFO event with the old count.

## K4 counting

K4 is driven by the Pico (`relay_owner.c`); no Pico-side counter and no new
link field were added — `firmware/SaftyFW/docs/RELAY_WEAR_ANALYSIS.md`'s
decision against a Pico counter stands. Instead the **ESP** counts K4 edges
by observing K4's state on every safety-status frame it already receives: a
fifth slot in `relay_cycles` (`RELAY_CYCLES_SAFETY_INDEX`), incremented on
an observed off→on or on→off transition, guarded against the Pico's
`boot_id` changing (a reboot loses the ESP's last-observed state, so a
spurious edge right after boot is not counted). This misses transitions
that happen while the ESP itself is rebooting — accepted, since the
indication only needs to track roughly ~10 transitions per firing against a
100,000-cycle budget. Shipped in `c6d41fc` (K4 edge counting in
`safety_link_frames.c` + the safety relay type in `safety_cfg_store`);
review fixes in `09769f5` and `81f2f34`.

## Persistence

`persist/relay_cycles.c/.h` counts lifetime transitions for all five
relays (four ESP heater relays + K4), versioned blob `relay_cyc` (NVS key
≤ 15 chars) in `kiln_nvs`, blob version 1→2 (adds the fifth slot),
persisted at most every 600 s and flushed at firing stop / autotune end,
written only from the flash worker (never a re-entrant dispatch). Budget
percent and tier (`none|warn|error`) are computed on read from the stored
cycle count and rated life, never stored themselves.

The dashboard JSON carries `relay_life: [{type, cycles, rated, percent,
tier}]` (5 entries) and `relay_life_tier` (the max tier over all budgeted
relays).

## Not yet verified on hardware

LCD icon/page rendering, K4 edge count against a real Pico.
