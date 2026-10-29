#include <kernel/blkdev.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>

/* The block layer: turns sector-granular disk drivers into /dev/sdX nodes
 * that behave like Linux block devices: read/write at any byte offset and
 * length, lseek/fstat report the size, BLKGETSIZE64 & co, fsync flushes the
 * drive's write cache. Requests are serialized per disk; a driver therefore
 * only ever sees one transfer at a time and needs no locking of its own.
 *
 * Every disk gets an sd* name in discovery order, whichever controller it is
 * on, so user space can always start from /dev/sda. */

#define BOUNCE_BYTES (64 * 1024)

/* ioctl numbers from <linux/fs.h> */
#define BLKROGET     0x125e
#define BLKGETSIZE   0x1260
#define BLKFLSBUF    0x1261
#define BLKSSZGET    0x1268
#define BLKPBSZGET   0x127b
#define BLKGETSIZE64 0x80081272

#define SCSI_DISK_MAJOR 8

static struct blkdev *disks;
static int ndisks;

/* A disk request can take milliseconds and polls for completion, so waiters
 * yield instead of spinning with interrupts off. */
static void disk_lock(struct blkdev *d) {
    while (__atomic_exchange_n(&d->busy, 1, __ATOMIC_ACQUIRE))
        thread_yield();
}

static void disk_unlock(struct blkdev *d) { __atomic_store_n(&d->busy, 0, __ATOMIC_RELEASE); }

static uint64_t disk_bytes(const struct blkdev *d) { return d->sectors * d->sector_size; }

/* Moves bytes [off, off+len) between the disk and `buf`. Whole, aligned
 * sectors go straight to the caller's buffer; partial sectors at either end
 * (and buffers too misaligned for DMA) are staged through the bounce buffer,
 * with a read-modify-write for partially overwritten sectors. */
static int64_t xfer(struct blkdev *d, uint8_t *buf, size_t len, uint64_t off, bool write) {
    uint64_t size = disk_bytes(d);
    if (len == 0)
        return 0;
    if (off >= size)
        return write ? -ENOSPC : 0;
    if (len > size - off)
        len = size - off;
    uint32_t ss = d->sector_size;
    size_t done = 0;
    int r = 0;
    disk_lock(d);
    while (done < len) {
        uint64_t pos = off + done, lba = pos / ss;
        size_t in = pos % ss, left = len - done;
        if (in == 0 && left >= ss && ((uintptr_t)(buf + done) & 3) == 0) {
            uint64_t n = left / ss;
            r = d->rw(d, lba, n, buf + done, write);
            if (r < 0)
                break;
            done += n * ss;
            continue;
        }
        size_t end = in + left; /* bytes from the start of sector `lba` */
        if (end > BOUNCE_BYTES)
            end = BOUNCE_BYTES;
        size_t bytes = end - in;
        uint64_t nsec = (end + ss - 1) / ss;
        if (write) {
            if (in)
                r = d->rw(d, lba, 1, d->bounce, false);
            uint64_t last = nsec - 1;
            if (r == 0 && end % ss && !(last == 0 && in))
                r = d->rw(d, lba + last, 1, d->bounce + last * ss, false);
            if (r == 0) {
                memcpy(d->bounce + in, buf + done, bytes);
                r = d->rw(d, lba, nsec, d->bounce, true);
            }
        } else {
            r = d->rw(d, lba, nsec, d->bounce, false);
            if (r == 0)
                memcpy(buf + done, d->bounce + in, bytes);
        }
        if (r < 0)
            break;
        done += bytes;
    }
    if (write && done)
        d->dirty = true;
    disk_unlock(d);
    return done ? (int64_t)done : r;
}

int64_t blkdev_read(struct blkdev *d, void *buf, size_t len, uint64_t off) { return xfer(d, buf, len, off, false); }

int64_t blkdev_write(struct blkdev *d, const void *buf, size_t len, uint64_t off) {
    if (d->readonly || (d->part_of && d->part_of->readonly))
        return -EROFS;
    return xfer(d, (uint8_t *)buf, len, off, true);
}

int blkdev_flush(struct blkdev *d) {
    if (!d->flush)
        return 0;
    disk_lock(d);
    int r = d->flush(d);
    if (r == 0)
        d->dirty = false;
    disk_unlock(d);
    return r;
}

void blkdev_sync_all(void) {
    for (struct blkdev *d = disks; d; d = d->next)
        if (d->dirty)
            blkdev_flush(d);
}

struct blkdev *blkdev_find(const char *name) {
    for (struct blkdev *d = disks; d; d = d->next)
        if (!strcmp(d->name, name))
            return d;
    return NULL;
}

/* ---- /dev/sdX ---- */

static int64_t sd_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)f;
    return blkdev_read(v->data, buf, len, off);
}

static int64_t sd_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)f;
    return blkdev_write(v->data, buf, len, off);
}

static int64_t sd_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)f;
    struct blkdev *d = v->data;
    switch (req) {
    case BLKGETSIZE64: {
        uint64_t bytes = disk_bytes(d);
        return copy_to_user(arg, &bytes, sizeof bytes);
    }
    case BLKGETSIZE: { /* in 512-byte units whatever the sector size */
        uint64_t n = disk_bytes(d) / 512;
        return copy_to_user(arg, &n, sizeof n);
    }
    case BLKSSZGET:
    case BLKPBSZGET: {
        uint32_t ss = d->sector_size;
        return copy_to_user(arg, &ss, sizeof ss);
    }
    case BLKROGET: {
        int32_t ro = 0;
        return copy_to_user(arg, &ro, sizeof ro);
    }
    case BLKFLSBUF: /* no kernel page cache: the drive cache is all there is */
        return blkdev_flush(d);
    }
    return -ENOTTY;
}

/* Like Linux, a disk is always ready: requests block until done instead. */
static int sd_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)v, (void)f, (void)wq;
    return POLLIN | POLLOUT;
}

static int sd_fsync(struct vnode *v) { return blkdev_flush(v->data); }

static const struct vnode_ops sd_ops = {
    .read = sd_read, .write = sd_write, .ioctl = sd_ioctl, .poll = sd_poll, .fsync = sd_fsync};

int blkdev_register(struct blkdev *d) {
    if (!d->rw || !d->sectors || !d->sector_size || d->sector_size > BOUNCE_BYTES ||
        BOUNCE_BYTES % d->sector_size)
        return -EINVAL;
    if (!d->part_of && ndisks >= 26)
        return -ENOSPC;
    d->bounce = kmalloc(BOUNCE_BYTES);
    if (!d->bounce)
        return -ENOMEM;
    /* disks get the next letter; a partition is named after its disk (sda2) */
    int index = d->part_of ? (d->part_of->name[2] - 'a') : ndisks++;
    if (!d->part_of)
        ksnprintf(d->name, sizeof d->name, "sd%c", 'a' + index);
    d->next = NULL;
    struct blkdev **tail = &disks;
    while (*tail)
        tail = &(*tail)->next;
    *tail = d;

    struct vnode *v = vnode_alloc(VN_BLK, 0660, &sd_ops);
    if (!v)
        return -ENOMEM;
    uint32_t minor = (uint32_t)index * 16 + (uint32_t)d->part_number; /* 15 partition minors per disk, as on Linux */
    v->rdev = (minor & 0xff) | (SCSI_DISK_MAJOR << 8) | ((minor & ~0xffu) << 12);
    v->size = disk_bytes(d);
    v->data = d;
    char path[16];
    ksnprintf(path, sizeof path, "/dev/%s", d->name);
    vfs_create_node(path, v);
    vnode_put(v);
    return 0;
}

/* The person's disk: the save slots (userland/shell/store.c) use its first
 * 257 MiB; everything from 320 MiB on is /dev/sda2, the file system for
 * what doesn't fit there (apps from Flathub, their data). */
#define DATA_START (320ull << 20)

/* Blank in its first 64 KiB, or starting with the save slots' header
 * (userland/shell/store.c). */
static bool disk_is_ours(struct blkdev *d) {
    static uint8_t buf[4096];
    for (uint64_t off = 0; off < 65536; off += sizeof buf) {
        if (blkdev_read(d, buf, sizeof buf, off) != (int64_t)sizeof buf)
            return false;
        if (off == 0 && !memcmp(buf, "LUMENSTORE1", 11))
            return true;
        for (size_t i = 0; i < sizeof buf; i++)
            if (buf[i])
                return false;
    }
    return true;
}

void storage_init(void) {
    ahci_init();
    virtio_blk_init();
    if (!ndisks) {
        kprintf("storage: no disks\n");
        return;
    }
    /* On a real computer the first drive may be someone's Windows disk:
     * only one that is blank at its start (a new virtual disk) or already
     * carries this system's save header is ever written. */
    for (struct blkdev *d = disks; d; d = d->next)
        if (!d->part_of && !disk_is_ours(d)) {
            d->readonly = true;
            kprintf("storage: %s (%s) is not this system's disk: left untouched, read-only\n", d->name, d->model);
        }
    struct blkdev *sda = blkdev_find("sda");
    if (sda && !sda->readonly && disk_bytes(sda) >= DATA_START + (64ull << 20)) {
        uint64_t first = DATA_START / sda->sector_size;
        struct blkdev *p = blkdev_add_part(sda, first, sda->sectors - first, 2);
        if (p)
            kprintf("storage: %s is %s from %lu MiB (%lu MiB)\n", p->name, sda->name,
                    (unsigned long)(DATA_START >> 20), (unsigned long)(disk_bytes(p) >> 20));
    }
}

/* ---- partitions: a range of a disk as a block device of its own ---- */

struct part {
    struct blkdev *parent;
    uint64_t first; /* in the parent's sectors */
};

static int part_rw(struct blkdev *d, uint64_t lba, uint64_t count, void *buf, bool write) {
    struct part *p = d->drv;
    if (lba + count > d->sectors)
        return -EIO;
    disk_lock(p->parent); /* the disk sees one request at a time, from the disk or its partitions */
    int r = p->parent->rw(p->parent, p->first + lba, count, buf, write);
    if (r == 0 && write)
        p->parent->dirty = true;
    disk_unlock(p->parent);
    return r;
}

static int part_flush(struct blkdev *d) {
    struct part *p = d->drv;
    return blkdev_flush(p->parent);
}

struct blkdev *blkdev_add_part(struct blkdev *parent, uint64_t first, uint64_t sectors, int number) {
    if (!parent || first >= parent->sectors || !sectors)
        return NULL;
    if (sectors > parent->sectors - first)
        sectors = parent->sectors - first;
    struct blkdev *d = kzalloc(sizeof *d);
    struct part *p = kzalloc(sizeof *p);
    if (!d || !p) {
        kfree(d);
        kfree(p);
        return NULL;
    }
    *p = (struct part){parent, first};
    ksnprintf(d->name, sizeof d->name, "%s%d", parent->name, number);
    ksnprintf(d->model, sizeof d->model, "%s part %d", parent->name, number);
    d->sectors = sectors;
    d->sector_size = parent->sector_size;
    d->rw = part_rw;
    d->flush = part_flush;
    d->drv = p;
    d->part_of = parent;
    d->part_number = number;
    if (blkdev_register(d) < 0) {
        kfree(d);
        kfree(p);
        return NULL;
    }
    return d;
}
