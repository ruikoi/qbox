// SPDX-License-Identifier: Apache-2.0
#include <callbacks.h>
#include <common.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include "driver/vortex_qbox_uapi.h"
#include <unistd.h>

static constexpr uint64_t APERTURE_BASE = 0x20000000;
static constexpr uint64_t APERTURE_SIZE = 64ull << 20;

class vx_device {
public:
  vx_device() : allocator_(APERTURE_BASE, APERTURE_SIZE, 4096, 64) {}

  ~vx_device() {
    if (regs_ != MAP_FAILED) {
      vortex_wait snapshot{read32(0x1128), 1, 0, 0};
      if (!ioctl(reg_fd_, VORTEX_WAIT, &snapshot)) { irq_seen_ = snapshot.irq_count; }
      uint64_t ticks = read32(0x10008) | (uint64_t(read32(0x1000c)) << 32);
      std::printf("VORTEX_QBOX_RTL cycles=%llu host_reads=%u host_writes=%u vram_reads=%u vram_writes=%u\n",
                  (unsigned long long)ticks, read32(0x10010), read32(0x10014), read32(0x10018), read32(0x1001c));
      std::printf("VORTEX_QBOX_IRQ waits=%u driver_irqs=%u rtl_pulses=%u seq_start=%u seq_end=%u aw_stalls=%u w_stalls=%u ar_stalls=%u idle_gates=%u uid=%u device=/dev/vortex0\n",
          waits_, irq_seen_, read32(0x1002c), seq_start_, read32(0x1128), read32(0x10030), read32(0x10034), read32(0x10038), read32(0x1003c), unsigned(getuid()));
      write32(0x10020, 0);
    }
    if (regs_ != MAP_FAILED) {
      munmap(regs_, 0x20000);
    }
    if (memory_ != MAP_FAILED) {
      munmap(memory_, APERTURE_SIZE);
    }
    if (reg_fd_ >= 0) {
      close(reg_fd_);
    }
    if (mem_fd_ >= 0) {
      close(mem_fd_);
    }
    if (lock_fd_ >= 0) {
      close(lock_fd_);
    }
  }

  int init() {
    lock_fd_ = open("/tmp/vortex-qbox.lock", O_CREAT | O_RDWR, 0600);
    if (lock_fd_ < 0 || flock(lock_fd_, LOCK_EX | LOCK_NB)) {
      return -1;
    }
    reg_fd_ = open("/dev/vortex0", O_RDWR | O_SYNC);
    mem_fd_ = dup(reg_fd_);
    if (reg_fd_ < 0 || mem_fd_ < 0) { return -1; }
    regs_ = mmap(nullptr, 0x20000, PROT_READ | PROT_WRITE, MAP_SHARED, reg_fd_, 0);
    memory_ = mmap(nullptr, APERTURE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd_, VORTEX_STAGING_OFFSET);
    if (regs_ == MAP_FAILED || memory_ == MAP_FAILED || read32(0x10000) != 0x5652544c) { return -1; }
    seq_start_ = read32(0x1128);
    std::puts("VORTEX_QBOX_BACKEND=AFU RTL AXI-Lite/TLM AXI/TLM cp=RTL gpu=RTL fpu=DPI");
    return 0;
  }

  int cp_reg_write(uint32_t off, uint32_t value) {
    if (off >= 0x200 || (off & 3)) {
      return -1;
    }
    fence();
    write32(0x1000 + off, value);
    if (off == 0x120) { tail_ = (tail_ & 0xffffffff00000000ull) | value; }
    if (off == 0x124) { tail_ = (tail_ & 0xffffffffull) | (uint64_t(value) << 32); }
    if (off == 0x120 || off == 0x124) { target_ = uint32_t(tail_ >> 6); }
    return 0;
  }

  int cp_reg_read(uint32_t off, uint32_t *value) {
    if (off >= 0x200 || (off & 3)) {
      return -1;
    }
    if (off == 0x128 && int32_t(read32(0x1128) - target_) < 0) {
      vortex_wait w{target_, 600000, 0, 0};
      if (ioctl(reg_fd_, VORTEX_WAIT, &w) < 0) { std::perror("VORTEX_WAIT"); return -1; }
      ++waits_;
      irq_seen_ = w.irq_count;
    }
    *value = read32(0x1000 + off);
    fence();
    return 0;
  }

  int host_mem_alloc(uint64_t size, void **ptr, uint64_t *cp_addr) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!size || size > APERTURE_SIZE) {
      return -1;
    }
    int result = allocator_.allocate(aligned_size(size, 64), cp_addr);
    if (result) {
      return result;
    }
    *ptr = static_cast<uint8_t *>(memory_) + *cp_addr - APERTURE_BASE;
    return 0;
  }

  int host_mem_free(uint64_t addr) {
    std::lock_guard<std::mutex> guard(mutex_);
    return allocator_.release(addr);
  }
  int host_mem_pull(uint64_t) {
    fence();
    return 0;
  }
  int host_mem_push(uint64_t) {
    fence();
    return 0;
  }

private:
  static void fence() {
#if defined(__riscv)
    __asm__ __volatile__("fence iorw, iorw" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("dmb sy" ::: "memory");
#else
    __sync_synchronize();
#endif
  }
  uint32_t read32(unsigned offset) {
    return *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(regs_) + offset);
  }
  void write32(unsigned offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(regs_) + offset) = value;
  }
  uint64_t tail_ = 0;
  uint32_t target_ = 0, waits_ = 0, irq_seen_ = 0, seq_start_ = 0;
  int reg_fd_ = -1, mem_fd_ = -1, lock_fd_ = -1;
  void *regs_ = MAP_FAILED;
  void *memory_ = MAP_FAILED;
  std::mutex mutex_;
  vortex::MemoryAllocator allocator_;
};

#include <callbacks.inc>
