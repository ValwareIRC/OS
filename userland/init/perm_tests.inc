/* Permissions (kernel/fs/vfs.c): an ordinary user, and a sandbox's "root", against files as Unix guards them.
 * Included by init.c; runs in test mode. Fixtures are made as root; each check runs in a child that has become
 * someone else, and reports through its exit code (0: all good, else the step that failed). */

#define PT_UID 1000

static int pt_put(const char *path, const char *text, int mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) && chmod(path, mode) == 0 ? 0 : -1;
}

static int pt_become_user(void) {
    return setgroups(0, NULL) == 0 && setgid(PT_UID) == 0 && setuid(PT_UID) == 0 ? 0 : -1;
}

/* an ordinary user: only what the bits give it */
static int child_plain_user(void) {
    STEP(1, pt_become_user() == 0 && getuid() == PT_UID && geteuid() == PT_UID);
    errno = 0;
    STEP(2, open("/tmp/pt/secret", O_RDONLY) < 0 && errno == EACCES); /* 0600 root's */
    int fd = open("/tmp/pt/open", O_RDONLY);                      /* 0644: readable */
    STEP(3, fd >= 0);
    close(fd);
    STEP(4, open("/tmp/pt/open", O_WRONLY) < 0 && errno == EACCES); /* ...but not writable */
    STEP(5, open("/tmp/pt/new", O_WRONLY | O_CREAT, 0644) < 0 && errno == EACCES); /* 0755 root's directory */
    STEP(6, open("/tmp/pt/locked/inside", O_RDONLY) < 0 && errno == EACCES);       /* 0700: not even searched */
    STEP(7, unlink("/tmp/rootfile") < 0 && errno == EPERM);  /* sticky /tmp: someone else's name */
    STEP(8, chmod("/tmp/pt/open", 0666) < 0 && errno == EPERM); /* not its owner */
    /* its own file: everything */
    fd = open("/tmp/pt-mine", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    STEP(9, fd >= 0 && write(fd, "x", 1) == 1);
    close(fd);
    STEP(10, chmod("/tmp/pt-mine", 0644) == 0 && unlink("/tmp/pt-mine") == 0);
    STEP(11, access("/tmp/pt/secret", R_OK) < 0 && access("/tmp/pt/open", R_OK) == 0 && access("/tmp/pt/open", W_OK) < 0);
    STEP(12, truncate("/tmp/pt/open", 0) < 0 && errno == EACCES);
    STEP(13, rename("/tmp/pt/open", "/tmp/pt-stolen") < 0 && errno == EACCES); /* out of a directory it can't write */
    STEP(14, mkdir("/etc/pt-not-mine", 0755) < 0 && errno == EACCES);
    STEP(15, symlink("/etc/passwd", "/tmp/pt/link") < 0 && errno == EACCES);
    /* a name already there is EEXIST first, even where it may not write (what `mkdir -p` goes on) */
    errno = 0;
    STEP(30, mkdir("/tmp/pt", 0755) < 0 && errno == EEXIST && mkdir("/etc", 0755) < 0 && errno == EEXIST);
    errno = 0;
    STEP(31, symlink("/x", "/tmp/pt/open") < 0 && errno == EEXIST);
    /* executing needs x */
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"/tmp/pt/noexec.sh", NULL};
        execv(argv[0], argv);
        _exit(errno == EACCES ? 0 : 1);
    }
    int st = 0;
    STEP(16, pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    pid = fork();
    if (pid == 0) {
        char *argv[] = {"/tmp/pt/exec.sh", NULL};
        execv(argv[0], argv);
        _exit(99);
    }
    STEP(17, pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7);
    STEP(18, setuid(0) < 0); /* and no way back */
    return 0;
}

/* a sandbox's root (a user namespace, as bubblewrap makes for Flatpak): root inside, nobody to the system */
static int child_userns_root(void) {
    STEP(1, pt_become_user() == 0);
    STEP(2, unshare(CLONE_NEWUSER) == 0);
    STEP(3, ns_write_text("/proc/self/setgroups", "deny") == 0);
    STEP(4, ns_write_text("/proc/self/uid_map", "0 1000 1") == 0 && ns_write_text("/proc/self/gid_map", "0 1000 1") == 0);
    STEP(5, getuid() == 0); /* root, as it sees itself */
    errno = 0;
    STEP(6, open("/tmp/pt/open", O_WRONLY) < 0 && errno == EACCES); /* the real root's file: not its to write */
    STEP(7, open("/tmp/pt/secret", O_RDONLY) < 0 && errno == EACCES);
    STEP(8, open("/tmp/pt/locked/inside", O_RDONLY) < 0);
    STEP(9, chmod("/tmp/pt/open", 0666) < 0);
    int fd = open("/tmp/pt-ns", O_WRONLY | O_CREAT | O_TRUNC, 0600); /* its own, in /tmp: fine */
    STEP(10, fd >= 0);
    close(fd);
    STEP(11, unlink("/tmp/pt-ns") == 0);
    return 0;
}

static void perm_tests(void) {
    /* the fixtures, as root */
    mkdir("/tmp/pt", 0755);
    chmod("/tmp/pt", 0755);
    mkdir("/tmp/pt/locked", 0700);
    chmod("/tmp/pt/locked", 0700);
    bool made = pt_put("/tmp/pt/secret", "secret", 0600) == 0 && pt_put("/tmp/pt/open", "open", 0644) == 0 &&
                pt_put("/tmp/pt/locked/inside", "in", 0644) == 0 && pt_put("/tmp/rootfile", "root's", 0666) == 0 &&
                pt_put("/tmp/pt/noexec.sh", "#!/bin/sh\nexit 0\n", 0644) == 0 &&
                pt_put("/tmp/pt/exec.sh", "#!/bin/sh\nexit 7\n", 0755) == 0;
    CHECK(made, "permissions: fixtures made as root");
    /* root itself: everything it always could */
    int fd = open("/tmp/pt/secret", O_RDWR);
    CHECK(fd >= 0, "permissions: root reads and writes anything");
    if (fd >= 0)
        close(fd);
    int r;
    CHECK((r = in_child(child_plain_user)) == 0,
          "permissions: an ordinary user gets only what the bits give (read, write, search, create, sticky, chmod, "
          "exec, access, truncate, rename)");
    if (r)
        printf("utest: (ordinary user: step %d)\n", r);
    CHECK((r = in_child(child_userns_root)) == 0, "permissions: a sandbox's root is nobody to the system's files");
    if (r)
        printf("utest: (sandbox root: step %d)\n", r);
    unlink("/tmp/rootfile");
}
