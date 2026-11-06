#pragma once
/* kernel/linuxkpi: the core of the Linux-shaped layer NVIDIA's open driver compiles against (docs/nvidia/plan.md).
 * Written here from what the driver uses, with Linux's names and meanings; the functions are implemented on our
 * kernel's own (the files of kernel/linuxkpi/src). Linux's many small headers (linux/types.h, linux/list.h, linux/mutex.h...)
 * each include this, so a file gets the same view whichever it names. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- types ---- */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef u8 __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s8 __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;
typedef u16 __le16, __be16;
typedef u32 __le32, __be32;
typedef u64 __le64, __be64;
typedef long ssize_t;
typedef long long loff_t;
typedef int pid_t;
typedef unsigned int uid_t, gid_t;
typedef unsigned short umode_t;
typedef u32 dev_t;
typedef u64 phys_addr_t;
typedef u64 resource_size_t;
typedef u64 dma_addr_t;
typedef unsigned int gfp_t;
typedef unsigned int fmode_t;
typedef unsigned long uintptr_t_linux;
typedef long long time64_t;
typedef s64 ktime_t;
typedef unsigned long pgoff_t;
typedef unsigned long irq_hw_number_t;
typedef int irqreturn_t;
typedef struct { int counter; } atomic_t;
typedef struct { s64 counter; } atomic64_t;
typedef atomic64_t atomic_long_t;
typedef struct { unsigned int refs; } refcount_struct_t;

#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQ_WAKE_THREAD 2

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---- the compiler ---- */
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define barrier() __asm__ __volatile__("" ::: "memory")
#define READ_ONCE(x) (*(const volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile typeof(x) *)&(x) = (v))
#define ACCESS_ONCE(x) READ_ONCE(x)
#define smp_mb() __asm__ __volatile__("mfence" ::: "memory")
#define smp_rmb() barrier()
#define smp_wmb() barrier()
#define mb() smp_mb()
#define rmb() __asm__ __volatile__("lfence" ::: "memory")
#define wmb() __asm__ __volatile__("sfence" ::: "memory")
#define dma_rmb() barrier()
#define dma_wmb() barrier()
#define smp_store_release(p, v) do { barrier(); WRITE_ONCE(*(p), (v)); } while (0)
#define smp_load_acquire(p) ({ typeof(*(p)) ___v = READ_ONCE(*(p)); barrier(); ___v; })
#define smp_mb__before_atomic() barrier()
#define smp_mb__after_atomic() barrier()
#define cpu_relax() __asm__ __volatile__("pause" ::: "memory")
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON: " #c)
#define BUILD_BUG_ON_MSG(c, m) _Static_assert(!(c), m)
#define BUILD_BUG_ON_ZERO(e) ((int)(sizeof(struct { int : (-!!(e)); })))
#define __must_be_array(a) 0
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define __stringify_1(x) #x
#define __stringify(x) __stringify_1(x)
#define EXPORT_SYMBOL(s)
#define EXPORT_SYMBOL_GPL(s)
#define EXPORT_SYMBOL_NS(s, ns)
#define EXPORT_SYMBOL_NS_GPL(s, ns)

/* ---- arithmetic ---- */
#define min(a, b) ({ typeof(a) _a = (a); typeof(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ typeof(a) _a = (a); typeof(b) _b = (b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ({ t _a = (a); t _b = (b); _a < _b ? _a : _b; })
#define max_t(t, a, b) ({ t _a = (a); t _b = (b); _a > _b ? _a : _b; })
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define clamp_t(t, v, lo, hi) min_t(t, max_t(t, v, lo), hi)
#define swap(a, b) do { typeof(a) _t = (a); (a) = (b); (b) = _t; } while (0)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_UP_ULL(n, d) DIV_ROUND_UP((unsigned long long)(n), (d))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) ((x) - ((x) % (y)))
#define round_up(x, y) ((((x) - 1) | ((typeof(x))(y) - 1)) + 1)
#define round_down(x, y) ((x) & ~((typeof(x))(y) - 1))
#define ALIGN(x, a) (((x) + ((typeof(x))(a) - 1)) & ~((typeof(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((typeof(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((typeof(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a) ((typeof(p))ALIGN((unsigned long)(p), (a)))
#define upper_32_bits(n) ((u32)(((u64)(n)) >> 32))
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))
#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define BITS_PER_LONG 64
#define BITS_PER_BYTE 8
#define BITS_TO_LONGS(n) DIV_ROUND_UP(n, BITS_PER_LONG)
#define U8_MAX 0xff
#define U16_MAX 0xffff
#define U32_MAX 0xffffffffu
#define U64_MAX 0xffffffffffffffffull
#define S32_MAX 0x7fffffff
#define S64_MAX 0x7fffffffffffffffll
#ifndef INT_MAX
#define INT_MAX 0x7fffffff
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX 0xffffffffu
#define LONG_MAX 0x7fffffffffffffffl
#define ULONG_MAX 0xfffffffffffffffful
#endif
static inline u64 div_u64(u64 n, u32 d) { return n / d; }
static inline s64 div_s64(s64 n, s32 d) { return n / d; }
static inline u64 div64_u64(u64 n, u64 d) { return n / d; }
static inline u64 div_u64_rem(u64 n, u32 d, u32 *rem) { *rem = (u32)(n % d); return n / d; }
#define do_div(n, base) ({ u32 __r = (u32)((n) % (base)); (n) /= (base); __r; })
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned long __ffs(unsigned long x) { return (unsigned long)__builtin_ctzl(x); }
static inline int ffs(int x) { return __builtin_ffs(x); }
static inline unsigned long ffz(unsigned long x) { return __ffs(~x); }
static inline int ilog2_u64(u64 n) { return fls64(n) - 1; }
#define ilog2(n) ilog2_u64((u64)(n))
#define is_power_of_2(n) ((n) != 0 && (((n) & ((n) - 1)) == 0))
static inline unsigned long roundup_pow_of_two(unsigned long n) { return n <= 1 ? 1 : 1UL << fls64(n - 1); }
static inline unsigned long rounddown_pow_of_two(unsigned long n) { return n ? 1UL << (fls64(n) - 1) : 0; }
#define hweight32(x) __builtin_popcount(x)
#define hweight64(x) __builtin_popcountll(x)
#define hweight_long(x) __builtin_popcountl(x)

/* ---- errors in pointers ---- */
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
static inline void *ERR_PTR(long e) { return (void *)e; }
static inline long PTR_ERR(const void *p) { return (long)p; }
static inline bool IS_ERR(const void *p) { return IS_ERR_VALUE((unsigned long)p); }
static inline bool IS_ERR_OR_NULL(const void *p) { return !p || IS_ERR(p); }
static inline int PTR_ERR_OR_ZERO(const void *p) { return IS_ERR(p) ? (int)PTR_ERR(p) : 0; }
#define ERR_CAST(p) ((void *)(p))

/* ---- printing and assertions ---- */
int printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintk(const char *fmt, va_list ap);
#define KERN_SOH "\001"
#define KERN_EMERG KERN_SOH "0"
#define KERN_ALERT KERN_SOH "1"
#define KERN_CRIT KERN_SOH "2"
#define KERN_ERR KERN_SOH "3"
#define KERN_WARNING KERN_SOH "4"
#define KERN_NOTICE KERN_SOH "5"
#define KERN_INFO KERN_SOH "6"
#define KERN_DEBUG KERN_SOH "7"
#define KERN_DEFAULT ""
#define KERN_CONT KERN_SOH "c"
#define pr_err(fmt, ...) printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) do { } while (0)
#define pr_cont(fmt, ...) printk(KERN_CONT fmt, ##__VA_ARGS__)
#define printk_ratelimited printk
void lkpi_bug(const char *file, int line) __attribute__((noreturn));
void lkpi_warn(const char *file, int line, const char *what);
#define BUG() lkpi_bug(__FILE__, __LINE__)
#define BUG_ON(c) do { if (unlikely(c)) BUG(); } while (0)
#define WARN_ON(c) ({ bool __w = !!(c); if (unlikely(__w)) lkpi_warn(__FILE__, __LINE__, #c); unlikely(__w); })
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN(c, fmt, ...) ({ bool __w = !!(c); if (unlikely(__w)) { lkpi_warn(__FILE__, __LINE__, #c); printk(fmt, ##__VA_ARGS__); } unlikely(__w); })
#define WARN_ONCE(c, fmt, ...) WARN(c, fmt, ##__VA_ARGS__)
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void dump_stack(void);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int scnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sscanf(const char *s, const char *fmt, ...);
char *kasprintf(gfp_t gfp, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap);
int kstrtoul(const char *s, unsigned int base, unsigned long *res);
int kstrtouint(const char *s, unsigned int base, unsigned int *res);
int kstrtoint(const char *s, unsigned int base, int *res);
unsigned long simple_strtoul(const char *s, char **end, unsigned int base);

/* ---- strings and memory ---- */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
ssize_t strscpy(char *d, const char *s, size_t n);
ssize_t sized_strscpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strsep(char **s, const char *delim);
char *strim(char *s);
char *skip_spaces(const char *s);
char *kstrdup(const char *s, gfp_t gfp);
char *kstrndup(const char *s, size_t n, gfp_t gfp);
void *kmemdup(const void *s, size_t n, gfp_t gfp);
int sysfs_streq(const char *a, const char *b);

/* ---- atomics ---- */
#define ATOMIC_INIT(i) { (i) }
static inline int atomic_read(const atomic_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); }
static inline void atomic_set(atomic_t *v, int i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); }
static inline void atomic_add(int i, atomic_t *v) { __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_sub(int i, atomic_t *v) { __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic_inc(atomic_t *v) { atomic_add(1, v); }
static inline void atomic_dec(atomic_t *v) { atomic_sub(1, v); }
static inline int atomic_add_return(int i, atomic_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_sub_return(int i, atomic_t *v) { return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_inc_return(atomic_t *v) { return atomic_add_return(1, v); }
static inline int atomic_dec_return(atomic_t *v) { return atomic_sub_return(1, v); }
static inline bool atomic_dec_and_test(atomic_t *v) { return atomic_dec_return(v) == 0; }
static inline bool atomic_inc_and_test(atomic_t *v) { return atomic_inc_return(v) == 0; }
static inline int atomic_cmpxchg(atomic_t *v, int old, int new_)
{ __atomic_compare_exchange_n(&v->counter, &old, new_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return old; }
static inline int atomic_xchg(atomic_t *v, int i) { return __atomic_exchange_n(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline int atomic_fetch_add(int i, atomic_t *v) { return __atomic_fetch_add(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline bool atomic_add_unless(atomic_t *v, int a, int u)
{ int c = atomic_read(v); while (c != u) { int o = atomic_cmpxchg(v, c, c + a); if (o == c) return true; c = o; } return false; }
#define atomic_inc_not_zero(v) atomic_add_unless((v), 1, 0)
static inline s64 atomic64_read(const atomic64_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_RELAXED); }
static inline void atomic64_set(atomic64_t *v, s64 i) { __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED); }
static inline void atomic64_add(s64 i, atomic64_t *v) { __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_sub(s64 i, atomic64_t *v) { __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline void atomic64_inc(atomic64_t *v) { atomic64_add(1, v); }
static inline void atomic64_dec(atomic64_t *v) { atomic64_sub(1, v); }
static inline s64 atomic64_add_return(s64 i, atomic64_t *v) { return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_inc_return(atomic64_t *v) { return atomic64_add_return(1, v); }
static inline s64 atomic64_dec_return(atomic64_t *v) { return __atomic_sub_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_cmpxchg(atomic64_t *v, s64 old, s64 new_)
{ __atomic_compare_exchange_n(&v->counter, &old, new_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return old; }
static inline bool atomic64_dec_and_test(atomic64_t *v) { return atomic64_dec_return(v) == 0; }
static inline s64 atomic64_xchg(atomic64_t *v, s64 i) { return __atomic_exchange_n(&v->counter, i, __ATOMIC_SEQ_CST); }
#define atomic_long_read atomic64_read
#define atomic_long_set atomic64_set
#define atomic_long_inc atomic64_inc
#define atomic_long_dec atomic64_dec
#define atomic_long_add atomic64_add
#define atomic_long_sub atomic64_sub
#define cmpxchg(p, o, n) ({ typeof(*(p)) __o = (o); __atomic_compare_exchange_n((p), &__o, (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); __o; })
#define xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
static inline void set_bit(long nr, volatile unsigned long *a) { __atomic_or_fetch(&a[nr / 64], 1UL << (nr % 64), __ATOMIC_SEQ_CST); }
static inline void clear_bit(long nr, volatile unsigned long *a) { __atomic_and_fetch(&a[nr / 64], ~(1UL << (nr % 64)), __ATOMIC_SEQ_CST); }
static inline bool test_bit(long nr, const volatile unsigned long *a) { return (a[nr / 64] >> (nr % 64)) & 1; }
static inline bool test_and_set_bit(long nr, volatile unsigned long *a)
{ return (__atomic_fetch_or(&a[nr / 64], 1UL << (nr % 64), __ATOMIC_SEQ_CST) >> (nr % 64)) & 1; }
static inline bool test_and_clear_bit(long nr, volatile unsigned long *a)
{ return (__atomic_fetch_and(&a[nr / 64], ~(1UL << (nr % 64)), __ATOMIC_SEQ_CST) >> (nr % 64)) & 1; }
#define __set_bit set_bit
#define __clear_bit clear_bit

/* ---- reference counts ---- */
typedef struct { atomic_t refs; } refcount_t;
#define REFCOUNT_INIT(n) { .refs = ATOMIC_INIT(n) }
static inline void refcount_set(refcount_t *r, int n) { atomic_set(&r->refs, n); }
static inline unsigned int refcount_read(const refcount_t *r) { return (unsigned int)atomic_read(&r->refs); }
static inline void refcount_inc(refcount_t *r) { atomic_inc(&r->refs); }
static inline bool refcount_dec_and_test(refcount_t *r) { return atomic_dec_and_test(&r->refs); }
static inline bool refcount_inc_not_zero(refcount_t *r) { return atomic_inc_not_zero(&r->refs); }
struct kref { refcount_t refcount; };
static inline void kref_init(struct kref *k) { refcount_set(&k->refcount, 1); }
static inline void kref_get(struct kref *k) { refcount_inc(&k->refcount); }
static inline int kref_put(struct kref *k, void (*release)(struct kref *))
{ if (refcount_dec_and_test(&k->refcount)) { release(k); return 1; } return 0; }

/* ---- lists ---- */
struct list_head { struct list_head *next, *prev; };
struct hlist_head { struct hlist_node *first; };
struct hlist_node { struct hlist_node *next, **pprev; };
#define LIST_HEAD_INIT(n) { &(n), &(n) }
#define LIST_HEAD(n) struct list_head n = LIST_HEAD_INIT(n)
static inline void INIT_LIST_HEAD(struct list_head *l) { l->next = l->prev = l; }
static inline void __list_add(struct list_head *n, struct list_head *p, struct list_head *x)
{ x->prev = n; n->next = x; n->prev = p; p->next = n; }
static inline void list_add(struct list_head *n, struct list_head *h) { __list_add(n, h, h->next); }
static inline void list_add_tail(struct list_head *n, struct list_head *h) { __list_add(n, h->prev, h); }
static inline void __list_del(struct list_head *p, struct list_head *x) { x->prev = p; p->next = x; }
static inline void list_del(struct list_head *e) { __list_del(e->prev, e->next); e->next = e->prev = NULL; }
static inline void list_del_init(struct list_head *e) { __list_del(e->prev, e->next); INIT_LIST_HEAD(e); }
static inline void list_move(struct list_head *e, struct list_head *h) { __list_del(e->prev, e->next); list_add(e, h); }
static inline void list_move_tail(struct list_head *e, struct list_head *h) { __list_del(e->prev, e->next); list_add_tail(e, h); }
static inline bool list_empty(const struct list_head *h) { return READ_ONCE(h->next) == h; }
static inline bool list_is_last(const struct list_head *e, const struct list_head *h) { return e->next == h; }
static inline bool list_is_singular(const struct list_head *h) { return !list_empty(h) && h->next == h->prev; }
static inline void list_splice_init(struct list_head *l, struct list_head *h)
{ if (!list_empty(l)) { struct list_head *f = l->next, *t = l->prev, *at = h->next; f->prev = h; h->next = f; t->next = at; at->prev = t; INIT_LIST_HEAD(l); } }
#define list_entry(p, t, m) container_of(p, t, m)
#define list_first_entry(h, t, m) list_entry((h)->next, t, m)
#define list_last_entry(h, t, m) list_entry((h)->prev, t, m)
#define list_first_entry_or_null(h, t, m) (list_empty(h) ? NULL : list_first_entry(h, t, m))
#define list_next_entry(p, m) list_entry((p)->m.next, typeof(*(p)), m)
#define list_prev_entry(p, m) list_entry((p)->m.prev, typeof(*(p)), m)
#define list_for_each(p, h) for (p = (h)->next; p != (h); p = p->next)
#define list_for_each_safe(p, n, h) for (p = (h)->next, n = p->next; p != (h); p = n, n = p->next)
#define list_for_each_entry(p, h, m) \
    for (p = list_first_entry(h, typeof(*p), m); &p->m != (h); p = list_next_entry(p, m))
#define list_for_each_entry_reverse(p, h, m) \
    for (p = list_last_entry(h, typeof(*p), m); &p->m != (h); p = list_prev_entry(p, m))
#define list_for_each_entry_safe(p, n, h, m) \
    for (p = list_first_entry(h, typeof(*p), m), n = list_next_entry(p, m); &p->m != (h); p = n, n = list_next_entry(n, m))
#define list_for_each_entry_continue(p, h, m) \
    for (p = list_next_entry(p, m); &p->m != (h); p = list_next_entry(p, m))
#define HLIST_HEAD_INIT { .first = NULL }
static inline void INIT_HLIST_HEAD(struct hlist_head *h) { h->first = NULL; }
static inline void INIT_HLIST_NODE(struct hlist_node *n) { n->next = NULL; n->pprev = NULL; }
static inline bool hlist_empty(const struct hlist_head *h) { return !h->first; }
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{ n->next = h->first; if (h->first) h->first->pprev = &n->next; h->first = n; n->pprev = &h->first; }
static inline void hlist_del(struct hlist_node *n)
{ *n->pprev = n->next; if (n->next) n->next->pprev = n->pprev; n->next = NULL; n->pprev = NULL; }
static inline void hlist_del_init(struct hlist_node *n) { if (n->pprev) { hlist_del(n); INIT_HLIST_NODE(n); } }
#define hlist_entry(p, t, m) container_of(p, t, m)
#define hlist_entry_safe(p, t, m) ({ typeof(p) ____p = (p); ____p ? hlist_entry(____p, t, m) : NULL; })
#define hlist_for_each_entry(p, h, m) \
    for (p = hlist_entry_safe((h)->first, typeof(*p), m); p; p = hlist_entry_safe(p->m.next, typeof(*p), m))
#define hlist_for_each_entry_safe(p, n, h, m) \
    for (p = hlist_entry_safe((h)->first, typeof(*p), m); p && ({ n = p->m.next; 1; }); p = hlist_entry_safe(n, typeof(*p), m))

/* ---- locks ---- */
typedef struct { volatile int locked; unsigned long flags_saved; } spinlock_t;
typedef spinlock_t raw_spinlock_t;
typedef struct { volatile int readers; spinlock_t w; } rwlock_t;
#define __SPIN_LOCK_UNLOCKED(n) { 0, 0 }
#define DEFINE_SPINLOCK(n) spinlock_t n = __SPIN_LOCK_UNLOCKED(n)
#define DEFINE_RAW_SPINLOCK(n) DEFINE_SPINLOCK(n)
void spin_lock_init(spinlock_t *l);
void spin_lock(spinlock_t *l);
void spin_unlock(spinlock_t *l);
int spin_trylock(spinlock_t *l);
unsigned long lkpi_spin_lock_irqsave(spinlock_t *l);
void spin_unlock_irqrestore(spinlock_t *l, unsigned long flags);
#define spin_lock_irqsave(l, f) do { (f) = lkpi_spin_lock_irqsave(l); } while (0)
#define raw_spin_lock_irqsave spin_lock_irqsave
#define raw_spin_unlock_irqrestore spin_unlock_irqrestore
#define raw_spin_lock_init spin_lock_init
#define raw_spin_lock spin_lock
#define raw_spin_unlock spin_unlock
void spin_lock_irq(spinlock_t *l);
void spin_unlock_irq(spinlock_t *l);
void spin_lock_bh(spinlock_t *l);
void spin_unlock_bh(spinlock_t *l);
static inline bool spin_is_locked(spinlock_t *l) { return l->locked != 0; }
#define assert_spin_locked(l) do { } while (0)
#define lockdep_assert_held(l) do { } while (0)
#define rwlock_init(l) do { (l)->readers = 0; spin_lock_init(&(l)->w); } while (0)
void read_lock(rwlock_t *l);
void read_unlock(rwlock_t *l);
void write_lock(rwlock_t *l);
void write_unlock(rwlock_t *l);
unsigned long lkpi_write_lock_irqsave(rwlock_t *l);
void write_unlock_irqrestore(rwlock_t *l, unsigned long f);
unsigned long lkpi_read_lock_irqsave(rwlock_t *l);
void read_unlock_irqrestore(rwlock_t *l, unsigned long f);
#define write_lock_irqsave(l, f) do { (f) = lkpi_write_lock_irqsave(l); } while (0)
#define read_lock_irqsave(l, f) do { (f) = lkpi_read_lock_irqsave(l); } while (0)
unsigned long lkpi_irq_save(void);
void lkpi_irq_restore(unsigned long f);
#define local_irq_save(f) do { (f) = lkpi_irq_save(); } while (0)
#define local_irq_restore(f) lkpi_irq_restore(f)
void local_irq_disable(void);
void local_irq_enable(void);
bool irqs_disabled(void);
void preempt_disable(void);
void preempt_enable(void);
#define preempt_enable_no_resched preempt_enable
bool in_interrupt(void);
bool in_atomic(void);
bool in_irq(void);
#define in_hardirq in_irq
bool in_task(void);

/* sleeping locks (the waiters sleep: never in an interrupt) */
struct mutex { volatile int locked; void *owner; struct list_head waiters; spinlock_t wait_lock; };
#define __MUTEX_INITIALIZER(n) { 0, NULL, LIST_HEAD_INIT((n).waiters), __SPIN_LOCK_UNLOCKED(n) }
#define DEFINE_MUTEX(n) struct mutex n = __MUTEX_INITIALIZER(n)
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
int mutex_lock_interruptible(struct mutex *m);
int mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_is_locked(struct mutex *m);
#define mutex_destroy(m) do { } while (0)
#define mutex_lock_nested(m, s) mutex_lock(m)
struct semaphore { volatile int count; spinlock_t lock; struct list_head waiters; };
#define __SEMAPHORE_INITIALIZER(n, c) { (c), __SPIN_LOCK_UNLOCKED(n), LIST_HEAD_INIT((n).waiters) }
#define DEFINE_SEMAPHORE(n, c) struct semaphore n = __SEMAPHORE_INITIALIZER(n, c)
void sema_init(struct semaphore *s, int count);
void down(struct semaphore *s);
int down_interruptible(struct semaphore *s);
int down_trylock(struct semaphore *s);
int down_timeout(struct semaphore *s, long jiffies);
void up(struct semaphore *s);
struct rw_semaphore { volatile long count; spinlock_t lock; struct list_head waiters; };
#define __RWSEM_INITIALIZER(n) { 0, __SPIN_LOCK_UNLOCKED(n), LIST_HEAD_INIT((n).waiters) }
#define DECLARE_RWSEM(n) struct rw_semaphore n = __RWSEM_INITIALIZER(n)
void init_rwsem(struct rw_semaphore *s);
void down_read(struct rw_semaphore *s);
int down_read_trylock(struct rw_semaphore *s);
void up_read(struct rw_semaphore *s);
void down_write(struct rw_semaphore *s);
int down_write_trylock(struct rw_semaphore *s);
void up_write(struct rw_semaphore *s);
void downgrade_write(struct rw_semaphore *s);
static inline int rwsem_is_locked(struct rw_semaphore *s) { return READ_ONCE(s->count) != 0; }

/* red-black trees (the driver keeps its allocations in them) */
struct rb_node { unsigned long __rb_parent_color; struct rb_node *rb_right, *rb_left; } __attribute__((aligned(sizeof(long))));
struct rb_root { struct rb_node *rb_node; };
struct rb_root_cached { struct rb_root rb_root; struct rb_node *rb_leftmost; };
#define RB_ROOT (struct rb_root){ NULL }
#define RB_ROOT_CACHED (struct rb_root_cached){ { NULL }, NULL }
#define rb_parent(r) ((struct rb_node *)((r)->__rb_parent_color & ~3UL))
#define rb_entry(p, t, m) container_of(p, t, m)
#define rb_entry_safe(p, t, m) ({ typeof(p) ____p = (p); ____p ? rb_entry(____p, t, m) : NULL; })
#define RB_EMPTY_ROOT(r) (READ_ONCE((r)->rb_node) == NULL)
#define RB_EMPTY_NODE(n) ((n)->__rb_parent_color == (unsigned long)(n))
#define RB_CLEAR_NODE(n) ((n)->__rb_parent_color = (unsigned long)(n))
static inline void rb_link_node(struct rb_node *n, struct rb_node *parent, struct rb_node **link)
{ n->__rb_parent_color = (unsigned long)parent; n->rb_left = n->rb_right = NULL; *link = n; }
void rb_insert_color(struct rb_node *n, struct rb_root *root);
void rb_erase(struct rb_node *n, struct rb_root *root);
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *n);
struct rb_node *rb_prev(const struct rb_node *n);
void rb_replace_node(struct rb_node *victim, struct rb_node *new_, struct rb_root *root);
#define rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
    for (pos = rb_entry_safe(rb_first_postorder(root), typeof(*pos), field); \
         pos && ({ n = rb_entry_safe(rb_next_postorder(&pos->field), typeof(*pos), field); 1; }); pos = n)
struct rb_node *rb_first_postorder(const struct rb_root *root);
struct rb_node *rb_next_postorder(const struct rb_node *n);

/* bitmaps */
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline void bitmap_zero(unsigned long *d, unsigned int n) { memset(d, 0, BITS_TO_LONGS(n) * sizeof(long)); }
static inline void bitmap_fill(unsigned long *d, unsigned int n) { memset(d, 0xff, BITS_TO_LONGS(n) * sizeof(long)); }
unsigned long find_first_bit(const unsigned long *a, unsigned long size);
unsigned long find_first_zero_bit(const unsigned long *a, unsigned long size);
unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off);
unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off);
#define for_each_set_bit(bit, addr, size) \
    for ((bit) = find_first_bit((addr), (size)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
int bitmap_weight(const unsigned long *a, unsigned int n);
void bitmap_set(unsigned long *a, unsigned int start, unsigned int n);
void bitmap_clear(unsigned long *a, unsigned int start, unsigned int n);
bool bitmap_empty(const unsigned long *a, unsigned int n);
bool bitmap_full(const unsigned long *a, unsigned int n);
void bitmap_copy(unsigned long *d, const unsigned long *s, unsigned int n);
void bitmap_or(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n);
int bitmap_and(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n);
unsigned long bitmap_find_next_zero_area(unsigned long *map, unsigned long size, unsigned long start, unsigned int nr, unsigned long align_mask);
unsigned long *bitmap_zalloc(unsigned int n, gfp_t gfp);
void bitmap_free(const unsigned long *b);
#define down_read_nested(s, n) down_read(s)
#define down_write_nested(s, n) down_write(s)
#define lockdep_assert_held_write(l) do { } while (0)
#define lockdep_assert_held_read(l) do { } while (0)
#define might_sleep() do { } while (0)
#define might_lock(l) do { } while (0)

/* ---- waiting ---- */
struct wait_queue_entry;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *w, unsigned mode, int flags, void *key);
struct wait_queue_entry { unsigned int flags; void *private; wait_queue_func_t func; struct list_head entry; };
typedef struct wait_queue_entry wait_queue_entry_t;
struct wait_queue_head { spinlock_t lock; struct list_head head; };
typedef struct wait_queue_head wait_queue_head_t;
#define __WAIT_QUEUE_HEAD_INITIALIZER(n) { __SPIN_LOCK_UNLOCKED(n), LIST_HEAD_INIT((n).head) }
#define DECLARE_WAIT_QUEUE_HEAD(n) wait_queue_head_t n = __WAIT_QUEUE_HEAD_INITIALIZER(n)
void init_waitqueue_head(wait_queue_head_t *q);
#define __init_waitqueue_head(q, n, k) init_waitqueue_head(q)
void wake_up(wait_queue_head_t *q);
void wake_up_all(wait_queue_head_t *q);
void wake_up_interruptible(wait_queue_head_t *q);
void wake_up_interruptible_all(wait_queue_head_t *q);
bool waitqueue_active(wait_queue_head_t *q);
/* (the conditions are looked at again each time it is woken, and at least every few milliseconds) */
long lkpi_wait_event(wait_queue_head_t *q, bool (*cond)(void *), void *arg, bool interruptible, long timeout);
#define __lkpi_wait(q, condition, intr, timeout) ({                                                  \
        long __left = (timeout);                                                                    \
        while (!(condition)) {                                                                      \
            __left = lkpi_wait_event(&(q), NULL, NULL, (intr), __left);                             \
            if (__left == -512 /* -ERESTARTSYS */ || __left == 0) break;                            \
        }                                                                                           \
        (condition) ? (__left > 0 ? __left : 1) : __left; })
#define wait_event(q, c) do { (void)__lkpi_wait(q, c, false, -1L); } while (0)
#define wait_event_interruptible(q, c) ({ long __r = __lkpi_wait(q, c, true, -1L); __r < 0 ? (int)__r : 0; })
#define wait_event_timeout(q, c, t) ({ long __r = __lkpi_wait(q, c, false, (long)(t)); __r < 0 ? 0 : __r; })
#define wait_event_interruptible_timeout(q, c, t) __lkpi_wait(q, c, true, (long)(t))
#define wait_event_killable(q, c) wait_event_interruptible(q, c)
struct completion { unsigned int done; wait_queue_head_t wait; };
#define COMPLETION_INITIALIZER(n) { 0, __WAIT_QUEUE_HEAD_INITIALIZER((n).wait) }
#define DECLARE_COMPLETION(n) struct completion n = COMPLETION_INITIALIZER(n)
void init_completion(struct completion *c);
#define reinit_completion(c) do { (c)->done = 0; } while (0)
void complete(struct completion *c);
void complete_all(struct completion *c);
void wait_for_completion(struct completion *c);
int wait_for_completion_interruptible(struct completion *c);
unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout);
bool completion_done(struct completion *c);
bool try_wait_for_completion(struct completion *c);
