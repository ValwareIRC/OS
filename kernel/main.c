#include <kernel/medium.h>
#include <kernel/arch.h>
#include <kernel/blkdev.h>
#include <kernel/boot.h>
#include <kernel/drivers.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/net.h>
#include <kernel/pci.h>
#include <kernel/platform.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/usb.h>
#include <kernel/vfs.h>

/* ---- Limine requests (placed in their own section, see linker.ld) ---- */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request boot_fb_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request boot_memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request boot_hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_executable_address_request boot_kaddr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request boot_rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_firmware_type_request boot_firmware_request = {.id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_smbios_request boot_smbios_request = {.id = LIMINE_SMBIOS_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
volatile struct limine_mp_request boot_mp_request = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .flags = LIMINE_MP_REQUEST_X86_64_X2APIC};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {.id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
                                                                             .revision = 0};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

uint64_t hhdm_offset;

static struct cpu bsp_cpu;

/* ---- Boot self-tests: each prints a line the boot test looks for ---- */

static void selftest_memory(void) {
    arch_selftest_breakpoint();

    uint64_t before = pmm_free_pages();
    uint64_t a = pmm_alloc(1), b = pmm_alloc(8);
    KASSERT(a && b && a != b && (a % PAGE_SIZE) == 0);
    pmm_free(a, 1);
    pmm_free(b, 8);
    KASSERT(pmm_free_pages() == before);
    kprintf("selftest: pmm ok\n");

    extern char __text_start[];
    KASSERT(vmm_translate(vmm_kernel_pml4(), (uint64_t)__text_start) ==
            boot_kaddr_request.response->physical_base + ((uint64_t)__text_start - 0xffffffff80000000UL));
    kprintf("selftest: vmm ok\n");

    void *ptrs[64];
    for (int i = 0; i < 64; i++) {
        ptrs[i] = kmalloc(16 + i * 40);
        KASSERT(ptrs[i] && ((uint64_t)ptrs[i] % 16) == 0);
        memset(ptrs[i], i, 16 + i * 40);
    }
    for (int i = 0; i < 64; i++)
        KASSERT(((uint8_t *)ptrs[i])[0] == i);
    for (int i = 0; i < 64; i += 2)
        kfree(ptrs[i]);
    for (int i = 1; i < 64; i += 2)
        kfree(ptrs[i]);
    void *big = kmalloc(1 << 20);
    KASSERT(big);
    kfree(big);
    kprintf("selftest: heap ok\n");
}

#define WORKERS 16
static volatile uint64_t cpus_used;
static volatile int workers_done;

static void worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; i++) {
        __atomic_or_fetch(&cpus_used, 1ULL << (this_cpu()->id & 63), __ATOMIC_RELAXED);
        for (volatile int spin = 0; spin < 2000000; spin++)
            ;
        if (i % 2)
            thread_sleep_ms(4);
        else
            thread_yield();
    }
    __atomic_add_fetch(&workers_done, 1, __ATOMIC_RELEASE);
}

static volatile bool spinners_stop;
static volatile uint64_t spinner_counts[MAX_CPUS + 1];

/* Never yields: only preemption lets the others (and init) run. */
static void spinner(void *arg) {
    volatile uint64_t *count = arg;
    while (!__atomic_load_n(&spinners_stop, __ATOMIC_ACQUIRE))
        (*count)++;
}

static void wait_for(volatile int *counter, int target, const char *what) {
    uint64_t deadline = global_ticks + 10 * HZ;
    while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < target) {
        if (global_ticks > deadline)
            panic("selftest: timed out waiting for %s (%d/%d)", what, *counter, target);
        thread_sleep_ms(10);
    }
}

static void selftest_scheduler(void) {
    for (int i = 0; i < WORKERS; i++)
        KASSERT(thread_create("worker", worker, NULL));
    wait_for(&workers_done, WORKERS, "workers");
    int used = 0;
    for (uint64_t m = cpus_used; m; m &= m - 1)
        used++;
    if (cpu_count > 1 && used < 2)
        panic("selftest: %d workers all ran on one cpu", WORKERS);
    kprintf("selftest: scheduler ok (%d threads across %d cpus)\n", WORKERS, used);

    uint32_t n = cpu_count + 1; /* more spinners than cpus */
    for (uint32_t i = 0; i < n; i++)
        KASSERT(thread_create("spinner", spinner, (void *)&spinner_counts[i]));
    thread_sleep_ms(300);
    __atomic_store_n(&spinners_stop, true, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < n; i++)
        if (spinner_counts[i] == 0)
            panic("selftest: spinner %u never ran (no preemption?)", i);
    kprintf("selftest: preemption ok\n");
}

void fpu_capture_clean_state(void);

/* ---- the big files, read by the kernel itself ----
 *
 * Firmware reads a CD many times slower than the kernel does (a minute and more for the base system and the
 * model; under Hyper-V each of its reads is a trip through the hypervisor), so when the system starts with
 * BIOS (limine.conf: ${FW_TYPE}) the bootloader loads placeholders instead: "OS-LAZY PATH SIZE", the real
 * file's place on the medium. The kernel reads them from there (medium.c): the base system before anything
 * starts, the model in the background, its file appearing once it is whole (PATH.coming meanwhile). With
 * UEFI (a real computer, from a USB stick the kernel can't read yet) the files come whole, as before. */

/* The placeholder's line (an index may follow it: the base system's, scripts/cpio-index.py). */
static bool lazy_stub(const struct limine_file *m, char *path, size_t pn, uint64_t *size) {
    const char *s = m->address;
    if (m->size < 9 || memcmp(s, "OS-LAZY ", 8))
        return false;
    size_t end = m->size < 400 ? m->size : 400, i = 8, k = 0;
    while (i < end && s[i] != ' ' && k + 1 < pn)
        path[k++] = s[i++];
    path[k] = 0;
    uint64_t v = 0;
    for (i++; i < end && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (uint64_t)(s[i] - '0');
    *size = v;
    return k > 0 && v > 0;
}

/* What the placeholder carries after its line, or NULL. */
static const void *stub_tail(const struct limine_file *m, uint64_t *len) {
    const char *s = m->address;
    for (size_t i = 0; i < m->size && i < 400; i++)
        if (s[i] == '\n') {
            *len = m->size - i - 1;
            return *len ? s + i + 1 : NULL;
        }
    *len = 0;
    return NULL;
}

static int medium_reader(void *ctx, uint64_t off, void *buf, size_t n) {
    struct medium_file *f = ctx;
    boot_step_progress("base", off + n < f->size ? off + n : f->size, f->size);
    return medium_read(f, off, buf, n);
}

/* The base system on the medium: its files are read from it as they are used (so it lives as long as they do). */
static struct medium_file base_file;

static bool unpack_from_medium(const char *path, uint64_t size, const void *index, uint64_t index_len,
                               bool *lazily) {
    if (!medium_find(path, size, &base_file)) {
        kprintf("medium: %s isn't on any drive the kernel can read: the base system is missing\n", path);
        return false;
    }
    if (index && initramfs_from_index(index, index_len, &base_file)) {
        initramfs_read_ahead();
        return *lazily = true;
    }
    if (index)
        kprintf("medium: the base system's index doesn't fit it: read whole instead\n");
    return initramfs_unpack_from(medium_reader, &base_file, size);
}

/* "1.8 GB", "640 MB" */
static void size_words(uint64_t bytes, char *out, size_t n) {
    if (bytes >= (1ull << 30))
        ksnprintf(out, n, "%lu.%lu GB", (unsigned long)(bytes >> 30), (unsigned long)((bytes >> 20) % 1024 * 10 / 1024));
    else
        ksnprintf(out, n, "%lu MB", (unsigned long)(bytes >> 20));
}

#define LAZY_PARTS 4
struct lazy_file {
    char dest[160];
    int parts;
    char path[LAZY_PARTS][128];
    uint64_t size[LAZY_PARTS];
    int state; /* LAZY_ON_DISC, LAZY_READING, LAZY_HERE */
};
enum { LAZY_ON_DISC, LAZY_READING, LAZY_HERE };

/* The big files left on the disc (the built-in model, 5 GB): read when something asks for one
 * (/proc/sys/kernel/os_from_disc: the desktop does, when the mind it uses is that model), not at every boot:
 * a person whose mind is elsewhere never waits for it, nor gives it the memory. PATH.ondisc says one is there. */
#define LAZY_MAX 12 /* (the model, and the GPU firmware's files) */
static struct lazy_file *lazies[LAZY_MAX];
static int nlazies;
static spinlock_t lazy_lock;

static void unlink_path(const char *p) {
    struct vnode *dir;
    char leaf[256];
    if (vfs_lookup_parent(NULL, p, &dir, leaf, sizeof leaf) == 0) {
        if (dir->ops->unlink)
            dir->ops->unlink(dir, leaf, false);
        vnode_put(dir);
    }
}

static void touch_path(const char *p) {
    struct file *f;
    if (vfs_open(NULL, p, O_WRONLY | O_CREAT | O_TRUNC, 0444, &f) == 0)
        file_put(f);
}

/* Reads a file's parts from the medium into memory and places it, read-only, at its path. */
static void lazy_loader(void *arg) {
    struct lazy_file *lf = arg;
    char coming[200], ondisc[200];
    ksnprintf(coming, sizeof coming, "%s.coming", lf->dest);
    ksnprintf(ondisc, sizeof ondisc, "%s.ondisc", lf->dest);
    touch_path(coming); /* (first: the file is never neither coming nor on the disc) */
    unlink_path(ondisc);
    uint64_t t0 = time_monotonic_ns(), total = 0;
    struct pageobj *o = pageobj_create();
    enum { CHUNK = 4u << 20 };
    uint64_t phys = pmm_alloc(CHUNK / PAGE_SIZE);
    bool ok = o && phys;
    uint64_t all = 0;
    for (int i = 0; i < lf->parts; i++)
        all += lf->size[i];
    boot_step_start("model");
    for (int i = 0; i < lf->parts && ok; i++) {
        struct medium_file f;
        if (!medium_find(lf->path[i], lf->size[i], &f)) {
            kprintf("medium: %s isn't on any drive the kernel can read\n", lf->path[i]);
            ok = false;
            break;
        }
        for (uint64_t off = 0; off < f.size && ok; off += CHUNK) {
            uint64_t n = f.size - off < CHUNK ? f.size - off : CHUNK;
            ok = medium_read(&f, off, phys_to_virt(phys), n) == 0 &&
                 pageobj_write(o, phys_to_virt(phys), n, total + off) == (int64_t)n;
            boot_step_progress("model", total + off + n, all);
        }
        total += f.size;
    }
    if (phys)
        pmm_free(phys, CHUNK / PAGE_SIZE);
    int state = LAZY_ON_DISC;
    if (ok && ramfs_add_object_file(lf->dest, o, 0444) == 0) {
        uint64_t ms = (time_monotonic_ns() - t0) / 1000000;
        kprintf("medium: %s read (%lu MiB in %lu ms, %lu MiB/s)\n", lf->dest, (unsigned long)(total >> 20),
                (unsigned long)ms, (unsigned long)(ms ? (total >> 20) * 1000 / ms : 0));
        char sz[24];
        size_words(total, sz, sizeof sz);
        boot_step_done("model", sz);
        state = LAZY_HERE;
    } else {
        kprintf("medium: %s couldn't be read; it isn't there this time\n", lf->dest);
        boot_step_failed("model", "couldn't be read from the disc");
        if (o)
            pageobj_put(o);
        touch_path(ondisc); /* (still on the disc: it can be asked for again) */
    }
    unlink_path(coming);
    uint64_t f = spin_lock_irqsave(&lazy_lock);
    lf->state = state;
    spin_unlock_irqrestore(&lazy_lock, f);
}

/* Asks for a file left on the disc (by its path, or "all"): read in the background, PATH.coming meanwhile.
 * 0 when it is being read or already here; -ENOENT when no such file is on the disc. */
int medium_request(const char *dest);
int medium_request(const char *dest) {
    int r = -ENOENT;
    for (int i = 0; i < nlazies; i++) {
        struct lazy_file *lf = lazies[i];
        if (strcmp(dest, "all") && strcmp(dest, lf->dest))
            continue;
        uint64_t f = spin_lock_irqsave(&lazy_lock);
        bool start = lf->state == LAZY_ON_DISC;
        if (start)
            lf->state = LAZY_READING;
        spin_unlock_irqrestore(&lazy_lock, f);
        r = 0;
        if (start) {
            kprintf("medium: %s asked for: reading it from the disc\n", lf->dest);
            if (!thread_create("medium", lazy_loader, lf)) {
                f = spin_lock_irqsave(&lazy_lock);
                lf->state = LAZY_ON_DISC;
                spin_unlock_irqrestore(&lazy_lock, f);
                r = -ENOMEM;
            }
        }
    }
    return r;
}

/* Lets go of a file read from the disc (its memory back): it is on the disc again, to be asked for again.
 * -ENOENT when it isn't one of these, or isn't in memory now. */
int medium_release(const char *dest);
int medium_release(const char *dest) {
    for (int i = 0; i < nlazies; i++) {
        struct lazy_file *lf = lazies[i];
        if (strcmp(dest, lf->dest))
            continue;
        uint64_t f = spin_lock_irqsave(&lazy_lock);
        bool here = lf->state == LAZY_HERE;
        if (here)
            lf->state = LAZY_ON_DISC;
        spin_unlock_irqrestore(&lazy_lock, f);
        if (!here)
            return -ENOENT;
        char ondisc[200];
        ksnprintf(ondisc, sizeof ondisc, "%s.ondisc", lf->dest);
        touch_path(ondisc); /* (first: never neither here nor on the disc) */
        unlink_path(lf->dest);
        kprintf("medium: %s let go of (back on the disc until it is asked for)\n", lf->dest);
        return 0;
    }
    return -ENOENT;
}

/* "PATH on-disc|reading|here BYTES" a line each (/proc/sys/kernel/os_from_disc). */
int medium_status(char *out, size_t n);
int medium_status(char *out, size_t n) {
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < nlazies && o < n; i++) {
        struct lazy_file *lf = lazies[i];
        uint64_t all = 0;
        for (int k = 0; k < lf->parts; k++)
            all += lf->size[k];
        static const char *const names[] = {"on-disc", "reading", "here"};
        o += (size_t)ksnprintf(out + o, n - o, "%s %s %lu\n", lf->dest, names[lf->state], (unsigned long)all);
    }
    return (int)(o < n ? o : n);
}

/* Modules whose string names an absolute path (limine.conf: module_string:
 * /usr/share/...) become read-only files there, backed by the memory the
 * bootloader loaded them into: a multi-gigabyte model costs no copy. */
static void files_from_modules(void) {
    struct limine_module_response *r = module_request.response;
    for (uint64_t i = 0; r && i < r->module_count; i++) {
        const char *dest = r->modules[i]->string;
        if (!dest || dest[0] != '/')
            continue;
        bool seen = false; /* a later part of a file placed already */
        for (uint64_t k = 0; k < i && !seen; k++)
            seen = r->modules[k]->string && !strcmp(r->modules[k]->string, dest);
        if (seen)
            continue;
        char lpath[128];
        uint64_t lsize;
        if (lazy_stub(r->modules[i], lpath, sizeof lpath, &lsize)) { /* left on the disc until asked for */
            struct lazy_file *lf = nlazies < LAZY_MAX ? kzalloc(sizeof *lf) : NULL;
            if (!lf)
                continue;
            ksnprintf(lf->dest, sizeof lf->dest, "%s", dest);
            for (uint64_t k = i; k < r->module_count && lf->parts < LAZY_PARTS; k++)
                if (r->modules[k]->string && !strcmp(r->modules[k]->string, dest) &&
                    lazy_stub(r->modules[k], lf->path[lf->parts], sizeof lf->path[0], &lf->size[lf->parts]))
                    lf->parts++;
            char coming[200];
            ksnprintf(coming, sizeof coming, "%s", dest);
            char *slash = coming + strlen(coming);
            while (slash > coming && *slash != '/')
                slash--;
            *slash = 0;
            if (coming[0])
                vfs_mkdir_p(coming, 0755);
            ksnprintf(coming, sizeof coming, "%s.ondisc", dest);
            touch_path(coming);
            lf->state = LAZY_ON_DISC;
            lazies[nlazies++] = lf;
            uint64_t all = 0;
            for (int k = 0; k < lf->parts; k++)
                all += lf->size[k];
            kprintf("medium: %s left on the disc until it is asked for (%lu MiB)\n", dest, (unsigned long)(all >> 20));
            continue;
        }
        /* A file too big for the boot medium's file system comes in parts
         * (each but the last a whole number of pages), in order, with the
         * same name: they become one file again. */
        struct pageobj *o = NULL;
        int parts = 0;
        bool ok = true;
        for (uint64_t k = i; k < r->module_count && ok; k++) {
            struct limine_file *m = r->modules[k];
            if (!m->string || strcmp(m->string, dest))
                continue;
            struct pageobj *p = pageobj_adopt(o, virt_to_phys_hhdm(m->address), m->size);
            ok = p != NULL;
            o = p ? p : o;
            parts++;
        }
        if (!ok || !o || ramfs_add_object_file(dest, o, 0444) < 0) {
            kprintf("modules: could not place %s\n", dest);
            continue;
        }
        kprintf("modules: %s (%lu MiB in %d part%s, in place)\n", dest, (unsigned long)(pageobj_size(o) >> 20), parts,
                parts == 1 ? "" : "s");
        if (!strcmp(dest, "/usr/share/os/mind/model.gguf"))
            boot_step_done("model", "loaded with the system");
    }
}

static struct limine_file *find_module(const char *suffix) {
    struct limine_module_response *r = module_request.response;
    for (uint64_t i = 0; r && i < r->module_count; i++) {
        const char *path = r->modules[i]->path;
        size_t pl = strlen(path), sl = strlen(suffix);
        if (pl >= sl && !strcmp(path + pl - sl, suffix))
            return r->modules[i];
    }
    return NULL;
}

/* Development aid: QEMU can hand the OS a saved mind and design
 * (-fw_cfg name=opt/os/mind.json,file=... and opt/os/design.json) so
 * the desktop can be previewed without repeating the onboarding. */
int fwcfg_read_file(const char *name, char *buf, size_t size);
void hda_init(void);

long fwcfg_stream_file(const char *name, void (*sink)(void *ctx, const char *p, size_t n), void *ctx);

static void sink_to_file(void *ctx, const char *p, size_t n) { file_write((struct file *)ctx, p, n); }

static void preload_state_from_fwcfg(void) {
    /* her own body, a VRM model (megabytes): streamed in */
    struct file *vf;
    char probe[2];
    if (fwcfg_read_file("opt/os/figure.vrm", probe, sizeof probe) > 0) {
        vfs_mkdir_p("/etc/os", 0700);
        if (vfs_open(NULL, "/etc/os/figure.vrm", O_WRONLY | O_CREAT | O_TRUNC, 0644, &vf) == 0) {
            long n = fwcfg_stream_file("opt/os/figure.vrm", sink_to_file, vf);
            file_put(vf);
            kprintf("preloaded /etc/os/figure.vrm from the host (%ld KB)\n", n >> 10);
        }
    }
    static const char *const names[] = {"mind.json", "design.json", "facet.xml", "app.env", "autorun.sh", "display", "presence",
                                        "figure.xml", "alive.conf"};
    static char buf[131072];
    for (size_t i = 0; i < ARRAY_LEN(names); i++) {
        char key[64], path[64];
        ksnprintf(key, sizeof key, "opt/os/%s", names[i]);
        int n = fwcfg_read_file(key, buf, sizeof buf);
        if (n <= 0)
            continue;
        vfs_mkdir_p("/etc/os", 0700);
        ksnprintf(path, sizeof path, "/etc/os/%s", names[i]);
        struct file *f;
        if (vfs_open(NULL, path, O_WRONLY | O_CREAT | O_TRUNC, 0600, &f) == 0) {
            file_write(f, buf, (size_t)n);
            file_put(f);
            kprintf("preloaded %s from the host\n", path);
        }
    }
}

/* Boot parameters: os.provider=, os.key=, os.model=, os.url= on
 * the kernel command line (limine.conf "cmdline:") become the AI config, so
 * a machine can start already connected. A config handed over by the VM
 * launcher (fw_cfg) wins. */
static bool param(const char *cmdline, const char *name, char *out, size_t n) {
    size_t l = strlen(name);
    for (const char *p = cmdline; p && *p;) {
        while (*p == 0x20)
            p++;
        if (!strncmp(p, name, l) && p[l] == 0x3d) {
            p += l + 1;
            size_t k = 0;
            while (*p && *p != 0x20 && k < n - 1)
                out[k++] = *p++;
            out[k] = 0;
            return true;
        }
        while (*p && *p != 0x20)
            p++;
    }
    return false;
}

static void json_str(char *dst, size_t n, const char *s) {
    size_t k = 0;
    for (; *s && k + 2 < n; s++)
        if (*s != 0x22 && *s != 0x5c && (unsigned char)*s >= 0x20)
            dst[k++] = *s;
    dst[k] = 0;
}

/* `w` is a word of the kernel command line (limine.conf: cmdline) */
static bool cmdline_word(const char *w) {
    struct limine_executable_cmdline_response *r = cmdline_request.response;
    if (!r || !r->cmdline)
        return false;
    size_t n = strlen(w);
    for (const char *p = r->cmdline; *p; p++)
        if ((p == r->cmdline || p[-1] == ' ') && !strncmp(p, w, n) && (p[n] == 0 || p[n] == ' '))
            return true;
    return false;
}

static void mind_from_cmdline(void) {
    struct limine_executable_cmdline_response *r = cmdline_request.response;
    if (!r || !r->cmdline || !r->cmdline[0])
        return;
    char provider[48] = "", key[256] = "", model[96] = "", url[160] = "";
    /* the old working name's prefix is still understood on boot menus written before the rename */
    const char *pre = param(r->cmdline, "os.provider", provider, sizeof provider) ? "os"
                      : param(r->cmdline, "lumen.provider", provider, sizeof provider) ? "lumen" : NULL;
    if (!pre)
        return;
    char name[24];
    ksnprintf(name, sizeof name, "%s.key", pre);
    param(r->cmdline, name, key, sizeof key);
    ksnprintf(name, sizeof name, "%s.model", pre);
    param(r->cmdline, name, model, sizeof model);
    ksnprintf(name, sizeof name, "%s.url", pre);
    param(r->cmdline, name, url, sizeof url);
    struct file *f;
    if (vfs_open(NULL, "/etc/os/mind.json", O_RDONLY, 0, &f) == 0) {
        file_put(f); /* already given one */
        return;
    }
    char p2[48], k2[256], m2[96], u2[160], buf[700];
    json_str(p2, sizeof p2, provider), json_str(k2, sizeof k2, key);
    json_str(m2, sizeof m2, model), json_str(u2, sizeof u2, url);
    int n = ksnprintf(buf, sizeof buf, "{\"provider\":\"%s\",\"base_url\":\"%s\",\"api_key\":\"%s\",\"model\":\"%s\"}\n",
                      p2, u2, k2, m2);
    vfs_mkdir_p("/etc/os", 0700);
    if (vfs_open(NULL, "/etc/os/mind.json", O_WRONLY | O_CREAT | O_TRUNC, 0600, &f) == 0) {
        file_write(f, buf, (size_t)n);
        file_put(f);
        kprintf("boot parameters: AI provider %s\n", p2);
    }
}

#ifdef LKPI
int lkpi_start(void);
static void nvidia_main(void *arg) {
    (void)arg;
    kprintf("nvidia: the layer starts\n");
    lkpi_start();
}
#endif

static void init_main(void *arg) {
    (void)arg;
    bootscreen_start_animation();
    selftest_memory();
    selftest_scheduler();
    time_init();
    fpu_capture_clean_state();

    vfs_init();
    pageobj_keeper_start(); /* (file pages given back when memory is short) */
    devfs_init();
    pci_init(); /* before input_init, since we find virtio input devices on PCI */
    fbdev_init();
    input_init();
    usb_init(); /* drivers/usb: keyboards, mice, sticks (its own thread: nothing waits for it) */
    struct limine_file *initrd = find_module("initrd.cpio");
    if (!initrd) {
        kprintf("no initrd module: kernel-only boot\n");
        kprintf("OS BOOT OK\n");
        platform_test_exit(0x00); /* QEMU exit code 1 = success in tests */
        return;
    }
    /* The drives first: the base system may be the kernel's to read from the medium. */
    boot_step_start("disks");
    storage_init();
    {
        int d = blkdev_count(), c = optical_count();
        char what[48];
        char discs[16] = "";
        if (c)
            ksnprintf(discs, sizeof discs, ", %d disc%s", c, c == 1 ? "" : "s");
        ksnprintf(what, sizeof what, "%d disk%s%s", d, d == 1 ? "" : "s", discs);
        boot_step_done("disks", what);
    }
    /* The base system first (the Freedesktop Platform and what is built on
     * it: /usr), then the OS's own files over it. */
    struct limine_file *base = find_module("base.cpio");
    char bpath[128];
    uint64_t bsize;
    boot_step_start("base");
    char bsz[40] = "";
    if (base && lazy_stub(base, bpath, sizeof bpath, &bsize)) {
        uint64_t ilen;
        const void *index = stub_tail(base, &ilen);
        bool lazily = false;
        if (unpack_from_medium(bpath, bsize, index, ilen, &lazily)) {
            size_words(bsize, bsz, sizeof bsz);
            if (lazily)
                ksnprintf(bsz + strlen(bsz), sizeof bsz - strlen(bsz), ", read as used");
            boot_step_done("base", bsz);
        } else
            boot_step_failed("base", "not on any drive the system can read");
    } else if (base) {
        initramfs_unpack(base->address, base->size);
        size_words(base->size, bsz, sizeof bsz), boot_step_done("base", bsz);
    } else {
        boot_step_done("base", "none in this image");
    }
    initramfs_unpack(initrd->address, initrd->size);
    /* Bundled applications (/apps), kept out of the core initrd. */
    struct limine_file *apps = find_module("apps.cpio");
    if (apps)
        initramfs_unpack(apps->address, apps->size);
    /* The archives are copied into the files now: their memory (gigabytes,
     * with the base system) goes back to be used. */
    uint64_t before = pmm_free_pages();
    struct limine_file *archives[] = {base, initrd, apps};
    for (int i = 0; i < 3; i++)
        if (archives[i])
            pmm_give(virt_to_phys_hhdm(archives[i]->address), archives[i]->size);
    kprintf("mm: %lu MiB of unpacked boot archives given back\n", (pmm_free_pages() - before) * PAGE_SIZE >> 20);
    files_from_modules();
    preload_state_from_fwcfg();
    mind_from_cmdline();
    procfs_init(); /* after the initramfs: /proc and /sys are mounted over its directories */
    sysfs_init();
    hda_init(); /* drivers/hda.c: /dev/dsp, /dev/mixer */
#ifdef LKPI
    /* NVIDIA's driver (linuxkpi): on a thread of its own, since probing a GPU takes a while and boot need not wait;
     * "nonvidia" on the command line leaves it out */
    if (!cmdline_word("nonvidia"))
        KASSERT(thread_create("nvidia", nvidia_main, NULL));
#endif
    netlog_wanted = cmdline_word("netlog");
    net_init();
    kprintf("OS BOOT OK\n");
    proc_start_init("/sbin/init");
}

void kmain(void);
void kmain(void) {
    platform_pointer_early(); /* (before anything that asks the machine: kernel/include/kernel/platform.h) */
    serial_init();
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        panic("bootloader does not support Limine base revision 3");
    KASSERT(boot_hhdm_request.response);
    hhdm_offset = boot_hhdm_request.response->offset;

    fbcon_init(cmdline_word("verbose"));
    kprintf("Operating System kernel starting\n");
    boot_step_start("memory");

    arch_init_bsp(&bsp_cpu);
    cpus[0] = &bsp_cpu;

    pmm_init();
    kprintf("mm: %lu MiB usable, %lu MiB free\n", pmm_total_pages() * PAGE_SIZE >> 20,
            pmm_free_pages() * PAGE_SIZE >> 20);
    {
        char mem[24];
        ksnprintf(mem, sizeof mem, "%lu GB", (unsigned long)((pmm_total_pages() * PAGE_SIZE + (512ull << 20)) >> 30));
        boot_step_done("memory", mem);
    }
    boot_step_start("cpu");
    /* the firmware's map, briefly: where the memory is (big usable stretches only) */
    for (uint64_t i = 0; i < boot_memmap_request.response->entry_count; i++) {
        struct limine_memmap_entry *e = boot_memmap_request.response->entries[i];
        if (e->length >= (64ull << 20))
            kprintf("mm: %lx-%lx type %lu (%lu MiB)\n", e->base, e->base + e->length, e->type, e->length >> 20);
    }
    vmm_init();
    serial_remap();
    kprintf("mm: running on kernel page tables\n");
    heap_init();

    arch_init_interrupts(&bsp_cpu);
    sched_init_cpu();
    smp_init();
    {
        char cores[24];
        ksnprintf(cores, sizeof cores, "%u core%s", cpu_count, cpu_count == 1 ? "" : "s");
        boot_step_done("cpu", cores);
    }

    KASSERT(thread_create("init", init_main, NULL));
    for (;;) /* this boot context is now CPU 0's idle thread */
        cpu_idle_wait();
}
