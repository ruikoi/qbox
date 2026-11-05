// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include "vortex_qbox_uapi.h"
struct vortex_device {
    void __iomem *regs;
    resource_size_t control, control_size, staging, staging_size;
    struct miscdevice misc;
    atomic_t owned, irqs;
    wait_queue_head_t completion;
};
static irqreturn_t vortex_interrupt(int irq, void *arg)
{
    struct vortex_device *v = arg;
    u32 pending = readl(v->regs + 0x10028);
    if (!(pending & 1)) return IRQ_NONE;
    writel(1, v->regs + 0x10028);
    readl(v->regs + 0x10028);
    atomic_inc(&v->irqs);
    wake_up_interruptible(&v->completion);
    return IRQ_HANDLED;
}
static int vortex_open(struct inode *inode, struct file *file)
{
    struct vortex_device *v = container_of(file->private_data, struct vortex_device, misc);
    if (atomic_cmpxchg(&v->owned, 0, 1)) return -EBUSY;
    file->private_data = v;
    return nonseekable_open(inode, file);
}
static int vortex_release(struct inode *inode, struct file *file)
{
    struct vortex_device *v = file->private_data;
    atomic_set(&v->owned, 0);
    return 0;
}
static int vortex_mmap(struct file *file, struct vm_area_struct *area)
{
    struct vortex_device *v = file->private_data;
    u64 offset = (u64)area->vm_pgoff << PAGE_SHIFT;
    unsigned long length = area->vm_end - area->vm_start;
    resource_size_t phys, size;
    if (offset < VORTEX_STAGING_OFFSET) {
        phys = v->control + offset;
        size = v->control_size;
    } else {
        offset -= VORTEX_STAGING_OFFSET;
        phys = v->staging + offset;
        size = v->staging_size;
    }
    if (offset >= size || length > size - offset) return -EINVAL;
    if (!(area->vm_flags & VM_SHARED) || (area->vm_flags & VM_EXEC)) return -EINVAL;
    vm_flags_set(area, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
    vm_flags_clear(area, VM_MAYEXEC);
    area->vm_page_prot = pgprot_noncached(area->vm_page_prot);
    return remap_pfn_range(area, area->vm_start, phys >> PAGE_SHIFT, length, area->vm_page_prot);
}
static long vortex_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vortex_device *v = file->private_data;
    struct vortex_wait w;
    long rc;
    if (cmd != VORTEX_WAIT) return -ENOTTY;
    if (copy_from_user(&w, (void __user *)arg, sizeof(w))) return -EFAULT;
    if (!w.timeout_ms || w.timeout_ms > 600000) return -EINVAL;
    rc = wait_event_interruptible_timeout(v->completion,
        (s32)(readl(v->regs + 0x1128) - w.target) >= 0,
        msecs_to_jiffies(w.timeout_ms));
    if (rc < 0) return rc;
    if (!rc) return -ETIMEDOUT;
    w.retired = readl(v->regs + 0x1128);
    w.irq_count = atomic_read(&v->irqs);
    if (copy_to_user((void __user *)arg, &w, sizeof(w))) return -EFAULT;
    return 0;
}
static const struct file_operations vortex_fops = {
    .owner=THIS_MODULE, .open=vortex_open, .release=vortex_release,
    .mmap=vortex_mmap, .unlocked_ioctl=vortex_ioctl,
};
static int vortex_probe(struct platform_device *pdev)
{
    struct vortex_device *v;
    struct resource *r;
    int irq, rc;
    v = devm_kzalloc(&pdev->dev, sizeof(*v), GFP_KERNEL);
    if (!v) return -ENOMEM;
    v->regs = devm_platform_ioremap_resource(pdev, 0);
    if (IS_ERR(v->regs)) return PTR_ERR(v->regs);
    if (readl(v->regs + 0x10000) != 0x5652544c) return -ENODEV;
    r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    v->control = r->start; v->control_size = resource_size(r);
    r = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    if (!r) return -EINVAL;
    v->staging = r->start; v->staging_size = resource_size(r);
    if (!devm_request_mem_region(&pdev->dev, r->start, resource_size(r), "vortex-staging")) return -EBUSY;
    init_waitqueue_head(&v->completion);
    irq = platform_get_irq(pdev, 0);
    if (irq < 0) return irq;
    writel(0, v->regs + 0x10024);
    writel(1, v->regs + 0x10028);
    rc = devm_request_irq(&pdev->dev, irq, vortex_interrupt, 0, "vortex-qbox", v);
    if (rc) return rc;
    v->misc.minor = MISC_DYNAMIC_MINOR;
    v->misc.name = "vortex0";
    v->misc.fops = &vortex_fops;
    v->misc.parent = &pdev->dev;
    rc = misc_register(&v->misc);
    if (rc) return rc;
    platform_set_drvdata(pdev, v);
    writel(1, v->regs + 0x10024);
    dev_info(&pdev->dev, "VORTEX_DRIVER_READY irq=%d device=/dev/vortex0\n", irq);
    return 0;
}
static void vortex_remove(struct platform_device *pdev)
{
    struct vortex_device *v = platform_get_drvdata(pdev);
    writel(0, v->regs + 0x10024);
    misc_deregister(&v->misc);
}
static const struct of_device_id vortex_match[] = {{.compatible="vortex,afu-rtl"}, {}};
MODULE_DEVICE_TABLE(of, vortex_match);
static struct platform_driver vortex_driver = {
    .probe=vortex_probe, .remove=vortex_remove,
    .driver={.name="vortex-qbox", .of_match_table=vortex_match},
};
module_platform_driver(vortex_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Vortex QBox AFU mmap and RTL completion interrupt driver");
