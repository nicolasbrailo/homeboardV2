#pragma once

#include <stdint.h>

enum rotation {
  ROT_0 = 0,
  ROT_90 = 90,
  ROT_180 = 180,
  ROT_270 = 270,
};

enum interpolation {
  INTERP_NEAREST,
  INTERP_BILINEAR,
};

// Render an RGB image to an XRGB8888 framebuffer.
// Scales to fit (preserving aspect ratio on the smallest axis),
// centers on screen, and applies rotation.
void img_render(uint32_t *fb, uint32_t fb_width, uint32_t fb_height, uint32_t fb_stride, const uint8_t *img_pixels,
                uint32_t img_width, uint32_t img_height, enum rotation rot, enum interpolation interp);
