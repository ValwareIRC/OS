#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>

#include "../host.h"

/* What the firmware told us, for the layer: UEFI or BIOS, ACPI's tables, the framebuffer the boot picture is in,
 * and SMBIOS's structures (the driver's quirks go by the machine's maker and model). */

bool kpi_host_efi(void) {
    struct limine_firmware_type_response *r = boot_firmware_request.response;
    return r && (r->firmware_type == LIMINE_FIRMWARE_TYPE_EFI64 || r->firmware_type == LIMINE_FIRMWARE_TYPE_EFI32);
}

const void *kpi_host_acpi_table(const char *sig, int index) { return acpi_find_table(sig, index); }

bool kpi_host_framebuffer(uint64_t *phys, uint32_t *width, uint32_t *height, uint32_t *pitch, uint16_t *bpp) {
    struct limine_framebuffer_response *r = boot_fb_request.response;
    if (!r || !r->framebuffer_count)
        return false;
    struct limine_framebuffer *fb = r->framebuffers[0];
    uint64_t a = (uint64_t)fb->address;
    *phys = a >= hhdm_offset ? a - hhdm_offset : a;
    *width = (uint32_t)fb->width, *height = (uint32_t)fb->height, *pitch = (uint32_t)fb->pitch, *bpp = fb->bpp;
    return true;
}

/* ---- SMBIOS ---- */

static const uint8_t *table;
static uint32_t table_len;
static bool looked;

static const uint8_t *map(uint64_t phys, uint64_t len) {
    if (phys >= hhdm_offset)
        phys -= hhdm_offset; /* (older protocol revisions give a virtual address) */
    vmm_map_mmio(phys, len, false);
    return phys_to_virt(phys);
}

static void find_table(void) {
    looked = true;
    struct limine_smbios_response *r = boot_smbios_request.response;
    if (!r)
        return;
    if (r->entry_64) {
        const uint8_t *e = map((uint64_t)r->entry_64, 24);
        if (!memcmp(e, "_SM3_", 5)) {
            uint32_t len;
            uint64_t at;
            memcpy(&len, e + 0x0c, 4);
            memcpy(&at, e + 0x10, 8);
            table = map(at, len), table_len = len;
            return;
        }
    }
    if (r->entry_32) {
        const uint8_t *e = map((uint64_t)r->entry_32, 32);
        if (!memcmp(e, "_SM_", 4) && !memcmp(e + 0x10, "_DMI_", 5)) {
            uint16_t len;
            uint32_t at;
            memcpy(&len, e + 0x16, 2);
            memcpy(&at, e + 0x18, 4);
            table = map(at, len), table_len = len;
        }
    }
}

/* the first structure of `type`: its start and (through *strings) where its strings begin */
static const uint8_t *structure(int type, const uint8_t **strings) {
    if (!looked)
        find_table();
    const uint8_t *p = table, *end = table ? table + table_len : NULL;
    while (p && p + 4 <= end) {
        uint8_t t = p[0], len = p[1];
        if (len < 4)
            return NULL;
        const uint8_t *s = p + len;
        const uint8_t *next = s;
        while (next + 1 < end && (next[0] || next[1]))
            next++;
        next += 2;
        if (t == type) {
            *strings = s;
            return p;
        }
        if (t == 127) /* end of table */
            return NULL;
        p = next;
    }
    return NULL;
}

int kpi_host_smbios_byte(int type, int offset) {
    const uint8_t *strings, *s = structure(type, &strings);
    return s && offset < s[1] ? s[offset] : -1;
}

const char *kpi_host_smbios_string(int type, int offset) {
    const uint8_t *strings, *s = structure(type, &strings);
    if (!s || offset >= s[1] || !s[offset])
        return NULL;
    const char *str = (const char *)strings;
    for (int n = 1; n < s[offset]; n++) {
        if (!*str)
            return NULL;
        str += strlen(str) + 1;
    }
    return *str ? str : NULL;
}
