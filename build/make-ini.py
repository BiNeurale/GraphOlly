#!/usr/bin/env python3
"""Generates ini/ollydbg-recommended.ini for the ORIGINAL OllyDbg 1.10.

Not written by hand: it is derived from two files in tools/ (not in the repo).

  tools/r4ndom/ollydbg.ini   an ini written by Olly, so the list of keys
  tools/odbg110/OLLYDBG.EXE  the key strings that 1.10 can actually read

Crossing the two keeps only the lines that do something. R4ndom's ini has 281
of them, but 124 exist in no binary of the package: they are OllyDbg 2.x
options, and 1.10 silently ignores them. Then whatever is personal and should
not be handed to anyone (paths, history, window positions, pane state) is
dropped, and the recommended values are applied.

NO COMMENTS in the output: Olly's ini parser was never tested with ';', and
Olly rewrites that file on exit anyway - any comment would be gone the first
time it closes. The explanations live in docs/OLLYDBG-INI.md and in the
README.txt that ships next to the file.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Where I depart from R4ndom's values, and why - the full reasoning is in
# docs/OLLYDBG-INI.md.
RECOMMENDED = {
    'Show jumpfrom path':                 '1',  # who jumps HERE: he keeps it off
    'Code highlighting':                  '1',  # he ships everything black
    'Index of default syntax highlighting':'2', # "Jumps'n'calls" scheme
    'Demangle symbolic names':            '1',  # needed on C++ binaries
    'Show ARG and LOCAL in procedures':   '1',
    'Decode ifs as switches':             '1',
    'First pause':                        '1',  # entry point, not the system bp
    'Number of lines that follow EIP':    '6',  # EIP does not stick to the bottom
    'Extended stack trace':               '1',
    'Highlight RETURNs in stack':         '1',
    'Restore width of columns':           '1',
    'Ignore path in user data file':      '1',  # the .udd is found if the file moves
    'Ignore timestamp in user data file': '1',
    'Backup UDD files':                   '1',
}

# UI state of whoever used that Olly: not configuration.
PERSONAL = ('Last edited', 'Last selected', 'X options', 'Y options')


def main():
    exe = (ROOT / 'tools/odbg110/OLLYDBG.EXE').read_bytes()
    src = (ROOT / 'tools/r4ndom/ollydbg.ini').read_text(errors='replace').split('\n')

    keys = {}
    for line in src[1:]:
        if line.startswith('['):
            break
        if '=' in line:
            k, v = line.split('=', 1)
            keys[k] = v

    real = {k: v for k, v in keys.items() if k.encode('latin1') in exe}
    inert = len(keys) - len(real)
    for k in [k for k in real if k.startswith(PERSONAL)]:
        del real[k]
    for k, v in RECOMMENDED.items():
        if k in real:
            real[k] = v
        else:
            print(f"WARNING: 1.10 does not know '{k}'", file=sys.stderr)

    out = ['[Settings]', 'Plugin path=.', 'UDD path=.', 'Symbolic data path=.']
    out += [f'{k}={v}' for k, v in real.items()]
    # NOT in dist/: make-dist.sh starts with "rm -rf dist" and would wipe it
    # on every package rebuild. This directory belongs to this script.
    dest = ROOT / 'ini/ollydbg-recommended.ini'
    dest.parent.mkdir(exist_ok=True)
    dest.write_text('\n'.join(out) + '\n')
    print(f'{dest}: {len(out)} lines '
          f'({len(keys)} keys in the source, {inert} inert on 1.10)')


if __name__ == '__main__':
    main()
