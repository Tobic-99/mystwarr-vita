#!/bin/sh
# Translate the game's 68000 code: rom/ -> gen/m68k/. Needs the ROM files in rom/.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R"
mkdir -p gen
python3 tools/m68k_image.py
python3 tools/m68k_discover.py > /dev/null
python3 tools/m68krec.py
