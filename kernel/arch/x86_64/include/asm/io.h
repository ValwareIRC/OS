#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

static inline uint8_t mmio_read8(const volatile void *p) { return *(const volatile uint8_t *)p; }
static inline uint16_t mmio_read16(const volatile void *p) { return *(const volatile uint16_t *)p; }
static inline uint32_t mmio_read32(const volatile void *p) { return *(const volatile uint32_t *)p; }
static inline uint64_t mmio_read64(const volatile void *p) { return *(const volatile uint64_t *)p; }
static inline void mmio_write8(volatile void *p, uint8_t v) { *(volatile uint8_t *)p = v; }
static inline void mmio_write16(volatile void *p, uint16_t v) { *(volatile uint16_t *)p = v; }
static inline void mmio_write32(volatile void *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static inline void mmio_write64(volatile void *p, uint64_t v) { *(volatile uint64_t *)p = v; }

/* x86 keeps loads in order with loads and stores with stores, also towards
 * devices, so ordering accesses to DMA memory only needs the compiler's help. */
static inline void dma_rmb(void) { __asm__ volatile("" ::: "memory"); }
static inline void dma_wmb(void) { __asm__ volatile("" ::: "memory"); }
