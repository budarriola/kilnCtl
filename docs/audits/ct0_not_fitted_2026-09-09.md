# "CT0 not fitted" vs owner report "it is fitted" — 2026-09-09

> **Status:** investigation, no fix applied (none identified). No config or
> firmware changes made in this pass — read-only board/schematic queries only.

## Owner report

"It says ct0 not fitted, but it is fitted." Treated as authoritative about
the physical hardware per standing instruction; this audit is the result of
taking that seriously rather than explaining it away.

## Raw evidence

### Live board state (`kiln_batch`, 2026-09-09)

```
safety_get_status:
  link up; SaftyFW armed; safety thermocouple valid | 42.32 C (CJ 39.33 C)
  currents not fitted, not fitted, 0.00 A | ct zone: - | ct_counts 17, 17, 55

safety_get_diag:
  state armed | trip_reason 0 | warn_mask 0x0000 | trip_mask 0x0000

safety_get_commissioning:
  commissioned=True, config CRC matches live
  S14 over-current (per channel): ch0 DORMANT (i_normal_a not measured);
                                   ch1 DORMANT (i_normal_a not measured);
                                   ch2 DORMANT (i_normal_a not measured)
  S15 under-current (per zone, ct_topology=summed): z0/z1/z2 DORMANT
  tc_type=3, mains_voltage_v=240

safety_get_ct_cal (legacy ct_cal[], separate from zero_counts/k_ct_v_per_a
  per CURRENT_SENSE.md §5.3 — does not gate presence/fitted):
  channel 0: uncalibrated; channel 1: uncalibrated; channel 2: uncalibrated
```

No firing/profile running (`profiles_get_exec_status`: `state=0`), all
relays off (`io_read`: `R1=0 R2=0 R3=0 R4=0`) — so no live-load correlation
test was possible this pass (see "What could not be determined" below).

### Raw CT ADC counts, captured live (`safety_capture_ct_counts`, 20 s @ ~4.6 Hz)

```
ch0: mean=16.70  std=0.459  min=16 max=17
ch1: mean=17.00  std=0.000  min=17 max=17
ch2: mean=56.11  std=2.187  min=54 max=60
```

Compare against the **2026-09-06 measured baseline** already recorded in
`firmware/SaftyFW/docs/CURRENT_SENSE.md` §"Measured noise floor" (60 s
capture, relays off, nothing calibrated differently since):

```
ch1 (unfitted, doc's 1-indexing = firmware channel 0): mean=16.23 std=0.418 min=16 max=17
ch2 (unfitted, doc's 1-indexing = firmware channel 1): mean=17.00 std=0.000 min=17 max=17
ch3 (fitted,   doc's 1-indexing = firmware channel 2): mean=66.89 std=4.678 min=60 max=76
```

Today's channel 0 reading (16.70 mean / 0.459 std) is essentially identical
to that 2026-09-06 "unfitted" baseline. Channel 2 today (56.11 mean / 2.187
std) is lower than the 2026-09-06 fitted-channel mean but still shows the
same qualitative signature — a noise floor an order of magnitude noisier
than channels 0/1, consistent with a live op-amp/rectifier front end rather
than a floating pin. (The mean shift 66.89→56.11 is plausibly the
`zero_counts[2] = 63` correction landed 2026-09-08 per CURRENT_SENSE.md
§5.3's "Bench state" note — not investigated further here, out of scope for
this question.)

**Channel 0 shows no measurable change from the "known unpopulated" baseline.
Channel 1 shows literally zero variance (std=0.000) both then and now — the
signature of a floating/tied ADC input, not a connected analog front end.**

## Tracing "not fitted" through the code

`firmware/KilnFW/App/drivers/http/dashboard_status_http.c` (around line
414-421, `dashboard_status_http_build_json_status` or its helper — comment
cites `CT_COMMISSIONING_PLAN.md` step 4):

```c
bool summed = dashboard_ct_topology_is_summed();
APPEND(",\"ct_topology\":\"%s\"", summed ? "summed" : "per_zone");
APPEND(",\"ct_fitted\":[");
for (unsigned ci = 0; ci < 3; ci++) {
    bool fitted = !summed || ci == 2u;
    APPEND("%s%s", ci == 0 ? "" : ",", fitted ? "true" : "false");
}
```

This is the single point that decides "not fitted" for the `/api/status`
JSON. It is a **hardcoded topology rule, not a per-channel measurement or a
separate config flag**: in `summed` mode, only array index 2 is fitted, by
definition. `ct_installed` (`config_store.h` line ~502, param `0x0109`) is a
separate whole-board yes/no ("are CTs fitted at all") and is not what drives
this per-channel split.

The LCD (`firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c` ~line
775-818) and the web dashboard (`firmware/KilnFW/App/drivers/http/main_page.html`
~line 796-819, `renderCurrentCard()`) both consume `ct_fitted[]` from that
same JSON field and both use **0-based indexing that agrees with the
firmware's own channel numbering**: `label = 'CT' + ci` where `ci` is the
loop index into `data.ct_current_a`/`data.ct_fitted` (`main_page.html` line
805). The commissioning page (`safety_commissioning_page.html` lines
606-643) uses the identical convention: `CT0`=`chIndex:0`, `CT1`=`chIndex:1`,
`CT2`=`chIndex:2`, with `ct_channel_map[0]`'s own note explicitly reading
"Which relay CT0 actually watches."

**No indexing mismatch was found anywhere in this chain** — LCD, web
dashboard, commissioning page, and the JSON producer all agree that `CT0` =
firmware channel index 0 = `ADC0`/`GPIO26`.

## Cross-referencing the hardware

Via the KiCad MCP facade (read-only; `kicad_call`), the three current-sense
jacks:

| Ref | Schematic sheet instance | Firmware channel (by GPIO) |
|---|---|---|
| J13 | `/SaftyProcessor/CurrentSense/`  | `Current1` = GPIO26/ADC0 = index 0 |
| J15 | `/SaftyProcessor/CurrentSense1/` | `Current2` = GPIO27/ADC1 = index 1 |
| J17 | `/SaftyProcessor/CurrentSense2/` | `Current3` = GPIO28/ADC2 = index 2 |

(`firmware/SaftyFW/docs/HARDWARE.md` lines 275-277 give the same
GPIO→net-name table.) The sheet numbering (`CurrentSense`, `CurrentSense1`,
`CurrentSense2`) runs 0/1/2 in the same order as the GPIO table and the
firmware's channel index — **no off-by-one or silkscreen/index mismatch
found in the schematic either.** (The PCB netlist query on this project
returned no nets/connections for these components — `list_kicad_nets`
returned empty and `get_kicad_component_connections` showed `"nets": []`
for J13/J15/J17 — so this cross-check relied on sheet-instance identity and
the documented GPIO/net table rather than a live PCB netlist; noted under
"what could not be determined" below.)

`firmware/SaftyFW/docs/HARDWARE.md` line 870 states plainly: **"a CT is now
fitted and confirmed working, on `Current3` (GPIO28/ADC2, above) only —
Current1/Current2 remain unpopulated."** This is a statement about the
analog front-end population, not merely "no CT clipped on" — consistent
with channel 1's std=0.000 reading (a floating/tied node, not an unenergized
but otherwise-live rectifier stage).

## Conclusion

Every layer checked — the JSON producer, the LCD, the web dashboard, the
commissioning page, the schematic's sheet/connector assignment, and (most
importantly) the **live raw ADC statistics** — agrees on the same mapping,
and the raw counts on channel 0 today are statistically indistinguishable
from the "known unpopulated" 2026-09-06 baseline capture recorded in
`CURRENT_SENSE.md`. Channel 1 reads with **zero measured variance**, which
is not consistent with a connected analog front end of any kind (even an
idle CT clamped on a de-energized load shows the op-amp's own noise/Vos —
see channel 2's 2.2-4.7 count std both then and now).

**This audit did not find a flag, index, or label bug.** `ct_fitted[]` is
computed as a hardcoded function of `ct_topology`, not read back from any
per-channel measurement, and every renderer agrees on what index means what
channel. The evidence available this pass supports the existing record
(the physically fitted CT is on channel 2 / `GPIO28` / `Current3` / J17,
not channel 0), not the owner's report that CT0 is fitted.

**This is not a claim that the owner is wrong** — see "what could not be
determined" below for the real gap: no live-current correlation test was
possible this pass (no firing in progress, and this task was explicitly
read-only), which is the one test (`CURRENT_SENSE.md` §5's commissioning
check, step 2: energize exactly one relay and confirm which channel
responds) that would be dispositive either way, including against the
possibility that a CT was very recently physically added to channel 0's
jack (J13) but its front-end components are unpopulated on this board
per `HARDWARE.md` line 870 — in which case a CT could be physically present
at the jack while still reading a dead floor, which would reconcile the
owner's observation with the electrical evidence above without either side
being wrong.

## What could not be determined this pass

- **No live-load correlation.** No firing/relay was active; per task
  constraints, none was commanded. The single test that would settle this
  beyond raw-noise-signature inference — `CURRENT_SENSE.md` §5 step 2,
  energize one relay at a time and watch which channel's counts move — was
  not run.
- **No physical inspection.** Whether a CT is now clipped onto J13 (or any
  jack) was not and cannot be verified from software; this audit is limited
  to what the ADC electrically reports.
- **Whether channel 0/1's analog front end (U8A/U8B, R43/R46/R77/C57 per
  channel, `CURRENT_SENSE.md` §1) is actually unpopulated on THIS specific
  board**, versus merely undriven, was not re-verified against the physical
  board — this audit relied on `HARDWARE.md` line 870's existing "remain
  unpopulated" statement and the matching electrical signature (std=0.000 on
  channel 1), not a fresh visual/continuity check.
- **The PCB-level netlist cross-check was inconclusive**: `list_kicad_nets`
  returned no nets for this project and `get_kicad_component_connections`
  showed empty `nets`/`connections` for J13/J15/J17, so the J13→channel-0
  mapping above rests on schematic sheet-instance identity
  (`/SaftyProcessor/CurrentSense/` etc.) and the documented GPIO/net table,
  not a live PCB netlist trace.

## Docs checked against this finding

- `firmware/SaftyFW/docs/CURRENT_SENSE.md` — consistent with the code and
  the raw counts; no correction needed.
- `firmware/SaftyFW/docs/HARDWARE.md` — consistent; line 870's "Current1/
  Current2 remain unpopulated" is corroborated, not contradicted, by
  today's capture.

No documentation defect found. No config or firmware change is proposed —
if the owner has physically clipped a CT onto a different jack since
2026-09-05, the actionable next step is the commissioning check itself
(`CURRENT_SENSE.md` §5 step 2, one relay at a time, owner present) the next
time a firing is run, not a config write made from this reading alone.
