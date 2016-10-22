#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mymiscdev_ioctl.h"

int main()
{
    int fd = open("/dev/simple_misc", O_RDWR);
    if (fd==-1) {
        perror("open");
        return -1;
    }

    size_t bufsize = 32768;
    int rc;

    // pass offset==0 for driver allocated memory
    // or offset!=0 for a kernel cmdline reserved memory region
    void *mapptr = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapptr==MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    void *memptr = 0;
    rc = posix_memalign(&memptr, 128, bufsize);
    assert(memptr!=0);

    // get_user_pages will fail
    // but that's okay. we already know the physical address.
    // it doesn't make sense to convert from physical to virtual then back to physical
    rc = read(fd, mapptr, bufsize);
    if (rc==-1) {
        perror("read");
    }

    printf("%p %zu\n", memptr, bufsize);
    struct mymiscdev_ioctl param = { memptr, bufsize };
    rc = ioctl(fd, SAMPLE_IOCTL_CMD_1, &param);
    if (rc==-1) {
        perror("read");
    }

    free(memptr);
    close(fd);

    return 0;
}

