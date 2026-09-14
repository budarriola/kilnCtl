# Recheck of conclusions drawn under the stale `kilnctrl` MCP server, 2026-09-14

Audit only. Read-only board access (`kiln_call`, `wifi_get_status`, one direct
`GET /api/zones`). No firmware, config, or board state changed; no board
flashed; no heating run.

## 1. The window, precisely

The `kilnctrl` MCP server process (pid serving 8767) was started at commit
`fd8d7b93` (2026-09-11 07:44:48 -0700, "Test H2: is the 62-75C coupling
under-prediction a mixed-provenance artifact?") and kept running, unrestarted,
until tonight. `docs/audits/plant_model_loss_investigation_2026-09-14.md`
restarted it after confirming no firing in progress; the restart landed on
commit `13f98665` (2026-09-14 12:05:10, "Adversarial review of the zones_diag
split..."), matching what `mcp_servers.ps1 status` reports right now: the
running process started **2026-09-14 13:38:58** at commit `13f98665`.

**So the affected window is 2026-09-11 07:44:48 through 2026-09-14 13:38:58 —
just over 3 days, not the 26 minutes between `fd8d7b93` and `a605df46` alone.**
That 26-minute figure (quoted in this task's brief) is only how quickly the
first fix landed after the server started; the server did not pick it up
until three days later.

Commits touching `tools/PcTools/src/kilnctrl` in that window that the stale
process therefore never served, in landing order:

| commit | date | what changed |
|---|---|---|
| `a605df46` | 09-11 08:10 | Fixed `control_get_zones()`'s hardcoded claim that `zone_coupling_use_measured_diag_k_dc()` "is compiled false" (it is compiled `true`) |
| `a465db75` | 09-13 23:36 | `control_get_zones()`: added the whole "plant model" section (`model_k_dc`/`tau`/`dead_time`/`tuning_valid`/`model_fit_temp_c`/`model_fit_ambient_c`) — previously rendered nothing here at all |
| `6bc3958c` | 09-14 00:36 | Client-side field-table mirror fix for `autotune_baseline_k_dc` (check-suite only, not user-visible tool output) |
| `f59b21c8` | 09-14 09:05 | Split diagnostics-only zones fields onto new `GET /api/zones_diag` |
| `0dae6a3c` | 09-14 11:14 | Added `fuzzy_model_valid` field |
| `13f98665` | 09-14 12:05 | Fixed a failed `zones_diag` fetch silently rendering as an absent field (now caught, restart picked this one up) |

Every `control_get_zones` call in the 3-day window is affected by whichever
of these commits postdated `fd8d7b93` at the time of that call. In practice
the two that matter for conclusions on file are `a605df46` (wrong flag
string, present for the whole window) and `a465db75` (missing model section
entirely, present until 09-13 23:36).

## 2. Claims enumerated

Searched `docs/audits/` (all files dated 2026-09-11 or later) for citations
of `control_get_zones`, `GET /api/zones`, `k_dc`, `coupling_use_measured_diag`,
`autotune_baseline_k_dc`, `fuzzy_model_valid`, and `zones_diag`. Filtered to
documents that draw a conclusion from a **live tool reading** taken inside the
window (2026-09-11 07:44 through 2026-09-14 13:38), as opposed to documents
that only discuss source code or design.

| # | Claim | Document | Reading taken |
|---|---|---|---|
| C1 | `s_coupling_use_measured_diag_k_dc` is compiled `true`, board-wide, no remaining private copy | `coupling_measured_diag_flag_audit_2026-09-11.md` (`ef8a374f`) | Source read (`zone_coupling_solve.c`), not the tool |
| C2 | Live `model_k_dc`/`tau`/`dead_time` match `coupling_diag_k_dc` and the `b64fe09d` measured constants, all three zones | Same doc | Raw `GET /api/zones` (bypassing the MCP tool) + `control_get_zones` for `coupling_diag_k_dc` only |
| C3 | The flag is a no-op on this board today (both sources already agree) | Same doc | Derived from C1+C2 |
| C4 | `control_get_zones`'s own hardcoded string calling the flag "compiled false" is itself stale/wrong | Same doc | Direct read of `mcp_server_control.py:120-122` (pre-fix) |
| C5 | `control_get_zones` never rendered any plant-model field, despite its docstring's claim | `mcp_zone_model_fields_2026-09-13.md` (`a465db75`) | Live `kiln_call(control_get_zones)`, 2026-09-13, pre-fix |
| C6 | `model_fit_temp_c`/`model_fit_ambient_c` read `-273.15` (UNKNOWN) on all three zones | Same doc | Raw `GET /api/zones` (bypassed the tool deliberately, since it couldn't render the field yet) |
| C7 | `autotune_baseline_k_dc` is "NOT exposed by GET /api/zones" | Same doc, and repeated in `zones_get_autotune_baseline_exposure_2026-09-13.md` and inside `control_get_zones()`'s own output string (`mcp_server_control.py:345`) | Source read of `zones_http_get.c` (true at the time) |
| C8 | The live bench board has all three zones autotuned, non-zero `model_k_dc` (42.731/32.397/33.849) | `fuzzy_no_model_no_fuzzy_2026-09-14.md` | **No reading at all** — restated `zones_post_model_key_omission_2026-09-13.md`'s explicitly-hedged `coupling_diag_k_dc` observation as if it were a `model_k_dc` observation |
| C9 | `coupling_diag_k_dc` reads 42.7310/32.3969/33.8493, "consistent with (but not proof of)" intact `model_k_dc` | `zones_post_model_key_omission_2026-09-13.md` (`6093b8b6`) | `kiln_call(control_get_zones)`, 2026-09-13, hedged deliberately |

## 3. Re-verification against ground truth now

Re-read live tonight via both the restarted MCP server (`kiln_call`,
commit `13f98665`) and an independent direct `GET http://192.168.1.156/api/zones`.
Both agree: `model_k_dc`/`model_tau_s`/`model_dead_time_s` =
`42.731/255.6/40.3`, `32.397/258.9/31.3`, `33.849/247.1/26.0` for z0/z1/z2;
`coupling_diag_k_dc` matches the same three k values; `fuzzy_model_valid=true`
on all three; `autotune_baseline_k_dc=0.0` (raw, unresolved sentinel).

| Claim | Verdict | Basis |
|---|---|---|
| C1 (flag compiled true) | **CONFIRMED** | Drawn from direct source read, not the stale tool — the staleness never touched this claim's evidentiary basis. Still true in source today. |
| C2 (diagonals match `b64fe09d`) | **CONFIRMED, with a timing correction** | `plant_model_loss_investigation_2026-09-14.md` establishes the model fields were zeroed by a whole-page-save incident on 2026-09-11, evidenced at 09:29 (`36f88d62`) — *after* this audit's own read at 07:42-ish. So the 42.7310/... reading was genuine and accurate at the moment it was taken, not an artifact of staleness (it came from raw HTTP, which the MCP staleness never affected anyway). The model was lost roughly 1-2 hours later and has since been restored to the identical values by `137dea1a`'s repair, confirmed again tonight by both paths above — so the claim also holds again today, coincidentally by restoration to the same numbers, not by having been continuously true. |
| C3 (flag is currently a no-op on this board) | **CONFIRMED** | Follows from C1+C2, both confirmed; independently re-derivable from tonight's own readings since the values still agree. |
| C4 (tool's own string was stale) | **CONFIRMED** | This is exactly the defect `a605df46` fixed; verified by reading `mcp_server_control.py`'s current source, which now derives the claim live from `zone_coupling_solve.c` (confirmed in tonight's `control_get_zones` output: "...compiled true in this source tree (read live from zone_coupling_solve.c, not hardcoded here)"). |
| C5 (tool rendered no model section) | **CONFIRMED** | True of the code at the time (the rendering code did not exist until `a465db75`); not an artifact of which server process was running, since even a freshly-started server before that commit would have shown the same gap. `a465db75`'s own commit message additionally notes its live-confirmation-via-restart was deliberately deferred (an active firing was in progress), so this claim was never asserted as an MCP-tool-verified fact in the first place — the fix was verified via mocked HTTP tests, not a live read. |
| C6 (`model_fit_temp_c` = UNKNOWN sentinel) | **CONFIRMED** | Read via raw HTTP outside the tool at the time (deliberately, per the doc's own text), and still reads UNKNOWN today via both paths — unaffected by the MCP staleness since the tool wasn't the source. |
| C7 ("`autotune_baseline_k_dc` NOT exposed") | **REFUTED** (already known-refuted, still shipping) | `zones_http_get.c` emits `autotune_baseline_k_dc` today — `137dea1a` confirmed it reads `0.0` over raw HTTP during its own investigation. `control_get_zones`'s **own live output tonight still prints the retracted "NOT exposed... as of 2026-09-13" line verbatim** (`mcp_server_control.py:344-349`, reproduced by this pass's own `kiln_call` above), because nobody has patched that hardcoded string yet, despite three separate documents (`plant_model_loss_investigation_2026-09-14.md` §5, `zones_get_autotune_baseline_exposure_2026-09-13.md`'s own "Follow-up" section, and this one) all naming it as stale. This is not new information — it is a known, named, still-open defect, restated here because the task asked specifically to check for this class. |
| C8 (asserted `model_k_dc` values as fact) | **REFUTED** | Already identified and named by `plant_model_loss_investigation_2026-09-14.md` §2 as a laundered hedge ("a retraction hid the stale claim" class). No `model_k_dc` reading ever supported this sentence at the time it was written — `coupling_diag_k_dc` was misread as `model_k_dc`. Confirmed here only to close the loop: this pass found no additional instance of the same field confusion beyond the one `137dea1a` already caught. |
| C9 (`coupling_diag_k_dc`, explicitly hedged) | **CONFIRMED**, and the hedge was correct | The doc explicitly declined to claim this proved `model_k_dc` was intact ("not a substitute for reading the actual fields") — which is exactly right, since `model_k_dc` was in fact already zero or about to become so. The `coupling_diag_k_dc` numeric reading itself is independently confirmed by tonight's raw HTTP fetch. |

No claim in this list falls into **CANNOT-RECHECK**: the board's zone
configuration state relevant to every claim above is either still live and
directly re-readable, or (for C2/C6) was read via raw HTTP at the time,
independent of the MCP staleness this task is about.

## 4. `s_coupling_use_measured_diag_k_dc` specifically

Established in §3: `coupling_measured_diag_flag_audit_2026-09-11.md`'s
conclusion that the flag is compiled `true` came from a **direct source
read** of `zone_coupling_solve.c:303-312`, not from the stale MCP tool — the
audit explicitly flags the tool's own string as wrong in the same breath
(C4). The numeric diagonal-matching conclusion (C2) came from raw
`GET /api/zones`, also independent of the MCP tool, with `control_get_zones`
used only to cross-check `coupling_diag_k_dc` — a field no commit in the
window's list touched. **Both halves of this audit's reasoning are
trustworthy and unaffected by the staleness**, despite being dated inside the
window and despite quoting the exact numbers that later turned out to belong
to a different field name (`coupling_diag_k_dc`, not `model_k_dc`) when a
*different* document (`fuzzy_no_model_no_fuzzy_2026-09-14.md`, C8) misquoted
them. The field-confusion defect belongs to C8 only; `ef8a374f`/`coupling_
measured_diag_flag_audit` itself never conflates the two fields — it
tabulates them as separate columns (§2's table has both `model_k_dc` and
`coupling_diag_k_dc` side by side) and states outright that `model_k_dc` "was
overwritten to the `b64fe09d` joint values... not via `autotune_engine_guard.c`'s
normal single-zone-fit write path," i.e. it already suspected the two fields'
agreement was not architecturally guaranteed, well before the loss was
discovered.

## 5. Other hardcoded compile-time/firmware-state claims in `tools/PcTools/src/kilnctrl/`

Grepped `mcp_server_control.py` and the rest of the `kilnctrl` tool source
for the same class of defect `a605df46` fixed: an English string asserting
something about compiled firmware state or protocol coverage, independent of
a live derivation.

- **`mcp_server_control.py:344-349`** — `"autotune_baseline_k_dc: NOT exposed
  by GET /api/zones as of 2026-09-13..."`. **Currently wrong** (§3, C7) and
  not derived from source or a live probe — a plain hardcoded string, same
  shape as the flag string `a605df46` fixed. Not fixed here (out of this
  pass's "audit only, one document" scope) but named explicitly per the
  task's instruction.
- **`mcp_server_control.py:109-182`** (`_read_coupling_use_measured_diag_k_dc_compiled_value()`)
  and **`:242-272`** (`_read_zone_model_fit_temp_unknown_sentinel()`) —
  these are the two claims already converted to live source-derivation (by
  `a605df46` and `a465db75` respectively), each with an explicit fallback
  path and an honest "falls back to the known literal only if..." comment.
  Not defects; listed for completeness since they are the fixed counterparts
  of the same class.
- No other instance of this specific pattern (a hardcoded assertion about
  *compiled* firmware behavior or *current* HTTP field coverage) was found
  elsewhere in `tools/PcTools/src/kilnctrl/` — the broader "no longer"/
  "used to be"/"deprecated" grep across the whole package turned up only
  ordinary changelog-style comments describing past code shape, not
  present-tense claims about what firmware does today.

## 6. Recommended guard

The information to catch this already existed and was already being computed
(`kiln_help()` and `mcp_servers.ps1 status` both print `fresh`/`stale` off
`/health`) — the failure was that nobody looked at it before trusting a
reading for three days. Two options considered:

- **Make every tool response carry its own freshness flag inline**, e.g.
  prefix a stale server's tool output with a `STALE (N files changed since
  <start>, restart via mcp_servers.ps1 restart)` banner, generated the same
  way `kiln_help()` already computes it. This directly attacks the actual
  failure mode: a person reading `control_get_zones`'s *output* never had to
  separately think to check `kiln_help()` or `status` first, and in every
  affected document here, nobody did. This is a small, mechanical change
  (the health-check logic already exists; it just needs to run before
  *every* tool call, not only when `kiln_help()`/`status` are explicitly
  invoked) and is worth doing — it converts staleness from
  "visible-if-asked" to "impossible-to-miss," which is the task's own
  framing of what's needed.
- **Auto-restart the server on detected staleness before serving a call.**
  Considered and rejected: this project deliberately avoids auto-restarting
  a long-running board-connected server mid-operation (an active firing must
  never have its control link interrupted — `mcp_zone_model_fields_2026-09-13.md`'s
  own restart was explicitly deferred for exactly this reason). An automatic
  restart could not safely distinguish "safe to restart now" from "a firing
  is in progress" without the same operator judgment a banner would prompt
  anyway, so it adds real risk without removing the need for a human to look.

**Recommendation: add the inline per-call staleness banner, do not
auto-restart.** It is a small addition (reuses existing `/health` logic),
directly closes the gap that let a 3-day-stale server go unnoticed (every
affected document here read a tool's output without separately checking
`kiln_help()`), and preserves the deliberate human-in-the-loop restart
decision this project already relies on around active firings.

## Verified hashes

- `fd8d7b93` — `git cat-file -t` -> `commit` (server's stale starting commit)
- `a605df46` — `git cat-file -t` -> `commit` (coupling-flag string fix)
- `a465db75` — `git cat-file -t` -> `commit` (model section added to `control_get_zones`)
- `6bc3958c` — `git cat-file -t` -> `commit` (autotune_baseline_k_dc client mirror fix)
- `f59b21c8` — `git cat-file -t` -> `commit` (zones_diag split)
- `0dae6a3c` — `git cat-file -t` -> `commit` (fuzzy_model_valid added)
- `13f98665` — `git cat-file -t` -> `commit` (restart-landing commit, current running server)
- `137dea1a` — `git cat-file -t` -> `commit` (plant-model loss investigation and repair)
- `ef8a374f` — `git cat-file -t` -> `commit` (coupling-flag audit)
- `36f88d62` — `git cat-file -t` -> `commit` (whole-page save zeroing incident, cited by `137dea1a`)
- `6093b8b6` — `git cat-file -t` -> `commit` (zones POST model-key omission audit)

## Bottom line

- Real affected window: **2026-09-11 07:44:48 to 2026-09-14 13:38:58** (~3
  days), not the 26-minute figure alone.
- **9 claims enumerated** across the audits that cite an MCP zones reading
  taken inside that window: **7 CONFIRMED, 2 REFUTED, 0 CANNOT-RECHECK.**
- Refuted, named: **C7** (`control_get_zones` still prints the retracted
  "`autotune_baseline_k_dc`: NOT exposed" line — the field has been emitted
  by `GET /api/zones` since before that string was even written) and **C8**
  (`fuzzy_no_model_no_fuzzy_2026-09-14.md`'s flat assertion of non-zero
  `model_k_dc` values, which no reading ever supported — it silently
  restated `zones_post_model_key_omission_2026-09-13.md`'s explicitly hedged
  `coupling_diag_k_dc` reading as fact; `plant_model_loss_investigation_2026-09-14.md`
  already caught this independently).
- The flag conclusion the task specifically asked about
  (`s_coupling_use_measured_diag_k_dc` compiled true, diagonals matching) is
  **CONFIRMED** on both counts, and was reached by direct source/raw-HTTP
  reads rather than the stale tool's own (wrong) string — the audit that drew
  it flagged the tool's defect in the same document rather than being fooled
  by it.
- One more hardcoded firmware-state string was found beyond the one this
  task named (`mcp_server_control.py:344-349`, the `autotune_baseline_k_dc`
  "NOT exposed" line) — already independently named by two other documents,
  restated here as requested; not fixed in this pass.
- Recommended guard: an inline per-tool-call staleness banner reusing the
  existing `/health`-derived fresh/stale check, not an automatic restart.
