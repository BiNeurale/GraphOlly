# What R4ndom's OllyDbg build changes (2026-09-23)

Starting question: do the **jump arrows** in the Disassembler come from a patch
to the executable? If so, I'd want to redo it, because it's useful and I don't
want to depend on a third-party binary.

**Measured answer: there is no code patch at all.** The `.text` section of the
two executables is **byte-identical**, and the arrows are an OllyDbg 1.10
feature, switched on by four ini options.

## Byte-by-byte comparison

`tools/odbg110/OLLYDBG.EXE` (1,117,696 bytes, 23/05/2004) against
`tools/r4ndom/OLLYDBG.EXE` (1,136,128 bytes, 25/05/2012): **88,358 bytes
differ**, distributed like this:

| where | bytes differing | what they are |
|---|---|---|
| PE header (<1536) | 8 | checksum and size of `.rsrc` |
| **`.text`** (0x600 + 0xAE800) | **0** | md5 `f14aa0b2eeca633b1f52d3f6166ac920` on both |
| `.data` | 164 | window class names (below) |
| `.tls .rdata .idata .edata` | 0 | — |
| `.rsrc` | 88,186 | 27 toolbar bitmaps + the title of one dialog |

### `.data`: randomised class names

A single kind of change, repeated 19 times: the strings Olly uses to register
its window classes are replaced with garbage of the same length. It's
anti-anti-debug: a protected program looking for the `OLLYDBG` window won't
find it.

| 1.10 | R4ndom |
|---|---|
| `OLLYDBG` | `fengyue` |
| `ACPUASM` `ACPUDUMP` `ACPUSTACK` `ACPUREG` `ACPUINFO` `ACPU` | `RKmMfPH` `cTd4oOiC` `QhS7eTjDw` `VZnSNHS` `C7PCWBCL` `mSk5` |
| `OT_PLUGIN_%04i` | `wB5JNlUyK_%04i` |
| `ABPOINT` `ADUMP` `ALIST` `AMODULE` `AMEMORY` `AREF` `APROCESS` `ATHREAD` `AWATCH` `ARTRACE` `ASOURCE` `ASRCLIST` `AWINDOWS` `ACALLSTK` `AHANDLES` `APATCH` | same, each with its own random string |

⚠️ This is why, in the GraphOlly trace, the graph window class is
`OT_PLUGIN_0002` in vanilla and `wB5JNlUyK_0001` in R4ndom: Olly picks the
format, not the plugin.

### `.rsrc`: appearance only

27 toolbar `BITMAP` resources rewritten (from 308 bytes each, 16 colours, to
926–1122 bytes): `RUN`, `PAUSE`, `STEPIN`, `STEPOVER`, `CPU`, `LOG`,
`MEMORY`, `MODULES`… That's the coloured toolbar. Plus a single `DIALOG`
resource, `DIA_ABOUT`, with the same byte count: the title "About OllyDbg"
becomes "About R4ndom". The link inside the dialog still points to Oleh
Yuschuk's site.

## The arrows: they're OllyDbg 1.10's, and the ini turns them on

The checkbox strings are **in vanilla 1.10** (Options → Debugging options →
CPU):

```
Show jump path
Show grayed path if jump is not taken
Show jumps to selected command
Show jump direction
```

R4ndom's ini writes them like this:

```
Show jump direction=1
Show jump path=1
Show path if jump is not taken=1
Show grayed path if jump is not taken=1
Show jumpfrom path=0
```

### The screenshot test

I started Olly three times on the same target:
(A) vanilla 1.10 with our package's ini, where the four lines are **absent**;
(B) the same vanilla with the four lines copied from R4ndom;
(C) R4ndom as shipped.

The target is `build/jumps.c` (`build/out/jumps.exe`): the entry point is a
small group of short jumps **written by hand in assembly**, because in C the
compiler resolves them with `CMOV` and there would be no jumps. The
destination has to be on screen: without a visible destination Olly has no
line to draw.

Result: **A and B are the same image**. With our package's minimal ini,
vanilla 1.10 already draws both the directions (`^` `v` next to the opcode)
and the loop bracket `40100E ← 401014`. Those four options are **on by
default**; R4ndom's ini just writes them out.

**Consequence: there's nothing to develop**, but the four lines are needed,
and the package's minimal ini didn't have them. They're there since 0.1.2.

### The first test was inconclusive, and the conclusion wrong

On 2026-09-22 the comparison "vanilla without the lines" / "vanilla with the
lines" had produced **two identical images**, and from that came the
conclusion that the options were on by default. It was wrong: in both
screenshots **no jump was selected**, and Olly draws the line connecting a jump
to its destination only for the jump **under the cursor**. The comparison
could not have shown any difference.

The brackets visible in those images (`┌S`/`└.` in the column between address
and opcode, and the bracket next to the loop mnemonics) are two other things:
the **procedure bracket** recognised by the analysis, and the loop path.
Mistaking them for the path of the selected jump is what let a test that
proved nothing pass as good.

On 2026-09-24, actually selecting the `JE SHORT 00401016` in 1.10 with the
package's minimal ini: **the line does not appear**. In the R4ndom screenshot,
with `JMP SHORT 7BCF0613` selected, it does.

**Lesson on method**: an A/B test has to put the subject in the state where
the difference can exist. Two identical screenshots say "can't see it here",
not "it isn't there".

## Effect on pitfall 7 (the empty module in R4ndom)

Tested in this round: forcing `First pause=1` on the test bench, R4ndom's build
**still** stops inside `ntdll` (`7BCF0576`), not at the module entry point.
The option isn't what decides that pause: some plugin stops earlier, which is
consistent with the empty module record described in pitfall 7 of
`docs/NOTES.md`.
