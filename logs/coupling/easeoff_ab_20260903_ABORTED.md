# Aborted: ease-off multiplier A/B, 2026-09-03 20:19

**Do not analyse these captures and do not `--resume` this state file.**
Files are suffixed `.aborted` for that reason. 13 telemetry lines exist; no arm
completed (0/6).

## Why it was stopped

Deliberately killed ~10 minutes in, by the coordinator — not a board fault and
not a concurrent session. Two independent reasons, either one fatal:

1. **Single-zone fallback.** The runner correctly REFUSED profile 7, whose first
   segment targets 45 C, below the 48 C stabilisation hold that became the
   default in `2bddcf8`. It fell back to profile #4 `cpl_z0`, which drives only
   zone 0. That makes the analysis rule — "same metric, same direction, on at
   least 3 zones" — inapplicable, so the run could not have produced a
   judgeable result.
2. **The arms may not have differed at all.** Arm B applied a preset setting
   `ease_off_window_mult = 3.0` and started; a live read of the board
   immediately afterwards returned **2.0**. The preset apply appears not to
   write that top-level field, which would have made both arms identical.

## Diagnostic note

The final capture line reads `"reset_reason": "software (esp_restart)",
"uptime_s": 361`. That is NOT a mid-run reboot — it describes the boot caused by
a `flash_firmware()` about six minutes earlier. `reset_reason` always names the
cause of the currently-running image's boot. Check `uptime_s` against the last
known flash before concluding a restart occurred.

## Leftover state

Profile #4 still carries the prepended 48 C / 45 min stabilisation hold
(`ensure_stabilized_profile()` saves in place, by design). Left that way
deliberately: the stabilisation target itself is being revised, so the profile
needs reshaping regardless.
