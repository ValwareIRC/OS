#include <lkpi/all.h>
#include "../host.h"

/* kernel/linuxkpi: tasks (`current`), kernel threads, time and delays, timers, work queues, tasklets, RCU and
 * per-processor calls, on our scheduler. Timers, work and tasklets each run on a thread of the layer's, never in an
 * interrupt (the driver's are written to allow either). */

extern int lkpi_preempt_depth[256];
struct mm_struct *lkpi_mm_of_process(int pid); /* (mm.c: the Linux-shaped view of a program's memory) */

/* ---- tasks ---- */

struct pid { int nr; };
struct pid_namespace { int level; };
struct pid_namespace init_pid_ns;

struct lkpi_task {
    struct task_struct t;
    struct list_head all;
    int refs;                 /* the thread's own, and a kernel thread's creator's until kthread_stop */
    bool kthread, started;
    volatile bool should_stop;
    int (*fn)(void *);
    void *data;
    int result;
    struct completion go, done;
};

static const struct cred root_cred; /* (one person's computer: programs that may open the device may use it) */
static DEFINE_SPINLOCK(tasks_lock);
static LIST_HEAD(tasks);

static void task_put(struct lkpi_task *k) {
    spin_lock(&tasks_lock);
    bool last = !--k->refs;
    if (last)
        list_del(&k->all);
    spin_unlock(&tasks_lock);
    if (last)
        kfree(k);
}

static void thread_gone(void *slot) { task_put(slot); }

static struct lkpi_task *task_new(kpi_host_thread *th) {
    struct lkpi_task *k = kzalloc(sizeof *k, GFP_ATOMIC);
    if (!k)
        return NULL;
    struct task_struct *t = &k->t;
    t->state = TASK_RUNNING;
    t->tgid = kpi_host_pid();
    t->pid = t->tgid ? kpi_host_tid() : 0;
    t->flags = t->tgid ? 0 : PF_KTHREAD;
    strscpy(t->comm, kpi_host_comm(), sizeof t->comm);
    t->cred = &root_cred;
    t->group_leader = t;
    t->mm = t->active_mm = t->tgid ? lkpi_mm_of_process(t->tgid) : NULL;
    t->stack = k;
    spin_lock_init(&t->alloc_lock);
    t->lkpi_thread = th;
    k->refs = 1;
    init_completion(&k->go);
    init_completion(&k->done);
    spin_lock(&tasks_lock);
    list_add(&k->all, &tasks);
    spin_unlock(&tasks_lock);
    return k;
}

struct task_struct *get_current(void) {
    kpi_host_thread *th = kpi_host_thread_self();
    void **slot = kpi_host_thread_slot(th);
    if (!*slot) {
        static bool hooked;
        if (!hooked)
            kpi_host_on_thread_gone(thread_gone), hooked = true;
        *slot = task_new(th);
        if (!*slot)
            kpi_host_panic("linuxkpi: no memory for a task");
    }
    return &((struct lkpi_task *)*slot)->t;
}

static struct lkpi_task *task_of(struct task_struct *t) { return container_of(t, struct lkpi_task, t); }

pid_t task_pid_vnr(struct task_struct *t) { return t->pid; }
struct pid_namespace *task_active_pid_ns(struct task_struct *t) { return &init_pid_ns; }
pid_t __task_pid_nr_ns(struct task_struct *t, int type, struct pid_namespace *ns) { return type == PIDTYPE_PID ? t->pid : t->tgid; }

/* a thread group's leader answers for the group (the driver names its clients by it) */
struct pid *get_task_pid(struct task_struct *t, int type) {
    struct pid *p = kzalloc(sizeof *p, GFP_KERNEL);
    if (p)
        p->nr = type == PIDTYPE_TGID || t == t->group_leader ? t->tgid : t->pid;
    return p;
}

void put_pid(struct pid *p) { kfree(p); }
pid_t pid_vnr(struct pid *p) { return p ? p->nr : 0; }

/* (find_vpid's pid is borrowed, not counted: a few kept per number, reused round the table) */
struct pid *find_vpid(int nr) {
    static struct pid table[64];
    static int next;
    spin_lock(&tasks_lock);
    struct pid *p = NULL;
    for (int i = 0; i < 64 && !p; i++)
        if (table[i].nr == nr)
            p = &table[i];
    if (!p) {
        p = &table[next];
        next = (next + 1) % 64;
        p->nr = nr;
    }
    spin_unlock(&tasks_lock);
    return p;
}

struct task_struct *pid_task(struct pid *p, int type) {
    if (!p)
        return NULL;
    struct task_struct *found = NULL;
    spin_lock(&tasks_lock);
    struct lkpi_task *k;
    list_for_each_entry(k, &tasks, all)
        if (k->t.tgid && (type == PIDTYPE_TGID ? k->t.tgid == p->nr : k->t.pid == p->nr || k->t.tgid == p->nr)) {
            found = &k->t;
            break;
        }
    spin_unlock(&tasks_lock);
    return found;
}

bool capable(int cap) { return true; }
bool signal_pending(struct task_struct *t) { return t == current && kpi_host_signal_pending(); }
bool fatal_signal_pending(struct task_struct *t) { return signal_pending(t); }

/* set_current_state(TASK_INTERRUPTIBLE) ... schedule(): sleeps until wake_up_process (a wake that came in between is
 * kept by our scheduler, so schedule returns at once then) */
void set_current_state(long state) { WRITE_ONCE(current->state, state); }

void schedule(void) {
    struct task_struct *t = current;
    if (t->state != TASK_RUNNING)
        kpi_host_sleep_until(0);
    else
        kpi_host_yield();
    t->state = TASK_RUNNING;
}

long schedule_timeout(long timeout) {
    struct task_struct *t = current;
    if (timeout == MAX_SCHEDULE_TIMEOUT) {
        schedule();
        return timeout;
    }
    if (timeout < 0)
        timeout = 0;
    u64 now = kpi_host_now_ns(), end = now + jiffies_to_nsecs((unsigned long)timeout);
    if (t->state != TASK_RUNNING)
        kpi_host_sleep_until(end > now ? end : now + 1);
    else
        kpi_host_yield();
    t->state = TASK_RUNNING;
    now = kpi_host_now_ns();
    return now >= end ? 0 : (long)((end - now + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC);
}

long schedule_timeout_interruptible(long timeout) {
    set_current_state(TASK_INTERRUPTIBLE);
    return schedule_timeout(timeout);
}

long schedule_timeout_uninterruptible(long timeout) {
    set_current_state(TASK_UNINTERRUPTIBLE);
    return schedule_timeout(timeout);
}

int wake_up_process(struct task_struct *t) {
    struct lkpi_task *k = task_of(t);
    if (k->kthread && !k->started) {
        k->started = true;
        complete(&k->go);
        return 1;
    }
    kpi_host_wake(t->lkpi_thread);
    return 1;
}

void yield(void) { kpi_host_yield(); }
void cond_resched(void) { /* (our scheduler preempts kernel threads at its tick: nothing to give up here) */ }
void set_user_nice(struct task_struct *t, long nice) {}

/* ---- kernel threads ---- */

static void kthread_main(void *arg) {
    struct lkpi_task *k = arg;
    *kpi_host_thread_slot(kpi_host_thread_self()) = k;
    k->t.lkpi_thread = kpi_host_thread_self();
    wait_for_completion(&k->go);
    if (!k->should_stop)
        k->result = k->fn(k->data);
    else
        k->result = -EINTR;
    complete_all(&k->done);
}

struct task_struct *kthread_create_on_node(int (*fn)(void *), void *data, int node, const char *namefmt, ...) {
    struct lkpi_task *k = task_new(NULL);
    if (!k)
        return ERR_PTR(-ENOMEM);
    va_list ap;
    va_start(ap, namefmt);
    vsnprintf(k->t.comm, sizeof k->t.comm, namefmt, ap);
    va_end(ap);
    k->t.pid = k->t.tgid = 0;
    k->t.flags = PF_KTHREAD;
    k->t.mm = k->t.active_mm = NULL;
    k->kthread = true;
    k->fn = fn, k->data = data;
    k->refs = 2;
    if (!kpi_host_thread_start(k->t.comm, kthread_main, k)) {
        k->refs = 1;
        task_put(k);
        return ERR_PTR(-ENOMEM);
    }
    return &k->t;
}

int kthread_stop(struct task_struct *t) {
    struct lkpi_task *k = task_of(t);
    k->should_stop = true;
    if (!k->started)
        wake_up_process(t);
    else
        kpi_host_wake(t->lkpi_thread);
    wait_for_completion(&k->done);
    int r = k->result;
    task_put(k);
    return r;
}

bool kthread_should_stop(void) {
    struct lkpi_task *k = task_of(current);
    return k->kthread && k->should_stop;
}

void kthread_bind(struct task_struct *t, unsigned int cpu) {}

/* ---- processors ---- */

unsigned int nr_cpu_ids = 1;
int smp_processor_id(void) { return kpi_host_cpu(); }
int raw_smp_processor_id(void) { return kpi_host_cpu(); }

int get_cpu(void) {
    preempt_disable();
    return kpi_host_cpu();
}

void put_cpu(void) { preempt_enable(); }
int num_online_cpus(void) { return kpi_host_ncpus(); }
int num_possible_cpus(void) { return kpi_host_ncpus(); }
bool cpu_online(int cpu) { return cpu >= 0 && cpu < kpi_host_ncpus(); }
int numa_node_id(void) { return 0; }

void on_each_cpu(void (*fn)(void *info), void *info, int wait) { kpi_host_on_each_cpu(fn, info); }

struct cond_call { bool (*cond)(int, void *); void (*fn)(void *); void *info; };

static void cond_call_one(void *a) {
    struct cond_call *c = a;
    if (!c->cond || c->cond(kpi_host_cpu(), c->info))
        c->fn(c->info);
}

void on_each_cpu_cond_mask(bool (*cond)(int cpu, void *info), void (*fn)(void *info), void *info, bool wait, const void *mask) {
    struct cond_call c = {cond, fn, info};
    kpi_host_on_each_cpu(cond_call_one, &c);
}

/* ---- time ---- */

unsigned long lkpi_jiffies(void) { return (unsigned long)(kpi_host_now_ns() / NSEC_PER_MSEC); }
u64 get_jiffies_64(void) { return kpi_host_now_ns() / NSEC_PER_MSEC; }
ktime_t ktime_get(void) { return (ktime_t)kpi_host_now_ns(); }
ktime_t ktime_get_raw(void) { return (ktime_t)kpi_host_now_ns(); }
ktime_t ktime_get_real(void) { return (ktime_t)kpi_host_wall_ns(); }
ktime_t ktime_get_boottime(void) { return (ktime_t)kpi_host_now_ns(); }
u64 ktime_get_ns(void) { return kpi_host_now_ns(); }
u64 ktime_get_raw_ns(void) { return kpi_host_now_ns(); }
u64 ktime_get_real_ns(void) { return kpi_host_wall_ns(); }
u64 ktime_get_boottime_ns(void) { return kpi_host_now_ns(); }
void ktime_get_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64((s64)kpi_host_now_ns()); }
void ktime_get_raw_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64((s64)kpi_host_now_ns()); }
void ktime_get_real_ts64(struct timespec64 *ts) { *ts = ns_to_timespec64((s64)kpi_host_wall_ns()); }
time64_t ktime_get_real_seconds(void) { return (time64_t)(kpi_host_wall_ns() / NSEC_PER_SEC); }

void ndelay(unsigned long ns) { kpi_host_delay_ns(ns); }
void udelay(unsigned long us) { kpi_host_delay_ns((u64)us * NSEC_PER_USEC); }
void mdelay(unsigned long ms) { kpi_host_delay_ns((u64)ms * NSEC_PER_MSEC); }

/* sleeps to the end (early wakes just go round again); with `intr`, a signal ends it: what was left, in ns */
static u64 sleep_ns(u64 ns, bool intr) {
    u64 end = kpi_host_now_ns() + ns;
    for (;;) {
        u64 now = kpi_host_now_ns();
        if (now >= end)
            return 0;
        if (intr && kpi_host_signal_pending())
            return end - now;
        kpi_host_sleep_until(end);
    }
}

void msleep(unsigned int ms) { (void)sleep_ns((u64)ms * NSEC_PER_MSEC, false); }
unsigned long msleep_interruptible(unsigned int ms) { return (unsigned long)(sleep_ns((u64)ms * NSEC_PER_MSEC, true) / NSEC_PER_MSEC); }
void usleep_range(unsigned long min, unsigned long max) { (void)sleep_ns((u64)min * NSEC_PER_USEC, false); }
void ssleep(unsigned int s) { (void)sleep_ns((u64)s * NSEC_PER_SEC, false); }

void fsleep(unsigned long us) {
    if (us < 10)
        udelay(us);
    else
        (void)sleep_ns((u64)us * NSEC_PER_USEC, false);
}

/* ---- the layer's own threads, started on first use ---- */

static DEFINE_SPINLOCK(start_lock);
static void timers_start(void);
static void wqs_start(void);
static void tasklets_start(void);
static volatile bool started;

void lkpi_sched_init(void) {
    if (READ_ONCE(started))
        return;
    spin_lock(&start_lock);
    bool first = !started;
    started = true;
    spin_unlock(&start_lock);
    if (!first)
        return;
    nr_cpu_ids = (unsigned int)kpi_host_ncpus();
    timers_start();
    wqs_start();
    tasklets_start();
}

/* ---- timers (and high-resolution ones): both on one thread, earliest first ---- */

static DEFINE_SPINLOCK(timer_lock);
static LIST_HEAD(timers);
static LIST_HEAD(hrtimers);
static kpi_host_thread *timer_thread;
static void *volatile timer_running; /* the timer (either kind) whose function runs now */

static inline u64 timer_due_ns(const struct timer_list *t) { return jiffies_to_nsecs(t->expires); }

static void timer_main(void *arg) {
    for (;;) {
        spin_lock(&timer_lock);
        u64 now = kpi_host_now_ns(), next = 0;
        struct timer_list *due = NULL, *t;
        struct hrtimer *hdue = NULL, *h;
        list_for_each_entry(t, &timers, entry) {
            u64 at = timer_due_ns(t);
            if (at <= now) {
                due = t;
                break;
            }
            if (!next || at < next)
                next = at;
        }
        if (!due)
            list_for_each_entry(h, &hrtimers, entry) {
                u64 at = (u64)h->expires;
                if (at <= now) {
                    hdue = h;
                    break;
                }
                if (!next || at < next)
                    next = at;
            }
        if (due) {
            list_del_init(&due->entry);
            due->pending = false;
            timer_running = due;
            spin_unlock(&timer_lock);
            due->function(due);
            timer_running = NULL;
            continue;
        }
        if (hdue) {
            list_del_init(&hdue->entry);
            hdue->pending = false;
            timer_running = hdue;
            spin_unlock(&timer_lock);
            enum hrtimer_restart again = hdue->function(hdue);
            spin_lock(&timer_lock);
            if (again == HRTIMER_RESTART && !hdue->pending) {
                list_add_tail(&hdue->entry, &hrtimers);
                hdue->pending = true;
            }
            timer_running = NULL;
            spin_unlock(&timer_lock);
            continue;
        }
        spin_unlock(&timer_lock);
        kpi_host_sleep_until(next);
    }
}

static void timers_start(void) {
    timer_thread = kpi_host_thread_start("lkpi-timers", timer_main, NULL);
    if (!timer_thread)
        kpi_host_panic("linuxkpi: no timer thread");
}

static void timer_kick(void) {
    lkpi_sched_init();
    kpi_host_wake(timer_thread);
}

/* waits until the timer's function (if it is running) has returned; never on the timer thread itself */
static void timer_wait_idle(void *t) {
    while (READ_ONCE(timer_running) == t && kpi_host_thread_self() != timer_thread)
        kpi_host_sleep_until(kpi_host_now_ns() + 200 * NSEC_PER_USEC);
}

void timer_setup(struct timer_list *t, void (*fn)(struct timer_list *), unsigned int flags) {
    INIT_LIST_HEAD(&t->entry);
    t->function = fn;
    t->flags = flags;
    t->pending = false;
    t->expires = 0;
}

int mod_timer(struct timer_list *t, unsigned long expires) {
    spin_lock(&timer_lock);
    bool was = t->pending;
    if (was)
        list_del(&t->entry);
    t->expires = expires;
    list_add_tail(&t->entry, &timers);
    t->pending = true;
    spin_unlock(&timer_lock);
    timer_kick();
    return was;
}

void add_timer(struct timer_list *t) { (void)mod_timer(t, t->expires); }

int del_timer(struct timer_list *t) {
    spin_lock(&timer_lock);
    bool was = t->pending;
    if (was) {
        list_del_init(&t->entry);
        t->pending = false;
    }
    spin_unlock(&timer_lock);
    return was;
}

int del_timer_sync(struct timer_list *t) {
    int was = del_timer(t);
    timer_wait_idle(t);
    return was;
}

void hrtimer_init(struct hrtimer *t, int clock, enum hrtimer_mode mode) {
    INIT_LIST_HEAD(&t->entry);
    t->pending = false;
    t->expires = 0;
}

void hrtimer_setup(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *), int clock, enum hrtimer_mode mode) {
    hrtimer_init(t, clock, mode);
    t->function = fn;
}

void hrtimer_start(struct hrtimer *t, ktime_t when, enum hrtimer_mode mode) {
    if (mode & HRTIMER_MODE_REL)
        when += (ktime_t)kpi_host_now_ns();
    spin_lock(&timer_lock);
    if (t->pending)
        list_del(&t->entry);
    t->expires = when;
    list_add_tail(&t->entry, &hrtimers);
    t->pending = true;
    spin_unlock(&timer_lock);
    timer_kick();
}

int hrtimer_try_to_cancel(struct hrtimer *t) {
    spin_lock(&timer_lock);
    int r = READ_ONCE(timer_running) == t ? -1 : t->pending;
    if (t->pending) {
        list_del_init(&t->entry);
        t->pending = false;
    }
    spin_unlock(&timer_lock);
    return r;
}

int hrtimer_cancel(struct hrtimer *t) {
    for (;;) {
        int r = hrtimer_try_to_cancel(t);
        if (r >= 0)
            return r;
        timer_wait_idle(t);
    }
}

u64 hrtimer_forward_now(struct hrtimer *t, ktime_t interval) {
    ktime_t now = (ktime_t)kpi_host_now_ns();
    if (interval <= 0 || t->expires > now)
        return 0;
    u64 n = (u64)((now - t->expires) / interval) + 1;
    t->expires += (ktime_t)n * interval;
    return n;
}

/* ---- work queues: a thread each, its items in order ---- */

struct workqueue_struct {
    char name[32];
    spinlock_t lock;
    struct list_head items;
    kpi_host_thread *thread;
    struct work_struct *volatile running;
    wait_queue_head_t idle; /* woken after each item */
    volatile bool dying;
    struct completion gone;
};

struct workqueue_struct *system_wq, *system_unbound_wq, *system_highpri_wq, *system_percpu_wq;

static void wq_main(void *arg) {
    struct workqueue_struct *wq = arg;
    for (;;) {
        spin_lock(&wq->lock);
        if (!list_empty(&wq->items)) {
            struct work_struct *w = list_first_entry(&wq->items, struct work_struct, entry);
            list_del_init(&w->entry);
            wq->running = w;
            WRITE_ONCE(w->pending, 0); /* (it may be queued again while it runs, as in Linux) */
            spin_unlock(&wq->lock);
            w->func(w);
            wq->running = NULL;
            wake_up_all(&wq->idle);
            continue;
        }
        bool dying = wq->dying;
        spin_unlock(&wq->lock);
        if (dying)
            break;
        kpi_host_sleep_until(0);
    }
    complete_all(&wq->gone);
}

static struct workqueue_struct *wq_new(const char *name) {
    struct workqueue_struct *wq = kzalloc(sizeof *wq, GFP_KERNEL);
    if (!wq)
        return NULL;
    strscpy(wq->name, name, sizeof wq->name);
    spin_lock_init(&wq->lock);
    INIT_LIST_HEAD(&wq->items);
    init_waitqueue_head(&wq->idle);
    init_completion(&wq->gone);
    wq->thread = kpi_host_thread_start(wq->name, wq_main, wq);
    if (!wq->thread) {
        kfree(wq);
        return NULL;
    }
    return wq;
}

static void wqs_start(void) {
    system_wq = wq_new("lkpi-events");
    system_unbound_wq = wq_new("lkpi-unbound");
    system_highpri_wq = wq_new("lkpi-highpri");
    if (!system_wq || !system_unbound_wq || !system_highpri_wq)
        kpi_host_panic("linuxkpi: no work queue threads");
    system_percpu_wq = system_wq;
}

static struct workqueue_struct *wq_or_system(struct workqueue_struct *wq) {
    lkpi_sched_init();
    return wq ? wq : system_wq;
}

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...) {
    lkpi_sched_init();
    char name[32];
    va_list ap;
    va_start(ap, max_active);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    return wq_new(name);
}

void flush_workqueue(struct workqueue_struct *wq) {
    wq = wq_or_system(wq);
    if (kpi_host_thread_self() == wq->thread)
        return;
    wait_event(wq->idle, list_empty(&wq->items) && !wq->running);
}

void destroy_workqueue(struct workqueue_struct *wq) {
    if (!wq)
        return;
    flush_workqueue(wq);
    spin_lock(&wq->lock);
    wq->dying = true;
    spin_unlock(&wq->lock);
    kpi_host_wake(wq->thread);
    wait_for_completion(&wq->gone);
    kfree(wq);
}

static void wq_add(struct workqueue_struct *wq, struct work_struct *w) {
    spin_lock(&wq->lock);
    w->wq = wq;
    list_add_tail(&w->entry, &wq->items);
    spin_unlock(&wq->lock);
    kpi_host_wake(wq->thread);
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *w) {
    wq = wq_or_system(wq);
    if (__atomic_exchange_n(&w->pending, 1, __ATOMIC_ACQ_REL))
        return false;
    wq_add(wq, w);
    return true;
}

bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *w) { return queue_work(wq, w); }
bool schedule_work(struct work_struct *w) { return queue_work(NULL, w); }

/* waits until w is neither queued nor running (true: it had something to wait for) */
bool flush_work(struct work_struct *w) {
    struct workqueue_struct *wq = w->wq;
    if (!wq || kpi_host_thread_self() == wq->thread)
        return false;
    bool busy = READ_ONCE(w->pending) || wq->running == w;
    wait_event(wq->idle, !READ_ONCE(w->pending) && wq->running != w);
    return busy;
}

static bool unqueue(struct work_struct *w) {
    struct workqueue_struct *wq = w->wq;
    if (!wq)
        return false;
    spin_lock(&wq->lock);
    bool queued = !list_empty(&w->entry);
    if (queued) {
        list_del_init(&w->entry);
        WRITE_ONCE(w->pending, 0);
    }
    spin_unlock(&wq->lock);
    return queued;
}

bool cancel_work_sync(struct work_struct *w) {
    bool was = unqueue(w);
    flush_work(w);
    return was;
}

static void delayed_fire(struct timer_list *t) {
    struct delayed_work *d = container_of(t, struct delayed_work, timer);
    wq_add(d->wq, &d->work);
}

bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *d, unsigned long delay) {
    wq = wq_or_system(wq);
    if (__atomic_exchange_n(&d->work.pending, 1, __ATOMIC_ACQ_REL))
        return false;
    d->wq = wq;
    d->work.wq = wq;
    if (!delay) {
        wq_add(wq, &d->work);
        return true;
    }
    timer_setup(&d->timer, delayed_fire, 0);
    mod_timer(&d->timer, lkpi_jiffies() + delay);
    return true;
}

bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *d, unsigned long delay) {
    return queue_delayed_work(wq, d, delay);
}

bool schedule_delayed_work(struct delayed_work *d, unsigned long delay) { return queue_delayed_work(NULL, d, delay); }

bool cancel_delayed_work(struct delayed_work *d) {
    if (d->timer.function == delayed_fire && del_timer(&d->timer)) {
        WRITE_ONCE(d->work.pending, 0);
        return true;
    }
    return unqueue(&d->work);
}

bool cancel_delayed_work_sync(struct delayed_work *d) {
    bool was = cancel_delayed_work(d);
    if (d->timer.function == delayed_fire)
        del_timer_sync(&d->timer); /* (fired just now: then it is queued, and unqueued or waited for below) */
    was |= unqueue(&d->work);
    flush_work(&d->work);
    return was;
}

bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *d, unsigned long delay) {
    bool was = cancel_delayed_work(d);
    queue_delayed_work(wq, d, delay);
    return was;
}

bool flush_delayed_work(struct delayed_work *d) {
    if (d->timer.function == delayed_fire && del_timer(&d->timer))
        wq_add(d->wq, &d->work);
    return flush_work(&d->work);
}

void flush_scheduled_work(void) { flush_workqueue(NULL); }

/* ---- tasklets: one thread for all; state bit 0 scheduled, bit 1 running, the rest a disable count ---- */

#define TL_SCHED 1
#define TL_RUN 2
#define TL_DISABLE 4

static DEFINE_SPINLOCK(tasklet_lock);
static LIST_HEAD(tasklets);
static kpi_host_thread *tasklet_thread;

static void tasklet_main(void *arg) {
    for (;;) {
        spin_lock(&tasklet_lock);
        struct tasklet_struct *t, *run = NULL;
        list_for_each_entry(t, &tasklets, entry)
            if (READ_ONCE(t->state) < TL_DISABLE) {
                run = t;
                break;
            }
        if (!run) {
            spin_unlock(&tasklet_lock);
            kpi_host_sleep_until(0);
            continue;
        }
        list_del_init(&run->entry);
        __atomic_or_fetch(&run->state, TL_RUN, __ATOMIC_ACQ_REL);
        __atomic_and_fetch(&run->state, ~TL_SCHED, __ATOMIC_ACQ_REL);
        spin_unlock(&tasklet_lock);
        if (run->use_callback)
            run->callback(run);
        else
            run->func(run->data);
        __atomic_and_fetch(&run->state, ~TL_RUN, __ATOMIC_ACQ_REL);
    }
}

static void tasklets_start(void) {
    tasklet_thread = kpi_host_thread_start("lkpi-tasklets", tasklet_main, NULL);
    if (!tasklet_thread)
        kpi_host_panic("linuxkpi: no tasklet thread");
}

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data) {
    INIT_LIST_HEAD(&t->entry);
    t->func = func, t->data = data, t->state = 0, t->use_callback = false, t->callback = NULL;
}

void tasklet_setup(struct tasklet_struct *t, void (*cb)(struct tasklet_struct *)) {
    INIT_LIST_HEAD(&t->entry);
    t->callback = cb, t->use_callback = true, t->state = 0, t->func = NULL, t->data = 0;
}

void tasklet_schedule(struct tasklet_struct *t) {
    lkpi_sched_init();
    if (__atomic_fetch_or(&t->state, TL_SCHED, __ATOMIC_ACQ_REL) & TL_SCHED)
        return;
    spin_lock(&tasklet_lock);
    list_add_tail(&t->entry, &tasklets);
    spin_unlock(&tasklet_lock);
    kpi_host_wake(tasklet_thread);
}

void tasklet_hi_schedule(struct tasklet_struct *t) { tasklet_schedule(t); }

static void tasklet_wait_not_running(struct tasklet_struct *t) {
    while ((READ_ONCE(t->state) & TL_RUN) && kpi_host_thread_self() != tasklet_thread)
        kpi_host_sleep_until(kpi_host_now_ns() + 200 * NSEC_PER_USEC);
}

void tasklet_kill(struct tasklet_struct *t) {
    spin_lock(&tasklet_lock);
    if (READ_ONCE(t->state) & TL_SCHED) {
        list_del_init(&t->entry);
        __atomic_and_fetch(&t->state, ~TL_SCHED, __ATOMIC_ACQ_REL);
    }
    spin_unlock(&tasklet_lock);
    tasklet_wait_not_running(t);
}

void tasklet_disable(struct tasklet_struct *t) {
    __atomic_add_fetch(&t->state, TL_DISABLE, __ATOMIC_ACQ_REL);
    tasklet_wait_not_running(t);
}

void tasklet_enable(struct tasklet_struct *t) {
    if (__atomic_sub_fetch(&t->state, TL_DISABLE, __ATOMIC_ACQ_REL) & TL_SCHED)
        kpi_host_wake(tasklet_thread);
}

/* ---- RCU: readers hold off preemption (rcu_read_lock), so once every other processor has been seen out of its
 * read side, every reader that began before is done ---- */

void synchronize_rcu(void) {
    int me = kpi_host_cpu(), n = kpi_host_ncpus();
    for (int c = 0; c < n && c < 256; c++)
        while (c != me && READ_ONCE(lkpi_preempt_depth[c]) > 0)
            kpi_host_yield();
}

static DEFINE_SPINLOCK(rcu_lock);
static struct rcu_head *rcu_pending;
static void rcu_work_fn(struct work_struct *w);
static DECLARE_WORK(rcu_work, rcu_work_fn);

static void rcu_work_fn(struct work_struct *w) {
    spin_lock(&rcu_lock);
    struct rcu_head *list = rcu_pending;
    rcu_pending = NULL;
    spin_unlock(&rcu_lock);
    if (!list)
        return;
    synchronize_rcu();
    while (list) {
        struct rcu_head *next = list->next;
        list->func(list);
        list = next;
    }
}

void call_rcu(struct rcu_head *h, void (*func)(struct rcu_head *)) {
    h->func = func;
    spin_lock(&rcu_lock);
    h->next = rcu_pending;
    rcu_pending = h;
    spin_unlock(&rcu_lock);
    schedule_work(&rcu_work);
}

void rcu_barrier(void) {
    for (;;) {
        flush_work(&rcu_work);
        spin_lock(&rcu_lock);
        bool none = !rcu_pending;
        spin_unlock(&rcu_lock);
        if (none && !READ_ONCE(rcu_work.pending))
            return;
        schedule_work(&rcu_work);
    }
}
