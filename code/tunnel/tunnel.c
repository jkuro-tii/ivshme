/*
 * SPDX-FileCopyrightText: 2022 TII
*/
#include <stdio.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>

#define PMEM_DEVICE "/dev/ivshmem"
#define IOCTL_WAIT_IRQ_LOCAL     (0)
#define IOCTL_WAIT_IRQ_REMOTE    (1)
#define IOCTL_READ_IV_POSN       (3)
#define IOCTL_DOORBELL           (4)

#define LOCAL_RESOURCE_INT_VEC  (0)
#define REMOTE_RESOURCE_INT_VEC (1)

#define MB (1048576)
#define TEST_LOOPS (500)

int pmem_fd = -1; 
void *pmem_ptr;
long int pmem_size;
volatile int *test_pmem;
long test_mem_size = 0;
static int memtest(unsigned int data, int verify);
clock_t cpu_test_time_start;
double real_time_start_msec;
unsigned int vm_id = 0;

struct
{
  volatile int iv_server;
  volatile int iv_client;
  volatile int data;
  volatile int shutdown;
} volatile *vm_control;

int get_pmem_size()
{
    int res;

    res = lseek(pmem_fd, 0 , SEEK_END);
    if (res < 0) 
    {
      perror(PMEM_DEVICE);
      return res;
    }
    lseek(pmem_fd, 0 , SEEK_SET);
    return res;
}

int is_server()
{
  char buf[1024];

  FILE *f = fopen("/proc/cmdline", "r");
  if (f)
  {
    fread(buf, sizeof(buf), 1, f);
    fclose(f);
    if (strstr(buf, "memtest_server"))
      return 1;
    return 0;
  }
  printf("/proc/cmdline cannot be open. Exiting.\n");
  exit(-1);
  return 1;
}

void hexdump(volatile void *mem, int size)
{
  for(int i = 0; i < size; i++)
  {
    printf("%02x ", *(unsigned char*) (mem + i));
  }
  printf("\n");
}

void print_report(double cpu_time_s, double real_time_s, long int data_written, long int data_read, long int int_counter)
{
  cpu_time_s /= 1000.0;
  real_time_s /= 1000.0;

  printf("CPU time: %.0fs Real time: %.0fs Read: %ld MB Written: %ld MB\n", 
    cpu_time_s, real_time_s, data_read/MB, data_written/MB);
  printf("I/O rate: read: %.2f MB/s write: %.2f MB/s R&W: %.2f MB/s        Total I/O in realtime: %.2f MB/s\n",
    (double)data_read/MB/cpu_time_s, (double)data_written/MB/cpu_time_s, (double)(data_read+data_written)/MB/cpu_time_s, 
    (double)(data_read+data_written)/MB/real_time_s);
  printf("Interrupts per second: CPU time: %.2f realtime %.2f\n", (double)int_counter/cpu_time_s, (double)int_counter/real_time_s);
}

void proc_server()
{
  int res;

  vm_control->iv_server = vm_id;
  vm_control->shutdown = 0;

  printf("Server: Ready. Id = 0x%lx\n", (vm_id >> 16));
  do
  {
    // Wait for external data
    printf("IOCTL_WAIT_IRQ_REMOTE\n");
    res = ioctl(pmem_fd, IOCTL_WAIT_IRQ_REMOTE);
    if (res < 0) {
      printf("%s:%d: IOCTL_WAIT_IRQ_REMOTE failed\n", __FILE__, __LINE__);
      exit(1);
    }
    memtest(vm_control->data, 0);
    // Release external data
    printf("IOCTL_DOORBELL REMOTE_RESOURCE_INT_VEC\n");
    res = ioctl(pmem_fd, IOCTL_DOORBELL, vm_control->iv_client | REMOTE_RESOURCE_INT_VEC);
    if (res < 0) {
      printf("IOCTL_DOORBELL to server failed\n");
      exit(1);
    }

    // Signal that task has been finished
    #ifdef DEBUG
    printf("Server: Task has been finished.\n");
    #endif
    printf("IOCTL_WAIT_IRQ_LOCAL\n");
    res = ioctl(pmem_fd, IOCTL_WAIT_IRQ_LOCAL);
    if (res < 0) {
      printf("%s:%d: IOCTL_WAIT_IRQ_LOCAL failed\n", __FILE__, __LINE__);
      exit(1);
    }
    printf("IOCTL_DOORBELL LOCAL_RESOURCE_INT_VEC\n");
    res = ioctl(pmem_fd, IOCTL_DOORBELL, vm_control->iv_client | LOCAL_RESOURCE_INT_VEC);
    if (res < 0) {
      printf("IOCTL_DOORBELL to server failed\n");
      exit(1);
    }
    printf("Loop...\n");
    
  } while(!vm_control->shutdown);
}

void proc_client()
{
  int res;
  // memset((void*)vm_control, 0, sizeof(*vm_control));
  // store VM Id
  vm_control->iv_client = vm_id;

  do
  {
    #ifdef DEBUG
    printf("Client: Starting the server.\n");
    #endif

    printf("IOCTL_WAIT_IRQ_LOCAL\n");
    // Hold local data token
    res = ioctl(pmem_fd, IOCTL_WAIT_IRQ_LOCAL);
    if (res < 0) {
      printf("IOCTL_WAIT_IRQ_LOCAL to server failed\n");
      exit(1);
    }
    printf("IOCTL_DOORBELL LOCAL_RESOURCE_INT_VEC\n");
    vm_control->data = rand();
    // Release local data
    res = ioctl(pmem_fd, IOCTL_DOORBELL, vm_control->iv_server | LOCAL_RESOURCE_INT_VEC);
    if (res < 0) {
      printf("IOCTL_DOORBELL to server failed\n");
      exit(1);
    }

    printf("IOCTL_WAIT_IRQ_REMOTE\n");
    // Wait for server completion
    res = ioctl(pmem_fd, IOCTL_WAIT_IRQ_REMOTE);
    if (res < 0) {
      printf("%s:%d: IOCTL_WAIT_IRQ_REMOTE failed\n", __FILE__, __LINE__);
      exit(1);
    }

    #ifdef DEBUG
    printf("Client: task done. Verifying.\n");
    #endif
    memtest(vm_control->data, 1);
    // Release remote data
    printf("IOCTL_DOORBELL REMOTE_RESOURCE_INT_VEC\n");
    res = ioctl(pmem_fd, IOCTL_DOORBELL, vm_control->iv_server | REMOTE_RESOURCE_INT_VEC);
    if (res < 0) {
      printf("IOCTL_DOORBELL to server failed\n");
      exit(1);
    }
    printf("Loop...\n");
  } while(!vm_control->shutdown);
}


int memtest(unsigned int data, int verify)
{
  static long int read_counter = 0, write_counter = 0, int_counter = 0, report_counter = 0;
  int ret_val = 0;
  struct timeval current_time;
  static double cpu_time_ms = 0.0;
  double real_time_ms;

  int_counter++;
  if(!verify) 
  {
    for(int i = 0; i < TEST_LOOPS; i++)
    {
      for(int n = 0; n < test_mem_size; n++)
      {
        write_counter++;
        test_pmem[n] = (n ^ data);
      }
    }
  } 
  else
  {
    ret_val = 0;
    for(int i = 0; i < TEST_LOOPS; i++)
    {
      for(unsigned int n = 0; n < test_mem_size; n++)
      {   
        if (test_pmem[n] != (n ^ data)) 
        {
          printf("-----------> memtest error at addr %p %d 0x%x 0x%x \n", &test_pmem[n],
            n, test_pmem[n], (n ^ data));
          ret_val = 1;
          vm_control->shutdown = 1;
          goto exit;
        }
        read_counter++;
      }
    }
  }

  exit:
    if (++report_counter % 50000) {
      return ret_val;
    }

    report_counter = 0;

    printf("read_counter=%ld write_counter=%ld int_counter=%ld\n", read_counter, write_counter, int_counter);

    cpu_time_ms = (double)(clock() - cpu_test_time_start) / CLOCKS_PER_SEC * 1000;
    gettimeofday(&current_time, NULL);
    real_time_ms = 1000.0*current_time.tv_sec + (double)current_time.tv_usec/1000.0 - real_time_start_msec;

    print_report(cpu_time_ms, real_time_ms, write_counter*sizeof(int), read_counter*sizeof(int), int_counter);
  return ret_val;
}

int main(int argc, char**argv )
{
  struct timeval time_start;
  int res;

  printf("Waiting for devices setup...\n");
  sleep(1);

  /* Open shared memory */
  pmem_fd = open(PMEM_DEVICE, O_RDWR);
  if (pmem_fd < 0)
  {
    perror(PMEM_DEVICE);
    goto exit_error;
  }  

  /* Get shared memory */
  pmem_size = get_pmem_size();
  if (pmem_size <= 0)
  {
    printf("No shared memory detected.\n");
    goto exit_close; 
  }
  pmem_ptr = mmap(NULL, pmem_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_NORESERVE, pmem_fd, 0);
  if (!pmem_ptr)
  {
    printf("Got NULL pointer from mmap.\n");
    goto exit_close;
  }

  /* Initialise memory area being tested */
  vm_control = pmem_ptr;
  test_pmem = pmem_ptr + sizeof(*vm_control);
  test_mem_size = (pmem_size - sizeof(*vm_control)) / sizeof(int);
  test_mem_size &= ~(sizeof(int) - 1);
  test_mem_size = 16; // TODO: temporary hack

  printf("Shared memory size=%ld addr=%p\n", pmem_size, pmem_ptr);
  printf("Test memory   size=%ld addr=%p\n", test_mem_size, test_pmem);

  /* get my VM Id */
  res = ioctl(pmem_fd, IOCTL_READ_IV_POSN, &vm_id);
  if (res < 0) {
    printf("IOCTL_READ_IV_POS failed\n");
    goto exit_close;
  }
  printf("My VM id = 0x%x\n", vm_id);
  vm_id = vm_id << 16;
  /* Initialise time stuff */
  cpu_test_time_start = clock();
  gettimeofday(&time_start, NULL);
  real_time_start_msec = 1000.0*time_start.tv_sec + (double)time_start.tv_usec/1000.0;

  if (is_server() || argc > 1)
  {
    proc_server();
  }
  else
  {
    proc_client();
  }

  if (munmap((void*)pmem_ptr, pmem_size))
  {
    perror(PMEM_DEVICE);
    goto exit_error;
  }

exit_close:
  close(pmem_fd);

exit_error:
  return 1;
}
