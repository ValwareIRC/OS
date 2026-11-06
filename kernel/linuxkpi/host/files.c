#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>
#include <linux/syscall.h>

#include "../host.h"

/* The layer's character devices (/dev/nvidiactl, /dev/nvidia0...) as our kernel's vnodes, and files for the layer:
 * descriptors, and files it opens itself (firmware, the driver's registry). Each open file of one of its devices
 * carries the layer's record of it, and a wait queue that the layer's poll_wait wakes. */

struct chrnode { unsigned major, minor; };
struct hfile { void *rec; struct waitq wq; };

static const struct kpi_host_chr_ops *chr;

void kpi_host_chr_init(const struct kpi_host_chr_ops *ops) { chr = ops; }

static int c_open(struct vnode *v, struct file *f) {
    struct chrnode *n = v->data;
    struct hfile *h = kzalloc(sizeof *h);
    if (!h)
        return -ENOMEM;
    waitq_init(&h->wq);
    f->private = h;
    int r = chr->open(n->major, n->minor, f->flags, (kpi_host_file *)f, &h->rec);
    if (r < 0) {
        f->private = NULL;
        kfree(h);
    }
    return r;
}

static void c_close(struct vnode *v, struct file *f) {
    (void)v;
    struct hfile *h = f->private;
    if (!h)
        return;
    if (h->rec)
        chr->close(h->rec);
    f->private = NULL;
    kfree(h);
}

static void *rec_of(struct file *f) { return f->private ? ((struct hfile *)f->private)->rec : NULL; }

static int64_t c_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)v;
    void *rec = rec_of(f);
    return rec ? chr->ioctl(rec, (unsigned)req, (unsigned long)arg) : -ENOTTY;
}

static int64_t c_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)v;
    void *rec = rec_of(f);
    return rec ? chr->read(rec, buf, len, off) : -EINVAL;
}

static int64_t c_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)v;
    void *rec = rec_of(f);
    return rec ? chr->write(rec, buf, len, off) : -EINVAL;
}

static int c_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)v;
    struct hfile *h = f->private;
    if (!h || !h->rec)
        return POLLERR;
    if (wq)
        *wq = &h->wq;
    return chr->poll(h->rec);
}

static int c_mmap(struct vnode *v, struct file *f, struct mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint64_t off) {
    (void)v, (void)mm; /* (the current program's: the layer finds it itself) */
    void *rec = rec_of(f);
    return rec ? chr->mmap(rec, addr, len, (int)prot, off) : -ENODEV;
}

static const struct vnode_ops chr_ops = {
    .open = c_open, .close = c_close, .ioctl = c_ioctl, .read = c_read, .write = c_write, .poll = c_poll,
    .mmap_dev = c_mmap,
};

static bool ours(struct file *f) { return f && f->vnode && f->vnode->ops == &chr_ops; }

int kpi_host_chr_node(const char *path, unsigned major, unsigned minor) {
    char dir[128];
    size_t len = strlen(path), cut = 0;
    if (len >= sizeof dir)
        return -ENAMETOOLONG;
    memcpy(dir, path, len + 1);
    for (size_t i = 0; i < len; i++)
        if (dir[i] == '/')
            cut = i;
    if (cut > 0) { /* (its directory, e.g. /dev/nvidia-caps) */
        dir[cut] = 0;
        vfs_mkdir_p(dir, 0755);
    }
    struct chrnode *n = kzalloc(sizeof *n);
    struct vnode *v = n ? vnode_alloc(VN_CHR, 0666, &chr_ops) : NULL;
    if (!v) {
        kfree(n);
        return -ENOMEM;
    }
    n->major = major, n->minor = minor;
    v->data = n;
    v->rdev = (major << 8) | minor;
    int r = vfs_create_node(path, v);
    vnode_put(v);
    return r;
}

void kpi_host_poll_wake(kpi_host_file *hf) {
    struct file *f = (struct file *)hf;
    if (ours(f) && f->private)
        waitq_wake_all(&((struct hfile *)f->private)->wq);
}

void kpi_host_file_get(kpi_host_file *hf) { file_ref((struct file *)hf); }
void kpi_host_file_put(kpi_host_file *hf) { file_put((struct file *)hf); }
void *kpi_host_file_rec(kpi_host_file *hf) { return ours((struct file *)hf) ? rec_of((struct file *)hf) : NULL; }
kpi_host_file *kpi_host_fget(int fd) { return (kpi_host_file *)fd_get(fd); }

/* a descriptor held by a placeholder until kpi_host_fd_set puts the real file there */
static struct vnode *placeholder;
static const struct vnode_ops placeholder_ops = {0};

int kpi_host_fd_reserve(bool cloexec) {
    if (!placeholder && !(placeholder = vnode_alloc(VN_ANON, 0, &placeholder_ops)))
        return -ENOMEM;
    vnode_ref(placeholder);
    struct file *f = file_from_vnode(placeholder, O_RDONLY);
    if (!f) {
        vnode_put(placeholder);
        return -ENOMEM;
    }
    int fd = fd_install(f, 0, cloexec);
    if (fd < 0)
        file_put(f);
    return fd;
}

void kpi_host_fd_set(int fd, kpi_host_file *hf) {
    struct fdtable *t = proc_current()->fds;
    struct file *f = (struct file *)hf, *old = NULL;
    file_ref(f);
    uint64_t fl = spin_lock_irqsave(&t->lock);
    if (fd >= 0 && fd < t->cap) {
        old = t->fd[fd];
        t->fd[fd] = f;
    }
    spin_unlock_irqrestore(&t->lock, fl);
    if (old)
        file_put(old);
    else
        file_put(f); /* (no such descriptor: nothing took it) */
}

int kpi_host_fd_close(int fd) { return fd_close(fd); }

bool kpi_host_fd_next(int *fd, kpi_host_file **hf) {
    struct fdtable *t = proc_current()->fds;
    if (!t)
        return false;
    uint64_t fl = spin_lock_irqsave(&t->lock);
    for (int i = *fd < 0 ? 0 : *fd; i < t->cap; i++)
        if (ours(t->fd[i])) {
            file_ref(t->fd[i]);
            *hf = (kpi_host_file *)t->fd[i];
            *fd = i;
            spin_unlock_irqrestore(&t->lock, fl);
            return true;
        }
    spin_unlock_irqrestore(&t->lock, fl);
    return false;
}

kpi_host_file *kpi_host_kopen(const char *path, unsigned flags, unsigned mode, int *err) {
    struct file *f = NULL;
    int r = vfs_open(NULL, path, flags, mode, &f);
    if (err)
        *err = r;
    return r < 0 ? NULL : (kpi_host_file *)f;
}

long kpi_host_kread(kpi_host_file *hf, void *buf, size_t n, uint64_t off) { return (long)file_pread((struct file *)hf, buf, n, off); }

long kpi_host_kwrite(kpi_host_file *hf, const void *buf, size_t n, uint64_t off) {
    struct file *f = (struct file *)hf;
    if (!f->vnode->ops->write)
        return -EINVAL;
    return (long)f->vnode->ops->write(f->vnode, f, buf, n, off);
}

uint64_t kpi_host_file_size(kpi_host_file *hf) { return ((struct file *)hf)->vnode->size; }
