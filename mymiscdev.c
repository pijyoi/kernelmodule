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

struct miscdevice sample_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "simple_misc",
    .fops = &sample_fops,
    .mode = S_IRUGO | S_IWUGO,
};

#define DMABUFSIZE 1048576
static void *alloc_ptr;
static dma_addr_t dma_handle;

static DECLARE_WAIT_QUEUE_HEAD(device_read_wait);
static unsigned int readable_count;



static int device_open(struct inode *inodep, struct file *filp)
{
    pr_debug("device_open\n");
    nonseekable_open(inodep, filp);
    return 0;
}

static int device_release(struct inode *inodep, struct file *filp)
{
    pr_debug("device_release\n");
    return 0;
}

static long device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int retcode = -ENOTTY;

    pr_debug("device_ioctl\n");

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

    sg_count = dma_map_sg(miscdev->this_device, sglist, num_pages, DMA_FROM_DEVICE);
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
    dma_unmap_sg(miscdev->this_device, sglist, num_pages, DMA_FROM_DEVICE);

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
    int nonblock = filp->f_flags & O_NONBLOCK;
    ssize_t readcnt;

    pr_debug("%s %zu\n", __func__, nbytes);

    if (nbytes==0) {
        return 0;
    }

    while (1) {
        if (readable_count > 0) {
            readcnt = readable_count <= nbytes ? readable_count : nbytes;    
            readable_count -= readcnt;
            return readcnt;
        }

        if (nonblock) {
            return -EAGAIN;
        }

        wait_event_interruptible(device_read_wait, readable_count > 0);

        if (signal_pending(current)) {
            pr_debug("%s got signal\n", __func__);
            return -ERESTARTSYS;
        }
    }
}

static ssize_t device_write(struct file *filp, const char __user *userbuf, size_t nbytes, loff_t *f_pos)
{
    pr_debug("device_write %zu, %llu\n", nbytes, *f_pos);
    return nbytes;
}

static int device_mmap(struct file *filp, struct vm_area_struct *vma)
{
    int rc;
    if (vma->vm_pgoff == 0)
    {
        long length = vma->vm_end - vma->vm_start;
        if (length > DMABUFSIZE) {
            return -EIO;
        }
        #if 0
        rc = dma_mmap_coherent(NULL, vma, alloc_ptr, dma_handle, length);
        if (rc!=0) {
            pr_warning("dma_mmap_coherent failed %d\n", rc);
        }
        #else
        // vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
        rc = remap_pfn_range(vma, vma->vm_start,
                             PHYS_PFN(virt_to_phys(bus_to_virt(dma_handle))),
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
    if (readable_count > 0) {
        mask |= POLLIN | POLLRDNORM;
    }
    return mask;
}

static int __init device_init(void)
{
    int rc;

    pr_debug("module_init\n");

    alloc_ptr = dma_alloc_coherent(NULL, DMABUFSIZE, &dma_handle, GFP_KERNEL);
    if (!alloc_ptr) {
        pr_warning("dma_alloc_coherent failed\n");
        return -ENOMEM;
    }

    pr_debug("dma_handle: %#llx\n", (unsigned long long)dma_handle);

    rc = misc_register(&sample_device);
    if (rc!=0) {
        pr_warning("misc_register failed %d\n", rc);
        dma_free_coherent(NULL, DMABUFSIZE, alloc_ptr, dma_handle);
    }
    return rc;
}

static void __exit device_exit(void)
{
    pr_debug("module_exit\n");

    misc_deregister(&sample_device);

    dma_free_coherent(NULL, DMABUFSIZE, alloc_ptr, dma_handle);
}

module_init(device_init)
module_exit(device_exit)

MODULE_DESCRIPTION("Simple Misc Driver");
MODULE_AUTHOR("Kiu Shueng Chuan");
MODULE_LICENSE("Dual MIT/GPL");

