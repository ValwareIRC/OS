#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/medium.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <linux/abi.h>

/* The medium the system started from, read by the kernel (kernel/medium.h): optical drives as their drivers
 * find them (ahci.c), and files on them found by path in their ISO 9660 file system, by the Rock Ridge names
 * the image is written with (xorriso -R), else by the plain ISO names. Read only, and only by the kernel: no
 * /dev node, so the disks keep their names (/dev/sda is where the person's state is kept). */

#define BLOCK 2048
#define MAX_DRIVES 4

static struct drive {
    char name[16];
    optical_read_fn read;
    void *drv;
    uint32_t blocks;
} drives[MAX_DRIVES];
static int ndrives;
static volatile int busy; /* one request at a time per kernel: the drives are slow to switch anyway */

void optical_register(const char *name, optical_read_fn read, void *drv, uint32_t blocks) {
    if (ndrives == MAX_DRIVES)
        return;
    struct drive *d = &drives[ndrives++];
    ksnprintf(d->name, sizeof d->name, "%s", name);
    d->read = read, d->drv = drv, d->blocks = blocks;
}

static int read_blocks(int drive, uint32_t lba, uint32_t count, void *buf) {
    struct drive *d = &drives[drive];
    if ((uint64_t)lba + count > d->blocks)
        return -EIO;
    while (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
        thread_yield();
    int r = d->read(d->drv, lba, count, buf);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return r;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* A directory record's name: its Rock Ridge NM, or the ISO name without ";1" and a trailing dot, lowercased. */
static void record_name(const uint8_t *r, char *out, size_t n) {
    uint8_t len = r[0], nlen = r[32];
    size_t k = 0;
    size_t su = 33 + nlen + ((nlen & 1) ? 0 : 1); /* System Use, after the name and its pad byte */
    bool rr = false;
    while (su + 4 <= len) {
        const uint8_t *e = r + su;
        uint8_t elen = e[2];
        if (elen < 4 || su + elen > len)
            break;
        if (e[0] == 'N' && e[1] == 'M' && elen >= 5) {
            for (int i = 5; i < elen && k + 1 < n; i++)
                out[k++] = (char)e[i];
            rr = true;
            if (!(e[4] & 1)) /* no continuation */
                break;
        }
        su += elen;
    }
    if (!rr) {
        for (int i = 0; i < nlen && k + 1 < n && r[33 + i] != ';'; i++)
            out[k++] = lower((char)r[33 + i]);
        if (k && out[k - 1] == '.')
            k--;
    }
    out[k] = 0;
}

/* In the directory at (lba, size), the entry `name`: its extent, size and whether it is a directory. */
static bool dir_find(int drive, uint32_t lba, uint32_t size, const char *name, size_t nl, uint32_t *elba,
                     uint64_t *esize, bool *isdir) {
    uint8_t *buf = kmalloc(BLOCK);
    if (!buf)
        return false;
    bool found = false;
    for (uint32_t b = 0; b * BLOCK < size && !found; b++) {
        if (read_blocks(drive, lba + b, 1, buf) < 0)
            break;
        for (uint32_t off = 0; off < BLOCK && !found;) {
            const uint8_t *r = buf + off;
            if (r[0] == 0)
                break; /* the rest of this block is padding */
            if (off + r[0] > BLOCK || r[0] < 34)
                break;
            bool special = r[32] == 1 && (r[33] == 0 || r[33] == 1); /* . and .. */
            if (!special) {
                char got[256];
                record_name(r, got, sizeof got);
                if (strlen(got) == nl && !strncmp(got, name, nl)) {
                    *elba = le32(r + 2);
                    *esize = le32(r + 10);
                    *isdir = r[25] & 2;
                    found = true;
                }
            }
            off += r[0];
        }
    }
    kfree(buf);
    return found;
}

static bool find_on(int drive, const char *path, uint64_t size, struct medium_file *f) {
    uint8_t *pvd = kmalloc(BLOCK);
    if (!pvd)
        return false;
    bool ok = false;
    /* the volume descriptors from block 16: the primary one (type 1) holds the root directory's record */
    for (uint32_t b = 16; b < 32; b++) {
        if (read_blocks(drive, b, 1, pvd) < 0 || memcmp(pvd + 1, "CD001", 5))
            break;
        if (pvd[0] == 255)
            break;
        if (pvd[0] != 1)
            continue;
        uint32_t lba = le32(pvd + 156 + 2);
        uint64_t sz = le32(pvd + 156 + 10);
        bool dir = true;
        const char *p = path;
        while (*p == '/')
            p++;
        ok = true;
        while (*p && ok) {
            const char *e = p;
            while (*e && *e != '/')
                e++;
            if (!dir) {
                ok = false;
                break;
            }
            ok = dir_find(drive, lba, (uint32_t)sz, p, (size_t)(e - p), &lba, &sz, &dir);
            p = e;
            while (*p == '/')
                p++;
        }
        ok = ok && !dir && sz == size;
        if (ok)
            *f = (struct medium_file){drive, lba, sz};
        break;
    }
    kfree(pvd);
    return ok;
}

int optical_count(void) { return ndrives; }

bool medium_find(const char *path, uint64_t size, struct medium_file *f) {
    for (int i = 0; i < ndrives; i++)
        if (find_on(i, path, size, f)) {
            kprintf("medium: %s on %s at block %u (%lu MiB)\n", path, drives[i].name, f->lba,
                    (unsigned long)(size >> 20));
            return true;
        }
    return false;
}

int medium_read(const struct medium_file *f, uint64_t off, void *buf, size_t n) {
    if (off % BLOCK || off > f->size)
        return -EINVAL;
    if (n > f->size - off)
        n = f->size - off;
    uint32_t count = (uint32_t)((n + BLOCK - 1) / BLOCK);
    return count ? read_blocks(f->drive, f->lba + (uint32_t)(off / BLOCK), count, buf) : 0;
}

/* ---- files read from the medium as they are used ----
 * Any bytes of a file on it, as the pages of in-memory files want them (the base system's, fs/cpio.c). The drive
 * reads whole blocks and each trip to it costs, while the small files of a directory lie side by side: so the
 * last few stretches read are kept (WINDOWS of WIN bytes each) and the next small file mostly comes from one.
 * Long reads (a run of a big file) go through a buffer of their own instead, not crowding the small ones out. */

#define WIN (256u << 10)
#define WINDOWS 8
#define LONG_READ (1u << 20) /* + a page, for the blocks at either end */

static struct window {
    const struct medium_file *f;
    uint64_t off, len, used;
    uint8_t *data;
} windows[WINDOWS];
static uint8_t *long_buf;
static uint64_t win_clock;
static volatile int win_busy; /* the windows and the long buffer are one reader's at a time */

static struct window *window_at(const struct medium_file *f, uint64_t start, int *err) {
    struct window *w = NULL, *old = &windows[0];
    for (int i = 0; i < WINDOWS; i++) {
        if (windows[i].f == f && windows[i].off == start && windows[i].len)
            w = &windows[i];
        if (windows[i].used < old->used)
            old = &windows[i];
    }
    if (!w) {
        w = old;
        if (!w->data) {
            uint64_t p = pmm_alloc(WIN / PAGE_SIZE);
            if (!p) {
                *err = -ENOMEM;
                return NULL;
            }
            w->data = phys_to_virt(p);
        }
        w->f = NULL;
        uint64_t len = f->size - start < WIN ? f->size - start : WIN;
        int r = medium_read(f, start, w->data, len);
        if (r < 0) {
            *err = r;
            return NULL;
        }
        w->f = f, w->off = start, w->len = len;
    }
    w->used = ++win_clock;
    return w;
}

static int64_t medium_pages_read(void *ctx, uint64_t off, void *buf, size_t len) {
    const struct medium_file *f = ctx;
    if (off >= f->size)
        return 0;
    if (len > f->size - off)
        len = f->size - off;
    while (__atomic_exchange_n(&win_busy, 1, __ATOMIC_ACQUIRE))
        thread_yield();
    uint8_t *dst = buf;
    size_t done = 0;
    int err = 0;
    while (done < len && !err) {
        uint64_t pos = off + done;
        size_t left = len - done;
        if (left >= WIN) {
            if (!long_buf) {
                uint64_t p = pmm_alloc(LONG_READ / PAGE_SIZE + 1);
                if (!p) {
                    err = -ENOMEM;
                    break;
                }
                long_buf = phys_to_virt(p);
            }
            uint64_t a = ALIGN_DOWN(pos, BLOCK);
            size_t n = left < LONG_READ ? left : LONG_READ;
            int r = medium_read(f, a, long_buf, pos - a + n);
            if (r < 0) {
                err = r;
                break;
            }
            memcpy(dst + done, long_buf + (pos - a), n);
            done += n;
            continue;
        }
        struct window *w = window_at(f, ALIGN_DOWN(pos, WIN), &err);
        if (!w)
            break;
        size_t n = w->off + w->len - pos;
        if (n > left)
            n = left;
        memcpy(dst + done, w->data + (pos - w->off), n);
        done += n;
    }
    __atomic_store_n(&win_busy, 0, __ATOMIC_RELEASE);
    return done ? (int64_t)done : err;
}

const struct pagesrc medium_pages = {.read = medium_pages_read, .max_run = 256};
