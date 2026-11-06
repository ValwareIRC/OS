/* The OS PID 1 (for now). Built with musl as an ordinary static Linux
 * program: running it at all proves the kernel's Linux ABI works.
 *
 * OS_MODE=test: run the userland self-tests, report, power off.
 * OS_MODE=persist-write:T / persist-check:T: the disk persistence test.
 * otherwise:       the desktop, as the person (session.inc), and the services it needs; a root shell on
 *                  the console only when the host asks for one (OS_CONSOLE=shell, for development). */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <math.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void proc_tests(void);
static void lo_own_address_test(void);

#define CHECK(cond, what)                                                                                \
    do {                                                                                                 \
        if (cond) {                                                                                      \
            printf("utest: %s ok\n", what);                                                              \
        } else {                                                                                         \
            printf("utest: %s FAILED (errno %d: %s)\n", what, errno, strerror(errno));                   \
            failures++;                                                                                  \
        }                                                                                                \
    } while (0)

static int run(char *const argv[], char *out, size_t out_size) {
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
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

/* Networking through the kernel's own TCP/IP stack: DHCP must have
 * configured the NIC, DNS goes through musl's resolver (UDP), and an HTTP
 * request goes over TCP to the real internet. */
static void net_tests(void) {
    int waited = 0;
    while (access("/etc/resolv.conf", F_OK) != 0 && waited < 100) {
        struct timespec t = {0, 100 * 1000 * 1000};
        nanosleep(&t, NULL);
        waited++;
    }
    CHECK(access("/etc/resolv.conf", F_OK) == 0, "network configured (DHCP)");
    lo_own_address_test();

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    int gai = getaddrinfo("example.com", "80", &hints, &res);
    CHECK(gai == 0 && res, "DNS lookup example.com");
    if (gai != 0 || !res)
        return;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0, "TCP connect example.com:80");
    freeaddrinfo(res);
    const char *req = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    CHECK(write(fd, req, strlen(req)) == (ssize_t)strlen(req), "TCP send HTTP request");
    static char resp[262144];
    size_t used = 0;
    ssize_t n;
    while (used < sizeof resp - 1 && (n = read(fd, resp + used, sizeof resp - 1 - used)) > 0)
        used += (size_t)n;
    resp[used] = 0;
    close(fd);
    CHECK(!strncmp(resp, "HTTP/1.1 ", 9) && strstr(resp, "Example Domain"), "HTTP response over TCP");
}

/* ---- disks ---- */

/* Bytes that depend on the disk position and a per-run seed, so data left
 * on the disk by an earlier run can never pass for this run's writes. */
static void fill_pattern(uint8_t *b, size_t len, uint64_t pos, uint32_t seed) {
    for (size_t i = 0; i < len; i++)
        b[i] = (uint8_t)(((pos + i) * 0x9e3779b97f4a7c15ull ^ seed) >> 29);
}

/* ---- loopback networking ----
 * 127.0.0.1 through the kernel's own TCP/IP stack, no NIC involved: what the
 * shell needs to reach a local AI server (llama-server on 127.0.0.1:8080).
 * The byte patterns depend on the stream position, so a lost, repeated or
 * reordered segment shows up as a mismatch. */

#define LO_ECHO_BYTES (4u << 20)

static struct sockaddr_in lo_sin(uint32_t addr, uint16_t port) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(addr)};
    return a;
}

/* A listening TCP socket on addr:<any port>; the port comes back from getsockname. */
static int lo_server(uint32_t addr, uint16_t *port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    struct sockaddr_in a = lo_sin(addr, 0);
    socklen_t len = sizeof a;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
        bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 64) < 0 ||
        getsockname(fd, (struct sockaddr *)&a, &len) < 0 || a.sin_addr.s_addr != htonl(addr) || !a.sin_port) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    *port = ntohs(a.sin_port);
    return fd;
}

static int lo_connect(uint32_t addr, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = lo_sin(addr, port);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof a) == 0)
        return fd;
    int e = errno;
    if (fd >= 0)
        close(fd);
    errno = e;
    return -1;
}

static bool write_all(int fd, const void *buf, size_t len) {
    for (size_t done = 0; done < len;) {
        ssize_t n = write(fd, (const char *)buf + done, len - done);
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

static bool read_all(int fd, void *buf, size_t len) {
    for (size_t done = 0; done < len;) {
        ssize_t n = read(fd, (char *)buf + done, len - done);
        if (n <= 0)
            return false;
        done += (size_t)n;
    }
    return true;
}

static bool is_addr(const struct sockaddr_in *a, uint32_t addr, uint16_t port) {
    return a->sin_family == AF_INET && a->sin_addr.s_addr == htonl(addr) && (!port || a->sin_port == htons(port));
}

/* The 4 MiB stream, made once before the clock starts: generating it byte
 * by byte during the transfer (under QEMU without KVM) costs more than
 * moving it, and the timing is meant to show the transport. */
static uint8_t *lo_data;

/* memcmp eight bytes per step (musl's goes byte by byte), for the same reason. */
static bool same_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
    for (; n >= 8; a += 8, b += 8, n -= 8) {
        uint64_t x, y;
        memcpy(&x, a, 8);
        memcpy(&y, b, 8);
        if (x != y)
            return false;
    }
    for (; n; n--)
        if (*a++ != *b++)
            return false;
    return true;
}

/* Echoes one accepted connection, checking the bytes; the exit status says
 * what went wrong. */
static int lo_echo_server(int ls, uint16_t port) {
    struct sockaddr_in peer, me;
    socklen_t pl = sizeof peer, ml = sizeof me;
    int c = accept(ls, (struct sockaddr *)&peer, &pl);
    close(ls);
    if (c < 0)
        return 1;
    if (getsockname(c, (struct sockaddr *)&me, &ml) < 0 || !is_addr(&me, INADDR_LOOPBACK, port) ||
        !is_addr(&peer, INADDR_LOOPBACK, 0))
        return 2;
    static uint8_t buf[65536];
    size_t total = 0;
    ssize_t n;
    while ((n = read(c, buf, sizeof buf)) > 0) {
        if (total + (size_t)n > LO_ECHO_BYTES || !same_bytes(buf, lo_data + total, (size_t)n))
            return 3;
        if (!write_all(c, buf, (size_t)n))
            return 4;
        total += (size_t)n;
    }
    if (n < 0)
        return 5;
    close(c);
    return total == LO_ECHO_BYTES ? 0 : 6;
}

struct lo_writer {
    int fd;
    bool ok;
};

/* Sends LO_ECHO_BYTES in odd-sized writes, then half-closes. */
static void *lo_writer_main(void *arg) {
    struct lo_writer *w = arg;
    w->ok = true;
    for (size_t done = 0; done < LO_ECHO_BYTES;) {
        size_t n = LO_ECHO_BYTES - done < 100003 ? LO_ECHO_BYTES - done : 100003;
        if (!write_all(w->fd, lo_data + done, n)) {
            w->ok = false;
            break;
        }
        done += n;
    }
    if (shutdown(w->fd, SHUT_WR) < 0)
        w->ok = false;
    return NULL;
}

static long ms_since(const struct timespec *t0);

/* One way, nothing checked: what the transport alone manages. The reader
 * answers one byte once it has everything, which stops the clock. */
static void lo_throughput_test(void) {
    uint16_t port = 0;
    int ls = lo_server(INADDR_LOOPBACK, &port);
    pid_t pid = ls >= 0 ? fork() : -1;
    if (pid == 0) {
        int c = accept(ls, NULL, NULL);
        static uint8_t buf[65536];
        size_t total = 0;
        ssize_t n;
        while (c >= 0 && total < LO_ECHO_BYTES && (n = read(c, buf, sizeof buf)) > 0)
            total += (size_t)n;
        _exit(total == LO_ECHO_BYTES && write(c, "k", 1) == 1 ? 0 : 1);
    }
    if (ls >= 0)
        close(ls);
    int c = pid > 0 ? lo_connect(INADDR_LOOPBACK, port) : -1;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    char k = 0;
    bool ok = c >= 0 && write_all(c, lo_data, LO_ECHO_BYTES) && read(c, &k, 1) == 1 && k == 'k';
    long ms = ms_since(&t0);
    if (c >= 0)
        close(c);
    int status = -1;
    if (pid > 0)
        waitpid(pid, &status, 0);
    CHECK(ok && WIFEXITED(status) && WEXITSTATUS(status) == 0, "lo: TCP 4 MiB one way");
    printf("utest: lo: 4 MiB one way in %ld ms (%ld MiB/s)\n", ms, ms > 0 ? 4000L / ms : 0L);
}

/* 4 MiB over loopback with every 7th data segment lost and another arriving late (the kernel's fault injector):
 * every byte must arrive in order (segments ahead of a gap kept, SACK, resends), soon, and the receiver's buffer
 * must have grown past its first 128 KiB (window scaling agreed, the ring doubling while data streams). */
static void lo_chaos_test(void) {
    int cf = open("/proc/sys/net/ipv4/os_loopback_chaos", O_WRONLY);
    CHECK(cf >= 0 && write(cf, "7", 1) == 1, "lo: loss and reordering switched on (tests)");
    if (cf >= 0)
        close(cf);
    uint16_t port = 0;
    int ls = lo_server(INADDR_LOOPBACK, &port);
    pid_t pid = ls >= 0 ? fork() : -1;
    if (pid == 0) {
        int c = accept(ls, NULL, NULL);
        static uint8_t buf[65536];
        size_t total = 0;
        ssize_t n;
        uint32_t h = 2166136261u, rcvbuf = 0;
        socklen_t sl = sizeof rcvbuf;
        struct timeval tv = {20, 0}; /* (a stalled connection fails the test, it doesn't hang it) */
        if (c >= 0)
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        while (c >= 0 && total < LO_ECHO_BYTES && (n = read(c, buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; i++)
                h = (h ^ buf[i]) * 16777619u;
            total += (size_t)n;
        }
        uint32_t want = 2166136261u;
        for (size_t i = 0; i < LO_ECHO_BYTES; i++)
            want = (want ^ lo_data[i]) * 16777619u;
        getsockopt(c, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &sl);
        char r = total != LO_ECHO_BYTES ? 'n' : h != want ? 'x' : rcvbuf <= 128 * 1024 ? 's' : 'k';
        _exit(write(c, &r, 1) == 1 ? 0 : 1);
    }
    if (ls >= 0)
        close(ls);
    int c = pid > 0 ? lo_connect(INADDR_LOOPBACK, port) : -1;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    char k = 0;
    struct timeval tv = {25, 0};
    if (c >= 0)
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv), setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    bool wrote = c >= 0 && write_all(c, lo_data, LO_ECHO_BYTES);
    long wrote_ms = ms_since(&t0);
    bool sent = wrote && read(c, &k, 1) == 1;
    long ms = ms_since(&t0);
    printf("utest: lo: (all of it handed to TCP after %ld ms, the receiver's answer after %ld)\n", wrote_ms, ms);
    if (c >= 0)
        close(c);
    if (pid > 0) {
        kill(pid, SIGKILL); /* (gone already, unless it stalled) */
        waitpid(pid, NULL, 0);
    }
    cf = open("/proc/sys/net/ipv4/os_loopback_chaos", O_WRONLY);
    if (cf >= 0) {
        if (write(cf, "0", 1) != 1)
            printf("utest: lo: couldn't switch the chaos off\n");
        close(cf);
    }
    CHECK(sent && k != 'n' && k != 'x', "lo: 4 MiB through loss and reordering, every byte in order");
    CHECK(sent && k == 'k', "lo: the receive buffer grew (window scaling agreed)");
    CHECK(sent && ms < 15000, "lo: and soon (no stalls on the lost ones)");
    printf("utest: lo: 4 MiB with every 7th segment lost in %ld ms (receiver said %c)\n", ms, k ? k : '-');
}

static void lo_echo_test(void) {
    uint16_t port = 0;
    int ls = lo_server(INADDR_LOOPBACK, &port);
    CHECK(ls >= 0, "lo: listen on 127.0.0.1, any port (getsockname)");
    if (ls < 0)
        return;
    pid_t pid = fork();
    if (pid == 0)
        _exit(lo_echo_server(ls, port));
    close(ls);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int c = lo_connect(INADDR_LOOPBACK, port);
    struct sockaddr_in me, peer;
    socklen_t ml = sizeof me, pl = sizeof peer;
    int one = 1, got = 0;
    socklen_t gl = sizeof got;
    CHECK(c >= 0 && getsockname(c, (struct sockaddr *)&me, &ml) == 0 && is_addr(&me, INADDR_LOOPBACK, 0) &&
              getpeername(c, (struct sockaddr *)&peer, &pl) == 0 && is_addr(&peer, INADDR_LOOPBACK, port),
          "lo: connect; getsockname/getpeername report 127.0.0.1");
    CHECK(c >= 0 && setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0 &&
              getsockopt(c, IPPROTO_TCP, TCP_NODELAY, &got, &gl) == 0 && got == 1,
          "lo: TCP_NODELAY accepted");
    if (c < 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return;
    }

    struct lo_writer w = {c, false};
    pthread_t writer;
    bool started = pthread_create(&writer, NULL, lo_writer_main, &w) == 0;
    static uint8_t buf[65536];
    size_t total = 0;
    bool match = true;
    ssize_t n;
    while (started && (n = read(c, buf, sizeof buf)) > 0) {
        if (total + (size_t)n > LO_ECHO_BYTES || !same_bytes(buf, lo_data + total, (size_t)n))
            match = false;
        total += (size_t)n;
    }
    if (started)
        pthread_join(writer, NULL);
    close(c);
    int status = -1;
    waitpid(pid, &status, 0);
    long ms = ms_since(&t0);
    CHECK(started && w.ok && match && total == LO_ECHO_BYTES && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "lo: TCP echo, 4 MiB each way, bytes checked");
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0) || total != LO_ECHO_BYTES)
        printf("utest: lo: echo got %zu bytes, server status %#x\n", total, status);
    printf("utest: lo: 4 MiB each way in %ld ms (%ld MiB/s per direction)\n", ms, ms > 0 ? 4000L / ms : 0L);

    /* The listener is gone and the client end lingers in TIME_WAIT. */
    int s2 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = lo_sin(INADDR_LOOPBACK, port);
    got = 0;
    gl = sizeof got;
    CHECK(s2 >= 0 && setsockopt(s2, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0 &&
              getsockopt(s2, SOL_SOCKET, SO_REUSEADDR, &got, &gl) == 0 && got == 1 &&
              bind(s2, (struct sockaddr *)&a, sizeof a) == 0 && listen(s2, 4) == 0,
          "lo: SO_REUSEADDR, rebind the port right after");
    if (s2 >= 0)
        close(s2);
}

/* Echo child for one of the concurrent connections. */
static void lo_echo_until_eof(int c) {
    static uint8_t buf[16384];
    ssize_t n;
    while ((n = read(c, buf, sizeof buf)) > 0)
        if (!write_all(c, buf, (size_t)n))
            _exit(1);
    _exit(n == 0 ? 0 : 2);
}

#define LO_CONNS 16
#define LO_CONN_BYTES 40000

static void lo_concurrent_test(void) {
    uint16_t port = 0;
    int ls = lo_server(INADDR_ANY, &port); /* a wildcard listener, reached through 127.0.0.1 */
    if (ls < 0) {
        CHECK(0, "lo: 16 concurrent connections (listen)");
        return;
    }
    pid_t server = fork();
    if (server == 0) {
        pid_t kids[LO_CONNS];
        int bad = 0;
        for (int i = 0; i < LO_CONNS; i++) {
            int c = accept(ls, NULL, NULL);
            if (c < 0)
                _exit(100);
            kids[i] = fork();
            if (kids[i] == 0) {
                close(ls);
                lo_echo_until_eof(c);
            }
            close(c);
        }
        close(ls);
        for (int i = 0; i < LO_CONNS; i++) {
            int st = -1;
            if (kids[i] < 0 || waitpid(kids[i], &st, 0) != kids[i] || !WIFEXITED(st) || WEXITSTATUS(st))
                bad++;
        }
        _exit(bad);
    }
    close(ls);

    /* All sixteen are open before any data moves. */
    int fds[LO_CONNS];
    bool ok = true;
    for (int i = 0; i < LO_CONNS; i++) {
        fds[i] = lo_connect(INADDR_LOOPBACK, port);
        ok = ok && fds[i] >= 0;
    }
    static uint8_t out[LO_CONN_BYTES], in[LO_CONN_BYTES];
    for (int i = 0; i < LO_CONNS && ok; i++) {
        fill_pattern(out, sizeof out, 0, 0x100 + (uint32_t)i);
        ok = write_all(fds[i], out, sizeof out);
    }
    for (int i = 0; i < LO_CONNS && ok; i++) {
        fill_pattern(out, sizeof out, 0, 0x100 + (uint32_t)i);
        ok = read_all(fds[i], in, sizeof in) && !memcmp(in, out, sizeof in);
    }
    for (int i = 0; i < LO_CONNS; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    int status = -1;
    waitpid(server, &status, 0);
    CHECK(ok && WIFEXITED(status) && WEXITSTATUS(status) == 0, "lo: 16 concurrent connections, 40000 bytes echoed on each");
}

static void lo_refused_test(void) {
    uint16_t port = 0;
    int tmp = lo_server(INADDR_LOOPBACK, &port); /* find a free port, then free it again */
    if (tmp >= 0)
        close(tmp);
    errno = 0;
    int fd = lo_connect(INADDR_LOOPBACK, port);
    CHECK(tmp >= 0 && fd < 0 && errno == ECONNREFUSED, "lo: connect to a closed port: ECONNREFUSED");
    if (fd >= 0)
        close(fd);

    /* Non-blocking: EINPROGRESS, then poll reports it and SO_ERROR names it. */
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in a = lo_sin(INADDR_LOOPBACK, port);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    int e = errno;
    struct pollfd p = {fd, POLLOUT, 0};
    int err = 0;
    socklen_t el = sizeof err;
    CHECK(r < 0 && e == EINPROGRESS && poll(&p, 1, 1000) == 1 && (p.revents & (POLLOUT | POLLERR)) &&
              getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == ECONNREFUSED,
          "lo: non-blocking connect to a closed port: SO_ERROR is ECONNREFUSED");
    close(fd);
}

static void lo_udp_test(void) {
    int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a1 = lo_sin(INADDR_LOOPBACK, 0), from, a2;
    socklen_t len = sizeof a1, fl = sizeof from, l2 = sizeof a2;
    bool ok = u1 >= 0 && u2 >= 0 && bind(u1, (struct sockaddr *)&a1, sizeof a1) == 0 &&
              getsockname(u1, (struct sockaddr *)&a1, &len) == 0 && is_addr(&a1, INADDR_LOOPBACK, 0) && a1.sin_port;
    char buf[64] = {0};
    ok = ok && sendto(u2, "ping", 4, 0, (struct sockaddr *)&a1, sizeof a1) == 4 &&
         getsockname(u2, (struct sockaddr *)&a2, &l2) == 0 &&
         recvfrom(u1, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl) == 4 && !memcmp(buf, "ping", 4) &&
         is_addr(&from, INADDR_LOOPBACK, ntohs(a2.sin_port));
    ok = ok && sendto(u1, "pong", 4, 0, (struct sockaddr *)&from, fl) == 4 &&
         recv(u2, buf, sizeof buf, 0) == 4 && !memcmp(buf, "pong", 4);
    CHECK(ok, "lo: UDP round trip on 127.0.0.1");

    /* Loopback's MTU is 64 KiB: a large datagram arrives whole. */
    static uint8_t big[60000], back[65536];
    fill_pattern(big, sizeof big, 0, 0x55);
    ok = ok && sendto(u2, big, sizeof big, 0, (struct sockaddr *)&a1, sizeof a1) == (ssize_t)sizeof big &&
         recv(u1, back, sizeof back, 0) == (ssize_t)sizeof big && !memcmp(big, back, sizeof big);
    CHECK(ok, "lo: 60000-byte UDP datagram in one piece");
    if (u1 >= 0)
        close(u1);
    if (u2 >= 0)
        close(u2);
}

/* Readiness through epoll and poll, and non-blocking flow control. */
static void lo_epoll_test(void) {
    uint16_t port = 0;
    int ls = lo_server(INADDR_LOOPBACK, &port);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 1}, out[4];
    bool ok = ls >= 0 && ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev) == 0 && epoll_wait(ep, out, 4, 0) == 0;
    int c = ok ? lo_connect(INADDR_LOOPBACK, port) : -1;
    ok = ok && c >= 0 && epoll_wait(ep, out, 4, 1000) == 1 && out[0].data.u32 == 1 && (out[0].events & EPOLLIN);
    int a = ok ? accept4(ls, NULL, NULL, SOCK_NONBLOCK) : -1;
    ev = (struct epoll_event){.events = EPOLLIN | EPOLLRDHUP, .data.u32 = 2};
    ok = ok && a >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, a, &ev) == 0 && epoll_wait(ep, out, 4, 0) == 0;
    char buf[16];
    ok = ok && read(a, buf, sizeof buf) < 0 && errno == EAGAIN;
    ok = ok && write(c, "x", 1) == 1 && epoll_wait(ep, out, 4, 1000) == 1 && out[0].data.u32 == 2 &&
         out[0].events == EPOLLIN && read(a, buf, sizeof buf) == 1;
    CHECK(ok, "lo: epoll: listener readable on connect, socket readable on data");

    /* Fill the pipe without blocking: the writer's buffer plus the reader's
     * window, then EAGAIN and no POLLOUT until the reader drains some. */
    size_t sent = 0;
    static uint8_t chunk[65536];
    ssize_t n;
    while ((n = write(a, chunk, sizeof chunk)) > 0)
        sent += (size_t)n;
    int e = errno;
    struct pollfd p = {a, POLLOUT, 0};
    bool full = n < 0 && e == EAGAIN && sent >= 128 * 1024 && poll(&p, 1, 0) == 0;
    /* Keep the window shut a while: the writer's zero-window probes (after
     * 250 and 500 ms) must neither break nor stall the connection. */
    struct timespec nap = {0, 600 * 1000 * 1000};
    nanosleep(&nap, NULL);
    size_t drained = 0;
    while (drained < sent / 2 && (n = read(c, chunk, sizeof chunk)) > 0)
        drained += (size_t)n;
    p.revents = 0;
    full = full && poll(&p, 1, 1000) == 1 && (p.revents & POLLOUT);
    CHECK(full, "lo: non-blocking write fills the window (EAGAIN), POLLOUT once drained");
    if (!full)
        printf("utest: lo: sent %zu before errno %d, drained %zu\n", sent, e, drained);
    while (drained < sent && (n = read(c, chunk, sizeof chunk)) > 0)
        drained += (size_t)n;

    /* The peer's close: EPOLLIN | EPOLLRDHUP, then EOF. */
    CHECK(full && drained == sent, "lo: every byte arrives after the window reopens");
    close(c);
    ok = epoll_wait(ep, out, 4, 1000) == 1 && out[0].data.u32 == 2 && (out[0].events & EPOLLRDHUP) &&
         (out[0].events & EPOLLIN) && read(a, buf, sizeof buf) == 0;
    CHECK(ok, "lo: epoll: EPOLLRDHUP and EOF after the peer closes");
    if (a >= 0)
        close(a);
    if (ls >= 0)
        close(ls);
    if (ep >= 0)
        close(ep);
}

/* busybox httpd serving a file and busybox wget fetching it. */
static void lo_httpd_test(void) {
    static const char body[] = "Hello from busybox httpd over the loopback.\n";
    mkdir("/tmp/www", 0755);
    int fd = open("/tmp/www/x", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0 && write(fd, body, sizeof body - 1) == (ssize_t)(sizeof body - 1);
    if (fd >= 0)
        close(fd);
    /* -f keeps it in the foreground, so we hold its pid and can stop it. */
    pid_t httpd = fork();
    if (httpd == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0), dup2(null, 1), dup2(null, 2);
        execl("/bin/busybox", "httpd", "-f", "-p", "127.0.0.1:8081", "-h", "/tmp/www", (char *)NULL);
        _exit(127);
    }
    int up = -1;
    for (int i = 0; i < 100 && up < 0; i++) { /* until it listens */
        up = lo_connect(INADDR_LOOPBACK, 8081);
        if (up < 0) {
            struct timespec t = {0, 20 * 1000 * 1000};
            nanosleep(&t, NULL);
        }
    }
    if (up >= 0)
        close(up);
    char out[4096];
    char *wget[] = {"/bin/busybox", "wget", "-q", "-O", "-", "http://127.0.0.1:8081/x", NULL};
    int r = up >= 0 ? run(wget, out, sizeof out) : -1;
    CHECK(ok && up >= 0 && r == 0 && !strcmp(out, body), "lo: busybox wget from busybox httpd on 127.0.0.1:8081");
    if (r != 0 || strcmp(out, body))
        printf("utest: lo: wget exit %d, said: %s\n", r, up >= 0 ? out : "(httpd not listening)");
    kill(httpd, SIGTERM);
    waitpid(httpd, NULL, 0);

    /* The name, not just the number: /etc/hosts maps localhost to 127.0.0.1. */
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    int gai = getaddrinfo("localhost", "8080", &hints, &res);
    CHECK(gai == 0 && res && is_addr((struct sockaddr_in *)res->ai_addr, INADDR_LOOPBACK, 8080),
          "lo: getaddrinfo(localhost) is 127.0.0.1");
    if (gai == 0)
        freeaddrinfo(res);
}

static void lo_tests(void) {
    lo_data = malloc(LO_ECHO_BYTES);
    if (!lo_data) {
        CHECK(0, "lo: malloc 4 MiB");
        return;
    }
    fill_pattern(lo_data, LO_ECHO_BYTES, 0, 0x10);
    lo_throughput_test();
    lo_chaos_test();
    lo_echo_test();
    lo_concurrent_test();
    lo_refused_test();
    lo_udp_test();
    lo_epoll_test();
    lo_httpd_test();
    free(lo_data);
}

/* After DHCP: the NIC's own address is delivered locally too. */
static void lo_own_address_test(void) {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = lo_sin(0x08080808, 53);
    socklen_t len = sizeof a;
    uint32_t me = 0;
    if (u >= 0 && connect(u, (struct sockaddr *)&a, sizeof a) == 0 &&
        getsockname(u, (struct sockaddr *)&a, &len) == 0)
        me = ntohl(a.sin_addr.s_addr); /* the source address the route picks */
    if (u >= 0)
        close(u);
    uint16_t port = 0;
    int ls = me ? lo_server(INADDR_ANY, &port) : -1;
    int c = ls >= 0 ? lo_connect(me, port) : -1;
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int s = c >= 0 ? accept(ls, (struct sockaddr *)&peer, &pl) : -1;
    char buf[8] = {0};
    bool ok = s >= 0 && is_addr(&peer, me, 0) && write(c, "mine", 4) == 4 && read(s, buf, sizeof buf) == 4 &&
              !memcmp(buf, "mine", 4);
    char what[96];
    snprintf(what, sizeof what, "lo: TCP to this machine's own address %u.%u.%u.%u", me >> 24, (me >> 16) & 0xff,
             (me >> 8) & 0xff, me & 0xff);
    CHECK(me && me >> 24 != 127 && ok, what);
    if (s >= 0)
        close(s);
    if (c >= 0)
        close(c);
    if (ls >= 0)
        close(ls);
}

/* /dev/sda as a Linux block device. The test harness attaches a scratch
 * disk; without one (other launchers) the tests are skipped. */
static void disk_tests(void) {
    int fd = open("/dev/sda", O_RDWR);
    if (fd < 0) {
        printf("utest: disk tests skipped (no disk)\n");
        return;
    }
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISBLK(st.st_mode) && major(st.st_rdev) == 8 && minor(st.st_rdev) == 0,
          "disk: /dev/sda is block device 8:0");
    uint64_t size = 0;
    int ssz = 0;
    CHECK(ioctl(fd, BLKGETSIZE64, &size) == 0 && size % 512 == 0 && (uint64_t)st.st_size == size,
          "disk: BLKGETSIZE64 matches fstat");
    CHECK(ioctl(fd, BLKSSZGET, &ssz) == 0 && (ssz == 512 || ssz == 4096), "disk: BLKSSZGET");
    CHECK(lseek(fd, 0, SEEK_END) == (off_t)size && lseek(fd, 0, SEEK_SET) == 0, "disk: lseek SEEK_END");
    if (size < (4u << 20)) {
        printf("utest: disk read/write tests skipped (disk smaller than 4 MiB)\n");
        close(fd);
        return;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint32_t seed = (uint32_t)now.tv_nsec ^ (uint32_t)now.tv_sec ^ ((uint32_t)getpid() << 16);
    /* Longer than one kernel bounce chunk (64 KiB), not a whole number of
     * sectors. */
    size_t big = 200000;
    uint8_t *w = malloc(big), *r = malloc(big);
    if (!w || !r) {
        CHECK(0, "disk: buffers");
        close(fd);
        return;
    }
    fill_pattern(w, big, 0, seed);
    CHECK(write(fd, w, big) == (ssize_t)big && lseek(fd, 0, SEEK_CUR) == (off_t)big, "disk: write at offset 0");
    memset(r, 0, big);
    CHECK(pread(fd, r, big, 0) == (ssize_t)big && !memcmp(r, w, big), "disk: read back offset 0");

    /* An unaligned write across the 1 MiB mark over a known background: the
     * bytes sharing its first and last sectors must survive. */
    off_t base = (1 << 20) - 4096, off = (1 << 20) - 333;
    size_t region = 12288, len = 5000;
    fill_pattern(w, region, (uint64_t)base, ~seed);
    bool ok = pwrite(fd, w, region, base) == (ssize_t)region;
    fill_pattern(w + (off - base), len, (uint64_t)off, seed);
    ok = ok && pwrite(fd, w + (off - base), len, off) == (ssize_t)len;
    memset(r, 0, region);
    CHECK(ok && pread(fd, r, region, base) == (ssize_t)region && !memcmp(r, w, region),
          "disk: unaligned write near 1 MiB keeps its neighbours");
    memset(r, 0, len);
    CHECK(lseek(fd, off, SEEK_SET) == off && read(fd, r, len) == (ssize_t)len && !memcmp(r, w + (off - base), len) &&
              lseek(fd, 0, SEEK_CUR) == off + (off_t)len,
          "disk: unaligned read() advances the offset");
    CHECK(fsync(fd) == 0 && fdatasync(fd) == 0 && ioctl(fd, BLKFLSBUF, 0) == 0, "disk: fsync, fdatasync");

    char tail[1024];
    CHECK(pread(fd, tail, sizeof tail, (off_t)size - 512) == 512 && pread(fd, tail, sizeof tail, (off_t)size) == 0,
          "disk: reads stop at the end");
    errno = 0;
    CHECK(pwrite(fd, tail, 1, (off_t)size) < 0 && errno == ENOSPC, "disk: write past the end fails (ENOSPC)");
    struct pollfd pf = {fd, POLLIN | POLLOUT, 0};
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & (POLLIN | POLLOUT)) == (POLLIN | POLLOUT), "disk: poll ready");
    close(fd);

    /* A fresh open sees the data (there is no cache: it comes off the disk). */
    fd = open("/dev/sda", O_RDONLY);
    fill_pattern(w, 4096, 0, seed);
    CHECK(fd >= 0 && read(fd, r, 4096) == 4096 && !memcmp(r, w, 4096), "disk: reopen and read");
    if (fd >= 0)
        close(fd);
    char out[256];
    char *dd[] = {"/bin/sh", "-c", "dd if=/dev/sda bs=4096 count=3 2>/dev/null | wc -c", NULL};
    CHECK(run(dd, out, sizeof out) == 0 && atoi(out) == 3 * 4096, "disk: busybox dd reads /dev/sda");
    free(w);
    free(r);
}

static void sleep_ms(long ms);
/* The time this process has spent in the kernel, in seconds (for the speed modes). */
static double kernel_cpu_s(void) {
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (double)r.ru_stime.tv_sec + (double)r.ru_stime.tv_usec / 1e6;
}
/* OS_MODE=wake-latency (tests/wake_latency_win.py): how long a program waiting for something takes to notice
 * it. Two processes pass a byte back and forth over pipes, each waiting in poll (as D-Bus, Wayland apps and
 * browsers wait), then in epoll; and a 1 ms sleep, timed. Printed as PERF lines (the Performance fixes panel). */
static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}
static void wake_latency_mode(void) {
    for (int how = 0; how < 2; how++) { /* 0: poll, 1: epoll */
        int ab[2], ba[2];
        if (pipe(ab) < 0 || pipe(ba) < 0)
            return;
        const int rounds = 200;
        pid_t c = fork();
        if (c == 0) {
            int ep = how ? epoll_create1(0) : -1;
            struct epoll_event ev = {.events = EPOLLIN};
            if (how)
                epoll_ctl(ep, EPOLL_CTL_ADD, ab[0], &ev);
            char b;
            for (int i = 0; i < rounds; i++) {
                if (how) {
                    struct epoll_event got;
                    epoll_wait(ep, &got, 1, -1);
                } else {
                    struct pollfd p = {ab[0], POLLIN, 0};
                    poll(&p, 1, -1);
                }
                if (read(ab[0], &b, 1) != 1 || write(ba[1], &b, 1) != 1)
                    _exit(1);
            }
            _exit(0);
        }
        char b = 'x';
        double t0 = now_us();
        for (int i = 0; i < rounds; i++) {
            struct pollfd p = {ba[0], POLLIN, 0};
            if (write(ab[1], &b, 1) != 1)
                break;
            poll(&p, 1, -1);
            if (read(ba[0], &b, 1) != 1)
                break;
        }
        double per = (now_us() - t0) / rounds;
        waitpid(c, NULL, 0);
        printf("PERF %s_round_trip_us=%.0f\n", how ? "epoll" : "poll", per);
        close(ab[0]), close(ab[1]), close(ba[0]), close(ba[1]);
    }
    double t0 = now_us();
    for (int i = 0; i < 50; i++) {
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }
    printf("PERF sleep_1ms_us=%.0f\n", (now_us() - t0) / 50);
    for (volatile long i = 0; i < 300000000; i++) /* some time of its own to report (a few hundred ms) */
        ;
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("PERF getrusage_works=%d\n", ru.ru_utime.tv_sec || ru.ru_utime.tv_usec || ru.ru_stime.tv_sec || ru.ru_stime.tv_usec);
    fflush(stdout);
}

/* OS_MODE=net-speed:PORT (tests/net_speed_win.py): a plain HTTP GET of /big from the PC (10.0.2.2:PORT), timed,
 * nothing else in the way: how fast our network card driver and TCP are, apart from Flatpak and the disk. */
static void net_speed_mode(int port, const char *host, const char *path) {
    for (int i = 0; i < 100 && access("/etc/resolv.conf", F_OK) != 0; i++)
        sleep_ms(100);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "10.0.2.2", &a.sin_addr);
    if (host && host[0]) { /* a server on the internet, by name */
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *ai = NULL;
        if (getaddrinfo(host, NULL, &hints, &ai) != 0 || !ai) {
            printf("net-speed: %s not found\n", host);
            return;
        }
        a.sin_addr = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
        freeaddrinfo(ai);
    }
    struct timespec t0, t1, tf;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("net-speed: can't connect (%s)\n", strerror(errno));
        return;
    }
    char req[600];
    int rl = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: os-net-speed\r\n\r\n",
                      path && path[0] ? path : "/big", host && host[0] ? host : "10.0.2.2");
    if (write(s, req, (size_t)rl) != rl)
        printf("net-speed: couldn't ask\n");
    static char buf[1 << 16];
    long long got = 0;
    ssize_t n;
    bool first = true;
    double k0 = kernel_cpu_s();
    while ((n = read(s, buf, 16384)) > 0) { /* (as curl reads) */
        if (first) { /* the answer's first line, to tell a redirect or a refusal from a download */
            clock_gettime(CLOCK_MONOTONIC, &tf), first = false;
            printf("net-speed: the answer begins: %.*s\n", (int)strcspn(buf, "\r\n") < 120 ? (int)strcspn(buf, "\r\n") : 120,
                   buf);
        }
        got += n;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    close(s);
    double secs = (double)(t1.tv_sec - tf.tv_sec) + (double)(t1.tv_nsec - tf.tv_nsec) / 1e9;
    printf("net-speed: %.1f MB in %.2f s = %.1f MB/s (first byte after %ld ms), %.2f s of kernel time\n",
           (double)got / 1e6, secs, secs > 0 ? (double)got / 1e6 / secs : 0, ms_since(&t0) - (long)(secs * 1000),
           kernel_cpu_s() - k0);
}

/* OS_MODE=persist-write:TOKEN, then persist-check:TOKEN on the next boot
 * (tests/disk_persist_test.sh): the first boot leaves TOKEN on the disk and
 * powers off; the second must find it. */
#define PERSIST_OFFSET ((8 << 20) + 123) /* deliberately not sector aligned */

static void persist_mode(const char *mode) {
    bool writing = !strncmp(mode, "persist-write:", 14);
    const char *what = writing ? "WRITE" : "CHECK";
    char want[96], got[96] = {0};
    int n = snprintf(want, sizeof want, "OS-PERSIST %s\n", strchr(mode, ':') ? strchr(mode, ':') + 1 : "");
    int fd = open("/dev/sda", O_RDWR);
    if (fd < 0) {
        printf("PERSIST %s FAILED (no /dev/sda: %s)\n", what, strerror(errno));
        return;
    }
    bool ok;
    if (writing) {
        ok = pwrite(fd, want, (size_t)n, PERSIST_OFFSET) == n && fsync(fd) == 0;
    } else {
        ok = pread(fd, got, (size_t)n, PERSIST_OFFSET) == n && !memcmp(got, want, (size_t)n);
        for (int i = 0; i < n; i++)
            if ((unsigned char)got[i] < 0x20 || (unsigned char)got[i] > 0x7e)
                got[i] = '.';
    }
    close(fd);
    if (ok)
        printf("PERSIST %s OK\n", what);
    else if (writing)
        printf("PERSIST WRITE FAILED (%s)\n", strerror(errno));
    else
        printf("PERSIST CHECK FAILED (found \"%s\")\n", got);
}

static long ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

/* ---- sound: /dev/dsp (OSS) on the HDA driver ---- */

#define SNDCTL_DSP_RESET     0x5000
#define SNDCTL_DSP_SYNC      0x5001
#define SNDCTL_DSP_SPEED     0xc0045002
#define SNDCTL_DSP_SETFMT    0xc0045005
#define SNDCTL_DSP_CHANNELS  0xc0045006
#define SNDCTL_DSP_GETOSPACE 0x8010500c
#define SOUND_MIXER_READ_VOLUME 0x80044d00
#define AFMT_S16_LE 0x10

struct audio_buf_info {
    int fragments, fragstotal, fragsize, bytes;
};

static int dsp_open(int flags, int rate, int channels) {
    int fd = open("/dev/dsp", flags);
    if (fd < 0)
        return -1;
    int fmt = AFMT_S16_LE, ch = channels, r = rate;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &fmt) < 0 || fmt != AFMT_S16_LE || ioctl(fd, SNDCTL_DSP_CHANNELS, &ch) < 0 ||
        ch != channels || ioctl(fd, SNDCTL_DSP_SPEED, &r) < 0 || r != rate) {
        close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

/* A sine tone as interleaved signed 16-bit PCM. One extra frame closes the
 * last cycle on a zero sample, so the tone ends without a step (the host's
 * audio check looks for steps). */
static int16_t *make_tone(int rate, int channels, double hz, double seconds, size_t *bytes) {
    int frames = (int)(rate * seconds) + 1;
    int16_t *pcm = malloc((size_t)frames * (size_t)channels * 2);
    for (int i = 0; i < frames; i++) {
        int16_t s = (int16_t)(12000 * sin(2 * M_PI * hz * i / rate));
        for (int c = 0; c < channels; c++)
            pcm[i * channels + c] = s;
    }
    *bytes = (size_t)frames * (size_t)channels * 2;
    return pcm;
}

static void on_alarm(int sig) { (void)sig; }

static void audio_tests(void) {
    int fd = open("/dev/dsp", O_RDWR);
    if (fd < 0) {
        printf("utest: sound skipped (no /dev/dsp: %s)\n", strerror(errno));
        return;
    }
    close(fd);
    /* A hang must fail the test, not stall it: reads get interrupted. */
    struct sigaction sa = {0}, old;
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, &old);

    fd = dsp_open(O_RDWR, 48000, 2);
    CHECK(fd >= 0, "dsp 48 kHz stereo s16le");
    size_t bytes;
    int16_t *tone = make_tone(48000, 2, 440, 0.5, &bytes);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    alarm(10);
    CHECK(write(fd, tone, bytes) == (ssize_t)bytes, "dsp write 0.5 s of 440 Hz");
    CHECK(ioctl(fd, SNDCTL_DSP_SYNC, 0) == 0, "dsp sync");
    long ms = ms_since(&t0);
    printf("utest: dsp played 0.5 s in %ld ms\n", ms);
    CHECK(ms >= 350 && ms < 5000, "dsp plays in real time");
    struct audio_buf_info info;
    CHECK(ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0 && info.bytes > 0 && info.fragsize > 0, "dsp GETOSPACE");
    struct pollfd p = {fd, POLLIN, 0};
    CHECK(poll(&p, 1, 3000) == 1 && (p.revents & POLLIN), "dsp poll POLLIN");
    static int16_t cap[4800 * 2]; /* 0.1 s */
    CHECK(read(fd, cap, sizeof cap) == (ssize_t)sizeof cap, "dsp read captured PCM");
    alarm(0);
    CHECK(ioctl(fd, SNDCTL_DSP_RESET, 0) == 0, "dsp reset");
    close(fd);
    free(tone);

    /* The speech formats: 16 kHz mono capture, 22.05 kHz mono playback
     * (converted by the driver), and non-blocking mode. */
    fd = dsp_open(O_RDONLY | O_NONBLOCK, 16000, 1);
    CHECK(fd >= 0, "dsp 16 kHz mono capture");
    int got = 0;
    for (int i = 0; i < 300 && got < 3200; i++) {
        ssize_t n = read(fd, (char *)cap + got, 3200 - got);
        if (n > 0)
            got += (int)n;
        else if (n < 0 && errno != EAGAIN)
            break;
        else
            sleep_ms(10);
    }
    CHECK(got == 3200, "dsp non-blocking read 0.1 s");
    close(fd);
    fd = dsp_open(O_WRONLY, 22050, 1);
    tone = make_tone(22050, 1, 880, 0.2, &bytes);
    alarm(10);
    CHECK(fd >= 0 && write(fd, tone, bytes) == (ssize_t)bytes && close(fd) == 0, "dsp 22.05 kHz mono write+close");
    alarm(0);
    free(tone);
    int mixfd = open("/dev/mixer", O_RDONLY), vol = 0;
    CHECK(mixfd >= 0 && ioctl(mixfd, SOUND_MIXER_READ_VOLUME, &vol) == 0 && (vol & 0xff) == 100, "mixer volume");
    close(mixfd);
    sigaction(SIGALRM, &old, NULL);
}

/* OS_MODE=audio (tests/audio_test.sh): plays 1 s of 440 Hz at 48 kHz
 * stereo, then 1 s of 880 Hz as 22.05 kHz mono, which the driver converts.
 * The host checks the recording QEMU made. */
static void audio_demo(void) {
    size_t bytes;
    int fd = dsp_open(O_WRONLY, 48000, 2);
    int16_t *tone = make_tone(48000, 2, 440, 1.0, &bytes);
    if (fd < 0 || write(fd, tone, bytes) != (ssize_t)bytes || ioctl(fd, SNDCTL_DSP_SYNC, 0) < 0)
        printf("audio: 440 Hz failed (%s)\n", strerror(errno));
    close(fd);
    free(tone);
    fd = dsp_open(O_WRONLY, 22050, 1);
    tone = make_tone(22050, 1, 880, 1.0, &bytes);
    if (fd < 0 || write(fd, tone, bytes) != (ssize_t)bytes)
        printf("audio: 880 Hz failed (%s)\n", strerror(errno));
    close(fd); /* drains */
    free(tone);
    sleep_ms(500); /* the emulator's own audio buffers play out before power-off */
    printf("AUDIO TEST DONE\n");
}

/* Sends `fd` over a unix socket with one byte of data, as Wayland does. */
static int send_fd(int sock, int fd) {
    char byte = 'F';
    struct iovec iov = {&byte, 1};
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf};
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof fd);
    return sendmsg(sock, &mh, MSG_NOSIGNAL) == 1 ? 0 : -1;
}

static int recv_fd(int sock) {
    char byte;
    struct iovec iov = {&byte, 1};
    union {
        char buf[CMSG_SPACE(sizeof(int) * 4)];
        struct cmsghdr align;
    } u;
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf};
    if (recvmsg(sock, &mh, MSG_CMSG_CLOEXEC) != 1 || byte != 'F' || (mh.msg_flags & MSG_CTRUNC))
        return -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS && cm->cmsg_len == CMSG_LEN(sizeof(int))) {
            int fd;
            memcpy(&fd, CMSG_DATA(cm), sizeof fd);
            return fd;
        }
    return -1;
}

/* What Wayland clients and their toolkits need from the kernel: unix
 * sockets with descriptor passing, memfd, eventfd, epoll and timerfd. */
static void ipc_tests(void) {
    char buf[64];

    int sv[2];
    bool ok = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0 && write(sv[0], "ping", 4) == 4 &&
              read(sv[1], buf, sizeof buf) == 4 && !memcmp(buf, "ping", 4) && write(sv[1], "pong!", 5) == 5 &&
              recv(sv[0], buf, sizeof buf, 0) == 5 && !memcmp(buf, "pong!", 5);
    if (ok) { /* EOF and EPIPE once the other end is gone */
        close(sv[1]);
        struct pollfd pf = {sv[0], POLLIN, 0};
        ok = poll(&pf, 1, 0) == 1 && (pf.revents & POLLHUP) && read(sv[0], buf, sizeof buf) == 0 &&
             send(sv[0], "x", 1, MSG_NOSIGNAL) < 0 && errno == EPIPE;
        close(sv[0]);
    }
    CHECK(ok, "unix socketpair round trip");

    ok = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0 && write(sv[0], "one", 3) == 3 && write(sv[0], "two", 3) == 3 &&
         read(sv[1], buf, sizeof buf) == 3 && !memcmp(buf, "one", 3) && read(sv[1], buf, sizeof buf) == 3 &&
         !memcmp(buf, "two", 3);
    close(sv[0]);
    close(sv[1]);
    CHECK(ok, "unix seqpacket keeps message boundaries");

    /* A listening socket in the file system, a forked client. */
    unlink("/tmp/test.sock");
    struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = "/tmp/test.sock"};
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct stat st;
    ok = ls >= 0 && bind(ls, (struct sockaddr *)&addr, sizeof addr) == 0 && listen(ls, 4) == 0 &&
         stat("/tmp/test.sock", &st) == 0 && S_ISSOCK(st.st_mode);
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        char reply[16] = {0};
        if (c < 0 || connect(c, (struct sockaddr *)&addr, sizeof addr) < 0 || write(c, "hello", 5) != 5 ||
            read(c, reply, sizeof reply) != 5 || strcmp(reply, "world"))
            _exit(1);
        _exit(0);
    }
    struct sockaddr_un peer;
    socklen_t plen = sizeof peer;
    int as = ok ? accept4(ls, (struct sockaddr *)&peer, &plen, SOCK_CLOEXEC) : -1;
    struct ucred cred = {0};
    socklen_t clen = sizeof cred;
    ok = as >= 0 && plen == sizeof(sa_family_t) &&
         getsockopt(as, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0 && cred.pid == pid &&
         read(as, buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5) && write(as, "world", 5) == 5;
    int status = -1;
    waitpid(pid, &status, 0);
    struct sockaddr_un name;
    socklen_t nlen = sizeof name;
    ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0 && getsockname(as, (struct sockaddr *)&name, &nlen) == 0 &&
         !strcmp(name.sun_path, "/tmp/test.sock");
    close(as);
    close(ls);
    unlink("/tmp/test.sock");
    CHECK(ok, "unix listen/connect/accept at /tmp/test.sock");

    /* SCM_RIGHTS: a child sends the read end of its pipe; we read through it. */
    ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
    pid = fork();
    if (pid == 0) {
        int p[2];
        if (pipe(p) < 0 || write(p[1], "through a passed fd", 19) != 19 || send_fd(sv[1], p[0]) < 0)
            _exit(1);
        _exit(0); /* our copies close; the one in flight keeps the pipe alive */
    }
    close(sv[1]);
    int got = ok ? recv_fd(sv[0]) : -1;
    memset(buf, 0, sizeof buf);
    ok = got >= 0 && (fcntl(got, F_GETFD) & FD_CLOEXEC) && read(got, buf, sizeof buf) == 19 &&
         !strcmp(buf, "through a passed fd") && read(got, buf, sizeof buf) == 0;
    waitpid(pid, &status, 0);
    ok = ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (got >= 0)
        close(got);
    close(sv[0]);
    CHECK(ok, "SCM_RIGHTS passes a pipe between processes");

    /* memfd: write, grow, map privately, seal. */
    int mfd = memfd_create("utest", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ok = mfd >= 0 && write(mfd, "shared pixels", 13) == 13 && ftruncate(mfd, 8192) == 0 &&
         fstat(mfd, &st) == 0 && st.st_size == 8192;
    char *map = ok ? mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, mfd, 0) : MAP_FAILED;
    ok = map != MAP_FAILED && !memcmp(map, "shared pixels", 13) && map[5000] == 0;
    if (map != MAP_FAILED)
        munmap(map, 8192);
    ok = ok && posix_fallocate(mfd, 0, 16384) == 0 && fstat(mfd, &st) == 0 && st.st_size == 16384 &&
         fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0 && ftruncate(mfd, 4096) < 0 && errno == EPERM &&
         pwrite(mfd, "x", 1, 20000) < 0 && errno == EPERM && pwrite(mfd, "y", 1, 100) == 1 &&
         fcntl(mfd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SEAL) == 0 && pwrite(mfd, "z", 1, 0) < 0 && errno == EPERM &&
         fcntl(mfd, F_GET_SEALS) == (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) &&
         fcntl(mfd, F_ADD_SEALS, F_SEAL_GROW) < 0 && errno == EPERM;
    close(mfd);
    int shm = shm_open("/os-utest", O_RDWR | O_CREAT | O_EXCL, 0600);
    ok = ok && shm >= 0 && ftruncate(shm, 4096) == 0 && shm_unlink("/os-utest") == 0;
    if (shm >= 0)
        close(shm);
    CHECK(ok, "memfd write + ftruncate + mmap + seals, shm_open");

    /* eventfd: a counter; semaphore mode hands out ones. */
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uint64_t v = 0, three = 3, four = 4;
    ok = efd >= 0 && read(efd, &v, 8) < 0 && errno == EAGAIN && write(efd, &three, 8) == 8 &&
         write(efd, &four, 8) == 8 && read(efd, &v, 8) == 8 && v == 7 && read(efd, &v, 8) < 0 && errno == EAGAIN;
    close(efd);
    efd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    ok = ok && efd >= 0 && read(efd, &v, 8) == 8 && v == 1 && read(efd, &v, 8) == 8 && v == 1 &&
         read(efd, &v, 8) < 0 && errno == EAGAIN;
    close(efd);
    CHECK(ok, "eventfd counter semantics");

    /* epoll over a pipe and an eventfd. */
    int ep = epoll_create1(EPOLL_CLOEXEC), p[2];
    efd = eventfd(0, EFD_NONBLOCK);
    ok = ep >= 0 && efd >= 0 && pipe(p) == 0;
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 1}, out[4];
    ok = ok && epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0;
    ev.data.u32 = 2;
    ok = ok && epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0 && epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) < 0 &&
         errno == EEXIST;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ok = ok && epoll_wait(ep, out, 4, 50) == 0 && ms_since(&t0) >= 45;
    ok = ok && write(efd, &three, 8) == 8 && epoll_wait(ep, out, 4, 1000) == 1 && out[0].data.u32 == 2 &&
         (out[0].events & EPOLLIN) && epoll_wait(ep, out, 4, 0) == 1 && read(efd, &v, 8) == 8 &&
         epoll_wait(ep, out, 4, 0) == 0;
    pid = fork();
    if (pid == 0) {
        sleep_ms(30);
        _exit(write(p[1], "!", 1) == 1 ? 0 : 1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ok = ok && epoll_wait(ep, out, 4, -1) == 1 && out[0].data.u32 == 1 && ms_since(&t0) >= 20;
    waitpid(pid, NULL, 0);
    /* edge-triggered: reported once until something new happens */
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u32 = 1;
    ok = ok && epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) == 0 && epoll_wait(ep, out, 4, 0) == 1 &&
         epoll_wait(ep, out, 4, 0) == 0 && write(p[1], "?", 1) == 1 && epoll_wait(ep, out, 4, 0) == 1;
    /* closing a registered descriptor removes it */
    close(p[0]);
    close(p[1]);
    ok = ok && epoll_wait(ep, out, 4, 0) == 0 && epoll_ctl(ep, EPOLL_CTL_DEL, efd, NULL) == 0;
    close(efd);
    close(ep);
    CHECK(ok, "epoll on a pipe and an eventfd");

    /* timerfd: fires after 50 ms; read blocks until then. */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = {.it_value = {0, 50 * 1000 * 1000}}, cur;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ok = tfd >= 0 && timerfd_settime(tfd, 0, &its, NULL) == 0 && timerfd_gettime(tfd, &cur) == 0 &&
         cur.it_value.tv_nsec > 0 && read(tfd, &v, 8) == 8 && v == 1;
    long ms = ms_since(&t0);
    ok = ok && ms >= 45 && ms < 1000;
    close(tfd);
    /* and a periodic one seen through select() */
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    its.it_interval = its.it_value = (struct timespec){0, 10 * 1000 * 1000};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(tfd, &rfds);
    struct timeval tv = {1, 0};
    ok = ok && timerfd_settime(tfd, 0, &its, NULL) == 0 && select(tfd + 1, &rfds, NULL, NULL, &tv) == 1 &&
         FD_ISSET(tfd, &rfds) && read(tfd, &v, 8) == 8 && v >= 1;
    close(tfd);
    CHECK(ok, "timerfd fires after 50 ms");
}

static volatile sig_atomic_t got_usr1;
static void on_usr1(int sig) { got_usr1 = sig; }

/* ---- threads (CLONE_THREAD, futex, per-thread signals) ---- */

#define FUTEX_WAIT_PRIVATE 128 /* FUTEX_WAIT | FUTEX_PRIVATE_FLAG (linux/futex.h) */

#define COUNTER_THREADS 8
#define COUNTER_ROUNDS 100000
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *counter_thread(void *arg) {
    (void)arg;
    for (int i = 0; i < COUNTER_ROUNDS; i++) {
        pthread_mutex_lock(&counter_lock);
        counter++;
        pthread_mutex_unlock(&counter_lock);
    }
    return (void *)(long)syscall(SYS_gettid);
}

#define PINGPONG_ROUNDS 2000
static pthread_mutex_t pp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pp_cond = PTHREAD_COND_INITIALIZER;
static int pp_turn, pp_moves; /* turn 0: ping, 1: pong */

static void *pingpong_thread(void *arg) {
    int me = (int)(long)arg;
    for (int i = 0; i < PINGPONG_ROUNDS; i++) {
        pthread_mutex_lock(&pp_lock);
        while (pp_turn != me)
            pthread_cond_wait(&pp_cond, &pp_lock);
        pp_moves++;
        pp_turn = !me;
        pthread_cond_signal(&pp_cond);
        pthread_mutex_unlock(&pp_lock);
    }
    return NULL;
}

static __thread int tls_value = 7;
static pthread_barrier_t tls_barrier;

static void *tls_thread(void *arg) {
    int id = (int)(long)arg;
    int ok = tls_value == 7; /* each thread starts from the initial image */
    tls_value = 100 + id;
    pthread_barrier_wait(&tls_barrier); /* everyone has written their copy */
    ok = ok && tls_value == 100 + id;
    return (void *)(long)ok;
}

static void *exit_group_thread(void *arg) {
    (void)arg;
    exit(7); /* exit_group from a non-main thread ends the whole process */
}

static volatile sig_atomic_t usr2_tid;
static void on_usr2(int sig) {
    (void)sig;
    usr2_tid = (sig_atomic_t)syscall(SYS_gettid);
}

struct reader_result {
    int fd;
    pid_t tid;
    ssize_t n;
    int err;
};

static void *blocked_reader(void *arg) {
    struct reader_result *r = arg;
    r->tid = (pid_t)syscall(SYS_gettid);
    char c;
    r->n = read(r->fd, &c, 1);
    r->err = errno;
    return NULL;
}

static volatile int spinner_started;
/* what the spinner reports, in a page that stays mapped: its CPU and how many reads it has made */
static volatile long *spin_stats;
static void *spin_reader(void *arg) {
    volatile int *page = arg;
    spinner_started = 1;
    unsigned cpu = 0;
    syscall(SYS_getcpu, &cpu, NULL, NULL);
    spin_stats[0] = (long)cpu;
    while (page) {
        (void)page[0]; /* keeps hitting its TLB entry until a fault */
        if ((++spin_stats[1] & 0xffff) == 0) { /* when it last read, now and then */
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            spin_stats[6] = now.tv_sec * 1000000000L + now.tv_nsec;
        }
    }
    return NULL;
}

static void *sleep_forever(void *arg) {
    int *fd = arg;
    char c;
    if (fd)
        read(*fd, &c, 1);
    for (;;)
        pause();
    return NULL;
}

static pid_t main_pid;
static void *outlive_main(void *arg) {
    (void)arg;
    struct timespec nap = {0, 100 * 1000 * 1000};
    nanosleep(&nap, NULL);
    exit(getpid() == main_pid ? 5 : 6);
}

/* Waits up to `ms` for pid; kills it if it is still running then. */
static int wait_with_deadline(pid_t pid, int ms) {
    int status;
    for (int waited = 0; waited < ms; waited += 10) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            return status;
        struct timespec nap = {0, 10 * 1000 * 1000};
        nanosleep(&nap, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -1;
}

static void thread_teardown_tests(void) {
    /* munmap in one thread must reach a thread spinning on the page on
     * another CPU (TLB shootdown): it faults instead of reading on. */
    /* (shared with the parent: what the child saw, if it went wrong) */
    spin_stats = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pid_t pid = fork();
    if (pid == 0) {
        int *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        page[0] = 1;
        pthread_t t;
        pthread_create(&t, NULL, spin_reader, page);
        while (!spinner_started)
            ;
        struct timespec nap = {0, 50 * 1000 * 1000};
        nanosleep(&nap, NULL);
        unsigned cpu = 0;
        syscall(SYS_getcpu, &cpu, NULL, NULL);
        spin_stats[2] = (long)cpu;
        long before = spin_stats[1];
        munmap(page, 4096);
        long at = spin_stats[1];
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        spin_stats[7] = now.tv_sec * 1000000000L + now.tv_nsec;
        long last_before = spin_stats[6];
        nanosleep(&nap, NULL);
        spin_stats[8] = spin_stats[6] - last_before; /* how long the reader went on reading after munmap */
        spin_stats[3] = before, spin_stats[4] = at, spin_stats[5] = spin_stats[1];
        nanosleep(&nap, NULL);
        _exit(0); /* the spinner did not notice */
    }
    int status = wait_with_deadline(pid, 5000);
    if (!(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV)) { /* which way it went wrong */
        printf("utest: (TLB shootdown: %s)\n", status == -1 ? "munmap hung waiting for the other CPU (killed after 5 s)"
               : WIFEXITED(status) ? "the other CPU kept reading the unmapped page" : "ended by another signal");
        printf("utest: (TLB shootdown: reader on cpu %ld, munmap on cpu %ld; reads %ld before munmap, %ld as it "
               "returned, %ld 50 ms later; its last read %ld us after munmap returned at %ld us)\n", spin_stats[0],
               spin_stats[2], spin_stats[3], spin_stats[4], spin_stats[5], (spin_stats[6] - spin_stats[7]) / 1000,
               spin_stats[7] / 1000);
    }
    CHECK(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "munmap seen by a thread on another CPU (TLB shootdown)");

    /* A fatal signal ends every thread, whatever each is doing. */
    int p[2];
    pipe(p);
    pid = fork();
    if (pid == 0) {
        static int page[1024];
        pthread_t t;
        pthread_create(&t, NULL, sleep_forever, &p[0]);
        pthread_create(&t, NULL, sleep_forever, NULL);
        pthread_create(&t, NULL, spin_reader, page);
        pthread_join(t, NULL); /* main blocks in a futex wait */
        _exit(0);
    }
    close(p[0]);
    struct timespec nap = {0, 100 * 1000 * 1000};
    nanosleep(&nap, NULL);
    kill(pid, SIGTERM);
    status = wait_with_deadline(pid, 5000);
    close(p[1]);
    CHECK(status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "fatal signal ends all threads");

    /* The main thread may leave first; the process lives on with its pid. */
    pid = fork();
    if (pid == 0) {
        main_pid = getpid();
        pthread_t t;
        pthread_create(&t, NULL, outlive_main, NULL);
        pthread_exit(NULL);
    }
    status = wait_with_deadline(pid, 5000);
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 5, "main thread exits first, process lives on");

    /* execve from a threaded process: the other threads go away. */
    pid = fork();
    if (pid == 0) {
        static int page[1024];
        pthread_t t;
        pthread_create(&t, NULL, sleep_forever, NULL);
        pthread_create(&t, NULL, spin_reader, page);
        nanosleep(&nap, NULL);
        execl("/bin/busybox", "true", (char *)NULL);
        _exit(9);
    }
    status = wait_with_deadline(pid, 5000);
    CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "execve from a threaded process");
}

#ifdef __x86_64__
/* A program's own GS base (arch_prctl ARCH_SET_GS): Firefox's wasm sandbox keeps its memory there, and aborted
 * when it was refused. It must survive the kernel's own use of GS and every switch to another thread. */
static long gs_cell = 0x5a5a1234;
static bool gs_reads(void) {
    long v;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(v));
    return v == 0x5a5a1234;
}
static void gs_test(void) {
    uint64_t back = 0;
    bool set = syscall(SYS_arch_prctl, 0x1001 /* ARCH_SET_GS */, (unsigned long)&gs_cell) == 0;
    bool got = set && syscall(SYS_arch_prctl, 0x1004 /* ARCH_GET_GS */, &back) == 0 && back == (uint64_t)&gs_cell;
    bool ok = got && gs_reads();
    for (int i = 0; ok && i < 20; i++) { /* other threads and the kernel run in between */
        sched_yield();
        usleep(2000);
        ok = gs_reads();
    }
    pid_t pid = fork(); /* a child starts with it too */
    if (pid == 0)
        _exit(gs_reads() ? 0 : 1);
    int st = 1;
    bool child = pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    syscall(SYS_arch_prctl, 0x1001, 0UL);
    CHECK(set && got && ok && child, "threads: a program's own GS base, kept across switches and into a child");
}
#endif

static void thread_tests(void) {
#ifdef __x86_64__
    gs_test();
#endif
    pthread_t t[COUNTER_THREADS];
    void *tid[COUNTER_THREADS];
    int created = 0;
    for (int i = 0; i < COUNTER_THREADS; i++)
        if (pthread_create(&t[i], NULL, counter_thread, NULL) == 0)
            created++;
    for (int i = 0; i < created; i++)
        pthread_join(t[i], &tid[i]);
    CHECK(created == COUNTER_THREADS && counter == (long)COUNTER_THREADS * COUNTER_ROUNDS,
          "pthread mutex counter (8 threads x 100000)");
    int distinct = 1;
    for (int i = 0; i < created; i++)
        distinct = distinct && (long)tid[i] != getpid() && (i == 0 || tid[i] != tid[i - 1]);
    CHECK(created && distinct && syscall(SYS_gettid) == getpid(), "gettid per thread, getpid shared");

    pthread_t a, b;
    int ok = pthread_create(&a, NULL, pingpong_thread, (void *)0L) == 0 &&
             pthread_create(&b, NULL, pingpong_thread, (void *)1L) == 0;
    if (ok) {
        pthread_join(a, NULL);
        pthread_join(b, NULL);
    }
    CHECK(ok && pp_moves == 2 * PINGPONG_ROUNDS, "pthread_cond_wait/signal ping-pong");

    pthread_barrier_init(&tls_barrier, NULL, 4);
    pthread_t tt[4];
    long tls_ok = 1;
    for (int i = 0; i < 4; i++)
        if (pthread_create(&tt[i], NULL, tls_thread, (void *)(long)i) != 0)
            tls_ok = 0;
    for (int i = 0; tls_ok && i < 4; i++) {
        void *r;
        pthread_join(tt[i], &r);
        tls_ok = tls_ok && (long)r;
    }
    CHECK(tls_ok && tls_value == 7, "thread-local storage (__thread)");

    pid_t pid = fork();
    if (pid == 0) {
        pthread_t x;
        if (pthread_create(&x, NULL, exit_group_thread, NULL) != 0)
            _exit(1);
        for (;;)
            pause(); /* only the other thread's exit() can end us */
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7,
          "exit() in a non-main thread ends the process");

    int word = 0;
    struct timespec t0, t1, timeout = {0, 50 * 1000 * 1000};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long fr = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &timeout, NULL, 0);
    int ferr = errno;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    long again = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 1, &timeout, NULL, 0);
    int aerr = errno;
    CHECK(fr == -1 && ferr == ETIMEDOUT && ms >= 45 && ms < 2000 && again == -1 && aerr == EAGAIN,
          "futex wait timeout (ETIMEDOUT), value mismatch (EAGAIN)");

    struct sigaction sa = {0};
    sa.sa_handler = on_usr2; /* no SA_RESTART: the read must fail with EINTR */
    sigaction(SIGUSR2, &sa, NULL);
    int p[2];
    struct reader_result rr = {0};
    pthread_t reader;
    ok = pipe(p) == 0;
    rr.fd = p[0];
    ok = ok && pthread_create(&reader, NULL, blocked_reader, &rr) == 0;
    if (ok) {
        struct timespec nap = {0, 100 * 1000 * 1000};
        nanosleep(&nap, NULL); /* let it block in read() */
        pthread_kill(reader, SIGUSR2);
        pthread_join(reader, NULL);
        close(p[0]);
        close(p[1]);
    }
    CHECK(ok && rr.n == -1 && rr.err == EINTR && usr2_tid == rr.tid && rr.tid != getpid(),
          "pthread_kill interrupts read() in that thread (EINTR)");

    /* A kernel write into a present read-only page must fail cleanly. */
    char *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = ro != MAP_FAILED && ro[0] == 0 && pipe(p) == 0;
    if (ok) {
        write(p[1], "x", 1);
        ok = read(p[0], ro, 1) == -1 && errno == EFAULT;
        close(p[0]);
        close(p[1]);
        munmap(ro, 4096);
    }
    CHECK(ok, "user copy to a read-only page returns EFAULT");

    thread_teardown_tests();
}

/* ---- signal masks: sigset_t bit n-1 is signal n, in every path ---- */

static volatile sig_atomic_t sig_hits[65];
static void count_sig(int sig) { sig_hits[sig]++; }

static bool mask_has(int sig) {
    sigset_t cur;
    return sigprocmask(SIG_SETMASK, NULL, &cur) == 0 && sigismember(&cur, sig) == 1;
}

/* SIGPIPE's handler runs with sa_mask = {SIGUSR1} added to the mask. */
static volatile sig_atomic_t pipe_handler_mask_ok;
static void pipe_mask_handler(int sig) {
    sig_hits[sig]++;
    pipe_handler_mask_ok = mask_has(SIGUSR1) && mask_has(SIGPIPE) && !mask_has(SIGUSR2) && !mask_has(SIGKILL);
}

static void signal_mask_tests(void) {
    static const int sigs[] = {SIGUSR1, SIGUSR2, SIGALRM, SIGPIPE, SIGTSTP};
    enum { NS = sizeof sigs / sizeof sigs[0] };
    struct sigaction sa = {0}, saved[NS];
    sa.sa_handler = count_sig;
    for (int i = 0; i < NS; i++)
        sigaction(sigs[i], &sa, &saved[i]);
    sa.sa_handler = pipe_mask_handler;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaction(SIGPIPE, &sa, NULL);
    for (int i = 0; i < 65; i++)
        sig_hits[i] = 0;
    pid_t me = getpid();

    /* Blocking SIGUSR1, SIGALRM and SIGTSTP blocks exactly those: not
     * SIGUSR2, nor SIGPIPE (SIGALRM - 1); and SIGTSTP is blockable. */
    sigset_t set, old, pend, none;
    sigemptyset(&none);
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGTSTP);
    sigprocmask(SIG_BLOCK, &set, &old);
    kill(me, SIGUSR1);
    kill(me, SIGALRM);
    kill(me, SIGTSTP);
    kill(me, SIGUSR2);
    kill(me, SIGPIPE);
    bool ok = sig_hits[SIGUSR1] == 0 && sig_hits[SIGALRM] == 0 && sig_hits[SIGTSTP] == 0 &&
              sig_hits[SIGUSR2] == 1 && sig_hits[SIGPIPE] == 1 && pipe_handler_mask_ok;
    ok = ok && mask_has(SIGUSR1) && mask_has(SIGALRM) && mask_has(SIGTSTP) && !mask_has(SIGUSR2) &&
         !mask_has(SIGPIPE) && !mask_has(SIGKILL) && !mask_has(SIGSTOP);
    ok = ok && sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1) && sigismember(&pend, SIGALRM) &&
         sigismember(&pend, SIGTSTP) && !sigismember(&pend, SIGUSR2) && !sigismember(&pend, SIGPIPE) &&
         !sigismember(&pend, SIGKILL) && !sigismember(&pend, SIGSEGV);
    sigprocmask(SIG_SETMASK, &old, NULL); /* each pending one runs once now */
    ok = ok && sig_hits[SIGUSR1] == 1 && sig_hits[SIGALRM] == 1 && sig_hits[SIGTSTP] == 1 &&
         sigpending(&pend) == 0 && !sigismember(&pend, SIGUSR1) && !mask_has(SIGUSR1);
    CHECK(ok, "sigprocmask blocks SIGUSR1 itself, not SIGUSR2 or a neighbour; sa_mask");

    /* SIGCHLD is ignored by default, but blocked it stays pending, so a
     * signalfd can read it. */
    struct sigaction dfl = {0}, saved_chld;
    dfl.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &dfl, &saved_chld);
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, &old);
    int sfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    int status;
    waitpid(pid, &status, 0);
    struct signalfd_siginfo si;
    struct pollfd pf = {sfd, POLLIN, 0};
    ok = sfd >= 0 && sigpending(&pend) == 0 && sigismember(&pend, SIGCHLD) && poll(&pf, 1, 0) == 1 &&
         read(sfd, &si, sizeof si) == sizeof si && si.ssi_signo == SIGCHLD && read(sfd, &si, sizeof si) < 0 &&
         errno == EAGAIN && sigpending(&pend) == 0 && !sigismember(&pend, SIGCHLD);
    if (sfd >= 0)
        close(sfd);
    sigprocmask(SIG_SETMASK, &old, NULL);
    sigaction(SIGCHLD, &saved_chld, NULL);
    CHECK(ok, "blocked SIGCHLD stays pending and reads through signalfd");

    /* rt_sigsuspend: the handler runs under the temporary mask, then the
     * original mask is back: woken by another process, and already pending. */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    sig_hits[SIGUSR1] = 0;
    pid = fork();
    if (pid == 0) {
        sleep_ms(30);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    int r = sigsuspend(&none), err = errno;
    waitpid(pid, NULL, 0);
    ok = r == -1 && err == EINTR && sig_hits[SIGUSR1] == 1 && mask_has(SIGUSR1);
    kill(me, SIGUSR1); /* blocked: stays pending */
    r = sigsuspend(&none);
    err = errno;
    ok = ok && r == -1 && err == EINTR && sig_hits[SIGUSR1] == 2 && mask_has(SIGUSR1);
    CHECK(ok, "rt_sigsuspend restores the signal mask after the handler");

    /* The temporary masks of ppoll, pselect6 and epoll_pwait end the same way. */
    struct timespec ts = {1, 0}, zero = {0, 0};
    kill(me, SIGUSR1);
    r = ppoll(NULL, 0, &ts, &none);
    err = errno;
    ok = r == -1 && err == EINTR && sig_hits[SIGUSR1] == 3 && mask_has(SIGUSR1) &&
         ppoll(NULL, 0, &zero, &none) == 0 && mask_has(SIGUSR1);
    kill(me, SIGUSR1);
    r = pselect(0, NULL, NULL, NULL, &ts, &none);
    err = errno;
    ok = ok && r == -1 && err == EINTR && sig_hits[SIGUSR1] == 4 && mask_has(SIGUSR1);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;
    kill(me, SIGUSR1);
    r = epoll_pwait(ep, &ev, 1, 1000, &none);
    err = errno;
    ok = ok && ep >= 0 && r == -1 && err == EINTR && sig_hits[SIGUSR1] == 5 && mask_has(SIGUSR1);
    if (ep >= 0)
        close(ep);
    sigprocmask(SIG_SETMASK, &old, NULL);
    CHECK(ok, "ppoll/pselect6/epoll_pwait restore the signal mask");

    for (int i = 0; i < NS; i++)
        sigaction(sigs[i], &saved[i], NULL);
}

static bool child_ok(pid_t pid) {
    int status;
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* MAP_SHARED: what a Wayland client and the compositor do with a pixel
 * buffer. Every view of the file (mappings in several processes, read and
 * write) must be the same memory. */
static void shared_mapping_tests(void) {
    const int RW = PROT_READ | PROT_WRITE;

    /* parent maps, forks; the child writes, the parent sees it */
    int mfd = memfd_create("utest-shared", MFD_CLOEXEC);
    bool ok = mfd >= 0 && ftruncate(mfd, 8192) == 0;
    char *a = ok ? mmap(NULL, 8192, RW, MAP_SHARED, mfd, 0) : MAP_FAILED;
    ok = a != MAP_FAILED;
    if (ok) {
        a[0] = 'P'; /* present before fork: the child inherits the mapped page */
        pid_t pid = fork();
        if (pid == 0) {
            strcpy(a + 100, "from the child");
            a[4096 + 5] = 'c'; /* untouched before fork: faulted in by the child */
            _exit(a[0] == 'P' ? 0 : 1);
        }
        ok = child_ok(pid) && !strcmp(a + 100, "from the child") && a[4096 + 5] == 'c';
    }
    CHECK(ok, "memfd MAP_SHARED: forked child's writes seen by parent");

    /* two mappings of the same memfd */
    char *b = ok ? mmap(NULL, 8192, RW, MAP_SHARED, mfd, 0) : MAP_FAILED;
    ok = b != MAP_FAILED && b != a && !strcmp(b + 100, "from the child");
    if (ok) {
        b[5000] = 'B';
        a[6000] = 'A';
        ok = a[5000] == 'B' && b[6000] == 'A';
    }
    CHECK(ok, "two mmaps of one memfd see each other's writes");

    /* write() and read() against the mappings */
    char buf[16] = {0};
    ok = ok && pwrite(mfd, "via write", 9, 300) == 9 && !memcmp(a + 300, "via write", 9) &&
         !memcmp(b + 300, "via write", 9);
    if (ok)
        memcpy(b + 7000, "via map", 7);
    ok = ok && pread(mfd, buf, 7, 7000) == 7 && !memcmp(buf, "via map", 7);
    CHECK(ok, "memfd write() visible through mapping and vice versa");

    /* grow after mapping, then map the new part */
    char *c = MAP_FAILED;
    ok = ok && ftruncate(mfd, 16384) == 0;
    c = ok ? mmap(NULL, 8192, RW, MAP_SHARED, mfd, 8192) : MAP_FAILED;
    ok = c != MAP_FAILED && c[0] == 0 && c[8191] == 0;
    if (ok) {
        c[10] = 'n';
        ok = pread(mfd, buf, 1, 8192 + 10) == 1 && buf[0] == 'n' && pwrite(mfd, "w", 1, 16000) == 1 &&
             c[16000 - 8192] == 'w';
    }
    CHECK(ok, "ftruncate grows a mapped memfd; the new part maps");

    /* the mapping outlives the descriptor, and later truncation */
    if (mfd >= 0)
        close(mfd);
    ok = a != MAP_FAILED && b != MAP_FAILED;
    if (ok) {
        int other = memfd_create("utest-reuse", 0); /* would reuse freed pages, if any were freed */
        char junk[8192];
        memset(junk, 0x77, sizeof junk);
        ok = other >= 0 && write(other, junk, sizeof junk) == sizeof junk;
        if (other >= 0)
            close(other);
        a[1] = 'Q';
        ok = ok && a[0] == 'P' && b[1] == 'Q' && !strcmp(b + 100, "from the child") && a[5000] == 'B';
    }
    CHECK(ok, "shared mapping survives close(fd)");
    if (a != MAP_FAILED)
        munmap(a, 8192);
    if (b != MAP_FAILED)
        munmap(b, 8192);
    if (c != MAP_FAILED)
        munmap(c, 8192);

    /* /dev/shm (shm_open): the child opens the name itself */
    shm_unlink("/os-utest-shm");
    int shm = shm_open("/os-utest-shm", O_RDWR | O_CREAT | O_EXCL, 0600);
    ok = shm >= 0 && ftruncate(shm, 4096) == 0;
    char *s = ok ? mmap(NULL, 4096, RW, MAP_SHARED, shm, 0) : MAP_FAILED;
    ok = s != MAP_FAILED;
    if (ok) {
        pid_t pid = fork();
        if (pid == 0) {
            int fd = shm_open("/os-utest-shm", O_RDWR, 0);
            char *m = fd >= 0 ? mmap(NULL, 4096, RW, MAP_SHARED, fd, 0) : MAP_FAILED;
            if (m == MAP_FAILED)
                _exit(1);
            strcpy(m + 64, "posix shm");
            _exit(0);
        }
        ok = child_ok(pid) && !strcmp(s + 64, "posix shm");
        munmap(s, 4096);
    }
    if (shm >= 0)
        close(shm);
    ok = ok && shm_unlink("/os-utest-shm") == 0;
    CHECK(ok, "/dev/shm shm_open + ftruncate + MAP_SHARED across fork");

    /* shared anonymous memory across fork */
    int *counter = mmap(NULL, 4096, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ok = counter != MAP_FAILED && *counter == 0;
    if (ok) {
        pid_t pid = fork();
        if (pid == 0) {
            *counter = 1234;
            _exit(0);
        }
        ok = child_ok(pid) && *counter == 1234;
        munmap(counter, 4096);
    }
    CHECK(ok, "MAP_SHARED|MAP_ANONYMOUS across fork");

    /* seals: no writable shared mapping of a write-sealed memfd */
    mfd = memfd_create("utest-sealed", MFD_ALLOW_SEALING);
    ok = mfd >= 0 && write(mfd, "sealed", 6) == 6 && fcntl(mfd, F_ADD_SEALS, F_SEAL_WRITE) == 0 &&
         mmap(NULL, 4096, RW, MAP_SHARED, mfd, 0) == MAP_FAILED && errno == EPERM;
    char *r = ok ? mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, 0) : MAP_FAILED;
    ok = r != MAP_FAILED && !memcmp(r, "sealed", 6);
    if (r != MAP_FAILED)
        munmap(r, 4096);
    if (mfd >= 0)
        close(mfd);
    CHECK(ok, "write-sealed memfd maps shared read-only only");

    /* mremap: grow anonymous memory, in place or moved */
    char *g = mmap(NULL, 2 * 4096, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = g != MAP_FAILED;
    if (ok) {
        g[0] = 'g';
        g[4096] = 'h';
        /* growing the first page alone must move (the second is in the way) */
        char *g2 = mremap(g, 4096, 4 * 4096, MREMAP_MAYMOVE);
        ok = g2 != MAP_FAILED && g2 != g && g2[0] == 'g' && g2[3 * 4096] == 0 && g[4096] == 'h';
        if (ok) {
            g2[3 * 4096] = 'x';
            char *g3 = mremap(g2, 4 * 4096, 64 * 4096, MREMAP_MAYMOVE);
            ok = g3 != MAP_FAILED && g3[0] == 'g' && g3[3 * 4096] == 'x' && g3[63 * 4096] == 0;
            if (g3 != MAP_FAILED)
                munmap(g3, 64 * 4096);
        }
        munmap(g, 2 * 4096);
    }
    CHECK(ok, "mremap grows anonymous memory");

    /* mremap of a shared mapping keeps sharing */
    mfd = memfd_create("utest-remap", 0);
    ok = mfd >= 0 && ftruncate(mfd, 4 * 4096) == 0 && pwrite(mfd, "page3", 5, 3 * 4096) == 5;
    char *m1 = ok ? mmap(NULL, 4096, RW, MAP_SHARED, mfd, 0) : MAP_FAILED;
    ok = m1 != MAP_FAILED;
    if (ok) {
        m1[0] = 'm';
        char *m2 = mremap(m1, 4096, 4 * 4096, MREMAP_MAYMOVE);
        ok = m2 != MAP_FAILED && m2[0] == 'm' && !memcmp(m2 + 3 * 4096, "page3", 5);
        if (ok) {
            m2[2 * 4096] = 'z';
            ok = pread(mfd, buf, 1, 2 * 4096) == 1 && buf[0] == 'z';
            munmap(m2, 4 * 4096);
        }
    }
    if (mfd >= 0)
        close(mfd);
    CHECK(ok, "mremap grows a shared mapping");

    /* madvise(MADV_DONTNEED): private anonymous pages come back zeroed,
     * shared ones keep the object's data */
    char *d = mmap(NULL, 2 * 4096, RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int *sh = mmap(NULL, 4096, RW, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ok = d != MAP_FAILED && sh != MAP_FAILED;
    if (ok) {
        d[0] = 'd';
        d[4096] = 'e';
        *sh = 77;
        ok = madvise(d, 4096, MADV_DONTNEED) == 0 && d[0] == 0 && d[4096] == 'e' &&
             madvise(sh, 4096, MADV_DONTNEED) == 0 && *sh == 77;
    }
    if (d != MAP_FAILED)
        munmap(d, 2 * 4096);
    if (sh != MAP_FAILED)
        munmap(sh, 4096);
    CHECK(ok, "madvise(MADV_DONTNEED)");
}

/* A 64 MiB file written and read back in 1 MiB pieces: ramfs storage must
 * stay fast (the initramfs unpacker and every app install go through it). */
static void ramfs_perf_test(void) {
    enum { CHUNK = 1 << 20, TOTAL = 64 };
    char *buf = malloc(CHUNK);
    int fd = buf ? open("/tmp/big64", O_CREAT | O_RDWR | O_TRUNC, 0644) : -1;
    bool ok = fd >= 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; ok && i < TOTAL; i++) {
        memset(buf, 'a' + i % 26, CHUNK);
        ok = write(fd, buf, CHUNK) == CHUNK;
    }
    long wms = ms_since(&t0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ok = ok && lseek(fd, 0, SEEK_SET) == 0;
    for (int i = 0; ok && i < TOTAL; i++)
        ok = read(fd, buf, CHUNK) == CHUNK && buf[0] == 'a' + i % 26 && buf[CHUNK - 1] == 'a' + i % 26 &&
             buf[CHUNK / 2 + 7] == 'a' + i % 26;
    long rms = ms_since(&t0);
    if (fd >= 0)
        close(fd);
    unlink("/tmp/big64");
    free(buf);
    printf("utest: ramfs 64 MiB write %ld ms, read %ld ms\n", wms, rms);
    CHECK(ok, "64 MiB ramfs file round trip");
}

#include "ns_tests.inc"
#include "ext2_tests.inc"
#include "lock_tests.inc"
#include "inotify_tests.inc"
#include "fd_tests.inc"
#include "filecache_tests.inc"
#include "wait_tests.inc"
#include "oom_tests.inc"
#include "perm_tests.inc"

#ifdef __aarch64__
#define MACHINE "aarch64"
#else
#define MACHINE "x86_64"
#endif

static void tests(void) {
    struct utsname u;
    CHECK(uname(&u) == 0 && !strcmp(u.machine, MACHINE), "uname");

    CHECK(mkdir("/tmp", 0755) == 0 || errno == EEXIST, "mkdir");
    int fd = open("/tmp/hello.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, "os\n", 6) == 6 && close(fd) == 0, "create+write file");
    char buf[64] = {0};
    fd = open("/tmp/hello.txt", O_RDONLY);
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) == 6 && !strcmp(buf, "os\n"), "read file back");
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_size == 6 && S_ISREG(st.st_mode), "fstat");
    close(fd);
    CHECK(rename("/tmp/hello.txt", "/tmp/renamed.txt") == 0 && access("/tmp/renamed.txt", F_OK) == 0,
          "rename");
    CHECK(unlink("/tmp/renamed.txt") == 0 && access("/tmp/renamed.txt", F_OK) < 0, "unlink");

    DIR *d = opendir("/");
    int entries = 0;
    for (struct dirent *e; d && (e = readdir(d));)
        entries++;
    if (d)
        closedir(d);
    CHECK(entries >= 4, "readdir /");

    char *big = malloc(8 << 20);
    CHECK(big != NULL, "malloc 8 MiB");
    if (big) {
        memset(big, 0x5a, 8 << 20);
        CHECK(big[(8 << 20) - 1] == 0x5a, "touch 8 MiB");
        free(big);
    }
    void *m = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED && ((char *)m)[12345] == 0, "mmap anonymous");
    if (m != MAP_FAILED)
        CHECK(munmap(m, 1 << 20) == 0, "munmap");

    double x = 1.0;
    for (int i = 0; i < 10; i++)
        x = x * 1.5 + 0.25;
    CHECK(x > 85.9 && x < 86.1, "floating point (SSE)"); /* 1.5^11 - 0.5 */

    pid_t pid = fork();
    if (pid == 0)
        _exit(42);
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42,
          "fork + wait exit status");

    /* copy-on-write: after fork both share pages read-only; writes by the
     * program and by the kernel (read() into a buffer, getrlimit) each give
     * the writer its own copy, and the other keeps what it had */
    {
        static char cow_buf[3 * 4096];
        memset(cow_buf, 'p', sizeof cow_buf);
        int cp[2];
        if (pipe(cp) == 0) {
            pid_t c = fork();
            if (c == 0) {
                struct rlimit rl;
                int bad = getrlimit(RLIMIT_NOFILE, &rl) != 0;
                char got[8];
                bad |= read(cp[0], cow_buf + 4096, 5) != 5 || memcmp(cow_buf + 4096, "child", 5) != 0;
                cow_buf[0] = 'c';
                bad |= cow_buf[8192] != 'p';
                (void)got;
                _exit(bad ? 1 : 0);
            }
            CHECK(write(cp[1], "child", 5) == 5, "copy-on-write: pipe");
            int st = -1;
            waitpid(c, &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "copy-on-write: the kernel writes into a forked child's pages");
            CHECK(cow_buf[0] == 'p' && cow_buf[4096] == 'p', "copy-on-write: the parent keeps its own copy");
            close(cp[0]);
            close(cp[1]);
        }
    }

    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    pid = fork();
    if (pid == 0) {
        close(p[0]);
        write(p[1], "through the pipe", 16);
        _exit(0);
    }
    close(p[1]);
    memset(buf, 0, sizeof buf);
    ssize_t n = read(p[0], buf, sizeof buf);
    close(p[0]);
    waitpid(pid, NULL, 0);
    CHECK(n == 16 && !strcmp(buf, "through the pipe"), "pipe between processes");

    struct sigaction sa = {0};
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    kill(getpid(), SIGUSR1);
    CHECK(got_usr1 == SIGUSR1, "signal handler");

    pid = fork();
    if (pid == 0) {
        for (;;)
            pause();
    }
    kill(pid, SIGTERM);
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
          "kill child with SIGTERM");

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct timespec nap = {0, 50 * 1000 * 1000};
    nanosleep(&nap, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ms >= 45 && ms < 1000, "nanosleep 50 ms");

    thread_tests();
    signal_mask_tests();

    /* A real third-party Linux binary (busybox, glibc static) running scripts. */
    char out[4096];
    char *echo[] = {"/bin/busybox", "sh", "-c", "echo busybox says hi; echo $((6 * 7))", NULL};
    CHECK(run(echo, out, sizeof out) == 0 && strstr(out, "busybox says hi") && strstr(out, "42"),
          "busybox sh");
    char *pipeline[] = {"/bin/sh", "-c", "ls /bin | wc -l", NULL};
    CHECK(run(pipeline, out, sizeof out) == 0 && atoi(out) > 50, "shell pipeline (ls | wc)");
    char *files[] = {"/bin/sh", "-c",
                     "mkdir -p /tmp/a/b && echo data > /tmp/a/b/f && cat /tmp/a/b/f && rm -r /tmp/a && "
                     "test ! -e /tmp/a && echo cleaned",
                     NULL};
    CHECK(run(files, out, sizeof out) == 0 && strstr(out, "data") && strstr(out, "cleaned"),
          "shell file operations");
    char *script[] = {"/etc/hello.sh", NULL};
    CHECK(run(script, out, sizeof out) == 0 && strstr(out, "hello from a #! script"), "#! script");

    ipc_tests();
    proc_tests();
    ns_tests();
    base_tests();
    ext2_tests();
    filecache_tests();
    lock_tests();
    inotify_tests();
    fd_tests();
    wait_tests();
    oom_tests();
    perm_tests();
    doc_fd_tests();
    shared_mapping_tests();
    ramfs_perf_test();

    disk_tests();
    audio_tests();

    lo_tests(); /* loopback needs no DHCP: before the NIC tests */
    net_tests();
}

/* ---- /proc and /sys ---- */

static ssize_t read_text(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t used = 0;
    ssize_t n;
    while (used + 1 < size && (n = read(fd, buf + used, size - 1 - used)) > 0)
        used += (size_t)n;
    buf[used] = 0;
    close(fd);
    return (ssize_t)used;
}

static int count_dir(const char *path, bool *has012) {
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int n = 0, low = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        n++;
        if (!strcmp(e->d_name, "0") || !strcmp(e->d_name, "1") || !strcmp(e->d_name, "2"))
            low++;
    }
    closedir(d);
    if (has012)
        *has012 = low == 3;
    return n;
}

static pthread_barrier_t proc_barrier;
static void *proc_waiter(void *arg) {
    (void)arg;
    pthread_barrier_wait(&proc_barrier); /* main has counted us */
    return NULL;
}

static void proc_tests(void) {
    static char buf[65536];
    char path[256], link[256];

    /* What std::filesystem::canonical (Piper) and $ORIGIN depend on. */
    struct stat st;
    CHECK(lstat("/proc", &st) == 0 && S_ISDIR(st.st_mode), "/proc: lstat /proc is a directory");
    ssize_t n = readlink("/proc/self", link, sizeof link - 1);
    if (n >= 0)
        link[n] = 0;
    snprintf(path, sizeof path, "%d", getpid());
    CHECK(lstat("/proc/self", &st) == 0 && S_ISLNK(st.st_mode) && n > 0 && !strcmp(link, path),
          "/proc: /proc/self is a link to the pid");
    n = readlink("/proc/self/exe", link, sizeof link - 1);
    if (n >= 0)
        link[n] = 0;
    CHECK(n > 0 && !strcmp(link, "/sbin/init"), "/proc: readlink /proc/self/exe is /sbin/init");
    char *real = realpath("/proc/self/exe", NULL);
    CHECK(real && !strcmp(real, "/sbin/init"), "/proc: realpath /proc/self/exe is /sbin/init");
    free(real);

    n = read_text("/proc/self/maps", buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "[stack]\n") && strstr(buf, " /sbin/init\n"),
          "/proc/self/maps has [stack] and the binary");
    /* Linux's layout: the path starts at column 73. */
    char *line = n > 0 ? strstr(buf, "/sbin/init\n") : NULL;
    while (line && line > buf && line[-1] != '\n')
        line--;
    char *name = line ? strchr(line, '/') : NULL;
    CHECK(name && name - line == 73, "/proc/self/maps column layout");

    bool has012 = false;
    CHECK(count_dir("/proc/self/fd", &has012) >= 3 && has012, "/proc/self/fd lists 0, 1, 2");
    int p[2];
    if (pipe(p) == 0) {
        snprintf(path, sizeof path, "/proc/self/fd/%d", p[0]);
        n = readlink(path, link, sizeof link - 1);
        if (n >= 0)
            link[n] = 0;
        CHECK(n > 0 && !strncmp(link, "pipe:[", 6), "/proc/self/fd/N of a pipe reads pipe:[ino]");
        close(p[0]);
        close(p[1]);
    }

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int procs = 0;
    if (read_text("/proc/cpuinfo", buf, sizeof buf) > 0)
        for (char *s = buf; (s = strstr(s, "processor\t: ")); s++)
            procs++;
    CHECK(procs > 0 && procs == cpus, "/proc/cpuinfo has a processor line per CPU");
    if (read_text("/sys/devices/system/cpu/online", buf, sizeof buf) > 0) {
        char want[32];
        if (cpus > 1)
            snprintf(want, sizeof want, "0-%ld\n", cpus - 1);
        else
            snprintf(want, sizeof want, "0\n");
        CHECK(!strcmp(buf, want), "/sys/devices/system/cpu/online");
    } else {
        CHECK(0, "/sys/devices/system/cpu/online");
    }

    long total = 0;
    if (read_text("/proc/meminfo", buf, sizeof buf) > 0)
        sscanf(buf, "MemTotal: %ld kB", &total);
    CHECK(total > 0, "/proc/meminfo MemTotal > 0");

    int pid = 0, ppid = -1;
    char comm[64] = "", state = 0;
    if (read_text("/proc/self/stat", buf, sizeof buf) > 0)
        sscanf(buf, "%d (%63[^)]) %c %d", &pid, comm, &state, &ppid);
    CHECK(pid == getpid() && !strcmp(comm, "init") && state == 'R' && ppid == 0, "/proc/self/stat parses");

    /* utime (field 14) counts user-mode CPU time in 1/100 s. */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile unsigned long spin = 0;
    do {
        for (int i = 0; i < 100000; i++)
            spin++;
        clock_gettime(CLOCK_MONOTONIC, &t1);
    } while ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < 300);
    unsigned long utime = 0;
    if (read_text("/proc/self/stat", buf, sizeof buf) > 0) {
        char *s = strrchr(buf, ')');
        sscanf(s ? s + 2 : buf, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu", &utime);
    }
    CHECK(utime >= 10, "/proc/self/stat utime counts user CPU time");

    pthread_barrier_init(&proc_barrier, NULL, 4);
    pthread_t t[3];
    int started = 0;
    for (int i = 0; i < 3; i++)
        if (pthread_create(&t[i], NULL, proc_waiter, NULL) == 0)
            started++;
    int tasks = count_dir("/proc/self/task", NULL);
    int threads = 0;
    if (read_text("/proc/self/status", buf, sizeof buf) > 0) {
        char *s = strstr(buf, "Threads:\t");
        if (s)
            threads = atoi(s + 9);
    }
    pthread_barrier_wait(&proc_barrier);
    for (int i = 0; i < started; i++)
        pthread_join(t[i], NULL);
    CHECK(started == 3 && tasks == 4 && threads == 4, "/proc/self/task has one entry per thread");

    CHECK(read_text("/proc/sys/kernel/ostype", buf, sizeof buf) > 0 && !strcmp(buf, "Linux\n"),
          "/proc/sys/kernel/ostype");
    int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && write(fd, "100\n", 4) == 4 && close(fd) == 0, "/proc/self/oom_score_adj is writable");
    CHECK(read_text("/sys/kernel/mm/transparent_hugepage/enabled", buf, sizeof buf) > 0 &&
              strstr(buf, "[never]"),
          "/sys/kernel/mm/transparent_hugepage/enabled");

    /* Third-party readers: busybox ps, tty on a pty, and /dev/fd on a pipe. */
    char out[8192];
    char *ps[] = {"/bin/busybox", "ps", NULL};
    CHECK(run(ps, out, sizeof out) == 0 && strstr(out, "PID") && strstr(out, "/sbin/init") &&
              strstr(out, "busybox ps"),
          "busybox ps");
    char *devfd[] = {"/bin/busybox", "sh", "-c", "echo through-dev-stdin | cat /dev/stdin", NULL};
    CHECK(run(devfd, out, sizeof out) == 0 && strstr(out, "through-dev-stdin"), "/dev/stdin reopens a pipe");

    int master = posix_openpt(O_RDWR | O_NOCTTY);
    char *slave = master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0 ? ptsname(master) : NULL;
    if (slave) {
        char slave_name[64];
        snprintf(slave_name, sizeof slave_name, "%s", slave);
        int out_pipe[2];
        pipe(out_pipe);
        pid_t c = fork();
        if (c == 0) {
            setsid();
            int s = open(slave_name, O_RDWR);
            dup2(s, 0);
            dup2(out_pipe[1], 1);
            dup2(out_pipe[1], 2);
            close(out_pipe[0]);
            execl("/bin/busybox", "tty", (char *)NULL);
            _exit(127);
        }
        close(out_pipe[1]);
        size_t used = 0;
        while ((n = read(out_pipe[0], out + used, sizeof out - 1 - used)) > 0)
            used += (size_t)n;
        out[used] = 0;
        close(out_pipe[0]);
        int status = 0;
        waitpid(c, &status, 0);
        char want[80];
        snprintf(want, sizeof want, "%s\n", slave_name);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0 && !strcmp(out, want), "busybox tty on a pty");
        if (strcmp(out, want))
            printf("utest: tty said: %s (wanted %s)\n", out, slave_name);
    } else {
        CHECK(0, "busybox tty on a pty (no pty)");
    }
    if (master >= 0)
        close(master);
}

/* OS_MODE=piper: Piper's own binary, found through RUNPATH $ORIGIN
 * (no LD_LIBRARY_PATH), turns text into a WAV file. */
static void piper_mode(void) {
    char out[16384];
    char *argv[] = {"/bin/busybox", "sh", "-c",
                    "echo hi | /opt/piper/piper --model /usr/share/os/voice/en_US-amy-medium.onnx "
                    "--output_file /tmp/x.wav; echo piper exit $?; ls -l /tmp/x.wav",
                    NULL};
    int r = run(argv, out, sizeof out);
    printf("%s", out);
    struct stat st;
    printf("PIPER %s (status %d, wav %ld bytes)\n", stat("/tmp/x.wav", &st) == 0 && st.st_size > 1000 ? "OK" : "FAILED",
           r, stat("/tmp/x.wav", &st) == 0 ? (long)st.st_size : -1L);
}

/* ---- D-Bus: the system bus, and the desktop session's bus ---- */

/* The person's session runs as an ordinary user (apps such as Chromium refuse
 * to sandbox themselves as root); only the system parts stay root. */
/* their home, /home/<their account> (shell/home.c, linked in) */
const char *home_dir(void);
#define SESSION_UID 1000
#define SESSION_RUNTIME "/run/user/1000"
#define SESSION_BUS "unix:path=" SESSION_RUNTIME "/bus"

/* /etc/machine-id: D-Bus and Flatpak want one. Kept if it exists (the disk
 * store brings it back), made from random bytes otherwise. */
static void ensure_machine_id(void) {
    if (access("/etc/machine-id", F_OK) == 0)
        return;
    unsigned char b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
        for (size_t i = 0; i < sizeof b; i++)
            b[i] = (unsigned char)(rand() ^ (int)(time(NULL) >> i));
    }
    if (fd >= 0)
        close(fd);
    char hex[34];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", b[i]);
    hex[32] = '\n';
    hex[33] = 0;
    fd = open("/etc/machine-id", O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd >= 0) {
        write(fd, hex, 33);
        close(fd);
    }
    mkdir("/var", 0755);
    mkdir("/var/lib", 0755);
    mkdir("/var/lib/dbus", 0755);
    symlink("/etc/machine-id", "/var/lib/dbus/machine-id");
}

static pid_t spawn_bus(const char *kind) {
    if (access("/usr/bin/dbus-daemon", X_OK) != 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        if (!strcmp(kind, "system")) {
            char *argv[] = {"/usr/bin/dbus-daemon", "--system", "--nofork", "--nopidfile", "--nosyslog", NULL};
            execv(argv[0], argv);
        } else {
            /* the session's bus is the person's: it and what it starts (portals) run as them */
            if (setgroups(0, NULL) < 0 || setgid(SESSION_UID) < 0 || setuid(SESSION_UID) < 0)
                _exit(126);
            setenv("HOME", home_dir(), 1); /* the person's files (session.inc: account_setup) */
            char *argv[] = {"/usr/bin/dbus-daemon", "--session", "--nofork", "--nopidfile", "--nosyslog",
                            "--address=" SESSION_BUS, NULL};
            execv(argv[0], argv);
        }
        _exit(127);
    }
    return pid;
}

/* Waits (briefly) for a bus socket to appear, so the desktop finds it. */
static void wait_for_socket(const char *path) {
    for (int i = 0; i < 100 && access(path, F_OK) != 0; i++)
        usleep(20000);
}

static pid_t system_bus = -1, session_bus = -1, sound = -1;

/* The sound server apps use (PulseAudio, /usr/share/os/pulse.pa), as the
 * session's user, playing through /dev/dsp. */
static pid_t spawn_sound(void) {
    if (access("/usr/bin/pulseaudio", X_OK) != 0)
        return -1;
    mkdir(SESSION_RUNTIME "/pulse", 0700);
    if (chown(SESSION_RUNTIME "/pulse", SESSION_UID, SESSION_UID) < 0) { /* only ownership */
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (setgroups(0, NULL) < 0 || setgid(SESSION_UID) < 0 || setuid(SESSION_UID) < 0)
            _exit(126);
        setenv("HOME", home_dir(), 1);
        char *argv[] = {"/usr/bin/pulseaudio", "-n", "-F", "/usr/share/os/pulse.pa", "--daemonize=no",
                        "--exit-idle-time=-1", "--log-target=stderr", "--log-level=error", NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static void start_buses(void) {
    if (access("/usr/bin/dbus-daemon", X_OK) != 0)
        return;
    ensure_machine_id();
    mkdir("/run", 0755);
    mkdir("/run/dbus", 0755);
    mkdir("/run/user", 0755);
    mkdir(SESSION_RUNTIME, 0700);
    if (chown(SESSION_RUNTIME, SESSION_UID, SESSION_UID) < 0) { /* only ownership */
    }
    setenv("XDG_RUNTIME_DIR", SESSION_RUNTIME, 1);
    setenv("XDG_CURRENT_DESKTOP", "OS", 1); /* the portal front end picks os-portal for this */
    setenv("DBUS_SESSION_BUS_ADDRESS", SESSION_BUS, 1);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", 1);
    system_bus = spawn_bus("system");
    session_bus = spawn_bus("session");
    wait_for_socket("/run/dbus/system_bus_socket");
    wait_for_socket(SESSION_RUNTIME "/bus");
    printf("init: D-Bus: system bus (pid %d), session bus (pid %d)\n", system_bus, session_bus);
    sound = spawn_sound();
}


/* ---- the disk's file system: /dev/sda2 (the area after the save slots) ----
 * Formatted the first time (with mke2fs), checked at every boot (e2fsck, as
 * Linux does), mounted at /var/lib/os-data. Apps installed from Flathub go
 * there (a Flatpak installation of its own, "os-data"), so they survive a
 * restart; the system's own runtime and Prism stay in the image. */

#define DATA_DEV "/dev/sda2"
#define DATA_DIR "/var/lib/os-data"

static bool data_disk_formatted(void) {
    int fd = open(DATA_DEV, O_RDONLY);
    if (fd < 0)
        return false;
    uint16_t magic = 0;
    bool ok = pread(fd, &magic, 2, 1024 + 56) == 2 && magic == 0xEF53;
    close(fd);
    return ok;
}

/* "Reset to factory" (the shell, store.c) leaves this mark in the unused gap
 * before the apps' area: that area is then formatted afresh, so the apps and
 * their data go too. (Erasing it from the running system wouldn't hold: the
 * file system is written back, cleanly shut down, as the computer restarts.) */
#define RESET_MARK_AT (300ull << 20)

static bool factory_reset_asked(void) {
    char mark[16] = {0};
    int fd = open("/dev/sda", O_RDWR);
    if (fd < 0)
        return false;
    bool asked = pread(fd, mark, sizeof mark, RESET_MARK_AT) == (ssize_t)sizeof mark && !memcmp(mark, "OSRESET1", 8);
    if (asked) {
        static const char zero[16];
        if (pwrite(fd, zero, sizeof zero, RESET_MARK_AT) != (ssize_t)sizeof zero)
            asked = false; /* never formatted over and over */
        fsync(fd);
    }
    close(fd);
    return asked;
}

/* Their home on the disk itself (the apps' area, journaled: kernel/fs/ext2.c), shown at /home/<name> by a bind
 * mount: no size limit, no file too big to keep, nothing rewritten whole at each save (the store kept the home
 * in memory and wrote all of it to its slot at every change, 128 MiB at most, files over 32 MiB not at all). The
 * first time, what the store kept (restored into memory a moment ago) is copied there and checked; the store's
 * copy stays as it was, and from then on the store keeps the system's state alone (HOME_ON_DISK says so). */
#define HOME_ON_DISK "/run/os/home-on-disk"
const char *account_name(void);
static void home_to_disk(void) {
    struct stat st;
    if (access(HOME_ON_DISK, F_OK) == 0 || stat(DATA_DIR "/lost+found", &st) != 0) /* done, or no disk mounted */
        return;
    const char *home = home_dir(), *name = account_name();
    char disk_home[300];
    snprintf(disk_home, sizeof disk_home, DATA_DIR "/home/%s", name);
    mkdir(DATA_DIR "/home", 0755);
    if (stat(disk_home, &st) != 0) {
        /* a home there under an older name (the account renamed): it takes the new one, as /home does */
        DIR *d = opendir(DATA_DIR "/home");
        char old[300] = "";
        for (struct dirent *e; d && (e = readdir(d));)
            if (e->d_name[0] != '.' && snprintf(old, sizeof old, DATA_DIR "/home/%s", e->d_name) > 0)
                break;
        if (d)
            closedir(d);
        if (old[0] && rename(old, disk_home) == 0) {
            printf("init: their home on the disk is now %s (it was %s)\n", disk_home, old);
        } else {
            /* the first time: what the store kept, copied onto the disk, then checked against it */
            if (mkdir(disk_home, 0700) != 0) {
                printf("init: couldn't make %s (%s): the home stays in memory\n", disk_home, strerror(errno));
                return;
            }
            char src[320], out[1024];
            snprintf(src, sizeof src, "%s/.", home);
            char *cp[] = {"/bin/cp", "-a", src, disk_home, NULL};
            int r = run(cp, out, sizeof out);
            char *cmp[] = {"/bin/diff", "-rq", (char *)home, disk_home, NULL};
            int same = r == 0 ? run(cmp, out, sizeof out) : -1;
            if (r != 0 || same != 0) {
                printf("init: their home couldn't be moved onto the disk (%s): it stays in memory\n", out);
                char *rm[] = {"/bin/rm", "-r", disk_home, NULL};
                run(rm, out, sizeof out);
                return;
            }
            sync();
            printf("init: their home moved onto the disk (%s)\n", disk_home);
        }
    }
    if (chown(disk_home, SESSION_UID, SESSION_UID) != 0 || chmod(disk_home, 0700) != 0) {
    }
    mkdir(home, 0700);
    if (mount(disk_home, home, NULL, MS_BIND, NULL) != 0) {
        printf("init: couldn't show %s at %s (%s): the home stays in memory\n", disk_home, home, strerror(errno));
        return;
    }
    mkdir("/run/os", 0755);
    FILE *f = fopen(HOME_ON_DISK, "w");
    if (f) {
        fprintf(f, "%s\n", disk_home);
        fclose(f);
    }
    printf("init: their home is on the disk (%s, journaled)\n", disk_home);
}

/* Whether the apps' area has a journal (the superblock's has_journal feature, at byte 1024 + 92). */
static bool data_disk_journaled(void) {
    int fd = open(DATA_DEV, O_RDONLY);
    uint32_t compat = 0;
    bool ok = fd >= 0 && pread(fd, &compat, 4, 1024 + 92) == 4;
    if (fd >= 0)
        close(fd);
    return ok && (compat & 0x4);
}

static void setup_data_disk(void) {
    if (access(DATA_DEV, F_OK) != 0) {
        printf("init: no disk area for apps (" DATA_DEV "): apps from Flathub won't be kept\n");
        return;
    }
    char out[1024];
    bool reset = factory_reset_asked();
    if (reset)
        printf("init: reset to factory: the apps' area is made new\n");
    if (reset || !data_disk_formatted()) {
        /* with a journal (ext3's): a power cut never leaves it half written (kernel/fs/ext2.c) */
        char *mk[] = {"/usr/sbin/mke2fs", "-q", "-F", "-t", "ext2", "-O",
                      "^dir_index,^resize_inode,^ext_attr,has_journal", "-J", "size=64", "-b", "4096", "-L", "os-data",
                      DATA_DEV, NULL};
        int r = run(mk, out, sizeof out);
        printf("init: formatted " DATA_DEV " for apps (%s)\n", r == 0 ? "ok" : out);
        if (r != 0)
            return;
    } else {
        /* -p: fix what is safe to fix without asking; 0-3 mean usable */
        char *ck[] = {"/usr/sbin/e2fsck", "-p", DATA_DEV, NULL};
        int r = run(ck, out, sizeof out);
        printf("init: checked " DATA_DEV ": %s%s", r == 0 ? "clean" : r < 4 ? "repaired" : "PROBLEMS: ", r < 4 ? "\n" : out);
        /* what -p won't fix alone: nobody is here to answer its questions, so yes to every repair (Linux's
         * fsck.repair=yes), and again until it is clean: mending one thing (a folder block that never reached
         * the disk) leaves others for the next pass (the files it named, their link counts) */
        for (int pass = 1; r >= 4 && r < 8 && pass <= 4; pass++) {
            char *fix[] = {"/usr/sbin/e2fsck", "-fy", DATA_DEV, NULL};
            r = run(fix, out, sizeof out);
            printf("init: repair %d of " DATA_DEV ": %s\n", pass, r < 4 ? "usable" : "more to mend");
        }
        if (r >= 4)
            return; /* not mounted: better no apps than a damaged disk made worse */
        /* a disk from before the journal: given one (checked and clean, nothing mounted: what tune2fs needs) */
        if (!data_disk_journaled()) {
            char *tj[] = {"/usr/sbin/tune2fs", "-O", "has_journal", "-J", "size=64", DATA_DEV, NULL};
            int tr = run(tj, out, sizeof out);
            printf("init: " DATA_DEV " given a journal: %s\n", tr == 0 ? "ok" : out);
        }
    }
    /* With the journal a power cut is mended by replaying it (the kernel keeps the disk "valid": e2fsck -p
     * then only replays, in seconds, instead of reading the whole disk). No whole check every so many starts
     * or days either (its maximum mount count at byte 1024 + 54, -1: never): it made one start in thirty take
     * minutes for nothing the journal doesn't already cover; a disk found damaged is still checked whole. */
    int16_t max_mounts = 0;
    int sfd = open(DATA_DEV, O_RDONLY);
    if (sfd >= 0 && pread(sfd, &max_mounts, 2, 1024 + 54) == 2 && max_mounts != -1 && data_disk_journaled()) {
        close(sfd), sfd = -1;
        char *tc[] = {"/usr/sbin/tune2fs", "-c", "-1", "-i", "0", DATA_DEV, NULL};
        int tr = run(tc, out, sizeof out);
        printf("init: " DATA_DEV " no longer checked whole on a count: %s\n", tr == 0 ? "ok" : out);
    }
    if (sfd >= 0)
        close(sfd);
    mkdir("/var/lib", 0755);
    mkdir(DATA_DIR, 0755);
    if (mount(DATA_DEV, DATA_DIR, "ext2", 0, NULL) != 0) {
        printf("init: couldn't mount " DATA_DEV " (%s)\n", strerror(errno));
        return;
    }
    /* Flatpak: the disk is an installation of its own, with Flathub */
    mkdir(DATA_DIR "/flatpak", 0755);
    mkdir("/etc/flatpak", 0755);
    mkdir("/etc/flatpak/installations.d", 0755);
    FILE *f = fopen("/etc/flatpak/installations.d/os-data.conf", "w");
    if (f) {
        fprintf(f, "[Installation \"os-data\"]\nPath=" DATA_DIR "/flatpak\nDisplayName=This computer's disk\n"
                   "StorageType=harddisk\n");
        fclose(f);
    }
    if (access(DATA_DIR "/flatpak/repo/config", F_OK) != 0) {
        char *add[] = {"/usr/bin/flatpak", "--installation=os-data", "remote-add", "--if-not-exists",
                       "--gpg-import=/var/lib/flatpak/repo/flathub.trustedkeys.gpg", "flathub",
                       "https://dl.flathub.org/repo/", NULL};
        int r = run(add, out, sizeof out);
        printf("init: Flathub for the disk's apps: %s", r == 0 ? "ok\n" : out);
    }
}
/* The kernel's boot screen keeps the steps (/proc/bootsteps): ours marked there as they happen. */
static void boot_step(const char *id, const char *what) {
    int fd = open("/proc/bootsteps", O_WRONLY);
    if (fd < 0)
        return;
    char l[96];
    int n = snprintf(l, sizeof l, "%s %s\n", id, what);
    if (write(fd, l, (size_t)n) < 0) {
    }
    close(fd);
}

/* The boot screen in their design (os-boot), until the desktop's first frame. */
static void start_boot_screen(void) {
    if (access("/usr/libexec/os-boot", X_OK) != 0) {
        printf("init: no boot screen to start (/usr/libexec/os-boot)\n");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0)
            dup2(nul, 0), dup2(nul, 1);
        char *argv[] = {"/usr/libexec/os-boot", NULL};
        execv(argv[0], argv);
        _exit(127);
    }
}

/* A development shell on the console, only when the host asked for one (OS_CONSOLE=shell, from fw_cfg
 * opt/os/console): otherwise keys typed while the desktop isn't listening (booting, or restarting after a crash)
 * would reach a root shell with no login, and so would anyone with a serial cable. */
static pid_t spawn_console_shell(void) {
    const char *con = getenv("OS_CONSOLE");
    if (!con || strcmp(con, "shell"))
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        tcsetpgrp(0, getpid());
        char *argv[] = {"/bin/sh", "-l", NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

#include "session.inc"

/* OS_MODE=disk-speed (tests/disk_speed_win.py): how fast the apps' area takes what Flatpak writes: many small files,
 * each made sure of (as ostree does), then one big one. */
static double secs_since(const struct timespec *t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) / 1e9;
}

static void disk_speed_mode(void) {
    setup_data_disk();
    char dir[] = DATA_DIR "/speed";
    mkdir(dir, 0755);
    static char buf[1 << 20];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (char)(i * 2654435761u >> 13);
    struct timespec t0;
    for (int pass = 0; pass < 2; pass++) { /* with fsync, then without */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int n = 2000;
        for (int i = 0; i < n; i++) {
            char p[96];
            snprintf(p, sizeof p, "%s/%c%04d", dir, pass ? 'n' : 's', i);
            int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || write(fd, buf, 20000) != 20000) {
                printf("disk-speed: couldn't write %s: %s\n", p, strerror(errno));
                return;
            }
            if (!pass)
                fsync(fd);
            close(fd);
        }
        double s = secs_since(&t0);
        printf("disk-speed: %d files of 20 KB %s: %.2f s, %.0f files/s, %.1f MB/s\n", n,
               pass ? "without fsync" : "each with fsync", s, n / s, n * 20000 / 1e6 / s);
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int fd = open(DATA_DIR "/speed/big", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (int i = 0; fd >= 0 && i < 200; i++)
        if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf)
            break;
    if (fd >= 0)
        fsync(fd), close(fd);
    double s = secs_since(&t0);
    printf("disk-speed: one 200 MB file: %.2f s, %.1f MB/s\n", s, 200 * 1.048576 / s);
    /* as curl writes a download: 16 KB at a time, appended, and what that costs the kernel */
    double k0 = kernel_cpu_s();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    fd = open(DATA_DIR "/speed/big16", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (int i = 0; fd >= 0 && i < 200 * 64; i++)
        if (write(fd, buf, 16384) != 16384)
            break;
    if (fd >= 0)
        close(fd);
    s = secs_since(&t0);
    printf("disk-speed: 200 MB in 16 KB writes: %.2f s, %.1f MB/s, %.2f s of kernel time\n", s, 200 * 1.048576 / s,
           kernel_cpu_s() - k0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sync();
    printf("disk-speed: sync %.2f s\n", secs_since(&t0));
}

int main(void) {
    printf("init: user space is alive (pid %d)\n", getpid());
    const char *mode = getenv("OS_MODE");
    if (mode && !strcmp(mode, "test")) {
        tests();
        if (failures)
            printf("USERLAND TESTS FAILED (%d)\n", failures);
        else
            printf("USERLAND TESTS PASSED\n");
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strncmp(mode, "persist-", 8)) {
        persist_mode(mode);
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strncmp(mode, "flatpak-install:", 16)) { /* an install from Flathub, as the system does it, told */
        setup_data_disk();
        for (int i = 0; i < 100 && access("/etc/resolv.conf", F_OK) != 0; i++)
            sleep_ms(100);
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        /* APP+URL: with a download of URL onto the disk at the same time (as the desktop's own downloads do) */
        static char app[200];
        snprintf(app, sizeof app, "%s", mode + 16);
        char *with = strchr(app, '+');
        pid_t dl = 0;
        if (with) {
            *with++ = 0;
            dl = fork();
            if (dl == 0) {
                execl("/usr/bin/curl", "curl", "-fL", "-sS", "-o", "/var/lib/os-data/with.part", with, (char *)NULL);
                _exit(127);
            }
        }
        pid_t pid = fork();
        if (pid == 0) { /* its whole account to the console: what it fetches, how (a delta or object by object) */
            setenv("FLATPAK_FANCY_OUTPUT", "0", 1);
            execl("/usr/bin/flatpak", "flatpak", "install", "--installation=os-data", "-y", "--noninteractive",
                  "--verbose", "--ostree-verbose", "flathub", app, (char *)NULL);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        printf("flatpak-install: %s exited %d after %.1f s\n", app, WIFEXITED(st) ? WEXITSTATUS(st) : -1,
               secs_since(&t0));
        if (dl > 0) {
            struct stat ws;
            long long had = stat("/var/lib/os-data/with.part", &ws) == 0 ? (long long)ws.st_size : -1;
            printf("flatpak-install: meanwhile the other download got %.1f MB\n", (double)had / 1e6);
            kill(dl, SIGKILL);
            waitpid(dl, NULL, 0);
        }
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }
    /* OS_MODE=journal-write, then journal-check:N (tests/journal_crash_win.py): files written and made sure of in
     * batches until the power is pulled mid-write; the next start replays the journal (e2fsck), the disk must
     * check clean, and every file said to be made sure of must be there, byte for byte. */
    if (mode && !strcmp(mode, "journal-write")) {
        setup_data_disk();
        mkdir(DATA_DIR "/jtest", 0755);
        static uint8_t blk[65536];
        for (int i = 0;; i++) {
            char p[96];
            snprintf(p, sizeof p, DATA_DIR "/jtest/f%05d", i);
            int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            size_t n = 4096 + (size_t)(i * 7919) % 60000; /* sizes of all kinds: small files, a few blocks */
            for (size_t k = 0; k < n; k++)
                blk[k] = (uint8_t)(i * 31 + k * 7);
            if (fd < 0 || write(fd, blk, n) != (ssize_t)n) {
                printf("journal-write: couldn't write %s\n", p);
                break;
            }
            if (i % 10 == 9) /* made sure of: this one, and the whole file system with it */
                fsync(fd);
            close(fd);
            if (i % 3 == 2) { /* folders and renames too: the file system's own blocks change */
                char q[96];
                snprintf(q, sizeof q, DATA_DIR "/jtest/r%05d", i);
                rename(p, q);
                rename(q, p);
            }
            if (i % 10 == 9) {
                printf("journal-write: synced %d\n", i + 1);
                fflush(stdout);
            }
        }
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strncmp(mode, "journal-check:", 14)) {
        int n = atoi(mode + 14);
        char out[4096];
        char *ck[] = {"/usr/sbin/e2fsck", "-p", DATA_DEV, NULL};
        int r = run(ck, out, sizeof out);
        printf("journal-check: e2fsck -p: %d\n%s\n", r, out);
        char *full[] = {"/usr/sbin/e2fsck", "-fn", DATA_DEV, NULL};
        int fr = run(full, out, sizeof out);
        printf("journal-check: e2fsck -fn (a full check, nothing changed): %d%s%s\n", fr, fr ? "\n" : "", fr ? out : "");
        setup_data_disk();
        static uint8_t want[65536], got[65536];
        int bad = 0, missing = 0;
        for (int i = 0; i < n; i++) {
            char p[96];
            snprintf(p, sizeof p, DATA_DIR "/jtest/f%05d", i);
            size_t len = 4096 + (size_t)(i * 7919) % 60000;
            for (size_t k = 0; k < len; k++)
                want[k] = (uint8_t)(i * 31 + k * 7);
            int fd = open(p, O_RDONLY);
            if (fd < 0) {
                missing++;
                continue;
            }
            ssize_t g = read(fd, got, sizeof got);
            close(fd);
            if (g != (ssize_t)len || memcmp(got, want, len))
                bad++;
        }
        printf("journal-check: %d files made sure of: %d missing, %d different; the disk %s\n", n, missing, bad,
               fr == 0 ? "checks clean" : "had problems");
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strcmp(mode, "wake-latency")) {
        wake_latency_mode();
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strcmp(mode, "disk-speed")) {
        disk_speed_mode();
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strncmp(mode, "net-speed:", 10)) { /* OS_MODE=net-speed:PORT: how fast a download from the PC is */
        /* net-speed:PORT (this PC), or net-speed:PORT:HOST:/PATH (a server on the internet) */
        char host[200] = "", path[400] = "";
        const char *c1 = strchr(mode + 10, ':');
        if (c1) {
            const char *c2 = strchr(c1 + 1, ':');
            snprintf(host, sizeof host, "%.*s", c2 ? (int)(c2 - c1 - 1) : 0, c1 + 1);
            snprintf(path, sizeof path, "%s", c2 ? c2 + 1 : "/");
        }
        /* PORT*N: N downloads at once (what two downloads together do to each other) */
        const char *star = strchr(mode + 10, '*');
        int many = star ? atoi(star + 1) : 1;
        fflush(stdout);
        for (int i = 1; i < many && i < 8; i++)
            if (fork() == 0) {
                net_speed_mode(atoi(mode + 10), host, path);
                fflush(stdout);
                _exit(0);
            }
        net_speed_mode(atoi(mode + 10), host, path);
        while (wait(NULL) > 0)
            ;
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strcmp(mode, "test-threads")) { /* only the thread tests: for chasing a scheduler bug quickly */
        for (int i = 0; i < 5; i++)
            thread_teardown_tests();
        printf(failures ? "USERLAND TESTS FAILED (%d)\n" : "USERLAND TESTS PASSED\n", failures);
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strcmp(mode, "piper")) {
        piper_mode();
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }
    if (mode && !strcmp(mode, "audio")) {
        audio_demo();
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 1;
    }

    setsid();
    boot_step("state", "active");
    restore_once(); /* their things first: the boot screen goes on in their design */
    account_setup(); /* and their account, named after them, its home /home/<name> */
    boot_step("state", "done");
    start_boot_screen();
    boot_step("apps", "active");
    setup_data_disk(); /* apps from Flathub live there */
    home_to_disk();    /* and their home */
    boot_step("apps", "done");
    boot_step("services", "active");
    start_buses();
    boot_step("services", "done");
    boot_step("desktop", "active");
    pid_t gui = spawn_gui(), sh = spawn_console_shell();
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0); /* also reaps orphans handed to init */
        if (pid == gui) {
            printf("init: the desktop exited (status %d), restarting it\n", status);
            sleep(1);
            gui = spawn_gui();
        } else if (pid == sh && sh > 0) {
            tcsetpgrp(0, getpid());
            sh = spawn_console_shell();
        } else if (pid == system_bus && pid > 0) {
            printf("init: the system bus exited (status %d), restarting it\n", status);
            sleep(1);
            system_bus = spawn_bus("system");
        } else if (pid == session_bus && pid > 0) {
            printf("init: the session bus exited (status %d), restarting it\n", status);
            sleep(1);
            session_bus = spawn_bus("session");
        } else if (pid == sound && pid > 0) {
            printf("init: the sound server exited (status %d), restarting it\n", status);
            sleep(2);
            sound = spawn_sound();
        }
    }
}
