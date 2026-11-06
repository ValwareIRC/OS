#pragma once
/* kernel/linuxkpi: devices, PCI, DMA, interrupts and power management, as NVIDIA's glue sees them. A struct pci_dev
 * here is the layer's view of one of our kernel's PCI devices (kernel/include/kernel/pci.h), made when the driver
 * registers and its ID table matches; DMA is direct (bus address = physical address: no IOMMU in our kernel). */
#include <lkpi/core.h>
#include <lkpi/mm.h>

/* (our kernel has a pci_enable_msi of its own, of another shape: the layer's is renamed, as kmalloc is) */
#define pci_enable_msi lkpi_pci_enable_msi

/* ---- the device model ---- */
struct device;
struct device_driver;
struct kobject { const char *name; struct kobject *parent; struct kref kref; };
struct fwnode_handle;
struct device_node { const char *name; const char *full_name; struct fwnode_handle *fwnode; };
struct of_phandle_args { struct device_node *np; int args_count; u32 args[16]; };
struct dev_pm_ops {
    int (*prepare)(struct device *d);
    void (*complete)(struct device *d);
    int (*suspend)(struct device *d);
    int (*resume)(struct device *d);
    int (*freeze)(struct device *d);
    int (*thaw)(struct device *d);
    int (*poweroff)(struct device *d);
    int (*restore)(struct device *d);
    int (*suspend_noirq)(struct device *d);
    int (*resume_noirq)(struct device *d);
    int (*freeze_noirq)(struct device *d);
    int (*thaw_noirq)(struct device *d);
    int (*poweroff_noirq)(struct device *d);
    int (*restore_noirq)(struct device *d);
    int (*runtime_suspend)(struct device *d);
    int (*runtime_resume)(struct device *d);
    int (*runtime_idle)(struct device *d);
};
enum probe_type { PROBE_DEFAULT_STRATEGY, PROBE_PREFER_ASYNCHRONOUS, PROBE_FORCE_SYNCHRONOUS };
struct of_device_id { char name[32]; char type[32]; char compatible[128]; const void *data; };
struct device_driver { const char *name; const struct dev_pm_ops *pm; void *owner; enum probe_type probe_type;
                       const struct of_device_id *of_match_table; };
struct dev_pm_info { bool runtime_auto; atomic_t usage_count; spinlock_t lock; bool is_suspended; int runtime_status; };
struct dev_pm_domain { struct dev_pm_ops ops; };
struct bus_type { const char *name; void (*shutdown)(struct device *d); };
struct class { char name[32]; char *(*devnode)(const struct device *d, umode_t *mode); };
struct device {
    struct device *parent;
    struct kobject kobj;
    const char *init_name;
    struct device_driver *driver;
    void *driver_data;
    struct dev_pm_info power;
    u64 *dma_mask;
    u64 coherent_dma_mask;
    u64 bus_dma_limit;
    int numa_node;
    struct device_node *of_node;
    struct fwnode_handle *fwnode;
    const struct bus_type *bus;
    struct class *class;
    dev_t devt;
    void (*release)(struct device *d);
    bool dma_skip_sync;
    void *iommu_group;
    struct dev_pm_domain *pm_domain;
};
static inline void *dev_get_drvdata(const struct device *d) { return d->driver_data; }
static inline void dev_set_drvdata(struct device *d, void *p) { d->driver_data = p; }
static inline int dev_to_node(struct device *d) { return d ? d->numa_node : NUMA_NO_NODE; }
static inline void set_dev_node(struct device *d, int n) { d->numa_node = n; }
const char *dev_name(const struct device *d);
const char *dev_driver_string(const struct device *d);
#define dev_err(d, fmt, ...) printk(KERN_ERR "%s: " fmt, dev_name(d), ##__VA_ARGS__)
#define dev_warn(d, fmt, ...) printk(KERN_WARNING "%s: " fmt, dev_name(d), ##__VA_ARGS__)
#define dev_info(d, fmt, ...) printk(KERN_INFO "%s: " fmt, dev_name(d), ##__VA_ARGS__)
#define dev_notice(d, fmt, ...) printk(KERN_NOTICE "%s: " fmt, dev_name(d), ##__VA_ARGS__)
#define dev_dbg(d, fmt, ...) do { } while (0)
#define _dev_info(d, fmt, ...) dev_info(d, fmt, ##__VA_ARGS__)
struct device *get_device(struct device *d);
void put_device(struct device *d);
int device_register(struct device *d);
void device_unregister(struct device *d);
void device_initialize(struct device *d);
int device_add(struct device *d);
void device_del(struct device *d);
int dev_set_name(struct device *d, const char *fmt, ...);
struct device *device_create(struct class *c, struct device *parent, dev_t devt, void *drvdata, const char *fmt, ...);
void device_destroy(struct class *c, dev_t devt);
struct class *class_create(const char *name);
void class_destroy(struct class *c);
int device_property_read_u32(struct device *d, const char *prop, u32 *val);
bool device_property_present(struct device *d, const char *prop);
#define of_node_put(n) do { (void)(n); } while (0)
#define of_fwnode_handle(n) ((n) ? (n)->fwnode : NULL)
int of_parse_phandle_with_args_map(const struct device_node *np, const char *list, const char *stem, int index,
                                   struct of_phandle_args *out);
struct property { char *name; int length; void *value; struct property *next; };
#define of_get_child_by_name(n, name) ((struct device_node *)NULL)
#define of_parse_phandle(n, name, i) ((struct device_node *)NULL)
#define of_device_is_available(n) false
#define of_find_node_by_path(p) ((struct device_node *)NULL)
#define of_find_matching_node(from, m) ((struct device_node *)NULL)
#define of_property_count_elems_of_size(n, p, sz) (-EINVAL)
#define of_property_read_u32_array(n, p, v, c) (-EINVAL)
#define of_property_read_variable_u8_array(n, p, v, mn, mx) (-EINVAL)
#define of_property_read_variable_u32_array(n, p, v, mn, mx) (-EINVAL)
struct reserved_mem { const char *name; phys_addr_t base; phys_addr_t size; void *priv; };
#define of_reserved_mem_lookup(n) ((struct reserved_mem *)NULL)
/* platform devices (Tegra's, on a system-on-chip): none on a PC */
struct platform_device { const char *name; int id; struct device dev; };
#define to_platform_device(d) container_of(d, struct platform_device, dev)
#define platform_get_drvdata(p) dev_get_drvdata(&(p)->dev)
#define platform_set_drvdata(p, v) dev_set_drvdata(&(p)->dev, v)
struct platform_driver {
    int (*probe)(struct platform_device *p);
    void (*remove)(struct platform_device *p);
    void (*shutdown)(struct platform_device *p);
    struct device_driver driver;
};
#define of_property_read_u32(n, p, v) (-EINVAL)
#define of_property_read_bool(n, p) false
#define of_find_property(n, p, l) NULL
#define of_device_is_compatible(n, c) 0
#define of_get_property(n, p, l) NULL
#define dev_of_node(d) ((d)->of_node)
#define is_of_node(f) false
#define to_of_node(f) ((struct device_node *)NULL)
struct gpio_device;
struct gpio_desc;
struct gpio_chip { int (*of_xlate)(struct gpio_chip *gc, const struct of_phandle_args *a, u32 *flags); };
struct gpio_device *gpio_device_find_by_fwnode(const struct fwnode_handle *f);
struct gpio_desc *gpio_device_get_desc(struct gpio_device *g, unsigned int hwnum);
struct gpio_chip *gpio_device_get_chip(struct gpio_device *g);
void gpio_device_put(struct gpio_device *g);
int desc_to_gpio(const struct gpio_desc *d);
#define gpio_is_valid(n) ((n) >= 0)

/* ---- resources ---- */
#define IORESOURCE_IO 0x00000100UL
#define IORESOURCE_MEM 0x00000200UL
#define IORESOURCE_PREFETCH 0x00002000UL
#define IORESOURCE_MEM_64 0x00100000UL
#define IORESOURCE_UNSET 0x20000000UL
#define IORESOURCE_DISABLED 0x10000000UL
#define IORESOURCE_BUSY 0x80000000UL
struct resource { resource_size_t start, end; const char *name; unsigned long flags; struct resource *parent, *sibling, *child; };
static inline resource_size_t resource_size(const struct resource *r) { return r->end - r->start + 1; }
extern struct resource iomem_resource;
struct resource *request_mem_region(resource_size_t start, resource_size_t n, const char *name);
void release_mem_region(resource_size_t start, resource_size_t n);
#define __request_region(p, s, n, name, f) request_mem_region(s, n, name)
#define __release_region(p, s, n) release_mem_region(s, n)
int region_intersects(resource_size_t start, size_t size, unsigned long flags, unsigned long desc);
#define REGION_INTERSECTS 1
#define REGION_DISJOINT 0
#define IORES_DESC_NONE 0

/* ---- PCI ---- */
#define PCI_ANY_ID (~0u)
#define PCI_VENDOR_ID_NVIDIA 0x10de
#define PCI_VENDOR_ID_INTEL 0x8086
#define PCI_VENDOR_ID_AMD 0x1022
#define PCI_CLASS_DISPLAY_VGA 0x0300
#define PCI_CLASS_DISPLAY_3D 0x0302
#define PCI_CLASS_DISPLAY_OTHER 0x0380
#define PCI_CLASS_BRIDGE_OTHER 0x0680
#define PCI_CLASS_BRIDGE_PCI 0x0604
#define PCI_BASE_CLASS_DISPLAY 0x03
#define PCI_BASE_CLASS_BRIDGE 0x06
#define PCI_DEVFN(slot, fn) ((((slot) & 0x1f) << 3) | ((fn) & 0x07))
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_COMMAND_IO 0x1
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4
#define PCI_COMMAND_INTX_DISABLE 0x400
#define PCI_STATUS 0x06
#define PCI_STATUS_CAP_LIST 0x10
#define PCI_REVISION_ID 0x08
#define PCI_CLASS_PROG 0x09
#define PCI_CLASS_DEVICE 0x0a
#define PCI_CACHE_LINE_SIZE 0x0c
#define PCI_HEADER_TYPE 0x0e
#define PCI_BASE_ADDRESS_0 0x10
#define PCI_BASE_ADDRESS_1 0x14
#define PCI_BASE_ADDRESS_2 0x18
#define PCI_BASE_ADDRESS_3 0x1c
#define PCI_BASE_ADDRESS_4 0x20
#define PCI_BASE_ADDRESS_5 0x24
#define PCI_BASE_ADDRESS_SPACE_IO 0x01
#define PCI_BASE_ADDRESS_MEM_TYPE_MASK 0x06
#define PCI_BASE_ADDRESS_MEM_TYPE_32 0x00
#define PCI_BASE_ADDRESS_MEM_TYPE_64 0x04
#define PCI_BASE_ADDRESS_MEM_PREFETCH 0x08
#define PCI_BASE_ADDRESS_MEM_MASK (~0x0fUL)
#define PCI_SUBSYSTEM_VENDOR_ID 0x2c
#define PCI_SUBSYSTEM_ID 0x2e
#define PCI_ROM_ADDRESS 0x30
#define PCI_CAPABILITY_LIST 0x34
#define PCI_INTERRUPT_LINE 0x3c
#define PCI_INTERRUPT_PIN 0x3d
#define PCI_CAP_ID_PM 0x01
#define PCI_CAP_ID_MSI 0x05
#define PCI_CAP_ID_EXP 0x10
#define PCI_CAP_ID_MSIX 0x11
#define PCI_EXT_CAP_ID_ERR 0x01
#define PCI_EXT_CAP_ID_SRIOV 0x10
#define PCI_EXT_CAP_ID_REBAR 0x15
#define PCI_EXT_CAP_ID_DVSEC 0x23
#define PCI_EXP_DEVCAP 4
#define PCI_EXP_DEVCTL 8
#define PCI_EXP_DEVSTA 10
#define PCI_EXP_LNKCAP 12
#define PCI_EXP_LNKCTL 16
#define PCI_EXP_LNKSTA 18
#define PCI_EXP_LNKCAP2 44
#define PCI_EXP_LNKCTL2 48
#define PCI_EXP_DEVCTL2 40
#define PCI_EXP_DEVCAP2 36
#define PCI_EXP_LNKSTA_CLS 0x000f
#define PCI_EXP_LNKSTA_NLW 0x03f0
#define PCI_EXP_LNKSTA_NLW_SHIFT 4
#define PCI_EXP_LNKCAP_SLS 0x0000000f
#define PCI_EXP_LNKCAP_MLW 0x000003f0
#define PCI_EXP_DEVCTL_RELAX_EN 0x0010
#define PCI_EXP_DEVCTL_NOSNOOP_EN 0x0800
#define PCI_EXP_DEVCAP2_ATOMIC_COMP32 0x00000080
#define PCI_EXP_DEVCAP2_ATOMIC_COMP64 0x00000100
#define PCI_EXP_DEVCAP2_ATOMIC_COMP128 0x00000200
#define PCI_EXP_DEVCTL2_ATOMIC_REQ 0x0040
#define PCI_EXP_TYPE_ROOT_PORT 0x4
#define PCI_EXP_TYPE_DOWNSTREAM 0x6
#define PCI_EXP_TYPE_UPSTREAM 0x5
#define PCI_EXP_TYPE_ENDPOINT 0x0
#define PCI_EXP_FLAGS 2
#define PCI_EXP_FLAGS_TYPE 0x00f0
#define PCI_PM_CTRL 4
#define PCI_PM_CTRL_STATE_MASK 0x0003
#define PCI_D0 0
#define PCI_D1 1
#define PCI_D2 2
#define PCI_D3hot 3
#define PCI_D3cold 4
#define PCI_UNKNOWN 5
#define PCI_POWER_ERROR (-1)
#define PCI_NUM_RESOURCES 11
#define PCI_ROM_RESOURCE 6
#define PCI_STD_NUM_BARS 6
#define DEVICE_COUNT_RESOURCE PCI_NUM_RESOURCES
typedef int pci_power_t;
typedef unsigned int pci_ers_result_t;
typedef unsigned int pci_channel_state_t;
#define PCI_ERS_RESULT_NONE 1
#define PCI_ERS_RESULT_CAN_RECOVER 2
#define PCI_ERS_RESULT_NEED_RESET 3
#define PCI_ERS_RESULT_DISCONNECT 4
#define PCI_ERS_RESULT_RECOVERED 5
#define pci_channel_io_normal 1
#define pci_channel_io_frozen 2
#define pci_channel_io_perm_failure 3
struct pci_bus { unsigned char number; struct pci_bus *parent; struct pci_dev *self; int domain_nr; struct list_head devices; struct device *bridge; };
struct pci_driver;
struct pci_dev {
    struct list_head bus_list;
    struct pci_bus *bus;
    struct pci_bus *subordinate;
    unsigned int devfn;
    unsigned short vendor, device, subsystem_vendor, subsystem_device;
    unsigned int class;
    u8 revision, hdr_type, pcie_cap, msi_cap, msix_cap, pin;
    u16 pcie_flags_reg;
    struct pci_driver *driver;
    u64 dma_mask;
    struct device dev;
    pci_power_t current_state;
    unsigned int irq;
    struct resource resource[DEVICE_COUNT_RESOURCE];
    unsigned int msi_enabled : 1, msix_enabled : 1, is_virtfn : 1, is_physfn : 1, no_msi : 1, d3cold_allowed : 1,
        is_busmaster : 1, broken_intx_masking : 1;
    struct pci_dev *physfn;
    int cfg_size;
    pci_channel_state_t error_state;
    void *lkpi_dev; /* our kernel's own (struct pci_dev in kernel/include/kernel/pci.h) */
};
#define to_pci_dev(d) container_of(d, struct pci_dev, dev)
struct pci_device_id { u32 vendor, device, subvendor, subdevice, class, class_mask; unsigned long driver_data; u32 override_only; };
#define PCI_DEVICE(v, d) .vendor = (v), .device = (d), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_CLASS(c, m) .class = (c), .class_mask = (m), .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_VDEVICE(v, d) .vendor = PCI_VENDOR_ID_##v, .device = (d), .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define MODULE_DEVICE_TABLE(type, name) extern const void *__lkpi_table_##name __attribute__((unused))
struct pci_error_handlers {
    pci_ers_result_t (*error_detected)(struct pci_dev *d, pci_channel_state_t s);
    pci_ers_result_t (*mmio_enabled)(struct pci_dev *d);
    pci_ers_result_t (*slot_reset)(struct pci_dev *d);
    void (*resume)(struct pci_dev *d);
};
struct pci_driver {
    const char *name;
    const struct pci_device_id *id_table;
    int (*probe)(struct pci_dev *d, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *d);
    void (*shutdown)(struct pci_dev *d);
    int (*sriov_configure)(struct pci_dev *d, int n);
    const struct pci_error_handlers *err_handler;
    struct device_driver driver;
};
int pci_register_driver(struct pci_driver *drv);
#define __pci_register_driver(drv, owner, name) pci_register_driver(drv)
void pci_unregister_driver(struct pci_driver *drv);
static inline void *pci_get_drvdata(struct pci_dev *d) { return dev_get_drvdata(&d->dev); }
static inline void pci_set_drvdata(struct pci_dev *d, void *p) { dev_set_drvdata(&d->dev, p); }
static inline int pci_domain_nr(struct pci_bus *b) { return b->domain_nr; }
const char *pci_name(const struct pci_dev *d);
#define pci_resource_start(d, b) ((d)->resource[(b)].start)
#define pci_resource_end(d, b) ((d)->resource[(b)].end)
#define pci_resource_flags(d, b) ((d)->resource[(b)].flags)
#define pci_resource_len(d, b) ((d)->resource[(b)].end ? resource_size(&(d)->resource[(b)]) : 0)
int pci_enable_device(struct pci_dev *d);
void pci_disable_device(struct pci_dev *d);
void pci_set_master(struct pci_dev *d);
void pci_clear_master(struct pci_dev *d);
int pci_request_regions(struct pci_dev *d, const char *name);
void pci_release_regions(struct pci_dev *d);
int pci_request_region(struct pci_dev *d, int bar, const char *name);
void pci_release_region(struct pci_dev *d, int bar);
int pci_read_config_byte(const struct pci_dev *d, int where, u8 *v);
int pci_read_config_word(const struct pci_dev *d, int where, u16 *v);
int pci_read_config_dword(const struct pci_dev *d, int where, u32 *v);
int pci_write_config_byte(const struct pci_dev *d, int where, u8 v);
int pci_write_config_word(const struct pci_dev *d, int where, u16 v);
int pci_write_config_dword(const struct pci_dev *d, int where, u32 v);
int pci_find_capability(struct pci_dev *d, int cap);
int pci_find_ext_capability(struct pci_dev *d, int cap);
int pci_pcie_cap(struct pci_dev *d);
bool pci_is_pcie(struct pci_dev *d);
int pci_pcie_type(const struct pci_dev *d);
int pcie_capability_read_word(struct pci_dev *d, int pos, u16 *v);
int pcie_capability_read_dword(struct pci_dev *d, int pos, u32 *v);
int pcie_capability_write_word(struct pci_dev *d, int pos, u16 v);
int pcie_capability_clear_and_set_word(struct pci_dev *d, int pos, u16 clear, u16 set);
#define pcie_capability_set_word(d, p, s) pcie_capability_clear_and_set_word(d, p, 0, s)
#define pcie_capability_clear_word(d, p, c) pcie_capability_clear_and_set_word(d, p, c, 0)
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from);
struct pci_dev *pci_get_class(unsigned int cls, struct pci_dev *from);
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn);
struct pci_dev *pci_dev_get(struct pci_dev *d);
void pci_dev_put(struct pci_dev *d);
struct pci_dev *pci_upstream_bridge(struct pci_dev *d);
struct pci_dev *pcie_find_root_port(struct pci_dev *d);
#define pci_is_root_bus(b) (!(b)->parent)
#define pci_dev_is_disconnected(d) false
int pci_set_power_state(struct pci_dev *d, pci_power_t s);
int pci_save_state(struct pci_dev *d);
void pci_restore_state(struct pci_dev *d);
int pci_enable_wake(struct pci_dev *d, pci_power_t s, bool enable);
void pci_d3cold_disable(struct pci_dev *d);
void pci_d3cold_enable(struct pci_dev *d);
int pci_reset_function(struct pci_dev *d);
void pci_ignore_hotplug(struct pci_dev *d);
int pci_enable_atomic_ops_to_root(struct pci_dev *d, u32 cap_mask);
void __iomem *pci_iomap(struct pci_dev *d, int bar, unsigned long max);
void pci_iounmap(struct pci_dev *d, void __iomem *p);
int pci_resize_resource(struct pci_dev *d, int bar, int size, int exclude_bars);
void pci_release_resource(struct pci_dev *d, int bar);
int pci_rebar_get_possible_sizes(struct pci_dev *d, int bar);
#define pci_rebar_bytes_to_size(b) (ilog2(b) - 20)
int pci_vfs_assigned(struct pci_dev *d);
int pci_num_vf(struct pci_dev *d);
int pci_enable_sriov(struct pci_dev *d, int n);
void pci_disable_sriov(struct pci_dev *d);
int pci_sriov_get_totalvfs(struct pci_dev *d);
void pci_stop_and_remove_bus_device(struct pci_dev *d);
void pci_bus_type_lkpi(void);
int pcibios_resource_to_bus_lkpi(void);
void pcibios_resource_to_bus(struct pci_bus *b, struct resource *bus_res, struct resource *res);
bool pcie_is_cxl(struct pci_dev *d);
int pcie_get_readrq(struct pci_dev *d);
int pcie_set_readrq(struct pci_dev *d, int rq);

/* ---- interrupts ---- */
typedef irqreturn_t (*irq_handler_t)(int irq, void *dev);
#define IRQF_SHARED 0x00000080
#define IRQF_ONESHOT 0x00002000
#define IRQF_NO_SUSPEND 0x00004000
#define IRQF_TRIGGER_NONE 0
int request_irq(unsigned int irq, irq_handler_t h, unsigned long flags, const char *name, void *dev);
int request_threaded_irq(unsigned int irq, irq_handler_t h, irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev);
const void *free_irq(unsigned int irq, void *dev);
void enable_irq(unsigned int irq);
void disable_irq(unsigned int irq);
void disable_irq_nosync(unsigned int irq);
void synchronize_irq(unsigned int irq);
int irq_set_affinity_hint(unsigned int irq, const void *mask);
struct msix_entry { u32 vector; u16 entry; };
#define PCI_IRQ_INTX 1
#define PCI_IRQ_LEGACY PCI_IRQ_INTX
#define PCI_IRQ_MSI 2
#define PCI_IRQ_MSIX 4
#define PCI_IRQ_ALL_TYPES (PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)
int pci_alloc_irq_vectors(struct pci_dev *d, unsigned int min, unsigned int max, unsigned int flags);
void pci_free_irq_vectors(struct pci_dev *d);
int pci_irq_vector(struct pci_dev *d, unsigned int nr);
int pci_enable_msi(struct pci_dev *d);
void pci_disable_msi(struct pci_dev *d);
int pci_enable_msix_range(struct pci_dev *d, struct msix_entry *e, int min, int max);
int pci_enable_msix_exact(struct pci_dev *d, struct msix_entry *e, int n);
void pci_disable_msix(struct pci_dev *d);
int pci_msix_vec_count(struct pci_dev *d);
void pci_intx(struct pci_dev *d, int enable);

/* ---- DMA (direct: the bus address is the physical one) ---- */
enum dma_data_direction { DMA_BIDIRECTIONAL = 0, DMA_TO_DEVICE = 1, DMA_FROM_DEVICE = 2, DMA_NONE = 3 };
#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR (~(dma_addr_t)0)
#define DMA_ATTR_SKIP_CPU_SYNC (1UL << 5)
#define DMA_ATTR_WRITE_COMBINE (1UL << 2)
#define DMA_ATTR_NO_KERNEL_MAPPING (1UL << 4)
struct dma_map_ops;
struct scatterlist { unsigned long page_link; unsigned int offset, length; dma_addr_t dma_address; unsigned int dma_length; };
struct sg_table { struct scatterlist *sgl; unsigned int nents, orig_nents; };
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)
struct page *sg_page(struct scatterlist *sg);
void sg_set_page(struct scatterlist *sg, struct page *p, unsigned int len, unsigned int offset);
void sg_init_table(struct scatterlist *sg, unsigned int n);
struct scatterlist *sg_next(struct scatterlist *sg);
#define for_each_sg(sglist, sg, nr, i) for ((i) = 0, (sg) = (sglist); (i) < (nr); (i)++, (sg) = sg_next(sg))
#define for_each_sgtable_sg(t, sg, i) for_each_sg((t)->sgl, sg, (t)->orig_nents, i)
#define for_each_sgtable_dma_sg(t, sg, i) for_each_sg((t)->sgl, sg, (t)->nents, i)
int sg_alloc_table(struct sg_table *t, unsigned int n, gfp_t gfp);
void sg_free_table(struct sg_table *t);
int sg_alloc_table_from_pages(struct sg_table *t, struct page **pages, unsigned int n, unsigned int offset, unsigned long size, gfp_t gfp);
dma_addr_t dma_map_page(struct device *d, struct page *p, size_t offset, size_t size, enum dma_data_direction dir);
#define dma_map_page_attrs(d, p, o, s, dir, a) dma_map_page(d, p, o, s, dir)
void dma_unmap_page(struct device *d, dma_addr_t a, size_t size, enum dma_data_direction dir);
#define dma_unmap_page_attrs(d, a, s, dir, at) dma_unmap_page(d, a, s, dir)
dma_addr_t dma_map_single(struct device *d, void *p, size_t size, enum dma_data_direction dir);
void dma_unmap_single(struct device *d, dma_addr_t a, size_t size, enum dma_data_direction dir);
dma_addr_t dma_map_resource(struct device *d, phys_addr_t p, size_t size, enum dma_data_direction dir, unsigned long attrs);
void dma_unmap_resource(struct device *d, dma_addr_t a, size_t size, enum dma_data_direction dir, unsigned long attrs);
int dma_map_sg(struct device *d, struct scatterlist *sg, int n, enum dma_data_direction dir);
#define dma_map_sg_attrs(d, s, n, dir, a) dma_map_sg(d, s, n, dir)
void dma_unmap_sg(struct device *d, struct scatterlist *sg, int n, enum dma_data_direction dir);
#define dma_unmap_sg_attrs(d, s, n, dir, a) dma_unmap_sg(d, s, n, dir)
int dma_map_sgtable(struct device *d, struct sg_table *t, enum dma_data_direction dir, unsigned long attrs);
void dma_unmap_sgtable(struct device *d, struct sg_table *t, enum dma_data_direction dir, unsigned long attrs);
static inline int dma_mapping_error(struct device *d, dma_addr_t a) { (void)d; return a == DMA_MAPPING_ERROR; }
void *dma_alloc_coherent(struct device *d, size_t size, dma_addr_t *handle, gfp_t gfp);
void dma_free_coherent(struct device *d, size_t size, void *p, dma_addr_t handle);
void *dma_alloc_attrs(struct device *d, size_t size, dma_addr_t *handle, gfp_t gfp, unsigned long attrs);
void dma_free_attrs(struct device *d, size_t size, void *p, dma_addr_t handle, unsigned long attrs);
int dma_set_mask(struct device *d, u64 mask);
int dma_set_coherent_mask(struct device *d, u64 mask);
int dma_set_mask_and_coherent(struct device *d, u64 mask);
u64 dma_get_mask(struct device *d);
u64 dma_get_required_mask(struct device *d);
#define dma_sync_single_for_cpu(d, a, s, dir) do { } while (0)
#define dma_sync_single_for_device(d, a, s, dir) do { } while (0)
#define dma_sync_sg_for_cpu(d, s, n, dir) do { } while (0)
#define dma_sync_sg_for_device(d, s, n, dir) do { } while (0)
struct dma_map_ops { void *(*alloc)(struct device *d, size_t s, dma_addr_t *h, gfp_t g, unsigned long a); dma_addr_t (*map_phys)(struct device *d, phys_addr_t p, size_t s, int dir, unsigned long a); };
struct pci_host_bridge { struct device dev; struct pci_bus *bus; int domain_nr; unsigned int preserve_config : 1; };
struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *b);
const struct dma_map_ops *get_dma_ops(struct device *d);
bool use_dma_iommu(struct device *d);
#define dma_is_direct(o) ((o) == NULL)
size_t dma_max_mapping_size(struct device *d);
bool dma_addressing_limited(struct device *d);
struct iommu_domain;
struct iommu_domain *iommu_get_domain_for_dev(struct device *d);
#define device_iommu_mapped(d) false
#define iommu_present(b) false

/* ---- power management ---- */
int pm_runtime_get_sync(struct device *d);
int pm_runtime_get_noresume(struct device *d);
int pm_runtime_put(struct device *d);
int pm_runtime_put_noidle(struct device *d);
int pm_runtime_put_sync(struct device *d);
int pm_runtime_put_autosuspend(struct device *d);
void pm_runtime_allow(struct device *d);
void pm_runtime_forbid(struct device *d);
void pm_runtime_enable(struct device *d);
void pm_runtime_disable(struct device *d);
int pm_runtime_set_active(struct device *d);
void pm_runtime_use_autosuspend(struct device *d);
void pm_runtime_dont_use_autosuspend(struct device *d);
void pm_runtime_set_autosuspend_delay(struct device *d, int ms);
void pm_runtime_mark_last_busy(struct device *d);
int pm_runtime_resume(struct device *d);
int pm_runtime_suspended(struct device *d);
int pm_runtime_active(struct device *d);
int pm_runtime_get_if_in_use(struct device *d);
#define pm_runtime_get(d) pm_runtime_get_sync(d)
#define pm_vt_switch_required(d, r) do { } while (0)
#define pm_vt_switch_unregister(d) do { } while (0)
#define device_may_wakeup(d) false
#define device_set_wakeup_enable(d, e) 0
#define PMSG_SUSPEND 0
struct notifier_block { int (*notifier_call)(struct notifier_block *nb, unsigned long action, void *data); struct notifier_block *next; int priority; };
#define NOTIFY_DONE 0
#define NOTIFY_OK 1
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD (NOTIFY_STOP_MASK | 2)
int register_pm_notifier(struct notifier_block *nb);
int unregister_pm_notifier(struct notifier_block *nb);
#define PM_HIBERNATION_PREPARE 1
#define PM_POST_HIBERNATION 2
#define PM_SUSPEND_PREPARE 3
#define PM_POST_SUSPEND 4
int register_reboot_notifier(struct notifier_block *nb);
int unregister_reboot_notifier(struct notifier_block *nb);
