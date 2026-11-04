#include <kernel/blkdev.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

/* ext2: the person's disk (the area after the save slots, /dev/sda2), in the
 * format Linux's own tools read and check (mke2fs, e2fsck). Revision 1 with
 * the "filetype" feature, sparse superblocks and large files; no journal, no
 * extents, no htree indexes: plain block maps and linked directories.
 *
 * Blocks are kept in a cache and changes written back in batches, neighbours
 * together: every few seconds, when much has changed, on fsync and sync, as
 * Linux does (installing an app writes thousands of small files; one disk
 * trip per bitmap bit and inode made that take hours). Before the first
 * batch after a clean point, the superblock on the disk says "not cleanly
 * unmounted". A sync (and power-off) writes everything and marks it clean.
 *
 * With a journal (ext3's, in JBD2's format, which e2fsck replays), each batch
 * is one transaction, as ext3's ordered mode: files' data goes to its place,
 * the file system's own blocks (inodes, bitmaps, folders, block maps, the
 * descriptors, the superblock) to the journal with a commit record, and only
 * then to their places. A power cut at any moment leaves the disk as it was
 * before a batch or after it, never half way: e2fsck replays the last
 * transaction at the next start. The journal's superblock always names the
 * latest committed transaction alone, so nothing older is ever replayed over
 * what came after it (and replaying the latest again writes the same blocks).
 *
 * One lock per file system (it may be taken again by the same thread). Each
 * inode in use has exactly one vnode (the inode cache), so mounts, parents
 * and open files all agree on what a file is. */

#define EXT2_MAGIC 0xEF53
#define ROOT_INO 2
#define INCOMPAT_FILETYPE 0x0002
#define INCOMPAT_RECOVER 0x0004  /* the journal may hold a transaction to replay (set while mounted) */
#define COMPAT_HAS_JOURNAL 0x0004
#define JBD2_MAGIC 0xC03B3998u
#define JBD2_DESCRIPTOR 1
#define JBD2_COMMIT 2
#define JBD2_SUPER_V1 3
#define JBD2_SUPER_V2 4
#define JBD2_FLAG_ESCAPE 1
#define JBD2_FLAG_SAME_UUID 2
#define JBD2_FLAG_LAST_TAG 8
#define JBD2_INCOMPAT_REVOKE 0x1
#define JBD2_INCOMPAT_64BIT 0x2
#define RO_COMPAT_SPARSE 0x0001
#define RO_COMPAT_LARGE_FILE 0x0002
#define RO_COMPAT_HUGE_FILE 0x0008 /* i_blocks in fs blocks: refused */
#define STATE_VALID 1

#define S_IFMT_ 0xF000
#define S_IFDIR_ 0x4000
#define S_IFREG_ 0x8000
#define S_IFLNK_ 0xA000

struct ext2_inode {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t blocks, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, size_high, faddr;
    uint16_t blocks_hi, file_acl_hi, uid_hi, gid_hi;
    uint32_t reserved;
};
_Static_assert(sizeof(struct ext2_inode) == 128, "ext2 inode");

struct ext2_gd {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, pad;
    uint32_t reserved[3];
};
_Static_assert(sizeof(struct ext2_gd) == 32, "ext2 group descriptor");

struct dirent2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len, file_type;
    char name[];
};

struct enode;

/* A disk block held in memory (the block cache). */
struct bbuf {
    uint32_t blk;
    bool dirty;
    bool is_data; /* a file's contents (to its place first), not the file system's own (to the journal) */
    uint8_t *data;
    struct bbuf *hnext, *prev, *next; /* hash chain; recently used list */
};

#define BHASH 2048
#define BCACHE_BYTES (48u << 20)  /* what the cache may hold */
#define BDIRTY_BYTES (16u << 20)  /* changed and not yet written: past this, written at once */
#define FLUSH_EVERY_MS 5000      /* and at the latest this long after a change */

struct ext2fs {
    struct blkdev *dev;
    uint32_t bs; /* block size */
    uint32_t inode_size, ipg, bpg, groups, first_data, blocks_count, inodes_count, first_ino;
    bool filetype;
    uint8_t sb[1024]; /* the superblock as on disk (byte 1024) */
    struct ext2_gd *gd;
    uint32_t *bhint, *ihint; /* per group: where the last free block / inode was found */
    uint32_t gdt_block;
    /* the lock: owner thread and depth (taken again by the same thread is fine) */
    void *owner;
    int depth;
    volatile int busy;
    struct enode *cache[256]; /* inode cache, by inode number */
    struct vnode *root;
    struct ext2fs *next;
    /* the block cache: write-back, so a change costs memory now and one
     * batched write within a few seconds instead of a disk trip each */
    struct bbuf *bhash[BHASH];
    struct bbuf lru; /* .next most recent, .prev least */
    uint32_t nbufs, ndirty, maxbufs, maxdirty;
    bool sb_dirty, gd_dirty;
    bool disk_clean; /* what the superblock on the disk says */
    uint8_t *stage;  /* for writing runs of neighbouring blocks at once */
    /* the journal: its length and first log block, the next transaction's sequence and place, the last
     * transaction's place and length (not to be written over), where each log block is on the disk */
    bool jon;
    uint32_t jmax, jfirst, jseq, jhead, jlast_len, jtag;
    uint32_t *jmap;
    uint8_t *jsb;    /* the journal's superblock block, as on the disk */
    uint8_t *jblk;   /* a block for descriptors and commit records */
    uint8_t *sbblk;  /* the block holding the superblock (the rest of it as on the disk) */
    uint32_t sbblk_no;
    bool jwarned;
};

struct enode {
    struct ext2fs *fs;
    uint32_t ino;
    struct ext2_inode in; /* the first 128 bytes of the on-disk inode */
    struct vnode *v;
    struct enode *hnext;
    /* readdir in order: where the entry numbered rd_index is */
    uint64_t rd_index, rd_pos;
};

static struct ext2fs *mounted_fs;
static spinlock_t list_lock;

static const struct vnode_ops dir_ops, file_ops, link_ops;

/* ---- locking ---- */

static void fs_lock(struct ext2fs *fs) {
    void *me = thread_current();
    if (__atomic_load_n(&fs->owner, __ATOMIC_ACQUIRE) == me) {
        fs->depth++;
        return;
    }
    while (__atomic_exchange_n(&fs->busy, 1, __ATOMIC_ACQUIRE))
        thread_yield();
    __atomic_store_n(&fs->owner, me, __ATOMIC_RELEASE);
    fs->depth = 1;
}

static void fs_unlock(struct ext2fs *fs) {
    if (--fs->depth > 0)
        return;
    __atomic_store_n(&fs->owner, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&fs->busy, 0, __ATOMIC_RELEASE);
}

/* ---- the superblock and group descriptors ---- */

static uint32_t sb32(struct ext2fs *fs, int off) {
    uint32_t v;
    memcpy(&v, fs->sb + off, 4);
    return v;
}
static void sb32_set(struct ext2fs *fs, int off, uint32_t v) { memcpy(fs->sb + off, &v, 4); }

/* With a journal it stays "valid", as ext3/4 keep it: the journal's RECOVER flag says it was in use, and
 * e2fsck replays the journal then (seconds) instead of checking the whole disk after every power cut or closed
 * window (init asks for a full check every 30 starts, by tune2fs -c). Without one, "not valid" while mounted. */
static void sb_state(struct ext2fs *fs, bool clean) {
    if (fs->jon)
        clean = true;
    uint16_t state;
    memcpy(&state, fs->sb + 58, 2);
    state = clean ? (uint16_t)(state | STATE_VALID) : (uint16_t)(state & ~STATE_VALID);
    memcpy(fs->sb + 58, &state, 2);
}

/* The superblock to the disk now, saying clean or not. */
static int super_out(struct ext2fs *fs, bool clean) {
    if (fs->jon && clean)
        sb32_set(fs, 96, sb32(fs, 96) & ~(uint32_t)INCOMPAT_RECOVER);
    sb_state(fs, clean);
    sb32_set(fs, 48, (uint32_t)time_realtime_s()); /* s_wtime */
    int r = blkdev_write(fs->dev, fs->sb, 1024, 1024) == 1024 ? 0 : -EIO;
    sb_state(fs, false); /* in memory it is in use for as long as it is mounted */
    if (fs->jon)
        sb32_set(fs, 96, sb32(fs, 96) | INCOMPAT_RECOVER);
    if (r == 0)
        fs->disk_clean = clean;
    return r;
}

/* Counts changed: written with the next batch. */
static void write_super(struct ext2fs *fs) { fs->sb_dirty = true; }
static void write_gd(struct ext2fs *fs, uint32_t g) {
    (void)g;
    fs->gd_dirty = true;
}

/* ---- the block cache ---- */

static void lru_unlink(struct bbuf *b) {
    b->prev->next = b->next;
    b->next->prev = b->prev;
}

static void lru_front(struct ext2fs *fs, struct bbuf *b) {
    b->next = fs->lru.next;
    b->prev = &fs->lru;
    fs->lru.next->prev = b;
    fs->lru.next = b;
}

static struct bbuf *bfind(struct ext2fs *fs, uint32_t blk) {
    for (struct bbuf *b = fs->bhash[blk % BHASH]; b; b = b->hnext)
        if (b->blk == blk)
            return b;
    return NULL;
}

static void bforget(struct ext2fs *fs, struct bbuf *b) {
    for (struct bbuf **pp = &fs->bhash[b->blk % BHASH]; *pp; pp = &(*pp)->hnext)
        if (*pp == b) {
            *pp = b->hnext;
            break;
        }
    lru_unlink(b);
    kfree(b->data);
    kfree(b);
    fs->nbufs--;
}

static int bflush(struct ext2fs *fs, bool clean);
static int write_runs(struct ext2fs *fs, struct bbuf **list, uint32_t n);

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

static uint32_t jnext(struct ext2fs *fs, uint32_t pos) { return pos + 1 >= fs->jmax ? fs->jfirst : pos + 1; }

static int jwrite(struct ext2fs *fs, uint32_t pos, const void *data) {
    return blkdev_write(fs->dev, data, fs->bs, (uint64_t)fs->jmap[pos] * fs->bs) == (int64_t)fs->bs ? 0 : -EIO;
}

/* The journal's superblock names where its live transaction starts (0: none) and that transaction's sequence. */
static int journal_point(struct ext2fs *fs, uint32_t start, uint32_t seq) {
    put_be32(fs->jsb + 24, seq);   /* s_sequence */
    put_be32(fs->jsb + 28, start); /* s_start */
    return jwrite(fs, 0, fs->jsb);
}

/* n blocks (where they go, what is in them) as one transaction: written to the log with its descriptors, then
 * its commit record, then the journal pointed at it. After this, a crash at any moment replays all of them. */
static int journal_commit(struct ext2fs *fs, const uint32_t *blks, uint8_t *const *datas, uint32_t n) {
    uint32_t per = (fs->bs - 12 - 16) / fs->jtag; /* tags in a descriptor (the first carries the journal's UUID) */
    uint32_t need = n + (n + per - 1) / per + 1;
    if (need + fs->jlast_len + 1 >= fs->jmax - fs->jfirst)
        return -ENOSPC; /* (bigger than the journal can hold beside the last: written without it) */
    uint32_t seq = fs->jseq, start = fs->jhead, pos = start;
    for (uint32_t i = 0; i < n;) {
        uint8_t *d = fs->jblk;
        memset(d, 0, fs->bs);
        put_be32(d, JBD2_MAGIC), put_be32(d + 4, JBD2_DESCRIPTOR), put_be32(d + 8, seq);
        uint8_t *t = d + 12, *last = NULL;
        uint32_t k = 0;
        for (; i + k < n && k < per; k++) {
            uint16_t flags = k ? JBD2_FLAG_SAME_UUID : 0;
            if (be32(datas[i + k]) == JBD2_MAGIC)
                flags |= JBD2_FLAG_ESCAPE; /* (a block that looks like the journal's own: written with that word 0) */
            put_be32(t, blks[i + k]);
            put_be16(t + 4, 0);
            put_be16(t + 6, flags);
            last = t;
            t += fs->jtag;
            if (!k) {
                memcpy(t, fs->jsb + 48, 16); /* s_uuid */
                t += 16;
            }
        }
        put_be16(last + 6, (uint16_t)(last[7] | last[6] << 8 | JBD2_FLAG_LAST_TAG));
        if (jwrite(fs, pos, d) < 0)
            return -EIO;
        pos = jnext(fs, pos);
        for (uint32_t j = 0; j < k; j++) {
            const uint8_t *src = datas[i + j];
            if (be32(src) == JBD2_MAGIC) {
                memcpy(fs->stage, src, fs->bs);
                memset(fs->stage, 0, 4);
                src = fs->stage;
            }
            if (jwrite(fs, pos, src) < 0)
                return -EIO;
            pos = jnext(fs, pos);
        }
        i += k;
    }
    blkdev_flush(fs->dev); /* what it is made of, on the disk before the record that it is whole */
    uint8_t *c = fs->jblk;
    memset(c, 0, fs->bs);
    put_be32(c, JBD2_MAGIC), put_be32(c + 4, JBD2_COMMIT), put_be32(c + 8, seq);
    uint64_t now = (uint64_t)time_realtime_s();
    put_be32(c + 48, (uint32_t)(now >> 32)), put_be32(c + 52, (uint32_t)now); /* h_commit_sec */
    if (jwrite(fs, pos, c) < 0)
        return -EIO;
    pos = jnext(fs, pos);
    if (journal_point(fs, start, seq) < 0)
        return -EIO;
    blkdev_flush(fs->dev);
    fs->jlast_len = need;
    fs->jhead = pos;
    fs->jseq = seq + 1;
    return 0;
}


/* Shell sort, by block number: the batch goes out in disk order. */
static void sort_bufs(struct bbuf **a, uint32_t n) {
    for (uint32_t gap = n / 2; gap > 0; gap /= 2)
        for (uint32_t i = gap; i < n; i++) {
            struct bbuf *t = a[i];
            uint32_t j = i;
            for (; j >= gap && a[j - gap]->blk > t->blk; j -= gap)
                a[j] = a[j - gap];
            a[j] = t;
        }
}

/* Block `blk` in the cache, read from the disk if `fill` (else its contents
 * are for the caller to set, whole). NULL when out of memory or unreadable. */
static struct bbuf *bget(struct ext2fs *fs, uint32_t blk, bool fill) {
    struct bbuf *b = bfind(fs, blk);
    if (b) {
        lru_unlink(b);
        lru_front(fs, b);
        return b;
    }
    if (fs->nbufs >= fs->maxbufs) {
        /* make room: the least recently used block that is already on the disk */
        struct bbuf *victim = NULL;
        for (struct bbuf *c = fs->lru.prev; c != &fs->lru; c = c->prev)
            if (!c->dirty) {
                victim = c;
                break;
            }
        if (!victim && bflush(fs, false) == 0)
            victim = fs->lru.prev != &fs->lru ? fs->lru.prev : NULL;
        if (victim)
            bforget(fs, victim);
    }
    b = kzalloc(sizeof *b);
    uint8_t *data = b ? kmalloc(fs->bs) : NULL;
    if (!data) {
        kfree(b);
        return NULL;
    }
    if (fill && blkdev_read(fs->dev, data, fs->bs, (uint64_t)blk * fs->bs) != (int64_t)fs->bs) {
        kfree(data);
        kfree(b);
        return NULL;
    }
    b->blk = blk;
    b->data = data;
    b->hnext = fs->bhash[blk % BHASH];
    fs->bhash[blk % BHASH] = b;
    lru_front(fs, b);
    fs->nbufs++;
    return b;
}

static void bdirty(struct ext2fs *fs, struct bbuf *b) {
    if (!b->dirty) {
        b->dirty = true;
        fs->ndirty++;
    }
    if (fs->ndirty >= fs->maxdirty)
        bflush(fs, false);
}

/* Sorted dirty blocks to their places, neighbours in one request; written ones are clean. */
static int write_runs(struct ext2fs *fs, struct bbuf **list, uint32_t n) {
    int r = 0;
    uint32_t per = fs->stage ? (256u * 1024) / fs->bs : 1;
    for (uint32_t i = 0; i < n;) {
        uint32_t run = 1;
        while (i + run < n && run < per && list[i + run]->blk == list[i]->blk + run)
            run++;
        int64_t want = (int64_t)run * fs->bs, got;
        uint64_t off = (uint64_t)list[i]->blk * fs->bs;
        if (run == 1) {
            got = blkdev_write(fs->dev, list[i]->data, fs->bs, off);
        } else {
            for (uint32_t k = 0; k < run; k++)
                memcpy(fs->stage + (size_t)k * fs->bs, list[i + k]->data, fs->bs);
            got = blkdev_write(fs->dev, fs->stage, (size_t)want, off);
        }
        if (got != want) {
            r = -EIO;
        } else {
            for (uint32_t k = 0; k < run; k++)
                list[i + k]->dirty = false;
            fs->ndirty -= run;
        }
        i += run;
    }
    return r;
}

/* Everything changed, to the disk: the superblock first says "in use" (if it
 * said clean), then the blocks in disk order, neighbours in one request, then
 * the descriptors and the superblock with the new counts, then the drive's
 * own cache. `clean` marks it cleanly unmounted at the end (sync, power-off). */
static int bflush(struct ext2fs *fs, bool clean) {
    bool any = fs->ndirty || fs->sb_dirty || fs->gd_dirty;
    if (!any && (!clean || fs->disk_clean))
        return 0;
    int r = 0;
    if (any && fs->disk_clean)
        r = super_out(fs, false);
    if (fs->ndirty || (fs->jon && any)) {
        struct bbuf **list = kmalloc(sizeof *list * (fs->ndirty + 1));
        if (!list)
            return -ENOMEM;
        uint32_t n = 0;
        for (struct bbuf *b = fs->lru.next; b != &fs->lru && n < fs->ndirty; b = b->next)
            if (b->dirty)
                list[n++] = b;
        sort_bufs(list, n);
        if (fs->jon) {
            /* files' data to its places first; the file system's own blocks, the descriptors and the superblock
             * to the journal as one transaction, then to their places (n sorted: both halves stay sorted) */
            uint32_t nd = 0;
            for (uint32_t i = 0; i < n; i++)
                if (list[i]->is_data) {
                    struct bbuf *t = list[i];
                    memmove(list + nd + 1, list + nd, sizeof *list * (i - nd));
                    list[nd++] = t;
                }
            r |= write_runs(fs, list, nd);
            uint32_t ngd = (uint32_t)(((size_t)fs->groups * sizeof(struct ext2_gd) + fs->bs - 1) / fs->bs);
            uint32_t nm = n - nd, total = nm + ngd + 1;
            uint32_t *blks = kmalloc(sizeof *blks * total);
            uint8_t **datas = kmalloc(sizeof *datas * total);
            uint8_t *gdimg = kzalloc((size_t)ngd * fs->bs);
            int jr = -ENOMEM;
            if (blks && datas && gdimg) {
                for (uint32_t i = 0; i < nm; i++)
                    blks[i] = list[nd + i]->blk, datas[i] = list[nd + i]->data;
                memcpy(gdimg, fs->gd, (size_t)fs->groups * sizeof(struct ext2_gd));
                for (uint32_t i = 0; i < ngd; i++)
                    blks[nm + i] = fs->gdt_block + i, datas[nm + i] = gdimg + (size_t)i * fs->bs;
                memcpy(fs->sbblk + 1024 % fs->bs, fs->sb, 1024); /* (in use, RECOVER: as mounted) */
                blks[nm + ngd] = fs->sbblk_no, datas[nm + ngd] = fs->sbblk;
                jr = journal_commit(fs, blks, datas, total);
            }
            kfree(blks), kfree(datas), kfree(gdimg);
            if (jr < 0 && !fs->jwarned) {
                fs->jwarned = true;
                kprintf("ext2: %s: a batch went to the disk without the journal (%d)\n", fs->dev->name, jr);
            }
            r |= write_runs(fs, list + nd, nm); /* then to their places: a power cut now is replayed */
            fs->gd_dirty = fs->sb_dirty = true;  /* (written below, after them) */
            kfree(list);
            n = 0;
            list = NULL;
        }
        uint32_t per = fs->stage ? (256u * 1024) / fs->bs : 1;
        for (uint32_t i = 0; i < n;) {
            uint32_t run = 1;
            while (i + run < n && run < per && list[i + run]->blk == list[i]->blk + run)
                run++;
            int64_t want = (int64_t)run * fs->bs, got;
            uint64_t off = (uint64_t)list[i]->blk * fs->bs;
            if (run == 1) {
                got = blkdev_write(fs->dev, list[i]->data, fs->bs, off);
            } else {
                for (uint32_t k = 0; k < run; k++)
                    memcpy(fs->stage + (size_t)k * fs->bs, list[i + k]->data, fs->bs);
                got = blkdev_write(fs->dev, fs->stage, (size_t)want, off);
            }
            if (got != want) {
                r = -EIO;
            } else {
                for (uint32_t k = 0; k < run; k++)
                    list[i + k]->dirty = false;
                fs->ndirty -= run;
            }
            i += run;
        }
        kfree(list);
    }
    if (fs->gd_dirty) {
        size_t gd_bytes = (size_t)fs->groups * sizeof(struct ext2_gd);
        if (blkdev_write(fs->dev, fs->gd, gd_bytes, (uint64_t)fs->gdt_block * fs->bs) == (int64_t)gd_bytes)
            fs->gd_dirty = false;
        else
            r = -EIO;
    }
    if (fs->jon && clean && r == 0) {
        /* everything is in its place: the journal holds nothing to replay, and the superblock says so */
        blkdev_flush(fs->dev);
        if (journal_point(fs, 0, fs->jseq) < 0)
            r = -EIO;
        fs->jhead = fs->jfirst, fs->jlast_len = 0;
        blkdev_flush(fs->dev);
    }
    if (fs->sb_dirty || clean) {
        if (super_out(fs, clean && r == 0) == 0)
            fs->sb_dirty = false;
        else
            r = -EIO;
    }
    blkdev_flush(fs->dev);
    return r;
}

static int bread(struct ext2fs *fs, uint32_t blk, void *buf) {
    if (blk == 0 || blk >= fs->blocks_count)
        return -EIO;
    struct bbuf *b = bget(fs, blk, true);
    if (!b)
        return -EIO;
    memcpy(buf, b->data, fs->bs);
    return 0;
}

static int bwrite_kind(struct ext2fs *fs, uint32_t blk, const void *buf, bool is_data) {
    if (blk == 0 || blk >= fs->blocks_count)
        return -EIO;
    struct bbuf *b = bget(fs, blk, false);
    if (!b)
        return -EIO;
    memcpy(b->data, buf, fs->bs);
    b->is_data = is_data;
    bdirty(fs, b);
    return 0;
}

static int bwrite(struct ext2fs *fs, uint32_t blk, const void *buf) { return bwrite_kind(fs, blk, buf, false); }

static int bzero_block(struct ext2fs *fs, uint32_t blk) {
    if (blk == 0 || blk >= fs->blocks_count)
        return -EIO;
    struct bbuf *b = bget(fs, blk, false);
    if (!b)
        return -EIO;
    memset(b->data, 0, fs->bs);
    b->is_data = false;
    bdirty(fs, b);
    return 0;
}

/* Part of a block (an inode in the inode table), through the cache. */
static int bpart(struct ext2fs *fs, uint64_t off, void *buf, size_t len, bool write) {
    uint32_t blk = (uint32_t)(off / fs->bs);
    size_t in = off % fs->bs;
    if (blk == 0 || blk >= fs->blocks_count || in + len > fs->bs)
        return -EIO;
    struct bbuf *b = bget(fs, blk, true);
    if (!b)
        return -EIO;
    if (write) {
        memcpy(b->data + in, buf, len);
        bdirty(fs, b);
    } else {
        memcpy(buf, b->data + in, len);
    }
    return 0;
}

/* Finds and sets a clear bit in a group's bitmap, below `limit`, looking from
 * where the last one was found (*hint) a byte at a time, so filling a group
 * doesn't rescan what is already full on every allocation. In the cached
 * block itself: no copies. */
static int64_t take_bit(struct ext2fs *fs, uint32_t bitmap_blk, uint32_t limit, uint32_t *hint) {
    struct bbuf *b = bget(fs, bitmap_blk, true);
    if (!b)
        return -EIO;
    uint8_t *bm = b->data;
    uint32_t bytes = (limit + 7) / 8, start = *hint / 8 < bytes ? *hint / 8 : 0;
    for (uint32_t k = 0; k < bytes; k++) {
        uint32_t i = (start + k) % bytes;
        if (bm[i] == 0xFF)
            continue;
        for (uint32_t bit = i * 8; bit < i * 8 + 8 && bit < limit; bit++)
            if (!(bm[bit / 8] & (1u << (bit % 8)))) {
                bm[bit / 8] |= (uint8_t)(1u << (bit % 8));
                bdirty(fs, b);
                *hint = bit;
                return bit;
            }
    }
    return -ENOSPC;
}

static int clear_bit(struct ext2fs *fs, uint32_t bitmap_blk, uint32_t bit) {
    struct bbuf *b = bget(fs, bitmap_blk, true);
    if (!b)
        return -EIO;
    b->data[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    bdirty(fs, b);
    return 0;
}

static uint32_t group_blocks(struct ext2fs *fs, uint32_t g) {
    uint32_t start = fs->first_data + g * fs->bpg;
    uint32_t n = fs->blocks_count - start;
    return n < fs->bpg ? n : fs->bpg;
}

/* A new block, near group `goal` if it has room; zeroed if asked. */
static int64_t alloc_block(struct ext2fs *fs, uint32_t goal, bool zero) {
    for (uint32_t k = 0; k < fs->groups; k++) {
        uint32_t g = (goal + k) % fs->groups;
        if (!fs->gd[g].free_blocks)
            continue;
        int64_t bit = take_bit(fs, fs->gd[g].block_bitmap, group_blocks(fs, g), &fs->bhint[g]);
        if (bit == -ENOSPC)
            continue;
        if (bit < 0)
            return bit;
        fs->gd[g].free_blocks--;
        sb32_set(fs, 12, sb32(fs, 12) - 1);
        write_gd(fs, g);
        write_super(fs);
        uint32_t blk = fs->first_data + g * fs->bpg + (uint32_t)bit;
        if (zero)
            bzero_block(fs, blk);
        return blk;
    }
    return -ENOSPC;
}

static void free_block(struct ext2fs *fs, uint32_t blk) {
    if (blk < fs->first_data || blk >= fs->blocks_count)
        return;
    uint32_t g = (blk - fs->first_data) / fs->bpg, bit = (blk - fs->first_data) % fs->bpg;
    if (clear_bit(fs, fs->gd[g].block_bitmap, bit) == 0) {
        fs->gd[g].free_blocks++;
        sb32_set(fs, 12, sb32(fs, 12) + 1);
        write_gd(fs, g);
        write_super(fs);
    }
}

static int64_t alloc_inode(struct ext2fs *fs, uint32_t goal_group, bool dir) {
    for (uint32_t k = 0; k < fs->groups; k++) {
        uint32_t g = (goal_group + k) % fs->groups;
        if (!fs->gd[g].free_inodes)
            continue;
        int64_t bit = take_bit(fs, fs->gd[g].inode_bitmap, fs->ipg, &fs->ihint[g]);
        if (bit == -ENOSPC)
            continue;
        if (bit < 0)
            return bit;
        uint32_t ino = g * fs->ipg + (uint32_t)bit + 1;
        if (ino < fs->first_ino) { /* reserved inodes are marked used by mke2fs; never hand them out */
            k--;
            continue;
        }
        fs->gd[g].free_inodes--;
        if (dir)
            fs->gd[g].used_dirs++;
        sb32_set(fs, 16, sb32(fs, 16) - 1);
        write_gd(fs, g);
        write_super(fs);
        return ino;
    }
    return -ENOSPC;
}

static void free_inode_bit(struct ext2fs *fs, uint32_t ino, bool dir) {
    uint32_t g = (ino - 1) / fs->ipg, bit = (ino - 1) % fs->ipg;
    if (g >= fs->groups || clear_bit(fs, fs->gd[g].inode_bitmap, bit) < 0)
        return;
    fs->gd[g].free_inodes++;
    if (dir && fs->gd[g].used_dirs)
        fs->gd[g].used_dirs--;
    sb32_set(fs, 16, sb32(fs, 16) + 1);
    write_gd(fs, g);
    write_super(fs);
}

/* ---- inodes ---- */

static uint64_t inode_off(struct ext2fs *fs, uint32_t ino) {
    uint32_t g = (ino - 1) / fs->ipg, idx = (ino - 1) % fs->ipg;
    return (uint64_t)fs->gd[g].inode_table * fs->bs + (uint64_t)idx * fs->inode_size;
}

static int read_inode(struct ext2fs *fs, uint32_t ino, struct ext2_inode *in) {
    if (ino == 0 || ino > fs->inodes_count)
        return -EIO;
    return bpart(fs, inode_off(fs, ino), in, sizeof *in, false);
}

static uint64_t isize(const struct ext2_inode *in) {
    return in->size | ((in->mode & S_IFMT_) == S_IFREG_ ? (uint64_t)in->size_high << 32 : 0);
}

static void set_isize(struct enode *en, uint64_t size) {
    en->in.size = (uint32_t)size;
    if ((en->in.mode & S_IFMT_) == S_IFREG_)
        en->in.size_high = (uint32_t)(size >> 32);
    if (en->v)
        en->v->size = size;
}

/* Writes the inode, with what the generic code may have changed on the vnode
 * (permissions, owner, times) folded in. */
static int write_inode(struct enode *en) {
    struct ext2fs *fs = en->fs;
    if (en->v) {
        struct vnode *v = en->v;
        en->in.mode = (uint16_t)((en->in.mode & S_IFMT_) | (v->mode & 07777));
        en->in.uid = (uint16_t)v->uid, en->in.uid_hi = (uint16_t)(v->uid >> 16);
        en->in.gid = (uint16_t)v->gid, en->in.gid_hi = (uint16_t)(v->gid >> 16);
        en->in.mtime = (uint32_t)v->mtime;
    }
    return bpart(fs, inode_off(fs, en->ino), &en->in, sizeof en->in, true);
}

static uint32_t inode_group(struct ext2fs *fs, uint32_t ino) { return (ino - 1) / fs->ipg; }

/* ---- block maps ---- */

/* The disk block holding file block `lb` (0: a hole). With `create`, holes
 * are filled with new blocks (zeroed unless `whole` says the caller writes
 * the entire block). */
static int64_t bmap(struct enode *en, uint64_t lb, bool create, bool whole) {
    struct ext2fs *fs = en->fs;
    uint32_t per = fs->bs / 4, goal = inode_group(fs, en->ino);
    uint32_t idx[4];
    int depth;
    uint32_t *slot;
    if (lb < 12) {
        slot = &en->in.block[lb];
        depth = 0;
    } else if ((lb -= 12) < per) {
        slot = &en->in.block[12];
        idx[0] = (uint32_t)lb;
        depth = 1;
    } else if ((lb -= per) < (uint64_t)per * per) {
        slot = &en->in.block[13];
        idx[0] = (uint32_t)(lb / per), idx[1] = (uint32_t)(lb % per);
        depth = 2;
    } else {
        lb -= (uint64_t)per * per;
        if (lb >= (uint64_t)per * per * per)
            return -EFBIG;
        slot = &en->in.block[14];
        idx[0] = (uint32_t)(lb / ((uint64_t)per * per)), idx[1] = (uint32_t)(lb / per % per), idx[2] = (uint32_t)(lb % per);
        depth = 3;
    }
    if (!create) { /* looking only: read the cached map blocks in place */
        uint32_t cur = *slot;
        for (int d = 0; d < depth && cur; d++) {
            if (cur >= fs->blocks_count)
                return -EIO;
            struct bbuf *b = bget(fs, cur, true);
            if (!b)
                return -EIO;
            cur = ((uint32_t *)b->data)[idx[d]];
        }
        return cur;
    }
    bool inode_dirty = false;
    if (!*slot) {
        int64_t b = alloc_block(fs, goal, depth > 0 || !whole);
        if (b < 0)
            return b;
        *slot = (uint32_t)b;
        en->in.blocks += fs->bs / 512;
        inode_dirty = true;
    }
    uint32_t cur = *slot;
    uint32_t *tab = depth ? kmalloc(fs->bs) : NULL;
    if (depth && !tab)
        return -ENOMEM;
    int64_t result = cur;
    for (int d = 0; d < depth; d++) {
        if (bread(fs, cur, tab) < 0) {
            result = -EIO;
            break;
        }
        uint32_t next = tab[idx[d]];
        if (!next) {
            if (!create) {
                result = 0;
                break;
            }
            int64_t b = alloc_block(fs, goal, d + 1 < depth || !whole);
            if (b < 0) {
                result = b;
                break;
            }
            tab[idx[d]] = (uint32_t)b;
            en->in.blocks += fs->bs / 512;
            inode_dirty = true;
            if (bwrite(fs, cur, tab) < 0) {
                result = -EIO;
                break;
            }
            next = (uint32_t)b;
        }
        cur = next;
        result = cur;
    }
    kfree(tab);
    if (inode_dirty)
        write_inode(en);
    return result;
}

/* Frees the blocks of the tree at `blk` (level 0: a data block) whose file
 * block numbers are >= keep; `base` is the first file block it covers, `span`
 * how many each entry covers. Returns true if `blk` itself is now unused. */
static bool free_tree(struct enode *en, uint32_t blk, int level, uint64_t base, uint64_t keep) {
    struct ext2fs *fs = en->fs;
    uint32_t per = fs->bs / 4;
    if (level == 0) {
        if (base >= keep) {
            free_block(fs, blk);
            en->in.blocks -= fs->bs / 512;
            return true;
        }
        return false;
    }
    uint64_t span = 1;
    for (int i = 1; i < level; i++)
        span *= per;
    uint32_t *tab = kmalloc(fs->bs);
    if (!tab || bread(fs, blk, tab) < 0) {
        kfree(tab);
        return false;
    }
    bool all = true, changed = false;
    for (uint32_t i = 0; i < per; i++) {
        if (!tab[i])
            continue;
        uint64_t b = base + i * span;
        if (b + span <= keep) {
            all = false;
            continue;
        }
        if (free_tree(en, tab[i], level - 1, b, keep)) {
            tab[i] = 0;
            changed = true;
        } else {
            all = false;
        }
    }
    if (all) {
        free_block(fs, blk);
        en->in.blocks -= fs->bs / 512;
    } else if (changed) {
        bwrite(fs, blk, tab);
    }
    kfree(tab);
    return all;
}

/* Frees every block from file block `keep` on. */
static void truncate_blocks(struct enode *en, uint64_t keep) {
    struct ext2fs *fs = en->fs;
    uint64_t per = fs->bs / 4;
    for (uint64_t i = keep; i < 12; i++)
        if (en->in.block[i]) {
            free_block(fs, en->in.block[i]);
            en->in.blocks -= fs->bs / 512;
            en->in.block[i] = 0;
        }
    uint64_t base[3] = {12, 12 + per, 12 + per + per * per};
    for (int k = 0; k < 3; k++)
        if (en->in.block[12 + k] && free_tree(en, en->in.block[12 + k], k + 1, base[k], keep < base[k] ? base[k] : keep))
            en->in.block[12 + k] = 0;
    /* a tree whose part >= keep was freed but that still holds earlier blocks stays */
}

/* ---- file contents ---- */

static int64_t file_rw(struct enode *en, uint8_t *buf, size_t len, uint64_t off, bool write) {
    struct ext2fs *fs = en->fs;
    uint64_t size = isize(&en->in);
    if (!write) {
        if (off >= size)
            return 0;
        if (len > size - off)
            len = (size_t)(size - off);
    }
    uint8_t *blk = kmalloc(fs->bs);
    if (!blk)
        return -ENOMEM;
    size_t done = 0;
    int64_t err = 0;
    while (done < len) {
        uint64_t pos = off + done, lb = pos / fs->bs;
        size_t in = pos % fs->bs, n = fs->bs - in < len - done ? fs->bs - in : len - done;
        bool whole = in == 0 && n == fs->bs;
        int64_t pb = bmap(en, lb, write, write && whole && pos >= size);
        if (pb < 0) {
            err = pb;
            break;
        }
        if (!write) {
            if (pb == 0)
                memset(buf + done, 0, n);
            else if (n == fs->bs && in == 0 && !bfind(fs, (uint32_t)pb)) {
                /* whole blocks not in the cache: as many neighbours as follow
                 * on the disk too, in one request straight into the caller's
                 * buffer (a library being loaded, a file copied), and not
                 * through the cache, which keeps the file system's own blocks */
                uint32_t run = 1, max = (uint32_t)((len - done) / fs->bs);
                while (run < max && run < 64) {
                    int64_t nb = bmap(en, lb + run, false, false);
                    if (nb != pb + run || bfind(fs, (uint32_t)nb))
                        break;
                    run++;
                }
                if (blkdev_read(fs->dev, buf + done, (size_t)run * fs->bs, (uint64_t)pb * fs->bs) !=
                    (int64_t)run * fs->bs) {
                    err = -EIO;
                    break;
                }
                done += (size_t)run * fs->bs;
                continue;
            } else if (n == fs->bs && in == 0) {
                if (bread(fs, (uint32_t)pb, buf + done) < 0) {
                    err = -EIO;
                    break;
                }
            } else {
                if (bread(fs, (uint32_t)pb, blk) < 0) {
                    err = -EIO;
                    break;
                }
                memcpy(buf + done, blk + in, n);
            }
        } else {
            if (whole) {
                if (bwrite_kind(fs, (uint32_t)pb, buf + done, true) < 0) {
                    err = -EIO;
                    break;
                }
            } else {
                if (bread(fs, (uint32_t)pb, blk) < 0) {
                    err = -EIO;
                    break;
                }
                memcpy(blk + in, buf + done, n);
                if (bwrite_kind(fs, (uint32_t)pb, blk, true) < 0) {
                    err = -EIO;
                    break;
                }
            }
        }
        done += n;
    }
    kfree(blk);
    if (write && done) {
        if (off + done > size)
            set_isize(en, off + done);
        if (en->v)
            en->v->mtime = time_realtime_s();
        en->in.mtime = en->in.ctime = (uint32_t)time_realtime_s();
        write_inode(en);
    }
    return done ? (int64_t)done : err;
}

/* ---- the inode cache: one vnode per inode in use ---- */

static enum vnode_type type_of(uint16_t mode) {
    switch (mode & S_IFMT_) {
    case S_IFDIR_: return VN_DIR;
    case S_IFLNK_: return VN_LNK;
    case 0x1000: return VN_FIFO;
    case 0xC000: return VN_SOCK;
    default: return VN_REG; /* device nodes on this disk are not supported: plain files */
    }
}

/* fs lock held. The vnode for `ino` (referenced), made if it isn't in use.
 * -EAGAIN: its old vnode is being released right now; try again. */
static int iget_locked(struct ext2fs *fs, uint32_t ino, struct vnode **out) {
    for (struct enode *e = fs->cache[ino % 256]; e; e = e->hnext)
        if (e->ino == ino) {
            int rc = __atomic_load_n(&e->v->refcount, __ATOMIC_ACQUIRE);
            if (rc <= 0)
                return -EAGAIN;
            vnode_ref(e->v);
            *out = e->v;
            return 0;
        }
    struct enode *en = kzalloc(sizeof *en);
    if (!en)
        return -ENOMEM;
    en->fs = fs;
    en->ino = ino;
    if (read_inode(fs, ino, &en->in) < 0) {
        kfree(en);
        return -EIO;
    }
    enum vnode_type t = type_of(en->in.mode);
    struct vnode *v = vnode_alloc(t, en->in.mode & 07777, t == VN_DIR ? &dir_ops : t == VN_LNK ? &link_ops : &file_ops);
    if (!v) {
        kfree(en);
        return -ENOMEM;
    }
    v->ino = ino;
    v->uid = en->in.uid | ((uint32_t)en->in.uid_hi << 16);
    v->gid = en->in.gid | ((uint32_t)en->in.gid_hi << 16);
    v->nlink = en->in.links;
    v->mtime = en->in.mtime;
    v->size = isize(&en->in);
    v->data = en;
    en->v = v;
    en->hnext = fs->cache[ino % 256];
    fs->cache[ino % 256] = en;
    *out = v;
    return 0;
}

static int iget(struct ext2fs *fs, uint32_t ino, struct vnode **out) {
    for (;;) {
        fs_lock(fs);
        int r = iget_locked(fs, ino, out);
        fs_unlock(fs);
        if (r != -EAGAIN)
            return r;
        thread_yield();
    }
}

/* The in-memory inode for `ino` if it is in use, else NULL (fs lock held). */
static struct enode *cached(struct ext2fs *fs, uint32_t ino) {
    for (struct enode *e = fs->cache[ino % 256]; e; e = e->hnext)
        if (e->ino == ino && __atomic_load_n(&e->v->refcount, __ATOMIC_ACQUIRE) > 0)
            return e;
    return NULL;
}

/* An inode with no names left: its blocks and the inode itself go. */
static void destroy_inode(struct ext2fs *fs, struct ext2_inode *in, uint32_t ino) {
    struct enode tmp = {.fs = fs, .ino = ino, .in = *in};
    bool dir = (in->mode & S_IFMT_) == S_IFDIR_;
    bool fast_link = (in->mode & S_IFMT_) == S_IFLNK_ && in->blocks == 0;
    if (!fast_link)
        truncate_blocks(&tmp, 0);
    tmp.in.links = 0;
    tmp.in.size = tmp.in.size_high = 0;
    tmp.in.blocks = 0;
    tmp.in.dtime = (uint32_t)time_realtime_s();
    memset(tmp.in.block, 0, sizeof tmp.in.block);
    bpart(fs, inode_off(fs, ino), &tmp.in, sizeof tmp.in, true);
    free_inode_bit(fs, ino, dir);
}

static void e_release(struct vnode *v) {
    struct enode *en = v->data;
    struct ext2fs *fs = en->fs;
    fs_lock(fs);
    for (struct enode **pp = &fs->cache[en->ino % 256]; *pp; pp = &(*pp)->hnext)
        if (*pp == en) {
            *pp = en->hnext;
            break;
        }
    if (en->in.links == 0) {
        destroy_inode(fs, &en->in, en->ino);
    } else {
        write_inode(en);
    }
    en->v = NULL;
    fs_unlock(fs);
    if (v->parent && v->parent != v)
        vnode_put(v->parent);
    kfree(en);
}

/* ---- directories ---- */

static uint8_t ftype_of(uint16_t mode) {
    switch (mode & S_IFMT_) {
    case S_IFREG_: return 1;
    case S_IFDIR_: return 2;
    case 0x2000: return 3;
    case 0x6000: return 4;
    case 0x1000: return 5;
    case 0xC000: return 6;
    case S_IFLNK_: return 7;
    }
    return 0;
}

static size_t rec_need(size_t name_len) { return (8 + name_len + 3) & ~(size_t)3; }

/* Finds `name` in directory `en`: its inode number, and where it is. */
static int dir_find(struct enode *en, const char *name, uint32_t *ino, uint64_t *where) {
    struct ext2fs *fs = en->fs;
    size_t nl = strlen(name);
    uint64_t size = isize(&en->in);
    uint8_t *b = kmalloc(fs->bs);
    if (!b)
        return -ENOMEM;
    int r = -ENOENT;
    for (uint64_t lb = 0; lb * fs->bs < size && r == -ENOENT; lb++) {
        int64_t pb = bmap(en, lb, false, false);
        if (pb <= 0 || bread(fs, (uint32_t)pb, b) < 0)
            continue;
        for (uint32_t off = 0; off + 8 <= fs->bs;) {
            struct dirent2 *d = (struct dirent2 *)(b + off);
            if (d->rec_len < 8 || off + d->rec_len > fs->bs)
                break;
            if (d->inode && d->name_len == nl && !memcmp(d->name, name, nl)) {
                *ino = d->inode;
                if (where)
                    *where = lb * fs->bs + off;
                r = 0;
                break;
            }
            off += d->rec_len;
        }
    }
    kfree(b);
    return r;
}

static int dir_add(struct enode *en, const char *name, uint32_t ino, uint8_t ftype) {
    struct ext2fs *fs = en->fs;
    size_t nl = strlen(name);
    if (nl == 0 || nl > 255)
        return -ENAMETOOLONG;
    size_t need = rec_need(nl);
    uint64_t size = isize(&en->in);
    uint8_t *b = kmalloc(fs->bs);
    if (!b)
        return -ENOMEM;
    int r = -ENOSPC;
    for (uint64_t lb = 0; lb * fs->bs < size; lb++) {
        int64_t pb = bmap(en, lb, false, false);
        if (pb <= 0 || bread(fs, (uint32_t)pb, b) < 0)
            continue;
        for (uint32_t off = 0; off + 8 <= fs->bs;) {
            struct dirent2 *d = (struct dirent2 *)(b + off);
            if (d->rec_len < 8 || off + d->rec_len > fs->bs)
                break;
            size_t used = d->inode ? rec_need(d->name_len) : 0;
            if (d->rec_len - used >= need) {
                struct dirent2 *n = d;
                if (used) { /* split: the existing entry keeps what it uses */
                    n = (struct dirent2 *)(b + off + used);
                    n->rec_len = (uint16_t)(d->rec_len - used);
                    d->rec_len = (uint16_t)used;
                }
                n->inode = ino;
                n->name_len = (uint8_t)nl;
                n->file_type = fs->filetype ? ftype : 0;
                memcpy(n->name, name, nl);
                r = bwrite(fs, (uint32_t)pb, b);
                goto out;
            }
            off += d->rec_len;
        }
    }
    { /* no room: a new block at the end */
        uint64_t lb = size / fs->bs;
        int64_t pb = bmap(en, lb, true, true);
        if (pb < 0) {
            r = (int)pb;
            goto out;
        }
        memset(b, 0, fs->bs);
        struct dirent2 *n = (struct dirent2 *)b;
        n->inode = ino;
        n->rec_len = (uint16_t)fs->bs;
        n->name_len = (uint8_t)nl;
        n->file_type = fs->filetype ? ftype : 0;
        memcpy(n->name, name, nl);
        r = bwrite(fs, (uint32_t)pb, b);
        if (r == 0) {
            set_isize(en, (lb + 1) * fs->bs);
            write_inode(en);
        }
    }
out:
    kfree(b);
    if (r == 0) {
        en->in.mtime = en->in.ctime = (uint32_t)time_realtime_s();
        if (en->v)
            en->v->mtime = en->in.mtime;
        write_inode(en);
    }
    return r;
}

static int dir_remove(struct enode *en, uint64_t where) {
    struct ext2fs *fs = en->fs;
    uint64_t lb = where / fs->bs;
    uint32_t at = (uint32_t)(where % fs->bs);
    uint8_t *b = kmalloc(fs->bs);
    if (!b)
        return -ENOMEM;
    int64_t pb = bmap(en, lb, false, false);
    int r = pb > 0 ? bread(fs, (uint32_t)pb, b) : -EIO;
    if (r == 0) {
        struct dirent2 *prev = NULL;
        for (uint32_t off = 0; off < at;) {
            struct dirent2 *d = (struct dirent2 *)(b + off);
            if (d->rec_len < 8)
                break;
            prev = d;
            off += d->rec_len;
        }
        struct dirent2 *d = (struct dirent2 *)(b + at);
        if (prev && (uint8_t *)prev + prev->rec_len == (uint8_t *)d)
            prev->rec_len = (uint16_t)(prev->rec_len + d->rec_len);
        else
            d->inode = 0;
        r = bwrite(fs, (uint32_t)pb, b);
    }
    kfree(b);
    if (r == 0) {
        en->in.mtime = en->in.ctime = (uint32_t)time_realtime_s();
        if (en->v)
            en->v->mtime = en->in.mtime;
        write_inode(en);
    }
    return r;
}

/* True if the directory holds nothing but "." and "..". */
static bool dir_empty(struct ext2fs *fs, struct ext2_inode *in, uint32_t ino) {
    struct enode tmp = {.fs = fs, .ino = ino, .in = *in};
    uint64_t size = isize(in);
    uint8_t *b = kmalloc(fs->bs);
    if (!b)
        return false;
    bool empty = true;
    for (uint64_t lb = 0; lb * fs->bs < size && empty; lb++) {
        int64_t pb = bmap(&tmp, lb, false, false);
        if (pb <= 0 || bread(fs, (uint32_t)pb, b) < 0)
            continue;
        for (uint32_t off = 0; off + 8 <= fs->bs;) {
            struct dirent2 *d = (struct dirent2 *)(b + off);
            if (d->rec_len < 8)
                break;
            if (d->inode && !(d->name_len == 1 && d->name[0] == '.') &&
                !(d->name_len == 2 && d->name[0] == '.' && d->name[1] == '.')) {
                empty = false;
                break;
            }
            off += d->rec_len;
        }
    }
    kfree(b);
    return empty;
}

/* Points a directory's ".." at `parent`. */
static int set_dotdot(struct ext2fs *fs, struct enode *dir, uint32_t parent) {
    uint32_t old;
    uint64_t where;
    int r = dir_find(dir, "..", &old, &where);
    if (r < 0)
        return r;
    int64_t pb = bmap(dir, where / fs->bs, false, false);
    if (pb <= 0)
        return -EIO;
    uint8_t *b = kmalloc(fs->bs);
    if (!b)
        return -ENOMEM;
    r = bread(fs, (uint32_t)pb, b);
    if (r == 0) {
        ((struct dirent2 *)(b + where % fs->bs))->inode = parent;
        r = bwrite(fs, (uint32_t)pb, b);
    }
    kfree(b);
    return r;
}

/* ---- vnode operations ---- */

static int e_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct enode *en = dir->data;
    struct ext2fs *fs = en->fs;
    uint32_t ino;
    fs_lock(fs);
    int r = dir_find(en, name, &ino, NULL);
    fs_unlock(fs);
    if (r < 0)
        return r;
    r = iget(fs, ino, out);
    if (r == 0 && (*out)->type == VN_DIR && !(*out)->parent && *out != dir) {
        (*out)->parent = dir; /* held, as parents are */
        vnode_ref(dir);
    }
    return r;
}

/* fs lock held: a new inode of `mode`, named `name` in `dir`. */
static int make_node(struct enode *dir, const char *name, uint16_t mode, uint32_t *ino_out, struct ext2_inode *in_out) {
    struct ext2fs *fs = dir->fs;
    uint32_t existing;
    if (dir_find(dir, name, &existing, NULL) == 0)
        return -EEXIST;
    bool is_dir = (mode & S_IFMT_) == S_IFDIR_;
    int64_t ino = alloc_inode(fs, inode_group(fs, dir->ino), is_dir);
    if (ino < 0)
        return (int)ino;
    struct process *p = proc_current();
    uint32_t now = (uint32_t)time_realtime_s();
    struct ext2_inode in = {0};
    in.mode = mode;
    uint32_t uid = p ? p->euid : 0, gid = p ? p->egid : 0;
    in.uid = (uint16_t)uid, in.uid_hi = (uint16_t)(uid >> 16);
    in.gid = (uint16_t)gid, in.gid_hi = (uint16_t)(gid >> 16);
    in.atime = in.ctime = in.mtime = now;
    in.links = 1;
    if (bpart(fs, inode_off(fs, (uint32_t)ino), &in, sizeof in, true) < 0) {
        free_inode_bit(fs, (uint32_t)ino, is_dir);
        return -EIO;
    }
    *ino_out = (uint32_t)ino;
    *in_out = in;
    return 0;
}

static uint32_t umask_mode(uint32_t mode) {
    struct process *p = proc_current();
    return mode & ~(p ? p->umask : 022) & 07777;
}

static int e_create(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out) {
    struct enode *d = dir->data;
    struct ext2fs *fs = d->fs;
    fs_lock(fs);
    uint32_t ino;
    struct ext2_inode in;
    int r = make_node(d, name, (uint16_t)(S_IFREG_ | (mode & 07777)), &ino, &in);
    if (r == 0) {
        r = dir_add(d, name, ino, 1);
        if (r < 0) {
            in.links = 0;
            destroy_inode(fs, &in, ino);
        }
    }
    fs_unlock(fs);
    if (r < 0)
        return r;
    return iget(fs, ino, out);
}

static int e_mkdir(struct vnode *dir, const char *name, uint32_t mode) {
    struct enode *d = dir->data;
    struct ext2fs *fs = d->fs;
    fs_lock(fs);
    uint32_t ino;
    struct ext2_inode in;
    int r = make_node(d, name, (uint16_t)(S_IFDIR_ | umask_mode(mode)), &ino, &in);
    if (r == 0) {
        struct enode child = {.fs = fs, .ino = ino, .in = in};
        child.in.links = 2;
        int64_t pb = bmap(&child, 0, true, true);
        uint8_t *b = pb > 0 ? kzalloc(fs->bs) : NULL;
        if (b) {
            struct dirent2 *dot = (struct dirent2 *)b;
            dot->inode = ino, dot->rec_len = 12, dot->name_len = 1, dot->file_type = fs->filetype ? 2 : 0;
            dot->name[0] = '.';
            struct dirent2 *dd = (struct dirent2 *)(b + 12);
            dd->inode = d->ino, dd->rec_len = (uint16_t)(fs->bs - 12), dd->name_len = 2, dd->file_type = fs->filetype ? 2 : 0;
            dd->name[0] = dd->name[1] = '.';
            r = bwrite(fs, (uint32_t)pb, b);
            kfree(b);
        } else {
            r = pb < 0 ? (int)pb : -ENOMEM;
        }
        if (r == 0) {
            child.in.size = fs->bs;
            r = write_inode(&child);
        }
        if (r == 0)
            r = dir_add(d, name, ino, 2);
        if (r == 0) {
            d->in.links++;
            dir->nlink = d->in.links;
            write_inode(d);
        } else {
            child.in.links = 0;
            destroy_inode(fs, &child.in, ino);
        }
    }
    fs_unlock(fs);
    return r;
}

static int e_symlink(struct vnode *dir, const char *name, const char *target) {
    struct enode *d = dir->data;
    struct ext2fs *fs = d->fs;
    size_t len = strlen(target);
    if (len >= fs->bs)
        return -ENAMETOOLONG;
    fs_lock(fs);
    uint32_t ino;
    struct ext2_inode in;
    int r = make_node(d, name, (uint16_t)(S_IFLNK_ | 0777), &ino, &in);
    if (r == 0) {
        struct enode link = {.fs = fs, .ino = ino, .in = in};
        if (len < sizeof link.in.block) { /* a "fast" link: the target lives in the inode */
            memcpy(link.in.block, target, len);
        } else {
            int64_t pb = bmap(&link, 0, true, false);
            uint8_t *b = pb > 0 ? kzalloc(fs->bs) : NULL;
            if (b) {
                memcpy(b, target, len);
                r = bwrite(fs, (uint32_t)pb, b);
                kfree(b);
            } else {
                r = pb < 0 ? (int)pb : -ENOMEM;
            }
        }
        link.in.size = (uint32_t)len;
        if (r == 0)
            r = write_inode(&link);
        if (r == 0)
            r = dir_add(d, name, ino, 7);
        if (r < 0) {
            link.in.links = 0;
            destroy_inode(fs, &link.in, ino);
        }
    }
    fs_unlock(fs);
    return r;
}

static int e_link(struct vnode *dir, const char *name, struct vnode *target) {
    if (target->ops != &file_ops && target->ops != &link_ops)
        return target->ops == &dir_ops ? -EPERM : -EXDEV;
    struct enode *d = dir->data, *t = target->data;
    if (t->fs != d->fs)
        return -EXDEV;
    struct ext2fs *fs = d->fs;
    fs_lock(fs);
    uint32_t existing;
    int r = dir_find(d, name, &existing, NULL) == 0 ? -EEXIST : 0;
    if (r == 0)
        r = dir_add(d, name, t->ino, ftype_of(t->in.mode));
    if (r == 0) {
        t->in.links++;
        t->in.ctime = (uint32_t)time_realtime_s();
        target->nlink = t->in.links;
        write_inode(t);
    }
    fs_unlock(fs);
    return r;
}

/* fs lock held: one name less for inode `ino` (a directory loses its own
 * "." too); freed now if nothing has it open, else when it is closed. */
static void drop_link(struct ext2fs *fs, uint32_t ino, bool dir) {
    struct enode *c = cached(fs, ino);
    if (c) {
        c->in.links = dir ? 0 : (c->in.links ? c->in.links - 1 : 0);
        c->in.ctime = (uint32_t)time_realtime_s();
        c->v->nlink = c->in.links;
        write_inode(c);
        return; /* the release frees it if that was the last name */
    }
    struct ext2_inode in;
    if (read_inode(fs, ino, &in) < 0)
        return;
    in.links = dir ? 0 : (in.links ? in.links - 1 : 0);
    if (in.links == 0) {
        destroy_inode(fs, &in, ino);
    } else {
        in.ctime = (uint32_t)time_realtime_s();
        bpart(fs, inode_off(fs, ino), &in, sizeof in, true);
    }
}

static int e_unlink(struct vnode *dir, const char *name, bool dir_only) {
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return -EINVAL;
    struct enode *d = dir->data;
    struct ext2fs *fs = d->fs;
    fs_lock(fs);
    uint32_t ino;
    uint64_t where;
    int r = dir_find(d, name, &ino, &where);
    struct ext2_inode in;
    if (r == 0) {
        struct enode *c = cached(fs, ino);
        if (c)
            in = c->in;
        else
            r = read_inode(fs, ino, &in);
    }
    bool is_dir = r == 0 && (in.mode & S_IFMT_) == S_IFDIR_;
    if (r == 0 && dir_only && !is_dir)
        r = -ENOTDIR;
    else if (r == 0 && !dir_only && is_dir)
        r = -EISDIR;
    else if (r == 0 && is_dir && !dir_empty(fs, &in, ino))
        r = -ENOTEMPTY;
    if (r == 0)
        r = dir_remove(d, where);
    if (r == 0) {
        drop_link(fs, ino, is_dir);
        if (is_dir && d->in.links > 2) {
            d->in.links--;
            dir->nlink = d->in.links;
            write_inode(d);
        }
    }
    fs_unlock(fs);
    return r;
}

/* Is directory inode `anc` the same as, or above, directory `dir`? (walks ".."; fs lock held) */
static bool is_ancestor(struct ext2fs *fs, uint32_t anc, uint32_t dir) {
    for (int guard = 0; guard < 4096; guard++) {
        if (dir == anc)
            return true;
        if (dir == ROOT_INO)
            return false;
        struct enode tmp = {.fs = fs, .ino = dir};
        if (read_inode(fs, dir, &tmp.in) < 0)
            return false;
        uint32_t up;
        if (dir_find(&tmp, "..", &up, NULL) < 0 || up == dir)
            return false;
        dir = up;
    }
    return true;
}

static int e_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname) {
    if (ndir->ops != &dir_ops)
        return -EXDEV;
    struct enode *od = odir->data, *nd = ndir->data;
    if (od->fs != nd->fs)
        return -EXDEV;
    struct ext2fs *fs = od->fs;
    fs_lock(fs);
    uint32_t sino, dino;
    uint64_t swhere, dwhere;
    int r = dir_find(od, oname, &sino, &swhere);
    struct ext2_inode sin;
    if (r == 0) {
        struct enode *c = cached(fs, sino);
        if (c)
            sin = c->in;
        else
            r = read_inode(fs, sino, &sin);
    }
    bool sdir = r == 0 && (sin.mode & S_IFMT_) == S_IFDIR_;
    if (r == 0 && sdir && nd->ino != od->ino && is_ancestor(fs, sino, nd->ino))
        r = -EINVAL; /* into itself */
    if (r == 0 && dir_find(nd, nname, &dino, &dwhere) == 0) {
        if (dino == sino) {
            fs_unlock(fs);
            return 0;
        }
        struct ext2_inode din;
        struct enode *c = cached(fs, dino);
        if (c)
            din = c->in;
        else
            r = read_inode(fs, dino, &din);
        bool ddir = r == 0 && (din.mode & S_IFMT_) == S_IFDIR_;
        if (r == 0 && sdir && !ddir)
            r = -ENOTDIR;
        else if (r == 0 && !sdir && ddir)
            r = -EISDIR;
        else if (r == 0 && ddir && !dir_empty(fs, &din, dino))
            r = -ENOTEMPTY;
        if (r == 0)
            r = dir_remove(nd, dwhere);
        if (r == 0) {
            drop_link(fs, dino, ddir);
            if (ddir && nd->in.links > 2)
                nd->in.links--;
        }
        /* the old name may have moved within the block we rewrote: find it again */
        if (r == 0 && nd->ino == od->ino)
            r = dir_find(od, oname, &sino, &swhere);
    }
    if (r == 0)
        r = dir_add(nd, nname, sino, ftype_of(sin.mode));
    if (r == 0) {
        if (nd->ino == od->ino) /* the new entry may have shifted the old one */
            r = dir_find(od, oname, &sino, &swhere);
        if (r == 0)
            r = dir_remove(od, swhere);
    }
    if (r == 0 && sdir && od->ino != nd->ino) {
        struct vnode *cv = NULL;
        struct enode *c = cached(fs, sino);
        struct enode tmp = {.fs = fs, .ino = sino, .in = sin};
        set_dotdot(fs, c ? c : &tmp, nd->ino);
        if (od->in.links > 2)
            od->in.links--;
        nd->in.links++;
        if (c && c->v->parent == odir) { /* its vnode's parent follows */
            cv = c->v;
            vnode_ref(ndir);
            cv->parent = ndir;
        }
        odir->nlink = od->in.links;
        ndir->nlink = nd->in.links;
        write_inode(od);
        write_inode(nd);
        if (cv)
            vnode_put(odir); /* after the lock: our hold on the old parent */
    } else if (r == 0) {
        write_inode(nd);
    }
    fs_unlock(fs);
    return r;
}

static int e_readdir(struct vnode *dir, uint64_t index, char *name, size_t name_size, uint8_t *type, uint64_t *ino) {
    struct enode *en = dir->data;
    struct ext2fs *fs = en->fs;
    fs_lock(fs);
    uint64_t size = isize(&en->in);
    /* carry on from the last call when reading in order */
    uint64_t pos = 0, n = 0;
    if (index >= en->rd_index && en->rd_index) {
        pos = en->rd_pos;
        n = en->rd_index;
    }
    uint8_t *b = kmalloc(fs->bs);
    int found = 0;
    int64_t loaded = -1;
    while (b && pos < size) {
        uint64_t lb = pos / fs->bs;
        if ((int64_t)lb != loaded) {
            int64_t pb = bmap(en, lb, false, false);
            if (pb <= 0 || bread(fs, (uint32_t)pb, b) < 0) {
                pos = (lb + 1) * fs->bs;
                continue;
            }
            loaded = (int64_t)lb;
        }
        struct dirent2 *d = (struct dirent2 *)(b + pos % fs->bs);
        if (d->rec_len < 8) {
            pos = (lb + 1) * fs->bs;
            continue;
        }
        if (d->inode) {
            if (n == index) {
                size_t l = d->name_len < name_size - 1 ? d->name_len : name_size - 1;
                memcpy(name, d->name, l);
                name[l] = 0;
                static const uint8_t dt[] = {0, 8 /* DT_REG */, 4 /* DT_DIR */, 2, 6, 1, 12, 10 /* DT_LNK */};
                *type = d->file_type < 8 ? dt[d->file_type] : 0;
                *ino = d->inode;
                en->rd_index = n + 1;
                en->rd_pos = pos + d->rec_len;
                found = 1;
                break;
            }
            n++;
        }
        pos += d->rec_len;
    }
    kfree(b);
    fs_unlock(fs);
    return found;
}

static int64_t e_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)f;
    struct enode *en = v->data;
    fs_lock(en->fs);
    int64_t r = file_rw(en, buf, len, off, false);
    fs_unlock(en->fs);
    return r;
}

static int64_t e_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)f;
    struct enode *en = v->data;
    fs_lock(en->fs);
    int64_t r = file_rw(en, (uint8_t *)buf, len, off, true);
    fs_unlock(en->fs);
    return r;
}

static int e_truncate(struct vnode *v, uint64_t size) {
    struct enode *en = v->data;
    struct ext2fs *fs = en->fs;
    fs_lock(fs);
    uint64_t old = isize(&en->in);
    int r = 0;
    if (size < old) {
        uint64_t keep = (size + fs->bs - 1) / fs->bs;
        truncate_blocks(en, keep);
        if (size % fs->bs) { /* the tail of the last kept block reads as zeros from now on */
            int64_t pb = bmap(en, size / fs->bs, false, false);
            uint8_t *b = pb > 0 ? kmalloc(fs->bs) : NULL;
            if (b && bread(fs, (uint32_t)pb, b) == 0) {
                memset(b + size % fs->bs, 0, fs->bs - size % fs->bs);
                bwrite(fs, (uint32_t)pb, b);
            }
            kfree(b);
        }
    }
    set_isize(en, size); /* growing: the rest is a hole */
    en->in.mtime = en->in.ctime = (uint32_t)time_realtime_s();
    v->mtime = en->in.mtime;
    r = write_inode(en);
    fs_unlock(fs);
    return r;
}

static int e_readlink(struct vnode *v, char *buf, size_t size) {
    struct enode *en = v->data;
    struct ext2fs *fs = en->fs;
    fs_lock(fs);
    size_t len = en->in.size;
    int r;
    if (len < sizeof en->in.block && en->in.blocks == 0) {
        r = (int)(len < size ? len : size);
        memcpy(buf, en->in.block, (size_t)r);
    } else {
        uint8_t *b = kmalloc(fs->bs);
        int64_t pb = b ? bmap(en, 0, false, false) : -ENOMEM;
        if (pb > 0 && bread(fs, (uint32_t)pb, b) == 0) {
            if (len > fs->bs)
                len = fs->bs;
            r = (int)(len < size ? len : size);
            memcpy(buf, b, (size_t)r);
        } else {
            r = -EIO;
        }
        kfree(b);
    }
    fs_unlock(fs);
    return r;
}

static int e_fsync(struct vnode *v) {
    struct enode *en = v->data;
    fs_lock(en->fs);
    write_inode(en);
    int r = bflush(en->fs, false); /* the whole batch: simpler than this file's share, and rarely much */
    fs_unlock(en->fs);
    return r;
}

static const struct vnode_ops dir_ops = {
    .lookup = e_lookup, .create = e_create, .mkdir = e_mkdir, .symlink = e_symlink, .link = e_link,
    .unlink = e_unlink, .rename = e_rename, .readdir = e_readdir, .release = e_release, .fsync = e_fsync,
};
static const struct vnode_ops file_ops = {
    .read = e_read, .write = e_write, .truncate = e_truncate, .release = e_release, .fsync = e_fsync,
};
static const struct vnode_ops link_ops = {.readlink = e_readlink, .release = e_release};

/* statfs for a file on an ext2 file system: its real size and free space
 * (struct statfs as 64-bit words). Returns -1 for anything else. */
int ext2_statfs(struct vnode *v, uint64_t *st);
int ext2_statfs(struct vnode *v, uint64_t *st) {
    if (v->ops != &dir_ops && v->ops != &file_ops && v->ops != &link_ops)
        return -1;
    struct ext2fs *fs = ((struct enode *)v->data)->fs;
    uint64_t free_blocks = sb32(fs, 12), reserved = sb32(fs, 8);
    st[0] = EXT2_MAGIC;
    st[1] = fs->bs;
    st[2] = fs->blocks_count;
    st[3] = free_blocks;
    st[4] = free_blocks > reserved ? free_blocks - reserved : 0; /* what an ordinary user may use */
    st[5] = fs->inodes_count;
    st[6] = sb32(fs, 16);
    st[7] = 0;
    st[8] = 255;
    st[9] = fs->bs;
    return 0;
}

/* ---- mounting ---- */

/* Writes back what changed, every few seconds. */
static void flush_main(void *arg) {
    (void)arg;
    for (;;) {
        thread_sleep_ms(FLUSH_EVERY_MS);
        for (struct ext2fs *fs = mounted_fs; fs; fs = fs->next) {
            if (!fs->ndirty && !fs->sb_dirty && !fs->gd_dirty)
                continue;
            fs_lock(fs);
            bflush(fs, false);
            fs_unlock(fs);
        }
    }
}

/* The journal of a file system that has one (inode s_journal_inum): where its blocks are, what it says. A
 * journal holding a transaction not yet replayed (e2fsck replays it before mounting) means nothing may be
 * written: -EIO. Features this writer doesn't know: written without the journal, as plain ext2. */
static int journal_open(struct ext2fs *fs) {
    if (!(sb32(fs, 92) & COMPAT_HAS_JOURNAL))
        return 0;
    uint32_t jino = sb32(fs, 224);
    struct enode tmp = {0};
    tmp.fs = fs, tmp.ino = jino;
    if (!jino || read_inode(fs, jino, &tmp.in) < 0) {
        kprintf("ext2: %s: its journal is elsewhere or unreadable: written without it\n", fs->dev->name);
        return 0;
    }
    uint32_t nblk = (uint32_t)(isize(&tmp.in) / fs->bs);
    int64_t b0 = nblk >= 1024 ? bmap(&tmp, 0, false, false) : 0;
    fs->jsb = kmalloc(fs->bs), fs->jblk = kmalloc(fs->bs), fs->sbblk = kmalloc(fs->bs);
    if (b0 <= 0 || !fs->jsb || !fs->jblk || !fs->sbblk ||
        blkdev_read(fs->dev, fs->jsb, fs->bs, (uint64_t)b0 * fs->bs) != (int64_t)fs->bs)
        goto plain;
    uint32_t type = be32(fs->jsb + 4), jbs = be32(fs->jsb + 12), maxlen = be32(fs->jsb + 16);
    uint32_t first = be32(fs->jsb + 20), incompat = type == JBD2_SUPER_V2 ? be32(fs->jsb + 40) : 0;
    if (be32(fs->jsb) != JBD2_MAGIC || (type != JBD2_SUPER_V1 && type != JBD2_SUPER_V2) || jbs != fs->bs ||
        maxlen > nblk || first < 1 || first >= maxlen) {
        kprintf("ext2: %s: its journal isn't one this system reads: written without it\n", fs->dev->name);
        goto plain;
    }
    if (be32(fs->jsb + 28)) { /* s_start: a transaction to replay first */
        kprintf("ext2: %s: its journal has changes to replay (e2fsck does it): not mounted\n", fs->dev->name);
        kfree(fs->jsb), kfree(fs->jblk), kfree(fs->sbblk);
        fs->jsb = fs->jblk = fs->sbblk = NULL;
        return -EIO;
    }
    if (incompat & ~(uint32_t)(JBD2_INCOMPAT_REVOKE | JBD2_INCOMPAT_64BIT)) {
        kprintf("ext2: %s: its journal uses features (%x) this system doesn't write: written without it\n",
                fs->dev->name, incompat);
        goto plain;
    }
    fs->jmap = kmalloc(sizeof *fs->jmap * maxlen);
    if (!fs->jmap)
        goto plain;
    for (uint32_t i = 0; i < maxlen; i++) {
        int64_t b = bmap(&tmp, i, false, false);
        if (b <= 0) {
            kfree(fs->jmap);
            fs->jmap = NULL;
            goto plain;
        }
        fs->jmap[i] = (uint32_t)b;
    }
    fs->sbblk_no = 1024 / fs->bs;
    if (blkdev_read(fs->dev, fs->sbblk, fs->bs, (uint64_t)fs->sbblk_no * fs->bs) != (int64_t)fs->bs)
        goto plain;
    fs->jmax = maxlen, fs->jfirst = first, fs->jhead = first, fs->jlast_len = 0;
    fs->jseq = be32(fs->jsb + 24);
    fs->jtag = (incompat & JBD2_INCOMPAT_64BIT) ? 12 : 8;
    fs->jon = true;
    sb32_set(fs, 96, sb32(fs, 96) | INCOMPAT_RECOVER); /* in use: e2fsck looks at the journal after a crash */
    kprintf("ext2: %s: journal of %u MiB\n", fs->dev->name, (uint32_t)((uint64_t)maxlen * fs->bs >> 20));
    return 0;
plain:
    kfree(fs->jsb), kfree(fs->jblk), kfree(fs->sbblk);
    fs->jsb = fs->jblk = fs->sbblk = NULL;
    return 0;
}

/* The root of the ext2 file system on `dev` (referenced): mounted once, the
 * same tree for every mount of it. */
int ext2_mount(struct blkdev *dev, struct vnode **out) {
    uint64_t f = spin_lock_irqsave(&list_lock);
    for (struct ext2fs *fs = mounted_fs; fs; fs = fs->next)
        if (fs->dev == dev) {
            spin_unlock_irqrestore(&list_lock, f);
            vnode_ref(fs->root);
            *out = fs->root;
            return 0;
        }
    spin_unlock_irqrestore(&list_lock, f);

    struct ext2fs *fs = kzalloc(sizeof *fs);
    if (!fs)
        return -ENOMEM;
    fs->dev = dev;
    if (blkdev_read(dev, fs->sb, 1024, 1024) != 1024) {
        kfree(fs);
        return -EIO;
    }
    uint16_t magic, rev_minor;
    memcpy(&magic, fs->sb + 56, 2);
    (void)rev_minor;
    if (magic != EXT2_MAGIC) {
        kfree(fs);
        return -EINVAL;
    }
    uint32_t rev = sb32(fs, 76), incompat = sb32(fs, 96), ro_compat = sb32(fs, 100);
    fs->bs = 1024u << sb32(fs, 24);
    fs->inodes_count = sb32(fs, 0);
    fs->blocks_count = sb32(fs, 4);
    fs->first_data = sb32(fs, 20);
    fs->bpg = sb32(fs, 32);
    fs->ipg = sb32(fs, 40);
    uint16_t isz = 128;
    if (rev >= 1)
        memcpy(&isz, fs->sb + 88, 2);
    fs->inode_size = isz;
    fs->first_ino = rev >= 1 ? sb32(fs, 84) : 11;
    if (incompat & ~(uint32_t)(INCOMPAT_FILETYPE | INCOMPAT_RECOVER)) {
        kprintf("ext2: %s uses features this system doesn't support (incompat %x)\n", dev->name, incompat);
        kfree(fs);
        return -EINVAL;
    }
    if (ro_compat & ~(uint32_t)(RO_COMPAT_SPARSE | RO_COMPAT_LARGE_FILE)) {
        kprintf("ext2: %s needs features this system can't write (ro_compat %x)\n", dev->name, ro_compat);
        kfree(fs);
        return -EINVAL;
    }
    if (fs->bs > 65536 || !fs->bpg || !fs->ipg || fs->inode_size < 128) {
        kfree(fs);
        return -EINVAL;
    }
    fs->filetype = incompat & INCOMPAT_FILETYPE;
    fs->groups = (fs->blocks_count - fs->first_data + fs->bpg - 1) / fs->bpg;
    fs->gdt_block = fs->first_data + 1;
    size_t gd_bytes = (size_t)fs->groups * sizeof(struct ext2_gd);
    fs->gd = kmalloc(gd_bytes);
    if (!fs->gd || blkdev_read(dev, fs->gd, gd_bytes, (uint64_t)fs->gdt_block * fs->bs) != (int64_t)gd_bytes) {
        kfree(fs->gd);
        kfree(fs);
        return -EIO;
    }
    /* mounted: in use (in memory; on the disk with the first change, so a
     * power cut before any change leaves it clean) */
    uint16_t state;
    memcpy(&state, fs->sb + 58, 2);
    fs->disk_clean = state & STATE_VALID;
    sb_state(fs, false);
    uint16_t mnt_count;
    memcpy(&mnt_count, fs->sb + 52, 2);
    mnt_count++;
    memcpy(fs->sb + 52, &mnt_count, 2);
    sb32_set(fs, 44, (uint32_t)time_realtime_s()); /* s_mtime */

    fs->bhint = kzalloc(sizeof *fs->bhint * fs->groups);
    fs->ihint = kzalloc(sizeof *fs->ihint * fs->groups);
    if (!fs->bhint || !fs->ihint) {
        kfree(fs->bhint);
        kfree(fs->ihint);
        kfree(fs->gd);
        kfree(fs);
        return -ENOMEM;
    }
    fs->lru.next = fs->lru.prev = &fs->lru;
    fs->maxbufs = BCACHE_BYTES / fs->bs;
    fs->maxdirty = BDIRTY_BYTES / fs->bs;
    fs->stage = kmalloc(256 * 1024);
    int jr = fs->stage ? journal_open(fs) : 0;
    if (jr == 0 && (incompat & INCOMPAT_RECOVER) && !fs->jon)
        jr = -EIO; /* it says a journal may need replaying, and there is none this system can read */
    if (jr < 0) {
        kfree(fs->stage);
        kfree(fs->bhint);
        kfree(fs->ihint);
        kfree(fs->gd);
        kfree(fs);
        return jr;
    }

    int r = iget(fs, ROOT_INO, &fs->root);
    if (r < 0) {
        kfree(fs->stage);
        kfree(fs->gd);
        kfree(fs);
        return r;
    }
    static bool flusher;
    if (!flusher) {
        flusher = true;
        thread_create("ext2-flush", flush_main, NULL);
    }
    fs->root->parent = fs->root;
    kprintf("ext2: %s mounted: %u MiB, %u-byte blocks, %u groups, %u MiB free\n", dev->name,
            (uint32_t)((uint64_t)fs->blocks_count * fs->bs >> 20), fs->bs, fs->groups,
            (uint32_t)((uint64_t)sb32(fs, 12) * fs->bs >> 20));
    f = spin_lock_irqsave(&list_lock);
    fs->next = mounted_fs;
    mounted_fs = fs;
    spin_unlock_irqrestore(&list_lock, f);
    vnode_ref(fs->root); /* one for the list, one for the caller */
    *out = fs->root;
    return 0;
}

/* sync(2) and power-off: inodes in use written, every mounted ext2 marked
 * clean, the disks flushed. */
void ext2_sync_all(void) {
    for (struct ext2fs *fs = mounted_fs; fs; fs = fs->next) {
        fs_lock(fs);
        for (int b = 0; b < 256; b++)
            for (struct enode *e = fs->cache[b]; e; e = e->hnext)
                write_inode(e);
        bflush(fs, true); /* still mounted: the next change marks it in use again */
        fs_unlock(fs);
    }
}
