#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/spinlock.h>

/* Physical frame allocator (bitmap, one bit per 4 KiB frame). Returns
 * physical addresses; 0 means out of memory (frame 0 is never handed out). */
void pmm_init(void);
uint64_t pmm_alloc(size_t pages);
void pmm_free(uint64_t phys, size_t pages);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);
/* Gives memory the bootloader lent (an unpacked boot module) to the allocator. */
void pmm_give(uint64_t phys, uint64_t len);

/* Reference-counted single pages, for memory with several owners at once
 * (a file's page that is also mapped by processes). page_alloc_shared
 * returns a page (not zeroed) holding one reference; the last page_unref
 * frees it. Never mix with pmm_free on the same page. */
uint64_t page_alloc_shared(void);
void page_ref(uint64_t phys);
void page_unref(uint64_t phys);
/* How many references a page has (copy-on-write: one means it is ours alone). */
uint32_t page_refcount(uint64_t phys);
void page_adopt(uint64_t phys); /* boot-module memory now owned by a file: freed with it */

/* Page-backed storage for in-memory files (ramfs, memfd) and shared
 * anonymous memory: an array of single pages, each allocated (zero-filled)
 * on first write or first mapping fault. Holes read as zeros. The object
 * holds one reference on each of its pages and every page-table entry
 * mapping one holds another, so a mapped page outlives truncation and the
 * file itself. Buffers passed in are kernel memory.
 *
 * An object may have a source: where its contents are kept when they aren't in memory (a file on the disk, the
 * base system on the medium). A missing page of such an object is read from the source when it is first wanted
 * (by a read or a mapping fault), not made of zeros, and a page that nothing but the object holds may be dropped
 * when memory runs short, since it can be read again. So every page of it always matches the source: what
 * changes the source changes the pages too (pageobj_update), and a write that only the object would hold ends
 * the source first, the whole contents read in (pageobj_detach). */
struct pageobj;
struct pagesrc {
    /* `len` bytes (whole pages) at `off` in the source into `buf` (contiguous kernel memory): the number of bytes
     * there were (fewer at its end; the rest is zeroed for it) or a negative errno. It may wait for the device,
     * so it is never called with a spinlock held. */
    int64_t (*read)(void *ctx, uint64_t off, void *buf, size_t len);
    /* The object is gone (optional; called later, from a thread of its own, never under a lock). */
    void (*release)(void *ctx, struct pageobj *o);
    uint32_t max_run;   /* pages read at once at most (the read-ahead of a reader going straight through) */
    uint32_t first_run; /* pages a fill reads that doesn't continue the last one (0: 16) */
};
struct pageobj {
    int refcount;
    spinlock_t lock;
    uint64_t size;   /* bytes */
    uint64_t nslots; /* entries in `pages` */
    uint64_t *pages; /* physical address per page index, 0 = hole (or, with a source, not read yet) */
    const struct pagesrc *src; /* NULL: none */
    void *src_ctx;
    uint64_t src_off;          /* where in the source the object's first byte is */
    uint32_t gen;              /* changes when pages change or go: a read begun before is stale */
    uint32_t ra;               /* pages the next fill reads, if it continues the last one */
    uint64_t ra_next;          /* the page after the last fill */
    uint64_t held;             /* pages present (objects with a source) */
    struct pageobj *cnext, *cprev; /* objects with a source, oldest-reclaimed first */
};
struct pageobj *pageobj_create(void);
void pageobj_ref(struct pageobj *o);
bool pageobj_tryget(struct pageobj *o); /* a reference, unless the last one already went */
void pageobj_put(struct pageobj *o);
uint64_t pageobj_size(struct pageobj *o);
int64_t pageobj_read(struct pageobj *o, void *buf, size_t len, uint64_t off);
/* Writes extend the size; returns bytes written or -ENOMEM. */
int64_t pageobj_write(struct pageobj *o, const void *buf, size_t len, uint64_t off);
int pageobj_truncate(struct pageobj *o, uint64_t size);
/* The page holding page index `idx`, allocated if it is a hole, with a new
 * reference for the caller (a page-table entry). 0 if `idx` lies wholly
 * past the end of the object, or memory ran out, or (with a source) it hasn't
 * been read yet: pageobj_missing says which, pageobj_fill reads it. */
uint64_t pageobj_map_page(struct pageobj *o, uint64_t idx);
/* The same, and if it is 0 whether that is because the page is the source's to read first: one look, under the
 * lock (two looks would miss a page read by another in between, and take it for no memory). */
uint64_t pageobj_map_page_or_missing(struct pageobj *o, uint64_t idx, bool *missing);
struct pageobj *pageobj_adopt(struct pageobj *o, uint64_t phys, uint64_t size); /* boot memory as (part of) a file */
/* Gives `o` (no pages yet, its size set) its source. */
void pageobj_set_source(struct pageobj *o, const struct pagesrc *src, void *ctx, uint64_t off);
/* True if page `idx` is the source's to read before it can be used. */
bool pageobj_missing(struct pageobj *o, uint64_t idx);
/* The first page from `from` on that the source still has to read, or ~0 (none, or no source). */
uint64_t pageobj_next_missing(struct pageobj *o, uint64_t from);
/* Reads page `idx` from the source, with the pages after it (at least `want` in all, if the source allows):
 * 0 (also when there was nothing to read) or a negative errno. May wait; no spinlock may be held. */
int pageobj_fill(struct pageobj *o, uint64_t idx, uint64_t want);
/* The source itself was just written (`len` bytes at `off`): the pages present take the bytes too. */
void pageobj_update(struct pageobj *o, const void *buf, size_t len, uint64_t off);
/* Ends the source: the pages below `keep` bytes are read in first (the rest are going anyway), and from then on
 * the object is an ordinary one. 0 or a negative errno. */
int pageobj_detach(struct pageobj *o, uint64_t keep);
/* Drops up to `want` pages that are only kept because they were read once (nothing maps them; their source
 * still has them). Returns how many went. Never under a spinlock. */
uint64_t pageobj_reclaim(uint64_t want);
uint64_t pageobj_cached_pages(void); /* pages objects with a source hold: what reclaim could free, at most */
void pageobj_keeper_start(void);     /* the thread that frees them when memory runs short, and ends sources */

/* Generic page mapping flags; <asm/pgtable.h> turns them into entries.
 * Leaf mappings are always present and readable. */
#define PG_WRITE    (1ULL << 0)
#define PG_USER     (1ULL << 1)
#define PG_EXEC     (1ULL << 2)
#define PG_GLOBAL   (1ULL << 3)
#define PG_UNCACHED (1ULL << 4)
#define PG_WC       (1ULL << 5)
#define PG_COW      (1ULL << 6) /* shared since fork; a write copies it */

#include <asm/pgtable.h>

/* We store the entry for a page or table we just filled, and another CPU's
 * table walk must not see the entry before the contents. */
static inline void pte_publish(uint64_t *slot, uint64_t e) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *slot = e;
}

/* Builds the kernel's own page tables (kernel image with W^X permissions +
 * direct map of physical memory) and switches to them. */
void vmm_init(void);
bool vmm_map(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
bool vmm_map_huge(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_translate(uint64_t *pml4, uint64_t virt);
uint64_t *vmm_kernel_pml4(void);
uint64_t vmm_unmap_page(uint64_t *pml4, uint64_t virt); /* (4 KiB mappings; TLBs are the caller's) */
/* Makes [phys, phys+len) reachable through the direct map (for firmware
 * tables and device registers outside the memory map). Pages already mapped
 * are left alone. `uncached` selects strong-uncacheable for device MMIO. */
void vmm_map_mmio(uint64_t phys, uint64_t len, bool uncached);
void *vmm_ioremap(uint64_t phys, uint64_t len); /* uncached, at an address of its own; kept for good */

/* Kernel heap. */
void heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void kfree(void *p);
size_t kmalloc_size(const void *p); /* what the block can hold (at least what was asked for) */
