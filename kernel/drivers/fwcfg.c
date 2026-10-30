#include <kernel/cpu.h>
#include <kernel/lib.h>

/* QEMU's fw_cfg interface: lets the test harness pass named values
 * (-fw_cfg name=opt/os/...,string=...) without a kernel command line.
 * Harmless on real hardware: the signature check fails and nothing is read. */

#define FW_CFG_SELECTOR 0x510
#define FW_CFG_DATA     0x511
#define FW_CFG_SIGNATURE 0x0000
#define FW_CFG_FILE_DIR  0x0019

/* The selector register takes a little-endian 16-bit write on x86; the
 * directory contents are big-endian. */
static void select(uint16_t key) { outw(FW_CFG_SELECTOR, key); }

static void read_bytes(void *buf, size_t len) {
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++)
        b[i] = inb(FW_CFG_DATA);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int fwcfg_read_file(const char *name, char *buf, size_t size);
int fwcfg_read_file(const char *name, char *buf, size_t size) {
    char sig[4];
    select(FW_CFG_SIGNATURE);
    read_bytes(sig, 4);
    if (memcmp(sig, "QEMU", 4))
        return -1;
    uint8_t cnt[4];
    select(FW_CFG_FILE_DIR);
    read_bytes(cnt, 4);
    uint32_t count = be32(cnt);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t ent[64];
        read_bytes(ent, sizeof ent);
        if (strcmp((const char *)ent + 8, name))
            continue;
        uint32_t fsize = be32(ent);
        uint16_t key = (uint16_t)(ent[4] << 8 | ent[5]);
        size_t n = fsize < size - 1 ? fsize : size - 1;
        select(key);
        read_bytes(buf, n);
        buf[n] = 0;
        return (int)n;
    }
    return -1;
}

#include <kernel/boot.h>
#include <kernel/mm.h>

#define FW_CFG_DMA 0x514
#define FW_CFG_ID 0x0001
#define DMA_PAGES 64 /* 256 KiB a request */

struct fw_dma {
    uint32_t control, length;
    uint64_t address;
} __attribute__((packed));

static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

/* QEMU says (bit 1 of its id) whether it takes DMA requests */
static bool dma_ok(void) {
    uint8_t id[4];
    select(FW_CFG_ID);
    read_bytes(id, 4);
    return (id[0] & 2) != 0;
}

/* A file of any size, handed to `sink` a piece at a time. Its size, or -1. */
long fwcfg_stream_file(const char *name, void (*sink)(void *ctx, const char *p, size_t n), void *ctx);
long fwcfg_stream_file(const char *name, void (*sink)(void *ctx, const char *p, size_t n), void *ctx) {
    char sig[4];
    select(FW_CFG_SIGNATURE);
    read_bytes(sig, 4);
    if (memcmp(sig, "QEMU", 4))
        return -1;
    uint8_t cnt[4];
    select(FW_CFG_FILE_DIR);
    read_bytes(cnt, 4);
    uint32_t count = be32(cnt);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t ent[64];
        read_bytes(ent, sizeof ent);
        if (strcmp((const char *)ent + 8, name))
            continue;
        uint32_t fsize = be32(ent);
        uint16_t key = (uint16_t)(ent[4] << 8 | ent[5]);
        /* By DMA when QEMU offers it: a byte at a time through the port, a
         * file of megabytes is millions of exits, which starves the Windows
         * QEMU's window until it freezes. */
        if (dma_ok()) {
            static uint64_t phys;
            if (!phys)
                phys = pmm_alloc(DMA_PAGES + 1);
            if (phys) {
                volatile struct fw_dma *d = phys_to_virt(phys);
                char *chunk = phys_to_virt(phys + 4096);
                bool ok = true;
                for (uint32_t done = 0; done < fsize && ok;) {
                    uint32_t n = fsize - done < DMA_PAGES * 4096u ? fsize - done : DMA_PAGES * 4096u;
                    uint32_t ctl = 0x02 | (done == 0 ? (0x08u | (uint32_t)key << 16) : 0); /* read (and select first) */
                    d->control = bswap32(ctl);
                    d->length = bswap32(n);
                    d->address = bswap64(phys + 4096);
                    __asm__ volatile("mfence" ::: "memory");
                    outl(FW_CFG_DMA + 0, bswap32((uint32_t)(phys >> 32)));
                    outl(FW_CFG_DMA + 4, bswap32((uint32_t)phys));
                    for (int spin = 0; spin < 100000000; spin++) {
                        uint32_t c = bswap32(d->control);
                        if (!(c & ~1u))
                            break;
                        __asm__ volatile("pause");
                    }
                    if (bswap32(d->control) & 1u)
                        ok = false;
                    else {
                        sink(ctx, chunk, n);
                        done += n;
                    }
                }
                if (ok)
                    return (long)fsize;
            }
        }
        static char chunk[65536];
        select(key);
        for (uint32_t done = 0; done < fsize;) {
            size_t n = fsize - done < sizeof chunk ? fsize - done : sizeof chunk;
            read_bytes(chunk, n);
            sink(ctx, chunk, n);
            done += (uint32_t)n;
        }
        return (long)fsize;
    }
    return -1;
}
