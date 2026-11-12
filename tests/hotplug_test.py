#!/usr/bin/env python3
"""Regression: unplugged mouse must not spin the record thread, re-plugged mouse
must be picked up (bug 2026-11-12: POLLHUP busy loop at ~93% CPU, no rescan).

Needs a graphical session, rw /dev/uinput, python3-evdev. Stop the running
mouse-finder first (two instances both react to the shake).
Usage: python3 tests/hotplug_test.py [path/to/mouse-finder]
"""
import os, subprocess, sys, time
from evdev import UInput, ecodes as e

BIN = sys.argv[1] if len(sys.argv) > 1 else "dist/mouse-finder"
CAPS = {e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT], e.EV_REL: [e.REL_X, e.REL_Y]}


def shake(ui):
    for stroke in range(8):
        d = 25 if stroke % 2 == 0 else -25
        for _ in range(8):
            ui.write(e.EV_REL, e.REL_X, d)
            ui.syn()
            time.sleep(0.01)


def cpu_s(pid):
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return (int(f[11]) + int(f[12])) / os.sysconf("SC_CLK_TCK")


def shakes(log):
    return open(log).read().count("shake detected")


log = "/tmp/mf_hotplug_test.log"
ui = UInput(CAPS, name="mf-test-mouse")  # present before start: found by initial scan
time.sleep(0.5)
p = subprocess.Popen([BIN], env={**os.environ, "MOUSE_FINDER_DEBUG": "1"},
                     stderr=open(log, "w"), stdout=subprocess.DEVNULL)
try:
    time.sleep(2)
    shake(ui); time.sleep(0.5)
    assert shakes(log) == 1, "initial device: shake not detected"

    ui.close()  # unplug
    time.sleep(1)
    c0 = cpu_s(p.pid); time.sleep(3); c1 = cpu_s(p.pid)
    assert c1 - c0 < 0.3, f"busy loop after unplug: {c1 - c0:.2f} s CPU in 3 s"

    ui = UInput(CAPS, name="mf-test-mouse")  # re-plug
    time.sleep(3)  # inotify picks it up at once; margin for udev ACL
    time.sleep(1.2)  # cooldown
    shake(ui); time.sleep(0.5)
    assert shakes(log) == 2, "re-plugged device: shake not detected"
    print(f"OK: detect, no spin after unplug ({c1 - c0:.2f} s CPU / 3 s), hotplug")
finally:
    p.terminate(); p.wait()
    ui.close()
