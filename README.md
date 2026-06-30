# Mouse Finder for Ubuntu

macOS-style **shake to find mouse** for Ubuntu 24 on X11.

Shake the mouse left and right — the cursor temporarily enlarges to 128 px so you can spot it instantly. A system tray icon lets you adjust settings and toggle autostart.

## Features

- Shake detection via XRecord (event-driven, ~0.1% CPU at idle)
- Cursor enlargement via a fullscreen InputOnly overlay — works over any window without stealing input
- System tray icon with click menu (left or right button)
- Settings dialog: cursor size, show duration, shake sensitivity
- Autostart toggle (writes `~/.config/autostart/mouse-finder.desktop`)
- Config saved to `~/.config/mouse-finder.ini`

## Requirements

- Ubuntu 24.04 with an **X11 session** (not Wayland — choose "Ubuntu on Xorg" at login)
- Runtime libs (pre-installed on Ubuntu Desktop): `libgtk-3-0 libxcursor1 libxtst6 libcairo2 libayatana-appindicator3-1`

## Installation

### Option 1 — Debian package (recommended)

Download `mouse-finder_1.0.0_amd64.deb` from the [Releases](../../releases) page:

```bash
sudo apt install ./mouse-finder_1.0.0_amd64.deb
mouse-finder &
```

### Option 2 — From source

```bash
sudo apt install build-essential libgtk-3-dev libxcursor-dev libxtst-dev libcairo2-dev libayatana-appindicator3-dev
git clone https://github.com/2246636/mouse-finder.git
cd mouse-finder
make
./dist/mouse-finder &
```

Use the **Autostart** toggle in the tray menu to launch automatically on login.

## Usage

1. Run `mouse-finder` — a white cursor arrow icon appears in the system tray.
2. Quickly shake the mouse left and right (3–4 reversals). The cursor grows large for ~1.5 s, then returns to normal.
3. Click the tray icon:
   - **Настройки…** — settings dialog
   - **Автостарт: ВКЛ/ВЫКЛ** — toggle launch on login
   - **О программе…** — version and description
   - **Выход** — quit

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
| Binary size | ~31 KB |
| RSS at idle | ~15 MB |
| CPU at idle | ~0.1% (XRecord, event-driven) |

## Wayland

Wayland does not expose a global pointer event stream, so XRecord-based shake detection is not available. Use an X11 session ("Ubuntu on Xorg" at the login screen).

## License

MIT — see [LICENSE](LICENSE).
