#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>

/* One bit per 4 KiB frame; 1 = used. The bitmap lives in the first usable
 * region large enough to hold it and is reached through the direct map. */

static uint64_t *bitmap;
static uint64_t total_frames; /* frames covered by the bitmap */
static uint64_t free_frames;
static uint64_t usable_frames;
static uint64_t next_hint;
/* Reference counts for pages that several owners hold at once (file pages
 * that are also mapped into address spaces). 0 = not reference counted:
 * ordinary pages have exactly one owner, who frees them with pmm_free. */
static uint32_t *page_refs;

static uint64_t alloc_locked(size_t pages);

static inline bool test_bit(uint64_t f) { return bitmap[f / 64] & (1ULL << (f % 64)); }
static inline void set_bit(uint64_t f) { bitmap[f / 64] |= 1ULL << (f % 64); }
static inline void clear_bit(uint64_t f) { bitmap[f / 64] &= ~(1ULL << (f % 64)); }

void pmm_init(void) {
    struct limine_memmap_response *mm = boot_memmap_request.response;
    KASSERT(mm != NULL);

    uint64_t top = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE || e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            if (e->base + e->length > top)
                top = e->base + e->length;
        }
    }
    total_frames = top / PAGE_SIZE;
    uint64_t bitmap_bytes = ALIGN_UP((total_frames + 63) / 64 * 8, PAGE_SIZE);

    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes && e->base >= PAGE_SIZE) {
            bitmap_phys = e->base;
            break;
        }
    }
    if (!bitmap_phys)
        panic("pmm: no region large enough for a %lu-byte frame bitmap", bitmap_bytes);

    bitmap = phys_to_virt(bitmap_phys);
    memset(bitmap, 0xff, bitmap_bytes);

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t first = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t last = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t f = first; f < last; f++) {
            clear_bit(f);
            free_frames++;
        }
    }
    usable_frames = free_frames;

    /* Never hand out frame 0 (0 is our "allocation failed" value), and keep
     * the bitmap's own frames reserved. */
    if (!test_bit(0)) {
        set_bit(0);
        free_frames--;
    }
    for (uint64_t f = bitmap_phys / PAGE_SIZE; f < (bitmap_phys + bitmap_bytes) / PAGE_SIZE; f++) {
        if (!test_bit(f)) {
            set_bit(f);
            free_frames--;
        }
    }

    size_t ref_pages = ALIGN_UP(total_frames * sizeof(uint32_t), PAGE_SIZE) / PAGE_SIZE;
    uint64_t refs_phys = alloc_locked(ref_pages);
    if (!refs_phys)
        panic("pmm: no room for %lu page reference counts", total_frames);
    page_refs = phys_to_virt(refs_phys);
    memset(page_refs, 0, ref_pages * PAGE_SIZE);
}

static spinlock_t pmm_lock = SPINLOCK_INIT;

uint64_t pmm_alloc(size_t pages) {
    uint64_t f = spin_lock_irqsave(&pmm_lock);
    uint64_t r = alloc_locked(pages);
    spin_unlock_irqrestore(&pmm_lock, f);
    return r;
}

static uint64_t alloc_locked(size_t pages) {
    if (pages == 0 || pages > free_frames)
        return 0;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? next_hint : 0;
        uint64_t end = pass == 0 ? total_frames : next_hint;
        uint64_t run = 0;
        for (uint64_t f = start; f < end; f++) {
            if (run == 0 && f % 64 == 0 && bitmap[f / 64] == ~0ULL) {
                f += 63; /* whole word used: skip it */
                continue;
            }
            if (test_bit(f)) {
                run = 0;
                continue;
            }
            if (++run == pages) {
                uint64_t first = f + 1 - pages;
                for (uint64_t g = first; g <= f; g++)
                    set_bit(g);
                free_frames -= pages;
                next_hint = f + 1;
                return first * PAGE_SIZE;
            }
        }
    }
    return 0;
}

void pmm_free(uint64_t phys, size_t pages) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    uint64_t first = phys / PAGE_SIZE;
    for (uint64_t f = first; f < first + pages; f++) {
        KASSERT(f < total_frames && test_bit(f));
        clear_bit(f);
    }
    free_frames += pages;
    if (first < next_hint)
        next_hint = first;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_free_pages(void) { return free_frames; }
uint64_t pmm_total_pages(void) { return usable_frames; }

uint64_t page_alloc_shared(void) {
    uint64_t p = pmm_alloc(1);
    if (p)
        __atomic_store_n(&page_refs[p / PAGE_SIZE], 1, __ATOMIC_RELAXED);
    return p;
}

void page_ref(uint64_t phys) {
    uint32_t old = __atomic_fetch_add(&page_refs[phys / PAGE_SIZE], 1, __ATOMIC_RELAXED);
    KASSERT(old != 0);
}

void page_unref(uint64_t phys) {
    uint32_t left = __atomic_sub_fetch(&page_refs[phys / PAGE_SIZE], 1, __ATOMIC_ACQ_REL);
    KASSERT(left != UINT32_MAX);
    if (left == 0)
        pmm_free(phys, 1);
}

/* Memory that was never the allocator's (boot modules): give it a count
 * that can't reach zero, so references to it come and go harmlessly. */
void page_pin(uint64_t phys) {
    uint32_t *r = &page_refs[phys / PAGE_SIZE];
    if (__atomic_load_n(r, __ATOMIC_RELAXED) == 0)
        __atomic_store_n(r, 2, __ATOMIC_RELAXED);
    else
        __atomic_add_fetch(r, 1, __ATOMIC_RELAXED);
}

uint32_t page_refcount(uint64_t phys) { return __atomic_load_n(&page_refs[phys / PAGE_SIZE], __ATOMIC_ACQUIRE); }
