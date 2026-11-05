// SPDX-License-Identifier: Apache-2.0
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void) {
  int fd = open("/dev/vortex0", O_RDWR | O_SYNC);
  if (fd < 0) { return 1; }
  volatile uint32_t *r = mmap(0, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (r == MAP_FAILED) { close(fd); return 1; }
  if (r[0x10000 / 4] != 0x5652544c) { return 1; }
  uint32_t before = r[0x1128 / 4];
  if (!before) { puts("VORTEX_RTL_RESET_FAIL no previously retired command"); return 1; }
  r[0x1000 / 4] = 3; // CP enable + native reset_all pulse.
  __asm__ __volatile__("fence iorw, iorw" ::: "memory");
  unsigned i;
  for (i = 0; i < 1000; ++i) {
    if (!r[0x1128 / 4] && !r[0x1120 / 4] && !r[0x1124 / 4]) { break; }
  }
  if (i == 1000) { puts("VORTEX_RTL_RESET_FAIL queue did not clear"); return 1; }
  r[0x1000 / 4] = 0;
  r[0x10020 / 4] = 0;
  printf("VORTEX_RTL_QUEUE_RESET_PASS seqnum_before=%u seqnum_after=0 tail_after=0 native_reset=1\n", before);
  munmap((void *)r, 0x20000);
  close(fd);
  return 0;
}
