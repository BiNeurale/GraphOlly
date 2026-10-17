# Implementation notes: OllyDbg 1.10 pitfalls (2026-09-22)

Things that cost me time and that you can't work out by reading the PDK. Each
one comes with the evidence that settled it.

## 1. OllyDbg 1.10 looks up exports WITH the underscore

`GetProcAddress` inside `OLLYDBG.EXE` asks for `_ODBG_Plugindata` (the Borland
convention for 32-bit cdecl). MinGW exports the bare name `ODBG_Plugindata`.

**Symptom**: no error, no message. The wine loader log shows the DLL being
loaded and **immediately unloaded**; `DllMain` gets called but
`ODBG_Plugindata` doesn't.

**Evidence**: `strings OLLYDBG.EXE | grep ODBG_` → all with the underscore; the
exports of the PDK's `BOOKMARK.DLL` are the same.

**Fix**: `build/grapholly.def` exports both forms
(`_ODBG_Plugindata = ODBG_Plugindata` plus the bare name).

## 2. `-fpack-struct=1` also packs the Windows structures

`plugin.h` requires byte alignment. Getting it through the global flag also
packs `WNDCLASS`, `SCROLLINFO`, `PAINTSTRUCT`, `BITMAPINFOHEADER`… which the
system does **not** pack: `cbSize` and the fields after it end up at the wrong
offsets.

**Fix**: `src/plugin_packed.h` wraps only `plugin.h` in
`#pragma pack(push,1)` / `pop`; the global flag is not used.

## 3. `Getproclimits()` almost never answers; `Findprocbegin/end` do

On an analysed module (`codedec` non-null, `processed=1`, 384 jumps in
`jddata`), `Getproclimits()` returned **0 for all 10** recognised procedures,
while `Findprocbegin`/`Findprocend` gave the right limits
(e.g. `00401530-0040155E`). `Getproclimits` looks at the procedures stored in
the `.udd`, not at the analysis.

## 4. Outside a recognised procedure, a linear sweep blows up

Asking for the graph at the entry point (which Olly does not consider part of
a procedure), the fallback "scan the memory block" produced **2048 blocks**
(the cap) and a layout of 288,297 × 1,289,212 pixels. Fix: recursive descent,
visiting only reachable code. On the same real procedure:
**12 blocks, 15 edges, 857 × 852 px**.

## 5. QA under headless wine: no synthetic input, and GDI captures lie

Measured inside `gamescope --backend headless`:

| method | result |
|---|---|
| `BitBlt` of the screen from inside wine | black (mean 0) |
| `PrintWindow` on the window | **returns 1** and draws black |
| `WM_PRINT` on the window | black |
| wine virtual desktop + BitBlt | black |
| **XComposite from the Linux side** (`build/xshot.py`) | **125 colours, mean 0.56** |

| input | result |
|---|---|
| `xdotool key` (XTEST), even with `windowfocus --sync` | doesn't arrive |
| `keybd_event` from inside wine | doesn't arrive |
| `mouse_event` + `SetCursorPos` | doesn't arrive |
| **`PostMessage(WM_LBUTTONDOWN/UP)`** to the right window | **arrives** (the Log opens) |
| `PostMessage(WM_LBUTTONDBLCLK)` | **arrives** (expansion starts) |
| `PostMessage(WM_MOUSEMOVE)` | **arrives** (the block under the mouse lightens) |
| `PostMessage(WM_KEYDOWN/UP)` to a window **owned by the plugin** | **arrives** (keyboard zoom changes) |
| **`PostMessage(WM_MOUSEWHEEL)`** | **does NOT arrive**: the plugin trace, which writes a line for every wheel event it receives, stays empty |

⚠️ I only caught the wheel row because the plugin **reports** what it
receives. Going by the screenshots alone, zoom seemed to work: there were two
different images, but the difference was the view settling, not the wheel. A
probe that measures the effect rather than the arrival can't tell the two
cases apart.

Also, aiming a click with `ChildWindowFromPoint` isn't enough: it stops at the
first level (the `MDIClient`) and the message selects nothing. The CPU panes
are its grandchildren and are found by **class** (`ACPUASM` is the
Disassembler), not by position: on screen the coordinates change depending on
whether the window is maximised, and `WindowFromPoint` picks up the wine
console sitting in front.

Practical consequence: in automated runs the graph is requested through the
plugin's **service door** (hidden window + registered message, see
`src/grapholly.c`). **That door does not prove that the menu item and the
shortcut are wired correctly**: those two get checked by hand on Windows.

## 6. Text inside the struct: 25 MB zeroed for twelve blocks

`t_goblock` held `char text[128][96]` (12,328 bytes per block) and `t_gograph`
has 2048 of them statically: **24,704 KB** per graph, `memset` on every request
even for a function with 12 blocks and 35 lines. Measured by compiling the
same probe against the old and the new header:

| | `t_goblock` | `t_gograph` |
|---|---|---|
| v0.1.0 | 12,328 bytes | 24,704 KB |
| v0.1.1 (lines in a pool) | 48 bytes | 144 KB |

The pool grows in steps (512 lines) and lives in `t_gograph.line`; the block
carries the index of its first line, not the text. **Indices, not pointers**:
`realloc` moves the pool.

## 7. In R4ndom's build the main module is not registered

Asking for the graph right after loading, `Findmodule(0x401750)` returns an
**empty** record in R4ndom: `name=""`, `codebase=0`, `codesize=0`,
`codedec=NULL`, `processed=-1`. In vanilla 1.10, at the same point in the
process's life, the same module already has 10 recognised procedures.

Measured consequences:
- `Analysecode(pm)` can't do anything with that record (which is why it is
  only called when `codesize>0`);
- the graph comes out labelled `[no procedure]`, which is accurate: it starts
  from the chosen address, not from the function's limits.

**It's not the first pause**: I tried `First pause=1` (module entry point)
instead of R4ndom's `3`, and got the same empty record. So the author's options
were left as they were. Remaining suspect (not verified): one of his 19
plugins, StrongOD first of all.

Added 2026-09-23: with `First pause=1`, R4ndom's Disassembler **still** shows
`ntdll` (`7BCF0576`), not the entry point. The option doesn't decide that
pause: something stops earlier. In vanilla 1.10, same target, Olly stops at
the module entry point.

## 8. R4ndom's executable isn't patched: it's re-skinned

`.text` is byte-identical to 1.10 (md5 `f14aa0b2eeca633b1f52d3f6166ac920`,
**0** bytes different over 0xAE800). The differences are 164 bytes in `.data`
(the randomised class names, which is why the graph window is called
`wB5JNlUyK_0001` instead of `OT_PLUGIN_0002`) and 88,186 in `.rsrc`: 27
toolbar bitmaps and the "About" title. **The jump arrows are OllyDbg 1.10's
own** and are on by default; see `docs/R4NDOM.md`.

## 9. Two places need to know the text margin

The layout computes the block height starting from the margin above the first
line; drawing decides from that where line *j* falls; and a click has to work
out **which line** was hit by inverting the same calculation. Three places, one
number: `GO_PADYPX` in the header. When the layout used one value and drawing
another (`GO_PADY` = 4 in `layout.c`, a hand-written `4` in `graphwnd.c`) the
graph looked perfectly fine; it was the **double click** that picked the wrong
line, i.e. the kind of bug you don't notice by looking.

## 10. Writing a new export in C isn't enough

`ODBG_Pluginmainloop` compiled but not added to `build/grapholly.def` does not
exist as far as Olly is concerned: no error, the graph just never follows EIP.
The same dual form as pitfall 1 applies (with and without underscore).

## 11. Ctrl+G belongs to Olly, and adding Shift doesn't help

`Ctrl+G` in the Disassembler is "Go to expression": Olly consumes it and never
passes it to `ODBG_Pluginshortcut`. That much was known.

**What wasn't known**: `Ctrl+Shift+G` doesn't get through either. Tested on
0.1.2: pressing it opens the "Go to expression" window, i.e. Olly **ignores
Shift** and treats the combination as plain `Ctrl+G`. You can't build a plugin
shortcut by adding a modifier to one Olly already uses: you need a **letter it
doesn't use at all**.

Which letters those are isn't guesswork; you read them from its menus inside
the executable (the strings containing `\tCtrl+…`):

| | |
|---|---|
| taken | `A B C E F G J K L N O R S T`, plus `Ctrl+F1 F2 F5`, `Ctrl+Gray *`, `Ctrl+Plus`, `Ctrl+Minus` |
| **free** | `D H I M P Q U V W X Y Z` |

GraphOlly uses `Ctrl+D`, and the `Shortcut` key in the ini lets you change it.
Every shortcut that gets through ends up in the trace: if a combination doesn't
show up there, Olly swallowed it first.

## 12. Dummy nodes without a cap make the problem they fix worse

An edge that skips more than one level, if it isn't split, has to cross the
whole graph in a horizontal channel: on a large function the result is a comb
of lines as wide as the window. The classic fix (Sugiyama) is to split it into
a chain of **dummy nodes**, one per intermediate level, which reserve column
space for it.

Applied without limits to `00401159` in `OLLYDBG.EXE` (599 blocks, 983 edges,
**342 levels**, an `if`-ladder dispatcher) it produced:

| | dummy nodes | columns | width |
|---|---|---|---|
| no cap | **5545** (cap reached) | 25 | 6460 px |
| with `GO_MAXSPAN 8` | **502** | 17 | 5425 px |

With 5545 dummy nodes every level widened to twenty-five columns and the graph
frayed out **diagonally**: worse than the comb the dummy nodes were supposed to
remove. The reason is that in a dispatcher hundreds of edges go from any level
to the common exit, and each of them drags three hundred dummy nodes along.

**Long edges are few, and routing them through the corridor is fine; the
medium-length ones are what make the comb.** Only edges spanning up to eight
levels are split.

## 13. Zooming around the window centre lands in empty space

Pressing "100%" on a 5425 x 39164 pixel graph filled the window with white:
anchoring the zoom to the **window centre**, on a large graph that centre is
almost always the gap between two columns. Keyboard zoom anchors to the block
under the **cursor**; wheel zoom anchors to the point under the mouse pointer,
which is the only place the user is actually looking at.

Same reasoning for "fit to window": when the required scale drops below the
minimum, instead of showing an unreadable postage stamp it fits to the
**width** only and scrolls to the cursor.

## 14. A fall-through is not a jump, and the layout has to know

In the graph, a fall-through (the block that follows in memory) and a jump are
both edges, but they don't weigh the same: the fall-through is the **natural
continuation of the code** and wants to stay in the column below its
predecessor. With a barycentre that treats them equally, a block with many
incoming jumps gets pulled out of its column and the fall-through turns into a
detour as wide as the graph.

Fix in two steps: `t_goedge.fall` is set in `cfg.c` (the target starts exactly
where the source ends); then in the layout a fall-through weighs **four times**
a jump in the barycentre, and a final straightening pass moves the
fall-through block into its predecessor's column when that column is free at
that level.

## 15. Changes made OUTSIDE the window send no message

A breakpoint set from the Disassembler generates no event for the graph
window: it kept showing the old ones until it was moved. Blindly redrawing
twice a second would be wasteful; instead a **signature** of the breakpoint
table (walking `Getnextbreakpoint`, summing `addr^type`) is computed on a
400 ms timer, and the window is redrawn only when it changes.

Tested: a breakpoint set at `00401780` through another path, with no message
reaching the window, shows up on its own.

## 16. `Go()` called from a plugin callback doesn't resume the program

To prove that the graph switches function on its own when execution enters
one, the QA called `Go(Getcputhreadid(),0x401030,STEP_RUN,0,0)` from inside
the service door. The call returns 0 (success) but the debuggee advances by
**a single instruction**: from `00401460` to `0040146A`. Also tried with
`threadid=0`, same result. Apparently Olly's debug loop doesn't resume when
`Go` comes from outside that loop.

Consequence: **function following is not proven by the QA** and stays on the
list of things to check by hand. The external-breakpoint mechanism, on the
other hand, can be tested just fine and is.
