#!/usr/bin/env python3
"""Drive Aurora's UI on an X display for manual/AI verification.

A tiny XTEST wrapper so a headless session can click, type and screenshot a
running aurora_browser. Development tool only — not built or shipped.

Usage:
  uidrive.py shot out.png
  uidrive.py click X Y [button]
  uidrive.py move X Y
  uidrive.py key <keysym-name> [...]
  uidrive.py type "text"
  uidrive.py windows            # list top-level windows and geometry
"""
import subprocess
import sys
import time

from Xlib import X, XK, display
from Xlib.ext import xtest

DISPLAY = ":98"

# Screenshots are downscaled to this width before being handed to a reader, so
# coordinates read off an image must be scaled back up by SHOT_SCALE to become
# X11 root coordinates. Keeping one fixed shot width makes that conversion
# deterministic instead of guesswork.
ROOT_W = 1440
SHOT_W = 1200
SHOT_SCALE = ROOT_W / SHOT_W


def dpy():
    return display.Display(DISPLAY)


def shot(path):
    env = {"DISPLAY": DISPLAY, "PATH": "/usr/bin:/bin"}
    subprocess.run(["import", "-window", "root", "-resize", str(SHOT_W), path],
                   env=env, check=True)


def click(x, y, button=1):
    d = dpy()
    xtest.fake_input(d, X.MotionNotify, x=int(x), y=int(y))
    d.sync()
    time.sleep(0.15)
    xtest.fake_input(d, X.ButtonPress, int(button))
    d.sync()
    time.sleep(0.08)
    xtest.fake_input(d, X.ButtonRelease, int(button))
    d.sync()


def move(x, y):
    d = dpy()
    xtest.fake_input(d, X.MotionNotify, x=int(x), y=int(y))
    d.sync()


def key(*names):
    d = dpy()
    for name in names:
        mods = []
        parts = name.split("+")
        base = parts[-1]
        for m in parts[:-1]:
            mods.append({"ctrl": "Control_L", "alt": "Alt_L",
                         "shift": "Shift_L"}[m.lower()])
        codes = [d.keysym_to_keycode(XK.string_to_keysym(m)) for m in mods]
        kc = d.keysym_to_keycode(XK.string_to_keysym(base))
        for c in codes:
            xtest.fake_input(d, X.KeyPress, c)
        xtest.fake_input(d, X.KeyPress, kc)
        xtest.fake_input(d, X.KeyRelease, kc)
        for c in reversed(codes):
            xtest.fake_input(d, X.KeyRelease, c)
        d.sync()
        time.sleep(0.05)


def type_text(text):
    d = dpy()
    for ch in text:
        keysym = XK.string_to_keysym({" ": "space", "/": "slash", ":": "colon",
                                      ".": "period", "-": "minus",
                                      "?": "question", "=": "equal",
                                      "_": "underscore", "&": "ampersand"}
                                     .get(ch, ch))
        if keysym == 0:
            continue
        kc = d.keysym_to_keycode(keysym)
        shift = ch in ':?_&' or ch.isupper()
        sk = d.keysym_to_keycode(XK.string_to_keysym("Shift_L"))
        if shift:
            xtest.fake_input(d, X.KeyPress, sk)
        xtest.fake_input(d, X.KeyPress, kc)
        xtest.fake_input(d, X.KeyRelease, kc)
        if shift:
            xtest.fake_input(d, X.KeyRelease, sk)
        d.sync()
        time.sleep(0.02)


def windows():
    d = dpy()
    root = d.screen().root

    def walk(win, depth=0):
        try:
            g = win.get_geometry()
            name = win.get_wm_name()
        except Exception:
            return
        print(f"{'  ' * depth}{win.id:#x} {g.width}x{g.height}+{g.x}+{g.y} {name or ''}")
        try:
            for c in win.query_tree().children:
                walk(c, depth + 1)
        except Exception:
            pass

    walk(root)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "shot":
        shot(sys.argv[2])
    elif cmd == "click":
        click(*sys.argv[2:])
    elif cmd == "clicki":  # coordinates read off a screenshot
        args = [float(a) * SHOT_SCALE for a in sys.argv[2:4]]
        click(*args, *sys.argv[4:])
    elif cmd == "move":
        move(*sys.argv[2:])
    elif cmd == "key":
        key(*sys.argv[2:])
    elif cmd == "type":
        type_text(sys.argv[2])
    elif cmd == "windows":
        windows()
    else:
        raise SystemExit(__doc__)
