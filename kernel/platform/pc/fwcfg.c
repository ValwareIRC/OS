#include <asm/io.h>
#include <kernel/boot.h>
#include <kernel/platform.h>

#define FW_CFG_SELECTOR 0x510
#define FW_CFG_DATA     0x511

/* The selector register takes a little-endian 16-bit write on x86. */
void fwcfg_select(uint16_t key) { outw(FW_CFG_SELECTOR, key); }

void fwcfg_read(void *buf, size_t len) {
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++)
        b[i] = inb(FW_CFG_DATA);
}

#define FW_CFG_DMA 0x514

/* bit 1 of QEMU's id: it takes DMA requests */
bool fwcfg_dma_available(void) {
    uint8_t id[4];
    spin_lock(&fwcfg_lock);
    fwcfg_select(0x0001);
    fwcfg_read(id, 4);
    spin_unlock(&fwcfg_lock);
    return (id[0] & 2) != 0;
}

bool fwcfg_dma_kick(uint64_t desc) {
    volatile uint32_t *control = phys_to_virt(desc);
    dma_wmb();
    outl(FW_CFG_DMA, __builtin_bswap32((uint32_t)(desc >> 32)));
    outl(FW_CFG_DMA + 4, __builtin_bswap32((uint32_t)desc));
    for (int spins = 0; spins < 100000000; spins++) {
        if (!(__builtin_bswap32(*control) & ~1u))
            return true;
        __asm__ volatile("pause");
    }
    return false;
}
