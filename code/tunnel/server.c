#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#define CLIENT_SOCKET_FN ("./client.sock")
#define SERVER_SOCKET_FN ("./server.sock")

#define MAX_EVENTS (1024)
#define BUFFER_SIZE (1024000)

/* TODO: remove */
#define stderr stdout

#define FATAL(msg)                                                             \
  { report(__FUNCTION__, __LINE__, msg, 1); }
#define LOG(msg)                                                               \
  { report(__FUNCTION__, __LINE__, msg, 0); }

// #define DLOG(fmt/**/, args*/...) { fprintf(stderr, "%s:%d: ", __FUNCTION__,
// __LINE__); fprintf(stderr, fmt, __VA_OPT__(,/*args*/)) ; }

#define REPORT(msg, terminate)                                                 \
  { report(__FUNCTION__, __LINE__, msg, terminate); }

struct epoll_event ev, events[MAX_EVENTS];
int epollfd;

int server_socket = -1, wayland_socket = -1;

void report(const char *where, int line, const char *msg, int terminate) {
  char tmp[256];
  sprintf(tmp, "%s:%d %s", where, line, msg);
  if (errno) {
    perror(tmp);
    if (terminate)
      exit(-1);
  } else {
    fprintf(stderr, "%s\n", tmp);
  }
}

int init_server() {
  struct sockaddr_un socket_name;

  // Remove socket file if exists
  if (access(SERVER_SOCKET_FN, F_OK) == 0) {
    remove(SERVER_SOCKET_FN);
  }
  server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket < 0) {
    REPORT("init_server: server socket", 1);
  }

  fprintf(stderr, "server socket: %d\n", server_socket);

  memset(&socket_name, 0, sizeof(socket_name));
  socket_name.sun_family = AF_UNIX;
  strncpy(socket_name.sun_path, SERVER_SOCKET_FN,
          sizeof(socket_name.sun_path) - 1);
  if (bind(server_socket, (struct sockaddr *)&socket_name,
           sizeof(socket_name)) < 0) {
    FATAL("bind");
  }

  if (listen(server_socket, MAX_EVENTS) < 0)
    FATAL("listen");

  ev.events = EPOLLIN;
  ev.data.fd = server_socket;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, server_socket, &ev) == -1) {
    FATAL("init_server: epoll_ctl: server_socket");
  }

  LOG("server initialized");
}

int init_wayland() {

  struct sockaddr_un socket_name;

  wayland_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (wayland_socket < 0) {
    REPORT("init_server: mem socket", 1); /* terminate */
  }

  fprintf(stderr, "wayland socket: %d\n", wayland_socket);

  memset(&socket_name, 0, sizeof(socket_name));
  socket_name.sun_family = AF_UNIX;
  strncpy(socket_name.sun_path, CLIENT_SOCKET_FN,
          sizeof(socket_name.sun_path) - 1);
  if (connect(wayland_socket, (struct sockaddr *)&socket_name,
              sizeof(socket_name)) < 0) {
    REPORT("connect", 1); /* terminate */
  }

  ev.events = EPOLLIN;
  ev.data.fd = wayland_socket;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, wayland_socket, &ev) == -1) {
    REPORT("epoll_ctl: wayland_socket", 1);
  }

  LOG("client side initialized");
  return 0;
}

void run_server() {
  fd_set rfds;
  struct timeval tv;
  int conn_sock, rv, nfds, n, current_client = -1;
  struct sockaddr_un caddr; /* client address */
  int len = sizeof(caddr);  /* address length could change */
  char buffer[BUFFER_SIZE + 1];

  fprintf(stderr, "Listening for clients...\n");
  int count;
  while (1) {

    nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      FATAL("epoll_wait");
    }

    for (n = 0; n < nfds; n++) {

      if (events[n].data.fd == server_socket) {
        conn_sock = accept(server_socket, (struct sockaddr *)&caddr, &len);
        if (conn_sock == -1) {
          FATAL("accept");
        }
        fcntl(conn_sock, F_SETFL, O_NONBLOCK);
        ev.events = EPOLLIN | EPOLLET | EPOLLHUP;
        ev.data.fd = conn_sock;
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_sock, &ev) == -1) {
          FATAL("epoll_ctl: conn_sock");
        }
        fprintf(stderr, "%d: Added client on fd %d\n", __LINE__, conn_sock);
      }

      else {
        fprintf(stderr, "%d: ", __LINE__);
        fprintf(stderr, "Event 0x%x on fd %d\n", events[n].events,
                events[n].data.fd);

        if (events[n].data.fd == wayland_socket) {
          fprintf(stderr, "%d: %s\n", __LINE__, "Reading from wayland socket");

          len = read(events[n].data.fd, buffer, sizeof(buffer));

          if (len <= 0) {
            REPORT("read", 0);
            continue;
          }
          fprintf(stderr, "Read %d bytes on fd#%d\n", len, events[n].data.fd);

          rv = write(current_client, buffer, len);
          if (rv != len) {
            fprintf(stderr, "Wrote %d out of %d bytes on fd#%d\n", rv, n,
                    events[n].data.fd);
          }

        } else if (events[n].data.fd == server_socket) {
          LOG("readserver socket");
        }

        else { // connected client event
          fprintf(stderr, "%d: %s\n", __LINE__,
                  "Reading from connected client");
          current_client = events[n].data.fd;

          len = read(events[n].data.fd, buffer, sizeof(buffer));
          if (len <= 0) {
            REPORT("read", 0);
            continue;
          }

          fprintf(stderr, "Read %d bytes on fd#%d\n", len, events[n].data.fd);
          // rv = write(current_client, buffer, len);
          rv = write(wayland_socket, buffer, len);
          if (rv != len) {
            fprintf(stderr, "Wrote only %d out of %d bytes on fd#%d\n", rv, len,
                    events[n].data.fd);
          }
        }
      }
    }

  } /* while(1) */
}

int main() {

  epollfd = epoll_create1(0);
  if (epollfd == -1) {
    REPORT("init_server: epoll_create1", 1);
  }

  init_server();
  init_wayland();

  run_server();

  return 0;
}
