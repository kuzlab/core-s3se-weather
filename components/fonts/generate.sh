#!/bin/bash
# Regenerates the subset Japanese fonts from ui_strings.txt.
#
# LVGL ships no Japanese glyphs, and a full CJK font is far too large, so the
# build carries a subset containing only the characters this UI can display.
# The generated .c files are committed, so a normal build needs neither node
# nor network -- you only need to run this when the wording changes.
#
# Requirements: node/npm (for npx lv_font_conv) and curl for the first run.
set -euo pipefail
cd "$(dirname "$0")"

FONT_FILE="NotoSansJP-Regular.otf"
FONT_URL="https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf"
SIZES="16 24"
BPP=4

if [ ! -f "$FONT_FILE" ]; then
    echo "Downloading Noto Sans JP..."
    curl -sSL -o "$FONT_FILE" "$FONT_URL"
fi

# Collect the unique non-ASCII characters used by the UI. ASCII is added
# separately as a range so digits, punctuation and Latin letters are always
# present regardless of what the strings happen to contain.
SYMBOLS=$(grep -v '^#' ui_strings.txt \
        | tr -d '[:space:]' \
        | python3 -c "
import sys
text = sys.stdin.read()
seen = []
for ch in text:
    if ord(ch) > 0x7E and ch not in seen:
        seen.append(ch)
print(''.join(sorted(seen)))
")

echo "Subset: ${#SYMBOLS} bytes of UTF-8, characters:"
echo "  $SYMBOLS"

for SIZE in $SIZES; do
    OUT="lw_font_jp_${SIZE}.c"
    echo "Generating $OUT (${SIZE}px, ${BPP}bpp)..."
    npx --yes lv_font_conv@latest \
        --font "$FONT_FILE" \
        --symbols "$SYMBOLS" \
        --range 0x20-0x7E \
        --size "$SIZE" \
        --bpp "$BPP" \
        --format lvgl \
        --lv-include "lvgl.h" \
        --lv-font-name "lw_font_jp_${SIZE}" \
        --no-compress \
        -o "$OUT"
done

echo "Done. Generated:"
ls -l lw_font_jp_*.c
