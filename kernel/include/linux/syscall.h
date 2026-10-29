#pragma once
#include <stdint.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>

/* Every system call has this shape; `r` is the caller's saved registers for
 * the few calls that need them (fork, execve, rt_sigreturn). */
typedef int64_t (*syscall_fn)(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                              struct regs *frame);

#define SYSCALL(name)                                                                                    \
    int64_t sys_##name(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,       \
                       struct regs *frame)
#define UNUSED_ARGS(...) (void)a1, (void)a2, (void)a3, (void)a4, (void)a5, (void)a6, (void)frame

/* Helpers shared by the syscall files. */
struct file *fd_get(int fd); /* takes a reference; NULL if not open */
int fd_install(struct file *f, int min_fd, bool cloexec);
int fd_close(int fd);
struct fdtable *fdtable_create(void);
struct fdtable *fdtable_dup(struct fdtable *src);
void fdtable_put(struct fdtable *t);
struct process *proc_alloc(struct process *parent);
int64_t proc_wait(int pid, int *status, int options);
int proc_kill_pgrp(int pgid, int sig);
void proc_discard_signal(struct process *p, int sig);
int64_t signal_return(struct regs *r);
void random_bytes(void *buf, size_t len);

/* Copies a user path into a kernel buffer of PATH_MAX bytes (kmalloc'd). */
#define PATH_MAX 4096
int64_t user_path(uint64_t uptr, char **out);
/* Resolves the starting directory for *at() calls. */
int at_dir(int dirfd, const char *path, struct path *out);

/* Opens `v` as a new file and installs it as a descriptor. Consumes the
 * caller's vnode reference, so on failure the vnode's release op cleans up. */
int vnode_install_fd(struct vnode *v, uint32_t flags, bool cloexec);
/* Ready POLL* bits of a file (POLLIN|POLLOUT for files without a poll op);
 * stores the wait queue that signals changes in *wq if wq is non-NULL. */
int poll_file(struct file *f, struct waitq **wq);
/* Timeout from a user timespec in milliseconds (rounded up); -1 for NULL. */
int64_t user_timeout_ms(uint64_t uts);
/* Temporary signal masks (pselect6, ppoll, epoll_pwait, rt_sigsuspend), as
 * Linux's saved_sigmask: sigmask_push installs the user sigset `uset` and
 * remembers the old mask; sigmask_pop(result) puts it back at once unless
 * the call was interrupted (-EINTR). Then the old mask comes back when the
 * handler's sigreturn runs, or at syscall exit if no handler runs. */
int sigmask_push(uint64_t uset, uint64_t size);
void sigmask_pop(int64_t result);
