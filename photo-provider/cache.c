#define _GNU_SOURCE
#include "cache.h"

#include "www_session.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct entry {
  int fd;
  char *meta;
};

struct pp_cache {
  struct pp_www_session *ws; // borrowed

  bool dump_to_disk;
  char dump_dir[256];
  int dump_counter;

  pthread_mutex_t mu;
  pthread_cond_t cv;

  // Bumped on every invalidate. Worker captures this before a fetch and
  // discards the result if the generation moved.
  uint32_t generation;

  struct entry *ring;
  uint32_t ring_cap;
  uint32_t ring_head;
  uint32_t ring_count;

  bool shutdown;
  pthread_t worker;
};

static void entry_close(struct entry *e) {
  if (e->fd >= 0)
    close(e->fd);
  free(e->meta);
  e->fd = -1;
  e->meta = NULL;
}

static void flush_locked(struct pp_cache *c) {
  for (uint32_t i = 0; i < c->ring_count; i++) {
    uint32_t idx = (c->ring_head + i) % c->ring_cap;
    entry_close(&c->ring[idx]);
  }
  c->ring_head = 0;
  c->ring_count = 0;
}

void pp_cache_invalidate(void *ud) {
  struct pp_cache *c = ud;
  pthread_mutex_lock(&c->mu);
  flush_locked(c);
  c->generation++;
  pthread_cond_broadcast(&c->cv);
  pthread_mutex_unlock(&c->mu);
}

static void dump_to_disk(struct pp_cache *c, int fd, const char *meta) {
  if (!c->dump_to_disk)
    return;
  mkdir(c->dump_dir, 0755);
  int counter = ++c->dump_counter;
  char path[512];
  snprintf(path, sizeof(path), "%s/%d.jpeg", c->dump_dir, counter);
  FILE *f = fopen(path, "wb");
  if (!f) {
    perror(path);
    return;
  }
  off_t save = lseek(fd, 0, SEEK_CUR);
  lseek(fd, 0, SEEK_SET);
  char buf[8192];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0)
    fwrite(buf, 1, (size_t)n, f);
  fclose(f);
  lseek(fd, save, SEEK_SET);

  snprintf(path, sizeof(path), "%s/%d.json", c->dump_dir, counter);
  f = fopen(path, "w");
  if (f) {
    fputs(meta ? meta : "", f);
    fclose(f);
  }
  printf("Dumped photo #%d to %s\n", counter, c->dump_dir);
}

static void *worker_main(void *arg) {
  struct pp_cache *c = arg;

  pthread_mutex_lock(&c->mu);
  while (!c->shutdown) {
    while (!c->shutdown && c->ring_count >= c->ring_cap)
      pthread_cond_wait(&c->cv, &c->mu);
    if (c->shutdown)
      break;

    uint32_t gen = c->generation;
    pthread_mutex_unlock(&c->mu);

    int fd = -1;
    char *meta = NULL;
    int rc = pp_www_session_fetch_next(c->ws, &fd, &meta);

    pthread_mutex_lock(&c->mu);
    if (rc < 0) {
      pthread_mutex_unlock(&c->mu);
      struct timespec ts = {.tv_sec = 2, .tv_nsec = 0};
      nanosleep(&ts, NULL);
      pthread_mutex_lock(&c->mu);
      continue;
    }
    if (gen != c->generation || c->shutdown) {
      close(fd);
      free(meta);
      continue;
    }
    dump_to_disk(c, fd, meta);
    uint32_t tail = (c->ring_head + c->ring_count) % c->ring_cap;
    c->ring[tail].fd = fd;
    c->ring[tail].meta = meta;
    c->ring_count++;
    pthread_cond_broadcast(&c->cv);
  }
  flush_locked(c);
  pthread_mutex_unlock(&c->mu);
  return NULL;
}

struct pp_cache *pp_cache_init(const struct pp_cache_params *p) {
  struct pp_cache *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  c->ws = p->ws;
  c->dump_to_disk = p->dump_to_disk;
  strncpy(c->dump_dir, p->dump_dir ? p->dump_dir : "", sizeof(c->dump_dir) - 1);
  c->ring_cap = p->cache_depth ? p->cache_depth : 1;
  c->ring = calloc(c->ring_cap, sizeof(*c->ring));
  if (!c->ring) {
    free(c);
    return NULL;
  }
  for (uint32_t i = 0; i < c->ring_cap; i++)
    c->ring[i].fd = -1;
  pthread_mutex_init(&c->mu, NULL);
  pthread_cond_init(&c->cv, NULL);

  if (pthread_create(&c->worker, NULL, worker_main, c) != 0) {
    pp_cache_free(c);
    return NULL;
  }
  return c;
}

void pp_cache_free(struct pp_cache *c) {
  if (!c)
    return;
  if (c->worker) {
    pthread_mutex_lock(&c->mu);
    c->shutdown = true;
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    pthread_join(c->worker, NULL);
  }
  free(c->ring);
  pthread_mutex_destroy(&c->mu);
  pthread_cond_destroy(&c->cv);
  free(c);
}

int pp_cache_pop(struct pp_cache *c, int *fd_out, char **meta_out, int timeout_ms) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&c->mu);
  while (c->ring_count == 0) {
    int r = pthread_cond_timedwait(&c->cv, &c->mu, &deadline);
    if (r == ETIMEDOUT) {
      pthread_mutex_unlock(&c->mu);
      return -1;
    }
  }
  struct entry *e = &c->ring[c->ring_head];
  int dup_fd = dup(e->fd);
  char *meta = e->meta;
  e->meta = NULL;
  close(e->fd);
  e->fd = -1;
  c->ring_head = (c->ring_head + 1) % c->ring_cap;
  c->ring_count--;
  pthread_cond_broadcast(&c->cv);
  pthread_mutex_unlock(&c->mu);

  if (dup_fd < 0) {
    free(meta);
    return -1;
  }
  *fd_out = dup_fd;
  *meta_out = meta;
  return 0;
}
