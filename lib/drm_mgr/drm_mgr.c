#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "drm_mgr.h"

#define SOCK_NAME "display-ctrl.sock"

struct DRM_Mgr {
  char sock_path[108];
  void *fb;
  int dmabuf_fd;
  uint32_t fb_size;
};

struct DRM_Mgr *drm_mgr_init() {
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (!xdg || !*xdg) {
    fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
    return NULL;
  }

  struct DRM_Mgr *self = malloc(sizeof(struct DRM_Mgr));
  if (!self)
    return NULL;

  self->fb = NULL;
  self->dmabuf_fd = -1;
  self->fb_size = 0;
  snprintf(self->sock_path, sizeof(self->sock_path), "%s/%s", xdg, SOCK_NAME);
  return self;
}

void drm_mgr_free(struct DRM_Mgr *self) { free(self); }

static int connect_daemon(const struct DRM_Mgr *self) {
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strncpy(addr.sun_path, self->sock_path, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("Failed to connect to DRM manager, is it running?");
    close(sock);
    return -1;
  }

  return sock;
}

static int recv_fd(int sock, int *out_fd, void *data, size_t datalen) {
  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  struct iovec iov = {.iov_base = data, .iov_len = datalen};
  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsgbuf,
      .msg_controllen = sizeof(cmsgbuf),
  };

  ssize_t n = recvmsg(sock, &msg, 0);
  if (n < 0) {
    perror("recvmsg");
    return -1;
  }
  if ((size_t)n < datalen) {
    fprintf(stderr, "short read from daemon\n");
    return -1;
  }

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
    fprintf(stderr, "no fd received (lease rejected?)\n");
    return -1;
  }

  memcpy(out_fd, CMSG_DATA(cmsg), sizeof(int));
  return 0;
}

void *drm_mgr_acquire_fb(struct DRM_Mgr *self, struct fb_info *info) {
  int sock = connect_daemon(self);
  if (sock < 0)
    return NULL;

  const char *cmd = "get_drm_fd\n";
  if (send(sock, cmd, strlen(cmd), 0) < 0) {
    perror("send");
    close(sock);
    return NULL;
  }

  int status;
  if (recv(sock, &status, sizeof(status), 0) != sizeof(status)) {
    fprintf(stderr, "failed to receive status from daemon\n");
    close(sock);
    return NULL;
  }
  if (status != 0) {
    fprintf(stderr, "daemon rejected get_drm_fd (error %d)\n", status);
    close(sock);
    return NULL;
  }

  int dmabuf_fd = -1;
  if (recv_fd(sock, &dmabuf_fd, info, sizeof(*info)) < 0) {
    close(sock);
    return NULL;
  }
  close(sock);

  void *fb = mmap(NULL, info->size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
  if (fb == MAP_FAILED) {
    perror("mmap");
    close(dmabuf_fd);
    return NULL;
  }

  self->fb = fb;
  self->dmabuf_fd = dmabuf_fd;
  self->fb_size = info->size;
  return fb;
}

void drm_mgr_release_fb(struct DRM_Mgr *self) {
  if (self->fb) {
    munmap(self->fb, self->fb_size);
    self->fb = NULL;
  }
  if (self->dmabuf_fd >= 0) {
    close(self->dmabuf_fd);
    self->dmabuf_fd = -1;
  }

  int sock = connect_daemon(self);
  if (sock < 0)
    return;

  const char *cmd = "close_drm_fd\n";
  send(sock, cmd, strlen(cmd), 0);

  char buf[16];
  recv(sock, buf, sizeof(buf), 0);
  close(sock);
}
