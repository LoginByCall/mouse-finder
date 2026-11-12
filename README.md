# Mouse Finder for Ubuntu

macOS-style **shake to find mouse** for Ubuntu, on X11 or Wayland.

Shake the mouse left and right — the cursor temporarily enlarges to 128 px so you can spot it instantly. A system tray icon lets you adjust settings and toggle autostart.

## Features

- Shake detection via direct `/dev/input` reads (evdev, event-driven) — works under X11 and
  Wayland alike, including Ubuntu 26+ where GDM no longer offers an X11 session. Mice
  (relative motion) and touchpads (absolute motion) are both supported
- Mouse hotplug: devices plugged in later (USB, Bluetooth, after suspend) are picked up
  automatically, removed ones are released
- Cursor enlargement visible over every window, Wayland-native ones included:
  - GNOME `org.gnome.desktop.interface cursor-size` — for everything the compositor draws
    (GNOME Shell UI, window borders)
  - a transparent fullscreen XWayland overlay with the big cursor for the duration of the
    effect — for apps that draw their own cursor and would otherwise pick up the new size
    only after the pointer re-enters them. The first click during the effect closes it early
- System tray icon with click menu (left or right button)
- Settings dialog: cursor size, show duration, shake sensitivity, with a **?** help window
  explaining every parameter in 7 languages (English, Русский, 中文, Español, Français,
  Deutsch, Italiano)
- Autostart toggle (writes `~/.config/autostart/mouse-finder.desktop`)
- Config saved to `~/.config/mouse-finder.ini`

## Requirements

- Ubuntu 24.04+, X11 or Wayland session
- Read access to `/dev/input/event*` mouse nodes. The `.deb` package installs a udev rule
  that grants this automatically to the active graphical session user (no `input` group,
  no re-login). If you installed from source, see [Installation](#option-2--from-source).
- Runtime libs (pre-installed on Ubuntu Desktop): `libgtk-3-0 libxcursor1 libcairo2 libayatana-appindicator3-1`

## Installation

### Option 1 — Debian package (recommended)

Download `mouse-finder_1.2.3_amd64.deb` from the [Releases](../../releases) page:

```bash
sudo apt install ./mouse-finder_1.2.3_amd64.deb
mouse-finder &
```

### Option 2 — From source

```bash
sudo apt install build-essential libgtk-3-dev libxcursor-dev libcairo2-dev libayatana-appindicator3-dev
git clone https://github.com/LoginByCall/mouse-finder.git
cd mouse-finder
make
sudo cp pkg/usr/lib/udev/rules.d/70-mouse-finder-uaccess.rules /usr/lib/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=input
./dist/mouse-finder &
```

The udev rule step is what lets the app read `/dev/input/event*` without root; skip it
only if you already have another way to grant that access (e.g. `input` group membership).

Use the **Autostart** toggle in the tray menu to launch automatically on login.

## Usage

1. Run `mouse-finder` — a white cursor arrow icon appears in the system tray.
2. Quickly shake the mouse (or your finger on the touchpad) left and right. The cursor grows large for ~1.5 s, then returns to normal.
3. Click the tray icon:
   - **Settings…** — open settings dialog; the **?** button opens the help
   - **Autostart: ON/OFF** — toggle launch on login
   - **About…** — version and description
   - **Quit** — exit

## Settings

| Parameter | Default | Range |
|-----------|---------|-------|
| Cursor size | 128 px | 32–256 px |
| Show duration | 1500 ms | 500–3000 ms |
| Reversals to detect | 3 | 2–6 |
| Min speed | 300 | 100–1000 |

Only horizontal movement within the last 0.8 s counts; after a trigger the next one is
possible no sooner than 1 s later. Min speed is measured in the device's own units (sensor
counts for a mouse, touchpad coordinates for a touchpad), not screen pixels. If shaking does
not trigger, lower *Reversals* or *Min speed*; if it triggers by accident, raise them
(2 reversals suits a touchpad, 3 a mouse).

Settings take effect immediately without restart.

## Resource usage

| Metric | Value |
|--------|-------|
| Binary size | ~50 KB |
| Private memory | ~11 MB (almost all GTK, for the tray menu and dialogs) |
| CPU / wakeups at idle | 0% / 0 — the input thread sleeps until input or a device change (inotify) |
| Memory over time | flat (150 shake cycles: +12 KB) |

## Troubleshooting

If shaking doesn't enlarge the cursor, mouse-finder likely can't read `/dev/input`. Run it
from a terminal — it prints a diagnostic if no mouse device is accessible. Reinstalling the
`.deb` re-applies the udev rule; a manual `input`-group membership needs a re-login to take
effect.

## Tests

Regression tests drive a virtual mouse through `/dev/uinput` (needs a graphical session,
`python3-evdev`, `python3-xlib`, write access to `/dev/uinput`). Stop the running
mouse-finder first — both instances would react. They move the real cursor briefly.

```bash
make binary
python3 tests/hotplug_test.py   # detect, no busy loop after unplug, re-plugged mouse works
python3 tests/overlay_test.py   # pointer is over the overlay during the effect, cursor-size restored
```

## License

MIT — see [LICENSE](LICENSE).
