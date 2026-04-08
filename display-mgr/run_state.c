#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "run_state.h"

static char g_state_path[108];

int run_state_init() {
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (!xdg || !*xdg) {
    fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
    return -1;
  }
  snprintf(g_state_path, sizeof(g_state_path), "%s/display-ctrl.state", xdg);
  return 0;
}

void run_state_free() { unlink(g_state_path); }

void run_state_write(const char *state) {
  int fd = open(g_state_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return;
  fchmod(fd, 0644);
  write(fd, state, strlen(state));
  write(fd, "\n", 1);
  close(fd);
}
