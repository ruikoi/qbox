#ifndef VORTEX_QBOX_UAPI_H
#define VORTEX_QBOX_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>
struct vortex_wait { __u32 target, timeout_ms, retired, irq_count; };
#define VORTEX_WAIT _IOWR('V', 1, struct vortex_wait)
#define VORTEX_STAGING_OFFSET 0x20000
#endif
