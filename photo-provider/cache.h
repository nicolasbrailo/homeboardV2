#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "www_session.h"

struct pp_cache;

struct pp_cache_params {
  struct pp_www_session *ws; // borrowed, not owned
  uint32_t cache_depth;
  bool dump_to_disk;
  const char *dump_dir;
};

// Starts a worker thread that refills the ring from ws. Main is expected
// to wire pp_cache_invalidate as ws's invalidate callback so the ring is
// flushed when a config change rotates the client_id server-side.
struct pp_cache *pp_cache_init(const struct pp_cache_params *p);
void pp_cache_free(struct pp_cache *c);

// Signature matches pp_ws_invalidate_fn; pass as ws's on_invalidate.
void pp_cache_invalidate(void *cache);

// Pops the head of the ring. On success, *fd_out is an owned memfd (caller
// closes) and *meta_out is a malloc'd JSON string (caller frees). Blocks up
// to timeout_ms. Returns 0 on success, -1 on timeout/error.
int pp_cache_pop(struct pp_cache *c, int *fd_out, char **meta_out, int timeout_ms);
