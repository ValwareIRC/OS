/* waitid (linux/sys_proc.c): what wait4 says, in a siginfo, and WNOWAIT to look without reaping. Firefox waits
 * for its processes with it: without it, its shutdown hung until its own watchdog crashed it. */

static void wait_tests(void) {
    siginfo_t si;
    pid_t pid = fork();
    if (pid == 0)
        _exit(7);
    memset(&si, 0xff, sizeof si);
    bool ok = waitid(P_PID, (id_t)pid, &si, WEXITED | WNOWAIT) == 0 && si.si_pid == pid && si.si_signo == SIGCHLD &&
              si.si_code == CLD_EXITED && si.si_status == 7;
    CHECK(ok, "waitid: an ended child, its exit code, looked at without reaping (WNOWAIT)");
    memset(&si, 0, sizeof si);
    ok = waitid(P_PID, (id_t)pid, &si, WEXITED) == 0 && si.si_pid == pid && si.si_status == 7;
    CHECK(ok, "waitid: the same child again, reaped this time");
    errno = 0;
    CHECK(waitid(P_PID, (id_t)pid, &si, WEXITED) < 0 && errno == ECHILD, "waitid: then there is none (ECHILD)");

    pid = fork();
    if (pid == 0) {
        pause();
        _exit(0);
    }
    memset(&si, 0xff, sizeof si);
    int r = waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG); /* (by its pid: earlier tests leave ended children) */
    ok = r == 0 && si.si_pid == 0;
    if (!ok)
        printf("utest: waitid (WNOHANG): %d (errno %d), si_pid %d, si_code %d, si_status %d; the child is %d\n", r,
               errno, si.si_pid, si.si_code, si.si_status, pid);
    CHECK(ok, "waitid: WNOHANG with a child still running says nothing yet (si_pid 0)");
    kill(pid, SIGKILL);
    memset(&si, 0, sizeof si);
    r = waitid(P_PID, (id_t)pid, &si, WEXITED);
    ok = r == 0 && si.si_pid == pid && si.si_code == CLD_KILLED && si.si_status == SIGKILL;
    if (!ok)
        printf("utest: waitid (killed): %d (errno %d), si_pid %d, si_code %d, si_status %d; the child is %d\n", r,
               errno, si.si_pid, si.si_code, si.si_status, pid);
    CHECK(ok, "waitid: a child killed is told as killed, by which signal");
    errno = 0;
    CHECK(waitid(P_ALL, 0, &si, 0) < 0 && errno == EINVAL, "waitid: no kind of event asked for is EINVAL");
}
