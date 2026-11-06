#include <asm/io.h>
#include <kernel/arch.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>
#include <kernel/spinlock.h>

/* PCI configuration access: the first 256 bytes through the legacy 0xCF8/0xCFC ports, which every x86 PC still
 * provides (an address, then the data: one CPU at a time, or one's address lands under another's read); PCIe's
 * extended space (256..4095: resizable BARs, vendor capabilities) through ECAM, the memory window the ACPI MCFG
 * table gives, each function's 4 KiB mapped uncached the first time it is touched. */

static spinlock_t port_lock = SPINLOCK_INIT;

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xfc);
}

struct mcfg_entry {
    uint64_t base;
    uint16_t segment;
    uint8_t start_bus, end_bus;
    uint32_t reserved;
} __attribute__((packed));

static uint64_t ecam_base;
static uint8_t ecam_start, ecam_end;
static volatile bool ecam_looked;
static spinlock_t ecam_lock = SPINLOCK_INIT;
static volatile uint32_t **ecam_bus[256]; /* per bus: each device/function's mapped page, once mapped */

static void ecam_find(void) {
    const uint8_t *t = acpi_find_table("MCFG", 0);
    if (t) {
        uint32_t len;
        memcpy(&len, t + 4, 4);
        for (uint32_t at = 44; at + sizeof(struct mcfg_entry) <= len; at += sizeof(struct mcfg_entry)) {
            struct mcfg_entry e;
            memcpy(&e, t + at, sizeof e);
            if (e.segment == 0) {
                ecam_base = e.base, ecam_start = e.start_bus, ecam_end = e.end_bus;
                break;
            }
        }
    }
    if (ecam_base)
        kprintf("pci: extended config space at %lx (buses %u-%u)\n", ecam_base, ecam_start, ecam_end);
    ecam_looked = true;
}

/* the function's config page, or NULL: no ECAM, or a bus it doesn't cover */
static volatile uint32_t *ecam(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint64_t f = spin_lock_irqsave(&ecam_lock);
    if (!ecam_looked)
        ecam_find();
    volatile uint32_t *page = NULL;
    if (ecam_base && bus >= ecam_start && bus <= ecam_end && dev < 32 && fn < 8) {
        if (!ecam_bus[bus])
            ecam_bus[bus] = kzalloc(256 * sizeof(void *));
        if (ecam_bus[bus]) {
            unsigned i = (unsigned)dev * 8 + fn;
            if (!ecam_bus[bus][i])
                ecam_bus[bus][i] = vmm_ioremap(ecam_base + ((uint64_t)(bus - ecam_start) << 20) + ((uint64_t)i << 12),
                                               PAGE_SIZE);
            page = ecam_bus[bus][i];
        }
    }
    spin_unlock_irqrestore(&ecam_lock, f);
    return page;
}

uint32_t platform_pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    if (off >= 256) {
        volatile uint32_t *p = off < 4096 ? ecam(bus, dev, fn) : NULL;
        return p ? p[(off & 0xffc) / 4] : 0xffffffffu;
    }
    uint64_t f = spin_lock_irqsave(&port_lock);
    outl(0xcf8, cfg_addr(bus, dev, fn, off));
    uint32_t v = inl(0xcfc);
    spin_unlock_irqrestore(&port_lock, f);
    return v;
}

void platform_pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t v) {
    if (off >= 256) {
        volatile uint32_t *p = off < 4096 ? ecam(bus, dev, fn) : NULL;
        if (p)
            p[(off & 0xffc) / 4] = v;
        return;
    }
    uint64_t f = spin_lock_irqsave(&port_lock);
    outl(0xcf8, cfg_addr(bus, dev, fn, off));
    outl(0xcfc, v);
    spin_unlock_irqrestore(&port_lock, f);
}

uint32_t platform_pci_io_read(uint16_t port, unsigned size) {
    return size == 1 ? inb(port) : size == 2 ? inw(port) : inl(port);
}

void platform_pci_io_write(uint16_t port, unsigned size, uint32_t v) {
    if (size == 1)
        outb(port, (uint8_t)v);
    else if (size == 2)
        outw(port, (uint16_t)v);
    else
        outl(port, v);
}
