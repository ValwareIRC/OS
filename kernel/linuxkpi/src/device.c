#include <lkpi/all.h>
#include "../host.h"

/* kernel/linuxkpi: the device model, PCI, interrupts, DMA and power management. Every PCI function our kernel
 * found gets a Linux-shaped struct pci_dev (on the buses it sits on, its bridges above it) the first time a driver
 * registers; matching ones are probed then. DMA is direct: the bus address is the physical one (no IOMMU here). */

/* ---- the device model ---- */

const struct bus_type pci_bus_type = {.name = "pci"};

const char *dev_name(const struct device *d) {
    if (!d)
        return "(no device)";
    return d->kobj.name ? d->kobj.name : d->init_name ? d->init_name : "?";
}

const char *dev_driver_string(const struct device *d) { return d && d->driver && d->driver->name ? d->driver->name : ""; }
bool dev_is_pci(const struct device *d) { return d && d->bus == &pci_bus_type; }
struct device *get_device(struct device *d) { return d; }
void put_device(struct device *d) {}
void device_initialize(struct device *d) { atomic_set(&d->power.usage_count, 0); }
int device_add(struct device *d) { return 0; }
void device_del(struct device *d) {}

int device_register(struct device *d) {
    device_initialize(d);
    return device_add(d);
}

void device_unregister(struct device *d) {
    if (d->release)
        d->release(d);
}

int dev_set_name(struct device *d, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *n = kvasprintf(GFP_KERNEL, fmt, ap);
    va_end(ap);
    if (!n)
        return -ENOMEM;
    d->kobj.name = n; /* (kept for the device's life) */
    return 0;
}

struct device *device_create(struct class *c, struct device *parent, dev_t devt, void *drvdata, const char *fmt, ...) {
    struct device *d = kzalloc(sizeof *d, GFP_KERNEL);
    if (!d)
        return ERR_PTR(-ENOMEM);
    d->parent = parent, d->class = c, d->devt = devt, d->driver_data = drvdata;
    va_list ap;
    va_start(ap, fmt);
    d->kobj.name = kvasprintf(GFP_KERNEL, fmt, ap);
    va_end(ap);
    return d;
}

void device_destroy(struct class *c, dev_t devt) {}

struct class *class_create(const char *name) {
    struct class *c = kzalloc(sizeof *c, GFP_KERNEL);
    if (!c)
        return ERR_PTR(-ENOMEM);
    strscpy(c->name, name, sizeof c->name);
    return c;
}

void class_destroy(struct class *c) {
    if (!IS_ERR_OR_NULL(c))
        kfree(c);
}

int device_property_read_u32(struct device *d, const char *prop, u32 *val) { return -EINVAL; }
bool device_property_present(struct device *d, const char *prop) { return false; }

/* (device trees and GPIOs are Tegra's: nothing of the kind on a PC) */
int of_parse_phandle_with_args_map(const struct device_node *np, const char *list, const char *stem, int index,
                                   struct of_phandle_args *out) {
    return -ENOENT;
}

struct gpio_device *gpio_device_find_by_fwnode(const struct fwnode_handle *f) { return NULL; }
struct gpio_desc *gpio_device_get_desc(struct gpio_device *g, unsigned int hwnum) { return ERR_PTR(-ENODEV); }
struct gpio_chip *gpio_device_get_chip(struct gpio_device *g) { return NULL; }
void gpio_device_put(struct gpio_device *g) {}
int desc_to_gpio(const struct gpio_desc *d) { return -1; }

/* resources: the regions the driver claims (nothing else here claims a GPU's) */
struct resource iomem_resource = {.start = 0, .end = ~(resource_size_t)0, .name = "PCI mem", .flags = IORESOURCE_MEM};
static struct resource claimed = {.name = "claimed", .flags = IORESOURCE_MEM | IORESOURCE_BUSY};

struct resource *request_mem_region(resource_size_t start, resource_size_t n, const char *name) { return &claimed; }
void release_mem_region(resource_size_t start, resource_size_t n) {}
int region_intersects(resource_size_t start, size_t size, unsigned long flags, unsigned long desc) { return REGION_DISJOINT; }

/* ---- PCI: the layer's view of our kernel's devices ---- */

#define MAX_BUSES 256

struct irq_slot {
    irq_handler_t handler, thread_fn;
    void *dev_id;
    const char *name;
    bool requested, disabled, pending;
    volatile int running;
    struct workqueue_struct *wq;
    struct work_struct work;
};

struct lkpi_pci {
    struct pci_dev pd;
    int host;      /* our kernel's index */
    char name[16]; /* 0000:bb:dd.f */
    u32 saved[16];
    bool saved_ok;
    struct irq_slot irq;
};

#define IRQ_BASE 0x1000 /* (the layer's interrupt numbers: IRQ_BASE + its device's number) */

static struct lkpi_pci *devs;
static int ndevs;
static struct pci_bus buses[MAX_BUSES];
static struct pci_host_bridge host_bridge;
static DEFINE_MUTEX(pci_lock);

static struct lkpi_pci *lp(struct pci_dev *d) { return container_of(d, struct lkpi_pci, pd); }

static u32 cfg_read(struct lkpi_pci *p, int off, int w) { return kpi_host_pci_read(p->host, off, w); }
static void cfg_write(struct lkpi_pci *p, int off, int w, u32 v) { kpi_host_pci_write(p->host, off, w, v); }

int pci_read_config_byte(const struct pci_dev *d, int where, u8 *v) { *v = (u8)cfg_read(lp((struct pci_dev *)d), where, 1); return 0; }
int pci_read_config_word(const struct pci_dev *d, int where, u16 *v) { *v = (u16)cfg_read(lp((struct pci_dev *)d), where, 2); return 0; }
int pci_read_config_dword(const struct pci_dev *d, int where, u32 *v) { *v = cfg_read(lp((struct pci_dev *)d), where, 4); return 0; }
int pci_write_config_byte(const struct pci_dev *d, int where, u8 v) { cfg_write(lp((struct pci_dev *)d), where, 1, v); return 0; }
int pci_write_config_word(const struct pci_dev *d, int where, u16 v) { cfg_write(lp((struct pci_dev *)d), where, 2, v); return 0; }
int pci_write_config_dword(const struct pci_dev *d, int where, u32 v) { cfg_write(lp((struct pci_dev *)d), where, 4, v); return 0; }

static int find_cap(struct lkpi_pci *p, int cap) {
    if (!(cfg_read(p, PCI_STATUS, 2) & PCI_STATUS_CAP_LIST))
        return 0;
    int at = (int)cfg_read(p, PCI_CAPABILITY_LIST, 1) & 0xfc;
    for (int guard = 0; at && guard < 48; guard++) {
        if ((int)cfg_read(p, at, 1) == cap)
            return at;
        at = (int)cfg_read(p, at + 1, 1) & 0xfc;
    }
    return 0;
}

int pci_find_capability(struct pci_dev *d, int cap) { return find_cap(lp(d), cap); }

/* the extended capabilities (from 0x100): each `id`, after `from` */
static int find_ext_cap(struct lkpi_pci *p, int id, int from) {
    int at = from ? (int)(cfg_read(p, from, 4) >> 20) & 0xffc : 0x100;
    for (int guard = 0; at >= 0x100 && guard < 480; guard++) {
        u32 h = cfg_read(p, at, 4);
        if (h == 0 || h == ~0u)
            return 0;
        if ((int)(h & 0xffff) == id)
            return at;
        at = (int)(h >> 20) & 0xffc;
    }
    return 0;
}

int pci_find_ext_capability(struct pci_dev *d, int cap) { return find_ext_cap(lp(d), cap, 0); }

int pci_find_dvsec_capability(struct pci_dev *d, u16 vendor, u16 dvsec) {
    struct lkpi_pci *p = lp(d);
    for (int at = find_ext_cap(p, PCI_EXT_CAP_ID_DVSEC, 0); at; at = find_ext_cap(p, PCI_EXT_CAP_ID_DVSEC, at)) {
        u32 h1 = cfg_read(p, at + 4, 4), h2 = cfg_read(p, at + 8, 2);
        if ((h1 & 0xffff) == vendor && h2 == dvsec)
            return at;
    }
    return 0;
}

int pci_pcie_cap(struct pci_dev *d) { return d->pcie_cap; }
bool pci_is_pcie(struct pci_dev *d) { return d->pcie_cap != 0; }
int pci_pcie_type(const struct pci_dev *d) { return (d->pcie_flags_reg & PCI_EXP_FLAGS_TYPE) >> 4; }

int pcie_capability_read_word(struct pci_dev *d, int pos, u16 *v) {
    *v = 0;
    if (!d->pcie_cap)
        return -EINVAL;
    return pci_read_config_word(d, d->pcie_cap + pos, v);
}

int pcie_capability_read_dword(struct pci_dev *d, int pos, u32 *v) {
    *v = 0;
    if (!d->pcie_cap)
        return -EINVAL;
    return pci_read_config_dword(d, d->pcie_cap + pos, v);
}

int pcie_capability_write_word(struct pci_dev *d, int pos, u16 v) {
    if (!d->pcie_cap)
        return -EINVAL;
    return pci_write_config_word(d, d->pcie_cap + pos, v);
}

int pcie_capability_clear_and_set_word(struct pci_dev *d, int pos, u16 clear, u16 set) {
    u16 v;
    int r = pcie_capability_read_word(d, pos, &v);
    return r ? r : pcie_capability_write_word(d, pos, (u16)((v & ~clear) | set));
}

int pcie_get_readrq(struct pci_dev *d) {
    u16 c;
    pcie_capability_read_word(d, PCI_EXP_DEVCTL, &c);
    return 128 << ((c >> 12) & 7);
}

int pcie_set_readrq(struct pci_dev *d, int rq) {
    int v = 0;
    while (v < 5 && (128 << v) < rq)
        v++;
    return pcie_capability_clear_and_set_word(d, PCI_EXP_DEVCTL, 0x7000, (u16)(v << 12));
}

bool pcie_is_cxl(struct pci_dev *d) { return false; }

/* every function our kernel found, as Linux shows them; once */
static void scan(void) {
    if (devs)
        return;
    int n = kpi_host_pci_count();
    devs = kzalloc((size_t)(n ? n : 1) * sizeof *devs, GFP_KERNEL);
    if (!devs)
        kpi_host_panic("linuxkpi: no memory for PCI devices");
    for (int b = 0; b < MAX_BUSES; b++) {
        buses[b].number = (unsigned char)b;
        INIT_LIST_HEAD(&buses[b].devices);
    }
    host_bridge.bus = &buses[0];
    for (int i = 0; i < n; i++) {
        struct lkpi_pci *p = &devs[ndevs];
        u16 vendor, device;
        u32 class_rev;
        u8 bus, dev, fn, irq_line;
        u64 bar[6], size[6];
        bool io[6];
        if (!kpi_host_pci_info(i, &vendor, &device, &class_rev, &bus, &dev, &fn, bar, size, io, &irq_line))
            continue;
        ndevs++;
        struct pci_dev *d = &p->pd;
        p->host = i;
        snprintf(p->name, sizeof p->name, "0000:%02x:%02x.%x", bus, dev, fn);
        d->vendor = vendor, d->device = device;
        d->class = class_rev >> 8;
        d->revision = (u8)class_rev;
        d->devfn = PCI_DEVFN(dev, fn);
        d->bus = &buses[bus];
        d->hdr_type = (u8)(cfg_read(p, PCI_HEADER_TYPE, 1) & 0x7f);
        d->subsystem_vendor = (u16)cfg_read(p, PCI_SUBSYSTEM_VENDOR_ID, 2);
        d->subsystem_device = (u16)cfg_read(p, PCI_SUBSYSTEM_ID, 2);
        d->pin = (u8)cfg_read(p, PCI_INTERRUPT_PIN, 1);
        d->irq = 0;
        d->pcie_cap = (u8)find_cap(p, PCI_CAP_ID_EXP);
        d->msi_cap = (u8)find_cap(p, PCI_CAP_ID_MSI);
        d->msix_cap = (u8)find_cap(p, PCI_CAP_ID_MSIX);
        if (d->pcie_cap)
            d->pcie_flags_reg = (u16)cfg_read(p, d->pcie_cap + PCI_EXP_FLAGS, 2);
        d->cfg_size = d->pcie_cap ? 4096 : 256;
        d->current_state = PCI_D0;
        d->d3cold_allowed = 1;
        d->error_state = pci_channel_io_normal;
        d->dma_mask = DMA_BIT_MASK(32);
        d->dev.dma_mask = &d->dma_mask;
        d->dev.coherent_dma_mask = DMA_BIT_MASK(32);
        d->dev.bus = &pci_bus_type;
        d->dev.kobj.name = p->name;
        d->dev.numa_node = NUMA_NO_NODE;
        d->lkpi_dev = p;
        for (int b = 0; b < 6; b++) {
            if (!size[b])
                continue;
            struct resource *r = &d->resource[b];
            r->start = bar[b], r->end = bar[b] + size[b] - 1, r->name = p->name;
            r->flags = io[b] ? IORESOURCE_IO : IORESOURCE_MEM;
            u32 raw = cfg_read(p, PCI_BASE_ADDRESS_0 + 4 * b, 4);
            if (!io[b] && (raw & PCI_BASE_ADDRESS_MEM_PREFETCH))
                r->flags |= IORESOURCE_PREFETCH;
            if (!io[b] && (raw & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64)
                r->flags |= IORESOURCE_MEM_64;
        }
        list_add_tail(&d->bus_list, &d->bus->devices);
    }
    /* bridges: the bus below each is its subordinate, and it is that bus's `self` */
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i].pd;
        if (d->hdr_type != 1)
            continue;
        u8 secondary = (u8)cfg_read(&devs[i], 0x19, 1);
        if (secondary && !buses[secondary].self) {
            buses[secondary].self = d;
            buses[secondary].parent = d->bus;
            buses[secondary].bridge = &d->dev;
            d->subordinate = &buses[secondary];
        }
    }
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i].pd;
        if (d->bus->self)
            d->dev.parent = &d->bus->self->dev;
    }
}

const char *pci_name(const struct pci_dev *d) { return lp((struct pci_dev *)d)->name; }

static bool id_matches(const struct pci_device_id *id, const struct pci_dev *d) {
    return (id->vendor == PCI_ANY_ID || id->vendor == d->vendor) && (id->device == PCI_ANY_ID || id->device == d->device) &&
           (id->subvendor == PCI_ANY_ID || id->subvendor == d->subsystem_vendor) &&
           (id->subdevice == PCI_ANY_ID || id->subdevice == d->subsystem_device) &&
           !((id->class ^ d->class) & id->class_mask);
}

static const struct pci_device_id *match(const struct pci_device_id *ids, const struct pci_dev *d) {
    for (; ids && (ids->vendor || ids->subvendor || ids->class_mask); ids++)
        if (id_matches(ids, d))
            return ids;
    return NULL;
}

bool pci_dev_present(const struct pci_device_id *ids) {
    mutex_lock(&pci_lock);
    scan();
    bool found = false;
    for (int i = 0; i < ndevs && !found; i++)
        found = match(ids, &devs[i].pd) != NULL;
    mutex_unlock(&pci_lock);
    return found;
}

int pci_register_driver(struct pci_driver *drv) {
    mutex_lock(&pci_lock);
    scan();
    mutex_unlock(&pci_lock);
    int bound = 0;
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i].pd;
        const struct pci_device_id *id = d->driver ? NULL : match(drv->id_table, d);
        if (!id)
            continue;
        d->driver = drv;
        d->dev.driver = &drv->driver;
        int r = drv->probe ? drv->probe(d, id) : 0;
        if (r) {
            printk("linuxkpi: %s: %s did not take it (%d)\n", devs[i].name, drv->name, r);
            d->driver = NULL;
            d->dev.driver = NULL;
            continue;
        }
        bound++;
    }
    printk("linuxkpi: %s: %d device(s)\n", drv->name, bound);
    return 0;
}

void pci_unregister_driver(struct pci_driver *drv) {
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i].pd;
        if (d->driver != drv)
            continue;
        if (drv->remove)
            drv->remove(d);
        d->driver = NULL;
        d->dev.driver = NULL;
    }
}

/* the devices after `from`, by what they are or where they are */
static struct pci_dev *next_dev(struct pci_dev *from, bool (*ok)(struct pci_dev *, const void *), const void *arg) {
    mutex_lock(&pci_lock);
    scan();
    mutex_unlock(&pci_lock);
    int i = from ? (int)(lp(from) - devs) + 1 : 0;
    for (; i < ndevs; i++)
        if (ok(&devs[i].pd, arg))
            return &devs[i].pd;
    return NULL;
}

struct vd { unsigned vendor, device; };
static bool is_vd(struct pci_dev *d, const void *a) {
    const struct vd *v = a;
    return (v->vendor == PCI_ANY_ID || d->vendor == v->vendor) && (v->device == PCI_ANY_ID || d->device == v->device);
}
static bool is_class(struct pci_dev *d, const void *a) { return d->class == *(const unsigned *)a; }

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from) {
    struct vd v = {vendor, device};
    return next_dev(from, is_vd, &v);
}

struct pci_dev *pci_get_class(unsigned int cls, struct pci_dev *from) { return next_dev(from, is_class, &cls); }

struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn) {
    if (domain != 0)
        return NULL;
    for (struct pci_dev *d = next_dev(NULL, is_vd, &(struct vd){PCI_ANY_ID, PCI_ANY_ID}); d;
         d = next_dev(d, is_vd, &(struct vd){PCI_ANY_ID, PCI_ANY_ID}))
        if (d->bus->number == bus && d->devfn == devfn)
            return d;
    return NULL;
}

struct pci_dev *pci_dev_get(struct pci_dev *d) { return d; }
void pci_dev_put(struct pci_dev *d) {}
struct pci_dev *pci_upstream_bridge(struct pci_dev *d) { return d->bus ? d->bus->self : NULL; }

struct pci_dev *pcie_find_root_port(struct pci_dev *d) {
    for (struct pci_dev *b = pci_upstream_bridge(d); b; b = pci_upstream_bridge(b))
        if (pci_is_pcie(b) && pci_pcie_type(b) == PCI_EXP_TYPE_ROOT_PORT)
            return b;
    return NULL;
}

struct pci_host_bridge *pci_find_host_bridge(struct pci_bus *b) { return &host_bridge; }

int pci_bus_read_config_dword(struct pci_bus *b, unsigned int devfn, int where, u32 *v) {
    struct pci_dev *d = pci_get_domain_bus_and_slot(0, b->number, devfn);
    *v = d ? cfg_read(lp(d), where, 4) : ~0u; /* (nothing there reads as all ones, as on the bus) */
    return 0;
}

int pci_bus_write_config_dword(struct pci_bus *b, unsigned int devfn, int where, u32 v) {
    struct pci_dev *d = pci_get_domain_bus_and_slot(0, b->number, devfn);
    if (d)
        cfg_write(lp(d), where, 4, v);
    return 0;
}

void pci_assign_unassigned_bus_resources(struct pci_bus *b) {}
void pcibios_resource_to_bus(struct pci_bus *b, struct resource *bus_res, struct resource *res) { *bus_res = *res; }
void pci_bus_type_lkpi(void) {}
int pcibios_resource_to_bus_lkpi(void) { return 0; }

int pci_enable_device(struct pci_dev *d) {
    kpi_host_pci_enable(lp(d)->host); /* (memory and I/O decoding, and bus mastering) */
    d->is_busmaster = 1;
    return 0;
}

void pci_disable_device(struct pci_dev *d) { pci_clear_master(d); }

void pci_set_master(struct pci_dev *d) {
    u16 c;
    pci_read_config_word(d, PCI_COMMAND, &c);
    pci_write_config_word(d, PCI_COMMAND, c | PCI_COMMAND_MASTER);
    d->is_busmaster = 1;
}

void pci_clear_master(struct pci_dev *d) {
    u16 c;
    pci_read_config_word(d, PCI_COMMAND, &c);
    pci_write_config_word(d, PCI_COMMAND, c & ~PCI_COMMAND_MASTER);
    d->is_busmaster = 0;
}

int pci_request_regions(struct pci_dev *d, const char *name) { return 0; }
void pci_release_regions(struct pci_dev *d) {}
int pci_request_region(struct pci_dev *d, int bar, const char *name) { return 0; }
void pci_release_region(struct pci_dev *d, int bar) {}

void __iomem *pci_iomap(struct pci_dev *d, int bar, unsigned long max) {
    resource_size_t len = pci_resource_len(d, bar);
    if (!len || (pci_resource_flags(d, bar) & IORESOURCE_IO))
        return NULL;
    if (max && max < len)
        len = max;
    return ioremap(pci_resource_start(d, bar), len);
}

void pci_iounmap(struct pci_dev *d, void __iomem *p) { iounmap(p); }

/* power states, saved state, resets */
int pci_set_power_state(struct pci_dev *d, pci_power_t s) {
    int pm = find_cap(lp(d), PCI_CAP_ID_PM);
    if (!pm || s > PCI_D3hot) {
        d->current_state = s;
        return 0;
    }
    u16 c;
    pci_read_config_word(d, pm + PCI_PM_CTRL, &c);
    pci_write_config_word(d, pm + PCI_PM_CTRL, (u16)((c & ~PCI_PM_CTRL_STATE_MASK) | s));
    msleep(s == PCI_D3hot || d->current_state == PCI_D3hot ? 10 : 1); /* (the settling times PCI asks for) */
    d->current_state = s;
    return 0;
}

int pci_save_state(struct pci_dev *d) {
    struct lkpi_pci *p = lp(d);
    for (int i = 0; i < 16; i++)
        p->saved[i] = cfg_read(p, i * 4, 4);
    p->saved_ok = true;
    return 0;
}

void pci_restore_state(struct pci_dev *d) {
    struct lkpi_pci *p = lp(d);
    if (!p->saved_ok)
        return;
    for (int i = 15; i >= 1; i--) /* (the command register last: decoding comes back once the BARs are right) */
        if (i != 1 && cfg_read(p, i * 4, 4) != p->saved[i])
            cfg_write(p, i * 4, 4, p->saved[i]);
    cfg_write(p, PCI_COMMAND, 2, p->saved[1] & 0xffff);
}

int pci_enable_wake(struct pci_dev *d, pci_power_t s, bool enable) { return 0; }
void pci_d3cold_disable(struct pci_dev *d) { d->d3cold_allowed = 0; }
void pci_d3cold_enable(struct pci_dev *d) { d->d3cold_allowed = 1; }
void pci_ignore_hotplug(struct pci_dev *d) {}

/* function-level reset (PCIe device control bit 15, where the device says it can) */
int pcie_reset_flr(struct pci_dev *d, bool probe) {
    u32 cap;
    if (pcie_capability_read_dword(d, PCI_EXP_DEVCAP, &cap) || !(cap & (1u << 28)))
        return -ENOTTY;
    if (probe)
        return 0;
    pcie_capability_clear_and_set_word(d, PCI_EXP_DEVCTL, 0, 1u << 15);
    msleep(100);
    return 0;
}

int pci_reset_function(struct pci_dev *d) {
    pci_save_state(d);
    int r = pcie_reset_flr(d, false);
    pci_restore_state(d);
    return r;
}

int pci_enable_atomic_ops_to_root(struct pci_dev *d, u32 cap_mask) { return -EINVAL; /* (not set up: the driver does without) */ }

/* resizable BARs: not offered (our kernel placed the BARs at boot and keeps them) */
int pci_rebar_get_possible_sizes(struct pci_dev *d, int bar) { return 0; }
int pci_resize_resource(struct pci_dev *d, int bar, int size, int exclude_bars) { return -ENOTSUPP; }
void pci_release_resource(struct pci_dev *d, int bar) {}

int pci_vfs_assigned(struct pci_dev *d) { return 0; }
int pci_num_vf(struct pci_dev *d) { return 0; }
int pci_enable_sriov(struct pci_dev *d, int n) { return -ENODEV; }
void pci_disable_sriov(struct pci_dev *d) {}
int pci_sriov_get_totalvfs(struct pci_dev *d) { return 0; }

void pci_stop_and_remove_bus_device(struct pci_dev *d) {
    printk("linuxkpi: %s: the driver gave the device up (it stays on the bus)\n", pci_name(d));
    if (d->driver && d->driver->remove)
        d->driver->remove(d);
    d->driver = NULL;
    d->dev.driver = NULL;
}

/* ---- interrupts: MSI, one vector a device (our kernel has no legacy INTx routing yet) ---- */

extern int lkpi_irq_depth[256];

static struct lkpi_pci *irq_dev(unsigned int irq) {
    int i = (int)irq - IRQ_BASE;
    return i >= 0 && i < ndevs ? &devs[i] : NULL;
}

static void irq_thread_work(struct work_struct *w) {
    struct irq_slot *s = container_of(w, struct irq_slot, work);
    struct lkpi_pci *p = container_of(s, struct lkpi_pci, irq);
    if (s->thread_fn)
        s->thread_fn((int)p->pd.irq, s->dev_id);
}

static void run_handler(struct lkpi_pci *p) {
    struct irq_slot *s = &p->irq;
    WRITE_ONCE(s->running, 1);
    irqreturn_t r = s->handler((int)p->pd.irq, s->dev_id);
    WRITE_ONCE(s->running, 0);
    if ((r & IRQ_WAKE_THREAD) && s->wq)
        queue_work(s->wq, &s->work);
}

/* in interrupt context (host.c's vector) */
static void dispatch(void *arg) {
    struct lkpi_pci *p = arg;
    struct irq_slot *s = &p->irq;
    int cpu = kpi_host_cpu();
    lkpi_irq_depth[cpu]++;
    if (READ_ONCE(s->disabled))
        s->pending = true; /* (MSI has no line to hold: it is run when enabled again) */
    else if (s->requested)
        run_handler(p);
    lkpi_irq_depth[cpu]--;
}

int pci_enable_msi(struct pci_dev *d) {
    if (!d->msi_cap || d->no_msi)
        return -EINVAL;
    d->msi_enabled = 1;
    d->irq = (unsigned)(IRQ_BASE + (lp(d) - devs));
    return 0;
}

void pci_disable_msi(struct pci_dev *d) {
    d->msi_enabled = 0;
    d->irq = 0;
}

int pci_enable_msix_range(struct pci_dev *d, struct msix_entry *e, int min, int max) { return -EINVAL; }
int pci_enable_msix_exact(struct pci_dev *d, struct msix_entry *e, int n) { return -EINVAL; }
void pci_disable_msix(struct pci_dev *d) {}
int pci_msix_vec_count(struct pci_dev *d) { return 0; }
void pci_intx(struct pci_dev *d, int enable) {}

int pci_alloc_irq_vectors(struct pci_dev *d, unsigned int min, unsigned int max, unsigned int flags) {
    if (min <= 1 && (flags & PCI_IRQ_MSI) && pci_enable_msi(d) == 0)
        return 1;
    return -ENOSPC;
}

void pci_free_irq_vectors(struct pci_dev *d) { pci_disable_msi(d); }
int pci_irq_vector(struct pci_dev *d, unsigned int nr) { return nr == 0 && d->irq ? (int)d->irq : -EINVAL; }

int request_threaded_irq(unsigned int irq, irq_handler_t h, irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev) {
    struct lkpi_pci *p = irq_dev(irq);
    if (!p || !p->pd.msi_enabled) {
        printk("linuxkpi: interrupt %u: none to give (only MSI here)\n", irq);
        return -EINVAL;
    }
    struct irq_slot *s = &p->irq;
    if (s->requested)
        return -EBUSY;
    s->handler = h, s->thread_fn = thread_fn, s->dev_id = dev, s->name = name;
    s->disabled = s->pending = false;
    if (thread_fn && !s->wq) {
        s->wq = alloc_workqueue("irq/%s", 0, 1, name ? name : p->name);
        if (!s->wq)
            return -ENOMEM;
        INIT_WORK(&s->work, irq_thread_work);
    }
    s->requested = true;
    if (kpi_host_pci_irq(p->host, dispatch, p) < 0) {
        s->requested = false;
        printk("linuxkpi: %s: no MSI vector\n", p->name);
        return -EBUSY;
    }
    return 0;
}

int request_irq(unsigned int irq, irq_handler_t h, unsigned long flags, const char *name, void *dev) {
    return request_threaded_irq(irq, h, NULL, flags, name, dev);
}

void synchronize_irq(unsigned int irq) {
    struct lkpi_pci *p = irq_dev(irq);
    if (!p)
        return;
    while (READ_ONCE(p->irq.running))
        cpu_relax();
    if (p->irq.wq)
        flush_work(&p->irq.work);
}

const void *free_irq(unsigned int irq, void *dev) {
    struct lkpi_pci *p = irq_dev(irq);
    if (!p || !p->irq.requested)
        return NULL;
    kpi_host_pci_irq_free(p->host);
    p->irq.requested = false;
    synchronize_irq(irq);
    const void *name = p->irq.name;
    return name;
}

void disable_irq_nosync(unsigned int irq) {
    struct lkpi_pci *p = irq_dev(irq);
    if (p)
        WRITE_ONCE(p->irq.disabled, true);
}

void disable_irq(unsigned int irq) {
    disable_irq_nosync(irq);
    struct lkpi_pci *p = irq_dev(irq);
    if (p)
        while (READ_ONCE(p->irq.running))
            cpu_relax();
}

void enable_irq(unsigned int irq) {
    struct lkpi_pci *p = irq_dev(irq);
    if (!p)
        return;
    unsigned long f = lkpi_irq_save();
    WRITE_ONCE(p->irq.disabled, false);
    bool missed = p->irq.pending;
    p->irq.pending = false;
    if (missed && p->irq.requested) { /* (what came while it was off, now: as from its interrupt) */
        int cpu = kpi_host_cpu();
        lkpi_irq_depth[cpu]++;
        run_handler(p);
        lkpi_irq_depth[cpu]--;
    }
    lkpi_irq_restore(f);
}

int irq_set_affinity_hint(unsigned int irq, const void *mask) { return 0; }

/* ---- DMA: direct ---- */

static u64 dev_mask(struct device *d) { return d && d->dma_mask ? *d->dma_mask : DMA_BIT_MASK(32); }

static dma_addr_t checked(struct device *d, u64 phys, size_t size) {
    if (phys + size - 1 > dev_mask(d)) {
        printk("linuxkpi: %s: memory at %llx is beyond what it can reach (%llx)\n", dev_name(d),
               (unsigned long long)phys, (unsigned long long)dev_mask(d));
        return DMA_MAPPING_ERROR;
    }
    return (dma_addr_t)phys;
}

dma_addr_t dma_map_page(struct device *d, struct page *p, size_t offset, size_t size, enum dma_data_direction dir) {
    return checked(d, page_to_phys(p) + offset, size);
}

void dma_unmap_page(struct device *d, dma_addr_t a, size_t size, enum dma_data_direction dir) {}

dma_addr_t dma_map_single(struct device *d, void *p, size_t size, enum dma_data_direction dir) {
    u64 phys = kpi_host_translate(p);
    return phys ? checked(d, phys, size) : DMA_MAPPING_ERROR;
}

void dma_unmap_single(struct device *d, dma_addr_t a, size_t size, enum dma_data_direction dir) {}

dma_addr_t dma_map_resource(struct device *d, phys_addr_t p, size_t size, enum dma_data_direction dir, unsigned long attrs) {
    return (dma_addr_t)p; /* (another device's memory: peer to peer, at its own address) */
}

void dma_unmap_resource(struct device *d, dma_addr_t a, size_t size, enum dma_data_direction dir, unsigned long attrs) {}

int dma_map_sg(struct device *d, struct scatterlist *sg, int n, enum dma_data_direction dir) {
    struct scatterlist *s;
    int i;
    for_each_sg(sg, s, n, i) {
        s->dma_address = checked(d, sg_phys(s), s->length);
        if (s->dma_address == DMA_MAPPING_ERROR)
            return 0;
        s->dma_length = s->length;
    }
    return n;
}

void dma_unmap_sg(struct device *d, struct scatterlist *sg, int n, enum dma_data_direction dir) {}

int dma_map_sgtable(struct device *d, struct sg_table *t, enum dma_data_direction dir, unsigned long attrs) {
    int n = dma_map_sg(d, t->sgl, (int)t->orig_nents, dir);
    if (n <= 0)
        return -EIO;
    t->nents = (unsigned)n;
    return 0;
}

void dma_unmap_sgtable(struct device *d, struct sg_table *t, enum dma_data_direction dir, unsigned long attrs) {}

void *dma_alloc_attrs(struct device *d, size_t size, dma_addr_t *handle, gfp_t gfp, unsigned long attrs) {
    size_t pages = PAGE_ALIGN(size) / PAGE_SIZE;
    u64 phys = kpi_host_pages(pages);
    if (!phys)
        return NULL;
    u64 limit = d ? d->coherent_dma_mask : DMA_BIT_MASK(32);
    if (phys + pages * PAGE_SIZE - 1 > limit) {
        kpi_host_pages_free(phys, pages); /* (our page allocator takes no limit yet) */
        return NULL;
    }
    void *v = kpi_host_phys_to_virt(phys);
    memset(v, 0, pages * PAGE_SIZE);
    *handle = (dma_addr_t)phys;
    return v;
}

void dma_free_attrs(struct device *d, size_t size, void *p, dma_addr_t handle, unsigned long attrs) {
    if (p)
        kpi_host_pages_free((u64)handle, PAGE_ALIGN(size) / PAGE_SIZE);
}

void *dma_alloc_coherent(struct device *d, size_t size, dma_addr_t *handle, gfp_t gfp) {
    return dma_alloc_attrs(d, size, handle, gfp, 0);
}

void dma_free_coherent(struct device *d, size_t size, void *p, dma_addr_t handle) { dma_free_attrs(d, size, p, handle, 0); }

int dma_set_mask(struct device *d, u64 mask) {
    if (!d->dma_mask)
        return -EIO;
    *d->dma_mask = mask;
    return 0;
}

int dma_set_coherent_mask(struct device *d, u64 mask) {
    d->coherent_dma_mask = mask;
    return 0;
}

int dma_set_mask_and_coherent(struct device *d, u64 mask) {
    int r = dma_set_mask(d, mask);
    return r ? r : dma_set_coherent_mask(d, mask);
}

u64 dma_get_mask(struct device *d) { return dev_mask(d); }

u64 dma_get_required_mask(struct device *d) {
    u64 top = kpi_host_max_phys();
    int bits = 32;
    while (bits < 64 && (1ULL << bits) < top)
        bits++;
    return DMA_BIT_MASK(bits);
}

int dma_set_max_seg_size(struct device *d, unsigned int size) { return 0; }
const struct dma_map_ops *get_dma_ops(struct device *d) { return NULL; }
bool use_dma_iommu(struct device *d) { return false; }
size_t dma_max_mapping_size(struct device *d) { return SIZE_MAX; }
bool dma_addressing_limited(struct device *d) { return dev_mask(d) < kpi_host_max_phys() - 1; }
struct iommu_domain *iommu_get_domain_for_dev(struct device *d) { return NULL; }

/* ---- scatterlists ---- */

#define SG_CHAIN 1UL
#define SG_END 2UL

struct page *sg_page(struct scatterlist *sg) { return (struct page *)(sg->page_link & ~(SG_CHAIN | SG_END)); }

void sg_set_page(struct scatterlist *sg, struct page *p, unsigned int len, unsigned int offset) {
    sg->page_link = (unsigned long)p | (sg->page_link & (SG_CHAIN | SG_END));
    sg->length = len, sg->offset = offset;
}

phys_addr_t sg_phys(struct scatterlist *sg) { return page_to_phys(sg_page(sg)) + sg->offset; }

void sg_init_table(struct scatterlist *sg, unsigned int n) {
    memset(sg, 0, n * sizeof *sg);
    if (n)
        sg[n - 1].page_link |= SG_END;
}

struct scatterlist *sg_next(struct scatterlist *sg) {
    if (sg->page_link & SG_END)
        return NULL;
    sg++;
    if (sg->page_link & SG_CHAIN)
        sg = (struct scatterlist *)(sg->page_link & ~(SG_CHAIN | SG_END));
    return sg;
}

int sg_alloc_table(struct sg_table *t, unsigned int n, gfp_t gfp) {
    memset(t, 0, sizeof *t);
    if (!n)
        return -EINVAL;
    t->sgl = kvzalloc(n * sizeof *t->sgl, gfp);
    if (!t->sgl)
        return -ENOMEM;
    sg_init_table(t->sgl, n);
    t->nents = t->orig_nents = n;
    return 0;
}

void sg_free_table(struct sg_table *t) {
    kvfree(t->sgl);
    t->sgl = NULL;
    t->nents = t->orig_nents = 0;
}

/* runs of physically adjacent pages become one entry each, up to max_segment bytes */
int sg_alloc_table_from_pages_segment(struct sg_table *t, struct page **pages, unsigned int n, unsigned int offset,
                                      unsigned long size, unsigned int max_segment, gfp_t gfp) {
    if (!max_segment)
        max_segment = UINT_MAX;
    max_segment = max_segment & PAGE_MASK ? max_segment & PAGE_MASK : PAGE_SIZE;
    unsigned int chunks = 0;
    for (unsigned int i = 0; i < n;) {
        unsigned long len = PAGE_SIZE;
        unsigned int j = i + 1;
        while (j < n && pages[j]->pfn == pages[j - 1]->pfn + 1 && len + PAGE_SIZE <= max_segment)
            j++, len += PAGE_SIZE;
        chunks++;
        i = j;
    }
    int r = sg_alloc_table(t, chunks, gfp);
    if (r)
        return r;
    struct scatterlist *s = t->sgl;
    unsigned long left = size;
    for (unsigned int i = 0; i < n; s = sg_next(s)) {
        unsigned long len = PAGE_SIZE;
        unsigned int j = i + 1;
        while (j < n && pages[j]->pfn == pages[j - 1]->pfn + 1 && len + PAGE_SIZE <= max_segment)
            j++, len += PAGE_SIZE;
        unsigned int off = i == 0 ? offset : 0;
        unsigned long take = len - off < left ? len - off : left;
        sg_set_page(s, pages[i], (unsigned int)take, off);
        left -= take;
        i = j;
    }
    return 0;
}

int sg_alloc_table_from_pages(struct sg_table *t, struct page **pages, unsigned int n, unsigned int offset, unsigned long size, gfp_t gfp) {
    return sg_alloc_table_from_pages_segment(t, pages, n, offset, size, UINT_MAX, gfp);
}

/* ---- power management (runtime: counted; the device stays on) ---- */

int pm_runtime_get_sync(struct device *d) { atomic_inc(&d->power.usage_count); return 0; }
int pm_runtime_get_noresume(struct device *d) { atomic_inc(&d->power.usage_count); return 0; }
int pm_runtime_get_if_in_use(struct device *d) { return atomic_inc_not_zero(&d->power.usage_count) ? 1 : 0; }
int pm_runtime_put(struct device *d) { atomic_dec(&d->power.usage_count); return 0; }
int pm_runtime_put_noidle(struct device *d) { atomic_dec(&d->power.usage_count); return 0; }
int pm_runtime_put_sync(struct device *d) { atomic_dec(&d->power.usage_count); return 0; }
int pm_runtime_put_autosuspend(struct device *d) { atomic_dec(&d->power.usage_count); return 0; }
void pm_runtime_allow(struct device *d) { d->power.runtime_auto = true; }
void pm_runtime_forbid(struct device *d) { d->power.runtime_auto = false; }
void pm_runtime_enable(struct device *d) {}
void pm_runtime_disable(struct device *d) {}
int pm_runtime_set_active(struct device *d) { d->power.runtime_status = 0; return 0; }
void pm_runtime_use_autosuspend(struct device *d) {}
void pm_runtime_dont_use_autosuspend(struct device *d) {}
void pm_runtime_set_autosuspend_delay(struct device *d, int ms) {}
void pm_runtime_mark_last_busy(struct device *d) {}
int pm_runtime_resume(struct device *d) { return 0; }
int pm_runtime_suspended(struct device *d) { return 0; }
int pm_runtime_active(struct device *d) { return 1; }
void dev_pm_domain_detach(struct device *d, bool power_off) {}

/* notifier chains: kept, highest priority first (lkpi_reboot calls the reboot ones) */
static struct notifier_block *pm_chain, *reboot_chain;
static DEFINE_SPINLOCK(chain_lock);

static int chain_add(struct notifier_block **head, struct notifier_block *nb) {
    spin_lock(&chain_lock);
    struct notifier_block **pp = head;
    while (*pp && (*pp)->priority >= nb->priority)
        pp = &(*pp)->next;
    nb->next = *pp;
    *pp = nb;
    spin_unlock(&chain_lock);
    return 0;
}

static int chain_remove(struct notifier_block **head, struct notifier_block *nb) {
    spin_lock(&chain_lock);
    for (struct notifier_block **pp = head; *pp; pp = &(*pp)->next)
        if (*pp == nb) {
            *pp = nb->next;
            break;
        }
    spin_unlock(&chain_lock);
    return 0;
}

int register_pm_notifier(struct notifier_block *nb) { return chain_add(&pm_chain, nb); }
int unregister_pm_notifier(struct notifier_block *nb) { return chain_remove(&pm_chain, nb); }
int register_reboot_notifier(struct notifier_block *nb) { return chain_add(&reboot_chain, nb); }
int unregister_reboot_notifier(struct notifier_block *nb) { return chain_remove(&reboot_chain, nb); }

void lkpi_reboot(void) {
    for (struct notifier_block *nb = reboot_chain; nb; nb = nb->next)
        nb->notifier_call(nb, 1 /* SYS_RESTART */, NULL);
    for (int i = 0; i < ndevs; i++) {
        struct pci_dev *d = &devs[i].pd;
        if (d->driver && d->driver->shutdown)
            d->driver->shutdown(d);
    }
}
