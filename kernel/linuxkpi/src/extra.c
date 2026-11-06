#include <lkpi/all.h>
#include "../host.h"

/* kernel/linuxkpi: the firmware as Linux describes it (ACPI, EFI, the boot framebuffer, DMI), the console, and
 * starting the layer and NVIDIA's driver. ACPI's methods answer "not found" until an AML interpreter is joined:
 * the driver then does without them (its _DSM power and backlight calls are for laptops' hybrid graphics). */

/* ---- ACPI ---- */

struct acpi_table_fadt acpi_gbl_FADT;

acpi_status acpi_evaluate_object(acpi_handle h, const char *path, struct acpi_object_list *args, struct acpi_buffer *out) {
    return AE_NOT_FOUND;
}

acpi_status acpi_evaluate_integer(acpi_handle h, const char *path, struct acpi_object_list *args, unsigned long long *v) {
    return AE_NOT_FOUND;
}

acpi_status acpi_get_handle(acpi_handle parent, const char *path, acpi_handle *out) {
    *out = NULL;
    return AE_NOT_FOUND;
}

acpi_status acpi_get_parent(acpi_handle h, acpi_handle *out) {
    *out = NULL;
    return AE_NOT_FOUND;
}

acpi_status acpi_get_name(acpi_handle h, u32 type, struct acpi_buffer *out) { return AE_NOT_FOUND; }
acpi_status acpi_install_notify_handler(acpi_handle h, u32 type, acpi_notify_handler fn, void *ctx) { return AE_OK; }
acpi_status acpi_remove_notify_handler(acpi_handle h, u32 type, acpi_notify_handler fn) { return AE_OK; }

acpi_status acpi_walk_namespace(acpi_object_type type, acpi_handle start, u32 depth, acpi_walk_callback down,
                                acpi_walk_callback up, void *ctx, void **ret) {
    return AE_OK; /* (nothing to walk: no namespace without the interpreter) */
}

bool acpi_has_method(acpi_handle h, const char *name) { return false; }
union acpi_object *acpi_evaluate_dsm(acpi_handle h, const void *guid, u64 rev, u64 func, union acpi_object *argv) { return NULL; }
bool acpi_check_dsm(acpi_handle h, const void *guid, u64 rev, u64 funcs) { return false; }
int register_acpi_notifier(struct notifier_block *nb) { return 0; }
int unregister_acpi_notifier(struct notifier_block *nb) { return 0; }
int acpi_bus_get_device_lkpi(acpi_handle h, struct acpi_device **out) { return -ENODEV; }

/* ---- EFI, the boot framebuffer, DMI ---- */

struct sysfb_display_info sysfb_primary_display;
struct screen_info screen_info;

bool efi_enabled(int feature) { return feature == EFI_BOOT && kpi_host_efi(); }

static void framebuffer_init(void) {
    u64 phys;
    u32 w, h, pitch;
    u16 bpp;
    if (!kpi_host_framebuffer(&phys, &w, &h, &pitch, &bpp))
        return;
    struct screen_info *si = &sysfb_primary_display.screen;
    si->orig_video_isVGA = kpi_host_efi() ? VIDEO_TYPE_EFI : VIDEO_TYPE_VLFB;
    si->lfb_base = (u32)phys;
    si->ext_lfb_base = (u32)(phys >> 32);
    if (phys >> 32)
        si->capabilities |= VIDEO_CAPABILITY_64BIT_BASE;
    si->lfb_width = (u16)w, si->lfb_height = (u16)h, si->lfb_depth = bpp, si->lfb_linelength = (u16)pitch;
    si->lfb_size = pitch * h;
    screen_info = *si;
}

const char *dmi_get_system_info(int field) {
    static char chassis[8];
    switch (field) {
    case DMI_SYS_VENDOR: return kpi_host_smbios_string(1, 4);
    case DMI_PRODUCT_NAME: return kpi_host_smbios_string(1, 5);
    case DMI_BOARD_VENDOR: return kpi_host_smbios_string(2, 4);
    case DMI_BOARD_NAME: return kpi_host_smbios_string(2, 5);
    case DMI_CHASSIS_TYPE: {
        int t = kpi_host_smbios_byte(3, 5);
        if (t < 0)
            return NULL;
        snprintf(chassis, sizeof chassis, "%d", t & 0x7f);
        return chassis;
    }
    default: return NULL;
    }
}

/* ---- odds and ends ---- */

static DEFINE_MUTEX(console_mutex);
void console_lock(void) { mutex_lock(&console_mutex); }
void console_unlock(void) { mutex_unlock(&console_mutex); }
int numa_mem_id(void) { return 0; }
struct dev_pagemap *lkpi_get_dev_pagemap(unsigned long pfn) { return NULL; /* (no device memory in struct pages here) */ }

/* ---- starting ---- */

void lkpi_mm_init(void);
void lkpi_fs_init(void);
void lkpi_sched_init(void);
int lkpi_module_init_nvidia_init_module(void);

static void fadt_init(void) {
    const u8 *t = kpi_host_acpi_table("FACP", 0);
    u32 len;
    if (!t)
        return;
    memcpy(&len, t + 4, 4);
    if (len >= 116)
        memcpy(&acpi_gbl_FADT.flags, t + 112, 4);
}

/* The layer, then NVIDIA's driver (its module init: the character devices, then the PCI driver, which probes the
 * GPUs). On a thread of its own: probing sleeps, and reads the GSP firmware from /lib/firmware. */
int lkpi_start(void) {
    kpi_host_log("linuxkpi: memory\n", 17);
    lkpi_mm_init();
    printk("linuxkpi: threads\n");
    lkpi_sched_init();
    printk("linuxkpi: files\n");
    lkpi_fs_init();
    fadt_init();
    framebuffer_init();
    printk("linuxkpi: starting NVIDIA's driver\n");
    int r = lkpi_module_init_nvidia_init_module();
    printk("linuxkpi: NVIDIA's driver %s (%d)\n", r ? "did not start" : "started", r);
    return r;
}
