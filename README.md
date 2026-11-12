# Mouse Finder for Ubuntu

macOS-style **shake to find mouse** for Ubuntu, on X11 or Wayland.

Shake the mouse left and right — the cursor temporarily enlarges to 128 px so you can spot it instantly. A system tray icon lets you adjust settings and toggle autostart.

## Features

- Shake detection via direct `/dev/input` reads (evdev, event-driven, ~0.1% CPU at idle) — works under X11 and Wayland alike, including Ubuntu 26+ where GDM no longer offers an X11 session. Mice (relative motion) and touchpads (absolute motion) both supported
- Cursor enlargement on GNOME via the `org.gnome.desktop.interface cursor-size` setting — the compositor redraws the pointer, so it works on Wayland. Non-GNOME X11 falls back to a fullscreen InputOnly overlay
- System tray icon with click menu (left or right button)
- Settings dialog: cursor size, show duration, shake sensitivity
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

Download `mouse-finder_1.2.0_amd64.deb` from the [Releases](../../releases) page:

```bash
sudo apt install ./mouse-finder_1.2.0_amd64.deb
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
2. Quickly shake the mouse left and right (3–4 reversals). The cursor grows large for ~1.5 s, then returns to normal.
3. Click the tray icon:
   - **Settings…** — open settings dialog
   - **Autostart: ON/OFF** — toggle launch on login
   - **About…** — version and description
   - **Quit** — exit

## Settings

| Parameter | Default | Range |
|-----------|---------|-------|
| Cursor size | 128 px | 32–256 px |
| Show duration | 1500 ms | 500–3000 ms |
| Reversals to detect | 3 | 2–6 |
| Min speed | 500 px/s | 100–1000 px/s |

Settings take effect immediately without restart.

## Resource usage

| Metric | Value |
|--------|-------|
| Binary size | ~35 KB |
| RSS at idle | ~15 MB |
| CPU at idle | ~0.1% (evdev, event-driven) |

## Troubleshooting

If shaking doesn't enlarge the cursor, mouse-finder likely can't read `/dev/input`. Run it
from a terminal — it prints a diagnostic if no mouse device is accessible. Reinstalling the
`.deb` re-applies the udev rule; a manual `input`-group membership needs a re-login to take
effect.

## License

MIT — see [LICENSE](LICENSE).
