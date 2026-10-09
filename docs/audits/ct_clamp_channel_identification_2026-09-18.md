# CT clamp channel identification, 2026-09-18

## Purpose

The owner states one current-transformer (CT) channel on the safety
processor's front end is definitely connected and clamped on, and asked for
the channel and the relay/zone it measures to be established by causal
measurement (energize a load, watch for a step) rather than by idle-reading
inference. Idle counts alone (channel 2 elevated at 76-93 counts against
channels 0/1 flat at 16-17) are consistent with a clamp, but are equally
consistent with lead pickup on an unclamped but connected lead, so this
audit is the measurement that was asked for.

## Precondition check (before any relay command)

Read via `safety_get_status()`, `safety_get_diag()`, `safety_get_fw_version()`,
first read then re-read roughly 65 s later (a `safety_capture_ct_counts`
baseline capture occupied the gap):

- link up; state armed; `relay_owner not tripped`
- `trip_reason 0 [SAFETY_TRIP_NONE]`, `trip_mask 0x0000`, `warn_mask 0x0000`
- Pico build commit ae3160df, `boot_id=216`, `config_version=162`,
  identical on both reads
- uptime went from 12228006 ms to 12310006 ms, a delta of 82000 ms matching
  the ~82 s of wall time between reads (not a reset)

The board was confirmed armed, untripped, and not mid-reboot/mid-flash, so
relay actuation proceeded.

## Method

All relay actuation used the sanctioned `io_set_relay(relay, on)` MCP tool
(kiln_io_owner-mediated; the tool itself reports a refusal rather than a bare
ACK if a profile, safety fault, or OTA blocks the command). No raw GPIO or
I/O-expander register writes were used. Relay numbering is the schematic
numbering the tool documents: 1=K3/J8, 2=K1/J3, 3=K2/J4, 4=K5/J11. Only one
relay was ever energized at a time; each was returned to OFF, with
`safety_get_status()` re-checked clean, before the next was tried.

Sampling used `safety_capture_ct_counts(seconds=...)`, which polls
`/api/status` at the safety link's own ~500 ms POWER cadence and reports
per-channel mean/std/min/max in raw ADC counts.

## Baseline (all relays OFF, 65 s, 284 samples)

| channel | mean | std | min | max |
|---|---|---|---|---|
| ch0 | 16.00 | 0.000 | 16 | 16 |
| ch1 | 17.00 | 0.000 | 17 | 17 |
| ch2 | 85.62 | 3.605 | 81 | 99 |

Channel 2's baseline noise (std ~3.6 counts) is roughly 40-50x channels 0/1's
(std 0.000, i.e. below the ADC's least-significant-count resolution over this
window), consistent with the documented noise ratio. Any candidate step on
channel 2 has to clear several counts to be distinguishable from this noise;
a step of a fraction of a count on channels 0/1 would already be visible
against their silent baseline.

## Per-relay result (25 s energized, then OFF, status re-verified clean each time)

| relay (schematic) | zone claiming it (relay_mask) | ch0 mean/std | ch1 mean/std | ch2 mean/std | verdict |
|---|---|---|---|---|---|
| 1 (K3/J8) | zone 0 (0x01) | 16.00 / 0.000 | 17.00 / 0.000 | 85.78 / 3.587 | no step on any channel |
| 2 (K1/J3) | zone 1 (0x02) | 16.16 / 0.365 | 17.00 / 0.000 | 85.60 / 2.962 | ch0 moved +0.16 counts, ~0.4x its own new std and with zero baseline std to compare against; noise-level, not a step |
| 3 (K2/J4) | zone 2 (0x04) | 16.09 / 0.282 | 17.00 / 0.000 | 88.23 / 5.508 | ch2 moved +2.6 counts, well inside its own baseline std (3.6); not distinguishable from noise |
| 4 (K5/J11) | none (unclaimed relay) | 16.00 / 0.000 | 17.00 / 0.000 | 87.06 / 4.389 | no step on any channel |

Channel 2's mean drifted slowly upward across the whole session regardless of
which relay (if any) was energized — 85.6 -> 85.8 -> 85.6 -> 88.2 -> 87.1 —
which is consistent with slow thermal/offset drift on that noisy channel, not
a response to any particular relay. No channel showed a delta from its own
baseline that exceeded its own baseline noise for any of the four relays.

All relays were confirmed back OFF at the end, with `safety_get_status()`
clean (armed, untripped) throughout and after.

## Headline finding

**No relay produced a current-transformer step distinguishable from baseline
noise, on any channel.** Given the fixture's own documented ~70 mA total
draw and the CT front end's rectified-peak-hold response, this is a
plausible outcome rather than a failed test: the load switched by any single
relay on this fixture may simply be below what any of the three CT channels
can resolve above their own noise floor, or the physically clamped CT (if
its lead truly is on one of the three ADC channels) may not be clamped
around a conductor that carries any of these four relays' load current at
all.

This measurement therefore does **not** confirm channel 2's idle elevation
is caused by a clamp around any of the four relay loads tested. It remains
equally explicable as lead pickup on an unclamped-but-connected lead, per
the caution in the task itself.

## Configured intent (read-only, for comparison)

- `control_get_zones()`: 3 zones, 4 relays total. `relay_mask` zone0=0x01,
  zone1=0x02, zone2=0x04 (relays 1/2/3 respectively by the schematic
  numbering above). Relay 4 (K5/J11) is claimed by no zone.
- No `ct_mask`, `ct_channel_map`, or per-zone CT-channel field was found on
  any zone via `control_get_zones()` or any other tool reachable through
  `kiln_find()` — none of the currently published MCP tools expose a
  per-zone CT-channel assignment field. This absence is itself a finding:
  there is no explicit configured per-zone CT attribution to compare against
  a measured one.
- `safety_get_commissioning()`: `ct_topology=summed` — the safety layer's
  S15 under-current guard is configured to treat the three CT channels as
  one summed presence signal per the whole board, not per zone. All three
  channels report S14 (over-current, per-channel) and S15 (under-current,
  per-zone) as DORMANT with `i_normal_a not measured`, i.e. none of the
  three channels has ever had a normal-operating-current baseline
  commissioned.
- `safety_get_ct_cal()` (the legacy end-to-end amps correction table) reports
  all three channels "uncalibrated" — this is the known caveat that this
  table is unrelated to and does not reflect the live ADC counts read by
  `safety_capture_ct_counts()`; it was checked only to rule it out as a
  calibration source, not used as evidence.
- No tool exposing raw per-channel calibration fields (`k_ct_v_per_a`,
  `zero_counts`, `gain`, `i_present_a`) was found in this session's
  `kiln_find()` results; if one has landed elsewhere it was not reachable
  from the facade at the time of this audit.
- `safety_get_status()` throughout this session reported `ct zone: -`,
  i.e. the live safety status itself reports no CT-to-zone attribution is
  currently active.

## Agree or disagree verdict

**Neither agreement nor disagreement can be established**, because there is
no configured per-zone CT-channel attribution published anywhere in the
current tooling to compare a measurement against — `ct_topology=summed`
plus the absence of any `ct_channel_map`/`ct_mask` field means the
configuration simply does not assert "zone N reads channel M" at all right
now. Separately, the causal measurement itself came back negative: none of
the four relays produced a current step distinguishable from noise on any
channel. So the honest state of this investigation is: channel 2's elevated
idle reading remains unattributed to any specific relay/zone by measurement,
and there is no explicit configured claim on file to check it against
either. The strongest statement supportable by today's data is the prior
noted in the task: channel 2 is the only channel with any non-trivial
reading and the only one with an idle-vs-flat contrast, but this session's
causal test did not confirm (or refute) that this is due to a clamp on any
of the four relay loads on this fixture.

## Recommendation

If a stronger causal signal is wanted, a higher-current load than the
~70 mA fixture (e.g. a bench heater load switched through the same relay,
if the owner is willing to authorize it) would give a step large enough to
clear channel 2's ~3.6-count baseline noise with margin, unlike the loads
available on this fixture today.
