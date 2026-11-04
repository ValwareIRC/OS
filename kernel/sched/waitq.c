#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>

void waitq_init(struct waitq *wq) {
    wq->lock = (spinlock_t)SPINLOCK_INIT;
    wq->head = NULL;
    wq->seq = 0;
}

static void unlink_entry(struct waitq *wq, struct waitq_entry *e) {
    for (struct waitq_entry **pp = &wq->head; *pp; pp = &(*pp)->next)
        if (*pp == e) {
            *pp = e->next;
            return;
        }
}

int waitq_wait_cond(struct waitq *wq, bool (*cond)(void *), void *arg) {
    struct waitq_entry e = {.t = thread_current()};
    uint64_t f = spin_lock_irqsave(&wq->lock);
    e.next = wq->head;
    wq->head = &e;
    spin_unlock_irqrestore(&wq->lock, f);

    /* Registered before the check, so a wakeup between the check and the
     * block is not lost (thread_wake sets wake_pending). */
    bool interrupted = false;
    if (!cond(arg)) {
        if (signal_pending_current())
            interrupted = true;
        else
            thread_block_current();
    }

    f = spin_lock_irqsave(&wq->lock);
    unlink_entry(wq, &e);
    spin_unlock_irqrestore(&wq->lock, f);
    return interrupted ? -EINTR : 0;
}

struct mw_item {
    struct mw_item *next;
    struct waitq *wq;
    struct waitq_entry e;
    struct file *f;
};

void multiwait_add(struct multiwait *m, struct waitq *wq, struct file *f) {
    if (!wq) {
        m->unwaitable = true;
        return;
    }
    for (struct mw_item *it = m->items; it; it = it->next)
        if (it->wq == wq)
            return;
    struct mw_item *it = kmalloc(sizeof *it);
    if (!it) {
        m->unwaitable = true; /* (looked at again soon instead) */
        return;
    }
    it->wq = wq;
    it->f = f;
    if (f)
        file_ref(f);
    it->e.t = thread_current();
    uint64_t fl = spin_lock_irqsave(&wq->lock);
    it->e.next = wq->head;
    wq->head = &it->e;
    spin_unlock_irqrestore(&wq->lock, fl);
    it->next = m->items;
    m->items = it;
    m->added = true;
}

void multiwait_block(struct multiwait *m, uint64_t deadline_ns) {
    if (m->added) { /* listening now: the caller looks once more before sleeping */
        m->added = false;
        return;
    }
    if (m->unwaitable) {
        uint64_t soon = time_monotonic_ns() + 4000000ULL;
        if (!deadline_ns || soon < deadline_ns)
            deadline_ns = soon;
    }
    thread_block_until_ns(deadline_ns);
}

void multiwait_end(struct multiwait *m) {
    while (m->items) {
        struct mw_item *it = m->items;
        m->items = it->next;
        uint64_t fl = spin_lock_irqsave(&it->wq->lock);
        unlink_entry(it->wq, &it->e);
        spin_unlock_irqrestore(&it->wq->lock, fl);
        if (it->f)
            file_put(it->f);
        kfree(it);
    }
    m->added = m->unwaitable = false;
}

int waitq_wait_until(struct waitq *wq, bool (*cond)(void *), void *arg, uint64_t deadline_ns) {
    if (!deadline_ns)
        return waitq_wait_cond(wq, cond, arg);
    struct waitq_entry e = {.t = thread_current()};
    uint64_t f = spin_lock_irqsave(&wq->lock);
    e.next = wq->head;
    wq->head = &e;
    spin_unlock_irqrestore(&wq->lock, f);
    int r = 0;
    if (!cond(arg)) {
        if (signal_pending_current())
            r = -EINTR;
        else if (time_monotonic_ns() >= deadline_ns)
            r = -EAGAIN;
        else
            thread_block_until_ns(deadline_ns);
    }
    f = spin_lock_irqsave(&wq->lock);
    unlink_entry(wq, &e);
    spin_unlock_irqrestore(&wq->lock, f);
    return r;
}

void waitq_wake_all(struct waitq *wq) {
    uint64_t f = spin_lock_irqsave(&wq->lock);
    wq->seq++;
    for (struct waitq_entry *e = wq->head; e; e = e->next)
        thread_wake(e->t);
    spin_unlock_irqrestore(&wq->lock, f);
}
