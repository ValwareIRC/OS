#pragma once
/* kernel/linuxkpi: tasks, kernel threads, time and timers, work queues, as NVIDIA's glue sees them. `current` is a
 * Linux-shaped view of our kernel's own thread and process (kernel/linuxkpi/sched.c keeps one per thread that
 * asks for it). */
#include <lkpi/core.h>

#define HZ 1000
#define MAX_SCHEDULE_TIMEOUT 0x7fffffffffffffffL
#define TASK_RUNNING 0
#define TASK_INTERRUPTIBLE 1
#define TASK_UNINTERRUPTIBLE 2
#define TASK_KILLABLE TASK_UNINTERRUPTIBLE
#define TASK_COMM_LEN 16
#define NSEC_PER_SEC 1000000000L
#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_USEC 1000L
#define USEC_PER_SEC 1000000L
#define USEC_PER_MSEC 1000L
#define MSEC_PER_SEC 1000L
#define PF_KTHREAD 0x00200000
#define PF_EXITING 0x00000004
#define PF_MEMALLOC 0x00000800

struct mm_struct;
struct pid;
typedef struct { uid_t val; } kuid_t;
typedef struct { gid_t val; } kgid_t;
struct cred { kuid_t uid, euid; kgid_t gid, egid; };
struct signal_struct;
struct task_struct {
    volatile long state;
    unsigned int flags;
    pid_t pid, tgid;
    char comm[TASK_COMM_LEN];
    struct mm_struct *mm, *active_mm;
    const struct cred *cred;
    struct task_struct *group_leader;
    void *stack;      /* (its kernel stack: only compared, never read through) */
    void *fs;
    void *files;
    spinlock_t alloc_lock;
    void *lkpi_thread; /* our kernel's thread */
};
static inline void task_lock(struct task_struct *t) { spin_lock(&t->alloc_lock); }
static inline void task_unlock(struct task_struct *t) { spin_unlock(&t->alloc_lock); }
struct task_struct *get_current(void);
#define current get_current()
static inline pid_t task_pid_nr(struct task_struct *t) { return t->pid; }
static inline pid_t task_tgid_nr(struct task_struct *t) { return t->tgid; }
pid_t task_pid_vnr(struct task_struct *t);
pid_t pid_vnr(struct pid *p);
struct pid *get_task_pid(struct task_struct *t, int type);
void put_pid(struct pid *p);
struct task_struct *pid_task(struct pid *p, int type);
#define PIDTYPE_PID 0
#define PIDTYPE_TGID 1
static inline void get_task_struct(struct task_struct *t) { (void)t; }
static inline void put_task_struct(struct task_struct *t) { (void)t; }
#define current_uid() (current->cred->uid)
#define current_euid() (current->cred->euid)
bool capable(int cap);
#define CAP_SYS_ADMIN 21
#define CAP_SYS_RAWIO 17
bool signal_pending(struct task_struct *t);
bool fatal_signal_pending(struct task_struct *t);
void schedule(void);
long schedule_timeout(long timeout);
long schedule_timeout_interruptible(long timeout);
long schedule_timeout_uninterruptible(long timeout);
void set_current_state(long state);
#define __set_current_state set_current_state
int wake_up_process(struct task_struct *t);
void yield(void);
void cond_resched(void);
int smp_processor_id(void);
int raw_smp_processor_id(void);
int get_cpu(void);
void put_cpu(void);
extern unsigned int nr_cpu_ids;
int num_online_cpus(void);
int num_possible_cpus(void);
bool cpu_online(int cpu);
#define for_each_online_cpu(c) for ((c) = 0; (c) < num_online_cpus(); (c)++)
#define for_each_possible_cpu(c) for ((c) = 0; (c) < num_possible_cpus(); (c)++)
int numa_node_id(void);
#define NUMA_NO_NODE (-1)
#define nr_node_ids 1
#define num_online_nodes() 1
#define node_online(n) ((n) == 0)
#define for_each_online_node(n) for ((n) = 0; (n) < 1; (n)++)
#define cpu_to_node(c) 0
static inline int dev_to_node_lkpi(void) { return 0; }
void set_user_nice(struct task_struct *t, long nice);

/* kernel threads */
struct task_struct *kthread_create_on_node(int (*fn)(void *), void *data, int node, const char *namefmt, ...);
#define kthread_create(fn, data, fmt, ...) kthread_create_on_node(fn, data, NUMA_NO_NODE, fmt, ##__VA_ARGS__)
#define kthread_run(fn, data, fmt, ...) ({ struct task_struct *__k = kthread_create(fn, data, fmt, ##__VA_ARGS__); \
                                            if (!IS_ERR(__k)) wake_up_process(__k); __k; })
int kthread_stop(struct task_struct *t);
bool kthread_should_stop(void);
void kthread_bind(struct task_struct *t, unsigned int cpu);

/* time */
extern volatile unsigned long jiffies;
u64 get_jiffies_64(void);
static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }
static inline unsigned long usecs_to_jiffies(unsigned int u) { return (u + 999) / 1000; }
static inline unsigned int jiffies_to_msecs(unsigned long j) { return (unsigned int)j; }
static inline unsigned int jiffies_to_usecs(unsigned long j) { return (unsigned int)j * 1000; }
static inline u64 nsecs_to_jiffies(u64 n) { return n / NSEC_PER_MSEC; }
static inline u64 jiffies_to_nsecs(unsigned long j) { return (u64)j * NSEC_PER_MSEC; }
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#define time_is_after_jiffies(a) time_before(jiffies, a)
#define time_is_before_jiffies(a) time_after(jiffies, a)
struct timespec64 { time64_t tv_sec; long tv_nsec; };
struct timeval_lkpi { long tv_sec; long tv_usec; };
ktime_t ktime_get(void);
ktime_t ktime_get_raw(void);
ktime_t ktime_get_real(void);
ktime_t ktime_get_boottime(void);
u64 ktime_get_ns(void);
u64 ktime_get_raw_ns(void);
u64 ktime_get_real_ns(void);
u64 ktime_get_boottime_ns(void);
void ktime_get_ts64(struct timespec64 *ts);
void ktime_get_raw_ts64(struct timespec64 *ts);
void ktime_get_real_ts64(struct timespec64 *ts);
time64_t ktime_get_real_seconds(void);
static inline s64 ktime_to_ns(ktime_t t) { return t; }
static inline s64 ktime_to_us(ktime_t t) { return t / 1000; }
static inline s64 ktime_to_ms(ktime_t t) { return t / 1000000; }
static inline ktime_t ns_to_ktime(u64 n) { return (ktime_t)n; }
static inline ktime_t ktime_add_ns(ktime_t t, u64 n) { return t + (ktime_t)n; }
static inline ktime_t ktime_sub(ktime_t a, ktime_t b) { return a - b; }
static inline ktime_t ktime_add(ktime_t a, ktime_t b) { return a + b; }
static inline ktime_t ktime_set(s64 s, unsigned long ns) { return s * NSEC_PER_SEC + (s64)ns; }
static inline struct timespec64 ns_to_timespec64(s64 n) { struct timespec64 t = {n / NSEC_PER_SEC, n % NSEC_PER_SEC}; return t; }
static inline s64 timespec64_to_ns(const struct timespec64 *t) { return t->tv_sec * NSEC_PER_SEC + t->tv_nsec; }
#define ktime_compare(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)
static inline bool ktime_after(ktime_t a, ktime_t b) { return a > b; }
static inline bool ktime_before(ktime_t a, ktime_t b) { return a < b; }
static inline ktime_t ktime_add_ms(ktime_t t, u64 ms) { return t + (ktime_t)(ms * NSEC_PER_MSEC); }
static inline ktime_t ktime_add_us(ktime_t t, u64 us) { return t + (ktime_t)(us * NSEC_PER_USEC); }
static inline s64 ktime_us_delta(ktime_t a, ktime_t b) { return (a - b) / 1000; }
static inline s64 ktime_ms_delta(ktime_t a, ktime_t b) { return (a - b) / 1000000; }
void udelay(unsigned long us);
void ndelay(unsigned long ns);
void mdelay(unsigned long ms);
void msleep(unsigned int ms);
unsigned long msleep_interruptible(unsigned int ms);
void usleep_range(unsigned long min, unsigned long max);
void ssleep(unsigned int s);
void fsleep(unsigned long us);

/* timers (a timer's function runs on a thread of the layer's, never in an interrupt) */
struct timer_list { struct list_head entry; unsigned long expires; void (*function)(struct timer_list *); u32 flags; bool pending; };
void timer_setup(struct timer_list *t, void (*fn)(struct timer_list *), unsigned int flags);
int mod_timer(struct timer_list *t, unsigned long expires);
void add_timer(struct timer_list *t);
int del_timer(struct timer_list *t);
int del_timer_sync(struct timer_list *t);
#define timer_delete del_timer
#define timer_delete_sync del_timer_sync
#define from_timer(var, t, field) container_of(t, typeof(*var), field)
#define timer_container_of from_timer
static inline bool timer_pending(const struct timer_list *t) { return t->pending; }
enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
enum hrtimer_mode { HRTIMER_MODE_ABS = 0, HRTIMER_MODE_REL = 1, HRTIMER_MODE_PINNED = 2, HRTIMER_MODE_REL_PINNED = 3 };
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
struct hrtimer { struct list_head entry; ktime_t expires; enum hrtimer_restart (*function)(struct hrtimer *); bool pending; };
void hrtimer_init(struct hrtimer *t, int clock, enum hrtimer_mode mode);
void hrtimer_setup(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *), int clock, enum hrtimer_mode mode);
void hrtimer_start(struct hrtimer *t, ktime_t when, enum hrtimer_mode mode);
int hrtimer_cancel(struct hrtimer *t);
int hrtimer_try_to_cancel(struct hrtimer *t);
u64 hrtimer_forward_now(struct hrtimer *t, ktime_t interval);
static inline void hrtimer_set_expires(struct hrtimer *t, ktime_t e) { t->expires = e; }
static inline bool hrtimer_active(const struct hrtimer *t) { return t->pending; }

/* work queues (each a thread of the layer's) */
struct work_struct;
typedef void (*work_func_t)(struct work_struct *w);
struct work_struct { struct list_head entry; work_func_t func; volatile int pending; void *wq; };
struct delayed_work { struct work_struct work; struct timer_list timer; void *wq; };
struct workqueue_struct;
#define INIT_WORK(w, f) do { INIT_LIST_HEAD(&(w)->entry); (w)->func = (f); (w)->pending = 0; (w)->wq = NULL; } while (0)
#define INIT_DELAYED_WORK(d, f) do { INIT_WORK(&(d)->work, f); (d)->timer.pending = false; } while (0)
#define DECLARE_WORK(n, f) struct work_struct n = { LIST_HEAD_INIT((n).entry), (f), 0, NULL }
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
#define WQ_UNBOUND 0x2
#define WQ_MEM_RECLAIM 0x8
#define WQ_HIGHPRI 0x10
#define WQ_FREEZABLE 0x4
struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...);
#define alloc_ordered_workqueue(fmt, flags, ...) alloc_workqueue(fmt, (flags) | WQ_UNBOUND, 1, ##__VA_ARGS__)
#define create_singlethread_workqueue(name) alloc_ordered_workqueue("%s", 0, name)
void destroy_workqueue(struct workqueue_struct *wq);
bool queue_work(struct workqueue_struct *wq, struct work_struct *w);
bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *w);
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *d, unsigned long delay);
bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *d, unsigned long delay);
bool mod_delayed_work(struct workqueue_struct *wq, struct delayed_work *d, unsigned long delay);
bool schedule_work(struct work_struct *w);
bool schedule_delayed_work(struct delayed_work *d, unsigned long delay);
bool cancel_work_sync(struct work_struct *w);
bool cancel_delayed_work(struct delayed_work *d);
bool cancel_delayed_work_sync(struct delayed_work *d);
bool flush_work(struct work_struct *w);
void flush_workqueue(struct workqueue_struct *wq);
bool flush_delayed_work(struct delayed_work *d);
void flush_scheduled_work(void);
extern struct workqueue_struct *system_wq, *system_unbound_wq, *system_highpri_wq, *system_percpu_wq;
static inline bool work_pending(struct work_struct *w) { return w->pending; }
#define delayed_work_pending(d) work_pending(&(d)->work)

/* tasklets: run on a thread of the layer's soon after being scheduled */
struct tasklet_struct { struct list_head entry; void (*func)(unsigned long); unsigned long data; volatile int state; void (*callback)(struct tasklet_struct *); bool use_callback; };
void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data);
void tasklet_setup(struct tasklet_struct *t, void (*cb)(struct tasklet_struct *));
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_hi_schedule(struct tasklet_struct *t);
void tasklet_kill(struct tasklet_struct *t);
void tasklet_disable(struct tasklet_struct *t);
void tasklet_enable(struct tasklet_struct *t);

/* RCU (readers never block writers here: synchronize waits for a scheduling pass on every processor) */
#define rcu_read_lock() preempt_disable()
#define rcu_read_unlock() preempt_enable()
void synchronize_rcu(void);
struct rcu_head { struct rcu_head *next; void (*func)(struct rcu_head *); };
void call_rcu(struct rcu_head *h, void (*func)(struct rcu_head *));
#define rcu_dereference(p) READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_assign_pointer(p, v) smp_store_release(&(p), (v))
#define RCU_INIT_POINTER(p, v) WRITE_ONCE(p, v)
#define kfree_rcu(p, f) kfree(p)
void rcu_barrier(void);

/* per-processor data: one copy (the layer runs these on any processor, the driver only counts with it) */
#define DEFINE_PER_CPU(type, name) type name
#define DECLARE_PER_CPU(type, name) extern type name
#define per_cpu(var, cpu) (*((void)(cpu), &(var)))
#define per_cpu_ptr(p, cpu) ((void)(cpu), (p))
#define this_cpu_ptr(p) (p)
#define get_cpu_ptr(p) (p)
#define put_cpu_ptr(p) do { } while (0)
#define this_cpu_read(v) (v)
#define this_cpu_write(v, x) ((v) = (x))
#define this_cpu_inc(v) ((v)++)
#define alloc_percpu(type) ((type *)kzalloc(sizeof(type), 0))
#define free_percpu(p) kfree(p)
