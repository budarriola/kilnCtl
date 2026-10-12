# REVIEW_LCDFX14 -- adversarial review of the LCD secret hardening (2026-10-12)

Scope: `bf81a7554` (code) and `b3d908166` (docs) on origin/dev. These commits fix
`docs/audits/REVIEW_LCDFX13_2026-10-12.md` MED-1, LOW-1/2/3 and INFO-1/2. They add
`firmware/KilnFW/App/drivers/ui/ui_page_network_secret.c/.h`, which wipes the AP
Wi-Fi secret and hides it on the LCD network page until it is revealed. They also
extend the `tools/check_lcd_admin_gates.ps1` rules, change
`tools/check_lcd_sink_callgraph.py` and its test, and add the host test
`test_ui_page_network_secret.c`. Reviewed in worktree `C:\wt\rvlcdfx14_4qfbh9` at
`b3d908166`.

## Summary

No HIGH or MED findings. MED-1 is fixed: a USER or NONE session no longer sees the
AP password or its QR code, and the cached QR URI is wiped. The wipe cannot be
optimized away. The `-Werror=shadow` setup does not break the recovery or sim
builds. The new check rules are not vacuous for the cases they target, and the
negtests below confirm this. The remaining gaps are other copies of the secret
outside the inventory, a narrow CMake scan and one untested call-graph path.

| Severity | Count |
|---|---|
| HIGH | 0 |
| MED | 0 |
| LOW | 3 |
| INFO | 4 |

## Answers to the brief

**Can the secret reach the LCD, a log or a stale buffer?**
- *Timers.* The only timer is `refresh_cb` (1000 ms, `ui_page_network.c:452`). Its
  masked branch (`ui_page_network.c:549-554`) writes only the mask literal, hides
  `s_ap_qr`, wipes both QR buffers and re-encodes the non-secret `"WIFI:;;"` payload
  before it returns. The plaintext path (`:556-560`) runs only under
  `ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN)`.
- *Page switch and relock.* `ui_page_network_relock_close()` (`:597-612`) wipes
  `s_ap_qr_uri`/`s_ap_qr_last`, hides the QR and blanks it, then clears the edit
  textareas. The relock edge reaches it through `handle_lcd_relock_to_home`
  (`kiln_ui.c:139`) before `kiln_ui_show("home")`.
- *Error path.* The apply/ESP_LOGW paths log only `esp_err_to_name()`. The tap-target
  dump (`kiln_ui.c` `log_tap_targets`) logs a row's first label. On the AP stat rows
  that label is the row name ("Password"), not the value, and the password
  textarea is in password mode (`:776`) so its label shows bullets. No log path
  carries the secret.
- *Caption fallback.* The STA caption (`:510`) shows `dashboard_url`, with a fixed
  fallback of `http://kilnctl.local`. No secret is involved.
- *Stale buffers.* `s_ap_qr_uri`/`s_ap_qr_last` are covered. Two other copies are not
  (LOW-1), and LVGL heap blocks are freed without zeroing (INFO-2).

**Can the wipe be optimized away?** No. `wipe_buf` stores through
`volatile char *` (`ui_page_network_secret.c:8`), and it is called across a TU
boundary without LTO. I compiled the file with xtensa-esp32s3-elf-gcc 15.2 `-Os
-Wall -Wextra -Werror -Werror=shadow`, and again with `-flto`. Both builds were clean
and the `s8i` stores are present. Dropping `volatile` is not caught by any test (H3,
INFO-4). That is inherent: a host test cannot see dead-store elimination.

**Does `-Werror=shadow` break the recovery or sim builds?** No. The flag predates
this change. It is set per source on `drivers/ui/*.c` (`App/drivers/CMakeLists.txt:802-803`),
and lcdfx14 changed only the scan that guards it. `KilnFW_recovery/main/CMakeLists.txt`
compiles no `drivers/ui` sources. The sim build uses the same CMake, and the new
TU has no config-dependent code. The new file compiles clean under the flag, as
shown above.

**Are the new check rules vacuous?** No for the pinned masked block, the per-function
inventory and the relock body: K4 and K6 are CAUGHT. They are incomplete in reach
(LOW-1, LOW-2), and one call-graph path has no test (LOW-3).

## Findings

### LOW-1: two plaintext copies of the new AP password are never wiped and are outside the L-secret inventory

`ui_page_network.c:317` `s_ap_pending_password` and `:319` `s_ap_identity_job_password`
hold the AP password the operator typed (`:706`, `:665`). Neither is wiped after
the apply job finishes, on cancel or in `ui_page_network_relock_close()`. Both
survive a relock and sit in `.bss`, which the coredump captures because
`CONFIG_ESP_COREDUMP_CAPTURE_DRAM=y` (`sdkconfig.defaults:467`).
`$invWant` (`tools/check_lcd_admin_gates.ps1:652-657`) lists only `s_ap_qr_uri`,
`s_ap_qr_last`, `s_ap_qr` and `s_ap_password_label`. A future reader of either
buffer therefore passes the check. Negtests E6 (STA caption shows
`s_ap_pending_password`) and E8 (file-scope alias of `s_ap_identity_job_password`
shown in the SSID label before the gate) both put the password on screen for a
USER session, and both were MISSED.

No current code displays these buffers, so this is LOW. The password also lives in
`wifi_prov` RAM for as long as the AP runs.

Fix: wipe both buffers with `ui_page_network_secret_wipe` (or a wider variant) when
the job completes, on cancel and in `ui_page_network_relock_close()`. Add both
buffers, and the password textarea's `lv_textarea_get_text` readers, to `$invWant`
with reviewed counts.

### LOW-2: the L-shadow CMake scan misses first-party components that reach the UI TUs through REQUIRES

The scan (`tools/check_lcd_admin_gates.ps1:511-519`) walks `firmware/KilnFW` but
excludes `components/`, and it never looks outside `firmware/KilnFW`. Two
first-party components escape it:
- `firmware/KilnFW/components/kilnlink/CMakeLists.txt` is a first-party wrapper, and
  `drivers` REQUIRES `kilnlink`.
- `firmware/hwAbstraction/idf/hwabstraction_esp/CMakeLists.txt` comes in through
  `EXTRA_COMPONENT_DIRS`, and `drivers` REQUIRES `hwabstraction_esp`.

A `target_compile_options(${COMPONENT_LIB} INTERFACE -w)` in either file propagates
to every `drivers/ui/*.c` and silences `-Werror=shadow`. `-w` suppresses it in
either option order. Negtests C3 and C4 were both MISSED.

Fix: scan every first-party component dir that the drivers component REQUIRES. At
minimum add `components/kilnlink` and `firmware/hwAbstraction/idf`, and exclude
only vendored trees (`components/lvgl`, `managed_components`). Alternatively,
inspect `build/compile_commands.json` for a UI source, when it exists, for `-w`
or `-Wno-*shadow` after `-Werror=shadow`.

### LOW-3: no test reaches the call-graph clone-name loop; the two seed paths are tested only together

`tools/check_lcd_sink_callgraph.py:155-158` adds `.constprop`/`.isra`/... clone names
that appear only as referrers (not as defined sections) to `sink_names`. No case in
`tools/test_check_lcd_sink_callgraph.ps1` needs it. CG3 (loop removed) was MISSED,
and CG34 (loop plus the sink-definition seed) was also MISSED. The sink-definition
seed (`:163-164`, CG4) and the same-object caller seed (`:165-168`, CG5) each mask the
other. Each removed alone was MISSED, and case T fails only when both are removed
(CG45 CAUGHT). Case S passes even with all three removed, because the
`sink_nodes` alias union covers it.

Fix: add a case where a sink clone is referenced but defined in an object outside
the scan set (this exercises `:155-158` alone). Add one case for each seed path that
the other cannot reach, or drop the redundant path.

### INFO-1: relock does not reset the plaintext password label

`ui_page_network_relock_close()` does not rewrite `s_ap_password_label`. After an
ADMIN session relocks, the label keeps the plaintext string in its LVGL heap text
until the next `refresh_cb` tick. That tick masks it within 1 s when the AP is
active. When the AP is not active, the stale text stays in the hidden section
indefinitely. Nothing is visible, because PIN entry takes longer than one tick and
the section is hidden while the AP is off. Fix: set the mask literal in
`relock_close` next to the QR blanking.

### INFO-2: LVGL frees secret-bearing heap without zeroing

`lv_qrcode_update` (`components/lvgl/src/libs/qrcode/lv_qrcode.c` ~124-137) does
`lv_malloc` on `qr0` and `data_tmp`, copies the URI and calls `lv_free` without
zeroing. `lv_label_set_text`/`lv_textarea_set_text` do the same with the old
text. The freed `qr0` bitmap can be decoded. With DRAM in the coredump, a crash
shortly after an ADMIN view can leak the AP password. Accepted residual risk:
`wifi_prov` holds the live password in RAM anyway.

### INFO-3: the masked branch re-encodes the blank QR every second, indefinitely

`ui_page_network.c:553` calls `lv_qrcode_update(blank)` on every 1 s tick while the page
is masked. That is a QR encode and two heap round-trips per second for no visual
change. A `s_ap_qr_blanked` flag, set here and cleared on the reveal path, would
avoid it. The L-secret pinned block would need the matching update.

### INFO-4: `volatile` in `wipe_buf` and the blank-payload macro are not pinned by any test or rule

H3 (drop `volatile`) is MISSED by the host tests. This is expected, because dead-store
elimination is not observable on the host, and today's cross-TU, non-LTO build
keeps the stores anyway. The L-macro regex does not cover
`UI_PAGE_NETWORK_AP_QR_BLANK`, so a redefinition of the macro to a secret-bearing
payload is not flagged by name. The pinned masked block does still require the
literal macro name. Optional: a text rule that `wipe_buf` stores through a
`volatile` pointer, and add the macro to the L-macro list.

## Negtests (run by this review)

All runs used `tools\negtest.ps1` from `C:\wt\rvlcdfx14_4qfbh9` at `b3d908166`, every
baseline passed, and every run reported `real_tree_unchanged: true`.

| ID | Target / preset | Mutation | Verdict | Evidence |
|---|---|---|---|---|
| E6 | check `tools/check_lcd_admin_gates.ps1` | STA caption shows `s_ap_pending_password` | MISSED | LOW-1 |
| E8 | check `tools/check_lcd_admin_gates.ps1` | file-scope alias of `s_ap_identity_job_password` shown in SSID label | MISSED | LOW-1 |
| C3 | check `tools/check_lcd_admin_gates.ps1` | `INTERFACE -w` in `components/kilnlink/CMakeLists.txt` | MISSED | LOW-2 |
| C4 | check `tools/check_lcd_admin_gates.ps1` | `INTERFACE -w` in `hwAbstraction/idf/hwabstraction_esp/CMakeLists.txt` | MISSED | LOW-2 |
| K4 | check `tools/check_lcd_admin_gates.ps1` | relock drops `lv_obj_add_flag(s_ap_qr, HIDDEN)` | CAUGHT | `FAIL: L-secret: 's_ap_qr' is mentioned 2 time(s) in ui_page_network_relock_close, reviewed count is 3` |
| K6 | check `tools/check_lcd_admin_gates.ps1` | masked branch blanks via `update_qr_if_changed` (caches payload in `s_ap_qr_last`) | CAUGHT | `FAIL: L-secret: refresh_cb's masked '!show_secret' block must be exactly ...` |
| CG3 | check `tools/test_check_lcd_sink_callgraph.ps1` | clone-name loop `sink_names.add(b)` -> `pass` | MISSED | LOW-3 |
| CG4 | check `tools/test_check_lcd_sink_callgraph.ps1` | drop sink-definition seed | MISSED | LOW-3 |
| CG5 | check `tools/test_check_lcd_sink_callgraph.ps1` | drop same-object caller seed | MISSED | LOW-3 |
| CG34 | check `tools/test_check_lcd_sink_callgraph.ps1` | CG3 + CG4 | MISSED | LOW-3 |
| CG45 | check `tools/test_check_lcd_sink_callgraph.ps1` | CG4 + CG5 | CAUGHT | `FAIL: T ui call reaching a sink clone via a wrapper fails` |
| CG345 | check `tools/test_check_lcd_sink_callgraph.ps1` | CG3 + CG4 + CG5 | CAUGHT | `FAIL: T ui call reaching a sink clone via a wrapper fails` |
| CG7 | check `tools/test_check_lcd_sink_callgraph.ps1` | INFO-2 `distinct` set collapses to one entry | CAUGHT | `FAIL: P two symbols sharing a section in a ui object fail` |
| H3 | kilnfw-host | `wipe_buf` drops `volatile` | MISSED (expected) | INFO-4; mutant compiled, all host tests pass |
| H4 | kilnfw-host | `wipe_buf(last, last_cap / 2)` | CAUGHT | mutant compiled (no BUILD FAILURES), `FAIL ...test_ui_page_network_secret.c:26: last-encoded buffer fully zeroed`, `RUN FAILURES (1)` |

The host preset used `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
The check-preset CAUGHT rows are failing check assertions printed by the check
itself. Nothing is compiled there. The MISSED logs contain no `FAIL` line at all
and end with `LCD admin gate check passed`.
