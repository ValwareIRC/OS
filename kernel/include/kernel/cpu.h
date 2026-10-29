#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
static inline void io_wait(void) { outb(0x80, 0); }

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" ::"c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}
static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" ::"r"(v) : "memory"); }
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory"); }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

#define MSR_EFER 0xC0000080
#define EFER_NXE (1ULL << 11)

/* Writing here makes QEMU exit when started with -device isa-debug-exit;
 * on real hardware and in `make run` the port is unused and this is a no-op. */
#define QEMU_DEBUG_EXIT_PORT 0xf4
