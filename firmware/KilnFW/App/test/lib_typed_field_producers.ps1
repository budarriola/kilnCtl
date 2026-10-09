# lib_typed_field_producers.ps1 -- shared helper for the *_producers checks.
#
# Returns the text in which an assignment to a field of ONE struct type is
# credible: (a) the brace body of every `TYPE var = {` / `(TYPE){` / `TYPE var[] = {`
# initializer (nested braces included, so `.rule = { .enable = ... }` counts),
# and (b) `var.field = ` / `var->field = ` statements for each variable or
# pointer declared with that type in the SAME file. A bare `.field =` anywhere
# else (a same-named field of an unrelated struct) is NOT counted -- the old
# whole-tree scan let `e.min_off_s =` / `tp->cross_zone_period_s =` in
# unrelated files mask a deleted real producer.
#
# Usage: . lib_typed_field_producers.ps1
#        $t = Get-TypedProducerText -CodeText $strippedFileText -TypeName 'foo_t'
#        then test $t.Init (initializer bodies) and $t.Vars (list of var names).
function Get-TypedProducerText {
    param([string]$CodeText, [string]$TypeName)
    $tn = [regex]::Escape($TypeName)
    $init = New-Object System.Text.StringBuilder
    # (a) initializer bodies: `TYPE <declarator> = {` or `(TYPE){`
    $starts = [regex]::Matches($CodeText, "(?:\(\s*(?:const\s+)?$tn\s*\)\s*\{|\b$tn\s+\**[A-Za-z_]\w*\s*(?:\[[^\]]*\])?\s*=\s*\{)")
    foreach ($m in $starts) {
        $i = $m.Index + $m.Length   # just past the opening brace
        $depth = 1
        $j = $i
        while ($j -lt $CodeText.Length -and $depth -gt 0) {
            $c = $CodeText[$j]
            if ($c -eq '{') { $depth++ } elseif ($c -eq '}') { $depth-- }
            $j++
        }
        [void]$init.Append($CodeText.Substring($i, $j - $i)).Append("`n")
    }
    # (b) variables/pointers declared with the type (not via typedef/prototype return).
    $vars = @()
    foreach ($m in [regex]::Matches($CodeText, "\b$tn\s+\**\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*[=;,)]")) {
        $vars += $m.Groups[1].Value
    }
    return [pscustomobject]@{ Init = $init.ToString(); Vars = @($vars | Select-Object -Unique) }
}

# A value that is a pure constant is a placeholder, not a producer.
function Test-ConstantValue {
    param([string]$Value)
    return ($Value.Trim() -match '^(?:\(\s*[A-Za-z_]\w*\s*\)\s*)?(?:0[xX]?0*[uUlL]*|0*\.0*[fF]?|0\.0+[fF]?|false|NULL|[A-Za-z_]\w*_NONE|\{\s*0?\s*\})$')
}

# Credits a field only for a NON-constant producing write. Initializer
# entries `.f = <v>` and statements `var.f = <v>;` with constant <v> do not count.
function Test-FieldProducedInText {
    param([string]$InitText, [string[]]$Vars, [string]$Field, [string]$CodeText, [string[]]$ChainMembers = @())
    $f = [regex]::Escape($Field)
    foreach ($m in [regex]::Matches($InitText, "\.\s*$f\s*=(?!=)\s*(\{[^}]*\}|[^,}]*)")) {
        if (-not (Test-ConstantValue $m.Groups[1].Value)) { return $true }
    }
    foreach ($v in $Vars) {
        $ve = [regex]::Escape($v)
        $chain = ""
        foreach ($cm in $ChainMembers) { $chain += "(?:" + [regex]::Escape($cm) + "\s*\.\s*)?" }
        foreach ($m in [regex]::Matches($CodeText, "\b$ve\s*(?:\.|->)\s*$chain$f\s*=(?!=)\s*([^;]*);")) {
            if (-not (Test-ConstantValue $m.Groups[1].Value)) { return $true }
        }
    }
    return $false
}

# Per-file crediting: $Files is a list of objects {Init; Vars; Code}, one per
# source file; a variable's writes count only in the file that declared it.
function Test-FieldProducedInFiles {
    param([object[]]$Files, [string]$Field, [string[]]$ChainMembers = @())
    foreach ($ff in $Files) {
        if (Test-FieldProducedInText -InitText $ff.Init -Vars $ff.Vars -Field $Field -CodeText $ff.Code -ChainMembers $ChainMembers) { return $true }
    }
    return $false
}
