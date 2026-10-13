# check_code_only_lines.ps1 -- pins lib_code_only_lines.ps1's tokenizer edge cases (REVIEW_LCD19FX7 INFO-4):
# char literal holding a quote, escaped quote, CR line ends, unterminated block comment to EOF.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here "lib_code_only_lines.ps1")
$script:fails = 0
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("codeonly_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$n = 0
function Case([string]$name, [string]$src, [string[]]$exp, [string[]]$expBlank) {
    $script:n++
    $f = Join-Path $tmp "c$($script:n).c"
    [IO.File]::WriteAllText($f, $src)
    foreach ($mode in 0, 1) {
        $e = if ($mode) { $expBlank } else { $exp }
        $got = if ($mode) { Get-CodeOnlyLines -Path $f -BlankLiterals } else { Get-CodeOnlyLines -Path $f }
        $g = [string]::Join("|", $got); $x = [string]::Join("|", $e)
        if ($g -ceq $x) { Write-Host "  ok: $name (blank=$mode)" }
        else { Write-Host "  FAIL: $name (blank=$mode): got [$g] expected [$x]" -ForegroundColor Red; $script:fails++ }
    }
}
$dq = [string][char]34; $sq = [string][char]39; $bs = [string][char]92
# T01: char literal holding a double quote must not open a string
Case "char-dq" ("char c = " + $sq + $dq + $sq + "; /* x */ int a;`n") @("char c = " + $sq + $dq + $sq + ";  int a;") @("char c = " + $sq + $sq + ";  int a;")
# T06: escaped quote inside a string does not end it, so // after it is still literal
Case "string-escaped-quote" ("s = " + $dq + "a" + $bs + $dq + "//b" + $dq + "; // tail`n") @("s = " + $dq + "a" + $bs + $dq + "//b" + $dq + "; ") @("s = " + $dq + $dq + "; ")
# comment starts inside literals are not comments
Case "slashstar-in-string" ("s = " + $dq + "/*" + $dq + "; x;`n") @("s = " + $dq + "/*" + $dq + "; x;") @("s = " + $dq + $dq + "; x;")
# T07: CR and CRLF line ends are line breaks (a // comment ends at CR)
Case "cr-lineends" "a; // c1`rb; // c2`r`nc;`n" @("a; ", "b; ", "c;") @("a; ", "b; ", "c;")
# T08: unterminated block comment runs to EOF, keeping newlines
Case "unterminated-block" "a;`n/* open`nstill`nb;`n" @("a;", "", "", "") @("a;", "", "", "")
# terminated block keeps newlines, code after on the same line survives
Case "block-multiline" "a; /* x`ny */ b;`n" @("a; ", " b;") @("a; ", " b;")
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($script:fails -gt 0) { Write-Host "FAIL: $($script:fails) tokenizer assertion(s)"; exit 1 }
Write-Host "PASS: lib_code_only_lines tokenizer edge cases"; exit 0
