/* The desktop's root errands, asked of init (priv.h). */
#define _GNU_SOURCE
#include "priv.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int fd = -2, next_id;
static pid_t owner; /* the process that may use it: the desktop's own, never one it forked */
static struct {
    int id;
    bool ok;
    char *text;
} got[16];

static int channel(void) {
    if (fd == -2) {
        const char *e = getenv("OS_PRIV_FD");
        fd = e ? atoi(e) : -1;
        if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) /* nothing it starts inherits it */
            fd = -1;
        unsetenv("OS_PRIV_FD");
        owner = getpid();
    }
    return getpid() == owner ? fd : -1;
}

bool priv_available(void) { return channel() >= 0; }

int priv_send(const char *errand) {
    int c = channel();
    if (c < 0)
        return -1;
    char m[1200]; /* (the child lock's words, hex, are the longest: init takes up to 1100) */
    int id = ++next_id;
    int n = snprintf(m, sizeof m, "%d %s", id, errand);
    if (n >= (int)sizeof m || send(c, m, (size_t)n, MSG_NOSIGNAL) < 0)
        return -1;
    return id;
}

/* every answer that has come, kept until asked for */
static void drain(void) {
    int c = channel();
    char m[4000];
    ssize_t n;
    while (c >= 0 && (n = recv(c, m, sizeof m - 1, MSG_DONTWAIT)) > 0) {
        m[n] = 0;
        int id = atoi(m);
        char *sp = strchr(m, ' '), *st = sp ? sp + 1 : m, *tx = strchr(st, ' ');
        for (int i = 0; i < 16; i++)
            if (!got[i].id) {
                got[i].id = id;
                got[i].ok = !strncmp(st, "ok", 2);
                got[i].text = strdup(tx ? tx + 1 : "");
                break;
            }
    }
}

bool priv_take(int id, bool *ok, char **text) {
    drain();
    for (int i = 0; i < 16; i++)
        if (got[i].id == id && id > 0) {
            *ok = got[i].ok;
            if (text)
                *text = got[i].text;
            else
                free(got[i].text);
            got[i].id = 0, got[i].text = NULL;
            return true;
        }
    return false;
}

bool priv_call(const char *errand, int timeout_ms) {
    int id = priv_send(errand);
    if (id < 0)
        return false;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        bool ok;
        if (priv_take(id, &ok, NULL))
            return ok;
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        long ms = (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms > timeout_ms)
            return false;
        struct pollfd p = {channel(), POLLIN, 0};
        poll(&p, 1, 50);
    }
}

bool priv_store_kept(void) {
    const char *s = getenv("OS_STORE");
    return s && !strcmp(s, "1");
}

int priv_store_restored(void) {
    const char *s = getenv("OS_STORE_KEPT");
    return s ? atoi(s) : -1;
}
