#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>

/* inotify: a descriptor that reads what happened to the files and folders it
 * watches (made, changed, closed after writing, moved, deleted), as Linux
 * gives it. GLib's file monitors, file managers, editors and browsers notice
 * changes this way.
 *
 * A watch holds its vnode, so the same file or folder is the same vnode for
 * as long as it is watched (ramfs, and ext2's one vnode per inode in use).
 * Changes to names (create, delete, rename) are told with the folder and the
 * name. Changes to a file itself (written, closed, chmod) are told to its own
 * watches and, when it was found by name in a watched folder (a hint kept at
 * lookup), to that folder's watches with its name, as Linux does from its
 * dentry. Nothing is done while nothing is watched (fsnotify_mask: what any
 * watch wants). */

#define IN_ACCESS 0x1
#define IN_MODIFY 0x2
#define IN_ATTRIB 0x4
#define IN_CLOSE_WRITE 0x8
#define IN_CLOSE_NOWRITE 0x10
#define IN_OPEN 0x20
#define IN_MOVED_FROM 0x40
#define IN_MOVED_TO 0x80
#define IN_CREATE 0x100
#define IN_DELETE 0x200
#define IN_DELETE_SELF 0x400
#define IN_MOVE_SELF 0x800
#define IN_UNMOUNT 0x2000
#define IN_Q_OVERFLOW 0x4000
#define IN_IGNORED 0x8000
#define IN_ONLYDIR 0x01000000
#define IN_DONT_FOLLOW 0x02000000
#define IN_EXCL_UNLINK 0x04000000
#define IN_MASK_CREATE 0x10000000
#define IN_MASK_ADD 0x20000000
#define IN_ISDIR 0x40000000
#define IN_ONESHOT 0x80000000
#define IN_ALL_EVENTS 0xfff

#define NAME_MAX 255
#define MAX_QUEUED 16384 /* events waiting in one descriptor (as Linux's default) */
#define MAX_WATCHES 8192 /* in all */
#define HINTS 512

struct in_event {
    struct in_event *next;
    int32_t wd;
    uint32_t mask, cookie, len; /* len: of the name, NUL-padded to a multiple of 16 (0: no name) */
    char name[];
};

struct watch {
    struct watch *next;      /* all watches */
    struct inotify *in;
    struct vnode *v;         /* referenced */
    int32_t wd;
    uint32_t mask;
};

struct inotify {
    struct in_event *head, *tail;
    uint32_t queued;
    uint64_t bytes;
    int32_t next_wd;
    bool overflowed;
    struct waitq wq;
};

/* who was found by name in a watched folder: not referenced (the vnode and its inode number together say it is
 * still the same one); forgotten when the name goes, and the oldest give way */
struct hint {
    struct vnode *child, *dir;
    uint64_t ino;
    char name[NAME_MAX + 1];
};

static spinlock_t nlock;
static struct watch *watches;
static int nwatches;
static struct hint hints[HINTS];
static unsigned next_hint;
static uint32_t cookies;
uint32_t fsnotify_mask; /* the events some watch wants: 0, nothing to do */

static void recompute_mask(void) {
    uint32_t m = 0;
    for (struct watch *w = watches; w; w = w->next)
        m |= w->mask & IN_ALL_EVENTS;
    if (watches)
        m |= IN_DELETE_SELF | IN_IGNORED; /* a watched one deleted, or the folder it was in: always seen to */
    __atomic_store_n(&fsnotify_mask, m, __ATOMIC_RELEASE);
}

static bool same_vnode(struct vnode *a, struct vnode *b) {
    return a == b || (a && b && a->ops == b->ops && a->ops->same && a->ops->same(a, b));
}

/* Queues an event on `in` (lock held); one just like the last is merged into it, as Linux does. */
static void queue(struct inotify *in, int32_t wd, uint32_t mask, uint32_t cookie, const char *name) {
    size_t nl = name && name[0] ? strlen(name) : 0;
    uint32_t len = nl ? (uint32_t)ALIGN_UP(nl + 1, 16) : 0;
    struct in_event *last = in->tail;
    if (last && last->wd == wd && last->mask == mask && last->cookie == cookie &&
        ((!len && !last->len) || (len && last->len == len && !strcmp(last->name, name))))
        return;
    if (in->queued >= MAX_QUEUED) {
        if (in->overflowed)
            return;
        in->overflowed = true;
        wd = -1, mask = IN_Q_OVERFLOW, cookie = 0, len = 0;
    }
    struct in_event *e = kzalloc(sizeof *e + len);
    if (!e)
        return;
    e->wd = wd, e->mask = mask, e->cookie = cookie, e->len = len;
    if (len)
        memcpy(e->name, name, nl);
    if (in->tail)
        in->tail->next = e;
    else
        in->head = e;
    in->tail = e;
    in->queued++;
    in->bytes += 16 + len;
    waitq_wake_all(&in->wq);
}

/* Takes watch `w` out (lock held): IN_IGNORED is its last event; the vnode is handed back to be put unlocked. */
static struct vnode *drop_watch(struct watch *w) {
    for (struct watch **pp = &watches; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    nwatches--;
    queue(w->in, w->wd, IN_IGNORED, 0, NULL);
    for (int i = 0; i < HINTS; i++) /* what was found in it (if a folder) is no longer told to anyone */
        if (hints[i].dir == w->v)
            hints[i].child = hints[i].dir = NULL;
    struct vnode *v = w->v;
    kfree(w);
    return v;
}

static void tell(struct vnode *target, uint32_t mask, uint32_t cookie, const char *name, struct vnode **puts, int *nputs) {
    for (struct watch *w = watches, *next; w; w = next) {
        next = w->next;
        if (!same_vnode(w->v, target) || !(w->mask & mask & IN_ALL_EVENTS))
            continue;
        queue(w->in, w->wd, mask & (w->mask | IN_ISDIR | IN_IGNORED), cookie, name);
        if ((w->mask & IN_ONESHOT) && *nputs < 16)
            puts[(*nputs)++] = drop_watch(w);
    }
}

static void put_all(struct vnode **puts, int n) {
    for (int i = 0; i < n; i++)
        vnode_put(puts[i]);
}

static bool watched(struct vnode *v) {
    for (struct watch *w = watches; w; w = w->next)
        if (same_vnode(w->v, v))
            return true;
    return false;
}

void fsnotify_dir(struct vnode *dir, const char *name, bool isdir, uint32_t mask, uint32_t cookie) {
    if (!(__atomic_load_n(&fsnotify_mask, __ATOMIC_ACQUIRE) & mask) || !dir)
        return;
    struct vnode *puts[16];
    int np = 0;
    uint64_t f = spin_lock_irqsave(&nlock);
    tell(dir, mask | (isdir ? IN_ISDIR : 0), cookie, name, puts, &np);
    spin_unlock_irqrestore(&nlock, f);
    put_all(puts, np);
}

void fsnotify(struct vnode *v, uint32_t mask) {
    if (!(__atomic_load_n(&fsnotify_mask, __ATOMIC_ACQUIRE) & mask) || !v)
        return;
    struct vnode *puts[16];
    int np = 0;
    uint32_t isdir = v->type == VN_DIR ? IN_ISDIR : 0;
    uint64_t f = spin_lock_irqsave(&nlock);
    tell(v, mask | isdir, 0, NULL, puts, &np);
    for (int i = 0; i < HINTS; i++)
        if (hints[i].child == v && hints[i].ino == v->ino && hints[i].dir)
            tell(hints[i].dir, mask | isdir, 0, hints[i].name, puts, &np);
    spin_unlock_irqrestore(&nlock, f);
    put_all(puts, np);
}

void fsnotify_lookup(struct vnode *dir, const char *name, struct vnode *child) {
    if (!__atomic_load_n(&fsnotify_mask, __ATOMIC_ACQUIRE) || !dir || !child || !name[0] || !strcmp(name, ".") ||
        !strcmp(name, ".."))
        return;
    uint64_t f = spin_lock_irqsave(&nlock);
    if (watched(dir)) {
        int free_at = -1;
        for (int i = 0; i < HINTS; i++) {
            if (hints[i].child == child && hints[i].dir == dir && !strcmp(hints[i].name, name)) {
                hints[i].ino = child->ino;
                free_at = -2;
                break;
            }
            if (free_at == -1 && !hints[i].child)
                free_at = i;
        }
        if (free_at != -2) {
            int i = free_at >= 0 ? free_at : (int)(next_hint++ % HINTS);
            hints[i].child = child, hints[i].dir = dir, hints[i].ino = child->ino;
            ksnprintf(hints[i].name, sizeof hints[i].name, "%s", name);
        }
    }
    spin_unlock_irqrestore(&nlock, f);
}

void fsnotify_gone(struct vnode *dir, const char *name, struct vnode *child, bool isdir) {
    if (!__atomic_load_n(&fsnotify_mask, __ATOMIC_ACQUIRE))
        return;
    struct vnode *puts[16 + 16];
    int np = 0;
    uint64_t f = spin_lock_irqsave(&nlock);
    tell(dir, IN_DELETE | (isdir ? IN_ISDIR : 0), 0, name, puts, &np);
    for (int i = 0; i < HINTS; i++)
        if (hints[i].dir == dir && !strcmp(hints[i].name, name))
            hints[i].child = hints[i].dir = NULL;
    if (child && (isdir || child->nlink == 0)) { /* the last name gone: the file itself (its watches end) */
        tell(child, IN_DELETE_SELF, 0, NULL, puts, &np);
        for (struct watch *w = watches, *next; w; w = next) {
            next = w->next;
            if (same_vnode(w->v, child) && np < 32)
                puts[np++] = drop_watch(w);
        }
        for (int i = 0; i < HINTS; i++)
            if (hints[i].dir == child || hints[i].child == child)
                hints[i].child = hints[i].dir = NULL;
        recompute_mask();
    }
    spin_unlock_irqrestore(&nlock, f);
    put_all(puts, np);
}

void fsnotify_moved(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname, struct vnode *child) {
    if (!__atomic_load_n(&fsnotify_mask, __ATOMIC_ACQUIRE))
        return;
    struct vnode *puts[16];
    int np = 0;
    uint64_t f = spin_lock_irqsave(&nlock);
    uint32_t cookie = ++cookies ? cookies : ++cookies;
    uint32_t isdir = child && child->type == VN_DIR ? IN_ISDIR : 0;
    tell(odir, IN_MOVED_FROM | isdir, cookie, oname, puts, &np);
    tell(ndir, IN_MOVED_TO | isdir, cookie, nname, puts, &np);
    if (child)
        tell(child, IN_MOVE_SELF | isdir, 0, NULL, puts, &np);
    for (int i = 0; i < HINTS; i++) { /* found again under its new name, if that folder is watched */
        if (hints[i].dir == ndir && !strcmp(hints[i].name, nname))
            hints[i].child = hints[i].dir = NULL;
        if (hints[i].dir == odir && !strcmp(hints[i].name, oname)) {
            if (watched(ndir))
                hints[i].dir = ndir, ksnprintf(hints[i].name, sizeof hints[i].name, "%s", nname);
            else
                hints[i].child = hints[i].dir = NULL;
        }
    }
    spin_unlock_irqrestore(&nlock, f);
    put_all(puts, np);
}

/* ---- the descriptor ---- */

static bool in_readable(void *arg) { return __atomic_load_n(&((struct inotify *)arg)->queued, __ATOMIC_ACQUIRE) != 0; }

static int64_t in_read(struct vnode *v, struct file *fl, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct inotify *in = v->data;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&nlock);
        if (in->head) {
            size_t done = 0;
            while (in->head && done + 16 + in->head->len <= len) {
                struct in_event *e = in->head;
                uint8_t *p = (uint8_t *)buf + done;
                memcpy(p, &e->wd, 4);
                memcpy(p + 4, &e->mask, 4);
                memcpy(p + 8, &e->cookie, 4);
                memcpy(p + 12, &e->len, 4);
                if (e->len)
                    memcpy(p + 16, e->name, e->len);
                done += 16 + e->len;
                in->head = e->next;
                if (!in->head)
                    in->tail = NULL;
                in->queued--;
                in->bytes -= 16 + e->len;
                if (e->mask == IN_Q_OVERFLOW)
                    in->overflowed = false;
                kfree(e);
            }
            spin_unlock_irqrestore(&nlock, f);
            return done ? (int64_t)done : -EINVAL; /* the first doesn't fit the buffer */
        }
        spin_unlock_irqrestore(&nlock, f);
        if (fl && (fl->flags & O_NONBLOCK))
            return -EAGAIN;
        if (waitq_wait_cond(&in->wq, in_readable, in) < 0)
            return -EINTR;
    }
}

static int in_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct inotify *in = v->data;
    if (wq)
        *wq = &in->wq;
    return __atomic_load_n(&in->queued, __ATOMIC_ACQUIRE) ? POLLIN : 0;
}

static int64_t in_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)f;
    struct inotify *in = v->data;
    if (req != FIONREAD)
        return -ENOTTY;
    uint64_t fl = spin_lock_irqsave(&nlock);
    int n = (int)in->bytes;
    spin_unlock_irqrestore(&nlock, fl);
    return copy_to_user(arg, &n, sizeof n) < 0 ? -EFAULT : 0;
}

static void in_release(struct vnode *v) {
    struct inotify *in = v->data;
    for (;;) { /* its watches, put one at a time outside the lock */
        struct vnode *put = NULL;
        uint64_t f = spin_lock_irqsave(&nlock);
        for (struct watch *w = watches; w; w = w->next)
            if (w->in == in) {
                put = drop_watch(w);
                break;
            }
        if (!put)
            recompute_mask();
        spin_unlock_irqrestore(&nlock, f);
        if (!put)
            break;
        vnode_put(put);
    }
    while (in->head) {
        struct in_event *e = in->head;
        in->head = e->next;
        kfree(e);
    }
    kfree(in);
}

static const struct vnode_ops inotify_ops = {
    .read = in_read, .poll = in_poll, .ioctl = in_ioctl, .release = in_release,
};

static int64_t do_inotify_init(uint64_t flags) {
    if (flags & ~(uint64_t)(O_NONBLOCK | O_CLOEXEC))
        return -EINVAL;
    struct vnode *v = vnode_alloc(VN_ANON, 0600, &inotify_ops);
    struct inotify *in = kzalloc(sizeof *in);
    if (!v || !in) {
        kfree(v);
        kfree(in);
        return -ENOMEM;
    }
    in->next_wd = 1;
    waitq_init(&in->wq);
    v->data = in;
    return vnode_install_fd(v, O_RDONLY | (flags & O_NONBLOCK), flags & O_CLOEXEC);
}

SYSCALL(inotify_init) {
    UNUSED_ARGS();
    return do_inotify_init(0);
}

SYSCALL(inotify_init1) {
    UNUSED_ARGS();
    return do_inotify_init(a1);
}

static struct inotify *inotify_of(struct file *f) {
    return f && f->vnode->ops == &inotify_ops ? f->vnode->data : NULL;
}

SYSCALL(inotify_add_watch) {
    UNUSED_ARGS();
    uint32_t mask = (uint32_t)a3;
    if (!(mask & IN_ALL_EVENTS) || ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)))
        return -EINVAL;
    struct file *fl = fd_get((int)a1);
    struct inotify *in = inotify_of(fl);
    if (!in) {
        if (fl)
            file_put(fl);
        return fl ? -EINVAL : -EBADF;
    }
    char *path;
    int64_t r = user_path(a2, &path);
    struct path p = {0};
    if (r >= 0) {
        struct path base;
        r = at_dir(AT_FDCWD, path, &base);
        if (r == 0) {
            r = vfs_lookup_path(&base, path, (mask & IN_DONT_FOLLOW) ? 0 : LOOKUP_FOLLOW, &p);
            path_put(&base);
        }
        kfree(path);
    }
    if (r == 0 && (mask & IN_ONLYDIR) && p.v->type != VN_DIR)
        r = -ENOTDIR;
    if (r == 0)
        r = vfs_permission(p.v, MAY_READ); /* watching is reading */
    if (r < 0) {
        if (p.v)
            path_put(&p);
        file_put(fl);
        return r;
    }
    struct vnode *v = p.v;
    vnode_ref(v);
    path_put(&p);
    struct watch *nw = kzalloc(sizeof *nw);
    uint64_t f = spin_lock_irqsave(&nlock);
    struct watch *w = NULL;
    for (struct watch *x = watches; x; x = x->next)
        if (x->in == in && same_vnode(x->v, v))
            w = x;
    bool put_v = true;
    if (w && (mask & IN_MASK_CREATE)) {
        r = -EEXIST;
    } else if (w) { /* the same one again: its mask changed (or added to) */
        w->mask = (mask & IN_MASK_ADD) ? (w->mask | (mask & ~IN_MASK_ADD)) : mask;
        r = w->wd;
    } else if (!nw || nwatches >= MAX_WATCHES) {
        r = -ENOSPC;
    } else {
        nw->in = in, nw->v = v, nw->wd = in->next_wd++, nw->mask = mask & ~(IN_MASK_ADD | IN_MASK_CREATE);
        nw->next = watches;
        watches = nw;
        nwatches++;
        r = nw->wd;
        nw = NULL;
        put_v = false;
    }
    recompute_mask();
    spin_unlock_irqrestore(&nlock, f);
    kfree(nw);
    if (put_v)
        vnode_put(v);
    file_put(fl);
    return r;
}

SYSCALL(inotify_rm_watch) {
    UNUSED_ARGS();
    struct file *fl = fd_get((int)a1);
    struct inotify *in = inotify_of(fl);
    if (!in) {
        if (fl)
            file_put(fl);
        return fl ? -EINVAL : -EBADF;
    }
    struct vnode *put = NULL;
    uint64_t f = spin_lock_irqsave(&nlock);
    for (struct watch *w = watches; w; w = w->next)
        if (w->in == in && w->wd == (int32_t)a2) {
            put = drop_watch(w);
            break;
        }
    recompute_mask();
    spin_unlock_irqrestore(&nlock, f);
    file_put(fl);
    if (!put)
        return -EINVAL;
    vnode_put(put);
    return 0;
}
