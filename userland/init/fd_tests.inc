/* Descriptors (kernel/proc/proc.c, linux/sys_fs.c): the table grows as it fills, to RLIMIT_NOFILE (1024 at
 * first, as Linux; a program may raise it to 65536). It was a fixed 256, and Firefox ran out: its next socket()
 * freed the socket twice and the kernel panicked. Here a full table must refuse every kind of new descriptor
 * with EMFILE and nothing else, and a raised limit must give that many. */

static int fd_fill(int *fds, int max) {
    int n = 0;
    while (n < max) {
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            break;
        fds[n++] = fd;
    }
    return n;
}

static void fd_tests(void) {
    struct rlimit was, lim;
    CHECK(getrlimit(RLIMIT_NOFILE, &was) == 0 && was.rlim_cur == 1024 && was.rlim_max == 65536,
          "descriptors: RLIMIT_NOFILE is 1024, up to 65536");
    int *fds = malloc(sizeof *fds * 5000);
    if (!fds)
        return;

    int n = fd_fill(fds, 5000);
    int e = errno;
    CHECK(n > 1000 && fds[n - 1] == 1023 && e == EMFILE, "descriptors: 1024 of them, then EMFILE");
    /* a full table: each kind of new descriptor refused, the kernel unharmed */
    errno = 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s < 0 && errno == EMFILE, "descriptors: socket() with a full table is EMFILE (it panicked)");
    errno = 0;
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(u < 0 && errno == EMFILE, "descriptors: a UDP socket too");
    int p[2] = {-1, -1};
    errno = 0;
    int pr = pipe(p);
    int ef = eventfd(0, 0), ep = epoll_create1(0), un = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(pr < 0 && ef < 0 && ep < 0 && un < 0 && errno == EMFILE, "descriptors: pipe, eventfd, epoll, AF_UNIX too");
    close(fds[--n]); /* one free: a socket works again */
    s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s == 1023, "descriptors: one closed, a socket in its place");
    if (s >= 0)
        close(s);

    /* raised, as Firefox raises it: more of them, past where the old table ended */
    lim.rlim_cur = 4096, lim.rlim_max = 65536;
    CHECK(setrlimit(RLIMIT_NOFILE, &lim) == 0, "descriptors: RLIMIT_NOFILE raised to 4096");
    n += fd_fill(fds + n, 5000 - n);
    CHECK(fds[n - 1] == 4095 && errno == EMFILE, "descriptors: 4096 of them now");
    close(fds[--n]); /* (one free, to read the limits with) */
    char line[256] = "";
    FILE *lf = fopen("/proc/self/limits", "r");
    while (lf && fgets(line, sizeof line, lf) && strncmp(line, "Max open files", 14))
        ;
    if (lf)
        fclose(lf);
    CHECK(strstr(line, "4096") && strstr(line, "65536"), "descriptors: /proc/self/limits says so");
    struct pollfd pf = {fds[n - 1], POLLIN, 0};
    CHECK(poll(&pf, 1, 0) == 1 && !(pf.revents & POLLNVAL), "descriptors: poll on one past 4000");
    close(fds[--n]);
    CHECK(dup2(0, 4094) == 4094 && dup2(0, 4096) < 0 && errno == EBADF, "descriptors: dup2 to 4094, not past the limit");
    close(4094);
    CHECK(fcntl(0, F_DUPFD, 5000) < 0 && errno == EINVAL, "descriptors: F_DUPFD past the limit is EINVAL");
    lim.rlim_max = 70000;
    CHECK(setrlimit(RLIMIT_NOFILE, &lim) < 0 && errno == EPERM, "descriptors: no hard limit past 65536");
    lim.rlim_cur = 5000, lim.rlim_max = 4096;
    CHECK(setrlimit(RLIMIT_NOFILE, &lim) < 0 && errno == EINVAL, "descriptors: no soft limit above the hard one");

    /* a child gets the whole table and the limit (fork copies them) */
    pid_t pid = fork();
    if (pid == 0) {
        struct rlimit l;
        struct stat st;
        _exit(getrlimit(RLIMIT_NOFILE, &l) == 0 && l.rlim_cur == 4096 && fstat(fds[n - 1], &st) == 0 ? 0 : 1);
    }
    int status = -1;
    if (pid > 0)
        waitpid(pid, &status, 0);
    CHECK(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "descriptors: a child has them all, and the limit");

    while (n > 0)
        close(fds[--n]);
    free(fds);
    CHECK(setrlimit(RLIMIT_NOFILE, &was) == 0, "descriptors: the limit back to 1024");
    s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(s >= 0 && s < 64, "descriptors: all closed, low numbers again");
    if (s >= 0)
        close(s);
}
