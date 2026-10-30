#include "virt.h"
#include <asm/io.h>
#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>

#define FW_CFG_DATA     0x00
#define FW_CFG_SELECTOR 0x08
#define FW_CFG_DMA      0x10

#define DMA_CTL_ERROR  0x01
#define DMA_CTL_SELECT 0x08
#define DMA_CTL_WRITE  0x10

static volatile uint8_t *regs(void) {
    vmm_map_mmio(VIRT_FW_CFG, 0x1000, true);
    return phys_to_virt(VIRT_FW_CFG);
}

/* The MMIO selector register takes a big-endian 16-bit write. */
void fwcfg_select(uint16_t key) {
    *(volatile uint16_t *)(regs() + FW_CFG_SELECTOR) = __builtin_bswap16(key);
}

void fwcfg_read(void *buf, size_t len) {
    volatile uint8_t *data = regs() + FW_CFG_DATA;
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++)
        b[i] = *data;
}

bool fwcfg_dma_available(void) { return true; } /* QEMU virt: always */

bool fwcfg_dma_kick(uint64_t desc) {
    volatile uint32_t *control = phys_to_virt(desc);
    dma_wmb();
    volatile uint8_t *r = regs();
    mmio_write32(r + FW_CFG_DMA, __builtin_bswap32((uint32_t)(desc >> 32)));
    mmio_write32(r + FW_CFG_DMA + 4, __builtin_bswap32((uint32_t)desc));
    for (int spins = 0; spins < 100000000; spins++) {
        if (!(__builtin_bswap32(*control) & ~1u)) {
            dma_rmb();
            return true;
        }
        cpu_relax();
    }
    return false;
}

struct dma_access {
    uint32_t control, length;
    uint64_t address;
};

/* QEMU takes fw_cfg writes only through the DMA interface, so we put the
 * access descriptor and the data in one page and wait for it to clear `control`. */
bool fwcfg_write(uint16_t key, const void *buf, uint32_t len) {
    static uint64_t page;
    if (len > PAGE_SIZE - sizeof(struct dma_access))
        return false;
    spin_lock(&fwcfg_lock);
    if (!page && !(page = pmm_alloc(1))) {
        spin_unlock(&fwcfg_lock);
        return false;
    }
    volatile struct dma_access *a = phys_to_virt(page);
    memcpy((uint8_t *)phys_to_virt(page) + sizeof *a, buf, len);
    a->control = __builtin_bswap32((uint32_t)key << 16 | DMA_CTL_SELECT | DMA_CTL_WRITE);
    a->length = __builtin_bswap32(len);
    a->address = __builtin_bswap64(page + sizeof *a);
    dma_wmb();
    volatile uint8_t *r = regs();
    mmio_write32(r + FW_CFG_DMA, __builtin_bswap32((uint32_t)(page >> 32)));
    mmio_write32(r + FW_CFG_DMA + 4, __builtin_bswap32((uint32_t)page));
    bool done = false;
    for (int spins = 0; spins <= 1000000; spins++) {
        if (!(a->control & ~__builtin_bswap32(DMA_CTL_ERROR))) {
            done = !a->control;
            break;
        }
        cpu_relax();
    }
    spin_unlock(&fwcfg_lock);
    return done;
}
