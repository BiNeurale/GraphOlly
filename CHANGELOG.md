# Changelog

## 1.0.0 - 2026-10-17

First public release. Compared with 0.1.5:

- **Search, export, minimap.** Ctrl+F searches the graph lines (address,
  mnemonic, operand); F3 / Shift+F3 and Ctrl+L walk the hits in address order;
  Esc clears them. Export of the whole graph to a 24-bit BMP, capped at
  16 Mpixel and 16384 px per side, scaled down past that. Minimap in the
  corner (`M`, `Minimap=` in the ini), cached and redrawn only when the layout
  or the search changes.
- **Layout speed and quality.** Adjacency lists, topological level order and
  precomputed barycentres: the same layout pixel for pixel (identical bench
  signatures), from 22-43 ms to about 1 ms on 2000 blocks. Barycentre ordering
  now runs level by level on the already updated neighbour and keeps the pass
  with the fewest crossings (counted with a Fenwick tree): crossings on the
  2000-block bench graphs went from 6215 to 371 and from 5605 to 265.
  Fall-throughs kept in their predecessor's column: 276 to 681 of 1557.
- **CFG.** Switches recognised by Olly get their cases in the graph instead of
  an unresolved exit. Calls that never return (`exit`, `ExitProcess`,
  `abort`...) end the block: the graph of winshot.exe's entry went from six
  spurious blocks to one. Full queues or edge tables now mark the graph
  truncated instead of losing edges silently. Overlapping instruction streams
  (obfuscated code) no longer duplicate block tails. Inlining uses the CALL
  destination itself as the entry: when Olly merged two functions into one
  procedure, double-clicking the call did nothing.
- **Text.** Drawn down to 5-pixel lines with a TrueType font at the exact
  cell height; below 10 pixels without anti-aliasing, which on Windows 7 had
  turned the text into a grey veil.
- **Windows 7.** The DLL and the test executables link against `msvcrt.dll`
  instead of the UCRT: on a clean Windows 7 the 0.1.5 DLL did not load and Olly
  said nothing. Test executables are linked without ASLR so addresses are
  fixed across runs.
- **Function following.** Opening the graph of a function other than the one
  EIP is in no longer gets replaced at once by EIP's function: the graph
  follows only when EIP moves while the window is open.
- Layout bench on the host (`build/bench/`), one wine smoke test
  (`build/qa-wine.sh`) instead of the per-version scripts, all sources, docs
  and tools in English.

## 0.1.x - 2026-09-22 to 2026-09-24

- **0.1.5** - Fall-throughs marked in the CFG, weighted four times a jump in
  the barycentre and kept in their predecessor's column: the large dispatcher
  function of OLLYDBG.EXE (00401159) turns from a diagonal staircase into a
  centred tree. Breakpoints set outside the graph show up by themselves (400 ms
  timer on a checksum of the breakpoint table). The graph follows execution
  into other functions (`FollowFunction=`).
- **0.1.4** - Debugging inside the graph: a line cursor, Enter to follow a
  jump or a call, Backspace to go back (32 steps), `*` / Space to EIP, F2 for
  breakpoints, shown the way Olly marks them. False branch always on the left,
  true on the right; T/F letters only with `Labels=1`. Dummy nodes for long
  edges, capped at 8 levels: on 00401159 (599 blocks, 983 edges, 342 levels)
  an uncapped split made 5545 dummy nodes and 25 columns, the cap brings it to
  502 and 17. Zoom anchored on the cursor instead of the window centre.
- **0.1.3** - Shortcut moved to Ctrl+D (Ctrl+Shift+G opened Olly's "Go to
  expression": Olly ignores the Shift), configurable with `Shortcut=`. Zoom
  with Ctrl+wheel, `+`/`-`, `0`, End to fit; the window opens fitted; overview
  mode at low zoom; hover highlight, middle-button drag, context menu.
  Recommended ini for vanilla 1.10: of the 281 keys in R4ndom's ini, 124 are
  OllyDbg 2.x options that 1.10 never reads.
- **0.1.2** - Grid layout with corridors and channels, one lane per edge, so
  edges no longer run over blocks. EIP tracking at every pause. Double-click on
  a CALL inlines the callee (up to 64 functions). The jump-path lines of the
  Disassembler turned out to be Olly options, not a patch in R4ndom's build
  (the `.text` sections are byte-identical).
- **0.1.1** - Text lines moved out of the block struct into a shared pool
  (`t_gograph` from 25 MB to 144 KB, zeroed on every request). Binary search
  for blocks, one disassembly pass fewer, double-buffered painting at 330-370
  us per paint on 12 blocks. Barycentre ordering. Same function asked twice
  reuses its window. Diagnostics menu entry and an optional trace file.
- **0.1.0** - First graph inside OllyDbg 1.10: exports with the leading
  underscore Olly looks for, `plugin.h` packed on its own instead of
  `-fpack-struct` on everything, recursive descent instead of a linear sweep,
  two ready-to-run packages (vanilla 1.10 and R4ndom's build) with relative
  paths in the ini.
