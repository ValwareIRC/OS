#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>

/* memfd_create: an in-memory file that lives in no directory. Wayland
 * clients put pixel buffers and keymaps in one, map it MAP_SHARED, and pass
 * the descriptor to the compositor, which maps it too: both see the same
 * pages because the data lives in a pageobj that mmap maps directly. Seals
 * (fcntl F_ADD_SEALS) let the receiver trust that the sender can no longer
 * shrink, grow or rewrite it. */

struct memfd {
    struct pageobj *obj;
    uint32_t seals;
};

static const struct vnode_ops memfd_ops;

static int64_t memfd_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)f;
    struct memfd *m = v->data;
    return pageobj_read(m->obj, buf, len, off);
}

static int64_t memfd_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)f;
    struct memfd *m = v->data;
    uint64_t fl = spin_lock_irqsave(&v->lock);
    int64_t r = 0;
    if (m->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))
        r = -EPERM;
    else if (off + len > v->size && (m->seals & F_SEAL_GROW))
        r = -EPERM;
    spin_unlock_irqrestore(&v->lock, fl);
    if (r == 0)
        r = pageobj_write(m->obj, buf, len, off);
    fl = spin_lock_irqsave(&v->lock);
    v->size = pageobj_size(m->obj);
    if (r > 0)
        v->mtime = time_realtime_s();
    spin_unlock_irqrestore(&v->lock, fl);
    return r;
}

static int memfd_truncate(struct vnode *v, uint64_t size) {
    struct memfd *m = v->data;
    uint64_t fl = spin_lock_irqsave(&v->lock);
    int r = 0;
    if ((size < v->size && (m->seals & F_SEAL_SHRINK)) || (size > v->size && (m->seals & F_SEAL_GROW)))
        r = -EPERM;
    else
        r = pageobj_truncate(m->obj, size);
    v->size = pageobj_size(m->obj);
    spin_unlock_irqrestore(&v->lock, fl);
    return r;
}

static int memfd_mmap_shared(struct vnode *v, uint32_t prot, struct pageobj **out) {
    struct memfd *m = v->data;
    if ((prot & PROT_WRITE) && (m->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)))
        return -EPERM;
    pageobj_ref(m->obj);
    *out = m->obj;
    return 0;
}

static void memfd_release(struct vnode *v) {
    struct memfd *m = v->data;
    if (m) {
        pageobj_put(m->obj);
        kfree(m);
    }
}

static const struct vnode_ops memfd_ops = {
    .read = memfd_read, .write = memfd_write, .truncate = memfd_truncate, .mmap_shared = memfd_mmap_shared,
    .release = memfd_release,
};

struct vnode *memfd_vnode_create(void) {
    struct vnode *v = vnode_alloc(VN_REG, 0777, &memfd_ops);
    if (!v)
        return NULL;
    struct memfd *m = kzalloc(sizeof *m);
    if (m && !(m->obj = pageobj_create())) {
        kfree(m);
        m = NULL;
    }
    v->data = m;
    if (!m) {
        vnode_put(v);
        return NULL;
    }
    return v;
}

int memfd_add_seals(struct file *f, uint32_t seals) {
    struct vnode *v = f->vnode;
    if (v->ops != &memfd_ops)
        return -EINVAL;
    if (seals & ~(uint32_t)(F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE |
                            F_SEAL_EXEC))
        return -EINVAL;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return -EPERM;
    struct memfd *m = v->data;
    uint64_t fl = spin_lock_irqsave(&v->lock);
    int r = 0;
    if (m->seals & F_SEAL_SEAL)
        r = -EPERM;
    else
        m->seals |= seals;
    spin_unlock_irqrestore(&v->lock, fl);
    return r;
}

int memfd_get_seals(struct file *f) {
    if (f->vnode->ops != &memfd_ops)
        return -EINVAL;
    return (int)((struct memfd *)f->vnode->data)->seals;
}

SYSCALL(memfd_create) {
    UNUSED_ARGS();
    uint32_t flags = (uint32_t)a2;
    if (flags & ~(uint32_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL | MFD_EXEC))
        return -EINVAL; /* MFD_HUGETLB included: there are no huge pages to back it */
    char name[250];
    int64_t n = strncpy_from_user(name, a1, sizeof name);
    if (n < 0)
        return n == -ENAMETOOLONG ? -EINVAL : n;
    struct vnode *v = memfd_vnode_create();
    if (!v)
        return -ENOMEM;
    struct memfd *m = v->data;
    /* Without MFD_ALLOW_SEALING the file is born sealed against sealing. */
    if (flags & MFD_NOEXEC_SEAL)
        m->seals = F_SEAL_EXEC;
    else if (!(flags & MFD_ALLOW_SEALING))
        m->seals = F_SEAL_SEAL;
    return vnode_install_fd(v, O_RDWR, flags & MFD_CLOEXEC);
}
