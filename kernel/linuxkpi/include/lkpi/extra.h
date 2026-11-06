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
struct percpu_counter { s64 count; };
static inline void percpu_counter_add(struct percpu_counter *c, s64 a) { __atomic_add_fetch(&c->count, a, __ATOMIC_RELAXED); }
static inline s64 percpu_counter_sum(struct percpu_counter *c) { return c->count; }

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
struct kernfs_open_file { struct seq_file *seq_file; void *priv; struct file *file; };
struct super_block { unsigned long s_magic; };
