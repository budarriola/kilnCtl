# Review: saftyfx and picofx fix sets (2026-10-14)

Adversarial review of two fix sets on origin/dev. I did not touch the board.

- **saftyfx** fixes A1-A5 of `AUDIT_SAFTYFW_GUARDS.md`. Commits: `0cc88612f`, `ff41e2c23`, `8bfbddf99`, `217ca9659`, `59773ecc3`, `e9e8de609`.
- **picofx** fixes HIGH-1 and MED-1 of `AUDIT_PICO_UPDATE.md`. Commits: `76544b804`, `6a415e86e`, `adc5d14fd`, `64b9aba14`.

Reviewed at `e9e8de609`.

**Counts:** HIGH 0, MED 0, LOW 6, INFO 7.

## What was checked

### Bypass of A1 (REBOOT/ROLLBACK while S9 is latched)

The Pico has exactly two software reboot sites:

- the rollback path, `update_task.c:1594`;
- `reboot_now`, `update_task.c:1666`.

Both are gated by `update_task_s9_latched()`, at `update_task.c:1512` and `:1648`. `process_end` deliberately does not reboot.

While the guard is tripped, `safety_guards_tick()` returns early after the S8/S9 handling. So `trip()` cannot overwrite the `TRIP_INEFFECTIVE` reason once it has latched, and the gate cannot be blinded by a later reason change.

On the ESP, `pico_auto_update` never sends REBOOT.

### A4 refusal (CLEAR_TRIP while S9 evidence accumulates)

The refusal is at `safety_guards.c:411-415`. It cannot deadlock recovery:

- If current persists on a commissioned chain, the streak latches S9. That is the intended unclearable state.
- If current stops, the streak resets to 0 and the clear is allowed again.

### Flash layout

The backup sector is `0x1C1000..0x1C2000`, starting exactly at the config region's end (`0x1B1000 + 0x10000`). It overlaps none of these:

- boot2;
- the bootloader, including the pubkey at `0xFD00`;
- the metadata sector `0x10000`;
- slot A `0x11000..0xE1000`;
- slot B `0xE1000..0x1B1000`;
- the config store A/B sectors.

`update_task.c`'s `hal_flash_map` region covers the whole flash, so reading the backup is in range. No PcTools tool erases near `0x1C1000`.

### Power-loss windows

Both writers erase, program and memcmp-verify the backup before erasing the main sector, and refuse without touching main when the verify fails:

- the bootloader, `bootloader/persist.c`;
- the app, `update_task_metadata_write.c:22`.

`find_latest_ab` (`bootloader/metadata.c:211`) takes the backup only when main has no valid record or the backup's seq is strictly newer. So each cut point lands on the old or the new record:

- a cut during the backup write leaves main intact;
- a cut after the main erase falls back to the backup;
- a torn main program also falls back to the backup.

`FROM_BACKUP` is handled at every `latest` use:

- `next_write_slot` returns 0;
- `next_write_needs_erase` returns true;
- `wrap_needs_backup` returns false;
- the app's `backup_src_offset` is computed only when `wrap_needs_backup` is true (`update_task.c:636`), so the sentinel is never multiplied into an offset.

### Compatibility of the new reply reasons

The new values are REBOOT reason 4 and ROLLBACK reason 5.

- **Older ESP with newer Pico:** the ESP maps the unknown value to its generic refusal text. The command is still refused.
- **Newer ESP with older Pico:** the ESP refuses up front with 409 (`sw_reset_http.c:417`) when the diag reason is fresh. When the diag reason is not fresh, the older Pico accepts the reboot. That is the pre-fix behaviour, not a regression.

No protocol bump is needed.

### SET_PARAM ceilings on re-save

The ESP defaults in `safety_cfg_http.c` are 75/60/200/85/10/120. All are inside the ceilings 600/300/60/125/300/500 (`config_params.c:495-601`).

The ceilings apply at SET time only. A value that is already stored still boots and is still honoured. The backup export does not carry these params. See LOW-6 for kiln config packages.

### Negative tests

I ran two negtests with `tools/negtest.ps1 -Preset saftyfw-host`. The baseline passed and the real tree was unchanged. Both mutants compiled, and both were CAUGHT by an assertion, not by a build failure:

- `a4_streak_branch_removed` replaced the `s9_current_present_streak > 0u` operand with `false`. It failed at `test_safety_guards.c:5305`: "A4: clear refused while the S9 streak counter is non-zero".
- `valid_stale_count_not_cleared` forced `needs_metadata_update = false` in the VALID branch. It failed at `test_bootloader_metadata.c:252`: "stale count on a VALID slot is cleared".

`test_bootloader_persist.c:66` cuts power at every flash op of a wrap and checks the outcome at each cut. It is not vacuous.

## LOW

### LOW-1: STATUS sends 0 A, not NaN, before the first current publish

**Where:** `firmware/SaftyFW/src/tasks/link_task.c:892`. The call is `status_amps_for_wire(true, ...)`, with `published` hardcoded to true.

**Why:** `current_task_get_snapshot()` returns void and gives a zeroed snapshot (timestamp 0, amps 0) before the first publish. In the first 2 s after a Pico boot, `now - 0 < 2000`, so the zero snapshot reads as fresh.

**Scenario:** The Pico reboots mid-firing. For up to 2 s, STATUS reports 0.0 A on every channel as valid data. This contradicts the A3 doc and the header claim that a never-published snapshot yields NaN. The ESP's H9 and the UI read "no current" instead of "unknown".

**Severity:** Low. The heaters are off across a Pico boot anyway.

**Fix:** pass `cur.timestamp_ms != 0u` as `published`.

### LOW-2: the A1-A3 resolution text cites SHAs that are not on dev

**Where:** `docs/audits/AUDIT_SAFTYFW_GUARDS.md:293`, `:302` and `:306`.

**Why:** These lines cite `28a446413`, `ee03954e6`, `4ec84cb64` and `c1ccd70a4`. None of them is reachable from origin/dev; they are pre-rebase SHAs. The landed commits are `0cc88612f`, `ff41e2c23`, `8bfbddf99` and `217ca9659`.

**Scenario:** A reader runs `git show` on a cited SHA and gets "bad object". The trail from finding to fix is broken.

### LOW-3: the flash layout comment still calls the backup's space unused

**Where:** `firmware/SaftyFW/bootloader/flash_layout.h:126-128`.

**Why:** The comment still describes "Reserved headroom ... ..0x1C2000, 56K, plus 0x1C2000..0x200000, 252K" and says nothing addresses it. It was not updated for the new `0x1C1000` backup sector.

**Scenario:** A future change reads the comment, places a new region in the "free" `0x1C1000` space, and overwrites the metadata backup. On the next torn wrap, the boot falls back to a corrupt backup.

**Fix:** update the comment to name the backup sector, and the remaining headroom as `0x1C2000..0x200000`.

### LOW-4: a doc comment in `safety_link.h` now sits above the wrong declaration

**Where:** `firmware/KilnFW/App/drivers/safety/safety_link.h:1897`.

**Why:** The new `SAFETY_LINK_TRIP_REASON_INEFFECTIVE` define and the `safety_link_get_diag_trip_reason()` declaration were inserted between the doc comment of `safety_link_get_diag_flags()` and its declaration at `:1905`.

**Scenario:** The diag-flags documentation now reads as if it describes the trip-reason accessor.

### LOW-5: a confirmed (VALID) slot that later crash-loops never falls back

**Where:** `firmware/SaftyFW/bootloader/metadata.c:300`.

**Why:** The VALID early return makes this intended (`BOOTLOADER.md:112`, `:143`), but the trade-off is not written down. Before HIGH-1 was fixed, three resets of any slot marked it BAD and switched slots. That was the bug, but it also acted as a fallback for an image that confirmed once and then started hard-faulting every boot.

**Scenario:** The new image passes its confirm checklist and goes VALID. A later persisted SET_PARAM value, or a sensor state, makes it fault during init on every boot. The watchdog resets loop forever. The bootloader no longer switches to the still-VALID other slot. Recovery then needs BOOTSEL or SWD.

**Severity:** The failure direction is safe. Relays stay de-energized, and the ESP sees S6b. The cost is availability only.

**Fix:** document the trade-off in BOOTLOADER.md. Optionally, add a separate fault-reset counter that only a watchdog reset increments.

### LOW-6: the new SET_PARAM ceilings could refuse an old kiln-config package

**Where:** `firmware/SaftyFW/src/config_params.c:495-601`.

**Why:** Kiln config packages (`kiln_pkg_safety_t`) re-send their captured params through SET_PARAM (`safety_cfg_write_apply_package_and_confirm`).

**Scenario:** A package captured before this change holds, say, `link_dead_hard_s = 900` or `tc_disagreement_c = 600`. That value is now refused, so the apply fails part-way with a generic SET_PARAM refusal. I found no evidence that any bench value or saved package exceeds a ceiling. The default and the exported values all fit.

**Fix:** none needed now. Name the param and the ceiling in the refusal if this ever comes up.

## INFO

- **INFO-1:** A watchdog or power-on reset still clears a latched S9. The A1 gate covers only commanded reboots. This was already true and is outside the scope of this fix.
- **INFO-2:** `safety_core.c:1052` sets `any_current_present = current_any_present() && current_fresh`. A stale current snapshot therefore reads as "no current", which blinds S9 and makes the A4 refusal permit the clear. This blindness existed before; A4 inherits it and does not make it worse.
- **INFO-3:** The commit and the doc say H9 holds on "all-NaN amps". But `ct_leak_alarm.c:82` holds when any relevant channel is NaN and none is above the floor. The code is the safer reading. The Pico never sends NaN per channel today, so the two readings never differ in practice.
- **INFO-4:** The `sw_reset` 409 (`sw_reset_http.c:417`) blocks the ESP's own restart while S9 is latched. Other ESP-only restart paths do not apply this check: the OTA rollback (`ota_http_esp.c:732`) and `recovery_enter` (`ota_http_recovery.c:257`). Neither clears the Pico's S9, so the difference is harmless, but the policy is inconsistent.
- **INFO-5:** An A4 refusal returns `false` from `safety_guards_try_clear()` with no distinct CLEAR_TRIP reply reason. The operator sees a generic refusal, not "S9 still verifying".
- **INFO-6:** Only the pure gate functions are unit-tested: `update_task_reboot_policy_decide` and `update_task_rollback_gate_decide`. The `update_task_s9_latched()` call sites in `update_task.c` cannot be reached from the host tests. The audit doc says so.
- **INFO-7:** The flash layout, the power-loss ordering and the `FROM_BACKUP` sentinel handling were all verified clean, as described under "What was checked".
