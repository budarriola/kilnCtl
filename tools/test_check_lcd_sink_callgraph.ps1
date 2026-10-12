# test_check_lcd_sink_callgraph.ps1 -- synthetic objdump -r dumps through check_lcd_sink_callgraph.py.
#   A: sink reached only via the gate edge (btn_clicked_cb -> clear_action)    -> exit 0
#   B: ui wrapper calls the sink directly                                       -> exit 1
#   C: ui function reaches the sink through a non-ui helper (macro/wrapper)    -> exit 1
#   D: ui function pointer table references clear_action                       -> exit 1
#   E: unreviewed non-UI caller of the sink                                     -> exit 1
#   F: no build objects present (real-build wrapper path)                    -> exit 3 SKIP
#   G: sinks renamed / absent (check must not pass vacuously)                  -> exit 1
#   H: clear_action referenced by bare name (global) from a non-ui object      -> exit 1
#   I: btn_clicked_cb in a DIFFERENT object references the gated action         -> exit 1
#   J: .iram1.N relocation section attributed via the symbol table              -> exit 1 (reaches sink)
#   L: sink with no referrers                                                  -> exit 1
#   M: gate edge btn_clicked_cb -> clear_action missing                        -> exit 1
#   N: ui reaches sink via a static helper chain inside an allowlisted object (node-keyed reverse walk) -> exit 1
#   O: gate clear_action no longer references any sink (gate renamed/hollowed)  -> exit 1
#   P: two symbols share one section in a ui object (hidden 2nd function)        -> exit 1
#   Q: non-ui alias shares a section with an allowlisted sink caller; a ui function calls the alias -> exit 1 (R: alias unused -> exit 0)
#   S: alias of a sink (own section symbol) called from ui                      -> exit 1 (REVIEW_LCDFX13 LOW-3)
#   T: .constprop clone of a sink reached via a wrapper called from ui          -> exit 1
#   U: two IPA-ICF-identical (same addr+size) ui symbols share a section        -> exit 0 (INFO-2)
#   K: unattributable numeric section without a symbol-table entry             -> exit 1
# -ScriptUnderTest lets negtest.ps1 point this at a mutated copy (must live in tools\).
# checkcache: ok
param([string]$ScriptUnderTest)
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $ScriptUnderTest) { $ScriptUnderTest = Join-Path $here 'check_lcd_sink_callgraph.py' }
$fail = 0
function Check($ok, $msg) { if ($ok) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:fail++ } }
$d = Join-Path ([IO.Path]::GetTempPath()) ("lcdcg_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $d | Out-Null
function Sec($name, $targets) {
    $s = "RELOCATION RECORDS FOR [$name]:`nOFFSET   TYPE              VALUE`n"
    $i = 0
    foreach ($t in $targets) { $s += ("{0:x8} R_XTENSA_SLOT0_OP     {1}`n" -f ($i * 3), $t); $i++ }
    return $s + "`n"
}
function Dump($file, $text) { [IO.File]::WriteAllText((Join-Path $d $file), $text); return (Join-Path $d $file) }
function Run($specs, $syms = @()) {
    $argv = @($ScriptUnderTest)
    foreach ($s in $specs) { $argv += @("--reloc-dump", $s) }
    foreach ($s in $syms) { $argv += @("--sym-dump", $s) }
    $out = & python @argv 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Out = $out }
}
try {
    $dash = Dump "dash.txt" ((Sec ".text.dashboard_safety_clear_trip" @("safety_link_send_clear_trip")) + (Sec ".text.safety_clear_trip_post_handler" @("dashboard_safety_clear_trip")))
    $link = Dump "link.txt" (Sec ".text.safety_link_send_clear_trip" @("memcpy"))
    $gateGood = (Sec ".text.clear_action" @("dashboard_safety_clear_trip")) + (Sec ".text.btn_clicked_cb" @(".text.clear_action"))
    $base = @("drivers/http/dashboard_exec_http.c.obj=$dash", "drivers/safety/safety_link_tx.c.obj=$link")
    $safetyOk = Dump "safetyok.txt" $gateGood
    $r = Run ($base + "drivers/ui/ui_page_safety.c.obj=$safetyOk")
    Check ($r.Code -eq 0) "A gate edge only passes: $($r.Out)"

    $b = Dump "b.txt" ($gateGood + (Sec ".text.other_cb" @("dashboard_safety_clear_trip")))
    $r = Run ($base + "drivers/ui/ui_page_safety.c.obj=$b")
    Check ($r.Code -eq 1 -and $r.Out -match 'other_cb') "B direct wrapper in ui fails"

    $helper = Dump "helper.txt" (Sec ".text.do_clear" @("dashboard_safety_clear_trip"))
    $c = Dump "c.txt" ($gateGood)
    $c2 = Dump "c2.txt" (Sec ".text.home_cb" @("do_clear"))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$c", "drivers/misc/helper.c.obj=$helper", "drivers/ui/ui_page_home.c.obj=$c2"))
    Check ($r.Code -eq 1 -and $r.Out -match 'home_cb') "C ui reaches sink via non-ui helper fails"

    $dd = Dump "d.txt" ($gateGood + (Sec ".data.ops_table" @(".text.clear_action")))
    $r = Run ($base + "drivers/ui/ui_page_safety.c.obj=$dd")
    Check ($r.Code -eq 1 -and $r.Out -match 'ops_table') "D table referencing gated action fails"

    $e = Dump "e.txt" (Sec ".text.sneaky" @("dashboard_safety_clear_trip"))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/misc/newcaller.c.obj=$e"))
    Check ($r.Code -eq 1 -and $r.Out -match 'unreviewed') "E unreviewed non-UI caller fails"

    # G: both sinks renamed everywhere, plus an ungated direct call from a ui function.
    $dashR = Dump "dashr.txt" ((Sec ".text.dashboard_safety_clear_trip_x" @("safety_link_send_clear_trip_x")) + (Sec ".text.safety_clear_trip_post_handler" @("dashboard_safety_clear_trip_x")))
    $linkR = Dump "linkr.txt" (Sec ".text.safety_link_send_clear_trip_x" @("memcpy"))
    $g = Dump "g.txt" ($gateGood + (Sec ".text.other_cb" @("dashboard_safety_clear_trip_x")))
    $r = Run @("drivers/http/dashboard_exec_http.c.obj=$dashR", "drivers/safety/safety_link_tx.c.obj=$linkR", "drivers/ui/ui_page_safety.c.obj=$g")
    Check ($r.Code -eq 1 -and $r.Out -match 'sink dashboard_safety_clear_trip ') "G renamed sinks fail (not a silent no-op)"

    # H: a global clear_action referenced by bare name from a non-ui object.
    $h = Dump "h.txt" (Sec ".text.dashboard_lcd_y" @("clear_action"))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/http/other.c.obj=$h"))
    Check ($r.Code -eq 1 -and $r.Out -match 'bare name') "H bare-name clear_action reference fails"

    # I: a btn_clicked_cb in another object referencing the gated action.
    $i1 = Dump "i.txt" (Sec ".text.btn_clicked_cb" @(".text.clear_action"))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk"))
    $gateObjCb = Dump "i2.txt" ((Sec ".text.clear_action" @("dashboard_safety_clear_trip")) + (Sec ".text.other_cb" @(".text.clear_action")))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$gateObjCb", "drivers/ui/ui_page_home.c.obj=$i1"))
    Check ($r.Code -eq 1) "I btn_clicked_cb elsewhere / missing gate edge fails"
    $i3 = Dump "i3.txt" ($gateGood + (Sec ".text.other_cb" @("clear_action")))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$i3"))
    Check ($r.Code -eq 1 -and $r.Out -match 'other_cb') "I2 non-gate caller of clear_action fails"

    # L: sinks defined but one has no referrer at all.
    $dl = Dump "dl.txt" (Sec ".text.dashboard_safety_clear_trip" @("memcpy"))
    $ll = Dump "ll.txt" (Sec ".text.safety_link_send_clear_trip" @("memcpy"))
    $r = Run @("drivers/http/dashboard_exec_http.c.obj=$dl", "drivers/safety/safety_link_tx.c.obj=$ll", "drivers/ui/ui_page_safety.c.obj=$safetyOk")
    Check ($r.Code -eq 1 -and $r.Out -match 'has no referrers') "L sink without referrers fails"
    # M: gate edge missing (clear_action reaches the sink, but nothing references clear_action).
    $mg = Dump "mg.txt" (Sec ".text.clear_action" @("dashboard_safety_clear_trip"))
    $r = Run ($base + "drivers/ui/ui_page_safety.c.obj=$mg")
    Check ($r.Code -eq 1 -and $r.Out -match 'gate edge') "M missing gate edge fails"

    # J: IRAM wrapper whose section name is numeric; the symbol table maps it to the function.
    $jr = Dump "j.txt" (Sec ".iram1.5" @("dashboard_safety_clear_trip"))
    $js = Dump "js.txt" ("SYMBOL TABLE:`n00000000 g     F .iram1.5`t00000010 iram_wrapper`n")
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/ui/ui_page_home.c.obj=$jr")) @("drivers/ui/ui_page_home.c.obj=$js")
    Check ($r.Code -eq 1 -and $r.Out -match 'iram_wrapper') "J IRAM wrapper in ui reaching the sink fails, named by function"
    # K: same section without a symbol-table entry -> cannot attribute -> fail
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/ui/ui_page_home.c.obj=$jr"))
    Check ($r.Code -eq 1 -and $r.Out -match 'cannot be attributed') "K unattributable relocation section fails"

    # N: static helper s_do (object-qualified node key) in an allowlisted object calls the sink; global w_clear
    # references .text.s_do; a ui function references w_clear by bare name. Only the node-keyed reverse walk
    # (rev.get(node)) follows s_do -> w_clear, so removing it makes this pass.
    $dashN = Dump "dashn.txt" ((Get-Content -Raw $dash) + (Sec ".text.s_do" @("dashboard_safety_clear_trip")) + (Sec ".text.w_clear" @(".text.s_do")))
    $hn = Dump "hn.txt" (Sec ".text.home_cb" @("w_clear"))
    $r = Run @("drivers/http/dashboard_exec_http.c.obj=$dashN", "drivers/safety/safety_link_tx.c.obj=$link", "drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/ui/ui_page_home.c.obj=$hn")
    Check ($r.Code -eq 1 -and $r.Out -match 'home_cb') "N ui reaches sink via static helper chain in allowlisted object fails"

    # O (REVIEW_LCDFX11 LOW-4): the gate edge btn_clicked_cb -> clear_action exists but clear_action references no sink.
    $og = Dump "og.txt" ((Sec ".text.clear_action" @("memcpy")) + (Sec ".text.btn_clicked_cb" @(".text.clear_action")))
    $r = Run ($base + "drivers/ui/ui_page_safety.c.obj=$og")
    Check ($r.Code -eq 1 -and $r.Out -match 'does not reference a trip-clear sink') "O gate that references no sink fails"

    # P (REVIEW_LCDFX11 LOW-2): a second symbol shares the gate caller's section, so its edges would be attributed to the first.
    $ps = Dump "ps.txt" ("SYMBOL TABLE:`n00000000 l     F .text.btn_clicked_cb`t0000000e btn_clicked_cb`n00000010 l     F .text.btn_clicked_cb`t0000000b evil`n")
    $r = Run ($base + "drivers/ui/ui_page_safety.c.obj=$safetyOk") @("drivers/ui/ui_page_safety.c.obj=$ps")
    Check ($r.Code -eq 1 -and $r.Out -match 'shared by btn_clicked_cb, evil') "P two symbols sharing a section in a ui object fail"

    # Q (REVIEW_LCDFX12 LOW-3): in an allowlisted non-UI object an alias shares a section with the sink caller; ui calls the alias.
    $qs = Dump "qs.txt" ("SYMBOL TABLE:`n00000000 g     F .text.safety_clear_trip_post_handler`t0000000e safety_clear_trip_post_handler`n00000000 g     F .text.safety_clear_trip_post_handler`t0000000e evil_alias`n")
    $qh = Dump "qh.txt" (Sec ".text.home_cb" @("evil_alias"))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/ui/ui_page_home.c.obj=$qh")) @("drivers/http/dashboard_exec_http.c.obj=$qs")
    Check ($r.Code -eq 1 -and $r.Out -match 'home_cb') "Q ui call to a non-ui alias sharing a sink caller's section fails"
    $qh2 = Dump "qh2.txt" (Sec ".text.home_cb" @("memcpy"))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/ui/ui_page_home.c.obj=$qh2")) @("drivers/http/dashboard_exec_http.c.obj=$qs")
    Check ($r.Code -eq 0) "R unused non-ui alias passes (no false positive): $($r.Out)"

    # S (REVIEW_LCDFX13 LOW-3): an alias of the sink, in the sink's own section; ui calls the alias.
    $ss = Dump "ss.txt" ("SYMBOL TABLE:`n00000000 g     F .text.safety_link_send_clear_trip`t0000000e safety_link_send_clear_trip`n00000000 g     F .text.safety_link_send_clear_trip`t0000000e sink_alias`n")
    $sh = Dump "sh.txt" (Sec ".text.home_cb" @("sink_alias"))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/ui/ui_page_home.c.obj=$sh")) @("drivers/safety/safety_link_tx.c.obj=$ss")
    Check ($r.Code -eq 1 -and $r.Out -match 'home_cb') "S ui call to an alias of a sink fails"
    # T: a .constprop clone of the sink, called by a global wrapper in the same object; ui calls the wrapper.
    $tl = Dump "tl.txt" ((Sec ".text.safety_link_send_clear_trip.constprop.0" @("memcpy")) + (Sec ".text.sink_wrapper" @(".text.safety_link_send_clear_trip.constprop.0")))
    $th = Dump "th.txt" (Sec ".text.home_cb" @("sink_wrapper"))
    $r = Run @("drivers/http/dashboard_exec_http.c.obj=$dash", "drivers/safety/safety_link_tx.c.obj=$tl", "drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/ui/ui_page_home.c.obj=$th")
    Check ($r.Code -eq 1 -and $r.Out -match 'home_cb') "T ui call reaching a sink clone via a wrapper fails"
    # U (INFO-2): identical code (same address and size) sharing a ui section is not a hidden function.
    $us = Dump "us.txt" ("SYMBOL TABLE:`n00000000 l     F .text.cb_a`t0000000e cb_a`n00000000 l     F .text.cb_a`t0000000e cb_b`n")
    $uh = Dump "uh.txt" (Sec ".text.cb_a" @("memcpy"))
    $r = Run ($base + @("drivers/ui/ui_page_safety.c.obj=$safetyOk", "drivers/ui/ui_page_home.c.obj=$uh")) @("drivers/ui/ui_page_home.c.obj=$us")
    Check ($r.Code -eq 0) "U identical ICF-merged ui symbols are not 'cannot be attributed': $($r.Out)"

    $nb = Join-Path $d "nobuild"
    New-Item -ItemType Directory -Force -Path $nb | Out-Null
    $r2 = & python $ScriptUnderTest --build-dir $nb | Out-String
    Check ($LASTEXITCODE -eq 3 -and $r2 -match 'SKIP') "F no build skips"
} finally { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }
if ($fail) { Write-Host "$fail failure(s)"; exit 1 }
Write-Host "test_check_lcd_sink_callgraph passed"
exit 0
