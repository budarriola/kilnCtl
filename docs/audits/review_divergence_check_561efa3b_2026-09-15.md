# Adversarial review: 561efa3b (Defect 2, broadened ESP/Pico divergence check)

Date 2026-09-15. Scope: `561efa3b`, plus the Pico-field capture it depends on
(`kiln_cfg_store_capture_expected_pico_fields()`, which `git log -S` shows landed with
`a93ee77b`). Context: `docs/audits/kiln_profiles_feature_review_2026-09-15.md` Defect 2.
Read-only review. **No sabotage experiment was run.** Every coverage claim below comes
from reading the build script and grepping call sites. None of it comes from a rebuild.

## Verdict

Wiring the check is correct, and it does not regress the Pico ceiling or the rule that
the Pico must never be unarmed. In production it does not do what the commit says.
The reference record is not "what the ESP last pushed". It is a snapshot of the Pico's
own cache, taken whenever the active slot is re-saved. Three things follow:

- The check clears itself after a revert (HIGH 1).
- It raises false alarms that block heat on ordinary operations (HIGH 2 and 3).
- The one scenario it was written for, a Pico reboot after a volatile swap, has no
  production producer today.

## Trace

1. **The check runs in production.** `main_control_bringup.c:147` registers the source.
   `safety_link_poll.c:297` calls `safety_ceiling_sync_reconcile_on_link_up()` on every
   tick while the link is up. `enforce_ceiling_divergence()` runs before the backoff.
   Consumer and producer both exist.
2. **Expected side.** The active slot's `kiln_pkg_safety_t` is read through
   `kiln_cfg_store_get_full_package()`. Params without the SET flag and `abs_max_temp_c`
   are skipped.
3. **Live side.** `safety_cfg_store`'s cache. The Pico's `config_store_get_config_crc()`
   (`SaftyFW/src/config_store_flash.c:875`) is computed over the live cached record, so
   a volatile install changes it. A reboot that falls back to the flash record changes it
   again. `safety_cfg_store_maybe_refetch()` then refreshes the cache. The trigger works.
4. **Comparison.** `config_divergence_check()` compares values field by field. A
   known/unknown mismatch counts as a difference. On divergence it forces all relays
   off and halts the run.

## Findings

**HIGH 1: the expected record is re-derived from the side it is checking.**
`kiln_cfg_store_autosave_from_live()` calls `kiln_cfg_store_save_current()`, which calls
`populate_pico_half_and_hash()`, which calls `kiln_package_capture_pico_half()`. That
capture re-snapshots the **live Pico cache** into the active slot. Autosave fires on
every zones `nvs_save()`, and those come from zones POSTs, autotune and coupling writes.

Suppose the Pico reboots and reverts. The latch trips. The next zones save then
overwrites the "expected" values with the reverted ones, and the latch clears
silently. There is no operator acknowledgement and nothing is pushed back to the Pico.
This is the "reset one side of a pair" bug class: the reference is derived from the
observed value.

Fix direction: do not recapture the Pico half while divergence is latched. Plan section
2.4 rule 6 already requires this and it is not implemented. Better, record the expected
set only when a push is confirmed.

**HIGH 2: the production apply path trips it (heat blocked, fails safe).**
`kiln_cfg_store_apply()` is the path behind the POST handler and the LCD. It imports
only the ESP half. It pushes nothing to the Pico (its own comment says "does not yet
push anything"), yet it sets `active_id = id`. Applying any slot whose captured Pico half
differs from what the Pico holds therefore latches the divergence immediately and forces
heat off. The only way out today is a zones save, which clears it through HIGH 1.

`kiln_cfg_swap_apply()`, the path that does push through 0x2D, still has no production
caller. So the reboot-revert case this commit targets cannot currently happen in
production. The new check only adds this false-positive path.

**HIGH 3: an ordinary Pico param commit trips it.**
Committing a safety param from the ESP (`safety_cfg_http.c`) never calls into
`kiln_cfg_store`, so the active slot is not recaptured. The Pico's CRC changes, the
cache refetches, and the slot stays stale. The result is a divergence and heaters off
until some unrelated zones save happens. Again this fails safe, but it is a nuisance
alarm on a normal commissioning action. The owner has warned that nuisance checks end up
being switched off.

**MEDIUM 4: "expected = last pushed/acked" is not what was built.**
Nothing ties the expected record to a 0x2D install. 0x2D has no success ACK, and the
swap confirms by reading the cache back, not by an ACK. The commit message says the
check relies on `config_crc`. That is misleading. The CRC is only a refetch trigger,
computed on the Pico alone. The ESP never recomputes it, so byte layout, endianness and
padding do not matter here. The comparison is by value, using the same type widening on
both sides (BOOL/U8 `&0xFF`, U16 `&0xFFFF`, F32 by bit copy against `row.value.*`).
No layout defect was found.

**MEDIUM 5: a stale cache hides a revert.**
When a refetch fails, the cache keeps its previous contents. `config_crc` is only zeroed
at init, line 429. After a Pico reboot the check keeps comparing against the pre-reboot
volatile values until a refetch succeeds. Retries back off to 30 s, and the audit notes
that refetch storms have run for hours without converging. Nothing marks the extra
fields unknown while the cache CRC differs from the live one. The ceiling field has the
same weakness, but that predates this commit.

**LOW 6: coverage is one-directional.**
Only params the slot captured as SET are checked. A param that is set on the Pico but
absent from an older slot is never compared. The cap is 96 against roughly 60 params, so
it is fine for now.

**LOW 7: the tests do not exercise the production seam.**
`test_broadened_field_divergence_detected()` uses a fake source and a fake cache. No
host test calls the real `kiln_cfg_store_capture_expected_pico_fields()`, the
registration in `main_control_bringup.c` (not built by `build_host_tests.ps1`), or its
interaction with apply and autosave. HIGH 1 to 3 are invisible to the suite. The
recorded negative test only proves the comparator loop, which is also the class
described in `project_negative_test_on_a_mirror_is_vacuous`.

## Checked and clean

- **Boot ordering.** Before the first fetch the ceiling field is already unknown, so the
  pre-existing check already latches then. The extra fields add no new boot-time false
  positive. `target_known == false` still short-circuits.
- **`abs_max_temp_c` parity and never-unarmed.** Field 0 is unchanged. The commit adds
  no Pico write and no disarm path. The enforcement only removes heat.
- **Stack.** The new arrays are static and have a single caller (safety_poll_task).
- **`kiln_cfg_store_apply()` override.** Moved inside `kiln_cfg_store_lock()` as part of
  the separate a93ee77b review follow-up. Correct. `msg[256]` with float varargs is
  fine.
- **Kiln swap and profile import.** Import does not change `active_id`, so it has no
  effect on the check until apply, which is where HIGH 2 applies.
