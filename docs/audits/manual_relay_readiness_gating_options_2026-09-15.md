# Manual relay control vs. the readiness gate — options for an owner decision (2026-09-15)

**Status: OPEN — owner decision needed. No safety behaviour was changed.**

Question: should `recovery_mode`, `crash_report` and `estop_verified` (which
refuse a firing and an autotune run) also refuse manual relay-ON commands and
the CT-calibration sweep? `docs/SAFETY_CASE.md` item 10 records the gap; this
audit re-verified it in code at HEAD and lays out the options.

## 1. What gates a firing / autotune today

One decision function, `readiness_gate_refuses_start()`
(`firmware/KilnFW/App/drivers/safety/readiness_gate.h`), called from two
choke points:

- `firmware/KilnFW/App/drivers/control/profile_executor_run.c:86`
- `firmware/KilnFW/App/drivers/control/autotune_engine.c:967`

plus HTTP legibility duplicates (same call, for a 409 naming the item) at
`firmware/KilnFW/App/drivers/http/dashboard_exec_http.c:669` and
`firmware/KilnFW/App/drivers/http/dashboard_autotune_http.c:211`.

It blocks on **five** items (`readiness_gate_block_t`): `recovery_mode`,
`safety_trip`, `crash_report`, `estop_verified`, and `safety_ceiling_match`
(added 2026-09-14). By its own header it has **no override**, by owner
instruction. `SAFETY_CASE.md` item 10 and the stale "four" comments in
`readiness_gate.h`/`check_readiness_gate_display_agreement.py` (both counted
five blocking items in the code but still said "four" in prose) were
corrected 2026-09-15; no behaviour changed.

## 2. What each manual heat surface checks

| Surface | Entry | Reaches `kiln_io_owner`? | Gates applied |
|---|---|---|---|
| LCD manual override | `firmware/KilnFW/App/drivers/ui/ui_page_temperature.c:327` via `dashboard_set_relay()` | yes, `firmware/KilnFW/App/drivers/http/dashboard_http.c:609` | safety fault sources + OTA interlock |
| benchproto `SET_RELAY` / `SET_RELAY_MASK` | `firmware/KilnFW/App/drivers/bridge/uart_bridge_io.c:201` | yes, direct | safety fault sources + OTA interlock |
| `POST /api/diagnostics/danger/relay` | `firmware/KilnFW/App/drivers/http/diagnostics_http.c:1021` | yes, via `dashboard_set_relay()` | requires danger mode armed; then **every** gate is bypassed |
| CT sweep | `zones_current_sweep_start()`, `firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c:1616`; writes via `zone_sweep_hw_energize()`, `firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c:824` (MANUAL entry point) | yes | start: hw present, zones cfg valid, no profile/autotune, link up and not tripped, no relay already on, CT topology known; per write: same owner gate as manual |

`POST /api/relay` **does not exist** — deleted 2026-08-27 with
`manual_page.html` (`dashboard_http.c:654`).

None of the four surfaces checks `recovery_mode`, `crash_report` or
`estop_verified`. The gap is real.

## 3. One layer down: `kiln_io_owner`

`relay_on_blocked()` (`firmware/KilnFW/App/drivers/owners/kiln_io_owner.c:180`)
is the single gate for every manual relay-ON. It checks only:

1. `danger_mode_active()` — if true, returns "not blocked" unconditionally
   (logging when that changed the outcome), `kiln_io_owner.c:190`;
2. `relay_authority_on_blocked()` — purely
   `safety_link_get_fault_sources() != 0`
   (`firmware/KilnFW/App/drivers/owners/relay_authority.c:15`);
3. `ota_http_heat_blocked_by_update()`.

It does not refuse on any of the three readiness conditions.

**No surface bypasses the owner.** The only `kiln_io_set_relay()` call outside
`kiln_io.c` is `kiln_io_owner.c:304`, and
`tools/check_relay_writes_through_owner.ps1` enforces it (passing at HEAD).
So there was no unambiguous defect to fix: the plumbing is sound; the missing
piece is a policy decision.

Implication: if a gate is wanted, `relay_on_blocked()` is the one place that
covers all four surfaces at once (the sweep's per-write path included),
rather than editing each caller.

## 4. Would the Pico catch it?

No. The Pico owns heat-enable (K4) and its own contactor, outside the ESP's
four-relay authority (`danger_mode.h`). **An operator manually energizing an
ESP relay on a crashed, recovery-mode or E-stop-unverified board is not
detected or prevented by the Pico's independent chain.** It sees only the
consequences, through its own temperature guards and, where a CT is fitted,
current guards — and only if K4 is enabled for heat to flow at all.

## 5. Per condition: does blocking break a legitimate diagnostic workflow?

- **`recovery_mode`** — yes, likely. Recovery mode is precisely when an
  operator is diagnosing the board; confirming a relay responds is part of
  that. It is a boot-time fact that cannot be cleared without a reboot, so
  blocking removes manual control for the whole boot.
- **`crash_report`** — little cost. Clearing it is one explicit operator
  action (`POST /api/crash_report/ack`) after reviewing the record, with no
  dependency on relay control. The condition means "this board's recent
  state is not trustworthy", which is exactly when an unreviewed manual
  energize is worst.
- **`estop_verified`** — risky. It is an operator-confirmed NVS record, not a
  live reading, and the bench procedure that justifies setting it
  (`firmware/SaftyFW/README.md`, "Bench verification procedure") may itself
  want a relay or contactor energized. Blocking could create a lockout where
  the record cannot honestly be set without the control it gates.

## 6. Existing confirm/override precedent

- **Danger mode** (`danger_mode.c`, `diagnostics_http.c`): explicit
  `accept=1`, refuses while a firing runs, time-limited window extended per
  command, auto-exit, a log line whenever a gate is bypassed. The ready-made
  shape for any confirm path — follow it rather than inventing a `force` flag.
- **Password-authenticated action**: `POST /api/ota/esp/recovery_exit`.

Tension to resolve: `readiness_gate.h` says "NO OVERRIDE ... raise it with
the owner". A confirm path on manual relay must not be read as weakening that
rule for firings.

## 7. Options

- **A. Status quo, documented.** Manual control stays gated only by safety
  fault + OTA. Zero lockout risk; the gap remains.
- **B. Gate `crash_report` only, in `relay_on_blocked()`** (danger mode still
  bypasses). Covers LCD, benchproto and CT sweep in one place; cheap to clear;
  no diagnostic chicken-and-egg. Needs a distinct refusal result (like
  `KILN_IO_OWNER_RELAY_ERR_UPDATING`) so callers do not misreport it as a
  safety fault, and no large locals on the HTTP path (httpd stack).
- **C. Gate all three in `relay_on_blocked()`, danger mode as the escape.**
  Consistent with firing, but makes danger mode the only way to touch a relay
  in recovery mode or before E-stop verification — pushing routine diagnosis
  into the most permissive mode there is.
- **D. Gate the CT sweep on the full readiness gate; leave manual relay as
  is.** The sweep energizes every zone automatically, closer to an autotune
  than to one operator click; but it also produces CT calibration that
  commissioning needs, so check commissioning order before choosing it.

## 8. Recommendation

**Option B.** It also gates the CT sweep on `crash_report` for free, since
sweep writes use the MANUAL owner entry point. Leave `recovery_mode` and
`estop_verified` ungated on manual control: the first describes the exact
situation manual control exists for, and the second risks locking an operator
out of the procedure that sets it. Keep danger mode's bypass as it is
(documented, deliberate).

Unaffected by every option: the Pico's `abs_max_temp_c` equality and Pico
arming.

## What was not verified

Static code reading only — no board contact, no build of a gated variant, no
negative test (nothing was implemented).
