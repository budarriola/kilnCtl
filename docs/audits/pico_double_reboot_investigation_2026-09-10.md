# Pico double-reboot investigation (2026-09-10)

Follow-up to `docs/audits/cplval45_scale_vs_offset_aborted_2026-09-10.md`
(commit `238e0eb9`), which recorded two unexplained RP2040 (SaftyFW) reboots
(`boot_id` 58→60) roughly coincident with the start of a 45 °C heating run,
`trip_mask=0x0000` throughout (no trip ever latched).

## Pre-flash diagnostics (running build, before touching anything)

Board on connect: ESP `commit=0dddd435` (**33 commits behind HEAD**,
`238e0eb9`), SaftyFW `commit=a87672ab`, built `2026-09-10 02:49:20Z`,
`boot_id=60` (i.e. already sitting at the state left by the prior session's
second reboot). `trip_mask=0x0000`, all thermocouples ~33–35 °C, link up.

Direct SWD reads against the running Pico image (`WATCHDOG_BASE=0x40058000`):

- `WATCHDOG->REASON` (0x40058008) = `0x00000001` — confirms the last reset
  was watchdog-forced, consistent with the prior session's own log ("boot
  reason: watchdog" both times).
- `watchdog_hw->scratch[5]` (0x40058020) = `0x00000000` — **no fatal tag
  latched.** None of the three new hooks landed in `31741ece`
  (`vApplicationMallocFailedHook`'s 0xB4, `configASSERT`'s 0xA5,
  `stack_overflow_hook`'s 0xE3) fired, or if one did, it was overwritten.

`31741ece` (the hooks themselves) turned out to already be an ancestor of
the running `a87672ab` — so the hooks *were* on-chip. But `1496e288`
("SaftyFW: stop the fatal-fault diagnostic from erasing its own evidence")
was **not** — `a87672ab` predates it. `1496e288` exists precisely because
`watchdog_task`, running on the other core, can overwrite a genuine fatal
tag in `scratch[5]` with its own `0xD9` "overdue checkin" tag before the
reset lands, and the overdue-diag reader then clears a `0xD9`-tagged
`scratch[5]` on the next boot as an ordinary (non-fatal) event. A clean
`scratch[5]=0` on this build is therefore **not proof nothing fatal
happened** — it is exactly the state the missing fix warns about. This
running build's diagnostics say nothing definitive about the two prior
reboots; that is being reported explicitly rather than reading "no tag" as
"no fault."

Also absent from the running build: `a75d88fc` (test-only kill switches
still compiled into config_store) and the config-store fallback ABA fix.

## Flash to HEAD

Built SaftyFW from a clean detached worktree (`git worktree add --detach
C:\wt\saftyfw_2026-09-10 HEAD`, `238e0eb9`) rather than the shared main
tree, configured against the same toolchain the main build directory uses
(`PICO_SDK_PATH=C:/pico-tools/pico-sdk`, Ninja), built clean (462/462), and
flashed via `debug_program(peer="pico", elf_path=".../SaftyFW.elf",
confirm=true)`.

Reset produced the expected S6a (mainFault) while the ESP's safety-link
handshake caught up: `safety_get_diag` read `trip_mask=0x0020`, `trip_reason
6 [SAFETY_TRIP_MAIN_FAULT]` — exactly and only the expected bit. Cleared
with `safety_clear_trip()`; confirmed `trip_mask=0x0000`, `state=grace`.

Commissioning read back unchanged after the flash:
`commissioned=True`, config CRC matches live (not stale), S1
`abs_max_temp_c=80C` ARMED, **S8 `max_rate_c_per_min=20C/min` ARMED**
(unchanged), S14/S15 present but DORMANT (no `i_normal_a` measured, as
before). No persistence defect.

## Reproduction attempt

Slot 7 (`pv08311918`) read back matching the previous session's saved
original (40/45/60 °C, confirming that restore held), then overwritten
again with the same single-segment `cplval45`-shaped profile (45 °C,
120 °C/hr, 70 min dwell, `zone_mask=0x7`) and started via `profiles_start`.

**The Pico did not reboot.** `safety_get_diag` was polled through
uptime 16 s (post-flash), 70 s, 122 s, and 186 s of continuous run
time — past both the ~30 s mark of the first original reboot and the
~2-minute mark of the second — with `boot_id` staying at 58 throughout,
`trip_mask=0x0000`, `context frames ok` climbing monotonically, zero `bad`.

**A different, unrelated fault occurred instead, on the ESP.** At
firing elapsed ~124 s the device log shows zone 0's per-zone thermal guard
tripping (`heating but rose only -0.21C in 1.0min (need >=0.50C)`),
`profile_executor` abandoning the whole firing
(`continue_on_zone_trip is off`) — both unremarkable — followed
**immediately** by `esp_reset_reason=4 (PANIC)` and a fresh ESP boot
(`uptime_s` back to 13). `get_heap_status` afterward showed
`reset_reason='panic/exception' (unclean boot)` with no
"UNACKNOWLEDGED CRASH REPORT" banner — the log line
"COREDUMP PRESENT in flash from a previous crash" appeared, but the
`/api/crash_report` acknowledgement path this task's brief describes
(`get_heap_status` failing loud on it) evidently does not fire the same way
on this ESP build (**still `0dddd435`, 33 commits behind HEAD, not
reflashed this session** — flashing the ESP was out of scope for this
Pico-focused investigation and was not attempted). The subsequent two
`kiln_call`s failing with "link or peer is down" / "destination task not
registered (NACK)" were this ESP reboot, not a Pico event — the Pico's own
`safety_get_diag` in the same window (uptime 186 s, taken moments earlier)
showed it healthy and unbroken.

The run was stopped (`profiles_stop`, `io_all_relays_off`) as soon as the
NACKs surfaced. No exact panic backtrace was pulled from the ESP coredump —
CLAUDE.md's guidance is to symbolize against the ELF matching the *running*
build, and pulling/decoding `espcoredump.py` output against a 33-commits-
stale `KilnCtrl.elf` was judged out of scope for a Pico-focused
investigation already past its core question. This ESP panic is a **new,
separate finding**, not a description of the original two-reboot incident.

## Findings

1. **The original double-reboot did not reproduce after flashing SaftyFW to
   HEAD.** One clean run (up to the point an unrelated ESP panic ended it)
   is not proof of a fix — per the brief's own instruction, this is not
   being declared solved on a single run. But the specific instability
   reported (Pico rebooting near profile start) was absent through the
   entire window in which it previously occurred twice.
2. **None of today's landed SaftyFW fixes can be named as *the* cause**,
   because the pre-flash forensic read came back empty (`scratch[5]=0`) —
   and that emptiness is itself explained by the still-missing
   `1496e288` fix's failure mode, not by an absence of a fault. The
   evidence needed to confirm which of `31741ece`/`1496e288`/`a75d88fc`/
   the config-store ABA fix (if any) was responsible was never available on
   this board; it was probably erased by the exact bug `1496e288` fixes
   before this session's SWD read could see it.
3. **New, independent defect found**: the ESP panics (`esp_reset_reason=4`)
   immediately after a per-zone thermal-guard trip abandons a firing
   (`profile_executor: zone 0 per-zone trip abandoned the whole firing`).
   This is on a stale ESP build (`0dddd435`, 33 commits behind HEAD) and may
   already be fixed upstream — not established either way. This needs its
   own follow-up: reproduce against a current ESP build, decode the
   coredump against the matching ELF, and determine whether it is a stack
   overflow, an assert, or something else in the per-zone-trip abandon path.
4. `commit_config_rejected` in the pre-flash link stats read 1 (cleared to
   0 after the flash/fresh boot) — no evidence tying it to the original
   incident was found; it was not investigated further given the ESP panic
   took priority as the more urgent live finding.

## Final state

Relays confirmed off (`io.relays=0` via `get_board_state`, and
`profiles_get_exec_status` state=0 idle) after both the flash and the
reproduction attempt. SaftyFW now running HEAD (`238e0eb9`), `boot_id=58`,
commissioning intact (S1/S8 ARMED, S14/S15 DORMANT as before), no trip
latched. User profile slot #7 restored to its original three-segment
content (40/45/60 °C) and confirmed by `profiles_get` readback. **Board
left safe and idle.**

## Next steps

- Reproduce the ESP panic deliberately with `get_device_log` watched live
  and, if it recurs, halt promptly and pull the coredump against the
  matching ELF (`espcoredump.py` vs. the archived ELF for `0dddd435`, or
  reflash the ESP to HEAD first via `flash_firmware()` and re-test against
  a current build — that also finally lands the 33 outstanding ESP commits).
- Give the Pico several more independent heating-run attempts before
  treating the double-reboot as resolved; one clean run is weak evidence
  against an intermittent fault.
- Once `1496e288` and `a75d88fc` are both confirmed flashed (done here),
  a *future* fatal fault on the Pico should leave a readable `scratch[5]`
  tag — that closes the forensic gap this investigation ran into.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
