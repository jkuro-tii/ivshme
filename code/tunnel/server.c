#include <arpa/inet.h>
#include <errno.h>
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

#define CLIENT_SOCKET_FN ("./client.sock")
#define SERVER_SOCKET_FN ("./server.sock")
#define CLIENT_TIMEOUT (1000)
#define SERVER_TIMEOUT (1000)

#define SHM_DEVICE_FN ("/dev/ivshmem")
#define SHMEM_IOC_MAGIC 's'

#define SHMEM_IOCWLOCAL _IOR(SHMEM_IOC_MAGIC, 1, int)
#define SHMEM_IOCWREMOTE _IOR(SHMEM_IOC_MAGIC, 2, int)
#define SHMEM_IOCIVPOSN _IOW(SHMEM_IOC_MAGIC, 3, int)
#define SHMEM_IOCDORBELL _IOR(SHMEM_IOC_MAGIC, 4, int)

#define REMOTE_RESOURCE_CONSUMED_INT_VEC (0)
#define LOCAL_RESOURCE_READY_INT_VEC (1)

#define MAX_EVENTS (1024)
#define BUFFER_SIZE (1024000)

#define SHMEM_BUFFER_SIZE (1024000 * 2)

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
typedef struct  {
  volatile int len;
  volatile unsigned char data[SHMEM_BUFFER_SIZE];
} volatile vm_data;

int epollfd;

int server_socket = -1, wayland_socket = -1, shmem_fd = -1;
int my_vmid = -1, peer_vm_id = -1, shmem_synced = 0;
vm_data *my_shm_data = NULL, *peer_shm_data = NULL;
int run_as_server = 0;

long int shmem_size;

struct {
  volatile int iv_server;
  volatile int iv_client;
  volatile vm_data server_data;
  volatile vm_data client_data;

  // volatile int server_data_len;
  // volatile unsigned char server_data[SHMEM_BUFFER_SIZE];
  // volatile int client_data_len;
  // volatile unsigned char client_data[SHMEM_BUFFER_SIZE];
} *vm_control;

void init_shmem_sync();

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

int get_shmem_size() {
  int res;

  res = lseek(shmem_fd, 0, SEEK_END);
  if (res < 0) {
    REPORT("seek", 1);
  }
  lseek(shmem_fd, 0, SEEK_SET);
  return res;
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

void shmem_test() {

  int timeout, res;
  unsigned int iv, data;
  unsigned int static counter = 0;
  struct pollfd fds = {
      .fd = shmem_fd, .events = POLLIN | POLLOUT, .revents = 0};

  // Ping
  // timeout = SERVER_TIMEOUT*0;
  // res = ioctl(shmem_fd, SHMEM_IOCWREMOTE, &timeout);
  // printf("SHMEM_IOCWREMOTE: %d\n", res);
  // Timeout?
  // Wait for pong
  init_shmem_sync();
  do {
    res = poll(&fds, 1, 0);
    if (fds.revents & POLLIN) {
      printf("POLLIN: ");

      data = peer_shm_data->len;
      peer_shm_data->len = -1;
      iv = peer_vm_id | REMOTE_RESOURCE_CONSUMED_INT_VEC;
      printf(" received %02x \n", data);
      usleep(random() % 3333333);
      res = ioctl(shmem_fd, SHMEM_IOCDORBELL, iv);
      if (res < 0) {
        REPORT("SHMEM_IOCDORBELL failed", 1);
      }
    }

    if (fds.revents & POLLOUT) {
      printf("POLLOUT");

      peer_shm_data->len = counter;
      iv = peer_vm_id | LOCAL_RESOURCE_READY_INT_VEC;
      printf(" sending %02x\n", counter);
      counter++;
      usleep(random() % 3333333);
      res = ioctl(shmem_fd, SHMEM_IOCDORBELL, iv);
      if (res < 0) {
        REPORT("SHMEM_IOCDORBELL failed", 1);
      }
    }

    // ?read -> wait, read
    // ?write -> wait, write

  } while (1);
}

void init_shmem_sync() {
  int timeout, res;
  unsigned int iv, data;
  unsigned int static counter = 0;
  struct pollfd fds = {
      .fd = shmem_fd, .events = POLLIN, .revents = 0};

  printf("Syncing ");
  do {
      usleep(random() % 3333333);
      printf(".");
      peer_vm_id = run_as_server ? vm_control->iv_client: vm_control->iv_server;
      iv = peer_vm_id;
      if (!iv)
        continue;
      iv |= LOCAL_RESOURCE_READY_INT_VEC;
      peer_shm_data->len = 0;
      res = ioctl(shmem_fd, SHMEM_IOCDORBELL, iv);
      if (res < 0) {
        REPORT("SHMEM_IOCDORBELL failed", 1);
      }
      res = poll(&fds, 1, 300);
      if ((res > 0) && (fds.revents & POLLIN))
        break;

  } while (1);
  printf(" done.\n");
}

int init_shmem_common() {
  int res = -1;

  printf("Waiting for devices setup...\n");
  sleep(1);

  /* Open shared memory */
  shmem_fd = open(SHM_DEVICE_FN, O_RDWR);
  if (shmem_fd < 0) {
    REPORT(SHM_DEVICE_FN, 1);
  }

  /* Get shared memory */
  shmem_size = get_shmem_size();
  if (shmem_size <= 0) {
    REPORT("No shared memory detected", 1);
  }
  vm_control = mmap(NULL, shmem_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_NORESERVE, shmem_fd, 0);
  if (!vm_control) {
    REPORT("Got NULL pointer from mmap", 1);
  }
  printf("Shared memory at address %p 0x%lx bytes\n", vm_control, shmem_size);

  if (run_as_server) {
    my_shm_data = &vm_control->server_data;
    peer_shm_data = &vm_control->client_data;
  } else {
    my_shm_data = &vm_control->client_data;
    peer_shm_data = &vm_control->server_data;
  }

  /* get my VM Id and store it */
  res = ioctl(shmem_fd, SHMEM_IOCIVPOSN, &my_vmid);
  if (res < 0) {
    REPORT("SHMEM_IOCIVPOSN failed", 1);
  }
  printf("My VM id = 0x%x running as a ", my_vmid);
  my_vmid = my_vmid << 16;
  if (run_as_server) {
    printf("server\n");
    vm_control->iv_server = my_vmid;
  } else {
    printf("client\n");
    vm_control->iv_client = my_vmid;
  }

  shmem_test();
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

      fprintf(stderr, "%d: ", __LINE__);
      fprintf(stderr, "Event 0x%x on fd %d\n", events[n].events,
              events[n].data.fd);

      if (events[n].events & (EPOLLHUP | EPOLLERR)) {
        fprintf(stderr, "%d: Closing fd#%d\n", __LINE__, events[n].data.fd);
        close(events[n].data.fd);
        continue;
      }
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

        if (events[n].data.fd == wayland_socket) {
          fprintf(stderr, "%d: %s\n", __LINE__, "Reading from wayland socket");
          // TODO read directly into shared memory
          len = read(events[n].data.fd, buffer, sizeof(buffer));

          if (len <= 0) {
            REPORT("read", 0);
            continue;
          }
          fprintf(stderr, "Read %d bytes on fd#%d\n", len, events[n].data.fd);

          // TODO: write directly from the share memory
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

          // TODO: read directly from the share memory
          len = read(events[n].data.fd, buffer, sizeof(buffer));
          if (len <= 0) {
            REPORT("read", 0);
            continue;
          }

          fprintf(stderr, "Read %d bytes on fd#%d\n", len, events[n].data.fd);
          // rv = write(current_client, buffer, len);
          // TODO: write directly from the share memory
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

int main(int argc, char **argv) {

  epollfd = epoll_create1(0);
  if (epollfd == -1) {
    REPORT("init_server: epoll_create1", 1);
  }

  // init_server();
  // init_wayland();

  if (argc > 1) {
    run_as_server = 1;
  }
  init_shmem_common();

  // run_server();

  return 0;
}
