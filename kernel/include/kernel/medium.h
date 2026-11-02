#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The medium the system started from (a CD/DVD image in a virtual machine), read by the kernel itself: the
 * big files on it (the base system, the model that comes with it) are left to the kernel rather than to the
 * firmware, which reads them many times slower (drivers/medium.c). */

/* An optical drive's reader: `count` 2048-byte blocks from `lba` into `buf` (any kernel memory). */
typedef int (*optical_read_fn)(void *drv, uint32_t lba, uint32_t count, void *buf);
void optical_register(const char *name, optical_read_fn read, void *drv, uint32_t blocks);

struct medium_file {
    int drive;
    uint32_t lba;  /* its first block */
    uint64_t size; /* bytes */
};

/* The file at `path` (as on the ISO 9660 image, Rock Ridge names) whose size is `size`, on any optical drive. */
bool medium_find(const char *path, uint64_t size, struct medium_file *f);
/* `n` bytes of the file from `off` (a multiple of 2048) into `buf`, which has room for `n` rounded up to 2048.
 * 0 or a negative errno. */
int medium_read(const struct medium_file *f, uint64_t off, void *buf, size_t n);
