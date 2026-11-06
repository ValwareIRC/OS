#include <asm/cpu.h>
#include <asm/irq.h>
#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <linux/syscall.h>

#include "../host.h"

/* The Linux-shaped layer's way into our kernel (linuxkpi/host.h): each a few lines on our own interfaces. */

void kpi_host_log(const char *s, size_t n) { kprintf("%.*s", (int)n, s); }
void kpi_host_panic(const char *msg) { panic("%s", msg); }
void kpi_host_backtrace(void) { kprintf("(no backtrace here)\n"); }
int kpi_host_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) { return kvsnprintf(buf, size, fmt, ap); }

/* ---- memory ---- */
void *kpi_host_alloc(size_t n) { return kzalloc(n ? n : 1); }
void kpi_host_free(void *p) { kfree(p); }
uint64_t kpi_host_pages(size_t count) { return pmm_alloc(count); }
void kpi_host_pages_free(uint64_t phys, size_t count) { pmm_free(phys, count); }
void *kpi_host_phys_to_virt(uint64_t phys) { return phys_to_virt(phys); }
uint64_t kpi_host_virt_to_phys(const void *v) { return vmm_translate(vmm_kernel_pml4(), (uint64_t)v); }
uint64_t kpi_host_translate(const void *v) { return vmm_translate(vmm_kernel_pml4(), (uint64_t)v); }
void kpi_host_map_device(uint64_t phys, uint64_t len, int cache) { vmm_map_mmio(phys, len, cache != 2); }
uint64_t kpi_host_total_pages(void) { return pmm_total_pages(); }
uint64_t kpi_host_free_pages(void) { return pmm_free_pages(); }
uint64_t kpi_host_max_phys(void) { return pmm_total_pages() * PAGE_SIZE; }
uint64_t kpi_host_direct_map_base(void) { return (uint64_t)phys_to_virt(0); }

/* ---- time ---- */
uint64_t kpi_host_now_ns(void) { return time_monotonic_ns(); }
uint64_t kpi_host_wall_ns(void) { return time_realtime_ns(); }
void kpi_host_delay_ns(uint64_t ns) {
    uint64_t end = time_monotonic_ns() + ns;
    while (time_monotonic_ns() < end)
        arch_spin_wait();
}

/* ---- threads ---- */
struct start { void (*fn)(void *); void *arg; };
static void trampoline(void *a) {
    struct start s = *(struct start *)a;
    kfree(a);
    s.fn(s.arg);
}
kpi_host_thread *kpi_host_thread_start(const char *name, void (*fn)(void *), void *arg) {
    struct start *s = kzalloc(sizeof *s);
    if (!s)
        return NULL;
    s->fn = fn, s->arg = arg;
    struct thread *t = thread_create(name, trampoline, s);
    if (!t)
        kfree(s);
    return (kpi_host_thread *)t;
}
kpi_host_thread *kpi_host_thread_self(void) { return (kpi_host_thread *)thread_current(); }
void **kpi_host_thread_slot(kpi_host_thread *t) { return &((struct thread *)t)->kpi; }
void kpi_host_on_thread_gone(void (*fn)(void *slot)) { kpi_thread_gone = fn; }
void kpi_host_sleep_until(uint64_t deadline_ns) { thread_block_until_ns(deadline_ns); }
void kpi_host_wake(kpi_host_thread *t) { if (t) thread_wake((struct thread *)t); }
void kpi_host_yield(void) { thread_yield(); }
int kpi_host_pid(void) {
    struct thread *t = thread_current();
    return t && t->user && t->user->proc ? t->user->proc->pid : 0;
}
int kpi_host_tid(void) {
    struct thread *t = thread_current();
    return t && t->user ? t->user->tid : (int)(t ? t->id : 0);
}
bool kpi_host_signal_pending(void) {
    struct thread *t = thread_current();
    return t && t->user && signal_pending_current();
}
const char *kpi_host_comm(void) {
    struct thread *t = thread_current();
    if (t && t->user && t->user->proc)
        return t->user->proc->name;
    return t ? t->name : "?";
}
int kpi_host_cpu(void) { return (int)this_cpu()->id; }
int kpi_host_ncpus(void) { return (int)cpu_count; }
bool kpi_host_in_interrupt(void) { return false; /* (the layer counts its own handlers: linuxkpi/src/irq.c) */ }
uint64_t kpi_host_irq_save(void) { return irq_save(); }
void kpi_host_irq_restore(uint64_t f) { irq_restore(f); }
bool kpi_host_irqs_on(void) {
    uint64_t f;
    __asm__ __volatile__("pushfq; popq %0" : "=r"(f));
    return f & 0x200;
}

void kpi_host_on_each_cpu(void (*fn)(void *), void *arg) { smp_call_all(fn, arg); }

/* ---- programs' memory ---- */
bool kpi_host_copy_from_user(void *dst, uint64_t src, size_t n) { return copy_from_user(dst, src, n) == 0; }
bool kpi_host_copy_to_user(uint64_t dst, const void *src, size_t n) { return copy_to_user(dst, src, n) == 0; }
bool kpi_host_user_range_ok(uint64_t addr, size_t n) { return addr + n >= addr && addr + n <= 0x0000800000000000ull; }

/* ---- PCI ---- */
int kpi_host_pci_count(void) { return pci_count(); }
bool kpi_host_pci_info(int i, uint16_t *vendor, uint16_t *device, uint32_t *class_rev, uint8_t *bus, uint8_t *dev, uint8_t *fn,
                       uint64_t bar[6], uint64_t bar_size[6], bool bar_io[6], uint8_t *irq_line) {
    struct pci_dev *d = pci_get(i);
    if (!d)
        return false;
    *vendor = d->vendor, *device = d->device, *bus = d->bus, *dev = d->dev, *fn = d->fn, *irq_line = d->irq_line;
    *class_rev = pci_read32(d, 0x08);
    for (int b = 0; b < 6; b++)
        bar[b] = d->bar[b], bar_size[b] = d->bar_size[b], bar_io[b] = d->bar_is_io[b];
    return true;
}
uint32_t kpi_host_pci_read(int i, int off, int width) {
    struct pci_dev *d = pci_get(i);
    if (!d)
        return ~0u;
    uint32_t v = pci_read32(d, (uint8_t)(off & ~3));
    v >>= (off & 3) * 8;
    return width == 4 ? v : width == 2 ? (v & 0xffff) : (v & 0xff);
}
void kpi_host_pci_write(int i, int off, int width, uint32_t v) {
    struct pci_dev *d = pci_get(i);
    if (!d)
        return;
    if (width == 4) {
        pci_write32(d, (uint8_t)off, v);
        return;
    }
    uint32_t w = pci_read32(d, (uint8_t)(off & ~3));
    int shift = (off & 3) * 8;
    uint32_t mask = (width == 2 ? 0xffffu : 0xffu) << shift;
    pci_write32(d, (uint8_t)(off & ~3), (w & ~mask) | ((v << shift) & mask));
}
void kpi_host_pci_enable(int i) {
    struct pci_dev *d = pci_get(i);
    if (d)
        pci_enable(d);
}

/* interrupts for the layer's devices: a few vectors of their own, each with a trampoline to its handler */
#define KPI_VEC0 0x30
#define KPI_NVEC 4
static struct { int pci; void (*handler)(void *); void *arg; } vecs[KPI_NVEC];
static void dispatch(int n) {
    if (vecs[n].handler)
        vecs[n].handler(vecs[n].arg);
    lapic_eoi();
}
static void v0(struct regs *r) { (void)r; dispatch(0); }
static void v1(struct regs *r) { (void)r; dispatch(1); }
static void v2(struct regs *r) { (void)r; dispatch(2); }
static void v3(struct regs *r) { (void)r; dispatch(3); }
static irq_handler_t const tramps[KPI_NVEC] = {v0, v1, v2, v3};

int kpi_host_pci_irq(int i, void (*handler)(void *arg), void *arg) {
    struct pci_dev *d = pci_get(i);
    if (!d)
        return -1;
    for (int n = 0; n < KPI_NVEC; n++)
        if (!vecs[n].handler) {
            vecs[n].pci = i, vecs[n].handler = handler, vecs[n].arg = arg;
            idt_set_handler((uint8_t)(KPI_VEC0 + n), tramps[n]);
            if (!pci_enable_msi(d, KPI_VEC0 + n)) { /* (no MSI: not handled yet; the driver then polls) */
                vecs[n].handler = NULL;
                return -1;
            }
            return KPI_VEC0 + n;
        }
    return -1;
}
void kpi_host_pci_irq_free(int i) {
    for (int n = 0; n < KPI_NVEC; n++)
        if (vecs[n].handler && vecs[n].pci == i)
            vecs[n].handler = NULL;
}

void kpi_host_random(void *buf, size_t n) { random_bytes(buf, n); }
