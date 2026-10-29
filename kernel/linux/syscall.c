#include <kernel/lib.h>
#include <kernel/proc.h>
#include <linux/syscall.h>

/* Linux x86-64 system call numbers -> implementations. Numbers come from
 * arch/x86/entry/syscalls/syscall_64.tbl and must never change. */

#define SYSCALLS(X)                                                                                      \
    X(0, read) X(1, write) X(2, open) X(3, close) X(4, stat) X(5, fstat) X(6, lstat) X(7, poll)            \
    X(8, lseek) X(9, mmap) X(10, mprotect) X(11, munmap) X(12, brk) X(13, rt_sigaction)                    \
    X(14, rt_sigprocmask) X(15, rt_sigreturn) X(16, ioctl) X(17, pread64) X(18, pwrite64) X(19, readv)     \
    X(20, writev) X(21, access) X(22, pipe) X(24, sched_yield) X(25, mremap) X(26, msync)                  \
    X(28, madvise) X(32, dup) X(33, dup2) X(34, pause) X(35, nanosleep) X(39, getpid) X(40, sendfile) X(41, socket) X(42, connect) X(43, accept) X(44, sendto) X(45, recvfrom) \
    X(46, sendmsg) X(47, recvmsg) X(48, shutdown) X(49, bind) X(50, listen) X(51, getsockname)              \
    X(52, getpeername) X(54, setsockopt) X(55, getsockopt) X(288, accept4) X(56, clone)        \
    X(57, fork) X(58, vfork) X(59, execve) X(60, exit) X(61, wait4) X(62, kill) X(63, uname)              \
    X(72, fcntl) X(73, flock) X(74, fsync) X(75, fsync) X(76, truncate) X(77, ftruncate)                  \
    X(79, getcwd) X(80, chdir) X(81, fchdir) X(82, rename) X(83, mkdir) X(84, rmdir) X(85, creat)        \
    X(86, link) X(87, unlink) X(88, symlink) X(89, readlink) X(90, chmod) X(91, fchmod) X(92, chown)      \
    X(93, fchown) X(94, lchown) X(95, umask) X(96, gettimeofday) X(97, getrlimit) X(98, getrusage)          \
    X(99, sysinfo) X(100, times) X(102, getuid) X(103, syslog) X(104, getgid) X(105, setuid)              \
    X(106, setgid) X(107, geteuid) X(108, getegid) X(109, setpgid) X(110, getppid) X(111, getpgrp)       \
    X(112, setsid) X(115, getgroups) X(116, setgroups) X(113, setreuid) X(114, setregid) X(117, setresuid) X(118, getresuid) X(119, setresgid) X(120, getresgid) X(121, getpgid) X(122, setfsuid) X(123, setfsgid) X(124, getsid) X(125, capget) X(126, capset) X(127, rt_sigpending)               \
    X(130, rt_sigsuspend) X(131, sigaltstack) X(137, statfs) X(138, fstatfs) X(140, getpriority) X(141, setpriority) X(142, sched_setparam) X(143, sched_getparam) X(144, sched_setscheduler) X(145, sched_getscheduler) X(157, prctl)                \
    X(158, arch_prctl) X(160, setrlimit) X(162, sync) X(169, reboot) X(170, sethostname) X(155, pivot_root) X(161, chroot) X(165, mount) X(166, umount2) X(272, unshare) X(308, setns) X(317, seccomp)                \
    X(186, gettid) X(200, tkill) X(201, time) X(202, futex) X(203, sched_setaffinity) X(204, sched_getaffinity)                    \
    X(217, getdents64) X(218, set_tid_address) X(228, clock_gettime) X(229, clock_getres)               \
    X(230, clock_nanosleep) X(231, exit_group) X(234, tgkill) X(257, openat) X(258, mkdirat)            \
    X(260, fchownat) X(262, newfstatat) X(263, unlinkat) X(264, renameat) X(265, linkat) X(266, symlinkat) \
    X(267, readlinkat) X(268, fchmodat) X(269, faccessat) X(271, ppoll) X(273, set_robust_list)         \
    X(280, utimensat) X(292, dup3) X(293, pipe2) X(302, prlimit64) X(316, renameat2) X(318, getrandom)  \
    X(332, statx) X(435, clone3) X(436, close_range) X(439, faccessat2) X(307, sendmmsg)                                                     \
    X(23, select) X(53, socketpair) X(213, epoll_create) X(232, epoll_wait) X(233, epoll_ctl)           \
    X(270, pselect6) X(281, epoll_pwait) X(282, signalfd) X(283, timerfd_create) X(284, eventfd)        \
    X(285, fallocate) X(286, timerfd_settime) X(287, timerfd_gettime) X(289, signalfd4)                 \
    X(290, eventfd2) X(291, epoll_create1) X(319, memfd_create) X(441, epoll_pwait2) X(306, syncfs)    \
    X(149, mlock) X(150, mlock) X(151, mlock) X(152, mlock) X(325, mlock) X(221, fadvise64) X(135, personality)

#define DECLARE(nr, name) SYSCALL(name);
SYSCALLS(DECLARE)

#define NR_SYSCALLS 512
#define ENTRY(nr, name) [nr] = sys_##name,
static const syscall_fn table[NR_SYSCALLS] = {SYSCALLS(ENTRY)};

static const char *const names[NR_SYSCALLS] = {
#define NAME(nr, name) [nr] = #name,
    SYSCALLS(NAME)
};

static uint8_t warned[NR_SYSCALLS / 8];

void signal_deliver(struct regs *r);
static bool strace;

void syscall_set_trace(bool on);
void syscall_set_trace(bool on) { strace = on; }

/* fw_cfg opt/os/strace=NAME: only the failing calls of programs called
 * NAME, which is what finding a crash usually needs, without the flood. */
static char strace_name[16];
void syscall_trace_failures(const char *name);
void syscall_trace_failures(const char *name) { ksnprintf(strace_name, sizeof strace_name, "%s", name); }
bool syscall_traced(const char *name);
bool syscall_traced(const char *name) { return strace_name[0] && !strcmp(name, strace_name); }

void syscall_dispatch(struct regs *r) {
    uint64_t nr = r->rax;
    int64_t ret;
    struct thread *self = thread_current();
    struct user_thread *ut = self ? self->user : NULL;
    if (ut)
        ut->sc_nr = (int64_t)nr, ut->sc_a1 = r->rdi, ut->sc_a2 = r->rsi, ut->sc_a3 = r->rdx;
    if (ut && ut->proc->seccomp_mode && !seccomp_check(r, nr, &ret)) {
        /* refused by a seccomp filter: ret is the answer */
    } else if (nr < NR_SYSCALLS && table[nr]) {
        ret = table[nr](r->rdi, r->rsi, r->rdx, r->r10, r->r8, r->r9, r);
        if (strace)
            kprintf("[%d] %s(%lx, %lx, %lx) = %ld\n", proc_current()->pid, names[nr], r->rdi, r->rsi, r->rdx, ret);
        else if (strace_name[0] && ret < 0 && ret != -EAGAIN && !strcmp(proc_current()->name, strace_name)) {
            /* the path, for the calls that take one */
            uint64_t up = nr == 2 || nr == 4 || nr == 6 || nr == 21 || nr == 89 ? r->rdi
                          : nr == 257 || nr == 262 || nr == 267 || nr == 269 || nr == 439 ? r->rsi : 0;
            char path[96] = "";
            if (up && strncpy_from_user(path, up, sizeof path) < 0)
                path[0] = 0;
            path[sizeof path - 1] = 0;
            kprintf("[%d/%d] %s(%lx, %lx, %lx) = %ld %s\n", proc_current()->pid, ut ? ut->tid : 0, names[nr], r->rdi,
                    r->rsi, r->rdx, ret, path);
        }
    } else {
        /* rseq (334) is an optional glibc fast path; ENOSYS is the expected answer. */
        if (nr < NR_SYSCALLS && nr != 334 && !(warned[nr / 8] & (1 << (nr % 8)))) {
            warned[nr / 8] |= (uint8_t)(1 << (nr % 8));
            kprintf("syscall %lu not implemented (pid %d, %s)\n", nr, proc_current()->pid, proc_current()->name);
        }
        ret = -ENOSYS;
    }
    if (ut) { /* recent history, for the hang dump */
        int i = ut->sc_hist_n++ % 16;
        ut->sc_hist[i].nr = (int16_t)nr, ut->sc_hist[i].ret = ret;
        ut->sc_hist[i].a1 = ut->sc_a1, ut->sc_hist[i].a2 = ut->sc_a2, ut->sc_hist[i].a3 = ut->sc_a3;
    }
    /* execve and rt_sigreturn rewrite the whole frame, rax included. */
    if (nr != 15 && !(nr == 59 && ret == 0))
        r->rax = (uint64_t)ret;
    self = thread_current();
    if (self && self->user)
        self->user->sc_nr = -1;
    signal_deliver(r);
}

const char *syscall_name(int64_t nr);
const char *syscall_name(int64_t nr) {
    return nr >= 0 && nr < NR_SYSCALLS && names[nr] ? names[nr] : "?";
}
