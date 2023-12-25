#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "edu.h"

static const char * const device_path = "/dev/edu";

static void print_usage(const char *argv0) {
    fprintf(stderr, "Usage: %s ident\n", argv0);
    fprintf(stderr, "       %s liveness <number>\n", argv0);
    fprintf(stderr, "       %s factorial <number>\n", argv0);
    fprintf(stderr, "       %s wait\n", argv0);
    fprintf(stderr, "       %s raise <number>\n", argv0);
    fprintf(stderr, "       %s dma-write\n", argv0);
    fprintf(stderr, "       %s dma-read <size>\n", argv0);
}

static bool parse_int_arg(const char *s, unsigned int *val) {
    const bool is_hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    return sscanf(s, is_hex ? "%x" : "%u", val) == 1;
}

int main(int argc, char *argv[]) {
    int fd;
    int ret;
    unsigned int val = 0;
    void *dma_mem;
    ssize_t num_read;
    ssize_t num_written;

    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    fd = open(device_path, O_RDWR);
    if (fd < 0) {
        perror("open");
        return -fd;
    }
    if (strcmp(argv[1], "ident") == 0) {
        ret = ioctl(fd, EDU_IOCTL_IDENT, &val);
        if (ret) goto ioctl_fail;
        // Format is 0xRRrr00ed
        // (RR = major version, rr = minor version)
        if ((val & 0xff) != 0xed) {
            fprintf(stderr, "read value has unexpected format: 0x%08x\n", val);
            return EXIT_FAILURE;
        }
        printf("Major version = %u\n", (val & 0xff000000) >> 24);
        printf("Minor version = %u\n", (val & 0x00ff0000) >> 16);
    } else if (strcmp(argv[1], "liveness") == 0) {
        if (argc != 3 || !parse_int_arg(argv[2], &val)) {
            goto bad_usage;
        }
        ret = ioctl(fd, EDU_IOCTL_LIVENESS, &val);
        if (ret) goto ioctl_fail;
        printf("Inverted value: 0x%08x\n", val);
    } else if (strcmp(argv[1], "factorial") == 0) {
        if (argc != 3 || !parse_int_arg(argv[2], &val)) {
            goto bad_usage;
        }
        ret = ioctl(fd, EDU_IOCTL_FACTORIAL, &val);
        if (ret) goto ioctl_fail;
        printf("Computed value: %u\n", val);
    } else if (strcmp(argv[1], "wait") == 0) {
        ret = ioctl(fd, EDU_IOCTL_WAIT_IRQ, &val);
        if (ret) goto ioctl_fail;
        printf("IRQ value: %u\n", val);
    } else if (strcmp(argv[1], "raise") == 0) {
        if (argc != 3 || !parse_int_arg(argv[2], &val)) {
            goto bad_usage;
        }
        ret = ioctl(fd, EDU_IOCTL_RAISE_IRQ, (unsigned int)val);
        if (ret) goto ioctl_fail;
    } else if (strcmp(argv[1], "dma-write") == 0) {
        dma_mem = mmap(NULL, EDU_DMA_BUF_SIZE, PROT_WRITE, MAP_SHARED, fd, 0);
        if (dma_mem == MAP_FAILED) goto mmap_fail;
        num_read = read(STDIN_FILENO, dma_mem, EDU_DMA_BUF_SIZE);
        if (num_read < 0) goto read_fail;
        ret = ioctl(fd, EDU_IOCTL_DMA_TO_DEVICE, (unsigned int)num_read);
        if (ret) goto ioctl_fail;
    } else if (strcmp(argv[1], "dma-read") == 0) {
        if (argc != 3 || !parse_int_arg(argv[2], &val)) {
            goto bad_usage;
        }
        dma_mem = mmap(NULL, EDU_DMA_BUF_SIZE, PROT_READ, MAP_SHARED, fd, 0);
        if (dma_mem == MAP_FAILED) goto mmap_fail;
        ret = ioctl(fd, EDU_IOCTL_DMA_FROM_DEVICE, val);
        if (ret) goto ioctl_fail;
        num_written = write(STDOUT_FILENO, dma_mem, val);
        if (num_written < 0) goto write_fail;
        else if (num_written < val) fprintf(stderr, "WARN: partial write\n");
    } else {
        goto bad_usage;
    }
    return EXIT_SUCCESS;
bad_usage:
    print_usage(argv[0]);
    return EXIT_FAILURE;
ioctl_fail:
    perror("ioctl");
    return EXIT_FAILURE;
mmap_fail:
    perror("mmap");
    return EXIT_FAILURE;
read_fail:
    perror("read");
    return EXIT_FAILURE;
write_fail:
    perror("write");
    return EXIT_FAILURE;
}
