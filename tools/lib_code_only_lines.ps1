# Shared C comment stripper for the check_*.ps1 scripts (dot-source it).
# String ("...") and char ('...') literals are honoured (with backslash
# escapes), so "/*" or "http://" inside a literal is NOT a comment start.
# -BlankLiterals empties string/char literal contents (for counting call tokens).
# Returns one entry per source line, comments blanked (a block comment keeps its
# newlines, so line numbers hold), literals preserved.
#
# REVIEW_LCD19FX6 LOW-3: one compiled .NET regex pass over the whole file instead of a
# per-character PowerShell loop, and each file is stripped once per mode and cached by
# full path (a check that asks for the same file again gets the cached lines). Output is
# identical to the former per-character stripper (proved by diffing both over every
# .c/.h file in both modes).
if (-not ('KilnCodeOnlyStripper' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Text.RegularExpressions;
public static class KilnCodeOnlyStripper {
    // string | char | line comment | block comment (an unterminated one runs to EOF)
    static readonly Regex Tok = new Regex(
        "\"(?:\\\\.|[^\"\\\\\\n])*\\\\?(?<cq>\")?|'(?:\\\\.|[^'\\\\\\n])*\\\\?(?<cq>')?|//[^\\n]*|/\\*[\\s\\S]*?(?:\\*/|$)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);
    public static string Strip(string text, bool blank) {
        return Tok.Replace(text, delegate(Match m) {
            char c = m.Value[0];
            if (c == '"' || c == '\'') {
                if (!blank) return m.Value;
                return m.Groups["cq"].Success ? (c.ToString() + c.ToString()) : c.ToString();
            }
            if (c == '/' && m.Value[1] == '/') return "";
            var sb = new StringBuilder();
            foreach (char ch in m.Value) { if (ch == '\n') sb.Append('\n'); }
            return sb.ToString();
        });
    }
}
'@
}
$script:CodeOnlyCache = @{}
function Get-CodeOnlyLines {
    param([string]$Path, [switch]$BlankLiterals)
    $full = (Resolve-Path -LiteralPath $Path).ProviderPath
    $key = $full + '|' + [int]$BlankLiterals.IsPresent
    if ($script:CodeOnlyCache.ContainsKey($key)) { return ,$script:CodeOnlyCache[$key] }
    $raw = Get-Content -LiteralPath $full -Raw
    if ([string]::IsNullOrEmpty($raw)) { $script:CodeOnlyCache[$key] = @(); return ,@() }
    # Same line model as Get-Content: CRLF, LF and lone CR all end a line; a trailing newline adds no empty line.
    $norm = $raw -replace "`r`n?", "`n"
    if ($norm.EndsWith("`n")) { $norm = $norm.Substring(0, $norm.Length - 1) }
    $out = [KilnCodeOnlyStripper]::Strip($norm, $BlankLiterals.IsPresent).Split([char]10)
    $script:CodeOnlyCache[$key] = $out
    return ,$out
}
