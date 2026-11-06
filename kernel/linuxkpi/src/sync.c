#include <lkpi/all.h>
#include "../host.h"

/* kernel/linuxkpi: Linux's locks and ways of waiting, on our scheduler (host.h: block, wake). A wake that comes
 * before the waiter has gone to sleep is kept by our scheduler, so a waiter queued under the lock never misses one.
 * Spin locks also hold off interrupts on their processor (so nothing preempts the holder), as the driver's do. */

#define IF_ON 0x200UL /* (the interrupt flag: what lkpi_irq_restore turns interrupts back on with) */

/* ---- interrupts and preemption ---- */

int lkpi_irq_depth[256];          /* how deep in the layer's interrupt handlers each processor is (irq.c) */
int lkpi_preempt_depth[256]; /* (sched.c: RCU waits for each processor to be out of its read side) */
static unsigned long preempt_flags[256];

unsigned long lkpi_irq_save(void) { return (unsigned long)kpi_host_irq_save(); }
void lkpi_irq_restore(unsigned long f) { kpi_host_irq_restore(f); }
void local_irq_disable(void) { (void)kpi_host_irq_save(); }
void local_irq_enable(void) { kpi_host_irq_restore(IF_ON); }
bool irqs_disabled(void) { return !kpi_host_irqs_on(); }

void preempt_disable(void) {
    unsigned long f = (unsigned long)kpi_host_irq_save();
    int c = kpi_host_cpu();
    if (!lkpi_preempt_depth[c]++)
        preempt_flags[c] = f;
}

void preempt_enable(void) {
    int c = kpi_host_cpu();
    if (lkpi_preempt_depth[c] > 0 && !--lkpi_preempt_depth[c])
        kpi_host_irq_restore(preempt_flags[c]);
}

bool in_irq(void) { return lkpi_irq_depth[kpi_host_cpu()] > 0; }
bool in_interrupt(void) { return in_irq(); }
bool in_task(void) { return !in_irq(); }
bool in_atomic(void) { return in_irq() || lkpi_preempt_depth[kpi_host_cpu()] > 0 || irqs_disabled(); }

/* ---- spin locks ---- */

static inline void cpu_relax_(void) { __asm__ __volatile__("pause" ::: "memory"); }

static void raw_lock(volatile int *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(l, __ATOMIC_RELAXED))
            cpu_relax_();
}

static void raw_unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

void spin_lock_init(spinlock_t *l) { l->locked = 0, l->flags_saved = 0; }

void spin_lock(spinlock_t *l) {
    unsigned long f = (unsigned long)kpi_host_irq_save();
    raw_lock(&l->locked);
    l->flags_saved = f;
}

void spin_unlock(spinlock_t *l) {
    unsigned long f = l->flags_saved;
    raw_unlock(&l->locked);
    kpi_host_irq_restore(f);
}

int spin_trylock(spinlock_t *l) {
    unsigned long f = (unsigned long)kpi_host_irq_save();
    if (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        kpi_host_irq_restore(f);
        return 0;
    }
    l->flags_saved = f;
    return 1;
}

unsigned long lkpi_spin_lock_irqsave(spinlock_t *l) {
    unsigned long f = (unsigned long)kpi_host_irq_save();
    raw_lock(&l->locked);
    return f;
}

void spin_unlock_irqrestore(spinlock_t *l, unsigned long f) {
    raw_unlock(&l->locked);
    kpi_host_irq_restore(f);
}

void spin_lock_irq(spinlock_t *l) {
    (void)kpi_host_irq_save();
    raw_lock(&l->locked);
}

void spin_unlock_irq(spinlock_t *l) {
    raw_unlock(&l->locked);
    kpi_host_irq_restore(IF_ON);
}

void spin_lock_bh(spinlock_t *l) { spin_lock(l); }
void spin_unlock_bh(spinlock_t *l) { spin_unlock(l); }

/* reader-writer spin locks: readers count under the lock word, a writer waits for none */
void read_lock(rwlock_t *l) {
    spin_lock(&l->w);
    __atomic_add_fetch(&l->readers, 1, __ATOMIC_ACQUIRE);
    spin_unlock(&l->w);
}

void read_unlock(rwlock_t *l) { __atomic_sub_fetch(&l->readers, 1, __ATOMIC_RELEASE); }

void write_lock(rwlock_t *l) {
    spin_lock(&l->w);
    while (__atomic_load_n(&l->readers, __ATOMIC_ACQUIRE))
        cpu_relax_();
}

void write_unlock(rwlock_t *l) { spin_unlock(&l->w); }

unsigned long lkpi_write_lock_irqsave(rwlock_t *l) {
    unsigned long f = lkpi_spin_lock_irqsave(&l->w);
    while (__atomic_load_n(&l->readers, __ATOMIC_ACQUIRE))
        cpu_relax_();
    return f;
}

void write_unlock_irqrestore(rwlock_t *l, unsigned long f) { spin_unlock_irqrestore(&l->w, f); }

unsigned long lkpi_read_lock_irqsave(rwlock_t *l) {
    unsigned long f = (unsigned long)kpi_host_irq_save();
    raw_lock(&l->w.locked);
    __atomic_add_fetch(&l->readers, 1, __ATOMIC_ACQUIRE);
    raw_unlock(&l->w.locked);
    return f;
}

void read_unlock_irqrestore(rwlock_t *l, unsigned long f) {
    __atomic_sub_fetch(&l->readers, 1, __ATOMIC_RELEASE);
    kpi_host_irq_restore(f);
}

/* ---- sleeping locks: a list of waiters each, and the lock handed straight to the one woken ---- */

struct waiter {
    struct list_head entry;
    kpi_host_thread *t;
    volatile int woken;
    bool writer;
};

static void hand_to(struct waiter *w) {
    list_del_init(&w->entry);
    __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
    kpi_host_wake(w->t); /* (under the lock, so the waiter's frame is still there) */
}

/* waits (with `lock` released) until handed what it waits for: 0, -EINTR on a signal, -ETIME at the deadline */
static int wait_handed(spinlock_t *lock, struct waiter *w, bool intr, u64 deadline_ns) {
    for (;;) {
        if (__atomic_load_n(&w->woken, __ATOMIC_ACQUIRE))
            return 0;
        bool sig = intr && kpi_host_signal_pending(), late = deadline_ns && kpi_host_now_ns() >= deadline_ns;
        if (sig || late) {
            int r = sig ? -EINTR : -ETIME;
            spin_lock(lock);
            bool got = w->woken; /* (handed over just now: it is ours after all) */
            if (!got)
                list_del_init(&w->entry);
            spin_unlock(lock);
            return got ? 0 : r;
        }
        kpi_host_sleep_until(deadline_ns);
    }
}

void mutex_init(struct mutex *m) {
    m->locked = 0, m->owner = NULL;
    INIT_LIST_HEAD(&m->waiters);
    spin_lock_init(&m->wait_lock);
}

static int mutex_lock_common(struct mutex *m, bool intr) {
    struct waiter w = {.t = kpi_host_thread_self()};
    spin_lock(&m->wait_lock);
    if (!m->locked) {
        m->locked = 1, m->owner = w.t;
        spin_unlock(&m->wait_lock);
        return 0;
    }
    list_add_tail(&w.entry, &m->waiters);
    spin_unlock(&m->wait_lock);
    return wait_handed(&m->wait_lock, &w, intr, 0);
}

void mutex_lock(struct mutex *m) { (void)mutex_lock_common(m, false); }
int mutex_lock_interruptible(struct mutex *m) { return mutex_lock_common(m, true); }

int mutex_trylock(struct mutex *m) {
    spin_lock(&m->wait_lock);
    int got = !m->locked;
    if (got)
        m->locked = 1, m->owner = kpi_host_thread_self();
    spin_unlock(&m->wait_lock);
    return got;
}

void mutex_unlock(struct mutex *m) {
    spin_lock(&m->wait_lock);
    if (list_empty(&m->waiters)) {
        m->locked = 0, m->owner = NULL;
    } else {
        struct waiter *w = list_first_entry(&m->waiters, struct waiter, entry);
        m->owner = w->t;
        hand_to(w);
    }
    spin_unlock(&m->wait_lock);
}

bool mutex_is_locked(struct mutex *m) { return READ_ONCE(m->locked) != 0; }

/* counting semaphores */
void sema_init(struct semaphore *s, int count) {
    s->count = count;
    spin_lock_init(&s->lock);
    INIT_LIST_HEAD(&s->waiters);
}

static int down_common(struct semaphore *s, bool intr, u64 deadline_ns) {
    struct waiter w = {.t = kpi_host_thread_self()};
    spin_lock(&s->lock);
    if (s->count > 0) {
        s->count--;
        spin_unlock(&s->lock);
        return 0;
    }
    list_add_tail(&w.entry, &s->waiters);
    spin_unlock(&s->lock);
    return wait_handed(&s->lock, &w, intr, deadline_ns);
}

void down(struct semaphore *s) { (void)down_common(s, false, 0); }
int down_interruptible(struct semaphore *s) { return down_common(s, true, 0); }
int down_timeout(struct semaphore *s, long j) { return down_common(s, false, kpi_host_now_ns() + jiffies_to_nsecs(j) + 1); }

int down_trylock(struct semaphore *s) {
    spin_lock(&s->lock);
    int busy = s->count <= 0;
    if (!busy)
        s->count--;
    spin_unlock(&s->lock);
    return busy; /* (Linux's way round: 0 means it got it) */
}

void up(struct semaphore *s) {
    spin_lock(&s->lock);
    if (list_empty(&s->waiters))
        s->count++;
    else
        hand_to(list_first_entry(&s->waiters, struct waiter, entry));
    spin_unlock(&s->lock);
}

/* reader-writer semaphores: count > 0 readers, -1 a writer; waiters are served in order, readers in runs */
void init_rwsem(struct rw_semaphore *s) {
    s->count = 0;
    spin_lock_init(&s->lock);
    INIT_LIST_HEAD(&s->waiters);
}

static void rwsem_wake(struct rw_semaphore *s) {
    while (!list_empty(&s->waiters)) {
        struct waiter *w = list_first_entry(&s->waiters, struct waiter, entry);
        if (w->writer) {
            if (s->count == 0) {
                s->count = -1;
                hand_to(w);
            }
            return;
        }
        if (s->count < 0)
            return;
        s->count++;
        hand_to(w);
    }
}

static void rwsem_down(struct rw_semaphore *s, bool writer) {
    struct waiter w = {.t = kpi_host_thread_self(), .writer = writer};
    spin_lock(&s->lock);
    bool free_now = list_empty(&s->waiters) && (writer ? s->count == 0 : s->count >= 0);
    if (free_now) {
        s->count = writer ? -1 : s->count + 1;
        spin_unlock(&s->lock);
        return;
    }
    list_add_tail(&w.entry, &s->waiters);
    spin_unlock(&s->lock);
    (void)wait_handed(&s->lock, &w, false, 0);
}

static int rwsem_try(struct rw_semaphore *s, bool writer) {
    spin_lock(&s->lock);
    bool ok = list_empty(&s->waiters) && (writer ? s->count == 0 : s->count >= 0);
    if (ok)
        s->count = writer ? -1 : s->count + 1;
    spin_unlock(&s->lock);
    return ok;
}

void down_read(struct rw_semaphore *s) { rwsem_down(s, false); }
void down_write(struct rw_semaphore *s) { rwsem_down(s, true); }
int down_read_trylock(struct rw_semaphore *s) { return rwsem_try(s, false); }
int down_write_trylock(struct rw_semaphore *s) { return rwsem_try(s, true); }

void up_read(struct rw_semaphore *s) {
    spin_lock(&s->lock);
    if (s->count > 0 && !--s->count)
        rwsem_wake(s);
    spin_unlock(&s->lock);
}

void up_write(struct rw_semaphore *s) {
    spin_lock(&s->lock);
    s->count = 0;
    rwsem_wake(s);
    spin_unlock(&s->lock);
}

void downgrade_write(struct rw_semaphore *s) {
    spin_lock(&s->lock);
    s->count = 1;
    rwsem_wake(s); /* (the readers waiting at the front join it) */
    spin_unlock(&s->lock);
}

/* ---- wait queues ---- */

void init_waitqueue_head(wait_queue_head_t *q) {
    spin_lock_init(&q->lock);
    INIT_LIST_HEAD(&q->head);
}

static void wake_queue(wait_queue_head_t *q) {
    spin_lock(&q->lock);
    struct wait_queue_entry *e, *n;
    list_for_each_entry_safe(e, n, &q->head, entry) {
        if (e->func)
            e->func(e, TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE, 0, NULL);
        else
            kpi_host_wake(e->private);
    }
    spin_unlock(&q->lock);
}

void wake_up(wait_queue_head_t *q) { wake_queue(q); }
void wake_up_all(wait_queue_head_t *q) { wake_queue(q); }
void wake_up_interruptible(wait_queue_head_t *q) { wake_queue(q); }
void wake_up_interruptible_all(wait_queue_head_t *q) { wake_queue(q); }
bool waitqueue_active(wait_queue_head_t *q) { return !list_empty(&q->head); }

void lkpi_waitqueue_add(wait_queue_head_t *q, struct wait_queue_entry *e) {
    spin_lock(&q->lock);
    list_add_tail(&e->entry, &q->head);
    spin_unlock(&q->lock);
}

void lkpi_waitqueue_remove(wait_queue_head_t *q, struct wait_queue_entry *e) {
    spin_lock(&q->lock);
    list_del_init(&e->entry);
    spin_unlock(&q->lock);
}

/* One wait on q: until woken, a signal (when interruptible), the timeout (in jiffies: milliseconds; negative: none)
 * or a few milliseconds, whichever is first (the caller looks at its condition again: a wake that came between its
 * looking and our queueing is caught that way). What is left of the timeout, 0 when it ran out, -ERESTARTSYS. */
long lkpi_wait_event(wait_queue_head_t *q, bool (*cond)(void *), void *arg, bool interruptible, long timeout) {
    if (interruptible && kpi_host_signal_pending())
        return -ERESTARTSYS;
    u64 now = kpi_host_now_ns();
    u64 end = timeout >= 0 ? now + jiffies_to_nsecs((unsigned long)timeout) : 0;
    struct wait_queue_entry e = {.private = kpi_host_thread_self()};
    lkpi_waitqueue_add(q, &e);
    if (!cond || !cond(arg)) {
        u64 nap = now + 4 * NSEC_PER_MSEC;
        kpi_host_sleep_until(end && end < nap ? end : nap);
    }
    lkpi_waitqueue_remove(q, &e);
    if (interruptible && kpi_host_signal_pending())
        return -ERESTARTSYS;
    if (timeout < 0)
        return timeout;
    now = kpi_host_now_ns();
    if (now >= end)
        return 0;
    long left = (long)((end - now + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC);
    return left > 0 ? left : 1;
}

/* ---- completions ---- */

void init_completion(struct completion *c) {
    c->done = 0;
    init_waitqueue_head(&c->wait);
}

void complete(struct completion *c) {
    spin_lock(&c->wait.lock);
    if (c->done != UINT_MAX)
        c->done++;
    spin_unlock(&c->wait.lock);
    wake_queue(&c->wait);
}

void complete_all(struct completion *c) {
    spin_lock(&c->wait.lock);
    c->done = UINT_MAX;
    spin_unlock(&c->wait.lock);
    wake_queue(&c->wait);
}

bool try_wait_for_completion(struct completion *c) {
    spin_lock(&c->wait.lock);
    bool got = c->done > 0;
    if (got && c->done != UINT_MAX)
        c->done--;
    spin_unlock(&c->wait.lock);
    return got;
}

bool completion_done(struct completion *c) { return READ_ONCE(c->done) > 0; }

/* 1 when it came, 0 at the timeout, -ERESTARTSYS on a signal (when interruptible) */
static long wait_completion(struct completion *c, bool intr, long timeout) {
    long left = timeout;
    for (;;) {
        struct wait_queue_entry e = {.private = kpi_host_thread_self()};
        lkpi_waitqueue_add(&c->wait, &e); /* (queued before looking: complete()'s wake is never missed) */
        if (try_wait_for_completion(c)) {
            lkpi_waitqueue_remove(&c->wait, &e);
            return left > 0 ? left : 1;
        }
        if (intr && kpi_host_signal_pending()) {
            lkpi_waitqueue_remove(&c->wait, &e);
            return -ERESTARTSYS;
        }
        u64 now = kpi_host_now_ns();
        kpi_host_sleep_until(left >= 0 ? now + jiffies_to_nsecs((unsigned long)left) + 1 : 0);
        lkpi_waitqueue_remove(&c->wait, &e);
        if (left >= 0) {
            u64 spent = (kpi_host_now_ns() - now) / NSEC_PER_MSEC;
            left = (long)spent >= left ? 0 : left - (long)spent;
            if (!left)
                return try_wait_for_completion(c) ? 1 : 0;
        }
    }
}

void wait_for_completion(struct completion *c) { (void)wait_completion(c, false, -1); }

int wait_for_completion_interruptible(struct completion *c) {
    long r = wait_completion(c, true, -1);
    return r < 0 ? (int)r : 0;
}

unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout) {
    long r = wait_completion(c, false, (long)(timeout > (unsigned long)LONG_MAX ? LONG_MAX : timeout));
    return r > 0 ? (unsigned long)r : 0;
}
