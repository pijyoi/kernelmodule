#ifndef MYMISCDEV_IOCTL_H
#define MYMISCDEV_IOCTL_H

#include <linux/ioctl.h>

#define SAMPLE_IOCTL_MAGIC_NUMBER 0xA5

struct mymiscdev_ioctl
{
    char *buffer;
    size_t len;
};

struct mymiscdev_ioctl_reg
{
    int write;
    uint32_t addr;
    uint32_t val;
};

struct mymiscdev_ioctl_dma
{
    int write;      // from cpu's point of view
    uint32_t offset;    // offset of bounce buffer
    uint32_t len;
};

#define SAMPLE_IOCTL_CMD_1 \
    _IOR(SAMPLE_IOCTL_MAGIC_NUMBER, 1, struct mymiscdev_ioctl)

#define MYMISCDEV_IOCTL_REG \
    _IOWR(SAMPLE_IOCTL_MAGIC_NUMBER, 2, struct mymiscdev_ioctl_reg)

#define MYMISCDEV_IOCTL_DMA \
    _IOW(SAMPLE_IOCTL_MAGIC_NUMBER, 3, struct mymiscdev_ioctl_dma)

#endif

