#!/usr/bin/env python3
"""Shake-to-find-mouse for Ubuntu 24 X11 — tray app that enlarges cursor on shake."""

import configparser
import copy
import ctypes
import threading
import time
from collections import deque
from pathlib import Path

import cairo
import gi
gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk
from Xlib import display as xdisplay

# ── libX11 / libXcursor ctypes bindings ─────────────────────────────────────
_xcursor = ctypes.CDLL("libXcursor.so.1")
_xlib    = ctypes.CDLL("libX11.so.6")

_xlib.XOpenDisplay.restype   = ctypes.c_void_p
_xlib.XOpenDisplay.argtypes  = [ctypes.c_char_p]
_xlib.XCloseDisplay.argtypes = [ctypes.c_void_p]
_xlib.XFreeCursor.argtypes   = [ctypes.c_void_p, ctypes.c_ulong]
_xlib.XDefaultRootWindow.restype  = ctypes.c_ulong
_xlib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
_xlib.XGrabPointer.restype   = ctypes.c_int
_xlib.XGrabPointer.argtypes  = [
    ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_uint,
    ctypes.c_int, ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
]
_xlib.XUngrabPointer.restype  = ctypes.c_int
_xlib.XUngrabPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
_xlib.XFlush.argtypes = [ctypes.c_void_p]

_GrabSuccess       = 0
_GrabModeAsync     = 1
_CurrentTime       = 0
_EventMask         = (1 << 6) | (1 << 2) | (1 << 3)  # Motion | ButtonPress | ButtonRelease

class _XcursorImage(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint), ("size", ctypes.c_uint),
        ("width",   ctypes.c_uint), ("height", ctypes.c_uint),
        ("xhot",    ctypes.c_uint), ("yhot",   ctypes.c_uint),
        ("delay",   ctypes.c_ulong),
        ("pixels",  ctypes.POINTER(ctypes.c_uint32)),
    ]

_xcursor.XcursorImageCreate.restype      = ctypes.POINTER(_XcursorImage)
_xcursor.XcursorImageCreate.argtypes     = [ctypes.c_int, ctypes.c_int]
_xcursor.XcursorImageLoadCursor.restype  = ctypes.c_ulong
_xcursor.XcursorImageLoadCursor.argtypes = [ctypes.c_void_p, ctypes.POINTER(_XcursorImage)]
_xcursor.XcursorImageDestroy.argtypes    = [ctypes.POINTER(_XcursorImage)]


# ── Settings ─────────────────────────────────────────────────────────────────
_CFG_PATH       = Path.home() / ".config" / "mouse-finder.ini"
_AUTOSTART_PATH = Path.home() / ".config" / "autostart" / "mouse-finder.desktop"
_SCRIPT         = Path(__file__).resolve()

class Settings:
    def __init__(self):
        self.cursor_size    = 128   # px
        self.show_duration  = 1500  # ms
        self.min_reversals  = 3
        self.min_speed      = 500   # px/s

    @classmethod
    def load(cls) -> "Settings":
        s = cls()
        cfg = configparser.ConfigParser()
        cfg.read(_CFG_PATH)
        if "mouse-finder" in cfg:
            sec = cfg["mouse-finder"]
            s.cursor_size   = int(sec.get("cursor_size",   s.cursor_size))
            s.show_duration = int(sec.get("show_duration", s.show_duration))
            s.min_reversals = int(sec.get("min_reversals", s.min_reversals))
            s.min_speed     = int(sec.get("min_speed",     s.min_speed))
        return s

    def save(self):
        cfg = configparser.ConfigParser()
        cfg["mouse-finder"] = {
            "cursor_size":   str(self.cursor_size),
            "show_duration": str(self.show_duration),
            "min_reversals": str(self.min_reversals),
            "min_speed":     str(self.min_speed),
        }
        _CFG_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(_CFG_PATH, "w") as f:
            cfg.write(f)

    def update_from(self, other: "Settings"):
        self.cursor_size   = other.cursor_size
        self.show_duration = other.show_duration
        self.min_reversals = other.min_reversals
        self.min_speed     = other.min_speed


def autostart_enabled() -> bool:
    return _AUTOSTART_PATH.exists()

def toggle_autostart():
    if autostart_enabled():
        _AUTOSTART_PATH.unlink()
    else:
        _AUTOSTART_PATH.parent.mkdir(parents=True, exist_ok=True)
        _AUTOSTART_PATH.write_text(
            f"[Desktop Entry]\nName=Mouse Finder\n"
            f"Exec=python3 {_SCRIPT}\nType=Application\n"
            f"X-GNOME-Autostart-enabled=true\n"
        )


# ── Cursor creation ───────────────────────────────────────────────────────────
def _draw_arrow_pixels(size: int) -> list[int]:
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
    ctx  = cairo.Context(surf)
    s = size / 32.0

    ctx.set_source_rgba(0, 0, 0, 0.35)
    _arrow_path(ctx, s, dx=1.5*s, dy=1.5*s)
    ctx.fill()

    ctx.set_source_rgba(1, 1, 1, 1)
    _arrow_path(ctx, s)
    ctx.set_line_width(2.5 * s)
    ctx.stroke_preserve()
    ctx.fill()

    ctx.set_source_rgba(0, 0, 0, 1)
    _arrow_path(ctx, s)
    ctx.set_line_width(0)
    ctx.fill()

    surf.flush()
    buf = bytes(surf.get_data())
    return list(ctypes.cast(ctypes.c_char_p(buf),
                ctypes.POINTER(ctypes.c_uint32 * (size * size))).contents)


def _arrow_path(ctx: cairo.Context, s: float, dx: float = 0, dy: float = 0):
    pts = [(1,1),(1,20),(5,16),(9,24),(11,23),(7.5,15),(13,15)]
    ctx.move_to((pts[0][0]+dx)*s, (pts[0][1]+dy)*s)
    for x, y in pts[1:]:
        ctx.line_to((x+dx)*s, (y+dy)*s)
    ctx.close_path()


def _make_cursor_xid(raw_dpy, size: int) -> int:
    img_ptr = _xcursor.XcursorImageCreate(size, size)
    img = img_ptr.contents
    img.xhot = max(1, size // 32)
    img.yhot = max(1, size // 32)
    pixels = _draw_arrow_pixels(size)
    for i, px in enumerate(pixels):
        img.pixels[i] = px
    xid = _xcursor.XcursorImageLoadCursor(raw_dpy, img_ptr)
    _xcursor.XcursorImageDestroy(img_ptr)
    return xid


# ── Shake detector ───────────────────────────────────────────────────────────
COOLDOWN  = 1.0   # s — between triggers, not configurable (prevents flickering)
POLL_MS   = 10
WINDOW_MS = 500

class ShakeDetector(threading.Thread):
    def __init__(self, on_shake, cfg: Settings):
        super().__init__(daemon=True)
        self._on_shake   = on_shake
        self._cfg        = cfg          # read live — reflects in-place updates
        self._samples    = deque()
        self._last_shake = 0.0
        self._stop       = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        dpy  = xdisplay.Display()
        root = dpy.screen().root
        while not self._stop.is_set():
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
        reversals   = 0
        prev_vx     = None
        prev_fast_t = None
        for i in range(1, len(samples)):
            dt = samples[i][0] - samples[i-1][0]
            if dt == 0:
                continue
            vx = (samples[i][1] - samples[i-1][1]) / dt
            if abs(vx) < self._cfg.min_speed:
                if prev_fast_t and (samples[i][0] - prev_fast_t) > 0.2:
                    prev_vx = None
                continue
            prev_fast_t = samples[i][0]
            if prev_vx is not None and (vx > 0) != (prev_vx > 0):
                reversals += 1
            prev_vx = vx
        return reversals >= self._cfg.min_reversals


# ── Settings dialog ───────────────────────────────────────────────────────────
class SettingsDialog(Gtk.Dialog):
    _PARAMS = [
        ("cursor_size",   "Размер курсора (px)",   32,  256, 32),
        ("show_duration", "Длительность (мс)",     500, 3000, 100),
        ("min_reversals", "Реверсов для тряски",   2,   6,   1),
        ("min_speed",     "Мин. скорость (px/s)",  100, 1000, 50),
    ]

    def __init__(self, cfg: Settings):
        super().__init__(title="Mouse Finder — Настройки", modal=True)
        self.set_resizable(False)
        grid = Gtk.Grid(column_spacing=12, row_spacing=8, margin=12)
        self.get_content_area().add(grid)

        self._scales = {}
        for i, (attr, label, lo, hi, step) in enumerate(self._PARAMS):
            grid.attach(Gtk.Label(label=label, xalign=0), 0, i, 1, 1)
            scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, lo, hi, step)
            scale.set_value(getattr(cfg, attr))
            scale.set_size_request(220, -1)
            scale.set_digits(0)
            grid.attach(scale, 1, i, 1, 1)
            self._scales[attr] = scale

        self.add_button("Отмена",   Gtk.ResponseType.CANCEL)
        self.add_button("Применить", Gtk.ResponseType.OK)
        self.show_all()

    def read_into(self, cfg: Settings):
        for attr, scale in self._scales.items():
            setattr(cfg, attr, int(scale.get_value()))


# ── Main application ─────────────────────────────────────────────────────────
class App:
    def __init__(self):
        self._cfg     = Settings.load()
        self._raw_dpy = _xlib.XOpenDisplay(None)
        if not self._raw_dpy:
            raise RuntimeError("XOpenDisplay failed")
        self._root_xid    = _xlib.XDefaultRootWindow(self._raw_dpy)
        self._cursor_xid  = _make_cursor_xid(self._raw_dpy, self._cfg.cursor_size)
        self._grabbed     = False

        self._tray = Gtk.StatusIcon()
        self._tray.set_from_icon_name("input-mouse")
        self._tray.set_tooltip_text("Mouse Finder — shake to find cursor")
        self._tray.connect("popup-menu", self._on_tray_menu)

        self._detector = ShakeDetector(self._on_shake, self._cfg)

    def _on_tray_menu(self, icon, button, activate_time):
        menu = Gtk.Menu()

        item_cfg = Gtk.MenuItem(label="Настройки…")
        item_cfg.connect("activate", self._open_settings)
        menu.append(item_cfg)

        label_auto = "Автостарт: ВКЛ ✓" if autostart_enabled() else "Автостарт: ВЫКЛ"
        item_auto = Gtk.MenuItem(label=label_auto)
        item_auto.connect("activate", lambda _: toggle_autostart())
        menu.append(item_auto)

        menu.append(Gtk.SeparatorMenuItem())

        item_quit = Gtk.MenuItem(label="Выход")
        item_quit.connect("activate", lambda _: Gtk.main_quit())
        menu.append(item_quit)

        menu.show_all()
        menu.popup(None, None, None, None, button, activate_time)

    def _open_settings(self, *_):
        dlg = SettingsDialog(self._cfg)
        if dlg.run() == Gtk.ResponseType.OK:
            new_cfg = copy.copy(self._cfg)
            dlg.read_into(new_cfg)
            self._apply(new_cfg)
            self._cfg.save()
        dlg.destroy()

    def _apply(self, new_cfg: Settings):
        if new_cfg.cursor_size != self._cfg.cursor_size:
            _xlib.XFreeCursor(self._raw_dpy, self._cursor_xid)
            self._cursor_xid = _make_cursor_xid(self._raw_dpy, new_cfg.cursor_size)
        self._cfg.update_from(new_cfg)  # in-place → ShakeDetector sees new values instantly

    def _on_shake(self) -> bool:
        if self._grabbed:
            return False
        status = _xlib.XGrabPointer(
            self._raw_dpy, self._root_xid,
            False, _EventMask,
            _GrabModeAsync, _GrabModeAsync,
            0, self._cursor_xid, _CurrentTime,
        )
        if status == _GrabSuccess:
            self._grabbed = True
            GLib.timeout_add(self._cfg.show_duration, self._ungrab)
        return False

    def _ungrab(self) -> bool:
        _xlib.XUngrabPointer(self._raw_dpy, _CurrentTime)
        _xlib.XFlush(self._raw_dpy)
        self._grabbed = False
        return False

    def run(self):
        self._detector.start()
        try:
            Gtk.main()
        finally:
            self._detector.stop()
            if self._grabbed:
                self._ungrab()
            _xlib.XFreeCursor(self._raw_dpy, self._cursor_xid)
            _xlib.XCloseDisplay(self._raw_dpy)


if __name__ == "__main__":
    App().run()
