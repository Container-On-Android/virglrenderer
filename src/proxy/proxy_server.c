/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "proxy_server.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "server/render_context.h"
#include "server/render_protocol.h"
#include "server/render_server.h"

int
proxy_server_connect(struct proxy_server *srv)
{
   int client_fd = srv->client_fd;
   /* transfer ownership */
   srv->client_fd = -1;
   return client_fd;
}

void
proxy_server_destroy(struct proxy_server *srv)
{
   if (srv->pid >= 0) {
      kill(srv->pid, SIGKILL);

      siginfo_t siginfo = { 0 };
      waitid(P_PID, srv->pid, &siginfo, WEXITED);
   }

   if (srv->client_fd >= 0)
      close(srv->client_fd);

#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   if (srv->in_process) {
      thrd_join(srv->thread, NULL);
      srv->in_process = false;
   }
#endif

   free(srv);
}

static bool
proxy_server_fork(struct proxy_server *srv)
{
   int socket_fds[2];
   if (!proxy_socket_pair(socket_fds))
      return false;
   const int client_fd = socket_fds[0];
   const int remote_fd = socket_fds[1];

   pid_t pid = fork();
   if (pid < 0) {
      proxy_log("failed to fork proxy server");
      close(client_fd);
      close(remote_fd);
      return false;
   }

   if (pid > 0) {
      srv->pid = pid;
      srv->client_fd = client_fd;
      close(remote_fd);
   } else {
      close(client_fd);

      /* do not receive signals from terminal */
      setpgid(0, 0);

      char fd_str[16];
      snprintf(fd_str, sizeof(fd_str), "%d", remote_fd);

      /* for devenv without installing server */
      char *const server_path = getenv("RENDER_SERVER_EXEC_PATH");
      char *const argv[] = {
         server_path ? server_path : RENDER_SERVER_EXEC_PATH,
         "--socket-fd",
         fd_str,
         NULL,
      };
      execv(argv[0], argv);

      proxy_log("failed to exec %s: %s", argv[0], strerror(errno));
      close(remote_fd);
      exit(-1);
   }

   return true;
}

static bool
proxy_server_init_fd(struct proxy_server *srv)
{
   /* the fd represents a connection to the server */
   srv->client_fd = proxy_renderer.cbs->get_server_fd(RENDER_SERVER_VERSION);
   if (srv->client_fd < 0)
      return false;

   return true;
}

#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
static int
proxy_server_start_thread(void *args)
{
   int remote_fd = (int)(uintptr_t)args;
   char fd_str[16];
   snprintf(fd_str, sizeof(fd_str), "%d", remote_fd);
   char *argv[] = {
      RENDER_SERVER_EXEC_PATH,
      "--socket-fd",
      fd_str,
      NULL,
   };
   struct render_context_args ctx_args;

   ctx_args.in_process = true;
   bool ok = render_server_main(3, argv, &ctx_args);

   return ok ? 0 : -1;
}

static bool
proxy_server_init_thread(struct proxy_server *srv)
{
   int socket_fds[2];

   if (!proxy_socket_pair(socket_fds))
      return false;

   const int client_fd = socket_fds[0];
   const uintptr_t remote_fd = socket_fds[1];

   bool ok = thrd_create(&srv->thread, proxy_server_start_thread, (void *)remote_fd) == thrd_success;

   if (ok) {
      srv->client_fd = client_fd;
      srv->in_process = true;
   } else {
      close(client_fd);
      close(remote_fd);
   }

   return ok;
}
#endif

struct proxy_server *
proxy_server_create(bool in_process)
{
   struct proxy_server *srv = calloc(1, sizeof(*srv));
   if (!srv)
      return NULL;

   srv->pid = -1;

   if (in_process) {
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
      if (!proxy_server_init_thread(srv)) {
         free(srv);
         return NULL;
      }
#else
      proxy_log("in process server not supported");
      free(srv);
      return NULL;
#endif
   } else if (!proxy_server_init_fd(srv)) {
      /* start the render server on demand when the client does not provide a
       * server fd
       */
      if (!proxy_server_fork(srv)) {
         free(srv);
         return NULL;
      }
   }

   proxy_log("proxy server with pid %d", srv->pid);

   return srv;
}
