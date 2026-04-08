# display-mgr

A simple DRM display management daemon for the homeboard. It owns the DRM master, manages display power state, and grants exclusive framebuffer access to a single client via a Unix domain socket.

## What it does

- Opens `/dev/dri/card0` (or `card1`), takes DRM master, and creates a dumb framebuffer
- Allocates a DMA-BUF for the framebuffer that can be shared with clients
- Accepts commands over a Unix domain socket at `$XDG_RUNTIME_DIR/display-ctrl.sock`
- Manages display on/off state, persisted to `$XDG_RUNTIME_DIR/display-ctrl.state`
- Grants exclusive framebuffer leases to one client at a time, tracked by PID
- On shutdown, restores the display to whatever power state it was in at startup

## Architecture

Single-threaded event loop: `sock_poll()` blocks for up to 1 second, accepts at most one client, reads its command, and returns. The main loop dispatches the command, closes the client socket, then checks if the lease holder is still alive.

### Source files

| File | Purpose |
|------|---------|
| `main.c` | Entry point, main loop, command dispatch |
| `drm.c` | DRM device init, framebuffer creation, display on/off via `drmModeSetCrtc` |
| `drm_lease.c` | Exclusive framebuffer lease tracking (assign/release/liveness by PID) |
| `sock.c` | Unix socket setup, accept+recv, fd passing via `SCM_RIGHTS` |
| `run_state.c` | Persists display power state to a file for external consumers |

## Socket protocol

Clients connect to `$XDG_RUNTIME_DIR/display-ctrl.sock` (a `SOCK_STREAM` Unix domain socket), send a single newline-terminated command, receive a response, and disconnect. All connections are one-shot.

### Commands

#### `on\n`
Turn the display on. Response: `ok\n`.

#### `off\n`
Turn the display off. Response: `ok\n`.

#### `status\n`
Query display power state. Response: `on\n` or `off\n`.

#### `get_drm_fd\n`
Request exclusive access to the framebuffer. The response uses a two-part wire format:

1. An `int` status code:
   - `0` = success
   - `-1` = lease already held by another client
   - `-2` = peer PID could not be identified
2. On success only, a `sendmsg` with `SCM_RIGHTS` carrying:
   - Ancillary data: the DMA-BUF file descriptor
   - Payload: a `struct fb_info` (20 bytes):
     ```c
     struct fb_info {
       uint32_t width;
       uint32_t height;
       uint32_t stride;
       uint32_t format;  // DRM_FORMAT_XRGB8888
       uint32_t size;
     };
     ```

The client can then `mmap()` the DMA-BUF fd to render directly into the framebuffer.

Only one client may hold the lease at a time. The daemon tracks the lease holder by PID (via `SO_PEERCRED`) and detects client death with `kill(pid, 0)`.

#### `close_drm_fd\n`
Release the framebuffer lease. The daemon verifies the caller's PID matches the lease holder. Response: `ok\n`.

## Build

Requires `libdrm-dev`. Cross-compiled for ARM (Raspberry Pi Zero) via `common.mk`:

```
make
```

## Permissions

The daemon needs DRM master access:

```
make set_perms   # adds user to video group, sets cap_sys_admin
```
