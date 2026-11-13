// SPDX-License-Identifier: MIT
// rdma_lab: char device exposing a DMA-style ring buffer.
// read/write stream through the ring; mmap gives zero-copy access
// (page 0: {u64 head, u64 tail}, then ring data); ioctl = stats/reset.
// Build (Linux): make -C driver.  QEMU test: see driver/README-qemu.md
// NOTE: untested on this dev laptop (macOS, no kernel build) — see README limitations.
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define RDL_RING_SIZE (1U << 20) /* 1 MiB, power of two */
#define RDL_IOC_MAGIC 'R'
struct rdl_stats { __u64 produced, consumed, overruns; __u32 cap; };
#define RDL_IOC_GET_STATS _IOR(RDL_IOC_MAGIC, 1, struct rdl_stats)
#define RDL_IOC_RESET _IO(RDL_IOC_MAGIC, 2)

static struct {
	struct miscdevice misc; spinlock_t lock; wait_queue_head_t rq, wq;
	u8 *base, *ring; struct rdl_stats st; /* base page: head at +0, tail at +8 */
} dev;
#define HEAD (*(u64 *)dev.base)
#define TAIL (*(u64 *)(dev.base + 8))
static size_t used(void) { return TAIL - HEAD; }

static ssize_t xfer(struct file *f, char __user *u, size_t len, bool wr)
{
	size_t done = 0;
	while (done < len) {
		size_t chunk, off; long r;
		r = wait_event_interruptible(wr ? dev.wq : dev.rq, wr ? (used() < RDL_RING_SIZE) : (used() > 0));
		if (r) return done ?: r;
		spin_lock(&dev.lock); off = (wr ? TAIL : HEAD) & (RDL_RING_SIZE - 1);
		chunk = min3(len - done, wr ? (size_t)(RDL_RING_SIZE - used()) : (size_t)used(), (size_t)(RDL_RING_SIZE - off));
		if (wr) { if (copy_from_user(dev.ring + off, u + done, chunk)) { spin_unlock(&dev.lock); return done ?: -EFAULT; } TAIL += chunk; dev.st.produced += chunk; }
		else    { if (copy_to_user(u + done, dev.ring + off, chunk)) { spin_unlock(&dev.lock); return done ?: -EFAULT; } HEAD += chunk; dev.st.consumed += chunk; }
		spin_unlock(&dev.lock); done += chunk; wake_up(wr ? &dev.rq : &dev.wq);
	}
	return done;
}
static ssize_t rdl_read(struct file *f, char __user *u, size_t n, loff_t *o) { return xfer(f, u, n, false); }
static ssize_t rdl_write(struct file *f, const char __user *u, size_t n, loff_t *o) { return xfer(f, (char __user *)u, n, true); }

static int rdl_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	if (vma->vm_pgoff + (size >> PAGE_SHIFT) > 1 + (RDL_RING_SIZE >> PAGE_SHIFT)) return -EINVAL;
	return remap_pfn_range(vma, vma->vm_start, (virt_to_phys(dev.base) >> PAGE_SHIFT) + vma->vm_pgoff, size, vma->vm_page_prot);
}
static long rdl_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	if (cmd == RDL_IOC_RESET) { spin_lock(&dev.lock); HEAD = TAIL = 0; memset(&dev.st, 0, sizeof(dev.st)); spin_unlock(&dev.lock); return 0; }
	if (cmd == RDL_IOC_GET_STATS) { struct rdl_stats s; spin_lock(&dev.lock); s = dev.st; s.cap = RDL_RING_SIZE; spin_unlock(&dev.lock);
		return copy_to_user((void __user *)arg, &s, sizeof(s)) ? -EFAULT : 0; }
	return -ENOTTY;
}
static __poll_t rdl_poll(struct file *f, poll_table *pt)
{ poll_wait(f, &dev.rq, pt); poll_wait(f, &dev.wq, pt);
	return (used() ? EPOLLIN | EPOLLRDNORM : 0) | (used() < RDL_RING_SIZE ? EPOLLOUT | EPOLLWRNORM : 0); }

static const struct file_operations fops = { .owner = THIS_MODULE, .read = rdl_read, .write = rdl_write,
	.mmap = rdl_mmap, .unlocked_ioctl = rdl_ioctl, .poll = rdl_poll, .llseek = no_llseek };

static int __init rdl_init(void)
{
	dev.base = kmalloc(RDL_RING_SIZE + PAGE_SIZE, GFP_KERNEL); if (!dev.base) return -ENOMEM;
	dev.ring = dev.base + PAGE_SIZE; HEAD = TAIL = 0;
	spin_lock_init(&dev.lock); init_waitqueue_head(&dev.rq); init_waitqueue_head(&dev.wq);
	dev.misc.minor = MISC_DYNAMIC_MINOR; dev.misc.name = "rdma_lab0"; dev.misc.fops = &fops;
	return misc_register(&dev.misc);
}
static void __exit rdl_exit(void) { misc_deregister(&dev.misc); kfree(dev.base); }
module_init(rdl_init); module_exit(rdl_exit);
MODULE_LICENSE("MIT"); MODULE_DESCRIPTION("DMA-style ring buffer char device");
