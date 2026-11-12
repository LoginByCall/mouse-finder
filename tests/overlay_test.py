#!/usr/bin/env python3
"""Regression: during the effect the pointer must be over our fullscreen XWayland
overlay (so our big cursor is shown over every window), and GNOME cursor-size
must be restored afterwards (bug 2026-11-12: cursor big only after touching a
window border).
Needs a graphical session, rw /dev/uinput, python3-evdev, python3-xlib;
stop mouse-finder first. Moves the real cursor horizontally for ~1 s.
Usage: python3 tests/overlay_test.py [path/to/mouse-finder]
"""
import os, subprocess, sys, time
from evdev import UInput, ecodes as e
from Xlib import display

BIN = sys.argv[1] if len(sys.argv) > 1 else "dist/mouse-finder"
size = lambda: subprocess.check_output(
    ["gsettings", "get", "org.gnome.desktop.interface", "cursor-size"]).strip()
root = display.Display().screen().root
rg = root.get_geometry()

before = size()
ui = UInput({e.EV_KEY: [e.BTN_LEFT], e.EV_REL: [e.REL_X, e.REL_Y]}, name="mf-test-mouse")
time.sleep(0.5)
p = subprocess.Popen([BIN], env={**os.environ, "MOUSE_FINDER_DEBUG": "1"},
                     stderr=open("/tmp/mf_overlay_test.log", "w"), stdout=subprocess.DEVNULL)
try:
    time.sleep(2)
    for k in range(8):
        for _ in range(8):
            ui.write(e.EV_REL, e.REL_X, 25 if k % 2 == 0 else -25); ui.syn()
            time.sleep(0.01)
    time.sleep(0.2)
    child = root.query_pointer().child
    assert child, "pointer is not over any XWayland window during the effect"
    g, a = child.get_geometry(), child.get_attributes()
    assert a.override_redirect and g.depth == 32 and (g.width, g.height) == (rg.width, rg.height), \
        f"window under pointer is not our overlay: {g.width}x{g.height} depth {g.depth}"
    time.sleep(2)  # effect (1.5 s default) ends
    assert size() == before, f"cursor-size not restored: {size()} != {before}"
    print("OK: pointer over overlay during effect, cursor-size restored")
finally:
    p.terminate(); p.wait(); ui.close()
