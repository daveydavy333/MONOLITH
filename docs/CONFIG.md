# MONOLITH — config reference

File: `$XDG_CONFIG_HOME/monolith/config.toml` (or `~/.config/monolith/config.toml`),
falling back to `/etc/monolith/config.toml`, then built-in defaults. Override with
`monolith -c path`. A broken file never stops startup (defaults are used and the error is
logged); a broken file on reload keeps the running settings and reports the error.

Reload: `monolithctl reload`, or the `config.reload` keybind. Cursor and keyboard
settings only apply at startup/keyboard detection.

An annotated example lives in `data/config.toml` (installed to `share/monolith/config.toml`).

## Keys

Animation and look need the "picture" path (see ARCHITECTURE.md): if `look` is neutral
(`tilt_degrees = 0`, `scale = 1.0`, `dim = 0`) neighbours are plain live windows and slots switch with a hard cut.

| Key | Default | Notes |
|---|---|---|
| `general.mod` | `"Alt"` | What `Mod` means in keybinds: `Alt`, `Super`, `Ctrl`. Alt so it works nested; use Super on real hardware. |
| `general.dbus_env` | `"auto"` | Hand `WAYLAND_DISPLAY`, `XDG_CURRENT_DESKTOP=monolith` etc. to D-Bus-activated services and the systemd user manager (`dbus-update-activation-environment --systemd`), which portals (file dialogs, screen capture) need. `"auto"`: only when MONOLITH is the real session, not when nested in another desktop (it would hijack that desktop's environment); `"always"`; `"never"`. |
| `general.reduced_motion` | `false` | Master switch: `true` turns every animation off (slots switch instantly). The static look of the neighbours is controlled by `[look]`. |
| `animation.enabled` | `true` | Animate slot switches. |
| `animation.duration_ms` | `200` | Length of a switch (0–3000). 0 = hard cut. |
| `animation.easing` | `"expo-out"` | Timing curve; every one starts at full speed and slows down. `"expo-out"` jumps then glides (XMB-like), `"quint-out"`, `"quart-out"`, `"cubic-out"`, `"quad-out"` (weaker and weaker), `"sine-out"` (gentle), `"linear"`. |
| `animation.bezier` | unset | `[x1, y1, x2, y2]` like CSS `cubic-bezier()`; overrides `easing`. x values must be 0–1. Example: `[0.05, 0.8, 0.15, 1.0]`. |
| `animation.jump_start` | `0.0` | Share of the move taken instantly at the start (0–0.9), the rest follows the curve. |
| `look.tilt_degrees` | `45` | Turn of the neighbours (−80…80). Positive: outer edge toward you (as in the concept sketch); negative: the other way; 0: no tilt. |
| `look.scale` | `0.8` | Neighbour size relative to the focused slot (0.3–1.0). |
| `look.dim` | `0.45` | 0 = unchanged, up to 0.9 = nearly black. |
| `look.perspective` | `1.6` | Camera distance in screen widths (0.8–6): smaller = stronger perspective. |
| `strip.focus_width` | `0.72` | Focused slot width, fraction of output width (0.3–1.0). |
| `strip.peek` | `0.09` | How far in from the screen edge each neighbour's inner edge sits (fraction of the width, 0–0.3): the visible slice of it. |
| `strip.gap` | `24` | Minimum px between the focused slot and a neighbour. |
| `strip.edge_gap_top`, `strip.edge_gap_bottom` | `-1` | Top/bottom gap on its own; `-1` = use `strip.edge_gap`. Left/right spacing comes from `focus_width`. |
| `strip.edge_gap` | `0` | Px of space between the focused window and the screen edges on all four sides (added to `strip.margin.*`). |
| `strip.margin.top/bottom/left/right` | `0` | Space reserved around the focused slot (px). Bars with exclusive zones add to this. |
| `strip.new_slot_end` | `"right"` | New slots open at the `"right"` or `"left"` end of the strip. |
| `strip.wrap_around` | `false` | `slot.next` on the last slot wraps to the first (and vice versa). |
| `strip.close_focus` | `"left"` | Focus after closing a slot: `"left"`, `"right"`, `"recent"`. |
| `windows_mode.popups` | `"auto"` | `"auto"` = windows with a parent join the parent's slot and flip it to windows mode; `"off"` disables. |
| `windows_mode.same_client` | `"join"` | `"join"`: any other window from an app that already has a slot (extra windows, dialogs that declare no parent) joins that app's slot and flips it to windows mode. `"new"`: every window gets its own slot. |
| `windows_mode.revert_when_single` | `true` | Auto-flipped slots leave windows mode when one window remains. |
| `windows_mode.default_size` | `[0.6, 0.6]` | New floating window size, fraction of slot size. |
| `windows_mode.margin` | `16` | Offset of the first floating window from the slot edge; later ones cascade by 32 px. |
| `sound.enabled` | `true` | Master switch for sound events. |
| `sound.cooldown_ms` | `60` | Ignore an event arriving sooner than this after the last sound. |
| `sound.overlap` | `"restart"` | Previous sound still playing: `"allow"`, `"restart"` (kill it), `"drop"` (skip new one). |
| `sound.events.slot_switch` / `slot_open` / `slot_close` / `windows_mode` | unset (silent) | Argument list run for the event, e.g. `["aplay","-q","/path/switch.wav"]`. No shell. `[]` or unset = silent. |
| `background.color` | `"#000000"` | Solid colour (`#rrggbb` / `#rrggbbaa`). For an image, autostart `swaybg`. |
| `cursor.theme` / `cursor.size` | `""` / `24` | |
| `keyboard.layout` / `variant` / `options` | `""` | XKB names. |
| `keyboard.repeat_rate` / `repeat_delay` | `30` / `300` | |

## Touchpad, mouse and gestures

Applied through libinput to real devices only (not when MONOLITH runs nested inside another desktop).
A missing key keeps the device default. Section `[input.touchpad]` is used for devices that can tap
(touchpads), `[input.mouse]` for the rest.

| Key | Default | Notes |
|---|---|---|
| `tap` | `true` (touchpad) | Tap to click. |
| `tap_and_drag`, `drag_lock` | unset | Tap-and-drag, and keeping the drag after lifting the finger. |
| `natural_scroll` | unset | Content follows the fingers. |
| `disable_while_typing` | `true` (touchpad) | |
| `accel_speed` | unset | −1.0 … 1.0. |
| `accel_profile` | unset | `"adaptive"` or `"flat"`. |
| `click_method` | unset | `"clickfinger"` (1/2/3-finger click = left/right/middle) or `"button_areas"`. |
| `scroll_method` | unset | `"two_finger"`, `"edge"`, `"on_button_down"`. |
| `left_handed`, `middle_emulation` | unset | |

`[gestures]`: a multi-finger swipe moves the strip with your fingers and settles on a slot when you lift them.

| Key | Default | Notes |
|---|---|---|
| `swipe_fingers` | `3` | Number of fingers; `0` turns the gesture off. Other finger counts are not passed to apps yet. |
| `swipe_distance` | `0.6` | Fraction of the screen width that moves the strip by one slot. |
| `swipe_commit` | `0.2` | How much of a slot you must drag to switch; less snaps back. |
| `swipe_natural` | `true` | Strip follows the fingers; `false` reverses it. |

## Keybinds

```toml
[keybinds]
"Mod+Right" = "slot.next"
"Mod+Shift+1" = "slot.swap 1"
```

Format `"Mods+Key" = "action"`. Modifiers: `Mod`, `Alt`, `Super`/`Logo`, `Ctrl`, `Shift`. Key is any XKB keysym
name (`Right`, `Return`, `Tab`, `w`, `1`…), matched case-insensitively. Your entries are added on
top of the built-in defaults and win on a clash.

Built-in defaults: `Mod+Right/Left` next/prev, `Mod+Tab` recent, `Mod+1..9` goto,
`Mod+Shift+1..9` swap, `Mod+Shift+Right/Left` move, `Mod+W` windows mode,
`Mod+Shift+Q` close window, `Mod+Shift+R` reload, `Mod+Shift+E` quit.

### Actions

| Action | Effect |
|---|---|
| `slot.next` / `slot.prev` | Move focus along the strip (wraps only if `wrap_around`). |
| `slot.goto N` | Focus slot N (1-based). |
| `slot.recent` | Back to the previously focused slot. |
| `slot.swap N` | Swap the focused slot with slot N; N past the end moves it to the end. |
| `slot.move-left` / `slot.move-right` | Swap with the direct neighbour. |
| `windows-mode.toggle` | Flip the focused slot between strip mode and windows mode. |
| `window.close` | Ask the focused window to close. |
| `exec COMMAND` | Run COMMAND through `/bin/sh -c`. |
| `config.reload` | Reload the config file. |
| `quit` | Exit the compositor. |

## Moving and resizing (windows mode)

`Mod` + left-drag moves a window, `Mod` + right-drag resizes it from the nearest corner (minimum
160×100). Apps that draw their own title bar can also start a move/resize themselves. Strip mode
has no dragging (the window fills its slot), so the drag is passed to the app unchanged there.

## Rules and autostart

```toml
[[rules]]
app_id = "pavucontrol"     # shell-style wildcard on the app_id
windows_mode = true        # its slot opens in windows mode
slot = "new"               # (optional) never join another slot, always open a new one

[[autostart]]
command = ["swaybg", "-i", "/home/you/wall.png", "-m", "fill"]
```

## monolithctl

`monolithctl <action> [args]` sends any action above to the running compositor, plus `reload` and:

* `query` — one line per slot: index, `*` if focused, app_id, `[windows]` if in windows mode.

Socket: `$MONOLITH_SOCK` (set for everything MONOLITH starts; also printed at startup), else
`$XDG_RUNTIME_DIR/monolith-$WAYLAND_DISPLAY.sock`. Exit status is 1 on an `error: …` reply.

## Reserved (not implemented yet)

`[[output]]`,
sound `{volume}` placeholder. The sample config does not use them.
