# Release gate vacuity audit: UI, js and web-commission checks, 2026-10-08

Scope: the `check_ui_*` rows, `check_js_host_tests`, `check_ui_content_smoke`,
`check_web_commission_cdp_driver`, `check_zones_per_zone_field_drift` and `selfcheck.py`.
Method: `tools/negtest.ps1` applied each mutation in a throwaway worktree (baseline required
green, real tree untouched). Not covered: `test_check_ui_responsive_sweep` (stays NOT AUDITED).

| Check | Mutation | Result |
|---|---|---|
| `check_ui_relay_reset_removed` | ui_topbar.c relay_cycles_reset() commented; flex align END->START; warning block moved after gear block | RED all 3 |
| `check_ui_shell_layout` | theme.css --kc-shell-max 100%; zones_page.html #zones grid 1fr; net/ota_page.html max-width 600px | RED all 3 |
| `check_ui_responsive_sweep` | #channels / #zones given min-width 900px | RED both |
| `check_ui_test_click_result_mirror_drift` | protocol.py 6->7; decoder entry dropped; header 0x06u->0x16u | RED all 3 |
| `check_zones_per_zone_field_drift` | client hyst_c dropped; form key minons renamed; firmware z%u_hystc renamed | RED all 3 |
| `selfcheck.py` | crc16 poly 0x1023; FRAME_DELIM 0x7F | RED both |
| `check_ui_content_smoke` | auxrow class; class="fix"; set('flashChip'); nav.js /readinessX | RED all 4 |
| `check_js_host_tests` | long-press x10; resetZone tie-break; self-reference forced:false; process.exit(1) | RED all 4 |

## Defects found and fixed

- `check_js_host_tests`: mutating `chColor` to return `CH_COLORS[0]` stayed GREEN because no
  host test exercised `chColor`. This was a coverage gap, not a runner defect. Fix:
  `test_firing_chart.js` now pins chColor(0/1/5) and the modulo wrap; the mutation is RED with
  it. Two earlier mutations were caught only through the extraction-marker error and were redone
  as behaviour changes. Cycle-guard-disabled mutations were avoided (infinite loop).
- `check_web_commission_cdp_driver`: `===` -> `.includes` on the aria-label match and
  `querySelector` -> `querySelectorAll().pop()` both stayed GREEN (20/20), because the fixture
  held one aria-label element and one css match. Fix: a decoy button whose label contains the
  real label, placed first, plus two `.twin` buttons, with 3 new assertions (23 total). With the
  fix both mutations are RED on named assertions (not on the harness timeout); against origin/main
  without the fix both stay GREEN. The post-cursor mutation was RED already.
