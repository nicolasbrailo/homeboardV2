#define _GNU_SOURCE
#include "slideshow.h"

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

#define DBUS_SERVICE "io.homeboard.PhotoProvider"
#define DBUS_PATH "/io/homeboard/PhotoProvider"
#define DBUS_INTERFACE "io.homeboard.PhotoProvider1"

// Lifecycle: start/stop are only ever called from the main (dbus dispatch)
// thread, so worker_running needs no synchronization. The worker is signalled
// to stop via stop_sem: stop() posts it, worker's sem_timedwait returns 0 and
// the loop exits. sem_post is durable — if it fires while the worker is
// mid-fetch, the next sem_timedwait returns immediately.
struct Slideshow {
  uint32_t *fb;
  struct fb_info fbi;
  uint32_t transition_time_s;
  enum rotation rotation;

  sd_bus *bus;
  pthread_t worker;
  bool worker_running;
  sem_t stop_sem;
};

static int fetch_one(sd_bus *bus, int *fd_out, char **meta_out) {
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  int r = sd_bus_call_method(bus, DBUS_SERVICE, DBUS_PATH, DBUS_INTERFACE, "GetPhoto", &err, &reply, "");
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
  printf("slideshow: %s\n", meta);
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

// Returns true if stop was requested, false on timeout.
static bool wait_or_stop(sem_t *sem, uint32_t secs) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += secs;
  for (;;) {
    if (sem_timedwait(sem, &ts) == 0)
      return true;
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
    if (fetch_one(s->bus, &fd, &meta) == 0) {
      printf("Displaying new picture\n");
      render_meta(s, meta);
      render_fd(s, fd);
      close(fd);
      free(meta);
    }
    if (wait_or_stop(&s->stop_sem, s->transition_time_s))
      break;
  }
  return NULL;
}

struct Slideshow *slideshow_init(uint32_t *fb, const struct fb_info *fbi, uint32_t transition_time_s,
                                 uint32_t rotation_deg) {
  if (!fb || !fbi || transition_time_s == 0)
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
  if (sem_init(&s->stop_sem, 0, 0) != 0) {
    perror("sem_init");
    free(s);
    return NULL;
  }
  int r = sd_bus_open_system(&s->bus);
  if (r < 0) {
    fprintf(stderr, "slideshow: sd_bus_open_system: %s\n", strerror(-r));
    sem_destroy(&s->stop_sem);
    free(s);
    return NULL;
  }
  return s;
}

void slideshow_free(struct Slideshow *s) {
  if (!s)
    return;
  slideshow_stop(s);
  sem_destroy(&s->stop_sem);
  if (s->bus)
    sd_bus_flush_close_unref(s->bus);
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
  sem_post(&s->stop_sem);
  pthread_join(s->worker, NULL);
  s->worker_running = false;
  // Drain the post in case the worker exited for another reason before
  // consuming it; otherwise the next start() would see an immediate stop.
  while (sem_trywait(&s->stop_sem) == 0) {
  }
}
