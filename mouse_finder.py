#!/usr/bin/env python3
"""Shake-to-find-mouse for Ubuntu 24 X11 — tray app that enlarges cursor on shake."""

import ctypes
import threading
import time
from collections import deque

import cairo
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk
from Xlib import X, display as xdisplay

# ── Tuning ──────────────────────────────────────────────────────────────────
CURSOR_SIZE   = 128    # px
SHOW_DURATION = 1500   # ms
COOLDOWN      = 1.0    # s between triggers
POLL_MS       = 10     # ms between position samples
WINDOW_MS     = 500    # shake detection window
MIN_REVERSALS = 4      # direction reversals to detect shake
MIN_SPEED     = 500    # px/s minimum speed during reversal

# ── libX11 / libXcursor ctypes bindings ─────────────────────────────────────
_xcursor = ctypes.CDLL("libXcursor.so.1")
_xlib    = ctypes.CDLL("libX11.so.6")

_xlib.XOpenDisplay.restype  = ctypes.c_void_p
_xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]
_xlib.XCloseDisplay.argtypes = [ctypes.c_void_p]
_xlib.XFreeCursor.argtypes   = [ctypes.c_void_p, ctypes.c_ulong]

class _XcursorImage(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint),
        ("size",    ctypes.c_uint),
        ("width",   ctypes.c_uint),
        ("height",  ctypes.c_uint),
        ("xhot",    ctypes.c_uint),
        ("yhot",    ctypes.c_uint),
        ("delay",   ctypes.c_ulong),
        ("pixels",  ctypes.POINTER(ctypes.c_uint32)),
    ]

_xcursor.XcursorImageCreate.restype  = ctypes.POINTER(_XcursorImage)
_xcursor.XcursorImageCreate.argtypes = [ctypes.c_int, ctypes.c_int]
_xcursor.XcursorImageLoadCursor.restype  = ctypes.c_ulong  # Cursor (XID)
_xcursor.XcursorImageLoadCursor.argtypes = [ctypes.c_void_p, ctypes.POINTER(_XcursorImage)]
_xcursor.XcursorImageDestroy.argtypes = [ctypes.POINTER(_XcursorImage)]


def _draw_arrow_pixels(size: int) -> list[int]:
    """Return ARGB pixel list for a cursor arrow of given size."""
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
    ctx  = cairo.Context(surf)

    s = size / 32.0  # scale factor (design at 32px)

    # Shadow
    ctx.set_source_rgba(0, 0, 0, 0.35)
    _arrow_path(ctx, s, dx=1.5*s, dy=1.5*s)
    ctx.fill()

    # White outline
    ctx.set_source_rgba(1, 1, 1, 1)
    _arrow_path(ctx, s)
    ctx.set_line_width(2.5 * s)
    ctx.stroke_preserve()
    ctx.fill()

    # Black fill
    ctx.set_source_rgba(0, 0, 0, 1)
    _arrow_path(ctx, s)
    ctx.set_line_width(0)
    ctx.fill()

    surf.flush()
    buf = bytes(surf.get_data())
    # cairo ARGB32 is 4 bytes per pixel, native-endian — same layout XcursorImage expects
    return list(ctypes.cast(ctypes.c_char_p(buf), ctypes.POINTER(ctypes.c_uint32 * (size * size))).contents)


def _arrow_path(ctx: cairo.Context, s: float, dx: float = 0, dy: float = 0) -> None:
    """Classic arrow cursor outline, scaled by s."""
    pts = [
        (1, 1), (1, 20), (5, 16), (9, 24),
        (11, 23), (7.5, 15), (13, 15),
    ]
    ctx.move_to((pts[0][0] + dx) * s, (pts[0][1] + dy) * s)
    for x, y in pts[1:]:
        ctx.line_to((x + dx) * s, (y + dy) * s)
    ctx.close_path()


def make_big_cursor() -> tuple[int, ctypes.c_void_p]:
    """Create an enlarged cursor XID using libXcursor.
    Returns (cursor_xid, raw_dpy_ptr) — caller owns raw_dpy_ptr."""
    size = CURSOR_SIZE
    img_ptr = _xcursor.XcursorImageCreate(size, size)
    img = img_ptr.contents
    img.xhot = int(size * 1 / 32)  # hotspot matches arrow tip
    img.yhot = int(size * 1 / 32)

    pixels = _draw_arrow_pixels(size)
    for i, px in enumerate(pixels):
        img.pixels[i] = px

    raw_dpy = _xlib.XOpenDisplay(None)
    if not raw_dpy:
        raise RuntimeError("XOpenDisplay failed")
    cursor_xid = _xcursor.XcursorImageLoadCursor(raw_dpy, img_ptr)
    _xcursor.XcursorImageDestroy(img_ptr)
    return cursor_xid, raw_dpy


# ── Shake detector ───────────────────────────────────────────────────────────
class ShakeDetector(threading.Thread):
    def __init__(self, on_shake):
        super().__init__(daemon=True)
        self._on_shake    = on_shake
        self._samples     = deque()   # (t, x, y)
        self._last_shake  = 0.0
        self._stop_event  = threading.Event()

    def stop(self):
        self._stop_event.set()

    def run(self):
        dpy  = xdisplay.Display()
        root = dpy.screen().root
        while not self._stop_event.is_set():
            data = root.query_pointer()
            now  = time.monotonic()
            self._samples.append((now, data.root_x, data.root_y))
            self._prune(now)
            if self._detect(now):
                self._last_shake = now
                GLib.idle_add(self._on_shake)
            time.sleep(POLL_MS / 1000)
        dpy.close()

    def _prune(self, now: float):
        cutoff = now - WINDOW_MS / 1000
        while self._samples and self._samples[0][0] < cutoff:
            self._samples.popleft()

    def _detect(self, now: float) -> bool:
        if now - self._last_shake < COOLDOWN:
            return False
        samples = list(self._samples)
        if len(samples) < 6:
            return False

        reversals = 0
        prev_vx   = None
        for i in range(1, len(samples)):
            dt = samples[i][0] - samples[i-1][0]
            if dt == 0:
                continue
            vx = (samples[i][1] - samples[i-1][1]) / dt
            if abs(vx) < MIN_SPEED:
                prev_vx = None
                continue
            if prev_vx is not None and (vx > 0) != (prev_vx > 0):
                reversals += 1
            prev_vx = vx

        return reversals >= MIN_REVERSALS


# ── Main application ─────────────────────────────────────────────────────────
class App:
    def __init__(self):
        self._dpy     = xdisplay.Display()
        self._root    = self._dpy.screen().root
        cursor_xid, self._raw_dpy = make_big_cursor()
        self._cursor = self._dpy.create_resource_object("cursor", cursor_xid)
        self._grabbed = False

        self._tray = Gtk.StatusIcon()
        self._tray.set_from_icon_name("input-mouse")
        self._tray.set_tooltip_text("Mouse Finder — shake to find cursor")
        self._tray.connect("popup-menu", self._on_tray_menu)

        self._detector = ShakeDetector(self._on_shake)

    def _on_tray_menu(self, icon, button, activate_time):
        menu = Gtk.Menu()
        quit_item = Gtk.MenuItem(label="Выход")
        quit_item.connect("activate", lambda _: Gtk.main_quit())
        menu.append(quit_item)
        menu.show_all()
        menu.popup(None, None, None, None, button, activate_time)

    def _on_shake(self) -> bool:  # called from GTK main loop
        if self._grabbed:
            return False
        status = self._root.grab_pointer(
            False,
            X.PointerMotionMask | X.ButtonPressMask | X.ButtonReleaseMask,
            X.GrabModeAsync, X.GrabModeAsync,
            X.NONE,
            self._cursor,
            X.CurrentTime,
        )
        if status == X.GrabSuccess:
            self._grabbed = True
            GLib.timeout_add(SHOW_DURATION, self._ungrab)
        return False  # don't repeat idle_add

    def _ungrab(self) -> bool:
        self._dpy.ungrab_pointer(X.CurrentTime)
        self._dpy.flush()
        self._grabbed = False
        return False  # don't repeat timeout

    def run(self):
        self._detector.start()
        try:
            Gtk.main()
        finally:
            self._detector.stop()
            if self._grabbed:
                self._ungrab()
            _xlib.XFreeCursor(self._raw_dpy, self._cursor.id)
            _xlib.XCloseDisplay(self._raw_dpy)
            self._dpy.close()


if __name__ == "__main__":
    App().run()
