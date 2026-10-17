#!/usr/bin/env python3
"""Capture an X window by name, inside a compositor (gamescope).

gamescope's X root is black because windows are redirected: so this reads the
window's composite pixmap (XComposite), not the screen. It writes a PPM, which
magick converts to PNG.

  xshot.py "OllyDbg" /tmp/out.ppm
"""
import sys
from Xlib import display, X
from Xlib.ext import composite


def find_window(dpy, root, needle):
    hits = []

    def walk(win):
        try:
            name = win.get_wm_name()
        except Exception:
            name = None
        if name and needle in name:
            geo = win.get_geometry()
            hits.append((win, name, geo.width, geo.height))
        try:
            for child in win.query_tree().children:
                walk(child)
        except Exception:
            pass

    walk(root)
    # The largest one: wine's shadow windows are tiny.
    hits.sort(key=lambda t: t[2] * t[3], reverse=True)
    return hits


def main():
    needle, out = sys.argv[1], sys.argv[2]
    dpy = display.Display()
    root = dpy.screen().root

    if not dpy.has_extension('Composite'):
        print("NO Composite extension")
        return 4

    hits = find_window(dpy, root, needle)
    if not hits:
        print(f"WINDOW NOT FOUND: {needle}")
        return 3
    for _, name, w, h in hits[:6]:
        print(f"  candidate: {name!r} {w}x{h}")
    win, name, w, h = hits[0]

    composite.redirect_window(win, composite.RedirectAutomatic)
    dpy.sync()
    pixmap = composite.name_window_pixmap(win)
    raw = pixmap.get_image(0, 0, w, h, X.ZPixmap, 0xFFFFFFFF)
    data = raw.data

    # 32-bit BGRA -> 24-bit PPM.
    px = bytearray()
    for i in range(0, w * h * 4, 4):
        px += bytes((data[i + 2], data[i + 1], data[i]))
    with open(out, 'wb') as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(px)
    print(f"OK {name!r} {w}x{h} -> {out}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
