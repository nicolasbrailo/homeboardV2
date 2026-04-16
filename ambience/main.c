#include "config.h"
#include "dbus.h"
#include "display.h"
#include "slideshow.h"

#include "drm_mgr/drm_mgr.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

struct ambience_ctx {
  struct Slideshow *slideshow;
  struct Display *display;
};

static void on_display_turned_on(void *ud) { slideshow_start((struct Slideshow *)ud); }

static void on_display_turned_off(void *ud) { slideshow_stop((struct Slideshow *)ud); }

static void on_slideshow_next(void *ud) { slideshow_next(((struct ambience_ctx *)ud)->slideshow); }

static void on_force_on(void *ud) { display_force_on(((struct ambience_ctx *)ud)->display); }

static void on_force_off(void *ud) { display_force_off(((struct ambience_ctx *)ud)->display); }

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
    return 1;
  }

  struct ambience_config cfg;
  if (ambience_config_load(argv[1], &cfg) < 0)
    return 1;

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  int ret = 0;
  sd_bus *bus = NULL;
  struct fb_info fbi;
  struct DRM_Mgr *drm_mgr = NULL;
  uint32_t *fb = NULL;
  struct Slideshow *slideshow = NULL;
  struct Display *display = NULL;
  struct AmbienceDbus *dbus_mgr = NULL;

  int r = sd_bus_open_system(&bus);
  if (r < 0 || !bus) {
    fprintf(stderr, "sd_bus_open_system: %s\n", strerror(-r));
    ret = 1;
    goto end;
  }

  drm_mgr = drm_mgr_init();
  fb = drm_mgr_acquire_fb(drm_mgr, &fbi);
  if (!drm_mgr || !fb) {
    ret = 1;
    goto end;
  }

  slideshow = slideshow_init(bus, fb, &fbi, cfg.transition_time_s, cfg.rotation, cfg.embed_qr,
                             cfg.use_eink_for_metadata);
  display = display_init(bus, on_display_turned_on, on_display_turned_off, slideshow);
  struct ambience_ctx ctx = {.slideshow = slideshow, .display = display};
  dbus_mgr = ambience_dbus_init(bus, on_slideshow_next, on_force_on, on_force_off, &ctx);
  if (!slideshow || !display || !dbus_mgr) {
    ret = 1;
    goto end;
  }

  printf("Running ambience service\n");
  while (!g_quit) {
    if (ambience_dbus_run_once(dbus_mgr) < 0)
      break;
  }

end:
  slideshow_free(slideshow);
  display_free(display);
  ambience_dbus_free(dbus_mgr);
  if (bus)
    sd_bus_flush_close_unref(bus);
  drm_mgr_release_fb(drm_mgr);
  drm_mgr_free(drm_mgr);
  return ret;
}
