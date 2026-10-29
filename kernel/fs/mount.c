#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/blkdev.h>
#include <kernel/vfs.h>
#include <linux/abi.h>
#include <linux/syscall.h>

#include "synth.h"

/* Mounts and mount namespaces.
 *
 * A mount attaches the tree under `root` (a file system's root, or any
 * directory or file of one, for bind mounts) at `mountpoint`, a vnode
 * inside its parent mount. Each namespace has its own set of mounts, so a
 * sandbox (bubblewrap, Flatpak, Chromium) can build a private view of the
 * file system: a tmpfs root with a few directories bound in, then
 * pivot_root into it.
 *
 * One spinlock covers every mount table. Paths hold references on the
 * mounts they go through; a child mount holds one on its parent, so
 * walking up never meets a freed mount. Mount propagation (shared, slave)
 * is accepted and ignored: every mount is private. */

#define NAME_MAX 255

struct mount {
    int id, refcount;
    struct vnode *root;       /* referenced */
    struct mount *parent;     /* referenced; NULL for a namespace's root, or once detached */
    struct vnode *mountpoint; /* referenced; in `parent` */
    uint32_t flags;           /* MNT_* */
    char fstype[16];
    char source[64];
    char fsroot[128];          /* where `root` is inside its file system, for mountinfo */
    int dev;                   /* minor device number shown for it */
    struct mntns *ns;          /* NULL once detached */
    struct mount *next;        /* in ns->mounts, oldest first */
    struct mount *copy;        /* scratch while a namespace is copied */
    struct vnode *mp_dir;      /* a mount on a file: the directory it is in (referenced) */
    char mp_name[256];         /* ...and its name there (files have no parent link) */
};

struct mntns {
    int refcount;
    uint64_t id;
    struct mount *root;
    struct mount *mounts;
};

static spinlock_t mnt_lock;
static int next_mount_id = 1, next_dev = 1;
uint64_t ns_next_id(void); /* proc/ns.c: namespace ids share one sequence */
static struct mntns *first_ns;

struct mount *mount_get(struct mount *m) {
    if (m)
        __atomic_add_fetch(&m->refcount, 1, __ATOMIC_RELAXED);
    return m;
}

void mount_put(struct mount *m) {
    while (m && __atomic_sub_fetch(&m->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        struct mount *parent = m->parent;
        vnode_put(m->root);
        if (m->mountpoint)
            vnode_put(m->mountpoint);
        if (m->mp_dir)
            vnode_put(m->mp_dir);
        kfree(m);
        m = parent; /* the child's reference on its parent */
    }
}

uint32_t mount_flags(const struct mount *m) { return m ? m->flags : 0; }
int mount_id(const struct mount *m) { return m ? m->id : 0; }
int mount_dev(const struct mount *m) { return m ? m->dev : 0; }

void path_get(struct path *p) {
    mount_get(p->mnt);
    if (p->v)
        vnode_ref(p->v);
}

void path_put(struct path *p) {
    if (p->v)
        vnode_put(p->v);
    mount_put(p->mnt);
    p->v = NULL;
    p->mnt = NULL;
}

static struct mount *mount_alloc(struct vnode *root, const char *fstype, const char *source, uint32_t flags) {
    struct mount *m = kzalloc(sizeof *m);
    if (!m)
        return NULL;
    m->refcount = 1;
    m->root = root;
    vnode_ref(root);
    m->flags = flags;
    ksnprintf(m->fstype, sizeof m->fstype, "%s", fstype);
    ksnprintf(m->source, sizeof m->source, "%s", source ? source : fstype);
    ksnprintf(m->fsroot, sizeof m->fsroot, "/");
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    m->id = next_mount_id++;
    m->dev = next_dev++;
    spin_unlock_irqrestore(&mnt_lock, f);
    return m;
}

/* mnt_lock held. Adds `m` (its reference goes to the namespace) under
 * `parent` at `mp`. */
static void attach_locked(struct mntns *ns, struct mount *m, struct mount *parent, struct vnode *mp) {
    m->ns = ns;
    m->parent = mount_get(parent);
    m->mountpoint = mp;
    if (mp)
        vnode_ref(mp);
    m->next = NULL;
    struct mount **pp = &ns->mounts;
    while (*pp)
        pp = &(*pp)->next;
    *pp = m;
}

/* mnt_lock held. Takes `m` out of its namespace's list (the list's
 * reference is returned to the caller). */
static void unlist_locked(struct mount *m) {
    if (!m->ns)
        return;
    for (struct mount **pp = &m->ns->mounts; *pp; pp = &(*pp)->next)
        if (*pp == m) {
            *pp = m->next;
            break;
        }
    m->ns = NULL;
    m->next = NULL;
}

void mount_init(struct vnode *rootfs) {
    struct mntns *ns = kzalloc(sizeof *ns);
    KASSERT(ns);
    ns->refcount = 1;
    ns->id = ns_next_id();
    struct mount *m = mount_alloc(rootfs, "rootfs", "rootfs", 0);
    KASSERT(m);
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    attach_locked(ns, m, NULL, NULL);
    ns->root = m;
    spin_unlock_irqrestore(&mnt_lock, f);
    first_ns = ns;
}

struct mntns *mntns_get(struct mntns *ns) {
    if (ns)
        __atomic_add_fetch(&ns->refcount, 1, __ATOMIC_RELAXED);
    return ns;
}

void mntns_put(struct mntns *ns) {
    if (!ns || __atomic_sub_fetch(&ns->refcount, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    /* Nobody lives in it any more: let go of every mount. */
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    struct mount *list = ns->mounts;
    ns->mounts = NULL;
    for (struct mount *m = list; m; m = m->next)
        m->ns = NULL;
    spin_unlock_irqrestore(&mnt_lock, f);
    while (list) {
        struct mount *next = list->next;
        mount_put(list);
        list = next;
    }
    kfree(ns);
}

uint64_t mntns_id(const struct mntns *ns) { return ns ? ns->id : 0; }

/* ---- the calling process's view ---- */

void fs_context(struct path *root, struct path *cwd) {
    struct process *p = proc_current();
    if (!p || !p->root.v) { /* early boot, before any process has a root: the first namespace's */
        struct path top = {first_ns->root, first_ns->root->root};
        if (root) {
            *root = top;
            path_get(root);
        }
        if (cwd) {
            *cwd = top;
            path_get(cwd);
        }
        return;
    }
    uint64_t lf = proc_table_lock();
    if (root) {
        *root = p->root;
        path_get(root);
    }
    if (cwd) {
        *cwd = p->cwd;
        path_get(cwd);
    }
    proc_table_unlock(lf);
}

void mntns_root(struct mntns *ns, struct path *out);
void mntns_root(struct mntns *ns, struct path *out) {
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    out->mnt = mount_get(ns->root);
    out->v = ns->root->root;
    vnode_ref(out->v);
    spin_unlock_irqrestore(&mnt_lock, f);
}

struct mntns *proc_first_mntns(void) { return mntns_get(first_ns); }

static struct mntns *current_ns(void) {
    struct process *p = proc_current();
    return p && p->mntns ? p->mntns : first_ns;
}

/* ---- walking ---- */

/* mnt_lock held: the newest mount at (parent, mp), or NULL. */
static struct mount *mounted_at_locked(struct mount *parent, struct vnode *mp) {
    struct mntns *ns = parent ? parent->ns : NULL;
    struct mount *top = NULL;
    if (!ns)
        return NULL;
    for (struct mount *m = ns->mounts; m; m = m->next)
        if (m->parent == parent && (m->mountpoint == mp || (mp && m->mountpoint && mp->ops == m->mountpoint->ops &&
                                                             mp->ops->same && mp->ops->same(mp, m->mountpoint))))
            top = m;
    return top;
}

void path_cross(struct path *p) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&mnt_lock);
        struct mount *m = mounted_at_locked(p->mnt, p->v);
        if (m) {
            mount_get(m);
            vnode_ref(m->root);
        }
        spin_unlock_irqrestore(&mnt_lock, f);
        if (!m)
            return;
        path_put(p);
        p->mnt = m;
        p->v = m->root;
    }
}

/* mnt_lock not held. If `p` is the root of its mount, moves it to the
 * mount point it covers. Returns false at a namespace root. */
static bool step_out_of_mount(struct path *p) {
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    struct mount *m = p->mnt;
    if (!m || p->v != m->root || !m->parent) {
        spin_unlock_irqrestore(&mnt_lock, f);
        return false;
    }
    struct path up = {mount_get(m->parent), m->mountpoint};
    vnode_ref(up.v);
    spin_unlock_irqrestore(&mnt_lock, f);
    path_put(p);
    *p = up;
    return true;
}

void path_parent(struct path *p, const struct path *root) {
    for (;;) {
        if (root && path_equal(p, root))
            return;
        if (p->mnt && p->v == p->mnt->root) {
            if (!step_out_of_mount(p))
                return; /* the top of a namespace (or of a detached tree) */
            continue;
        }
        break;
    }
    struct vnode *up = p->v->parent ? p->v->parent : p->v;
    vnode_ref(up);
    vnode_put(p->v);
    p->v = up;
    path_cross(p);
}

/* The name `v` has in directory `dir` (by inode number). */
static int name_in(struct vnode *dir, struct vnode *v, char *name, size_t size) {
    if (!dir->ops->readdir)
        return -ENOENT;
    uint8_t type;
    uint64_t ino;
    for (uint64_t i = 2; dir->ops->readdir(dir, i, name, size, &type, &ino) > 0; i++)
        if (ino == v->ino)
            return 0;
    return -ENOENT;
}

static int name_from(const struct path *p, const struct path *root, char *buf, size_t size, bool *reached) {
    char *tmp = kmalloc(4096);
    if (!tmp)
        return -ENOMEM;
    size_t pos = 4095;
    tmp[pos] = 0;
    struct path cur = *p;
    path_get(&cur);
    int r = 0, guard = 0;
    if (reached)
        *reached = false;
    while (guard++ < 512) {
        if (root && path_equal(&cur, root)) {
            if (reached)
                *reached = true;
            break;
        }
        if (cur.mnt && cur.v == cur.mnt->root) {
            struct mount *m = cur.mnt;
            if (m->mp_dir && m->parent) { /* a file mounted on a file: named from its directory */
                size_t n = strlen(m->mp_name);
                if (n + 1 > pos) {
                    r = -ENAMETOOLONG;
                    break;
                }
                pos -= n;
                memcpy(tmp + pos, m->mp_name, n);
                tmp[--pos] = '/';
                struct path up = {mount_get(m->parent), m->mp_dir};
                vnode_ref(up.v);
                path_put(&cur);
                cur = up;
                continue;
            }
            if (!step_out_of_mount(&cur))
                break; /* outside the caller's root: what can be named is named */
            continue;
        }
        struct vnode *parent = cur.v->parent;
        if (!parent || parent == cur.v)
            break;
        char name[NAME_MAX + 1];
        if ((r = name_in(parent, cur.v, name, sizeof name)) < 0)
            break;
        size_t n = strlen(name);
        if (n + 1 > pos) {
            r = -ENAMETOOLONG;
            break;
        }
        pos -= n;
        memcpy(tmp + pos, name, n);
        tmp[--pos] = '/';
        vnode_ref(parent);
        vnode_put(cur.v);
        cur.v = parent;
    }
    path_put(&cur);
    if (r == 0) {
        if (pos == 4095)
            tmp[--pos] = '/';
        size_t len = 4095 - pos;
        if (len + 1 > size)
            r = -ERANGE;
        else {
            memcpy(buf, tmp + pos, len + 1);
            r = (int)len;
        }
    }
    kfree(tmp);
    return r;
}

int path_name(const struct path *p, const struct path *root, char *buf, size_t size) {
    return name_from(p, root, buf, size, NULL);
}

int path_name_here(const struct path *p, char *buf, size_t size) {
    struct path root;
    fs_context(&root, NULL);
    int r = path_name(p, &root, buf, size);
    path_put(&root);
    return r;
}

void path_for_vnode(struct vnode *v, struct path *out) {
    struct mntns *ns = current_ns();
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    struct mount *found = NULL;
    for (struct mount *m = ns->mounts; m; m = m->next)
        if (m->root == v)
            found = m;
    out->mnt = mount_get(found ? found : ns->root);
    spin_unlock_irqrestore(&mnt_lock, f);
    out->v = v;
    vnode_ref(v);
}

int path_may_write(const struct path *p) { return p->mnt && (p->mnt->flags & MNT_RDONLY) ? -EROFS : 0; }

/* ---- boot-time mounts ---- */

int vfs_mount(const char *path, struct vnode *fsroot, const char *fstype, uint32_t flags) {
    int r = vfs_mkdir_p(path, 0555);
    struct path at;
    if (r == 0)
        r = vfs_lookup_path(NULL, path, LOOKUP_FOLLOW, &at);
    if (r < 0)
        return r;
    struct mount *m = mount_alloc(fsroot, fstype, fstype, flags);
    if (!m) {
        path_put(&at);
        return -ENOMEM;
    }
    if (!fsroot->parent && at.v->parent) { /* for code that still walks vnodes (held, as parents are) */
        fsroot->parent = at.v->parent;
        vnode_ref(fsroot->parent);
    }
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    attach_locked(at.mnt->ns ? at.mnt->ns : first_ns, m, at.mnt, at.v);
    spin_unlock_irqrestore(&mnt_lock, f);
    path_put(&at);
    return 0;
}

/* ---- copying a namespace ---- */

struct mntns *mntns_copy(struct mntns *old, struct path *root, struct path *cwd) {
    struct mntns *ns = kzalloc(sizeof *ns);
    if (!ns)
        return NULL;
    ns->refcount = 1;
    /* Copies first (they allocate), wiring under the lock afterwards. */
    int n = 0;
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    ns->id = ns_next_id();
    for (struct mount *m = old->mounts; m; m = m->next)
        n++;
    spin_unlock_irqrestore(&mnt_lock, f);
    struct mount **copies = kzalloc(sizeof *copies * (size_t)(n + 1));
    if (!copies) {
        kfree(ns);
        return NULL;
    }
    for (int i = 0; i < n; i++)
        if (!(copies[i] = kzalloc(sizeof **copies)))
            goto fail;
    f = spin_lock_irqsave(&mnt_lock);
    int i = 0;
    for (struct mount *m = old->mounts; m && i < n; m = m->next, i++) {
        struct mount *c = copies[i];
        *c = *m;
        c->refcount = 1;
        c->id = next_mount_id++;
        c->ns = ns;
        c->next = NULL;
        c->copy = NULL;
        vnode_ref(c->root);
        if (c->mountpoint)
            vnode_ref(c->mountpoint);
        if (c->mp_dir)
            vnode_ref(c->mp_dir);
        m->copy = c;
    }
    int made = i;
    for (struct mount *m = old->mounts; m; m = m->next) {
        struct mount *c = m->copy;
        if (!c)
            continue;
        c->parent = m->parent && m->parent->copy ? mount_get(m->parent->copy) : NULL;
        struct mount **pp = &ns->mounts;
        while (*pp)
            pp = &(*pp)->next;
        *pp = c;
        if (m == old->root)
            ns->root = c;
    }
    /* the caller's places move over to the copies */
    struct path *places[2] = {root, cwd};
    for (int k = 0; k < 2; k++) {
        struct path *pl = places[k];
        if (pl && pl->mnt && pl->mnt->copy) {
            struct mount *c = mount_get(pl->mnt->copy);
            struct mount *was = pl->mnt;
            pl->mnt = c;
            mount_put(was); /* not the last reference: the old namespace holds it */
        }
    }
    for (struct mount *m = old->mounts; m; m = m->next)
        m->copy = NULL;
    spin_unlock_irqrestore(&mnt_lock, f);
    for (int k = made; k < n; k++)
        kfree(copies[k]);
    kfree(copies);
    return ns;
fail:
    for (int k = 0; k < n; k++)
        kfree(copies[k]);
    kfree(copies);
    kfree(ns);
    return NULL;
}

/* ---- /proc/<pid>/mounts and mountinfo ---- */

static void put_escaped(struct sbuf *b, const char *s) {
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\\')
            sb_printf(b, "\\%03o", (unsigned char)*s);
        else
            sb_put(b, s, 1);
    }
}

void mounts_show(struct mntns *ns, const struct path *root, struct sbuf *b, bool info) {
    if (!ns)
        ns = first_ns;
    /* Snapshot (referenced) so names can be worked out without the lock. */
    struct mount *list[256];
    int n = 0;
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    for (struct mount *m = ns->mounts; m && n < 256; m = m->next)
        list[n++] = mount_get(m);
    spin_unlock_irqrestore(&mnt_lock, f);
    char *where = kmalloc(4096);
    for (int i = 0; i < n && where; i++) {
        struct mount *m = list[i];
        struct path top = {m, m->root};
        /* Only what the reader can reach from its root is shown. */
        bool reached = false;
        int r = name_from(&top, root, where, 4096, &reached);
        bool reachable = r >= 0 && (reached || !root);
        if (!reachable)
            continue;
        const char *ro = m->flags & MNT_RDONLY ? "ro" : "rw";
        char opts[64];
        ksnprintf(opts, sizeof opts, "%s%s%s%s,relatime", ro, m->flags & MNT_NOSUID ? ",nosuid" : "",
                  m->flags & MNT_NODEV ? ",nodev" : "", m->flags & MNT_NOEXEC ? ",noexec" : "");
        if (info) {
            int parent_id = m->parent ? m->parent->id : m->id;
            sb_printf(b, "%d %d 0:%d ", m->id, parent_id, m->dev);
            put_escaped(b, m->fsroot);
            sb_put(b, " ", 1);
            put_escaped(b, where);
            sb_printf(b, " %s - %s ", opts, m->fstype);
            put_escaped(b, m->source);
            sb_printf(b, " %s\n", ro);
        } else {
            put_escaped(b, m->source);
            sb_put(b, " ", 1);
            put_escaped(b, where);
            sb_printf(b, " %s %s 0 0\n", m->fstype, opts);
        }
    }
    kfree(where);
    for (int i = 0; i < n; i++)
        mount_put(list[i]);
}

/* ---- mount(2), umount2(2), pivot_root(2), chroot(2) ---- */

#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_REMOUNT     32
#define MS_BIND        4096
#define MS_MOVE        8192
#define MS_REC         16384
#define MS_UNBINDABLE  (1 << 17)
#define MS_PRIVATE     (1 << 18)
#define MS_SLAVE       (1 << 19)
#define MS_SHARED      (1 << 20)
#define MNT_DETACH_FLAG 2

static uint32_t mnt_flags_from(uint64_t ms) {
    return (ms & MS_RDONLY ? MNT_RDONLY : 0) | (ms & MS_NOSUID ? MNT_NOSUID : 0) | (ms & MS_NODEV ? MNT_NODEV : 0) |
           (ms & MS_NOEXEC ? MNT_NOEXEC : 0);
}

struct vnode *procfs_new_root(void);
struct vnode *sysfs_root(void);

/* A new instance of a file system type, or -ENODEV. */
int ext2_mount(struct blkdev *dev, struct vnode **out);

/* ext2 (and ext3/ext4 names, for the disks this driver can read): `source` names the block device. */
static int disk_instance(const char *source, struct vnode **out) {
    struct vnode *dv;
    int r = vfs_lookup(NULL, source, LOOKUP_FOLLOW, &dv);
    if (r < 0)
        return r;
    if (dv->type != VN_BLK) {
        vnode_put(dv);
        return -ENOTBLK;
    }
    r = ext2_mount(dv->data, out);
    vnode_put(dv);
    return r;
}

static int fs_instance(const char *type, const char *source, const char *data, struct vnode **out) {
    if (!strcmp(type, "ext2") || !strcmp(type, "ext3") || !strcmp(type, "ext4"))
        return disk_instance(source, out);
    struct vnode *v = NULL;
    if (!strcmp(type, "tmpfs") || !strcmp(type, "ramfs") || !strcmp(type, "mqueue") ||
        !strcmp(type, "cgroup2") || !strcmp(type, "cgroup") || !strcmp(type, "bpf") || !strcmp(type, "securityfs") ||
        !strcmp(type, "debugfs") || !strcmp(type, "tracefs") || !strcmp(type, "fusectl")) {
        v = ramfs_create_root();
        if (v && data) { /* mode=0755 and friends; size= is not enforced */
            const char *m = data;
            while (*m && strncmp(m, "mode=", 5))
                m++;
            if (*m) {
                uint32_t mode = 0;
                for (m += 5; *m >= '0' && *m <= '7'; m++)
                    mode = mode * 8 + (uint32_t)(*m - '0');
                v->mode = mode & 07777;
            }
        }
    } else if (!strcmp(type, "proc")) {
        v = procfs_new_root();
    } else if (!strcmp(type, "sysfs")) {
        v = sysfs_root();
    } else if (!strcmp(type, "devpts")) {
        /* every instance is the one set of terminals (newinstance is accepted) */
        struct path top = {first_ns->root, first_ns->root->root}, pts;
        if (vfs_lookup_path(&top, "dev/pts", LOOKUP_FOLLOW, &pts) == 0) {
            v = pts.v;
            vnode_ref(v);
            path_put(&pts);
        }
    } else {
        return -ENODEV;
    }
    if (!v)
        return -ENOMEM;
    if (!v->parent)
        v->parent = v;
    *out = v;
    return 0;
}

/* Is `p` at or below `top` (both in the same namespace)? */
static bool path_under(const struct path *p, const struct path *top) {
    struct path cur = *p;
    path_get(&cur);
    bool under = false;
    for (int guard = 0; guard < 512; guard++) {
        if (path_equal(&cur, top)) {
            under = true;
            break;
        }
        struct path prev = cur;
        path_get(&prev);
        path_parent(&cur, NULL);
        bool stuck = path_equal(&prev, &cur);
        path_put(&prev);
        if (stuck)
            break;
    }
    path_put(&cur);
    return under;
}

/* Binds `src` (a whole mount or part of one) at `dst`. With `rec`, the
 * mounts below `src` come along to the same places under the new one. */
static int bind_mount(const struct path *src, const struct path *dst, bool rec, uint32_t flags) {
    struct mntns *ns = dst->mnt->ns;
    if (!ns)
        return -EINVAL;
    struct mount *m = mount_alloc(src->v, src->mnt ? src->mnt->fstype : "none",
                                  src->mnt ? src->mnt->source : "none", flags);
    if (!m)
        return -ENOMEM;
    /* the bound directory's place inside its own file system */
    if (src->mnt) {
        struct path fsroot = {src->mnt, src->mnt->root};
        char *rel = kmalloc(4096);
        if (rel && path_name(src, &fsroot, rel, 4096) >= 0) {
            if (strcmp(src->mnt->fsroot, "/"))
                ksnprintf(m->fsroot, sizeof m->fsroot, "%s%s", src->mnt->fsroot, strcmp(rel, "/") ? rel : "");
            else
                ksnprintf(m->fsroot, sizeof m->fsroot, "%s", rel);
        }
        kfree(rel);
        m->dev = src->mnt->dev; /* the same file system */
    }
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    attach_locked(ns, m, dst->mnt, dst->v);
    spin_unlock_irqrestore(&mnt_lock, f);
    if (!rec || !src->mnt || !src->mnt->ns)
        return 0;
    /* Submounts: each child of a mount under `src` is bound again at the
     * same vnode under the copy. Mounts made by this loop are new ones of
     * `ns` and are skipped by `seen`. */
    struct mount *orig[128], *copy_of[128];
    int n = 0;
    orig[n] = src->mnt;
    copy_of[n++] = m;
    for (int i = 0; i < n; i++) {
        struct mount *kids[64];
        int nk = 0;
        f = spin_lock_irqsave(&mnt_lock);
        for (struct mount *k = src->mnt->ns->mounts; k && nk < 64; k = k->next)
            if (k->parent == orig[i] && k != m)
                kids[nk++] = mount_get(k);
        spin_unlock_irqrestore(&mnt_lock, f);
        for (int j = 0; j < nk; j++) {
            struct mount *k = kids[j];
            struct path kp = {k->parent, k->mountpoint};
            bool inside = i > 0 || path_under(&kp, src);
            if (inside && n < 128) {
                struct mount *c = mount_alloc(k->root, k->fstype, k->source, k->flags);
                if (c) {
                    memcpy(c->fsroot, k->fsroot, sizeof c->fsroot);
                    c->dev = k->dev;
                    f = spin_lock_irqsave(&mnt_lock);
                    attach_locked(ns, c, copy_of[i], k->mountpoint);
                    spin_unlock_irqrestore(&mnt_lock, f);
                    orig[n] = k;
                    copy_of[n++] = c;
                }
            }
            mount_put(k);
        }
    }
    return 0;
}

/* Detaches `m` and every mount below it from their namespace. */
static void detach_tree(struct mount *m) {
    struct mount *gone[256];
    int n = 0;
    uint64_t f = spin_lock_irqsave(&mnt_lock);
    struct mntns *ns = m->ns;
    if (ns) {
        gone[n++] = m;
        for (int i = 0; i < n; i++)
            for (struct mount *k = ns->mounts; k && n < 256; k = k->next)
                if (k->parent == gone[i]) {
                    bool already = false;
                    for (int j = 0; j < n; j++)
                        already |= gone[j] == k;
                    if (!already)
                        gone[n++] = k;
                }
        for (int i = 0; i < n; i++)
            unlist_locked(gone[i]);
    }
    spin_unlock_irqrestore(&mnt_lock, f);
    for (int i = 0; i < n; i++)
        mount_put(gone[i]); /* the namespace's reference */
}

static int user_lookup(uint64_t upath, int flags, struct path *out) {
    char *path;
    int64_t r = user_path(upath, &path);
    if (r < 0)
        return (int)r;
    r = vfs_lookup_path(NULL, path, flags, out);
    kfree(path);
    return (int)r;
}

/* A mount on a file (bubblewrap binds /dev/null, resolv.conf...): remember
 * the directory and name it is mounted under, for naming it later. */
static void note_file_mount(uint64_t upath, const struct path *target) {
    if (target->v->type == VN_DIR)
        return;
    char *path;
    if (user_path(upath, &path) < 0)
        return;
    struct path dir;
    char name[256];
    if (vfs_lookup_parent_path(NULL, path, &dir, name, sizeof name) == 0) {
        uint64_t f = spin_lock_irqsave(&mnt_lock);
        struct mount *m = mounted_at_locked(target->mnt, target->v);
        if (m && !m->mp_dir) {
            m->mp_dir = dir.v;
            vnode_ref(dir.v);
            ksnprintf(m->mp_name, sizeof m->mp_name, "%s", name);
        }
        spin_unlock_irqrestore(&mnt_lock, f);
        path_put(&dir);
    }
    kfree(path);
}

SYSCALL(mount) {
    UNUSED_ARGS();
    uint64_t ms = a4;
    if ((ms & 0xffff0000) == 0xC0ED0000) /* MS_MGC_VAL: an old magic number, not flags */
        ms &= 0xffff;
    char type[32] = "", source[64] = "", data[256] = "";
    if (a3 && strncpy_from_user(type, a3, sizeof type) < 0)
        return -EFAULT;
    type[sizeof type - 1] = 0;
    if (a1 && strncpy_from_user(source, a1, sizeof source) < 0)
        source[0] = 0;
    source[sizeof source - 1] = 0;
    if (a5 && strncpy_from_user(data, a5, sizeof data) < 0)
        data[0] = 0;
    data[sizeof data - 1] = 0;

    struct path target;
    int r = user_lookup(a2, LOOKUP_FOLLOW, &target);
    if (r < 0)
        return r;

    if (ms & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE)) {
        /* propagation: every mount here is private already */
        path_put(&target);
        return 0;
    }
    if (ms & MS_REMOUNT) {
        /* new flags for the mount whose root this is */
        if (target.v != target.mnt->root) {
            path_put(&target);
            return -EINVAL;
        }
        uint64_t f = spin_lock_irqsave(&mnt_lock);
        target.mnt->flags = mnt_flags_from(ms);
        spin_unlock_irqrestore(&mnt_lock, f);
        path_put(&target);
        return 0;
    }
    if (ms & MS_BIND) {
        struct path src;
        r = user_lookup(a1, LOOKUP_FOLLOW, &src);
        if (r == 0) {
            if ((src.v->type == VN_DIR) != (target.v->type == VN_DIR))
                r = src.v->type == VN_DIR ? -ENOTDIR : -EISDIR;
            else
                r = bind_mount(&src, &target, ms & MS_REC, mnt_flags_from(ms) & ~MNT_RDONLY);
            if (r == 0)
                note_file_mount(a2, &target);
            path_put(&src);
        }
        path_put(&target);
        return r;
    }
    if (ms & MS_MOVE) {
        struct path src;
        r = user_lookup(a1, LOOKUP_FOLLOW, &src);
        if (r == 0) {
            if (src.v != src.mnt->root || !src.mnt->parent)
                r = -EINVAL;
            else if (path_under(&target, &src))
                r = -ELOOP;
            else {
                uint64_t f = spin_lock_irqsave(&mnt_lock);
                struct mount *m = src.mnt, *oldp = m->parent;
                struct vnode *oldmp = m->mountpoint;
                m->parent = mount_get(target.mnt);
                m->mountpoint = target.v;
                vnode_ref(target.v);
                /* to the end of the list: it is the newest at its new place */
                struct mntns *ns = m->ns;
                unlist_locked(m);
                if (ns) {
                    m->ns = ns;
                    struct mount **pp = &ns->mounts;
                    while (*pp)
                        pp = &(*pp)->next;
                    *pp = m;
                }
                spin_unlock_irqrestore(&mnt_lock, f);
                vnode_put(oldmp);
                mount_put(oldp);
            }
            path_put(&src);
        }
        path_put(&target);
        return r;
    }
    struct vnode *fsroot;
    r = fs_instance(type, source, data, &fsroot);
    if (r == 0) {
        struct mount *m = mount_alloc(fsroot, type, source[0] ? source : type, mnt_flags_from(ms));
        vnode_put(fsroot);
        if (!m)
            r = -ENOMEM;
        else if (!target.mnt->ns) {
            mount_put(m);
            r = -EINVAL;
        } else {
            uint64_t f = spin_lock_irqsave(&mnt_lock);
            attach_locked(target.mnt->ns, m, target.mnt, target.v);
            spin_unlock_irqrestore(&mnt_lock, f);
        }
    }
    path_put(&target);
    return r;
}

SYSCALL(umount2) {
    UNUSED_ARGS();
    struct path target;
    int r = user_lookup(a1, (a2 & 8) ? 0 : LOOKUP_FOLLOW, &target); /* UMOUNT_NOFOLLOW */
    if (r < 0)
        return r;
    if (target.v != target.mnt->root || !target.mnt->parent)
        r = -EINVAL; /* not a mount point (or the namespace's root) */
    else
        detach_tree(target.mnt);
    path_put(&target);
    return r;
}


SYSCALL(pivot_root) {
    UNUSED_ARGS();
    struct path new_root, put_old, root;
    int r = user_lookup(a1, LOOKUP_FOLLOW, &new_root);
    if (r < 0)
        return r;
    if ((r = user_lookup(a2, LOOKUP_FOLLOW, &put_old)) < 0) {
        path_put(&new_root);
        return r;
    }
    fs_context(&root, NULL);
    struct mntns *ns = proc_current()->mntns;
    struct mount *nm = new_root.mnt, *om = root.mnt;
    if (new_root.v->type != VN_DIR || put_old.v->type != VN_DIR)
        r = -ENOTDIR;
    else if (new_root.v != nm->root || root.v != om->root || nm == om || !nm->parent || nm->ns != ns ||
             om->ns != ns)
        r = -EINVAL;
    else if (!path_under(&put_old, &new_root))
        r = -EINVAL;
    if (r == 0) {
        uint64_t f = spin_lock_irqsave(&mnt_lock);
        /* new_root leaves its parent and becomes the namespace's root */
        struct mount *np = nm->parent;
        struct vnode *nmp = nm->mountpoint;
        nm->parent = NULL;
        nm->mountpoint = NULL;
        /* the old root goes under it, at put_old */
        struct mount *op = om->parent;
        struct vnode *omp = om->mountpoint;
        om->parent = mount_get(put_old.mnt == om ? nm : put_old.mnt);
        om->mountpoint = put_old.v;
        vnode_ref(put_old.v);
        if (ns->root == om)
            ns->root = nm;
        /* order: the old root is now mounted after (on top of) new_root */
        unlist_locked(om);
        om->ns = ns;
        struct mount **pp = &ns->mounts;
        while (*pp)
            pp = &(*pp)->next;
        *pp = om;
        spin_unlock_irqrestore(&mnt_lock, f);
        mount_put(np);
        if (nmp)
            vnode_put(nmp);
        mount_put(op);
        if (omp)
            vnode_put(omp);
        struct path nr = {nm, nm->root};
        proc_move_roots(ns, &root, &nr);
    }
    path_put(&root);
    path_put(&new_root);
    path_put(&put_old);
    return r;
}

SYSCALL(chroot) {
    UNUSED_ARGS();
    struct path to;
    int r = user_lookup(a1, LOOKUP_FOLLOW, &to);
    if (r < 0)
        return r;
    if (to.v->type != VN_DIR) {
        path_put(&to);
        return -ENOTDIR;
    }
    struct process *p = proc_current();
    uint64_t lf = proc_table_lock();
    struct path old = p->root;
    p->root = to;
    proc_table_unlock(lf);
    path_put(&old);
    return 0;
}
