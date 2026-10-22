#!/usr/bin/env python3
"""Regenerate the word-box CJK fonts (assets/fonts/wb_font_cjk_{14,20}.c).

Glyphs: printable ASCII + all 6763 GB2312 hanzi (levels 1 and 2) + common CJK punctuation.
Source font: Noto Sans SC Medium (SIL OFL 1.1), from the npm package
@expo-google-fonts/noto-sans-sc@0.4.4. Converter: lv_font_conv@1.5.3.

Usage:
    npm i lv_font_conv@1.5.3 @expo-google-fonts/noto-sans-sc@0.4.4
    python3 tools/gen_wb_fonts.py <path/to/NotoSansSC_500Medium.ttf> [lv_font_conv binary]
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTRA = "，。、；：？！“”‘’（）《》…—·～【】"


def gb2312_hanzi() -> str:
    chars = []
    for hi in range(0xB0, 0xF8):
        for lo in range(0xA1, 0xFF):
            if hi == 0xD7 and lo > 0xF9:
                break
            chars.append(bytes([hi, lo]).decode("gb2312"))
    return "".join(chars)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    ttf = sys.argv[1]
    conv = sys.argv[2] if len(sys.argv) > 2 else "lv_font_conv"
    symbols = gb2312_hanzi() + EXTRA
    for size in (14, 20):
        name = f"wb_font_cjk_{size}"
        out = ROOT / "assets" / "fonts" / f"{name}.c"
        subprocess.run(
            [conv, "--font", ttf, "--range", "0x20-0x7E", "--symbols", symbols,
             "--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
             "--lv-font-name", name, "--lv-include", "lvgl.h", "-o", str(out)],
            check=True,
        )
        # The converter echoes every symbol into the header comment; keep it short.
        text = out.read_text(encoding="utf-8")
        text = re.sub(r" \* Opts: .*\n",
                      f" * Opts: see tools/gen_wb_fonts.py (size {size}, bpp 4, "
                      "ASCII + GB2312 hanzi)\n", text, count=1)
        out.write_text(text, encoding="utf-8")
        print(f"wrote {out.relative_to(ROOT)} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
