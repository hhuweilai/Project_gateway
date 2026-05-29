/*
 * bh1750_test.c -- user-space test program for BH1750 driver
 *
 * Usage: ./bh1750_test [read|write <value>|ioctl|loop]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define DEVICE    "/dev/bh1750"
#define READ_CMD   1

int main(int argc, char *argv[])
{
    int fd, value;
    char buf[32];

    fd = open(DEVICE, O_RDWR);
    if (fd < 0) {
        perror("open /dev/bh1750");
        return 1;
    }

    if (argc < 2 || strcmp(argv[1], "read") == 0) {
        /* ===== read ===== */
        memset(buf, 0, sizeof(buf));
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf));
        printf("Light: %s", buf);

    } else if (strcmp(argv[1], "write") == 0 && argc == 3) {
        /* ===== write ===== */
        snprintf(buf, sizeof(buf), "%s", argv[2]);
        write(fd, buf, strlen(buf));
        printf("Value written: %s\n", argv[2]);

        /* read back to confirm */
        memset(buf, 0, sizeof(buf));
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf));
        printf("Confirm read: %s", buf);

    } else if (strcmp(argv[1], "ioctl") == 0) {
        /* ===== ioctl ===== */
        if (ioctl(fd, READ_CMD, &value) == 0)
            printf("IOCTL read: %u lux\n", value);
        else
            perror("ioctl");

    } else if (strcmp(argv[1], "loop") == 0) {
        /* ===== stress test ===== */
        for (int i = 0; i < 10; i++) {
            memset(buf, 0, sizeof(buf));
            read(fd, buf, sizeof(buf));
            printf("[%d] %s", i, buf);
            usleep(100000);
        }

    } else {
        printf("Usage: %s read|write <val>|ioctl|loop\n", argv[0]);
    }

    close(fd);
    return 0;
}