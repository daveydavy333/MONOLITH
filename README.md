# MONOLITH

**A slot-strip Wayland compositor, the flagship desktop of [HDXI](#about-hdxi).**

Every app gets its own screen-sized place, and those places sit side by side in a row you slide
along. No workspaces to manage, no tiling to fight: one app per slot,
neighbours peeking in from the edges, and a smooth move to the next one.

> **Early software.** It is usable for testing, but expect rough edges. See
> [Known issues](#known-issues) before filing a bug.

<!-- screenshot: docs/screenshot.png -->

## Ideas

- **Slot.** The replacement for a workspace. It holds exactly one app.
- **Strip.** All slots in one row. You only ever move left or right.
- **Focused slot.** Drawn large in the middle (72% of the screen width by default). The slot on each side
  peeks in as a tilted, dimmed sliver so you always see what is next to you.
- **Windows mode.** A per-slot switch that turns a slot into a normal floating-window desktop.
  Dialogs and extra windows of an app join that app's slot and flip it into windows mode automatically,
  and it flips back when only one window is left. Dialogs never steal a slot of their own.

The full design is in [`docs/CONCEPT.md`](docs/CONCEPT.md).

## Features

- Animated slot switching with a configurable curve (`expo-out` by default, or your own Bézier)
- Tilt, scale and dim of neighbouring slots, all adjustable (or flat and plain if you prefer)
- Windows mode with Mod+drag to move and Mod+right-drag to resize
- Per-app rules (`app_id` wildcards): force windows mode, force a separate slot
- Touchpad gestures: three-finger swipe moves the strip, with tap-to-click, natural scroll and other
  libinput settings
- Optional sounds for slot switch, open and windows-mode events
- `wlr-layer-shell` support for bars, launchers and wallpapers (yambar, fuzzel, swaybg, ...)
- Hot-reloadable TOML config and a small control tool, `monolithctl`
- Works nested inside another Wayland or X11 session, which is how it is developed

Not built yet: multi-monitor, fullscreen, XWayland, tap-to-click is only in the config for real
hardware. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for what exists today.

## Building

Dependencies: a C11 compiler, `meson`, `ninja`, `wayland`, `wayland-protocols`, `libxkbcommon`,
`pixman`, `libdrm`, `libinput`, and **wlroots 0.18, 0.19 or 0.20** (picked automatically).

| wlroots | status |
|---|---|
| 0.18.2 | runtime-tested |
| 0.19.2 | runtime-tested (used for the automated headless tests) |
| 0.20 | compile-tested only; needs wayland-protocols 1.4x or newer |

On Arch:

```sh
sudo pacman -S --needed base-devel meson wayland wayland-protocols libxkbcommon pixman libdrm libinput wlroots0.18
meson setup build
ninja -C build
sudo ninja -C build install        # optional: monolith, monolithctl, sample config
```

## Default keys

| Key | Action |
|---|---|
| Mod+Left / Mod+Right | previous / next slot |
| Mod+1 ... 9 | go to slot N |
| Mod+Tab | back to the previous slot |
| Mod+Shift+Left / Right | move the slot along the strip |
| Mod+Shift+1 ... 9 | swap with slot N |
| Mod+W | toggle windows mode for the slot |
| Mod+Shift+Q | close the focused window |
| Mod+Return | run `foot` |
| Mod+D | run `fuzzel` |
| Mod+Shift+R | reload the config |
| Mod+Shift+E | quit |

Everything is rebindable in `[keybinds]`; the list of actions is in [`docs/CONFIG.md`](docs/CONFIG.md).

## Configuration

`~/.config/monolith/config.toml`. Every key is optional, and the shipped
[`data/config.toml`](data/config.toml) documents each one. Reload with `monolithctl reload` or Mod+Shift+R.

```toml
[strip]
focus_width = 0.72      # focused slot, fraction of the screen width
peek = 0.09             # visible part of each neighbour
edge_gap_top = 48       # space above and below the focused window
edge_gap_bottom = 48

[look]
tilt_degrees = 45
scale = 0.8
dim = 0.45
```

`monolith -v` prints the settings it actually loaded (lines starting with `effective:`). A config with a
syntax error makes it fall back to defaults and log `config error`, so check there first if a setting
seems ignored.

### Sounds

Put a command on an event, or do nothing: if the bundled `switch.wav`, `open.wav` and `mode.wav` are
installed in `/usr/local/share/monolith/sounds`, `/usr/share/monolith/sounds` or
`~/.local/share/monolith/sounds`, they play through `aplay`. `slot_close = []` keeps an event silent.

### Wallpaper and bar

Start them with the compositor in the config:

```toml
[[autostart]]
command = ["swaybg", "-i", "/path/to/wall.png", "-m", "fill"]
[[autostart]]
command = ["yambar"]
```

A solid `[background] color` is cheaper than a wallpaper process if you want the smoothest animation.

## monolithctl

```sh
monolithctl query           # slots, focus, mode
monolithctl slot.next       # any keybind action
monolithctl reload
```

It talks to the compositor over a Unix socket (`MONOLITH_SOCK`, printed at startup).

## Known issues

- No multi-monitor, fullscreen or XWayland yet.
- A wallpaper process such as swaybg can make animations less smooth.

## About HDXI

HDXI is an independent Linux distribution built as a personal project. MONOLITH is its flagship
environment and is developed to be usable on its own, on any wlroots-capable Linux system.

