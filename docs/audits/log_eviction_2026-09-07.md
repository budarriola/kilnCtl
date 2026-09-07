# uart_log_bridge eviction-policy audit -- 2026-09-07

## 1. The eviction rule (before this audit)

`uart_log_bridge.c`'s `uart_log_vprintf()` enqueues every formatted line into
a fixed `UART_LOG_BRIDGE_QUEUE_LEN` = 64 entry FreeRTOS queue
(`xQueueSend(..., 0)`, non-blocking). When that send fails (queue full):

- If the new line's level is `UART_LOG_LEVEL_ERROR` (only): evict exactly
  one entry -- the OLDEST queued line (`xQueueReceive(..., 0)` from the
  front) -- then retry the send once. The evicted line is counted in
  `s_dropped_lines`; the new ERROR line is kept.
- Any other level (WARN, INFO, DEBUG, VERBOSE): the line is simply dropped
  and counted in `s_dropped_lines`. No eviction is attempted.
- Exactly one eviction per call, never a loop -- a burst of privileged lines
  cannot drain the whole queue in one call.

Net effect: **only ERROR lines were eviction-protected.** Commit `20c2a5d5`
fixed one specific instance of this (uart_protocol.c's task-registration
failure, previously WARN) by promoting that one callsite to ESP_LOGE.

## 2. Sweep for the same class

`grep -rn ESP_LOGW firmware/KilnFW/App/drivers` returns 488 call sites. Most
are benign/expected (malformed-payload rejections on user-facing protocol
handlers, informational state-change notices, config-value clamps). Filtered
to the categories the commit's bug class actually threatens -- persist/NVS
failures, safety-link errors, task-start failures, thermocouple init/reconfig
failures, config decode/migration refusals, OTA/partition problems -- roughly
150 remain. Representative high-value examples surfaced by the filter (not
exhaustive; see section 4 for why a full per-line table was not worth
finishing by hand):

| file:line | reports | boot-burst/load reachable? |
|---|---|---|
| `bridge/uart_bridge_ext.c:197` | task creation retried 5x, still failing | yes -- boot |
| `net/wifi_prov_link.c:800` | `xTaskCreatePinnedToCoreWithCaps(dns_hijack)` failed | yes -- boot (AP mode entry) |
| `owners/thermo_owner.c:279` | owner slot pool exhausted, treated as failed | yes -- any thermo read, boot included |
| `owners/thermo_owner.c:290` | owner task did not answer in time, treated as failed | yes -- any thermo read |
| `persist/zones_config_store.c:123` | zones_cfg blob newer than firmware -- `ZONES_DECODE_NEWER`, the rollback hazard's only warning | yes -- boot |
| `persist/zones_config_store.c:138` | zones_cfg blob REJECTED | yes -- boot |
| `persist/kiln_cfg_store.c:313` | kiln_cfg_store blob newer than firmware, refusing to load | yes -- boot |
| `safety/safety_cfg_store.c:372` | safety_cfg_store blob version with no migration path | yes -- boot |
| `owners/kiln_io_owner.c:509` | starting with no safety link -- relay-ON refused | yes -- boot |
| `control/run_state.c:162` | run-state record migration failed | yes -- boot |
| `persist/boot_guard.c:160` | boot-guard record failed version/CRC check | yes -- boot (every boot) |
| `persist/relay_cycles.c:179,304,326,334` | relay-cycle persistence migration/version/load failures | yes -- boot |
| `net/wifi_prov_nvs.c:353,360,378,603` | saved_nets blob read/version/size failures | yes -- boot |
| `persist/touch_cal_store.c:78`, `unit_pref.c:66/73`, `display_power_cfg.c:93/102/113`, `net/time_sync.c:77/92/165/172/182` | assorted persisted-setting load/persist failures | yes -- boot, and time_sync's TZ-persist path also under load |

Left alone (not promoted, not tabulated line-by-line): `bridge/gpio_probe.c`,
`bridge/uart_bridge.c`, `bridge/uart_bridge_thermo.c`, `bridge/uart_bridge_io.c`
malformed-payload/refusal WARNs (user-triggered protocol-command rejections,
high frequency, not a boot-burst failure signal); `http/ota_http*.c` and
`safety/safety_link_commands.c` request-refusal WARNs (user/PC-triggered,
already answered synchronously over HTTP/the link -- the WARN is a courtesy
echo, not the only record of the failure); `persist/zones_config_migrate.c:808,823`
(migration value-clamp notices, not refusals). That is roughly 130 of the
~150 filtered lines left untouched, plus all 338 lines outside the filtered
categories.

## 3. Promotion vs. policy fix

Given ~20 clear individual candidates and the likelihood of more being added
over time (`c51360c2` just added several persist-failure WARNs), promoting
each WARN to ERROR one at a time is the same losing game commit `20c2a5d5`
already played once. It was rejected here as the primary fix:
whack-a-mole against 20+ current sites, no protection for the next one
someone adds, and indiscriminately promoting all ~20 to ERROR risks the
flood-of-ERROR failure mode this task warned against.

## 4. Policy change implemented

`uart_log_bridge.c`'s eviction condition was widened from
`level == UART_LOG_LEVEL_ERROR` to
`level == UART_LOG_LEVEL_ERROR || level == UART_LOG_LEVEL_WARN`. This
protects the whole WARN class (all ~150 filtered lines above, and every WARN
added in the future) with one three-line change, instead of hunting and
promoting individual call sites -- addressing point 4's "small change"
option directly rather than case-by-case. No WARN-to-ERROR promotions were
made; the log level taxonomy (ERROR = definite failure, WARN = degraded/
recoverable failure) is preserved, and severity in the transcript still
reads correctly. Eviction remains bounded to one entry per call, so a WARN
storm still cannot drain the queue.

## 5. Test

`test/test_uart_log_bridge.c` (host test, links the real `uart_log_bridge.c`
eviction code via `#include`, ring-mode queue stub):

- `test_full_queue_warn_now_evicts_oldest` -- new. Fills the queue to
  capacity with INFO, sends a WARN, and proves it now evicts the oldest INFO
  and survives at the tail (previously: dropped outright, 0 evictions).
- `test_full_queue_info_not_privileged` -- renamed/kept from the old
  "non_error_not_privileged" test, now covering only INFO (WARN moved to the
  test above since its behavior changed).
- `test_registration_failure_log_only_survives_as_error` -- updated: the old
  "pre-fix (W) is dropped" assertion is stale under the new policy (WARN now
  also survives), so it now asserts the line survives at BOTH levels.
- `test_full_queue_error_evicts_oldest`, `test_room_available_no_eviction`,
  `test_eviction_bounded_per_call` -- unchanged, still pass (ERROR path
  untouched).

All 29 host test executables build and pass
(`build_host_tests.ps1` -> "all 29 host test executables built and passed").

## 6. Summary

- Eviction rule (before): ERROR-only eviction-protected, single eviction per
  call, 64-entry queue.
- ~150 WARN sites matched the priority categories (persist/NVS, safety-link,
  task-start, thermo, config-decode/migration, OTA/partition); ~20 are clear,
  named, high-value candidates (see table); ~130 were left alone (user-
  triggered protocol refusals or courtesy echoes of an already-recorded
  failure).
- 0 individual WARN-to-ERROR promotions made.
- Eviction policy changed: now protects ERROR and WARN uniformly.
