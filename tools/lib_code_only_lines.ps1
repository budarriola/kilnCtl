# Shared C comment stripper for the check_*.ps1 scripts (dot-source it).
# String ("...") and char ('...') literals are honoured (with backslash
# escapes), so "/*" or "http://" inside a literal is NOT a comment start.
# -BlankLiterals empties string/char literal contents (for counting call tokens).
# Returns one entry per source line, comments blanked, literals preserved.
$script:CodeOnlySpecial = [char[]]@('"', "'", '/')
function Get-CodeOnlyLines {
    param([string]$Path, [switch]$BlankLiterals)
    $inBlock = $false
    $result = New-Object System.Collections.Generic.List[string]
    foreach ($line in (Get-Content -Path $Path)) {
        if (-not $inBlock -and $line.IndexOfAny($script:CodeOnlySpecial) -lt 0) { $result.Add($line); continue }
        $sb = New-Object System.Text.StringBuilder
        $n = $line.Length
        $i = 0
        while ($i -lt $n) {
            $c = $line[$i]
            if ($inBlock) {
                if ($c -eq '*' -and ($i + 1) -lt $n -and $line[$i + 1] -eq '/') { $inBlock = $false; $i += 2 } else { $i++ }
                continue
            }
            if ($c -eq '"' -or $c -eq "'") {
                $q = $c
                [void]$sb.Append($c); $i++
                while ($i -lt $n) {
                    $d = $line[$i]
                    $i++
                    if ($d -eq '\') { if (-not $BlankLiterals) { [void]$sb.Append($d); if ($i -lt $n) { [void]$sb.Append($line[$i]) } }; if ($i -lt $n) { $i++ } }
                    elseif ($d -eq $q) { [void]$sb.Append($d); break }
                    elseif (-not $BlankLiterals) { [void]$sb.Append($d) }
                }
                continue
            }
            if ($c -eq '/' -and ($i + 1) -lt $n) {
                $nx = $line[$i + 1]
                if ($nx -eq '/') { break }
                if ($nx -eq '*') { $inBlock = $true; $i += 2; continue }
            }
            [void]$sb.Append($c); $i++
        }
        $result.Add($sb.ToString())
    }
    return ,@($result)
}
