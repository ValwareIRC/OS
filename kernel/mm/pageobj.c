#include <asm/pgtable.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <linux/abi.h>

/* Page-backed in-memory objects: the storage behind ramfs files, memfds and
 * MAP_SHARED|MAP_ANONYMOUS memory. Single pages instead of one contiguous
 * buffer mean growing never copies, big files need no contiguous memory,
 * and a page can be handed to page tables as is, so every process that maps
 * the object shares the same physical memory as read() and write().
 *
 * The lock is held per page, not per call: a large write (an initramfs
 * member is up to a few hundred MiB) never keeps interrupts off for long.
 *
 * Objects with a source (mm.h) are the file cache: the base system's files, read from the medium as they are
 * first used rather than all at boot, and the pages of the disk's files that programs map (their libraries),
 * shared by every program that maps them rather than copied whole into each. Their pages are read from the
 * source a run at a time (pageobj_fill), never with the object's lock held, and installed only if nothing
 * changed meanwhile (`gen`). The keeper thread frees the ones nothing maps when memory runs short. */

#define MAX_OBJECT_SIZE (1ULL << 44) /* keeps page-count arithmetic far from overflow */
#define RUN_FIRST 16                 /* pages a fill reads when it doesn't continue the last one (64 KiB) */

/* The objects with a source, in the order reclaim visits them (it moves each to the back as it does). */
static spinlock_t src_lock = SPINLOCK_INIT;
static struct pageobj *src_head, *src_tail;
static uint64_t src_count;
static uint64_t src_pages; /* pages they hold (atomic) */
/* Objects gone whose source still has to be told (pagesrc.release), for the keeper thread: the last reference
 * often goes under a lock (an address space's, as a mapping is taken down), and the release may need to wait. */
static struct pageobj *released;

static void src_unlink_locked(struct pageobj *o) {
    if (o->cprev)
        o->cprev->cnext = o->cnext;
    else
        src_head = o->cnext;
    if (o->cnext)
        o->cnext->cprev = o->cprev;
    else
        src_tail = o->cprev;
    o->cnext = o->cprev = NULL;
    src_count--;
}

static void src_append_locked(struct pageobj *o) {
    o->cnext = NULL;
    o->cprev = src_tail;
    if (src_tail)
        src_tail->cnext = o;
    else
        src_head = o;
    src_tail = o;
    src_count++;
}

struct pageobj *pageobj_create(void) {
    struct pageobj *o = kzalloc(sizeof *o);
    if (o)
        o->refcount = 1;
    return o;
}

void pageobj_ref(struct pageobj *o) { __atomic_add_fetch(&o->refcount, 1, __ATOMIC_RELAXED); }

bool pageobj_tryget(struct pageobj *o) {
    int n = __atomic_load_n(&o->refcount, __ATOMIC_RELAXED);
    while (n > 0)
        if (__atomic_compare_exchange_n(&o->refcount, &n, n + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return true;
    return false;
}

static void free_pages_of(struct pageobj *o) {
    for (uint64_t i = 0; i < o->nslots; i++)
        if (o->pages[i])
            page_unref(o->pages[i]);
    kfree(o->pages);
    o->pages = NULL;
    o->nslots = 0;
}

void pageobj_put(struct pageobj *o) {
    if (!o || __atomic_sub_fetch(&o->refcount, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    if (o->src) {
        uint64_t f = spin_lock_irqsave(&src_lock);
        src_unlink_locked(o);
        spin_unlock_irqrestore(&src_lock, f);
        __atomic_sub_fetch(&src_pages, o->held, __ATOMIC_RELAXED);
        o->held = 0;
    }
    free_pages_of(o);
    if (o->src && o->src->release) {
        /* the object waits for the keeper, which tells the source */
        uint64_t f = spin_lock_irqsave(&src_lock);
        o->cnext = released;
        released = o;
        spin_unlock_irqrestore(&src_lock, f);
        return;
    }
    kfree(o);
}

uint64_t pageobj_size(struct pageobj *o) { return __atomic_load_n(&o->size, __ATOMIC_RELAXED); }

static inline uint64_t npages(const struct pageobj *o) { return ALIGN_UP(o->size, PAGE_SIZE) / PAGE_SIZE; }
static inline uint64_t slot(const struct pageobj *o, uint64_t idx) { return idx < o->nslots ? o->pages[idx] : 0; }

/* Makes room for `n` page slots. Lock held. */
static bool ensure_slots(struct pageobj *o, uint64_t n) {
    if (n <= o->nslots)
        return true;
    uint64_t cap = o->nslots ? o->nslots * 2 : 16;
    if (cap < n)
        cap = n;
    uint64_t *np = kmalloc(cap * sizeof *np);
    if (!np)
        return false;
    if (o->pages)
        memcpy(np, o->pages, o->nslots * sizeof *np);
    memset(np + o->nslots, 0, (cap - o->nslots) * sizeof *np);
    kfree(o->pages);
    o->pages = np;
    o->nslots = cap;
    return true;
}

/* The page at `idx`, allocated if missing. A page about to be overwritten
 * completely (`whole`) skips the zero fill. Lock held; not for objects with
 * a source (their missing pages are the source's, not zeros). */
static uint64_t slot_page(struct pageobj *o, uint64_t idx, bool whole) {
    if (!ensure_slots(o, idx + 1))
        return 0;
    uint64_t p = o->pages[idx];
    if (!p) {
        p = page_alloc_shared();
        if (!p)
            return 0;
        if (!whole)
            memset(phys_to_virt(p), 0, PAGE_SIZE);
        o->pages[idx] = p;
    }
    return p;
}

void pageobj_set_source(struct pageobj *o, const struct pagesrc *src, void *ctx, uint64_t off) {
    o->src = src;
    o->src_ctx = ctx;
    o->src_off = off;
    o->ra = RUN_FIRST;
    uint64_t f = spin_lock_irqsave(&src_lock);
    src_append_locked(o);
    spin_unlock_irqrestore(&src_lock, f);
}

bool pageobj_missing(struct pageobj *o, uint64_t idx) {
    uint64_t fl = spin_lock_irqsave(&o->lock);
    bool m = o->src && idx < npages(o) && !slot(o, idx);
    spin_unlock_irqrestore(&o->lock, fl);
    return m;
}

int pageobj_fill(struct pageobj *o, uint64_t idx, uint64_t want) {
    uint64_t fl = spin_lock_irqsave(&o->lock);
    const struct pagesrc *src = o->src;
    uint64_t end = npages(o);
    if (!src || idx >= end || slot(o, idx)) {
        spin_unlock_irqrestore(&o->lock, fl);
        return 0;
    }
    /* A reader going straight through reads further ahead each time (to the source's limit); one that jumps
     * about gets RUN_FIRST pages around what it touched. */
    uint64_t run = idx == o->ra_next ? o->ra : RUN_FIRST;
    if (run < want)
        run = want;
    if (run > src->max_run)
        run = src->max_run;
    uint64_t n = 1;
    while (n < run && idx + n < end && !slot(o, idx + n))
        n++;
    o->ra_next = idx + n;
    o->ra = run * 2 > src->max_run ? src->max_run : (uint32_t)(run * 2);
    uint32_t gen = o->gen;
    void *ctx = o->src_ctx;
    uint64_t at = o->src_off + idx * PAGE_SIZE, avail = o->size - idx * PAGE_SIZE;
    spin_unlock_irqrestore(&o->lock, fl);

    /* Read straight into the pages the object will keep: one run of memory, each page of it then owned
     * on its own (page_adopt). Short of memory, a shorter run. */
    uint64_t phys = pmm_alloc(n);
    while (!phys && n > 1)
        phys = pmm_alloc(n /= 2);
    if (!phys)
        return -ENOMEM;
    uint8_t *buf = phys_to_virt(phys);
    int64_t got = src->read(ctx, at, buf, n * PAGE_SIZE);
    if (got < 0) {
        pmm_free(phys, n);
        return (int)got;
    }
    if ((uint64_t)got > avail)
        got = (int64_t)avail; /* past the object's end: zeros, whatever else the source holds there */
    if ((uint64_t)got < n * PAGE_SIZE)
        memset(buf + got, 0, n * PAGE_SIZE - (uint64_t)got);
    arch_sync_icache(buf, n * PAGE_SIZE); /* (a library's code is run straight from these pages) */

    uint64_t added = 0;
    fl = spin_lock_irqsave(&o->lock);
    bool fresh = o->gen == gen && o->src == src && ensure_slots(o, idx + n);
    end = npages(o);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t p = phys + i * PAGE_SIZE;
        if (fresh && idx + i < end && !o->pages[idx + i]) {
            page_adopt(p);
            o->pages[idx + i] = p;
            added++;
        } else {
            pmm_free(p, 1); /* stale, or read meanwhile by someone else */
        }
    }
    o->held += added;
    spin_unlock_irqrestore(&o->lock, fl);
    __atomic_add_fetch(&src_pages, added, __ATOMIC_RELAXED);
    return 0;
}

int64_t pageobj_read(struct pageobj *o, void *buf, size_t len, uint64_t off) {
    uint8_t *dst = buf;
    size_t done = 0;
    int tries = 0;
    while (done < len) {
        uint64_t pos = off + done, idx = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        uint64_t fl = spin_lock_irqsave(&o->lock);
        if (pos >= o->size) {
            spin_unlock_irqrestore(&o->lock, fl);
            break;
        }
        size_t n = PAGE_SIZE - in;
        if (n > len - done)
            n = len - done;
        if (n > o->size - pos)
            n = o->size - pos;
        uint64_t p = slot(o, idx);
        if (!p && o->src) {
            /* not read yet: read it (and as much of the rest of this read as the source allows), then again */
            spin_unlock_irqrestore(&o->lock, fl);
            int r = tries++ < 64 ? pageobj_fill(o, idx, (in + len - done + PAGE_SIZE - 1) / PAGE_SIZE) : -EIO;
            if (r < 0)
                return done ? (int64_t)done : r;
            continue;
        }
        tries = 0;
        if (p)
            memcpy(dst + done, (uint8_t *)phys_to_virt(p) + in, n);
        else
            memset(dst + done, 0, n);
        spin_unlock_irqrestore(&o->lock, fl);
        done += n;
    }
    return (int64_t)done;
}

int64_t pageobj_write(struct pageobj *o, const void *buf, size_t len, uint64_t off) {
    if (off >= MAX_OBJECT_SIZE || len > MAX_OBJECT_SIZE - off)
        return -EFBIG;
    if (o->src) {
        /* what is written here would be in memory only: the object becomes an ordinary one first */
        int r = pageobj_detach(o, pageobj_size(o));
        if (r < 0)
            return r;
    }
    const uint8_t *src = buf;
    size_t done = 0;
    /* Size the slot array once for the whole write rather than doubling
     * through it page by page. */
    uint64_t fl = spin_lock_irqsave(&o->lock);
    bool room = ensure_slots(o, ALIGN_UP(off + len, PAGE_SIZE) / PAGE_SIZE);
    spin_unlock_irqrestore(&o->lock, fl);
    if (!room)
        return -ENOMEM;
    while (done < len) {
        uint64_t pos = off + done, idx = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        size_t n = PAGE_SIZE - in;
        if (n > len - done)
            n = len - done;
        fl = spin_lock_irqsave(&o->lock);
        uint64_t p = slot_page(o, idx, n == PAGE_SIZE);
        if (p) {
            memcpy((uint8_t *)phys_to_virt(p) + in, src + done, n);
            if (pos + n > o->size)
                o->size = pos + n;
        }
        spin_unlock_irqrestore(&o->lock, fl);
        if (!p)
            return done ? (int64_t)done : -ENOMEM;
        done += n;
    }
    return (int64_t)done;
}

void pageobj_update(struct pageobj *o, const void *buf, size_t len, uint64_t off) {
    const uint8_t *s = buf;
    uint64_t fl = spin_lock_irqsave(&o->lock);
    o->gen++; /* a fill that read before this wrote would bring the old bytes */
    for (size_t done = 0; done < len;) {
        uint64_t pos = off + done, idx = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        size_t n = PAGE_SIZE - in < len - done ? PAGE_SIZE - in : len - done;
        uint64_t p = slot(o, idx);
        if (p)
            memcpy((uint8_t *)phys_to_virt(p) + in, s + done, n);
        done += n;
    }
    if (off + len > o->size)
        o->size = off + len;
    spin_unlock_irqrestore(&o->lock, fl);
}

int pageobj_truncate(struct pageobj *o, uint64_t size) {
    if (size > MAX_OBJECT_SIZE)
        return -EFBIG;
    uint64_t dropped = 0;
    uint64_t fl = spin_lock_irqsave(&o->lock);
    o->gen++;
    if (size < o->size) {
        /* Drop whole pages past the end. Pages still mapped somewhere live on
         * through their page-table references (see the header comment). */
        for (uint64_t i = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE; i < o->nslots; i++) {
            if (o->pages[i]) {
                page_unref(o->pages[i]);
                o->pages[i] = 0;
                dropped++;
            }
        }
        /* Growing again must read zeros, so clear the cut-off tail of the
         * last page. */
        uint64_t last = size / PAGE_SIZE;
        if ((size % PAGE_SIZE) && last < o->nslots && o->pages[last])
            memset((uint8_t *)phys_to_virt(o->pages[last]) + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
        if (size == 0) {
            kfree(o->pages);
            o->pages = NULL;
            o->nslots = 0;
        }
    }
    o->size = size;
    if (o->src) {
        o->held -= dropped;
        __atomic_sub_fetch(&src_pages, dropped, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&o->lock, fl);
    return 0;
}

int pageobj_detach(struct pageobj *o, uint64_t keep) {
    for (int pass = 0;; pass++) {
        uint64_t fl = spin_lock_irqsave(&o->lock);
        if (!o->src) {
            spin_unlock_irqrestore(&o->lock, fl);
            return 0;
        }
        uint64_t end = ALIGN_UP(keep < o->size ? keep : o->size, PAGE_SIZE) / PAGE_SIZE, idx = 0;
        while (idx < end && slot(o, idx))
            idx++;
        if (idx == end) {
            /* all there: from now on the pages are the object's own, and it isn't the reclaimer's to empty */
            o->src = NULL;
            uint64_t held = o->held;
            o->held = 0;
            o->gen++;
            spin_unlock_irqrestore(&o->lock, fl);
            fl = spin_lock_irqsave(&src_lock);
            src_unlink_locked(o);
            spin_unlock_irqrestore(&src_lock, fl);
            __atomic_sub_fetch(&src_pages, held, __ATOMIC_RELAXED);
            return 0;
        }
        spin_unlock_irqrestore(&o->lock, fl);
        int r = pass < 1000000 ? pageobj_fill(o, idx, end - idx) : -EIO;
        if (r < 0)
            return r;
    }
}

uint64_t pageobj_map_page_or_missing(struct pageobj *o, uint64_t idx, bool *missing) {
    uint64_t fl = spin_lock_irqsave(&o->lock);
    uint64_t p = 0;
    *missing = false;
    if (idx < npages(o)) {
        p = o->src ? slot(o, idx) : slot_page(o, idx, false);
        if (p)
            page_ref(p);
        else
            *missing = o->src != NULL;
    }
    spin_unlock_irqrestore(&o->lock, fl);
    return p;
}

uint64_t pageobj_map_page(struct pageobj *o, uint64_t idx) {
    bool missing;
    return pageobj_map_page_or_missing(o, idx, &missing);
}

/* An object over memory that is already filled: a boot module (a large model
 * file, say), so it becomes a file without a single byte being copied. With
 * `o` given, the memory is added after what it already holds (a file that
 * came in parts: every part but the last a whole number of pages). The
 * object owns the pages from then on, so they are freed with it. */
struct pageobj *pageobj_adopt(struct pageobj *o, uint64_t phys, uint64_t size) {
    if (phys % PAGE_SIZE || (o && o->size % PAGE_SIZE))
        return NULL;
    bool made = !o;
    if (made && !(o = pageobj_create()))
        return NULL;
    uint64_t first = o->size / PAGE_SIZE, n = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!ensure_slots(o, first + n)) {
        if (made)
            pageobj_put(o);
        return NULL;
    }
    for (uint64_t i = 0; i < n; i++) {
        page_adopt(phys + i * PAGE_SIZE);
        o->pages[first + i] = phys + i * PAGE_SIZE;
    }
    o->size += size;
    return o;
}

/* ---- giving memory back ---- */

uint64_t pageobj_cached_pages(void) { return __atomic_load_n(&src_pages, __ATOMIC_RELAXED); }

/* Pages of `o` only it holds (nothing maps them), up to `want`, freed. */
static uint64_t reclaim_from(struct pageobj *o, uint64_t want) {
    uint64_t got = 0;
    uint64_t fl = spin_lock_irqsave(&o->lock);
    if (o->src) {
        for (uint64_t i = 0; i < o->nslots && got < want; i++) {
            uint64_t p = o->pages[i];
            if (p && page_refcount(p) == 1) {
                o->pages[i] = 0;
                page_unref(p);
                got++;
            }
        }
        if (got) {
            o->gen++;
            o->held -= got;
            o->ra_next = ~0ULL;
        }
    }
    spin_unlock_irqrestore(&o->lock, fl);
    return got;
}

uint64_t pageobj_reclaim(uint64_t want) {
    uint64_t got = 0;
    /* each object once at most: the one at the front, moved to the back */
    uint64_t f = spin_lock_irqsave(&src_lock);
    uint64_t visits = src_count;
    spin_unlock_irqrestore(&src_lock, f);
    for (; visits && got < want; visits--) {
        f = spin_lock_irqsave(&src_lock);
        struct pageobj *o = src_head;
        if (!o) {
            spin_unlock_irqrestore(&src_lock, f);
            break;
        }
        src_unlink_locked(o);
        src_append_locked(o);
        bool held = o->held && pageobj_tryget(o);
        spin_unlock_irqrestore(&src_lock, f);
        if (!held)
            continue;
        uint64_t n = reclaim_from(o, want - got);
        got += n;
        __atomic_sub_fetch(&src_pages, n, __ATOMIC_RELAXED);
        pageobj_put(o);
    }
    return got;
}

/* The keeper: tells sources of objects gone (out of the locks the last reference may have been dropped under),
 * and keeps some memory free by dropping file pages nothing maps, before anything has to be ended for it. */
static void keeper(void *arg) {
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&src_lock);
        struct pageobj *list = released;
        released = NULL;
        spin_unlock_irqrestore(&src_lock, f);
        while (list) {
            struct pageobj *o = list;
            list = o->cnext;
            o->src->release(o->src_ctx, o);
            kfree(o);
        }
        /* low: under 1/32 of memory (or 64 MiB) free; then up to twice that */
        uint64_t total = pmm_total_pages(), low = total / 32;
        if (low < (64u << 20) / PAGE_SIZE)
            low = (64u << 20) / PAGE_SIZE;
        uint64_t free = pmm_free_pages();
        if (free < low && pageobj_cached_pages())
            pageobj_reclaim(2 * low - free);
        thread_sleep_ms(100);
    }
}

void pageobj_keeper_start(void) { KASSERT(thread_create("pagekeeper", keeper, NULL)); }
