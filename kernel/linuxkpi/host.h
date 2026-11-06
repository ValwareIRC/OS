#pragma once
/* kernel/linuxkpi/host.h: how the Linux-shaped layer (linuxkpi/src) reaches our kernel. Only plain C types cross
 * here, so the layer's headers (Linux's names and shapes) and our kernel's never meet in one file; the files in host/
 * implement these against our kernel's own interfaces. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* the log */
void kpi_host_log(const char *s, size_t n);
void kpi_host_panic(const char *msg) __attribute__((noreturn));
void kpi_host_backtrace(void);
int kpi_host_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* memory: the kernel heap, whole pages, the direct map, device memory */
void *kpi_host_alloc(size_t n);          /* zeroed; NULL when out */
size_t kpi_host_alloc_size(const void *p); /* what it can hold */
void kpi_host_free(void *p);
uint64_t kpi_host_pages(size_t count);   /* physically contiguous; 0 when out */
void kpi_host_pages_free(uint64_t phys, size_t count);
void *kpi_host_phys_to_virt(uint64_t phys);
uint64_t kpi_host_virt_to_phys(const void *v);  /* the direct map's (and the kernel's own addresses) */
uint64_t kpi_host_translate(const void *v);     /* any kernel address, through the page tables; 0 unmapped */
void kpi_host_map_device(uint64_t phys, uint64_t len, int cache); /* 0 uncached, 1 write-combining, 2 cached */
uint64_t kpi_host_total_pages(void);
uint64_t kpi_host_free_pages(void);
uint64_t kpi_host_max_phys(void);
uint64_t kpi_host_direct_map_base(void);
/* the layer's own kernel addresses (vmalloc, vmap, ioremap): 4 KiB pages, each mapping with its caching */
void *kpi_host_vmap(const uint64_t *phys, size_t pages, int cache, void *owner); /* NULL when out */
void *kpi_host_ioremap(uint64_t phys, size_t len, int cache);  /* (the address of `phys` itself, offset kept) */
void *kpi_host_vunmap(const void *v);         /* unmaps (not the pages): the owner given to kpi_host_vmap */
bool kpi_host_vmapped(const void *v, void **start, size_t *pages, void **owner); /* inside one of them? */
bool kpi_host_in_vmap_area(const void *v);
void kpi_host_flush_cache(const void *v, size_t len); /* written back and dropped from every cache */
void kpi_host_page_ref(uint64_t phys);   /* (pages programs had: counted, so a device can hold one) */
void kpi_host_page_unref(uint64_t phys);

/* time */
uint64_t kpi_host_now_ns(void);
uint64_t kpi_host_wall_ns(void);
void kpi_host_delay_ns(uint64_t ns);    /* spins */

/* threads */
typedef struct kpi_host_thread kpi_host_thread;
kpi_host_thread *kpi_host_thread_start(const char *name, void (*fn)(void *), void *arg);
kpi_host_thread *kpi_host_thread_self(void);
void **kpi_host_thread_slot(kpi_host_thread *t); /* one pointer of the layer's in each thread */
void kpi_host_on_thread_gone(void (*fn)(void *slot)); /* called with it when the thread is freed */
void kpi_host_sleep_until(uint64_t deadline_ns); /* blocks; woken early by kpi_host_wake */
void kpi_host_wake(kpi_host_thread *t);
void kpi_host_yield(void);
int kpi_host_pid(void);        /* the process the calling thread is in (0: a kernel thread) */
int kpi_host_tid(void);
bool kpi_host_signal_pending(void);
const char *kpi_host_comm(void);
int kpi_host_cpu(void);
int kpi_host_ncpus(void);
bool kpi_host_in_interrupt(void);
uint64_t kpi_host_irq_save(void);
void kpi_host_irq_restore(uint64_t f);
bool kpi_host_irqs_on(void);
void kpi_host_on_each_cpu(void (*fn)(void *), void *arg); /* every processor, this one too; returns when all ran it */

/* programs' memory */
bool kpi_host_copy_from_user(void *dst, uint64_t src, size_t n); /* false: not all of it could be read */
bool kpi_host_copy_to_user(uint64_t dst, const void *src, size_t n);
bool kpi_host_user_range_ok(uint64_t addr, size_t n);

/* programs' address spaces (our kernel's, as they are) */
typedef struct kpi_host_mm kpi_host_mm;
kpi_host_mm *kpi_host_mm_current(void); /* NULL on a kernel thread */
void **kpi_host_mm_slot(kpi_host_mm *mm); /* one pointer of the layer's in each */
void kpi_host_on_mm_gone(void (*fn)(void *slot)); /* called with it when the address space ends */
/* the first mapping ending after addr: its range, protection (PROT_*), whether a device's, and the layer's record
 * when it is one of the layer's devices' */
bool kpi_host_vma_find(kpi_host_mm *mm, uint64_t addr, uint64_t *start, uint64_t *end, int *prot, bool *io, void **rec);
/* A driver's mappings (mmap of the layer's devices): the layer's callbacks, set once, then each mapping its record */
struct kpi_host_devmap_ops {
    void (*get)(void *rec);
    void (*put)(void *rec);              /* (may sleep) */
    void *(*fork)(void *rec);            /* the child's own record, or NULL: no copy (never sleeps) */
    int (*fault)(void *rec, kpi_host_mm *mm, uint64_t va, bool write); /* (may sleep) */
};
void kpi_host_devmap_init(const struct kpi_host_devmap_ops *ops);
int kpi_host_dev_attach(kpi_host_mm *mm, uint64_t start, void *rec);
int kpi_host_dev_insert(kpi_host_mm *mm, uint64_t va, uint64_t phys, uint64_t len, int cache, bool write);
void kpi_host_dev_zap(kpi_host_mm *mm, uint64_t va, uint64_t len);
int kpi_host_dev_lookup(kpi_host_mm *mm, uint64_t va, uint64_t *phys, bool *writable);
int kpi_host_pin(kpi_host_mm *mm, uint64_t va, bool write, uint64_t *phys); /* held until kpi_host_page_unref */

/* the layer's character devices, as nodes in /dev, and files */
typedef struct kpi_host_file kpi_host_file;
struct kpi_host_chr_ops {
    int (*open)(unsigned major, unsigned minor, unsigned flags, kpi_host_file *hf, void **rec); /* rec: the layer's */
    void (*close)(void *rec);                                   /* the file's last reference went */
    long (*ioctl)(void *rec, unsigned int cmd, unsigned long arg); /* (arg: as the program gave it) */
    long (*read)(void *rec, void *kbuf, size_t n, uint64_t off);   /* (a kernel buffer) */
    long (*write)(void *rec, const void *kbuf, size_t n, uint64_t off);
    int (*poll)(void *rec);                                     /* POLL* bits now (kpi_host_poll_wake: again) */
    int (*mmap)(void *rec, uint64_t addr, uint64_t len, int prot, uint64_t off); /* (a VMA_DEV range, reserved) */
};
void kpi_host_chr_init(const struct kpi_host_chr_ops *ops);
int kpi_host_chr_node(const char *path, unsigned major, unsigned minor); /* (its directories made too) */
void kpi_host_poll_wake(kpi_host_file *hf);
void kpi_host_file_get(kpi_host_file *hf);
void kpi_host_file_put(kpi_host_file *hf);
void *kpi_host_file_rec(kpi_host_file *hf); /* the layer's record, when the file is one of its devices' */
kpi_host_file *kpi_host_fget(int fd);       /* referenced; NULL when not open */
int kpi_host_fd_reserve(bool cloexec);      /* a descriptor kept (by a placeholder) for kpi_host_fd_set */
void kpi_host_fd_set(int fd, kpi_host_file *hf); /* (it takes its own reference) */
int kpi_host_fd_close(int fd);
bool kpi_host_fd_next(int *fd, kpi_host_file **hf); /* the next of the layer's files open in the program, from *fd */
kpi_host_file *kpi_host_kopen(const char *path, unsigned flags, unsigned mode, int *err);
long kpi_host_kread(kpi_host_file *hf, void *buf, size_t n, uint64_t off);
long kpi_host_kwrite(kpi_host_file *hf, const void *buf, size_t n, uint64_t off);
uint64_t kpi_host_file_size(kpi_host_file *hf);

/* PCI */
int kpi_host_pci_count(void);
bool kpi_host_pci_info(int i, uint16_t *vendor, uint16_t *device, uint32_t *class_rev, uint8_t *bus, uint8_t *dev, uint8_t *fn,
                       uint64_t bar[6], uint64_t bar_size[6], bool bar_io[6], uint8_t *irq_line);
uint32_t kpi_host_pci_read(int i, int off, int width);
void kpi_host_pci_write(int i, int off, int width, uint32_t v);
void kpi_host_pci_enable(int i);
/* an interrupt for the device (MSI when it has it): the handler runs in interrupt context */
int kpi_host_pci_irq(int i, void (*handler)(void *arg), void *arg);
void kpi_host_pci_irq_free(int i);

/* random numbers */
void kpi_host_random(void *buf, size_t n);
