/* Copyright 2022-2024 TII (SSRC) and the Ghaf contributors
   SPDX-License-Identifier: Apache-2.0
*/
#include <arpa/inet.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "../module/kvm_ivshmem.h"

#define SHM_DEVICE_FN "/dev/ivshmem"

#define REMOTE_RESOURCE_CONSUMED_INT_VEC (0)
#define LOCAL_RESOURCE_READY_INT_VEC (1)

#define MAX_EVENTS (1024)
#define MAX_CLIENTS (100)
#define BUFFER_SIZE (1024000)
#define SHMEM_POLL_TIMEOUT (300)
#define SHMEM_BUFFER_SIZE (1024000)
#define TEST_SLEEP_TIME (3333333)
#define SYNC_SLEEP_TIME (333333)
#define UNKNOWN_PEER (-1)
#if 1
#define DEBUG(fmt, ...)                                                        \
  {}
#else
#define DEBUG(fmt, ...)                                                        \
  {                                                                            \
    char tmp1[256], tmp2[256];                                                 \
    sprintf(tmp2, fmt, __VA_ARGS__);                                           \
    sprintf(tmp1, "%s:%d: %s\n", __FUNCTION__, __LINE__, tmp2);                \
    errno = 0;                                                                 \
    report(tmp1, 0);                                                           \
  }
#endif

#if 0
#define INFO(fmt, ...)                                                         \
  {}
#else
#define INFO(fmt, ...)                                                         \
  {                                                                            \
    char tmp1[256], tmp2[256];                                                 \
    sprintf(tmp2, fmt, __VA_ARGS__);                                           \
    sprintf(tmp1, "[%s:%d] %s\n", __FUNCTION__, __LINE__, tmp2);               \
    errno = 0;                                                                 \
    report(tmp1, 0);                                                           \
  }
#endif

#define ERROR(fmt, ...)                                                        \
  {                                                                            \
    char tmp1[256], tmp2[256];                                                 \
    sprintf(tmp2, fmt, __VA_ARGS__);                                           \
    sprintf(tmp1, "[%s:%d] %s\n", __FUNCTION__, __LINE__, tmp2);               \
    report(tmp1, 0);                                                           \
  }

#define FATAL(msg)                                                             \
  {                                                                            \
    char tmp1[256], tmp2[256];                                                 \
    sprintf(tmp2, msg);                                                        \
    sprintf(tmp1, "[%s:%d]: %s\n", __FUNCTION__, __LINE__, tmp2);              \
    report(tmp1, 1);                                                           \
  }

enum { CMD_LOGIN, CMD_CONNECT, CMD_DATA, CMD_CLOSE, CMD_RST, CMD_START };
#define FD_MAP_COUNT (sizeof(fd_map) / sizeof(fd_map[0]))
struct {
  int my_fd;
  int remote_fd;
} fd_map[MAX_CLIENTS];

struct epoll_event ev, events[MAX_EVENTS];
typedef struct {
  volatile int cmd;
  volatile int fd;
  volatile int len;
  volatile unsigned char data[SHMEM_BUFFER_SIZE];
} vm_data;

int epollfd;
char *socket_path;
int server_socket = -1, shmem_fd = -1;
int my_vmid = -1, peer_vm_id = -1;
vm_data *my_shm_data = NULL, *peer_shm_data = NULL;
int run_as_server = 0;

long int shmem_size;

struct {
  volatile int iv_client;
  volatile int iv_server;
  vm_data client_data;
  vm_data server_data;
} *vm_control;

static const char usage_string[] = "Usage: memsocket [-c|-s] socket_path\n";

void report(const char *msg, int terminate) {
  char tmp[256];
  if (errno)
    perror(msg);
  else
    fprintf(stderr, "%s", msg);

  if (terminate)
    exit(-1);
}

int get_shmem_size() {
  int res;

  res = lseek(shmem_fd, 0, SEEK_END);
  if (res < 0) {
    FATAL("seek");
  }
  lseek(shmem_fd, 0, SEEK_SET);
  return res;
}

int server_init() {
  struct sockaddr_un socket_name;

  // Remove socket file if exists
  if (access(socket_path, F_OK) == 0) {
    remove(socket_path);
  }

  server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket < 0) {
    FATAL("server socket");
  }

  DEBUG("server socket: %d", server_socket);

  memset(&socket_name, 0, sizeof(socket_name));
  socket_name.sun_family = AF_UNIX;
  strncpy(socket_name.sun_path, socket_path, sizeof(socket_name.sun_path) - 1);
  if (bind(server_socket, (struct sockaddr *)&socket_name,
           sizeof(socket_name)) < 0) {
    FATAL("bind");
  }

  if (listen(server_socket, MAX_EVENTS) < 0)
    FATAL("listen");

  ev.events = EPOLLIN;
  ev.data.fd = server_socket;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, server_socket, &ev) == -1) {
    FATAL("server_init: epoll_ctl: server_socket");
  }

  INFO("server initialized", "");
}

int wayland_connect() {

  struct sockaddr_un socket_name;
  int wayland_fd;

  wayland_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (wayland_fd < 0) {
    FATAL("wayland socket");
  }

  DEBUG("wayland socket: %d", wayland_fd);

  memset(&socket_name, 0, sizeof(socket_name));
  socket_name.sun_family = AF_UNIX;
  strncpy(socket_name.sun_path, socket_path, sizeof(socket_name.sun_path) - 1);
  if (connect(wayland_fd, (struct sockaddr *)&socket_name,
              sizeof(socket_name)) < 0) {
    FATAL("connect");
  }

  ev.events = EPOLLIN;
  ev.data.fd = wayland_fd;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, wayland_fd, &ev) == -1) {
    FATAL("epoll_ctl: wayland_fd");
  }

  INFO("client initialized", "");
  return wayland_fd;
}

void make_wayland_connection(int peer_fd) {

  int i;

  for (i = 0; i < FD_MAP_COUNT; i++) {
    if (fd_map[i].my_fd == -1) {
      fd_map[i].my_fd = wayland_connect();
      fd_map[i].remote_fd = peer_fd;
      return;
    }
  }

  ERROR("FAILED fd#%d", peer_fd);
  FATAL("fd_map table full");
}

int map_peer_fd(int peer_fd, int close_fd) {

  int i, rv;

  for (i = 0; i < FD_MAP_COUNT; i++) {
    if (fd_map[i].remote_fd == peer_fd) {
      rv = fd_map[i].my_fd;
      if (close_fd)
        fd_map[i].my_fd = -1;
      return rv;
    }
  }
  ERROR("FAILED fd#%d", peer_fd);
  FATAL("peer fd  not found");
  return -1;
}

int get_remote_socket(int my_fd, int close_fd, int ignore_error) {

  int i;

  for (i = 0; i < FD_MAP_COUNT; i++) {
    if (fd_map[i].my_fd == my_fd) {
      if (close_fd)
        fd_map[i].my_fd = -1;
      return fd_map[i].remote_fd;
    }
  }
  if (ignore_error)
    return -1;

  FATAL("my fd not found");
  return -1;
}

int shmem_init() {
  int res = -1;

  /* Open shared memory */
  shmem_fd = open(SHM_DEVICE_FN, O_RDWR);
  if (shmem_fd < 0) {
    FATAL("Open " SHM_DEVICE_FN);
  }
  INFO("shared memory fd: %d", shmem_fd);

  /* Get shared memory */
  shmem_size = get_shmem_size();
  if (shmem_size <= 0) {
    FATAL("No shared memory detected");
  }
  vm_control = mmap(NULL, shmem_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_NORESERVE, shmem_fd, 0);
  if (!vm_control) {
    FATAL("Got NULL pointer from mmap");
  }
  DEBUG("Shared memory at address %p 0x%lx bytes", vm_control, shmem_size);

  if (run_as_server) {
    my_shm_data = &vm_control->server_data;
    peer_shm_data = &vm_control->client_data;
  } else {
    my_shm_data = &vm_control->client_data;
    peer_shm_data = &vm_control->server_data;
  }

  /* get my VM Id */
  res = ioctl(shmem_fd, SHMEM_IOCIVPOSN, &my_vmid);
  if (res < 0) {
    FATAL("ioctl SHMEM_IOCIVPOSN failed");
  }
  my_vmid = my_vmid << 16;
  if (run_as_server) {
    vm_control->iv_server = my_vmid;
  } else {
    vm_control->iv_client = my_vmid;
    vm_control->iv_server = UNKNOWN_PEER;
  }
  INFO("My VM id = 0x%x. Running as a ", my_vmid);
  if (run_as_server) {
    INFO("server", "");
  } else {
    INFO("client", "");
  }

  ev.events = EPOLLIN;
  ev.data.fd = shmem_fd;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, shmem_fd, &ev) == -1) {
    FATAL("epoll_ctl: -1");
  }
  ioctl(shmem_fd, SHMEM_IOCRESTART, 0);

  INFO("shared memory initialized", "");

  return 0;
}

int run() {
  fd_set rfds;
  struct timeval tv;
  int conn_fd, rv, nfds, n;
  struct sockaddr_un caddr; /* client address */
  int len = sizeof(caddr);  /* address length could change */
  char buffer[BUFFER_SIZE + 1];
  struct pollfd my_buffer_fds = {
      .fd = shmem_fd, .events = POLLOUT, .revents = 0};

  DEBUG("Listening for events", "");
  while (1) {

    nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      FATAL("epoll_wait");
    }

    for (n = 0; n < nfds; n++) {

      DEBUG("Event 0x%x on fd %d", events[n].events, events[n].data.fd)

      /* Handle the new connection on the socket */
      if (events[n].events & EPOLLIN) {
        if (run_as_server && events[n].data.fd == server_socket) {
          conn_fd = accept(server_socket, (struct sockaddr *)&caddr, &len);
          if (conn_fd == -1) {
            FATAL("accept");
          }
          fcntl(conn_fd, F_SETFL, O_NONBLOCK);
          ev.events = EPOLLIN | EPOLLET | EPOLLHUP;
          ev.data.fd = conn_fd;
          if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_fd, &ev) == -1) {
            FATAL("epoll_ctl: conn_fd");
          }

          rv = poll(&my_buffer_fds, 1, SHMEM_POLL_TIMEOUT);
          if (rv < 0) {
            ERROR("shmem poll timeout", "");
          }

          if (my_buffer_fds.revents & ~POLLOUT) {
            ERROR("unexpected event on shmem_fd %d: 0x%x", shmem_fd,
                  my_buffer_fds.revents);
          }
          /* Send the connect request to the wayland peer */
          my_shm_data->cmd = CMD_CONNECT;
          my_shm_data->fd = conn_fd;
          ioctl(shmem_fd, SHMEM_IOCDORBELL,
                peer_vm_id | LOCAL_RESOURCE_READY_INT_VEC);
          DEBUG("Added client on fd %d", conn_fd);
        }

        /* Display/client side: received data from Wayland server. It needs to
          be sent to the peer (server) */
        else if (!run_as_server &&
                 get_remote_socket(events[n].data.fd, 0, 1) > 0) {

          int conn_fd = get_remote_socket(events[n].data.fd, 0, 1);
          DEBUG("get_remote_socket: %d", conn_fd);

          /* Wait for the memory buffer to be ready */
          DEBUG("Data from wayland. Waiting for shmem buffer", "");
          rv = poll(&my_buffer_fds, 1, SHMEM_POLL_TIMEOUT);
          if ((rv <= 0) || (my_buffer_fds.revents & ~POLLOUT)) {
            ERROR("unexpected event on shmem_fd %d: 0x%x poll=%d", shmem_fd,
                  my_buffer_fds.revents, rv);
          }

          DEBUG("Reading from wayland socket", "");
          len = read(events[n].data.fd, (void *)my_shm_data->data,
                     sizeof(my_shm_data->data));
          if (len <= 0) {
            ERROR("read from wayland socket failed fd=%d", events[n].data.fd);
          } else { /* TODO: verify this change */
            DEBUG("Read & sent %d bytes on fd#%d sent to %d", len,
                  events[n].data.fd, conn_fd);

            /* Send the data to the peer Wayland app server */
            my_shm_data->cmd = CMD_DATA;
            my_shm_data->fd = conn_fd;
            my_shm_data->len = len;
            ioctl(shmem_fd, SHMEM_IOCDORBELL,
                  peer_vm_id | LOCAL_RESOURCE_READY_INT_VEC);
          }
        } /* received data from Wayland server */

        /* Both sides: Received data from the peer via shared memory*/
        else if (events[n].data.fd == shmem_fd) {
          DEBUG("shmem_fd event: 0x%x cmd: %d remote fd: %d remote len: %d",
                events[n].events, peer_shm_data->cmd, peer_shm_data->fd,
                peer_shm_data->len);

          switch (peer_shm_data->cmd) {
          case CMD_RST:
            ERROR("Cmd RST received. Restarting.", "");
            return 1;
            break;
          case CMD_LOGIN:
            DEBUG("Received login request from 0x%x", peer_shm_data->fd);
            peer_vm_id = peer_shm_data->fd;
            ioctl(shmem_fd, SHMEM_IOCSETPEERID, peer_shm_data->fd);
            peer_shm_data->fd = -1;
            break;
          case CMD_DATA:
            conn_fd = run_as_server ? peer_shm_data->fd
                                    : map_peer_fd(peer_shm_data->fd, 0);
            DEBUG("shmem: received %d bytes for %d", peer_shm_data->len,
                  conn_fd);
            rv =
                write(conn_fd, (void *)peer_shm_data->data, peer_shm_data->len);
            if (rv != peer_shm_data->len) {
              ERROR("Wrote %d out of %d bytes on fd#%d", rv, peer_shm_data->len,
                    conn_fd);
            }
            DEBUG("Received data has been sent", "");
            break;
          case CMD_CONNECT:
            make_wayland_connection(peer_shm_data->fd);
            break;
          case CMD_CLOSE:
            if (run_as_server) {
              conn_fd = peer_shm_data->fd;
              DEBUG("Closing %d", conn_fd);
            } else {
              conn_fd = map_peer_fd(peer_shm_data->fd, 1);
              DEBUG("Closing %d peer fd=%d", conn_fd, peer_shm_data->fd);
            }
            if (conn_fd > 0) {
              if (epoll_ctl(epollfd, EPOLL_CTL_DEL, conn_fd, NULL) == -1) {
                ERROR("epoll_ctl: EPOLL_CTL_DEL", "");
              }
              close(conn_fd);
            }
          case -1:
          default:
            ERROR("Invalid CMD from peer!", "");
            break;
          } /* case peer_shm_data->cmd */

          /* Signal the other side that its buffer has been processed */
          DEBUG("Exec ioctl REMOTE_RESOURCE_CONSUMED_INT_VEC", "");
          peer_shm_data->cmd = -1;
          ioctl(shmem_fd, SHMEM_IOCDORBELL,
                peer_vm_id | REMOTE_RESOURCE_CONSUMED_INT_VEC);

        } /* End of "data arrived from the peer via shared memory" */

        else if (events[n].data.fd == server_socket) {
          ERROR("Ignored data from server socket", "");
        }

        /* Server side: Data arrived from connected waypipe server */
        else {
          /* Wait for the memory buffer to be ready */
          DEBUG("Data from client. Waiting for shmem buffer", "");
          rv = poll(&my_buffer_fds, 1, SHMEM_POLL_TIMEOUT);
          if (rv < 0) {
            ERROR("shmem poll for client fd=%d", events[n].data.fd);
          } else if (rv == 0) {
            ERROR("shmem poll timeout for client fd=%d", events[n].data.fd);
          }
          if (my_buffer_fds.revents & ~POLLOUT) {
            ERROR("unexpected event on shmem_fd %d: 0x%x poll=%d for client "
                  "fd=%d\n",
                  shmem_fd, my_buffer_fds.revents, rv, events[n].data.fd);
          }
          DEBUG("Reading from connected client #%d", events[n].data.fd);
          len = read(events[n].data.fd, (void *)my_shm_data->data,
                     sizeof(my_shm_data->data));
          if (len <= 0) {
            ERROR("read from connected client failed fd=%d", events[n].data.fd);
          } else {
            DEBUG("Read & sent %d bytes on fd#%d", len, events[n].data.fd);
            /* Send the data to the wayland display side */
            my_shm_data->cmd = CMD_DATA;
            my_shm_data->fd = events[n].data.fd;
            my_shm_data->len = len;
            ioctl(shmem_fd, SHMEM_IOCDORBELL,
                  peer_vm_id | LOCAL_RESOURCE_READY_INT_VEC);
          }
        } // End of "Data arrived from connected waypipe server"
      }

      /* Handling connection close */
      if (events[n].events & (EPOLLHUP | EPOLLERR)) {
        DEBUG("Closing fd#%d", events[n].data.fd);

        // Inform the peer that the closed is being closed
        rv = poll(&my_buffer_fds, 1, SHMEM_POLL_TIMEOUT);
        if (rv < 0) {
          ERROR("shmem poll timeout", "");
        }

        my_shm_data->cmd = CMD_CLOSE;
        if (run_as_server)
          my_shm_data->fd = events[n].data.fd;
        else {
          DEBUG("get_remote_socket: %d",
                get_remote_socket(events[n].data.fd, 0, 1));
          my_shm_data->fd = get_remote_socket(events[n].data.fd, 1, 1);
        }
        if (my_shm_data->fd > 0) {
          DEBUG("Sending close request for %d", my_shm_data->fd);
          ioctl(shmem_fd, SHMEM_IOCDORBELL,
                peer_vm_id | LOCAL_RESOURCE_READY_INT_VEC);
        }
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, events[n].data.fd, NULL) == -1) {
          ERROR("epoll_ctl: EPOLL_CTL_DEL", "");
        }
        close(events[n].data.fd);
      } /* Handling connection close */
    }
  } /* while(1) */
  return 0;
}

void print_usage_and_exit() {
  fprintf(stderr, usage_string);
  exit(1);
}
int main(int argc, char **argv) {

  int i, res;

  if (argc != 3)
    print_usage_and_exit();

  if (!strcmp(argv[1], "-c")) {
    run_as_server = 0;
  } else if (!strcmp(argv[1], "-s")) {
    run_as_server = 1;
  } else
    print_usage_and_exit();

  socket_path = argv[2];

  for (i = 0; i < FD_MAP_COUNT; i++) {
    fd_map[i].my_fd = -1;
    fd_map[i].remote_fd = -1;
  }

  epollfd = epoll_create1(0);
  if (epollfd == -1) {
    FATAL("server_init: epoll_create1");
  }

  shmem_init();

  if (run_as_server) {
    /* Create socket that waypipe can write to
     * Add the socket fd to the epollfd
     */
    server_init();

    /* Send LOGIN cmd to the client. Supply my_vmid
     *  TODO: Wait for reply?
     */
    peer_vm_id = vm_control->iv_client;
    my_shm_data->cmd = CMD_LOGIN;
    my_shm_data->fd = my_vmid;
    res = ioctl(shmem_fd, SHMEM_IOCDORBELL,
                vm_control->iv_client | LOCAL_RESOURCE_READY_INT_VEC);
    DEBUG("Client #%d: sent login vmid: 0x%x res=%d peer_vm_id=0x%x", 0,
          my_vmid, res, peer_vm_id);
  }

  run();

  return 0;
}
