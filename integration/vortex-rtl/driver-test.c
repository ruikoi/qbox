// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "driver/vortex_qbox_uapi.h"
int main(void) {
  int fd=open("/dev/vortex0",O_RDWR|O_SYNC);
  if(fd<0) { perror("vortex0"); return 1; }
  int other=open("/dev/vortex0",O_RDWR);
  if(other>=0 || errno!=EBUSY) return 1;
  void *bad=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,VORTEX_STAGING_OFFSET+(64ul<<20));
  if(bad!=MAP_FAILED || errno!=EINVAL) return 1;
  bad=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
  if(bad!=MAP_FAILED || errno!=EINVAL) return 1;
  volatile unsigned *r=mmap(0,0x20000,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(r==MAP_FAILED) return 1;
  struct vortex_wait wait={r[0x1128/4]+1,10,0,0};
  if(ioctl(fd,VORTEX_WAIT,&wait)!=-1 || errno!=ETIMEDOUT) return 1;
  if(ioctl(fd,_IO('V',99),0)!=-1 || errno!=ENOTTY) return 1;
  int mem=open("/dev/mem",O_RDWR);
  if(mem>=0 || (errno!=EACCES && errno!=EPERM)) return 1;
  printf("VORTEX_DRIVER_API_PASS uid=%u exclusive=1 mmap_bounds=1 private_rejected=1 timeout=1 unknown_ioctl=1 devmem_denied=1\n",(unsigned)getuid());
  munmap((void*)r,0x20000); close(fd);
  return 0;
}
