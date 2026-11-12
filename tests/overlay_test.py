#!/usr/bin/env python3
"""Regression: during the effect the fullscreen XWayland overlay must be on top
of (Wayland-native) windows and receive pointer input; GNOME cursor-size must be
restored (bug 2026-11-12: cursor big only after touching a window border).
A wheel scroll (harmless if the overlay is missing) must reach the overlay.
Needs a graphical session, rw /dev/uinput, python3-evdev; stop mouse-finder first.
Usage: python3 tests/overlay_test.py [path/to/mouse-finder]
"""
import os, subprocess, sys, time
from evdev import UInput, ecodes as e

BIN = sys.argv[1] if len(sys.argv) > 1 else "dist/mouse-finder"
size = lambda: subprocess.check_output(
    ["gsettings", "get", "org.gnome.desktop.interface", "cursor-size"]).strip()
before = size()
ui = UInput({e.EV_KEY: [e.BTN_LEFT], e.EV_REL: [e.REL_X, e.REL_Y, e.REL_WHEEL]},
            name="mf-test-mouse")
time.sleep(0.5)
log = "/tmp/mf_overlay_test.log"
p = subprocess.Popen([BIN], env={**os.environ, "MOUSE_FINDER_DEBUG": "1"},
                     stderr=open(log, "w"), stdout=subprocess.DEVNULL)
try:
    time.sleep(2)
    for k in range(8):
        for _ in range(8):
            ui.write(e.EV_REL, e.REL_X, 25 if k % 2 == 0 else -25); ui.syn()
            time.sleep(0.01)
    time.sleep(0.4)
    ui.write(e.EV_REL, e.REL_WHEEL, -1); ui.syn()
    time.sleep(0.5)
    out = open(log).read()
    assert "shake detected" in out, "shake not detected"
    assert "overlay click" in out, "overlay did not receive pointer input (not on top?)"
    assert size() == before, f"cursor-size not restored: {size()} != {before}"
    print("OK: overlay on top, receives input, cursor-size restored")
finally:
    p.terminate(); p.wait(); ui.close()
