#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/platform.h>
#include <kernel/proc.h>

/* Wall clock from the CMOS real-time clock at boot, then the TSC for
 * everything after (assumes an invariant TSC, true on every CPU we target).
 * Set later (clock_settime: the network's time, from init) by moving where
 * the wall clock starts; the monotonic clock never jumps. */

static uint64_t tsc_hz, tsc_boot;
static int64_t boot_epoch;
static int64_t real_base_ns; /* the wall clock = this + the monotonic clock */

/* The counter's rate as the architecture measured it (x86: against the PIT, in apic.c), or 0: then measured here
 * over 50 ms of the (already calibrated) timer tick. */
__attribute__((weak)) uint64_t arch_counter_hz(void);
__attribute__((weak)) uint64_t arch_counter_hz(void) { return 0; }

void time_init(void) {
    boot_epoch = platform_rtc_read();
    uint64_t hz = arch_counter_hz(), t0, tick0;
    if (hz) {
        uint64_t f = irq_save(); /* (the tick and the counter read together) */
        t0 = arch_counter(), tick0 = global_ticks;
        irq_restore(f);
        tsc_hz = hz;
    } else {
        uint64_t start_tick = global_ticks;
        while (global_ticks == start_tick)
            cpu_idle_wait();
        t0 = arch_counter(), tick0 = global_ticks;
        while (global_ticks < tick0 + HZ / 20)
            cpu_idle_wait();
        uint64_t t1 = arch_counter();
        tsc_hz = (t1 - t0) * 20;
    }
    tsc_boot = t0 - (tick0 * tsc_hz / HZ);
    /* the wall clock was boot_epoch when it was read (about now): less the time the monotonic clock had counted
     * by then (it started with the timer, before this; counted on top, the clock began about a second ahead) */
    __atomic_store_n(&real_base_ns, boot_epoch * 1000000000LL - (int64_t)(tick0 * (1000000000ULL / HZ)),
                     __ATOMIC_RELAXED);
    kprintf("time: counter %lu MHz, wall clock %ld\n", tsc_hz / 1000000, boot_epoch);
}

/* Whether the clock runs on the counter yet (before time_init it counts timer ticks). */
bool time_precise(void) { return tsc_hz != 0; }

uint64_t time_monotonic_ns(void) {
    if (!tsc_hz)
        return global_ticks * (1000000000ULL / HZ);
    uint64_t d = arch_counter() - tsc_boot;
    return d / tsc_hz * 1000000000ULL + (d % tsc_hz) * 1000000000ULL / tsc_hz;
}

/* Wall clock with the monotonic clock's resolution. */
uint64_t time_realtime_ns(void) {
    int64_t base = __atomic_load_n(&real_base_ns, __ATOMIC_RELAXED);
    if (!base && boot_epoch) /* before time_init */
        base = boot_epoch * 1000000000LL;
    return (uint64_t)(base + (int64_t)time_monotonic_ns());
}

int64_t time_realtime_s(void) { return (int64_t)(time_realtime_ns() / 1000000000ULL); }

void time_set_realtime_ns(uint64_t ns) {
    int64_t was = (int64_t)time_realtime_ns();
    __atomic_store_n(&real_base_ns, (int64_t)ns - (int64_t)time_monotonic_ns(), __ATOMIC_RELAXED);
    kprintf("time: wall clock set to %lu (moved %ld ms)\n", (unsigned long)(ns / 1000000000ULL),
            (long)(((int64_t)ns - was) / 1000000));
}
