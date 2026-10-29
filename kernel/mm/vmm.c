#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/cpu.h>
#include <kernel/spinlock.h>
#include <kernel/lib.h>
#include <kernel/mm.h>

#define HUGE_PAGE (2UL * 1024 * 1024)

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[],
    __data_start[], __kernel_end[];

static uint64_t *kernel_pml4;
static bool have_nx;

static uint64_t *alloc_table(void) {
    uint64_t phys = pmm_alloc(1);
    if (!phys)
        return NULL;
    uint64_t *t = phys_to_virt(phys);
    memset(t, 0, PAGE_SIZE);
    return t;
}

/* Returns the table that `level` (3 = PDPT, 2 = PD, 1 = PT) entries for
 * `virt` live in, creating missing intermediate tables. */
static uint64_t *walk(uint64_t *pml4, uint64_t virt, int level, uint64_t flags) {
    uint64_t *table = pml4;
    for (int l = 4; l > level; l--) {
        uint64_t idx = (virt >> (12 + 9 * (l - 1))) & 0x1ff;
        uint64_t e = table[idx];
        if (!(e & PTE_PRESENT)) {
            uint64_t *next = alloc_table();
            if (!next)
                return NULL;
            e = virt_to_phys_hhdm(next) | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
            table[idx] = e;
        } else if (e & PTE_HUGE) {
            return NULL; /* splitting huge pages is not supported yet */
        } else if (flags & PTE_USER) {
            table[idx] = e | PTE_USER;
        }
        table = phys_to_virt(e & PTE_ADDR);
    }
    return table;
}

bool vmm_map(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pt = walk(pml4, virt, 1, flags);
    if (!pt)
        return false;
    pt[(virt >> 12) & 0x1ff] = (phys & PTE_ADDR) | flags | PTE_PRESENT;
    return true;
}

bool vmm_map_huge(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pd = walk(pml4, virt, 2, flags);
    if (!pd)
        return false;
    pd[(virt >> 21) & 0x1ff] = (phys & ~(HUGE_PAGE - 1) & PTE_ADDR) | flags | PTE_PRESENT | PTE_HUGE;
    return true;
}

uint64_t vmm_translate(uint64_t *pml4, uint64_t virt) {
    uint64_t *table = pml4;
    for (int l = 4; l >= 1; l--) {
        uint64_t e = table[(virt >> (12 + 9 * (l - 1))) & 0x1ff];
        if (!(e & PTE_PRESENT))
            return 0;
        if (l == 2 && (e & PTE_HUGE))
            return (e & PTE_ADDR & ~(HUGE_PAGE - 1)) + (virt & (HUGE_PAGE - 1));
        if (l == 1)
            return (e & PTE_ADDR) + (virt & 0xfff);
        table = phys_to_virt(e & PTE_ADDR);
    }
    return 0;
}

uint64_t *vmm_kernel_pml4(void) { return kernel_pml4; }

static spinlock_t kernel_map_lock = SPINLOCK_INIT;

void vmm_map_mmio(uint64_t phys, uint64_t len, bool uncached) {
    uint64_t flags = PTE_WRITE | PTE_GLOBAL | (have_nx ? PTE_NX : 0) | (uncached ? PTE_PCD | PTE_PWT : 0);
    uint64_t f = spin_lock_irqsave(&kernel_map_lock);
    for (uint64_t p = ALIGN_DOWN(phys, PAGE_SIZE); p < phys + len; p += PAGE_SIZE)
        if (!vmm_translate(kernel_pml4, p + hhdm_offset) && !vmm_map(kernel_pml4, p + hhdm_offset, p, flags))
            panic("vmm: cannot map MMIO page %lx", p);
    spin_unlock_irqrestore(&kernel_map_lock, f);
}

static void map_kernel_range(uint64_t phys_base, uint64_t virt_base, char *start, char *end, uint64_t flags) {
    for (uint64_t v = ALIGN_DOWN((uint64_t)start, PAGE_SIZE); v < (uint64_t)end; v += PAGE_SIZE)
        if (!vmm_map(kernel_pml4, v, v - virt_base + phys_base, flags | PTE_GLOBAL))
            panic("vmm: cannot map kernel page %lx", v);
}

void vmm_init(void) {
    struct limine_executable_address_response *ka = boot_kaddr_request.response;
    struct limine_memmap_response *mm = boot_memmap_request.response;
    KASSERT(ka && mm);

    uint32_t a, b, c, d;
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    have_nx = d & (1u << 20);
    cpu_enable_features();
    uint64_t nx = have_nx ? PTE_NX : 0;

    kernel_pml4 = alloc_table();
    KASSERT(kernel_pml4);

    /* Kernel image, W^X: code is read-only + executable, rodata read-only,
     * everything else read/write but never executable. */
    uint64_t pb = ka->physical_base, vb = ka->virtual_base;
    map_kernel_range(pb, vb, __kernel_start, __text_start, PTE_WRITE | nx);
    map_kernel_range(pb, vb, __text_start, __text_end, 0);
    map_kernel_range(pb, vb, __rodata_start, __rodata_end, nx);
    map_kernel_range(pb, vb, __data_start, __kernel_end, PTE_WRITE | nx);

    /* Direct map of all physical memory the bootloader told us about. */
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_BAD_MEMORY)
            continue;
        uint64_t flags = PTE_WRITE | nx;
        if (e->type == LIMINE_MEMMAP_FRAMEBUFFER)
            flags |= PTE_PWT; /* write-through until PAT write-combining exists */
        for (uint64_t p = ALIGN_DOWN(e->base, HUGE_PAGE); p < e->base + e->length; p += HUGE_PAGE)
            if (!vmm_map_huge(kernel_pml4, p + hhdm_offset, p, flags))
                panic("vmm: cannot direct-map %lx", p);
    }

    /* Pre-create every upper-half PML4 slot so user address spaces, which
     * copy these 256 entries once, see all future kernel mappings. */
    for (int i = 256; i < 512; i++) {
        if (!(kernel_pml4[i] & PTE_PRESENT)) {
            uint64_t *t = alloc_table();
            KASSERT(t);
            kernel_pml4[i] = virt_to_phys_hhdm(t) | PTE_PRESENT | PTE_WRITE;
        }
    }

    write_cr3(virt_to_phys_hhdm(kernel_pml4));
}
