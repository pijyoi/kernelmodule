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
#include <linux/pci.h>

#include "mymiscdev_ioctl.h"

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char __user *, size_t, loff_t *);
static int device_mmap(struct file *, struct vm_area_struct *);
static long device_ioctl(struct file *, unsigned int cmd, unsigned long arg);
static unsigned int device_poll(struct file *, poll_table *wait);

static int user_scatter_gather(struct device *dev, char __user *userbuf, size_t nbytes);

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

#define DMABUFSIZE_ORDER 8
#define DMABUFSIZE ((1 << DMABUFSIZE_ORDER)*PAGE_SIZE)

struct DmaAddress
{
    void *virtual;
    dma_addr_t dma_handle;
};

struct mymiscdev_data {
    struct miscdevice miscdev;
    void *iomem;
    struct DmaAddress da_coherent;
    struct DmaAddress da_single;
};

struct TimeStamp {
    long tv_sec;
    long tv_nsec;
};

static DECLARE_WAIT_QUEUE_HEAD(device_read_wait);
static atomic_t hardirq_cnt = ATOMIC_INIT(0);

static int gpioButton = -1;    // specific to your setup
static int dmaBitMask = 32;
module_param(gpioButton, int, 0);
module_param(dmaBitMask, int, 0);

DEFINE_KFIFO(fifo_timeval, struct TimeStamp, 32);
DEFINE_KFIFO(fifo_timestamp, char, 4096);

static void
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
gpio_do_tasklet(unsigned long data)
#else
gpio_do_tasklet(struct tasklet_struct *tasklet)
#endif
{
    // NOTE: another interrupt could be delivered while the tasklet is executing

    int saved_hardirq_cnt = atomic_xchg(&hardirq_cnt, 0);
    struct TimeStamp ts;
    char strbuf[16];
    int idx;

    // any interrupts that arrive during the execution of this tasklet will
    // be processed by the next tasklet_schedule
    for (idx=0; idx < saved_hardirq_cnt; idx++) {
        int ret = kfifo_out(&fifo_timeval, &ts, 1);
        BUG_ON(ret==0);
        snprintf(strbuf, sizeof(strbuf), "%08u.%06u",
                (int)(ts.tv_sec % 100000000), (int)(ts.tv_nsec / 1000));
        strbuf[sizeof(strbuf)-1] = '\n';
        kfifo_in(&fifo_timestamp, strbuf, sizeof(strbuf));
    }

    pr_debug("interrupt: %d\n", saved_hardirq_cnt);
    wake_up_interruptible(&device_read_wait);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
DECLARE_TASKLET(gpio_tasklet, gpio_do_tasklet, 0);
#else
DECLARE_TASKLET(gpio_tasklet, gpio_do_tasklet);
#endif

static irqreturn_t
gpio_irq_handler(int irq, void *dev_id)
{
    struct TimeStamp dst;

    #if LINUX_VERSION_CODE < KERNEL_VERSION(3,17,0)
    struct timeval src;
    do_gettimeofday(&src);
    dst.tv_sec = src.tv_sec;
    dst.tv_nsec = src.tv_usec * 1000;
    #else
    struct timespec64 src;
    ktime_get_real_ts64(&src);
    dst.tv_sec = src.tv_sec;
    dst.tv_nsec = src.tv_nsec;
    #endif

    kfifo_in(&fifo_timeval, &dst, 1);
    atomic_inc(&hardirq_cnt);
    tasklet_schedule(&gpio_tasklet);
    return IRQ_HANDLED;
}

static void
print_pfn(void *cpu_addr, dma_addr_t bus_addr)
{
    int idx;
    pr_debug("cpu_addr: %#llx; bus_addr: %#llx\n",
        (unsigned long long)cpu_addr,
        (unsigned long long)bus_addr);

    for (idx=0; idx < 8; idx++) {
        void *ptr = cpu_addr + idx * PAGE_SIZE;
        unsigned long pfn = 0;
        bool is_virt = is_vmalloc_addr(ptr);
        if (is_vmalloc_addr(ptr)) {
            pfn = vmalloc_to_pfn(ptr);
        } else {
            pfn = virt_to_phys(ptr) >> PAGE_SHIFT;
        }
        pr_debug("%d: %#lx %s\n", idx, pfn, is_virt ? "virtual" : "logical");
    }
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

static void
start_dma(void *mmio, int dirn, dma_addr_t ram_busaddr, uint32_t edu_offset, uint32_t len,
    int wait)
{
    uint32_t src_addr, dst_addr;
    uint32_t edu_devaddr = 0x40000 + edu_offset;
    uint32_t dmacmd = 0;

    if (dirn == DMA_TO_DEVICE) {
        src_addr = ram_busaddr;
        dst_addr = edu_devaddr;
        dmacmd = 1;
    } else if (dirn == DMA_FROM_DEVICE) {
        src_addr = edu_devaddr;
        dst_addr = ram_busaddr;
        dmacmd = 3;
    }
    if (dmacmd) {
        writel(src_addr, mmio + 0x80);
        writel(dst_addr, mmio + 0x88);
        writel(len, mmio + 0x90);
        writel(dmacmd, mmio + 0x98);
    }

    while (wait) {
        uint32_t status = readl(mmio + 0x98);
        if ((status & 1) == 0)
            wait = 0;
    }
}

static long
device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct miscdevice *miscdev = filp->private_data;    // filled in by misc_register
    struct device *dev = miscdev->parent;
    struct mymiscdev_data *drvdata = container_of(miscdev, struct mymiscdev_data, miscdev);

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
        retcode = user_scatter_gather(dev, param.buffer, param.len);
        break;
    }
    case MYMISCDEV_IOCTL_REG:
    {
        struct mymiscdev_ioctl_reg param;
        void *ioaddr;
        int err = copy_from_user(&param, (void*)arg, sizeof(param));
        if (err) {
            return -EFAULT;
        }
        pr_debug("ioctl_reg %d 0x%x %u\n", param.write, param.addr, param.val);

        if (param.addr & 0x3) {
            return -EIO;
        }
        if (param.addr > 0xFFFFC) {
            return -EIO;
        }
        ioaddr = drvdata->iomem + param.addr;
        if (param.write) {
            writel(param.val, ioaddr);
            retcode = 0;
        } else {
            param.val = readl(ioaddr);
            retcode = copy_to_user((void*)arg, &param, sizeof(param));
        }
        break;
    }
    case MYMISCDEV_IOCTL_DMA_KERNEL:
    {
        struct mymiscdev_ioctl_dma_kernel param;
        uint32_t ram_busaddr;
        uint32_t edu_offset;
        int err = copy_from_user(&param, (void*)arg, sizeof(param));
        if (err) {
            return -EFAULT;
        }
        pr_debug("ioctl_dma_kernel %d %u\n", param.write, param.len);

        if (param.len > 4096) {
            return -EINVAL;
        }

        ram_busaddr = (uint32_t)drvdata->da_coherent.dma_handle + param.offset;
        edu_offset = 0;
        pr_debug("ram_busaddr: 0x%x\n", ram_busaddr);
        start_dma(
            drvdata->iomem,
            param.write ? DMA_TO_DEVICE : DMA_FROM_DEVICE,
            ram_busaddr, edu_offset, param.len, 0);
        retcode = 0;
        break;
    }
    case MYMISCDEV_IOCTL_DMA_USER:
    {
        struct mymiscdev_ioctl_dma_user param;
        int offset;
        int num_pages;
        int actual_pages;
        struct page *one_page;
        dma_addr_t dma_handle;
        int dirn;

        int err = copy_from_user(&param, (void*)arg, sizeof(param));
        if (err) {
            return -EFAULT;
        }
        pr_debug("ioctl_dma_user %d %u\n", param.write, param.len);

        if (param.len == 0) {
            return 0;
        }

        offset = offset_in_page(param.buffer);

        if (param.len > 4096) {
            return -EINVAL;
        }
        if (offset + param.len > 4096) {
            return -EINVAL;
        }

        num_pages = 1;
        actual_pages = get_user_pages_unlocked(
            (unsigned long)param.buffer, num_pages, &one_page, FOLL_WRITE
        );
        if (actual_pages != num_pages) {
            return -EAGAIN;
        }

        dirn = param.write ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
        dma_handle = dma_map_page(dev, one_page, offset, param.len, dirn);
        if (dma_mapping_error(dev, dma_handle)) {
            put_page(one_page);
            return -EIO;
        }

        start_dma(drvdata->iomem, dirn, dma_handle, 0, param.len, 1);

        dma_unmap_page(dev, dma_handle, param.len, dirn);

        if (dirn == DMA_FROM_DEVICE) {
            set_page_dirty(one_page);
        }
        put_page(one_page);

        retcode = 0;
        break;
    }
    }

    return retcode;
}

static int user_scatter_gather(struct device *dev, char __user *userbuf, size_t nbytes)
{
    int errcode = 0;
    int num_pages;
    int actual_pages;
    struct page **pages = NULL;
    int page_idx;
    int offset;
    struct sg_table sgtbl;
    int sg_count;

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
    if (!pages) {    // okay if allocation for vmas failed
        return -ENOMEM;
    }

    // get_user_pages also locks the user-space buffer into memory
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4,9,0)
    down_read(&current->mm->mmap_sem);
    actual_pages = get_user_pages(
        #if LINUX_VERSION_CODE < KERNEL_VERSION(4,6,0)
        current, current->mm,
        #endif
        (unsigned long)userbuf, num_pages,
        1, /* write access for out data */
        0, /* no force */
        pages, NULL
    );
    up_read(&current->mm->mmap_sem);
    #else
    actual_pages = get_user_pages_unlocked(
        (unsigned long)userbuf, num_pages, pages, FOLL_WRITE
    );
    #endif

    if (actual_pages != num_pages) {
        pr_warn("get_user_pages returned %d / %d\n", actual_pages, num_pages);
        errcode = actual_pages < 0 ? actual_pages : -EAGAIN;
        // need to cleanup_pages for the case 0 < actual_pages < num_pages
        goto cleanup_pages;
    }

    pr_debug("get_user_pages returned %d\n", actual_pages);

    errcode = sg_alloc_table_from_pages(&sgtbl, pages, num_pages, offset, nbytes, GFP_KERNEL);
    if (errcode) {
        pr_warn("sg_alloc_table_from_pages returned %d\n", errcode);
        goto cleanup_pages;
    }

    {
        struct scatterlist *sg;
        int sg_idx;

        for_each_sg(sgtbl.sgl, sg, sgtbl.nents, sg_idx) {
            struct page *page = (struct page *)(sg->page_link & ~3UL);
            unsigned long paddr = page_to_phys(page);
            pr_debug("%d: %#08lx %u\n", sg_idx, paddr + sg->offset, sg->length);
        }
    }

    sg_count = dma_map_sg(dev, sgtbl.sgl, sgtbl.nents, DMA_FROM_DEVICE);
    if (sg_count==0) {
        pr_warn("dma_map_sg returned 0\n");
        errcode = -EAGAIN;
        goto cleanup_sgtbl;
    }

    pr_debug("dma_map_sg returned %d\n", sg_count);

    {
        struct scatterlist *sg;
        int sg_idx;

        for_each_sg(sgtbl.sgl, sg, sg_count, sg_idx) {
            unsigned long hwaddr = sg_dma_address(sg);
            unsigned int dmalen = sg_dma_len(sg);
            pr_debug("%d: %#08lx %u\n", sg_idx, hwaddr, dmalen);
        }
    }

// cleanup_sglist:
    dma_unmap_sg(dev, sgtbl.sgl, sgtbl.nents, DMA_FROM_DEVICE);

cleanup_sgtbl:
    sg_free_table(&sgtbl);

cleanup_pages:
    for (page_idx=0; page_idx < actual_pages; page_idx++) {
        // alias page_cache_release has been removed
        set_page_dirty(pages[page_idx]);
        put_page(pages[page_idx]);
    }

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
    struct mymiscdev_data *drvdata = container_of(miscdev, struct mymiscdev_data, miscdev);
    struct device *dev = miscdev->parent;

    int rc;
    long length = vma->vm_end - vma->vm_start;

    pr_debug("vma: %#lx %#lx %ld\n", vma->vm_start, vma->vm_end, vma->vm_pgoff);

    if (length > DMABUFSIZE) {
        return -EIO;
    }

    if (vma->vm_pgoff == 0)
    {
        // for some reason, dma_mmap_coherent will fail for vm_pgoff != 0.
        // dma_mmap_coherent makes use of vm_pgoff to calculate the offset of
        // cpu_addr when calling remap_pfn_range, so passing in non-zero vm_pgoff
        // isn't what we want anyway.
        struct DmaAddress *da = &drvdata->da_coherent;

        rc = dma_mmap_coherent(dev, vma, da->virtual, da->dma_handle, length);
        if (rc!=0) {
            pr_warn("dma_mmap_coherent failed %d\n", rc);
        }
    }
    else if (vma->vm_pgoff == 1)
    {
        struct DmaAddress *da = &drvdata->da_coherent;

        unsigned long pfn;
        if (is_vmalloc_addr(da->virtual)) {
            pfn = vmalloc_to_pfn(da->virtual);
        } else {
            pfn = virt_to_phys(da->virtual) >> PAGE_SHIFT;
        }

        // vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        rc = remap_pfn_range(vma, vma->vm_start,
                             pfn,
                             length, vma->vm_page_prot);
        if (rc!=0) {
            pr_warn("remap_pfn_range failed %d\n", rc);
        }
    }
    else if (vma->vm_pgoff == 2)
    {
        struct DmaAddress *da = &drvdata->da_single;

        rc = remap_pfn_range(vma, vma->vm_start,
                             virt_to_phys(da->virtual) >> PAGE_SHIFT,
                             length, vma->vm_page_prot);
        if (rc!=0) {
            pr_warn("remap_pfn_range failed %d\n", rc);
        }
    }
    else
    {
        rc = -EIO;
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

static struct pci_device_id my_driver_id_table[] = {
    { PCI_DEVICE(0x1234, 0x11e8) },
    {0, }
};

MODULE_DEVICE_TABLE(pci, my_driver_id_table);

static int mypcidev_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    int err;
    int bar = 0;
    struct mymiscdev_data *drvdata;
    unsigned long mmio_base, mmio_len;
    struct DmaAddress *da;

    pr_debug("%s\n", __func__);

    drvdata = devm_kzalloc(&pdev->dev, sizeof(*drvdata), GFP_KERNEL);
    if (!drvdata)
        return -ENOMEM;

    pr_debug("pcim_enable_device: %d\n", err);
    err = pcim_enable_device(pdev);
    if (err) {
        return err;
    }

    // request and map
    err = pcim_iomap_regions(pdev, BIT(bar), "mypcidev");
    pr_debug("pcim_iomap_regions: %d\n", err);
    if (err) {
        return err;
    }

    mmio_base = pci_resource_start(pdev, bar);
    mmio_len = pci_resource_len(pdev, bar);
    pr_debug("bar %d: base 0x%lx; len 0x%lx\n", bar, mmio_base, mmio_len);

    drvdata->iomem = pcim_iomap_table(pdev)[bar];
    pr_debug("pcim_iomap_table: %p\n", drvdata->iomem);

    pci_set_master(pdev);

    pci_set_drvdata(pdev, drvdata);

    if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(28))) {
        pr_warn("mypcidev: No suitable DMA available\n");
    }

    da = &drvdata->da_coherent;
    da->virtual = dmam_alloc_coherent(&pdev->dev, DMABUFSIZE, &da->dma_handle, GFP_KERNEL);
    if (!da->virtual) {
        pr_warn("dmam_alloc_coherent failed\n");
        return -ENOMEM;
    }
    // dma_alloc_coherent returns kernel virtual addresses (at least on ARM)
    print_pfn(da->virtual, da->dma_handle);

    da = &drvdata->da_single;
    da->virtual = (void*)__get_free_pages(GFP_KERNEL, DMABUFSIZE_ORDER);
    if (!da->virtual) {
        pr_warn("get_free_pages failed\n");
        return -ENOMEM;
    }
    #if 0
    da->dma_handle = dma_map_single(&pdev->dev, da->virtual, DMABUFSIZE, DMA_FROM_DEVICE);
    if (dma_mapping_error(&pdev->dev, da->dma_handle)) {
        pr_warn("dma_map_single failed\n");

        free_pages((unsigned long)da->virtual, DMABUFSIZE_ORDER);
        return -EBUSY;
    }
    // __get_free_pages returns kernel logical addresses
    print_pfn(da->virtual, da->dma_handle);
    #endif

    if (gpioButton >= 0)
        setup_gpio(&pdev->dev);

    drvdata->miscdev.minor = MISC_DYNAMIC_MINOR,
    drvdata->miscdev.name = "mymiscdev",
    drvdata->miscdev.fops = &sample_fops,
    drvdata->miscdev.parent = &pdev->dev;
    drvdata->miscdev.mode = S_IRUGO | S_IWUGO,

    err = misc_register(&drvdata->miscdev);
    if (err) {
        pr_warn("misc_register failed %d\n", err);

        da = &drvdata->da_single;
        #if 0
        dma_unmap_single(&pdev->dev, da->dma_handle, DMABUFSIZE, DMA_FROM_DEVICE);
        #endif
        free_pages((unsigned long)da->virtual, DMABUFSIZE_ORDER);
    }

    return err;
}

static void mypcidev_remove(struct pci_dev *pdev)
{
    struct mymiscdev_data *drvdata = pci_get_drvdata(pdev);
    struct DmaAddress *da;

    pr_debug("%s\n", __func__);

    misc_deregister(&drvdata->miscdev);

    da = &drvdata->da_single;
    #if 0
    dma_unmap_single(&pdev->dev, da->dma_handle, DMABUFSIZE, DMA_FROM_DEVICE);
    #endif
    free_pages((unsigned long)da->virtual, DMABUFSIZE_ORDER);
}

static struct pci_driver my_driver = {
    .name = "mypcidev",
    .id_table = my_driver_id_table,
    .probe = mypcidev_probe,
    .remove = mypcidev_remove,
};

static int __init device_init(void)
{
    int err;

    err = pci_register_driver(&my_driver);
    if (err)
        return err;

    return err;
}

static void __exit device_exit(void)
{
    pci_unregister_driver(&my_driver);
}

module_init(device_init)
module_exit(device_exit)

MODULE_DESCRIPTION("Simple Misc Driver");
MODULE_AUTHOR("Kiu Shueng Chuan");
MODULE_LICENSE("Dual MIT/GPL");

