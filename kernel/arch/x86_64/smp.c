#include <asm/msr.h>
#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

struct cpu *cpus[MAX_CPUS];
uint32_t cpu_count = 1;

void arch_send_resched(struct cpu *c) { lapic_send_ipi(c->arch.lapic_id, VEC_RESCHED); }

/* Device interrupts all go to the boot CPU's local APIC. */
bool arch_msi_setup(const struct pci_dev *d, unsigned irq, uint64_t *addr, uint32_t *data) {
    (void)d;
    *addr = 0xfee00000u | (cpus[0]->arch.lapic_id << 12);
    *data = irq;
    return true;
}

void sched_preempt(void);
void lapic_timer_in(uint64_t ns);

/* The one-shot timer fired: a tick if it is time for one (counted, the scheduler's turn), sleepers that are due
 * woken; then it is set again for whichever comes first. */
static void timer_irq(struct regs *r) {
    lapic_eoi();
    struct cpu *c = this_cpu();
    const uint64_t tick_ns = 1000000000ULL / HZ;
    uint64_t now = time_monotonic_ns();
    if (!time_precise()) {
        /* Until the counter is measured the clock moves only with ticks, and time_init measures it against them:
         * exactly one tick every 1/HZ s then (anything else and the clock runs at the wrong speed for good). */
        int woken;
        lapic_timer_in(tick_ns);
        c->arch.next_tick_ns = c->arch.armed_ns = 0;
        (void)sched_timer(now, &woken);
        proc_account_tick(r);
        sched_tick();
        return;
    }
    bool tick = now + 50000 >= c->arch.next_tick_ns;
    if (tick && now > c->arch.next_tick_ns + tick_ns) { /* it slept through ticks while idle: idle they were */
        void proc_account_idle(uint64_t ticks);
        proc_account_idle((now - c->arch.next_tick_ns) / tick_ns);
    }
    if (tick) {
        c->arch.next_tick_ns += tick_ns;
        if (c->arch.next_tick_ns <= now || c->arch.next_tick_ns > now + 2 * tick_ns) /* fell behind, or the clock moved */
            c->arch.next_tick_ns = now + tick_ns;
    }
    int woken = 0;
    uint64_t next = sched_timer(now, &woken);
    if (next > c->arch.next_tick_ns)
        next = c->arch.next_tick_ns;
    c->arch.armed_ns = next;
    lapic_timer_in(next > now ? next - now : 1000);
    if (tick) {
        proc_account_tick(r); /* CPU times for /proc */
        sched_tick();
    } else if (woken && c->current == c->idle) {
        sched_preempt(); /* a sleeper due now, and nothing else running here: at once, not at the tick */
    }
}

static void resched_irq(struct regs *r) {
    (void)r;
    lapic_eoi();
    sched_preempt();
}

/* ---- TLB shootdown ----
 * Each CPU caches translations for the user page tables it has loaded. When
 * a thread unmaps or reprotects memory, the other CPUs running threads of
 * the same mm must drop those entries before the change is relied on (and
 * before freed pages are reused). We flush the whole TLB on the target:
 * simpler than passing ranges, and user mappings are not global. */

volatile int tlb_shootdowns_active;

void tlb_service(void) {
    struct cpu *c = this_cpu();
    /* Clear first, then flush: a request that arrives after the clear stays
     * pending (and has its own IPI), so none is lost. */
    if (__atomic_exchange_n(&c->arch.tlb_flush_pending, 0, __ATOMIC_ACQ_REL))
        write_cr3(read_cr3());
}

static void tlb_irq(struct regs *r) {
    (void)r;
    lapic_eoi();
    tlb_service();
}

void tlb_shootdown(struct mm *mm) {
    if (cpu_count == 1)
        return;
    uint64_t f = irq_save(); /* stay on this CPU while we compare against it */
    struct cpu *self = this_cpu();
    /* Our PTE writes must be visible before we look at who has the mm
     * loaded; mm_activate publishes active_mm before loading CR3, so a CPU
     * we miss here loads CR3 after our change and sees it. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t sent[MAX_CPUS / 64] = {0};
    bool any = false;
    for (uint32_t i = 0; i < cpu_count; i++) {
        struct cpu *c = cpus[i];
        if (!c || c == self || !c->online || __atomic_load_n(&c->active_mm, __ATOMIC_ACQUIRE) != mm)
            continue;
        if (!any)
            __atomic_add_fetch(&tlb_shootdowns_active, 1, __ATOMIC_SEQ_CST);
        any = true;
        __atomic_store_n(&c->arch.tlb_flush_pending, 1, __ATOMIC_SEQ_CST);
        sent[i / 64] |= 1ULL << (i % 64);
        lapic_send_ipi(c->arch.lapic_id, VEC_TLB);
    }
    for (uint32_t i = 0; any && i < cpu_count; i++) {
        if (!(sent[i / 64] & (1ULL << (i % 64))))
            continue;
        /* Answer requests aimed at us while we wait, or two CPUs shooting
         * at each other would wait forever. */
        while (__atomic_load_n(&cpus[i]->arch.tlb_flush_pending, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
            tlb_service();
        }
    }
    if (any)
        __atomic_sub_fetch(&tlb_shootdowns_active, 1, __ATOMIC_SEQ_CST);
    irq_restore(f);
}

/* Each application processor arrives here on a small bootloader stack, which
 * becomes its idle thread's stack. */
static void ap_entry(struct limine_mp_info *info) {
    struct cpu *c = (struct cpu *)info->extra_argument;
    write_cr3(virt_to_phys_hhdm(vmm_kernel_pml4()));
    cpu_enable_features();
    fpu_init_cpu();
    syscall_init_cpu();
    gdt_init(c);
    cpu_set_percpu(c);
    idt_load();
    lapic_init_cpu();
    sched_init_cpu();
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    for (;;) {
        __asm__ volatile("cli");
        arch_idle_quiet();
        __asm__ volatile("sti; hlt"); /* (sti's one-instruction shadow: nothing slips in before the hlt) */
    }
}

void smp_init(void) {
    idt_set_handler(VEC_TIMER, timer_irq);
    idt_set_handler(VEC_RESCHED, resched_irq);
    idt_set_handler(VEC_TLB, tlb_irq);
    lapic_init_cpu(); /* BSP tick */
    this_cpu()->online = true; /* local APIC ready: may send and receive IPIs */

    struct limine_mp_response *mp = boot_mp_request.response;
    if (!mp) {
        kprintf("smp: bootloader gave no CPU list, running on one core\n");
        return;
    }
    uint32_t started = 0;
    for (uint64_t i = 0; i < mp->cpu_count && i < MAX_CPUS; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id)
            continue;
        struct cpu *c = kzalloc(sizeof *c);
        KASSERT(c);
        c->id = cpu_count;
        c->arch.lapic_id = info->lapic_id;
        cpus[cpu_count++] = c;
        info->extra_argument = (uint64_t)c;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_SEQ_CST);
        started++;
    }
    uint64_t deadline = global_ticks + HZ; /* one second */
    for (uint32_t i = 1; i < cpu_count; i++)
        while (!__atomic_load_n(&cpus[i]->online, __ATOMIC_ACQUIRE)) {
            if (global_ticks > deadline)
                panic("smp: cpu %u (apic %u) did not come online", i, cpus[i]->arch.lapic_id);
            __asm__ volatile("sti; pause");
        }
    kprintf("smp: %u cpu(s) online%s\n", cpu_count, mp->flags & LIMINE_MP_RESPONSE_X86_64_X2APIC ? " (x2apic)" : "");
}
