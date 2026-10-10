# Host test coverage gaps, round 2 (2026-10-10)

Follows `HOST_TEST_COVERAGE_GAPS_2026-10-09.md` (campaigns 1-10). Campaigns 8
(relay/aux/exec HTTP handlers) and 10 (persistence stores/parsers) were being
run by other agents and are excluded here. Method is the same heuristic map:
look for modules with a safety, heat-path, persistence, parsing or update role
whose functions have no host-test caller. No coverage instrumentation was run.

## Top 10

| # | Module | Role | Why it matters | Status |
|---|--------|------|----------------|--------|
| R2-1 | `drivers/control/ct_leak_alarm_service.c` | H9 CT leak alarm glue | Pure core was tested; the glue (param scan, default-safe `ct_installed`, link-stale gating, activity suppression, relay-shadow gating, 500 ms throttle) was not. A wrong default silently disables the alarm. | DONE: `test_ct_leak_alarm_service.c` (26 checks) |
| R2-2 | `SaftyFW/src/tasks/discrete_task.c` | E-stop (S7) and mainFault (S6a) sampling loop | Policy files were tested, the task wiring pins, polarity, debounce and watchdog check-in was not (only source-text tests). | DONE: `test_discrete_task_loop.c` (30 checks) |
| R2-3 | `drivers/owners/kiln_io_owner.c` `owner_task()` dispatch | Relay-pin reconfig refusals, RegData relay-ON gate | Header of `test_kiln_io_owner.c` stated dispatch was uncovered because the queue stub never delivers; predicates were tested, the switch wiring them to `KILN_IO_OWNER_SX_REFUSED_RELAY` was not. | DONE: two dispatch tests added to `test_kiln_io_owner.c` (via shadowed `xQueueReceive` over the ring stub). `CMD_SET_RELAY` and `CMD_SX_RESET` dispatch still open (see R2-3b). |
| R2-3b | `kiln_io_owner.c` `CMD_SX_RESET`, `CMD_SET_RELAY*` dispatch | Relay shadow clear after expander POR | Needs real `kiln_io_reset_and_reinit` against the fake I2C chip from `test_kiln_io_sx_fake.c`. | Open, campaign R2-A |
| R2-4 | `link_task.c` SET_PARAM and update commands | ESP-to-Pico command handling | Open from campaign 2; parameter writes reach safety config. | Open, campaign R2-B |
| R2-5 | `saftyfw_image_identity_record` | Update identity record read/write | OTA/update chain; a wrong record mis-gates an install. | Open, campaign R2-C |
| R2-6 | `wifi_prov_api` (14 of 36 functions untested) | Wi-Fi credential store/parse | Input parsing plus persistence; overlaps lightly with campaign 10. | Closed (test_wifi_prov.c: add/forget/getters/cache/scan bodies), negtested CAUGHT |
| R2-7 | `dashboard_http` relay POST and `dashboard_http_get_safety_trip` | Manual relay HTTP input and trip report | Heat path via HTTP; campaign 8 covers aux/exec only. | Open, campaign R2-E |
| R2-8 | `cfg_fs_mount` format state machine | Data-loss gate (auto-format vs defer) | Wrong branch erases user data. Partly overlaps campaign 10. | Open, campaign R2-F |
| R2-9 | `thermo_owner` slot accessors, `adaptive_tune_model` commit paths | Sensor ownership, tuning persistence | Sensor slot mix-ups feed the control loop. | Closed (test_thermo_owner.c, 54 checks; test_adaptive_tune_status.c commit_zone failure, persist-failed and writer-race paths), negtested CAUGHT |
| R2-10 | `safety_link_frame` endian helpers, `ui_lcd_lock`, `security_backend_web_auth` | Wire encoding, UI lock, session issue/verify | `security_backend_web_auth.c` exposes only `install`/`start`, so it needs a harness that drives the registered callbacks, not a direct unit test. | Closed (test_safety_link_endian.c 24 checks; test_ui_lcd_lock.c 58 checks; test_security_backend_web_auth.c 117 checks driving the installed vtable and the captured bootstrap route over the real web_auth_store and session table), negtested CAUGHT |

## Campaign plan

Each campaign is one agent, one new test file or extension, negtest required.

- R2-A: extend `test_kiln_io_sx_fake.c` or the dispatch harness so `CMD_SX_RESET` and `CMD_SET_RELAY` run through `owner_task()` with the fake chip; assert relay shadow cleared and off-tracker updated.
- R2-B: link_task command dispatch through the task harness (`th_run_captured_task`) with scripted frames.
- R2-C: identity record round trip, corrupt record, version mismatch.
- R2-D: `wifi_prov_api` do_*() bodies.
- R2-E: `dashboard_http` relay POST parsing and refusal mapping.
- R2-F: `cfg_fs_mount` states with a fake LittleFS.
- R2-G: thermo_owner slots, adaptive_tune_model commit.
- R2-H: frame endian helpers; web-auth callback harness.

## Harness notes for the next agent

- `stubs/freertos/queue.h` has a delivering ring mode (`g_stub_queue_ring_enabled`). Shadow `xQueueReceive` with a macro that `longjmp`s when the ring is empty to run a `for(;;)` task body for exactly the queued commands (see `test_kiln_io_owner.c`).
- SaftyFW tasks run through `test/stubs/task_harness` (`th_set_delay_hook` scripts per-iteration inputs; `th_abort` ends the loop).

## Defects found

None from campaigns R2-D, R2-G and R2-H (all new tests passed against unmodified firmware once their own harness errors were fixed). Note: `web_auth_password_check` rejects any password containing "kiln" as common, which the test discovered while choosing fixtures; this is by design. R2-A..F entries:

None. All three new tests passed against unmodified firmware on the first run; each was negtested (CAUGHT): `ct_installed` default flipped to false, `CMD_SX_SET_PULLUP` relay gate removed, mainFault debounce window shortened by one sample.
