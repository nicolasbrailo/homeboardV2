#pragma once

#include <stdint.h>

int sock_init();
void sock_free();

// Accept a client and read its command. Returns:
//  - client fd (> 0) and fills cmd on success
//  - 0 if no client or read failed
int sock_poll(char *cmd, size_t cmdlen);

pid_t sock_get_peer_pid(int client_sock);
int sock_send_fd(int client_sock, int fd_to_send, const void *data, size_t datalen);
int sock_send_error(int client_sock, int errcode);
