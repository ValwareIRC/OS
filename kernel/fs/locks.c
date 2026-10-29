#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

/* Record locks (fcntl F_SETLK and friends), as on Linux: byte ranges of a
 * file, shared (read) or exclusive (write). Two kinds of owner:
 *   POSIX locks belong to a process: they go when it closes any descriptor
 *   of the file, or exits; a child of fork has none of them.
 *   OFD locks (F_OFD_*) belong to an open file description: they go when its
 *   last descriptor closes, and are shared by dup and fork.
 * Programs check whether others are still alive this way: Flatpak keeps
 * a running app's details while a lock on its instance is held, and deletes
 * them once it can take that lock itself, so a lock that is always granted
 * made every running app look gone. */

#define F_RDLCK_ 0
#define F_WRLCK_ 1
#define END_OF_ALL UINT64_MAX

struct rlock {
    const struct vnode *vn;
    uint64_t start, end; /* [start, end) */
    int16_t type;
    bool ofd;
    const void *owner; /* struct process * or struct file * */
    int pid;
    struct rlock *next;
};

/* struct flock on x86-64 */
struct kflock {
    int16_t l_type, l_whence;
    int32_t pad;
    int64_t l_start, l_len;
    int32_t l_pid, pad2;
};

static struct rlock *locks;
static spinlock_t locks_lock;

bool proc_alive(int pid);

/* A POSIX lock whose process has gone (it exited without closing the file
 * through close(), e.g. killed) holds nothing. */
static bool holds(const struct rlock *r) { return r->ofd || proc_alive(r->pid); }

static bool conflicts(const struct rlock *r, const struct vnode *vn, uint64_t s, uint64_t e, int16_t type,
                      const void *owner) {
    return r->vn == vn && r->owner != owner && r->start < e && s < r->end &&
           (r->type == F_WRLCK_ || type == F_WRLCK_) && holds(r);
}

static struct rlock *find_conflict(const struct vnode *vn, uint64_t s, uint64_t e, int16_t type, const void *owner) {
    for (struct rlock *r = locks; r; r = r->next)
        if (conflicts(r, vn, s, e, type, owner))
            return r;
    return NULL;
}

/* The owner's locks on [s, e) of vn go (split where they reach outside it);
 * then, unless unlocking, one of `type` covers it. Lock held. */
static int set_range(const struct vnode *vn, uint64_t s, uint64_t e, int16_t type, bool ofd, const void *owner,
                     int pid) {
    struct rlock *add = NULL, *tail_piece = NULL;
    if (type != F_UNLCK) {
        add = kzalloc(sizeof *add);
        if (!add)
            return -ENOLCK;
    }
    for (struct rlock **pp = &locks; *pp;) {
        struct rlock *r = *pp;
        if (!holds(r)) { /* left by a process that has gone */
            *pp = r->next;
            kfree(r);
            continue;
        }
        if (r->vn != vn || r->owner != owner || r->end <= s || e <= r->start) {
            pp = &r->next;
            continue;
        }
        if (r->start < s && r->end > e) { /* the new range is inside: keep both ends */
            tail_piece = kzalloc(sizeof *tail_piece);
            if (!tail_piece) {
                kfree(add);
                return -ENOLCK;
            }
            *tail_piece = *r;
            tail_piece->start = e;
            r->end = s;
            tail_piece->next = r->next;
            r->next = tail_piece;
            pp = &tail_piece->next;
        } else if (r->start < s) {
            r->end = s;
            pp = &r->next;
        } else if (r->end > e) {
            r->start = e;
            pp = &r->next;
        } else {
            *pp = r->next;
            kfree(r);
        }
    }
    if (add) {
        *add = (struct rlock){vn, s, e, type, ofd, owner, pid, locks};
        locks = add;
    }
    return 0;
}

int64_t locks_fcntl(struct file *f, int cmd, uint64_t uarg) {
    struct kflock fl;
    if (copy_from_user(&fl, uarg, sizeof fl) < 0)
        return -EFAULT;
    bool ofd = cmd == F_OFD_GETLK || cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW;
    bool get = cmd == F_GETLK || cmd == F_OFD_GETLK, wait = cmd == F_SETLKW || cmd == F_OFD_SETLKW;
    if (fl.l_type != F_RDLCK_ && fl.l_type != F_WRLCK_ && fl.l_type != F_UNLCK)
        return -EINVAL;
    if (ofd && fl.l_pid != 0)
        return -EINVAL;
    if (!get && ((fl.l_type == F_RDLCK_ && (f->flags & O_ACCMODE) == O_WRONLY) ||
                 (fl.l_type == F_WRLCK_ && (f->flags & O_ACCMODE) == O_RDONLY)))
        return -EBADF;
    int64_t base = fl.l_whence == 0 ? 0 : fl.l_whence == 1 ? (int64_t)f->offset
                 : fl.l_whence == 2 ? (int64_t)f->vnode->size : -1;
    if (base < 0)
        return -EINVAL;
    int64_t s = base + fl.l_start;
    uint64_t e;
    if (fl.l_len > 0) {
        e = (uint64_t)(s + fl.l_len);
    } else if (fl.l_len < 0) {
        e = (uint64_t)s;
        s += fl.l_len;
    } else {
        e = END_OF_ALL;
    }
    if (s < 0)
        return -EINVAL;
    struct process *me = proc_current();
    const void *owner = ofd ? (const void *)f : (const void *)me;
    for (;;) {
        uint64_t irq = spin_lock_irqsave(&locks_lock);
        if (get) {
            struct rlock *r = find_conflict(f->vnode, (uint64_t)s, e, fl.l_type == F_UNLCK ? F_RDLCK_ : fl.l_type, owner);
            if (r) {
                fl.l_type = r->type;
                fl.l_whence = 0;
                fl.l_start = (int64_t)r->start;
                fl.l_len = r->end == END_OF_ALL ? 0 : (int64_t)(r->end - r->start);
                fl.l_pid = r->ofd ? -1 : r->pid;
            } else {
                fl.l_type = F_UNLCK;
            }
            spin_unlock_irqrestore(&locks_lock, irq);
            return copy_to_user(uarg, &fl, sizeof fl);
        }
        if (fl.l_type == F_UNLCK || !find_conflict(f->vnode, (uint64_t)s, e, fl.l_type, owner)) {
            int r = set_range(f->vnode, (uint64_t)s, e, fl.l_type, ofd, owner, me->pid);
            spin_unlock_irqrestore(&locks_lock, irq);
            return r;
        }
        spin_unlock_irqrestore(&locks_lock, irq);
        if (!wait)
            return -EAGAIN;
        if (signal_pending_current())
            return -EINTR;
        thread_sleep_ms(10); /* until the holder lets go */
    }
}

static void drop_where(bool (*match)(const struct rlock *, const void *, const void *), const void *a, const void *b) {
    uint64_t irq = spin_lock_irqsave(&locks_lock);
    for (struct rlock **pp = &locks; *pp;) {
        struct rlock *r = *pp;
        if (match(r, a, b)) {
            *pp = r->next;
            kfree(r);
        } else {
            pp = &r->next;
        }
    }
    spin_unlock_irqrestore(&locks_lock, irq);
}

static bool ofd_of(const struct rlock *r, const void *file, const void *unused) {
    (void)unused;
    return r->ofd && r->owner == file;
}

static bool posix_of(const struct rlock *r, const void *proc, const void *vn) {
    return !r->ofd && r->owner == proc && r->vn == vn;
}

/* The last descriptor of an open file description has closed. */
void locks_file_released(struct file *f) {
    if (__atomic_load_n(&locks, __ATOMIC_RELAXED))
        drop_where(ofd_of, f, NULL);
}

/* A process closed a descriptor of vn: all its POSIX locks on vn go. */
void locks_closed_by(struct process *p, struct vnode *vn) {
    if (__atomic_load_n(&locks, __ATOMIC_RELAXED))
        drop_where(posix_of, p, vn);
}
