# checkcache: ok
# check_source_bytes.ps1 -- refuses damaged bytes in tracked text sources
# (REVIEW_LCDFX2_DEVBREAK_2026-10-10 LOW-1).
#
# Why: 5c89d3a3b wrote a CRLF file (an 1846-line diff) and turned the '\0' escape into a raw NUL;
# four other tracked files carried "escape written as a raw byte" damage (raw CR in a char literal,
# FF/BS in comments). A lone CR or a NUL makes git treat a file as binary (-text), so the damage is
# committed byte for byte, and GCC rejects a lone CR in a char literal.
#
# What it reads: the bytes git would commit. A private copy of the index is refreshed with
# `git add -u` (the real index is never touched), so a dirty working-tree edit is checked as well as
# committed content. Normal git EOL normalization still applies (an all-CRLF working copy of an LF
# file is committed as LF and is fine); what survives into the blob is judged.
# Refused, per tracked file of a text type:
#   - any NUL byte;
#   - any control byte other than TAB and LF (a CR is allowed only as part of CRLF);
#   - a lone CR (CR not followed by LF);
#   - CRLF line endings in the index, except where CRLF is legitimate (none of the scanned types today:
#     .ps1 is CRLF in the working tree only, the index stays LF; an LF-in-index .ps1 is fine and a
#     CRLF-in-index .ps1 is also tolerated);
#   - git classing the file as binary (`i/-text`) although its extension says text.
# Real binaries are excluded by extension (only the text extensions below are scanned); .kicad_* and
# submodules are not scanned.
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_source_bytes.ps1 [-Repo <path>]
param([string]$Repo)
$ErrorActionPreference = "Stop"
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Repo = (Resolve-Path $Repo).Path

$textExt = @('.c','.h','.ps1','.py','.js','.mjs','.html','.css','.cmake','.json','.csv','.txt','.ino')
$crlfOkExt = @('.ps1')

# Sweep leftovers of earlier runs killed before their finally block (REVIEW_LCDFX3 INFO-2).
Get-ChildItem -LiteralPath ([IO.Path]::GetTempPath()) -Directory -Filter 'srcbytes_*' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -lt (Get-Date).AddHours(-1) } |
    ForEach-Object { Remove-Item -Recurse -Force -LiteralPath $_.FullName -ErrorAction SilentlyContinue }

# Run git, capture stdout and stderr, and FAIL on a nonzero exit (REVIEW_LCDFX3 LOW-1/LOW-2): a git
# failure must never degrade to a PASS over an unrefreshed index or an empty list.
function Invoke-GitChecked([string[]]$GitArgs) {
    $errf = Join-Path $tmp "git_err.txt"
    $prev = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    $out = & git -C $Repo @GitArgs 2>$errf
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) {
        $msg = if (Test-Path $errf) { (Get-Content -Raw $errf) } else { "" }
        Write-Host "FAIL: git $($GitArgs -join ' ') exited $code : $msg"
        Write-Host "source byte check FAILED: cannot read the bytes git would commit (not a pass)"
        exit 1
    }
    return $out
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("srcbytes_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    $gitDir = (Invoke-GitChecked @('rev-parse','--absolute-git-dir')) | Select-Object -First 1
    $idx = Join-Path $tmp "index"
    Copy-Item -LiteralPath (Join-Path $gitDir "index") -Destination $idx
    $env:GIT_INDEX_FILE = $idx
    [void](Invoke-GitChecked @('add','-u'))
    $stage = Invoke-GitChecked @('-c','core.quotepath=false','ls-files','-s')
    $eol = Invoke-GitChecked @('-c','core.quotepath=false','ls-files','--eol')
    Remove-Item Env:\GIT_INDEX_FILE
    $files = New-Object System.Collections.Generic.List[object]
    foreach ($e in $stage) {
        if (-not $e) { continue }
        $tab = $e.IndexOf("`t"); if ($tab -lt 0) { continue }
        $meta = $e.Substring(0, $tab) -split ' '
        $path = $e.Substring($tab + 1)
        if ($meta[0] -eq '160000') { continue }
        $name = [IO.Path]::GetFileName($path)
        $ext = [IO.Path]::GetExtension($path).ToLowerInvariant()
        if ($textExt -notcontains $ext -and $name -ne 'CMakeLists.txt') { continue }
        $files.Add([pscustomobject]@{ Sha = $meta[1]; Path = $path; Ext = $ext })
    }
    # Floor / anchor (LOW-2): an empty or truncated list is a failure, never a vacuous pass.
    $anchor = $files | Where-Object { $_.Path -eq 'tools/check_source_bytes.ps1' }
    if ($files.Count -lt 1000 -or -not $anchor) {
        Write-Host "FAIL: only $($files.Count) tracked text files listed (floor 1000) or anchor tools/check_source_bytes.ps1 missing; wrong -Repo or a git listing failure"
        exit 1
    }
    $bad = New-Object System.Collections.Generic.List[string]
    # git's own text/binary verdict (index side) for text-typed files.
    foreach ($e in $eol) {
        if ($e -match '^i/-text\s.*?\t(.+)$') {
            $p = $Matches[1]
            $x = [IO.Path]::GetExtension($p).ToLowerInvariant()
            if ($textExt -contains $x) { $bad.Add("${p}: git treats this text-typed file as binary (i/-text; NUL or lone CR)") }
        }
    }
    $list = Join-Path $tmp "in.txt"; $outf = Join-Path $tmp "out.bin"
    [IO.File]::WriteAllText($list, (($files | ForEach-Object { $_.Sha }) -join "`n") + "`n")
    $cmd = 'git -C "' + $Repo + '" cat-file --batch < "' + $list + '" > "' + $outf + '"'
    & cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "git cat-file --batch failed" }

    if (-not ('KilnSourceBytes' -as [type])) {
        Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.IO; using System.Text;
public static class KilnSourceBytes {
    // Returns one result string per input blob (null = clean): "nul@line", "ctl 0xNN@line", "lonecr@line" or "crlf@line".
    public static string[] Scan(string batchFile, int count, bool[] crlfOk) {
        byte[] d = File.ReadAllBytes(batchFile);
        string[] res = new string[count];
        int pos = 0;
        for (int k = 0; k < count; k++) {
            int nl = Array.IndexOf(d, (byte)10, pos);
            string hdr = Encoding.ASCII.GetString(d, pos, nl - pos);
            string[] f = hdr.Split(' ');
            if (f.Length != 3 || f[1] != "blob") { res[k] = "catfile:" + hdr; return res; }
            int size = int.Parse(f[2]); int start = nl + 1;
            int line = 1; string first = null; bool crlf = false;
            for (int i = start; i < start + size; i++) {
                byte b = d[i];
                if (b == 10) { line++; continue; }
                if (b == 9) continue;
                if (b == 13) {
                    if (i + 1 < start + size && d[i + 1] == 10) { if (!crlf) { crlf = true; if (!crlfOk[k] && first == null) first = "crlf@line " + line; } continue; }
                    if (first == null) first = "lone CR@line " + line;
                    continue;
                }
                if (b == 0) { if (first == null) first = "NUL@line " + line; continue; }
                if (b < 32) { if (first == null) first = "control byte 0x" + b.ToString("X2") + "@line " + line; }
            }
            res[k] = first;
            pos = start + size + 1;
        }
        return res;
    }
}
'@
    }
    $ok = [bool[]]($files | ForEach-Object { $crlfOkExt -contains $_.Ext })
    $res = [KilnSourceBytes]::Scan($outf, $files.Count, $ok)
    for ($i = 0; $i -lt $files.Count; $i++) {
        if ($res[$i]) { $bad.Add("$($files[$i].Path): $($res[$i])") }
    }
    if ($bad.Count) {
        foreach ($b in $bad) { Write-Host "FAIL: $b" }
        Write-Host "source byte check FAILED ($($bad.Count)): write escapes ('\0', '\r', ...) as escapes, never as raw bytes; keep LF line endings"
        exit 1
    }
    Write-Host "source byte check passed: $($files.Count) tracked text files clean."
    exit 0
} finally {
    if (Test-Path Env:\GIT_INDEX_FILE) { Remove-Item Env:\GIT_INDEX_FILE }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
