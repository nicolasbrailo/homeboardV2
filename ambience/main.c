#include "config.h"
#include "display.h"
#include "slideshow.h"

#include "drm_mgr/drm_mgr.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_quit;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

static void on_display_turned_on(void *ud) { slideshow_start((struct Slideshow *)ud); }

static void on_display_turned_off(void *ud) { slideshow_stop((struct Slideshow *)ud); }

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
  struct fb_info fbi;
  struct DRM_Mgr *drm_mgr = NULL;
  uint32_t *fb = NULL;
  struct Slideshow *slideshow = NULL;
  struct Display *display = NULL;

  drm_mgr = drm_mgr_init();
  fb = drm_mgr_acquire_fb(drm_mgr, &fbi);
  if (!drm_mgr || !fb) {
    ret = 1;
    goto end;
  }

  slideshow = slideshow_init(fb, &fbi, cfg.transition_time_s, cfg.rotation);
  display = display_init(on_display_turned_on, on_display_turned_off, slideshow);
  if (!slideshow || !display) {
    ret = 1;
    goto end;
  }

  printf("Running ambience service\n");
  while (!g_quit) {
    if (display_run_dbus_loop(display) != EINTR)
      break;
  }

end:
  slideshow_free(slideshow);
  display_free(display);
  drm_mgr_release_fb(drm_mgr);
  drm_mgr_free(drm_mgr);
  return ret;
}
