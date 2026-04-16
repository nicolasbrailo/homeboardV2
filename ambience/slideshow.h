#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-bus.h>

#include "drm_mgr/drm_mgr.h"

struct Slideshow;

// Creates a slideshow that renders into `fb` (borrowed; owned by the caller).
// `bus` is borrowed; owned by the caller and must outlive the slideshow.
// Initially stopped.
struct Slideshow *slideshow_init(sd_bus *bus, uint32_t *fb, const struct fb_info *fbi, uint32_t transition_time_s,
                                 uint32_t rotation_deg, bool embed_qr);

// Free the slideshow. Stops the worker if running.
void slideshow_free(struct Slideshow *s);

// Start / stop the worker. Safe to call repeatedly. Calling start when
// already running (or stop when already stopped) is a no-op.
void slideshow_start(struct Slideshow *s);
void slideshow_stop(struct Slideshow *s);

// Advance to the next picture immediately, cutting short the current wait.
// No-op when the worker is stopped.
void slideshow_next(struct Slideshow *s);
