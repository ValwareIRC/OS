#pragma once
/* The annotations every Linux file takes for granted (kernel/linuxkpi; included first, as Linux's is). Address-space
 * markers (__user, __iomem...) mean nothing to the compiler here: in our kernel a user pointer is copied with
 * copy_from_user like any other, and device memory is reached through the direct map. */

#define __user
#define __kernel
#define __iomem
#define __force
#define __percpu
#define __rcu
#define __private
#define __bitwise
#define __safe
#define __nocast
#define __must_hold(x)
#define __acquires(x)
#define __releases(x)
#define __acquire(x) (void)0
#define __release(x) (void)0
#define __cond_lock(x, c) (c)

#define __always_inline inline __attribute__((__always_inline__))
#define noinline __attribute__((__noinline__))
#define __noinline noinline
#define __inline__ inline
#define __inline inline
#define __attribute_const__ __attribute__((__const__))
#define __pure __attribute__((__pure__))
#define __cold __attribute__((__cold__))
#define __hot __attribute__((__hot__))
#define __weak __attribute__((__weak__))
#define __used __attribute__((__used__))
#define __maybe_unused __attribute__((__unused__))
#define __always_unused __attribute__((__unused__))
#define __must_check __attribute__((__warn_unused_result__))
#define __noreturn __attribute__((__noreturn__))
#define __packed __attribute__((__packed__))
#define __aligned(x) __attribute__((__aligned__(x)))
#define __section(s) __attribute__((__section__(s)))
#define __printf(a, b) __attribute__((__format__(printf, a, b)))
#define __scanf(a, b) __attribute__((__format__(scanf, a, b)))
#define __malloc __attribute__((__malloc__))
#define __alloc_size(...) __attribute__((__alloc_size__(__VA_ARGS__)))
#define __counted_by(m)
#define __nonstring
#define __randomize_layout
#define __no_randomize_layout
#define __visible __attribute__((__externally_visible__))
#define notrace __attribute__((__no_instrument_function__))
#define __no_sanitize_address
#define __noclone
#define __naked __attribute__((__naked__))
#define __flatten __attribute__((__flatten__))
#define __cacheline_aligned __aligned(64)
#define ____cacheline_aligned __aligned(64)
#define ____cacheline_aligned_in_smp __aligned(64)
#define __read_mostly
#define __ro_after_init
#define asmlinkage
#define fallthrough __attribute__((__fallthrough__))
#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))
#define __unqual_scalar_typeof(x) typeof(x)
