#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>

/* Event descriptors: eventfd (a counter), timerfd (a timer read as an
 * expiration count) and signalfd (pending signals read as records). Each is
 * an anonymous vnode, so read, poll, epoll and close work as for any file. */

/* ---- eventfd ---- */

#define EVENTFD_MAX 0xfffffffffffffffeULL

struct eventfd {
    uint64_t count;
    bool semaphore;
    spinlock_t lock;
    struct waitq wq; /* readers, writers and pollers */
};

static bool efd_readable(void *arg) { return __atomic_load_n(&((struct eventfd *)arg)->count, __ATOMIC_ACQUIRE) != 0; }

struct efd_write_wait {
    struct eventfd *e;
    uint64_t add;
};

static bool efd_writable(void *arg) {
    struct efd_write_wait *w = arg;
    return __atomic_load_n(&w->e->count, __ATOMIC_ACQUIRE) <= EVENTFD_MAX - w->add;
}

static int64_t efd_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct eventfd *e = v->data;
    if (len < 8)
        return -EINVAL;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&e->lock);
        uint64_t value = e->semaphore && e->count ? 1 : e->count;
        e->count -= value;
        spin_unlock_irqrestore(&e->lock, fl);
        if (value) {
            waitq_wake_all(&e->wq); /* blocked writers may fit now */
            memcpy(buf, &value, 8);
            return 8;
        }
        if (f && (f->flags & O_NONBLOCK))
            return -EAGAIN;
        if (waitq_wait_cond(&e->wq, efd_readable, e) < 0)
            return -EINTR;
    }
}

static int64_t efd_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)off;
    struct eventfd *e = v->data;
    uint64_t add;
    if (len < 8)
        return -EINVAL;
    memcpy(&add, buf, 8);
    if (add == UINT64_MAX)
        return -EINVAL;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&e->lock);
        bool fits = e->count <= EVENTFD_MAX - add;
        if (fits)
            e->count += add;
        spin_unlock_irqrestore(&e->lock, fl);
        if (fits) {
            if (add)
                waitq_wake_all(&e->wq);
            return 8;
        }
        if (f && (f->flags & O_NONBLOCK))
            return -EAGAIN;
        struct efd_write_wait w = {e, add};
        if (waitq_wait_cond(&e->wq, efd_writable, &w) < 0)
            return -EINTR;
    }
}

static int efd_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct eventfd *e = v->data;
    uint64_t count = __atomic_load_n(&e->count, __ATOMIC_ACQUIRE);
    if (wq)
        *wq = &e->wq;
    return (count ? POLLIN : 0) | (count < EVENTFD_MAX ? POLLOUT : 0);
}

static void efd_release(struct vnode *v) { kfree(v->data); }

static const struct vnode_ops eventfd_ops = {
    .read = efd_read, .write = efd_write, .poll = efd_poll, .release = efd_release,
};

static int64_t do_eventfd(uint64_t initval, uint64_t flags) {
    if (flags & ~(uint64_t)(EFD_SEMAPHORE | O_NONBLOCK | O_CLOEXEC))
        return -EINVAL;
    struct vnode *v = vnode_alloc(VN_ANON, 0600, &eventfd_ops);
    struct eventfd *e = kzalloc(sizeof *e);
    if (!v || !e) {
        kfree(v);
        kfree(e);
        return -ENOMEM;
    }
    e->count = (uint32_t)initval;
    e->semaphore = flags & EFD_SEMAPHORE;
    waitq_init(&e->wq);
    v->data = e;
    return vnode_install_fd(v, O_RDWR | (flags & O_NONBLOCK), flags & O_CLOEXEC);
}

SYSCALL(eventfd) {
    UNUSED_ARGS();
    return do_eventfd(a1, 0);
}

SYSCALL(eventfd2) {
    UNUSED_ARGS();
    return do_eventfd(a1, a2);
}

/* ---- timerfd ---- */

/* Evaluated lazily: the expiration count is brought up to date against the
 * clock whenever the timer is read, polled or queried. Pollers rescan every
 * tick, so they notice expirations without a kernel timer. Deadlines are kept
 * on the monotonic clock; CLOCK_REALTIME times are converted when set. */
struct timerfd {
    int clock;
    uint64_t next;     /* monotonic ns of the next expiration; 0 = disarmed */
    uint64_t interval; /* ns; 0 = one-shot */
    uint64_t ticks;    /* expirations not yet read */
    spinlock_t lock;
    struct waitq wq;
};

/* Lock held. Returns true if new expirations were counted. */
static bool tfd_update(struct timerfd *t, uint64_t now) {
    if (!t->next || now < t->next)
        return false;
    if (t->interval) {
        uint64_t n = (now - t->next) / t->interval + 1;
        t->ticks += n;
        t->next += n * t->interval;
    } else {
        t->ticks++;
        t->next = 0;
    }
    return true;
}

static int64_t tfd_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct timerfd *t = v->data;
    if (len < 8)
        return -EINVAL;
    for (;;) {
        uint64_t now = time_monotonic_ns();
        uint64_t fl = spin_lock_irqsave(&t->lock);
        tfd_update(t, now);
        uint64_t ticks = t->ticks, next = t->next;
        t->ticks = 0;
        spin_unlock_irqrestore(&t->lock, fl);
        if (ticks) {
            memcpy(buf, &ticks, 8);
            return 8;
        }
        if (f && (f->flags & O_NONBLOCK))
            return -EAGAIN;
        if (signal_pending_current())
            return -EINTR;
        /* Sleep until the deadline, in slices so a settime from another
         * process and signals are noticed. */
        uint64_t ms = next > now ? (next - now + 999999) / 1000000 : 50;
        thread_sleep_ms(ms < 50 ? ms : 50);
    }
}

static int tfd_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct timerfd *t = v->data;
    uint64_t fl = spin_lock_irqsave(&t->lock);
    bool fired = tfd_update(t, time_monotonic_ns());
    int ev = t->ticks ? POLLIN : 0;
    spin_unlock_irqrestore(&t->lock, fl);
    if (fired)
        waitq_wake_all(&t->wq); /* an event for edge-triggered epoll */
    if (wq)
        *wq = &t->wq;
    return ev;
}

static void tfd_release(struct vnode *v) { kfree(v->data); }

static const struct vnode_ops timerfd_ops = {.read = tfd_read, .poll = tfd_poll, .release = tfd_release};

static bool valid_clock(int64_t clock) {
    return clock == CLOCK_REALTIME || clock == CLOCK_MONOTONIC || clock == CLOCK_BOOTTIME;
}

SYSCALL(timerfd_create) {
    UNUSED_ARGS();
    if (!valid_clock((int64_t)a1))
        return -EINVAL;
    if (a2 & ~(uint64_t)(O_NONBLOCK | O_CLOEXEC))
        return -EINVAL;
    struct vnode *v = vnode_alloc(VN_ANON, 0600, &timerfd_ops);
    struct timerfd *t = kzalloc(sizeof *t);
    if (!v || !t) {
        kfree(v);
        kfree(t);
        return -ENOMEM;
    }
    t->clock = (int)a1;
    waitq_init(&t->wq);
    v->data = t;
    return vnode_install_fd(v, O_RDWR | (a2 & O_NONBLOCK), a2 & O_CLOEXEC);
}

static struct timerfd *fd_timer(int fd, struct file **out) {
    struct file *f = fd_get(fd);
    if (f && f->vnode->ops == &timerfd_ops) {
        *out = f;
        return f->vnode->data;
    }
    if (f)
        file_put(f);
    return NULL;
}

static uint64_t ts_ns(const struct linux_timespec *ts) {
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static struct linux_timespec ns_ts(uint64_t ns) {
    return (struct linux_timespec){(int64_t)(ns / 1000000000ULL), (int64_t)(ns % 1000000000ULL)};
}

/* Lock held: the current setting as an itimerspec (time left, interval). */
static struct linux_itimerspec tfd_current(struct timerfd *t, uint64_t now) {
    struct linux_itimerspec cur = {.interval = ns_ts(t->interval)};
    if (t->next)
        cur.value = ns_ts(t->next > now ? t->next - now : 1);
    return cur;
}

SYSCALL(timerfd_settime) {
    UNUSED_ARGS();
    if (a2 & ~(uint64_t)(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
        return -EINVAL;
    struct linux_itimerspec its;
    if (copy_from_user(&its, a3, sizeof its) < 0)
        return -EFAULT;
    const struct linux_timespec *chk[2] = {&its.interval, &its.value};
    for (int i = 0; i < 2; i++)
        if (chk[i]->tv_sec < 0 || chk[i]->tv_nsec < 0 || chk[i]->tv_nsec >= 1000000000)
            return -EINVAL;
    struct file *f;
    struct timerfd *t = fd_timer((int)a1, &f);
    if (!t)
        return -EINVAL;
    uint64_t now = time_monotonic_ns(), value = ts_ns(&its.value);
    uint64_t next = 0;
    if (value) {
        if (!(a2 & TFD_TIMER_ABSTIME)) {
            next = now + value;
        } else if (t->clock == CLOCK_REALTIME) {
            uint64_t real = time_realtime_ns();
            next = value > real ? now + (value - real) : now;
        } else {
            next = value;
        }
        if (!next)
            next = 1; /* 0 means disarmed; an absolute time of 0 has passed anyway */
    }
    uint64_t fl = spin_lock_irqsave(&t->lock);
    tfd_update(t, now);
    struct linux_itimerspec old = tfd_current(t, now);
    t->next = next;
    t->interval = ts_ns(&its.interval);
    t->ticks = 0;
    spin_unlock_irqrestore(&t->lock, fl);
    waitq_wake_all(&t->wq);
    file_put(f);
    if (a4 && copy_to_user(a4, &old, sizeof old) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL(timerfd_gettime) {
    UNUSED_ARGS();
    struct file *f;
    struct timerfd *t = fd_timer((int)a1, &f);
    if (!t)
        return -EINVAL;
    uint64_t now = time_monotonic_ns();
    uint64_t fl = spin_lock_irqsave(&t->lock);
    tfd_update(t, now);
    struct linux_itimerspec cur = tfd_current(t, now);
    spin_unlock_irqrestore(&t->lock, fl);
    file_put(f);
    return copy_to_user(a2, &cur, sizeof cur);
}

/* ---- signalfd ---- */

/* Reads the calling thread's pending signals that are in the descriptor's
 * mask (normally blocked with sigprocmask, so no handler takes them first):
 * those sent to the thread itself, then those sent to the whole process,
 * as Linux's dequeue_signal does. Signal arrival wakes no queue here, so
 * readers and pollers rescan. */
struct signalfd {
    uint64_t mask; /* a sigset: bit n-1 is signal n, as everywhere (proc.h) */
};

static uint64_t sfd_pending(struct signalfd *s) {
    struct user_thread *ut = thread_current()->user;
    return (__atomic_load_n(&ut->pending_signals, __ATOMIC_ACQUIRE) |
            __atomic_load_n(&ut->proc->pending_signals, __ATOMIC_ACQUIRE)) &
           s->mask;
}

static int64_t sfd_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct signalfd *s = v->data;
    struct user_thread *ut = thread_current()->user;
    struct process *p = ut->proc;
    size_t rec = sizeof(struct linux_signalfd_siginfo);
    if (len < rec)
        return -EINVAL;
    size_t done = 0;
    for (;;) {
        while (done + rec <= len) {
            uint64_t pending = sfd_pending(s);
            if (!pending)
                break;
            int sig = __builtin_ctzll(pending) + 1;
            uint64_t bit = sigbit(sig);
            if (!(__atomic_fetch_and(&ut->pending_signals, ~bit, __ATOMIC_ACQ_REL) & bit) &&
                !(__atomic_fetch_and(&p->pending_signals, ~bit, __ATOMIC_ACQ_REL) & bit))
                continue; /* delivered elsewhere meanwhile */
            struct linux_signalfd_siginfo si = {.signo = (uint32_t)sig};
            memcpy((uint8_t *)buf + done, &si, rec);
            done += rec;
        }
        if (done)
            return (int64_t)done;
        if (f && (f->flags & O_NONBLOCK))
            return -EAGAIN;
        if (signal_pending_current())
            return -EINTR;
        thread_sleep_ms(4);
    }
}

static int sfd_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct signalfd *s = v->data;
    if (wq)
        *wq = NULL; /* no queue to sleep on: level-triggered for epoll */
    return sfd_pending(s) ? POLLIN : 0;
}

static void sfd_release(struct vnode *v) { kfree(v->data); }

static const struct vnode_ops signalfd_ops = {.read = sfd_read, .poll = sfd_poll, .release = sfd_release};

static int64_t do_signalfd(int fd, uint64_t umask, uint64_t size, uint64_t flags) {
    if (size != 8 || (flags & ~(uint64_t)(O_NONBLOCK | O_CLOEXEC)))
        return -EINVAL;
    uint64_t set;
    if (copy_from_user(&set, umask, 8) < 0)
        return -EFAULT;
    uint64_t mask = set & ~SIG_UNBLOCKABLE;
    if (fd != -1) { /* change the mask of an existing signalfd */
        struct file *f = fd_get(fd);
        if (!f)
            return -EBADF;
        int64_t r = fd;
        if (f->vnode->ops == &signalfd_ops)
            ((struct signalfd *)f->vnode->data)->mask = mask;
        else
            r = -EINVAL;
        file_put(f);
        return r;
    }
    struct vnode *v = vnode_alloc(VN_ANON, 0600, &signalfd_ops);
    struct signalfd *s = kzalloc(sizeof *s);
    if (!v || !s) {
        kfree(v);
        kfree(s);
        return -ENOMEM;
    }
    s->mask = mask;
    v->data = s;
    return vnode_install_fd(v, O_RDWR | (flags & O_NONBLOCK), flags & O_CLOEXEC);
}

SYSCALL(signalfd) {
    UNUSED_ARGS();
    return do_signalfd((int)a1, a2, a3, 0);
}

SYSCALL(signalfd4) {
    UNUSED_ARGS();
    return do_signalfd((int)a1, a2, a3, a4);
}
