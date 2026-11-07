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
int optical_count(void);

struct medium_file {
    int drive;
    uint32_t lba;  /* its first block */
    uint64_t size; /* bytes */
};

/* The file at `path` (as on the ISO 9660 image, Rock Ridge names) whose size is `size`, on any optical drive. */
bool medium_find(const char *path, uint64_t size, struct medium_file *f);
/* As medium_find, waiting up to `ms` for a drive that has it (a USB stick is found a moment after the start) */
bool medium_find_wait(const char *path, uint64_t size, struct medium_file *f, uint64_t ms);
/* `n` bytes of the file from `off` (a multiple of 2048) into `buf`, which has room for `n` rounded up to 2048.
 * 0 or a negative errno. */
int medium_read(const struct medium_file *f, uint64_t off, void *buf, size_t n);

/* A file on the medium as the source of in-memory files' pages (mm.h): the context is the medium_file (which
 * must outlive them), the offset where in it each file's bytes are. So the base system's files are read from
 * the medium as they are used, not all at boot. */
struct pagesrc;
extern const struct pagesrc medium_pages;
/* How many are reading through it now (or waiting to): a reader in the background (fs/cpio.c's read-ahead)
 * lets them go first. */
int medium_readers(void);
