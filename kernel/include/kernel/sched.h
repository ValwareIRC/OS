#pragma once
#include <stdbool.h>
#include <stdint.h>

enum thread_state { THREAD_READY, THREAD_RUNNING, THREAD_SLEEPING, THREAD_BLOCKED, THREAD_DEAD };

struct thread {
    uint64_t rsp; /* saved kernel stack pointer; must stay first (switch.S) */
    uint64_t id;
    enum thread_state state;
    int last_cpu;
    uint8_t *kstack;
    uint64_t kstack_size;
    uint64_t wake_ns; /* sleeping: when it wakes (time_monotonic_ns) */
    int sleep_cpu;    /* sleeping: whose list it is on (each CPU keeps its own sleepers and arms its own timer) */
    bool wake_pending;
    void (*entry)(void *);
    void *arg;
    struct thread *next; /* run queue / sleep list link */
    struct user_thread *user; /* NULL for kernel-only threads */
    uint64_t switch_reserve;  /* bytes kept free at the stack top (user frame) */
    char name[32];
};

/* Called once per CPU from that CPU's boot context, which becomes its idle
 * thread. */
void sched_init_cpu(void);

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);
/* Like thread_create, but keeps `reserve` bytes at the top of the kernel
 * stack (for a user register frame) and attaches user state. The thread is
 * not started until thread_start_ready(). */
struct thread *thread_create_ex(const char *name, void (*entry)(void *), void *arg, uint64_t reserve,
                                struct user_thread *user);
void thread_start_ready(struct thread *t);
static inline uint64_t thread_kstack_top(struct thread *t) { return (uint64_t)t->kstack + t->kstack_size; }
__attribute__((noreturn)) void thread_exit(void);
void thread_yield(void);
void thread_sleep_ms(uint64_t ms);
void thread_sleep_ns(uint64_t ns); /* to the nanosecond the timer allows (x86: the local APIC, one-shot) */
struct thread *thread_current(void);

/* Blocking primitive for wait queues: block_current() parks the running
 * thread; thread_wake() makes it runnable again. */
void thread_block_current(void);
/* Blocks until thread_wake() or until the monotonic clock reaches `deadline_ns` (0: no timeout). Either way the
 * caller re-checks what it waits for. */
void thread_block_until_ns(uint64_t deadline_ns);
void thread_wake(struct thread *t);

/* Timer interrupt hooks: sched_timer wakes this CPU's sleepers that are due (counting them in *woken) and returns
 * the earliest deadline still waiting (UINT64_MAX: none); sched_tick is the scheduler's tick (HZ a second). */
uint64_t sched_timer(uint64_t now_ns, int *woken);
void sched_tick(void);
