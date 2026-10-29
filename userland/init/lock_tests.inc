/* Record locks (kernel/fs/locks.c) and the terminal device sandboxes use. */

static void lock_tests(void) {
    int fd = open("/tmp/locktest", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "locks: test file");
    int ready[2], done[2];
    if (fd < 0 || pipe(ready) < 0 || pipe(done) < 0)
        return;
    pid_t child = fork();
    if (child == 0) { /* holds a read lock on the whole file until told to stop */
        struct flock rl = {.l_type = F_RDLCK, .l_whence = SEEK_SET};
        char c = fcntl(fd, F_SETLK, &rl) == 0 ? 'y' : 'n';
        write(ready[1], &c, 1);
        read(done[0], &c, 1);
        _exit(0);
    }
    char c = 0;
    read(ready[0], &c, 1);
    CHECK(c == 'y', "locks: a read lock is granted");
    struct flock wl = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    CHECK(fcntl(fd, F_SETLK, &wl) < 0 && (errno == EAGAIN || errno == EACCES), "locks: a write lock waits for the reader");
    struct flock q = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    CHECK(fcntl(fd, F_GETLK, &q) == 0 && q.l_type == F_RDLCK && q.l_pid == child, "locks: F_GETLK names the holder");
    struct flock rl2 = {.l_type = F_RDLCK, .l_whence = SEEK_SET};
    CHECK(fcntl(fd, F_SETLK, &rl2) == 0, "locks: readers share");
    struct flock un = {.l_type = F_UNLCK, .l_whence = SEEK_SET};
    fcntl(fd, F_SETLK, &un);
    /* an OFD lock (what Flatpak's instance check uses) conflicts with the child's */
    struct flock ow = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    CHECK(fcntl(fd, F_OFD_SETLK, &ow) < 0 && errno == EAGAIN, "locks: OFD write lock refused while held");
    write(done[1], "x", 1);
    waitpid(child, NULL, 0);
    CHECK(fcntl(fd, F_SETLK, &wl) == 0, "locks: granted once the holder exits");
    close(fd);
    close(ready[0]), close(ready[1]), close(done[0]), close(done[1]);
    unlink("/tmp/locktest");
    /* bubblewrap links /dev/ptmx to pts/ptmx in its sandboxes */
    int m = open("/dev/pts/ptmx", O_RDWR | O_NOCTTY);
    CHECK(m >= 0, "pty: /dev/pts/ptmx opens a terminal");
    if (m >= 0)
        close(m);
}

/* What Flatpak's document portal checks before it lets a sandboxed app have a
 * file the person chose (xdg-desktop-portal: xdp_get_path_for_fd, validate_fd):
 * an O_PATH descriptor, passed over a socket, whose /proc/self/fd link names a
 * path that is the same file, also when looked up from its parent. */
static const char *doc_fd_problem(const char *path) {
    int sv[2], fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return "open O_PATH";
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return "socketpair";
    char cbuf[CMSG_SPACE(sizeof(int))], one = 'x';
    struct iovec iov = {&one, 1};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf};
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET, cm->cmsg_type = SCM_RIGHTS, cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof fd);
    if (sendmsg(sv[0], &mh, 0) != 1)
        return "sendmsg";
    close(fd);
    mh.msg_controllen = sizeof cbuf;
    if (recvmsg(sv[1], &mh, 0) != 1 || !(cm = CMSG_FIRSTHDR(&mh)))
        return "recvmsg";
    memcpy(&fd, CMSG_DATA(cm), sizeof fd);
    close(sv[0]), close(sv[1]);
    const char *why = NULL;
    int fl = fcntl(fd, F_GETFL);
    struct stat st, real, pst;
    char proc[64], got[4096], dir[4096];
    ssize_t n;
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    if (fl < 0 || (fl & O_PATH) != O_PATH)
        why = "F_GETFL lacks O_PATH";
    else if (fl & O_NOFOLLOW)
        why = "F_GETFL has O_NOFOLLOW";
    else if (fstat(fd, &st) < 0)
        why = "fstat";
    else if ((n = readlink(proc, got, sizeof got - 1)) <= 0)
        why = "readlink /proc/self/fd";
    else if (access(proc, R_OK | (S_ISDIR(st.st_mode) ? X_OK : 0)) != 0)
        why = "access through /proc/self/fd";
    else if ((got[n] = 0, got[0] != '/'))
        why = "link not absolute";
    else if (stat(got, &real) < 0 || real.st_dev != st.st_dev || real.st_ino != st.st_ino)
        why = "path is another file";
    else {
        snprintf(dir, sizeof dir, "%s", got);
        char *slash = strrchr(dir, '/'), name[256];
        snprintf(name, sizeof name, "%s", slash[1] ? slash + 1 : ".");
        if (slash == dir)
            slash[1] = 0;
        else
            *slash = 0;
        int dfd = open(dir, O_PATH | O_CLOEXEC);
        if (dfd < 0 || fstat(dfd, &pst) < 0)
            why = "parent O_PATH";
        else if (fstatat(dfd, name, &real, AT_SYMLINK_NOFOLLOW) < 0 || real.st_dev != st.st_dev ||
                 real.st_ino != st.st_ino)
            why = "fstatat from the parent";
        if (dfd >= 0)
            close(dfd);
    }
    close(fd);
    return why;
}

static void doc_fd_tests(void) {
    /* as the person, like the portal itself */
    int p[2];
    if (pipe(p) == 0) {
        pid_t c = fork();
        if (c == 0) {
            close(p[0]);
            const char *why = setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000) ? "setuid" : doc_fd_problem("/tmp");
            dprintf(p[1], "%s", why ? why : "");
            _exit(0);
        }
        close(p[1]);
        char why[128] = "";
        ssize_t n = read(p[0], why, sizeof why - 1);
        why[n > 0 ? n : 0] = 0;
        close(p[0]);
        waitpid(c, NULL, 0);
        if (why[0])
            printf("utest: (as uid 1000: %s)\n", why);
        CHECK(!why[0], "document portal accepts a chosen /tmp from uid 1000");
    }
    const char *paths[] = {"/root", "/tmp", "/etc/passwd"};
    for (int i = 0; i < 3; i++) {
        const char *why = doc_fd_problem(paths[i]);
        char what[96];
        snprintf(what, sizeof what, "document portal accepts a chosen %s", paths[i]);
        if (why)
            printf("utest: (%s: %s)\n", paths[i], why);
        CHECK(!why, what);
    }
}
