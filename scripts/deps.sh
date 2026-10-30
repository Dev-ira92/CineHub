#!/bin/sh
# Dovlaci header-only zavisnosti u src/ (stb: public domain, font8x8: PD).
set -e
cd "$(dirname "$0")/../src"
fetch() { [ -f "$2" ] && { echo "  $2 vec postoji"; return; }
  echo "  dovlacim $2"; curl -fsSL "$1" -o "$2"; }
fetch https://raw.githubusercontent.com/nothings/stb/master/stb_image.h stb_image.h
fetch https://raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h font8x8_basic.h
echo Gotovo.
