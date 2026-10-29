/* The disk file system (kernel/fs/ext2.c) on the test disk's /dev/sda2:
 * formatted with e2fsprogs' mke2fs, mounted, and put through what apps and
 * Flatpak do. After power-off tests/boot_test.sh runs the host's e2fsck on
 * it, so every structure the kernel wrote is checked by Linux's own tool. */

static int ext2_fill(const char *path, size_t bytes, unsigned seed) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    static unsigned char buf[65536];
    size_t done = 0;
    while (done < bytes) {
        size_t n = bytes - done < sizeof buf ? bytes - done : sizeof buf;
        for (size_t i = 0; i < n; i++)
            buf[i] = (unsigned char)((done + i) * 31 + seed);
        if (write(fd, buf, n) != (ssize_t)n) {
            close(fd);
            return -1;
        }
        done += n;
    }
    return close(fd);
}

static int ext2_check(const char *path, size_t bytes, unsigned seed) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    static unsigned char buf[65536];
    size_t done = 0;
    int bad = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] != (unsigned char)((done + (size_t)i) * 31 + seed))
                bad = 1;
        done += (size_t)n;
    }
    close(fd);
    return bad || done != bytes ? -1 : 0;
}

static void ext2_tests(void) {
    if (access("/dev/sda2", F_OK) != 0) {
        printf("utest: (no /dev/sda2: disk file system tests skipped)\n");
        return;
    }
    char out[512];
    char *mk[] = {"/usr/sbin/mke2fs", "-q", "-F", "-t", "ext2", "-O", "^dir_index,^resize_inode,^ext_attr", "-b", "4096",
                  "-L", "os-test", "/dev/sda2", NULL};
    int r = run(mk, out, sizeof out);
    CHECK(r == 0, "ext2: mke2fs formats /dev/sda2");
    if (r != 0) {
        printf("utest: mke2fs said: %s\n", out);
        return;
    }
    mkdir("/mnt", 0755);
    mkdir("/mnt/disk", 0755);
    CHECK(mount("/dev/sda2", "/mnt/disk", "ext2", 0, NULL) == 0, "ext2: mount");
    /* files: big, small, sparse, appended, truncated */
    CHECK(ext2_fill("/mnt/disk/big", 9u << 20, 7) == 0 && ext2_check("/mnt/disk/big", 9u << 20, 7) == 0,
          "ext2: 9 MiB written and read back (indirect blocks)");
    CHECK(ext2_fill("/mnt/disk/small", 100, 3) == 0 && ext2_check("/mnt/disk/small", 100, 3) == 0, "ext2: a small file");
    int fd = open("/mnt/disk/sparse", O_RDWR | O_CREAT, 0644);
    char z[16] = {0}, got[16];
    CHECK(fd >= 0 && pwrite(fd, "end", 3, 50u << 20) == 3 && pread(fd, got, 16, 1u << 20) == 16 && !memcmp(got, z, 16),
          "ext2: a sparse file reads zeros in its hole");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == (50 << 20) + 3, "ext2: sparse file size");
    CHECK(ftruncate(fd, 4096) == 0 && fstat(fd, &st) == 0 && st.st_size == 4096, "ext2: truncate shrinks");
    close(fd);
    fd = open("/mnt/disk/small", O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "more", 4) == 4 && close(fd) == 0 && stat("/mnt/disk/small", &st) == 0 && st.st_size == 104,
          "ext2: append");
    /* directories with many entries */
    CHECK(mkdir("/mnt/disk/many", 0755) == 0, "ext2: mkdir");
    int ok = 1;
    for (int i = 0; i < 400 && ok; i++) {
        char p[64];
        snprintf(p, sizeof p, "/mnt/disk/many/file-with-a-longish-name-%03d", i);
        ok = ext2_fill(p, (size_t)(i * 37), (unsigned)i) == 0;
    }
    CHECK(ok, "ext2: 400 files in one directory");
    DIR *d = opendir("/mnt/disk/many");
    int count = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        count++;
    if (d)
        closedir(d);
    CHECK(count == 402, "ext2: readdir lists them all (with . and ..)");
    CHECK(ext2_check("/mnt/disk/many/file-with-a-longish-name-123", 123 * 37, 123) == 0, "ext2: one of them reads back");
    for (int i = 0; i < 400; i += 2) {
        char p[64];
        snprintf(p, sizeof p, "/mnt/disk/many/file-with-a-longish-name-%03d", i);
        unlink(p);
    }
    d = opendir("/mnt/disk/many");
    count = 0;
    while (d && (e = readdir(d)))
        count++;
    if (d)
        closedir(d);
    CHECK(count == 202, "ext2: unlink half of them");
    /* names: rename within and across directories, links, symlinks */
    CHECK(rename("/mnt/disk/small", "/mnt/disk/many/moved") == 0 && ext2_check("/mnt/disk/many/moved", 100, 3) != 0 &&
              stat("/mnt/disk/many/moved", &st) == 0 && st.st_size == 104,
          "ext2: rename across directories");
    CHECK(mkdir("/mnt/disk/a", 0755) == 0 && mkdir("/mnt/disk/a/b", 0755) == 0 && rename("/mnt/disk/a/b", "/mnt/disk/c") == 0 &&
              stat("/mnt/disk/c", &st) == 0 && S_ISDIR(st.st_mode) && stat("/mnt/disk/a", &st) == 0 && st.st_nlink == 2,
          "ext2: move a directory (link counts follow)");
    CHECK(rename("/mnt/disk/a", "/mnt/disk/c/x") == 0 && access("/mnt/disk/c/x", F_OK) == 0, "ext2: a directory into another");
    CHECK(rename("/mnt/disk/c", "/mnt/disk/c/x/y") < 0 && errno == EINVAL, "ext2: not into itself");
    CHECK(link("/mnt/disk/big", "/mnt/disk/big2") == 0 && stat("/mnt/disk/big", &st) == 0 && st.st_nlink == 2,
          "ext2: hard link");
    CHECK(unlink("/mnt/disk/big") == 0 && ext2_check("/mnt/disk/big2", 9u << 20, 7) == 0, "ext2: the other name keeps the data");
    char lnk[300];
    CHECK(symlink("big2", "/mnt/disk/short") == 0 && readlink("/mnt/disk/short", lnk, sizeof lnk) == 4 &&
              ext2_check("/mnt/disk/short", 9u << 20, 7) == 0,
          "ext2: short symlink, followed");
    char longt[200];
    memset(longt, 'x', sizeof longt - 1);
    longt[sizeof longt - 1] = 0;
    CHECK(symlink(longt, "/mnt/disk/long") == 0 && readlink("/mnt/disk/long", lnk, sizeof lnk) == 199, "ext2: long symlink");
    CHECK(rmdir("/mnt/disk/c") < 0 && errno == ENOTEMPTY, "ext2: rmdir refuses a full directory");
    CHECK(rmdir("/mnt/disk/c/x/b") < 0 && errno == ENOENT, "ext2: rmdir of what isn't there");
    /* open while unlinked: readable until closed, then freed */
    fd = open("/mnt/disk/big2", O_RDONLY);
    CHECK(fd >= 0 && unlink("/mnt/disk/big2") == 0 && read(fd, got, 16) == 16 && got[1] == (char)(31 + 7),
          "ext2: an open file outlives its last name");
    if (fd >= 0)
        close(fd);
    /* replace by rename, as installers do */
    ext2_fill("/mnt/disk/new", 5000, 9);
    ext2_fill("/mnt/disk/old", 7000, 1);
    CHECK(rename("/mnt/disk/new", "/mnt/disk/old") == 0 && ext2_check("/mnt/disk/old", 5000, 9) == 0 &&
              access("/mnt/disk/new", F_OK) != 0,
          "ext2: rename over an existing file");
    /* a program run from the disk (its pages read from it) */
    int in = open("/bin/busybox", O_RDONLY), outfd = open("/mnt/disk/busybox", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    static char cbuf[65536];
    ssize_t cn;
    while (in >= 0 && outfd >= 0 && (cn = read(in, cbuf, sizeof cbuf)) > 0)
        if (write(outfd, cbuf, (size_t)cn) != cn)
            break;
    if (in >= 0)
        close(in);
    if (outfd >= 0)
        close(outfd);
    char *echo[] = {"/mnt/disk/busybox", "echo", "ran from the disk", NULL};
    r = run(echo, out, sizeof out);
    CHECK(r == 0 && strstr(out, "ran from the disk"), "ext2: a program runs from the disk");
    sync();
    printf("utest: ext2 tests done\n");
}
