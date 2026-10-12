# test_check_lcd_sink_callgraph.ps1 -- synthetic objdump -r dumps through check_lcd_sink_callgraph.py.
#   A: sink reached only via the gate edge (btn_clicked_cb -> clear_action)    -> exit 0
#   B: ui wrapper calls the sink directly                                       -> exit 1
#   C: ui function reaches the sink through a non-ui helper (macro/wrapper)    -> exit 1
#   D: ui function pointer table references clear_action                       -> exit 1
#   E: unreviewed non-UI caller of the sink                                     -> exit 1
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
function Run($specs) {
    $argv = @($ScriptUnderTest)
    foreach ($s in $specs) { $argv += @("--reloc-dump", $s) }
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

    $r = Run @()
    $r2 = & python $ScriptUnderTest --build-dir (Join-Path $d "nobuild") | Out-String
    Check ($LASTEXITCODE -eq 3 -and $r2 -match 'SKIP') "F no build skips"
} finally { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }
if ($fail) { Write-Host "$fail failure(s)"; exit 1 }
Write-Host "test_check_lcd_sink_callgraph passed"
exit 0
