# ASCII-only: verify column positions in display.c (CJK counts as 2 cells)
$src = Get-Content -Encoding UTF8 "app\display.c"
$row = 0
foreach ($line in $src) {
    $row++
    if ($row -lt 146 -or $row -gt 159) { continue }
    if ($line -notmatch 'prints\((\d+), 0, "(.+)"\);') { continue }
    $dispRow = $Matches[1]
    $s = $Matches[2]
    $col = 0; $bar = -1; $na = -1
    foreach ($ch in $s.ToCharArray()) {
        $w = 2
        if ([int]$ch -lt 128) { $w = 1 }
        if ($ch -eq '|') { $bar = $col }
        if ($ch -eq 'A' -and $na -lt 0) { $na = $col - 2 }
        $col += $w
    }
    Write-Output "row$dispRow |@$bar NA@$na total=$col"
}
