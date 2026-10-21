# Review: PF fix (PF-01..PF-03), 2026-10-17

Scope: origin/dev 6d5b7830b (code), 4e6b9a8dc (test hardening), 5d6eb2fe7 (docs). Context:
`INVESTIGATE_PICO_FATAL_REBOOT_2026_10_17.md`. The rule: remove only the phantom reboot after an ESP boot,
never weaken real lost-trip or fatal-reboot protection. Review only; nothing was changed or run on the bench.

Counts: HIGH 0, MED 2, LOW 4, INFO 1.

## MED

### PR-01 MED: the first boot id after an ESP boot is never classified, so a real fatal Pico reset goes unseen
`drivers/safety/safety_link_frames.c:412,445`, `drivers/control/heat_enable.c:780`.
PF-01 makes the first boot id a baseline, and heat_enable takes its baseline from the first seq after
that. Nothing looks at `diag_boot_reason` of that Pico boot. Before the fix, the phantom bump classified it,
so a WATCHDOG/ASSERT/HARDFAULT boot reason latched `reboot_fatal_latched`.
Scenario: during a firing the Pico watchdog-resets (fatal). The ESP also goes down around the same
time (shared brownout, ESP panic, a dual flash, or a restart by the operator). After the ESP boots, the Pico's
DIAG says boot reason WATCHDOG, but the first-sight path drops it. The next `profiles_start` is granted with no
`pico_fatal_reboot` hold. The phantom came from the lost-trip (`was_tripped`) input, not from fatal boot bits,
so the fix removed more than it needed to.
Fix direction: on first sight, still apply the fatal-boot-bit rule to the current Pico boot reason. Drop only
the `was_tripped` lost-trip input, or gate it on Pico uptime < ESP uptime, meaning the Pico booted after the ESP.

### PR-02 MED: PF-02 expiry fires on any TRIPPED then ARMED of the new boot, including an unrelated trip
`drivers/control/heat_enable.c:961-975` (the expiry), `:820-828` (setting `latch_lost_trip_only`).
The comment says "a real lost trip reboots into ARMED without ever showing TRIPPED". That is not guaranteed.
After a Pico reboot the new boot often trips on its own. S6a mainFault during the link handshake is common
after a restart (`docs/agent_rules/FLASH_OTA.md`), and the physical cause of the lost trip may re-trip it.
Scenario: the Pico is TRIPPED (say over-temp), then brown-out reboots (POWERON, no fatal bit), so the trip is
lost. The lost-trip latch is set (lost_trip_only, latch_seq = new seq). The new boot trips S6a on the
handshake. The operator clears S6a, the DIAG goes TRIPPED then ARMED on the same boot, and the latch expires.
The original trip was never shown or cleared. This also applies to a latch set while a claim was held:
`latch_lost_trip_only` is set whatever `held_mask` is, so after the release the same TRIPPED then ARMED
expires it.
The DIAG state input is otherwise sound. It is fed only when `safety_diag_valid` (fresh) and only with
`diag_since_reboot`, otherwise INIT (`profile_executor.c:2632`), so an old-boot cached TRIPPED cannot set
`latch_tripped_seen`. The seq compare uses heat_enable's own `seen_reboot_seq`, updated in the same tick from
the same status snapshot, so no seq race was found.
Fix direction: expire only when the cleared trip covers the lost trip's reason. Record the pre-reboot
`diag_trip_mask` at latch time and require that bit in the TRIPPED DIAG seen before ARMED. Or require an
explicit operator acknowledgement of the lost-trip latch.

## LOW

### PR-03 LOW: `boot_id_unseen` is fail-open by default and goes stale when status reads fail
`drivers/control/heat_enable.c:234`, `drivers/control/profile_executor.c:2626`. init sets "seen". Only the
guard tick sets it, and only when `safety_link_get_status` succeeds. No start path acquires before the first
guard tick (no boot-resume claimant was found; the watchdog task is created in profile_executor init), so the
window is not reachable with a claim today. But if early status reads fail, the flag stays "seen" and an
acquire is not withheld. Fix direction: default `boot_id_unseen = true` in init (the tests already call
`note_boot_id_seen(true)`).

### PR-04 LOW: a boot id that never arrives withholds heat silently
`heat_enable.c:507`. A protocol-17 Pico (30-byte DIAG, boot id only in FW_VERSION) whose FW_VERSION is lost or
truncated keeps `boot_id_unseen` true. `profiles_start` succeeds with heat blocked (`profile_executor_run.c:1597`).
The run then stays cold with no pause reason, because `heat_enable_reboot_undecided` covers only
`reboot_classify_pending`. This fails closed but cannot be seen. Fix direction: a pause reason such as
`pico_boot_id_unknown`, and the flag in /api/status.

### PR-05 LOW: a reboot counted by uptime before the first boot id is absorbed into the baseline
`heat_enable.c:780`, `safety_link_frames.c:1150-1158`. While the boot id is unseen, the uptime path can still
bump `pico_reboot_seq`, but heat_enable ignores it. When the id arrives, the baseline is the bumped seq. With a
protocol-17 Pico and a lost FW_VERSION, a Pico reboot in that window (for example out of TRIPPED) is never
classified, so its lost-trip latch is lost. The window is short (FW_VERSION re-announce on DIAG), and no claim
can be granted in it.

### PR-06 LOW: the tests do not cover the cases above
`test/test_heat_enable.c:1503-1595`, `test/test_safety_link_compile.c:2170`. These cases have no test: a fatal
boot reason on first sight (PR-01); a new-boot trip for a different reason, cleared, expiring a lost-trip latch
(PR-02); a latch set with a claim held, then released and expired; status-read failure at boot (PR-03). The
heat_enable tests drive `note_boot_id_seen` by hand, and only a source-order check pins the guard-tick wiring
(`test_heat_enable.c:672`). The first-sight test checks only the seq. It does not check that the per-boot
bookkeeping reset still happens.

## INFO

### PR-07 INFO: on first sight, the per-boot reset still runs without a seq bump
`safety_link_frames.c:443`. `safety_note_pico_reboot_locked` clears `diag_since_reboot` and
`diag_boot_id_known` for the first id. Until the next DIAG, the guard tick feeds INIT and no reboot verdict is
pending. This is harmless (fail closed for one DIAG period), but it is a seq/flag pair that disagrees for a
moment.
