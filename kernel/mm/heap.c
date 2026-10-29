#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>

/* The kernel heap: size classes. Small requests are rounded up to a power of
 * two (32 bytes to 64 KiB) and served from that class's free list; a class
 * with none free carves a fresh run of pages into blocks of its size. Bigger
 * requests take whole pages from the page allocator and give them back when
 * freed. Every operation is constant time under the lock.
 *
 * (It was one first-fit list of every block ever made, searched from the
 * front on each allocation with interrupts off: as the disk cache and long
 * sessions filled it with thousands of blocks, everything slowed down until
 * the system stopped answering.)
 *
 * Memory is reached through the direct map and every block is physically
 * contiguous (DMA buffers rely on it). A header before each block says which
 * class it is in, so kfree needs no size; it also catches double frees. */

#define HEAP_MAGIC 0x4f534b4845415053ULL /* "OSKHEAPS" */
#define MIN_SHIFT  5                     /* 32 bytes */
#define MAX_SHIFT  16                    /* 64 KiB: above this, whole pages */
#define NCLASSES   (MAX_SHIFT - MIN_SHIFT + 1)
#define RUN_BYTES  (64 * 1024)           /* pages carved at a time (at least 4 blocks) */
#define BIG        0xff                  /* class of a whole-page block */

struct block {
    uint64_t magic;
    uint64_t phys;  /* whole-page blocks: where its pages start */
    uint32_t pages; /* whole-page blocks: how many */
    uint8_t cls;    /* size class, or BIG */
    bool free;
    bool poisoned;  /* checking: filled with POISON when freed */
    uint8_t pad;
    struct block *next_free; /* on its class's free list, while free */
};
_Static_assert(sizeof(struct block) % 16 == 0, "block header must keep payload aligned");

static struct block *free_list[NCLASSES], *free_tail[NCLASSES];

/* Debugging use after free (fw_cfg opt/os/heapcheck=1): freed blocks are
 * filled with POISON and wait their turn at the back of their list; when one
 * is handed out again, anything that wrote to it meanwhile is reported. */
#define POISON 0xDE
static bool checking;
void heap_set_check(bool on) { checking = on; }

static size_t class_bytes(int c) { return (size_t)1 << (MIN_SHIFT + c); }

static void verify(struct block *b) {
    const uint8_t *p = (const uint8_t *)(b + 1);
    size_t n = class_bytes(b->cls);
    for (size_t i = 0; i < n; i++)
        if (p[i] != POISON) {
            uint64_t w[4] = {0};
            memcpy(w, p + (i & ~(size_t)7), sizeof w < n - (i & ~(size_t)7) ? sizeof w : n - (i & ~(size_t)7));
            panic("heap: block %p (%zu-byte class) written after it was freed, at offset %zu: %lx %lx %lx %lx", (void *)p,
                  n, i, w[0], w[1], w[2], w[3]);
        }
}
static spinlock_t heap_lock = SPINLOCK_INIT;

static int class_of(size_t size) {
    int c = 0;
    while (((size_t)1 << (MIN_SHIFT + c)) < size)
        c++;
    return c;
}

/* A run of pages cut into blocks of class c, onto its free list. Lock held. */
static bool refill(int c) {
    size_t each = sizeof(struct block) + ((size_t)1 << (MIN_SHIFT + c));
    size_t bytes = RUN_BYTES < each * 4 ? each * 4 : RUN_BYTES;
    size_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    uint64_t phys = pmm_alloc(pages);
    if (!phys)
        return false;
    uint8_t *p = phys_to_virt(phys), *end = p + pages * PAGE_SIZE;
    for (; p + each <= end; p += each) {
        struct block *b = (struct block *)p;
        b->magic = HEAP_MAGIC;
        b->cls = (uint8_t)c;
        b->free = true;
        b->poisoned = false;
        b->next_free = free_list[c];
        if (!free_list[c])
            free_tail[c] = b;
        free_list[c] = b;
    }
    return true;
}

void heap_init(void) {}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;
    if (size > ((size_t)1 << MAX_SHIFT)) {
        size_t pages = ALIGN_UP(size + sizeof(struct block), PAGE_SIZE) / PAGE_SIZE;
        uint64_t phys = pmm_alloc(pages);
        if (!phys)
            return NULL;
        struct block *b = phys_to_virt(phys);
        *b = (struct block){.magic = HEAP_MAGIC, .phys = phys, .pages = (uint32_t)pages, .cls = BIG};
        return b + 1;
    }
    int c = class_of(size);
    uint64_t f = spin_lock_irqsave(&heap_lock);
    if (!free_list[c] && !refill(c)) {
        spin_unlock_irqrestore(&heap_lock, f);
        return NULL;
    }
    struct block *b = free_list[c];
    free_list[c] = b->next_free;
    if (!free_list[c])
        free_tail[c] = NULL;
    if (b->poisoned)
        verify(b);
    b->poisoned = false;
    b->free = false;
    spin_unlock_irqrestore(&heap_lock, f);
    return b + 1;
}

void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *p) {
    if (!p)
        return;
    struct block *b = (struct block *)p - 1;
    uint64_t f = spin_lock_irqsave(&heap_lock);
    if (b->magic != HEAP_MAGIC || b->free)
        panic("kfree: bad or double free of %p", p);
    if (b->cls == BIG) {
        uint64_t phys = b->phys;
        uint32_t pages = b->pages;
        b->magic = 0;
        spin_unlock_irqrestore(&heap_lock, f);
        pmm_free(phys, pages);
        return;
    }
    if (b->cls >= NCLASSES)
        panic("kfree: bad block %p", p);
    b->free = true;
    if (checking) {
        memset(p, POISON, class_bytes(b->cls));
        b->poisoned = true;
        b->next_free = NULL; /* at the back: the longer it waits, the more a late write is caught */
        if (free_tail[b->cls])
            free_tail[b->cls]->next_free = b;
        else
            free_list[b->cls] = b;
        free_tail[b->cls] = b;
    } else {
        b->next_free = free_list[b->cls];
        free_list[b->cls] = b;
        if (!free_tail[b->cls])
            free_tail[b->cls] = b;
    }
    spin_unlock_irqrestore(&heap_lock, f);
}
