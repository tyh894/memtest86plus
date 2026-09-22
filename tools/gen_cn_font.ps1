# gen_cn_font.ps1
#
# Generates system/font_cn.c: a 16x16 bitmap font (one bit per pixel) for the
# subset of CJK / full-width characters used in the UTF-8 string literals of
# the source tree.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File tools\gen_cn_font.ps1
#
# Requirements: Windows with GDI+ (System.Drawing) and the SimSun font
# (simsun.ttc, ships with every zh-CN Windows). SimSun 12pt at 96 dpi is
# exactly 16px and has embedded 16x16 CJK bitmaps, so with
# SingleBitPerPixelGridFit the rendered glyphs are the classic crisp
# DOS-style dot-matrix forms.  The thumbs-up emoji (U+1F44D) is emitted
# from a fixed hand-tuned bitmap (see below) because emoji fonts are
# hollow outlines that threshold into unreadable thin lines at 16x16.

param(
    [string]   $OutFile  = (Join-Path $PSScriptRoot "..\system\font_cn.c"),
    [string]   $FontName = "Microsoft YaHei",
    [double]   $FontSize = 12.0
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

# ----------------------------------------------------------------------------
# 1. Collect the set of code points used in the sources.
# ----------------------------------------------------------------------------

$repoRoot = Split-Path $PSScriptRoot -Parent
$scanFiles = @(
    Get-ChildItem (Join-Path $repoRoot "app")    -Include *.c, *.h -Recurse -File
    Get-ChildItem (Join-Path $repoRoot "tests")  -Include *.c, *.h -Recurse -File
    Get-ChildItem (Join-Path $repoRoot "lib")    -Include *.c, *.h -Recurse -File
    # system/ holds the SPD strings that use the thumbs-up emoji; skip the
    # generated font itself so the scan does not feed on its own comments.
    Get-ChildItem (Join-Path $repoRoot "system") -Include *.c, *.h -Recurse -File |
        Where-Object { $_.Name -ne "font_cn.c" }
)

$codepoints = New-Object 'System.Collections.Generic.SortedSet[uint32]'

foreach ($file in $scanFiles) {
    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)

    # Strip comments so Chinese text in comments does not bloat the font.
    $text = [regex]::Replace($text, '(?s)/\*.*?\*/', ' ')
    $text = [regex]::Replace($text, '//[^\r\n]*', ' ')

    $chars = $text.ToCharArray()
    for ($i = 0; $i -lt $chars.Length; $i++) {
        $cp = [uint32]$chars[$i]
        if ($cp -ge 0xD800 -and $cp -le 0xDBFF -and $i + 1 -lt $chars.Length) {
            # Surrogate pair -> full code point.
            $lo = [uint32]$chars[$i + 1]
            if ($lo -ge 0xDC00 -and $lo -le 0xDFFF) {
                $cp = 0x10000 + (($cp - 0xD800) -shl 10) + ($lo - 0xDC00)
                $i++
            }
        }
        # 0x2103 = '℃' (full-width, lives below 0x2E80 but needs the CJK font)
        # 0x1F44D = '👍' (SMP emoji, outside the normal CJK collection range)
        if (($cp -ge 0x2E80 -and $cp -le 0x2FFFF -and $cp -ne 0xFFFD) -or $cp -eq 0x2103 -or $cp -eq 0x1F44D) {
            [void]$codepoints.Add($cp)
        }
    }
}

if ($codepoints.Count -eq 0) {
    Write-Error "No CJK characters found in the scanned sources."
}

# ----------------------------------------------------------------------------
# 2. Render each code point as a 16x16 bitmap.
# ----------------------------------------------------------------------------

$family = $null
foreach ($name in @($FontName, "SimHei", "SimSun", "NSimSun", "Microsoft YaHei")) {
    try {
        $family = New-Object System.Drawing.FontFamily($name)
        break
    } catch { continue }
}
if ($null -eq $family) {
    Write-Error "Font '$FontName' (SimSun) not installed on this system."
}

$font = New-Object System.Drawing.Font($family, $FontSize, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Point)
$fmt = [System.Drawing.StringFormat]::GenericTypographic

# Emoji-capable font for emoji glyphs other than the fixed ones (e.g. future
# U+1Fxxx code points).  GDI+ cannot draw colour glyphs, so Segoe UI Emoji
# contributes its monochrome outline; the render loop below thickens it by
# drawing at many offsets (a poor man's stroke) so the hollow outline
# swells into a solid silhouette before thresholding.
$emojiFamily = $null
foreach ($name in @("Segoe UI Emoji", "Segoe UI Symbol")) {
    try {
        $emojiFamily = New-Object System.Drawing.FontFamily($name)
        break
    } catch { continue }
}

# Render on a 32x32 canvas with the em box at (8,8), then centre the ink
# bounding box in the 16x16 cell.
$canvas  = New-Object System.Drawing.Bitmap(32, 32)
$gfx     = [System.Drawing.Graphics]::FromImage($canvas)
$gfx.PageUnit = [System.Drawing.GraphicsUnit]::Pixel
$gfx.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
$whiteBrush = [System.Drawing.Brushes]::White

$emptyGlyphs = @()
$rowsByCp = @{}

foreach ($cp in $codepoints) {
    $gfx.Clear([System.Drawing.Color]::Black)
    $str = [string][char]$cp
    if ($cp -gt 0xFFFF) {
        # Beyond-BMP code point: build from surrogate pair.
        $v = $cp - 0x10000
        $str = [string]([char](0xD800 + ($v -shr 10))) + [string]([char](0xDC00 + ($v -band 0x3FF)))
    }
    if ($cp -eq 0x1F44D) {
        # Fixed hand-tuned thumbs-up: emoji fonts are hollow outlines whose
        # 16x16 threshold is unreadable, so emit the verified solid bitmap
        # (derived from a stroked 320px Noto Emoji render) verbatim.
        $rowsByCp[[uint32]$cp] = @(0x0000, 0x01C0, 0x01E0, 0x01E0, 0x03E0,
                                   0x07F8, 0x0FFC, 0x7EFC, 0x7CFE, 0x73FE,
                                   0x73FC, 0x78FC, 0x7EFC, 0x3FF8, 0x03F8,
                                   0x0000)
        continue
    }
    if ($cp -ge 0x1F000 -and $null -ne $emojiFamily) {
        # Emoji: render anti-aliased at 4x resolution with Segoe UI Emoji.
        # The glyph is hollow, so redraw it at every offset inside a disc
        # (a poor man's stroke) to swell the outline into a solid
        # silhouette, then threshold each 4x4 block at 50% coverage.
        $ss = 4
        $ecanvas = New-Object System.Drawing.Bitmap(32 * $ss, 32 * $ss)
        $egfx = [System.Drawing.Graphics]::FromImage($ecanvas)
        $egfx.PageUnit = [System.Drawing.GraphicsUnit]::Pixel
        $egfx.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
        $efont = New-Object System.Drawing.Font($emojiFamily, [single](($FontSize + 2.0) * $ss), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Point)
        $rad = [int](2.5 * $ss)   # stroke radius in supersampled pixels
        for ($oy = -$rad; $oy -le $rad; $oy++) {
            for ($ox = -$rad; $ox -le $rad; $ox++) {
                if ($ox * $ox + $oy * $oy -gt $rad * $rad) { continue }
                $egfx.DrawString($str, $efont, $whiteBrush,
                                 [single](8.0 * $ss + $ox), [single](8.0 * $ss + $oy), $fmt)
            }
        }
        $efont.Dispose()
        $egfx.Dispose()
        for ($y = 0; $y -lt 32; $y++) {
            for ($x = 0; $x -lt 32; $x++) {
                $cov = 0
                for ($iy = 0; $iy -lt $ss; $iy++) {
                    for ($ix = 0; $ix -lt $ss; $ix++) {
                        if ($ecanvas.GetPixel($x * $ss + $ix, $y * $ss + $iy).R -gt 0) { $cov++ }
                    }
                }
                if ($cov -ge [int]($ss * $ss / 2)) {
                    $gfx.FillRectangle($whiteBrush, $x, $y, 1, 1)
                }
            }
        }
        $ecanvas.Dispose()
    } else {
        $gfx.DrawString($str, $font, $whiteBrush, [single]8.0, [single]8.0, $fmt)
    }

    # Find the ink bounding box.
    $minX = 99; $maxX = -1; $minY = 99; $maxY = -1
    for ($y = 0; $y -lt 32; $y++) {
        for ($x = 0; $x -lt 32; $x++) {
            if ($canvas.GetPixel($x, $y).R -gt 127) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }

    $row = @(0) * 16
    if ($maxX -ge 0) {
        # Centre the bounding box in the 16x16 cell, clamping to the cell.
        $dx = [Math]::Floor((16 - ($maxX - $minX + 1)) / 2) - $minX
        $dy = [Math]::Floor((16 - ($maxY - $minY + 1)) / 2) - $minY
        $dx = [Math]::Max(-$minX, [Math]::Min($dx, 15 - $maxX))
        $dy = [Math]::Max(-$minY, [Math]::Min($dy, 15 - $maxY))
        for ($y = 0; $y -lt 32; $y++) {
            $ty = $y + $dy
            if ($ty -lt 0 -or $ty -gt 15) { continue }
            for ($x = 0; $x -lt 32; $x++) {
                $tx = $x + $dx
                if ($tx -lt 0 -or $tx -gt 15) { continue }
                if ($canvas.GetPixel($x, $y).R -gt 127) {
                    $row[$ty] = $row[$ty] -bor (0x8000 -shr $tx)
                }
            }
        }
    } else {
        $emptyGlyphs += $str
    }
    $rowsByCp[[uint32]$cp] = $row
}

$gfx.Dispose()
$canvas.Dispose()

if ($emptyGlyphs.Count -gt 0) {
    Write-Warning ("Glyphs rendered empty (font lacks them?): " + ($emptyGlyphs -join " "))
}

# ----------------------------------------------------------------------------
# 3. Emit system/font_cn.c.
# ----------------------------------------------------------------------------

$n = $codepoints.Count
$cpList  = ($codepoints | ForEach-Object { "0x{0:X4}" -f $_ }) -join ", "

$glyphBlocks = New-Object System.Text.StringBuilder
$i = 0
foreach ($cp in $codepoints) {
    $rowText = ($rowsByCp[[uint32]$cp] | ForEach-Object { "0x{0:X4}" -f $_ }) -join ", "
    $ch = if ($cp -le 0xFFFF) { [string][char]$cp } else { "" }
    [void]$glyphBlocks.AppendLine("    /* $ch */ { $rowText },")
    $i++
}

$cSource = @"
// SPDX-License-Identifier: GPL-2.0
//
// 16x16 CJK bitmap font. GENERATED by tools/gen_cn_font.ps1 -- DO NOT EDIT.
// Source font: $FontName $($FontSize)pt (16px @ 96dpi), grid-fitted.
// Covers the $n CJK / full-width characters used by the UI string literals.

#include <stdint.h>

#include "font_cn.h"

const int cn_font_count = $n;

const uint32_t cn_font_codepoints[$n] = { $cpList };

const uint16_t cn_font_data[$n][16] = {
$($glyphBlocks.ToString().TrimEnd("`r","`n"))
};

int cn_font_lookup(uint32_t codepoint)
{
    int lo = 0;
    int hi = cn_font_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cn_font_codepoints[mid] == codepoint) return mid;
        if (cn_font_codepoints[mid] <  codepoint) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}
"@

[System.IO.File]::WriteAllText($OutFile, $cSource, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Wrote {0} ({1} glyphs) to {2}" -f $n, $n, $OutFile)
