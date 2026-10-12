# Review LCDFX13 (2026-10-12)

Reviewer: Opus, no subagents. This review covers origin/dev `0214a1ea7` and `ce18b24bb` (lcdfx13). They are the fixes for `REVIEW_LCDFX12_2026-10-12.md` MED-1, LOW-1, LOW-2, LOW-3 and INFO-3:

- `tools/check_lcd_admin_gates.ps1`: L-secret now requires the masked block to be exactly four statements and blanks string literals before it matches. The L-shadow pragma scan now follows includes transitively.
- `tools/check_lcd_sink_callgraph.py`: the reverse walk now follows section aliases.
- `tools/test_check_lcd_sink_callgraph.ps1`: adds cases Q and R.

The worktree was minted at origin/dev `ce18b24bb`.

Counts: **HIGH 0, MED 1, LOW 3, INFO 3.**

The fixes do what REVIEW_LCDFX12 asked:

- Omitting a statement from the masked block now fails (control mutant K1).
- Using the secret buffers before the gate now fails (K2).
- A shadow pragma in an App header that UI code reaches only transitively now fails (K3).
- The alias walk is real: removing it, or removing the alias record, fails case Q (CG1, CG2).

The open items are about what the checks still do not constrain.

## Answers to the brief

**Q1. Can the AP password or QR secret still leak on the LCD while unauthenticated?**

The production code does not leak today. The masked branch runs from the always-on 1000 ms `refresh_cb` timer. It masks the label, hides `s_ap_qr` and clears `s_ap_qr_last`. A relock goes through `handle_lcd_relock_to_home`, which first clears the edit textareas. An unauthenticated user can reach only the dashboards.

The check, however, constrains only three things:

- the region before the gate;
- the masked block itself;
- the getter's call sites (L-sink).

It does not constrain:

- code after the masked block;
- other code paths in `refresh_cb`;
- other functions or timers in the file;
- file-scope declarations.

The secret also outlives the mask in two static buffers and in one widget:

- `s_ap_qr_uri` holds `WIFI:...;P:<password>;;`. It is never cleared.
- `s_ap_qr_last` is cleared only inside the masked branch, which runs only while `ap_active`.
- The `s_ap_qr` widget keeps its encoded data while it is hidden.

All four of the following bypass mutants pass the check (MISSED; see the table below):

- **A second label (E1).** In STA mode, an `else` arm of `if (ap_active)` puts `s_ap_qr_uri` on `s_dashboard_qr_caption`. After an admin has viewed the AP page once, a USER-level viewer sees the stale password.
- **A timer callback (E2).** A second `lv_timer_create(qr_peek_cb, 100, NULL)` un-hides `s_ap_qr` every 100 ms. The QR still encodes the last admin-rendered secret.
- **A different reader before the gate (E3).** The file-scope alias `static char *const s_peek = s_ap_qr_uri;` is read before the gate as `lv_label_set_text(s_ap_ssid_label, s_peek[0] ? s_peek : ap_ssid)`. The pre-gate rule bans only the literal names `ap_password`, `s_ap_qr_uri` and `build_wifi_qr_uri`, so a new name passes.
- **The QR rendered elsewhere, or a hide that is neutralised (E5).** Add `#define lv_obj_add_flag(o, f) ((o) == s_ap_qr ? (void)0 : lv_obj_add_flag(o, f))` near the top of the file. The masked block still has its exact four-statement shape, but the hide no longer happens. L1-macro covers only gate and opener tokens.

The control mutants show the parts that hold:

- Rendering the AP URI on the dashboard QR before the gate is CAUGHT (K2).
- Dropping the hide is CAUGHT (K1).

**Q2. Is the exact-shape match brittle in ways that will force unsafe workarounds?**

It fails closed. It is brittle to equivalent rewrites, for example:

- `s_ap_qr_last[0] = 0` or `memset`;
- `ap_password[0] != '\0'`;
- also hiding a caption;
- `if/else` instead of an early `return`.

Each of these needs a reviewed edit to `$wantStmts`, which is the right friction. The workaround that brittleness invites is the dangerous one: someone who cannot add a statement to the pinned block puts the new display code after it, or in another function. That is exactly the unconstrained area in MED-1. So the pinned block is safe only when paired with a whole-file inventory of the secret-bearing names.

**Q3. Does the transitive include scan resolve paths the way the build does? Can a shadow pragma hide in a header it cannot find?**

For App it works. A scratch include-reachability map found:

- 148 App files reachable from `drivers/ui`;
- no duplicate basenames among non-test App headers;
- no `<...>` or macro includes of App headers.

So resolving by basename matches the build there.

The scan reads nothing outside App. The `drivers` component's `INCLUDE_DIRS` contains `../../../hwAbstraction/interface`, and UI translation units reach these first-party headers that the scan never opens:

- directly: `hal_kv.h`, `hal_sysinfo.h`, `hal_time.h`;
- through other headers:
  - `hal_spi.h` and `hal_status.h`;
  - `hwAbstraction/esp/{spi,i2c,uart}/*_owner.h` and `uart_protocol.h`;
  - `CommonFW/include/kilnlink/*.h`, through `safety_link.h`.

Results:

- **P2 (MISSED):** a `#pragma GCC diagnostic ignored "-Wshadow"` in `hal_time.h`.
- **C1 (MISSED):** a CMake `-w` on the UI sources.

C1 is real. I confirmed on xtensa GCC 15.2 that `-w` suppresses `-Werror=shadow` in either order. `-Wno-shadow` also wins when it comes later, but the L-shadow regex already catches that spelling. See LOW-2.

**Q4. What false-positive or false-negative risk do `-ffunction-sections` and the LTO/linker layout create for section aliasing?**

The build is not LTO: there are 0 `-flto` and 2127 `-ffunction-sections` occurrences in `compile_commands`. The check reads pre-link `.c.obj` files, so linker ICF and `--gc-sections` cannot affect it. There are no `--defsym`, `--wrap` or `PROVIDE` aliases in KilnFW CMake or linker fragments.

Compile-time IPA-ICF does merge identical functions within one translation unit. `pid_fuzzy.c.obj` shows `clamp_gain` and `sanitize_base` at the same address in `.text.clamp_gain`.

I ran the check read-only against two builds:

| Build | Result | Referenced symbols |
|---|---|---|
| `C:\wt\checkbuild_30301fbe48` | passed | 22771 |
| `C:\wt\checkbuild_280d180149` | passed | 22779 |

A survey of the checkbuild directory in the first row of the table above found 110 alias groups. Almost all of them are ESP-IDF, plus four in `drivers/`, none of which is UI.

**False negatives (LOW-3).** The walk begins at the sinks' callers. It never treats the sink definitions themselves as nodes. Synthetic probes, run directly against the check:

- **A3:** an alias of `safety_link_send_clear_trip` in its own section, with a UI function that calls the alias. The check passes, exit 0.
- **A4:** a `.constprop.0` clone of `safety_link_send_clear_trip`, called by a global wrapper in the same translation unit, with UI calling the wrapper. The check passes, exit 0.
- **A1 and A2:** the same shapes on `dashboard_safety_clear_trip` are caught. They are caught only because that sink itself calls the second sink, so its node is walked.

**False positive (INFO-2).** IPA-ICF on two identical static callbacks in one UI file produces a shared UI section. The strict rule fails it as "cannot be attributed" (probe FP1). There are none today.

## Findings

### MED-1: the L-secret check pins the masked block but not where the secret-bearing names go

- **Location:** `tools/check_lcd_admin_gates.ps1`, L-secret (about lines 547-610).
- **Scenario:** E1, E2 and E3 above. Each mutant shows the AP password, or the QR that encodes it, to an unauthenticated or USER-level viewer, and the check passes. Code after the masked block, other functions and timers in the file, and file-scope aliases are all unchecked. The data survives after masking (INFO-1), so a leak needs no fresh fetch.
- **Fix:** pin a use-count inventory, like L-sink, for every mention in `ui_page_network.c` of `s_ap_qr_uri`, `s_ap_qr_last`, `s_ap_qr` and `s_ap_password_label`. Allow exactly the current sites: declarations, the build function, the masked block and the admin branch. Fail on any other mention, including an `&name`, `name +` or initializer use at file scope. Also fail on any `lv_obj_remove_flag(s_ap_qr, ...)` outside the admin branch.

### LOW-1: a selective function-like macro over LVGL calls can neutralise the masked block

- **Location:** `check_lcd_admin_gates.ps1`, L1-macro (gate and opener tokens only).
- **Scenario:** E5. `#define lv_obj_add_flag(o, f) ((o) == s_ap_qr ? (void)0 : lv_obj_add_flag(o, f))` keeps the exact four-statement text but removes the hide. The same works for `lv_label_set_text`.
- **Fix:** fail any `#define` in `drivers/ui/*.c` or a UI-reachable header whose name is one of the identifiers in `$wantStmts` (`lv_label_set_text`, `lv_obj_add_flag`, `s_ap_qr`, `s_ap_qr_last`, `s_ap_password_label`, `LV_OBJ_FLAG_HIDDEN`). Better: fail any function-like `#define` in `ui_page_network.c` at all.

### LOW-2: L-shadow misses headers outside App and the `-w` spelling

- **Location:** `check_lcd_admin_gates.ps1`, the pragma scan (about lines 515-545) and the CMake assertion (about lines 502-510).
- **Scenario:**
  - P2: a shadow pragma in `firmware/hwAbstraction/interface/hal_time.h` passes. This directory is an `INCLUDE_DIRS` entry of the `drivers` component, and four UI files include the header directly.
  - C1: `set_property(SOURCE ${_kiln_lcd_ui_sources} APPEND PROPERTY COMPILE_OPTIONS "-w")` passes, and `-w` silences `-Werror=shadow`. The same flag in `idf_build_set_property(COMPILE_OPTIONS ...)` in another CMake file would not be scanned at all.
- The evidence row's known-gap note names only ESP-IDF and LVGL, so it understates this gap.
- **Fix:**
  - Extend `$hdrIndex` to `firmware/hwAbstraction/**` and `firmware/CommonFW/include/**`, and resolve `<...>` includes against the same index.
  - Extend the CMake regex to `(?i)(^|[\s";])-w([\s";)]|$)|-Wno-shadow|-Wno-error=shadow|-Wno-error\b`.
  - Scan every `CMakeLists.txt`/`*.cmake` under `firmware/KilnFW`, outside `components/` and `managed_components/`.

### LOW-3: the call-graph walk does not cover aliases or IPA clones of the sinks themselves

- **Location:** `tools/check_lcd_sink_callgraph.py`, `analyse()`. The work list starts from `rev[s]` for each sink. The sink's own node `obj::sink` is never popped, so the alias loop never runs for it.
- **Scenario:**
  - A3: an alias of `safety_link_send_clear_trip` (same section, second symbol) called from UI passes.
  - A4: a `safety_link_send_clear_trip.constprop.0` clone called by a global wrapper in `safety_link_tx.c`, with the wrapper called from UI, passes.
  - In both cases the "unreviewed non-UI caller" rule is also blind, because nothing references the sink by name.
- **Fix:**
  - Seed the walk with every defining node of each sink: `obj::s` from `defined`.
  - Treat `<sink>.(constprop|isra|part|cold|lto_priv).N` and the alias set of the sink's section as sinks too.
  - Add synthetic cases for A3 and A4 to the test harness.

### INFO-1: the secret stays in RAM and in the hidden QR after masking

`s_ap_qr_uri` is never cleared. `s_ap_qr_last` is cleared only while `ap_active`. The hidden `s_ap_qr` keeps its bitmap. These are what turn MED-1's bypasses into real leaks without a fresh fetch.

Hardening:

- In the masked branch and in `ui_page_network_relock_close`, `memset` `s_ap_qr_uri` and re-encode `s_ap_qr` with a non-secret string, such as an empty `WIFI:;;` or the SSID only.
- Clear `s_ap_qr_last` unconditionally on relock.

### INFO-2: IPA-ICF can false-positive the strict UI shared-section rule

At `-Os`, GCC merges identical functions within a translation unit. Two identical static LVGL callbacks in one UI file would share a section, and the check would FAIL as "cannot be attributed" (probe FP1). This fails closed, but the likely workaround is to make the two functions artificially different.

A better rule fails only when the symbols in a shared UI section have different addresses or sizes. Symbols at the same address with the same size are true aliases of identical code. Union their edges through the alias walk instead.

### INFO-3: the exact-shape match needs a whole-file companion

This is Q2: the brittleness pushes new code out of the pinned block and into the unconstrained area. Fixing MED-1 removes the incentive.

## Fix status (lcdfx14)

All six findings FIXED in the lcdfx14 commit (SHA recorded in the landing note below; local commit 97f940af8 before rebase).

- **MED-1 FIXED.** `check_lcd_admin_gates.ps1` L-secret now counts every mention of `s_ap_qr_uri`, `s_ap_qr_last`, `s_ap_qr` and `s_ap_password_label` per enclosing function across the whole file and fails on any change (`$invWant`). The masked block is pinned to 5 statements, and `lv_obj_remove_flag(s_ap_qr, ...)` must appear exactly once, after the masked block. E1, E2, E3 CAUGHT.
- **INFO-1 FIXED.** `ui_page_network_secret_wipe()` (new `ui_page_network_secret.c`) zeroes `s_ap_qr_uri` and `s_ap_qr_last` through a volatile loop; the masked branch and `ui_page_network_relock_close` call it and re-encode the hidden QR to the blank payload `WIFI:;;`. Host test `test_ui_page_network_secret.c` asserts the buffers are zeroed.
- **LOW-1 FIXED.** L-macro scan: any `#define`/`#undef` of an `lv_*` name, `LV_OBJ_FLAG_*` or a secret-path helper in a UI file or a header it includes fails. E5 CAUGHT.
- **LOW-2 FIXED.** The header index now covers hwAbstraction and CommonFW headers (`<...>` includes too), and every KilnFW CMakeLists/*.cmake is scanned for `-w`, `-Wno-shadow`, `-Wno-error`. P2, C1 CAUGHT. Also fixed the test-directory exclusion regex (`[\/]test[\/]`).
- **LOW-3 FIXED.** `check_lcd_sink_callgraph.py` seeds the walk from each sink's own definition and treats aliases and `.constprop/.isra/.part/.cold/.lto_priv` clones as sinks. Cases S, T added to `test_check_lcd_sink_callgraph.ps1`.
- **INFO-2 FIXED.** A shared strict UI section fails only when the symbols differ in address or size. Case U added.

Negative tests (lcdfx14, `tools\negtest.ps1`, baseline passed and real tree unchanged in every run):

| Check / preset | Mutant | Verdict |
|---|---|---|
| check_lcd_admin_gates.ps1 | E1 else caption, E2 peek timer, E3 alias, E5 macro, P2 hal_time pragma, C1 `-w`, C2 `-w` in root CMakeLists, K_drop_wipe, K_unhide_early | 9 of 9 CAUGHT by the check's own `FAIL: L-` line |
| test_check_lcd_sink_callgraph.ps1 | S_no_sink_nodes, T_no_clone_sinks, U_icf_strict | 3 of 3 CAUGHT (FAIL S/T/U lines) |
| kilnfw-host | H1 wipe loop `i < 1`, H2 skip `last` wipe | 2 of 2 CAUGHT; both compiled, `test_ui_page_network_secret.c` assertions failed |

## Negative tests

All runs were `tools\negtest.ps1 -Preset check` from `C:\wt\rvlcdfx13_fbwkjc` at `ce18b24bb`. In every run the unmutated baseline passed, `real_tree_unchanged` was true and the copies were removed. Every CAUGHT is a real `FAIL:` assertion line, not a parse error.

`-PresetArg tools/check_lcd_admin_gates.ps1 -ExpectPattern '(?m)^FAIL: L-'`:

| Mutant | Verdict | Line or note |
|---|---|---|
| E1 STA `else` puts `s_ap_qr_uri` on `s_dashboard_qr_caption` | MISSED | MED-1 |
| E2 second 100 ms timer un-hides `s_ap_qr` | MISSED | MED-1 |
| E3 file-scope alias of `s_ap_qr_uri` read before the gate | MISSED | MED-1 |
| E5 selective `lv_obj_add_flag` macro | MISSED | LOW-1 |
| P2 shadow pragma in `hwAbstraction/interface/hal_time.h` | MISSED | LOW-2 |
| C1 `-w` appended to the UI sources' COMPILE_OPTIONS | MISSED | LOW-2 |
| K1 drop the masked QR hide (control) | CAUGHT | `FAIL: L-secret: refresh_cb's masked '!show_secret' block must be exactly: ...` |
| K2 dashboard QR renders `s_ap_qr_uri` before the gate (control) | CAUGHT | `FAIL: L-secret: refresh_cb uses 's_ap_qr_uri' before the show_secret gate ...` |
| K3 shadow pragma in `ui_lcd_lock.h` (control) | CAUGHT | `FAIL: L-shadow: ui_lcd_lock.h has a diagnostic pragma naming shadow ...` |

`-PresetArg tools/test_check_lcd_sink_callgraph.ps1 -ExpectPattern '(?m)^FAIL: Q '`:

| Mutant | Verdict | Line |
|---|---|---|
| CG1 alias loop replaced with `for al in []` | CAUGHT | `FAIL: Q ui call to a non-ui alias sharing a sink caller's section fails` |
| CG2 alias record replaced with `pass` | CAUGHT | `FAIL: Q ui call to a non-ui alias sharing a sink caller's section fails` |

The E, P and C mutants are textual inputs to a text check. They were not compiled. C1's effect on GCC was confirmed separately with xtensa-esp32s3-elf-gcc 15.2.

The synthetic probes A1, A2, A3, A4 and FP1 ran directly against `check_lcd_sink_callgraph.py` with `--reloc-dump`/`--sym-dump`:

| Probe | Exit | Result |
|---|---|---|
| A1 | 1 | caught |
| A2 | 1 | caught |
| A3 | 0 | missed, LOW-3 |
| A4 | 0 | missed, LOW-3 |
| FP1 | 1 | false positive, INFO-2 |
