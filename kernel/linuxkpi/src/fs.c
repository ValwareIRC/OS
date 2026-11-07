#include <lkpi/all.h>
#include "../host.h"

/* kernel/linuxkpi: files. The driver's character devices (file_operations) as nodes in /dev (host/files.c makes
 * them our kernel's vnodes), descriptors, files the driver opens itself (firmware, its registry), seq_file, the
 * little of /proc it makes (kept, not shown), and shrinkers. */

int lkpi_mmap(struct file *f, u64 addr, u64 len, int prot, u64 off);

#define PF_LKPI_KBUF 0x80000000u /* (current: the "user" buffer of this read or write is the kernel's) */
#define MAX_POLLQ 8

struct poll_table_struct { struct file *f; };

struct lkpi_file {
    struct file f;
    bool kfile; /* opened by the layer (filp_open), not a device's */
    struct inode kinode;
    struct pollq { wait_queue_head_t *q; struct wait_queue_entry e; } pq[MAX_POLLQ];
    int npq;
};

static struct lkpi_file *lf(struct file *f) { return container_of(f, struct lkpi_file, f); }

/* ---- character device numbers and cdevs ---- */

struct region { unsigned major, first, count; char name[32]; struct list_head all; };
struct dev_inode { dev_t dev; struct inode inode; struct list_head all; };

static DEFINE_MUTEX(chr_lock);
static LIST_HEAD(regions);
static LIST_HEAD(cdevs);
static LIST_HEAD(inodes);
static unsigned next_dynamic = 240; /* (dynamic majors, downwards: as Linux gives them) */

static bool major_used(unsigned major) {
    struct region *r;
    list_for_each_entry(r, &regions, all)
        if (r->major == major)
            return true;
    return false;
}

static int region_add(unsigned major, unsigned first, unsigned count, const char *name) {
    struct region *r = kzalloc(sizeof *r, GFP_KERNEL);
    if (!r)
        return -ENOMEM;
    r->major = major, r->first = first, r->count = count;
    strscpy(r->name, name ? name : "?", sizeof r->name);
    list_add_tail(&r->all, &regions);
    return 0;
}

static struct region *region_of(dev_t dev) {
    struct region *r;
    list_for_each_entry(r, &regions, all)
        if (r->major == MAJOR(dev) && MINOR(dev) >= r->first && MINOR(dev) < r->first + r->count)
            return r;
    return NULL;
}

int alloc_chrdev_region(dev_t *dev, unsigned int first, unsigned int count, const char *name) {
    mutex_lock(&chr_lock);
    while (next_dynamic > 1 && major_used(next_dynamic))
        next_dynamic--;
    unsigned major = next_dynamic--;
    int r = region_add(major, first, count, name);
    mutex_unlock(&chr_lock);
    if (!r)
        *dev = MKDEV(major, first);
    return r;
}

int register_chrdev_region(dev_t from, unsigned int count, const char *name) {
    mutex_lock(&chr_lock);
    int r = region_add(MAJOR(from), MINOR(from), count, name);
    mutex_unlock(&chr_lock);
    return r;
}

void unregister_chrdev_region(dev_t from, unsigned int count) {
    mutex_lock(&chr_lock);
    struct region *r = region_of(from);
    if (r) {
        list_del(&r->all);
        kfree(r);
    }
    mutex_unlock(&chr_lock);
}

void cdev_init(struct cdev *c, const struct file_operations *fops) {
    memset(c, 0, sizeof *c);
    c->ops = fops;
    INIT_LIST_HEAD(&c->list);
}

struct cdev *cdev_alloc(void) {
    struct cdev *c = kzalloc(sizeof *c, GFP_KERNEL);
    if (c)
        INIT_LIST_HEAD(&c->list);
    return c;
}

static int count_nvidia_gpus(void) {
    int n = 0;
    for (struct pci_dev *d = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, NULL); d;
         d = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, d))
        if ((d->class >> 16) == PCI_BASE_CLASS_DISPLAY || d->class == (PCI_CLASS_BRIDGE_OTHER << 8))
            n++;
    return n;
}

/* the /dev nodes made, so a device taken away (cdev_del) takes its nodes too */
struct node { char path[96]; dev_t dev; struct list_head all; };
static LIST_HEAD(nodes);

static void node_add(const char *path, unsigned major, unsigned minor) {
    if (kpi_host_chr_node(path, major, minor) < 0)
        return;
    struct node *n = kzalloc(sizeof *n, GFP_KERNEL);
    if (!n)
        return;
    strscpy(n->path, path, sizeof n->path);
    n->dev = MKDEV(major, minor);
    mutex_lock(&chr_lock);
    list_add_tail(&n->all, &nodes);
    mutex_unlock(&chr_lock);
}

/* the nodes the programs look for (as nvidia-modprobe would make them on Linux) */
static void make_nodes(const char *name, unsigned major, unsigned first, unsigned count) {
    char path[96];
    if (!strcmp(name, "nvidia") || !strcmp(name, "nvidia-frontend")) {
        int gpus = count_nvidia_gpus();
        for (unsigned m = first; m < first + count; m++) {
            if (m == 255)
                node_add("/dev/nvidiactl", major, m);
            else if (m == 254)
                node_add("/dev/nvidia-modeset", major, m);
            else if ((int)m < gpus) {
                snprintf(path, sizeof path, "/dev/nvidia%u", m);
                node_add(path, major, m);
            }
        }
        return;
    }
    for (unsigned m = first; m < first + count && m < first + 64; m++) {
        if (!strcmp(name, "nvidia-uvm"))
            snprintf(path, sizeof path, m == 0 ? "/dev/nvidia-uvm" : "/dev/nvidia-uvm-tools");
        else if (!strcmp(name, "nvidia-caps"))
            snprintf(path, sizeof path, "/dev/nvidia-caps/nvidia-cap%u", m);
        else if (!strcmp(name, "nvidia-caps-imex-channels"))
            snprintf(path, sizeof path, "/dev/nvidia-caps-imex-channels/channel%u", m);
        else
            snprintf(path, sizeof path, count == 1 ? "/dev/%s" : "/dev/%s%u", name, m);
        if (m > 1 && !strcmp(name, "nvidia-uvm"))
            break;
        node_add(path, major, m);
    }
}

int cdev_add(struct cdev *c, dev_t dev, unsigned int count) {
    mutex_lock(&chr_lock);
    c->dev = dev, c->count = count;
    list_add_tail(&c->list, &cdevs);
    struct region *r = region_of(dev);
    char name[32];
    strscpy(name, r ? r->name : "nvidia-dev", sizeof name);
    mutex_unlock(&chr_lock);
    make_nodes(name, MAJOR(dev), MINOR(dev), count);
    return 0;
}

void cdev_del(struct cdev *c) {
    mutex_lock(&chr_lock);
    list_del_init(&c->list);
    struct node *n, *t;
    list_for_each_entry_safe(n, t, &nodes, all)
        if (MAJOR(n->dev) == MAJOR(c->dev) && MINOR(n->dev) >= MINOR(c->dev) && MINOR(n->dev) < MINOR(c->dev) + c->count) {
            kpi_host_chr_node_remove(n->path);
            list_del(&n->all);
            kfree(n);
        }
    mutex_unlock(&chr_lock);
}

int __register_chrdev(unsigned int major, unsigned int first, unsigned int count, const char *name, const struct file_operations *f) {
    dev_t dev;
    int r;
    if (major) {
        dev = MKDEV(major, first);
        r = register_chrdev_region(dev, count, name);
    } else {
        r = alloc_chrdev_region(&dev, first, count, name);
    }
    if (r)
        return r;
    struct cdev *c = cdev_alloc();
    if (!c)
        return -ENOMEM;
    c->ops = f;
    cdev_add(c, dev, count);
    return (int)MAJOR(dev);
}

void __unregister_chrdev(unsigned int major, unsigned int first, unsigned int count, const char *name) {
    unregister_chrdev_region(MKDEV(major, first), count);
}

static struct cdev *cdev_of(dev_t dev) {
    struct cdev *c;
    list_for_each_entry(c, &cdevs, list)
        if (MAJOR(c->dev) == MAJOR(dev) && MINOR(dev) >= MINOR(c->dev) && MINOR(dev) < MINOR(c->dev) + c->count)
            return c;
    return NULL;
}

static struct inode *inode_of(dev_t dev, struct cdev *c) {
    struct dev_inode *di;
    list_for_each_entry(di, &inodes, all)
        if (di->dev == dev)
            return &di->inode;
    di = kzalloc(sizeof *di, GFP_KERNEL);
    if (!di)
        return NULL;
    di->dev = dev;
    struct inode *i = &di->inode;
    i->i_mode = S_IFCHR | 0666;
    i->i_rdev = dev;
    i->i_ino = dev;
    i->i_cdev = c;
    i->i_mapping = &i->i_data;
    address_space_init_once(&i->i_data);
    i->i_data.host = i;
    list_add(&di->all, &inodes);
    return i;
}

/* ---- the devices' files, as our kernel calls them ---- */

static const struct cred file_cred;

static int c_open(unsigned major, unsigned minor, unsigned flags, kpi_host_file *hf, void **rec) {
    dev_t dev = MKDEV(major, minor);
    mutex_lock(&chr_lock);
    struct cdev *c = cdev_of(dev);
    struct inode *i = c ? inode_of(dev, c) : NULL;
    mutex_unlock(&chr_lock);
    if (!c || !i)
        return -ENXIO;
    struct lkpi_file *l = kzalloc(sizeof *l, GFP_KERNEL);
    if (!l)
        return -ENOMEM;
    struct file *f = &l->f;
    f->f_op = c->ops;
    f->f_inode = i;
    f->f_mapping = i->i_mapping;
    f->f_flags = flags;
    unsigned acc = flags & 3;
    f->f_mode = (acc != 1 ? FMODE_READ : 0) | (acc != 0 ? FMODE_WRITE : 0);
    f->f_cred = &file_cred;
    f->lkpi_file = hf;
    int r = f->f_op && f->f_op->open ? f->f_op->open(i, f) : 0;
    if (r) {
        kfree(l);
        return r;
    }
    *rec = f;
    return 0;
}

static void c_close(void *rec) {
    struct file *f = rec;
    struct lkpi_file *l = lf(f);
    for (int k = 0; k < l->npq; k++)
        lkpi_waitqueue_remove(l->pq[k].q, &l->pq[k].e);
    l->npq = 0;
    if (f->f_op && f->f_op->release)
        f->f_op->release(f->f_inode, f);
    kfree(l);
}

static long c_ioctl(void *rec, unsigned int cmd, unsigned long arg) {
    struct file *f = rec;
    if (f->f_op && f->f_op->unlocked_ioctl)
        return f->f_op->unlocked_ioctl(f, cmd, arg);
    return -ENOTTY;
}

static long kbuf_io(struct file *f, void *kbuf, size_t n, u64 off, bool write) {
    ssize_t (*rd)(struct file *, char __user *, size_t, loff_t *) = f->f_op ? f->f_op->read : NULL;
    ssize_t (*wr)(struct file *, const char __user *, size_t, loff_t *) = f->f_op ? f->f_op->write : NULL;
    if (write ? !wr : !rd)
        return -EINVAL;
    struct task_struct *t = current;
    loff_t pos = (loff_t)off;
    t->flags |= PF_LKPI_KBUF;
    long r = write ? (long)wr(f, kbuf, n, &pos) : (long)rd(f, kbuf, n, &pos);
    t->flags &= ~PF_LKPI_KBUF;
    return r;
}

static long c_read(void *rec, void *kbuf, size_t n, u64 off) { return kbuf_io(rec, kbuf, n, off, false); }
static long c_write(void *rec, const void *kbuf, size_t n, u64 off) { return kbuf_io(rec, (void *)kbuf, n, off, true); }

static int c_poll(void *rec) {
    struct file *f = rec;
    if (!f->f_op || !f->f_op->poll)
        return POLLIN | POLLOUT;
    struct poll_table_struct pt = {f};
    return (int)f->f_op->poll(f, &pt);
}

static int c_mmap(void *rec, u64 addr, u64 len, int prot, u64 off) { return lkpi_mmap(rec, addr, len, prot, off); }

static const struct kpi_host_chr_ops chr_ops = {c_open, c_close, c_ioctl, c_read, c_write, c_poll, c_mmap};

void lkpi_fs_init(void) { kpi_host_chr_init(&chr_ops); }

/* a wake on a queue the file's poll named: our kernel's poll and epoll look at the file again */
static int poll_wake(struct wait_queue_entry *e, unsigned mode, int flags, void *key) {
    kpi_host_poll_wake(e->private);
    return 0;
}

void poll_wait(struct file *f, wait_queue_head_t *q, poll_table *p) {
    if (!p || !q || !p->f)
        return;
    struct lkpi_file *l = lf(p->f);
    for (int k = 0; k < l->npq; k++)
        if (l->pq[k].q == q)
            return; /* (each queue once a file, for as long as it is open) */
    if (l->npq == MAX_POLLQ) {
        printk("linuxkpi: a file waits on more than %d queues\n", MAX_POLLQ);
        return;
    }
    struct pollq *w = &l->pq[l->npq++];
    w->q = q;
    w->e = (struct wait_queue_entry){.private = p->f->lkpi_file, .func = poll_wake};
    lkpi_waitqueue_add(q, &w->e);
}

loff_t noop_llseek(struct file *f, loff_t off, int whence) { return f->f_pos; }
loff_t no_llseek(struct file *f, loff_t off, int whence) { return -ESPIPE; }
int nonseekable_open(struct inode *i, struct file *f) { return 0; }

/* ---- descriptors ---- */

struct file *fget(unsigned int fd) {
    kpi_host_file *hf = kpi_host_fget((int)fd);
    if (!hf)
        return NULL;
    struct file *f = kpi_host_file_rec(hf);
    if (!f) {
        kpi_host_file_put(hf); /* (not one of the driver's: the driver only looks for its own) */
        return NULL;
    }
    return f; /* (the reference taken: fput lets it go) */
}

void get_file_lkpi(struct file *f) { kpi_host_file_get(f->lkpi_file); }

void fput(struct file *f) {
    if (!f)
        return;
    struct lkpi_file *l = lf(f);
    kpi_host_file_put(f->lkpi_file);
    if (l->kfile)
        kfree(l);
}

int get_unused_fd_flags(unsigned int flags) { return kpi_host_fd_reserve((flags & O_CLOEXEC) != 0); }
void put_unused_fd(unsigned int fd) { kpi_host_fd_close((int)fd); }

void lkpi_fd_install(unsigned int fd, struct file *f) {
    kpi_host_fd_set((int)fd, f->lkpi_file);
    kpi_host_file_put(f->lkpi_file); /* (the caller's reference is the descriptor's now) */
}

int close_fd(unsigned int fd) { return kpi_host_fd_close((int)fd); }

struct file *anon_inode_getfile(const char *name, const struct file_operations *fops, void *priv, int flags) {
    return ERR_PTR(-ENOSYS);
}

int iterate_fd(void *files, unsigned int n, int (*fn)(const void *p, struct file *file, unsigned int fd), const void *p) {
    int fd = (int)n;
    kpi_host_file *hf;
    while (kpi_host_fd_next(&fd, &hf)) {
        struct file *f = kpi_host_file_rec(hf);
        int r = f ? fn(p, f, (unsigned)fd) : 0;
        kpi_host_file_put(hf);
        if (r)
            return r;
        fd++;
    }
    return 0;
}

/* files the driver opens itself */
struct file *filp_open(const char *path, int flags, umode_t mode) {
    int err = 0;
    kpi_host_file *hf = kpi_host_kopen(path, (unsigned)flags, mode, &err);
    if (!hf)
        return ERR_PTR(err ? err : -ENOENT);
    struct lkpi_file *l = kzalloc(sizeof *l, GFP_KERNEL);
    if (!l) {
        kpi_host_file_put(hf);
        return ERR_PTR(-ENOMEM);
    }
    l->kfile = true;
    l->kinode.i_size = (loff_t)kpi_host_file_size(hf);
    l->kinode.i_mode = S_IFREG | 0644;
    l->f.f_inode = &l->kinode;
    l->f.f_flags = (unsigned)flags;
    l->f.lkpi_file = hf;
    return &l->f;
}

int filp_close(struct file *f, void *id) {
    fput(f);
    return 0;
}

ssize_t kernel_read(struct file *f, void *buf, size_t n, loff_t *pos) {
    long r = kpi_host_kread(f->lkpi_file, buf, n, pos ? (u64)*pos : 0);
    if (r > 0 && pos)
        *pos += r;
    return r;
}

ssize_t kernel_write(struct file *f, const void *buf, size_t n, loff_t *pos) {
    long r = kpi_host_kwrite(f->lkpi_file, buf, n, pos ? (u64)*pos : 0);
    if (r > 0 && pos)
        *pos += r;
    return r;
}

/* (the driver's temporary files: for saving video memory over a suspend, which this system does not do yet) */
struct file *shmem_kernel_file_setup(const char *name, loff_t size, unsigned long flags) { return ERR_PTR(-ENOSYS); }
int vfs_fallocate(struct file *f, int mode, loff_t off, loff_t len) { return -EOPNOTSUPP; }

void address_space_init_once(struct address_space *m) {
    memset(m, 0, sizeof *m);
    init_rwsem(&m->i_mmap_rwsem);
}

int iocb_flags(struct file *f) { return 0; }

void iov_iter_kvec(struct iov_iter *i, unsigned int direction, const struct kvec *kvec, unsigned long nr, size_t count) {
    i->iter_type = (int)direction, i->kvec = kvec, i->nr_segs = nr, i->count = count;
}

/* ---- seq_file (single_open's kind) ---- */

typedef int (*show_fn)(struct seq_file *s, void *v);

static void seq_room(struct seq_file *s, size_t more) {
    if (s->count + more + 1 <= s->size)
        return;
    size_t want = (s->size ? s->size : 256);
    while (want < s->count + more + 1)
        want *= 2;
    if (want > (1 << 20))
        return; /* (a megabyte at most: the rest is cut) */
    char *b = kvmalloc(want, GFP_KERNEL);
    if (!b)
        return;
    if (s->buf)
        memcpy(b, s->buf, s->count);
    kvfree(s->buf);
    s->buf = b;
    s->size = want;
}

void seq_write(struct seq_file *s, const void *d, size_t n) {
    seq_room(s, n);
    size_t k = s->count + n < s->size ? n : (s->size > s->count + 1 ? s->size - s->count - 1 : 0);
    memcpy(s->buf + s->count, d, k);
    s->count += k;
}

void seq_puts(struct seq_file *s, const char *str) { seq_write(s, str, strlen(str)); }
void seq_putc(struct seq_file *s, char c) { seq_write(s, &c, 1); }

void seq_printf(struct seq_file *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *t = kvasprintf(GFP_KERNEL, fmt, ap);
    va_end(ap);
    if (t) {
        seq_puts(s, t);
        kfree(t);
    }
}

int single_open_size(struct file *f, int (*show)(struct seq_file *s, void *v), void *data, size_t size) {
    struct seq_file *s = kzalloc(sizeof *s, GFP_KERNEL);
    if (!s)
        return -ENOMEM;
    s->private = data;
    s->op = (const void *)show;
    s->file = f;
    f->private_data = s;
    return 0;
}

int single_open(struct file *f, int (*show)(struct seq_file *s, void *v), void *data) { return single_open_size(f, show, data, 0); }

int single_release(struct inode *i, struct file *f) {
    struct seq_file *s = f->private_data;
    if (s) {
        kvfree(s->buf);
        kfree(s);
        f->private_data = NULL;
    }
    return 0;
}

ssize_t seq_read(struct file *f, char __user *b, size_t n, loff_t *p) {
    struct seq_file *s = f->private_data;
    if (!s)
        return -EINVAL;
    if (*p == 0) {
        s->count = 0;
        ((show_fn)s->op)(s, s->private);
    }
    if ((size_t)*p >= s->count)
        return 0;
    size_t k = s->count - (size_t)*p < n ? s->count - (size_t)*p : n;
    if (copy_to_user(b, s->buf + *p, k))
        return -EFAULT;
    *p += (loff_t)k;
    return (ssize_t)k;
}

ssize_t seq_read_iter(struct kiocb *k, struct iov_iter *i) { return -EINVAL; }

loff_t seq_lseek(struct file *f, loff_t o, int w) {
    if (w != 0 || o < 0)
        return -EINVAL;
    f->f_pos = o;
    return o;
}

/* ---- /proc: the driver's entries are kept (nothing shows them yet) ---- */

struct proc_dir_entry { char name[48]; struct proc_dir_entry *parent; const struct proc_ops *ops; void *data; };

static struct proc_dir_entry *pde_new(const char *name, struct proc_dir_entry *parent, const struct proc_ops *ops, void *data) {
    struct proc_dir_entry *e = kzalloc(sizeof *e, GFP_KERNEL);
    if (e) {
        strscpy(e->name, name, sizeof e->name);
        e->parent = parent, e->ops = ops, e->data = data;
    }
    return e;
}

struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent) { return pde_new(name, parent, NULL, NULL); }
struct proc_dir_entry *proc_mkdir_mode(const char *name, umode_t mode, struct proc_dir_entry *parent) { return pde_new(name, parent, NULL, NULL); }

struct proc_dir_entry *proc_create_data(const char *name, umode_t mode, struct proc_dir_entry *parent, const struct proc_ops *ops, void *data) {
    return pde_new(name, parent, ops, data);
}

struct proc_dir_entry *proc_symlink(const char *name, struct proc_dir_entry *parent, const char *dest) { return pde_new(name, parent, NULL, NULL); }
void proc_remove(struct proc_dir_entry *e) { kfree(e); }
void remove_proc_entry(const char *name, struct proc_dir_entry *parent) {}
void proc_remove_subtree_lkpi(struct proc_dir_entry *e) { kfree(e); }
void *pde_data(const struct inode *i) { return i->i_private; }

/* ---- firmware: from /lib/firmware, read whole ---- */

int request_firmware(const struct firmware **fw, const char *name, struct device *d) {
    *fw = NULL;
    char path[160];
    snprintf(path, sizeof path, "/lib/firmware/%s", name);
    int err = 0;
    kpi_host_file *hf = kpi_host_kopen(path, 0 /* O_RDONLY */, 0, &err);
    if (!hf) {
        /* still on the disc (the boot leaves big files there until asked for): asked for, and waited for */
        char ondisc[176];
        snprintf(ondisc, sizeof ondisc, "%s.ondisc", path);
        kpi_host_file *mark = kpi_host_kopen(ondisc, 0, 0, NULL);
        if (mark) {
            kpi_host_file_put(mark);
            if (kpi_host_medium_request(path) == 0) {
                printk("linuxkpi: firmware %s: reading it from the disc\n", path);
                for (int waited = 0; !hf && waited < 1200; waited++) { /* (two minutes at most) */
                    msleep(100);
                    hf = kpi_host_kopen(path, 0, 0, &err);
                }
            }
        }
    }
    if (!hf) {
        printk("linuxkpi: no firmware %s (%d)\n", path, err);
        return err ? err : -ENOENT;
    }
    size_t size = (size_t)kpi_host_file_size(hf);
    struct firmware *f = kzalloc(sizeof *f, GFP_KERNEL);
    u8 *data = size ? vmalloc(size) : NULL;
    if (!f || (size && !data)) {
        kfree(f);
        vfree(data);
        kpi_host_file_put(hf);
        return -ENOMEM;
    }
    size_t got = 0;
    while (got < size) {
        long r = kpi_host_kread(hf, data + got, size - got, got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    kpi_host_file_put(hf);
    if (got != size) {
        printk("linuxkpi: firmware %s: read %zu of %zu bytes\n", path, got, size);
        vfree(data);
        kfree(f);
        return -EIO;
    }
    f->data = data, f->size = size;
    *fw = f;
    return 0;
}

int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *d) { return request_firmware(fw, name, d); }

void release_firmware(const struct firmware *fw) {
    if (!fw)
        return;
    vfree(fw->data);
    kfree(fw);
}

bool try_module_get(void *m) { return true; }
void module_put(void *m) {}

/* ---- shrinkers: kept; lkpi_shrink asks them for memory back ---- */

static LIST_HEAD(shrinkers);
static DEFINE_MUTEX(shrink_lock);

struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...) {
    struct shrinker *s = kzalloc(sizeof *s, GFP_KERNEL);
    if (s) {
        s->flags = flags;
        s->seeks = DEFAULT_SEEKS;
        INIT_LIST_HEAD(&s->list);
    }
    return s;
}

void shrinker_register(struct shrinker *s) {
    mutex_lock(&shrink_lock);
    list_add_tail(&s->list, &shrinkers);
    mutex_unlock(&shrink_lock);
}

void shrinker_free(struct shrinker *s) {
    if (!s)
        return;
    mutex_lock(&shrink_lock);
    if (!list_empty(&s->list))
        list_del_init(&s->list);
    mutex_unlock(&shrink_lock);
    kfree(s);
}

int register_shrinker(struct shrinker *s, const char *fmt, ...) {
    INIT_LIST_HEAD(&s->list);
    shrinker_register(s);
    return 0;
}

void unregister_shrinker(struct shrinker *s) {
    mutex_lock(&shrink_lock);
    if (!list_empty(&s->list))
        list_del_init(&s->list);
    mutex_unlock(&shrink_lock);
}

unsigned long lkpi_shrink(unsigned long want) {
    unsigned long freed = 0;
    mutex_lock(&shrink_lock);
    struct shrinker *s;
    list_for_each_entry(s, &shrinkers, list) {
        struct shrink_control sc = {.gfp_mask = GFP_KERNEL, .nr_to_scan = want - freed};
        if (s->count_objects && !s->count_objects(s, &sc))
            continue;
        unsigned long r = s->scan_objects ? s->scan_objects(s, &sc) : 0;
        if (r != SHRINK_STOP && r != SHRINK_EMPTY)
            freed += r;
        if (freed >= want)
            break;
    }
    mutex_unlock(&shrink_lock);
    return freed;
}
