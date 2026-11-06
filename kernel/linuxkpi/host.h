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
