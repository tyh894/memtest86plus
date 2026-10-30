#!/usr/bin/env python3
# gen_cn_font.py
#
# Linux counterpart of tools/gen_cn_font.ps1. Behaviour is identical to the
# PowerShell script: it scans the whole source tree for CJK / full-width
# characters, renders EVERY code point as a 16x16 1-bit glyph (no
# anti-aliasing, ink bounding box centred in the cell) and fully regenerates
# system/font_cn.c.
#
# Usage (from the repository root):
#   python3 tools/gen_cn_font.py /path/to/font.ttf|ttc|otf [size-in-px]
#   python3 tools/gen_cn_font.py --full font.ttf [size]   # full 20902-char font
#
# Requirements: python3 with Pillow. On Windows prefer the original
# gen_cn_font.ps1 (GDI+ SingleBitPerPixelGridFit + YaHei).

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent
FONT_C = REPO / "system" / "font_cn.c"

# --full charset: every ideograph of the CJK Unified Ideographs block plus
# CJK punctuation, the full-width ASCII forms and the special symbols, so
# UI strings can be edited freely without regenerating the font.
FULL_RANGES = [(0x3001, 0x301F), (0x4E00, 0x9FA5), (0xFF01, 0xFF5E)]
FULL_EXTRA = (0x2103, 0x1F44D)

SCAN_DIRS = ["app", "tests", "lib", "system"]

COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

# Fixed 16x16 bitmap for U+1F44D THUMBS UP SIGN. Emoji fonts (Noto Emoji,
# Segoe UI Emoji) are hollow outlines; thresholding them at 16x16 leaves
# unreadable thin lines. This solid silhouette comes from a 320px Noto
# Emoji render thickened with a 26px stroke, downsampled with 50%
# coverage. Emitted verbatim for deterministic output on every machine.
THUMBS_UP_1F44D = [0x0000, 0x01C0, 0x01E0, 0x01E0, 0x03E0, 0x07F8, 0x0FFC,
                   0x7EFC, 0x7CFE, 0x73FE, 0x73FC, 0x78FC, 0x7EFC, 0x3FF8,
                   0x03F8, 0x0000]


def collect_codepoints():
    """Step 1: strip comments, collect CJK code points from the sources."""
    cps = set()
    for d in SCAN_DIRS:
        for f in (REPO / d).rglob("*"):
            # Skip the generated font itself so the scan does not feed on
            # its own glyph comments.
            if f.suffix not in (".c", ".h") or f.name == "font_cn.c":
                continue
            text = COMMENT_RE.sub(" ", f.read_text(encoding="utf-8"))
            for ch in text:  # Python iterates full code points (no surrogates)
                cp = ord(ch)
                if (0x2E80 <= cp <= 0x2FFFF or cp in (0x2103, 0x1F44D)) and cp != 0xFFFD:
                    cps.add(cp)
    if not cps:
        sys.exit("No CJK characters found in the scanned sources.")
    return cps


def render_glyph(font, cp):
    """Step 2: render one code point as 16 rows of uint16 (MSB = leftmost)."""
    img = Image.new("L", (32, 32), 0)
    draw = ImageDraw.Draw(img)
    draw.text((8, 8), chr(cp), font=font, fill=255)
    px = img.load()

    min_x, max_x, min_y, max_y = 99, -1, 99, -1
    for y in range(32):
        for x in range(32):
            if px[x, y] > 127:
                min_x, max_x = min(min_x, x), max(max_x, x)
                min_y, max_y = min(min_y, y), max(max_y, y)

    rows = [0] * 16
    if max_x < 0:
        return rows, False  # empty glyph
    dx = (16 - (max_x - min_x + 1)) // 2 - min_x
    dy = (16 - (max_y - min_y + 1)) // 2 - min_y
    dx = max(-min_x, min(dx, 15 - max_x))
    dy = max(-min_y, min(dy, 15 - max_y))
    for y in range(32):
        ty = y + dy
        if not 0 <= ty <= 15:
            continue
        for x in range(32):
            tx = x + dx
            if 0 <= tx <= 15 and px[x, y] > 127:
                rows[ty] |= 0x8000 >> tx
    return rows, True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("font", help="font file (.ttf/.ttc/.otf)")
    ap.add_argument("size", nargs="?", type=int, default=16,
                    help="glyph size in px (default 16)")
    ap.add_argument("--full", action="store_true",
                    help="emit the full CJK charset (U+4E00..U+9FA5 plus "
                         "punctuation and full-width forms) instead of only "
                         "the characters found in the sources")
    ap.add_argument("--face", type=int, default=0,
                    help="face index inside a .ttc collection (default 0)")
    ap.add_argument("--out", default=str(FONT_C),
                    help="output file (default system/font_cn.c)")
    args = ap.parse_args()

    font = ImageFont.truetype(args.font, args.size, index=args.face)

    cps = collect_codepoints()
    if args.full:
        cps.update(FULL_EXTRA)
        for lo, hi in FULL_RANGES:
            cps.update(range(lo, hi + 1))
    codepoints = sorted(cps)

    rows_by_cp = {}
    empty = []
    for cp in codepoints:
        if cp == 0x1F44D:
            # Fixed bitmap: rendering emoji from a font is not viable (see
            # THUMBS_UP_1F44D) and the result must not depend on the
            # installed fonts.
            rows_by_cp[cp] = list(THUMBS_UP_1F44D)
            continue
        rows, ok = render_glyph(font, cp)
        if not ok:
            empty.append(chr(cp))
        rows_by_cp[cp] = rows

    # Step 3: emit the font C file.
    out = Path(args.out)
    n = len(codepoints)
    cp_list = ", ".join(f"0x{cp:04X}" for cp in codepoints)

    if args.full:
        coverage = (f"Full CJK font: every ideograph U+4E00..U+9FA5 plus CJK\n"
                    f"// punctuation, the full-width forms and the special symbols\n"
                    f"// ({n} glyphs in total, incl. any extra code points found in\n"
                    f"// the sources). UI strings can be edited without regenerating.")
    else:
        coverage = (f"Covers the {n} CJK / full-width characters used by the UI "
                    f"string literals.")

    lines = []
    lines.append("// SPDX-License-Identifier: GPL-2.0")
    lines.append("//")
    lines.append("// 16x16 CJK bitmap font. GENERATED by tools/gen_cn_font.py -- DO NOT EDIT.")
    lines.append(f"// Source font: {Path(args.font).name} {args.size}px (1-bit threshold, grid-fitted).")
    lines.append(f"// {coverage}")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append('#include "font_cn.h"')
    lines.append("")
    lines.append(f"const int cn_font_count = {n};")
    lines.append("")
    lines.append(f"const uint32_t cn_font_codepoints[{n}] = {{ {cp_list} }};")
    lines.append("")
    lines.append(f"const uint16_t cn_font_data[{n}][16] = {{")
    for cp in codepoints:
        rows = ", ".join(f"0x{v:04X}" for v in rows_by_cp[cp])
        lines.append(f"    /* {chr(cp)} */ {{ {rows} }},")
    lines.append("};")
    lines.append("")
    lines.append("int cn_font_lookup(uint32_t codepoint)")
    lines.append("{")
    lines.append("    int lo = 0;")
    lines.append("    int hi = cn_font_count - 1;")
    lines.append("    while (lo <= hi) {")
    lines.append("        int mid = lo + (hi - lo) / 2;")
    lines.append("        if (cn_font_codepoints[mid] == codepoint) return mid;")
    lines.append("        if (cn_font_codepoints[mid] <  codepoint) {")
    lines.append("            lo = mid + 1;")
    lines.append("        } else {")
    lines.append("            hi = mid - 1;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return -1;")
    lines.append("}")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"Wrote {out} ({n} glyphs)")
    if empty:
        print(f"WARNING: glyphs rendered empty (font lacks them?): {''.join(empty)}")


if __name__ == "__main__":
    main()
