/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef PROXY_SERVER_H
#define PROXY_SERVER_H

#include "proxy_common.h"

#include <sys/types.h>

#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
#include "c11/threads.h"
#endif

struct proxy_server {
   pid_t pid;
   int client_fd;
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   bool in_process;
   thrd_t thread;
#endif
};

struct proxy_server *
proxy_server_create(bool in_process);

void
proxy_server_destroy(struct proxy_server *srv);

int
proxy_server_connect(struct proxy_server *srv);

#endif /* PROXY_SERVER_H */
