# Review: IRAMGATE (internal RAM drift fixes and allowlist gate)

Date: 2026-10-13. Reviewer: Claude Opus 5.5 (adversarial review, no subagents).

Scope, all on origin/dev:

- `1e5970ce7`: the code change.
  - Moves `kiln_cfg_store.c` `s_store` to PSRAM with `EXT_RAM_BSS_ATTR`.
  - Routes transient buffers in 10 files through `persist_scratch_alloc()`.
  - Adds `tools/check_internal_alloc_allowlist.ps1`.
  - Re-pins `check_kilnfw_dram_bss_budget.py`: bss 81500, new `.dram0.data` ceiling 25000.
  - Adds `docs/audits/INTERNAL_RAM_DRIFT_2026-10-13.md`.
- `682b6ea31`, `7ad520f12`, `084e45073`: gate evidence and doc counts.

Verdict: no device bug found. 0 HIGH, 0 MED, 3 LOW, 5 INFO. All the
findings are about the gate's reach or the budget script. Nothing on the
firmware path needs a fix before release.

## Device-side review (no findings)

**`s_store` in PSRAM** (`kiln_cfg_store.c:278`, `EXT_RAM_BSS_ATTR static kiln_cfg_store_blob_t s_store;`).
- Every reference is task context, under the store lock.
- No ISR, IRAM function, DMA descriptor or cache-disabled window touches it.
- The flash writes that read from it go through LittleFS/`esp_flash`. Those bounce a PSRAM source buffer through internal RAM, the same path the other PSRAM-resident persist buffers already use.
- `.ext_ram.bss` is zeroed at startup like `.bss`, so the old zero-init is unchanged.
- PSRAM is up before app_main, so no early-boot use comes before PSRAM init.
- ELF check: `s_store` sits at `0x3c2c96b0`, size `0x42ec` (17132 B), in the PSRAM range.

**`persist_scratch_alloc()` swaps.** Every swap is 1:1 `malloc(n)` -> `persist_scratch_alloc(n)` with the same `n`, and each buffer is still released by the same `free()`. The ten sites:
- `autotune_engine_guard.c:257`
- `firing_compare.c:78-80`, frees at 82-84 and 221-223
- `dashboard_autotune_http.c:547-548`
- `dashboard_exec_http.c:630-631`
- `http_auth_http.c:179`
- `readiness_http.c:167`
- `setup_progress_http.c:83`
- `web_auth_login_http.c:437`
- `zones_http.c:516`
- `safety_cfg_store.c:1528`

That is correct because `persist_scratch_alloc()` falls back to plain `malloc()` and its contract is "release with `free()`". No site relied on zeroed memory, `malloc` gave none either, and no `calloc` was converted. Every NULL check is still there, and the OOM replies are unchanged (500/503).

**Internal fallback.** `persist_scratch.h` still tries `heap_caps_malloc(SPIRAM|8BIT)` first and then `malloc()`. Under `SPIRAM_MALLOC_ALWAYSINTERNAL=8192`, the fallback can land a small buffer in either pool, which is the intended behaviour. K1 below shows that a host test pins the fallback.

**Size and zeroing.** Unchanged at every site, as stated above.

## Findings

### LOW-1: the gate counts only five call spellings; internal-capable allocations by other names pass

`check_internal_alloc_allowlist.ps1` matches only `malloc|calloc|realloc|xTaskCreate|xTaskCreatePinnedToCore` followed by `(`. These forms all allocate internal RAM and pass the gate (negtest G3-G6, G9, all MISSED):
- `heap_caps_malloc(..., MALLOC_CAP_INTERNAL ...)`. The tree already has 76 `MALLOC_CAP_INTERNAL` uses in non-test App sources, none gated.
- `xTaskCreate*WithCaps(..., MALLOC_CAP_INTERNAL)`.
- `pvPortMalloc`, `strdup`/`strndup`, `asprintf`.
- `xTaskCreateStatic` with an internal `.bss` stack. It shows up in the bss budget instead.
- A `#define` alias (`#define RD_ALLOC malloc`).
- The parenthesised form `(malloc)(n)`.
- A function pointer to `malloc`.

The gate's stated purpose is to stop new plain internal-RAM consumers, so the explicit-INTERNAL spelling is the most direct bypass. Suggested fix:
- Add `heap_caps_(m|c|re)alloc[_prefer]?` and `*WithCaps` when the caps argument contains `MALLOC_CAP_INTERNAL`/`MALLOC_CAP_DMA`, plus `pvPortMalloc|strn?dup|v?asprintf`.
- Match a bare `malloc` token that is not followed by `(` (aliases, `(malloc)`, function pointers).
- Allowlist the existing sites by count, as now.

### LOW-2: the count-per-file allowlist cannot see a same-file swap, and `.inc`/`.cpp` sources are not scanned

- The allowlist key is file -> count, so moving an allowlisted site within a file keeps the count and passes. G7 shows the case: in `diagnostics_http.c`, the scratch alloc at :1541 becomes `malloc` and the coredump `malloc` at :588 becomes scratch. Pinning the enclosing function name per site (or a hash of the call line) would close this. That is a design choice for a cheap gate; recorded so nobody reads more into it.
- The scan uses `-Include *.c, *.h`. `drivers/persist/profiles_builtin_table.inc` is compiled via `#include`, and a `malloc` added there passes (G8). No `.inc` file has an alloc today.
- Project components outside `App/` (`components/kilnlink`, `components/tls_spike`) are not scanned either; they have no plain allocs today.

### LOW-3: the budget script passes when `.dram0.data` is missing, and a 0 ceiling override is ignored

In `check_kilnfw_dram_bss_budget.py`:
- `data = sizes.get(DATA_SECTION, 0)` makes a missing `.dram0.data` section read as 0 B and PASS. The graded `.dram0.bss` section, by contrast, FAILs loudly when it is absent (line 167). A renamed section (an IDF linker-script change) would silently stop grading data.
- `args.data_ceiling_bytes if args.data_ceiling_bytes else ...` (and the same for `--ceiling-bytes`) treats an explicit `0` as "not given". This only affects the negative-test override.

Fix: FAIL when the data section is absent, and compare `is not None`.

Not negtested: the copy negtest makes has no ELF, so the script SKIPs there. Found by reading lines 145-146 and 183.

### INFO-1: ceiling headroom is honest but tight

Measured on the IRAMGATE ELF:
- `.dram0.bss` 80728 B against 81500: 772 B headroom.
- `.dram0.data` 24503 B against 25000: 497 B headroom.

Both are measurements, not guesses, and the ceiling was lowered from 101000, a real ratchet. Expect the next few static additions to need a deliberate re-pin per the module docstring.

### INFO-2: secrets now in PSRAM

The login request body (`web_auth_login_http.c:437`) and the session cookie buffer (`http_auth_http.c:179`) now live in PSRAM when it has room.
- The login body is wiped before `free()` on every exit (pre-existing).
- The cookie buffer is freed without a wipe, which predates IRAMGATE. It was internal RAM before, with the same exposure.

PSRAM is not more readable than internal RAM on this board (no external debug access beyond JTAG, which reads both), so this is not a regression. A `memset` before the cookie `free()` would be cheap hygiene.

### INFO-3: tiny buffers through the PSRAM path

The 64 B CSV line buffers (`dashboard_autotune_http.c:547`, `dashboard_exec_http.c:630`) now go through `persist_scratch_alloc()`. Below 8192 B, `SPIRAM_MALLOC_ALWAYSINTERNAL` makes plain `malloc` choose internal RAM, but `heap_caps_malloc(SPIRAM)` forces PSRAM. That is fine, slightly slower per byte, and on an HTTP path where it does not matter.

### INFO-4: gate evidence for the budget check is a ceiling-override stand-in

`GATE_NEGATIVE_TEST_EVIDENCE.md` says so plainly: no recompile with a 1000 B static. That is acceptable and honestly labelled. The allowlist row's two CAUGHT mutations are reproduced here (G1/G2).

### INFO-5: most converted OOM branches have no host test

Host negtest K3-K8b and K10 mutate the NULL branch at seven of the ten
converted sites: the status code changes, a refusal returns `true`, a leak, a
disabled NULL check. All were MISSED. The scratch fallback (K1), the setup
progress OOM reply (K2) and the firing_compare free pairing (K9) are pinned.
The other branches existed before IRAMGATE and this change did not alter
them. That is a test gap, not a device bug.

## Negative tests

All runs were made with `tools\negtest.ps1` from `C:\wt\rviram_ixz0id` at base `084e45073`. In both runs: real tree unchanged, copies removed, baseline PASS.

### Host tests (KilnFW)

`-Command` wrapper running `build_host_tests.ps1 -Only "firing_compare|setup_progress|dashboard_exec|readiness|zones_http|web_auth_login|safety_cfg_store|dashboard_autotune|http_auth|autotune|persist_campaign10|profiles_http|zones_config_cfg_fs"`, with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`. For every CAUGHT, the log was checked to confirm the mutant compiled and a test assertion failed.

Baseline PASS (74.6 s). Log dir `negtest_logs\20261013_084642_jgvf`.

| # | Mutation | Verdict | Assertion |
|---|---|---|---|
| K1 | `persist_scratch.h`: internal `malloc()` fallback removed (`p = NULL`) | CAUGHT | `test_backup_import.c:7412: error names the validation OOM` |
| K2 | `setup_progress_http.c`: OOM reply 500 -> 400 | CAUGHT | `test_setup_progress_http.c:279: alloc failure: 500` |
| K3 | `zones_http.c` sweep status: OOM reply 500 -> 400 | MISSED | INFO-5 |
| K4 | `readiness_http.c`: OOM status 503 -> 200 | MISSED | INFO-5 |
| K5 | `safety_cfg_store.c` refetch: OOM returns `true` | MISSED | INFO-5 |
| K6 | `web_auth_login_http.c`: OOM reply 500 -> 400 | MISSED | INFO-5 |
| K7 | `http_auth_http.c` cookie copy: OOM returns `true` | MISSED | INFO-5 |
| K8a | `dashboard_autotune_http.c`: OOM path leaks `line` | MISSED | INFO-5 |
| K8b | `dashboard_exec_http.c`: OOM reply 500 -> 400 | MISSED | INFO-5 |
| K9 | `firing_compare.c`: final `free(in_band)` dropped | CAUGHT | `test_firing_compare_alloc.c:137: all three blocks freed` (and :154) |
| K10 | `autotune_engine_guard.c`: unpack NULL check disabled | MISSED | INFO-5 |

For every CAUGHT row, the log shows no build failure. The FAIL line is a test assertion.

### Gate check

`-Preset check -PresetArg tools/check_internal_alloc_allowlist.ps1`, baseline PASS (14 s).

| # | Mutation | Verdict | Note |
|---|---|---|---|
| G1 | `readiness_http.c`: `persist_scratch_alloc(READINESS_JSON_CAP)` -> `malloc(...)` | CAUGHT | `FAIL: NEW plain alloc/task-create site(s) in drivers/http/readiness_http.c (1)` |
| G2 | same -> `calloc(1, ...)` | CAUGHT | same message |
| G3 | same -> `heap_caps_malloc(..., MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT)` | MISSED | LOW-1 |
| G4 | same -> `pvPortMalloc(...)` | MISSED | LOW-1 |
| G5 | `#define RD_ALLOC malloc` + `RD_ALLOC(...)` | MISSED | LOW-1 |
| G6 | `(malloc)(...)` | MISSED | LOW-1 |
| G7 | `diagnostics_http.c`: :1541 scratch -> `malloc`, :588 `malloc` -> scratch (count kept) | MISSED | LOW-2 |
| G8 | `malloc(4096)` helper appended to `profiles_builtin_table.inc` | MISSED | LOW-2 |
| G9 | `strdup("x")` | MISSED | LOW-1 |

## Fix status

Open: LOW-1, LOW-2, LOW-3 (tooling only). Per the owner's 2026-10-12 "device bugs only" rule, these are recorded, not fixed here.
