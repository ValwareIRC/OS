#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Block devices. A disk driver (AHCI, virtio-blk) describes each disk with a
 * struct blkdev and registers it; the block layer names it (sda, sdb, ...),
 * serializes requests, and publishes /dev/sdX with byte-granular read/write
 * on top of whole-sector transfers. File systems inside the kernel (osfs
 * later) use the blkdev_read/write calls directly. */

struct blkdev {
    char name[8];       /* "sda": filled in by blkdev_register */
    char model[41];     /* for the boot log */
    uint64_t sectors;   /* logical sectors */
    uint32_t sector_size;
    /* Transfers `count` whole sectors starting at `lba` to/from `buf`, which
     * is any kernel memory with at least 4-byte alignment (drivers translate
     * it page by page for DMA). Returns 0 or a negative errno. */
    int (*rw)(struct blkdev *d, uint64_t lba, uint64_t count, void *buf, bool write);
    /* Makes completed writes durable (drains the drive's write cache). */
    int (*flush)(struct blkdev *d);
    void *drv;
    struct blkdev *part_of; /* a partition: the disk it is part of (else NULL) */
    int part_number;
    /* Not this system's disk (a real drive with someone's partitions on it):
     * it may be read, never written. */
    bool readonly;
    /* block layer state */
    volatile int busy;  /* one request at a time per disk (sleeping lock) */
    uint8_t *bounce;    /* sector-aligned staging for unaligned requests */
    bool dirty;         /* written since the last flush */
    struct blkdev *next;
};

/* Names the disk, creates /dev/<name> and returns 0, or a negative errno. */
int blkdev_register(struct blkdev *d);
struct blkdev *blkdev_find(const char *name);
/* Byte-granular access; the range is clamped to the disk. */
int64_t blkdev_read(struct blkdev *d, void *buf, size_t len, uint64_t off);
int64_t blkdev_write(struct blkdev *d, const void *buf, size_t len, uint64_t off);
int blkdev_flush(struct blkdev *d);
/* Flushes every disk (sync(2), power-off). */
void blkdev_sync_all(void);
/* A range of `parent` (in its sectors) as a block device of its own,
 * /dev/<parent><number> (sda2). NULL if it doesn't fit. */
struct blkdev *blkdev_add_part(struct blkdev *parent, uint64_t first, uint64_t sectors, int number);

/* Probes the storage controllers on the PCI bus (after pci_init). */
void storage_init(void);
void ahci_init(void);
void virtio_blk_init(void);
