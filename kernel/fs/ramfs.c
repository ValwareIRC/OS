#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

/* In-memory file system: the root file system until osfs exists, and the
 * home of the unpacked initramfs. All buffers passed in are kernel memory.
 * A regular file's data is a pageobj (v->data), which MAP_SHARED maps
 * directly: that is what /dev/shm (shm_open) relies on. */

struct dirent_node {
    char *name;
    struct vnode *v;
    struct dirent_node *next;
};

static const struct vnode_ops ramfs_dir_ops, ramfs_file_ops, ramfs_link_ops;

static struct dirent_node *find(struct vnode *dir, const char *name) {
    for (struct dirent_node *d = dir->data; d; d = d->next)
        if (!strcmp(d->name, name))
            return d;
    return NULL;
}

static char *kstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *c = kmalloc(n);
    if (c)
        memcpy(c, s, n);
    return c;
}

int ramfs_add_node(struct vnode *dir, const char *name, struct vnode *node) {
    if (dir->type != VN_DIR)
        return -ENOTDIR;
    struct dirent_node *d = kmalloc(sizeof *d);
    char *n = kstrdup(name);
    if (!d || !n) {
        kfree(d);
        kfree(n);
        return -ENOMEM;
    }
    uint64_t f = spin_lock_irqsave(&dir->lock);
    if (find(dir, name)) {
        spin_unlock_irqrestore(&dir->lock, f);
        kfree(d);
        kfree(n);
        return -EEXIST;
    }
    vnode_ref(node);
    struct vnode *old_parent = NULL;
    if (node->type == VN_DIR) {
        /* a directory holds its parent (walking up a name must find it alive) */
        old_parent = node->parent != node ? node->parent : NULL;
        node->parent = dir;
        vnode_ref(dir);
        dir->nlink++;
    }
    d->name = n;
    d->v = node;
    d->next = dir->data;
    dir->data = d;
    dir->size++;
    dir->mtime = time_realtime_s();
    spin_unlock_irqrestore(&dir->lock, f);
    vnode_put(old_parent);
    return 0;
}

static int r_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    uint64_t f = spin_lock_irqsave(&dir->lock);
    struct dirent_node *d = find(dir, name);
    if (d)
        vnode_ref(d->v);
    spin_unlock_irqrestore(&dir->lock, f);
    if (!d)
        return -ENOENT;
    *out = d->v;
    return 0;
}

static struct vnode *new_file(uint32_t mode) {
    struct vnode *v = vnode_alloc(VN_REG, mode, &ramfs_file_ops);
    if (v && !(v->data = pageobj_create())) {
        kfree(v);
        return NULL;
    }
    return v;
}

static int r_create(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out) {
    struct vnode *v = new_file(mode);
    if (!v)
        return -ENOMEM;
    int r = ramfs_add_node(dir, name, v);
    if (r < 0) {
        vnode_put(v);
        return r;
    }
    *out = v; /* keep the allocation reference for the caller */
    return 0;
}

static int r_mkdir(struct vnode *dir, const char *name, uint32_t mode) {
    struct vnode *v = vnode_alloc(VN_DIR, mode, &ramfs_dir_ops);
    if (!v)
        return -ENOMEM;
    v->nlink = 2;
    int r = ramfs_add_node(dir, name, v);
    vnode_put(v);
    return r;
}

static int r_symlink(struct vnode *dir, const char *name, const char *target) {
    struct vnode *v = vnode_alloc(VN_LNK, 0777, &ramfs_link_ops);
    if (!v)
        return -ENOMEM;
    v->data = kstrdup(target);
    v->size = strlen(target);
    int r = v->data ? ramfs_add_node(dir, name, v) : -ENOMEM;
    vnode_put(v);
    return r;
}

static int r_link(struct vnode *dir, const char *name, struct vnode *target) {
    if (target->type == VN_DIR)
        return -EPERM;
    int r = ramfs_add_node(dir, name, target);
    if (r == 0)
        target->nlink++;
    return r;
}

static int r_unlink(struct vnode *dir, const char *name, bool dir_only) {
    uint64_t f = spin_lock_irqsave(&dir->lock);
    struct dirent_node **pp = (struct dirent_node **)&dir->data, *d;
    while ((d = *pp) && strcmp(d->name, name))
        pp = &d->next;
    int r = 0;
    if (!d)
        r = -ENOENT;
    else if (dir_only && d->v->type != VN_DIR)
        r = -ENOTDIR;
    else if (!dir_only && d->v->type == VN_DIR)
        r = -EISDIR;
    else if (d->v->type == VN_DIR && d->v->data)
        r = -ENOTEMPTY;
    if (r == 0) {
        *pp = d->next;
        dir->size--;
        if (d->v->type == VN_DIR)
            dir->nlink--;
        else
            d->v->nlink--;
    }
    spin_unlock_irqrestore(&dir->lock, f);
    if (r == 0) {
        vnode_put(d->v);
        kfree(d->name);
        kfree(d);
    }
    return r;
}

static int r_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname) {
    struct vnode *v;
    int r = r_lookup(odir, oname, &v);
    if (r < 0)
        return r;
    struct vnode *existing;
    if (r_lookup(ndir, nname, &existing) == 0) {
        bool same = existing == v;
        r = existing->type == VN_DIR ? r_unlink(ndir, nname, true) : r_unlink(ndir, nname, false);
        vnode_put(existing);
        if (same || r < 0) {
            vnode_put(v);
            return same ? 0 : r;
        }
    }
    uint32_t nlink = v->nlink;
    r = ramfs_add_node(ndir, nname, v);
    if (r == 0) {
        r_unlink(odir, oname, v->type == VN_DIR);
        if (v->type != VN_DIR)
            v->nlink = nlink;
    }
    vnode_put(v);
    return r;
}

static int r_readdir(struct vnode *dir, uint64_t index, char *name, size_t name_size, uint8_t *type, uint64_t *ino) {
    static const uint8_t dt[] = {[VN_REG] = DT_REG, [VN_DIR] = DT_DIR, [VN_CHR] = DT_CHR,
                                 [VN_LNK] = DT_LNK, [VN_FIFO] = DT_FIFO, [VN_SOCK] = 12, [VN_BLK] = 6 /* DT_BLK */};
    if (index == 0 || index == 1) {
        ksnprintf(name, name_size, index == 0 ? "." : "..");
        *type = DT_DIR;
        *ino = index == 0 ? dir->ino : (dir->parent ? dir->parent->ino : dir->ino);
        return 1;
    }
    uint64_t f = spin_lock_irqsave(&dir->lock);
    struct dirent_node *d = dir->data;
    for (uint64_t i = 2; d && i < index; i++)
        d = d->next;
    int r = 0;
    if (d) {
        ksnprintf(name, name_size, "%s", d->name);
        *type = dt[d->v->type];
        *ino = d->v->ino;
        r = 1;
    }
    spin_unlock_irqrestore(&dir->lock, f);
    return r;
}

static int64_t r_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)f;
    return pageobj_read(v->data, buf, len, off);
}

/* The pageobj's size is the truth; v->size mirrors it for stat, lseek and
 * O_APPEND. Copying it under v->lock means the last writer to get here
 * stores the newest value. */
static void sync_size(struct vnode *v) {
    uint64_t fl = spin_lock_irqsave(&v->lock);
    v->size = pageobj_size(v->data);
    v->mtime = time_realtime_s();
    spin_unlock_irqrestore(&v->lock, fl);
}

static int64_t r_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)f;
    int64_t n = pageobj_write(v->data, buf, len, off);
    sync_size(v);
    return n;
}

static int r_truncate(struct vnode *v, uint64_t size) {
    int r = pageobj_truncate(v->data, size);
    sync_size(v);
    return r;
}

static int r_mmap_shared(struct vnode *v, uint32_t prot, struct pageobj **out) {
    (void)prot;
    pageobj_ref(v->data);
    *out = v->data;
    return 0;
}

static void r_release_file(struct vnode *v) { pageobj_put(v->data); }

static int r_readlink(struct vnode *v, char *buf, size_t size) {
    size_t n = v->size < size ? v->size : size;
    memcpy(buf, v->data, n);
    return (int)n;
}

static void r_release_link(struct vnode *v) { kfree(v->data); }

/* The last reference to a directory: its entries go (they held what they
 * name), and so does its hold on its parent. */
static void r_release_dir(struct vnode *v) {
    struct dirent_node *d = v->data;
    while (d) {
        struct dirent_node *next = d->next;
        vnode_put(d->v);
        kfree(d->name);
        kfree(d);
        d = next;
    }
    v->data = NULL;
    if (v->parent && v->parent != v)
        vnode_put(v->parent);
}

static const struct vnode_ops ramfs_dir_ops = {
    .lookup = r_lookup, .create = r_create, .mkdir = r_mkdir, .symlink = r_symlink,
    .link = r_link, .unlink = r_unlink, .rename = r_rename, .readdir = r_readdir, .release = r_release_dir,
};
static const struct vnode_ops ramfs_file_ops = {
    .read = r_read, .write = r_write, .truncate = r_truncate, .mmap_shared = r_mmap_shared,
    .release = r_release_file,
};
static const struct vnode_ops ramfs_link_ops = {
    .readlink = r_readlink, .release = r_release_link,
};

struct vnode *ramfs_create_root(void) {
    struct vnode *v = vnode_alloc(VN_DIR, 0755, &ramfs_dir_ops);
    if (v)
        v->nlink = 2;
    return v;
}

/* A file over an existing page object (a boot module's memory), created at
 * an absolute path with its directories. */
int ramfs_add_object_file(const char *path, struct pageobj *o, uint32_t mode) {
    struct vnode *v = vnode_alloc(VN_REG, mode, &ramfs_file_ops);
    if (!v)
        return -ENOMEM;
    v->data = o;
    v->size = pageobj_size(o);
    char dir[256];
    size_t n = strlen(path);
    while (n && path[n - 1] != '/')
        n--;
    if (n > 1 && n < sizeof dir) {
        memcpy(dir, path, n - 1);
        dir[n - 1] = 0;
        vfs_mkdir_p(dir, 0755);
    }
    int r = vfs_create_node(path, v);
    vnode_put(v);
    return r;
}
