# Release hardening - pending work

Pending items only. History, rationale and the closed sections (0-1, 3, 6-9, 11-12) live in `docs/RELEASE_HARDENING.md` (same section numbers). Section numbers below match it, so older references stay valid.

Release means an unattended firing on a real kiln. Every item below is hardware- or owner-gated; no software-only work remains.

## 2. Release-duration soak (gate: hardware, a running firing)

No `stability_soak_*.csv` or audit record of release duration exists. The harness, slope test and negative test are done (`tools/PcTools/scripts/stability_soak.py`, `tools/PcTools/tests/test_stability_soak_trend.py`).

Bench steps:
1. Board idle-cold and healthy: `get_heap_status` shows no unacknowledged crash report.
2. Start a non-trivial profile, keep a web client polling and the LCD rendering.
3. `python tools/PcTools/scripts/stability_soak.py --host <board> --duration <release duration> --interval <s> --out logs/stability_soak_<date>.csv --firing-in-progress`
4. Pass: exit 0. Flat `crc_errors`/`timeouts`/`broadcast_dropped`, no DOWN trend on heap-free or stack-min-headroom%, internal heap well above the 8 KB owner floor and the ~11.9 kB socket-reset threshold.
5. Record the CSV path and verdict in `docs/BENCH_TEST_LOG.md`; the `soak-24h` release gate (`docs/release_gates.json`) reads that evidence.

## 4. Safety argument off the bench (gate: hardware / owner)

Of 21 tracked guard claims, 21 are host-tested and 3 hardware-verified (S5 fit, S5 masking-before-fit, KilnFW guard 6). The host-test-only bucket is empty (re-verified 2026-09-20), so nothing here is closable in software. Record results in `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` section 3.4 (entirely unexecuted).

- Bucket A, provokable now on this bench (hardware): S1 ceiling trip, S2 overshoot-sustained, S5 fault injection (out-of-band reading, `tc_type` mismatch), S6b both link-dead tiers, S7 press-to-open, S11 frozen sensor once its gate is reachable, KilnFW per-zone guards. Each: provoke, observe the trip, confirm relay state afterwards, record. Never `safety_clear_trip()` before checking `trip_mask == 1 << (trip_reason - 1)`.
- Bucket B, owner-blocked: S3, S4, S9, S14, S15 are inert until CT commissioning (`CT_COMMISSIONING_PLAN.md` steps 0 and 6) is done by the owner. The safety case must say they are inert until then.
- Bucket C, owner decision (not schedulable): S9 welded-contactor escalation (needs an AC-injection jig, none exists), S8 real rate threshold (needs a full-power ramp a 4 W fixture cannot make), E-stop pole 1 (unwired by owner decision), thermal magnitudes. For each the owner picks: build the jig, ship unproven with the safety case saying so, or sign off the risk. Put these to the owner early; "build the jig" is XL.

## 5. Failure injection and recovery (gate: hardware)

Build each as a repeatable script in `tools/PcTools/scripts/` with a defined end state (relays open, fault surfaced, firing aborted or resumed).

- Dead-link 30 s firing abort (size S, take it on the next firing): while a firing is RUNNING, run `tools/PcTools/scripts/bench_firing_abort_stopwatch.py` (`--poll-period`, `--window`). The 1.5 s staleness ceiling is already bench-verified.
- Pico reboot mid-firing: `tools/PcTools/scripts/bench_pico_reboot_midfiring.py --i-am-rebooting-the-pico-mid-firing` during a live firing (`--poll-period`, `--out`). Expect: ESP blocks heat, link recovers, dedup state re-established, no silent stall.
- Pico-side thermocouple fault on the safety processor's own MAX31856 (hardware).
- Power cut mid-write of the RP2040 `config_store` A/B sector, repeated, intact read-back each time. Judged unsafe on the only bench Pico; torn writes are proven only by the host power-cut harness. Owner decides whether to ever tear a real write.
- S6b persistent OpenOCD halt session (owner decision 2026-10-07: halt via probe, always resume).
- Welded contactor: bucket C above.

## 10. `cfg` partition un-park (gate: bench time plus owner)

Parked 2026-09-17; the bench board now has `cfg` mounted and populated (7 files), NVS still authoritative, `tools/check_cfgfs_never_gates_nvs.ps1` guards the best-effort property. Owner decision 2026-10-05: remove the NVS writers after the blob-drift fix and a re-soak. To close, per `docs/FILESYSTEM.md` (closing criterion; pending part in `docs/FILESYSTEM_PLAN.md`): 20 consecutive clean boots, one complete file-backed firing, one verified backup/restore round trip, then remove NVS writers. Nothing acts on `window_may_close` automatically.

## 13. Order

Soak (2) runs alongside everything. Guard provocation (4A) and failure injection (5) are the bulk of the hardware work and interleave; the dead-link stopwatch is taken on the next firing. Put the bucket C decisions to the owner first.
