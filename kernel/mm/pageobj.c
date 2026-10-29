#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <linux/abi.h>

/* Page-backed in-memory objects: the storage behind ramfs files, memfds and
 * MAP_SHARED|MAP_ANONYMOUS memory. Single pages instead of one contiguous
 * buffer mean growing never copies, big files need no contiguous memory,
 * and a page can be handed to page tables as is, so every process that maps
 * the object shares the same physical memory as read() and write().
 *
 * The lock is held per page, not per call: a large write (an initramfs
 * member is up to a few hundred MiB) never keeps interrupts off for long. */

#define MAX_OBJECT_SIZE (1ULL << 44) /* keeps page-count arithmetic far from overflow */

struct pageobj *pageobj_create(void) {
    struct pageobj *o = kzalloc(sizeof *o);
    if (o)
        o->refcount = 1;
    return o;
}

void pageobj_ref(struct pageobj *o) { __atomic_add_fetch(&o->refcount, 1, __ATOMIC_RELAXED); }

void pageobj_put(struct pageobj *o) {
    if (!o || __atomic_sub_fetch(&o->refcount, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    for (uint64_t i = 0; i < o->nslots; i++)
        if (o->pages[i])
            page_unref(o->pages[i]);
    kfree(o->pages);
    kfree(o);
}

uint64_t pageobj_size(struct pageobj *o) { return __atomic_load_n(&o->size, __ATOMIC_RELAXED); }

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
 * completely (`whole`) skips the zero fill. Lock held. */
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

int64_t pageobj_read(struct pageobj *o, void *buf, size_t len, uint64_t off) {
    uint8_t *dst = buf;
    size_t done = 0;
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
        uint64_t p = idx < o->nslots ? o->pages[idx] : 0;
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

int pageobj_truncate(struct pageobj *o, uint64_t size) {
    if (size > MAX_OBJECT_SIZE)
        return -EFBIG;
    uint64_t fl = spin_lock_irqsave(&o->lock);
    if (size < o->size) {
        /* Drop whole pages past the end. Pages still mapped somewhere live on
         * through their page-table references (see the header comment). */
        for (uint64_t i = ALIGN_UP(size, PAGE_SIZE) / PAGE_SIZE; i < o->nslots; i++) {
            if (o->pages[i]) {
                page_unref(o->pages[i]);
                o->pages[i] = 0;
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
    spin_unlock_irqrestore(&o->lock, fl);
    return 0;
}

uint64_t pageobj_map_page(struct pageobj *o, uint64_t idx) {
    uint64_t fl = spin_lock_irqsave(&o->lock);
    uint64_t p = 0;
    if (idx < ALIGN_UP(o->size, PAGE_SIZE) / PAGE_SIZE) {
        p = slot_page(o, idx, false);
        if (p)
            page_ref(p);
    }
    spin_unlock_irqrestore(&o->lock, fl);
    return p;
}

/* An object over memory that is already filled and will never be freed: a
 * boot module (a large model file, say), so it becomes a file without a
 * single byte being copied. The pages are pinned: their count never drops
 * to zero, so they are never handed to the allocator. */
struct pageobj *pageobj_adopt(uint64_t phys, uint64_t size) {
    if (phys % PAGE_SIZE)
        return NULL;
    struct pageobj *o = pageobj_create();
    if (!o)
        return NULL;
    uint64_t n = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!ensure_slots(o, n)) {
        pageobj_put(o);
        return NULL;
    }
    for (uint64_t i = 0; i < n; i++) {
        page_pin(phys + i * PAGE_SIZE);
        o->pages[i] = phys + i * PAGE_SIZE;
    }
    o->size = size;
    return o;
}
