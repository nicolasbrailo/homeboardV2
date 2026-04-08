#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

#include "drm_lease.h"

static pid_t g_lease_pid = 0;

void drm_lease_release(pid_t expected_owner) {
  if (g_lease_pid == 0)
    return; // Already free

  if (expected_owner > 0 && g_lease_pid != expected_owner) {
    fprintf(stderr, "drm_lease_release from pid %d but lease held by pid %d\n", expected_owner, g_lease_pid);
    return;
  }

  fprintf(stderr, "lease released (was pid %d)\n", g_lease_pid);
  g_lease_pid = 0;
}

bool drm_lease_is_alive() {
  if (g_lease_pid == 0)
    return false;

  if (kill(g_lease_pid, 0) < 0) {
    fprintf(stderr, "error: lease holder pid %d died without releasing\n", g_lease_pid);
    drm_lease_release(0);
    return false;
  }
  return true;
}

int drm_lease_assign(pid_t peer) {
  drm_lease_is_alive();

  if (g_lease_pid == peer)
    return 0;

  if (g_lease_pid == 0) {
    g_lease_pid = peer;
    return 0;
  }

  return -1;
}

pid_t drm_lease_get_active() {
  drm_lease_is_alive();
  return g_lease_pid;
}

