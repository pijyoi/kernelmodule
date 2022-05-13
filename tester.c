#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mymiscdev_ioctl.h"

void sigint_handler(int sig)
{
}

int main()
{
    int fd = open("/dev/mymiscdev", O_RDWR);
    if (fd==-1) {
        perror("open");
        return -1;
    }

    size_t bufsize = 32768;
    int rc;

    long page_size = sysconf(_SC_PAGESIZE);

    // pass offset==1*page_size for driver allocated consistent dma mapping
    // pass offset==2*page_size for driver allocated streaming dma mapping
    void *mapptr = mmap(NULL, bufsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 1*page_size);
    if (mapptr==MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    memset(mapptr, 0x5A, bufsize);

    void *memptr = 0;
    rc = posix_memalign(&memptr, 128, bufsize);
    assert(memptr!=0);

    printf("%p %zu\n", memptr, bufsize);
    struct mymiscdev_ioctl param = { memptr, bufsize };
    rc = ioctl(fd, SAMPLE_IOCTL_CMD_1, &param);
    if (rc==-1) {
        perror("ioctl");
    }

    signal(SIGINT, sigint_handler);

	printf("attempting read\n");
    rc = read(fd, memptr, bufsize);
    if (rc==-1) {
        perror("read");
    }
	else {
		printf("read returned %d\n", rc);
	}

    free(memptr);
    close(fd);

    return 0;
}

