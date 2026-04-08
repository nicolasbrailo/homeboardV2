#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "sock.h"

#define SOCK_NAME "display-ctrl.sock"

static char g_sock_path[108];
static int g_listen_sock = -1;

int sock_init() {
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (!xdg || !*xdg) {
    fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
    sock_free();
    return -1;
  }
  snprintf(g_sock_path, sizeof(g_sock_path), "%s/%s", xdg, SOCK_NAME);

  unlink(g_sock_path);
  g_listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (g_listen_sock < 0) {
    perror("socket");
    sock_free();
    return -1;
  }

  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);
  if (bind(g_listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    sock_free();
    return -1;
  }
  chmod(g_sock_path, 0666);

  if (listen(g_listen_sock, 2) < 0) {
    perror("listen");
    sock_free();
    return -1;
  }

  return 0;
}

void sock_free() {
  if (g_listen_sock >= 0) {
    close(g_listen_sock);
    g_listen_sock = -1;
  }
  unlink(g_sock_path);
}

int sock_poll(char *cmd, size_t cmdlen) {
  struct pollfd fds = {
    .fd = g_listen_sock,
    .events = POLLIN
  };

  int pr = poll(&fds, 1, 1000);
  if (pr < 0) {
    if (errno != EINTR)
      perror("poll");
    return 0;
  }

  if (!(fds.revents & POLLIN))
    return 0;

  int client = accept(g_listen_sock, NULL, NULL);
  if (client < 0)
    return 0;

  int optval = 1;
  setsockopt(client, SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval));

  ssize_t n = recv(client, cmd, cmdlen - 1, 0);
  if (n <= 0) {
    close(client);
    return 0;
  }

  cmd[n] = '\0';
  if (cmd[n - 1] == '\n')
    cmd[n - 1] = '\0';

  return client;
}

pid_t sock_get_peer_pid(int client_sock) {
  struct ucred cred;
  socklen_t len = sizeof(cred);
  if (getsockopt(client_sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
    return 0;
  return cred.pid;
}

int sock_send_fd(int client_sock, int fd_to_send, const void *data, size_t datalen) {
  int status = 0;
  if (send(client_sock, &status, sizeof(status), 0) < 0)
    return -1;

  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  struct iovec iov = {.iov_base = (void *)data, .iov_len = datalen};
  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsgbuf,
      .msg_controllen = sizeof(cmsgbuf),
  };

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

  return sendmsg(client_sock, &msg, 0) < 0 ? -1 : 0;
}

int sock_send_error(int client_sock, int errcode) {
  return send(client_sock, &errcode, sizeof(errcode), 0) < 0 ? -1 : 0;
}
