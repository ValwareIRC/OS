#include <kernel/lib.h>
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

void waitq_wake_all(struct waitq *wq) {
    uint64_t f = spin_lock_irqsave(&wq->lock);
    wq->seq++;
    for (struct waitq_entry *e = wq->head; e; e = e->next)
        thread_wake(e->t);
    spin_unlock_irqrestore(&wq->lock, f);
}
