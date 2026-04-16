# ambience

A C service that renders a slideshow of images onto a display using DRM/KMS on
a Raspberry Pi. It reacts to room occupancy (starting/stopping the slideshow
as people come and go), fetches photos from a sibling service, and exposes a
D-Bus interface for manual control.

## What it does

- Acquires a DRM framebuffer via `display-mgr` and draws JPEGs directly into it.
- Subscribes to occupancy signals from `occupancy-sensor`; when the room is
  occupied it calls `display-mgr` to turn the panel on and starts the
  slideshow; when vacant it does the inverse.
- Runs a background worker that pulls photos from `photo-provider` and renders
  them at the configured interval with optional rotation and QR-code overlay.
- Exposes `io.homeboard.Ambience1.Next()` so external triggers (buttons,
  automations) can skip to the next picture immediately.

## Dependencies

Ambience is the presentation layer and relies on three other services:

| Service | Role |
|---|---|
| `display-mgr` | Owns the DRM device; ambience gets its framebuffer through it and calls `io.homeboard.Display1.On/Off` to control panel power. |
| `photo-provider` | Caches and serves JPEGs; ambience calls `io.homeboard.PhotoProvider1.GetPhoto` for each transition, plus `SetTargetSize` / `SetEmbedQr` at startup. |
| `occupancy-sensor` | Emits `io.homeboard.Occupancy1.StateChanged`; ambience uses it to decide when to run. |

All three must be reachable on the **system** bus.

## Features

- DRM/KMS rendering via the shared `drm_mgr` library
- JPEG decode and scale (bilinear) to the framebuffer, with 0/90/180/270°
  rotation
- Occupancy-gated lifecycle: slideshow only runs when someone is in the room
- Worker can be interrupted mid-wait for immediate advance (`Next`)
- Single sd_bus event loop shared between all D-Bus consumers

## Build & deploy

Cross-compiled for ARMv6 using `rpiz-xcompile`:

```sh
make xcompile-start         # mount sysroot
make                        # build/ambience
make deploy                 # scp binary to DEPLOY_TGT_HOST
make deploy-config          # scp config.json
make deploy-dbus-policy     # install D-Bus policy (required, one-time)
make next                   # trigger Ambience.Next() over SSH (for testing)
```

## Config

`config.json`:

```json
{
  "transition_time_s": 30,
  "rotation": 0,
  "embed_qr": false
}
```

- `transition_time_s` — seconds between pictures. Clamped ≥1, warning outside
  `(3, 300]`.
- `rotation` — one of `0`, `90`, `180`, `270`. Axes swap for 90/270 when
  requesting target size from `photo-provider` so the server renders at the
  correct aspect ratio.
- `embed_qr` — if true, `photo-provider` overlays a QR code on each image.

## D-Bus interface

| | |
|---|---|
| Bus | system |
| Service | `io.homeboard.Ambience` |
| Object | `/io/homeboard/Ambience` |
| Interface | `io.homeboard.Ambience1` |
| Method | `Next()` — advance to the next picture immediately |

Shell invocation:

```sh
busctl --system call io.homeboard.Ambience /io/homeboard/Ambience io.homeboard.Ambience1 Next
```

The policy file `io.homeboard.Ambience.conf` grants `own` to the running user
and send/receive access to everyone else. Without it,
`sd_bus_request_name` returns `EACCES`.

## Layer map

```
main.c             thin: config, lifecycle, wires modules, runs the sd_bus loop
config.{c,h}       json-c config parsing
dbus.{c,h}         owns the shared sd_bus; hosts io.homeboard.Ambience1 (Next)
display.{c,h}      borrows the sd_bus; subscribes to Occupancy1.StateChanged,
                   calls Display1.On/Off, invokes caller callbacks on toggle
slideshow.{c,h}    worker thread: fetches from PhotoProvider, decodes, renders.
                   Holds its own sd_bus connection (calls into PhotoProvider
                   are synchronous; a dedicated connection keeps them off the
                   dispatch bus).
../lib/drm_mgr/              framebuffer acquisition via display-mgr
../lib/jpeg_render/          libjpeg-based decode + scaler
```

### Threading

- Main thread runs a single `sd_bus_process` / `sd_bus_wait` loop via
  `ambience_dbus_run_once`. Both `dbus.c` (server) and `display.c` (client
  matches + method calls) share that bus. `Next()` is dispatched on this
  thread.
- Slideshow worker thread: fetches one photo (`GetPhoto`), decodes, renders,
  then waits on `wake_sem` up to `transition_time_s`. `slideshow_stop()` and
  `slideshow_next()` both post the sem; `stop_requested` (plain bool,
  synchronised by the sem) disambiguates. The worker has its own sd_bus
  connection so a slow `GetPhoto` doesn't starve the dispatch loop.

### Startup ordering

`dbus` must be created before `display`: `display_init` takes a borrowed
`sd_bus *` from `ambience_dbus_get_bus(dbus)`. Teardown is the reverse —
`display_free` unrefs its slots on the shared bus before `ambience_dbus_free`
closes it.

## Build gotchas

- `json-c` is statically linked; `libsystemd` and `libjpeg` are dynamic.
- The Makefile's `build/%.o` rule uses `$(notdir ...)`, so source files with
  the same basename across directories would collide.
- Cross-compile only: targets `arm-linux-gnueabihf`.
