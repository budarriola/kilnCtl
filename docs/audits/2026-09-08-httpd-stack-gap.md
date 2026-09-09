# httpd_worker stack: static-vs-live gap (2026-09-08)

## Question
Static walk (`check_httpd_task_stack_budget.py`) says the worst reachable
handler path uses 4832 B of the 8192 B `httpd_worker` stack (revert_post_handler,
via `adaptive_tune_revert -> zones_config_set_model -> nvs_save ->
zones_config_cfg_fs_save -> zones_config_json_compute_crc`), implying ~3360 B
free. Live board (build `4bbfcfbe`, verified `0f08ae6b`) reported 1656 B free
at the time this task was assigned; a fresh boot measured during this pass
(uptime 188 s) showed **1528 B free, 18.7% headroom, classified LOW** — worse,
not better, and already fixed within the first 3 minutes of uptime.

## 1. Units / semantics of the live number
Confirmed correct, not a units bug. `stack_margin.c` reads
`uxTaskGetStackHighWaterMark()` and `stack_margin_calc.h` documents (with a
citation to ESP-IDF's own `task.h`) that **ESP-IDF's port already returns
bytes**, not words — a prior revision of this file multiplied by 4 assuming
vanilla FreeRTOS semantics and was caught because it produced >100% headroom,
which is impossible. `STACK_MARGIN_WORD_BYTES` is `1` today, correctly. The
figure is also confirmed to be a **high-water mark (worst-ever-since-boot)**,
not an instantaneous reading — `stack_margin_classify()`'s own comment states
this explicitly, and `test_stack_margin.c` pins the conversion behavior. So
the reported 1528/1656 B are genuine worst-case bytes, and the ~1700-1830 B
gap against the static model's implied ~3360 B is real.

## 2. What the static walk cannot see (enumerated)
The script's own header is explicit that it under-estimates, for these
reasons, in decreasing likely contribution:

- **ESP-IDF httpd dispatch overhead before the handler runs is not counted at
  all.** Each handler is its own root (the real root — the httpd worker loop
  calling through a function-pointer table — can't be walked). Session/request
  parsing, `httpd_uri`, header/query parsing, and the transport read loop all
  run in frames *below* the measured root and are invisible to this tool.
- **ISR / window-overflow spill onto the task stack is not modelled at all**
  (stated as a limit). Xtensa's windowed register ABI spills register windows
  onto whichever stack is current when a window overflow/underflow exception
  fires; on a task making deep calls under load (SPI/I2C ISRs, the safety UART
  ISR, LVGL timer ISR) this is not bounded by the static frame-size walk,
  which only sums `entry aN,N` immediates.
- **Compiler-inserted spills/extra sp adjustment** beyond the `entry`
  instruction's declared frame size are not separately re-verified in this
  pass (no evidence found of a specific case, but the walk trusts the `entry`
  immediate as the whole frame).
- **Indirect calls are not followed** — `cJSON` allocator/printer callbacks,
  and the URI dispatch table itself, are function-pointer calls. Any large
  frame reachable only through one of these is invisible.
- **ESP_LOGx/`vsnprintf`-based formatting** used throughout these handlers for
  diagnostics involves its own hidden stack usage (format string layout,
  varargs) that is easy to under-count if the callee has no stack-affecting
  local by the entry-immediate metric alone but does real work in a stdlib
  frame the walk still should have caught if compiled in — this was not fully
  isolated in this pass and is flagged as unresolved.
- **Recursion is cut at first repeat** — not believed relevant here (no
  recursive path found in the affected handlers).

## 3. Which handler actually set the live mark
Live testing this pass (fresh boot, uptime 188 s) found `httpd_worker`
already at its worst observed mark (1528 B free) essentially immediately —
well before any of the deliberately-deep handlers below were exercised.
Read-only probes of known-deep paths did **not** move the mark further:

| probe | static depth | result | margin after |
|---|---|---|---|
| `GET /api/cfgfs` | 4304 B (3rd deepest) | 200, 2850 B | 1528 B (unchanged) |
| `GET /api/backup/export` | not in top 5 | 200, 4828 B | 1528 B (unchanged) |
| `GET /api/profile?id=0` | shallow | 200, 356 B | 1528 B (unchanged) |
| `GET /api/history.csv` | shallow | 200, 117 B | 1528 B (unchanged) |
| `GET /api/firing_history`, `/api/cfgfs/file` | n/a | 400 (bad args), not reached | 1528 B (unchanged) |

Conclusion: the mark was already set by **routine startup/UI traffic**
(dashboard/status polling that runs continuously from the moment the httpd
server comes up, likely including local page loads) before any deliberate
probing began, at 188 s of uptime. `revert_post_handler`/`ct_cal_post_handler`
(mutating, safety-config endpoints) were deliberately NOT exercised in this
read-only pass — they are plausible next-worse candidates but require a
config write this task is scoped to avoid.

Given `cfgfs_status_get_handler` (statically the 3rd-deepest at 4304 B) ran
and did not move a mark already sitting at 1528 B (8192-4304=3888 nominal
free by the static model alone), the ~1700 B unaccounted-for gap is
consistent across handlers, not specific to one path — supporting "static
model is missing a roughly constant per-request overhead" (dispatch + ISR
frames) rather than "one path is mismeasured."

## 4. Is the budget honest?
**No — not as currently framed.** `CEILING_BYTES = 4832` is treated as if it
tells you free-stack headroom (`8192 - 4832 = 3360 B nominal`), but live
measurement shows real worst-case free stack is closer to **1500-1700 B**,
i.e. the static model is missing roughly **1700-1900 B** of real usage that
never shows up in any handler's frame sum. That gap is now attributed (best
evidence available without instrumenting the dispatcher itself) to
ESP-IDF's own httpd dispatch/session-parsing overhead plus ISR window-spill,
neither of which the tool can see by design.

Recommendation (not yet implemented in this pass, since it is a check/tooling
change, not a firmware behavior change, and does not require a reflash):
add an explicit, documented safety margin to `CEILING_BYTES`'s *interpretation*
— e.g. treat anything statically over ~2500 B (8192 - 1700 live-observed
overhead - 4000 B desired live floor) as failing, or, more directly, change
the script's printed "implies N B free" framing to subtract a stated
"~1800 B unmodelled dispatch/ISR allowance" so nobody reads the 3360 B nominal
figure as real margin. This is a check/doc fix, not a firmware change — no
reflash needed for it.

## 5. Verdict
`httpd_worker` is **marginal, not comfortable, and not yet panicking**:
live headroom is 18.7% (1528 B of 8192 B), which the module's own
`stack_margin_calc.h` classifies LOW (between 15% CRITICAL and 30% LOW
cutoffs) — one more handler-depth regression of a few hundred bytes pushes it
into CRITICAL (<15%, <1229 B), and the static ceiling check alone would not
catch that regression early enough because it does not know about the ~1700 B
already consumed by mechanisms outside its model. No panic has occurred
today from httpd_worker specifically (the panics referenced in the task
prompt were the `safety_poll`/thermo-slot corruption class, unrelated). This
class should be considered still open until either the ~1700 B gap is
mechanically explained (e.g. by instrumenting `httpd_uri_dispatch` stack
depth directly) or the ceiling is retuned to reflect the true live number
with margin.
