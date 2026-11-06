#include <lkpi/all.h>
#include "../host.h"

/* kernel/linuxkpi: memory. The heap (kmalloc: our kernel's, so kfree is one function for both), vmalloc and vmap
 * in the layer's own address region (host/mem.c), struct page records, caching, programs' address spaces as Linux
 * shows them (mm_struct, vm_area_struct), the driver's mappings into programs, and programs' pages pinned for the
 * device. */

#define PROT_READ_ 1
#define PROT_WRITE_ 2

unsigned long lkpi_page_offset;
unsigned long max_pfn, num_physpages;
void *high_memory;

/* ---- the heap ---- */

void *lkpi_kmalloc(size_t size, gfp_t gfp) { return kpi_host_alloc(size); /* (zeroed, as it happens) */ }
void *lkpi_kzalloc(size_t size, gfp_t gfp) { return kpi_host_alloc(size); }

void *kcalloc(size_t n, size_t size, gfp_t gfp) {
    if (size && n > SIZE_MAX / size)
        return NULL;
    return kpi_host_alloc(n * size);
}

void *kmalloc_array(size_t n, size_t size, gfp_t gfp) { return kcalloc(n, size, gfp); }
size_t ksize(const void *p) { return p ? kpi_host_alloc_size(p) : 0; }

void *krealloc(const void *p, size_t size, gfp_t gfp) {
    if (!p)
        return kpi_host_alloc(size);
    if (!size) {
        kfree(p);
        return NULL;
    }
    size_t had = kpi_host_alloc_size(p);
    if (size <= had)
        return (void *)p;
    void *n = kpi_host_alloc(size);
    if (n) {
        memcpy(n, p, had);
        kfree(p);
    }
    return n;
}

void kfree_sensitive(const void *p) {
    if (!p)
        return;
    memset((void *)p, 0, kpi_host_alloc_size(p));
    kfree(p);
}

/* vmalloc: pages one by one, mapped together in the layer's region */
struct vm_pages { size_t n; u64 phys[]; };

static void *vmalloc_pages(unsigned long size, int cache) {
    size_t n = PAGE_ALIGN(size) / PAGE_SIZE;
    if (!n)
        n = 1;
    struct vm_pages *r = kpi_host_alloc(sizeof *r + n * sizeof(u64));
    if (!r)
        return NULL;
    for (r->n = 0; r->n < n; r->n++) {
        r->phys[r->n] = kpi_host_pages(1);
        if (!r->phys[r->n])
            goto fail;
        memset(kpi_host_phys_to_virt(r->phys[r->n]), 0, PAGE_SIZE);
    }
    void *v = kpi_host_vmap(r->phys, n, cache, r);
    if (v)
        return v;
fail:
    for (size_t i = 0; i < r->n; i++)
        kpi_host_pages_free(r->phys[i], 1);
    kfree(r);
    return NULL;
}

void *vmalloc(unsigned long size) { return vmalloc_pages(size, 2); }
void *vzalloc(unsigned long size) { return vmalloc_pages(size, 2); }
void *vmalloc_noprof(unsigned long size) { return vmalloc_pages(size, 2); }
void *vzalloc_noprof(unsigned long size) { return vmalloc_pages(size, 2); }
void *__vmalloc(unsigned long size, gfp_t gfp) { return vmalloc_pages(size, 2); }
bool is_vmalloc_addr(const void *p) { return kpi_host_in_vmap_area(p); }

void vfree(const void *p) {
    if (!p)
        return;
    struct vm_pages *r = kpi_host_vunmap(p);
    if (!r)
        return; /* (a vmap of someone's pages, or nothing: not ours to free) */
    for (size_t i = 0; i < r->n; i++)
        kpi_host_pages_free(r->phys[i], 1);
    kfree(r);
}

void *kvmalloc(size_t size, gfp_t gfp) { return size <= 64 * 1024 ? kpi_host_alloc(size) : vmalloc(size); }
void *kvzalloc(size_t size, gfp_t gfp) { return kvmalloc(size, gfp); }

void kvfree(const void *p) {
    if (is_vmalloc_addr(p))
        vfree(p);
    else
        kfree(p);
}

/* object caches: the heap, each object its size (the heap's blocks are aligned to 16) */
struct kmem_cache { unsigned int size; void (*ctor)(void *); char name[32]; };

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align, unsigned long flags, void (*ctor)(void *)) {
    struct kmem_cache *c = kpi_host_alloc(sizeof *c);
    if (!c)
        return NULL;
    c->size = align > 16 ? ALIGN(size, align) : size;
    c->ctor = ctor;
    strscpy(c->name, name ? name : "?", sizeof c->name);
    return c;
}

void *kmem_cache_alloc(struct kmem_cache *c, gfp_t gfp) {
    void *p = kpi_host_alloc(c->size);
    if (p && c->ctor)
        c->ctor(p);
    return p;
}

void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t gfp) { return kpi_host_alloc(c->size); }
void kmem_cache_free(struct kmem_cache *c, void *p) { kfree(p); }
void kmem_cache_destroy(struct kmem_cache *c) { kfree(c); }

/* ---- struct page: the layer's record of a physical page, found by its number ---- */

#define PGF_OURS 1   /* from alloc_pages: freed by __free_pages */
#define PGF_PINNED 2 /* a program's page held for the device (private: how many times) */
#define PGF_SEEN 4   /* looked up (pfn_to_page and friends): kept */

struct pnode { unsigned long pfn; struct page *page; struct pnode *next; };
#define PBUCKETS 4096
static struct pnode *pbuckets[PBUCKETS];
static DEFINE_SPINLOCK(pages_lock);

static unsigned pbucket(unsigned long pfn) { return (unsigned)((pfn * 0x9e3779b97f4a7c15ULL) >> 52) % PBUCKETS; }

static struct page *page_find_locked(unsigned long pfn) {
    for (struct pnode *n = pbuckets[pbucket(pfn)]; n; n = n->next)
        if (n->pfn == pfn)
            return n->page;
    return NULL;
}

static bool page_add_locked(struct page *p) {
    struct pnode *n = kpi_host_alloc(sizeof *n);
    if (!n)
        return false;
    n->pfn = p->pfn, n->page = p;
    n->next = pbuckets[pbucket(p->pfn)];
    pbuckets[pbucket(p->pfn)] = n;
    return true;
}

static void page_remove_locked(unsigned long pfn) {
    for (struct pnode **pp = &pbuckets[pbucket(pfn)]; *pp; pp = &(*pp)->next)
        if ((*pp)->pfn == pfn) {
            struct pnode *n = *pp;
            *pp = n->next;
            kfree(n);
            return;
        }
}

static void page_init(struct page *p, unsigned long pfn, unsigned long flags) {
    p->pfn = pfn;
    p->flags = flags;
    atomic_set(&p->_refcount, 1);
    atomic_set(&p->_mapcount, -1);
    INIT_LIST_HEAD(&p->lru);
    p->virtual = pfn < max_pfn ? kpi_host_phys_to_virt((u64)pfn << PAGE_SHIFT) : NULL;
}

/* the record of `pfn`, made (and kept, `flags` added) when there is none */
static struct page *page_get(unsigned long pfn, unsigned long flags) {
    spin_lock(&pages_lock);
    struct page *p = page_find_locked(pfn);
    if (!p) {
        p = kpi_host_alloc(sizeof *p);
        if (p) {
            page_init(p, pfn, 0);
            if (!page_add_locked(p)) {
                kfree(p);
                p = NULL;
            }
        }
    }
    if (p)
        p->flags |= flags;
    spin_unlock(&pages_lock);
    return p;
}

struct page *alloc_pages(gfp_t gfp, unsigned int order) {
    size_t n = 1UL << order;
    u64 phys = kpi_host_pages(n);
    if (!phys)
        return NULL;
    if ((gfp & (__GFP_DMA32 | __GFP_DMA)) && phys + n * PAGE_SIZE > (1ULL << 32)) {
        /* (our page allocator takes no limit yet: the driver falls back when told no) */
        kpi_host_pages_free(phys, n);
        return NULL;
    }
    if (gfp & __GFP_ZERO)
        memset(kpi_host_phys_to_virt(phys), 0, n * PAGE_SIZE);
    struct page *pages = kpi_host_alloc(n * sizeof *pages);
    if (!pages) {
        kpi_host_pages_free(phys, n);
        return NULL;
    }
    spin_lock(&pages_lock);
    for (size_t i = 0; i < n; i++) {
        page_init(&pages[i], (unsigned long)(phys >> PAGE_SHIFT) + i, PGF_OURS);
        pages[i].private = i ? 0 : order;
        page_remove_locked(pages[i].pfn); /* (a record left from when it was someone else's) */
        page_add_locked(&pages[i]);
    }
    spin_unlock(&pages_lock);
    return pages;
}

void __free_pages(struct page *p, unsigned int order) {
    if (!p)
        return;
    size_t n = 1UL << order;
    u64 phys = (u64)p->pfn << PAGE_SHIFT;
    spin_lock(&pages_lock);
    for (size_t i = 0; i < n; i++)
        page_remove_locked(p[i].pfn);
    spin_unlock(&pages_lock);
    kpi_host_pages_free(phys, n);
    kfree(p);
}

unsigned long __get_free_pages(gfp_t gfp, unsigned int order) {
    struct page *p = alloc_pages(gfp, order);
    return p ? (unsigned long)page_address(p) : 0;
}

unsigned long get_zeroed_page(gfp_t gfp) { return __get_free_pages(gfp | __GFP_ZERO, 0); }

void free_pages(unsigned long addr, unsigned int order) {
    if (!addr)
        return;
    spin_lock(&pages_lock);
    struct page *p = page_find_locked((unsigned long)(kpi_host_virt_to_phys((void *)addr) >> PAGE_SHIFT));
    spin_unlock(&pages_lock);
    if (p && (p->flags & PGF_OURS))
        __free_pages(p, order);
}

struct page *pfn_to_page(unsigned long pfn) { return page_get(pfn, PGF_SEEN); }
unsigned long page_to_pfn(const struct page *p) { return p->pfn; }
phys_addr_t page_to_phys(const struct page *p) { return (phys_addr_t)p->pfn << PAGE_SHIFT; }

void *page_address(const struct page *p) {
    return p->virtual ? p->virtual : kpi_host_phys_to_virt((u64)p->pfn << PAGE_SHIFT);
}

void *lkpi_phys_to_virt(phys_addr_t p) { return kpi_host_phys_to_virt(p); }
phys_addr_t lkpi_virt_to_phys(const void *v) { return kpi_host_translate(v); }
bool lkpi_pfn_valid(unsigned long pfn) { return pfn < max_pfn; }

bool virt_addr_valid(const void *v) {
    unsigned long a = (unsigned long)v;
    return a >= lkpi_page_offset && a < lkpi_page_offset + (max_pfn << PAGE_SHIFT);
}

struct page *virt_to_page(const void *v) {
    u64 phys = kpi_host_translate(v);
    return phys ? page_get((unsigned long)(phys >> PAGE_SHIFT), PGF_SEEN) : NULL;
}

struct page *vmalloc_to_page(const void *v) { return virt_to_page(v); }
unsigned long vmalloc_to_pfn(const void *v) { return (unsigned long)(kpi_host_translate(v) >> PAGE_SHIFT); }

static void unpin_one(struct page *p) {
    kpi_host_page_unref((u64)p->pfn << PAGE_SHIFT);
    spin_lock(&pages_lock);
    bool last = p->private > 0 && !--p->private;
    bool drop = last && p->flags == PGF_PINNED; /* (nothing else knows it: the record goes too) */
    if (last)
        p->flags &= ~PGF_PINNED;
    if (drop)
        page_remove_locked(p->pfn);
    spin_unlock(&pages_lock);
    if (drop)
        kfree(p);
}

void put_page(struct page *p) {
    if (p->flags & PGF_PINNED)
        unpin_one(p);
    else
        atomic_dec(&p->_refcount);
}

int set_page_dirty(struct page *p) { return 1; }
int set_page_dirty_lock(struct page *p) { return 1; }
void *kmap(struct page *p) { return page_address(p); }
void kunmap(struct page *p) {}
void *kmap_atomic(struct page *p) { return page_address(p); }
void kunmap_atomic(void *v) {}
void clear_page(void *p) { memset(p, 0, PAGE_SIZE); }
void copy_page(void *d, void *s) { memcpy(d, s, PAGE_SIZE); }

/* ---- caching ---- */

/* (with x86's PAT as Linux sets it up: PWT alone is write-combining, PCD uncached-minus, both uncached) */
static int cache_of(pgprot_t p) {
    u64 c = pgprot_val(p) & (_PAGE_PWT | _PAGE_PCD);
    return c == 0 ? 2 : c == _PAGE_PWT ? 1 : 0;
}

pgprot_t pgprot_noncached(pgprot_t p) { return __pgprot((pgprot_val(p) & ~_PAGE_CACHE_MASK) | _PAGE_PCD | _PAGE_PWT); }
pgprot_t pgprot_writecombine(pgprot_t p) { return __pgprot((pgprot_val(p) & ~_PAGE_CACHE_MASK) | _PAGE_PWT); }
pgprot_t pgprot_decrypted_lkpi(pgprot_t p) { return p; }

unsigned long cachemode2protval(enum page_cache_mode m) {
    switch (m) {
    case _PAGE_CACHE_MODE_WC: return _PAGE_PWT;
    case _PAGE_CACHE_MODE_UC_MINUS: return _PAGE_PCD;
    case _PAGE_CACHE_MODE_UC: return _PAGE_PCD | _PAGE_PWT;
    default: return 0;
    }
}

/* The direct map keeps its caching (it is in 2 MiB pages); what matters is that no dirty line of it is left to be
 * written over what a device and an uncached or write-combining mapping of the same page put there later: the
 * pages are written back and dropped from the caches instead. */
int set_memory_uc(unsigned long addr, int pages) {
    kpi_host_flush_cache((void *)addr, (size_t)pages * PAGE_SIZE);
    return 0;
}

int set_memory_wc(unsigned long addr, int pages) { return set_memory_uc(addr, pages); }
int set_memory_wb(unsigned long addr, int pages) { return 0; }
int set_pages_uc(struct page *p, int n) { return set_memory_uc((unsigned long)page_address(p), n); }
int set_pages_wb(struct page *p, int n) { return 0; }

int set_pages_array_uc(struct page **p, int n) {
    for (int i = 0; i < n; i++)
        kpi_host_flush_cache(page_address(p[i]), PAGE_SIZE);
    return 0;
}

int set_pages_array_wc(struct page **p, int n) { return set_pages_array_uc(p, n); }
int set_pages_array_wb(struct page **p, int n) { return 0; }

void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot) {
    u64 *phys = kpi_host_alloc(count * sizeof *phys);
    if (!phys)
        return NULL;
    for (unsigned i = 0; i < count; i++)
        phys[i] = (u64)pages[i]->pfn << PAGE_SHIFT;
    void *v = kpi_host_vmap(phys, count, cache_of(prot), NULL);
    kfree(phys);
    return v;
}

void *vmap_pfn(unsigned long *pfns, unsigned int count, unsigned long prot) {
    u64 *phys = kpi_host_alloc(count * sizeof *phys);
    if (!phys)
        return NULL;
    for (unsigned i = 0; i < count; i++)
        phys[i] = (u64)pfns[i] << PAGE_SHIFT;
    void *v = kpi_host_vmap(phys, count, cache_of(__pgprot(prot)), NULL);
    kfree(phys);
    return v;
}

void vunmap(const void *v) {
    if (v)
        (void)kpi_host_vunmap(v);
}

void *ioremap(phys_addr_t addr, size_t size) { return kpi_host_ioremap(addr, size, 0); }
void *ioremap_uc(phys_addr_t addr, size_t size) { return kpi_host_ioremap(addr, size, 0); }
void *ioremap_wc(phys_addr_t addr, size_t size) { return kpi_host_ioremap(addr, size, 1); }
void *ioremap_cache(phys_addr_t addr, size_t size) { return kpi_host_ioremap(addr, size, 2); }

void iounmap(volatile void *p) {
    if (p)
        (void)kpi_host_vunmap((const void *)p);
}

void *memremap(resource_size_t addr, size_t size, unsigned long flags) {
    if ((flags & MEMREMAP_WB) && addr + size <= ((u64)max_pfn << PAGE_SHIFT))
        return kpi_host_phys_to_virt(addr); /* (memory: the direct map has it, cached) */
    return kpi_host_ioremap(addr, size, (flags & MEMREMAP_WC) ? 1 : 2);
}

void memunmap(void *p) {
    if (p && is_vmalloc_addr(p))
        (void)kpi_host_vunmap(p);
}

bool arch_phys_wc_add_lkpi(void) { return false; }
int arch_phys_wc_add(unsigned long base, unsigned long size) { return 0; }
void arch_phys_wc_del(int handle) {}
int arch_io_reserve_memtype_wc(resource_size_t start, resource_size_t size) { return 0; }
void arch_io_free_memtype_wc(resource_size_t start, resource_size_t size) {}
bool pat_enabled(void) { return true; }

/* ---- how much memory ---- */

unsigned long totalram_pages(void) { return (unsigned long)kpi_host_total_pages(); }
unsigned long get_num_physpages(void) { return (unsigned long)kpi_host_total_pages(); }

void si_meminfo(struct sysinfo *s) {
    memset(s, 0, sizeof *s);
    s->totalram = (unsigned long)kpi_host_total_pages();
    s->freeram = (unsigned long)kpi_host_free_pages();
    s->mem_unit = PAGE_SIZE;
}

static struct pglist_data node0;

struct pglist_data *lkpi_node_data(int nid) { return &node0; }
void mod_node_page_state(struct pglist_data *pgdat, int item, long delta) {}
int register_memory_notifier_lkpi(void) { return 0; }

void lkpi_mm_init(void) {
    lkpi_page_offset = (unsigned long)kpi_host_direct_map_base();
    max_pfn = (unsigned long)(kpi_host_max_phys() >> PAGE_SHIFT);
    num_physpages = (unsigned long)kpi_host_total_pages();
    high_memory = (void *)(lkpi_page_offset + (max_pfn << PAGE_SHIFT));
    node0.nr_zones = 1;
    node0.node_present_pages = node0.node_spanned_pages = num_physpages;
    node0.node_zones[0] = (struct zone){0, max_pfn, num_physpages, "Normal"};
}

/* ---- programs' address spaces ---- */

struct lkpi_mm {
    struct mm_struct mm;
    struct vm_area_struct scratch; /* what find_vma answers for a mapping that is not the layer's */
    bool freeing;
};

static DEFINE_SPINLOCK(mms_lock);

/* (freed once: no one holds it, and our kernel's address space is gone) */
static void mm_free_if_unused(struct mm_struct *mm) {
    struct lkpi_mm *m = container_of(mm, struct lkpi_mm, mm);
    spin_lock(&mms_lock);
    bool last = atomic_read(&mm->mm_count) <= 0 && !mm->lkpi_proc && !m->freeing;
    if (last)
        m->freeing = true;
    spin_unlock(&mms_lock);
    if (last)
        kfree(m);
}

/* our kernel's address space ended: its view stays for whoever still holds it, knowing it is gone */
static void mm_gone(void *slot) {
    struct mm_struct *mm = slot;
    spin_lock(&mms_lock);
    mm->lkpi_proc = NULL;
    spin_unlock(&mms_lock);
    mm_free_if_unused(mm);
}

static kpi_host_mm *host_mm(struct mm_struct *mm) { return mm ? mm->lkpi_proc : NULL; }

/* the current program's (a counted reference: the caller's, mmdrop'd), NULL on a kernel thread */
struct mm_struct *lkpi_mm_current(void) {
    kpi_host_mm *h = kpi_host_mm_current();
    if (!h)
        return NULL;
    static bool hooked;
    spin_lock(&mms_lock);
    if (!hooked)
        kpi_host_on_mm_gone(mm_gone), hooked = true;
    void **slot = kpi_host_mm_slot(h);
    struct mm_struct *mm = *slot;
    if (!mm) {
        struct lkpi_mm *m = kpi_host_alloc(sizeof *m);
        if (m) {
            mm = &m->mm;
            init_rwsem(&mm->mmap_lock);
            mm->lkpi_proc = h;
            *slot = mm;
        }
    }
    if (mm)
        atomic_inc(&mm->mm_count);
    spin_unlock(&mms_lock);
    return mm;
}

void mmgrab(struct mm_struct *mm) { atomic_inc(&mm->mm_count); }
void mmget(struct mm_struct *mm) { atomic_inc(&mm->mm_count); }

void mmdrop(struct mm_struct *mm) {
    if (mm && atomic_dec_and_test(&mm->mm_count))
        mm_free_if_unused(mm);
}

void mmput(struct mm_struct *mm) { mmdrop(mm); }
bool mmget_not_zero(struct mm_struct *mm) { return mm && mm->lkpi_proc && atomic_inc_not_zero(&mm->mm_count); }

struct mm_struct *get_task_mm(struct task_struct *t) {
    struct mm_struct *mm = t->mm;
    if (mm && !mmget_not_zero(mm))
        mm = NULL;
    return mm;
}

void add_mm_counter(struct mm_struct *mm, int member, long value) { mm->rss_stat[member].count += value; }
void mmap_read_lock(struct mm_struct *mm) { down_read(&mm->mmap_lock); }
void mmap_read_unlock(struct mm_struct *mm) { up_read(&mm->mmap_lock); }
int mmap_read_trylock(struct mm_struct *mm) { return down_read_trylock(&mm->mmap_lock); }
void mmap_write_lock(struct mm_struct *mm) { down_write(&mm->mmap_lock); }
void mmap_write_unlock(struct mm_struct *mm) { up_write(&mm->mmap_lock); }

/* ---- the driver's mappings into programs ---- */

struct lkpi_vma {
    struct vm_area_struct vma;
    int refs; /* the pieces of our kernel's mapping that hold it */
    struct list_head all;
};

static DEFINE_SPINLOCK(vmas_lock);
static LIST_HEAD(vmas);

/* (find_vma: a mapping the layer did not make, as Linux would describe it, in the mm's scratch record) */
static struct vm_area_struct *vma_describe(struct mm_struct *mm, unsigned long addr) {
    u64 start, end;
    int prot;
    bool io;
    void *rec;
    if (!host_mm(mm) || !kpi_host_vma_find(host_mm(mm), addr, &start, &end, &prot, &io, &rec))
        return NULL;
    if (rec)
        return &((struct lkpi_vma *)rec)->vma;
    struct vm_area_struct *v = &container_of(mm, struct lkpi_mm, mm)->scratch;
    memset(v, 0, sizeof *v);
    v->vm_start = (unsigned long)start, v->vm_end = (unsigned long)end;
    v->vm_flags = ((prot & PROT_READ_) ? VM_READ | VM_MAYREAD : 0) | ((prot & PROT_WRITE_) ? VM_WRITE | VM_MAYWRITE : 0) |
                  (io ? VM_IO | VM_PFNMAP : 0);
    v->vm_page_prot = PAGE_SHARED;
    v->vm_mm = mm;
    return v;
}

struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr) { return vma_describe(mm, addr); }

struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr) {
    struct vm_area_struct *v = vma_describe(mm, addr);
    return v && v->vm_start <= addr ? v : NULL;
}

struct vm_area_struct *find_vma_intersection(struct mm_struct *mm, unsigned long start, unsigned long end) {
    struct vm_area_struct *v = vma_describe(mm, start);
    return v && v->vm_start < end ? v : NULL;
}

void vm_flags_set(struct vm_area_struct *v, unsigned long f) { v->vm_flags |= f; }
void vm_flags_clear(struct vm_area_struct *v, unsigned long f) { v->vm_flags &= ~f; }

static int insert(struct vm_area_struct *v, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot) {
    if (addr < v->vm_start || addr + size > v->vm_end || !host_mm(v->vm_mm))
        return -EINVAL;
    return kpi_host_dev_insert(host_mm(v->vm_mm), addr, (u64)pfn << PAGE_SHIFT, size, cache_of(prot),
                               (v->vm_flags & VM_WRITE) != 0);
}

int remap_pfn_range(struct vm_area_struct *v, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot) {
    v->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
    return insert(v, addr, pfn, size, prot);
}

int io_remap_pfn_range(struct vm_area_struct *v, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot) {
    return remap_pfn_range(v, addr, pfn, size, prot);
}

static vm_fault_t fault_result(int r) { return r == 0 ? VM_FAULT_NOPAGE : r == -ENOMEM ? VM_FAULT_OOM : VM_FAULT_SIGBUS; }

vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *v, unsigned long addr, unsigned long pfn, pgprot_t prot) {
    return fault_result(insert(v, addr & PAGE_MASK, pfn, PAGE_SIZE, prot));
}

vm_fault_t vmf_insert_pfn(struct vm_area_struct *v, unsigned long addr, unsigned long pfn) {
    return vmf_insert_pfn_prot(v, addr, pfn, v->vm_page_prot);
}

vm_fault_t vmf_insert_mixed(struct vm_area_struct *v, unsigned long addr, unsigned long pfn) {
    return vmf_insert_pfn_prot(v, addr, pfn, v->vm_page_prot);
}

vm_fault_t vmf_insert_page(struct vm_area_struct *v, unsigned long addr, struct page *p) {
    return vmf_insert_pfn_prot(v, addr, p->pfn, v->vm_page_prot);
}

int vm_insert_page(struct vm_area_struct *v, unsigned long addr, struct page *p) {
    v->vm_flags |= VM_MIXEDMAP;
    return insert(v, addr & PAGE_MASK, p->pfn, PAGE_SIZE, v->vm_page_prot);
}

void zap_vma_ptes(struct vm_area_struct *v, unsigned long addr, unsigned long size) {
    if (host_mm(v->vm_mm))
        kpi_host_dev_zap(host_mm(v->vm_mm), addr, size);
}

/* every program's mapping of `m` in [start, start+len) of the file taken away (touching it faults to the driver) */
void unmap_mapping_range(struct address_space *m, loff_t start, loff_t len, int even_cows) {
    spin_lock(&vmas_lock);
    struct lkpi_vma *l;
    list_for_each_entry(l, &vmas, all) {
        struct vm_area_struct *v = &l->vma;
        if (!v->vm_file || v->vm_file->f_mapping != m || !host_mm(v->vm_mm))
            continue;
        u64 vs = (u64)v->vm_pgoff << PAGE_SHIFT, ve = vs + (v->vm_end - v->vm_start);
        u64 s = (u64)start, e = len ? (u64)start + (u64)len : ~0ULL;
        if (e <= vs || s >= ve)
            continue;
        u64 from = s > vs ? s : vs, to = e < ve ? e : ve;
        kpi_host_dev_zap(host_mm(v->vm_mm), v->vm_start + (from - vs), to - from);
    }
    spin_unlock(&vmas_lock);
}

static void dm_get(void *rec) { __atomic_add_fetch(&((struct lkpi_vma *)rec)->refs, 1, __ATOMIC_ACQ_REL); }

void fput(struct file *f);

static void dm_put(void *rec) {
    struct lkpi_vma *l = rec;
    if (__atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL))
        return;
    spin_lock(&vmas_lock);
    list_del(&l->all);
    spin_unlock(&vmas_lock);
    if (l->vma.vm_ops && l->vma.vm_ops->close)
        l->vma.vm_ops->close(&l->vma);
    if (l->vma.vm_file)
        fput(l->vma.vm_file);
    mmdrop(l->vma.vm_mm);
    kfree(l);
}

/* (a forked program does not get the driver's mappings: the GPU's memory stays the parent's, as CUDA asks) */
static void *dm_fork(void *rec) { return NULL; }

static int dm_fault(void *rec, kpi_host_mm *mm, u64 va, bool write) {
    struct lkpi_vma *l = rec;
    struct vm_area_struct *v = &l->vma;
    if (!v->vm_ops || !v->vm_ops->fault)
        return -EFAULT;
    struct vm_fault vmf = {.vma = v, .address = (unsigned long)va,
                           .pgoff = v->vm_pgoff + (((unsigned long)va - v->vm_start) >> PAGE_SHIFT),
                           .flags = write ? 1 /* FAULT_FLAG_WRITE */ : 0};
    vm_fault_t r = v->vm_ops->fault(&vmf);
    if (r & (VM_FAULT_SIGBUS | VM_FAULT_OOM))
        return -EFAULT;
    if (vmf.page && !(r & VM_FAULT_NOPAGE)) {
        int e = insert(v, (unsigned long)va & PAGE_MASK, vmf.page->pfn, PAGE_SIZE, v->vm_page_prot);
        put_page(vmf.page); /* (the fault handler's reference: the mapping holds none here) */
        return e;
    }
    return 0;
}

static const struct kpi_host_devmap_ops devmap_ops = {dm_get, dm_put, dm_fork, dm_fault};

void get_file_lkpi(struct file *f);

/* mmap of one of the layer's devices: [addr, addr+len) is reserved in the current program (our kernel's VMA_DEV);
 * the driver's mmap fills it in */
int lkpi_mmap(struct file *f, u64 addr, u64 len, int prot, u64 off) {
    static bool hooked;
    if (!hooked)
        kpi_host_devmap_init(&devmap_ops), hooked = true;
    if (!f->f_op || !f->f_op->mmap)
        return -ENODEV;
    struct lkpi_vma *l = kpi_host_alloc(sizeof *l);
    if (!l)
        return -ENOMEM;
    struct vm_area_struct *v = &l->vma;
    v->vm_start = (unsigned long)addr, v->vm_end = (unsigned long)(addr + len);
    v->vm_pgoff = (unsigned long)(off >> PAGE_SHIFT);
    v->vm_flags = VM_SHARED | VM_MAYSHARE | ((prot & PROT_READ_) ? VM_READ | VM_MAYREAD : 0) |
                  ((prot & PROT_WRITE_) ? VM_WRITE | VM_MAYWRITE : 0);
    v->vm_page_prot = PAGE_SHARED;
    v->vm_mm = lkpi_mm_current(); /* (the mapping's reference: dm_put lets it go) */
    v->vm_file = f;
    l->refs = 1;
    int r = f->f_op->mmap(f, v);
    bool mapped = r == 0;
    if (mapped)
        r = kpi_host_dev_attach(host_mm(v->vm_mm), addr, l);
    if (r) {
        if (mapped && v->vm_ops && v->vm_ops->close)
            v->vm_ops->close(v);
        mmdrop(v->vm_mm);
        kfree(l);
        return r;
    }
    get_file_lkpi(f);
    spin_lock(&vmas_lock);
    list_add(&l->all, &vmas);
    spin_unlock(&vmas_lock);
    return 0;
}

/* ---- programs' pages, for the device ---- */

static long pin_range(struct mm_struct *mm, unsigned long start, unsigned long n, unsigned int gup_flags, struct page **pages) {
    kpi_host_mm *h = mm ? host_mm(mm) : kpi_host_mm_current();
    if (!h)
        return -EFAULT;
    unsigned long i;
    for (i = 0; i < n; i++) {
        u64 phys;
        if (kpi_host_pin(h, (start & PAGE_MASK) + i * PAGE_SIZE, gup_flags & FOLL_WRITE, &phys) < 0)
            break;
        struct page *p = page_get((unsigned long)(phys >> PAGE_SHIFT), PGF_PINNED);
        if (!p) {
            kpi_host_page_unref(phys);
            break;
        }
        spin_lock(&pages_lock);
        p->private++;
        spin_unlock(&pages_lock);
        if (pages)
            pages[i] = p;
        else
            unpin_one(p); /* (only faulted in) */
    }
    return i ? (long)i : -EFAULT;
}

long pin_user_pages(unsigned long start, unsigned long n, unsigned int gup_flags, struct page **pages) {
    return pin_range(NULL, start, n, gup_flags, pages);
}

long pin_user_pages_remote(struct mm_struct *mm, unsigned long start, unsigned long n, unsigned int gup_flags,
                           struct page **pages, int *locked) {
    return pin_range(mm, start, n, gup_flags, pages);
}

long get_user_pages(unsigned long start, unsigned long n, unsigned int gup_flags, struct page **pages) {
    return pin_range(NULL, start, n, gup_flags, pages);
}

long get_user_pages_remote(struct mm_struct *mm, unsigned long start, unsigned long n, unsigned int gup_flags,
                           struct page **pages, int *locked) {
    return pin_range(mm, start, n, gup_flags, pages);
}

int get_user_pages_fast(unsigned long start, int n, unsigned int gup_flags, struct page **pages) {
    return (int)pin_range(NULL, start, (unsigned long)n, gup_flags, pages);
}

void unpin_user_page(struct page *p) { unpin_one(p); }

void unpin_user_pages(struct page **p, unsigned long n) {
    for (unsigned long i = 0; i < n; i++)
        unpin_one(p[i]);
}

void unpin_user_pages_dirty_lock(struct page **p, unsigned long n, bool dirty) { unpin_user_pages(p, n); }

vm_fault_t handle_mm_fault(struct vm_area_struct *v, unsigned long addr, unsigned int flags, void *regs) {
    struct page *p;
    if (pin_range(v->vm_mm, addr, 1, (flags & 1) ? FOLL_WRITE : 0, &p) != 1)
        return VM_FAULT_SIGBUS;
    unpin_one(p);
    return 0;
}

int follow_pfnmap_start(struct follow_pfnmap_args *a) {
    u64 phys;
    bool writable;
    if (!a->vma || !host_mm(a->vma->vm_mm) || kpi_host_dev_lookup(host_mm(a->vma->vm_mm), a->address, &phys, &writable))
        return -EINVAL;
    a->pfn = (unsigned long)(phys >> PAGE_SHIFT);
    a->writable = writable;
    a->special = true;
    a->pgprot = a->vma->vm_page_prot;
    return 0;
}

void follow_pfnmap_end(struct follow_pfnmap_args *a) {}

/* ---- copying to and from programs ---- */

/* A kernel buffer is taken for a program's only during a read or write of a device through our kernel (fs.c marks
 * those): never otherwise, so a pointer a program put in an ioctl never reaches the kernel's memory. */
#define PF_LKPI_KBUF 0x80000000u
#define KERNEL_HALF 0xffff800000000000ULL

static bool kernel_buffer(u64 p) { return p >= KERNEL_HALF && (current->flags & PF_LKPI_KBUF); }

unsigned long lkpi_copy_from_user(void *to, const void __user *from, unsigned long n) {
    if (kernel_buffer((u64)from)) {
        memcpy(to, (const void *)from, n);
        return 0;
    }
    return kpi_host_copy_from_user(to, (u64)from, n) ? 0 : n;
}

unsigned long lkpi_copy_to_user(void __user *to, const void *from, unsigned long n) {
    if (kernel_buffer((u64)to)) {
        memcpy((void *)to, from, n);
        return 0;
    }
    return kpi_host_copy_to_user((u64)to, from, n) ? 0 : n;
}

unsigned long clear_user(void __user *to, unsigned long n) {
    static const char zeros[256];
    for (unsigned long done = 0; done < n;) {
        unsigned long k = n - done < sizeof zeros ? n - done : sizeof zeros;
        if (!kpi_host_copy_to_user((u64)to + done, zeros, k))
            return n - done;
        done += k;
    }
    return 0;
}

bool lkpi_access_ok(const void *p, unsigned long n) { return kpi_host_user_range_ok((u64)p, n); }

long strncpy_from_user(char *d, const char __user *s, long n) {
    for (long i = 0; i < n; i++) {
        if (!kpi_host_copy_from_user(&d[i], (u64)s + (u64)i, 1))
            return -EFAULT;
        if (!d[i])
            return i;
    }
    return n;
}
