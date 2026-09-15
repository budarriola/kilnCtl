# Review: safety-processor thermocouple type settable (`61ebbfe4`, `316705de`)

2026-09-15. Adversarial, read-only review (no flashing). Owner request: "Safety
processor thermocouple type should be settable."

## Verdict

Not done as described. The live-reapply mechanism in `61ebbfe4` works in
isolation, but the path the owner's UI uses never reaches it. Its safety claim
("the ARMED gate blocks mid-firing") misreads what ARMED is, and no test would
catch a runtime break. `316705de`'s test does not test tc_type at all.

## Findings

### F1 (HIGH): ARMED is the normal idle state, not "firing"
`relay_grace_tick()` (`SaftyFW/src/tasks/relay_grace.c:5-15`) moves GRACE to ARMED
60 s after boot, whether or not anything is heating. From then on ARMED stays set
until a trip. `config_store_decide_write()` (`config_store.c:1121`) refuses every
flash write while ARMED. The consequences:

- `SET_CONFIG` (0x16) and `COMMIT_CONFIG` both refuse on any healthy board more
  than 60 s after boot. A tc_type change only lands in the first 60 s after a
  boot, or while TRIPPED.
- The commit message says the gate "refuses the change mid-firing". That is true,
  but only because it refuses at all times. The reapply is therefore reachable
  only while the relay is de-energized. That is safe, but it is not the feature
  the commit describes.
- With the "Pico should never be unarmed" rule, the only way to change the type
  is a reboot followed by a write inside the 60 s window.

### F2 (HIGH): the owner's UI path never triggers the reapply
The web commissioning page (`safety_commissioning_page.html:506`, field 261
`tc_type`, enum B/E/J/K/N/R/S/T, `confirmOnChange`) POSTs to
`/api/safety/commissioning`. That becomes SET_PARAM plus **COMMIT_CONFIG**.
`link_task_handle_commit_config()` (`link_task.c` ~2210-2222) calls
`current_task_reload_cal()` but **not** `thermo_task_request_tc_type_reapply()`.
Only the legacy `SET_CONFIG` handler (`link_task.c:1529`) calls it. So a type
committed from the UI takes effect at the next boot (which main.c:525 does apply),
not live. The `APPLY_CONFIG_VOLATILE` path (`link_task.c` ~2301) does not call it
either (see F4).

### F3 (MEDIUM): two writers own tc_type and can overwrite each other silently
- Writer A is the commissioning page (param 0x0105 through COMMIT_CONFIG). It sets
  `CONFIG_STORE_SET_TC_TYPE`.
- Writer B is the ESP zones config `safety_tc_type` (`safety_config_page.html:291`),
  pushed by `safety_sync_tc_type()` (`KilnFW/.../safety_link_poll.c:173-197`) on
  every change and every link reconnect as SET_CONFIG.
  `link_frame_apply_set_config()` overwrites tc_type and does not set the
  fields_set bit.

Writer B is fire-and-forget. `tc_type_last_sent` is updated when the send returns
`ESP_OK`, even if the Pico then refuses it because it is ARMED (which, per F1, is
nearly always). The result is an ESP that believes it synced and a Pico still on
the old type, with no retry until the next reconnect. If a Pico write does land
(during GRACE or TRIPPED), B can also silently replace a commissioned type from A.
The `config_crc` divergence check may catch the difference only if the saved kiln
package carries 0x0105.

### F4 (MEDIUM): a volatile install can change tc_type on an uncommissioned board without reconfiguring the part
`config_store_volatile_would_loosen_safety()` refuses a tc_type change while ARMED
only when `cur` already has `SET_TC_TYPE`. On an uncommissioned board the install
lands in the RAM cache, but no reapply runs. The MAX31856 keeps the old CR1 type
and `max31856_tc_type_verified()` stays true. thermo_task then applies the
plausibility band for the new type to readings linearized under the old one. This
is exactly the "plausible and wrong" state the verification flag exists to prevent.
Fix: call the reapply from the volatile and COMMIT paths whenever tc_type changes.

### F5 (MEDIUM): no behavioural test of the reapply
The only test file the commit touched is the stack-budget script. The runtime
negative test: in a clean worktree (`C:\wt\tcrev`), `s_force_tc_reconfigure = true;`
was replaced with a no-op, followed by a fresh full SaftyFW host-test build.
Result: **255/255 passed, "all passed"**. `thermo_task.c` is not linked into any
host test (only `thermo_task_drdy_recovery.c` is). The sabotage was restored by
hand, `git diff` came back empty, and the worktree was removed. The "sabotage,
then build failure" check in the commit proved only that the code compiles.

### F6 (MEDIUM): `316705de`'s tc_type test is vacuous
`test_tc_type_revert_divergence_detected()` sets `param_id = 0x0105`, then
overwrites it with `FAKE_EXTRA_PARAM_ID` before the assertion. It runs the same
code with the same id as the existing `test_broadened_field_divergence_detected()`,
so it would pass even if tc_type were excluded from the capture. The commit says
itself that no production path is exercised beyond the existing test. The name
gives false assurance.

### F7 (LOW): stale comment
`SaftyFW/src/main.c` ~520 still says "there is still no commissioning UI/wire
command that ever WRITES a non-default tc_type".

## Checked and OK
- **Range checks.** Pico: `config_params.c:386/590` (`CHECK_U8_MAX 7`) and
  `link_task_handle_set_config` (`> MAX31856_TC_TYPE_T`). `config_store.c:478`
  clamps a CRC-valid out-of-range byte. ESP: the zones setter is bounded by
  `ZONE_TC_TYPE_MAX_REAL`. The bridge allows up to 0x0F, but the Pico rejects
  anything above 7. The page is an enum with a type-to-abs_max contradiction
  check, enforced on both client and server (`config_params.c:771-792`).
- **Boot apply.** `main.c:525` calls `max31856_configure(config_store_get_tc_type())`.
- **CR1 verification uses the configured type.**
  `max31856_cr1_readback_check(tc_type, cr1)` (`max31856.c:286`) is not hardcoded
  to K. The flag is cleared at entry (`:205`).
- **Idle spurious trip.** The reapply runs synchronously inside one thermo_task
  iteration, and S5 needs streak, time and a 60 s `blind_grace_s` to trip. A single
  invalid sample cannot trip it. A wrong type that reads implausible will
  eventually trip S5 (latches TRIPPED), which is the correct fail-safe. The reapply
  never touches relay state.
- **Flag race.** The flag is read, then cleared, then `config_store_get_tc_type()`
  is read afterwards. A request landing between the read and the clear is lost,
  but the configure that follows still reads the newest committed type. That is
  benign.
- **Config-store atomicity (D2).** Fixed and bench-verified 2026-09-14
  (`docs/CONFIG_FILESYSTEM.md`). The flash write now reads back and verifies before
  the RAM cache adopts the value, and the reapply is only requested after
  `config_store_write()` returns true. There is no new interaction.

## Recommended fixes
1. Call `thermo_task_request_tc_type_reapply()` from the COMMIT_CONFIG and volatile
   handlers whenever the committed or installed tc_type differs from the previous
   value (F2, F4).
2. Pick one source of truth for tc_type. Retire the zones-config `safety_tc_type`
   SET_CONFIG sync, or make it confirm through GET_CONFIG_PAGE before updating
   `tc_type_last_sent` (F3).
3. Decide with the owner how a type change is meant to happen while ARMED is the
   steady state: reboot inside the grace window, or a narrow carve-out that only
   runs while de-energized (F1).
4. Extract the reapply decision into a host-testable policy function and
   negative-test it at runtime. Make the 316705de test use the real 0x0105 row
   (F5, F6).
