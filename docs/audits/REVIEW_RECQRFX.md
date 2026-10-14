# REVIEW_RECQRFX: adversarial review of the REVIEW_RECQR LOW-1..5 fixes

Reviewed: `18f2d5446` (fixes) and `8fa90166f` (fix-status lines in
`docs/audits/REVIEW_RECQR.md`) on origin/dev. Read-only review, no board access.
Reviewer worktree at origin/dev `8fa90166f`.

Result: **HIGH 0, MED 0, LOW 4, INFO 6.**

## Scope checked

- Can the compiler drop the volatile wipe, and does it reach every copy of the passphrase?
- Is the +1024 B main-task stack justified, and does it fit the recovery image's internal RAM?
- Do the policy helpers in `recovery_lcd_policy.h` match the real draw path in `recovery_lcd.c`?
- Does the PC reader's upper-right-quarter filter match the real QR position without refusing the real QR?
- Is the passphrase logged anywhere?
- Are the fixer's claimed mutants real compiled mutants with failing assertions? Independent negtests are below.

## LOW

### LOW-1: QR paint order and hide/show wiring in `draw_status()` are pinned only by substring presence

`firmware/KilnFW_recovery/main/recovery_lcd.c:396` and `:477` hold two calls:

- the pre-text `draw_qr_area(false)`, which blanks the area when the QR is hidden;
- the post-text `draw_qr_area(true)`, which paints the QR when it is visible.

`check_recovery_lcd_policy.ps1` tests the predicates `rlcd_qr_visible`/`rlcd_qr_draw_after_text` by compiling them. Its `qr_handcopy` mutant is a source scan; it confirms the helpers are named in `recovery_lcd.c`. The check never compiles `recovery_lcd.c`, so these call sites are unpinned. Independent negtest (`check` preset, baseline 125 assertions PASS), all MISSED:

| mutant | edit | consequence |
|---|---|---|
| `lcd_qr_shown_while_hidden` | `:396` `draw_qr_area(false)` -> `draw_qr_area(true)` | **The Wi-Fi passphrase QR is painted while the AP is down or under an error banner.** This is the state LOW-1 of REVIEW_RECQR was fixing. |
| `lcd_no_prepaint_hidden` | drop the `:396` pre-paint | Stale `s_qr` modules stay on screen after the QR should be hidden, because `s_qr` is never cleared (`:127`). |
| `lcd_old_postpaint` | restore the pre-fix unconditional post-paint | Reintroduces the original order bug. |

Failure scenario: a later edit to `draw_status()` flips the boolean or drops the
pre-paint, and every check still passes. The passphrase QR then shows for an AP
that is not up. Exposure is low: only someone at the LCD can see it, and the
passphrase is meant to be shown on the LCD anyway. That is why this is LOW.
Fix: in the check, pin the literal call sites and their order. The minimum is
the exact text `!rlcd_qr_draw_after_text(show_qr) && draw_qr_area(false)`
appearing before the text block, and
`rlcd_qr_draw_after_text(show_qr) && draw_qr_area(true)` appearing after it.
A better fix is a host-compiled `draw_status()` against a stub panel.

### LOW-2: LOW-3 of REVIEW_RECQR is fixed only for the hidden-QR case; the visible QR still clips fault bands, and the comment is stale

The QR area is x 356..479, y 0..123 (`recovery_lcd.c:354`). When the QR is visible it is painted after the text (`:477`). It therefore still overwrites the right part of every text band in y 0..123:

- `Y_BOOT` (50..63), which carries the NVS / BOOT GUARD lines;
- `Y_RELAY` (104..117), which carries RELAY CTRL FAULT.

The QR is visible in the normal AP-up state, so a relay-controller fault line is cut at x=356 exactly when the operator is reading the screen. The comment above `draw_qr_area` (`:350-353`) still describes the old "drawn after the text lines ... fault bands this paints over" behaviour for both cases.
Failure scenario: a long RELAY CTRL FAULT detail string loses its tail behind the QR. Cosmetic only, because the fault head word stays left of x=356.
Fix: either wrap or shorten the band text to x<356 whenever `show_qr` is set, or move the QR below y=123 out of the fault bands. Also update the comment.

### LOW-3: `rlcd_qr_visible()` claims to be the single predicate, but the text branch is still a hand copy

`recovery_lcd_policy.h:63` says "One predicate so the QR cannot drift from the
text branch". The predicate is defined at `:65`. The text branch in
`recovery_lcd.c` still re-derives the same state by hand:

- `if (s_err_head[0] || s_ap_down)`, then
- `else if (s_net_none || s_net_storage_fail)`, then
- `else if (s_net_set)` at `:455`.

Independent mutant `lcd_text_ignores_apdown` drops `s_ap_down` from the text branch: MISSED.
Failure scenario: an edit adds a state to only one of the two. The text then shows the passphrase line while the QR is hidden, or the reverse. This is the drift the comment says cannot happen.
Fix: drive the text branch from the same predicate (`if (!show_qr) {...}`), or soften the comment and pin the branch in the check.

### LOW-4: the reader's region call-site wiring, y bound and upscale filtering are untested

`tools/PcTools/src/kilnctrl/lcd_ap_reader.py:960` passes `RECOVERY_QR_REGION` (`:895`) to `_wifi_qr_payloads`. The QR-selection tests patch the decoder with
`_pin_payloads` (`tools/PcTools/tests/test_lcd_ap_reader.py:384-385`, `lambda img, region=None: ...`). That patch ignores `region`, so those tests never exercise the filter. Independent pytest negtest:

- MISSED:
  - `py_callsite_no_region` (call without the region);
  - `py_region_x_only` (y bound removed);
  - `py_upscale_unfiltered` (filter skipped on the 2-4x retry passes).
- CAUGHT:
  - `py_region_no_scale`;
  - `py_region_whole_frame`;
  - `py_ssid_ge`.

Failure scenario: a refactor drops the region argument at `:960`. A foreign WIFI: QR anywhere in frame, such as a phone held near the bench, is then accepted again. This is the condition LOW-4 of REVIEW_RECQR was fixing, and the tests stay green.
Fix: add one end-to-end test that renders a decoy WIFI: QR outside the upper-right quarter and runs it through the real `_wifi_qr_payloads`. Add one test with a decoy that decodes only at an upscale.

## INFO

- **INFO-1: the wipe is correct.**
  - `rqr_secure_zero` (`recovery_wifi_qr.c:8`) is a volatile byte loop, which the compiler cannot elide.
  - It covers the stack `payload` (`:66`), the encoder scratch `tmp` (`:68`), and the overflow `out` (`:49`).
  - The vendored nayuki `qrcodegen_encodeText` copies byte-mode text into `tempBuffer` (`qrcodegen.c:154`), and wiping `tmp` covers that copy.
  - The following copies remain by design and are out of scope for this fix:
    - `s_net_pass`;
    - the QR bitmap `s_qr`, which is a deliberate encoding of the passphrase and persists for the boot;
    - the Wi-Fi driver config;
    - the transient scanline buffer `s_line`.
  - There is no LVGL in the recovery image.
- **INFO-2: the stack change is safe but unmeasured.**
  - `sdkconfig.defaults:88` raises `CONFIG_ESP_MAIN_TASK_STACK_SIZE` to 4608. The 1024 B extra is transient: `app_main` returns, which frees the main task. Recovery `heap_internal_min_free` was about 246 KB on the bench (BENCH_TEST_LOG 2026-10-03), far above the 8 KB floor.
  - The bump is defensive, not sized from a measured high-water mark. Read the main-task hwm on the bench once to confirm it.
  - `test_main_stack` (`test_recovery_wifi_qr.c:173`) reads only the first `CONFIG_ESP_MAIN_TASK_STACK_SIZE=` match. A later duplicate line in `sdkconfig.defaults` would win in the build while the test still passes.
- **INFO-3: region geometry was computed, not measured.**
  - I projected the panel QR centre (x 417, y 62 of 480x320) through the bench `FRAME_CORNERS`. It lands at about (922, 222) of 1280x720, which is (0.72, 0.31): well inside `(0.5, 0.0, 1.0, 0.5)`.
  - The synthetic end-to-end tests run the real zxing on renders at `TRUE_CORNERS` and pass, so the filter does not refuse the real QR position.
  - No real recovery-QR camera capture exists in `logs/`.
  - The comment at `lcd_ap_reader.py:893` gives y ~70..230. The computed span is about y 118..327.
- **INFO-4: no passphrase logging.**
  - None of the following log lines carries the passphrase or the QR payload:
    - the `ESP_LOG*` lines at `recovery_wifi.c:322` and `:389`;
    - `recovery_lcd.c:615`.
  - Reader error strings name the SSID or a count, never the password.
- **INFO-5: the fixer's claimed mutants are real.** I re-ran them through `tools/negtest.ps1`. Each compiled and failed at a runtime assertion:
  - `stack_3584` failed at test:187;
  - `securezero_partial` failed at test:199 and test:52;
  - `overflow_halfwipe` failed at test:52;
  - pytest `multi_qr_dropped`, `empty_ssid_dropped` and `len_loose` were all CAUGHT.

  Two of the fixer's mutants are source-scan assertions, not compiled behaviour: `payload_memset` and `qr_handcopy`.
- **INFO-6: dependency.** `zxing-cpp>=3.0` is the floor, and `uv.lock` pins 3.1.1.

## Negtest log

| run | preset | mutants | verdict |
|---|---|---|---|
| `20261014_082959_8pje` | check `check_recovery_wifi_qr.ps1` (baseline 45 asserts) | c_stack_3584, c_securezero_partial, c_overflow_halfwipe | all CAUGHT (runtime FAIL, compiled) |
| `20261014_082959_xvrc` | check `check_recovery_lcd_policy.ps1` (baseline 125 asserts) | lcd_no_prepaint_hidden, lcd_old_postpaint, lcd_text_ignores_apdown, lcd_qr_shown_while_hidden | all MISSED (LOW-1, LOW-3) |
| `20261014_082959_qt3z` | pytest `test_lcd_ap_reader.py` | py_region_no_scale, py_region_whole_frame, py_ssid_ge / py_callsite_no_region, py_region_x_only, py_upscale_unfiltered | 3 CAUGHT / 3 MISSED (LOW-4) |
| `20261014_083802_x22n` | pytest `test_lcd_ap_reader.py` | py_multi_qr_dropped, py_empty_ssid_dropped, py_len_loose | all CAUGHT |

Every run reported `real_tree_unchanged: true`.
