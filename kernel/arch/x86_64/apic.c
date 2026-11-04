#include <asm/io.h>
#include <asm/msr.h>
#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/spinlock.h>
#include <kernel/sched.h>

/* Local APIC (x2APIC when the CPU has it, xAPIC MMIO otherwise), I/O APIC,
 * and the periodic scheduler tick. The legacy 8259 PIC is masked for good. */

#define MSR_APIC_BASE      0x1b
#define APIC_BASE_ENABLE   (1ULL << 11)
#define APIC_BASE_X2APIC   (1ULL << 10)

#define LAPIC_ID       0x020
#define LAPIC_EOI      0x0b0
#define LAPIC_SVR      0x0f0
#define LAPIC_ICR_LO   0x300
#define LAPIC_ICR_HI   0x310
#define LAPIC_LVT_TMR  0x320
#define LAPIC_TMR_INIT 0x380
#define LAPIC_TMR_CUR  0x390
#define LAPIC_TMR_DIV  0x3e0

#define LVT_MASKED   (1u << 16)
#define LVT_PERIODIC (1u << 17)

struct madt {
    char signature[4];
    uint32_t length;
    uint8_t rev, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

struct ioapic {
    volatile uint32_t *base;
    uint32_t gsi_base, gsi_count;
};

struct iso {
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
};

static bool x2apic;
static volatile uint32_t *lapic_mmio;
static struct ioapic ioapics[8];
static int ioapic_count;
static struct iso isos[16];
static int iso_count;
static uint32_t ticks_per_period; /* APIC timer count per 1/HZ s, divider 16 */
volatile uint64_t global_ticks;

static uint32_t lapic_read(uint32_t reg) {
    return x2apic ? (uint32_t)rdmsr(0x800 + (reg >> 4)) : lapic_mmio[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t v) {
    if (x2apic)
        wrmsr(0x800 + (reg >> 4), v);
    else
        lapic_mmio[reg / 4] = v;
}

uint32_t lapic_id(void) { return x2apic ? lapic_read(LAPIC_ID) : lapic_read(LAPIC_ID) >> 24; }

void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    if (x2apic) {
        wrmsr(0x800 + (LAPIC_ICR_LO >> 4), ((uint64_t)apic_id << 32) | vector);
    } else {
        lapic_write(LAPIC_ICR_HI, apic_id << 24);
        lapic_write(LAPIC_ICR_LO, vector);
        while (lapic_read(LAPIC_ICR_LO) & (1u << 12)) /* delivery pending */
            __asm__ volatile("pause");
    }
}

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg) {
    io->base[0] = reg;
    return io->base[4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t v) {
    io->base[0] = reg;
    io->base[4] = v;
}

static struct ioapic *ioapic_for(uint32_t gsi) {
    for (int i = 0; i < ioapic_count; i++)
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].gsi_count)
            return &ioapics[i];
    return NULL;
}

void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t dest_apic, bool level, bool active_low) {
    struct ioapic *io = ioapic_for(gsi);
    if (!io)
        panic("ioapic: no I/O APIC handles GSI %u", gsi);
    uint32_t pin = gsi - io->gsi_base;
    uint32_t lo = vector | (level ? 1u << 15 : 0) | (active_low ? 1u << 13 : 0);
    ioapic_write(io, 0x10 + pin * 2 + 1, dest_apic << 24);
    ioapic_write(io, 0x10 + pin * 2, lo);
}

uint32_t irq_to_gsi(uint8_t isa_irq, bool *level, bool *active_low) {
    *level = false;
    *active_low = false;
    for (int i = 0; i < iso_count; i++) {
        if (isos[i].source != isa_irq)
            continue;
        uint16_t pol = isos[i].flags & 3, trig = (isos[i].flags >> 2) & 3;
        *active_low = pol == 3;
        *level = trig == 3;
        return isos[i].gsi;
    }
    return isa_irq;
}

static void pic_disable(void) {
    /* Remap to 0x20/0x28 so a stray legacy IRQ can't masquerade as an
     * exception, then mask every line. */
    outb(0x20, 0x11); io_wait();
    outb(0xa0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xa1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xa1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xa1, 0x01); io_wait();
    outb(0x21, 0xff);
    outb(0xa1, 0xff);
}

static void parse_madt(void) {
    struct madt *m = acpi_find_table("APIC", 0);
    if (!m)
        panic("apic: no MADT");
    uint64_t lapic_phys = m->lapic_addr;
    for (uint8_t *p = m->entries; p < (uint8_t *)m + m->length; p += p[1]) {
        switch (p[0]) {
        case 1: { /* I/O APIC */
            if (ioapic_count == (int)ARRAY_LEN(ioapics))
                break;
            uint32_t addr, gsi_base;
            memcpy(&addr, p + 4, 4);
            memcpy(&gsi_base, p + 8, 4);
            vmm_map_mmio(addr, PAGE_SIZE, true);
            struct ioapic *io = &ioapics[ioapic_count++];
            io->base = phys_to_virt(addr);
            io->gsi_base = gsi_base;
            io->gsi_count = ((ioapic_read(io, 1) >> 16) & 0xff) + 1;
            for (uint32_t i = 0; i < io->gsi_count; i++)
                ioapic_write(io, 0x10 + i * 2, LVT_MASKED);
            break;
        }
        case 2: /* interrupt source override */
            if (iso_count < (int)ARRAY_LEN(isos)) {
                isos[iso_count].source = p[3];
                memcpy(&isos[iso_count].gsi, p + 4, 4);
                memcpy(&isos[iso_count].flags, p + 8, 2);
                iso_count++;
            }
            break;
        case 5: /* 64-bit local APIC address override */
            memcpy(&lapic_phys, p + 4, 8);
            break;
        }
    }
    if (!x2apic) {
        vmm_map_mmio(lapic_phys, PAGE_SIZE, true);
        lapic_mmio = phys_to_virt(lapic_phys);
    }
}

static uint64_t tsc_hz_measured;
static uint32_t tsc_tries, tsc_unc_ppm;

/* The counter's (TSC's) rate, against 50 ms of PIT channel 2. In a virtual machine every port access is a trip
 * to the emulator, and one can stall for milliseconds while it is busy (opening its window, say): measured as one
 * window of 10 ms that a stall stretched, a VM's clocks once ran 2.15 times too slow for good (a second lasted
 * two, the disk was written every 11 s, Firefox's start took 14 minutes). So both ends are pinned between two
 * counter readings, and a measurement that isn't sure to 0.1% is taken again (the surest of ten, at worst). */
static uint64_t calibrate_tsc(void) {
    const uint32_t pit_hz = 1193182, count = pit_hz / 20; /* 50 ms (59659: fits the 16-bit counter) */
    uint64_t best_hz = 0, best_ppm = UINT64_MAX;
    for (uint32_t attempt = 1; attempt <= 10; attempt++) {
        outb(0x61, (inb(0x61) & ~0x02) | 0x01); /* gate on, speaker off */
        outb(0x43, 0xb0);                         /* channel 2, lo/hi, mode 0: OUT goes high at 0 */
        outb(0x42, count & 0xff);
        uint64_t s0 = arch_counter();
        outb(0x42, count >> 8); /* it counts from this write */
        uint64_t s1 = arch_counter();
        uint64_t before = s1, e0, e1; /* `before`: when the last reading that said "not yet" began */
        bool ended = false;
        for (;;) {
            e0 = arch_counter();
            ended = inb(0x61) & 0x20;
            e1 = arch_counter();
            if (ended || e1 - s1 > (1ULL << 35)) /* (no PIT answering: given up after some seconds) */
                break;
            before = e0;
        }
        if (!ended)
            break;
        /* it began in [s0, s1] and ended in [before, e1] */
        uint64_t span = (before + e1) / 2 - (s0 + s1) / 2, unc = (s1 - s0) + (e1 - before);
        uint64_t ppm = span ? unc * 1000000 / span : UINT64_MAX;
        if (ppm < best_ppm) {
            best_ppm = ppm, best_hz = span * 20;
            tsc_tries = attempt, tsc_unc_ppm = (uint32_t)(ppm < 1000000 ? ppm : 999999);
        }
        if (ppm <= 1000)
            break;
    }
    return best_hz;
}

/* The counter's rate against the ACPI power-management timer (3.579545 MHz), where the machine has one: unlike
 * the PIT's "ended yet?", a read of it says what time it is, however long the read itself took. QEMU in full
 * screen on Windows answered port reads in 10-20 ms (its own drawing keeps its main thread busy): against the PIT
 * that left the rate unsure to 79%, so seconds lasted anything from half a second to two. Here every read is
 * pinned between two counter readings, and of the many made over 300 ms (longer while unsure, to 3 s) only the
 * quickest near each end count: a few always come back in microseconds. 0 if there is no such timer. */
static uint32_t pm_port;
static bool pm_32bit;

static void find_pm_timer(void) {
    const uint8_t *fadt = acpi_find_table("FACP", 0);
    uint32_t len = 0;
    if (fadt)
        memcpy(&len, fadt + 4, 4);
    if (!fadt || len < 116)
        return;
    uint32_t port = 0, flags = 0;
    memcpy(&port, fadt + 76, 4); /* PM_TMR_BLK */
    memcpy(&flags, fadt + 112, 4);
    if (port && port < 0x10000) {
        pm_port = port;
        pm_32bit = flags & (1u << 8); /* TMR_VAL_EXT */
    }
}

static uint64_t calibrate_tsc_pm(uint32_t *ppm_out) {
    const uint64_t pm_hz = 3579545;
    if (!pm_port)
        return 0;
    uint32_t mask = pm_32bit ? 0xffffffffu : 0xffffffu;
    struct mark { uint64_t mid, pm, width; } first = {0, 0, UINT64_MAX}, last = {0, 0, UINT64_MAX};
    uint64_t pm = 0, window = pm_hz * 3 / 10; /* 300 ms, then longer while unsure */
    uint32_t prev = inl((uint16_t)pm_port) & mask, same = 0;
    for (uint64_t reads = 0; reads < 2000000; reads++) {
        uint64_t t0 = arch_counter();
        uint32_t v = inl((uint16_t)pm_port) & mask;
        uint64_t t1 = arch_counter();
        if (v == prev && ++same > 100000)
            return 0; /* it doesn't move: not a timer */
        if (v != prev)
            same = 0;
        pm += (v - prev) & mask;
        prev = v;
        struct mark m = {(t0 + t1) / 2, pm, t1 - t0};
        if (pm < pm_hz / 20) { /* the first 50 ms: the quickest read is the start */
            if (m.width < first.width)
                first = m;
            continue;
        }
        if (first.width == UINT64_MAX)
            return 0;
        if (pm >= window - pm_hz / 20 && m.width < last.width) /* the last 50 ms: the quickest is the end */
            last = m;
        if (pm >= window && last.width != UINT64_MAX) {
            uint64_t span = last.mid - first.mid, ticks = last.pm - first.pm;
            uint64_t ppm = span ? (first.width + last.width) / 2 * 1000000 / span : UINT64_MAX;
            if (ppm <= 200 || window >= pm_hz * 3) {
                *ppm_out = (uint32_t)(ppm < 1000000 ? ppm : 999999);
                return ticks ? span * pm_hz / ticks : 0;
            }
            window += pm_hz / 2; /* unsure: half a second more */
            last.width = UINT64_MAX;
        }
    }
    return 0;
}

/* The APIC timer's ticks in 10 ms, against the counter (read inside the processor: no trip to the emulator). */
static uint32_t calibrate_timer(uint64_t tsc_hz) {
    lapic_write(LAPIC_TMR_DIV, 0x3); /* divide by 16 */
    uint64_t t0 = arch_counter();
    lapic_write(LAPIC_TMR_INIT, 0xffffffff);
    uint64_t t1 = arch_counter();
    while (arch_counter() - t1 < tsc_hz / 100)
        cpu_relax();
    uint64_t t2 = arch_counter();
    uint32_t elapsed = 0xffffffff - lapic_read(LAPIC_TMR_CUR);
    uint64_t t3 = arch_counter();
    lapic_write(LAPIC_TMR_INIT, 0);
    uint64_t span = (t2 + t3) / 2 - (t0 + t1) / 2;
    return span ? (uint32_t)((uint64_t)elapsed * (tsc_hz / 100) / span) : elapsed;
}

/* The counter's rate as measured at boot (time.c runs the clock on it). */
uint64_t arch_counter_hz(void);
uint64_t arch_counter_hz(void) { return tsc_hz_measured; }

static void enable_lapic_mode(void) {
    uint64_t base = rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE;
    if (x2apic)
        base |= APIC_BASE_X2APIC;
    wrmsr(MSR_APIC_BASE, base);
}

void apic_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    x2apic = c & (1u << 21);
    enable_lapic_mode();
    pic_disable();
    parse_madt();
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    find_pm_timer();
    uint32_t pm_ppm = 0;
    uint64_t pm_hz = calibrate_tsc_pm(&pm_ppm);
    if (pm_hz) {
        tsc_hz_measured = pm_hz, tsc_unc_ppm = pm_ppm, tsc_tries = 0; /* (try 0: the power-management timer) */
    } else {
        tsc_hz_measured = calibrate_tsc();
    }
    if (tsc_hz_measured < 100000000ULL || tsc_hz_measured > 20000000000ULL)
        tsc_hz_measured = 0; /* (no sensible answer: time.c measures it against the timer's ticks instead) */
    uint32_t per_10ms = tsc_hz_measured ? calibrate_timer(tsc_hz_measured) : 0;
    if (!per_10ms) { /* the old way: the APIC timer across one 10 ms of the PIT */
        lapic_write(LAPIC_TMR_DIV, 0x3);
        outb(0x43, 0xb0);
        outb(0x42, 11932 & 0xff);
        lapic_write(LAPIC_TMR_INIT, 0xffffffff);
        outb(0x42, 11932 >> 8);
        while (!(inb(0x61) & 0x20))
            ;
        per_10ms = 0xffffffff - lapic_read(LAPIC_TMR_CUR);
        lapic_write(LAPIC_TMR_INIT, 0);
    }
    ticks_per_period = per_10ms * 100 / HZ;
    char how[32];
    if (tsc_tries)
        ksnprintf(how, sizeof how, "the PIT, try %u", tsc_tries);
    else
        ksnprintf(how, sizeof how, "the ACPI timer");
    kprintf("apic: %s, %d I/O APIC(s), timer %u kHz; counter %lu kHz (sure to %u ppm, against %s)\n",
            x2apic ? "x2APIC" : "xAPIC", ioapic_count, per_10ms / 10 * 16, (unsigned long)(tsc_hz_measured / 1000),
            tsc_unc_ppm, how);
}

/* The timer is one-shot: each interrupt sets it again, for the next tick or a sleeper due before that, so a 1 ms
 * sleep is 1 ms and not a 4 ms tick. */
#define TICK_NS (1000000000ULL / HZ)
uint64_t time_monotonic_ns(void);

void lapic_timer_in(uint64_t ns) {
    uint64_t count = ns * ticks_per_period / TICK_NS;
    lapic_write(LAPIC_TMR_INIT, count < 1 ? 1 : count > 0xffffffffu ? 0xffffffffu : (uint32_t)count);
}

void arch_timer_arm(uint64_t deadline_ns) {
    struct cpu *c = this_cpu();
    if (!c || deadline_ns >= c->arch.armed_ns)
        return;
    uint64_t now = time_monotonic_ns();
    c->arch.armed_ns = deadline_ns;
    lapic_timer_in(deadline_ns > now ? deadline_ns - now : 1000);
}

void proc_account_idle(uint64_t ticks);
bool time_precise(void);

void arch_idle_quiet(void) {
    struct cpu *c = this_cpu();
    if (!c || c->id == 0 || !time_precise()) /* (CPU 0 keeps the system's tick: global_ticks, load averages) */
        return;
    uint64_t now = time_monotonic_ns(), next = sched_next_wake(), far = now + 1000000000ULL;
    uint64_t at = next < far ? next : far;
    c->arch.armed_ns = at;
    lapic_timer_in(at > now ? at - now : 1000);
}

void lapic_init_cpu(void) {
    enable_lapic_mode();
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    lapic_write(LAPIC_TMR_DIV, 0x3);
    lapic_write(LAPIC_LVT_TMR, VEC_TIMER); /* one-shot */
    struct cpu *c = this_cpu();
    c->arch.next_tick_ns = time_monotonic_ns() + TICK_NS;
    c->arch.armed_ns = c->arch.next_tick_ns;
    lapic_timer_in(TICK_NS);
}
