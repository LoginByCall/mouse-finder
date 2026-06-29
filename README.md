# Mouse Finder for Ubuntu

macOS-style **shake to find mouse** for Ubuntu 24 on X11.

Shake the mouse left and right — the cursor temporarily enlarges to 128 px so you can spot it instantly. A system tray icon lets you adjust settings and toggle autostart.

## Features

- Shake detection via sliding-window velocity analysis (no dependencies beyond Python stdlib + GTK)
- Cursor enlargement using `libXcursor` + `XGrabPointer` — works over any window
- System tray icon with right-click menu
- Settings dialog: cursor size, show duration, shake sensitivity
- Autostart toggle (writes `~/.config/autostart/mouse-finder.desktop`)
- Config saved to `~/.config/mouse-finder.ini`

## Requirements

- Ubuntu 24.04 (X11 session — not Wayland)
- Python 3.10+

System packages (pre-installed on Ubuntu Desktop):

```
python3-gi  python3-cairo  python3-xlib
libxcursor1  gir1.2-gtk-3.0
```

If any are missing:

```bash
sudo apt install python3-gi python3-cairo python3-xlib libxcursor1
```

## Installation

### Option 1 — Debian package (recommended)

Download `mouse-finder_1.0.0_all.deb` from the [Releases](../../releases) page, then:

```bash
sudo apt install ./mouse-finder_1.0.0_all.deb
mouse-finder &
```

`apt` will automatically install all required dependencies.

### Option 2 — Standalone binary

Download `mouse-finder` from the [Releases](../../releases) page. No Python or GTK required:

```bash
chmod +x mouse-finder
./mouse-finder &
```

### Option 3 — From source

```bash
git clone https://github.com/your-username/mouse-finder.git
cd mouse-finder
sudo apt install python3-gi python3-cairo python3-xlib libxcursor1
python3 mouse_finder.py &
```

Use the **Autostart** toggle in the tray menu to launch automatically on login.

## Usage

1. Run `python3 mouse_finder.py` — a mouse icon appears in the system tray.
2. Quickly shake the mouse left and right (3–4 times). The cursor grows large for ~1.5 s, then returns to normal.
3. Right-click the tray icon for the menu:
   - **Settings…** — open the settings dialog
   - **Autostart: OFF/ON** — toggle launch on login
   - **Quit**

## Settings

| Parameter | Default | Description |
|-----------|---------|-------------|
| Cursor size | 128 px | Enlarged cursor size (32–256 px) |
| Show duration | 1500 ms | How long the big cursor stays visible |
| Reversals to detect | 3 | Direction changes required to trigger (2–6) |
| Min speed | 500 px/s | Minimum shake speed threshold |

Settings are saved to `~/.config/mouse-finder.ini` and take effect immediately without restart.

## Wayland

Wayland does not support `XGrabPointer`. Run your session in **X11 mode** (choose "Ubuntu on Xorg" on the login screen).

## License

MIT — see [LICENSE](LICENSE).
