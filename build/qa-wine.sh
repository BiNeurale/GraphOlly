#!/usr/bin/env bash
# Wine smoke test of the vanilla 1.10 package.
#
#   bash build/qa-wine.sh
#
# Builds the packages, copies the vanilla one into qa/wine-<time>/, starts
# OllyDbg under wine on sample\winshot.exe and drives it with ollyqa.exe:
# graph of main (004012F0), inline a call, zoom 100/45/15, search, export,
# minimap. Screenshots go to qa/wine-<time>/*.png (build/xshot.py), the
# plugin trace to qa/wine-<time>/pkg/grapholly-trace.txt.
#
# Needs: wine, python3-xlib, ImageMagick (magick), an X display (a headless
# gamescope is fine) and tools/ for make-dist.sh. Runs are serialised through
# build/qa-lock. Exit code = number of failed checks.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
STAMP=$(date +%Y%m%d-%H%M%S)
QA=$ROOT/qa/wine-$STAMP
PKG=$QA/pkg
MAIN=4012f0

bash "$ROOT/build/make-dist.sh" >/dev/null
mkdir -p "$QA"
cp -r "$ROOT/dist/GraphOlly-OllyDbg110" "$PKG"
cp "$ROOT/build/out/ollyqa.exe" "$PKG/"
printf '\n[Plugin GraphOlly]\nTrace=1\n' >> "$PKG/ollydbg.ini"

"$ROOT/build/qa-lock" -c "
  set -u
  export WINEPREFIX=\${WINEPREFIX:-\$HOME/.wine-olly} WINEDEBUG=-all
  cd '$PKG'
  FAIL=0
  q()    { wine ollyqa.exe \"\$@\" 2>/dev/null | tr -d '\r'; }
  shot() { python3 '$ROOT/build/xshot.py' OllyDbg \"$QA/\$1.ppm\" 2>&1 | tail -1; }
  check(){ if eval \"\$2\"; then echo \"PASS  \$1\"; else echo \"FAIL  \$1\"; FAIL=\$((FAIL+1)); fi; }

  wine OLLYDBG.EXE sample\\\\winshot.exe >'$QA/wine.log' 2>&1 &
  sleep 20
  check 'plugin loaded' \"q modules | grep -qi GraphOlly.dll\"
  # With First pause=1 Olly should stop at the entry point, but it can still
  # stop at the system breakpoint in ntdll first: F9 gets past it.
  if q alive | grep -qi 'module ntdll'; then
    echo '(system breakpoint: F9)'; q postkey 78 >/dev/null; sleep 4
  fi

  q graph $MAIN | tail -1; sleep 4
  check 'graph window open' \"q graphs | grep -q 'open: 1'\"
  shot 1-fit

  check 'double-click on a CALL line' \"q expand | grep -q 'double-click posted'\"
  sleep 2
  shot 2-expanded

  for z in 100 45 15; do
    q zoom \$z | tail -1
    shot 3-zoom-\$z
  done
  check 'zoom 100 draws text' \"q zoom 100 | grep -q 'line height'\"
  check 'zoom 15 is the overview' \"q zoom 15 | grep -q overview\"

  q graphkey end >/dev/null
  check 'find printf' \"q find printf | grep -qE ': [1-9][0-9]* lines found'\"
  q findnext | tail -1
  shot 4-find

  # Absolute Windows path: Olly's current directory is not necessarily ours.
  rm -f '$QA/export.bmp'
  EXP=\$(winepath -w '$QA/export.bmp' 2>/dev/null)
  check 'export to BMP' \"q export '\$EXP' | grep -q done && [ -s '$QA/export.bmp' ]\"

  check 'minimap visible' \"q minimap | grep -q 'minimap:'\"
  q minimapclick 50 50 | tail -1
  shot 6-minimap

  check 'OllyDbg still alive' \"q alive >/dev/null\"
  echo '--- trace (tail) ---'
  tail -15 grapholly-trace.txt 2>/dev/null || echo '(no trace)'
  wineserver -k 2>/dev/null || true
  echo \"failed checks: \$FAIL\"
  exit \$FAIL
" && RC=0 || RC=$?

for f in "$QA"/*.ppm; do
  [ -f "$f" ] || continue
  magick "$f" "${f%.ppm}.png" && rm -f "$f"
done
echo "--- screenshots in $QA ---"
ls "$QA"/*.png 2>/dev/null || echo "(none)"
exit $RC
