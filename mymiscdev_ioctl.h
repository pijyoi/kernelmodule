#ifndef MYMISCDEV_IOCTL_H
#define MYMISCDEV_IOCTL_H

#include <linux/ioctl.h>

#define SAMPLE_IOCTL_MAGIC_NUMBER 0xA5

struct mymiscdev_ioctl
{
    char *buffer;
    size_t len;
};

#define SAMPLE_IOCTL_CMD_1 \
    _IOR(SAMPLE_IOCTL_MAGIC_NUMBER, 1, struct mymiscdev_ioctl)

#endif

