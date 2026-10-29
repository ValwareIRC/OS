#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/lib.h>
#include <kernel/proc.h>

/* Wall clock from the CMOS real-time clock at boot, then the TSC for
 * everything after (assumes an invariant TSC, true on every CPU we target). */

static uint64_t tsc_hz, tsc_boot;
static int64_t boot_epoch;

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t read_rtc(void) {
    while (cmos(0x0a) & 0x80) /* update in progress */
        ;
    uint8_t s = cmos(0x00), mi = cmos(0x02), h = cmos(0x04), d = cmos(0x07), mo = cmos(0x08), y = cmos(0x09);
    uint8_t b = cmos(0x0b);
    if (!(b & 0x04)) { /* BCD */
#define BCD(x) (uint8_t)(((x) & 0x0f) + ((x) >> 4) * 10)
        s = BCD(s), mi = BCD(mi), d = BCD(d), mo = BCD(mo), y = BCD(y);
        h = (uint8_t)(BCD(h & 0x7f) | (h & 0x80));
#undef BCD
    }
    if (!(b & 0x02) && (h & 0x80)) /* 12-hour clock */
        h = (uint8_t)(((h & 0x7f) + 12) % 24);
    int64_t days = days_from_civil(2000 + y, mo, d);
    return days * 86400 + h * 3600 + mi * 60 + s;
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Measures the TSC over 50 ms of the (already calibrated) APIC tick. */
void time_init(void) {
    boot_epoch = read_rtc();
    uint64_t start_tick = global_ticks;
    while (global_ticks == start_tick)
        __asm__ volatile("sti; hlt");
    uint64_t t0 = rdtsc(), tick0 = global_ticks;
    while (global_ticks < tick0 + HZ / 20)
        __asm__ volatile("sti; hlt");
    uint64_t t1 = rdtsc();
    tsc_hz = (t1 - t0) * 20;
    tsc_boot = t0 - (tick0 * tsc_hz / HZ);
    kprintf("time: TSC %lu MHz, wall clock %ld\n", tsc_hz / 1000000, boot_epoch);
}

uint64_t time_monotonic_ns(void) {
    if (!tsc_hz)
        return global_ticks * (1000000000ULL / HZ);
    uint64_t d = rdtsc() - tsc_boot;
    return d / tsc_hz * 1000000000ULL + (d % tsc_hz) * 1000000000ULL / tsc_hz;
}

int64_t time_realtime_s(void) { return boot_epoch + (int64_t)(time_monotonic_ns() / 1000000000ULL); }

/* Wall clock with the monotonic clock's resolution (boot_epoch + uptime). */
uint64_t time_realtime_ns(void) { return (uint64_t)boot_epoch * 1000000000ULL + time_monotonic_ns(); }
