# System-mode command gate — design (Phase 6)

**Status: DESIGN DOC ONLY, no code. Owner-approved for a design pass,
2026-09-25.** Source: `firmware/KilnFW/TODO.md` section 10.14, "Phase 6"
(added mid-Phase-1, user request). Do not implement ahead of an owner
decision on the open questions below.

## 1. Problem statement

The command-queue work in TODO.md 10.14 (Phases 1/2/4/5) gives each
state-owning domain (`kiln_io_owner`, `thermo_owner`, `wifi_prov`) a single
task and a lock that answers "can two writers race on this state." Phase 6
is a different question: **is this *class* of command allowed at all, given
what the system is doing right now** — independent of who's asking and
whether they'd race. Example: while a profile is firing, pause/stop/modify-
this-run is fine, but starting a *different* profile, starting autotune, or
a raw GPIO/SX1509 debug write should be refused outright, regardless of
whether the write itself would have been race-free.

`kiln_io_owner`/`thermo_owner` correctly know nothing about
`profile_executor`/`autotune_engine` state — that's the ownership boundary
Phase 1/2 drew on purpose. Mode policy is a layer *above* the owners.

## 2. What already exists (research findings)

### 2.1 System modes/states in the code today

| Mode/state | Where it lives |
|---|---|
| Idle vs. firing | `profile_executor` state machine (`profile_executor_state.h`); `profile_executor_get_status()` |
| Autotune running | `autotune_engine.c`'s `s_at` state, guarded by `s_at.lock` |
| OTA in progress | `ota_interlock.c`/`.h` — a pure, host-testable predicate already consulted by more than one caller (see 2.3) |
| Recovery mode | `boot_guard_is_recovery_mode()` (`boot_guard.h`) — fixed for the boot, no HTTP route exposes the raw counter, `GET /api/ota/esp/status`'s `recovery_mode` is the boolean |
| Safety tripped / ARMED | `safety_link.c` link state, `trip_reason`/`trip_mask`; ARMED is a latch, not a PWM window (project memory) |
| Commissioning incomplete | `readiness_http.c`'s checklist; `readiness_gate.h`'s `READY_NOT_DONE` biconditional over the *same* predicates (see 2.4) |
| Web auth on/off | `web_auth_store.c` / `security_backend_web_auth.c` |
| LCD PIN / session | LCD session timeout (10 min) tracked alongside web (30 min) per `web_auth_store` |

**There is no single "get current mode" function today.** Each caller that
cares reaches into a different module's state directly, at a call site it
chooses for itself. Two existing pieces of this codebase already point at
the shape Phase 6 should generalize — both worth citing because one does it
right and one doesn't:

### 2.2 Right-shaped precedent: `readiness_gate.h`, and it's already fully unified

`profile_executor_run.c:156-168`'s own doc comment names every start entry
point explicitly: `POST /api/profile_exec/start` (`dashboard_exec_http.c`),
both LCD start buttons (`ui_page_home_actions.c`, `ui_page_profile_detail.c`)
and the benchproto `RUN` command (`uart_bridge_ext_control.c`) all funnel
through this one function, which checks (in order) `readiness_gate_refuses_
start()` (`:184`, itself folding in `boot_guard_is_recovery_mode()` per
`readiness_gate.c:47` and the safety-trip state per `readiness_gate.c:54-58`),
the `s_exec.lock == NULL`/recovery check (`:194`), profile/zones validity,
and `ota_http_heat_blocked_by_update()` (`:286`). `autotune_engine.c:962-978`
documents and implements the identical pattern for autotune's three start
functions. **This is already the shape Phase 6 should generalize** — one
owning function, one call site per engine, every transport gets the same
answer for free. Its header states the design principle in one line worth
reusing verbatim:

> "the gate blocks item X <=> item X's displayed status is READY_NOT_DONE"
> — a thin test over the *same* predicates the display already renders,
> so the gate and the page shown to the operator are physically incapable
> of disagreeing. `check_readiness_gate_display_agreement.ps1` enforces it.

(`recovery_start_refusal.h`'s two HTTP-only call sites, `dashboard_exec_
http.c:675` / `dashboard_autotune_http.c:207`, sit *on top of* this already-
unified gate purely to surface a recovery-specific message before the
generic one — see 2.4. They are redundant with, not a replacement for, the
real gate.)

### 2.3 Right-shaped precedent: the OTA/profile mutual interlock

`ota_interlock.c`/`.h` is a pure, ESP-IDF-free predicate consulted from
both directions: `profile_executor_run.c:278-283` refuses a firing start
when OTA holds the interlock, and `ota_http_check_interlocks()`/
`ota_interlock_check()` refuse OTA while a firing/heat cycle holds it. This
is the "one owning function, called from both sides" shape Phase 6 needs —
just narrower in scope (one pairwise relationship, not a general table).

### 2.4 Minor drift, same class: recovery mode's message-only inconsistency

`recovery_start_refusal.h`'s `recovery_mode_refuses_start()` is called from
exactly two places: `dashboard_exec_http.c:675` and
`dashboard_autotune_http.c:207` — both HTTP, layered on top of the already-
unified gate in 2.2 purely to substitute a recovery-specific message ahead
of the generic one. A UART or LCD start request in recovery mode is refused
today too (same `readiness_gate_refuses_start()` call sees the same
`recovery_mode` fact) — just with a more generic reason string than HTTP's.
Not a safety gap, but exactly the kind of one-transport-only wording drift
Phase 6's shared `reason` string (3.3) should retire this HTTP-only
special case in favor of, rather than a template for the *headline* gap
(2.5's manual relay writes).

### 2.5 The real, safety-relevant gap: manual relay writes have no firing-mode check

`owners/kiln_io_owner.c`'s relay-ON choke point (`relay_on_blocked()`
family, doc block ~`:146-176`) is explicitly "the ONE choke point every
MANUAL relay-ON command reaches" — `dashboard_http.c`'s `/api/relay`, the
LCD's manual override (`ui_page_temperature.c`), and `uart_bridge.c`'s
`SET_RELAY`/`SET_RELAY_MASK` all funnel through it. It checks
`relay_authority_on_blocked()` (safety-fault state) and
`ota_http_heat_blocked_by_update()` (OTA interlock) — **but nothing checks
`PROFILE_EXEC_RUNNING`/autotune-running.** Starting a *new* profile while
one is already running is heavily gated (2.2); flipping a relay by hand,
on any of the three transports, while that same profile is mid-firing is
not gated by mode at all today. This is the concrete instance of the
problem statement's own example ("a raw GPIO/SX1509 debug write should be
refused outright" while firing) that is genuinely open today, not merely
inconsistent in wording.

### 2.6 Inventory table — action × entry point × current check

| Action | HTTP | UART bridge | LCD | Notes |
|---|---|---|---|---|
| Start profile | `dashboard_exec_http.c` → `profile_executor_run()`'s single choke point (2.2): `readiness_gate_refuses_start()` + prestart NULL-lock/recovery + OTA interlock, plus an HTTP-only `recovery_mode_refuses_start()` pre-check for a nicer message | same choke point, via `uart_bridge_ext_control.c`'s `RUN` command | same choke point, via `ui_page_home_actions.c`/`ui_page_profile_detail.c` | Fully gated on all three transports; only the recovery-mode *wording* differs (2.4) |
| Start autotune | `dashboard_autotune_http.c` → same pattern (`autotune_engine.c:962-978`) | same choke point | same choke point | Mirrors profile start |
| Zones/config write during a firing | `zones_http.c`/`zones_http_post.c` — **no `PROFILE_EXEC_*`/is-running check found**, only a comment noting the handler keeps its own possibly-stale view | not exposed on the UART bridge today | n/a | Value-bound checks (guard-5) exist; a mode check ("a firing is in progress") does not |
| Manual relay write (HTTP `/api/relay`, UART `SET_RELAY`/`SET_RELAY_MASK`, LCD manual override) | `kiln_io_owner`'s single choke point (`relay_on_blocked()` family) checks safety-fault + OTA interlock | same choke point | same choke point | **Headline gap (2.5): no `PROFILE_EXEC_RUNNING`/autotune-running check anywhere in this already-unified choke point** |
| Factory reset | `factory_reset.c`: `ota_http_authenticate_request()` (auth) then `ota_http_check_interlocks()` (OTA interlock only) | not exposed | not exposed | No explicit firing/autotune-running check distinct from the OTA interlock |
| cfgfs format | `cfg_fs_format_http.c` (auth-gated route) + `cfg_fs_format_gate.c` (pure on-disk structural validation, no mode awareness) | referenced in `uart_bridge_ext.c` | not exposed | **No mode check of any kind found** |
| OTA start/flash | `ota_http_check_interlocks()`/`ota_interlock_check()` — the mutual interlock's other direction, itself informed by profile/autotune heating state (2.3) | same interlock | n/a (LCD does not initiate OTA) | Best-shaped example — symmetric, mutual, kept as-is |

`route_tier_table.h` confirms tiers are **auth only**: `ROUTE_TIER_OPEN`
(no credential), `ROUTE_TIER_USER`, `ROUTE_TIER_ADMIN`, plus
`ROUTE_TIER_SAFETY_REDUCE` and `ROUTE_TIER_ADMIN_BOOTSTRAP` for narrow
documented exceptions. Nothing in that table encodes "and also not during a
firing" — mode is a wholly separate axis today, checked (if at all) ad hoc
per handler.

## 3. Proposed gate

### 3.1 One owning function, one table

```c
// system_mode_gate.h -- pure, host-testable, ESP-IDF-free (same layering
// discipline as ota_interlock.h/readiness_gate.h).

typedef enum {
    SYS_ACTION_START_PROFILE,
    SYS_ACTION_START_AUTOTUNE,
    SYS_ACTION_WRITE_ZONES_CONFIG,
    SYS_ACTION_RAW_RELAY_DEBUG_WRITE,
    SYS_ACTION_FACTORY_RESET,
    SYS_ACTION_CFGFS_FORMAT,
    SYS_ACTION_OTA_START,
    // ... one entry per class of command, not one per route/subcommand
} sys_action_t;

typedef struct {
    bool profile_running;
    bool autotune_running;
    bool ota_holds_interlock;
    bool recovery_mode;
    bool safety_tripped;      // ARMED latch state
    bool readiness_gate_ready; // same READY_NOT_DONE biconditional readiness_gate.h already computes
} sys_mode_snapshot_t;

// Pure function: snapshot in, verdict out. No I/O, no locks taken inside it.
bool system_mode_gate_check(sys_action_t action,
                             const sys_mode_snapshot_t *snap,
                             char *reason, size_t reason_cap);
```

A single table (`action -> which snapshot fields refuse it`) inside the
`.c` file, reviewed as one unit — the same "one legible record" discipline
`route_tier_table.h` already uses for auth. Adding a new action or entry
point means adding one row, not hunting for the right `if` to copy.

### 3.2 Composition with the auth gate

**Auth first, then mode — never the reverse, and mode never weakens auth.**
An unauthenticated caller gets 401/403 before the mode gate is ever
consulted; the mode gate only runs for a caller who already cleared
`route_tier_table.h` (or the UART bridge's own credential check, or the LCD
PIN). This matches the existing `recovery_mode_refuses_start()` call order
(auth middleware already ran by the time a `dashboard_*_http.c` handler
body executes) and keeps the two axes orthogonal: auth answers "who," mode
answers "what, right now."

### 3.3 Per-entry-point response contract

| Entry point | On refusal |
|---|---|
| HTTP | Existing convention: 4xx (409 for a mode conflict, matching the existing OTA-interlock/readiness-gate precedent of using 428/409 for "not now" vs. 403 for "not you") with a JSON body carrying the same `reason` string the pure function produced — no per-handler re-wording |
| UART bridge | Existing `{subcmd, ok, reason}` reply shape (`bridge_reply_reject()`/`bx_reply_ok_err()`, the convention section 11 of TODO.md already established) — the `reason` field is the *same string* `system_mode_gate_check()` wrote, so a UART caller sees the recovery-mode-specific wording that today only HTTP gets |
| LCD | The existing generic "Cannot Start" modal (kept per the 2026-09-24 owner decision on the firing-ceiling revert) gains the same `reason` string as its body text instead of a fixed generic message |

One string, three renderings — never three independently-worded refusals
for the same fact, which is the exact drift `recovery_start_refusal.h`
introduced for HTTP-only.

### 3.4 Lock/snapshot strategy

**Never hold a module lock across a producer call.** `system_mode_gate_check()`
takes a plain-old-data snapshot, not locks. The caller (whichever entry
point invokes it) is responsible for building that snapshot by calling each
domain's existing cheap read accessor (`profile_executor_get_status()`,
`autotune_engine_get_status()`, `ota_interlock_check()`,
`boot_guard_is_recovery_mode()`, the readiness predicates) *before* taking
any lock of its own, then calls the gate against the resulting struct with
no locks held. This mirrors the `dashboard_get_status()` fix already in
this codebase (CLAUDE.md's "cache a snapshot outside the lock" note) and
keeps `s_exec.lock` → `s_at.lock` as the only lock order anywhere near this
code — the gate itself never acquires either.

### 3.5 Host-testability

Pure input-struct-in/verdict-out function: trivially host-testable with a
fake snapshot per row of the action table, no board, no ESP-IDF — same
shape as `test_readiness_gate.c`'s full-cross-product test and
`test_recovery_start_refusal.c`. A single test file can assert every
`(action, snapshot)` combination in the table without touching hardware.

### 3.6 Rollout slices

1. **LANDED** — `system_mode_gate.h`/`.c` with the table above and a host
   test (`test_system_mode_gate.c`, 27 checks, negative-tested), unwired
   beyond slice 3 below.
2. **LANDED, 2026-09-27** — wired `SYS_ACTION_START_PROFILE`/
   `SYS_ACTION_START_AUTOTUNE`'s recovery-mode rule into
   `profile_executor_run()`'s and `autotune_begin_run_locked()`'s existing
   single choke points, ahead of their `readiness_gate_evaluate()` call
   (both now collect `readiness_gate_facts_t` once and share it between the
   two calls). `dashboard_exec_http.c`/`dashboard_autotune_http.c`'s two
   HTTP-only call sites were rewired onto the same `system_mode_gate_check()`
   call, each still answering with the JSON envelope its frontend parse
   expects. `App/drivers/http/recovery_start_refusal.h` and
   `test_recovery_start_refusal.c` are deleted — this was their only
   caller. All three transports (HTTP, UART, LCD) now produce the exact
   string `system_mode_gate.c` writes for a start request in recovery mode.
   No owner decision needed — this action encodes only recovery_mode, no new
   rule; every other start refusal stays owned by `readiness_gate.h`,
   unchanged.
3. **LANDED** — wired `SYS_ACTION_RAW_RELAY_DEBUG_WRITE` into
   `kiln_io_owner.c`'s single `relay_on_blocked()` choke point (the
   headline gap, §2.5), encoding owner decision Q1 above (blanket refusal,
   any relay, while `profile_running || autotune_running`). Because this is
   one already-unified choke point, this slice closes the gap for all three
   transports (HTTP's danger-mode relay route, UART's `SET_RELAY`/
   `SET_RELAY_MASK`, the LCD's manual override) in one change:
   `kiln_io_owner.h`'s new `KILN_IO_OWNER_RELAY_ERR_RUNNING`,
   `dashboard_http.h`'s new `DASHBOARD_RELAY_ERR_RUNNING`, a UART reject
   reason word `"running"`, and an LCD message
   ("Relay N refused -- firing/autotune active"). **Correction (review,
   2026-09-25):** the first landing of this slice believed HTTP's only
   surviving manual-relay route (`POST /api/diagnostics/danger/relay`) was
   unreachable, because `danger_mode_active()`'s early return in
   `relay_on_blocked()` bypasses the entire chain below it, same as the
   update-in-progress and unacknowledged-crash checks. That was true for
   those two, but wrong for this one: `danger_mode_request_start()`
   (`danger_mode.c`) only refuses an *active profile*, never checks
   autotune, so danger mode + a live autotune run let this HTTP route
   energize a relay despite the blanket refusal (Q1) — a real gate bypass,
   not a documentation gap. Fixed by moving the mode-gate check ahead of
   `danger_mode_active()`'s early return in `relay_on_blocked()` (it is now
   the one gate danger mode does NOT skip) and mapping
   `DASHBOARD_RELAY_ERR_RUNNING` to HTTP 409 in
   `danger_relay_post_handler()` (`diagnostics_http.c`). The blanket refusal
   is enforced on all three transports, including HTTP, as Q1 always
   intended.
4. **LANDED, 2026-09-25 (review pass)** — wired `SYS_ACTION_WRITE_ZONES_CONFIG`
   into `zones_http_post.c`, `uart_bridge_ext_control.c` (`SET_ZONE_PID`/
   `SET_ZONE_MODEL`), `kiln_cfg_http.c` (apply, at submit time),
   `backup_import.c` (top of `backup_import_post_handler()`),
   `zones_http_pid.c` (`POST /api/zones/pid`, revoking its prior deliberate
   carve-out that allowed PID-only edits even while a firing was
   RUNNING/PAUSED), `iter_tune_http.c`'s `restore_commissioned`, and
   `adaptive_tune_http.c`'s `enable`/`revert` handlers — per owner decision
   Q2 above (refuse all zones/config writes while a firing or autotune run
   is active, not scoped to zones the run touches). Autotune/adaptive_tune's
   own internal accept-path writes (calling `zones_config_set_*()` directly
   while a run IS active) stay ungated by design.
   **Narrowed, 2026-09-25 (later same day, owner decision):**
   `adaptive_tune_http.c`'s `enable_post_handler()` now gates only the
   `enabled=true` case — turning adaptive tune OFF during a run is allowed,
   since it can only PREVENT a future change, never apply one (the same
   shape as pausing a firing, not a config write). `revert_post_handler()`
   is unaffected and still refuses unconditionally while a run is active.
   **Review fix (dead-code ordering):** `zones_http_post.c`, `kiln_cfg_http.c`
   and `backup_import.c` originally called the mode gate AFTER
   `ota_http_check_interlocks()`, which answers first while a firing is
   active and made the mode gate's own 409 unreachable in that state --
   fixed by reordering to mode gate first, then the OTA interlock, then
   `http_async_job_busy()` (landed alongside A1). Handler-level test:
   `test_zones_http.c`'s
   `test_zones_post_refused_by_mode_gate_before_interlock`.
   **Handler-level gate coverage:** `kiln_cfg_http.c` and `backup_import.c`
   (`test_kiln_cfg_http.c`, `test_backup_import.c`), `zones_http_pid.c` and
   `iter_tune_http.c` (`test_zones_http.c`, `test_iter_tune_http.c`), and
   `uart_bridge_ext_control.c`'s `SET_ZONE_PID`/`SET_ZONE_MODEL` UART entry
   point (`test_uart_bridge_ext_control_gate.c`, its own executable in
   `build_host_tests.ps1`).
   **Closed:** `adaptive_tune_http.c`'s `enable`/`revert` handlers (including
   the 2026-09-25 enabled=false carve-out) now have their own executable,
   `test_adaptive_tune_http_gate.c` (separate from `test_adaptive_tune_http.c`,
   which deliberately hand-mirrors only `status_get_handler()`'s render
   format and never links the rest of the translation unit).
5. **LANDED, 2026-09-25** — `SYS_ACTION_FACTORY_RESET` wired into
   `factory_reset.c` (after auth) and the UART-exclusive
   `factory_reset_execute()` entry point (`uart_bridge_system.c`);
   `SYS_ACTION_CFGFS_FORMAT` wired into `cfg_fs_format_http.c` (first line
   of the handler), per owner decision Q3 above (refuse outright while
   running). **Closed:** the UART `factory_reset_execute()` path (no
   dedicated `test_factory_reset.c` file exists) is now covered by
   `test_ota_http.c`'s
   `test_factory_reset_execute_refused_by_mode_gate_during_firing()`,
   proving the exact return-code contract `uart_bridge_system.c`'s mapping
   depends on (`FACTORY_RESET_ERR_MODE_GATE_REFUSED`, never
   `ESP_ERR_INVALID_STATE`).
6. **LANDED, 2026-10-08** -- `tools/check_system_mode_gate_call_sites.ps1`: explicit
   allowlist (19 files, 23 entries) of the handlers/helpers that must contain a
   `system_mode_gate_check()` call (zones POST/PID, kiln_cfg apply, backup_import,
   iter_tune restore, adaptive_tune enable/revert, factory_reset, cfgfs format, recovery boot,
   aux outputs, zone aux convert, UART zones write, relay choke point, update stage write and
   settings, start profile/autotune). Fails if an entry disappears, loses its call, the entry
   count changes, or an unlisted driver file adds a call. Presence-in-function only; "gate
   first" ordering stays with the handler host tests. Negative-tested with `negtest.ps1`.
7. **LANDED, 2026-09-28** — PcTools' `factory_default_then_load_preset()`
   (`mcp_server_ui_test.py`) used to "confirm" a factory reset by calling
   `get_fw_version()`, a plain UART query the always-alive INFO task answers
   whether or not the board ever rebooted — indistinguishable from a reset
   silently refused by this gate. Fixed by
   `InfoClient.wait_for_boot_push()` (`info.py`), which blocks specifically
   for the device's own UNSOLICITED once-per-boot FW-version push and
   reports "refused" on a timeout instead of treating a still-live link as
   success. Unit-tested with a bare `InfoClient` (no real serial link),
   `tools/PcTools/tests/test_info_boot_push.py`.
8. **Verified, no code change needed, 2026-09-28** — the 96-byte
   `SYSTEM_MODE_GATE_REASON_MAX` refusal-reason buffers this plan's HTTP
   handlers added are already covered by `check_httpd_task_stack_budget.py`
   (each handler is its own enumerated root, not a hardcoded buffer list),
   and the UART `factory_reset_execute()` path's own buffer is covered the
   same way by `check_system_uart_bridge_stack_budget.py` (rooted at
   `system_bridge_task`, which statically reaches it). Both are wired into
   `check_all_task_stack_budgets.py`. Not re-validated against a fresh
   target build in this pass — the next `check_00_kilnfw_target_build.ps1`
   run will reflect any actual `CEILING_BYTES` movement.

Each slice lands independently and is negative-tested per COMMON.md/
IMPLEMENTER.md discipline before merge; no slice depends on a later one.

## 4. Non-goals

- Not a replacement for `kiln_io_owner`/`thermo_owner`'s race-arbitration
  locks, or for the OTA/profile mutual interlock, or for
  `readiness_gate.h`'s firing checklist — those stay exactly as they are;
  the gate composes with them (calls their read accessors into its
  snapshot) rather than replacing their logic.
- Not a UART-bridge or LCD architecture change — it reuses each transport's
  existing reply/error convention, adding a shared reason string, not a new
  wire format.
- Not touching `route_tier_table.h` or any auth tier.

## 5. Owner decisions (2026-09-25)

1. **Manual relay writes during a firing (§2.5's headline gap):**
   **BLANKET-REFUSE** — any manual relay-ON write (HTTP's danger-mode relay
   route, the UART bridge's `SET_RELAY`/`SET_RELAY_MASK`, and the LCD's
   manual override) is refused while a firing or autotune session is
   active, for ANY relay, not scoped to relays the run actually claims.
   This is the owner's explicit choice over this doc's own claimed-relays-
   only recommendation above. **Landed** — see §3.6 slice 3.
2. **Zones/config writes during a firing:** **REFUSE ALL** — any zones/
   config write is refused while a firing or autotune session is active,
   not scoped to zones the run actually touches. Same override of this
   doc's own scoped recommendation as Q1. **LANDED, 2026-09-25** — see
   §3.6 slice 4 for the full call-site list and known test gaps.
3. **Factory reset / cfgfs format while firing:** refuse outright, as
   recommended. **LANDED, 2026-09-25** — see §3.6 slice 5.
4. **HTTP status code for a mode refusal:** 409 for all new
   `system_mode_gate` refusals, as recommended; OTA's existing 428
   interlock is untouched. **LANDED, 2026-09-25** — every HTTP call site
   wired in slice 4/5 sends this 409 via the shared
   `system_mode_gate_http_send_refusal()` sender, distinct from OTA's 428
   and (where applicable) `http_async_job_busy()`'s own 409.
