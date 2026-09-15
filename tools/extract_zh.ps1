# extract_zh.ps1
#
# Extracts every string literal containing CJK characters from the source
# tree (comments stripped, adjacent literal concatenation merged), and
# writes "file<TAB>line<TAB>literal" lines to tools/zh_strings.txt.
# The literal is emitted in its raw C source form (escapes preserved) so it
# can be copied verbatim into lib/lang.c as a table key.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path $PSScriptRoot -Parent
$outFile  = Join-Path $PSScriptRoot "zh_strings.txt"

$scanFiles = @(
    Get-ChildItem (Join-Path $repoRoot "app")   -Include *.c, *.h -Recurse -File
    Get-ChildItem (Join-Path $repoRoot "tests") -Include *.c, *.h -Recurse -File
    Get-ChildItem (Join-Path $repoRoot "lib")   -Include *.c, *.h -Recurse -File
)

function Test-Han([string]$s) {
    foreach ($ch in $s.ToCharArray()) {
        $cp = [uint32]$ch
        if ($cp -ge 0x2E80 -and $cp -le 0xFFFF) { return $true }
    }
    return $false
}

$seen = New-Object 'System.Collections.Generic.HashSet[string]'
$lines = New-Object System.Collections.Generic.List[string]

foreach ($file in $scanFiles) {
    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)

    # Strip comments (block, then line).
    $text = [regex]::Replace($text, '(?s)/\*.*?\*/', ' ')
    $text = [regex]::Replace($text, '//[^\r\n]*', ' ')

    # Track line numbers while scanning.
    $lineNo = 1
    $pos = 0
    $len = $text.Length

    while ($pos -lt $len) {
        $ch = $text[$pos]
        if ($ch -eq "`n") { $lineNo++; $pos++; continue }
        if ($ch -ne '"') { $pos++; continue }

        # Start of a literal (or a run of adjacent literals "a" "b").
        $startLine = $lineNo
        $merged = ""
        $ok = $true
        while ($true) {
            if ($pos -ge $len -or $text[$pos] -ne '"') { break }
            # Parse one literal.
            $pos++
            $inner = ""
            while ($true) {
                if ($pos -ge $len) { $ok = $false; break }
                $c = $text[$pos]
                if ($c -eq '\\') {
                    $inner += $c.ToString() + $text[$pos + 1].ToString()
                    $pos += 2
                    continue
                }
                if ($c -eq '"') { $pos++; break }
                if ($c -eq "`n") { $lineNo++ }
                $inner += $c.ToString()
                $pos++
            }
            $merged += $inner
            # Skip whitespace / adjacent literal.
            $j = $pos
            while ($j -lt $len -and ($text[$j] -eq ' ' -or $text[$j] -eq "`t" -or $text[$j] -eq "`r" -or $text[$j] -eq "`n")) {
                if ($text[$j] -eq "`n") { $lineNo++ }
                $j++
            }
            if ($j -lt $len -and $text[$j] -eq '"') { $pos = $j; continue }
            break
        }
        if (-not $ok) { continue }

        if ((Test-Han $merged) -and $seen.Add($merged)) {
            $rel = $file.FullName.Substring($repoRoot.Length + 1)
            $lines.Add("$rel`t$startLine`t$merged")
        }
    }
}

[System.IO.File]::WriteAllText($outFile, ($lines -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Extracted {0} unique Chinese string literals -> {1}" -f $lines.Count, $outFile)
