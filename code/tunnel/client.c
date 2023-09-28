#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
//#include "sock.h"

#define MaxConnects (3)
#define ConversationLen (1024)
#define BuffSize (1024)

void report(const char* msg, int terminate) {
  perror(msg);
  if (terminate) exit(-1); /* failure */
}

int main() {

  char *filename = "./socket";
  struct sockaddr_un addr;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) report("socket", 1); /* terminate */
/*
       int connect(int sockfd, const struct sockaddr *addr,
                   socklen_t addrlen);
*/

  addr.sun_family = AF_FILE;
  strcpy(addr.sun_path, filename);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)))
    report("connect", 1); /* terminate */

  send(fd, filename, strlen(filename), 0);
  sleep(10);
  send(fd, filename, strlen(filename), 0);
  sleep(1000000);
  return 0;
}
