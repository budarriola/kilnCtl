# Investigation: pico_fatal_reboot pause at profile start (2026-10-17)

Bench main 176bd438c on both chips (benchmain3). `profiles_start(1)` ran a 30 C single-segment profile
with no heat demand. About a minute later the executor showed paused, pause_reason `pico_fatal_reboot`.
The investigation was read-only on the board: no flash, no writes, no trip clears, no probe halt.

## Verdict
The Pico did not reboot. The ESP reported a reboot verdict that it had latched **before** the run
started. The run's first heat-enable acquire consumed that latch. Best hypothesis: the latch came from a
spurious claimless "reboot" that the ESP counted at its own boot. A Pico TRIPPED state from the
dual-flash/restart window, or from TEST_TRIP, was snapshotted at that moment and latched as a "lost trip".

## Evidence (read 2026-10-17)
- `safety_get_diag`: boot reason **power-on**, state armed, trip_mask 0, Pico uptime 13 098 s.
  The ESP log stamp at profile start was 12 996 s. So the Pico has been up longer than the ESP, and it
  did not restart at or after the start.
- `safety_get_link_stats`: timeouts 0, resync 0, crc mismatch 0, 4 framing errors, 6 length
  mismatches, trip_event 4, fw_version 5. No link loss.
- ESP log at the start:
  - `W (12996340) heat_enable: heat-enable acquire (firing) WITHHELD: Pico reboot verdict pending or fatal`
  - `W (12996340) profile_executor: start: heat_enable_acquire_since() failed ...`
  - `W (12998510) heat_enable: heat-enable released ...`

  The acquire was refused at the very first tick. No reboot was seen during the run. The ring no
  longer holds the ESP boot lines.
- The Pico's own persisted fault info: no tool exposes a Pico-side fault record beyond DIAG
  boot_reason, which says power-on. Neither `pico_reboot_seq` nor `reboot_fatal_latched` is exposed
  over HTTP or MCP.

## Mechanism (source, origin/dev)
- `heat_enable.c` `heat_enable_acquire()` (~l.477-490): an unconsumed `reboot_fatal_latched` becomes
  `reboot_hold` on the next acquire. The acquire is then WITHHELD. `profile_executor.c:2636-2639` pauses
  with `pico_fatal_reboot` on the next guard-9 tick.
- `heat_enable.c` `heat_enable_note_pico_boot()` (l.785-798): when no claim is held, a reboot latches
  fatal if boot_reason has a fatal bit **or** `reboot_was_tripped`. That flag is the last DIAG state at
  the moment the reboot seq bumped (T3 lost-trip rule). The latch survives for hours, until the next
  acquire. Only an acquire or init clears it.
- `safety_link_frames.c:409`: `boot_id_changed = !pico_boot_id_ever_seen || ...`. The first FW_VERSION
  after an ESP boot always counts as a Pico reboot (`pico_reboot_seq++`), unless a 32-byte DIAG was seen
  first. The guard-9 loop feeds `heat_enable_note_pico_boot()` whenever `safety_link_get_status()`
  returns OK, even before any frame has arrived. heat_enable can therefore take seq 0 as its baseline,
  then see 0->1 when FW_VERSION lands. That is a phantom reboot.
- After an ESP restart or dual flash, the Pico is commonly TRIPPED by link loss (S6a). If the phantom
  bump lands while the cached DIAG says TRIPPED, `reboot_was_tripped` is true. The verdict then latches
  fatal ("Pico rebooted after a TRIPPED DIAG ... latching the reboot fatal"). Clearing the trip later
  does not clear the latch.

## Earlier bench actions
- Dual flash and ESP restart: the likely trigger. They give the ESP-boot phantom bump plus a
  link-loss-tripped Pico.
- Erased ESP partitions (cfg empty, E-stop attestation lost): no path into heat_enable. Not a cause.
- TEST_TRIP and clear: no reboot by itself. It only contributes if a reboot-seq bump (phantom or real)
  landed while the test trip was latched. That was not seen in the log, and it is less likely than the
  boot race.
- Zone actuals 0.0 C invalid: separate. The bench TCs and zone sensors were not configured after the cfg
  wipe. Unrelated to the latch.

## Not confirmed
The ESP boot log lines (the claimless-latch ERROR) have rotated out of the ring. Confirming needs one of
these:
1. Restart the ESP with the Pico TRIPPED. Watch for "Pico rebooted after a TRIPPED DIAG ... no heat claim
   held", then a WITHHELD acquire.
2. A host test: the guard-9 loop runs before the first FW_VERSION, with a cached TRIPPED DIAG.

## Proposed fix (not implemented)
1. `safety_link_frames.c`: do not count the first boot id ever seen since ESP boot as a Pico reboot.
   Seed the baseline only. A reboot is a change from a known id, or the uptime regression.
2. `heat_enable_note_pico_boot()`: take no baseline until the link has seen a boot-id frame. One way is
   to expose `pico_boot_id_ever_seen` in the status.
3. When a later DIAG of the same boot shows ARMED after an operator clear, drop a claimless
   lost-trip latch. Or at least expire it.
4. Diagnostics: expose `pico_reboot_seq`, `reboot_fatal_latched` and the latch cause in `/api/status`.
   Use a distinct pause reason for a pre-start latched verdict, for example
   `pico_reboot_verdict_latched`. The UI should not claim that the safety processor "rebooted after a
   fatal fault" when it did not.
