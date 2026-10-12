# Review LCDFX12 (2026-10-12)

Reviewer: Opus, no subagents. This review covers origin/dev `01554a779` (lcdfx12) and the range `c90069512..01554a779`:

- `c278cb21c`: the main fix for REVIEW_LCDFX11 MED-1, MED-2 and LOW-1..4.
- `1e65de7e8`: changes the splice regex from `'\\r?\n'` to `'\\\r?\n'`.
- `01554a779`: docs only.

The worktree was minted at origin/dev `44289aa41`, which contains the range.

Counts: **HIGH 0, MED 2, LOW 3, INFO 4.**

## Answers to the brief

**Q1. Do bypasses remain?**

These cases are now caught:

- A second local or alias of the secret before the gate. Control mutant K1 is CAUGHT with `L-secret ... uses 'ap_password' before the show_secret gate`.
- A `strcpy` into a buffer inside the masked block. K3 is CAUGHT on `s_ap_qr_uri`.
- A helper that receives the local. The pre-gate token rule catches it.
- A macro that wraps the getter. K2 is CAUGHT by the L-sink inventory and by the single-fetch rule.
- Several refresh functions. Each extra getter mention is outside the sink inventory and fails L-sink.
- `&X[0]`, `X + 0` and a conditional `return`. These were B1, B2, B3, B10 and B11, and they still fail.

These cases are still open: MED-1, LOW-1 and LOW-2 below. They are an omission inside the masked block, another variable that holds the secret, a decoy `show_secret`, a brace inside a string literal, and a header included only transitively.

**Q2. Is there a false-positive risk on the real tree?**

None was found for the call-graph check. I ran `check_lcd_sink_callgraph.py` read-only against `C:\wt\checkbuild_280d180149` (HEAD `53131caf1`), and the strict UI shared-section rule included: it passed, 22779 referenced symbols, exit 0.

A survey of the same build with `parse_syms` found:

- 0 shared sections in UI objects.
- 116 shared sections in non-UI objects. These are ESP-IDF aliases, App `pid_fuzzy.c.obj` `.text.clamp_gain` shared by `clamp_gain` and `sanitize_base`, and `$N` rodata clones.

Making the rule strict everywhere would therefore false-positive at once. Limiting it to UI objects is needed. The admin-gates check passes on the tree (24 gates). INFO-3 below notes one latent false-positive path in L-shadow.

**Q3. Is "fail a shared section only in UI objects" sound?**

It is sound for its stated target: a hidden second function in a UI object. It does not cover a symbol hidden inside a non-UI object. The walk from a non-UI node follows only the referrers of the section's first symbol. See LOW-3.

**Q4. Are the evidence rows accurate?**

The counts are correct. C1-C4 plus B1-B11 make 15. On the callgraph test row, S8, S10 and S11 reproduce here: my CG3, CG1 and CG2/CG6 are CAUGHT by `FAIL: O` and `FAIL: P`.

The planted real-object result T5 cannot be checked, because its mutation JSON is private (INFO-4).

The row for `check_lcd_admin_gates.ps1` has a literal newline in it. This breaks `check_gate_negative_test_table.ps1` on dev (MED-2, fixed in this commit).

**Q5. Is there a stray B6 mutation?**

No. The range touches only:

- `tools/check_lcd_admin_gates.ps1`
- `tools/check_lcd_sink_callgraph.py`
- `tools/test_check_lcd_sink_callgraph.ps1`
- `docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md`
- `docs/audits/REVIEW_LCDFX11_2026-10-12.md`

No firmware or `ui_page_*.c` file changed. The negtest baseline PASS on `44289aa41` confirms the tree is unmutated.

## Findings

### MED-1: the masked block is pinned by a denylist, so omissions and other secret holders pass

**Location:**

- `tools/check_lcd_admin_gates.ps1:551-561`: masked-block match, denylist and return rule.
- `firmware/KilnFW/App/drivers/ui/ui_page_network.c:547-553`.

The check fails the masked `!show_secret` block only when it names a denylisted token (the local, `ap_password`, `ap_ssid`, `s_ap_qr_uri`, `wifi_prov_get_*`, `build_wifi_qr_uri`) or does not end in `return;`. It never requires that the block actually masks anything.

LCD pages are never torn down (`kiln_ui.h`), so labels keep the text from the previous refresh.

**Failure scenario:**

1. A refactor deletes `lv_label_set_text(s_ap_password_label, ...)` (mutant O1) or `lv_obj_add_flag(s_ap_qr, LV_OBJ_FLAG_HIDDEN)` (O2) from the masked block.
2. An ADMIN opens the Network page, then the LCD relocks.
3. A USER now sees the plaintext AP password, or a scannable QR code that contains `P:<password>`.

Both mutants are **MISSED**.

The same block can also show other state that holds the secret, and these are **MISSED** too:

- A1: `s_ap_qr_last`, which holds the full QR URI.
- A2: `lv_textarea_get_text(s_ap_edit_password_ta)`. The edit text area is prefilled with the password by the ADMIN editor at line 723. Line 602 clears it on close, so A2 matters only while the editor is open.

**Fix:** pin the masked block's exact shape as an allowlist, not a denylist. It should be exactly these statements:

1. The label write with the two literals, keyed on `X[0]`.
2. The QR hide.
3. `s_ap_qr_last[0] = '\0';`
4. `return;`

### MED-2: a split row in the evidence table breaks `check_gate_negative_test_table.ps1` on dev

**Location:** `docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md`, the `check_lcd_admin_gates.ps1` row (line 165/166 at `01554a779`, still present at origin/dev `ef90208eb`).

The text "the splice regex was `\r?`" contains a real line break where `\n` was meant. The table row is cut in two.

**Failure scenario:** `tools/check_gate_negative_test_table.ps1` fails on every dev run with:

```
line 166: non-table line inside the table block (split row?)
```

Every promote run then carries a failing standing check that hides new failures in the same check.

**Fixed in this commit.** The row is joined, and the regex is written as `'\\r?\n'` with an explanation. The check's other failure in this worktree (no row for `check_host_test_verdict.ps1`) is already fixed on newer dev in `2cd62abc5`.

### LOW-1: the masked-block and pre-gate scans can be steered with a decoy or a brace inside a string

**Location:** `tools/check_lcd_admin_gates.ps1:551` and `:564`.

The masked block is found with `[regex]::Match(body, 'if\s*\(!show_secret\)\s*\{([^{}]*)\}')`. That takes the first match, and the regex does not understand strings. The pre-gate region is cut at the first `IndexOf('bool show_secret')`. `Get-CachedCode` does not blank string literals.

**Mutant D1** puts a sibling-scope decoy `{ bool show_secret = true; if (!show_secret) { return; } }` before the fetch. It also makes the real masked block show `ap_password`. Both scans latch onto the decoy, so the real leak goes unscanned. **MISSED.**

**Mutant D2** adds a literal `"; return;}"` in the masked block. The `[^{}]*` group ends at the brace inside the string. A `lv_label_set_text(s_ap_password_label, ap_password)` after it is never scanned. **MISSED.**

Both mutants compile, and both show the password to a USER. This is deliberate obfuscation, not an accidental regression, so the severity is LOW.

**Fix:**

- Blank string literals before the brace scan.
- Require exactly one `bool show_secret` declaration and exactly one `if (!show_secret)` in `refresh_cb`.

### LOW-2: L-shadow scans only headers included directly

**Location:** `tools/check_lcd_admin_gates.ps1:519-528`.

**Mutant P1** adds `#pragma GCC diagnostic ignored "-Wshadow"` to `drivers/control/pid.h`. That header reaches the UI translation units only through another App header (`profile_executor.h`). The pragma switches off `-Werror=shadow` for every UI file that follows it, which re-opens the shadowed-`show_secret` class. **MISSED.**

The row's own "known gap" mentions only headers outside App, not transitive App headers.

**Fix:** follow `#include "..."` recursively, keeping a visited set, inside App.

### LOW-3: non-UI shared sections are walked through the first symbol only

**Location:**

- `tools/check_lcd_sink_callgraph.py:60`: `return names[0]` when not strict.
- `tools/check_lcd_sink_callgraph.py:155-158`: the walk follows `rev[node]` and `rev[fn]` only.

Suppose `w` in an `NON_UI_ALLOW` object calls the sink, and a global alias `evil` shares `w`'s section. Aliases like this exist in the real build: `pid_fuzzy.c.obj` `.text.clamp_gain` holds both `clamp_gain` and `sanitize_base`, and there are 116 such sections overall.

**Failure scenario:** a UI function calls `evil` by its bare name. That relocation lands in `rev["evil"]`. The walk starts from node `<obj>::w` and follows `rev["w"]` only, so the UI caller is never visited and the check passes.

The text check's L-wrapper rule (one level of non-UI wrapper) is the only backstop.

**Fix:** when walking from a node, take the union of the referrers of every symbol in its section. Add a synthetic case: a non-UI alias, with the UI calling the alias.

### INFO-1: CG4 (strict off for relocation targets) is MISSED and is an equivalent mutant

`tools/check_lcd_sink_callgraph.py:99`. Every section that has relocations is already checked strictly through its `RELOCATION RECORDS` header.

A shared UI section with no relocations of its own (only leaf code) cannot reach a sink. Only the target-side strict call sees it, and missing it is harmless for sink reachability.

### INFO-2: CG5 (`ui = True`, strict in every object) is MISSED by the synthetic test

`tools/check_lcd_sink_callgraph.py:77`. No case covers a shared section in a non-UI object that must be allowed, so the UI-only scoping is unpinned.

It fails closed: the real-build path would fail on the 116 non-UI shared sections. The regression would therefore show up as a real-build false positive, not as a hole.

**Fix:** add a synthetic case with an allowed non-UI alias.

### INFO-3: possible false positives in L-shadow (`hdrIndex` and the raw scan)

**Location:** `tools/check_lcd_admin_gates.ps1:519-520` and `:532`.

- The basename index is built over all of `App/`, including `App/test` stub headers whose names repeat real ones (`lvgl_port.h`, `build_info.h`, `nvs.h`). The first file found wins, so a UI `#include "nvs.h"` may scan a test stub instead of the real header. That can miss the real header, or flag a stub.
- The pragma scan reads the raw text without stripping comments, so a comment that mentions `_Pragma(` fails the check.

Neither of these happens on the current tree.

### INFO-4: the T5 planted real-object result cannot be verified

The `check_lcd_sink_callgraph.ps1` row says the planted shared-section object went "PASS before, FAIL after". The mutation JSON is private, so the result cannot be reproduced from the repo.

The synthetic case P covers the same logic. CG1, CG2 and CG6 are CAUGHT by `FAIL: P`.

## Negative tests (tools\negtest.ps1, base 44289aa41, baseline PASS, real tree unchanged, copies removed)

`-Preset check -PresetArg tools\check_lcd_admin_gates.ps1`:

| Mutant | Change | Verdict |
|---|---|---|
| D2 | `"; return;}"` literal in masked block, then the label shows `ap_password` | MISSED (LOW-1) |
| O1 | masked `lv_label_set_text(s_ap_password_label, ...)` deleted | MISSED (MED-1) |
| O2 | masked `lv_obj_add_flag(s_ap_qr, HIDDEN)` deleted | MISSED (MED-1) |
| A1 | masked label shows `s_ap_qr_last` | MISSED (MED-1) |
| A2 | masked label shows `lv_textarea_get_text(s_ap_edit_password_ta)` | MISSED (MED-1) |
| D1 | sibling-scope decoy `show_secret` before the fetch, plus the masked label shows `ap_password` | MISSED (LOW-1) |
| P1 | shadow pragma in `drivers/control/pid.h`, included only transitively | MISSED (LOW-2) |
| K1 (control) | `const char *pw2 = ap_password;` shown before the gate | CAUGHT `FAIL: L-secret: refresh_cb uses 'ap_password' before the show_secret gate` |
| K2 (control) | getter wrapped in `#define RV_PW` | CAUGHT `FAIL: L-sink ... outside the inventory`, `FAIL: L-secret: ... exactly one wifi_prov_get_ap_password()` |
| K3 (control) | `strcpy(s_ap_qr_uri, ap_password)` in the masked block | CAUGHT `FAIL: L-secret: refresh_cb's masked (non-ADMIN) branch references 's_ap_qr_uri'` |

`-Preset check -PresetArg tools\test_check_lcd_sink_callgraph.ps1`, mutating `tools/check_lcd_sink_callgraph.py`:

| Mutant | Change | Verdict |
|---|---|---|
| CG1 | strict off for section headers (`ui` -> `False`) | CAUGHT `FAIL: P two symbols sharing a section in a ui object fail` |
| CG2 | `parse_syms` keeps the first symbol only | CAUGHT `FAIL: P` |
| CG3 | gate-references-sink `fails.append` -> `pass` | CAUGHT `FAIL: O gate that references no sink fails` |
| CG4 | strict off for relocation targets | MISSED (INFO-1, equivalent) |
| CG5 | `ui = True` (strict everywhere) | MISSED (INFO-2, fails closed on the real build) |
| CG6 | unattributable sections not reported | CAUGHT `FAIL: P` |

Every CAUGHT result above is the check's own `FAIL:` line with exit 1. None of them is a script crash. The admin-gates check is a text scan, so no build is involved.
