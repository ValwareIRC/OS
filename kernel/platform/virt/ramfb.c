#include "virt.h"
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>

/* QEMU's ramfb display: the screen is ordinary memory, and the fw_cfg file
 * "etc/ramfb" tells QEMU its address, format and size. The firmware offers
 * Limine only up to 1024x768, so larger modes come from here: we point the
 * device at our own memory, sized for the largest mode once. */

#define DRM_FORMAT_XRGB8888 0x34325258
#define MAX_W 2560
#define MAX_H 1600

struct ramfb_cfg {
    uint64_t addr;
    uint32_t fourcc, flags, width, height, stride;
} __attribute__((packed));

static spinlock_t lock = SPINLOCK_INIT;
static int state = -1; /* -1 not probed, 0 absent, 1 present */
static uint16_t key;
static uint64_t vram_phys;

bool platform_fb_mode_fits(uint32_t w, uint32_t h) {
    spin_lock(&lock);
    if (state < 0)
        state = fwcfg_find_file("etc/ramfb", &key) == (int)sizeof(struct ramfb_cfg);
    bool present = state == 1;
    spin_unlock(&lock);
    return present && w >= 320 && h >= 200 && w <= MAX_W && h <= MAX_H && (w % 8) == 0;
}

bool platform_fb_set_mode(uint32_t w, uint32_t h, uint64_t *phys, uint64_t *vram_bytes) {
    if (!platform_fb_mode_fits(w, h))
        return false;
    uint64_t size = ALIGN_UP((uint64_t)MAX_W * MAX_H * 4, PAGE_SIZE);
    spin_lock(&lock);
    if (!vram_phys && (vram_phys = pmm_alloc(size / PAGE_SIZE)))
        memset(phys_to_virt(vram_phys), 0, size);
    struct ramfb_cfg c = {__builtin_bswap64(vram_phys), __builtin_bswap32(DRM_FORMAT_XRGB8888), 0,
                          __builtin_bswap32(w), __builtin_bswap32(h), __builtin_bswap32(w * 4)};
    bool ok = vram_phys && fwcfg_write(key, &c, sizeof c);
    spin_unlock(&lock);
    if (!ok)
        return false;
    *phys = vram_phys;
    *vram_bytes = size;
    return true;
}
