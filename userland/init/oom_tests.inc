/* Out of memory (kernel/proc/proc.c): a program that takes every page there is is ended by the kernel (SIGKILL,
 * the biggest one), not whoever happens to need the next page, and the system goes on. oom_score_adj is real:
 * kept by fork, back to 0 at execve when it was below (the desktop's -1000 isn't handed to what it starts). */

static int adj_read(const char *path) {
    char b[16] = "";
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 9999;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    b[n > 0 ? n : 0] = 0;
    return atoi(b);
}

static bool adj_write(const char *path, const char *v) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return false;
    bool ok = write(fd, v, strlen(v)) == (ssize_t)strlen(v);
    close(fd);
    return ok;
}

static void oom_tests(void) {
    /* (an earlier test wrote 100 here: what was written is what is read) */
    CHECK(adj_read("/proc/self/oom_score_adj") == 100 && adj_write("/proc/self/oom_score_adj", "0") &&
              adj_read("/proc/self/oom_score_adj") == 0,
          "out of memory: oom_score_adj is what was written");
    CHECK(adj_write("/proc/self/oom_score_adj", "-500") && adj_read("/proc/self/oom_score_adj") == -500,
          "out of memory: oom_score_adj can be set (root may lower it)");
    CHECK(!adj_write("/proc/self/oom_score_adj", "-1001") && !adj_write("/proc/self/oom_score_adj", "x"),
          "out of memory: nonsense refused");
    pid_t pid = fork();
    if (pid == 0)
        _exit(adj_read("/proc/self/oom_score_adj") == -500 ? 0 : 1);
    int st = -1;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "out of memory: a fork keeps it");
    char out[64] = "";
    char *cat[] = {"/bin/cat", "/proc/self/oom_score_adj", NULL};
    CHECK(run(cat, out, sizeof out) == 0 && atoi(out) == 0, "out of memory: a new program begins at 0 again");
    adj_write("/proc/self/oom_score_adj", "0");

    /* a program that takes all the memory there is: ended by the kernel, not by a segmentation fault */
    pid = fork();
    if (pid == 0) {
        for (;;) {
            char *p = mmap(NULL, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                continue;
            for (size_t i = 0; i < (64u << 20); i += 4096)
                p[i] = 1;
        }
    }
    st = -1;
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "out of memory: the program taking it all is ended (SIGKILL)");
    char *echo[] = {"/bin/busybox", "echo", "still here", NULL};
    CHECK(run(echo, out, sizeof out) == 0 && strstr(out, "still here"), "out of memory: the system goes on after");
}
