#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>

/* epoll, built on the files' poll ops. epoll_wait rescans the registered
 * files every tick, as poll() does, instead of hooking callbacks into every
 * wait queue: with the handful of descriptors a Wayland client or event loop
 * watches, a scan costs next to nothing.
 *
 * Level-triggered items report whenever ready. Edge-triggered items report
 * when ready and their file's wait queue has been woken since the last report
 * (waitq.seq), which is when Linux would queue a new event; files without a
 * wait queue are treated as level-triggered.
 *
 * Registrations belong to the open file description, as on Linux: an item
 * holds no reference, and the last close of the file removes it from every
 * epoll instance (epoll_file_closed, called from file_put). */

struct ep_item {
    struct ep_item *next;
    struct file *file;
    int fd;
    uint32_t events;
    uint64_t data;
    struct waitq *wq;  /* where the file signals changes, learnt from its poll op */
    uint32_t seen_seq; /* EPOLLET: wq->seq when last reported */
    bool reported;     /* EPOLLET: seen_seq is valid */
    bool disabled;     /* EPOLLONESHOT fired; EPOLL_CTL_MOD re-arms it */
};

struct epoll {
    spinlock_t lock;
    struct ep_item *items;
    struct epoll *next_all;
};

/* Lock order: epolls_lock, then an epoll's lock, then whatever locks the
 * files' poll ops take. Files are never released with these held. */
static spinlock_t epolls_lock = SPINLOCK_INIT;
static struct epoll *all_epolls;

static const struct vnode_ops epoll_ops;

/* Lock held. Collects up to `max` ready events into `out` (if non-NULL).
 * `consume` applies the edge-triggered and one-shot bookkeeping (epoll_wait);
 * without it the scan only looks (poll on the epoll descriptor itself). */
static int ep_scan(struct epoll *ep, struct linux_epoll_event *out, int max, bool consume) {
    int n = 0;
    struct ep_item *last = NULL;
    for (struct ep_item *it = ep->items; it && n < max; it = it->next) {
        if (it->disabled)
            continue;
        /* Read before polling: a wake racing with the poll then shows up as
         * one more (harmless) event rather than a lost one. */
        uint32_t seq = it->wq ? __atomic_load_n(&it->wq->seq, __ATOMIC_ACQUIRE) : 0;
        struct waitq *wq;
        uint32_t ev = (uint32_t)poll_file(it->file, &wq) & (it->events | POLLERR | POLLHUP);
        if (wq != it->wq) {
            it->wq = wq;
            it->reported = false;
        }
        if (!ev)
            continue;
        if ((it->events & EPOLLET) && wq && it->reported && seq == it->seen_seq)
            continue; /* nothing happened since the last report */
        if (consume) {
            it->seen_seq = seq;
            it->reported = true;
            if (it->events & EPOLLONESHOT)
                it->disabled = true;
        }
        if (out)
            out[n] = (struct linux_epoll_event){ev, it->data};
        n++;
        last = it;
    }
    /* With more ready items than room, start after the last one reported next
     * time, so level-triggered items early in the list cannot starve the rest. */
    if (consume && n == max && last && last->next) {
        struct ep_item *tail = last->next;
        while (tail->next)
            tail = tail->next;
        tail->next = ep->items;
        ep->items = last->next;
        last->next = NULL;
    }
    return n;
}

static int ep_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    (void)wq; /* no queue: nested epoll is level-triggered */
    struct epoll *ep = v->data;
    uint64_t fl = spin_lock_irqsave(&ep->lock);
    int n = ep_scan(ep, NULL, 1, false);
    spin_unlock_irqrestore(&ep->lock, fl);
    return n ? POLLIN : 0;
}

static void ep_release(struct vnode *v) {
    struct epoll *ep = v->data;
    if (!ep)
        return;
    uint64_t g = spin_lock_irqsave(&epolls_lock);
    for (struct epoll **pp = &all_epolls; *pp; pp = &(*pp)->next_all)
        if (*pp == ep) {
            *pp = ep->next_all;
            break;
        }
    struct ep_item *items = ep->items;
    for (struct ep_item *it = items; it; it = it->next)
        __atomic_sub_fetch(&it->file->epoll_refs, 1, __ATOMIC_ACQ_REL);
    ep->items = NULL;
    spin_unlock_irqrestore(&epolls_lock, g);
    while (items) {
        struct ep_item *next = items->next;
        kfree(items);
        items = next;
    }
    kfree(ep);
}

static const struct vnode_ops epoll_ops = {.poll = ep_poll, .release = ep_release};

/* Debug (proc_dump_threads): what an epoll instance watches, and what each
 * watched file says right now. A ready item next to a sleeping waiter is a
 * lost event. */
void epoll_debug_dump(struct file *f);
void epoll_debug_dump(struct file *f) {
    if (!f || f->vnode->ops != &epoll_ops)
        return;
    struct epoll *ep = f->vnode->data;
    uint64_t fl = spin_lock_irqsave(&ep->lock);
    for (struct ep_item *it = ep->items; it; it = it->next) {
        struct waitq *wq;
        int ready = poll_file(it->file, &wq);
        kprintf("      watches fd %d (type %d) for %x%s%s: ready %x, seq %u seen %u%s\n", it->fd,
                (int)it->file->vnode->type, it->events & 0xffff, it->events & EPOLLET ? " ET" : "",
                it->events & EPOLLONESHOT ? " ONESHOT" : "", ready, wq ? wq->seq : 0, it->seen_seq,
                it->disabled ? " disabled" : "");
    }
    spin_unlock_irqrestore(&ep->lock, fl);
}

void epoll_file_closed(struct file *f) {
    struct ep_item *dead = NULL;
    uint64_t g = spin_lock_irqsave(&epolls_lock);
    for (struct epoll *ep = all_epolls; ep; ep = ep->next_all) {
        uint64_t fl = spin_lock_irqsave(&ep->lock);
        for (struct ep_item **pp = &ep->items; *pp;) {
            struct ep_item *it = *pp;
            if (it->file == f) {
                *pp = it->next;
                it->next = dead;
                dead = it;
            } else {
                pp = &it->next;
            }
        }
        spin_unlock_irqrestore(&ep->lock, fl);
    }
    spin_unlock_irqrestore(&epolls_lock, g);
    while (dead) {
        struct ep_item *next = dead->next;
        kfree(dead);
        dead = next;
    }
}

static int64_t do_epoll_create(uint64_t flags) {
    if (flags & ~(uint64_t)O_CLOEXEC)
        return -EINVAL;
    struct vnode *v = vnode_alloc(VN_ANON, 0600, &epoll_ops);
    struct epoll *ep = kzalloc(sizeof *ep);
    if (!v || !ep) {
        kfree(v);
        kfree(ep);
        return -ENOMEM;
    }
    v->data = ep;
    uint64_t g = spin_lock_irqsave(&epolls_lock);
    ep->next_all = all_epolls;
    all_epolls = ep;
    spin_unlock_irqrestore(&epolls_lock, g);
    return vnode_install_fd(v, O_RDWR, flags & O_CLOEXEC);
}

SYSCALL(epoll_create) {
    UNUSED_ARGS();
    return (int)a1 <= 0 ? -EINVAL : do_epoll_create(0);
}

SYSCALL(epoll_create1) {
    UNUSED_ARGS();
    return do_epoll_create(a1);
}

/* An epoll instance may only be added to another one while it watches no
 * epoll instances itself. That keeps nesting acyclic (a cycle would need its
 * last link to point at an instance that already has a nested one), so the
 * nested scans always take locks in the same order. */
static bool has_nested(struct epoll *ep) {
    bool found = false;
    uint64_t fl = spin_lock_irqsave(&ep->lock);
    for (struct ep_item *it = ep->items; it && !found; it = it->next)
        found = it->file->vnode->ops == &epoll_ops;
    spin_unlock_irqrestore(&ep->lock, fl);
    return found;
}

SYSCALL(epoll_ctl) {
    UNUSED_ARGS();
    int op = (int)a2, fd = (int)a3;
    struct linux_epoll_event ev = {0, 0};
    if (op != EPOLL_CTL_DEL && copy_from_user(&ev, a4, sizeof ev) < 0)
        return -EFAULT;
    if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL)
        return -EINVAL;
    struct file *ef = fd_get((int)a1), *tf = fd_get(fd);
    int64_t r = 0;
    if (!ef || !tf)
        r = -EBADF;
    else if (ef->vnode->ops != &epoll_ops || tf == ef)
        r = -EINVAL;
    else if (!tf->vnode->ops->poll)
        r = -EPERM; /* regular files and directories, as on Linux */
    else if (op == EPOLL_CTL_ADD && tf->vnode->ops == &epoll_ops && has_nested(tf->vnode->data))
        r = -ELOOP;
    else if (op == EPOLL_CTL_MOD && (ev.events & EPOLLEXCLUSIVE))
        r = -EINVAL;
    struct ep_item *fresh = NULL, *gone = NULL;
    if (r == 0 && op == EPOLL_CTL_ADD && !(fresh = kzalloc(sizeof *fresh)))
        r = -ENOMEM;
    if (r == 0) {
        struct epoll *ep = ef->vnode->data;
        uint64_t fl = spin_lock_irqsave(&ep->lock);
        struct ep_item **pp = &ep->items;
        while (*pp && ((*pp)->file != tf || (*pp)->fd != fd))
            pp = &(*pp)->next;
        struct ep_item *it = *pp;
        if (op == EPOLL_CTL_ADD) {
            if (it) {
                r = -EEXIST;
            } else {
                *fresh = (struct ep_item){.file = tf, .fd = fd, .events = ev.events, .data = ev.data};
                *pp = fresh; /* at the tail: registration order */
                fresh = NULL;
                __atomic_add_fetch(&tf->epoll_refs, 1, __ATOMIC_ACQ_REL);
            }
        } else if (!it) {
            r = -ENOENT;
        } else if (op == EPOLL_CTL_MOD) {
            it->events = ev.events;
            it->data = ev.data;
            it->disabled = false;
            it->reported = false; /* report current readiness, as Linux does */
        } else {
            *pp = it->next;
            gone = it;
            __atomic_sub_fetch(&tf->epoll_refs, 1, __ATOMIC_ACQ_REL);
        }
        spin_unlock_irqrestore(&ep->lock, fl);
    }
    kfree(fresh);
    kfree(gone);
    if (tf)
        file_put(tf);
    if (ef)
        file_put(ef);
    return r;
}

static int64_t do_epoll_wait(int epfd, uint64_t uevents, int maxevents, int64_t timeout_ms) {
    if (maxevents <= 0 || maxevents > 0x7fffffff / (int)sizeof(struct linux_epoll_event))
        return -EINVAL;
    struct file *f = fd_get(epfd);
    if (!f)
        return -EBADF;
    if (f->vnode->ops != &epoll_ops) {
        file_put(f);
        return -EINVAL;
    }
    struct epoll *ep = f->vnode->data;
    int cap = maxevents < 256 ? maxevents : 256;
    struct linux_epoll_event *out = kmalloc((size_t)cap * sizeof *out);
    if (!out) {
        file_put(f);
        return -ENOMEM;
    }
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : time_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    int64_t n;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&ep->lock);
        n = ep_scan(ep, out, cap, true);
        spin_unlock_irqrestore(&ep->lock, fl);
        if (n > 0 || timeout_ms == 0 || time_monotonic_ns() >= deadline)
            break;
        if (signal_pending_current()) {
            n = -EINTR;
            break;
        }
        thread_sleep_ms(4);
    }
    if (n > 0 && copy_to_user(uevents, out, (size_t)n * sizeof *out) < 0)
        n = -EFAULT;
    kfree(out);
    file_put(f);
    return n;
}

SYSCALL(epoll_wait) {
    UNUSED_ARGS();
    return do_epoll_wait((int)a1, a2, (int)a3, (int64_t)(int)a4);
}

static int64_t epoll_pwait_common(uint64_t epfd, uint64_t uevents, uint64_t maxevents, int64_t timeout_ms,
                                  uint64_t usigmask, uint64_t sigsetsize) {
    if (usigmask) {
        int r = sigmask_push(usigmask, sigsetsize);
        if (r < 0)
            return r;
    }
    int64_t r = do_epoll_wait((int)epfd, uevents, (int)maxevents, timeout_ms);
    if (usigmask)
        sigmask_pop(r);
    return r;
}

SYSCALL(epoll_pwait) {
    UNUSED_ARGS();
    return epoll_pwait_common(a1, a2, a3, (int64_t)(int)a4, a5, a6);
}

SYSCALL(epoll_pwait2) {
    UNUSED_ARGS();
    int64_t timeout = user_timeout_ms(a4);
    if (timeout < -1)
        return timeout;
    return epoll_pwait_common(a1, a2, a3, timeout, a5, a6);
}
