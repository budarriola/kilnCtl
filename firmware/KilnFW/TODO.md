# TODO — Web UI / Kiln Control Front End

Top-level ordering across both processors lives in [`../../ROADMAP.md`](../../ROADMAP.md);
this file owns the main-firmware detail. The cross-processor items — the swapped
safety-UART pins, `CommonFW`, and the safety-liveness gate on heating — are
sequenced there and tracked in [`../SaftyFW/TODO.md`](../SaftyFW/TODO.md).

## Bench sweep 2026-08-27 -- running profiles and autotunes, no heaters

Heaters disconnected, thermocouples connected and reading ambient. A firing
and an autotune run back to back on zone 0 -- the sequence an operator
investigating a bad element would perform. Fixed and committed from this
round: the all-OFF profile that ran silently, the PC-link watchdog opening
relays a firing owned, the guard-trip latch that disabled a zone permanently,
autotune blaming the kiln for its own inaction, and the missing
heat_block_sources / zone_blocked_mask reporting.

What behaved correctly and is now known-good on hardware: the bang-bang
control loop (relay closed at the 2 C hysteresis crossing and held for
minutes); thermal guard 1, which tripped at 296 s of commanded heat with
"heating but rose only -0.0C in 5.0min (need >=25.0C)", dropped the relay and
aborted the run; and autotune's fit, which refused to produce a model from a
flat trace instead of proposing gains from a zero-gain plant.

Still open from this round:

- [x] **`guard_wrong_dir_window_s` does not control the guard an operator
      would think it does.** DONE 2026-08-26: the field now feeds both
      branches, and guard 1 additionally gained its own
      `guard_progress_window_s` (and `guard_progress_duty_min`) on
      /settings/safety, so the heating-failed case is configurable in its own
      right rather than borrowing guard 2's field. Bench-confirmed: a dead
      element tripped at 64 s against a configured 60 s window, where it
      previously took the hardcoded 300 s. The per-zone field is exposed in Settings > Zones
      and reads as "how long heat may be commanded without a response", but it
      only feeds guard 2 (already at/above setpoint and falling). The
      heating-failed case -- the one that matters when an element dies -- uses
      the hardcoded `PROGRESS_WINDOW_S` (300 s) and cannot be configured at
      all. Either wire the field to both or rename it.
- [x] **The frozen-sensor guard cannot fire on real hardware.** DONE
      2026-08-26: `FROZEN_EPS_C` (0.05 C) epsilon band added, and exposed as
      the per-zone `guard_frozen_eps_c` override on /settings/safety. It resets its
      window on any change at all (`in->measurement_c != state->frozen_last_c`),
      and a live MAX31856 dithers by 0.01-0.1 C every read, so the 120 s window
      never accumulates. It can only catch a bit-exact frozen value. Needs an
      epsilon band.
- [x] **Autotune's relay switching is not counted toward contact wear.**
      DONE 2026-08-26; bench-confirmed, the counter moved 9 -> 27 across one
      step test.
      `relay_cycles` is fed only by profile_executor's accounting, so a long
      relay-feedback autotune ages the contacts invisibly.
- [x] **A completed or faulted run never releases its relay ownership**
      DONE 2026-08-26 -- `release_profile_relay_claim()` on every path leaving
      RUNNING; bench-confirmed by a manual relay command succeeding after a
      faulted run.
      (already tracked below) -- confirmed live this round: relay 1 stays
      RELAY_OWNER_PROFILE after the run ends.

---

## Bench sweep 2026-08-27 -- exercising the LCD and web interfaces

Every HTTP endpoint fetched and strict-parsed, every mutating endpoint
exercised against the live board (47 checks, heaters powered down), and the
LCD walked page by page with injected touches. Fixed and committed from this
sweep: `/api/status` emitting bare `nan`, danger mode leaving relays closed
on exit, the tap-target dump captioning containers with a hidden child's
text, and the safety-link config-page hardening. Still open:

- [x] **Page 1 of a two-page safety config is never received.** This note
      predates the actual root cause, found the next day: `3149393` (2026-08-28)
      -- the ESP's `uart_protocol_rx_task` read UART bytes 32 at a time, so a
      ~157-253 byte CONFIG_PAGE took 5-8 scheduler round trips to assemble
      against the reply window, while small STATUS/DIAG/POWER frames always
      finished it in 1-2; page 1 (the bigger page) missed its window almost
      every time and page 0 only "succeeded" via stash-adoption. Fixed by
      reading a whole frame in one go (chunk buffer 32 -> `STUFFED_FRAME_MAX`)
      plus 300 ms of fixed reply margin -- not a bigger multi-page ceiling,
      which is exactly what caused the panic-reboot regression named in
      `safety_cfg_store.c`'s own history (see `SAFETY_CFG_STORE_REFETCH_BUDGET_MS`
      and `safety_cfg_store_refetch_nonblocking()`'s comments). Verified live
      then (`cached_config_crc == live_config_crc == 31328`) and re-confirmed
      2026-09-04: `GET /api/safety/commissioning` reads `config CRC 42736
      (matches live, not stale)` and `safety_get_link_stats` shows
      `config_page=0` in the cmd histogram -- i.e. no refetch is even running
      right now because the cache is already converged, which is the designed
      steady state (`safety_cfg_store_maybe_refetch()`: "no UART traffic at
      all, by design"). No analyser capture was ever needed; the Pico was
      innocent, as suspected.
- [x] **The PC-link watchdog drops all relays every 5 s of host silence** --
      confirmed 2026-09-07 that this does NOT abort a running firing: the
      2026-08-27 owner-qualification fix (uart_bridge.c's link_watchdog_task)
      already excludes any relay claimed PROFILE/RULE/AUTOTUNE via
      relay_authority_manual_blocked_by_owner() -- an idle host only expires
      relays it (or a manual UI command) left NONE/MANUAL-owned. Pulled the
      decision logic out into `bridge/link_watchdog_decide.{c,h}` (mirrors
      `profile_executor_wd_decide()`'s split) so it is host-tested against the
      REAL relay_authority.c: `test_link_watchdog.c` proves a PROFILE- or
      AUTOTUNE-owned relay stays out of the watchdog's mask, a MANUAL/NONE-
      owned one stays in it, danger mode zeroes the mask, and traffic resets
      the liveness timer. No behavior change; this closes the "check what
      this does to a running firing" ask with an automated proof instead of a
      one-off bench observation. (`dac91b32`)
- [x] **`max_temp_c == 0` and `max_ramp_c_per_hr == 0` mean opposite things**
      on the same uncommissioned zone -- reconciled 2026-09-06 (see
      `docs/PROFILES.md` "Zero-commissioning semantics"). Rule: both mean
      "not commissioned, refuse to start a firing/autotune" on any zone that
      can heat (`profile_executor_run()`'s existing guard-5/ramp-ceiling
      refusals, `autotune_engine.c`'s prestart checks); everywhere else that
      reads them mid-run treats 0 as "no extra limit", since the start-time
      refusal already made that state unreachable in a real firing. The one
      consumer out of step with this rule, `GET /api/readiness`'s "Guard
      limits (max_temp_c)" item, reported a heating zone's `max_temp_c == 0`
      as ok/deliberately_off instead of not_done -- fixed in
      `readiness_http.h`'s new `readiness_guard_max_temp_status()`, tested in
      `test_readiness_commissioning.c`. `max_simultaneous_relays == 0` is a
      different field (board-wide load-staggering cap, not a per-zone
      commissioning gate) and correctly keeps its "0 = unlimited" meaning
      unchanged -- no inconsistency there. (`1fc9b1dd`)
- [x] **The LCD numbered relays from 1 and zones from 0 on the same page** --
      fixed 2026-08-30 (`UI_PLAN.md` 5.2 audit): `ui_page_temperature.c`'s
      relay tiles, relay error toasts and zone rows are now all 0-based,
      matching the web side's R0-R3.
- [x] **The Safety Processor LCD page never shows the current state.** FIXED
      2026-09-07 -- Diagnostics' Safety & Board Health page (`ui_page_diagnostics.c`)
      now shows live link UP/DOWN (`ds.safety_ready`, the staleness-gated bit,
      not the sticky `link_version_known`), the live ARMED/TRIPPED word (with
      trip reason inline when tripped, already present), and a Commissioned
      Yes/No/unknown row (`ds.safety_build_known` + `ds.safety_config_crc`).
      No new link frames or protocol fields; the Enclosure temp row was
      dropped to hold the page at 5 text rows + 3 cold-junction rows, same as
      before, so the existing no-scroll budget proof still holds.
- [x] **Unknown `/api/*` paths serve the 90 kB dashboard HTML with 200** rather
      than 404 (the catch-all handler). A client gets a page where it expects
      JSON and has to guess. DONE -- `captive_portal_404_handler()`
      (`wifi_provision_http.c:696-713`) exempts the `/api/` prefix from the
      captive-portal redirect and answers it with a real 404 + JSON error
      body instead; only non-`/api/` paths still get the 302-to-dashboard
      redirect.
- [x] **`Accept-Encoding: identity` gets a 406 for every page.** DONE
      2026-09-06 -- documented rather than reworked (storing an uncompressed
      fallback was rejected on the flash budget): `docs/ARCHITECTURE_DECISIONS.md`
      "Web server / transport" now names the behavior and the fix for a
      plain client (`urllib`, some `curl` invocations) -- send
      `Accept-Encoding: gzip` explicitly, or `curl --compressed`.
- [x] **Internal-DRAM regression: superseded.** Re-baselined (`af17e3d`,
      `f426ec6`); `dram_margin.h`'s known-good figures are now largest=18432,
      free=46083 at `app_main_done`, with no regression logged on boot.

---

## 2026-08-28 -- the bench changed, and a batch of owner requests

**THE HEATING ELEMENTS ARE NOW PHYSICALLY CONNECTED.** Every note in this file
that reasons from "relays may be activated but nothing will get warm" is now
false. Read anything about relay behaviour with that in mind.

- The test fixture is capped at **80 C**, enforced today by `max_temp_c = 80`
  on all three zones, set live over `/api/zones`. **This is a fixture
  threshold, not a kiln limit** -- it must come out before a real kiln, and it
  is a concrete instance of `SaftyFW/TODO.md` phase 9's "confirm no test
  threshold was left in place".
- **CORRECTED 2026-09-04, verified read-only against the live board
  (`GET /api/safety/commissioning`):** the safety processor's `abs_max_temp_c`
  no longer reads 0. It is now `{"set":true,"value":80}`, `commissioned:true`,
  config CRC not stale. Something (this pass did not trace exactly what --
  most likely the commissioning-gate interlock, `GUARD_TEST_MATRIX.md` §8)
  has since commissioned it. S1 is therefore now ARMED: `safety_guards.c`
  (`SAFETY_TRIP_OVERTEMP`, gated on `cfg->abs_max_temp_c > 0.0f`) will trip
  and de-energize K4 independently of the ESP on the 3rd consecutive reading
  above `abs_max_temp_c` (the firing-time `min(abs_max_temp_c, firing_max_c +
  firing_margin_c)` tightening was retired 2026-09-24). That ceiling
  (80C) currently equals, not undercuts, the ESP-side zones' own
  `max_temp_c` (also 80) -- real independent backstop, but not a tighter
  second line of defense. See `docs/SAFETY_CASE.md` §2 (H1 row) and §3.1 for
  the full verification and an OWNER-DECISION recommendation to commission a
  tighter value (~70C) given this rig has never fired above 60C --
  superseded by the 2026-09-24 owner decision that the Pico's limits must
  equal the ESP's, never be tighter. The
  original note below is preserved for history; it no longer describes the
  live board.
  <details><summary>Original 2026-08-28 note (superseded)</summary>
  The safety processor was NOT enforcing it. Its `abs_max_temp_c` read
  `set: true, value: 0`, and 0 there means never trip -- so the only ceiling in
  force ran on the same processor that commands the heat. `/api/readiness`
  reporting "all 58 safety parameters have values" is what hid this: it counted
  a zero as a value, while `commissioned` right beside it said `false`.
  </details>
- Relay 4 is currently held closed by rule `R0 (TEMP zone0 >= 25)`, a leftover
  bench rule whose condition is satisfied at ambient. Deleting the rules engine
  (below) will open it.

### Owner requests, 2026-08-28

Done: manual relay page removed (and `POST /api/relay` with it -- the page was
its only caller); board health and thermocouple faults folded into
`/diagnostics` and their routes deleted; safety pages became an expanding nav
group; firing profiles moved to the top of the menu; shared **safety timing
profiles** landed (`ZONES_CFG_VERSION` 8->9, lossless, de-duplicating).

Open:

- [x] **Relay/IO segments in firing profiles**, blocking or non-blocking, each
      segment choosing whether its relay is left in its last state at run end
      (default off). `PROFILE_VERSION` 2->3. DONE -- `PROFILE_SEG_KIND_RELAY_IO`
      (`persist/profiles_types.h`: `io_blocking`, `io_leave_on_at_end`) already
      implements this verbatim; on/off zones (docs/ON_OFF_ZONE_PLAN.md) are a
      distinct, later feature and explicitly say so in that plan's own intro.
- [x] **Delete the rules engine** afterwards, never before -- otherwise there
      is a window with no way to drive a non-zone relay. `rules_task`'s own
      watchdog force-releases and force-offs its relays on a stale tick; that
      protection must be replaced, not merely deleted. DONE -- `rules_task.c`
      no longer exists in the tree.
- [x] **Names for relays not assigned to a zone.** DONE -- `relay_names_cfg_t`
      (`zones_config_accessors.c`/`.h`, `zones_config_get_relay_name`/
      `zones_config_set_relay_name`) already persists a name per relay
      regardless of zone assignment, in its own NVS-backed blob keyed by
      relay number, not zone; exposed dense over `relay_names`/`relay_types`
      in the zones GET/POST HTTP API (`zones_http_get.c`/`zones_http_post.c`),
      editable on `zones_page.html`'s per-relay name/device-type inputs for
      any relay no zone currently claims (`renderRelayNames()`), and shown on
      the LCD temperature page (`ui_page_temperature.c`) in place of the
      zone-owned label. Covered by `test_relay_names_cfg_fs.c`.
- [x] **LCD**: no manual toggling of zone-assigned relays on the temperature
      page (visible, not hidden); safety-processor / board-health /
      thermocouple-fault pages folded into LCD diagnostics and removed; kiln
      setup, thermocouple types and kiln config pages removed; profiles to the
      top-left of the main menu, whole menu on one page. DONE -- verified
      2026-09-20 against the tree: `ui_page_temperature.c` already lists every
      physical relay once, disables (`LV_STATE_DISABLED`, re-checked again in
      `relay_toggle_cb()`) whichever ones `relay_is_zone_owned()` claims for a
      zone while leaving their live state visible, and drives every write
      through `dashboard_set_relay()` (same manual-block/safety-fault gating
      as the web dashboard's own manual relay control) -- see that file's
      header comment. `ui_page_board_health.c`, `ui_page_safety.c`,
      `ui_page_thermo_faults.c`, `ui_page_tc_types.c`, `ui_page_kiln_setup.c`,
      `ui_page_kiln_cfg_setup.c` and `ui_page_zones.c` no longer exist in the
      tree and are not registered in `kiln_ui.c`. `ui_page_config.c`'s nav
      grid builds "Profiles" as its first (top-left) cell, one page, no
      scrolling.

### Two constraints that now bind everything here

**Bytes.** `zones_cfg_t` is 500 of a hard 512
(`ZONES_CONFIG_BLOB_MAX_SIZE`, which also sizes `kiln_cfg_store.c`'s buffer).
The timing-profile work spent the slack getting there -- it reordered
`zone_cfg_t` to kill alignment padding and cut the profile name to 7
characters. Anything new that wants to persist per-zone or per-relay state must
find bytes or take its own NVS key.

**Arrays by value.** `zones_cfg_t.zones[]` and `profile_t.segments[]` are
embedded arrays, so one new field in an element displaces every element after
the first. Every migration needs a FROZEN snapshot struct of the old layout and
a field-by-field walk. `profiles_http.c` already shipped the version that
returns `sizeof` the *current* struct for the *old* version: it rejected every
profile on the owner's board and marked them unused, and only a hardware flash
caught it.

---

## Audit 2026-08-27 -- open items

A six-agent read-only audit of both firmwares. Already fixed and committed: a
stack overflow in `safety_parse_fw_version()`, the unknown-peer-version
fail-open, the trip-event dedup desync, the factory-reset interlock, the
gitignored OTA-rollback setting, and the zones-page error conflation.
Everything below is **confirmed by code reading and still open**, ordered by
severity.

- [x] **A v3/v4 zones blob is adopted as garbage and marked valid.** DONE
      2026-08-26 (commit 03a3424), exactly as prescribed below. All six
      historical layouts were recovered from this file's own git history
      rather than guessed, so every version 1-6 got a real typed converter.
      The mid-struct insertion was v5's `tc_type`, ahead of `model_k_dc` and
      `thermo_mask`. `nvs_load_from()` and `zones_config_import_blob()` now
      share one decoder -- they had each carried a copy of the same bug.
      Bench-verified: the live blob upconverted with every field identical.
      **The `profiles_http.c` sibling immediately below is still open.**
      `zones_http.c`'s older-version NVS load path does **no length check** and
      never calls `validate_zones_cfg()`, and the migration assumes the struct
      only ever grew at the tail -- false, since three of four bumps added
      fields to `zone_cfg_t`, which is an *array element*, so `zones[1]` and
      `zones[2]` are displaced. `tc_type` was also inserted mid-struct.
      Result: `relay_mask`, `max_temp_c` and all eight guard thresholds on
      zones 1-2 become reinterpreted float garbage with the config flagged
      trustworthy. **This is the owner's "did a partial config from an
      incompatible firmware load?" question, and the answer is yes.** Fix: a
      per-version expected-length table checked before anything is touched;
      typed per-version structs with explicit field-by-field upconversion
      instead of memcpy-and-patch; run `validate_zones_cfg()` on this path too
      and clear the valid flag rather than partially defaulting; add a CRC.
      Note the snapshot-restore path has the same memcpy defect but *does*
      validate afterwards, which is why it usually rejects -- same bug, one has
      a net.
- [x] **`profiles_http.c` accepts `version == 0`** DONE 2026-08-27
      (`ee38e39`), built to zones_http.c's design and closed BEFORE
      PROFILE_VERSION reached 2, as this item asked. Note the near-miss: the
      first version of the fix returned `sizeof(the current struct)` for
      version 1, which rejected every profile the bench board already had and
      marked them unused -- it reproduced the data loss it was written to
      prevent, and only a hardware flash caught it. profiles_http.c now has
      its own host-test executable (the seventh) so that case is a permanent
      regression test rather than a scratch harness. at any length >= 1 and
      installs it as a used slot. Same structural sibling waiting to happen:
      `profile_t` embeds a `segments[]` array, so the first field ever added to
      `profile_segment_t` reproduces the zones bug exactly. Fix before
      `PROFILE_VERSION` reaches 2.
- [x] **`POST /api/factory_reset` is still unauthenticated.** DONE
      2026-08-27 (`f58e040`), as prescribed: `ota_http_authenticate_request()`
      exported from `ota_http.c` (the private-header-parsing gap this item
      named), a new `OTA_HTTP_CONTEXT_FACTORY_RESET` with its own lockout
      budget, and auth ahead of the interlock so a refusal reason naming a
      live zone temperature cannot leak to an unauthenticated caller.
      Bench-verified: no header -> 400, bogus MAC -> 403, config intact. The interlock
      landed; auth did not. It is strictly more destructive than
      `POST /api/ota/esp/rollback`, which *is* challenge-response
      authenticated. Needs a new `ota_http_context_t` plus an exported
      request-verify helper (the header parsing is currently private to
      `ota_http.c`).
- [x] **An open AP (empty AP password) reduces OTA auth to nothing.** DONE
      2026-08-27 (`f58e040`) -- refused after the BOOT-button check and
      before any HMAC math, and surfaced as `ota_auth_disabled` on
      `/api/status` next to `boot_button_bypass_active`. The physical
      BOOT-button recovery window is untouched, so an open-AP board is not
      left unflashable.
      `ota_http.c` HMACs with a zero-length key and does not reject it, so
      anyone who can reach the board can compute a valid MAC from public
      information and flash both processors. Refuse when `pw_len == 0`, and
      surface the condition on `/api/status` next to
      `boot_button_bypass_active` -- the codebase already accepts that "this
      board has no OTA auth right now" must be permanently visible.
- [x] **`POST /api/zones` and `POST /api/rules` are not gated on a running
      firing** DONE 2026-08-27 -- both now run the same
      `ota_http_check_interlocks()` gate as `kiln_cfg_http`'s apply and
      `backup_http`'s restore. Proven both ways on the board: 409 "a profile
      is running" during a firing, 200 for the identical request when idle., unlike `kiln_cfg_http`'s apply and `backup_http`'s restore,
      which write the same `zones_cfg_t`. Changing `relay_mask` mid-run moves
      the firing onto a different physical relay and leaves the old one
      wherever it was last commanded, with nobody driving it off.
- [x] **`/api/status` has ~46 bytes of margin in a 2500-byte buffer, and goes
      ~120 bytes negative if the build strings need escaping.** DONE
      2026-08-27: overflow is now a 500 with a valid body naming the cause,
      matching `zones_http.c`, so the endpoint can no longer emit a truncated
      document however the buffer's margin drifts as fields are added. The
      hand-sizing itself is no longer load-bearing, which was the point.
      `safety_build_commit`/`datetime` arrive over the link from the RP2040, so
      a corrupt or hostile FW_VERSION frame overflows it deterministically ->
      truncated JSON -> the dashboard's "Loading..." hang. Stop hand-sizing
      this one: chunk it (as `profiles_http.c` already does) or return 500 on
      overflow instead of a half-object.
- [x] **`GET /api/profiles` overflows at 8 slots with escape-heavy names**
      DONE 2026-08-27 (`ee38e39`): the per-entry budget is sized against a
      fully-escaped name, a reserve the per-entry writes may not touch keeps
      the document closable, and a dropped entry is reported as an entry.
      (800 bytes needed vs 784 available) producing syntactically invalid JSON.
      Per-entry budget of 96 ignores that a 15-char name escapes to 30.
- [x] **`/api/readiness` drops items silently and can emit invalid JSON.**
      DONE 2026-08-26 (commit 4e8e1f1). A reserve is held back so the closing
      `]}` can never be the thing that overflows; `first` is only cleared when
      the offset actually moved, so a dropped first item can no longer open
      the array `[,`; and any drop now appears AS an item
      (`checklist_truncated`, READY_CANNOT_YET) rather than shortening a list
      an operator reads as a go/no-go. Negative-tested on the board with the
      buffer cut to 900 bytes.
      `append_item` returns the offset unchanged on overflow while `first` is
      set unconditionally, so a failed first item yields `[,{...}`; the closing
      `]}` is skipped on overflow too. ~156 bytes of margin at 12 items.
- [x] **`ota_http.c` and `factory_reset.c` have no host-test coverage at
      all.** DONE 2026-08-27 (`da4918c`) by the first of the two routes this
      item proposed: `ota_http.c` and `factory_reset.c` now compile on the
      host and are `#include`d by an 8th executable, `test_ota_http.c` -- so
      the real decision code is under test, not a restatement of it. The
      blocking surface (`psa/crypto.h`, `esp_ota_ops.h`, `esp_partition.h`,
      `esp_app_desc.h` and friends) is stubbed. The HMAC stub is a documented
      FAKE: it establishes that identical inputs give identical output and
      different inputs do not, which is all the three decisions under test
      depend on -- it is NOT evidence that the real crypto is correct, and its
      header says so. Negative-tested: removing the `pw_len == 0` refusal, and
      making factory-reset reuse `"esp"`'s context, both go red.
      **Still uncovered:** the streaming transfer handlers
      (`ota_esp_do_transfer`, `ota_pico_do_stage`) and the route wrappers
      around them remain hardware-verified only.

- [x] **`httpd_resp_send(req, json, n)` sites use `snprintf`'s return
      unclamped at the high end** -- a stack over-read that sends adjacent
      stack memory to the client if the format ever exceeds the buffer. DONE
      2026-08-27 (`8782c0a`). The count in this item was wrong: a sweep of
      every `httpd_resp_send(req, buf, count)` in `App/drivers/` found
      **seven** genuinely unclamped sites, all of them in `ota_http.c`. The
      rest either accumulate through an `APPEND` macro that already stops on
      overflow, clamp explicitly (`wifi_provision_http.c`), or pass
      `HTTPD_RESP_USE_STRLEN`. All seven now go through
      `send_json_clamped()`. Negative-tested on the board: with
      `/api/ota/challenge`'s buffer cut to 16 bytes the reply is 15 bytes and
      the log names the undersized buffer; unclamped it would have sent 76.
- [x] **PID zones can command heat with no valid reading.** DONE
      2026-08-26 -- the deferred-on-time payback block moved inside
      `if (sensor_ok[zi])`, matching bang-bang's refusal. The
      deferred-on-time payback block in `profile_executor.c` runs
      unconditionally, outside the `if (sensor_ok[zi])` that zeroes `duty`, so
      a zone that accrued credit under the load cap and then loses its
      thermocouple gets `boosted_duty` up to 1.0. Bang-bang mode explicitly
      refuses this case; PID has no equivalent. >= 3 s of full duty before
      guard 6 debounces.
- [x] **Danger mode leaves relays energized when its window closes.** Both the
      timeout and the operator stop release heat-enable and restore the gate,
      but neither commands the four ESP relays off -- and restoring the gate
      only blocks *new* ON commands. A relay closed during danger mode stays
      closed indefinitely under a gate that would now refuse to close it.
      DONE -- both `danger_mode_stop()` (`danger_mode.c:255-282`) and the
      timeout path in `danger_mode_task()` (`danger_mode.c:290-318`) now call
      `kiln_io_owner_command_all_relays_off()` before restoring the gate.
- [x] **Fault sources asserted by `autotune_engine` and by guard 9 are never
      deasserted.** Neither has the `global_fault_source` bookkeeping
      `profile_executor` uses, and nothing else clears an arbitrary source, so
      one autotune trip or one 10-second executor stall blocks all heat
      board-wide until reboot -- and masks any later, different fault. DONE
      -- `autotune_engine.c:1009-1035` (this was around line 3193 before
      later edits shifted the file) clears a stale zone/global fault
      source on the next autotune start, and
      `profile_executor_relay_io.c:555-568`'s `clear_this_runs_faults()`
      deasserts `s_exec.global_fault_source` on every exit from a run.
- [x] **Guard 5's absolute ceiling is off by default.** `max_temp_c == 0` means
      "no ceiling", and a zone never saved through `/settings/zones` fires with
      none -- while `autotune_engine` and `SAFETY_MODEL.md` both already assume
      the guard is armed. Either substitute a hard ceiling or refuse to start a
      firing on such a zone. DONE -- `profile_executor_start.c`'s
      `profile_zones_have_ceiling()` now refuses to start a firing on any
      zone that can command heat and has `max_temp_c == 0`, matching
      `autotune_engine_run_relay()`'s existing guard-5 refusal for the same
      reason (host-tested in `test_profile_executor_prestart.c`).
- [x] **A completed run never releases its relay ownership** (only
      `profile_executor_halt()` does), so after a normal finish every manual
      relay command is refused as "owned by a profile" when none is running.
      DONE -- duplicate of the DONE item above ("A completed or faulted run
      never releases its relay ownership", 2026-08-26): `release_profile_
      relay_claim()` is called on every exit from RUNNING
      (`profile_executor.c:501,620,1253`).
- [x] **`rules_task` applies zone calibration with a channel index** where the
      executor correctly combines raw then applies once with the zone index --
      so a rule threshold means a different temperature than the control loop
      on any multi-thermocouple zone. MOOT -- `rules_task.c` no longer exists.
- [x] **`IO_CMD_SX_LED_DRIVER` can PWM a relay coil with no safety gate, no
      ownership gate and no remap**, and never updates `relay_shadow` -- so the
      dashboard reports the coil off while it is energized. Its two immediate
      neighbours in the same switch both check `kiln_io_relay_pin_mask()`.
      `IO_CMD_SX_RESET` and the pullup/opendrain/int-mask setters have related
      gaps. DONE -- `IO_CMD_SX_LED_DRIVER` is now unconditionally refused on
      any relay pin (`kiln_io_owner.c:253` `sx_led_driver_touches_relay()`/
      `:422`, `uart_bridge_io.c:497-521`), matching its neighbours rather than
      trying to retrofit a duty-cycle onto a mechanical coil.
- [x] **`board_temps.c` labels cold-junction readings by array position**, but
      `MAX31856_read_all()` compacts over failed channels -- so with channel 0
      dead, channel 1's reading is displayed as "ch 0" and the dead channel is
      not the one shown absent. Index by `.channel`, as
      `ui_page_thermo_faults.c` already does. DONE -- `board_temps.c:90-129`
      now indexes `thermo_cj_valid[]`/`thermo_cj_c[]` by `r->channel`, never
      by loop position.
- [x] **A CJRANGE fault NaNs only the cold junction**, but the hot-junction
      value is cold-junction-compensated in hardware -- so an out-of-range cold
      junction yields a wrong hot-junction temperature reported as a plausible
      number. DONE -- `max31856_fault_invalidates_tc()` (`max31856_codec.h`)
      now also NaNs `tc_temperature_c` on a CJRANGE fault; see
      `MAX31856.c`'s read path (~line 1121) and its 2026-08-27 doc comment.
- [x] **`dashboard_get_status()` calls `kiln_io_read()` from the LVGL task**,
      bypassing `kiln_io_owner` and consuming the SX1509 interrupt latch that
      another reader may be waiting for. DONE (not one of the original 12 in
      this pass's assignment, but fixed as part of it) -- routed through
      `kiln_io_owner_command_read()` (`dashboard_http.c:146`). Checked for the
      flash-worker-style self-deadlock first: this handler only ever runs on
      the httpd worker task or the LVGL task, never on `kiln_io_owner`'s own
      `owner_task`, and `post_and_wait()` fails closed on a bounded timeout
      (`KILN_IO_OWNER_WAIT_MS`) rather than blocking forever -- no reentrancy
      risk found.
- [x] **`spi_owner_transfer()` waits `portMAX_DELAY`**, defeating every
      caller-side timeout above it if the owner task wedges. DONE
      (`DISPLAY_ST7796_PLAN.md` Phase 1): bounded to
      `SPI_OWNER_TRANSFER_TIMEOUT_MS` (1000 ms) backed by a heap slot pool
      (`owner_slot_pool.h`), with a `wedged` latch surfaced on `/api/status`
      as `thermo_spi_wedged` and a banner in `main_page.html`.
- [x] **`gpio_probe`'s deny-list omits the three `~FAULT` pins**, so a probe
      session can drive them high permanently and forge "no fault" at the pin
      level. DONE (not one of the original 12 in this pass's assignment, but
      fixed as part of it) -- `THERMO_FAULT0_IO`/`THERMO_FAULT1_IO`/
      `THERMO_FAULT2_IO` added to the deny-list (`gpio_probe_denylist.h`,
      called from `gpio_probe.c`), negative-tested in `test_gpio_probe.c`
      (removing the three entries makes all three checks fail, confirmed and
      reverted).
- [x] **Docs contradicting code: fixed.** `KilnFW/docs/HARDWARE.md` now
      records both the thermocouple channel rotation and the relay 2<->4
      swap, in the SX1509/relay section and the thermocouple daughterboard
      section respectively.

---

This is a planning doc: it tracks work not yet done, plus enough context on
finished, adjacent work to act on what's left. A finished section is
collapsed to a short status line pointing at the doc that owns its detail —
`docs/PROJECT_STATUS.md` (dated engineering log), `docs/ARCHITECTURE.md` /
`docs/ARCHITECTURE_DECISIONS.md` (settled design), `docs/BRINGUP_HAZARDS.md`
(recurring bugs and their root causes), `docs/PID_CONTROL.md` +
`docs/GUARD_TEST_MATRIX.md` (control loop and thermal-safety detail), and the
per-feature docs (`WEB_UI.md`, `WIFI_PROVISIONING.md`, `PROFILES.md`,
`SAFETY_MODEL.md`, `UI_PLAN.md`, `UI_THEME.md`). **Most of the "still open"
items below are open specifically because no ESP32-S3/RP2040/thermocouple/
relay hardware combination exists in this environment to verify them
against** — treat "logic-verified"/"build-clean" language literally, not as
a synonym for done. Cross-references `docs/SAFETY_MODEL.md` throughout,
since the profile-execution engine here is a **new actor that commands
relays** and must go through the same safety-wins gate as the existing
PC/MCP link — not a parallel path that bypasses it.

## 0. Architecture decisions

**All settled and built.** Full writeup moved to
[`docs/ARCHITECTURE_DECISIONS.md`](docs/ARCHITECTURE_DECISIONS.md): web
server on-device, relay ownership tags, the rule-engine composition rules,
the history ring buffer design, and storage (NVS, no LittleFS).

**Rule engine — BUILT 2026-08-22.** `rules_eval.{c,h}` (pure decision logic)
plus `rules_task.{c,h}` (1 Hz FreeRTOS task) close the "evaluator has no
caller" gap this line used to record. Precedence is
PROFILE/AUTOTUNE > RULE > MANUAL through the existing `relay_authority`
tags; a safety fault, a down safety link, or an OTA in progress forces every
rule-driven relay off, and turning off is never gated. A stale-tick watchdog
forces the same off-state if the evaluator stops ticking for 5 s.

Scope limit set by the owner the same day, and enforced in three places
(`rules_eval_decide`'s `is_heater_relay` gate, `rules_task`'s per-tick
recomputation of the zone-relay union, and the POST handler's refusal):
**rules may never command a relay assigned to a zone.** Those are PID/
thermocouple-controlled heaters. Rules exist for the non-PID hardware —
reduction flame, vents, blowers — and may still READ heater relays as rule
conditions.

## 0.5 Page organization

Grouping: Dashboard (section 2, home/live status), Profiles (section 5),
Settings split into Thermocouples & Zones (section 3, incl. PID tuning),
Relays & Rules, Network (section 4). Safety-relevant controls stay reachable
in the fewest taps (phone-screen constraint). Built as described.

**Four more pages added to plan 2026-08-18** (web + LCD both):

- ~~**Safety / Alarm page.**~~ **DONE 2026-10-07** (`ui_page_safety.c`, hub cell
  "Safety", tap on the home trip strip): live state, last trip (reason, age,
  cause, remedy) and a Clear Trip button. Viewing is dashboard-level (no PIN);
  Clear needs an LCD admin login (`ui_lcd_lock_run_gated`) and sends the same
  `dashboard_safety_clear_trip()` the web route uses (same refusals). The alarm
  is derived from live DIAG state every refresh (`ui_safety_view_derive`, shared
  with the home strip, host-tested), never a page-local latch, so a clear from
  web/MCP/UART removes it. History is only the last Frame D event (no multi-trip
  ring on the link); the diagnostics Trip Detail page still shows it in full.
  The config hub became 3 columns to fit the extra cell.
- **Diagnostics / System info page — DONE for the ESP-only half; SAFETY-LINK
  STATS HALF STILL BLOCKED ON M5.** `ui_page_diagnostics.c`/`.h` (fw version,
  uptime, heap/PSRAM, die temp) built 2026-08-20. A separate
  `ui_page_thermo_faults.c`/`.h` page (per-channel MAX31856 fault bits,
  visibility only, no clear action) was added the same day per explicit
  request to split diagnostics into multiple pages.
- ~~**Manual zone control page.**~~ **Declined by explicit user request
  (2026-08-20)** — no manual setpoint override bypassing a running profile.
  `ui_page_temperature.c` (per-zone reading + manual relay toggles) stays.
- Backup/restore, LCD tc-type page (`docs/ARCHITECTURE_DECISIONS.md`):
  - [x] **Stale as of 2026-09-22 — closed docs-only, no code gap found.**
        Restore already gates through `ota_http_check_interlocks()`
        (`backup_import.c:2671`, same wrapper `ota_http.c`'s OTA routes use).
        Every `zones_http.h`-exposed field (via `zones_config_accessors.h`)
        has a validating setter and round-trips through
        `backup_import_apply()` — see
        `test_backup_import.c`'s `test_export_round_trips_through_import_to_identical_config`
        (line ~3145), which checks PID gains, tc_type, name, masks,
        cal_offset_c, sanity_rate_c_per_min, control_mode, max/min_temp_c,
        heater_window_ms, fuzzy_strength_pct, coupling_coeff[], the
        coupling_diag_k_dc addition, settings_source and failsafe_state
        individually. `test_wifi_prov.c` and `test_backup_import.c` both
        exist and link `backup_http.c`'s split files
        (`backup_json.c`/`backup_export.c`/`backup_import.c`) and
        `wifi_prov.c` into host tests via the same `#include`-the-.c
        convention used elsewhere in this suite. Matches
        `project_backup_round_trip_coverage.md`'s "only the Wi-Fi password
        is irreducible" note.

**Web page structure rework — DONE (2026-08-20/21).** `settings_page.html`
(now trimmed to just the danger zone), `manual_page.html`, `safety_page.html`,
`diagnostics_page.html`, `thermo_faults_page.html`, a scrollable/LCD-like
dashboard reorder, and the top drop-down nav/Home button chrome rework are
all built — see `docs/ARCHITECTURE_DECISIONS.md` ("Page organization").
Still open, unauthorized-to-build: the session-token auth layer on writes
and TLS for this UI and OTA (section 9.3) — see `docs/UI_PLAN.md`.

## 1. Wi-Fi provisioning and resilience — DONE, hardware-verified

Implemented in `App/drivers/wifi_prov.{c,h}` + `App/drivers/wifi_provision_http.{c,h}`
+ `wifi_provision_page.html`. AP fallback, single home/AP mode toggle (redesigned
2026-08-11 from the original three-flag model), editable AP identity, saved-network
list with forget (section 8.4), DHCP/static IP toggle (web only, 2026-08-20), and the
"losing Wi-Fi never stops the control loop" hard requirement are all built and
hardware-verified — AP-join, station-join, captive portal (real associated client,
DNS hijack + 302 to `/`), and a 35-minute soak with zero failures. Full detail and
every dated fix: `docs/WIFI_PROVISIONING.md` and `docs/PROJECT_STATUS.md`. Several
real bugs were found and fixed during hardware testing this pass (Wi-Fi
mode-switch ordering, `has_creds` disagreement, internal-SRAM task-registration
race, coredump partition sizing, UART log congestion) — condensed writeups in
`docs/BRINGUP_HAZARDS.md` rather than kept here.

Open items, not yet fixed:

- [ ] **AP-fallback fix unverified end to end** (`ARCHITECTURE_DECISIONS.md`,
      "Static-IP AP-fallback fix") — needs a router with both correct and
      deliberately-wrong static config, plus a second device.
- [ ] **Not exercised against a real router**: the DHCP/static toggle is
      build-verified and flashed but has no live network in this environment to
      confirm actual join behavior against.
- [x] **Wi-Fi driver log lines arrive with an empty body** (`W (...) wifi:` with
      nothing after the colon) roughly every 30s. RESOLVED 2026-09-22, not a
      pipeline defect: `uart_log_bridge.c`'s parse/format path
      (`uart_log_parse_level()`, `uart_log_vprintf()`) passes tag/message bytes
      through untouched and reports truncation and drops explicitly, neither of
      which matches this symptom -- it is ESP-IDF's own
      `ESP_LOGW("wifi", "")` inside the closed-source esp_wifi blobs, upstream
      and by design. No content is lost. Optional, not recommended: silence it
      with `esp_log_level_set("wifi", ESP_LOG_ERROR)`.

## 2. Web UI — Main / Dashboard page

DONE and hardware-verified — `App/drivers/dashboard_http.{c,h}`, `main_page.html`.
Live per-channel thermocouple/relay status, the desired-vs-actual graph
(`GET /api/history.csv`, section 6A.9), Start/Stop/Pause, and a saved-profile
picker are all built; all five pages (dashboard + 4 sub-pages) verified live to
be browsable with every card marked hardware-absent when the daughterboard/
expander are missing, rather than crashing or lying. `docs/WEB_UI.md` has the
full page/API inventory.

Idle-state chart behavior: `docs/ARCHITECTURE_DECISIONS.md`, "Idle chart / pinned dots".

- [ ] **Still polled (2s), not pushed.** `CONFIG_HTTPD_WS_SUPPORT` is off; not
      yet justified. Revisit (WebSocket or SSE) if 2s polling proves too coarse
      once real hardware is attached and watched during a firing.
      Re-evaluated 2026-09-25 from source: still not worth it. `.dram0.bss` has
      under 1.5 KB of headroom, a held WS/SSE connection permanently takes one of
      the 13 httpd sockets (the pool has already saturated once from two tabs
      polling), no async-handler pattern exists yet to build it LRU-safe on, and
      there is no path for a session expiring mid-stream. If 2s ever proves
      inadequate, do the cheap wins first: ETag/304 on `/api/status` and a
      slower idle interval, as `pollHistory`/`pollAutotune` already use.
      Also covers the 6A.9 polling note (merged 2026-10-07).
      **Cheap wins landed 2026-10-07:** `/api/status` sends an FNV-1a ETag of
      the rendered body and answers a matching `If-None-Match` with 304
      (`dashboard_etag_*` in `dashboard_json.c`; saves bandwidth/JS parse, not
      render cost), and `main_page.html`'s `poll()` backs off 2 s -> 5 s while
      no firing or autotune is active (`pollDelayMs()`).
- [ ] **Pixel-level appearance of the idle dots was not visually confirmed**
      — no framebuffer readback and the browser was not driven during
      verification; only the API/serving behavior was checked.

## 3. Web UI — Settings page

DONE — `/settings/zones` (`App/drivers/zones_http.{c,h}` + `zones_page.html`) and
`/settings/relays` (`App/drivers/rules_http.{c,h}` + `rules_page.html`, both
removed 2026-08-27 along with the rule engine — see `docs/PROFILES.md`).
Thermocouple calibration (offset, applied in firmware to every consumer including
UART/MCP as of 2026-08-13), thermo/relay count config, named zones, PID tuning
per zone (same page, per explicit request), and persistence are all built and
hardware-verified as reachable/functional with no thermocouple/relay hardware
attached. The relay alternate-function rule *editor* (DSL, `RELAY n DRIVEN`/`R
<rule> TEMP|TIME|RELAY`) is built and persists; the rule *evaluator* that would
make saved rules actually drive relays does not exist — `rules_http.h`'s own
comment and the page both say saved rules are inert (see section 0's rule-engine
design in `docs/ARCHITECTURE_DECISIONS.md`).

Max-ramp-rate ceiling is a **user-entered** value (`max_ramp_c_per_hr`), not
estimated from PID tuning/observed performance — the estimated alternative was
deliberately left undesigned as "more work, needs a design of its own."
(Profile-page °C/°F display and its builder-editable-field seam:
`docs/ARCHITECTURE_DECISIONS.md`, "Profile builder: Celsius-only editable
fields".)

## 4. Web UI — Network settings page — DONE, folded into section 1

Same page as section 1's provisioning page (`wifi_provision_page.html`) rather
than a separate route — scan/select SSID, mode toggle (superseded the original
local-only checkbox 2026-08-11), and live connection-state display are all built.
See section 1 and `docs/WIFI_PROVISIONING.md`.

## 5A. Shipped firing schedules (Digital Fire catalogue) — DONE 2026-08-20

The 28 published schedules at https://digitalfire.com/schedule ship in flash as
read-only profiles (ids `PROFILE_BUILTIN_ID_BASE`+index, separate from the 8 user
slots), hide-not-delete via an NVS mask, generated from a scraper
(`tools/scripts/gen_builtin_profiles.py`/`scrape_digitalfire_schedules.py`) rather
than hand-transcribed, and paged over the size-limited UART LIST command. Credited
on `profiles_page.html` with a source link per entry.

### 5A.1 Feasibility marking — red when the tuning says it cannot be fired

`profile_feasibility.c` judges each segment against the autotune-fitted first-order
model (`dT/dt = (K*u - (T - T_amb)) / tau`): max heating/cooling rate, an
`UNREACHABLE` verdict for a target above `T_amb + K`, and the existing
`max_ramp_c_per_hr` ceiling on top. With no tuned model the verdict is `UNKNOWN`
(never a false OK or false red). Verified on hardware that the badge agrees with
what `profiles_start()` actually refuses. DONE and hardware-verified 2026-08-20.

- [x] **`T_amb` is now each zone's own persisted autotune ambient, not a live
      cold-junction reading (2026-09-05, reworked — the 2026-09-05
      live-cold-junction version below was reviewed and rejected same day).**
      `get_persisted_ambient_c()` in `profile_feasibility.c` reads
      `zone_tuning_quality_t::baseline_c` via `zones_config_get_tuning_quality()`
      — the settled chamber temperature autotune measured immediately before
      that zone's identification step began (`fopdt_model_t::baseline_c`,
      `pid_autotune.h`) — and falls back to the `FEASIBILITY_AMBIENT_C`
      constant (20 C) whenever `tuning_valid` is false, `method` is not
      `AUTOTUNE_METHOD_STEP`, the getter cannot answer, or the stored value
      is non-finite or outside `[5, 60]` C. Sampled once per public entry
      point (`profile_feasibility_segment_in_mask()`,
      `profile_feasibility_profile_in_mask()`) and threaded down as a
      parameter, so every segment of one profile walk sees the identical
      value even though `start_c` and `t_amb` used to be sampled at two
      separate call sites. Pure config lookup — no I/O, no lock, no cache,
      nothing that can go stale between calls.

      **2026-09-05 Opus review fix (of the `baseline_c`/`step_ambient_c`
      choice above, and of the `[0, 60]`/no-method-gate range check):** the
      field originally read here was `step_ambient_c` (a cold-junction
      reading), documented as "the ambient the fitted model is relative to"
      — which was wrong. `k_dc` is fitted in `autotune_finalize_fit()` as
      `raw_rise_c = final_c - baseline_c`, i.e. relative to `baseline_c`
      (the settled *chamber* reading), not the cold junction, which sits
      1.6-2.0 C below chamber per `autotune_engine_step_identify.c:920`.
      Mixing the two zero points skewed every ceiling by that gap. Switched
      to `baseline_c`, the field the fit is actually relative to.

      Separately, MAJOR: `zone_tuning_quality_t::step_ambient_c` (and
      `baseline_c`) is zeroed to `0.0f` at the start of every autotune run
      (`autotune_engine.c` ~line 1090) and is only ever given a real value at
      the SETTLING→STEPPING transition of a STEP-method run (~line 597); the
      old `[0, 60]` range check let that `0.0f` sentinel pass as a plausible
      cold-room reading, silently dropping the computed ceiling by 20 C.
      Fixed two ways: the range floor moved to `[5, 60]` (0.0 no longer
      passes on its own), and `q.method == AUTOTUNE_METHOD_STEP` is now
      required explicitly as defence in depth — `zones_config_
      set_tuning_quality()` is, as of this review, only ever called from the
      STEP-method accept path in `autotune_engine_guard.c` (the RELAY branch
      returns before that call, since a relay test fits no FOPDT model to
      attach a quality record to), so today the method check is always true
      for any `valid` record; it guards against that call site ever changing,
      or a stub/future caller reaching the getter with a RELAY-shaped record.
      Both breaks (dropping the `valid` check; dropping the `method`+range
      check) were confirmed to redden `test_profile_feasibility.c` by hand
      before being reverted — see that file's own tests for the receipts.

      **Why the live cold-junction version (below) was wrong, not just
      differently implemented:** Opus review the same day rejected it on
      four independent grounds — ~300 B of added stack on the httpd and LVGL
      tasks, both already near-overflowed elsewhere in this codebase; the
      LVGL task blocking up to `THERMO_OWNER_WAIT_MS` on the same SPI bus the
      display uses, from inside a render path; the ambient cache
      (`s_ambient_cache_c`/`_last_us`) being a plain, non-atomic pair
      written by whichever task called in and read by every other, i.e.
      torn under concurrent access with nothing serialising it; and,
      decisively, wrong physics — a MAX31856 cold junction reads the
      **board's own temperature**, which self-heats over the course of a
      firing, so using it as "ambient" would inflate the computed ceiling
      exactly when a firing is running and the check matters most.
      `autotune_engine_step_identify.c` (around line 920-935) already
      documents rejecting the cold junction as an absolute reference for
      this same reason — the live-ambient version above re-introduced
      exactly the input that file explicitly avoids. The tests added
      alongside the live version also never executed the live-reading
      branch (the stub always returned "no reading"), so the one bit of
      behavior that actually changed shipped unverified.

      Bench check 2026-09-05: `GET /api/zones` on the live board shows all
      three zones with `tuning_valid: false` (baseline/ambient fields
      unpopulated) — no zone has ever been autotuned with this record
      populated, so today every zone still gets the 20 C fallback in
      practice. The fix is correct and in place regardless; it starts
      mattering as soon as a zone is next STEP-autotuned and this record
      gets a real `baseline_c`.

      **Also documented here (not itself a code change):** any later write
      to a zone's PID gains — an accepted autotune of either method, an
      adaptive-tune adjustment during a normal firing, a backup import, or a
      manual edit — clears `tuning_valid` unconditionally
      (`zones_config_set_pid()`, `zones_config_accessors.c` ~line 401), which
      reverts that zone's feasibility ambient to the 20 C fallback until the
      next accepted STEP autotune. This is expected, conservative behaviour
      (an unreachable ambient is safer than a stale one) and is deliberately
      not being changed here — see this task's own constraints.

      Tests drive `test_stub_zones_set_coupling()`-style stub hooks
      (`stub_zone_ambient()`/`stub_zone_ambient_method()` local to
      `test_profile_feasibility.c`, backing the file's own
      `zones_config_get_tuning_quality()` stand-in) covering: a persisted
      30 C ambient reaching a 70 C target the 20 C fallback calls
      `UNREACHABLE`; getter failure, NaN, and out-of-range (80 C) all
      falling back to 20 C; `tuning_valid == false` with an otherwise
      in-range `baseline_c` (30) falling back; the `baseline_c == 0.0f`
      sentinel specifically falling back even with `valid == true` (the
      2026-09-05 review's MAJOR finding, now caught by the `[5, 60]` floor);
      a RELAY-method record with an in-range `baseline_c` falling back (the
      method-gate defence in depth); and two zones in the same profile
      judged independently against their own persisted ambients.
      Negative-tested two ways: reducing `get_persisted_ambient_c()` to
      `return FEASIBILITY_AMBIENT_C;` unconditionally (confirms the
      ceiling-raising and per-zone tests go red), and separately dropping
      just the `valid` check and just the `method`+range check (each
      confirmed, by hand, to redden a distinct test before being reverted).

- [ ] **One global bridge stall in a 25-minute soak (2026-08-20), cause not
      found.** 78 polling ticks, 77 clean; at t=1031s EVERY bridge surface
      (control/profiles/autotune/wifi/touch) failed in the same tick — "ACKed but
      no reply within 3.0s" — then the next tick was clean. The ACK proves the
      link/owner task were alive, so whatever blocked was downstream of frame
      delivery and hit all five task inboxes at once — the shared flash-safe
      executor only serialises control/profiles/autotune, so it cannot alone
      explain wifi/touch stalling too. Suspect a long flash/NVS op or the
      log-bridge queue backing up. Not reproduced since. Worth a targeted soak
      logging per-surface latency percentiles rather than pass/fail.

### 5A.2 Still open

- [x] **UNREACHABLE test edge fixed.** The steady-state ceiling test is now
      gated on `target > start_c` (rising only) in `profile_feasibility.c`;
      see `test_ceiling_test_gated_to_rising_segments` in
      `test_profile_feasibility.c`.

LCD profile access (browse/select/start) and the flash-headroom constraint
that used to block it are both done — see `docs/ARCHITECTURE_DECISIONS.md`
("Page organization") and §9.1 above for the reworked partition table.

## 5. Web UI — Fire profile creation page

DONE — `App/drivers/profiles_http.{c,h}` + `profiles_page.html`. Profile editor
(name, up to 12 segments of target/ramp-rate/dwell, `zone_mask`), save/load/delete
(8 NVS slots, versioned per section 8.2, same handlers the UART CONTROL bridge
uses), sanity-bound validation, and the feasibility check against each zone's
`max_ramp_c_per_hr` (hard reject over ceiling, warn within 20% margin) are all
built. The feasibility check re-running at profile-*start* time (not just save
time) is closed by section 6A.7's mid-firing ceiling re-check.

## 6. Firmware-side profile execution engine (implied by sections 2 and 5)

DONE — `App/drivers/control/profile_executor.c`, 1Hz tick. Segment stepping/ramp
interpolation, per-zone `OFF`/`BANGBANG`/`PID` control mode (live-reloadable
since 2026-08-12), the direction/rate sanity monitor (thermal_guard.c guards 1/2,
full detail in section 6A.3), relay-authority gating, fail-toward-off on pause,
and preemption by every existing safety mechanism are all built. Guards 1/2/4/7
are written but not yet exercised live (no thermocouple hardware to provoke
them) — tracked as a hardware-verification gap in section 6A.3/6A.8, not
reopened here.

## 6A. PID control, thermal protection, autotune, and zone interaction

**Status: built and hardware-verified where a thermocouple/kiln exists to verify
against; a large slice remains logic-verified only for lack of hardware.** Full
design and current status: `docs/PID_CONTROL.md` (control loop, guard table,
autotune flow, host-test summary) and `docs/GUARD_TEST_MATRIX.md` (one row per
guard: provocation, threshold, expected trip time, what the operator sees).
This section now only tracks what remains open.

Built: `pid.c` (positional PID, derivative-on-measurement, low-pass D filter,
anti-windup, functional-range blending, bumpless transfer at every
discontinuity, feedforward from the identified plant model, 2-DOF setpoint
weighting); `thermal_guard.c` (all 9 Klipper/Marlin-class guards, see
`docs/GUARD_TEST_MATRIX.md`); `heater_output.c` (time-proportioned duty,
min-on/off, contact-cycle accounting, phase-offset load staggering,
`max_simultaneous_relays` cap with deferred-not-dropped credit); `pid_autotune.c`
+ `autotune_engine.c` (step-test FOPDT fit + SIMC/ZN/Tyreus-Luyben tuning,
relay-feedback identification, cross-zone coupling-matrix capture, RGA display);
concurrent multi-zone execution with ramp-lock; the per-zone relay-authority
gate (6A.6); mid-firing config reload with bumpless transfer per field; a
host-side kiln plant simulator (`App/test/sim_plant.c`, multi-zone, injectable
faults) plus an on-target `KILNCTL_SIM_PLANT` build option
(`App/drivers/sim_backend.{c,h}`) for exercising the whole chain without real
hardware.

**Never run against a real firing.** No thermocouple/relay hardware has ever
completed the guard-provocation walk against real elements — every guard
proof to date is either a host-simulator test or, on the bench, guard 6
(sensor-invalid, trivially reachable with no thermocouple attached at all).
Treat every "implemented" claim in the linked docs as logic-verified unless
its own text says otherwise.

### 6A.0 What already exists to build on

See `docs/PID_CONTROL.md`'s module table. Two facts from `docs/HARDWARE.md`
that constrain the design: relays are EE2-12NUH electromechanical parts on
12V coils (expander pin high = energized), each on a 3-pin terminal block; a
"duty cycle" is therefore a slow time-proportioned window over I2C, not PWM.

- [x] **ANSWERED 2026-08-24 (owner): the on-board relays exist for galvanic
      isolation and switch other relays only.** They never carry element
      current. The part itself makes this unambiguous -- K1-K4 are
      **EE2-12NUH, a KEMET EC2/EE2 miniature SIGNAL relay**
      (`hardware/datasheets/mainBoard_Relay/EE2-12NUH.pdf` p7): max switching
      current **2 A**, max switching power **60 W / 125 VA**. A kiln element
      is kW-scale at tens of amps, so this relay physically cannot switch one
      and was never intended to.

      **Consequence for the duty window:** the pessimistic ~1e5-op budget this
      section assumed does not apply. The datasheet's loaded running spec is
      **1e6 operations** (50 VDC 0.1 A resistive, 85 degC, 5 Hz), with
      non-load life 1e8 and stable characteristics to 1e7 -- pilot duty into
      an SSR input or a small coil sits at the easy end of that. Taking 1e6 as
      the budget and one operation per window: a **10 s window** spends it in
      ~2,800 hours of *continuous* firing (~116 days), i.e. over a decade at
      40 h/week. A 2 s window still gives ~555 hours continuous (~2.7 years at
      40 h/week). So the window is a control-quality decision now, not a
      contact-life one; 10 s is comfortable and shorter is viable.

      Operate ~2 ms / release ~1 ms, so switching latency is irrelevant at any
      window we would pick.
- [ ] **New, narrower open question this raises:** what does each relay
      actually drive, and what is ITS switching cost? The on-board relay is no
      longer the limiting part -- the downstream device is. An SSR input is
      effectively unlimited; a mechanical contactor has its own electrical
      life (typically 1e5-1e6 ops) which then dominates the window choice.
      **Also a real ratings check, not a formality:** if a downstream
      contactor coil is 240 VAC, the relay's 125 VA switching limit allows
      only ~0.52 A at that voltage, and contactor coil INRUSH can exceed
      sealed current several-fold. Confirm the coil's inrush VA against the
      125 VA limit before driving one directly.

### 6A.1 Actuator model: time-proportioned output, not bang-bang

DONE — `App/drivers/heater_output.{c,h}`. Per-relay `window_ms`/`min_on_ms`/
`min_off_ms` (page-configurable per *zone*, not yet per individual relay —
see 6A.9), anti-chatter (an unachievably-short on-time renders as OFF, not a
minimum pulse), and NVS-persisted contact-cycle accounting (saturates rather
than wraps, exposed on Settings → Relays & Rules) are all built. Window
phase-offset across zones (load staggering) shipped as part of 6A.5.

### 6A.2 The PID loop itself

DONE — `App/drivers/pid.{c,h}`, pure C (no FreeRTOS/ESP-IDF/logging/I/O), so
it is host-testable against the simulator. See `docs/PID_CONTROL.md` for the
full form (derivative-on-measurement, anti-windup, functional-range blending,
bumpless transfer, feedforward, setpoint weighting). The
"cannot follow, cooling-limited" diagnostic is built
(`profile_exec_zone_status_t.cooling_limited`, exposed on `/api/control`) but
not yet exercised live — needs a real ramp-down faster than natural cooling,
which needs either a real kiln or `KILNCTL_SIM_PLANT` (neither run this pass).

### 6A.3 Thermal protection — the Klipper/Marlin-class safety suite

DONE — `App/drivers/thermal_guard.{c,h}`, a pure function of
`(setpoint, reading, commanded_duty, dt, guard_state)`. All 9 guards
(heating-failed, wrong-direction, runaway-with-heat-off, drift-at-setpoint,
absolute limits, sensor validity, frozen sensor, cross-zone plausibility,
control-tick liveness) are implemented, latch until an explicit operator
clear, fail toward off with retry, and escalate to the global fault mask or
a per-zone block as appropriate. `continue_on_zone_trip` (default: abort the
whole firing on any zone's trip) is configurable. Full per-guard status,
including which guards have been observed firing (guard-precedence findings:
several guards overlap on the same physical failure and the shortest window
wins — see `docs/GUARD_TEST_MATRIX.md`), lives in that doc, not here.

- [x] **Owner decision, 2026-09-28: thermal protection gets no disable
      switch, ever.** No per-firing-acknowledgement/Kconfig gate design is
      needed since no "disable thermal protection" affordance will exist.
- [x] **Guard thresholds are per-zone config.** The first eight closed
      2026-08-16 (`zone_cfg_t`, `zones_page.html`'s "Advanced guard
      thresholds" disclosure). The rest closed 2026-08-26 at the owner's
      request ("i dont realy like magic numbers"): guard 1's arming duty and
      no-progress window, guard 4's drift band, guard 7's frozen epsilon and
      guard 8's period are now per-zone fields too, alongside the executor's
      bang-bang band, cooling-limited margin/hold and ramp-lock band, and the
      one global PC-link abort timeout. Guard 8's period is the notable one:
      it already existed in `thermal_guard_cfg_t` but was never plumbed to the
      API and was passed 0 at the only call site, so it was invisible rather
      than absent. Edited on **/settings/safety**; NVS `ZONES_CFG_VERSION`
      7 -> 8. Every one keeps the "0 = not configured, the module substitutes
      its own named `#define`" convention, so the fallback numbers stay in
      code as the documented defaults and an unconfigured board is unchanged.
      Kconfig-tunable compile-time defaults were never asked for and remain a
      scope note, not a gap.

### 6A.4 PID autotune, for every zone

DONE for the step-test path (recommended first, per this section's original
reasoning: slow relay-cycling oscillation is thermally abusive on a kiln at
cone temperature) and for relay-feedback identification's math and on-target
state machine. `pid_autotune.c` + `autotune_engine.c`. Full detail in
`docs/PID_CONTROL.md`. Never run against real hardware — every on-target
autotune run aborts on guard 6 (sensor-invalid) before reaching a fit, since
no thermocouple is attached in this environment.

- [ ] **Gain scheduling by temperature band.** A kiln's plant gain is
      strongly temperature-dependent (radiative loss ~T^4). v1 ships a single
      band per zone, by design — `zone_cfg_t`/the NVS blob would need a band
      array to add this without a storage migration, and don't carry one yet.
      Also owns the Settings → Thermocouples & Zones page remainder (per-band PID
      gains/fitted model UI), merged here 2026-10-07 from section 6A.9.
- [x] DONE: predicted ramp ceiling adoption. `autotune_engine_accept(opts, out)`, with
      `opts->adopt_ceiling` set (autotune_engine.c/.h), writes `max_ramp_c_per_hr` via the
      existing `zones_config_set_max_ramp()` when requested; `POST /api/autotune/accept`'s optional
      `adopt_ceiling=1` form field and a checkbox next to the Accept button on `/settings/zones` opt in,
      default false.

### 6A.5 Multi-zone interaction — "they are not really separate"

DONE for (a) detuned decentralized PID (concurrent multi-zone execution),
(b) coupling-matrix capture during autotune, (c) RGA display, and (d)
ramp-lock/setpoint governor — all hardware-unverified for lack of a real
cross-gain to measure against (the math is exercised by the host simulator's
coupled two-zone model). Electrical load staggering (phase-offset windows +
`max_simultaneous_relays` cap with deferred-not-dropped credit) is also
built. Full detail in `docs/PID_CONTROL.md`.

- [ ] **(e) Static decoupler / cross-feedforward** using the measured coupling
      matrix. Targeted for v1.5, after (a)-(d) are proven on a real firing,
      gated on the RGA from (c) saying it's warranted.
- [ ] **Cross-zone plausibility guard's threshold (6A.3 guard 8) still
      defaults to 0 (disabled).** The guard's logic is built and host-tested;
      what remains is informing the threshold from a measured coupling
      matrix rather than a hand-picked constant, which needs either a real
      matrix (no thermocouple hardware to capture one) or an operator
      willing to enter a number for their own kiln. Its *period* stopped
      being hardcoded on 2026-08-26 (`guard_cross_zone_period_s`,
      /settings/safety); the delta that arms the guard is what still needs a
      measurement.
- (f) Full MIMO / model-predictive control: **decided out of scope** — the
  benefit over (a)+(d)+(e) doesn't justify the cost on a plant this slow, and
  there's no way to validate it safely on a device that fires unattended.
  Not open work, recorded as a decision.

### 6A.6 Safety plumbing that has to change

DONE: per-zone relay-authority blocking (`relay_authority_zone_blocked()`
layered on the global gate), the `THERMO`/`THERMAL_SANITY` fault-source
split, and closing the `SX_WRITE_REG`/`SX_SET_DIR` gate bypass. See
`docs/SAFETY_MODEL.md`'s updated summary table.

DONE (2026-08-21, `relay_authority.h`'s `relay_owner_t` — this bullet was
left open after the fix landed): **ownership tags are a real enum**
(`RELAY_OWNER_NONE`/`MANUAL`/`PROFILE`/`RULE`/`AUTOTUNE`), not just section
0's prose. `autotune_engine.c`'s `begin_run_locked()` claims
`RELAY_OWNER_AUTOTUNE` on the zone's relay mask for the duration of a
step-test/relay-test run; `force_relays_off()` — the single chokepoint every
terminal path (`finalize_fit()`, `finalize_relay_fit()`,
`escalate_and_abort()`, `abort_locked()`) calls before leaving a running
state — releases it, so a claim cannot outlive an aborted run. A manual
`SET_RELAY`/`SET_RELAY_MASK` against an autotune-owned relay is refused with
a reply (`kiln_io_owner.c`'s `relay_authority_manual_blocked_by_owner()` ->
`uart_bridge.c`'s `bridge_reply_reject(..., "owned")`, same wording as the
existing `PROFILE` case), not silently dropped. Safety keeps precedence:
`relay_authority_zone_blocked()`/`relay_authority_on_blocked()` are checked
first and their block is final regardless of ownership. **2026-09-04
correction: no longer true when danger mode is active.**
`kiln_io_owner.c`'s `relay_on_blocked()` checks `danger_mode_active()` first
and, if set, skips both `relay_authority_on_blocked()` and the OTA
heat-interlock check entirely (still logging a warning when one of them
would otherwise have blocked) — deliberately, per `danger_mode.c`'s and
`danger_mode.h`'s top comments, so an operator who has explicitly accepted
the risk can bench-test a relay/contactor with nothing fighting the test.
This is this ESP's own relay-on gate only; it does not touch SaftyFW's
independent contactor authority, which stays outside this board's reach
either way (`danger_mode.h`'s top comment). Host-tested in
`test_autotune_engine_prestart.c` (claim-on-start, release-on-guard-trip,
release-on-manual-abort, each proven able to fail by temporarily disabling
the release call and watching the new checks fail). Not host-tested: the
`uart_bridge.c`/`kiln_io_owner.c` refusal-with-reply path itself — neither
file is part of `build_host_tests.ps1`'s harness (FreeRTOS-task-shaped code,
not pulled in host-side anywhere today), so that leg is verified by reading
the code, not by a test that can fail.

### 6A.7 Task, timing, and module layout

DONE: all five modules exist (`pid.c`, `thermal_guard.c`, `heater_output.c`,
`pid_autotune.c`, `profile_executor.c`); 1Hz tick reading all channels once
per tick; task priority tied with the UART bridge tasks, below the
link-loss watchdog (closest achievable match to "above bridge, below
watchdog" given FreeRTOS priority integers); calibration applied on the
control path with guards seeing raw readings; mid-firing config reload with
per-field bumpless-transfer/cold-restart/immediate-apply rules; an
unowned-relay sweep; and the mid-firing `max_ramp_c_per_hr` re-check (warns,
doesn't block or re-run feasibility).

- [x] **The tick must not block** — both bus accesses go through the
      existing `i2c_owner`/`esp_spi_owner` queues with bounded timeouts, but
      whether a timeout is actually treated as a bad read (feeding guard 6's
      debounce) rather than skipping guard evaluation was never confirmed by
      reading `MAX31856_read_all()`'s timeout contract. CONFIRMED 2026-09-04
      by reading the code: a channel lock timeout surfaces as
      `MAX31856Reading.spi_failed`, and `profile_executor_run.c` (e.g.
      lines 601-602, 645) treats `spi_failed`/`isnan()` readings as bad --
      feeding guard evaluation rather than being skipped.

### 6A.8 Verification — how any of this gets trusted

DONE: host plant simulator (single- and multi-zone, injectable faults),
on-target `KILNCTL_SIM_PLANT` build option, host unit tests for `pid.c`/
`thermal_guard.c`/`heater_output.c`/`sim_kiln.c` (see `docs/GUARD_TEST_MATRIX.md`
for the coverage table), and autotune validated against the simulator's known
ground truth. The on-target guard walk (start a profile, inject each fault
over `POST /api/sim`, watch the right guard trip) has not been performed —
was blocked by DRAM exhaustion (see `docs/BRINGUP_HAZARDS.md`), now fixed but
not yet exercised.

- [ ] **Bench test before kiln test**: a small resistive load and a
      thermocouple (soldering-iron element, heat gun into a can) to exercise
      the whole chain — real SPI reads, real I2C relay writes, real timing —
      at temperatures that cannot hurt anything.
- [ ] **First real firing is attended, low-temperature, and logged**, with
      the history/trace export (6A.9) reviewed afterward before anything
      runs unattended.

### 6A.9 UI and telemetry additions this section implies

DONE: history ring buffer gains `duty`/guard state; the dashboard graph
overlays desired/actual/duty with guard trips marked; `GET /api/control`
live status endpoint; CSV export of history and autotune traces (paginated
after an early out-of-memory bug, see `docs/BRINGUP_HAZARDS.md`).

- [x] **Settings → Thermocouples & Zones page is only partially grown to
      match this section.** Control mode, `max_temp_c`/`min_temp_c`, per-zone
      heater timing, and a working autotune card are on the page. Bang-bang
      hysteresis is DONE -- exposed per-timing-profile as
      `bangbang_hysteresis_c` (`zones_http_handlers.c:250-257`), no longer
      hardcoded. **CLOSED 2026-09-22:** the six always-emitted, POST-round-
      tripped per-zone scalars that had zero renders anywhere on the page --
      `coupling_diag_k_dc`, `ease_off_window_mult`, `approach_rate_cap_c_per_hr`,
      `error_band_c`, `rate_band_c_per_s`, `progress_band_c` -- now render
      under two own-value (no inheritance group) disclosures, "Coupling
      diagnostic gain" and "Controller bands & shaping", and round-trip on
      save; covered by `test_zones_type_toggle.js`. Still missing: per-band
      PID gains/fitted model (gain scheduling is unbuilt, 6A.4) -- the only
      remaining part of this line item.
      MERGED 2026-10-07 into the "Gain scheduling by temperature band" item
      (6A.4): that is now the single tracker; this checkbox is closed.
- [x] **Per-relay `window_ms`/`min_on_ms`/`min_off_ms`** — CLOSED
      2026-09-20, deliberately kept per-zone, not implemented per-relay.
      `zones_config_json.h`'s `zone_cfg_t::heater_window_ms` comment (added
      alongside this field) already gives the reason: a zone's relay group
      is switched as one unit by a single `heater_output_cfg_t` decision in
      `profile_executor.c`/`autotune_engine.c` — there is no
      per-individual-relay timing concept anywhere in the control path to
      hang independent values off of. Building one would mean decomposing
      a multi-relay zone's single duty-cycle decision into independently
      timed sub-relay control, a control-path redesign, not a config/UI
      addition — out of scope for this line item.
- [x] The 2s dashboard polling revisit -- MERGED 2026-10-07 into the "Still
      polled (2s), not pushed" item in section 2 (single tracker).

### 6A.10 Suggested build order

Reference only, all steps through PID mode + feedforward + autotune + RGA
+ ramp-lock have shipped. Remaining: cross-feedforward decoupler (6A.5e),
load-staggering refinements, relay-feedback autotune as the routinely-used
alternate method.

### 6A.11 Open questions, collected

- [x] Do the on-board relays switch elements directly or drive external
      SSRs/contactors? **ANSWERED 2026-08-24: galvanic isolation only, they
      switch other relays.** K1-K4 are EE2-12NUH signal relays (2 A / 125 VA
      max), so element switching was never physically possible. Duty window is
      now a control-quality choice, not a contact-life one — see 6A.0 for the
      arithmetic and for the narrower question it replaces (what the
      downstream device is, and its coil inrush against the 125 VA limit).
- [ ] Element power per zone and total supply/breaker capacity — decides
      whether load staggering (6A.5) is mandatory or optional.
- [ ] Maximum rated temperature of the kiln and of the thermocouples fitted
      (guard 5's `max_temp_c` is mandatory config; it needs a real number).
- [ ] Is there any active cooling or a vent that materially changes the
      plant (the rule engine can drive a vent relay — if it does, the
      identified model is only valid for one vent state, which the autotune
      procedure must record and the docs must state).
- [x] ~~Physical zone arrangement~~ — **answered 2026-09-03, owner-confirmed.**
      Stacked vertically as rings up the chamber wall; zone 2 is the BOTTOM
      element, zone 0 the TOP (zone 1 middle). Corroborates the coupling
      matrix's measured asymmetry (zone 1 leaks into zone 0 harder than the
      reverse). See `firmware/KilnFW/docs/HARDWARE.md` "Physical zone
      arrangement (test kiln)".
- [ ] The sanity rate the request deferred ("I will determine later"), plus
      first-pass values for every other threshold in 6A.3 — the current
      defaults are engineering guesses, explicitly labeled as such.

## 7. Documentation

DONE — `docs/WEB_UI.md`, `docs/WIFI_PROVISIONING.md`, `docs/PROFILES.md`,
`docs/PID_CONTROL.md`, `docs/GUARD_TEST_MATRIX.md`, and `docs/SAFETY_MODEL.md`'s
relay-caller summary table are all written and current. Keep them updated as
their subject areas change (see each doc's own dated-entry convention).


## 8. Storage partitioning, boot-time compatibility, and setup UX

**All five asks from this section are built** (per-concern NVS partitions,
boot-time version compatibility, the config-wizard/readiness page, and the
network status/saved-networks page). Requested 2026-08-12 after a board with
valid credentials reported itself unprovisioned. What remains open below is
purely hardware verification — none of it has touched a physical board.

### 8.1 One partition per concern

DONE — `wifi_nvs`, `kiln_nvs` (zones/rules/relay_cycles/run_state), and
`profiles_nvs` are split (`partitions.csv`), each with its own one-time
migration from the old shared default `nvs` partition (old copy never
deleted, so a rollback still finds working data), and each module owns its
own `nvs_flash_init_partition()`/`nvs_flash_erase_partition()` — no more
blanket `nvs_flash_erase()`. `POST /api/factory_reset` (`App/drivers/
factory_reset.{c,h}`) resets by scope (`wifi`/`kiln`/`profiles`/`all`), a
clean 400 on a missing/unrecognized scope, erase-then-reboot.

- [ ] **Factory-reset erase-then-reboot cycle not exercised on real
      hardware** — in particular, whether the "ok, rebooting" HTTP response
      reliably reaches the browser before the socket drops.

### 8.2 Boot-time compatibility check for every non-volatile section

DONE — every persisted structure (`zones_cfg_t`, rules, profiles,
relay-cycles) carries a schema version checked *before* any size-mismatch
wipe (a real ordering bug here was found and fixed 2026-08-13 — the version
check originally ran after the size check, defeating the whole point of a
migration path; exercised for real by the `continue_on_zone_trip` v1→v2
field addition). Three outcomes (load / migrate / refuse-newer-and-keep,
never wipe a newer-than-firmware blob), one boot-time report
(`App/drivers/nvs_report.{c,h}`, `nvs_sections` on `/api/status`,
partition-granularity only), and `zones_config_is_valid()` gating
`profile_executor_run()`/`autotune_engine.c` are all built.

`rules_http.c`/`relay_cycles.c`/`profiles_http.c` share the same
version-vs-size-ordering pattern as the pre-fix `zones_http.c` bug — not
independently fixed since none of them needed a field added this pass; fix
the same way whichever future change actually grows one of those structs.

A related bug (fixed 2026-08-24, see `docs/ARCHITECTURE_DECISIONS.md`'s "NVS
rollback-refusal vs. legacy-partition migration" section): a refused
newer-than-firmware blob could still get silently overwritten by 8.1's
legacy-partition migration, because the migration decision couldn't tell
"refused" apart from "nothing was ever saved." Any new module that adds
both a version check AND a migration off another partition must read that
section before wiring the two together.

- [ ] **Migration path never verified on real hardware** — that a board with
      real saved zone/rules/profile/relay-cycle data actually carries it
      forward into `kiln_nvs`/`profiles_nvs` on first boot after this
      change, and that `zones_config_valid` reads `true` after migration and
      `false` on a genuinely unconfigured board.

### 8.3 Config wizard page: what is set up, what is not

DONE — `GET /readiness` + `GET /api/readiness` (`App/drivers/
readiness_http.{c,h}`), a pure read-only aggregator over 11 items (network,
thermo/zone mapping, relay assignment, control mode, guard limits,
calibration, profiles, autotune, hardware present, storage compatible), each
`ok`/`not_done`/`cannot_yet`/`deliberately_off`, ordered so a page doesn't
nag about a step that's blocked on an earlier one.

Two honest, documented gaps left as `not_done`/`ok`-only rather than
inventing a distinction the storage doesn't carry: **control mode** has no
"explicitly set" bit distinct from its zero-initialized default, and
**calibration**'s `cal_offset_c == 0` has no "deliberate zero" convention the
way `max_temp_c`/`cross_zone_max_delta_c` do. Closing either needs a new
persisted flag per field — deliberately not added (out of this pass's scope).

### 8.4 Network page: live status, mode switch, and saved networks

DONE — live status (mode, join state, RSSI, IP, mDNS name, AP client count)
on the same page as the mode switch; a bounded (8-entry) saved-network list
with a documented RSSI tie-break and Forget; `GET /networks` merges saved +
scanned into one view rather than two lists; forgetting the last saved
network is allowed (drops to AP mode, never blocked); stored passwords are
never returned by `wifi_prov_get_saved_networks()` (structurally absent from
its output type) — except the board's **own** AP identity, an explicit,
user-confirmed scoped exception, always shown pre-filled for editing.

- [ ] **Nothing in this subsection has been flashed/tested on real
      hardware.** In particular: a pre-8.4 single saved network migrates
      into `nets[0]` and still joins on boot; two saved networks and the
      board actually prefers the stronger one; forgetting the last saved
      network drops to AP mode without stranding the operator; the periodic
      30s rescan (`rescan_timer_cb`) rejoins a saved network coming back
      into range without waiting for a disconnect event; and moving the
      scan-based tie-break off the Wi-Fi driver's disconnect-event handler
      (onto that periodic timer instead) doesn't introduce a noticeably
      slower reconnect when the active network is just flapping.

### 8.5 Sequencing

Reference only, all four sub-sections shipped in the dependency order this
called for (8.2's versioning before 8.1's partition split; 8.1 before 8.4's
saved list; 8.3 last as a view over the other three).

---

## 9. Firmware updates — ESP OTA and relaying the Pico's image

Full design (interlocks, shared authentication): [`../CommonFW/docs/
UPDATE_PROTOCOL.md`](../CommonFW/docs/UPDATE_PROTOCOL.md). RP2040 half:
[`../SaftyFW/docs/BOOTLOADER.md`](../SaftyFW/docs/BOOTLOADER.md). Two things
make this more than a normal OTA feature: the partition table can only be
changed over a cable (**you cannot OTA your way into being OTA-capable**, so
the first flash of the new table is a one-time USB step); and the ESP is the
only thing authorising a Pico update, making this endpoint the safety
processor's attack surface too.

**Everything through 9.6a is built and either host-tested or `idf.py build`
clean** (mutual version compatibility, the 8/16MB partition table +
rollback, HMAC-challenge auth with lockout, interlocks that refuse an update
unless idle/cool/safety-link-healthy, streamed transfer for both processors
via a `pico_img` staging partition, the web page, and MCP tools). **No
physical ESP32-S3 or Pico exists in this environment — nothing in this
section has ever run against real hardware.** Section 9.7 below is
therefore the entire hardware-verification surface still owed, and every
other open item below is a real, separately-named gap rather than a
hardware caveat.

### 9.0 Mutual version compatibility (prerequisite)

DONE — `ANNOUNCE_VERSION` burst at boot/on `boot_id` change (now via the
shared `kilnlink_announce` codec), bidirectional `KILNLINK_MIN_COMPATIBLE`
check, a mismatch treated as a dead link, the GUI naming both versions and
which is older (always "update ESP first"), and an explicit `GET_FW_VERSION`
retried until answered. Full detail: `LINK_PROTOCOL.md` sec 4.

- [x] **Refuse to push a Pico image this build could not then talk to**
      (protocol-incompatible). DONE — closed by 9.5's matching item:
      `ota_pico_do_stage()` (`ota_http_pico.c`) refuses a staged image whose
      `link_protocol_version` differs with `409 Conflict` unless
      `X-Ota-Force-Version: 1` is sent.

### 9.1 Partition table

DONE — confirmed N16R8 (16MB flash/8MB PSRAM); `partitions.csv` carries
`otadata` + `ota_0`/`ota_1` (2048K each) + `pico_img` (896K, corrected up
from an originally-proposed 512K — too small for SaftyFW's 832K app slot),
all above `0x200000` where nothing else lives. Rollback enabled
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`). `idf.py build` clean, no
overlap/overflow.

- [x] **`esptool flash_id` never run** — CLOSED 2026-10-07: esptool is forbidden
      by policy (`docs/agent_rules/BENCH.md`; OpenOCD only) and the N16R8 part is
      known. (The buy-list and 3D-model records claiming 8MB/N8R8 were noted stale
      here; not re-verified in this pass.)
- [x] **Bootloader + partition table reflashed on the physical board** —
      2026-08-24 (the date this was directly observed; an earlier flash on
      2026-08-22 is likely but was not confirmed against this table), via
      `flash_firmware()`'s JTAG path (`tools/PcTools`):
      `bootloader.bin`@0x0, `partition-table.bin`@0x8000, `KilnCtrl.bin`@0x810000,
      each `program_esp ... verify`, reported "flashed and verified OK". Note
      the app offset: 0x810000 is `factory`'s offset in this table, so this
      path has never written an OTA slot (see 9.2's note on what that means
      for rollback confirmation).
- [x] **Pre-change partition table archived** — MOOT 2026-10-07: the table was
      redesigned (single-slot, `docs/OTA_SINGLE_SLOT_PLAN.md`) and flashed; there is
      no pre-OTA table to roll back to.
- [x] **One-time serial flash documented as a prerequisite step**, not a
      footnote. DONE — added a dedicated bullet to
      `firmware/KilnFW/docs/PROJECT_STATUS.md`'s "Current build/hardware
      configuration" section, naming the JTAG/serial requirement explicitly
      and pointing at `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §3, which
      already carried the underlying rationale but only as narrative prose,
      not a bring-up-checklist item.
- [x] **`nvs`/`wifi_nvs`/`kiln_nvs`/`profiles_nvs` archived with `esptool
      read_flash` before the new table is flashed** — MOOT 2026-10-07: the new
      table was flashed long ago, and esptool is forbidden by policy (OpenOCD
      only); the step can no longer be done as written. Use `backup_export` for a
      config backup.

### 9.1a PSRAM — enabled 2026-08-17

DONE. Originally decided off (four reasons: no framebuffer needed since the
ILI9488 driver streams to its own GRAM, determinism/stall risk, a new boot
failure mode, DMA-buffer audit cost); reversed once LVGL (section 10) needed
draw buffers — the named trigger this decision itself called out in advance.
`sdkconfig`: `CONFIG_SPIRAM=y`/`_MODE_OCT=y`/`_BOOT_INIT=y`. LVGL's draw
buffers and allocator, plus several bridge-task stacks, now live in PSRAM
(see `docs/BRINGUP_HAZARDS.md` for the internal-SRAM race this interacted
with). R8 (with PSRAM) is now a hard requirement on any board reorder — an
R2/no-PSRAM part would break the LCD memory plan.

- [ ] **Second, weaker trigger**: TLS on the web server, or many concurrent
      HTTP connections — measure the heap before assuming either needs more
      PSRAM use.
Standing rule (not a task): keep GPIO 33-37 unassigned -- they are consumed by
the R8 module's own PSRAM regardless of software config. No conflict today (board
uses 0-21, 38, 43, 44, 47, 48); keep it that way on any future pin assignment.

### 9.2 Rollback

DONE — `esp_ota_mark_app_valid_cancel_rollback()` runs from a background
task gated on NVS-readable + web-server-up + OTA-routes-up
(`boot_confirm_is_healthy()`, `App/drivers/persist/boot_guard.h`); a live
safety-link exchange was dropped from the bar 2026-08-22 (a board with no
RP2040 answering could otherwise never confirm, silently reverting every OTA
update). `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` deliberately left off.

- [x] **Factory-boot false ERROR fixed, 2026-08-24** — every JTAG-flashed
      build boots from `factory` (see 9.1), and
      `esp_ota_mark_app_valid_cancel_rollback()` has nothing to cancel there;
      it reliably returned `ESP_FAIL`, logged as an ERROR every boot.
      `ota_rollback_confirm_task()` (`App/main.c`) now checks
      `esp_ota_get_running_partition()` and, via
      `boot_confirm_decide()` (`boot_guard.h`), skips that call on a factory
      boot with an explanatory INFO line instead — `boot_guard_mark_healthy()`
      still runs. OTA-slot boots are unchanged.
- [ ] **Still not exercised end-to-end by anything on this bench** (re-checked
      2026-10-07: no evidence in `docs/BENCH_TEST_LOG.md` or `logs/bench_test/`). The
      PENDING_VERIFY / rollback-cancel machinery has only run in host tests
      (`test_boot_guard.c`). Under the single-slot design
      (`docs/OTA_SINGLE_SLOT_PLAN.md`) a direct push into the running `app` slot is
      refused (OT-E01), so exercising it needs the recovery-image path
      (`recovery_enter` -> `recovery_apply_staged` -> `recovery_exit`); that apply
      has not been performed on the bench (the stage step only, BENCH_TEST_LOG).

### 9.3 Authentication — the AP password, not sent over the wire

DONE — single-use 30s-expiry challenge, HMAC-SHA256 proof of knowledge (PSA
Crypto API), constant-time comparison, lockout after 3 failures (doubling to
15 min, per-endpoint), every attempt logged with source IP, and only the
derived key (never the literal PSK) ever reaches the comparison.

- [x] **Stated user-visible on the OTA web page.** DONE — the prose exists
      in `UPDATE_PROTOCOL.md` §2, and `ota_page.html:135-139`'s note under the
      AP-password field already says the same thing in user-facing copy:
      "This proves you're on the network with the AP password -- it does not
      stop anyone else who also knows it...". Predates this classifier pass
      (present since the file's 2026-09-05 `drivers/` reorg move, `9f18ca5c`);
      this item was stale.
- [ ] **TLS — planned in `docs/UI_PLAN.md` §6, not authorized to build.**
      Adds confidentiality/integrity to the image transfer (today plaintext
      LAN); does not replace or weaken the HMAC scheme above, and is not
      image signing (that stays a separate, unplanned concern). Switching
      the shared `esp_http_server` instance to `httpd_ssl_start()` would
      cover every OTA route with no per-route change.

### 9.4 Interlocks

DONE — both update paths refused unless idle (no profile/autotune running,
no heater commanded, every zone below a configurable ceiling, safety link
healthy), refusals name the specific unmet precondition, and a single
in-RAM mutex refuses starting either update path while the other is in
progress. During a Pico update the safety-link-down alarm *text* is
suppressed (never the relay-on block itself).

### 9.5 Transfer

DONE — streamed `esp_ota_ops` POST handler for the ESP image (no full image
held in RAM), a `pico_img` staging partition + background relay task driving
the five `UPDATE_*` frames with gap-based retransmission (up to 10 rounds,
then always attempts `UPDATE_END` and lets the Pico's own CRC verification
arbitrate), whole-transfer socket timeouts, image magic/chip-ID validation
before `esp_ota_begin()`, `SAFETY_CMD_ANNOUNCE_REBOOT` sent before a routine
reboot so it doesn't trip the safety processor's S6b guard, the update
mutex's first real caller, an append-only (single-record) NVS update log,
and polled progress for both paths.

- [x] **Protocol-version mismatch between an uploaded Pico image and the
      running ESP is now proactively warned about with a second
      confirmation** before the relay starts. DONE — the uploaded image's own
      `KILNLINK_PROTOCOL_VERSION` was not previously captured anywhere on
      disk (the wire-frame `UPDATE_BEGIN` header the ESP builds always
      states the ESP's own version, never a value read out of the uploaded
      bytes), so this added a new `link_protocol_version` field to the
      `saftyfw_image_identity_t` build-identity record every SaftyFW slot
      image already carries (`firmware/CommonFW/include/kilnlink/saftyfw_image_identity.h`,
      `SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION` bumped 1 -> 2, reusing the
      previously-unused `reserved` field so the record stays 60 bytes; a
      record_version 1 image correctly reads as "unknown", never a
      fabricated protocol 0). `ota_http_pico.c`'s manual-upload handler
      (`ota_pico_do_stage()`) now calls the existing
      `pico_image_source_describe()` scanner right after staging finishes
      and, if the staged image's `link_protocol_version` disagrees with this
      board's compiled `KILNLINK_PROTOCOL_VERSION`, refuses with
      `409 Conflict` and a JSON body naming both versions
      (`{"error":"protocol_version_mismatch","image_protocol_version":N,"esp_protocol_version":M}`)
      unless the request carries a new `X-Ota-Force-Version: 1` header
      (mirroring `ota_http.h`'s existing `X-Ota-Ack-No-Safety` pattern — a
      header, not a body field, since this route's body is the raw image).
      No new HTTP route was added. The Pico's own
      `UPDATE_STATUS_ERR_VERSION_INCOMPATIBLE` refusal is unchanged and still
      runs regardless — this is a strictly earlier, proactive warning on top
      of it. The boot-time auto-update path (`pico_auto_update_boot.c`)
      calls `ota_pico_relay_start()` directly and never reaches this
      handler, so it can never set the force header and is structurally
      unaffected. PC-side: `tools/PcTools/src/kilnctrl/ota_http_client.py`'s
      `push_pico_image()` gained a `force_version` parameter (default
      `False`) and raises a new typed `OtaPicoProtocolVersionMismatch` on the
      409; the `ota_update_pico` MCP tool surfaces the same. Host-tested:
      `firmware/CommonFW/test/test_saftyfw_image_identity.c` (new,
      CMake-registered) covers the record_version bump end to end, including
      that a record_version 1 image is rejected rather than misread; pytest
      covers the client/MCP surface
      (`tools/PcTools/tests/test_ota_http_client.py`) and the drift guard
      against the C header
      (`tools/PcTools/tests/test_pico_image_freshness.py`).
- [x] **`ota_record_t` has no image SHA-256 field** — flagged, not faked;
      would need hashing the stream as it passes through the transfer
      handler (mbedTLS/PSA is already linked for the HMAC path). DONE --
      `image_sha256_hex` is hashed via PSA as the transfer streams through
      (`ota_http_esp.c:207-342`, split out of what was then `ota_http.c`)
      and surfaced on `/api/ota/status` (`ota_http_esp.c:538`, likewise
      split out of `ota_http.c`).

### 9.6 Web page

DONE — `GET /ota` (`ota_page.html`), per-processor version/commit/build-date/
dirty-flag/active-inactive-slot (ESP side), interlock state shown before the
file picker (unauthenticated `GET /api/ota/interlock`, file pickers hidden
entirely until it reports idle), progress bars, and an ESP rollback button
(`POST /api/ota/esp/rollback`, its own HMAC context so a push-authorization
signature can never double as a rollback authorization).

- [x] **Pico build commit/dirty flag/build date are not retrievable.** DONE
      (found shipped, eighth audit pass, 2026-09-04): `safety_link.h` now
      carries `peer_build_known`/`peer_build_dirty`/`peer_build_commit[64]`/
      `peer_build_commit_len`/`peer_build_datetime[32]`/`peer_build_datetime_len`
      (`safety_link.h:1145-1155`), populated by the FW_VERSION parse and read
      via `safety_link_get_peer_build_status()` (`safety_link.h:1349`,
      `safety_link.c:832-841`). Surfaced through `dashboard_http.c`'s
      `safety_build_*` fields (`GET /api/status`) and rendered on the OTA
      page's `renderPicoInfo()` (`ota_page.html:490-494`).
- [x] **Pico rollback button is a disabled placeholder** ("not yet
      available") — no HTTP route exists; a Pico rollback trigger is
      expected to go over the safety UART link (`SAFETY_CMD_ROLLBACK`, 0x17,
      already built on the send side per `safety_link_send_rollback()`), not
      HTTP. DONE -- the button is live and wired (`ota_page.html:168`,
      `:597-624`), POSTing `/api/ota/pico/rollback` with MAC-authenticated
      request/poll-for-status handling.

### 9.6a MCP tools (`tools/PcTools`)

DONE — `ota_get_challenge`/`ota_update_esp`/`ota_update_pico`/`ota_status`/
`ota_rollback_esp` all wrapped as `kilnctrl` MCP tools (reachable via
`kiln_call`), unit-tested
against mocked HTTP. No physical board has ever answered a real request from
these tools. `image SHA-256` is unbuilt (same gap as 9.5's `ota_record_t`
note).

### 9.7 Verification

Superseded 2026-10-07 by the scripted OT matrix in
`tools/PcTools/src/kilnctrl/bench_test/cases_ota.py`
(`docs/BENCH_TEST_SYSTEM_PLAN.md` section 3.4; run with `bench_test_run(suite="ota")`
or `ota_matrix_run`). Execution status, from `logs/bench_test/*ota*` (gitignored) and
`docs/BENCH_TEST_LOG.md`, not from the matrix existing:

- [x] Corrupt image rejected (OT-E03 bad CRC, OT-E04 truncated, OT-E05 wrong build):
      real-board PASS for the ESP push path, run `20261003T223938Z_ota_ot_410c346c_rerun`
      (with OT-E01 self-push 409, OT-E09 no-credential, OT-E12 otadata state). Pico-side
      corrupt-image refusal is not covered by an OT case.
- [x] Update attempted while firing/autotune refused (OT-E07/OT-E08): real-board PASS,
      run `20261005T160721Z_ota_bench20261005` (an earlier run `20261004T000619Z`
      SKIPped both because no state was running).
- [x] Dual reflash (OT-B01): PASS on hardware 2026-10-03, with the caveat that
      `sw_reset_esp` does not latch S6a, so the "expected S6a trip" branch has not
      been observed via this path.
- [ ] Power pulled mid-transfer, both processors (OT-E06): attended case, never
      executed to a verdict. Needs an operator.
- [ ] An image that boots but fails bring-up is rolled back with no intervention:
      no OT case exists for it; still open.
- [ ] Recovery from a deliberately bricked Pico over SWD: blocked on the missing
      CMSIS-DAP probe (2026-10-03, see the Pico debug-probe note in
      `docs/BENCH_TEST_LOG.md`).
- [x] ~~Wrong password: refused, locked out, logged~~ MOOT 2026-10-07: the AP-password
      HMAC scheme was retired 2026-09-29 ("Retire; open when login off",
      `docs/WEB_AUTH_PLAN.md` item 2b); OT-E09/E10 cover the web-auth gate instead.

## 10. LCD touchscreen GUI (Klipper-style screen)

The LCD (ILI9488, physically connected) has its own touchscreen UI, kept in
sync with the web dashboard rather than duplicating/diverging from it (see
10.5). Usability/no-scroll budget audit and fix queue: `docs/UI_PLAN.md`.

### 10.1 Generic screen/page/widget framework

DONE — LVGL chosen and owns the ILI9488 outright (replacing the old
UART-remote-drawn `DISPLAY_CMD_*` path; this was an explicit user decision
once two independent draw-call owners proved to be an unresolvable race).
`App/drivers/ui/kiln_ui.c`/`.h` is the page registry/switcher; every page is its
own `ui_page_<name>.c`/`.h` pair, never inlined into `kiln_ui.c`. Fixed a real
bug in the process: a touch waking the screen from idle previously set a flag
with nothing to repaint it — `lvgl_port.c`'s flush callback now forces a
redraw on the wake edge.

- [x] **The twelve stale `display_*` MCP tools were deleted** (ROADMAP.md:1140,
      1071-1083). This entry was stale — see `DISPLAY_ST7796_PLAN.md` §0 and
      §14.
- [x] **`uart_bridge.c`'s `display_bridge_task`/`uart_bridge_start_display_task()`
      were deleted 2026-08-27 (`ddbd0248`)** — confirmed dead code first
      (`main.c` never called `uart_bridge_start_display_task()`; LVGL owns the
      panel outright), then removed along with its `xTaskCreatePinnedToCore()`
      call site, header declaration, and its stack-margin registration entry.
      `DISPLAY_CMD_*`/`UART_TASK_ID_DISPLAY` stay defined in `uart_task_ids.h`
      as wire-protocol constants only — kilnctrl's `gui.py`/`actions.py`
      Display panel still speaks them, even though nothing on the firmware
      side answers any more. A later pass (2026-09-17, re-verified against
      `origin/main`) confirmed no trace of the task, its constants, or a
      registration entry remains anywhere in `firmware/`; `ARCHITECTURE.md`,
      `ARCHITECTURE_DECISIONS.md`, and `UART_PROTOCOL.md` already recorded the
      same removal and needed no further edit.
- [ ] Decide the color/asset story once the visual style (10.2) is picked —
      LVGL widgets are themeable, so this is a theme/style pass on stock
      widgets, not custom-drawn ones.

### 10.1a Shared backend with the web UI

**Standing rule, not a one-time task**: a page's data access and actions
must go through the same plain-C functions both the LCD and the web HTTP
handlers use — one backend, two front ends, never two independently
maintained readings of the same state. Built incrementally, split out of an
HTTP handler at the point a 10.3 LCD page first needs the same data (not
speculatively ahead of a real second caller). See `docs/ARCHITECTURE_
DECISIONS.md`'s LVGL/LCD section for the rule statement.

- [x] As each 10.3 page is built, extract its backend data access from the
      matching HTTP handler into a shared plain-C getter/action function —
      actions (`profile_executor_run()`/`_pause()`/`_halt()`,
      `relay_authority_*`) are already single-implementation and correct as
      called from the LCD; it's specifically the *read* side each handler
      still inlines that needs splitting when a page reaches for it. DONE for
      every page built to date: `dashboard_get_status()`
      (`dashboard_http.c:88`, consumed by `ui_page_home_refresh.c:116`),
      `dashboard_exec_http.c:120`'s exported read seam (consumed by
      `ui_page_home.c`), and `wifi_prov_*()` getters (`wifi_prov_api.c`,
      consumed by `ui_page_network.c`, whose own header comment at
      `ui_page_network.c:23-40` enumerates the shared call for every control
      on the page) are each cited by name at both call sites — grep
      `TODO.md 10.1a` across `App/drivers/ui/*.c` and `App/drivers/http/*.c`
      for the full cross-reference list. This bullet is a standing rule
      (line above), so it still applies to any page built after this pass.
- [x] Where a handler is split, update the HTTP handler to call the new
      shared getter too — never leave it calling the data-owning module
      directly while only the LCD gets the seam. DONE for every page built to
      date — same citations as above; each shared getter's own call sites
      list both an `App/drivers/http/*.c` and an `App/drivers/ui/*.c` caller,
      not the LCD side alone.

### 10.2 Visual style — match KlipperScreen

DONE — `App/drivers/ui/ui_theme.h` + `docs/UI_THEME.md`: dark-navy palette, five
named accents, minimum touch target (72px), corner radius, padding, and status
bar height, all as a single shared source of truth (10.6 restyles the web
dashboard to match). First-pass values inferred from a description of
KlipperScreen's look, not extracted from its actual theme files — flagged as
unverified-against-real-hardware in `ui_theme.h` itself; sanity-check against
the physical ILI9488 and correct both files together if it's off. Font sizes
are not yet part of the shared table (LVGL font selection wasn't scoped in).

### 10.3 Page designs

DONE — home (zone cards, desired-vs-actual chart, Start/Stop/Menu),
Configuration hub (7 nav destinations), Temperature (per-zone reading +
manual relay toggles, no setpoint override per explicit decline), Network,
Safety Processor card, Board Health, Diagnostics, Thermocouple Faults, and
Touch Calibration (with a Cancel path, fixed 2026-08-19) all exist and are
rebuilt to fit the hard **no-scroll rule**: every page must fit
`ui_theme.h`'s `UI_THEME_PAGE_CONTENT_BUDGET_PX` (268px, computed from real
constants as of 2026-08-24 — every "~264px" figure in this file and in
several pages' own comments predates that `#define` and is 4px off, harmless
in practice but worth knowing the real number now has one source of truth)
of real content height (320px panel minus status bar/padding) — not
pixel-verified on hardware. Every field comes from the same plain-C getters
the web HTTP handlers use (10.1a).

The four pages this section added to the plan 2026-08-18 (Safety/Alarm,
Diagnostics, Thermocouple Faults, Backup/restore) are tracked in section 0.5,
not duplicated here.

**LCD scope narrowed 2026-08-22 — the LCD sheds configuration to the web
GUI.** Owner: the LCD should show live state; settings belong on the web
pages. Removed outright (files, hub cells, routes and CMake entries):
`ui_page_zones.c` (Zones & Thermocouples — a settings editor), the
"Relays & Rules" hub cell (only ever a non-clickable "not built yet"
placeholder), and `ui_page_history.c` (Temperature History — duplicated the
home chart). The config hub repacked 3 pages → 2 with no empty slots.

Live zone temperatures and relay status were never on the deleted zones page
— they are on `ui_page_temperature.c`, which is now the LCD's zones/relay
view and stays.

Home page: the run-state card, progress bar and bottom spacer between the
chart and the Start button are gone; the chart takes `flex_grow(1)` and
expands into that space, with Start still the last child pinned to the
bottom. The live safety-trip callout the removed card carried is back as a
strip that is HIDDEN unless a trip is live (LVGL skips hidden flex children,
so it costs zero height when clear) — a layout request must not make a trip
invisible on the page the operator watches.

- [ ] **LCD home layout not verified on hardware.** Chart filling to the
      Start button, the hidden trip strip, and no-scroll at 480x320 were all
      built after the board was disconnected. Needs a look on the bench.
- [x] **Owner reports some LCD back buttons don't work** -- CLOSED
      2026-10-07: tested on 2663925c, all back buttons pass (config,
      temperature, diagnostics, network, network_manage, profiles,
      profile_detail, profile_segments, profile_builder_zones); the first
      tap after screen blank is a wake swallow (`touch_get_state`
      `last_swallow_reason=wake`), which is the likely source of the report.
      One wake tap still navigated: a touch-controller dropout mid-touch
      read as a release, so the next sample was a "new" press on the
      now-awake screen; fixed by the release debounce in
      `display_power_touch_gate_*` (display_power_policy.c). Still
      unverified on hardware: profile_builder_segment, profile_builder_review,
      live_decide, edit_firing back buttons.

- [x] **`UART_TASK_ID_WIFI` (11) sometimes doesn't register at boot** — DONE
      2026-09-07. Root-caused, not a race and not Wi-Fi-provisioning
      ordering: `uart_protocol_register_task()`
      (`firmware/hwAbstraction/esp/uart/uart_protocol.c`) already retries the
      PSRAM-backed inbox `xQueueCreate` 5x and, on final failure, already
      logged the task id and reason -- but at `ESP_LOGW`, not `ESP_LOGE`.
      `uart_log_bridge.c`'s boot-burst queue only protects `ESP_LOGE` lines
      from eviction when full (see its own HAZARD comment, which already
      named this exact call site); a WARN during the boot burst this failure
      mode occurs in is silently dropped with no eviction, indistinguishable
      from never having logged at all -- exactly the "no corresponding
      failure logged" symptom, even though the producer code did call
      `ESP_LOG*`. Live board check: `wifi_get_status()` answers
      normally, so task 11 is registered on this boot -- confirms
      "intermittent", not "always broken". Fix (`20c2a5d5`): promoted that one
      log call to `ESP_LOGE`, so a real registration failure for any task id
      now survives the same boot burst that used to eat it. Widened further
      (`b12faf41`, `cb6f3cd5`): eviction now protects WARN as well as ERROR,
      and prioritizes ERROR over WARN when both compete for the same slot.
      Host test:
      `test_registration_failure_log_only_survives_as_error()` in
      `App/test/test_uart_log_bridge.c` reproduces the exact
      pre-fix/post-fix line against a full boot-burst queue via the real
      `uart_log_bridge.c` eviction logic and proves W is silently dropped
      while E survives.
- [x] **`ui_page_temperature.c`'s no-scroll fit depends on relay count per
      zone at runtime** — DONE 2026-08-24. The real budget is
      `ui_theme.h`'s new `UI_THEME_PAGE_CONTENT_BUDGET_PX` (268px, computed
      from real constants — every prior "~264px" comment across this
      codebase was off by 4px, harmless in practice but now a single
      source of truth). Re-deriving this page's worst case turned up a
      LARGER, previously-unnoticed overflow than the relay-count risk this
      item named: `MAX31856_CHANNEL_COUNT` (3) zone cards at the old fixed
      relay-row height (80px, 2 button rows) summed to ~362-380px, ~100px+
      over budget, independent of relay count entirely — the 2026-08-19
      "relay-count-bound" pass had bounded each card's own height but
      nobody had summed all the cards against the real page budget. Fixed
      by shrinking `UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX` to one button
      row (40px, still internally scrollable for a zone with more relays
      than fit in one row) and adding a `_Static_assert` bounding
      `MAX31856_CHANNEL_COUNT` cards + the status label against the real
      budget. Relays-per-zone itself is now `UI_PAGE_TEMPERATURE_MAX_
      RELAYS_PER_ZONE` (`== KILN_IO_RELAY_COUNT`, the true structural
      ceiling — a zone can't claim more relays than the board has),
      cross-checked by its own `_Static_assert`, with a RUNTIME clamp+log
      in `build_zone_row()` for a corrupted/out-of-range `relay_mask`
      (persisted config data no static assert can see the value of).
      `firmware/KilnFW/App/test/check_ui_budget_asserts.ps1` is the lint
      half — fails loudly if any of these `_Static_assert`s is ever
      deleted. Still NOT hardware-confirmed (no ILI9488 panel in this
      environment) — arithmetic against real constants, not a pixel-verified
      fit; the one-row relay display (vs. the old 2-row layout) is also an
      unverified real UX narrowing for a zone with several relays.
- [x] **`ui_page_network.c`'s worst-case fit (~268px against a ~264px
      budget) is computed, not hardware-confirmed** — DONE 2026-08-24, via
      the exact remedy this item named: Scan/Saved/Connect/Forget split into
      their own page, `ui_page_network_manage.c`, reachable from a "Manage
      networks" button via `kiln_ui_show("network_manage")` (same pattern
      Safety Processor/Temperature History use). Re-deriving the real
      numbers first (not trusting the old ~268px estimate) found the true
      worst case was worse than documented: the "Change network"/"Show QR"
      button being visible at the SAME TIME as the list block once
      connected was never summed into any prior pass's arithmetic. A
      SECOND, entirely undocumented ~34px overflow was also found in the
      AP-identity section (`s_ap_section`) while deriving this — nothing in
      TODO.md or this file's history ever named it, since every earlier
      budget pass here was chasing the Scan/Saved list. Both are fixed:
      `ui_page_network.c` now carries two `_Static_assert`s (STA-connected
      state, AP-mode state — the two real mutually-exclusive states
      `content` can show) and `ui_page_network_manage.c` carries its own,
      all three checked by `ui_theme.h`'s new `UI_THEME_PAGE_CONTENT_
      BUDGET_PX` and enforced by `check_ui_budget_asserts.ps1`. Still NOT
      hardware-confirmed.

### 10.4 Touch hit-testing

DONE — confirmed by reading LVGL's source rather than assuming (`lv_indev_
search_obj()` does first-match-in-z-order rectangle containment, not
nearest-center). `ui_theme_apply_touch_area()` (dynamic per-widget extended
click area, sparse vs. compact-layout cases) and `ui_theme_register_touch_
group()`/`ui_theme_resolve_touch_target()` (opt-in nearest-center arbitration
for a registered cluster, built ahead of a real consumer per explicit
request) are both built. See `docs/ARCHITECTURE_DECISIONS.md`. No dense grid
page exists yet to actually register a touch group, but the mechanism is in
place and costs nothing when unused.

### 10.5 Web/LCD parity rule

Standing rule (not a task): whenever either the LCD screen or the web interface
changes, consider whether the other should change too. If the answer isn't clear,
ask the user rather than guessing; if it's clear-cut (e.g. a new zone field needs
to show up in both places), make the matching change without asking. Applies in
both directions -- a web feature added later needs the same consideration for the
LCD, not just LCD to web.

### 10.6 Web dashboard restyle to match the LCD

DONE — all seven web pages (`main_page.html`, `zones_page.html`,
`rules_page.html`, `profiles_page.html`, `wifi_provision_page.html`,
`readiness_page.html`, and later `ota_page.html`) restyled to the same
palette as `docs/UI_THEME.md`, applied only to each page's existing dark-mode
CSS variant (light mode untouched) via a duplicated `<style>` block per page
— a shared `theme.css` route was considered and rejected as riskier than
warranted for a visual-only change, since no single file registers all seven
pages' handlers. No `.c`/`.h`, element ID, or JS behavior changed.

- [ ] **Not visually verified against real hardware/browser** — a phone and
      the LCD's own browser should confirm the restyle once a build is
      possible in this environment.

### 10.6a Gzip the embedded web pages

DONE — the six original HTML pages plus `theme.css` are gzipped at CMake
configure time (Python `gzip` module, not a Unix binary — avoids the Windows
dev-machine wrinkle) and embedded pre-compressed; `zones_page.html` is
deliberately excluded and stays raw (confirmed safe: its handler sends no
`Content-Encoding` header). Content negotiation (`App/drivers/web_encoding.
{c,h}`) now serves gzip whenever a client doesn't explicitly exclude it (per
RFC 9110 — a request with no `Accept-Encoding` header legally accepts any
coding) and returns 406 with an uncompressed body only when a client
explicitly excludes gzip (`identity`, `q=0` forms). The "compile CSS/JS to
native code for the LCD" half of the suggestion that prompted this was
evaluated and **rejected** — LVGL stays the LCD rendering backend.

- [x] **Re-verify byte-for-byte that nothing about the page *content*
      changed** — this was a transport-encoding-only change; verify by
      comparison rather than trust. DONE, see `tools/check_web_gzip_parity.py`
      (standing check, decompresses each embedded `.gz` and byte-compares it
      against its source; SKIPs when the KilnFW build hasn't produced the
      `.gz` files yet).

### 10.7 Onboard IC temperature sensors

DONE — `App/drivers/board_temps.{c,h}` surfaces the ESP32-S3's internal die
temperature and every MAX31856's cold-junction reading. `GET /api/board_temps`
(web) and `ui_page_board_health.c` (LCD, reached from the Configuration hub)
both read the same `board_temps_get_live()` getter (10.1a's shared-backend
rule) — deliberately a separate page/route from the main dashboard, since
this is board-health diagnostic data, not kiln-process data.

### 10.8 Multi-thermocouple-per-zone (cross-reference: section 3)

DONE — a zone can have more than one thermocouple assigned
(`zone_cfg_t::thermo_mask`, `ZONES_CFG_VERSION` 3->4, migration fills the
implicit pre-10.8 one-channel-per-zone mapping so no existing zone silently
goes invalid). Combining function is **arithmetic mean of valid readings**
(a deliberate decision — outlier rejection / max-biased combining were
considered and left explicitly undecided, not guessed at) in the new pure,
host-tested `App/drivers/thermo_combine.{c,h}`. Both `profile_executor.c`
(control tick + run-start baseline) and `autotune_engine.c`'s step-test read
path consume the combined value; `thermal_guard.c`'s guard 6 extends to
"all assigned thermocouples invalid" for free, since it was already written
against an opaque `sensor_ok` bool.

### 10.9 LCD network settings page + QR codes for AP/site connect

DONE — `ui_page_network.c`/`.h` mirrors the web provisioning page (mode
readout, scan with the AP-mode "disabled" message, saved-network list with
forget, AP identity display, an on-screen-keyboard connect flow via
`lv_keyboard`), all through the same `wifi_prov.h` getters/setters the web
handlers use. QR codes ship on both surfaces: an AP-join QR (`ui_page_
network.c` and, compact, `ui_page_home.c` while in AP mode) and a
dashboard-URL QR (`kiln.local` preferred, raw IP fallback) once connected,
using LVGL's `lv_qrcode` on the LCD and an embedded client-side JS QR
library (no CDN) on the web.

**Recurring hazard found twice this section: `sdkconfig` vs. `sdkconfig.
defaults`.** `sdkconfig` is gitignored/machine-local; several settings this
firmware depends on (`LV_USE_QRCODE`, `LWIP_MAX_SOCKETS=16`, the `SPIRAM_*`
block, `PARTITION_TABLE_CUSTOM`/`ESPTOOLPY_FLASHSIZE_16MB`) were only ever
set in the local file, so a fresh checkout would silently fail to build or
build a non-working image. All now pinned in the committed
`sdkconfig.defaults` with comments explaining the dependency and failure
mode, each verified by deleting `sdkconfig` and reconfiguring from
`sdkconfig.defaults` alone. Worth checking this class of gap again after any
future menuconfig change.

### 10.10 Safety processor GUI panel (ROADMAP.md M6)

DONE — safety-processor thermocouple, enclosure (cold-junction) temperature,
and power (Frame E, `kilnlink_power.{c,h}`, host-tested) are all plumbed
into `dashboard_status_t`/`GET /api/status` and a "Safety Processor" card on
`ui_page_home.c`, per `LINK_PROTOCOL.md` sec 7's explicit placement (this is
kiln-process data an operator watches while firing, unlike 10.7's
board-health diagnostics). SaftyFW now sends Frame E too, but guards S3/S4
are deliberately not wired to it yet — needs the per-channel CT-mapping
commissioning check on real hardware first (`SaftyFW/docs/CURRENT_SENSE.md`
sec 5).

- [ ] **Not verified against the live Pico's Frame A/Frame E fields.** The
      M0 half is CLOSED 2026-10-07: the isolated link has passed bytes
      end-to-end since 2026-08-23 (`ROADMAP.md` M0) and the bench board has a Pico
      attached. What remains is confirming on hardware that each field on this
      page shows the live value rather than `null`/"---".

### 10.11 Liveness: 1.5 s fault, 30 s firing-abort (ROADMAP.md M6)

DONE — `LINK_PROTOCOL.md` sec 8's two-timeout rule. `SAFETY_LINK_STALE_MS`
(1500) is a fixed ceiling, decoupled from the configurable poll period (a
reconfigured period can only make the fault fire sooner, never later). 30s
of continued silence while a firing is running/paused aborts it via the
existing guard-9 watchdog path (relays dropped and retried, `run_state`
records the end) rather than a new mechanism.

- [ ] **Not hardware-timing-verified.** No ESP32-S3/Pico is attached — whether
      the fault really asserts at 1.5s and the abort at 30s on real hardware
      is unverified; this closes the code gap, not the timing-verified gap.
      (Host-test part CLOSED 2026-10-07: `safety_link_is_stale()` is covered by
      `App/test/test_safety_link.c` -- threshold boundary at 1500/1501/1499 ms --
      and `App/test/test_safety_watchdog.c`.)

### 10.12 ESP → Pico context broadcast, `SAFETY_CMD_PUSH_CONTEXT` (ROADMAP.md M5)

DONE — `LINK_PROTOCOL.md` sec 4's 0x07 frame (relay now/recent masks,
per-zone measured temp/fault/type/setpoint/active/relay-on/guard-tripped,
top-level profile-running/any-zone-faulted/heat-requested flags) is built
from real board state and broadcast every poll period via
`safety_build_and_send_context()`.

- [ ] **Not verified against a real Pico.** No RP2040 is attached — the frame
      is built/broadcast and matches the codec's own contract, but nothing
      confirms a real SaftyFW build decodes it correctly. SaftyFW's receive
      side is tracked separately in `firmware/SaftyFW/TODO.md`.

### 10.13 DIAG / TRIP_EVENT decode + dispatch (ROADMAP.md M5)

DONE — `LINK_PROTOCOL.md` sec 6's Frame B (`SAFETY_CMD_DIAG`, 0x08) and Frame
D (`SAFETY_CMD_TRIP_EVENT`, 0x0D) are decoded and cached in `safety_link.c`
(`safety_apply_diag()`/`safety_apply_trip_event()`, `TRIP_EVENT` deduped on
`trip_seq`), passed through to `dashboard_status_t`/`GET /api/status`
(present only once a frame has ever arrived), and a "Last trip" row on
`ui_page_safety.c` (the only field that fit its no-scroll budget; DIAG's
warn/trip masks are cached and HTTP-exposed but deliberately not added to
this page — a future dedicated diagnostics page is the better home). Also
mirrored onto the PC-link SAFETY UART task as two new query subcommands
(`SAFETY_CMD_GET_DIAG` 0x0C, `SAFETY_CMD_GET_TRIP_EVENT` 0x15), answered
purely from the cache. This closes "the ESP can receive and decode these
frames" — SaftyFW does not send either frame yet (tracked in
`firmware/SaftyFW/TODO.md`).

- [ ] **Not verified on the wire.** The M0 half is CLOSED 2026-10-07 (link
      passes bytes end-to-end since 2026-08-23, `ROADMAP.md` M0). Still open:
      `safety_apply_diag()`/`safety_apply_trip_event()` decoding a real Frame B/D
      from a SaftyFW build that sends them (not re-checked in this pass).
- [x] **`pc_tools`/MCP client-side decode of the two new GET_DIAG/
      GET_TRIP_EVENT subcommands is not built** — DONE 2026-09-22:
      `tools/PcTools/src/kilnctrl/devices_safety.py`'s `parse_safety_response()`
      decodes both replies into `SafetyDiag`/`SafetyTripEvent` dataclasses
      (commits `0d4ca0b6`, `df80c49f`, `f59b21c8`, `661d229f`).

### 10.14 Command queue between every control surface and the tasks that own state

Full task inventory, ownership doctrine, and honest verification status:
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Design rationale and the
build-order table: [`docs/ARCHITECTURE_DECISIONS.md`](docs/ARCHITECTURE_
DECISIONS.md). Filed from a real bench bug (2026-08-19): `ui_page_network.c`
froze the *entire* LCD display whenever Scan/mode-switch/connect ran a
blocking `wifi_prov_*()` call from the one task allowed to touch LVGL.

**Pattern** (copied from `firmware/SaftyFW/src/tasks/relay_owner.c`, already
proven on the RP2040 side): one owning FreeRTOS task per state-owning
domain, a small bounded queue, `<owner>_command_<verb>()` producer functions
(fire-and-forget or request/response), owner drains with a bounded timeout.

**Built**: `kiln_io_owner.c`/`.h` (Phase 1 — closes a real lost-update race:
UART/HTTP/LCD/profile-executor/autotune all wrote relay state with zero
coordination and two independently-duplicated safety-gate checks);
`thermo_owner.c`/`.h` (Phase 2 — architectural consistency, not a bug fix,
since `MAX31856.c`'s own per-channel lock already prevented a lost update;
lays the chokepoint Phase 6 will need); `wifi_prov.c`'s own owning task
(Phase 4 — the largest real bug in this section, `s_wifi` had zero locking
across four independent writers including the Wi-Fi driver's own event
callbacks; every `wifi_prov_*()` signature stayed unchanged so no caller
needed editing); and the LCD-side stopgap (`ui_page_network.c`'s three
worker-task-per-action job structs, now posting through the owners instead
of touching state directly).

**Phase 3 (`profile_executor` command queue) was reviewed and deliberately
skipped, not deferred** — all four run/pause/halt/resume entry points
already wrap their bodies in one correct mutex with no lost-update bug to
fix; converting a safety-critical state machine to a drop-on-full-queue path
would be pure regression risk for zero safety gain. Revisit only if Phase
6's system-mode gate genuinely needs a choke point the lock can't provide.

**Verification**: build-clean under `-Werror` throughout. Phase 4 reached a
partial hardware data point (flashed, boots, stays running) but the PC-link
UART in this environment is physically broken, so no Wi-Fi behavior itself
(AP fallback, rescan, a real join, concurrent-command contention) has been
observed. Phases 1/2 are build-verified only — no board, and separately no
thermocouple daughterboard, to observe the owned hardware paths working.
Two real THERMO protocol bugs (fabricated fault data, a silently-dropped
query reply) were found and fixed while bringing Phase 2 up on a real bench
— see `docs/BRINGUP_HAZARDS.md`. A bench stack-overflow crash-loop, initially
suspected to be related to this work, was root-caused separately (also in
`docs/BRINGUP_HAZARDS.md`) and was not caused by anything in this section.

- [x] **Web side**: `dashboard_http.c`/`zones_http.c`/`profiles_http.c`/
      `rules_http.c`/`ota_http.c`/`wifi_provision_http.c` action-taking POST
      handlers should post commands instead of running inline on
      `esp_http_server`'s one shared worker task. `ota_http.c`'s transfer
      handlers and `provision_post_handler()` are the highest-value targets
      (longest/most blocking). OTA's transfer handlers are explicitly
      out-of-scope for migration — they legitimately need to hold a
      streaming HTTP body open across the whole transfer.
      **Done 2026-10-02** (planned 2026-09-25, owner approved): slices W1/A1-A4 in
      `docs/HTTP_POST_OWNER_MIGRATION.md`; `rules_http.c` no longer
      exists, and most dashboard/zones/profiles POSTs are short writes, not worth moving.
- [x] **Debug/PC-link UART side** (explicit user request): closed
      2026-09-24 -- TOUCH and UI_TEST (`LIST_TAP_TARGETS`/`CLICK_BY_NAME`)
      both route their LVGL tree walk through `lvgl_port_collect_tap_targets()`
      on `lvgl_port_task`; SYSTEM/INFO need no owner (no shared state) and
      WIFI/SAFETY already route through `wifi_prov.c`/`safety_link.c`. DISPLAY
      was dead code, deleted 2026-08-27. See
      `docs/UART_BRIDGE_SUBSYSTEM_OWNER_SURVEY_2026-09-24.md`.
- [x] **Phase 5: HTTP handler migration**, per-domain, alongside whichever
      owner (1/2/4) each handler calls into — not a separate final phase.
      Closed 2026-09-22: `docs/HTTP_HANDLER_OWNERSHIP.md`'s audit found
      four remaining direct-driver-read call sites in `dashboard_http.c` (x2)
      and `ota_http.c` (x2) — `MAX31856_read_all()`/`kiln_io_read()` used
      directly instead of `thermo_owner_command_read_all()`/
      `kiln_io_owner_command_read()` — plus one comment-only false positive
      in `diagnostics_http.c`. All four routed through their owner accessor
      in the same batch; `wifi_provision_http.c` (Phase 4) was already clean.
      `tools/check_no_handler_direct_driver_calls.ps1` now enforces this
      mechanically (zero-entry allowlist for MAX31856/kiln_io, one narrow
      documented exception for `factory_reset.c`'s driver-storage-reset
      `esp_wifi_restore()`/`esp_wifi_set_storage()` calls).
- [x] **Phase 6** (added mid-Phase-1, user request): a **system-mode command
      gate**, distinct from the owner-task pattern above. The owners answer
      "can two writers race on this state"; this answers "is this *class* of
      command allowed at all given what the system is doing right now" —
      e.g. while a profile is firing, stop/pause/modify-this-run is fine, but
      starting a *different* profile, running autotune, or a raw GPIO/SX1509
      debug write should be refused outright. **All five rollout slices
      LANDED** (`docs/SYSTEM_MODE_GATE.md` section 3.6, owner decisions
      2026-09-25): `system_mode_gate.h`/`.c` (slice 1); manual relay writes
      via `kiln_io_owner.c`'s `relay_on_blocked()` (slice 3); recovery-mode
      start refusals shared across HTTP/UART/LCD, retiring the HTTP-only
      `recovery_start_refusal.h` (slice 2, 2026-09-27); zones/config writes
      (slice 4); and factory reset/cfgfs format (slice 5) — each wired into
      the callers' existing single choke points ahead of
      `readiness_gate.h`/the owner locks, never replacing them.
#### Design notes / not planned (2026-10-07)

Not scheduled, kept for context; reopen only if a real freeze or race is observed.

- `profile_executor.c`/`relay_authority.c` gaining the same
  `relay_owner.c`-style queue is a candidate once the web/LCD callers
  that drive them are migrated -- not urgent (current call pattern
  hasn't been observed to freeze anything).
- `ui_page_network.c`'s three job structs are candidates to migrate onto
  `wifi_prov`'s real owner queue once a shared async shape exists,
  rather than staying page-local one-offs.

## 11. PC-link command acknowledgement (moved from ROADMAP.md 2026-08-24)

Scoped to the **PC↔ESP link only** (`uart_protocol.c`, `uart_bridge.c`,
`uart_bridge_ext.c`, `tools/PcTools/src/kilnctrl`) — the ESP↔Pico safety
link's no-ACK doctrine (`CommonFW/docs/LINK_PROTOCOL.md` §1–2) is deliberate
and separate; do not "fix" that side.

**The rule to not reintroduce**: `uart_protocol.c`'s RX task ACKs a `DATA`
frame the instant it lands in the destination bridge task's inbox, *before*
that task's `switch (subcmd)` ever runs. The transport ACK proves delivery,
never that the command did anything — a handler that falls through with no
reply makes "queued" and "executed" indistinguishable to a host that only
checks the transport ACK. Every new mutating subcommand handler on this hop
must answer through `bridge_reply_reject()` (`uart_bridge.c`) or
`bx_reply_ok_err()` (`uart_bridge_ext.c`) — same `{subcmd, ok, [len,
reason]}` shape both already use — on every path that refuses or fails, not
just the happy path.

**Firmware half done (commit `5df2190`, 2026-08-24)**: extended that reply
convention to every rejection that used to fall through silently —
truncated args and out-of-range args across THERMO/IO/SAFETY
(`uart_bridge.c`), and, worst of the set, `IO_CMD_SET_RELAY`/
`SET_RELAY_MASK`'s `KILN_IO_OWNER_RELAY_ERR_OWNED`/`ERR_SAFETY`/
`ERR_UPDATING` refusals, which used to be indistinguishable on the wire from
a relay that actually switched. Purely additive — no protocol version bump
(`UART_PROTOCOL_VERSION` stayed at 6 on both sides).

**PC side, done this pass**:
- `devices.py`: shared `_decode_ok_reason()`/`OkReason` decoder for the
  `{subcmd, ok, [reason]}` shape now used by every parser below it.
  `parse_control_response()`'s `SET_ZONE_PID`/`SET_ZONE_MODEL`/
  `SET_UNIT_PREF` case used to decode the reply's reason string and then
  throw it away (`return subcommand, bool(payload[1])`); it now returns an
  `OkReason` (still truthy/falsy like the old bare bool, so existing
  `if not result:` callers keep working) carrying the text.
- `devices.py`: `RelayRefusal` (enum: `TRUNCATED`/`OUT_OF_RANGE`/`OWNED`/
  `SAFETY`/`UPDATING`/`DRIVER_ERROR`/`OTHER`) and `RelayResult` classify
  `IO_CMD_SET_RELAY`/`SET_RELAY_MASK` refusals so a caller can branch on
  "profile owns it" vs. "safety fault" vs. "OTA in progress" instead of a
  flat ok=0. Classified from the firmware's free-text reason strings rather
  than a new numeric wire code, since the firmware side already shipped
  text and there's no version bump backing a format change.
- `io_expander.py`: `IoClient.set_relay()`/`set_relay_mask()` replace the
  old fire-and-forget `send()` path for these two commands. `io_bridge_task`
  replies nothing on success and `{subcmd, 0, reason}` on refusal, so these
  send and wait out a short window (`SET_RELAY_REJECT_WINDOW_S`, 0.5 s) for
  the *optional* reply: a reply within the window is decoded as the
  refusal; silence means the write went through. `send()` itself is
  unchanged (still used by every other IO write) and its docstring now
  says why relay writes shouldn't go through it.
- Wired through to callers: `mcp_server.io_set_relay`/`io_set_relay_mask`
  and `mcp_server.control_set_zone_pid`/`control_set_zone_model` surface the
  reason text; `actions.py`'s "IO: Set Relay"/"IO: Set Relay Mask" registry
  entries (the `press_button` MCP tool's backing) go through
  `ctx.io.set_relay()`/`set_relay_mask()` instead of raw `_send()`; `gui.py`
  shows the refusal reason in the status bar and puts a rejected relay
  checkbox back to the board's actual state instead of leaving it showing
  the requested-but-refused value; the Zones popup's PID/model status text
  now includes the reason on rejection.
- Tests: `tools/PcTools/tests/test_bridge_reject_reply.py` — byte-exact
  vectors for the CONTROL reason-discard bug and for each of the six IO
  relay refusal strings, including a check that two different refusal
  reasons (`owned` vs. `safety`) never classify the same. Proved able to
  fail: reverting both fixes turned 10 relay tests into `IoResponseError:
  unknown IO response subcommand 0x01` errors and 1 CONTROL test into an
  `AssertionError` (bare `bool` where an `OkReason` was expected).

**THERMO/SAFETY host-side decode closed this pass (2026-08-24)**: the claim
above ("nothing on the PC side waits for or decodes") held for both.
- `devices.parse_thermo_response()` gained a case for
  `CONFIG_CHANNEL`/`SET_THRESHOLDS`/`SET_CJ_OFFSET`/`ONE_SHOT`/
  `CLEAR_FAULTS`/`SET_AUTO_REPORT`/`WRITE_REG` (all of `thermo_bridge_task`'s
  mutating subcommands, not just the five originally named) via the shared
  `_decode_ok_reason()`; before this, any of those frames raised
  `ThermoResponseError("unknown THERMO response subcommand...")`.
  `devices.parse_safety_response()` got the matching case for
  `REQUEST_ENABLE`/`SET_POLL_PERIOD`/`SET_FAULT_OUT`/`SET_CT_CAL`/
  `SET_CONFIG` (the last wasn't in the original list but has the identical
  shape and gap).
- `thermo.py`/`safety.py`: new `config_channel()`/`set_thresholds()`/
  `set_cj_offset()`/`one_shot()`/`clear_faults()`/`set_auto_report()`/
  `write_reg()` on `ThermoClient`, and `request_enable()`/
  `set_poll_period()`/`set_fault_out()`/`set_config()`/`set_ct_cal()` on
  `SafetyClient`, all following `io_expander.py`'s `set_relay()` shape
  exactly: send, then wait a short window (`MUTATING_REJECT_WINDOW_S`) for
  the *optional* refusal reply — silence means it went through. `send()`'s
  docstring on both clients now points here instead of staying silent about
  the risk. Wired through `mcp_server.py`'s matching tools and `gui.py`'s
  Thermo/Safety pages (new `_thermo_mutating_async()`/
  `_safety_mutating_async()` helpers) — both previously called the generic
  fire-and-forget `_send()`/`send_async()`, which doesn't wait for a reply
  frame at all; a refusal would land in the client's own consumer thread
  with nothing pending and get logged at debug level as "ignoring
  unsolicited response", invisible to the caller either way.
- **`display.py`'s share of the original claim was overstated.**
  `display_bridge_task` (`uart_bridge.c`) never received *any* of 5df2190's
  reply treatment — not truncated, not out-of-range, not driver-error; its
  `bridge_args_ok()` failures just set `rejected = true` and `continue` with
  no reply at all, unlike THERMO/SAFETY where the same check is followed by
  an explicit `bridge_reply_reject()` call. So there is currently no
  refusal frame for `parse_display_response()`/`display.py` to decode —
  fixing the host side first would have nothing to consume. This is a wider
  gap than the item below (which only names DISPLAY's *driver-error* path);
  DISPLAY has no reply on *any* rejection path yet.
- Tests: `tools/PcTools/tests/test_bridge_reject_reply.py` gained
  `ThermoMutatingReasonTests`/`SafetyMutatingReasonTests` (byte-exact,
  mirroring the existing `ControlSetZonePidReasonTests` idiom) plus
  `ProfilesLifecycleReasonTests` for the item below. Negative-tested by
  reverting each of the three `devices.py` decode blocks in turn and
  re-running: all three produced real failures (six THERMO tests raising
  `ThermoResponseError: unknown THERMO response subcommand 0x0A`, five
  SAFETY tests likewise on `0x05`, six PROFILES tests either missing the
  `OkReason` type or losing `.reason` to a bare `bool`), then were restored
  and the suite re-confirmed green. `test_safety_ct_cal.py`/
  `test_safety_set_config.py` had two pre-existing tests that patched
  `mcp_server._send` directly; updated to patch the new
  `SafetyClient.set_ct_cal()`/`set_config()` methods instead, since those
  tools no longer go through `_send` at all.

**`devices.parse_profiles_response()`'s reason-discard bug closed this
pass**: `PROFILES_CMD_DELETE`/`PAUSE`/`RESUME`/`ACK_LAST_RUN`/`STOP` now
return `OkReason` via `_decode_ok_reason()` instead of
`bool(payload[1])` — same bug class as the CONTROL fix, e.g. DELETE's
"cannot delete a builtin profile" refusal no longer vanishes.
`profiles.py`'s `delete()`/`stop()`/`pause()`/`resume()`/`ack_last_run()`
return `OkReason` now (still truthy/falsy-compatible); `mcp_server.py`'s and
`gui.py`'s matching call sites surface `.reason` on refusal instead of a
bare "REJECTED".

**SAFETY / DISPLAY / TOUCH's bottom "driver call failed" path closed
(commit `a458a8f`, 2026-08-24, ahead of this section catching up)**:
`SAFETY_CMD_GET_CT_CAL`'s request/reply id-sharing blocker is gone —
`GET_CT_CAL` moved to its own request id (0x22, `KILNLINK_PROTOCOL_VERSION`
7), leaving the reply's old id (0x1A, `SAFETY_CMD_CT_CAL`) exclusively for
the 28-byte success shape. A driver-error refusal now goes out under 0x22
and can never again be misread as a truncated/malformed CT_CAL reply.
`GET_PARAM`/`GET_CONFIG_PAGE` had the identical shape and moved under the
same bump (0x23/0x24). All three bridge tasks' bottom `err != ESP_OK` block
now calls `bridge_reply_reject(..., "driver error")` — verified by reading
the current file, not assumed from an old commit message.

**FIRST, THE CAVEAT THAT OUTRANKS EVERYTHING BELOW: `display_bridge_task`
is dead code.** `main.c` never calls `uart_bridge_start_display_task()` —
LVGL owns the panel exclusively now, and the comment at `main.c:1392` says
so explicitly ("no longer called here; it stays in uart_bridge.c as dead
code for now"). This TODO already recorded the consequence at section 10.1
("`tools/PcTools`' 12+ MCP `display_*` tools are stale ... every invocation
silently fails"), and it was confirmed on the bench 2026-08-24 after
flashing `750dc33`:

```
W uart_proto: uart0: dst task 4 not registered, replying NACK (undeliverable)
```

i.e. every DISPLAY frame is refused by the *transport* before any bridge
handler exists to reach. So the DISPLAY half of the work described below is
**correct but unreachable in shipped firmware** — it matters only if section
10.1 is ever resolved as option (b), "restore a minimal firmware handler".
It was not wasted (the file is now internally consistent, and a revived task
would be right from the first boot), but nobody should read the entry below
as having changed observable behaviour on the DISPLAY group. **TOUCH is a
different matter and is live** — `uart_bridge_start_touch_task()` *is*
called (`main.c:1426`, gated on `screen_idle_ready`), verified on the same
boot by a successful `touch_set_tap_dump` round trip.

Both agents that worked this item missed the dead-code note, as did the
item's own framing. Check section 10.1 before spending further effort on
`display_bridge_task`.

**TOUCH's new reject path found a real defect while being exercised, and
that is the one part of this whole item verified against silicon.**
`TOUCH_CMD_INJECT` had no bounds check on x/y at all — only a truncation
check — so injecting (9999,9999) on a 320x480 panel returned
`ok - touch injected` *and* `TOUCH_CMD_GET_STATE`'s `injected_delivered`
counter went 0 → 107. The operator therefore had both a success reply and
positive delivery evidence for a touch that cannot hit any widget: LVGL
hit-tests nothing out there and silently discards it. Same defect class as
everything else in this section — an action that did nothing looking
identical to one that worked — and a coordinate typo is the likely real
cause. Now range-checked and answered with `"out of range"`.

The check is rotation-agnostic on purpose: frame memory is always 320x480
and rotation only swaps the axes, so a valid point satisfies
`(x<320 && y<480) || (x<480 && y<320)`. This task holds no display handle
and cannot ask which rotation is live, so the union is accepted — a point
valid only in the *other* rotation still passes. That is a deliberate
false-accept, not a missed bound; such a point is in-panel, merely rotated,
and plumbing rotation state into this task would buy nothing.

Proved on hardware, both directions, which no other part of this item can
claim:

```
W uart_bridge: touch: inject (9999,9999) is outside the 320x480 panel in either rotation -- rejected
```

host side `refused - touch injected (9999,9999,down): out of range`, while
(160,240) still returns `ok` and increments `injected_delivered`.

One trap worth recording: the first host-side attempt printed `ok` despite
the firmware log showing the refusal, because a **stale `kilnctrl` MCP
server process** was still holding pre-change Python. `close_server` and
retry gave the refusal. If a wire-level fix appears not to reach the host,
suspect that before suspecting the fix.

**DISPLAY / TOUCH's per-guard rejection paths closed this pass**: the
gap this section flagged as *wider* than the driver-error item above —
`display_bridge_task`'s and `touch_bridge_task`'s `bridge_args_ok()`/
`bridge_range_ok()` failures (truncated frame, out-of-range rotation/text
size, an odd-length BLIT_DATA chunk) set `rejected = true` and `continue`
with no reply at all, unlike THERMO/IO/SAFETY where the same guard is
followed by `bridge_reply_reject(..., "truncated"/"out of range")`. Every
such path in both tasks now replies the same way, matching THERMO/IO/SAFETY
exactly. No PC-side decoder yet consumes these DISPLAY/TOUCH guard
refusals (`parse_display_response()` only recognizes `READ_ID`; most other
DISPLAY/TOUCH subcommands have no PC-side reply decode at all) — this pass
was scoped to the firmware wire behavior per the assignment; wiring a PC
consumer is follow-up work, not a re-opened gap (a client that never reads
the reply is unaffected either way, same as every other addition
`bridge_reply_reject()` has made to this file).

**`THERMO_CMD_READ_FAULTS`/`IO_CMD_SX_SCAN` vs. an unsupported-subcommand
reject closed this pass**: both queries reply `{subcmd, count}` where
`count` can legitimately be 0 — an honest empty result — which used to be
byte-identical to `bridge_reply_unsupported()`'s `{subcmd, 0}` 2-byte
output for an unrecognized subcommand (this section's own flag, from
`5df2190`'s commit message). `bridge_reply_unsupported()` now passes a real
reason, `"unsupported"`, into `bridge_reply_reject()` instead of `NULL`, so
that reply is always longer than 2 bytes and can never again collide with
an empty-but-successful `READ_FAULTS`/`SX_SCAN` reply. Checked before
making this change, per the assignment: no `tools/PcTools` parser depends on
the literal 2-byte `{subcmd, 0}` shape (every `parse_*_response()` routes on
the subcmd byte alone and raises "unknown ... subcommand" regardless of
length for anything it doesn't recognize; only `_decode_ok_reason()` reads
the tail generically, and it already tolerates a variable-length reason).
Swept the rest of the file for the same collision class (any other
`{subcmd, count}` reply where `count` can be 0) — none found: `THERMO_CMD_
READ`'s count is always ≥1 (its `chan_mask` is rejected before this point if
it would select zero channels), and every other query in this file replies
a fixed size. `UART_PROTOCOL_VERSION` (7, hard-equality gated) makes this
safe: the shape change touches only the *unsupported* reply, never a real
subcommand's success shape, so no existing decoder for a known subcommand
is affected.

`tools/PcTools/tests/test_bridge_reject_reply.py`'s
`UnsupportedVsEmptySuccessCollisionTests` covers the *host decoder* side:
that the 2-byte shape decodes as a clean, wrong "empty success" while the
reasoned shape raises. Those tests build their own frames with a hardcoded
reason string, so **they cannot fail if the C side regresses** — flipping
`bridge_reply_unsupported()` back to a `NULL` reason leaves every one of them
green, because no Python test reads `uart_bridge.c`. An earlier revision of
this entry claimed those tests were the negative test for the fix; they are
not, and that claim was wrong in exactly the way this repo has been bitten by
three times before.

The C-side rule therefore gets a C-side guard:
**`tools/check_bridge_reject_reason.ps1`**, modelled on
`tools/check_uart_version_independence.ps1` (standalone, comment-stripping,
non-zero exit via `throw`). It fails if any `bridge_reply_reject()` call in
`uart_bridge.c` passes `NULL` or `""` as the reason, and it also refuses to
run blind: it throws if `bridge_reply_unsupported()` has vanished or been
renamed, or if fewer than 10 calls are recognised at all (which would mean
the call style changed — e.g. calls split across lines — and the check had
stopped seeing anything).

Proved able to fail, on the real file rather than on a simulation of it:
reverting `bridge_reply_unsupported()` to pass `NULL` produced

```
BRIDGE REJECT REASON CHECK FAILED:
  ...uart_bridge.c:272: bridge_reply_reject() called with a NULL/empty reason -- bridge_reply_reject(proto, msg, src_task, subcmd, NULL);
```

then, restored, `Bridge reject reason check passed: all 78
bridge_reply_reject() call(s) supply a reason.`

**PC-side consumer for DISPLAY/TOUCH's guard refusals, and the rest of
IO's write subcommands, wired this pass.** The entry above left "wiring a
PC consumer" as explicit follow-up, not a re-opened gap; this closes it,
plus a wider IO gap the same audit turned up that no earlier pass of this
section had named.

- `devices.parse_display_response()` used to recognize only
  `DISPLAY_CMD_READ_ID`; every other subcommand (`RESET`..`BLIT_END`) raised
  `"unknown DISPLAY response subcommand"` on any reply, so the refusal
  frames the firmware work above now sends for truncated/out-of-range/
  driver-error were dropped in `DisplayClient._handle_reply` with a debug
  log line, invisible to the caller. Every write subcommand's id now
  decodes via `_decode_ok_reason()` into an `OkReason` (`READ_ID` keeps its
  own 9-byte layout, unchanged — its case sets `err = ESP_OK`
  unconditionally, so it can never actually emit this shape).
  `parse_touch_response()` got the matching fix for `INJECT`/
  `SET_TAP_DUMP`/`LOG_TAP_TARGETS`, plus a decode (previously a raise) for
  `GET_STATE`'s own id-sharing refusal — `screen_idle_get_state()` can fail,
  unlike `DISPLAY_CMD_READ_ID`, so a non-{6,23}-byte `GET_STATE` reply is now
  an `OkReason` refusal instead of an unparseable frame the caller's query
  just times out waiting for.
- `display.py`: `DisplayClient` gained one write method per subcommand
  (`reset`/`set_power`/`set_rotation`/`set_invert`/`clear`/`fill_rect`/
  `draw_rect`/`draw_line`/`set_text_cursor`/`set_text_style`/`print_text`),
  all going through a new `_write()` following `io_expander.py`'s
  `set_relay()` shape exactly (send, then wait
  `DRIVER_ERROR_REJECT_WINDOW_S` = 0.5 s for the *optional* refusal reply).
  `blit()`'s `BLIT_BEGIN`/`BLIT_END` now go through `_write()` too;
  `BLIT_DATA` deliberately still uses the raw fire-and-forget `send()` — a
  full image is thousands of chunks, and a 0.5 s wait after each one would
  turn a ~1-minute transfer into ~20 minutes for a failure BLIT_BEGIN/
  BLIT_END already bookend. A mid-stream `BLIT_DATA` driver failure is
  therefore still not surfaced to the caller beyond the existing transport-
  ACK check — a known, documented gap, not a silent one.
  `touch.py`: `TouchClient.inject()`/`set_tap_dump()`/`log_tap_targets()`
  changed from returning a bare `SendResult` (fire-and-forget) to the same
  send/wait-window shape, returning `OkReason`; `get_state()` now raises
  `TouchQueryError` with the decoded reason on a `GET_STATE` refusal instead
  of returning it mistyped as a `TouchState`.
- Wired through to callers: `mcp_server.py`'s `display_*`/`touch_*` tools
  (previously every one of them called the generic fire-and-forget `_send()`)
  now go through the new client methods and surface `.reason` on refusal,
  via new `_display_mutating()`/`_touch_mutating()` helpers matching
  `thermo_config_channel()`'s established formatting. `gui.py`'s Display
  page (Reset/Power/Rotation/Invert/Clear/Fill Rect/Draw Rect/Draw Line/
  Print/Set Cursor/Apply Style buttons) moved from `send_async` (fire-and-
  forget) to a new `_display_mutating_async()` helper mirroring
  `_thermo_mutating_async()`'s status-bar convention exactly. `gui.py` has
  no Touch page, so there was nothing to rewire there.
- **A wider gap in the same family, found while auditing IO for this
  pass**: `parse_io_response()` only ever decoded `SET_RELAY`/
  `SET_RELAY_MASK` (into `RelayResult`) plus the three queries (`READ`/
  `SX_READ_REG`/`SX_SCAN`) — every *other* IO write subcommand
  (`SET_IO`, `SET_IO_DIR`, `SET_AUTO_REPORT`, `ALL_RELAYS_OFF`,
  `SX_WRITE_REG`, `SX_SET_DIR`, `SX_SET_PULLUP`, `SX_SET_OPENDRAIN`,
  `SX_SET_DEBOUNCE`, `SX_SET_INT_MASK`, `SX_LED_DRIVER`, `SX_RESET`) has
  had truncated/out-of-range/`"safety"` (the relay-pin guard `SX_WRITE_REG`/
  `SX_SET_DIR` share with the relay commands) and driver-error replies in
  `uart_bridge.c` since the first pass of this section (2026-08-24,
  `5df2190`) — that pass's own description ("extended that reply convention
  to every rejection... across THERMO/IO/SAFETY") already covered them, but
  nothing in `tools/PcTools` ever followed up on the plain-IO half of it.
  Every write besides the two relay commands went through `IoClient.send()`
  or `mcp_server._send()`, both fire-and-forget, straight through
  `gui.py`'s IO/Expander pages too. Closed the same way: `parse_io_response()`
  decodes all twelve into `OkReason` via `_decode_ok_reason()`;
  `IoClient._set_relay_style()` generalized into `_write_style()` (relay
  callers pass a `RelayResult`-shaped silence fallback, the twelve new
  methods default to `OkReason(ok=True)`); one wrapper method per
  subcommand; `mcp_server.py`'s `io_all_relays_off`/`io_set_output`/
  `io_set_direction`/`io_set_auto_report`/`expander_write_reg`/
  `expander_set_dir`/`expander_set_pullup`/`expander_set_opendrain`/
  `expander_set_debounce`/`expander_set_int_mask`/`expander_led_driver`/
  `expander_reset` tools and `gui.py`'s matching IO/Expander-page buttons
  rewired from `send_async`/`_send()` to the new methods via new
  `_io_mutating()`/`_io_mutating_async()` helpers.
- Tests: `tools/PcTools/tests/test_bridge_reject_reply.py` gained
  `DisplayWriteRefusalTests`/`TouchWriteRefusalTests`/
  `IoOtherWriteRefusalTests` (byte-exact, mirroring `ThermoMutatingReasonTests`),
  and `DisplayTouchDriverErrorTests`'s two `TOUCH_CMD_GET_STATE` cases were
  rewritten from "must raise" to "decodes to a reasonless/reasoned
  `OkReason`" — they encoded the pre-fix behavior as correct, so the fix
  above turned them red until updated. Negative-tested by reverting all
  three new decode branches in `devices.py` in turn: all ten new tests
  failed with `unknown {DISPLAY,TOUCH,IO} response subcommand 0x..`, then
  were restored and the suite re-confirmed green.
- PC suite 688 passed, 91 subtests passed, 0 failed (from 676 passed/91
  subtests/2 failed at the point this pass started — the 2 failures were the
  pre-existing `TOUCH_CMD_GET_STATE` tests above, not a regression this pass
  introduced).

**Two more decode-then-discard instances of the CONTROL/PROFILES bug,
found while finishing the audit above**: `parse_autotune_response()`'s
`ABORT`/`ACCEPT` branch and `parse_wifi_uart_response()`'s
`ADD_NETWORK`/`SET_MODE`/`SET_AP_IDENTITY`/`FORGET` branch both still
reduced their reply to a bare `bool(payload[1])`, discarding the reason
text `autotune_handle_message()`/`wifi_bridge_task()` (`uart_bridge_ext.c`)
already send on refusal (`ACCEPT`'s "no completed autotune result to
accept"; WIFI's "ssid too long", "saved network list is full",
"ap_password must be empty or 8-63 characters", "could not forget network",
etc). Both now return `OkReason`. `autotune.py`'s `AutotuneClient.abort()`/
`accept()` and `wifi_uart.py`'s `WifiUartClient.add_network()`/
`set_mode()`/`set_ap_identity()`/`forget()` changed their return type from
bare `bool` to `OkReason` (queries, not fire-and-forget writes — always
answered — so this is purely the same decode fix, no wait-window needed).
Wired through `mcp_server.py`'s matching tools (surfacing `.reason`) and
`gui.py`'s Wi-Fi Settings popup (`wifi_connect_async()`/
`wifi_set_mode_async()`'s `apply()` callbacks, previously typed for a bare
`bool`). `gui.py` has no UI for `set_ap_identity`/`forget`, so nothing else
to rewire there. Tests: `AutotuneAbortAcceptReasonTests`/
`WifiUartReasonTests` added (2 + 5 tests), each negative-tested by
reverting its decode branch back to `bool(payload[1])` (2 and 5 failures
respectively, all `AttributeError: 'bool' object has no attribute 'ok'` or
a missed `OkReason` type check), then restored.

**Running total for this whole pass, `tools/PcTools -m pytest -q`,
excluding `tests/test_kilnsim_testmgr.py`** (owned by a concurrent session
mid-edit at the time of this pass; 4 unrelated failures there, none in a
file this pass touched): 628 passed / 91 subtests / 0 failed right after
the DISPLAY/TOUCH/IO fixes, then 630 (+2, autotune) then 635 (+5, wifi) —
**635 passed, 91 subtests, 0 failed** final. Task-stated baseline before
this pass was 672 passed/91 subtests; the two figures aren't directly
comparable since several unrelated commits (the `KILNLINK_PROTOCOL_VERSION`
id-sharing bump, `kiln_ui.c`/`ui_page_*` work) landed in the same window
from other concurrent sessions and changed the suite's total independent of
this pass.

## 12. HTTP `max_uri_handlers` cap — DONE 2026-08-24, now machine-guarded

Bench boot log (commit `750dc33`) showed
`httpd_register_uri_handler(/api/safety/commissioning/bench_preset) failed:
ESP_ERR_HTTPD_HANDLERS_FULL` — `wifi_provision_http.c`'s
`config.max_uri_handlers` (then 84) had fallen behind the tree's real route
count for the **fourth** time (previous bumps: 59→72, 72→80, 80→84, all
logged in that file's own comment history above the assignment). Real
worst-case count, recounted by machine rather than by hand: every
`.uri = "..."` `httpd_uri_t` literal under `App/drivers/*.c` (comments
stripped) is **85 in a normal build, 87 with `CONFIG_KILNCTL_SIM_PLANT`**
(sim_backend.c's 2 `/api/sim` routes are the only conditionally-compiled
ones; no loops or macro-generated route tables exist anywhere in the tree).
Cap raised 84 → **95** (worst case + 8, same headroom order as every
previous bump). RAM cost: `esp_http_server` allocates
`hd_calls = calloc(max_uri_handlers, sizeof(httpd_uri_t *))` — an array of
4-byte pointers on this target, not of structs — so the bump costs
11 × 4 = **44 bytes**, negligible against the 12483-byte `dram_free`
this same boot log measured (and negligible next to the ~11.9 kB failure
floor documented for that stage).

**The actual fix**: `tools/check_uri_handler_cap.ps1` — a standalone
PowerShell guard (same shape as `tools/check_bridge_reject_reason.ps1` /
`tools/check_uart_version_independence.ps1`: comments stripped, non-zero
exit via `throw`, refuses to run blind if its own route count drops below
a sanity floor). It recounts every `.uri = "..."` under `App/drivers/`
(every `.c` file there, not a hardcoded module list, so a brand-new route
file is covered automatically) and fails if `max_uri_handlers` is below
that count. Not wired into a build step yet — that's the one thing left
open here; running it is currently a manual/CI-TODO step, not enforced on
every build. **Verified it actually catches the bug it exists for**: run
against the tree exactly as found (cap still 84) it failed with the real
87-route count before any fix was applied; lowering the restored cap by
one (95→86) reproduces the same failure; and, separately, adding 9 dummy
routes to `kiln_cfg_http.c` with the cap left untouched at 95 also failed
(96 routes > 95 cap) — proving the guard is coupled to the actual route
literals in source, not just to a number typed by hand, and would catch
tomorrow's forgotten bump the same way it caught this one. All three
failure modes were restored and the guard re-confirmed passing (87 routes,
cap 95, 8 spare) before this entry was written.

## 13. Internal-DRAM boot-time low-water alarm — DONE 2026-08-24

Same bench boot log as section 12 above prompted a separate investigation:
`heap stage uart_bridges_1 largest= 7680 delta= +0 dram_free= 12483`, a
~35KB drop in `dram_free` since the `executor+autotune` stage three log
lines earlier. Full itemization, the fragmentation-vs-consumption verdict,
and the risk read against the documented `free=11903, largest=8704`
HTTP-socket-reset failure (steady-state, 8000s uptime — NOT the same
measurement point as this boot-time trough, so "~500 bytes of margin" is
suggestive, not literal) are written up in `docs/PROJECT_STATUS.md`'s
2026-08-24 session-log entry — that is the authoritative record, not
repeated here. Short version: ~20.5KB of the ~35KB is attributable to six
internal-only task stacks created in that window (none uses
`MALLOC_CAP_SPIRAM`); ~14KB is an honest, unfilled gap.

**Shipped**: `App/drivers/common/dram_margin.h` (`dram_margin_check()`) wired into
`main.c`'s `heap_stage()` — every boot-time heap-stage log line now also
checks `largest`/`dram_free` against the documented failure's own figures
and logs `ESP_LOGE` if either is crossed, so a future regression here shows
up in the boot log instead of only surfacing later as "the web page won't
load". Host-tested in `App/test/test_dram_margin.c`, including a test using
this investigation's real bench figures that must trip the alarm — proven
non-vacuous by temporarily weakening the threshold, watching that test fail
(2 FAILURE(S), 919/921), then restoring it (921/921 green). Not yet seen to
fire on real hardware.

**Left open, needs bench measurement (`uxTaskGetStackHighWaterMark()`)
before any of it is safe to act on** — do NOT resize any of these from the
numbers in this entry alone:
- `uart_owner`/`uart_protocol` task stacks (`Kconfig`'s
  `KILNCTL_UART_OWNER_STACK_SIZE`/`KILNCTL_UART_PROTOCOL_STACK_SIZE`,
  4096B each, 3 tasks, all internal) — candidates to shrink or move to
  PSRAM, but PSRAM-stacking any of them needs the same flash-cache-disabled
  audit `uart_bridge_ext.c`'s header comment already did for the bridge
  tasks before this is safe.
- `rules_task`/`rules_watchdog` (3072B/2048B, internal,
  `App/drivers/rules_task.c`) — removed 2026-08-27 with the rule engine,
  see `docs/PROFILES.md`; no longer a candidate.
- `system_uart_bridge` (3072B, internal,
  `App/drivers/bridge/uart_bridge.c`) — same two options, same caveat.
- The ~14KB unattributed gap itself — worth a live coredump/heap-trace pass
  with real hardware rather than further static-analysis guessing.

**2026-08-24 follow-up — the blocker above is now measurable, not yet
measured.** Added `App/drivers/stack_margin.{h,c}` (registry wrapping
`uxTaskGetStackHighWaterMark()`, converting FreeRTOS's WORD-granularity
result to bytes — see `stack_margin_calc.h`'s `STACK_MARGIN_WORD_BYTES`
comment for why that conversion is called out explicitly and not a bare
`* 4`) and registered all six task stacks named above against it
(`uart_owner_task`/`uart_owner_evt_task`/`uart_proto_rx` in `main.c`,
`rules_task`/`rules_watchdog` in `rules_task.c`, `system_uart_bridge` in
`uart_bridge.c`). Exposed PC-side as a new, purely additive INFO subcommand,
`INFO_CMD_GET_STACK_MARGIN` (0x04, `uart_task_ids.h`) — no
`UART_PROTOCOL_VERSION` bump needed (see that constant's own bump policy: a
brand-new task_id/subcommand an older peer has never heard of is exactly the
case the policy says does NOT need one). `tools/PcTools/src/kilnctrl`:
`devices.parse_stack_margin_response()`/`StackMarginEntry`/
`StackMarginLevel`, wired into `info.InfoClient.get_stack_margin()` and
`parse_info_response()`'s structural dispatch. Host-tested both sides
(`App/test/test_stack_margin.c` for the word→byte conversion and headroom
classification; `tools/PcTools/tests/test_stack_margin_info.py` for the wire
decode), each with a negative-test failure demonstrated and reverted (see
those files' own comments for the exact failing assertions).

**Only the PC-link `uart_owner`/`uart_proto` instance is registered.**
`safety_link.c` runs the exact same `uart_owner.c`/`uart_protocol.c` code
for the isolated Pico link, with the same task names — registering both
under identical names would make the report ambiguous about which link a
reading belongs to. Left for whoever needs that link's own figures next
(would need a naming scheme, e.g. a `(pc)`/`(sfty)` suffix at the
`stack_margin_register()` call site in `safety_link.c`).

**Still true, unchanged by this follow-up: no real high-water-mark figure
has been read on this board.** Nothing here was flashed or read from
hardware (bench is in use elsewhere) — this only makes the reading
possible. The classification bands (`STACK_MARGIN_CRITICAL_PCT`/
`STACK_MARGIN_LOW_PCT` in `stack_margin_calc.h`, 15%/30%) are an ordinary
embedded-FreeRTOS triage heuristic, not a figure derived from this board;
treat a CRITICAL/LOW flag as "look at this one first," not as a proven
overflow risk. **Next step on the bench**: flash this build, connect
pc_tools, and call `InfoClient.get_stack_margin()` (or the MCP tool once one
is wired to it) after the board has been running long enough to have
exercised its worst-case code paths on each task (a UART burst for
`uart_owner`/`uart_proto`, a rule evaluation for `rules_task`, a
SYSTEM command for `system_uart_bridge`) — a high-water mark only reflects
paths actually taken, so reading it right after boot understates the true
worst case. Only once real per-task figures exist should any of the six
stacks above be resized, and the resize should update this entry with the
measured numbers, the same way `dram_margin.h`'s own thresholds are kept
current.

**2026-09-02 follow-up — DRAM_PSRAM_STATUS.md Phase 0 (4.2), registration
completed for the rest of the long-lived tasks.** Before this pass:
`profile_executor`/`profile_exec_wdt` (`profile_executor_start.c`),
`safety_owner_task`/`safety_owner_evt`/`safety_proto_rx`/`safety_poll`
(`safety_link.c`), `bx_flash_worker` (`uart_bridge_ext.c`),
`system_uart_bridge` (`uart_bridge_system.c`), `httpd_worker`
(`wifi_provision_http.c`), and the PC-link `uart_owner_task`/
`uart_owner_evt_task`/`uart_proto_rx` (`main.c`) — 12 total, exactly filling
`STACK_MARGIN_MAX_TASKS`.

**Correction to this section's own text above**: "Only the PC-link
`uart_owner`/`uart_proto` instance is registered" undersold it —
`uart_proto_rx` for the PC link (`main.c:1579`) was in fact already
registered by the 2026-08-24 pass this section describes, contradicting
DRAM_PSRAM_STATUS.md section 7.1's claim that it was still uninstrumented
(that plan doc is stale on this point as of this writing; not corrected
there per this project's own "code is truth, not the checkboxes" rule and
the instruction not to edit firmware/KilnFW/docs/ from this pass). Nothing
needed re-registering for `uart_proto_rx`.

Sixteen more tasks registered this pass, all registration-only (no stack
size changed): `spi_owner` (`esp_spi_owner.c`), `i2c_owner` (`i2c_owner.c`),
`kiln_io_owner` (`kiln_io_owner.c`), `thermo_owner` (`thermo_owner.c`),
`screen_idle` (`screen_idle.c`), `autotune_engine` (`autotune_engine.c`),
`telemetry_log` (`telemetry_log.c`), `link_watchdog` (`uart_bridge.c`),
`info_uart_bridge` (`uart_bridge_info.c`), `gpio_probe` (`gpio_probe.c`),
`lvgl` (`lvgl_port.c`, static allocation), `boot_button` (`boot_button.c`),
`danger_mode` (`danger_mode.c`), `recovery_exit` (`ota_http.c`'s
`recovery_exit_reboot_task` — label shortened from the 21-char FreeRTOS task
name `recovery_exit_reboot`, which would have silently truncated against
`STACK_MARGIN_NAME_MAX`), `ota_rollback_reboot` (`ota_http.c`), and
`ota_pico_rollback` (`ota_http.c`). `STACK_MARGIN_MAX_TASKS` raised 12 -> 28
in `stack_margin.h` to hold all of it; occupancy is now 28/28, no spare
slots — the next task registered here needs another bump.

Still true, unchanged: no real high-water-mark figures exist yet for any of
these sixteen. Same rule as above — exercise each task's real worst-case
path before reading, and do not resize anything from a number read right
after boot.

---

## 14. HTTP connection resets under concurrency — reproducible, cause NOT established

**Status: measured, not diagnosed.** Written down because the symptom matches
a failure this repo has already named, and the obvious attribution appears to
be wrong. Anyone who assumes "connection reset" means "internal DRAM
exhaustion" here will chase the wrong thing.

**Reproducer** (board at 192.168.1.156, commit `d90986c`, ~1 hour uptime):

```bash
for i in $(seq 1 8); do
  curl -s -o /dev/null -w "%{http_code} " --max-time 30 http://<board>/app.js &
done; wait
```

**Measured: 9 failures in 80 requests (11.25%)**, ten rounds of eight, almost
exactly one per round. The failure is always
`curl: (56) Recv failure: Connection was reset`.

### What is established

| Observation | Result |
|---|---|
| 8 parallel `/app.js` | ~1 in 8 reset, reproducible over 10 rounds |
| 4 parallel `/app.js` | 0 failures in 5 rounds |
| 8 parallel `/status` (small response) | 0 failures |
| Sequential `/app.js`, 6 in a row | 0 failures, all byte-identical, complete |
| Timing of the failing request | **0.13 s — the FASTEST of the eight**, not the slowest |
| `heap_internal.free` during the load | 23019–23127, essentially flat |
| `heap_internal.largest_free_block` | 8704, unchanged under load |
| `heap_internal.min_free` | 9955, **not moved by the load at all** |

So it needs BOTH high concurrency AND a large response. Small responses at the
same concurrency are fine; the same large response at half the concurrency is
fine.

### Two mechanisms RULED OUT

- **Total internal-DRAM exhaustion.** `min_free` (9955) is untouched by the
  load, and free internal heap stays flat at ~23 kB throughout. Whatever is
  failing, the load is not driving the heap lower than it already went during
  boot. This matters because `dram_margin.h` names this exact failure
  signature, so the temptation to close this as "the known DRAM problem" is
  strong and, on this evidence, unfounded.
- **Worker serialisation hitting the 3 s socket timeout.** The failing request
  fails FASTEST (~0.13 s) while all seven successes complete in 0.20–0.30 s.
  Nothing is waiting 3 s, so `lru_purge_enable` ageing out a stalled
  connection does not fit either, despite that being the mechanism
  `wifi_provision_http.c`'s own socket-sizing comment describes.

### Leading hypothesis, UNCONFIRMED

`/app.js` is **8589 bytes** on the wire (gzip; 23737 decompressed).
`largest_free_block` is **8704 bytes**. Those are 115 bytes apart. A
per-connection buffer for that payload needs a contiguous block, and two
concurrent ones cannot both come out of a single 8704-byte block — which would
make this a *fragmentation* failure (largest-block bound), not a *total free*
one, and would explain why only the large response fails and only under
concurrency.

This is a coincidence of two numbers, not a proof. To confirm or kill it:
instrument the actual allocation on the send path and log the failing size,
rather than reasoning from the two figures above.

### Why it matters

A browser loading a page issues several parallel requests, which is exactly
the 4–8 range where this starts. Losing one asset is precisely how the
originally-reported symptom ("the page says loading forever") presents. The
2026-08-20 socket-pool fix raised `max_open_sockets` to 13 specifically to
survive bursts, and 8 concurrent connections is well inside that — so this is
not the cap being hit.

### Correction to the record

An earlier note in this session said the documented failure "does not
reproduce". That was true for SEQUENTIAL fetches and false under concurrency,
and the distinction was not drawn at the time. Sequential testing is not
sufficient evidence that this class of failure is gone.
