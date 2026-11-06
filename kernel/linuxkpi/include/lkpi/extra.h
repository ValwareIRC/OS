#pragma once
/* kernel/linuxkpi: the rest of what NVIDIA's glue uses: which Linux it is written for, port and register access,
 * processors, random numbers, and the odds and ends one or two of its files need. */
#include <lkpi/core.h>
#include <lkpi/sched.h>
#include <lkpi/mm.h>
#include <lkpi/device.h>
#include <lkpi/fs.h>

/* The layer stands for Linux 7.2 (the version the glue was measured against: docs/nvidia/plan.md). */
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define LINUX_VERSION_CODE KERNEL_VERSION(7, 2, 0)
#define UTS_RELEASE "7.2.0-os"

/* I/O ports and device registers */
static inline void outb(u8 v, u16 port) { __asm__ __volatile__("outb %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outw(u16 v, u16 port) { __asm__ __volatile__("outw %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outl(u32 v, u16 port) { __asm__ __volatile__("outl %0, %1" ::"a"(v), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ __volatile__("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port) { u32 v; __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u8 readb(const volatile void __iomem *p) { return *(const volatile u8 *)p; }
static inline u16 readw(const volatile void __iomem *p) { return *(const volatile u16 *)p; }
static inline u32 readl(const volatile void __iomem *p) { return *(const volatile u32 *)p; }
static inline u64 readq(const volatile void __iomem *p) { return *(const volatile u64 *)p; }
static inline void writeb(u8 v, volatile void __iomem *p) { *(volatile u8 *)p = v; }
static inline void writew(u16 v, volatile void __iomem *p) { *(volatile u16 *)p = v; }
static inline void writel(u32 v, volatile void __iomem *p) { *(volatile u32 *)p = v; }
static inline void writeq(u64 v, volatile void __iomem *p) { *(volatile u64 *)p = v; }
#define readl_relaxed readl
#define writel_relaxed writel
#define ioread8 readb
#define ioread16 readw
#define ioread32 readl
#define iowrite8 writeb
#define iowrite16 writew
#define iowrite32 writel
#define memcpy_fromio(d, s, n) memcpy((d), (const void *)(s), (n))
#define memcpy_toio(d, s, n) memcpy((void *)(d), (s), (n))
#define memset_io(d, c, n) memset((void *)(d), (c), (n))
#define mmiowb() barrier()

/* processors */
void on_each_cpu(void (*fn)(void *info), void *info, int wait);
void on_each_cpu_cond_mask(bool (*cond)(int cpu, void *info), void (*fn)(void *info), void *info, bool wait, const void *mask);
int numa_mem_id(void);
struct cpumask { DECLARE_BITMAP(bits, CONFIG_NR_CPUS); };
typedef struct cpumask cpumask_t;
extern const struct cpumask *const cpu_online_mask, *const cpu_possible_mask;
#define cpumask_of(c) cpu_online_mask
int set_cpus_allowed_ptr(struct task_struct *t, const struct cpumask *m);
unsigned int cpufreq_get(unsigned int cpu);
struct cpuinfo_x86 { u8 x86, x86_vendor, x86_model, x86_stepping; int x86_clflush_size; u32 x86_capability[32]; };
extern struct cpuinfo_x86 boot_cpu_data;
#define X86_VENDOR_INTEL 0
#define X86_VENDOR_AMD 2
bool boot_cpu_has_lkpi(int feature);
#define boot_cpu_has(f) boot_cpu_has_lkpi(f)
#define static_cpu_has(f) boot_cpu_has_lkpi(f)
#define X86_FEATURE_PAT (0 * 32 + 16)
#define X86_FEATURE_CLFLUSH (0 * 32 + 19)
#define X86_FEATURE_HYPERVISOR (4 * 32 + 31)
#define X86_FEATURE_SEV (19 * 32 + 1)
void clflush_cache_range(void *addr, unsigned int size);
void wbinvd_on_all_cpus(void);

#define percpu_counter_add(c, a) __atomic_add_fetch(&(c)->count, (a), __ATOMIC_RELAXED)
#define percpu_counter_sum(c) ((c)->count)

/* time */
static inline void jiffies_to_timespec64(unsigned long j, struct timespec64 *ts) { *ts = ns_to_timespec64((s64)jiffies_to_nsecs(j)); }

/* random numbers */
void get_random_bytes(void *buf, size_t n);
static inline int wait_for_random_bytes(void) { return 0; }
u32 get_random_u32(void);

/* probing, PCI extras */
static inline void wait_for_device_probe(void) { }
int pci_find_dvsec_capability(struct pci_dev *d, u16 vendor, u16 dvsec);
bool pci_dev_present(const struct pci_device_id *ids);
int pci_bus_read_config_dword(struct pci_bus *b, unsigned int devfn, int where, u32 *v);
int pci_bus_write_config_dword(struct pci_bus *b, unsigned int devfn, int where, u32 v);
#define pci_bus_address(d, bar) ((dma_addr_t)pci_resource_start(d, bar))
static inline bool pci_ats_supported(struct pci_dev *d) { (void)d; return false; }
void pci_assign_unassigned_bus_resources(struct pci_bus *b);
int pcie_reset_flr(struct pci_dev *d, bool probe);
#define PCI_RESET_DO_RESET false
#define PCI_RESET_PROBE true
struct soc_device_attribute { const char *machine, *family, *revision, *serial_number, *soc_id; const void *data; };
static inline const struct soc_device_attribute *soc_device_match(const struct soc_device_attribute *m) { (void)m; return NULL; }
static inline bool iommu_is_dma_domain(struct iommu_domain *d) { (void)d; return false; }

/* memory extras */
#define page_zonenum(p) 0
#define zone_page_state_snapshot(z, item) 0UL
#define NR_FREE_PAGES 0
int sg_alloc_table_from_pages_segment(struct sg_table *t, struct page **pages, unsigned int n, unsigned int offset,
                                      unsigned long size, unsigned int max_segment, gfp_t gfp);
struct dev_pagemap;
static inline void put_dev_pagemap(struct dev_pagemap *p) { (void)p; }
int vfs_fallocate(struct file *f, int mode, loff_t offset, loff_t len);
#define FALLOC_FL_KEEP_SIZE 0x01
#define FALLOC_FL_PUNCH_HOLE 0x02

/* processes, as the driver accounts its memory */
struct pid_namespace;
struct pid_namespace *task_active_pid_ns(struct task_struct *t);
pid_t __task_pid_nr_ns(struct task_struct *t, int type, struct pid_namespace *ns);
extern struct pid_namespace init_pid_ns;
struct cgroup;
static inline struct cgroup *task_cgroup(struct task_struct *t, int id) { (void)t; (void)id; return NULL; }
struct kernfs_open_file { struct seq_file *seq_file; void *priv; struct file *file; struct kernfs_node *kn; struct mutex mutex; };
struct super_block { unsigned long s_magic; char s_id[32]; };

/* event tracing: the driver's tracepoints, kept as calls that do nothing (TRACE_EVENT makes trace_<name>()) */
#define TP_PROTO(...) __VA_ARGS__
#define TP_ARGS(...) __VA_ARGS__
#define TP_STRUCT__entry(...)
#define TP_fast_assign(...)
#define TP_printk(...)
#define TRACE_EVENT(name, proto, args, tstruct, assign, print) static inline void trace_##name(proto) { }
#define DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define DEFINE_EVENT(tmpl, name, proto, args) static inline void trace_##name(proto) { }

/* ACPI extras */
typedef char *acpi_string;
acpi_status acpi_get_next_object(acpi_object_type type, acpi_handle parent, acpi_handle child, acpi_handle *out);

/* device memory pages (peer-to-peer: not used here) */
struct dev_pagemap *lkpi_get_dev_pagemap(unsigned long pfn);
#define get_dev_pagemap(pfn, ...) lkpi_get_dev_pagemap(pfn)

/* the newer two-step DMA mapping (an IOMMU's): never available here, the driver maps page by page */
struct dma_iova_state { dma_addr_t addr; u64 __size; };
static inline bool dma_iova_try_alloc(struct device *d, struct dma_iova_state *s, phys_addr_t p, size_t n) { (void)d; (void)s; (void)p; (void)n; return false; }
static inline size_t dma_iova_size(struct dma_iova_state *s) { return s->__size; }
static inline void dma_iova_free(struct device *d, struct dma_iova_state *s) { (void)d; (void)s; }
static inline dma_addr_t dma_addr_unencrypted(dma_addr_t a) { return a; }
int dma_set_max_seg_size(struct device *d, unsigned int size);

/* control groups (none here: memory is accounted to no group) */
struct cgroup;
static inline struct cgroup *cgroup_get_from_fd(int fd) { (void)fd; return ERR_PTR(-EBADF); }
static inline void cgroup_get(struct cgroup *c) { (void)c; }
static inline void cgroup_put(struct cgroup *c) { (void)c; }
static inline struct cgroup *cgroup_parent(struct cgroup *c) { (void)c; return NULL; }
#define cgroup_subsys_enabled(ss) false

/* hashing */
#define GOLDEN_RATIO_32 0x61C88647
#define GOLDEN_RATIO_64 0x61C8864680B583EBull
static inline u32 hash_32(u32 v, unsigned int bits) { return (v * GOLDEN_RATIO_32) >> (32 - bits); }
static inline u32 hash_64(u64 v, unsigned int bits) { return (u32)((v * GOLDEN_RATIO_64) >> (64 - bits)); }
#define hash_long(v, bits) hash_64((u64)(v), bits)
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len);

/* the rest */
unsigned long get_num_physpages(void);
#define for_each_node_state(n, s) for ((n) = 0; (n) < 1; (n)++)
struct pid *find_vpid(int nr);
struct vm_area_struct *find_vma_intersection(struct mm_struct *mm, unsigned long start, unsigned long end);
#define EFI_BOOT 0
#define EFI_RUNTIME_SERVICES 3
bool efi_enabled(int feature);
int dev_printk_emit(int level, const struct device *d, const char *fmt, ...);
bool dev_is_pci(const struct device *d);
void console_lock(void);
void console_unlock(void);
#define compound_order(p) 0
#define __kuid_val(u) ((u).val)
#define __kgid_val(g) ((g).val)
#define DECLARE_COMPLETION_ONSTACK(n) struct completion n = COMPLETION_INITIALIZER(n)

/* the firmware's framebuffer (what the boot picture is drawn in): the driver keeps clear of it; the layer fills
 * it from the framebuffer Limine handed our kernel */
struct screen_info {
    u8 orig_x, orig_y; u16 ext_mem_k, orig_video_page; u8 orig_video_mode, orig_video_cols, flags, unused2;
    u16 orig_video_ega_bx, unused3; u8 orig_video_lines, orig_video_isVGA; u16 orig_video_points;
    u16 lfb_width, lfb_height, lfb_depth; u32 lfb_base, lfb_size; u16 cl_magic, cl_offset; u16 lfb_linelength;
    u8 red_size, red_pos, green_size, green_pos, blue_size, blue_pos, rsvd_size, rsvd_pos;
    u16 vesapm_seg, vesapm_off, pages, vesa_attributes; u32 capabilities; u32 ext_lfb_base; u8 _reserved[2];
} __attribute__((packed));
#define VIDEO_TYPE_VLFB 0x23
#define VIDEO_TYPE_EFI 0x70
#define VIDEO_CAPABILITY_64BIT_BASE (1 << 1)
struct sysfb_display_info { struct screen_info screen; };
extern struct sysfb_display_info sysfb_primary_display;
extern struct screen_info screen_info;

/* sysfs files the driver keeps (a few attributes under its device) */
struct kernfs_open_file;
struct kernfs_ops;
struct kernfs_node { const char *name; struct kernfs_node *parent; void *priv; atomic_t active; struct { const struct kernfs_ops *ops; } attr; };
#define IRQ_RETVAL(x) ((x) ? IRQ_HANDLED : IRQ_NONE)
static inline bool atomic_inc_unless_negative(atomic_t *v)
{ int c = atomic_read(v); while (c >= 0) { int o = atomic_cmpxchg(v, c, c + 1); if (o == c) return true; c = o; } return false; }
#define list_for_each_prev(p, h) for (p = (h)->prev; p != (h); p = p->prev)
static inline void native_cpuid(unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d)
{ __asm__ __volatile__("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "0"(*a), "2"(*c)); }
const char *dmi_get_system_info(int field);
#define DMI_SYS_VENDOR 4
#define DMI_PRODUCT_NAME 5
#define DMI_BOARD_VENDOR 9
#define DMI_BOARD_NAME 10
phys_addr_t sg_phys(struct scatterlist *sg);
struct pm_domain_lkpi;
int dev_pm_domain_attach(struct device *d, int flags);
void dev_pm_domain_detach(struct device *d, bool power_off);
#define to_pci_host_bridge(n) container_of(n, struct pci_host_bridge, dev)

/* reading a file into the kernel (the driver's registry and firmware go through these) */
struct kvec { void *iov_base; size_t iov_len; };
struct iov_iter { int iter_type; size_t count; const struct kvec *kvec; unsigned long nr_segs; };
#define ITER_SOURCE 1
#define ITER_DEST 0
void iov_iter_kvec(struct iov_iter *i, unsigned int direction, const struct kvec *kvec, unsigned long nr, size_t count);
struct kiocb { struct file *ki_filp; loff_t ki_pos; int ki_flags; u16 ki_ioprio; };
int iocb_flags(struct file *f);
ssize_t seq_read_iter(struct kiocb *k, struct iov_iter *i);

/* the last of it: constants and fields single places in the glue use */
#define MODULE_ALIAS_CHARDEV_MAJOR(m) extern const char __lkpi_alias_unused __attribute__((unused))
struct snd_card { int number; char id[16]; }; /* (the HDMI audio function's card: its number named in a path) */
struct acpi_table_fadt { u32 flags; };
extern struct acpi_table_fadt acpi_gbl_FADT;
#define ACPI_FADT_LOW_POWER_S0 (1u << 21)
#define ACPI_VIDEO_CLASS "video"
#define ACPI_VIDEO_NOTIFY_PROBE 0x81
#define ACPI_TYPE_POWER 0x0C
#define AE_CTRL_TERMINATE 0x4004
#define IOPRIO_DEFAULT 0
#define READ 0
#define WRITE 1
#define DMI_CHASSIS_TYPE 13
#define CAP_SYS_NICE 23
#define LOGLEVEL_EMERG 0
#define LOGLEVEL_ALERT 1
#define LOGLEVEL_CRIT 2
#define LOGLEVEL_ERR 3
#define LOGLEVEL_WARNING 4
#define LOGLEVEL_NOTICE 5
#define LOGLEVEL_INFO 6
#define LOGLEVEL_DEBUG 7
#define TASK_SIZE 0x00007ffffffff000UL
#define X86_EFLAGS_IF 0x00000200UL
#define __GFP_NOMEMALLOC 0x10000u
#define O_TMPFILE 020200000
#define MEM_GOING_ONLINE 5
enum zone_type { ZONE_DMA, ZONE_DMA32, ZONE_NORMAL, ZONE_MOVABLE, __MAX_NR_ZONES };
#define misc_cgrp_id 0
#define DMA_ATTR_REQUIRE_COHERENT (1UL << 12)
#define PCI_DVSEC_HEADER1 0x4
#define PCI_BASE_ADDRESS_SPACE 0x01
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0x00
#define IORESOURCE_ROM_SHADOW (1UL << 1)
#define PCI_HEADER_TYPE_NORMAL 0
#define PCI_HEADER_TYPE_BRIDGE 1
#define PCI_CAP_LIST_ID 0
#define PCI_CAP_LIST_NEXT 1
#define PCIBIOS_SUCCESSFUL 0x00
#define SHRINKER_NUMA_AWARE (1 << 2)
#define PAGE_KERNEL_NOENC PAGE_KERNEL
extern const struct bus_type pci_bus_type;
#define RPM_ACTIVE 0
#define RPM_RESUMING 1
#define RPM_SUSPENDED 2
#define RPM_SUSPENDING 3
struct kernfs_ops { ssize_t (*read)(struct kernfs_open_file *of, char *buf, size_t n, loff_t off); };
