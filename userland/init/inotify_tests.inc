/* inotify (kernel/linux/sys_inotify.c): a watched folder is told what is made, written, closed, moved and
 * deleted in it, by name; a watched file what happens to it; poll wakes for it; one-shot watches end. */

#include <sys/inotify.h>

/* The events waiting, as "MASK:name" words ("" when none), in order. */
static int inotify_drain(int fd, char *out, size_t n) {
    char buf[4096] __attribute__((aligned(8)));
    size_t used = 0;
    out[0] = 0;
    int count = 0;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r <= 0)
            break;
        for (ssize_t off = 0; off < r;) {
            struct inotify_event *e = (struct inotify_event *)(buf + off);
            const char *what = (e->mask & IN_CREATE) ? "CREATE" : (e->mask & IN_MODIFY) ? "MODIFY"
                             : (e->mask & IN_CLOSE_WRITE) ? "CLOSE_WRITE" : (e->mask & IN_MOVED_FROM) ? "MOVED_FROM"
                             : (e->mask & IN_MOVED_TO) ? "MOVED_TO" : (e->mask & IN_DELETE_SELF) ? "DELETE_SELF"
                             : (e->mask & IN_DELETE) ? "DELETE" : (e->mask & IN_ATTRIB) ? "ATTRIB"
                             : (e->mask & IN_IGNORED) ? "IGNORED" : (e->mask & IN_OPEN) ? "OPEN"
                             : (e->mask & IN_CLOSE_NOWRITE) ? "CLOSE_NOWRITE" : "OTHER";
            used += (size_t)snprintf(out + used, used < n ? n - used : 0, "%s%s%s:%s", used ? " " : "",
                                     (e->mask & IN_ISDIR) ? "DIR_" : "", what, e->len ? e->name : "");
            off += (ssize_t)sizeof *e + e->len;
            count++;
        }
    }
    return count;
}

static void inotify_tests(void) {
    mkdir("/tmp/watched", 0755);
    unlink("/tmp/watched/a.txt"), unlink("/tmp/watched/b.txt"), rmdir("/tmp/watched/sub");
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(fd >= 0, "inotify: a descriptor");
    if (fd < 0)
        return;
    int wd = inotify_add_watch(fd, "/tmp/watched", IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM |
                                                       IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_ATTRIB);
    CHECK(wd > 0, "inotify: a folder watched");
    char ev[1024];
    CHECK(inotify_drain(fd, ev, sizeof ev) == 0 && errno == EAGAIN, "inotify: nothing yet (and no blocking)");

    int f = open("/tmp/watched/a.txt", O_CREAT | O_WRONLY, 0644);
    write(f, "one", 3);
    write(f, "two", 3); /* merged with the one before, as Linux does */
    close(f);
    struct pollfd pf = {fd, POLLIN, 0};
    CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "inotify: poll says there is something");
    int pending = 0;
    CHECK(ioctl(fd, FIONREAD, &pending) == 0 && pending > 0, "inotify: FIONREAD says how much");
    inotify_drain(fd, ev, sizeof ev);
    CHECK(!strcmp(ev, "CREATE:a.txt MODIFY:a.txt CLOSE_WRITE:a.txt"), "inotify: made, written, closed, by name");
    if (strcmp(ev, "CREATE:a.txt MODIFY:a.txt CLOSE_WRITE:a.txt"))
        printf("utest: inotify: got \"%s\"\n", ev);

    /* written through a descriptor opened later, by another name lookup: still told with its name */
    f = open("/tmp/watched/a.txt", O_WRONLY | O_APPEND);
    write(f, "x", 1);
    close(f);
    chmod("/tmp/watched/a.txt", 0600);
    inotify_drain(fd, ev, sizeof ev);
    CHECK(!strcmp(ev, "MODIFY:a.txt CLOSE_WRITE:a.txt ATTRIB:a.txt"), "inotify: changes to a file, told to its folder");
    if (strcmp(ev, "MODIFY:a.txt CLOSE_WRITE:a.txt ATTRIB:a.txt"))
        printf("utest: inotify: got \"%s\"\n", ev);

    /* the file itself watched too: its own events come without a name */
    int fwd = inotify_add_watch(fd, "/tmp/watched/a.txt", IN_MOVE_SELF | IN_DELETE_SELF | IN_ATTRIB);
    CHECK(fwd > 0 && fwd != wd, "inotify: a file watched");
    CHECK(inotify_add_watch(fd, "/tmp/watched/a.txt", IN_ATTRIB | IN_MASK_CREATE) < 0 && errno == EEXIST,
          "inotify: IN_MASK_CREATE refuses a second watch");
    CHECK(inotify_add_watch(fd, "/tmp/watched/a.txt", IN_ATTRIB | IN_ONLYDIR) < 0 && errno == ENOTDIR,
          "inotify: IN_ONLYDIR on a file");
    rename("/tmp/watched/a.txt", "/tmp/watched/b.txt");
    mkdir("/tmp/watched/sub", 0755);
    rmdir("/tmp/watched/sub");
    unlink("/tmp/watched/b.txt");
    char want[] = "MOVED_FROM:a.txt MOVED_TO:b.txt OTHER: DIR_CREATE:sub DIR_DELETE:sub DELETE:b.txt DELETE_SELF: IGNORED:";
    inotify_drain(fd, ev, sizeof ev);
    CHECK(!strcmp(ev, want), "inotify: moved (a pair), a folder made and gone, deleted, and the file's own watch ends");
    if (strcmp(ev, want))
        printf("utest: inotify: got \"%s\"\n", ev);

    /* the two halves of a move share a cookie */
    f = open("/tmp/watched/a.txt", O_CREAT | O_WRONLY, 0644);
    close(f);
    inotify_drain(fd, ev, sizeof ev);
    rename("/tmp/watched/a.txt", "/tmp/watched/b.txt");
    char buf[512] __attribute__((aligned(8)));
    ssize_t r = read(fd, buf, sizeof buf);
    struct inotify_event *e1 = (struct inotify_event *)buf;
    struct inotify_event *e2 = r > 0 ? (struct inotify_event *)(buf + sizeof *e1 + e1->len) : NULL;
    CHECK(r > 0 && e2 && (e1->mask & IN_MOVED_FROM) && (e2->mask & IN_MOVED_TO) && e1->cookie && e1->cookie == e2->cookie,
          "inotify: a move's two events share a cookie");
    unlink("/tmp/watched/b.txt");
    inotify_drain(fd, ev, sizeof ev);

    /* a buffer too small for the next event: EINVAL, the event kept */
    f = open("/tmp/watched/a-rather-long-name-for-a-file.txt", O_CREAT | O_WRONLY, 0644);
    close(f);
    char tiny[8];
    CHECK(read(fd, tiny, sizeof tiny) < 0 && errno == EINVAL, "inotify: too small a buffer is EINVAL");
    CHECK(inotify_drain(fd, ev, sizeof ev) >= 1, "inotify: the event is still there after");
    unlink("/tmp/watched/a-rather-long-name-for-a-file.txt");
    inotify_drain(fd, ev, sizeof ev);

    /* removing a watch: IN_IGNORED, then silence */
    CHECK(inotify_rm_watch(fd, wd) == 0, "inotify: a watch removed");
    CHECK(inotify_rm_watch(fd, wd) < 0 && errno == EINVAL, "inotify: removing it again is EINVAL");
    f = open("/tmp/watched/a.txt", O_CREAT | O_WRONLY, 0644);
    close(f);
    inotify_drain(fd, ev, sizeof ev);
    CHECK(!strcmp(ev, "IGNORED:"), "inotify: after its IN_IGNORED, nothing more from it");
    unlink("/tmp/watched/a.txt");

    /* one-shot: the first event, then it ends */
    int os = inotify_add_watch(fd, "/tmp/watched", IN_CREATE | IN_ONESHOT);
    f = open("/tmp/watched/a.txt", O_CREAT | O_WRONLY, 0644);
    close(f);
    f = open("/tmp/watched/b.txt", O_CREAT | O_WRONLY, 0644);
    close(f);
    inotify_drain(fd, ev, sizeof ev);
    CHECK(os > 0 && !strcmp(ev, "CREATE:a.txt IGNORED:"), "inotify: IN_ONESHOT ends after its first event");
    if (strcmp(ev, "CREATE:a.txt IGNORED:"))
        printf("utest: inotify: got \"%s\"\n", ev);
    unlink("/tmp/watched/a.txt"), unlink("/tmp/watched/b.txt");
    close(fd);
    rmdir("/tmp/watched");
}
