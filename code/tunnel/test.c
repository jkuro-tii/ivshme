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
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/time.h>
#include <execinfo.h>

#define CLIENT_SOCKET_FN ("./client.sock")
#define SERVER_SOCKET_FN ("./server.sock")

/* TODO: remove */
//#define stderr stdout

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

int server_socket = -1;
int run_as_server = 0;
clock_t cpu_test_time_start, cpu_test_time_total;
double real_time_start_msec, real_time_total_msec;
struct timeval time_start_s, time_total_s;


void report(const char *msg, int terminate) {
  char tmp[256];
  fprintf(stderr, "%s", msg);
  if (errno)
      perror(tmp);
  if (terminate)
    exit(-1);
}

int server_init() {
  struct sockaddr_un my_socket;

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

  memset(&my_socket, 0, sizeof(my_socket));
  my_socket.sun_family = AF_UNIX;
  strncpy(my_socket.sun_path, SERVER_SOCKET_FN,
          sizeof(my_socket.sun_path) - 1);
  if (bind(server_socket, (struct sockaddr *)&my_socket,
           sizeof(my_socket)) < 0) {
    FATAL("bind");
  }

  if (listen(server_socket, 1) < 0)
    FATAL("listen");

  LOG("server initialized", "");
}

void time_start()
{
  cpu_test_time_start = clock();
  gettimeofday(&time_start_s, NULL);
  real_time_start_msec = 1000.0*time_start_s.tv_sec + (double)time_start_s.tv_usec/1000.0;
}
void time_end(long int bytes) 
{
  cpu_test_time_total = (clock() - cpu_test_time_start)*1000/CLOCKS_PER_SEC;
  gettimeofday(&time_total_s, NULL);
  real_time_total_msec = 1000.0*time_total_s.tv_sec + (double)time_total_s.tv_usec/1000.0;
  real_time_total_msec -= real_time_start_msec;
  
  printf("real_time_total_msec=%f cpu_test_time_total=%ld\n", real_time_total_msec, cpu_test_time_total);

  printf("CPU time: %f Real time: %fms Data received: %ld MiB\n",
  (double) cpu_test_time_total, (double)real_time_total_msec, bytes/1024/1024);

  printf("I/O rate: %.2f MB/s realtime: %.2f MiB/s\n", (double)bytes/1024/1024/cpu_test_time_total,
              (double)bytes*1000/1024/1024/real_time_total_msec); 
}

int main(int argc, char**argv)
{
  if (argc == 1)
    run_as_server = 1;
  int bufsize = 1024*1024;
  int count = 1024*10;
  int i;
  long int total_bytes = 0;
  unsigned char *buf = NULL;
  int client_fd;
  struct sockaddr_un caddr; /* client address */
  int len = sizeof(caddr);  /* address length could change */
  struct sockaddr_un my_socket;
  int socket_fd;

  buf = malloc(bufsize);
  socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    printf("socket");
    exit(-1);
  }
  memset(&my_socket, 0, sizeof(my_socket));
  my_socket.sun_family = AF_UNIX;

  if (!run_as_server) {
    strncpy(my_socket.sun_path, SERVER_SOCKET_FN,
            sizeof(my_socket.sun_path) - 1);

    if (connect(socket_fd, (struct sockaddr *)&my_socket,
                sizeof(my_socket)) < 0) {
      printf("connect");
      exit(-1);
    }
    printf("Sending %d bytes %d times. Total %ld bytes.\n", bufsize, count, (long int)bufsize*count);
    for (i = 0; i < count; i++) {
      send(socket_fd, buf, bufsize, 0);
    }

    close(socket_fd);
  }
  else {
    // Remove socket file if exists
    if (access(CLIENT_SOCKET_FN, F_OK) == 0) {
      remove(CLIENT_SOCKET_FN);
    }
    strncpy(my_socket.sun_path, CLIENT_SOCKET_FN,
            sizeof(my_socket.sun_path) - 1);
    if (bind(socket_fd, (struct sockaddr *)&my_socket,
            sizeof(my_socket)) < 0) {
      FATAL("bind");
    }

    if (listen(socket_fd, 1) < 0)
      FATAL("listen");

    LOG("test: server initialized", "");

    client_fd = accept(socket_fd, (struct sockaddr *)&caddr, &len);
    printf("test: client fd=%d\n", client_fd);
    time_start();
    do {

      i = read(client_fd, buf, bufsize);
      if (i <= 0) {
        time_end(total_bytes);
        printf("Read %ld bytes.\n", total_bytes);
        exit(0);
      }
      total_bytes += i;
    } while(1);
  }
  return 0;
}