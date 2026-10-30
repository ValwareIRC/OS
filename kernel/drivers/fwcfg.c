#include <kernel/lib.h>
#include <kernel/platform.h>

/* QEMU's fw_cfg interface: lets the test harness pass named values
 * (-fw_cfg name=opt/os/...,string=...) without a kernel command line.
 * Harmless on real hardware: the signature check fails and nothing is read.
 * The directory contents are big-endian. */

#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_FILE_DIR  0x0019

spinlock_t fwcfg_lock = SPINLOCK_INIT;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int find_file(const char *name, uint16_t *key) {
    char sig[4];
    fwcfg_select(FW_CFG_SIGNATURE);
    fwcfg_read(sig, 4);
    if (memcmp(sig, "QEMU", 4))
        return -1;
    uint8_t cnt[4];
    fwcfg_select(FW_CFG_FILE_DIR);
    fwcfg_read(cnt, 4);
    uint32_t count = be32(cnt);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t ent[64];
        fwcfg_read(ent, sizeof ent);
        if (strcmp((const char *)ent + 8, name))
            continue;
        *key = (uint16_t)(ent[4] << 8 | ent[5]);
        return (int)be32(ent);
    }
    return -1;
}

int fwcfg_find_file(const char *name, uint16_t *key) {
    spin_lock(&fwcfg_lock);
    int fsize = find_file(name, key);
    spin_unlock(&fwcfg_lock);
    return fsize;
}

int fwcfg_read_file(const char *name, char *buf, size_t size) {
    uint16_t key;
    spin_lock(&fwcfg_lock);
    int fsize = find_file(name, &key);
    if (fsize < 0) {
        spin_unlock(&fwcfg_lock);
        return -1;
    }
    size_t n = (size_t)fsize < size - 1 ? (size_t)fsize : size - 1;
    fwcfg_select(key);
    fwcfg_read(buf, n);
    spin_unlock(&fwcfg_lock);
    buf[n] = 0;
    return (int)n;
}

/* A file of any size, handed to `sink` a piece at a time; its size, or -1.
 * By DMA where the machine offers it: a byte at a time through a port, a
 * file of megabytes (her VRM model) is millions of exits, which starves the
 * Windows QEMU's window until it freezes. Each piece selects the file again
 * and skips what was read, so the lock is never held while `sink` runs. */
#include <kernel/boot.h>
#include <kernel/mm.h>

#define DMA_PAGES 64 /* 256 KiB a request */
#define DMA_ERROR 0x01
#define DMA_READ 0x02
#define DMA_SKIP 0x04
#define DMA_SELECT 0x08

struct fw_dma {
    uint32_t control, length;
    uint64_t address;
};

static bool dma_run(volatile struct fw_dma *d, uint64_t phys, uint32_t control, uint32_t len, uint64_t addr) {
    d->control = __builtin_bswap32(control);
    d->length = __builtin_bswap32(len);
    d->address = __builtin_bswap64(addr);
    return fwcfg_dma_kick(phys) && !(__builtin_bswap32(d->control) & DMA_ERROR);
}

long fwcfg_stream_file(const char *name, void (*sink)(void *ctx, const char *p, size_t n), void *ctx) {
    uint16_t key;
    int fsize = fwcfg_find_file(name, &key);
    if (fsize < 0)
        return -1;
    static uint64_t phys;
    if (!phys && fwcfg_dma_available())
        phys = pmm_alloc(DMA_PAGES + 1);
    if (phys) {
        volatile struct fw_dma *d = phys_to_virt(phys);
        char *chunk = phys_to_virt(phys + 4096);
        bool ok = true;
        for (uint32_t done = 0; done < (uint32_t)fsize && ok;) {
            uint32_t n = (uint32_t)fsize - done < DMA_PAGES * 4096u ? (uint32_t)fsize - done : DMA_PAGES * 4096u;
            spin_lock(&fwcfg_lock);
            uint32_t sel = DMA_SELECT | (uint32_t)key << 16;
            ok = done ? dma_run(d, phys, sel | DMA_SKIP, done, 0) && dma_run(d, phys, DMA_READ, n, phys + 4096)
                      : dma_run(d, phys, sel | DMA_READ, n, phys + 4096);
            spin_unlock(&fwcfg_lock);
            if (ok) {
                sink(ctx, chunk, n);
                done += n;
            }
        }
        if (ok)
            return fsize;
    }
    /* no DMA: the slow way, a piece at a time */
    static char piece[65536];
    for (uint32_t done = 0; done < (uint32_t)fsize;) {
        uint32_t n = (uint32_t)fsize - done < sizeof piece ? (uint32_t)fsize - done : (uint32_t)sizeof piece;
        spin_lock(&fwcfg_lock);
        fwcfg_select(key);
        for (uint32_t skip = done; skip > 0;) {
            uint32_t k = skip < sizeof piece ? skip : (uint32_t)sizeof piece;
            fwcfg_read(piece, k);
            skip -= k;
        }
        fwcfg_read(piece, n);
        spin_unlock(&fwcfg_lock);
        sink(ctx, piece, n);
        done += n;
    }
    return fsize;
}
