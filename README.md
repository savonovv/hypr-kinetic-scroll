# hypr-kinetic-scroll

Hyprland compositor plugin that adds kinetic (inertial) scrolling for touchpads
at the compositor level, so momentum scrolling works consistently across apps
(not just in browsers).

Prebuilt binaries are tied to a specific Hyprland version/ABI. If you’re not on
the same version, install via hyprpm (build locally) instead.

Releases: https://github.com/savonovv/hypr-kinetic-scroll/releases

## Features

- Touchpad-only inertia (ignores real mouse wheels)
- Exponential velocity smoothing with configurable decay
- Cumulative momentum when swiping again during an active decay
- Synthetic scroll emission via Hyprland seat manager
- Configurable thresholds and frame interval

## Requirements

- Hyprland development headers
- `pkg-config` dependencies for Hyprland
- C++23-capable compiler

## Build

```bash
make
```

## Install via hyprpm (build locally)

This is the safest way to get a matching binary for your Hyprland version:

```bash
hyprpm add https://github.com/savonovv/hypr-kinetic-scroll
hyprpm update
hyprpm enable hypr-kinetic-scroll
```

## Load / Unload

Hyprland caches plugins by path. When reloading after rebuilds, unload and load
from a fresh path:

```bash
# unload (if previously loaded)
hyprctl plugin unload /tmp/hypr-kinetic-scroll-*.so

# load from a new temp path
cp hypr-kinetic-scroll.so /tmp/hypr-kinetic-scroll-$(date +%s).so
hyprctl plugin load /tmp/hypr-kinetic-scroll-$(date +%s).so
```

## Configuration

Add these to your Hyprland config (e.g. `~/.config/hypr/input.conf`):

```ini
plugin:kinetic-scroll:enabled = 1
plugin:kinetic-scroll:decel = 0.99
plugin:kinetic-scroll:min_velocity = 1.3
plugin:kinetic-scroll:interval_ms = 8
plugin:kinetic-scroll:delta_multiplier = 1.25

# Optional debug
plugin:kinetic-scroll:debug = 0
plugin:kinetic-scroll:stop_on_click = 0
plugin:kinetic-scroll:stop_on_focus = 0
```

Notes:

- `decel` is a multiplier applied each frame (lower = faster stop).
- `min_velocity` is the cutoff threshold for stopping inertia.
- `interval_ms` controls the decay frame rate (lower = smoother).
- `delta_multiplier` scales swipe impulse (higher = faster acceleration buildup).

The plugin also respects Hyprland's `input:touchpad:scroll_factor` for
synthetic events.

## Test / Verify

1) Load the plugin (or enable via hyprpm).
2) Run `hyprctl plugin list` and verify `hypr-kinetic-scroll` is listed.
3) Do a short two-finger scroll and lift. You should see momentum.

## Debug

When `plugin:kinetic-scroll:debug = 1`, the plugin writes to:

```
/tmp/hypr-kinetic-scroll.log
```

This log shows incoming axis events, timing, and state transitions.

## Notes / Troubleshooting

- Some touchpads report scrolls as `mouse` events with smooth deltas. The plugin
  treats `mouse=1` with `deltaDiscrete=0` as eligible touchpad input.
- If you see a version mismatch error when loading, rebuild against the running
  Hyprland headers, or temporarily disable strict version checks (if applicable).

## License

MIT
