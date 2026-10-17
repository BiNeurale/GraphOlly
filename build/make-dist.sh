#!/usr/bin/env bash
# Builds the two ready-to-run packages: vanilla OllyDbg 1.10 and R4ndom's
# build, both with GraphOlly already installed and a sample executable.
# The ini paths are RELATIVE (tested: Olly 1.10 accepts them), so the folder
# can be moved or copied to Windows without touching anything.
#
# Needs tools/odbg110/ and tools/r4ndom/ (the two OllyDbg distributions; they
# are not in the repository).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST=$ROOT/dist
VER=$(grep -oP 'GO_VERSION\s+"\K[^"]+' "$ROOT/src/grapholly.h")

for d in odbg110 r4ndom; do
  [ -d "$ROOT/tools/$d" ] || { echo "MISSING $ROOT/tools/$d (OllyDbg distribution)"; exit 2; }
done

bash "$ROOT/build/build.sh" >/dev/null

rm -rf "$DIST"
mkdir -p "$DIST"

# --- 1. Vanilla OllyDbg 1.10 -------------------------------------------------
V=$DIST/GraphOlly-OllyDbg110
mkdir -p "$V/sample"
cp "$ROOT/tools/odbg110/"{OLLYDBG.EXE,OLLYDBG.HLP,BOOKMARK.DLL,Cmdline.dll,dbghelp.dll,PSAPI.DLL,license.txt,readme.txt,register.txt} "$V/"
cp "$ROOT/build/out/GraphOlly.dll" "$V/"
cp "$ROOT/build/out/winshot.exe" "$V/sample/"
# The recommended ini travels inside the package too, but its master copy
# lives in ini/: dist/ is wiped at the top of this script.
cp "$ROOT/ini/ollydbg-recommended.ini" "$ROOT/ini/README.txt" \
   "$ROOT/docs/OLLYDBG-INI.md" "$V/"
# The four jump lines are copied verbatim from R4ndom's ini. Without them 1.10
# does not draw the line connecting the SELECTED jump to its destination
# (measured: same Olly, same JE, with and without). They are the checkboxes in
# Options > Debugging options > CPU and have nothing to do with the
# executable: the .text sections of the two builds are byte-identical.
cat > "$V/ollydbg.ini" <<'EOF'
[Settings]
Plugin path=.
UDD path=.
Check DLL versions=0
Show jump direction=1
Show jump path=1
Show path if jump is not taken=1
Show grayed path if jump is not taken=1
Show jumpfrom path=1
Code highlighting=1
Index of default syntax highlighting=2
Analyse main module automatically=1
Analyse code structure=1
Decode tricks=1
Save analysis to file=1
Compress analysis data=1
Show ARG and LOCAL in procedures=1
Demangle symbolic names=1
First pause=1
Number of lines that follow EIP=6
Center FOLLOWed command=1
Show stack frames=1
Show local names in stack=1
Extended stack trace=1
Highlight RETURNs in stack=1
Show arguments in call stack=1
Enable last error=1
Backup UDD files=1
Ignore path in user data file=1
Ignore timestamp in user data file=1
Restore windows=1
Restore window positions=1
Restore width of columns=1
EOF

# --- 2. R4ndom's build -------------------------------------------------------
R=$DIST/GraphOlly-R4ndom
mkdir -p "$R/sample"
cp -r "$ROOT/tools/r4ndom/." "$R/"
rm -f "$R/grapholly-trace.txt" "$R/ollyqa.exe" "$R/winshot.exe" "$R/GraphOlly.dll" "$R"/shot-*.bmp
cp "$ROOT/build/out/GraphOlly.dll" "$R/Plugin/"
cp "$ROOT/build/out/winshot.exe" "$R/sample/"
mkdir -p "$R/UDD"
# The author's paths (C:\Users\Random\Desktop\Olly) do not exist anywhere
# else: with an invalid Plugin path R4ndom's build loads NO plugin, not even
# its own.
python3 - "$R/ollydbg.ini" <<'EOP'
import sys,pathlib,re
p=pathlib.Path(sys.argv[1]); s=p.read_text(errors="replace")
s=re.sub(r'(?m)^Plugin path=.*$',  r'Plugin path=.\\Plugin', s)
s=re.sub(r'(?m)^UDD path=.*$',     r'UDD path=.\\UDD', s)
s=re.sub(r'(?m)^Symbolic data path=.*$', r'Symbolic data path=.', s)
# NOTE: "First pause" was tried at 1 (main module entry point), on the theory
# that R4ndom's 3 stopped too early. It changes nothing: in this build the
# main module record stays empty (name "", codebase 0) either way. The
# author's options stay as he left them.
s=re.sub(r'(?m)^\[GraphOlly\]\nTrace=1\n?','',s)
p.write_text(s)
EOP

# --- 3. Instructions ---------------------------------------------------------
AUT=$(grep -oP 'GO_AUTHOR\s+"\K[^"]+' "$ROOT/src/grapholly.h")
for D in "$V" "$R"; do
  NAME=$(basename "$D")
  PLUGDIR=$([ "$D" = "$R" ] && echo "Plugin\\" || echo "")
  cat > "$D/README-GraphOlly.txt" <<EOF
GraphOlly v$VER - control-flow graph view inside OllyDbg 1.10
=============================================================
$AUT - MIT license - https://github.com/BiNeurale/GraphOlly
Package: $NAME
The plugin is already installed as  ${PLUGDIR}GraphOlly.dll  - nothing else
to do.

TRYING IT
1. Start OLLYDBG.EXE and open a 32-bit executable (File > Open).
   If you have none at hand, use  sample\\winshot.exe  (the tool the plugin
   was tested with; its main is at 004012F0).
2. The module has to be ANALYSED: without analysis Olly does not know where
   functions start and end. The plugin notices and runs the analysis itself
   (the same one as Ctrl+A). To do it yourself: right-click in the
   Disassembler > Analysis > Analyse code, or Ctrl+A.
   Olly may stop at the system breakpoint inside ntdll first: press F9 once
   to get to the entry point.
   R4ndom's build, measured: right after loading, the main module is not
   registered yet (Olly knows neither its codebase nor its codesize), so
   neither the automatic analysis nor Ctrl+A has anything to work on and the
   graph comes out marked [no procedure]. Let the program reach its entry
   point (F9 / Run) and analyse then: from there on the function bounds are
   known. Vanilla 1.10 does not have this problem.
3. Put the selection on a line INSIDE a function.
4. Ask for the graph in one of three ways:
     - Ctrl+D
     - right-click in the Disassembler > GraphOlly > Graph function
     - Plugins menu > GraphOlly > Graph current function
   Why Ctrl+D and not Ctrl+G: Ctrl+G is Olly's "Go to expression"; Olly
   consumes it and never passes it to plugins. Ctrl+Shift+G does not work
   either: Olly ignores Shift and opens its own dialog anyway. Reading the
   menus inside the executable, 1.10 uses Ctrl with A B C E F G J K L N O R S
   T: D H I M P Q U V W X Y Z are free. If you need Ctrl+D for something else,
   set it in ollydbg.ini (see SETTINGS below):  Shortcut=Ctrl+M
   Asking again on the SAME function does not open a new window: the open one
   is reused and the block of the chosen address is highlighted.

IN THE GRAPH WINDOW
     - drag with the left or MIDDLE button to pan; wheel and scroll bars to
       scroll, Shift+wheel scrolls sideways; arrows, PgUp/PgDn on the keyboard
     - ZOOM: Ctrl+wheel (zooms on the point under the mouse), or + and -,
       0 for 100%, End to fit the window, Home to go to the function entry.
       The window opens already fitted, so the first thing you see is the
       shape of the whole function, not a random corner.
       When a text line would be shorter than 5 pixels on screen the graph
       switches to the OVERVIEW: solid blocks with just the address. That is
       how to look at a large function from afar and decide where to go.
     - M toggles the MINIMAP in the corner: click or drag in it to move.
     - Ctrl+F searches the graph (address, mnemonic, operand); F3 / Shift+F3
       (or Ctrl+L / Ctrl+Shift+L) next and previous; Esc clears the marks.
     - the block under the mouse lightens; the one you click stays selected,
       and ITS edges are drawn thicker and on top of the others, so they can
       be followed through the traffic
     - the CURSOR is on a line, not a block: move it by clicking or with the
       up/down arrows (with Ctrl the arrows move the view instead)

   WHILE DEBUGGING
     - ENTER follows what is under the cursor, inside the graph: on a CALL
       line it inlines the called function and goes there, on a jump it moves
       the cursor to the destination block. BACKSPACE goes back.
     - *  (or the space bar) returns to the CURRENT INSTRUCTION, the EIP line,
       as in the Disassembler
     - F2 sets or clears the BREAKPOINT on the cursor line. Breakpoints show
       in the graph the way Olly marks them: red background under the
       address, grey if disabled. They update by themselves, also when you
       set them from the Disassembler.
     - at every pause the block containing EIP gets a red border and its line
       is highlighted; with "Follow EIP" on, the view moves onto it
     - and if execution ENTERS ANOTHER FUNCTION, the graph is rebuilt there by
       itself, in the same window. Turn it off from the context menu, or with
       FollowFunction=0 in the ini.
       Why not draw the whole module at once: ONE function of OLLYDBG.EXE
       (00401159, 599 blocks) already takes 5425x39164 pixels. A whole module
       would not be readable; following the function you are in gives the
       same convenience without the wall.
     - Ctrl+C copies the block to the clipboard; right-click for the rest,
       including EXPORT TO BMP (the whole graph, at 100% if it fits in
       16384 pixels per side and 16 Mpixel, scaled down otherwise)
     - DOUBLE-CLICK ON A "CALL" LINE: the called function is inlined into the
       graph, with a dashed grey edge showing who called it. That is how the
       graph grows past the starting function: it grows where you need it,
       instead of drawing the whole module and reading nothing. Up to 64
       functions in the same graph.
     - double-click on any other line: the CPU window jumps to that line
     - edges: GREEN = jump taken, RED = not taken, BLUE = unconditional jump
       or fall-through, dashed GREY = inlined call. Besides colour there is
       POSITION, learnt once: from a conditional jump the false branch always
       leaves on the LEFT and the true branch on the RIGHT.
       (If you also want T/F letters on the edges: Labels=1 in the ini.)
     - "unresolved exit" under a block means the graph stops there (jmp eax,
       a jump table Olly did not recognise): the CFG is incomplete and says so
     - "[truncated]" in the title: the graph does not cover the whole function
       (module not analysed, or the cap of 2048 blocks / 256 KB was hit)

SETTINGS
     In ollydbg.ini, section [Plugin GraphOlly] (Olly's naming for plugin
     sections):
         Shortcut=Ctrl+D       key that opens the graph from the Disassembler
         FollowEIP=1           the view follows EIP at every pause
         FollowFunction=1      rebuild the graph when execution enters
                               another function
         Minimap=1             minimap in the corner (also the M key)
         Labels=0              T/F letters on conditional edges
         Trace=0               diagnostic trace file

IF SOMETHING IS WRONG
     1) Plugins menu > GraphOlly > Diagnostics (also in the Disassembler
        context menu). It writes to Olly's Log whether the module is analysed
        and how many procedures Olly sees: that answers almost every case of
        the graph not coming out as expected.
     2) Trace to file. Add at the end of ollydbg.ini:
         [Plugin GraphOlly]
         Trace=1
        and restart: next to OLLYDBG.EXE you get grapholly-trace.txt with
        date and time, loading, function bounds, blocks and edges found, and
        the measured times for building, layout and painting.

HOW THE BLOCKS ARE PLACED
     Blocks sit on levels (longest path from the entry, loops not counted)
     and within a level they are ordered by barycentre, to cross as little as
     possible. Edges that skip a few levels get a chain of dummy nodes, one
     per level, so they get their own column instead of a long horizontal
     run. Then everything is aligned on a GRID shared by all levels: that is
     what leaves vertical CORRIDORS between columns and horizontal CHANNELS
     between levels. An edge between adjacent levels goes through the
     channel; a long or backward one drops into the channel, takes a corridor
     and climbs back. Inside channels and corridors every edge has its own
     lane. That is why no edge runs over a block.

LIMITS OF THIS VERSION ($VER)
     - inlining follows only CALLs with a CONSTANT destination. A "call eax"
       or an indirect call has no target to inline, and double-clicking it
       moves the CPU window instead
     - corridors have eight lanes: past the eighth edge in the same corridor
       lanes repeat and two polylines can overlap
     - one function per graph (plus the ones you inline); not a whole module
EOF
done

cd "$DIST"
for D in GraphOlly-OllyDbg110 GraphOlly-R4ndom; do
  python3 -c "
import shutil,sys
shutil.make_archive(sys.argv[1],'zip','.',sys.argv[2])" "$D-v$VER" "$D"
done
# Plugin only: what goes on the releases page next to the vanilla bundle. The
# R4ndom bundle stays local: that build is a modified OllyDbg, and the 1.10
# licence allows redistribution only of the original, unchanged files.
P=GraphOlly-v$VER
rm -rf "$P" && mkdir -p "$P"
cp "$ROOT/build/out/GraphOlly.dll" "$ROOT/LICENSE" "$ROOT/ini/ollydbg-recommended.ini" "$P/"
cp GraphOlly-OllyDbg110/README-GraphOlly.txt "$P/"
python3 -c "
import shutil,sys
shutil.make_archive(sys.argv[1],'zip','.',sys.argv[1])" "$P"
rm -rf "$P"
echo "--- packages ---"
ls -lh "$DIST"/*.zip
