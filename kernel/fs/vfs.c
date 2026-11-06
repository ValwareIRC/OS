#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/ns.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

#define MAX_SYMLINK_DEPTH 8
#define NAME_MAX 255
#define PATH_MAX 4096

static struct vnode *root;
static uint64_t next_ino = 1;

struct vnode *vfs_root(void) { return root; }

struct vnode *vnode_alloc(enum vnode_type type, uint32_t mode, const struct vnode_ops *ops) {
    struct vnode *v = kzalloc(sizeof *v);
    if (!v)
        return NULL;
    v->type = type;
    v->mode = mode & 07777;
    v->ops = ops;
    v->refcount = 1;
    v->nlink = 1;
    v->ino = __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED);
    v->mtime = time_realtime_s();
    struct process *p = proc_current(); /* owned by whoever made it */
    if (p) {
        v->uid = p->euid;
        v->gid = p->egid;
    }
    return v;
}

void vnode_ref(struct vnode *v) { __atomic_add_fetch(&v->refcount, 1, __ATOMIC_RELAXED); }

void vnode_put(struct vnode *v) {
    if (!v)
        return;
    int left = __atomic_sub_fetch(&v->refcount, 1, __ATOMIC_ACQ_REL);
    if (left < 0) /* let go of more often than taken: say what it is, not just "double free" */
        panic("vnode_put: %p released once too often (type %d, ops %p, ino %lu, mode %o)", (void *)v, (int)v->type,
              (void *)v->ops, (unsigned long)v->ino, v->mode);
    if (left == 0) {
        if (v->ops && v->ops->release)
            v->ops->release(v);
        v->refcount = -1000; /* freed: a later put says so above, before the heap notices */
        kfree(v);
    }
}

/* ---- permissions ---- */

bool proc_has_cap(int cap);

/* privileged over v: the capability, and v's owner and group named in the caller's user namespace */
static bool cap_over(const struct vnode *v, int cap) {
    return proc_has_cap(cap) && uid_to_user(v->uid) != OVERFLOW_ID && gid_to_user(v->gid) != OVERFLOW_ID;
}

#define CAP_CHOWN 0
#define CAP_DAC_OVERRIDE 1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER 3

static int permission_as(const struct vnode *v, int mask, uint32_t uid, uint32_t gid, bool caps) {
    if (!mask)
        return 0;
    if (caps) {
        if (v->type == VN_DIR) {
            if (cap_over(v, CAP_DAC_OVERRIDE) || (!(mask & MAY_WRITE) && cap_over(v, CAP_DAC_READ_SEARCH)))
                return 0;
        } else {
            /* execute: some x bit, even for the privileged */
            bool exec_ok = !(mask & MAY_EXEC) || (v->mode & 0111);
            if (exec_ok && cap_over(v, CAP_DAC_OVERRIDE))
                return 0;
            if (mask == MAY_READ && cap_over(v, CAP_DAC_READ_SEARCH))
                return 0;
        }
    }
    uint32_t bits = uid == v->uid ? (v->mode >> 6) & 7 : gid == v->gid ? (v->mode >> 3) & 7 : v->mode & 7;
    return (bits & (uint32_t)mask) == (uint32_t)mask ? 0 : -EACCES;
}

int vfs_permission(const struct vnode *v, int mask) {
    struct process *p = proc_current();
    if (!p) /* the kernel itself */
        return 0;
    return permission_as(v, mask, p->euid, p->egid, true);
}

int vfs_permission_real(const struct vnode *v, int mask) {
    struct process *p = proc_current();
    if (!p)
        return 0;
    /* access(): the real user; its capabilities only if the real user is root */
    return permission_as(v, mask, p->uid, p->gid, p->uid == 0);
}

int vfs_may_create(const struct vnode *dir) { return vfs_permission(dir, MAY_WRITE | MAY_EXEC); }

int vfs_may_delete(const struct vnode *dir, const struct vnode *victim) {
    int r = vfs_permission(dir, MAY_WRITE | MAY_EXEC);
    if (r < 0)
        return r;
    struct process *p = proc_current();
    if (p && (dir->mode & 01000) && victim && p->euid != victim->uid && p->euid != dir->uid &&
        !cap_over(victim, CAP_FOWNER))
        return -EPERM; /* a sticky directory (/tmp): only the owner takes a name out */
    return 0;
}

bool vfs_owner_or(const struct vnode *v, int cap) {
    struct process *p = proc_current();
    return !p || p->euid == v->uid || cap_over(v, cap);
}

uint32_t vnode_stat_mode(const struct vnode *v) {
    static const uint32_t type_bits[] = {
        [VN_REG] = S_IFREG, [VN_DIR] = S_IFDIR, [VN_CHR] = S_IFCHR,
        [VN_LNK] = S_IFLNK, [VN_FIFO] = S_IFIFO, [VN_SOCK] = S_IFSOCK, [VN_ANON] = 0, [VN_BLK] = S_IFBLK,
    };
    return type_bits[v->type] | v->mode;
}

void vfs_init(void) {
    root = ramfs_create_root();
    KASSERT(root);
    root->parent = root;
    mount_init(root);
    /* POSIX shared memory: shm_open() is open() under /dev/shm in libc. */
    vfs_mkdir_p("/dev", 0755);
    vfs_mkdir_p("/dev/shm", 01777);
    /* the shared scratch places every system has (OSTree keeps its temporary files in /var/tmp) */
    vfs_mkdir_p("/tmp", 01777);
    vfs_mkdir_p("/var/tmp", 01777);
}

static int walk(const struct path *base, const char *path, int flags, struct path *out, char *last,
                size_t last_size, int depth);

/* Follows the symlink `link`, found in directory `dir`. */
static int follow_link(const struct path *dir, struct vnode *link, struct path *out, int depth) {
    if (depth >= MAX_SYMLINK_DEPTH)
        return -ELOOP;
    if (link->ops->follow) { /* a magic link: the object itself */
        struct path t = {0};
        int r = link->ops->follow(link, &t);
        if (r == 0 && t.mnt) {
            *out = t; /* exactly where it was opened */
        } else if (r == 0) {
            path_for_vnode(t.v, out);
            vnode_put(t.v);
            path_cross(out);
        }
        return r;
    }
    char *target = kmalloc(PATH_MAX);
    if (!target)
        return -ENOMEM;
    int n = link->ops->readlink ? link->ops->readlink(link, target, PATH_MAX - 1) : -EINVAL;
    if (n < 0) {
        kfree(target);
        return n;
    }
    target[n] = 0;
    int r = walk(dir, target, LOOKUP_FOLLOW, out, NULL, 0, depth + 1);
    kfree(target);
    return r;
}

/* Walks `path` from `base` (NULL: the cwd; absolute paths start at the
 * process root). With LOOKUP_PARENT, stops before the last component,
 * copies its name into `last`, and returns the directory. */
static int walk(const struct path *base, const char *path, int flags, struct path *out, char *last,
                size_t last_size, int depth) {
    if (!path[0])
        return -ENOENT;
    struct path proot, cur;
    fs_context(&proot, base || path[0] == '/' ? NULL : &cur);
    if (path[0] == '/') {
        cur = proot;
        path_get(&cur);
    } else if (base) {
        cur = *base;
        path_get(&cur);
    }
    path_cross(&cur);

    const char *p = path;
    char name[NAME_MAX + 1];
    int r = 0;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p) {
            if ((flags & LOOKUP_PARENT) && last && last_size) /* path was "/" or ended with '/' */
                ksnprintf(last, last_size, ".");
            break;
        }
        size_t len = 0;
        while (p[len] && p[len] != '/')
            len++;
        if (len > NAME_MAX) {
            r = -ENAMETOOLONG;
            break;
        }
        memcpy(name, p, len);
        name[len] = 0;
        p += len;
        const char *rest = p;
        while (*rest == '/')
            rest++;
        bool is_last = !*rest;

        if (cur.v->type != VN_DIR) {
            r = -ENOTDIR;
            break;
        }
        if (is_last && (flags & LOOKUP_PARENT)) {
            if (last)
                ksnprintf(last, last_size, "%s", name);
            break;
        }
        if (!strcmp(name, "."))
            continue;
        if (!strcmp(name, "..")) {
            path_parent(&cur, &proot);
            continue;
        }
        r = vfs_permission(cur.v, MAY_EXEC); /* going through a directory is searching it */
        if (r < 0)
            break;
        struct path next = {mount_get(cur.mnt), NULL};
        r = cur.v->ops->lookup ? cur.v->ops->lookup(cur.v, name, &next.v) : -ENOTDIR;
        if (r < 0) {
            mount_put(next.mnt);
            break;
        }
        fsnotify_lookup(cur.v, name, next.v);
        path_cross(&next);
        if (next.v->type == VN_LNK && (!is_last || (flags & LOOKUP_FOLLOW) || *p == '/')) {
            struct path target;
            r = follow_link(&cur, next.v, &target, depth);
            path_put(&next);
            if (r < 0)
                break;
            next = target;
        }
        path_put(&cur);
        cur = next;
    }
    path_put(&proot);
    if (r < 0) {
        path_put(&cur);
        return r;
    }
    *out = cur;
    return 0;
}

int vfs_lookup_path(const struct path *base, const char *path, int flags, struct path *out) {
    return walk(base, path, flags, out, NULL, 0, 0);
}

int vfs_lookup(const struct path *base, const char *path, int flags, struct vnode **out) {
    struct path p;
    int r = walk(base, path, flags, &p, NULL, 0, 0);
    if (r < 0)
        return r;
    *out = p.v;
    vnode_ref(p.v);
    path_put(&p);
    return 0;
}

int vfs_lookup_parent_path(const struct path *base, const char *path, struct path *dir, char *name,
                           size_t name_size) {
    int r = walk(base, path, LOOKUP_PARENT, dir, name, name_size, 0);
    if (r == 0 && (!strcmp(name, ".") || !strcmp(name, ".."))) {
        path_put(dir);
        return -EEXIST;
    }
    return r;
}

int vfs_lookup_parent(const struct path *base, const char *path, struct vnode **dir, char *name, size_t name_size) {
    struct path d;
    int r = vfs_lookup_parent_path(base, path, &d, name, name_size);
    if (r < 0)
        return r;
    *dir = d.v;
    vnode_ref(d.v);
    path_put(&d);
    return 0;
}

struct file *file_from_vnode(struct vnode *v, uint32_t flags) {
    struct file *f = kzalloc(sizeof *f);
    if (!f)
        return NULL;
    vnode_ref(v);
    f->vnode = v;
    f->flags = flags;
    f->refcount = 1;
    return f;
}

void file_ref(struct file *f) { __atomic_add_fetch(&f->refcount, 1, __ATOMIC_RELAXED); }

void file_put(struct file *f) {
    if (f && __atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (__atomic_load_n(&f->epoll_refs, __ATOMIC_ACQUIRE))
            epoll_file_closed(f); /* before it goes away: epoll holds no reference */
        void locks_file_released(struct file *f);
        locks_file_released(f); /* its OFD locks */
        if (f->vnode->ops->close)
            f->vnode->ops->close(f->vnode, f);
        if ((f->vnode->type == VN_REG || f->vnode->type == VN_DIR) && !(f->flags & O_PATH))
            fsnotify(f->vnode, (f->flags & O_ACCMODE) != O_RDONLY ? FSN_CLOSE_WRITE : FSN_CLOSE_NOWRITE);
        vnode_put(f->vnode);
        path_put(&f->path_base);
        mount_put(f->mnt);
        kfree(f->path);
        kfree(f);
    }
}

int vfs_open(const struct path *base, const char *path, uint32_t flags, uint32_t mode, struct file **out) {
    if (flags & 020000000) /* O_TMPFILE: not on these file systems; callers fall back to named files */
        return -EOPNOTSUPP;
    struct path at = {0};
    int r;
    bool writing = (flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC);
    bool created = false; /* made by this call: its maker uses it whatever its mode says */
    if (flags & O_CREAT) {
        struct path dir;
        char name[NAME_MAX + 1];
        r = vfs_lookup_parent_path(base, path, &dir, name, sizeof name);
        if (r == -EEXIST) /* "." or "..": an existing directory */
            r = vfs_lookup_path(base, path, LOOKUP_FOLLOW, &at);
        else if (r == 0) {
            struct vnode *v = NULL;
            r = dir.v->ops->lookup ? dir.v->ops->lookup(dir.v, name, &v) : -ENOTDIR;
            if (r == 0) {
                fsnotify_lookup(dir.v, name, v);
                at.mnt = mount_get(dir.mnt);
                at.v = v;
                path_cross(&at);
                if (flags & O_EXCL) {
                    path_put(&at);
                    r = -EEXIST;
                } else if (at.v->type == VN_LNK && !(flags & O_NOFOLLOW)) {
                    struct path t;
                    r = follow_link(&dir, at.v, &t, 0);
                    path_put(&at);
                    if (r == 0)
                        at = t;
                }
            } else if (r == -ENOENT) {
                r = path_may_write(&dir);
                if (r == 0)
                    r = vfs_may_create(dir.v);
                created = r == 0;
                if (r == 0)
                    r = dir.v->ops->create ? dir.v->ops->create(dir.v, name, mode & ~proc_current()->umask, &v)
                                           : -EROFS;
                if (r == 0) {
                    at.mnt = mount_get(dir.mnt);
                    at.v = v;
                    fsnotify_dir(dir.v, name, false, FSN_CREATE, 0);
                    fsnotify_lookup(dir.v, name, v);
                }
            }
            path_put(&dir);
        }
    } else {
        r = vfs_lookup_path(base, path, (flags & O_NOFOLLOW) ? 0 : LOOKUP_FOLLOW, &at);
    }
    if (r < 0)
        return r;

    /* what it opens for, against its permissions (a file this call just made is its maker's to use) */
    if (r == 0 && !(flags & O_PATH) && !created) {
        int mask = ((flags & O_ACCMODE) != O_WRONLY ? MAY_READ : 0) | (writing ? MAY_WRITE : 0);
        r = vfs_permission(at.v, mask);
        if (r < 0) {
            path_put(&at);
            return r;
        }
    }
    struct vnode *v = at.v;
    if ((flags & O_DIRECTORY) && v->type != VN_DIR)
        r = -ENOTDIR;
    else if (v->type == VN_DIR && (flags & O_ACCMODE) != O_RDONLY)
        r = -EISDIR;
    else if (v->type == VN_LNK && !(flags & O_PATH))
        r = -ELOOP;
    else if ((v->type == VN_SOCK || v->type == VN_ANON) && !(flags & O_PATH))
        r = -ENXIO; /* as on Linux; reachable through /proc/<pid>/fd links */
    else if (writing && !(flags & O_PATH) && (v->type == VN_REG || v->type == VN_DIR))
        r = path_may_write(&at);
    else if ((v->type == VN_CHR || v->type == VN_BLK) && !(flags & O_PATH) && (mount_flags(at.mnt) & MNT_NODEV))
        r = -EACCES;
    if (r == 0 && (flags & O_TRUNC) && v->type == VN_REG && (flags & O_ACCMODE) != O_RDONLY) {
        r = v->ops->truncate ? v->ops->truncate(v, 0) : -EINVAL;
        if (r == 0)
            fsnotify(v, FSN_MODIFY);
    }
    if (r == 0 && !(flags & O_PATH) && (v->type == VN_REG || v->type == VN_DIR))
        fsnotify(v, FSN_OPEN);
    if (r < 0) {
        path_put(&at);
        return r;
    }

    struct file *f = file_from_vnode(v, flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY | O_CLOEXEC));
    if (!f) {
        path_put(&at);
        return -ENOMEM;
    }
    f->mnt = mount_get(at.mnt);
    path_put(&at);
    /* Remembered for /proc/<pid>/fd and maps; resolved only when asked. */
    size_t plen = strlen(path) + 1;
    if ((f->path = kmalloc(plen)) != NULL) {
        memcpy(f->path, path, plen);
        if (path[0] != '/') {
            if (base) {
                f->path_base = *base;
                path_get(&f->path_base);
            } else {
                fs_context(NULL, &f->path_base);
            }
        }
    }
    if (v->ops->open && !(flags & O_PATH)) {
        r = v->ops->open(v, f);
        if (r < 0) {
            file_put(f);
            return r;
        }
    }
    *out = f;
    return 0;
}

int64_t file_pread(struct file *f, void *buf, size_t len, uint64_t off) {
    struct vnode *v = f->vnode;
    if (v->type == VN_DIR)
        return -EISDIR;
    if (!v->ops->read)
        return -EINVAL;
    return v->ops->read(v, f, buf, len, off);
}

int64_t file_read(struct file *f, void *buf, size_t len) {
    if ((f->flags & O_ACCMODE) == O_WRONLY || (f->flags & O_PATH))
        return -EBADF;
    int64_t n = file_pread(f, buf, len, f->offset);
    if (n > 0 && vnode_seekable(f->vnode))
        __atomic_add_fetch(&f->offset, (uint64_t)n, __ATOMIC_RELAXED);
    return n;
}

int64_t file_write(struct file *f, const void *buf, size_t len) {
    if ((f->flags & O_ACCMODE) == O_RDONLY || (f->flags & O_PATH))
        return -EBADF;
    struct vnode *v = f->vnode;
    if (!v->ops->write)
        return -EINVAL;
    uint64_t off = (f->flags & O_APPEND) ? v->size : f->offset;
    int64_t n = v->ops->write(v, f, buf, len, off);
    if (n > 0 && vnode_seekable(v))
        f->offset = off + (uint64_t)n;
    if (n > 0 && v->type == VN_REG)
        fsnotify(v, FSN_MODIFY);
    return n;
}

int vfs_read_all(struct vnode *v, void **buf, size_t *len) {
    if (v->type != VN_REG)
        return -EACCES;
    size_t size = v->size;
    uint8_t *b = kmalloc(size ? size : 1);
    if (!b)
        return -ENOMEM;
    size_t done = 0;
    while (done < size) {
        int64_t n = v->ops->read(v, NULL, b + done, size - done, done);
        if (n <= 0) {
            kfree(b);
            return n < 0 ? (int)n : -EIO;
        }
        done += (size_t)n;
    }
    *buf = b;
    *len = size;
    return 0;
}

int vfs_mkdir_p(const char *path, uint32_t mode) {
    char partial[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof partial)
        return -ENAMETOOLONG;
    for (size_t i = 1; i <= n; i++) {
        if (path[i] != '/' && path[i] != 0)
            continue;
        memcpy(partial, path, i);
        partial[i] = 0;
        struct vnode *dir, *existing;
        char name[NAME_MAX + 1];
        if (vfs_lookup(NULL, partial, LOOKUP_FOLLOW, &existing) == 0) {
            vnode_put(existing);
            continue;
        }
        int r = vfs_lookup_parent(NULL, partial, &dir, name, sizeof name);
        if (r < 0)
            return r;
        r = dir->ops->mkdir ? dir->ops->mkdir(dir, name, mode) : -EROFS;
        if (r == 0)
            fsnotify_dir(dir, name, true, FSN_CREATE, 0);
        vnode_put(dir);
        if (r < 0 && r != -EEXIST)
            return r;
    }
    return 0;
}

int vfs_remove_node(const char *path) {
    struct vnode *dir;
    char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(NULL, path, &dir, name, sizeof name);
    if (r < 0)
        return r;
    r = dir->ops->unlink ? dir->ops->unlink(dir, name, false) : -EPERM;
    vnode_put(dir);
    return r;
}

int vfs_create_node(const char *path, struct vnode *node) {
    struct vnode *dir;
    char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(NULL, path, &dir, name, sizeof name);
    if (r < 0)
        return r;
    r = ramfs_add_node(dir, name, node);
    vnode_put(dir);
    return r;
}
