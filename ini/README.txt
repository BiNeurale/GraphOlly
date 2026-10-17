ollydbg-recommended.ini - settings for the ORIGINAL OllyDbg 1.10
================================================================

HOW TO USE IT
  Copy it next to OLLYDBG.EXE as  ollydbg.ini.  If you already have one you
  care about, put it aside first: Olly rewrites that file by itself every time
  it exits.
  The three paths at the top (Plugin path, UDD path, Symbolic data path) are
  RELATIVE: they point to Olly's own folder. If you keep plugins elsewhere,
  change them.
  The file has NO comments, on purpose: Olly rewrites it on exit and would
  drop them anyway.

WHERE IT COMES FROM
  It is not hand-written. It is derived from the ini that ships with R4ndom's
  build - an ini written by Olly, so it lists the real keys - keeping only the
  lines 1.10 can actually read (build/make-ini.py).

  His file has 281 keys. 124 of them exist in NO binary of that package: not
  in the executable (checked on both builds) and not in any of its 19 plugins.
  They are OllyDbg 2.x options - Show loops, Fuzzy analysis, Allow .NET
  debugging, Debug child processes, the whole "Warnmode when..." family. That
  file was used by two different versions, and 1.10 silently ignores whatever
  it does not know. General lesson: do not copy lines from an ini found
  somewhere and expect them to do anything.
  That leaves 157 keys. From those I removed the paths of his machine, the
  recent-files history, window positions and pane state: those are his, not
  configuration.

WHERE I DEPART FROM HIS VALUES, AND WHY
  Show jumpfrom path=1
      The jumps that ARRIVE at the current line, not the ones leaving it. It
      is the only one of the five jump lines R4ndom keeps off, and the most
      useful one during analysis. In the dialog: "Show jumps to selected
      command".
  Show jump path=1 (+ direction, + path/grayed if not taken)
      The line connecting the SELECTED jump to its destination. Without these
      1.10 does not draw it at all.
  Code highlighting=1  +  Index of default syntax highlighting=2
      He ships the disassembly all black. Scheme 2 is "Jumps'n'calls" and
      colours jumps and calls.
  First pause=1
      Stop at the entry point of your own executable instead of at the system
      breakpoint inside ntdll, where the main module is not registered yet and
      the analysis has nothing to work on.
  Number of lines that follow EIP=6
      While stepping, EIP does not stick to the last line: you always see six
      instructions of what is about to happen.
  Demangle symbolic names=1, Show ARG and LOCAL in procedures=1,
  Decode ifs as switches=1, Extended stack trace=1,
  Highlight RETURNs in stack=1, Restore width of columns=1,
  Ignore path and Ignore timestamp in user data file=1, Backup UDD files=1
      Readability, and work that does not get lost. With the last two the .udd
      is found again even if you move or rebuild the file.

A WARNING, HONESTLY
  Of all these keys I MEASURED four - the ones that turn on the line between a
  jump and its destination. The others are read from the key name and the
  matching label in the Options dialog inside the executable: they are
  reasoned choices, not tests.

  One group is better left alone in advance: the "Accept ..." family and
  "Looseness of code analysis". They help when the analysis stops where it
  should not on a protected binary; raising them makes the analyser accept the
  very sequences a packer uses to confuse it. Change them in response to a
  problem, not before.

  The full list of the 157 keys, grouped by topic, is in OLLYDBG-INI.md (next
  to this file in the package, docs/OLLYDBG-INI.md in the repository).
