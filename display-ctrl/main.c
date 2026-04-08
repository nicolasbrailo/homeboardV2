#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCK_NAME "display-ctrl.sock"

int main(int argc, char *argv[]) {
  if (argc != 2 || !(strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0 || strcmp(argv[1], "status") == 0)) {
    fprintf(stderr, "Usage: %s on|off|status\n", argv[0]);
    return 1;
  }

  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (!xdg || !*xdg) {
    fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
    return 1;
  }

  char sock_path[108];
  snprintf(sock_path, sizeof(sock_path), "%s/%s", xdg, SOCK_NAME);

  const int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return 1;
  }

  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    fprintf(stderr, "display-mgr not running (can't connect to %s)\n", sock_path);
    return 1;
  }

  char msg[64];
  int len = snprintf(msg, sizeof(msg), "%s\n", argv[1]);
  send(sock, msg, (size_t)len, 0);

  char buf[64];
  const ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
  if (n > 0) {
    buf[n] = '\0';
    printf("%s", buf);
  }

  close(sock);
}
