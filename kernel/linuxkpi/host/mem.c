#include <asm/cpu.h>
#include <asm/irq.h>
#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

#include "../host.h"

/* The layer's memory on our kernel: an address region of its own for vmalloc, vmap and ioremap (each mapping with
 * its own caching, 4 KiB pages, a guard page after each), programs' address spaces and the driver's mappings in
 * them (mm/uvm.c: VMA_DEV). */

#define AREA_BASE 0xffffc90000000000ULL /* (where Linux keeps vmalloc: far above any direct-mapped memory) */
#define AREA_SIZE (32ULL << 40)

struct area {
    uint64_t start, pages; /* pages mapped (the guard page after them is not) */
    void *owner;
    bool dying;
    struct area *next; /* sorted by start */
};

static spinlock_t area_lock = SPINLOCK_INIT;
static struct area *areas;

static uint64_t cache_flags(int cache) { return cache == 0 ? PG_UNCACHED : cache == 1 ? PG_WC : 0; }

/* a free range of `pages` (and its guard page): linked in, or NULL. Lock held. */
static struct area *area_new(uint64_t pages, void *owner) {
    struct area *a = kzalloc(sizeof *a);
    if (!a)
        return NULL;
    uint64_t want = (pages + 1) * PAGE_SIZE, at = AREA_BASE;
    struct area **pp = &areas;
    for (; *pp; pp = &(*pp)->next) {
        if ((*pp)->start - at >= want)
            break;
        at = (*pp)->start + ((*pp)->pages + 1) * PAGE_SIZE;
    }
    if (at + want > AREA_BASE + AREA_SIZE) {
        kfree(a);
        return NULL;
    }
    a->start = at, a->pages = pages, a->owner = owner;
    a->next = *pp;
    *pp = a;
    return a;
}

static void flush_tlb_here(void *arg) {
    (void)arg;
    arch_tlb_flush_all(); /* (the area's mappings are not global: a full flush drops them) */
}

/* unmaps a's pages on every CPU, then gives its range back */
static void area_free(struct area *a) {
    for (uint64_t i = 0; i < a->pages; i++)
        vmm_unmap_page(vmm_kernel_pml4(), a->start + i * PAGE_SIZE);
    smp_call_all(flush_tlb_here, NULL);
    uint64_t f = spin_lock_irqsave(&area_lock);
    for (struct area **pp = &areas; *pp; pp = &(*pp)->next)
        if (*pp == a) {
            *pp = a->next;
            break;
        }
    spin_unlock_irqrestore(&area_lock, f);
    kfree(a);
}

static void *area_map(const uint64_t *phys, uint64_t contiguous, size_t pages, int cache, void *owner) {
    if (!pages)
        return NULL;
    uint64_t f = spin_lock_irqsave(&area_lock);
    struct area *a = area_new(pages, owner);
    bool ok = a != NULL;
    for (size_t i = 0; ok && i < pages; i++) {
        uint64_t p = phys ? phys[i] : contiguous + i * PAGE_SIZE;
        ok = vmm_map(vmm_kernel_pml4(), a->start + i * PAGE_SIZE, p, PG_WRITE | cache_flags(cache));
    }
    if (a && !ok)
        a->dying = true;
    spin_unlock_irqrestore(&area_lock, f);
    if (a && !ok) {
        area_free(a);
        return NULL;
    }
    return a ? (void *)a->start : NULL;
}

void *kpi_host_vmap(const uint64_t *phys, size_t pages, int cache, void *owner) {
    return area_map(phys, 0, pages, cache, owner);
}

void *kpi_host_ioremap(uint64_t phys, size_t len, int cache) {
    uint64_t base = ALIGN_DOWN(phys, PAGE_SIZE);
    size_t pages = (ALIGN_UP(phys + len, PAGE_SIZE) - base) / PAGE_SIZE;
    char *v = area_map(NULL, base, pages, cache, NULL);
    return v ? v + (phys - base) : NULL;
}

void *kpi_host_vunmap(const void *v) {
    uint64_t f = spin_lock_irqsave(&area_lock);
    struct area *a = areas;
    while (a && !((uint64_t)v >= a->start && (uint64_t)v < a->start + a->pages * PAGE_SIZE))
        a = a->next;
    bool mine = a && !a->dying;
    if (mine)
        a->dying = true;
    spin_unlock_irqrestore(&area_lock, f);
    if (!mine)
        return NULL;
    void *owner = a->owner;
    area_free(a);
    return owner;
}

bool kpi_host_vmapped(const void *v, void **start, size_t *pages, void **owner) {
    uint64_t f = spin_lock_irqsave(&area_lock);
    struct area *a = areas;
    while (a && !((uint64_t)v >= a->start && (uint64_t)v < a->start + a->pages * PAGE_SIZE))
        a = a->next;
    if (a) {
        if (start)
            *start = (void *)a->start;
        if (pages)
            *pages = a->pages;
        if (owner)
            *owner = a->owner;
    }
    spin_unlock_irqrestore(&area_lock, f);
    return a != NULL;
}

bool kpi_host_in_vmap_area(const void *v) { return (uint64_t)v >= AREA_BASE && (uint64_t)v < AREA_BASE + AREA_SIZE; }

void kpi_host_flush_cache(const void *v, size_t len) {
    for (uint64_t p = ALIGN_DOWN((uint64_t)v, 64); p < (uint64_t)v + len; p += 64)
        __asm__ __volatile__("clflush (%0)" ::"r"(p) : "memory");
    __asm__ __volatile__("mfence" ::: "memory");
}

void kpi_host_page_ref(uint64_t phys) { page_ref(phys); }
void kpi_host_page_unref(uint64_t phys) { page_unref(phys); }

/* ---- programs' address spaces ---- */

kpi_host_mm *kpi_host_mm_current(void) {
    struct thread *t = thread_current();
    return t && t->user && t->user->proc ? (kpi_host_mm *)t->user->proc->mm : NULL;
}

void **kpi_host_mm_slot(kpi_host_mm *mm) { return &((struct mm *)mm)->kpi; }
void kpi_host_on_mm_gone(void (*fn)(void *slot)) { kpi_mm_gone = fn; }

bool kpi_host_vma_find(kpi_host_mm *mm, uint64_t addr, uint64_t *start, uint64_t *end, int *prot, bool *io, void **rec) {
    uint32_t p, flags;
    void *priv;
    if (!mm_vma_info((struct mm *)mm, addr, start, end, &p, &flags, &priv))
        return false;
    *prot = (int)p;
    *io = flags & (VMA_PHYS | VMA_DEV);
    *rec = priv;
    return true;
}

static const struct kpi_host_devmap_ops *dev_ops;

static void dev_get(void *rec) { dev_ops->get(rec); }
static void dev_put(void *rec) { dev_ops->put(rec); }
static void *dev_fork(void *rec) { return dev_ops->fork(rec); }
static int dev_fault(void *rec, struct mm *mm, uint64_t va, bool write) { return dev_ops->fault(rec, (kpi_host_mm *)mm, va, write); }
static const struct vma_dev_ops kernel_dev_ops = {dev_get, dev_put, dev_fork, dev_fault};

void kpi_host_devmap_init(const struct kpi_host_devmap_ops *ops) { dev_ops = ops; }

int kpi_host_dev_attach(kpi_host_mm *mm, uint64_t start, void *rec) {
    return mm_dev_attach((struct mm *)mm, start, &kernel_dev_ops, rec);
}

int kpi_host_dev_insert(kpi_host_mm *mm, uint64_t va, uint64_t phys, uint64_t len, int cache, bool write) {
    return mm_dev_insert((struct mm *)mm, va, phys, len, cache, write);
}

void kpi_host_dev_zap(kpi_host_mm *mm, uint64_t va, uint64_t len) { mm_dev_zap((struct mm *)mm, va, len); }

int kpi_host_dev_lookup(kpi_host_mm *mm, uint64_t va, uint64_t *phys, bool *writable) {
    return mm_dev_lookup((struct mm *)mm, va, phys, writable);
}

int kpi_host_pin(kpi_host_mm *mm, uint64_t va, bool write, uint64_t *phys) { return mm_pin_page((struct mm *)mm, va, write, phys); }
