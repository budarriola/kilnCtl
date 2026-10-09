# Live profile edit - pending work

Pending items only. The design, the owner decisions, the HTTP surface and the delivered LCD pages live in `docs/LIVE_PROFILE_EDIT.md` (same section numbers). The feature is delivered; what is left is bench verification.

## Bench verification on a non-heating firing (gate: hardware, a session authorized to start one)

Never completed: the 2026-09-20 attempt on `cd6073e5` stopped at the permission layer. The LCD Edit page had a first PASS 2026-10-01 on `eb83c1ac`. Steps:
1. Start a non-heating firing (zone relays off or monitor-only); `profile_live_get` shows no fork yet.
2. `profile_live_fork`, then `profile_live_edit` with a future-segment change: accepted. An edit above the zone ceiling: 400. An edit below the running segment index or changing its `seg_kind`: 409.
3. Confirm the executor adopts the change (log line "live profile edit adopted").
4. End the firing: `pending_decision` true; `profile_live_decide` with discard, save_as and overwrite (overwrite of a builtin origin refused 403).

## Section 11 firing-only list (gate: hardware, real firing)

Unverified until a real firing: setpoint genuinely continuous through a pickup; a mid-run swap does not disturb ramp-lock across more than one real zone; the end-of-firing prompt appears after a genuine DONE hours later; a pickup refused by the ceiling re-check leaves the firing running undisturbed.

## LCD Keep?/Discard case (gate: hardware)

Write and run a bench case that drives the LCD Keep? button and decide page (PIN gate, Discard confirm, Save as, Overwrite) with `capture_lcd.ps1`. LCD-24 only discards over HTTP. `tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py` was owned by another agent at the time of writing: coordinate before editing.

Closed, no longer pending (`ROADMAP.md`): the stray "LiveEditTest" profile was gone by 2026-10-03, and the 2026-10-04 administrator login with the bench env-var credentials succeeded, so the stored `kiln_auth` record and the env vars agree. The login hang on `cd6073e5` was fixed and verified in 2026-09 (`d4e59c02`, `77e90ad7`).
