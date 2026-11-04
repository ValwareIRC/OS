/* The file cache (kernel/mm/pageobj.c). A disk file's pages, as programs map them, are read from the disk as they
 * are touched and shared by everyone who maps it: mmap made a copy of the whole file before, for each mapping,
 * and an app's first start took minutes for those copies (ldconfig maps every library it indexes). And the base
 * system's files are made at boot from an index (kernel/fs/cpio.c) and read from the medium when first used,
 * not all 1.8 GB of them at every start. Run after ext2_tests, with its disk at /mnt/disk. */

static unsigned fc_sum(unsigned s, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        s = s * 31 + p[i];
    return s;
}

static unsigned char fc_byte(size_t off, unsigned seed) { return (unsigned char)(off * 31 + seed); }

static long fc_cached_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    long kb = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "Cached: %ld kB", &kb) == 1)
            break;
    if (f)
        fclose(f);
    return kb;
}

/* The base system file `path` starts with `magic`: its bytes are the right ones from the archive. */
static bool fc_starts(const char *path, const char *magic, size_t n) {
    char got[8] = {0};
    int fd = open(path, O_RDONLY);
    bool ok = fd >= 0 && read(fd, got, n) == (ssize_t)n && !memcmp(got, magic, n);
    if (fd >= 0)
        close(fd);
    return ok;
}

static void filecache_tests(void) {
    const size_t SZ = (3u << 20) + 123, PG = 4096; /* not a whole number of pages */
    const char *path = "/mnt/disk/mapped";
    bool ok = ext2_fill(path, SZ, 5) == 0;
    int fd = ok ? open(path, O_RDONLY) : -1;
    unsigned char *a = fd >= 0 ? mmap(NULL, SZ, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    ok = a != MAP_FAILED;
    for (size_t i = 0; ok && i < SZ; i += 4093)
        ok = a[i] == fc_byte(i, 5);
    size_t end = (SZ + PG - 1) / PG * PG;
    ok = ok && a[SZ - 1] == fc_byte(SZ - 1, 5) && a[SZ] == 0 && a[end - 1] == 0;
    CHECK(ok, "file cache: a disk file mapped privately reads as the file (zeros after its end)");

    int wfd = open(path, O_WRONLY);
    ok = a != MAP_FAILED && wfd >= 0 && pwrite(wfd, "NEW!", 4, 8192 + 10) == 4 && !memcmp(a + 8192 + 10, "NEW!", 4);
    CHECK(ok, "file cache: what is written to the file shows in a mapping of it");

    unsigned char *b = fd >= 0 ? mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    ok = b != MAP_FAILED && a != MAP_FAILED;
    char c = 0;
    if (ok) {
        b[100] = 'x';
        ok = b[100] == 'x' && a[100] == fc_byte(100, 5) && pread(fd, &c, 1, 100) == 1 && c == (char)fc_byte(100, 5);
    }
    CHECK(ok, "file cache: a private mapping written to has its own copy (the file and the other mapping keep theirs)");
    ok = ok && madvise(b, PG, MADV_DONTNEED) == 0 && b[100] == fc_byte(100, 5);
    CHECK(ok, "file cache: MADV_DONTNEED on it brings the file's bytes back");

    unsigned char *r = mmap(NULL, 4 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *m = r != MAP_FAILED && fd >= 0 ? mmap(r + PG, 2 * PG, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 3 * PG)
                                                  : MAP_FAILED;
    CHECK(m == r + PG && m[0] == fc_byte(3 * PG, 5) && m[PG + 7] == fc_byte(4 * PG + 7, 5),
          "file cache: mapped at a fixed place over a reservation (as the loader puts a library's parts)");

    unsigned char *s = fd >= 0 ? mmap(NULL, 2 * PG, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    errno = 0;
    CHECK(s != MAP_FAILED && s[1] == fc_byte(1, 5) && mprotect(s, 2 * PG, PROT_READ | PROT_WRITE) < 0 && errno == EACCES,
          "file cache: shared and read-only it reads the file, and can't be made writable");

    int rw = open(path, O_RDWR);
    unsigned char *w = rw >= 0 ? mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, rw, 0) : MAP_FAILED;
    ok = w != MAP_FAILED && w[1] == fc_byte(1, 5);
    if (ok) {
        w[2] = 'w';
        ok = w[2] == 'w';
    }
    CHECK(ok, "file cache: shared and writable it still works (a copy, as before)");

    /* a fresh mapping, nothing of it read yet: the file shrinks, then loses its name, and is still read through it */
    unsigned char *u = fd >= 0 ? mmap(NULL, 3 * PG, PROT_READ, MAP_PRIVATE, fd, (1u << 20)) : MAP_FAILED;
    ok = u != MAP_FAILED && rw >= 0 && ftruncate(rw, (1u << 20) + 5000) == 0 && unlink(path) == 0;
    if (fd >= 0)
        close(fd);
    if (wfd >= 0)
        close(wfd);
    if (rw >= 0)
        close(rw);
    ok = ok && u[0] == fc_byte(1u << 20, 5) && u[4999] == fc_byte((1u << 20) + 4999, 5) && u[5000] == 0;
    CHECK(ok, "file cache: shrunk and unlinked, a mapped file is still read through its mapping (zeros past its end)");
    ok = a != MAP_FAILED && a[(1u << 20) + 4999] == fc_byte((1u << 20) + 4999, 5) && a[(1u << 20) + 5000 + 10] == 0;
    CHECK(ok, "file cache: what was cut off reads as zeros in the old mapping's last page");
    if (a != MAP_FAILED)
        munmap(a, SZ);
    if (b != MAP_FAILED)
        munmap(b, SZ);
    if (r != MAP_FAILED)
        munmap(r, 4 * PG);
    if (s != MAP_FAILED)
        munmap(s, 2 * PG);
    if (w != MAP_FAILED)
        munmap(w, 2 * PG);
    if (u != MAP_FAILED)
        munmap(u, 3 * PG);

    /* the base system */
    CHECK(fc_starts("/usr/bin/llama-server", "\177ELF", 4) &&
              fc_starts("/usr/lib/x86_64-linux-gnu/libcrypto.so.3", "\177ELF", 4) &&
              fc_starts("/usr/share/misc/magic.mgc", "\x1c\x04\x1e\xf1", 4),
          "base: files left on the medium start with the right bytes (their places in the archive)");
    const char *bf = "/usr/share/misc/magic.mgc";
    struct stat st;
    ok = stat(bf, &st) == 0 && st.st_size > (1 << 20);
    int bfd = ok ? open(bf, O_RDONLY) : -1;
    static unsigned char chunk[3001];
    unsigned rsum = 0;
    size_t rn = 0;
    ssize_t k;
    while (bfd >= 0 && (k = read(bfd, chunk, sizeof chunk)) > 0) {
        rsum = fc_sum(rsum, chunk, (size_t)k);
        rn += (size_t)k;
    }
    unsigned char *bm = ok && bfd >= 0 ? mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, bfd, 0) : MAP_FAILED;
    ok = ok && rn == (size_t)st.st_size && bm != MAP_FAILED && fc_sum(0, bm, rn) == rsum;
    CHECK(ok, "base: a file from the medium reads whole, and the same by read() and by mmap");
    CHECK(fc_cached_kb() >= (long)(st.st_size >> 10), "base: what was read counts as cached memory (available)");

    /* written to: the file becomes memory's own, what was written and the rest both kept (then put back) */
    unsigned char was[4] = {0}, now[4] = {0};
    int bw = open(bf, O_RDWR);
    ok = bw >= 0 && pread(bw, was, 4, (1 << 20) + 1) == 4 && pwrite(bw, "OSOS", 4, (1 << 20) + 1) == 4 &&
         pread(bw, now, 4, (1 << 20) + 1) == 4 && !memcmp(now, "OSOS", 4) && fc_starts(bf, "\x1c\x04\x1e\xf1", 4);
    unsigned wsum = 0;
    if (ok && pwrite(bw, was, 4, (1 << 20) + 1) == 4 && lseek(bw, 0, SEEK_SET) == 0) {
        while ((k = read(bw, chunk, sizeof chunk)) > 0)
            wsum = fc_sum(wsum, chunk, (size_t)k);
        ok = wsum == rsum;
    }
    CHECK(ok, "base: a file from the medium written to keeps what was written, and all the rest");
    if (bw >= 0)
        close(bw);
    if (bm != MAP_FAILED)
        munmap(bm, (size_t)st.st_size);
    if (bfd >= 0)
        close(bfd);
}
