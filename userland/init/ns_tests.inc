/* Sandboxing: mounts, namespaces, seccomp (what bubblewrap, Flatpak and
 * Chromium's sandbox are built from). Included by init.c; runs in test mode.
 * Each test that changes namespaces does so in a child, which reports
 * through its exit code (0: all good, else the number of the step that
 * failed). */

#include <sched.h>
#include <sys/prctl.h>
#include <ucontext.h>

#ifndef MS_REC
#define MS_REC 16384
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif

static int ns_read_text(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t n = 0;
    for (ssize_t r; n + 1 < size && (r = read(fd, buf + n, size - 1 - n)) > 0;)
        n += (size_t)r;
    buf[n] = 0;
    close(fd);
    return (int)n;
}

static int ns_write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

/* Runs fn in a child; its exit status (0: passed, else the failing step). */
static int in_child(int (*fn)(void)) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(fn());
    int st = 0;
    if (pid < 0 || waitpid(pid, &st, 0) != pid)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 200 + WTERMSIG(st);
}

#define STEP(n, cond)                                                                                    \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            printf("utest: (step %d failed: errno %d %s)\n", n, errno, strerror(errno));                  \
            return n;                                                                                    \
        }                                                                                                \
    } while (0)

static int child_private_mounts(void) {
    STEP(1, unshare(CLONE_NEWNS) == 0);
    STEP(2, mount("tmpfs", "/tmp/nst/a", "tmpfs", 0, NULL) == 0);
    STEP(3, ns_write_text("/tmp/nst/a/only-here", "x") == 0);
    return 0;
}

static int child_pivot_root(void) {
    STEP(1, unshare(CLONE_NEWNS) == 0);
    STEP(2, mkdir("/tmp/nst/root", 0755) == 0 || errno == EEXIST);
    STEP(3, mount("tmpfs", "/tmp/nst/root", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") == 0);
    STEP(4, mkdir("/tmp/nst/root/oldroot", 0755) == 0 && mkdir("/tmp/nst/root/etc", 0755) == 0);
    STEP(5, ns_write_text("/tmp/nst/root/etc/marker", "inside") == 0);
    STEP(6, chdir("/tmp/nst/root") == 0);
    STEP(7, syscall(SYS_pivot_root, ".", "oldroot") == 0);
    STEP(8, chdir("/") == 0);
    char buf[64];
    STEP(9, ns_read_text("/etc/marker", buf, sizeof buf) > 0 && !strcmp(buf, "inside"));
    STEP(10, access("/oldroot/tmp/nst", F_OK) == 0); /* the old world, under oldroot */
    /* bind something from the old world in, read-only, as bubblewrap does */
    STEP(11, mkdir("/bin", 0755) == 0);
    STEP(12, mount("/oldroot/bin", "/bin", NULL, MS_BIND | MS_REC, NULL) == 0);
    STEP(13, mount(NULL, "/bin", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0);
    STEP(14, access("/bin/sh", X_OK) == 0);
    STEP(15, open("/bin/new-file", O_CREAT | O_WRONLY, 0644) < 0 && errno == EROFS);
    STEP(16, umount2("/oldroot", MNT_DETACH) == 0);
    STEP(17, access("/oldroot/tmp", F_OK) < 0);
    char cwd[64];
    STEP(18, getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/"));
    /* the mount table, as this process sees it, names its new root "/" */
    STEP(21, mkdir("/proc", 0555) == 0 && mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0);
    char *info = malloc(8192);
    STEP(19, info && ns_read_text("/proc/self/mountinfo", info, 8192) > 0);
    STEP(20, strstr(info, " / ") && strstr(info, " /bin ") && !strstr(info, "oldroot"));
    free(info);
    return 0;
}

/* bubblewrap's own trick: pivot_root(".", ".") then detach "." */
static int child_pivot_dot(void) {
    STEP(1, unshare(CLONE_NEWNS) == 0);
    STEP(2, mkdir("/tmp/nst/root2", 0755) == 0 || errno == EEXIST);
    STEP(3, mount("tmpfs", "/tmp/nst/root2", "tmpfs", 0, NULL) == 0);
    STEP(4, ns_write_text("/tmp/nst/root2/hello", "dot") == 0);
    STEP(5, chdir("/tmp/nst/root2") == 0);
    STEP(6, syscall(SYS_pivot_root, ".", ".") == 0);
    STEP(7, umount2(".", MNT_DETACH) == 0);
    STEP(8, chdir("/") == 0);
    char buf[16];
    STEP(9, ns_read_text("/hello", buf, sizeof buf) > 0 && !strcmp(buf, "dot"));
    STEP(10, access("/tmp", F_OK) < 0);
    return 0;
}

/* bubblewrap's own sequence, step by step */
static int proc_fd = -1;

static int newroot_is_mount(void) {
    char *info = calloc(1, 16384);
    int fd = openat(proc_fd, "self/mountinfo", O_RDONLY);
    int n = fd >= 0 ? (int)read(fd, info, 16383) : -1;
    if (fd >= 0)
        close(fd);
    int ok = info && n > 0 && strstr(info, " /newroot ");
    if (!ok && info)
        printf("utest: mountinfo now (fd %d, read %d, errno %d):\n%s", fd, n, errno, info);
    free(info);
    return ok;
}

/* bwrap's bind: mount, then find it again through /oldroot/proc/self/fd and
 * mountinfo, and remount it with its flags */
static int bind_like_bwrap(const char *src, const char *dst) {
    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) != 0)
        return 1;
    int fd = open(dst, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return 2;
    char via[64], where[256] = "";
    snprintf(via, sizeof via, "/oldroot/proc/self/fd/%d", fd);
    ssize_t n = readlink(via, where, sizeof where - 1);
    close(fd);
    if (n <= 0 || strcmp(where, dst) != 0) {
        printf("utest: readlink %s said %s\n", via, where);
        return 3;
    }
    if (mount("none", dst, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) != 0)
        return 4;
    return 0;
}

static int child_bwrap_like(void) {
    proc_fd = open("/proc", O_PATH | O_DIRECTORY);
    STEP(1, unshare(CLONE_NEWNS) == 0);
    STEP(2, mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) == 0);
    STEP(3, mkdir("/tmp/bwb", 0755) == 0 || errno == EEXIST);
    STEP(4, mount("tmpfs", "/tmp/bwb", "tmpfs", MS_NODEV | MS_NOSUID, NULL) == 0);
    STEP(5, chdir("/tmp/bwb") == 0);
    STEP(6, mkdir("newroot", 0755) == 0);
    STEP(7, mount("newroot", "newroot", NULL, MS_MGC_VAL | MS_BIND | MS_REC, NULL) == 0);
    STEP(8, mkdir("oldroot", 0755) == 0);
    STEP(9, syscall(SYS_pivot_root, "/tmp/bwb", "oldroot") == 0);
    STEP(10, chdir("/") == 0 && newroot_is_mount());
    STEP(11, mkdir("/newroot/usr", 0755) == 0);
    int br = bind_like_bwrap("/oldroot/usr", "/newroot/usr");
    STEP(30 + br, br == 0);
    STEP(12, mkdir("/newroot/proc", 0755) == 0 && mount("proc", "/newroot/proc", "proc", MS_NOSUID | MS_NODEV, NULL) == 0);
    STEP(13, access("/newroot/usr/bin", F_OK) == 0 && access("/newroot/proc/self/stat", F_OK) == 0);
    STEP(14, mount("oldroot", "oldroot", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
    STEP(15, umount2("oldroot", MNT_DETACH) == 0);
    STEP(16, newroot_is_mount());
    int oldrootfd = open("/", O_DIRECTORY | O_RDONLY);
    STEP(17, oldrootfd >= 0 && chdir("/newroot") == 0);
    STEP(18, syscall(SYS_pivot_root, ".", ".") == 0);
    STEP(19, fchdir(oldrootfd) == 0 && umount2(".", MNT_DETACH) == 0);
    STEP(20, chdir("/") == 0 && access("/usr/bin", F_OK) == 0 && access("/proc/self/stat", F_OK) == 0);
    STEP(21, access("/newroot", F_OK) != 0 && access("/oldroot", F_OK) != 0);
    return 0;
}

static int child_user_ns(void) {
    STEP(1, unshare(CLONE_NEWUSER) == 0);
    STEP(2, getuid() == 65534); /* no name for us there yet */
    STEP(3, ns_write_text("/proc/self/setgroups", "deny") == 0);
    STEP(4, ns_write_text("/proc/self/uid_map", "1000 0 1") == 0);
    STEP(5, ns_write_text("/proc/self/gid_map", "1000 0 1") == 0);
    STEP(6, getuid() == 1000 && getgid() == 1000);
    STEP(7, ns_write_text("/proc/self/uid_map", "0 0 1") < 0); /* only once */
    char buf[64];
    STEP(8, ns_read_text("/proc/self/uid_map", buf, sizeof buf) > 0 && strstr(buf, "1000"));
    struct stat st;
    STEP(9, stat("/", &st) == 0 && st.st_uid == 1000); /* root's directory, as this namespace names root */
    /* and a mount namespace of its own, as its owner */
    STEP(10, unshare(CLONE_NEWNS) == 0);
    return 0;
}

static int grandchild_pid1(void) {
    STEP(1, getpid() == 1);
    STEP(2, getppid() == 0); /* the parent is outside */
    char buf[16];
    STEP(3, readlink("/proc/self", buf, sizeof buf) > 0); /* /proc is the old one: its own pid as there */
    return 0;
}

static int child_pid_ns(void) {
    STEP(1, unshare(CLONE_NEWPID) == 0);
    int r = in_child(grandchild_pid1);
    STEP(10 + (r > 0 ? r : 0), r == 0);
    /* the second child of the namespace dies with its pid 1 */
    return 0;
}

static int child_net_ns(void) {
    STEP(1, unshare(CLONE_NEWNET) == 0);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80)};
    a.sin_addr.s_addr = htonl(0x0a000202);
    STEP(2, s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) < 0 && errno == ENETUNREACH);
    close(s);
    /* netlink: only "lo" is here */
    int nl = socket(16 /* AF_NETLINK */, SOCK_RAW | SOCK_CLOEXEC, 0);
    STEP(3, nl >= 0);
    struct {
        uint32_t len;
        uint16_t type, flags;
        uint32_t seq, pid;
        uint8_t family, pad[3];
        uint32_t rest[3];
    } rq = {sizeof rq, 18 /* RTM_GETLINK */, 1 | 0x300 /* REQUEST|DUMP */, 7, 0, 0, {0}, {0}};
    STEP(4, send(nl, &rq, sizeof rq, 0) == (ssize_t)sizeof rq);
    char buf[4096];
    ssize_t n = recv(nl, buf, sizeof buf, 0);
    STEP(5, n > 16);
    int links = 0, done = 0;
    for (ssize_t off = 0; off + 16 <= n;) {
        uint32_t len;
        uint16_t type;
        memcpy(&len, buf + off, 4);
        memcpy(&type, buf + off + 4, 2);
        if (type == 16)
            links++;
        if (type == 3)
            done = 1;
        if (len < 16)
            break;
        off += (len + 3) & ~3u;
    }
    STEP(6, links == 1 && done);
    /* bubblewrap brings lo up and gets an acknowledgement */
    struct {
        uint32_t len;
        uint16_t type, flags;
        uint32_t seq, pid;
        uint8_t family, pad;
        uint16_t iftype;
        int32_t index;
        uint32_t ifflags, change;
    } up = {sizeof up, 16 /* RTM_NEWLINK */, 1 | 4 /* REQUEST|ACK */, 8, 0, 0, 0, 0, 1, 1, 1};
    STEP(7, send(nl, &up, sizeof up, 0) == (ssize_t)sizeof up);
    n = recv(nl, buf, sizeof buf, 0);
    uint16_t type = 0;
    int32_t err = -1;
    if (n >= 20) {
        memcpy(&type, buf + 4, 2);
        memcpy(&err, buf + 16, 4);
    }
    STEP(8, type == 2 && err == 0);
    close(nl);
    return 0;
}

static int child_uts_ns(void) {
    STEP(1, unshare(CLONE_NEWUTS) == 0);
    STEP(2, sethostname("sandbox", 7) == 0);
    struct utsname u;
    STEP(3, uname(&u) == 0 && !strcmp(u.nodename, "sandbox"));
    return 0;
}

static int child_setns(void) {
    char before[64], after[64];
    int fd = open("/proc/self/ns/net", O_RDONLY);
    STEP(1, fd >= 0);
    ssize_t n = readlink("/proc/self/ns/net", before, sizeof before - 1);
    STEP(2, n > 0);
    before[n] = 0;
    STEP(3, !strncmp(before, "net:[", 5));
    STEP(4, unshare(CLONE_NEWNET) == 0);
    n = readlink("/proc/self/ns/net", after, sizeof after - 1);
    STEP(5, n > 0);
    after[n] = 0;
    STEP(6, strcmp(before, after) != 0);
    STEP(7, setns(fd, CLONE_NEWNET) == 0);
    n = readlink("/proc/self/ns/net", after, sizeof after - 1);
    after[n > 0 ? n : 0] = 0;
    STEP(8, !strcmp(before, after));
    return 0;
}

/* seccomp: classic BPF, as installed with prctl or seccomp(2) */
struct t_filter {
    uint16_t code;
    uint8_t jt, jf;
    uint32_t k;
};
struct t_prog {
    unsigned short len;
    struct t_filter *filter;
};
#define LD_NR {0x20, 0, 0, 0}                       /* ld [0]: the call's number */
#define JEQ(v, t, f) {0x15, t, f, (uint32_t)(v)}     /* jeq #v */
#define RET(v) {0x06, 0, 0, (uint32_t)(v)}           /* ret #v */

#ifdef __aarch64__
#define UC_NR(uc) ((uc)->uc_mcontext.regs[8])
#define UC_RET(uc) ((uc)->uc_mcontext.regs[0])
#else
#define UC_NR(uc) ((uc)->uc_mcontext.gregs[REG_RAX])
#define UC_RET(uc) ((uc)->uc_mcontext.gregs[REG_RAX])
#endif

static volatile int trapped_nr, trapped_code;

static void on_sigsys(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    ucontext_t *uc = ctx;
    trapped_code = si->si_code;
    trapped_nr = si->si_syscall;
    /* Chromium checks the saved register holds the call's number, then answers in the return register */
    if (UC_NR(uc) == (unsigned long)si->si_syscall)
        UC_RET(uc) = 777;
}

static int child_seccomp_inherited(void) {
    errno = 0;
    return syscall(SYS_getuid) == -1 && errno == 42 ? 0 : 1;
}

static int child_seccomp(void) {
    STEP(1, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    STEP(2, prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
    struct t_filter f[] = {
        LD_NR,
        JEQ(SYS_getuid, 0, 1), RET(0x00050000 | 42),  /* getuid: ERRNO(42) */
        JEQ(SYS_getppid, 0, 1), RET(0x00030000),      /* getppid: TRAP */
        RET(0x7fff0000),                              /* the rest: ALLOW */
    };
    struct t_prog prog = {sizeof f / sizeof f[0], f};
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    STEP(3, syscall(SYS_seccomp, 1 /* SET_MODE_FILTER */, 1 /* TSYNC */, &prog) == 0);
    STEP(4, prctl(PR_GET_SECCOMP, 0, 0, 0, 0) == 2);
    errno = 0;
    STEP(5, syscall(SYS_getuid) == -1 && errno == 42);
    STEP(6, getpid() > 0); /* allowed */
    long r = syscall(SYS_getppid);
    STEP(7, r == 777 && trapped_nr == SYS_getppid && trapped_code == 1 /* SYS_SECCOMP */);
    /* filters stack and are inherited */
    int st = in_child(child_seccomp_inherited);
    STEP(8, st == 0);
    return 0;
}

static int child_seccomp_kill(void) {
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    struct t_filter f[] = {LD_NR, JEQ(SYS_getpid, 0, 1), RET(0x80000000 /* KILL_PROCESS */), RET(0x7fff0000)};
    struct t_prog prog = {sizeof f / sizeof f[0], f};
    if (syscall(SYS_seccomp, 1, 0, &prog) != 0)
        return 1;
    getpid(); /* never returns */
    return 2;
}

static void ns_tests(void) {
    CHECK(mkdir("/tmp/nst", 0755) == 0 || errno == EEXIST, "mounts: a directory to work in");
    CHECK(mount("tmpfs", "/tmp/nst", "tmpfs", 0, "mode=0755") == 0, "mounts: tmpfs");
    char *info = malloc(16384);
    CHECK(info && ns_read_text("/proc/self/mountinfo", info, 16384) > 0 && strstr(info, " /tmp/nst ") &&
              strstr(info, "tmpfs"),
          "mounts: /proc/self/mountinfo lists it");
    free(info);
    CHECK(mkdir("/tmp/nst/a", 0755) == 0 && mkdir("/tmp/nst/b", 0755) == 0 &&
              ns_write_text("/tmp/nst/a/f", "hi") == 0,
          "mounts: files in the tmpfs");
    char buf[64];
    CHECK(mount("/tmp/nst/a", "/tmp/nst/b", NULL, MS_BIND, NULL) == 0 && ns_read_text("/tmp/nst/b/f", buf, sizeof buf) > 0 &&
              !strcmp(buf, "hi"),
          "mounts: bind a directory");
    char cwd[128] = "";
    CHECK(chdir("/tmp/nst/b") == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/tmp/nst/b"),
          "mounts: getcwd inside a bind mount");
    CHECK(chdir("..") == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/tmp/nst"), "mounts: .. out of a bind mount");
    chdir("/");
    CHECK(mount(NULL, "/tmp/nst/b", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) == 0 &&
              open("/tmp/nst/b/g", O_CREAT | O_WRONLY, 0644) < 0 && errno == EROFS &&
              ns_write_text("/tmp/nst/a/g", "ok") == 0,
          "mounts: read-only bind (the original stays writable)");
    CHECK(ns_write_text("/tmp/nst/c", "") == 0 && mount("/tmp/nst/a/f", "/tmp/nst/c", NULL, MS_BIND, NULL) == 0 &&
              ns_read_text("/tmp/nst/c", buf, sizeof buf) > 0 && !strcmp(buf, "hi"),
          "mounts: bind a single file");
    CHECK(umount2("/tmp/nst/b", MNT_DETACH) == 0 && access("/tmp/nst/b/f", F_OK) < 0, "mounts: lazy unmount");
    int r = in_child(child_private_mounts);
    CHECK(r == 0 && access("/tmp/nst/a/only-here", F_OK) < 0 && access("/tmp/nst/a/f", F_OK) == 0,
          "namespaces: a mount namespace's mounts stay in it");
    CHECK((r = in_child(child_pivot_root)) == 0, "namespaces: pivot_root into a tmpfs, bind, read-only, detach");
    CHECK((r = in_child(child_pivot_dot)) == 0, "namespaces: pivot_root(\".\", \".\") as bubblewrap does");
    CHECK((r = in_child(child_bwrap_like)) == 0, "namespaces: bubblewrap's own pivot sequence");
    CHECK((r = in_child(child_user_ns)) == 0, "namespaces: user namespace, uid_map, gid_map, setgroups");
    CHECK((r = in_child(child_pid_ns)) == 0, "namespaces: pid namespace (its first process is pid 1)");
    CHECK((r = in_child(child_net_ns)) == 0, "namespaces: network namespace (no IP; netlink sees only lo)");
    CHECK((r = in_child(child_uts_ns)) == 0, "namespaces: host name of its own");
    struct utsname u;
    CHECK(uname(&u) == 0 && strcmp(u.nodename, "sandbox") != 0, "namespaces: ...not ours");
    CHECK((r = in_child(child_setns)) == 0, "namespaces: /proc/self/ns links and setns");
    CHECK((r = in_child(child_seccomp)) == 0, "seccomp: ERRNO, TRAP with SIGSYS, inherited");
    r = in_child(child_seccomp_kill);
    CHECK(r == 200 + SIGSYS, "seccomp: KILL_PROCESS ends it with SIGSYS");
    umount2("/tmp/nst/c", MNT_DETACH);
    CHECK(umount2("/tmp/nst", MNT_DETACH) == 0, "mounts: unmount the tmpfs");
}

static void ensure_machine_id(void);
static void start_buses(void);

/* run(), as another user (the person's session runs as uid 1000) */
static int run_as(uid_t uid, char *const argv[], char *out, size_t out_size) {
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        if (setgroups(0, NULL) < 0 || setgid(uid) < 0 || setuid(uid) < 0)
            _exit(126);
        setenv("HOME", "/root", 1);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t used = 0;
    ssize_t n;
    while ((n = read(pipefd[0], out + used, out_size - 1 - used)) > 0)
        used += (size_t)n;
    out[used] = 0;
    close(pipefd[0]);
    int status;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* The base system's sandbox and bus (when the image has them). */
static void base_tests(void) {
    if (access("/usr/bin/bwrap", X_OK) != 0) {
        printf("utest: (no /usr/bin/bwrap in this image: base tests skipped)\n");
        return;
    }
    char out[512];
    char *bw[] = {"/usr/bin/bwrap", "--ro-bind", "/usr", "/usr", "--symlink", "usr/lib", "/lib",
                  "--symlink", "usr/lib64", "/lib64", "--symlink", "usr/bin", "/bin", "--proc", "/proc",
                  "--dev", "/dev", "--tmpfs", "/tmp", "--unshare-all", "--die-with-parent", "--hostname", "box",
                  "/usr/bin/bash", "-c", "echo sandboxed $(id -u) $$ $(hostname) $(ls / | tr '\n' ' ')", NULL};
    int r = run(bw, out, sizeof out);
    printf("utest: bwrap said (status %d): %s", r, out);
    CHECK(r == 0 && strstr(out, "sandboxed 0") && strstr(out, " box ") && !strstr(out, "root "),
          "base: bubblewrap runs a program in a sandbox of its own");
    if (access("/usr/bin/dbus-daemon", X_OK) != 0)
        return;
    /* the buses exactly as a normal boot starts them */
    start_buses();
    char *sys_call[] = {"/usr/bin/gdbus", "call", "--system", "--dest", "org.freedesktop.DBus", "--object-path",
                        "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.GetId", NULL};
    r = run(sys_call, out, sizeof out);
    CHECK(r == 0 && strstr(out, "('"), "base: the system bus answers");
    char *call[] = {"/usr/bin/gdbus", "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path",
                    "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.ListActivatableNames", NULL};
    r = run_as(1000, call, out, sizeof out); /* the session bus is the person's */
    printf("utest: session bus can start: %s", out);
    CHECK(r == 0 && strstr(out, "org.freedesktop.DBus"), "base: the session bus answers");
    if (access("/usr/bin/flatpak", X_OK) != 0)
        return;
    char big[4096];
    char *fp[] = {"/usr/bin/bash", "-c",
                  "export HOME=/root; flatpak --version && flatpak list --columns=ref && "
                  "echo apps: $(flatpak remote-ls flathub --app --columns=application | wc -l) && "
                  "flatpak install -y --noninteractive flathub com.github.dimkr.gplaces 2>&1 | tail -3 && "
                  "flatpak run --command=sh com.github.dimkr.gplaces -c 'echo inside $FLATPAK_ID; ls /app/bin'", NULL};
    r = run(fp, big, sizeof big);
    printf("utest: flatpak said (status %d): %s\n", r, big);
    CHECK(strstr(big, "apps: ") && !strstr(big, "apps: 0"), "base: flatpak lists Flathub's apps");
    CHECK(strstr(big, "inside com.github.dimkr.gplaces") != NULL, "base: flatpak installs an app from Flathub and runs it in its sandbox");
    CHECK(strstr(big, "com.valware.prism/x86_64") != NULL, "base: Prism comes installed as a Flatpak");
    /* as the person's session runs apps: an ordinary user, through the session bus */
    char *as_user[] = {"/usr/bin/flatpak", "run", "--command=sh", "com.github.dimkr.gplaces", "-c",
                       "echo as user $(id -u) in $FLATPAK_ID", NULL};
    r = run_as(1000, as_user, big, sizeof big);
    printf("utest: as uid 1000 (status %d): %s\n", r, big);
    CHECK(strstr(big, "as user 1000 in com.github.dimkr.gplaces") != NULL,
          "base: an ordinary user runs a Flatpak app in its sandbox");
}
