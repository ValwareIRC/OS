#pragma once
/* kernel/linuxkpi: ACPI as NVIDIA's glue sees it (ACPICA's interface: handles, objects, evaluating a method). The
 * driver uses it for laptop extras (switchable graphics, power limits, the backlight); without them a desktop card
 * runs as usual. Our kernel reads the firmware's tables but has no interpreter for their methods yet, so these
 * answer "not found" until one is joined here (docs/nvidia/plan.md: uACPI). */
#include <lkpi/core.h>
#include <lkpi/device.h>

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef u32 acpi_object_type;
typedef u64 acpi_size;
typedef u64 acpi_integer;
typedef char acpi_string_lkpi;
#define AE_OK 0x0000
#define AE_ERROR 0x0001
#define AE_NO_ACPI_TABLES 0x0002
#define AE_NO_MEMORY 0x0004
#define AE_NOT_FOUND 0x0005
#define AE_NOT_EXIST 0x0006
#define AE_BAD_PARAMETER 0x1001
#define AE_SUPPORT 0x0007
#define AE_TYPE 0x000E
#define ACPI_SUCCESS(s) ((s) == AE_OK)
#define ACPI_FAILURE(s) ((s) != AE_OK)
#define ACPI_TYPE_ANY 0x00
#define ACPI_TYPE_INTEGER 0x01
#define ACPI_TYPE_STRING 0x02
#define ACPI_TYPE_BUFFER 0x03
#define ACPI_TYPE_PACKAGE 0x04
#define ACPI_TYPE_DEVICE 0x06
#define ACPI_TYPE_METHOD 0x08
#define ACPI_TYPE_LOCAL_REFERENCE 0x14
#define ACPI_ALLOCATE_BUFFER ((acpi_size)-1)
#define ACPI_UINT32_MAX 0xffffffffu
#define ACPI_ROOT_OBJECT ((acpi_handle)NULL)
#define ACPI_SYSTEM_NOTIFY 0x1
#define ACPI_DEVICE_NOTIFY 0x2
#define ACPI_ALL_NOTIFY 0x3
#define ACPI_FULL_PATHNAME 0
#define ACPI_HANDLE(d) ((acpi_handle)NULL)
#define ACPI_COMPANION(d) ((struct acpi_device *)NULL)
#define ACPI_VIDEO_HID "LNXVIDEO"
union acpi_object {
    acpi_object_type type;
    struct { acpi_object_type type; u64 value; } integer;
    struct { acpi_object_type type; u32 length; char *pointer; } string;
    struct { acpi_object_type type; u32 length; u8 *pointer; } buffer;
    struct { acpi_object_type type; u32 count; union acpi_object *elements; } package;
    struct { acpi_object_type type; acpi_object_type actual_type; acpi_handle handle; } reference;
};
struct acpi_buffer { acpi_size length; void *pointer; };
struct acpi_object_list { u32 count; union acpi_object *pointer; };
struct acpi_device { acpi_handle handle; struct device dev; struct list_head children; struct acpi_device *parent; };
struct acpi_bus_event { struct list_head node; char device_class[20]; char bus_id[15]; u32 type; u32 data; };
typedef void (*acpi_notify_handler)(acpi_handle h, u32 event, void *context);
typedef acpi_status (*acpi_walk_callback)(acpi_handle h, u32 level, void *context, void **ret);
acpi_status acpi_evaluate_object(acpi_handle h, const char *path, struct acpi_object_list *args, struct acpi_buffer *out);
acpi_status acpi_evaluate_integer(acpi_handle h, const char *path, struct acpi_object_list *args, unsigned long long *v);
acpi_status acpi_get_handle(acpi_handle parent, const char *path, acpi_handle *out);
acpi_status acpi_get_parent(acpi_handle h, acpi_handle *out);
acpi_status acpi_get_name(acpi_handle h, u32 type, struct acpi_buffer *out);
acpi_status acpi_install_notify_handler(acpi_handle h, u32 type, acpi_notify_handler fn, void *ctx);
acpi_status acpi_remove_notify_handler(acpi_handle h, u32 type, acpi_notify_handler fn);
acpi_status acpi_walk_namespace(acpi_object_type type, acpi_handle start, u32 depth, acpi_walk_callback down,
                                acpi_walk_callback up, void *ctx, void **ret);
bool acpi_has_method(acpi_handle h, const char *name);
union acpi_object *acpi_evaluate_dsm(acpi_handle h, const void *guid, u64 rev, u64 func, union acpi_object *argv);
bool acpi_check_dsm(acpi_handle h, const void *guid, u64 rev, u64 funcs);
int register_acpi_notifier(struct notifier_block *nb);
int unregister_acpi_notifier(struct notifier_block *nb);
int acpi_bus_get_device_lkpi(acpi_handle h, struct acpi_device **out);
struct acpi_device *acpi_fetch_acpi_dev(acpi_handle h);
bool is_acpi_device_node(const struct fwnode_handle *f);
extern int acpi_disabled;
#define acpi_handle_info(h, fmt, ...) printk(KERN_INFO "ACPI: " fmt, ##__VA_ARGS__)
#define acpi_handle_debug(h, fmt, ...) do { } while (0)
#define ACPI_FREE(p) kfree(p)
int acpi_video_backlight_use_native(void);
#define acpi_video_register_backlight() do { } while (0)
#define acpi_video_unregister() do { } while (0)
typedef struct { u8 b[16]; } guid_t;
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) ((guid_t){{ (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
    (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})
