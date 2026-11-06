#pragma once
/* kernel/linuxkpi: memory, as NVIDIA's glue sees it: allocation (kmalloc, vmalloc, pages), the direct map, struct
 * page, and a process's address space (mm_struct, vm_area_struct) as far as the driver maps memory into programs.
 * A struct page here is the layer's own record of one physical page, kept for pages the driver asked for or
 * looked up (kernel/linuxkpi/mm.c); its fields are only the ones the driver uses. */
#include <lkpi/core.h>

typedef struct { u64 pgprot; } pgprot_t; /* (a page's attributes in a mapping: see below) */
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x) ALIGN((x), PAGE_SIZE)
#define PAGE_ALIGNED(x) IS_ALIGNED((unsigned long)(x), PAGE_SIZE)
#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)
#define PFN_UP(x) (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x) ((x) >> PAGE_SHIFT)
#define PFN_PHYS(x) ((phys_addr_t)(x) << PAGE_SHIFT)
#define PHYS_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))
#define MAX_ORDER 10
#define MAX_PAGE_ORDER 10
#define get_order(size) ((int)((size) <= PAGE_SIZE ? 0 : fls64(((size) - 1) >> PAGE_SHIFT)))

/* allocation flags (only their meaning to us matters: may it sleep, must it be zeroed, below 4 GiB) */
#define __GFP_DMA 0x01u
#define __GFP_HIGHMEM 0x02u
#define __GFP_DMA32 0x04u
#define __GFP_RECLAIM 0x400u
#define __GFP_IO 0x40u
#define __GFP_FS 0x80u
#define __GFP_ZERO 0x100u
#define __GFP_NOWARN 0x200u
#define __GFP_RETRY_MAYFAIL 0x400u
#define __GFP_NORETRY 0x1000u
#define __GFP_NOFAIL 0x8000u
#define __GFP_COMP 0x4000u
#define __GFP_HIGH 0x20u
#define __GFP_DIRECT_RECLAIM 0x400u
#define __GFP_KSWAPD_RECLAIM 0x800u
#define __GFP_THISNODE 0x200000u
#define __GFP_ACCOUNT 0x400000u
#define __GFP_MOVABLE 0x08u
#define GFP_ATOMIC 0x20u
#define GFP_NOWAIT 0x800u
#define GFP_KERNEL 0xcc0u
#define GFP_NOIO 0x400u
#define GFP_NOFS 0x440u
#define GFP_USER 0xcc0u
#define GFP_HIGHUSER 0xcc2u
#define GFP_HIGHUSER_MOVABLE 0xccau
#define GFP_DMA __GFP_DMA
#define GFP_DMA32 __GFP_DMA32
#define GFP_KERNEL_ACCOUNT (GFP_KERNEL | __GFP_ACCOUNT)
#define gfpflags_allow_blocking(g) (((g) & __GFP_DIRECT_RECLAIM) != 0)

/* (our kernel has its own kmalloc, kzalloc and copy_to/from_user with other shapes: the layer's are renamed, so
 * the two never meet; NVIDIA's code calls these through the macros) */
#define kmalloc lkpi_kmalloc
#define kzalloc lkpi_kzalloc
#define copy_from_user lkpi_copy_from_user
#define copy_to_user lkpi_copy_to_user
#define strncpy_from_user lkpi_strncpy_from_user
void *kmalloc(size_t size, gfp_t gfp);
void *kzalloc(size_t size, gfp_t gfp);
void *kcalloc(size_t n, size_t size, gfp_t gfp);
void *kmalloc_array(size_t n, size_t size, gfp_t gfp);
void *krealloc(const void *p, size_t size, gfp_t gfp);
void kfree(const void *p);
void kfree_sensitive(const void *p);
#define kzfree kfree_sensitive
size_t ksize(const void *p);
#define kmalloc_node(s, g, n) kmalloc(s, g)
#define kzalloc_node(s, g, n) kzalloc(s, g)
#define kmalloc_noprof kmalloc
void *kvmalloc(size_t size, gfp_t gfp);
void *kvzalloc(size_t size, gfp_t gfp);
void kvfree(const void *p);
#define kvmalloc_array(n, s, g) kvmalloc((n) * (s), g)
#define kvcalloc(n, s, g) kvzalloc((n) * (s), g)
void *vmalloc(unsigned long size);
void *vzalloc(unsigned long size);
void *vmalloc_noprof(unsigned long size);
void *vzalloc_noprof(unsigned long size);
void *__vmalloc(unsigned long size, gfp_t gfp);
void vfree(const void *p);
bool is_vmalloc_addr(const void *p);
struct kmem_cache;
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align, unsigned long flags, void (*ctor)(void *));
#define kmem_cache_create_usercopy(n, s, a, f, uo, us, c) kmem_cache_create(n, s, a, f, c)
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t gfp);
void *kmem_cache_zalloc(struct kmem_cache *c, gfp_t gfp);
void kmem_cache_free(struct kmem_cache *c, void *p);
void kmem_cache_destroy(struct kmem_cache *c);
#define KMEM_CACHE(s, f) kmem_cache_create(#s, sizeof(struct s), __alignof__(struct s), (f), NULL)
#define SLAB_HWCACHE_ALIGN 0x2000UL
#define SLAB_ACCOUNT 0x4000000UL
#define SLAB_RECLAIM_ACCOUNT 0x20000UL
#define ZERO_SIZE_PTR ((void *)16)
#define ZERO_OR_NULL_PTR(x) ((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

/* pages */
struct address_space;
struct page {
    unsigned long flags;
    unsigned long pfn;       /* (the layer's: which physical page this is) */
    atomic_t _refcount;
    atomic_t _mapcount;
    unsigned long private;
    struct address_space *mapping;
    pgoff_t index;
    struct list_head lru;
    void *virtual;
};
struct folio { struct page page; };
struct page *alloc_pages(gfp_t gfp, unsigned int order);
#define alloc_page(gfp) alloc_pages(gfp, 0)
#define alloc_pages_node(n, g, o) alloc_pages(g, o)
void __free_pages(struct page *p, unsigned int order);
#define __free_page(p) __free_pages(p, 0)
unsigned long __get_free_pages(gfp_t gfp, unsigned int order);
unsigned long get_zeroed_page(gfp_t gfp);
#define __get_free_page(g) __get_free_pages(g, 0)
void free_pages(unsigned long addr, unsigned int order);
#define free_page(a) free_pages(a, 0)
struct page *pfn_to_page(unsigned long pfn);
unsigned long page_to_pfn(const struct page *p);
struct page *virt_to_page(const void *v);
struct page *vmalloc_to_page(const void *v);
unsigned long vmalloc_to_pfn(const void *v);
void *page_address(const struct page *p);
phys_addr_t page_to_phys(const struct page *p);
#define page_to_nid(p) 0
void *lkpi_phys_to_virt(phys_addr_t p);
phys_addr_t lkpi_virt_to_phys(const void *v);
#define __va(p) lkpi_phys_to_virt((phys_addr_t)(p))
#define __pa(v) lkpi_virt_to_phys((const void *)(v))
#define phys_to_virt __va
#define virt_to_phys __pa
#define pfn_valid(pfn) lkpi_pfn_valid(pfn)
bool lkpi_pfn_valid(unsigned long pfn);
bool virt_addr_valid(const void *v);
#define __virt_addr_valid virt_addr_valid
static inline void get_page(struct page *p) { atomic_inc(&p->_refcount); }
void put_page(struct page *p);
static inline int page_count(const struct page *p) { return atomic_read(&p->_refcount); }
static inline int page_mapcount(const struct page *p) { return atomic_read(&p->_mapcount) + 1; }
#define page_ref_count(p) page_count(p)
static inline void set_page_count(struct page *p, int v) { atomic_set(&p->_refcount, v); }
static inline void init_page_count(struct page *p) { set_page_count(p, 1); }
#define PG_reserved 0
#define PG_dirty 1
#define PG_locked 2
#define SetPageReserved(p) set_bit(PG_reserved, &(p)->flags)
#define ClearPageReserved(p) clear_bit(PG_reserved, &(p)->flags)
#define PageReserved(p) test_bit(PG_reserved, &(p)->flags)
#define SetPageDirty(p) set_bit(PG_dirty, &(p)->flags)
#define PageDirty(p) test_bit(PG_dirty, &(p)->flags)
#define PageHighMem(p) 0
#define PageCompound(p) 0
#define PageSlab(p) 0
#define page_folio(p) ((struct folio *)(p))
#define compound_head(p) (p)
int set_page_dirty(struct page *p);
int set_page_dirty_lock(struct page *p);
void *kmap(struct page *p);
void kunmap(struct page *p);
void *kmap_atomic(struct page *p);
void kunmap_atomic(void *v);
#define kmap_local_page kmap_atomic
#define kunmap_local kunmap_atomic
void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot);
void vunmap(const void *v);
void *vmap_pfn(unsigned long *pfns, unsigned int count, unsigned long prot);
#define VM_MAP 0x4
void clear_page(void *p);
void copy_page(void *d, void *s);
unsigned long totalram_pages(void);
struct sysinfo { unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap, totalhigh, freehigh; unsigned int mem_unit; };
void si_meminfo(struct sysinfo *s);
extern unsigned long max_pfn;
extern void *high_memory;
extern unsigned long num_physpages;

/* page attributes (caching of the direct map and of mappings) */
#define __pgprot(x) ((pgprot_t){ (x) })
#define pgprot_val(p) ((p).pgprot)
#define _PAGE_PRESENT 0x001ULL
#define _PAGE_RW 0x002ULL
#define _PAGE_USER 0x004ULL
#define _PAGE_PWT 0x008ULL
#define _PAGE_PCD 0x010ULL
#define _PAGE_PAT 0x080ULL
#define _PAGE_NX (1ULL << 63)
#define _PAGE_CACHE_MASK (_PAGE_PWT | _PAGE_PCD | _PAGE_PAT)
#define PAGE_KERNEL __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_NX)
#define __PAGE_KERNEL_NOCACHE (_PAGE_PRESENT | _PAGE_RW | _PAGE_NX | _PAGE_PCD)
#define __PAGE_KERNEL (_PAGE_PRESENT | _PAGE_RW | _PAGE_NX)
#define PAGE_KERNEL_NOCACHE __pgprot(__PAGE_KERNEL_NOCACHE)
#define PAGE_SHARED __pgprot(_PAGE_PRESENT | _PAGE_RW | _PAGE_USER | _PAGE_NX)
pgprot_t pgprot_noncached(pgprot_t p);
pgprot_t pgprot_writecombine(pgprot_t p);
pgprot_t pgprot_decrypted_lkpi(pgprot_t p);
#define pgprot_decrypted pgprot_decrypted_lkpi
#define pgprot_encrypted(p) (p)
#define pgprot_device pgprot_noncached
int set_memory_uc(unsigned long addr, int pages);
int set_memory_wc(unsigned long addr, int pages);
int set_memory_wb(unsigned long addr, int pages);
int set_pages_uc(struct page *p, int n);
int set_pages_wb(struct page *p, int n);
int set_pages_array_uc(struct page **p, int n);
int set_pages_array_wc(struct page **p, int n);
int set_pages_array_wb(struct page **p, int n);
void *ioremap(phys_addr_t addr, size_t size);
void *ioremap_cache(phys_addr_t addr, size_t size);
void *ioremap_wc(phys_addr_t addr, size_t size);
void *ioremap_uc(phys_addr_t addr, size_t size);
#define ioremap_nocache ioremap
#define ioremap_driver_hardened ioremap
void iounmap(volatile void *p);
void *memremap(resource_size_t addr, size_t size, unsigned long flags);
void memunmap(void *p);
#define MEMREMAP_WB 1
#define MEMREMAP_WT 2
#define MEMREMAP_WC 4
bool arch_phys_wc_add_lkpi(void);
int arch_phys_wc_add(unsigned long base, unsigned long size);
void arch_phys_wc_del(int handle);
int arch_io_reserve_memtype_wc(resource_size_t start, resource_size_t size);
void arch_io_free_memtype_wc(resource_size_t start, resource_size_t size);
bool pat_enabled(void);

/* a program's address space, as the driver maps memory into it */
struct vm_area_struct;
struct vm_fault;
typedef unsigned int vm_fault_t;
#define VM_FAULT_OOM 0x0001
#define VM_FAULT_SIGBUS 0x0002
#define VM_FAULT_NOPAGE 0x0100
#define VM_FAULT_LOCKED 0x0200
#define VM_FAULT_RETRY 0x0400
struct vm_operations_struct {
    void (*open)(struct vm_area_struct *vma);
    void (*close)(struct vm_area_struct *vma);
    vm_fault_t (*fault)(struct vm_fault *vmf);
    int (*access)(struct vm_area_struct *vma, unsigned long addr, void *buf, int len, int write);
    int (*mremap)(struct vm_area_struct *vma);
};
enum { MM_FILEPAGES, MM_ANONPAGES, MM_SWAPENTS, MM_SHMEMPAGES, NR_MM_COUNTERS };
struct mm_struct {
    struct rw_semaphore mmap_lock;
    atomic_t mm_users, mm_count;
    unsigned long start_brk, brk;
    struct { s64 count; void *counters; } rss_stat[NR_MM_COUNTERS]; /* (a percpu_counter in Linux: one counter here) */
    void *lkpi_proc;    /* our kernel's process */
};
void add_mm_counter(struct mm_struct *mm, int member, long value);
#define inc_mm_counter(mm, m) add_mm_counter(mm, m, 1)
#define dec_mm_counter(mm, m) add_mm_counter(mm, m, -1)
typedef unsigned long vm_flags_t;

/* memory nodes and zones: one node, as our kernel has (no NUMA) */
#define MAX_NUMNODES 1
#define MAX_NR_ZONES 4
enum node_states { N_POSSIBLE, N_ONLINE, N_NORMAL_MEMORY, N_HIGH_MEMORY, N_MEMORY, N_CPU, NR_NODE_STATES };
static inline int node_state(int node, enum node_states s) { (void)s; return node == 0; }
struct zone { unsigned long zone_start_pfn, spanned_pages, present_pages; const char *name; };
struct pglist_data { struct zone node_zones[MAX_NR_ZONES]; int nr_zones; unsigned long node_start_pfn, node_present_pages, node_spanned_pages; int node_id; };
typedef struct pglist_data pg_data_t;
struct pglist_data *lkpi_node_data(int nid);
#define NODE_DATA(n) lkpi_node_data(n)
static inline unsigned long zone_end_pfn(const struct zone *z) { return z->zone_start_pfn + z->spanned_pages; }
static inline bool managed_zone(const struct zone *z) { return z->present_pages > 0; }
#define populated_zone(z) managed_zone(z)
struct memory_notify { unsigned long start_pfn, nr_pages; int status_change_nid; };
#define MEM_GOING_OFFLINE 1
#define MEM_OFFLINE 2
#define MEM_ONLINE 3
#define MEM_CANCEL_OFFLINE 4
int register_memory_notifier_lkpi(void);
#define hotplug_memory_notifier(fn, pri) 0
void mod_node_page_state(struct pglist_data *pgdat, int item, long delta);
#define NR_KERNEL_MISC_RECLAIMABLE 0

/* caching modes (x86's PAT) */
enum page_cache_mode { _PAGE_CACHE_MODE_WB = 0, _PAGE_CACHE_MODE_WC = 1, _PAGE_CACHE_MODE_UC_MINUS = 2,
                       _PAGE_CACHE_MODE_UC = 3, _PAGE_CACHE_MODE_WT = 4, _PAGE_CACHE_MODE_WP = 5 };
unsigned long cachemode2protval(enum page_cache_mode m);
/* memory encryption (AMD SEV, Intel TDX guests): not here; nothing to undo */
static inline int set_memory_encrypted(unsigned long addr, int n) { (void)addr; (void)n; return 0; }
static inline int set_memory_decrypted(unsigned long addr, int n) { (void)addr; (void)n; return 0; }
#define cc_platform_has(x) false
extern unsigned long lkpi_page_offset;
#define PAGE_OFFSET lkpi_page_offset
struct file;
struct vm_area_struct {
    unsigned long vm_start, vm_end, vm_pgoff;
    unsigned long vm_flags;
    pgprot_t vm_page_prot;
    struct mm_struct *vm_mm;
    struct file *vm_file;
    const struct vm_operations_struct *vm_ops;
    void *vm_private_data;
};
struct vm_fault { struct vm_area_struct *vma; unsigned long address; pgoff_t pgoff; unsigned int flags; struct page *page; };
#define VM_READ 0x1UL
#define VM_WRITE 0x2UL
#define VM_EXEC 0x4UL
#define VM_SHARED 0x8UL
#define VM_MAYREAD 0x10UL
#define VM_MAYWRITE 0x20UL
#define VM_MAYEXEC 0x40UL
#define VM_MAYSHARE 0x80UL
#define VM_IO 0x4000UL
#define VM_DONTCOPY 0x20000UL
#define VM_DONTEXPAND 0x40000UL
#define VM_ACCOUNT 0x100000UL
#define VM_NORESERVE 0x200000UL
#define VM_PFNMAP 0x400UL
#define VM_MIXEDMAP 0x10000000UL
#define VM_DONTDUMP 0x4000000UL
#define VM_LOCKED 0x2000UL
#define VM_WIPEONFORK 0x2000000UL
void vm_flags_set(struct vm_area_struct *v, unsigned long f);
void vm_flags_clear(struct vm_area_struct *v, unsigned long f);
#define vm_flags_reset(v, f) ((v)->vm_flags = (f))
int remap_pfn_range(struct vm_area_struct *v, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot);
int io_remap_pfn_range(struct vm_area_struct *v, unsigned long addr, unsigned long pfn, unsigned long size, pgprot_t prot);
vm_fault_t vmf_insert_pfn(struct vm_area_struct *v, unsigned long addr, unsigned long pfn);
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *v, unsigned long addr, unsigned long pfn, pgprot_t prot);
vm_fault_t vmf_insert_page(struct vm_area_struct *v, unsigned long addr, struct page *p);
vm_fault_t vmf_insert_mixed(struct vm_area_struct *v, unsigned long addr, unsigned long pfn);
int vm_insert_page(struct vm_area_struct *v, unsigned long addr, struct page *p);
void zap_vma_ptes(struct vm_area_struct *v, unsigned long addr, unsigned long size);
void unmap_mapping_range(struct address_space *m, loff_t start, loff_t len, int even_cows);
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr);
struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr);
void mmap_read_lock(struct mm_struct *mm);
void mmap_read_unlock(struct mm_struct *mm);
int mmap_read_trylock(struct mm_struct *mm);
void mmap_write_lock(struct mm_struct *mm);
void mmap_write_unlock(struct mm_struct *mm);
#define mmap_assert_locked(mm) do { } while (0)
#define mmap_assert_write_locked(mm) do { } while (0)
void mmgrab(struct mm_struct *mm);
void mmdrop(struct mm_struct *mm);
void mmget(struct mm_struct *mm);
bool mmget_not_zero(struct mm_struct *mm);
void mmput(struct mm_struct *mm);
struct mm_struct *get_task_mm(struct task_struct *t);
#define FOLL_WRITE 0x01
#define FOLL_FORCE 0x10
#define FOLL_LONGTERM 0x10000
long pin_user_pages(unsigned long start, unsigned long n, unsigned int gup_flags, struct page **pages);
long pin_user_pages_remote(struct mm_struct *mm, unsigned long start, unsigned long n, unsigned int gup_flags,
                           struct page **pages, int *locked);
long get_user_pages(unsigned long start, unsigned long n, unsigned int gup_flags, struct page **pages);
long get_user_pages_remote(struct mm_struct *mm, unsigned long start, unsigned long n, unsigned int gup_flags,
                           struct page **pages, int *locked);
int get_user_pages_fast(unsigned long start, int n, unsigned int gup_flags, struct page **pages);
void unpin_user_page(struct page *p);
void unpin_user_pages(struct page **p, unsigned long n);
void unpin_user_pages_dirty_lock(struct page **p, unsigned long n, bool dirty);
vm_fault_t handle_mm_fault(struct vm_area_struct *v, unsigned long addr, unsigned int flags, void *regs);
#define FAULT_FLAG_WRITE 0x01
#define FAULT_FLAG_REMOTE 0x80
struct follow_pfnmap_args { struct vm_area_struct *vma; unsigned long address; unsigned long pfn; pgprot_t pgprot; bool writable; bool special; };
int follow_pfnmap_start(struct follow_pfnmap_args *a);
void follow_pfnmap_end(struct follow_pfnmap_args *a);

/* copying to and from programs */
unsigned long copy_from_user(void *to, const void __user *from, unsigned long n);
unsigned long copy_to_user(void __user *to, const void *from, unsigned long n);
#define _copy_from_user copy_from_user
#define _copy_to_user copy_to_user
unsigned long clear_user(void __user *to, unsigned long n);
#define access_ok(p, n) lkpi_access_ok((const void *)(p), (n))
bool lkpi_access_ok(const void *p, unsigned long n);
#define get_user(x, p) ({ typeof(*(p)) __v; int __e = copy_from_user(&__v, (p), sizeof __v) ? -14 : 0; (x) = __v; __e; })
#define put_user(x, p) ({ typeof(*(p)) __v = (x); copy_to_user((p), &__v, sizeof __v) ? -14 : 0; })
long strncpy_from_user(char *d, const char __user *s, long n);
