# REVIEW_LCDFX11: adversarial review of d8d6713ac (LCDFX10 review fixes)

Date: 2026-10-12. Base: origin/dev d78551461 (contains d8d6713ac). Reviewer: opus.

Scope: d8d6713ac fixes REVIEW_LCDFX10 MED-2/3/4 and LOW-1/2/4/5 in
`tools/check_lcd_sink_callgraph.py`, `tools/check_lcd_admin_gates.ps1` and
`tools/test_check_lcd_sink_callgraph.ps1`, plus the matching rows of
`docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md` (rows 165 and 167, plus the test row).

Questions asked:
1. Can L-secret still be bypassed?
2. Is the objdump `-t` section mapping right for `.iram1.N`, `.text.N` and `.literal`?
   Can a real-build edge be dropped silently?
3. Is the M1 "equivalent mutant" claim right?
4. Are the evidence rows truthful?
5. Does the check hold up against a real checkbuild tree?

## Verdict

No HIGH findings.

**The call-graph check holds up.**
- Every cited mutation reproduced with the same FAIL line.
- On a copy of a real checkbuild, every planted bypass but one failed closed: an IRAM wrapper, a function-pointer table in DRAM, and a non-UI helper called from UI.
  - The exception is an evil function that GCC places after `btn_clicked_cb` in the gate's own section. LOW-2 covers it, and a text pin backstops it.
- Five mutations of the real-build path all fail the check. One of them failed on a different line than the one predicted.

**The L-secret text rule in `check_lcd_admin_gates.ps1` is still bypassable.** It misses three syntactically ordinary, compiling edits that show the AP password to a USER: MED-1, MED-2 and LOW-1. Two L-shadow pragma spellings also slip past it (LOW-3).

The evidence rows are truthful.

## Findings

### MED-1: renaming the fetch local turns every L-secret token rule off

**Status: Fixed (lcdfx12): the fetch local is derived from the single `wifi_prov_get_ap_password()` assignment (fail loud otherwise); B4 CAUGHT.**
Location: `tools/check_lcd_admin_gates.ps1:530-541`.

- Every L-secret rule keys on the identifier `ap_password`: the masked-branch token list at line 531 and the pre-gate rule at lines 538-541.
- Mutation B4 renamed the local `ap_password` to `pw` in `ui_page_network.c` refresh_cb, then wrote `pw` to `s_ap_ssid_label` before the `show_secret` gate.
  - Result: MISSED, exit 0.
- `\bap_password\b` does not match inside `wifi_prov_get_ap_password`, and the sink inventory row still sees exactly one fetch, so nothing fires.
- A harmless refactor (shortening the local name) makes the check vacuous without failing it.

Fix: key the rules on whatever local the single `wifi_prov_get_ap_password()` result is assigned to.
1. Extract the local name with a regex over the declaration.
2. Fail if no such declaration is found.
3. Substitute that name into lines 531 and 539-541.

### MED-2: a conditional return satisfies "masked block ends with return;"

**Status: Fixed (lcdfx12): the masked block must end with a bare `return;` statement; B3 and an `else return` variant CAUGHT.**
Location: `tools/check_lcd_admin_gates.ps1:533`.

- Mutation B3 replaced the masked block's final `return;` with `if (!ap_password[0]) return;`.
  - Result: MISSED.
- The block text still ends with `return;`, and the `ap_password[0]` strip at line 530 hides the token.
- When a password is set, a USER falls through into the ADMIN reveal (the password label and QR). This is exactly the P2 hole that MED-4 meant to close.

Fix:
- Require the masked block's last statement to be an unconditional `return;`. For example, the text before it must end in `;` or `{`, not `)`.
- Alternatively, forbid `if` and `?` inside the masked block entirely. It holds only label writes today.

### LOW-1: stripping `ap_password[0]` also hides `&ap_password[0]`

**Status: Fixed (lcdfx12): only emptiness tests of the local are exempt, never `&X[0]` or a value use; B1, B2 CAUGHT.**
Location: `tools/check_lcd_admin_gates.ps1:530` and `:540`.

Both rules delete every `ap_password[0]` before scanning.

| Mutation | Edit | Result |
|---|---|---|
| B1 | `lv_label_set_text(s_ap_ssid_label, &ap_password[0]);` before the gate | MISSED |
| B2 | the masked branch label set to `&ap_password[0]` | MISSED |

Both put the full password on screen for a USER. This needs a deliberate spelling, hence LOW.

Fix: strip only the truth-test form. Use a negative lookbehind for `&`, and require a following `?`, `)` or `&&`/`||`. Better, apply the MED-1 local-name approach and allow `X[0]` only in conditional position.

### LOW-2: one symbol per section hides a second global in the gate's section

**Status: Fixed (lcdfx12): `parse_syms` keeps every symbol; a section shared by several symbols in a UI object is unattributable and fails (non-UI objects keep first-symbol attribution: aliases and `$part$0` clones share legitimately). T5 now FAILS; case P covers it.**
Location: `tools/check_lcd_sink_callgraph.py:45`, `parse_syms` with `setdefault`.

- Experiment T5 compiled a replacement `ui_page_safety.c` with `-fno-toplevel-reorder`. In it, `btn_clicked_cb` and a global `evil_entry` share the section `.text.btn_clicked_cb`, with `btn_clicked_cb` first.
  - Result: PASS on the real-build copy.
- `evil_entry -> clear_action` is attributed to `btn_clicked_cb`, the first symbol, so it looks like the allowed gate edge.
- With GCC's default ordering (T4), evil came first and the check FAILED ("references the gated clear action").
- This needs hand-written section attributes or toplevel-reorder flags in a UI file. The text check's `clear_action` use-count pin (`check_lcd_admin_gates.ps1` inventory row 66) still catches a second caller in the source, hence LOW.

Fix: when a code section holds more than one `F` symbol, either fail it as unattributable or split it by symbol value against the relocation offsets.

### LOW-3: two pragma spellings slip past L-shadow

**Status: Fixed (lcdfx12): line splices are joined, any `_Pragma` is refused, and App headers included by drivers/ui sources are scanned; B5, B6, B8, B9 CAUGHT. Gap: headers outside App (ESP-IDF, LVGL) are not scanned.**
Location: `tools/check_lcd_admin_gates.ps1:518`.

| Mutation | Spelling | Result |
|---|---|---|
| B5 | `#define RV_PR(x) _Pragma(#x)` then `RV_PR(GCC diagnostic ignored "-Wshadow")` | MISSED; `_Pragma\s*\([^)]*diagnostic` sees only `#x` |
| B6 | `#pragma GCC diagnostic ignored "-Wsha\` + newline + `dow"` (line splice) | MISSED; `[^\r\n]*shadow` stops at the newline |

Both were compiled with xtensa-esp32s3-elf-gcc esp-15.2.0 and `-Werror=shadow`:
- the plain shadowing control (b0.c) fails;
- B5 and B6 both compile clean, so both really switch the error off.

Both are deliberate evasions, hence LOW.

Fix:
- Before matching, join line splices (`\\\r?\n`).
- Fail on any `_Pragma` in `drivers/ui`, and on any `#`-stringizing macro whose body contains `_Pragma`.

Scope gap, not mutated: the scan covers only `drivers/ui/*.{c,h}` (`$uiAll`, non-recursive). A pragma or weak redeclaration in a non-UI header that a UI file includes is not seen.

### LOW-4: the gate-references-sink check has no negative test

**Status: Fixed (lcdfx12): case O added; S8 CAUGHT.**
Location: `tools/check_lcd_sink_callgraph.py:116-118`.

- Mutation S8 replaced its `fails.append(...)` with `pass`.
  - Result: MISSED, exit 0, with every case A-M PASS.
- No synthetic case has a gate object whose `clear_action` stops referencing the sinks. The evidence row does not claim this check either.

Fix: add a test case where `clear_action` in `ui_page_safety.c.obj` references nothing from SINKS.

### INFO-1: `.iram1.N.literal` maps to node `N.literal`, not to the function
Location: `tools/check_lcd_sink_callgraph.py:56-62`.

- Real Xtensa IRAM code puts callee relocations in `.iram1.N.literal`.
- That section has no symbol of its own, and `KIND_PFX` yields `7.literal`. This is non-numeric, so it is not flagged as unattributable.
- Experiment T1 (a UI `IRAM_ATTR` wrapper calling the sink) still FAILED on the real-build copy, because the node keeps the UI object key. It named `7.literal`, `iram_wrap` and the caller. Fail-closed, but unnamed.
- Test case J models `.iram1.5` with direct relocations, which does not match the real layout.

Fix (optional): map `<sec>.literal` to `secmap[<sec>]` when present.

### INFO-2: the gate is matched by basename only
Location: `tools/check_lcd_sink_callgraph.py:111` and `:137`.

A second `ui_page_safety.c` in another directory would also be treated as the gate. `drivers/CMakeLists.txt` lists UI sources explicitly, so this is not reachable today.

## Q3: the M1 equivalent-mutant claim is correct

- S7 drops the object comparison at `check_lcd_sink_callgraph.py:140`.
  - Result: MISSED.
- In `parse()` (lines 76 and 90), a section-relative reloc target is keyed with its own object, so the static `<obj>::clear_action` node only gains referrers from that same object.
- Any other object can reach it only through an undefined global symbol, which the bare-name rule catches. S1, which turns the bare-name rule off, is CAUGHT (H, I2).
- The object comparison is therefore redundant, as claimed.

## Q2: section mapping on a real build

Setup:
- Source: a copy of the 1993 `*.c.obj` from `C:\wt\checkbuild_280d180149` (HEAD 53131caf1), placed in private scratch.
- Command: `python tools/check_lcd_sink_callgraph.py --build-dir <copy>`.
- Unmutated result: PASS, 22627 referenced symbols, about 26 s.

Planted objects were compiled with `-O2 -ffunction-sections -fdata-sections -mlongcalls`:

| Exp | Planted code | Result |
|---|---|---|
| T1 | UI `IRAM_ATTR` wrapper calling the sink (`.iram1.7`) | FAIL (INFO-1 naming) |
| T2 | DRAM function-pointer table holding the sink (`.dram1.3`) | FAIL, names `evil_tbl` |
| T3 | non-UI helper (in `http/`) calling the sink, called from UI | FAIL, "reaches sink" and "unreviewed non-UI caller" |
| T4 | evil global in `ui_page_safety.c`, GCC default order | FAIL, "references the gated clear action" |
| T5 | as T4, but `btn_clicked_cb` first in the shared section | PASS: LOW-2 |

No silent edge drop was found for `.text.N`, `.literal.N`, `.iram1.N` or `.dram1.N`. The check fails on unattributable numeric sections (S5 and S9 are CAUGHT).

## Mutation table

All runs used `tools\negtest.ps1`. Each run had a passing baseline, reported `real_tree_unchanged: true` and removed its copies. Every CAUGHT below was confirmed from the log as a FAIL line printed by the check itself (or by the test harness's own Check), not as a build or parse error. Mutation JSON is kept in private scratch.

### check_lcd_admin_gates.ps1

Run: `-Preset check -PresetArg tools\check_lcd_admin_gates.ps1`. Baseline PASS, 35.1 s.

| # | Mutation (ui_page_network.c) | Verdict | Firing line |
|---|---|---|---|
| C1 | `s_ap_ssid_label` set to `ap_password` before the gate (cited) | CAUGHT | `FAIL: L-secret: refresh_cb uses 'ap_password' before the show_secret gate` |
| C2 | masked block's `return;` removed (cited) | CAUGHT | `FAIL: L-secret: ... does not end with 'return;'` |
| C3 | `#pragma GCC diagnostic ignored "-Wshadow"` (cited) | CAUGHT | `FAIL: L-shadow: ui_page_network.c has a diagnostic pragma` |
| C4 | `__attribute__((weak))` declaration (cited) | CAUGHT | `FAIL: L-weak: ui_page_network.c mentions weak` |
| B1 | `&ap_password[0]` before the gate | MISSED | LOW-1 |
| B2 | `&ap_password[0]` in the masked branch | MISSED | LOW-1 |
| B3 | `if (!ap_password[0]) return;` as the masked block's end | MISSED | MED-2 |
| B4 | fetch local renamed to `pw`, `pw` written before the gate | MISSED | MED-1 |
| B5 | `_Pragma(#x)` stringizing macro naming `-Wshadow` | MISSED | LOW-3 |
| B6 | `-Wsha\`+newline+`dow` line splice | MISSED | LOW-3 |
| B7 | `__attribute__((RV_CAT(we, ak)))` token paste | CAUGHT | `FAIL: L4: ui_page_network.c uses the ## token-paste operator` |

### check_lcd_sink_callgraph.py, synthetic harness

Run: `-Preset check -PresetArg tools\test_check_lcd_sink_callgraph.ps1`. Baseline PASS, 11.4 s.

| # | Mutation | Verdict | Firing line |
|---|---|---|---|
| S1 | bare-name `clear_action` rule emptied (cited) | CAUGHT | `FAIL: H bare-name clear_action reference fails`, `FAIL: I2 ...` |
| S2 | sink-defined check `if False:` (cited) | CAUGHT | `FAIL: G renamed sinks fail` |
| S3 | sink-referrer check `if False:` (cited) | CAUGHT | `FAIL: L sink without referrers fails` |
| S4 | gate-edge check `if False:` (cited) | CAUGHT | `FAIL: M missing gate edge fails` |
| S5 | numeric section not recorded as unattributable (cited) | CAUGHT | `FAIL: K unattributable relocation section fails` |
| S6 | symbol-table section map ignored (cited) | CAUGHT | `FAIL: J IRAM wrapper in ui reaching the sink fails` |
| S7 | M1: gate caller object comparison dropped (cited MISSED) | MISSED | equivalent mutant, see Q3 |
| S8 | gate-references-sink `fails.append` -> `pass` | MISSED | LOW-4 |
| S9 | unattributable loop in `main` dropped | CAUGHT | `FAIL: K unattributable relocation section fails` |

### check_lcd_sink_callgraph.py, real-build copy

Run: `-Command "python tools\check_lcd_sink_callgraph.py --build-dir <copy>"`. Baseline PASS, 276.9 s (three negtest runs were in parallel).

| # | Mutation | Verdict | Firing line |
|---|---|---|---|
| R1 | sink renamed `dashboard_safety_clear_trip_v2` | CAUGHT | `sink dashboard_safety_clear_trip_v2 has no defining section` |
| R2 | `GATE_CALLER` renamed | CAUGHT | `gate edge ui_page_safety.c.obj::btn_clicked_cb2 -> clear_action not found` |
| R3 | `.literal*` sections skipped | fails closed (negtest reported MISSED; see note) | `gate edge ui_page_safety.c.obj::btn_clicked_cb -> clear_action not found` |
| R4 | `.debug*` no longer skipped | CAUGHT | `.debug_info references the gated clear action` (and the other `.debug_*` sections) |
| R5 | non-UI allowlist emptied | CAUGHT | `unreviewed non-UI caller of safety_link_send_clear_trip: ...safety_link_service_boot_clear_if_pending` |

Note on R3: the check exited 1 with a real FAIL line from its own gate-edge rule. negtest scored the run MISSED only because the expected pattern (`has no referrers`) was predicted wrongly. The check failed closed, so this is not a coverage gap.

## Q4: the evidence rows are truthful

- Row 165 (`check_lcd_admin_gates.ps1`): the four 2026-10-12 mutations reproduce as C1-C4 with the same FAIL lines. The account of the first L-weak attempt being MISSED, then fixed, is consistent with the current raw-text scan at line 513.
- Row 167 (`check_lcd_sink_callgraph.ps1`, PARTIAL):
  - All six CAUGHT claims reproduce as S1-S6 with the same case letters.
  - The M1 MISSED claim reproduces as S7, and the equivalence argument holds.
  - The by-hand real-build claim (PASS, 22773 symbols) matches a PASS here: 22627 symbols on a different checkbuild tree.
  - PARTIAL remains the right status because of LOW-4.
- Row for `test_check_lcd_sink_callgraph.ps1`: consistent with the above.

The real-build mutations R1-R5 above can be cited as evidence for the real-build path, which row 167 says was not mutated.
