#define DEBUG

#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/ioctl.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direction.h>
#include <asm/uaccess.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/kfifo.h>
#include <linux/platform_device.h>

#include "mymiscdev_ioctl.h"

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char __user *, size_t, loff_t *);
static int device_mmap(struct file *, struct vm_area_struct *);
static long device_ioctl(struct file *, unsigned int cmd, unsigned long arg);
static unsigned int device_poll(struct file *, poll_table *wait);

static int user_scatter_gather(struct file *filp, char __user *userbuf, size_t nbytes);

static struct file_operations sample_fops = {
    .owner = THIS_MODULE,
    .read = device_read,
    .write = device_write,
    .mmap = device_mmap,
    .open = device_open,
    .release = device_release,
    .llseek = no_llseek,
    .unlocked_ioctl = device_ioctl,
    .poll = device_poll,
};

struct miscdevice sample_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "mymiscdev",
    .fops = &sample_fops,
    .mode = S_IRUGO | S_IWUGO,
};

#define DMABUFSIZE_ORDER 8
#define DMABUFSIZE ((1 << DMABUFSIZE_ORDER)*PAGE_SIZE)

struct DmaAddress
{
    void *virtual;
    dma_addr_t dma_handle;
};

static struct DmaAddress da_coherent;
static struct DmaAddress da_single;

static DECLARE_WAIT_QUEUE_HEAD(device_read_wait);
static atomic_t hardirq_cnt = ATOMIC_INIT(0);

static int gpioButton = -1;    // specific to your setup
module_param(gpioButton, int, 0);

DEFINE_KFIFO(fifo_timeval, struct timeval, 32);
DEFINE_KFIFO(fifo_timestamp, char, 4096);

static void
gpio_do_tasklet(unsigned long data)
{
    // NOTE: another interrupt could be delivered while the tasklet is executing

    int saved_hardirq_cnt = atomic_xchg(&hardirq_cnt, 0);
    struct timeval tv;
    char strbuf[16];
    int idx;

    // any interrupts that arrive during the execution of this tasklet will
    // be processed by the next tasklet_schedule
    for (idx=0; idx < saved_hardirq_cnt; idx++) {
        int ret = kfifo_out(&fifo_timeval, &tv, 1);
        BUG_ON(ret==0);
        snprintf(strbuf, sizeof(strbuf), "%08u.%06u",
                (int)(tv.tv_sec % 100000000), (int)(tv.tv_usec));
        strbuf[sizeof(strbuf)-1] = '\n';
        kfifo_in(&fifo_timestamp, strbuf, sizeof(strbuf));
    }

    pr_debug("interrupt: %d\n", saved_hardirq_cnt);
    wake_up_interruptible(&device_read_wait);
}

DECLARE_TASKLET(gpio_tasklet, gpio_do_tasklet, 0);

static irqreturn_t
gpio_irq_handler(int irq, void *dev_id)
{
    struct timeval tv;
    do_gettimeofday(&tv);

    kfifo_in(&fifo_timeval, &tv, 1);
    atomic_inc(&hardirq_cnt);
    tasklet_schedule(&gpio_tasklet);
    return IRQ_HANDLED;
}

static int
device_open(struct inode *inodep, struct file *filp)
{
    pr_debug("%s\n", __func__);
    nonseekable_open(inodep, filp);
    return 0;
}

static int
device_release(struct inode *inodep, struct file *filp)
{
    pr_debug("%s\n", __func__);
    return 0;
}

static long
device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int retcode = -ENOTTY;

    pr_debug("%s\n", __func__);

    switch (cmd) {
    case SAMPLE_IOCTL_CMD_1:
    {
        struct mymiscdev_ioctl param;
        int rc = copy_from_user(&param, (void*)arg, sizeof(param));
        if (rc!=0) {
            return -EFAULT;
        }
        // struct mymiscdev_ioctl *param = (struct mymiscdev_ioctl*)arg;
        pr_debug("ioctl_cmd_1 %p %zu\n", param.buffer, param.len);
        retcode = user_scatter_gather(filp, param.buffer, param.len);
        break;
    }
    }

    return retcode;
}

static int user_scatter_gather(struct file *filp, char __user *userbuf, size_t nbytes)
{
    int errcode = 0;
    int num_pages;
    int actual_pages;
    struct page **pages = NULL;
    int page_idx;
    int offset;
    struct scatterlist *sglist = NULL;
    int sg_count;
    struct miscdevice *miscdev = filp->private_data;    // filled in by misc_register
    struct device *dev = miscdev->parent;

    if (nbytes==0) {
        return 0;
    }

    offset = offset_in_page(userbuf);

    #if 0
    if (offset != 0) {
        pr_debug("buffer %p needs to be page aligned\n", userbuf);
        return -EINVAL;
    }
    #endif
    
    num_pages = (offset + nbytes + PAGE_SIZE - 1) / PAGE_SIZE;

    pages = kmalloc(num_pages * sizeof(*pages), GFP_KERNEL);
    sglist = kmalloc(num_pages * sizeof(*sglist), GFP_KERNEL);
    if (!pages || !sglist) {    // okay if allocation for vmas failed
        errcode = -ENOMEM;
        goto cleanup_alloc;
    }

    // get_user_pages also locks the user-space buffer into memory
    down_read(&current->mm->mmap_sem);
    actual_pages = get_user_pages(
        #if LINUX_VERSION_CODE < KERNEL_VERSION(4,6,1)
        current, current->mm,
        #endif
        (unsigned long)userbuf, num_pages, 1, 0, pages, NULL);
    up_read(&current->mm->mmap_sem);

    if (actual_pages != num_pages) {
        pr_warning("get_user_pages returned %d / %d\n", actual_pages, num_pages);
        errcode = actual_pages < 0 ? actual_pages : -EAGAIN;
        // need to cleanup_pages for the case 0 < actual_pages < num_pages
        goto cleanup_pages;
    }

    pr_debug("get_user_pages returned %d\n", actual_pages);

    // populate sglist
    sg_init_table(sglist, num_pages);
    sg_set_page(&sglist[0], pages[0], PAGE_SIZE - offset, offset);
    for (page_idx=1; page_idx < num_pages-1; page_idx++) {
        sg_set_page(&sglist[page_idx], pages[page_idx], PAGE_SIZE, 0);
    }
    if (num_pages > 1) {
        sg_set_page(&sglist[num_pages-1], pages[num_pages-1],
            nbytes - (PAGE_SIZE - offset) - ((num_pages-2)*PAGE_SIZE), 0);
    }

    if (1) {
        for (page_idx=0; page_idx < num_pages; page_idx++) {
            struct scatterlist *sg = &sglist[page_idx];
            pr_debug("%d: %p %u\n", page_idx, pages[page_idx], sg->length);
        }
    }

    sg_count = dma_map_sg(dev, sglist, num_pages, DMA_FROM_DEVICE);
    if (sg_count==0) {
        pr_warning("dma_map_sg returned 0\n");
        errcode = -EAGAIN;
        goto cleanup_pages;
    }

    pr_debug("dma_map_sg returned %d\n", sg_count);

    {
        struct scatterlist *sg;
        int sg_idx;

        for_each_sg(sglist, sg, sg_count, sg_idx) {
            unsigned long hwaddr = sg_dma_address(sg);
            unsigned int dmalen = sg_dma_len(sg);
            pr_debug("%d: %#08lx %u\n", sg_idx, hwaddr, dmalen);
        }
    }

// cleanup_sglist:
    dma_unmap_sg(dev, sglist, num_pages, DMA_FROM_DEVICE);

cleanup_pages:
    for (page_idx=0; page_idx < actual_pages; page_idx++) {
        // alias page_cache_release has been removed
        put_page(pages[page_idx]);
    }

cleanup_alloc:
    kfree(sglist);
    kfree(pages);

    return errcode;
}

static ssize_t
device_read(struct file *filp, char __user *userbuf, size_t nbytes, loff_t *f_pos)
{
    int ret;
    int nonblock = filp->f_flags & O_NONBLOCK;
    int copied;

    pr_debug("%s %zu\n", __func__, nbytes);

    if (nbytes==0) {
        return 0;
    }

    if (kfifo_is_empty(&fifo_timestamp) && nonblock)
        return -EAGAIN;

    ret = wait_event_interruptible(device_read_wait, !kfifo_is_empty(&fifo_timestamp));
    if (ret==-ERESTARTSYS) {
        pr_debug("%s got signal\n", __func__);
        // NOTE: if we return ERESTARTSYS, userspace will not see EINTR
        return -EINTR;
    }

    // spin_lock_bh(&device_read_wait.lock);

    ret = kfifo_to_user(&fifo_timestamp, userbuf, nbytes, &copied);

    // spin_unlock_bh(&device_read_wait.lock);

    return ret ? ret : copied;
}

static ssize_t
device_write(struct file *filp, const char __user *userbuf, size_t nbytes, loff_t *f_pos)
{
    pr_debug("%s %zu\n", __func__, nbytes);
    return nbytes;
}

static int
device_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct miscdevice *miscdev = filp->private_data;    // filled in by misc_register
    struct device *dev = miscdev->parent;
    struct DmaAddress *da = &da_coherent;

    int rc;
    if (vma->vm_pgoff == 0)
    {
        long length = vma->vm_end - vma->vm_start;
        if (length > DMABUFSIZE) {
            return -EIO;
        }
        #if 1
        rc = dma_mmap_coherent(dev, vma, da->virtual, da->dma_handle, length);
        if (rc!=0) {
            pr_warning("dma_mmap_coherent failed %d\n", rc);
        }
        #else
        // vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        rc = remap_pfn_range(vma, vma->vm_start,
                             PHYS_PFN(virt_to_phys(da->virtual)),
                             length, vma->vm_page_prot);
        if (rc!=0) {
            pr_warning("remap_pfn_range failed %d\n", rc);
        }
        #endif
    }
    else
    {
        rc = remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
                             vma->vm_end - vma->vm_start, vma->vm_page_prot);
        if (rc!=0) {
            pr_warning("remap_pfn_range failed %d\n", rc);
        }
    }
    return rc;
}

static unsigned int
device_poll(struct file *filp, poll_table *wait)
{
    unsigned int mask = 0;

    poll_wait(filp, &device_read_wait, wait);
    if (!kfifo_is_empty(&fifo_timestamp)) {
        mask |= POLLIN | POLLRDNORM;
    }
    return mask;
}

static void
setup_gpio(struct device *dev)
{
    int rc;
    unsigned int irqnum;

    rc = devm_gpio_request_one(dev, gpioButton,
            GPIOF_DIR_IN | GPIOF_EXPORT_DIR_FIXED, "button");
    if (rc!=0) {
        dev_warn(dev, "gpio_request_one failed %d\n", rc);
        return;
    }

    irqnum = gpio_to_irq(gpioButton);
    rc = devm_request_irq(dev, irqnum, gpio_irq_handler, IRQF_TRIGGER_RISING,
        "mymiscdev", NULL);
    if (rc!=0) {
        dev_warn(dev, "request_irq failed %d\n", rc);
        return;
    }
}

static int mymiscdev_probe(struct platform_device *pdev)
{
    int rc;
    struct DmaAddress *da;

    pr_debug("%s\n", __func__);

    if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))) {
        pr_warning("mymiscdev: No suitable DMA available\n");
    }

    da = &da_coherent;
    da->virtual = dmam_alloc_coherent(&pdev->dev, DMABUFSIZE, &da->dma_handle, GFP_KERNEL);
    if (!da->virtual) {
        pr_warning("dmam_alloc_coherent failed\n");
        return -ENOMEM;
    }
    pr_debug("dma_handle: %#llx\n", (unsigned long long)da->dma_handle);

    da = &da_single;
    da->virtual = (void*)devm_get_free_pages(&pdev->dev,
                            GFP_KERNEL | GFP_DMA, DMABUFSIZE_ORDER);
    if (!da->virtual) {
        pr_warning("devm_get_free_pages failed\n");
        return -ENOMEM;
    }
    da->dma_handle = dma_map_single(&pdev->dev, da->virtual, DMABUFSIZE, DMA_FROM_DEVICE);
    if (dma_mapping_error(&pdev->dev, da->dma_handle)) {
        pr_warning("dma_map_single failed\n");
        return -EBUSY;
    }
    pr_debug("dma_handle: %#llx\n", (unsigned long long)da->dma_handle);

    if (gpioButton >= 0)
        setup_gpio(&pdev->dev);

    sample_misc.parent = &pdev->dev;
    rc = misc_register(&sample_misc);
    if (rc!=0) {
        pr_warning("misc_register failed %d\n", rc);

        dma_unmap_single(&pdev->dev, da_single.dma_handle, DMABUFSIZE, DMA_FROM_DEVICE);
    }

    return rc;
}

static int mymiscdev_remove(struct platform_device *pdev)
{
    pr_debug("%s\n", __func__);

    misc_deregister(&sample_misc);

    dma_unmap_single(&pdev->dev, da_single.dma_handle, DMABUFSIZE, DMA_FROM_DEVICE);

    return 0;
}

static struct platform_driver mymiscdev_driver = {
    .probe      = mymiscdev_probe,
    .remove     = mymiscdev_remove,
    .driver     = {
        .name   = "mymiscdev",
    },
};

static struct platform_device *platform_device;

static int __init device_init(void)
{
    int err;
    struct platform_device_info pdevinfo = {
        .name       = "mymiscdev",
        .id         = -1,
        .res        = NULL,
        .num_res    = 0,
        .dma_mask   = DMA_BIT_MASK(32),
    };

    err = platform_driver_register(&mymiscdev_driver);
    if (err)
        return err;

    // platform_device = platform_device_register_simple("mymiscdev", -1, NULL, 0);
    platform_device = platform_device_register_full(&pdevinfo);

    if (IS_ERR(platform_device)) {
        err = PTR_ERR(platform_device);
        platform_driver_unregister(&mymiscdev_driver);
    }

    return err;
}

static void __exit device_exit(void)
{
    platform_device_unregister(platform_device);
    platform_driver_unregister(&mymiscdev_driver);
}

module_init(device_init)
module_exit(device_exit)

MODULE_DESCRIPTION("Simple Misc Driver");
MODULE_AUTHOR("Kiu Shueng Chuan");
MODULE_LICENSE("Dual MIT/GPL");

