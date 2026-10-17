# `ollydbg.ini`: what OllyDbg 1.10 actually understands, and what's worth turning on

Worked out from two files I have locally, not from a forum: the ini that
R4ndom distributes (`tools/r4ndom/ollydbg.ini`) is an ini **written by Olly**,
so it lists the keys; and `tools/odbg110/OLLYDBG.EXE` contains the strings of
the keys that version can read. Cross-referencing the two tells you which lines
do something and which don't.

## First: that ini is mixed, and 124 of its 281 lines are inert

Of the **281** keys in R4ndom's `[Settings]`, **157** exist as strings inside
`OLLYDBG.EXE` 1.10. The other **124 don't exist in any binary in the package**:
not in the executable (in neither build: the list of missing keys is
identical), nor in any of the 19 plugins. They are **OllyDbg 2.x** options:
`Show loops`, `Fuzzy analysis`, `Allow .NET debugging`, `Activate speech`,
`Debug child processes`, `Flash duration`, the whole `Warnmode when…` family,
the whole `Hit trace…` family. That file was used by two different versions,
and 1.10 silently ignores what it doesn't know.

⚠️ Practical consequence: **don't copy lines from an ini you found somewhere**
and expect them to have an effect. If a key isn't in the list below, it does
nothing on 1.10.

## The 157 real keys

All of them go under `[Settings]`. Values are `0`/`1` unless stated otherwise.
The "R4ndom" column is the value he ships.

### Arrows and Disassembler readability

| key | R4ndom | what it does |
|---|---|---|
| `Show jump direction` | 1 | the `^` and `v` marks next to a jump's opcode |
| `Show jump path` | 1 | **the line connecting the selected jump to its destination** |
| `Show path if jump is not taken` | 1 | draws it even when the jump won't be taken |
| `Show grayed path if jump is not taken` | 1 | and in that case draws it in grey |
| `Show jumpfrom path` | **0** | the jumps that **land** on the selected line |
| `Code highlighting` | **0** | turns on syntax highlighting |
| `Index of default syntax highlighting` | 0 | which scheme: `2` = "Jumps'n'calls" |
| `Index of default colours` | 0 | colour scheme (0 = black on white) |
| `Index of default font` | 1 | font |
| `Underline fixups` | 1 | underlines relocated addresses |
| `Gray data used as filling` | 1 | greys out padding between procedures |
| `Highlighted trace register` | -1 | register to highlight in the run trace |
| `Remove code hilite on register hilite` | — | *(one of the 124 inert ones)* |

### How the disassembly reads

`Disassemble in lowercase`, `IDEAL disassembling mode`, `Separate arguments with TAB`,
`Extra space between arguments`, `Show default segments`, `Always show memory size`,
`NEAR jump modifiers`, `Use short form of string commands`, `Use RET instead of RETN`,
`SSE size decoding mode`, `Size sensitive mnemonics`, `Top of FPU stack`,
`Decode VxD calls`, `Tab stops` (a number, usually 8).

### Names and symbols

`Show symbolic addresses`, `Show local module names`, `Demangle symbolic names`,
`Label display mode` (0-2), `Label includes module name`, `Highlight symbolic labels`,
`Keep ordinal in name`, `Default sort mode in Names`, `String decoding`,
`Decode pascal strings`, `Only ASCII printable in dump`, `Allow diacritical symbols`.

### Analysis

| key | R4ndom | what it does |
|---|---|---|
| `Analyse main module automatically` | 1 | analyses the main module on its own |
| `Analyse code structure` | 1 | reconstructs procedures |
| `Decode tricks` | 1 | recognises packers' "clever" sequences |
| `Decode ifs as switches` | 0 | cascaded `if`s become a switch |
| `Functions preserve registers` | 1 | assumes the standard convention |
| `Guess number of arguments` | 1 | guesses how many arguments a function takes |
| `Looseness of code analysis` | 1 | how permissive the analyser is |
| `Save analysis to file` / `Compress analysis data` | 1 / 1 | the analysis survives the session |
| `Show ARG and LOCAL in procedures` | 0 | names of arguments and local variables |
| `Show decoded arguments` / `…in stack` / `Show arguments in call stack` | 1 | decoded arguments |
| `Show induced calls` | 1 | |

The `Accept …` family decides what the analyser accepts as valid code:
`Accept far calls and returns`, `Accept direct segment modifications`,
`Accept privileged commands`, `Accept I/O commands`, `Accept NOPs`,
`Accept shifts out of range`, `Accept superfluous prefixes`,
`Accept unaligned stack operations`, `Accept non-standard command forms`,
`Accept LOCK prefixes`.

### Execution and pauses

| key | R4ndom | what it does |
|---|---|---|
| `First pause` | **3** | where it stops on load: 1 = module entry point, 2 = WinMain, 3 = system breakpoint |
| `Number of lines that follow EIP` | 0 | how many lines stay visible **below** EIP while stepping |
| `Center FOLLOWed command` | 1 | the "followed" command ends up centred |
| `Visible lines when scrolling disasm` | — | *(inert on 1.10)* |
| `Stop on new DLL` / `DLL unload` / `debug string` / `new thread` / `thread end` | 0 | extra pauses |
| `Autoreturn`, `Use ExitProcess`, `Warn when frequent breaks`, `Warn when break not in code` | | |
| `Step in unknown commands` | 0 | also steps into instructions it doesn't recognise |
| `Use hardware breakpoints to step` | 0 | |
| `Set high priority` | 1 | raises priority while tracing |
| `Animate over system DLLs` | 0 | |

### Exceptions

`Ignore access violations in KERNEL32`, `Ignore INT3`, `Ignore TRAP`,
`Ignore access violations`, `Ignore division by 0`, `Ignore illegal instructions`,
`Ignore all FPU exceptions`, `Ignore custom exceptions`.

### Stack and registers

`Show stack frames`, `Show local names in stack`, `Extended stack trace`,
`Highlight RETURNs in stack`, `Enable last error`, `Decode SSE registers`,
`Decode registers for any IP`, `Automatically select register type`,
`Modify tag of FPU register`, `MMX display mode`.

### Run trace and log

`Run trace buffer size index`, `Trace over string commands`,
`Synchronize CPU and Run trace`, `Save original command in trace`,
`Show traced ESP`, `Show traced flags`, `Save trace to file`,
`Trace contents of registers`, `Group adjacent commands in profile`,
`Log buffer size index`, `Tabulate columns in log file`,
`Append data to existing log file`, `Flush gathered data to log file`.

### `.udd` files (the store of work done on an executable)

`Backup UDD files`, `Ignore path in user data file`,
`Ignore timestamp in user data file`, `Ignore CRC in user data file`,
`Save out-of-module user data`.

### SFX (self-extracting executables)

`Include SFX extractor in code`, `SFX trace mode`,
`Use real SFX entry from previous run`, `Ignore SFX exceptions`.

### Windows, search, miscellaneous

`Check DLL versions`, `Topmost window`, `Show toolbar`, `Status in toolbar`,
`Restore windows`, `Restore window positions`, `Restore width of columns`,
`Highlight sorted column`, `Horizontal scroll`, `Scroll MDI`,
`Snow-free drawing`, `Append arguments`, `File graph mode`, `Align dialogs`,
`Use font of calling window`, `Specified dialog font`,
`Global search`, `Aligned search`, `Ignore case`, `Ignore case in text search`,
`Reference search mode`, `Allow error margin`, `Keep size of hex edit selection`,
`Hex inspector limits`, `Letter key in Disassembler`,
`Fill rest of command with NOPs`, `Smart update`, `Allow injection to get WinProc`,
`Sort WM_XXX by name`, `Type of last WinProc breakpoint`,
`Warn if not administrator`, `Warn when terminating process`,
`Show internal handle names`, `Hide irrelevant handles`,
`Synchronize source with CPU`, `Skip spaces in source comments`,
`Hide non-existing source files`, `Last selected options card`,
`Last selected appearance card`.

## What I recommend, and why

I haven't measured any of these except the four jump options: the rest I read
from the name and from the matching label in the Options panel inside the
executable. These are recommendations, not measurements.

```ini
[Settings]
; --- jump arrows -------------------------------------------------------
Show jump direction=1
Show jump path=1
Show path if jump is not taken=1
Show grayed path if jump is not taken=1
Show jumpfrom path=1          ; who jumps HERE: R4ndom keeps it off

; --- colour: jumps and calls stand out at a glance ---------------------
Code highlighting=1
Index of default syntax highlighting=2   ; "Jumps'n'calls" scheme

; --- analysis, which is also what makes GraphOlly work -----------------
Analyse main module automatically=1
Analyse code structure=1
Decode tricks=1
Save analysis to file=1
Compress analysis data=1
Show ARG and LOCAL in procedures=1
Demangle symbolic names=1     ; needed on C++ binaries

; --- while stepping ----------------------------------------------------
First pause=1                 ; module entry point instead of the system bp
Number of lines that follow EIP=6   ; EIP doesn't stay stuck at the bottom
Center FOLLOWed command=1

; --- a readable stack --------------------------------------------------
Show stack frames=1
Show local names in stack=1
Extended stack trace=1
Highlight RETURNs in stack=1
Show arguments in call stack=1
Enable last error=1

; --- don't lose the work done ------------------------------------------
Backup UDD files=1
Ignore path in user data file=1      ; the .udd is found even if you move the file
Ignore timestamp in user data file=1
Restore windows=1
Restore window positions=1
Restore width of columns=1

; --- faster tracing ----------------------------------------------------
Set high priority=1
```

Four notes on the less obvious choices:

**`Show jumpfrom path=1`.** It's the only one of the five jump lines that
R4ndom keeps **off**, and for analysis it's the most useful of the five: it
tells you who jumps *to* the line you're on, not where you're going. It
corresponds to "Show jumps to selected command" in the Options panel.

**`Code highlighting=1` with scheme 2.** R4ndom ships it at `0`: the
disassembly is all black. The `Jumps'n'calls` scheme colours jumps and calls,
and **GraphOlly takes its colours from there** (`VAL_COLORS`), so the graph
follows the scheme you pick instead of having its own palette.

**`First pause=1`.** R4ndom stops at the system breakpoint (`3`), which is
inside `ntdll`, before the main module is registered; that's why the graph
comes out marked `[no procedure]` there (pitfall 7 in `docs/NOTES.md`). With
`1` you stop at your executable's entry point. ⚠️ In R4ndom's build this **is
not enough**: measured, it still stays in `ntdll` because one of his 19
plugins stops earlier. On vanilla 1.10 it works.

**The `Accept …` family.** On normal code, leave it as is. On a protected
binary, if the analysis stops where it shouldn't, raising
`Accept superfluous prefixes`, `Accept shifts out of range`,
`Accept privileged commands` and `Looseness of code analysis` makes the
analyser accept the sequences the packer uses specifically to confuse it. It's
the only group worth touching *in response to a problem*, not beforehand.
