# GET /api/zones JSON headroom plan (2026-09-14)

**STATUS (2026-09-14, later same day): sec 3a implemented, partially.** See
`docs/audits/zones_diag_endpoint_split_2026-09-14.md` for the full account. Summary: a new
`GET /api/zones_diag` route was added and took `coupling_tau_c%u`/`coupling_dead_time_c%u` and
`model_fit_temp_c`/`model_fit_ambient_c` off this endpoint, recovering headroom from 161 to 854
bytes (not the ~1800 bytes this plan projected). **The `tuning_*` group (927 bytes, the single
largest candidate below) was NOT moved** -- re-verifying this plan's own consumer claim against
`zones_page.html` directly found `renderTuningQuality()` actually renders all 11 `tuning_*` keys
from the operator page's single `/api/zones` fetch, contradicting sec 2c/3a's premise that this
group was diagnostics-only. Section 6's "Implemented savings: None" below is superseded for the
two groups that did move; the `tuning_*`/guard-threshold/`settings_source_groups` candidates
below are still open for whoever picks up the next field addition.

## 1. Measurement

`json_cap` in `zones_get_handler()` (`firmware/KilnFW/App/drivers/http/zones_http_get.c:93`)
is **7360 bytes** (heap, `heap_caps_malloc(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)` — not a stack
buffer, so it is not itself subject to the httpd-worker-stack hazard, but see section 3 for
why splitting/streaming would still touch that stack).

`test_zones_get_handler_max_width_response_fits_json_cap()`
(`firmware/KilnFW/App/test/test_zones_http.c:4254`) drives the real handler with every
zone/profile/relay field pinned at its documented `ZONE_*_MAX` bound
(`MAX31856_CHANNEL_COUNT` = 3 zones, `KILN_IO_RELAY_COUNT` = 4 relays, all names at their max
length). Re-running it today (`build_host_tests.ps1 -OutDir C:\wt\kfw_host_test_zonesplan`,
then running `kilnctl_host_tests_zones.exe`) prints:

```
GET /api/zones max-width render: 7199 bytes, against json_cap=7360 -- measured headroom = 161 bytes
```

**Confirmed: 161 bytes of headroom, worst case 7199 bytes.** Worst case is 3 zones (this
board's actual `MAX31856_CHANNEL_COUNT`), every field at its documented max/widest render
(all "false"/"0" boolean literals where that is the wider option, all names filled to their
max length with non-escaping characters, all floats at their maximum magnitude).

### 1a. Byte-composition breakdown (measured, not estimated)

Parsing the actual max-width response as JSON and re-serializing each top-level key in
isolation gives this composition of the 7199-byte body:

| Section | Bytes | % of body |
|---|---:|---:|
| `zones` (3 zones) | 5972 | 83.0% |
| `timing_profiles` (3 profiles) | 1005 | 14.0% |
| `safety_wiring` | 152 | 2.1% |
| `relay_names` (4 relays) | 93 | 1.3% |
| `safety_ceiling` | 84 | 1.2% |
| everything else (11 top-level scalars) | ~93 | 1.3% |

`zones` dominates, as expected — it is the only section whose per-item field count is large
AND repeats `MAX31856_CHANNEL_COUNT` times. Breaking down one zone object (1985 bytes â€” 72
keys) by field group:

| Field group | Fields | Bytes/zone | Bytes Ã— 3 zones |
|---|---|---:|---:|
| `tuning_*` record (11 keys) | valid/method/rule/settled/extrapolation_converged/tau_consistent/baseline_c/step_ambient_c/raw_rise_c/rise_inf_c/seq | 309 | 927 |
| guard thresholds (8 keys) | wrong_dir_window_s/rate/off_settle_s/runaway_rate/runaway_margin/drift_period_s/sensor_fault_debounce_ticks/frozen_window_s | 284 | 852 |
| coupling matrices (9 keys) | `coupling_c%u`Ã—3 (66) + `coupling_tau_c%u`Ã—3 (84) + `coupling_dead_time_c%u`Ã—3 (102) | 252 | 756 |
| `settings_source_groups` (nested obj, 5 keys) | limits/relaytiming/control/guards/tc | 105 | 315 |
| `model_fit_temp_c`/`model_fit_ambient_c` | 2 keys | 59 | 177 |
| zone `name` (padded to max) | 1 key | 27 | 81 |
| everything else (~44 keys: pid gains, ramp/sanity, temp bounds, timing_profile, relay/tc/ct type, fuzzy, coil_power_w, model_k_dc/tau/dead_time, coupling_diag_k_dc, ease_off/approach_rate/error_band/rate_band/progress_band, on/off fields, index/masks) | ~44 | ~949 | ~2847 |

(Row sums to 1985 modulo small rounding from JSON re-serialization not exactly matching the
handler's own `%.Nf` widths; treat the ranking, not the last digit, as authoritative.)

**Ranked byte consumers, in order:**
1. `zones` array overall (83% of the body) — of which:
   1. `tuning_*` read-only record — 927 bytes (13% of body)
   2. guard thresholds — 852 bytes (12%)
   3. coupling matrices (`coupling_c%u`/`coupling_tau_c%u`/`coupling_dead_time_c%u`) — 756 bytes (10.5%)
   4. `settings_source_groups` — 315 bytes (4.4%)
   5. `model_fit_temp_c`/`model_fit_ambient_c` — 177 bytes (2.5%)
2. `timing_profiles` — 1005 bytes (14%)
3. `safety_wiring` — 152 bytes
4. `relay_names` — 93 bytes
5. `safety_ceiling` — 84 bytes

## 2. Safe-savings candidates, ranked by risk

For each candidate, "consumers checked" lists every place searched:
`firmware/KilnFW/App/drivers/http/zones_page.html`,
`tools/PcTools/src/kilnctrl/zones_http_client.py`, `tools/PcTools/src/kilnctrl/mcp_server_control.py`,
and a repo-wide grep for the field name.

### 2a. LOW risk, not yet worth doing alone (small)

- **`tuning_baseline_c`/`tuning_step_ambient_c` at `%.1f` instead of `%.2f`.**
  Consumers checked: `zones_page.html:3032` renders both with `.toFixed(1)` — the second
  decimal digit the wire format sends is never displayed. `zones_http_client.py`'s
  `_ZONE_TUNING_READONLY_KEYS` (line ~418) treats these as an opaque preserved passthrough
  (parses to float, never reformats), so a narrower wire value is not a compatibility break.
  **Saving: up to 1 byte/field Ã— 2 fields Ã— 3 zones = 6 bytes worst case.** Too small to be
  worth a standalone change, but free to fold into a larger pass. `tuning_raw_rise_c`/
  `tuning_rise_inf_c` must stay at `%.2f` — `zones_page.html:3033` computes
  `(tuning_rise_inf_c - tuning_raw_rise_c).toFixed(2)` and a 1-decimal wire value would round
  differently in that subtraction than the 2-decimal display claims.

### 2b. MEDIUM risk, real bytes, requires a judgement call, NOT implemented here

- **`coupling_tau_c%u`/`coupling_dead_time_c%u` (6 keys/zone, 186 bytes/zone worst case, 558
  bytes total worst case) and `model_fit_temp_c`/`model_fit_ambient_c` (177 bytes total).**
  Consumers checked: neither pair appears anywhere in `zones_page.html` by name (grepped
  directly, zero hits) — the operator-facing page does not render them. They ARE consumed:
  `tools/PcTools/src/kilnctrl/zones_http_client.py` treats `coupling_tau_c%u`/
  `coupling_dead_time_c%u` as part of its fetched/round-tripped zone dict (exercised by
  `tools/PcTools/tests/test_zones_http_client.py`), and `mcp_server_control.py` reads
  `model_fit_temp_c`/`model_fit_ambient_c` directly for the gain-schedule use case
  (`docs: gain_scheduling_design_2026-09-13.md`'s consumer). **This is exactly the "move to a
  diagnostics-only endpoint" case in section 3, not a deletion** — the data is real and used
  by tools, just never by the page a human loads. Do not remove; consider relocating (3b).

- **`%.4f` precision on `pid_kp`/`pid_ki`/`pid_kd`, `model_k_dc`, `coupling_c%u`,
  `coupling_diag_k_dc`, `autotune_baseline_k_dc`, `rate_band_c_per_s`.**
  These are the fields this codebase's own comments explicitly justify at 4 decimals
  ("`%.4f` matches model_k_dc's own precision -- same unit, same small-gain-zone concern...
  the feedforward divides by it" — `zones_http_get.c:337-339` and repeated at
  `coupling_diag_k_dc`/`autotune_baseline_k_dc`). Reducing precision here is not a formatting
  change, it is a **real information loss** in a value a control loop divides by. Consumers
  checked: `zones_http_client.py` round-trips these as opaque floats (would accept fewer
  digits without erroring), `zones_page.html` displays several with `toFixed(2)`/`toFixed(3)`
  in various places — a wire value with fewer than the currently-guaranteed digits could
  under some inputs display as `0.00` where today it shows a small nonzero gain. **Judgement
  call: do not implement.** If this is ever revisited, it needs a numerical-impact review of
  `adaptive_tune_refine_zone_locked()`'s plausibility-ratio test (the documented consumer of
  `autotune_baseline_k_dc`'s precision), not a byte-counting one.

- **`settings_source_groups` (105 bytes/zone Ã— 3 = 315 bytes) is per-zone but its 5 group
  values are frequently identical across zones in practice** (an operator who has not
  overridden per-zone settings has every zone reporting the same group values). It is NOT
  provably constant across zones by the schema, though (`zones_config_json.h`'s
  `settings_source[SRC_GROUP_COUNT]` is a genuine per-zone array, and
  `docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs`
  is explicit that a zone can diverge from another). Hoisting it out to "emit once if all
  zones agree, else per-zone" would save up to 210 bytes in the common case but is exactly
  the kind of "constant across zones" transformation the task calls a judgement call, since
  `zones_page.html` and `zones_http_client.py` would both need updating to handle the
  divergent case, and it reduces worst-case (all-different) headroom by 0 while helping only
  the typical case. **Not implemented** — flagging for whoever owns this field's design
  next, since fixing it well requires deciding the schema for "some zones differ."

### 2c. No safe candidate found for the two biggest single groups

The `tuning_*` record (927 bytes, 13%) and the 8 guard-threshold fields (852 bytes, 12%) are
both **already documented as always-emitted, read-back-and-repost fields** in
`zones_http_get.c`'s own inline comments, and both are exercised end-to-end by
`zones_http_client.py`'s round-trip tests. No unused field was found in either group by
grepping every consumer. These are the fields most worth a structural fix (3), not a
formatting one.

## 3. Structural alternatives

### 3a. Split the endpoint (recommend)

Move the read-only, tool/diagnostics-oriented fields — `tuning_*` (11 keys, 927 bytes),
`model_fit_temp_c`/`model_fit_ambient_c` (177 bytes), `coupling_tau_c%u`/
`coupling_dead_time_c%u` (558 bytes), `autotune_baseline_k_dc` (already flagged
write-only-until-recently in `docs/audits/zones_get_autotune_baseline_exposure_2026-09-13.md`)
— to a new `GET /api/zones_diag` (or similar) route, leaving the operator-facing
`GET /api/zones` with only the fields `zones_page.html` actually renders plus whatever
`zones_http_client.py`'s ordinary read/write cycle needs.

**Effort:** moderate. A new httpd route + handler in the same `zones_http_*.c` family, sharing
`zones_get_safety_wiring()`/`zones_json_escape()` etc. Removing ~1662 bytes from the main
response's worst case (927 + 177 + 558) would take worst-case render from 7199 to roughly
5537 bytes against the SAME 7360 cap — over 1800 bytes of headroom, undoing several
generations of field-addition pressure in one move.

**Risk against this codebase's specific hazards:** LOW. This does not change how the httpd
worker task handles either request — each remains one `heap_caps_malloc` + one
`httpd_resp_send`, same pattern as today, so it does not touch the "large stack locals on the
shared httpd worker stack" hazard class (`project_httpd_stack_blob_class`,
`project_httpd_stack_near_overflow` in memory) at all — no new stack buffer, no chunked I/O.
The one thing that DOES need checking: `tools/PcTools/check_zones_per_zone_field_drift.ps1`
(owned by another agent in this session, not touched here) very likely enumerates
`GET /api/zones`'s per-zone keys against some reference list — a split would need that check
(or its reference list) updated to know two endpoints exist, otherwise it will either miss
the moved fields entirely or flag their absence as drift. Also: `zones_http_client.py`'s
fetch-and-repost cycle currently gets these fields from ONE response; splitting means either
(a) it fetches both and merges before re-posting (POST already ignores most of these as
read-only/preserved, per section 1's field-group table, so this is mechanical) or (b) the new
diagnostics route also needs to accept whatever passthrough-preserve semantics
`autotune_baseline_k_dc` currently has via `zones_http_post_parse.c`.

### 3b. Streaming / chunked response

`httpd_resp_send_chunk()` instead of building the whole body in one heap buffer first. Would
remove the 7360-byte cap entirely (no single buffer to size).

**Effort:** substantial. Every `APPEND()` call site becomes a `httpd_resp_send_chunk()` call
(or accumulates into a much smaller rotating buffer flushed periodically) — this is a rewrite
of `zones_get_handler()`'s body, not a parameter change.

**Risk against this codebase's specific hazards:** HIGHER than the split. This repo has a
documented httpd wedge from an uncounted socket
(`project_httpd_wedge_is_dram` / `httpd_socket_budget_has_headroom` test) — a chunked
handler holds its `httpd_req_t`/socket open across many more scheduler-visible steps than a
single `httpd_resp_send()` does (one send call vs. N chunk calls, each a potential blocking
point and each an opportunity for a slow/stalled client to hold the socket open far longer
than today's single-shot response), which changes this endpoint's contribution to the
"how many sockets can be open/half-finished at once" budget that check already audits. Before
attempting this, `httpd_socket_budget_has_headroom` (or its production analog) would need to
be re-evaluated with "one more long-lived streaming responder" as an explicit case, and the
error-path story (what happens to a partially-chunked response if a later `APPEND` would have
hit `truncated:` today) needs a real answer — a chunked response has no way to retroactively
turn into a clean 500 once the first chunk (and a 200 status) has already gone out.

### 3c. Paginate by zone

`GET /api/zones?zone=0`, three (or `thermo_count`) requests instead of one array.

**Effort:** moderate, similar shape to 3a. **Risk:** LOW on the httpd-hazard axis (same
single-buffer-per-request shape as today, just smaller per request), but HIGHER on the
consumer-breakage axis — `zones_page.html` currently does one fetch and renders the whole
table; paginating means every consumer (page AND `zones_http_client.py`) must learn to issue
multiple requests and merge, which is a much bigger client-side change than 3a's "read from
one more endpoint sometimes." Not recommended as the primary path for that reason — 3a gets
most of the same headroom win (the big fields move out) without forcing every consumer to
change its fetch shape for fields it already ignores.

## 4. Recommendation

**Criteria:** (a) recovers real, multi-generations-of-headroom bytes rather than a one-field
patch; (b) does not touch the httpd-worker shared-stack or socket-budget hazards this repo has
already been bitten by twice; (c) does not require every consumer to change how it fetches
data, only to optionally read a second endpoint for fields it mostly ignores today.

- **Primary: 3a, split the read-only/diagnostic fields to a new `GET /api/zones_diag` route.**
  Meets all three criteria, recovers ~1662 bytes (23% of the current worst-case body) in one
  pass, and the removed fields are exactly the ones every consumer search in section 2b
  showed are read by tools, not by the operator page — the natural diagnostics/tooling split
  the task anticipated.
- **Fallback: 2a (trim `tuning_baseline_c`/`tuning_step_ambient_c` to `%.1f`) plus revisiting
  2b's `settings_source_groups` hoist** if 3a is deferred and another field needs to land
  before it's done. Together these recover on the order of 6-200 bytes depending on whether
  zones actually agree at runtime — enough for one more small field, not a durable fix.

Do **not** pursue 3b (streaming) unless 3a and 2's savings are both exhausted and the socket-
budget re-audit it requires has actually been done — its risk profile is the worst of the
three structural options against this specific codebase's history.

## 5. Failure-message improvement (implemented)

`test_zones_get_handler_max_width_response_fits_json_cap()`
(`firmware/KilnFW/App/test/test_zones_http.c`) previously failed with a bare
`"max-width GET /api/zones response must fit inside json_cap with room to spare"` (or, for the
realistic overflow shape, `"max-width GET /api/zones must NOT hit the handler's own
truncation path"`) and no pointer to why or what to do about it.

Both `TEST_CHECK`s in that test now share one message, built with `snprintf` before either
check runs, that reports the actual rendered byte count, `json_cap`, which of the two failure
shapes was hit (mid-render truncation by the handler's own `APPEND` guard vs. an outright
length overflow), states this document's finding that headroom was 161 bytes before this
attempt, explicitly says **do NOT enlarge `json_cap`** and names the two-panics/standing-rule
reason why, and points at this file
(`docs/audits/zones_json_headroom_plan_2026-09-14.md`) for the ranked savings and structural
options above.

**Negative-tested:** a field
(`"NEGATIVE_TEST_OVERFLOW_FIELD_DELETE_ME":"AAAA...(230 x 'A')..."`) was added to
`zones_http_get.c` right after `coil_power_w`, confirmed to make the test fail with the new
message (`FAIL ... max-width GET /api/zones must fit inside json_cap without hitting the
handler's own truncation path (rendered 127 bytes; json_cap is 7360 bytes; measured headroom
before this field addition was 161 bytes; this attempt overflowed mid-render and was
truncated by the handler itself). Do NOT enlarge json_cap ...`), confirming the realistic
overflow shape (mid-render truncation, not a clean length overflow) is exactly what the
message now describes. The field was then removed by hand and the host-test build directory
was deleted and rebuilt from clean before re-measuring, per this repo's standing rule for
restoring after a negative test — the restored build reproduces the original 7199-byte /
161-byte-headroom measurement exactly.

## 6. Implemented savings

None. Every candidate identified in section 2 either (a) is too small to be worth a
standalone change (2a, ~6 bytes), or (b) requires a judgement call this task explicitly
declines to make unilaterally (2b's precision and `settings_source_groups` candidates). Per
the task's own instruction, these are documented, not implemented, and left for whoever picks
up the actual next field addition (or 3a) to decide with full context.
