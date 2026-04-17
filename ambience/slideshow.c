#define _GNU_SOURCE
#include "slideshow.h"
#include "dbus_helpers.h"
#include "eink_meta.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "jpeg_render/img_render.h"
#include "jpeg_render/jpeg_loader.h"

#define DBUS_PHOTO_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_PHOTO_PATH "/io/homeboard/PhotoProvider"
#define DBUS_PHOTO_INTERFACE "io.homeboard.PhotoProvider1"

// Lifecycle: start/stop/next are only ever called from the main (dbus
// dispatch) thread, so worker_running needs no synchronization. The worker
// waits on wake_sem: stop() and next() both post it to cut the wait short.
// stop_requested disambiguates: set before a stop post, clear otherwise.
// sem_post is durable — if it fires while the worker is mid-fetch, the next
// sem_timedwait returns immediately.
struct Slideshow {
  uint32_t *fb;
  struct fb_info fbi;
  uint32_t transition_time_s;
  enum rotation rotation;
  uint32_t target_w;
  uint32_t target_h;
  bool embed_qr;

  // `bus` is shared with the main dispatch loop; only touched on the main
  // thread (monitor setup, push_initial_config, teardown). `worker_bus` is
  // private to the worker thread and used for blocking GetPhoto calls, so the
  // shared bus stays free of cross-thread access.
  sd_bus *bus;
  sd_bus *worker_bus;
  sd_bus_slot *photo_svc_monitor;
  pthread_t worker;
  bool worker_running;
  sem_t wake_sem;
  bool stop_requested;

  struct EinkMeta *eink_meta;
};

// (Re)open the worker's private system-bus connection. Closes any existing
// connection first so callers can use this both for initial connect and for
// recovery after the bus drops (e.g. dbus-daemon restart → ENOTCONN).
static int connect_worker_bus(struct Slideshow *s) {
  if (s->worker_bus) {
    sd_bus_flush_close_unref(s->worker_bus);
    s->worker_bus = NULL;
  }
  int r = sd_bus_open_system(&s->worker_bus);
  if (r < 0) {
    fprintf(stderr, "slideshow: sd_bus_open_system: %s\n", strerror(-r));
    return -1;
  }
  return 0;
}

static int fetch_one(struct Slideshow *s, int *fd_out, char **meta_out) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  int r = sd_bus_call_method(s->worker_bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, "GetPhoto",
                             &err, &reply, "");
  if (r == -ENOTCONN) {
    sd_bus_error_free(&err);
    err = SD_BUS_ERROR_NULL;
    fprintf(stderr, "GetPhoto: worker bus disconnected, reconnecting\n");
    if (connect_worker_bus(s) < 0)
      return -1;
    r = sd_bus_call_method(s->worker_bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, "GetPhoto", &err,
                           &reply, "");
  }
  if (r < 0) {
    fprintf(stderr, "GetPhoto failed: %s\n", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }

  int fd = -1;
  const char *meta = NULL;
  r = sd_bus_message_read(reply, "hs", &fd, &meta);
  if (r < 0) {
    fprintf(stderr, "bad GetPhoto reply: %s\n", strerror(-r));
    sd_bus_message_unref(reply);
    return -1;
  }

  int dup_fd = dup(fd);
  if (dup_fd < 0) {
    perror("dup");
    sd_bus_message_unref(reply);
    return -1;
  }

  *fd_out = dup_fd;
  *meta_out = strdup(meta ? meta : "");
  sd_bus_message_unref(reply);
  return 0;
}

static void render_meta(struct Slideshow *s, char *meta) {
  if (s->eink_meta)
    eink_meta_render(s->eink_meta, meta);
}

static void render_fd(struct Slideshow *s, int fd) {
  struct jpeg_image *img = jpeg_load_fd(fd, s->fbi.width, s->fbi.height);
  if (!img) {
    fprintf(stderr, "jpeg decode failed\n");
    return;
  }
  img_render(s->fb, s->fbi.width, s->fbi.height, s->fbi.stride, img->pixels, img->width, img->height, s->rotation,
             INTERP_BILINEAR);
  jpeg_free(img);
}

// Returns true if stop was requested, false on timeout or next-request.
static bool wait_or_stop(struct Slideshow *s, uint32_t secs) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += secs;
  for (;;) {
    if (sem_timedwait(&s->wake_sem, &ts) == 0)
      return s->stop_requested;
    if (errno == ETIMEDOUT)
      return false;
    if (errno == EINTR)
      continue;
    perror("sem_timedwait");
    return false;
  }
}

static void *worker_main(void *ud) {
  struct Slideshow *s = ud;
  for (;;) {
    int fd = -1;
    char *meta = NULL;
    if (fetch_one(s, &fd, &meta) == 0) {
      printf("Displaying new picture\n");
      render_meta(s, meta);
      render_fd(s, fd);
      close(fd);
      free(meta);
    }
    if (wait_or_stop(s, s->transition_time_s))
      break;
  }
  return NULL;
}

// Push initial config to photo-provider: target size matched to the physical
// screen (axes swapped for 90/270 rotation so the server renders at the
// correct aspect ratio) and embed_qr.
static int push_initial_config(sd_bus *bus, uint32_t w, uint32_t h, bool embed_qr) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  int r = sd_bus_call_method(bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, "SetTargetSize", &err,
                             NULL, "uu", w, h);
  if (r < 0) {
    fprintf(stderr, "SetTargetSize failed: %s\n", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }
  sd_bus_error_free(&err);

  r = sd_bus_call_method(bus, DBUS_PHOTO_SERVICE, DBUS_PHOTO_PATH, DBUS_PHOTO_INTERFACE, "SetEmbedQr", &err, NULL, "b",
                         (int)(embed_qr ? 1 : 0));
  if (r < 0) {
    fprintf(stderr, "SetEmbedQr failed: %s\n", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    return -1;
  }
  sd_bus_error_free(&err);
  printf("photo-provider configured: %ux%u embed_qr=%d\n", w, h, embed_qr);
  return 0;
}

static void on_photo_svc_updown(void *ud, bool up) {
  if (!up) {
    fprintf(stderr, "WARNING: Occupancy service is down, assuming no presence (and slideshow will shutdown)\n");
    return;
  }

  struct Slideshow *s = ud;
  if (push_initial_config(s->bus, s->target_w, s->target_h, s->embed_qr) < 0) {
    fprintf(stderr, "WARNING: Failed to config photo provider service '%s'.\n", DBUS_PHOTO_SERVICE);
  }
}

struct Slideshow *slideshow_init(sd_bus *bus, uint32_t *fb, const struct fb_info *fbi, uint32_t transition_time_s,
                                 uint32_t rotation_deg, bool embed_qr, bool use_eink_for_metadata) {
  if (!bus || !fb || !fbi || transition_time_s == 0)
    return NULL;
  if (rotation_deg != 0 && rotation_deg != 90 && rotation_deg != 180 && rotation_deg != 270) {
    fprintf(stderr, "slideshow_init: invalid rotation %u\n", rotation_deg);
    return NULL;
  }
  if (transition_time_s < 3 || transition_time_s > 300) {
    fprintf(stderr, "slideshow_init: invalid transition time %d, must be (3, 300]\n", transition_time_s);
  }
  struct Slideshow *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->fb = fb;
  s->fbi = *fbi;
  s->transition_time_s = transition_time_s;
  s->rotation = (enum rotation)rotation_deg;
  if (sem_init(&s->wake_sem, 0, 0) != 0) {
    perror("sem_init");
    free(s);
    return NULL;
  }
  if (connect_worker_bus(s) < 0) {
    sem_destroy(&s->wake_sem);
    free(s);
    return NULL;
  }
  s->bus = bus;

  s->embed_qr = embed_qr;
  s->target_w = fbi->width;
  s->target_h = fbi->height;
  if (rotation_deg == 90 || rotation_deg == 270) {
    s->target_w = fbi->height;
    s->target_h = fbi->width;
  }

  if (use_eink_for_metadata) {
    s->eink_meta = eink_meta_init();
    if (!s->eink_meta)
      fprintf(stderr, "WARNING: eink metadata display unavailable\n");
  }

  s->photo_svc_monitor = on_service_updown(s->bus, DBUS_PHOTO_SERVICE, on_photo_svc_updown, s);
  if (!is_service_up(s->bus, DBUS_PHOTO_SERVICE)) {
    fprintf(stderr, "WARNING: %s is not running; photos can't be displayed until it starts.\n", DBUS_PHOTO_SERVICE);
  }

  if (push_initial_config(s->bus, s->target_w, s->target_h, s->embed_qr) < 0) {
    // We'll retry if the service comes up later, but warn the user
    fprintf(stderr, "WARNING: Failed to config photo provider service '%s'.\n", DBUS_PHOTO_SERVICE);
  }
  return s;
}

void slideshow_free(struct Slideshow *s) {
  if (!s)
    return;
  slideshow_stop(s);
  sem_destroy(&s->wake_sem);
  if (s->photo_svc_monitor)
    sd_bus_slot_unref(s->photo_svc_monitor);
  if (s->worker_bus)
    sd_bus_flush_close_unref(s->worker_bus);
  eink_meta_free(s->eink_meta);
  free(s);
}

void slideshow_start(struct Slideshow *s) {
  if (s->worker_running)
    return;
  printf("Starting slideshow\n");
  s->worker_running = true;
  if (pthread_create(&s->worker, NULL, worker_main, s) != 0) {
    perror("pthread_create");
    s->worker_running = false;
  }
}

void slideshow_stop(struct Slideshow *s) {
  if (!s->worker_running)
    return;
  printf("Stopping slideshow\n");
  s->stop_requested = true;
  sem_post(&s->wake_sem);
  pthread_join(s->worker, NULL);
  s->worker_running = false;
  s->stop_requested = false;
  // Drain any leftover posts (e.g. a next() queued just before stop) so the
  // next start() begins with a clean wait.
  while (sem_trywait(&s->wake_sem) == 0) {
  }
}

void slideshow_next(struct Slideshow *s) {
  printf("User requested to advance to the next picture\n");
  if (!s->worker_running)
    return;
  printf("Advancing slideshow\n");
  sem_post(&s->wake_sem);
}
