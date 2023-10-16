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
#include <execinfo.h>

#define CLIENT_SOCKET_FN ("./client.sock")
#define SERVER_SOCKET_FN ("./server.sock")
#define CLIENT_TIMEOUT (1000)
#define SERVER_TIMEOUT (1000)

#define SHM_DEVICE_FN "/dev/ivshmem"
#define SHMEM_IOC_MAGIC 's'

#define SHMEM_IOCWLOCAL _IOR(SHMEM_IOC_MAGIC, 1, int)
#define SHMEM_IOCWREMOTE _IOR(SHMEM_IOC_MAGIC, 2, int)
#define SHMEM_IOCIVPOSN _IOW(SHMEM_IOC_MAGIC, 3, int)
#define SHMEM_IOCDORBELL _IOR(SHMEM_IOC_MAGIC, 4, int)
#define SHMEM_IOCRESTART _IOR(SHMEM_IOC_MAGIC, 5, int)

#define REMOTE_RESOURCE_CONSUMED_INT_VEC (0)
#define LOCAL_RESOURCE_READY_INT_VEC (1)

#define MAX_EVENTS (1024)
#define MAX_CLIENTS (100)
#define BUFFER_SIZE (1024000)
#define SHMEM_POLL_TIMEOUT (300)
#define SHMEM_BUFFER_SIZE (1024000 * 2)

/* TODO: remove */
#define stderr stdout

#define LOG(fmt, ...) \
{ char tmp1[256], tmp2[256]; \
  sprintf(tmp2, fmt, __VA_ARGS__); \
  sprintf(tmp1, "%s:%d: %s\n", __FUNCTION__, __LINE__, tmp2); \
  report(tmp1, 0); \
} 

#define ERROR(fmt, ...) \
{ char tmp1[256], tmp2[256]; \
  sprintf(tmp2, fmt, __VA_ARGS__); \
  sprintf(tmp1, "%s:%d: %s\n", __FUNCTION__, __LINE__, tmp2); \
  report(tmp1, 0); \
} 

#define FATAL(msg) \
{ char tmp1[256], tmp2[256]; \
  sprintf(tmp2, msg); \
  sprintf(tmp1, "%s:%d: %s\n", __FUNCTION__, __LINE__, tmp2); \
  report(tmp1, 1); \
} 

enum {
  CMD_CONNECT, CMD_DATA, CMD_CLOSE, CMD_SYNC
};
#define FD_MAP_COUNT (sizeof(fd_map)/sizeof(fd_map[0]))
struct {
  int my_fd;
  int remote_fd;
} fd_map[MAX_CLIENTS];

struct epoll_event ev, events[MAX_EVENTS];
typedef struct  {
  volatile int cmd;
  volatile int fd;
  volatile int len;
  volatile unsigned char data[SHMEM_BUFFER_SIZE];
} vm_data;

int epollfd;

int server_socket = -1, shmem_fd = -1;
int my_vmid = -1, peer_vm_id = -1, shmem_synced = 0;
vm_data *my_shm_data = NULL, *peer_shm_data = NULL;
int run_as_server = 0;

long int shmem_size;

struct {
  volatile int iv_server;
  volatile int iv_client;
  vm_data server_data;
  vm_data client_data;
} *vm_control;

void shmem_sync();

void report(const char *msg, int terminate) {
  char tmp[256];
  fprintf(stderr, "%s", msg);
  if (errno)
      perror(tmp);
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
  if (access(SERVER_SOCKET_FN, F_OK) == 0) {
    remove(SERVER_SOCKET_FN);
  }
  server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket < 0) {
    FATAL("server_init: server socket < 0");
  }
  
  //  fprintf(stderr, "%s:%d: server socket: %d\n", __FILE__, __LINE__, server_socket);          
  LOG("server socket: %d", server_socket);          

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
    FATAL("server_init: epoll_ctl: server_socket");
  }

  LOG("server initialized", "");
}

int wayland_connect() {

  struct sockaddr_un socket_name;
  int wayland_fd;

  wayland_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (wayland_fd < 0) {
    FATAL("wayland socket");
  }

  fprintf(stderr, "%s:%d: wayland socket: %d\n", __FILE__, __LINE__, wayland_fd);          

  memset(&socket_name, 0, sizeof(socket_name));
  socket_name.sun_family = AF_UNIX;
  strncpy(socket_name.sun_path, CLIENT_SOCKET_FN,
          sizeof(socket_name.sun_path) - 1);
  if (connect(wayland_fd, (struct sockaddr *)&socket_name,
              sizeof(socket_name)) < 0) {
    FATAL("connect");
  }

  ev.events = EPOLLIN;
  ev.data.fd = wayland_fd;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, wayland_fd, &ev) == -1) {
    FATAL("epoll_ctl: wayland_fd");
  }

  LOG("client side initialized", "");
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

  fprintf(stderr, "FAILED fd#%d\n", peer_fd);
  FATAL("fd_map table full");
}

int get_wayland_fd(int peer_fd, int close) {

  int i, rv;

  for (i = 0; i < FD_MAP_COUNT; i++) {
    if (fd_map[i].remote_fd == peer_fd) {
      rv = fd_map[i].my_fd;
      if (close)
        fd_map[i].my_fd = -1;
      return rv;
    }
  }
  fprintf(stderr, "FAILED fd#%d\n", peer_fd);
  FATAL("peer fd  not found");
  return -1;
}

int get_remote_socket(int my_fd, int close, int ignore_error) {

  int i;

  for (i = 0; i < FD_MAP_COUNT; i++) {
    if (fd_map[i].my_fd == my_fd) {
      if (close)
        fd_map[i].my_fd = -1;
      return fd_map[i].remote_fd;
    }
  }
  if (ignore_error)
    return -1;

  FATAL("my fd not found");
  return -1;
}


void shmem_test() {

  int timeout, res;
  unsigned int iv, data;
  unsigned int counter;
  struct pollfd fds = {
      .fd = shmem_fd, .events = POLLIN | POLLOUT, .revents = 0};

  shmem_sync();

  counter = my_vmid;

  fprintf(stderr, "%s:%d my_vmid=0x%x my_shm_data=%p\n", __FILE__, __LINE__, my_vmid, my_shm_data);
  do {
    res = poll(&fds, 1, SHMEM_POLL_TIMEOUT);
    if (res && (fds.revents & POLLIN)) {
      data = my_shm_data->len;
      my_shm_data->len = -1;
      iv = peer_vm_id | REMOTE_RESOURCE_CONSUMED_INT_VEC;
      fprintf(stderr, "%s:%d received %02x \n", __FILE__, __LINE__, data);
      usleep(random() % 3333333);
      res = ioctl(shmem_fd, SHMEM_IOCDORBELL, iv);
      if (res < 0) {
        FATAL("SHMEM_IOCDORBELL failed");
      }
    }

    if (res && (fds.revents & POLLOUT)) {
      fprintf(stderr, "%s:%d POLLOUT", __FILE__, __LINE__);

      peer_shm_data->len = counter;
      iv = peer_vm_id | LOCAL_RESOURCE_READY_INT_VEC;
      fprintf(stderr, "%s:%d sending %02x\n", __FILE__, __LINE__, counter);
      counter++;
      usleep(random() % 3333333);
      res = ioctl(shmem_fd, SHMEM_IOCDORBELL, iv);
      if (res < 0) {
        FATAL("SHMEM_IOCDORBELL failed");
      }
    }

    // ?read -> wait, read
    // ?write -> wait, write

  } while (1);
}

void shmem_sync() {
  int timeout, res;
  unsigned int iv, data;
  unsigned int static counter = 0;
  struct pollfd fds = {
      .fd = shmem_fd, .events = POLLIN, .revents = 0};

  if (run_as_server)
    vm_control->iv_client = 0;
  else
    vm_control->iv_server = 0;

  fprintf(stderr, "%s:%d Syncing...\n", __FILE__, __LINE__);
  do {
      usleep(random() % 333333);
      if (run_as_server) {
        vm_control->iv_server = my_vmid;
        peer_vm_id = vm_control->iv_client;
      }
      else {
        vm_control->iv_client = my_vmid;
        peer_vm_id = vm_control->iv_server;
      }
      iv = peer_vm_id;
      if (!iv)
        continue;
      iv |= LOCAL_RESOURCE_READY_INT_VEC;
      peer_shm_data->len = 0;
      ioctl(shmem_fd, SHMEM_IOCDORBELL, iv);
      res = poll(&fds, 1, SHMEM_POLL_TIMEOUT);
      if ((res > 0) && (fds.revents & POLLIN))
        break;
  } while (1);

  ioctl(shmem_fd, SHMEM_IOCRESTART, 0x5555);
  fprintf(stderr, "%s:%d done\n", __FILE__, __LINE__);
}

int shmem_init() {
  int res = -1;

  fprintf(stderr, "%s:%d Waiting for devices setup...\n", __FILE__, __LINE__);
  sleep(1);

  /* Open shared memory */
  shmem_fd = open(SHM_DEVICE_FN, O_RDWR);
  if (shmem_fd < 0) {
    FATAL("Open "SHM_DEVICE_FN);
  }
  LOG("shared memory fd: %d", shmem_fd);

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
  LOG("Shared memory at address %p 0x%lx bytes", vm_control, shmem_size);

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
  LOG("My VM id = 0x%x running as a ", my_vmid);
  my_vmid = my_vmid << 16;
  if (run_as_server) {
    LOG("server", "");
    vm_control->iv_server = my_vmid;
  } else {
    LOG("client", "");
    vm_control->iv_client = my_vmid;
  }
  
  // shmem_test();
  shmem_sync();

  // fcntl(shmem_fd, F_SETFL, O_NONBLOCK);
  ev.events = EPOLLIN;
  ev.data.fd = shmem_fd;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, shmem_fd, &ev) == -1) {
    FATAL("epoll_ctl: -1");
  }
  ioctl(shmem_fd, SHMEM_IOCRESTART, 0x5554);

  LOG("shared memory initialized", "");

  return 0;
}


void run() {
  fd_set rfds;
  struct timeval tv;
  int conn_fd, rv, nfds, n;
  struct sockaddr_un caddr; /* client address */
  int len = sizeof(caddr);  /* address length could change */
  char buffer[BUFFER_SIZE + 1];
  struct pollfd my_buffer_fds = {
    .fd = shmem_fd, .events = POLLOUT, .revents = 0};


  LOG("Listening for events", "");
  int count;
  while (1) {

    nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      FATAL("epoll_wait");
    }

    for (n = 0; n < nfds; n++) {

      LOG("Event 0x%x on fd %d", events[n].events, events[n].data.fd)

      if (events[n].events & (EPOLLHUP | EPOLLERR)) {
        LOG("Closing fd#%d", events[n].data.fd);
        if (epoll_ctl(epollfd, EPOLL_CTL_DEL, events[n].data.fd, NULL) == -1) {
          FATAL("epoll_ctl: EPOLL_CTL_DEL");
        }

        close(events[n].data.fd);
        // Inform the peer that the socket has been closed
        rv = poll(&my_buffer_fds, 1, SHMEM_POLL_TIMEOUT);
        if (rv < 0) {
          ERROR("shmem poll timeout", "");
        }
        my_shm_data->cmd = CMD_CLOSE;
        if (run_as_server)
          my_shm_data->fd =  events[n].data.fd;
        else {
          LOG("get_remote_socket: %d", get_remote_socket(events[n].data.fd, 1, 1));
          my_shm_data->fd = get_remote_socket(events[n].data.fd, 1, 0);
        }
        LOG("Sending close request for %d", my_shm_data->fd);          
        ioctl(shmem_fd, SHMEM_IOCDORBELL, peer_vm_id|LOCAL_RESOURCE_READY_INT_VEC);
        continue;
      }
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

        if (my_buffer_fds.revents ^ POLLOUT) {
          ERROR("unexpected event on shmem_fd %d: 0x%x", shmem_fd, 
                    my_buffer_fds.events); 
        }
        // Send connect request to the wayland peer
        my_shm_data->cmd = CMD_CONNECT;
        my_shm_data->fd =  conn_fd;
        ioctl(shmem_fd, SHMEM_IOCDORBELL, peer_vm_id|LOCAL_RESOURCE_READY_INT_VEC);
        LOG("Added client on fd %d", conn_fd);

      } else 
      if (!run_as_server && get_remote_socket(events[n].data.fd, 0, 1) > 0) {
        int remote_fd = get_remote_socket(events[n].data.fd, 0, 1);
        LOG("get_remote_socket: %d", remote_fd);

        /* Wait for the memory buffer to be ready */
        LOG("Data from wayland. Waiting for shmem buffer", "");
        poll(&my_buffer_fds, 1, SHMEM_POLL_TIMEOUT);
        if (my_buffer_fds.revents ^ POLLOUT) {
          fprintf(stderr,"%s:%d: unexpected event on shmem_fd %d: 0x%x\n", __FILE__, __LINE__, shmem_fd, 
                    my_buffer_fds.events); 
        }
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, "Reading from wayland socket");
        len = read(events[n].data.fd, (void*)my_shm_data->data, sizeof(my_shm_data->data));
        if (len <= 0) {
          ERROR("read", "");
          continue;
        }
        fprintf(stderr, "%s:%d:Read & sent %d bytes on fd#%d sending to %d\n", __FILE__, __LINE__, len, events[n].data.fd,
          remote_fd);

        // Send the data to the server
        my_shm_data->cmd = CMD_DATA;
        my_shm_data->fd =  remote_fd;
        my_shm_data->len = len;
        ioctl(shmem_fd, SHMEM_IOCDORBELL, peer_vm_id|LOCAL_RESOURCE_READY_INT_VEC);

      } else 
      if (events[n].data.fd == shmem_fd) { // Data arrived from the peer via shared memory
        LOG("shmem_fd event: 0x%x cmd: %d remote fd: %d", events[n].events, peer_shm_data->cmd, peer_shm_data->fd);
        if (peer_shm_data->cmd == -1) {
          ERROR("INVALID CMD!", "");
        } else 
        if (peer_shm_data->cmd == CMD_DATA) {
          n = run_as_server ? peer_shm_data->fd : get_wayland_fd(peer_shm_data->fd, 0);
          LOG("shmem: received %d bytes for %d", peer_shm_data->len, n);
          rv = write(n, (void*)peer_shm_data->data, peer_shm_data->len);
          if (rv != peer_shm_data->len) {
            ERROR("Wrote %d out of %d bytes on fd#%d", rv, peer_shm_data->len, n);
          }
          LOG("Received data sent", "");
        }
        else if (peer_shm_data->cmd == CMD_CONNECT) {
          make_wayland_connection(peer_shm_data->fd);
        }
        else if (peer_shm_data->cmd == CMD_CLOSE) {
          if (run_as_server) {
            LOG("Closing %d", peer_shm_data->fd);
            close(peer_shm_data->fd);
          }
          else {
            int fd = get_wayland_fd(peer_shm_data->fd, 1);
            LOG("Closing %d peer fd=%d", fd, peer_shm_data->fd);
            close(fd);
          }
        }
        LOG("Exec ioctl REMOTE_RESOURCE_CONSUMED_INT_VEC", "");
        peer_shm_data->cmd = -1;
        ioctl(shmem_fd, SHMEM_IOCDORBELL, peer_vm_id|REMOTE_RESOURCE_CONSUMED_INT_VEC);
      } 
      else if (events[n].data.fd == server_socket) {
        LOG("readserver socket", "");
      } 
        
      else { // Data arrived from connected client
        /* Wait for the memory buffer to be ready */
        LOG("Data from client. Waiting for shmem buffer", "");
        rv = poll(&my_buffer_fds, 1, SHMEM_POLL_TIMEOUT);
        if (rv < 0) {
          ERROR("shmem poll timeout", "");
        }
        if (my_buffer_fds.revents ^ POLLOUT) {
          ERROR("unexpected event on shmem_fd %d: 0x%x\n", shmem_fd, 
                    my_buffer_fds.events); 
        }

        ERROR("Reading from connected client #%d", events[n].data.fd);
        len = read(events[n].data.fd, (void*)my_shm_data->data, sizeof(my_shm_data->data));
        if (len <= 0) {
          ERROR("read", "");
          continue;
        }
        LOG("Read %d bytes on fd#%d", len, events[n].data.fd);
        my_shm_data->cmd = CMD_DATA;
        my_shm_data->fd = events[n].data.fd;
        my_shm_data->len = len;
        ioctl(shmem_fd, SHMEM_IOCDORBELL, peer_vm_id|LOCAL_RESOURCE_READY_INT_VEC);
      }
    }

  } /* while(1) */
}

int main(int argc, char **argv) {

  int i;
  
  for (i = 0; i < FD_MAP_COUNT; i++) {
    fd_map[i].my_fd = -1;
    fd_map[i].remote_fd = -1;
  }

  epollfd = epoll_create1(0);
  if (epollfd == -1) {
    FATAL("server_init: epoll_create1");
  }

  if (argc > 1) {
    run_as_server = 1;
  }

  shmem_init();

  if(run_as_server)
    server_init();

  run();

  return 0;
}
