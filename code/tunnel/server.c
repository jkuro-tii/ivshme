#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#define CLIENT_SOCKET_FN ("./client.sock")
#define SERVER_SOCKET_FN ("./server.sock")

#define MAX_EVENTS (1024)
#define ConversationLen (1024)
#define BuffSize (1024)

/* TODO: remove */
#define stderr stdout

#define FATAL(msg) { report(__FUNCTION__, __LINE__, msg, 1); }
#define LOG(msg) { report(__FUNCTION__, __LINE__, msg, 0); }

//#define DLOG(fmt/**/, args*/...) { fprintf(stderr, "%s:%d: ", __FUNCTION__, __LINE__); fprintf(stderr, fmt, __VA_OPT__(,/*args*/)) ; }

#define REPORT(msg, terminate) { report(__FUNCTION__, __LINE__, msg, terminate); }

struct epoll_event ev, events[MAX_EVENTS];
int epollfd;

int server_socket = -1, wayland_socket = -1;

void report(const char* where, int line, const char *msg, int terminate) {
  char tmp[256];
  sprintf(tmp, "%s:%d %s", where, line, msg);
  if(errno) {
    perror(tmp);
    if (terminate) 
      exit(-1);
  } else {
    fprintf(stderr, "%s\n", tmp);
  }
}


int send_to_client(void *buf, int len)
{
  return 0;
}

void run_client()
{

}

int init_server()
{
  struct sockaddr_un name;

  // Remove socket file if exists
  if (access(SERVER_SOCKET_FN, F_OK) == 0) {
    remove(SERVER_SOCKET_FN);
  } 
  server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket < 0) {
    REPORT("init_server: server socket", 1); /* terminate */
  }

  fprintf(stderr, "server socket: %d\n", server_socket);

  memset(&name, 0, sizeof(name));
  name.sun_family = AF_UNIX;
  strncpy(name.sun_path, SERVER_SOCKET_FN, sizeof(name.sun_path) - 1);
  if (bind(server_socket, (struct sockaddr *)&name,
			    sizeof(name)) < 0) {
    FATAL("bind");
  }

  if (listen(server_socket, MAX_EVENTS) < 0) /* listen for clients, up to MAX_EVENTS */
    FATAL("listen");

  ev.events = EPOLLIN;
  ev.data.fd = server_socket;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, server_socket, &ev) == -1) {
      FATAL("init_server: epoll_ctl: server_socket");
  }
  
  LOG("server initialized");
}

int init_wayland()
{
  
  struct sockaddr_un name;

  wayland_socket = socket(AF_UNIX, SOCK_STREAM, 0);
  if (wayland_socket < 0) {
    REPORT("init_server: mem socket", 1); /* terminate */
  }

  fprintf(stderr, "wayland socket: %d\n", wayland_socket);

  memset(&name, 0, sizeof(name));
  name.sun_family = AF_UNIX;
  strncpy(name.sun_path, CLIENT_SOCKET_FN, sizeof(name.sun_path) - 1);
  if (connect(wayland_socket, (struct sockaddr *)&name,
			    sizeof(name)) < 0) {
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

void do_use_fd(int fd)
{

}

void run_server()
{
  fd_set rfds;
  struct timeval tv;
  int conn_sock, rv, nfds, n, current_client = -1;
  struct sockaddr_un caddr; /* client address */
  int len = sizeof(caddr);  /* address length could change */
  char buffer[BuffSize + 1];

  fprintf(stderr, "Listening for clients...\n");
  int count;
  while (1) {

    nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      FATAL("epoll_wait");
    }


    for (n = 0; n < nfds; n++) {

      if (events[n].data.fd == server_socket) {
          conn_sock = accept(server_socket,
                            (struct sockaddr *) &caddr, &len);
          if (conn_sock == -1) {
              FATAL("accept");
          }
          fcntl(conn_sock, F_SETFL, O_NONBLOCK);
          ev.events = EPOLLIN | EPOLLET | EPOLLHUP;
          ev.data.fd = conn_sock;
          if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_sock,
                      &ev) == -1) {
              FATAL("epoll_ctl: conn_sock");
          }
          fprintf(stderr, "%d: Added client on fd %d ", __LINE__,  conn_sock);
      } 
      
      else {
        fprintf(stderr, "%d: ", __LINE__);
        fprintf(stderr, "Event 0x%x on fd %d\n", events[n].events, events[n].data.fd);  

        if (events[n].data.fd == wayland_socket) {
          fprintf(stderr, "%d: %s\n", __LINE__, "wayland socket");

          len = read(events[n].data.fd, buffer, sizeof(buffer));

          if (len <= 0) {
            REPORT("read", 0);
            continue;
          } 
          fprintf(stderr, "Read %d bytes on fd#%d\n", len, events[n].data.fd);
          rv = write(current_client, buffer, len);
          if (rv != len) {
            fprintf(stderr, "Wrote %d out of %d bytes on fd#%d\n", rv, n, events[n].data.fd);
          }

        } else if (events[n].data.fd == server_socket) {
          fprintf(stderr, "%d: %s\n", __LINE__, "server socket");  
        } else { // connected client event
          fprintf(stderr, "%d: %s\n", __LINE__, "Connected client");
          current_client = events[n].data.fd;
          len = read(events[n].data.fd, buffer, sizeof(buffer));
          
          if (len <= 0) {
            REPORT("read", 0);
            continue;
          }

          fprintf(stderr, "Read %d bytes on fd#%d\n", len, events[n].data.fd);
          rv = write(wayland_socket, buffer, len);
          if (rv != len) {
            fprintf(stderr, "Wrote %d out of %d bytes on fd#%d\n", rv, len, events[n].data.fd);
          }

        }
      }
  }





#if 0
    printf("accept...\n");
    int client_fd = accept(server_socket, (struct sockaddr*) &caddr, &len);  /* accept blocks */
    if (client_fd < 0) {
      REPORT("accept", 0); /* don't terminate, though there's a problem */
      continue;
    }

    printf("fd=%d\n", client_fd);
    /* read from client */
    int i;
    for (i = 0; i < 10; i++) {
      char buffer[BuffSize + 1];
      memset(buffer, '\0', sizeof(buffer));

      FD_ZERO(&rfds);
      FD_SET(client_fd, &rfds);

      tv.tv_sec = 1;
      tv.tv_usec = 0;

      // count = read(client_fd, buffer, sizeof(buffer));
      count = recv(client_fd, buffer, sizeof(buffer), 0);
      // if (!count)
      //   close(client_fd);
      printf("read %d bytes\n", count);

      if(!count) {
        int retval;
        retval = select(1, &rfds, NULL, NULL, &tv);
        printf("select: %d\n", retval);
      }
      rv = send_to_client(buffer, count);
     
    }
    // close(client_fd); /* break connection */
#endif
  }  /* while(1) */

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
