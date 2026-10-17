#!/usr/bin/env bash
# Builds GraphOlly.dll - a 32-bit DLL for OllyDbg 1.10, cross-compiled from
# Linux with MinGW.
#
# The PDK import library (Vc50/OLLYDBG.LIB) is MSVC 5 COFF: rather than hoping
# it links, it is regenerated from the official .def with dlltool. The names
# in the .def already carry the 32-bit cdecl underscore, so dlltool runs with
# --no-leading-underscore and the entries are stripped of their ordinals.
set -euo pipefail

CC=${CC:-i686-w64-mingw32-gcc}
DLLTOOL=${DLLTOOL:-i686-w64-mingw32-dlltool}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
OUT=$HERE/out
mkdir -p "$OUT"

command -v "$CC" >/dev/null || { echo "MISSING $CC - install mingw-w64-gcc"; exit 2; }

# --- 1. import library from Ollydbg.def --------------------------------------
# The PDK .def is Borland style (NAME/EXETYPE/CODE/DATA); dlltool wants
# LIBRARY + EXPORTS and nothing else.
awk '
  BEGIN { print "LIBRARY OLLYDBG.EXE"; print "EXPORTS" }
  /^ *_/ { sub(/@[0-9]+/,""); gsub(/^ +| +$/,""); print "  " $1 }
' "$HERE/Ollydbg.def" > "$OUT/ollydbg-mingw.def"

"$DLLTOOL" --no-leading-underscore \
  -d "$OUT/ollydbg-mingw.def" -D OLLYDBG.EXE -l "$OUT/libollydbg.a"

# --- 2. compile --------------------------------------------------------------
# NO -fpack-struct: it would also pack the Windows structures (WNDCLASS,
# SCROLLINFO, PAINTSTRUCT...) and break the API calls. The byte alignment the
# PDK needs comes from src/plugin_packed.h, which wraps plugin.h alone.
# -funsigned-char  : the PDK expects unsigned char
CFLAGS="-m32 -O2 -Wall -funsigned-char -DWIN32 -D_WIN32 \
        -I$ROOT/src -Wno-unused-variable -Wno-unused-but-set-variable"

# Current MinGW links against the UCRT (api-ms-win-crt-*.dll), which an
# unpatched Windows 7 or XP does not have: the DLL failed to load and Olly said
# nothing (seen on a clean Windows 7 VM). We link against msvcrt.dll, which is
# always there: no toolchain startup files, entry straight into DllMain.
CRT="-nostdlib -lmsvcrt-os -lkernel32 -lgcc"
$CC $CFLAGS -s -shared -nostartfiles -Wl,-e,_DllMain@12 -o "$OUT/GraphOlly.dll" \
  "$ROOT/src/grapholly.c" "$ROOT/src/cfg.c" "$ROOT/src/layout.c" "$ROOT/src/graphwnd.c" \
  "$HERE/grapholly.def" \
  -L"$OUT" -lollydbg -lgdi32 -luser32 -lcomdlg32 $CRT \
  -Wl,--enable-stdcall-fixup

# Test executables (targets and the QA driver), same constraint.
# No ASLR: Windows 7 would load them somewhere else on every run, and the
# tests ask for the graph at fixed addresses (winshot's main is 004012F0).
for x in winshot ollyqa; do
  $CC -m32 -O2 -Wall -nostartfiles -Wl,-e,_mainCRTStartup -Wl,--disable-dynamicbase -o "$OUT/$x.exe" \
    "$HERE/$x.c" "$HERE/crt-msvcrt.c" -lgdi32 -luser32 $CRT
done
# jumps.exe has no CRT at all: the entry point is hand-written assembly.
$CC -m32 -nostdlib -Wl,-e,__start -o "$OUT/jumps.exe" "$HERE/jumps.c" -lkernel32

# --- 3. check the exports (by name, never by ordinal) ------------------------
echo "--- GraphOlly.dll exports ---"
i686-w64-mingw32-objdump -p "$OUT/GraphOlly.dll" | sed -n '/Export Address Table/,/^$/p' | head -20
echo "--- imports from OLLYDBG.EXE ---"
i686-w64-mingw32-objdump -p "$OUT/GraphOlly.dll" | grep -A3 "DLL Name: OLLYDBG.EXE" | head -5
echo "--- imported DLLs (no api-ms-win-crt-*) ---"
for f in "$OUT"/GraphOlly.dll "$OUT"/{winshot,jumps,ollyqa}.exe; do
  if i686-w64-mingw32-objdump -p "$f" | grep -qi 'DLL Name: api-ms-win-crt'; then
    echo "ERROR: $f depends on the UCRT"; exit 1
  fi
done
echo "OK: $OUT/GraphOlly.dll"
