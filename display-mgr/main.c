#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "drm.h"
#include "drm_lease.h"
#include "run_state.h"
#include "sock.h"

static volatile sig_atomic_t g_quit;
static bool g_display_on;

static void sig_handler(int sig) {
  (void)sig;
  g_quit = 1;
}

static void handle_client(struct DRM_state *drm, int client_sock, const char *cmd) {
  if (strcmp(cmd, "on") == 0) {
    if (!g_display_on) {
      drm_display_on(drm);
      g_display_on = true;
      run_state_write("on");
      fprintf(stderr, "display on\n");
    }
    send(client_sock, "ok\n", 3, 0);

  } else if (strcmp(cmd, "off") == 0) {
    if (g_display_on) {
      drm_display_off(drm);
      g_display_on = false;
      run_state_write("off");
      fprintf(stderr, "display off\n");
    }
    send(client_sock, "ok\n", 3, 0);

  } else if (strcmp(cmd, "status") == 0) {
    const char *state = g_display_on ? "on\n" : "off\n";
    send(client_sock, state, strlen(state), 0);

  } else if (strcmp(cmd, "get_drm_fd") == 0) {
    const pid_t peer = sock_get_peer_pid(client_sock);
    if (peer == 0) {
      fprintf(stderr, "get_drm_fd: can't identify peer\n");
      sock_send_error(client_sock, -2);
      return;
    }

    if (drm_lease_assign(peer) != 0) {
      fprintf(stderr, "get_drm_fd rejected: leased to pid %d\n", drm_lease_get_active());
      sock_send_error(client_sock, -1);
      return;
    }

    const struct fb_info *fbi = drm_get_fb_info(drm);
    if (sock_send_fd(client_sock, drm_get_dmabuf_fd(drm), fbi, sizeof(*fbi)) < 0) {
      perror("sock_send_fd");
      return;
    }

    fprintf(stderr, "lease granted to pid %d\n", peer);

  } else if (strcmp(cmd, "close_drm_fd") == 0) {
    pid_t peer = sock_get_peer_pid(client_sock);
    drm_lease_release(peer);
    send(client_sock, "ok\n", 3, 0);

  } else {
    fprintf(stderr, "unknown command: '%s'\n", cmd);
  }
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  if (run_state_init() < 0)
    return 1;
  if (sock_init() < 0)
    return 1;

  struct DRM_state *drm = drm_init();
  if (!drm)
    return 1;
  if (drm_create_framebuffer(drm) < 0)
    return 1;

  g_display_on = drm_is_display_on(drm);
  const bool display_was_on_on_startup = g_display_on;
  run_state_write(g_display_on ? "on" : "off");

  signal(SIGTERM, sig_handler);
  signal(SIGINT, sig_handler);

  printf("display-mgr running, %zu output(s)\n", drm_get_num_outputs(drm));
  while (!g_quit) {
    char cmd[64];
    int client_fd = sock_poll(cmd, sizeof(cmd));

    if (client_fd > 0) {
      handle_client(drm, client_fd, cmd);
      close(client_fd);
    }

    drm_lease_is_alive();
  }

  drm_lease_release(0);
  if (display_was_on_on_startup) {
    drm_display_on(drm);
  } else {
    drm_display_off(drm);
  }

  drm_free(drm);
  sock_free();
  run_state_free();

  return 0;
}
