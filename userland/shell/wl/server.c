#define _GNU_SOURCE
#define WLPROTO_TABLES
#include "wlint.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* The Wayland wire protocol over a Unix socket: every message is the object
 * id (32 bits), then size << 16 | opcode, then 32-bit aligned arguments.
 * File descriptors travel beside the bytes as SCM_RIGHTS. Requests are
 * decoded from the signatures in wlproto.h and handed to the interface's
 * handler; events are encoded the same way. */

#define MAX_CLIENTS 32
#define MAX_GLOBALS 24
#define MAX_HANDLERS 48
#define INBUF 65536
#define OUTBUF 262144
#define MAX_FDS_IN 64

struct wl_client {
    int fd;
    bool dead;
    uint8_t in[INBUF];
    size_t inlen;
    int fds[MAX_FDS_IN];
    int nfds;
    uint8_t out[OUTBUF];
    size_t outlen;
    int outfds[16];
    int noutfds;
    wl_obj **objs;
    uint32_t nobjs;
};

static int listen_fd = -1;
static char sock_path[108];
static wl_client *clients[MAX_CLIENTS];
static uint32_t serial;

static struct {
    const struct wl_iface *iface;
    int version;
    wl_bind_fn bind;
} globals[MAX_GLOBALS];
static int nglobals;

static struct {
    const struct wl_iface *iface;
    wl_handler fn;
} handlers[MAX_HANDLERS];
static int nhandlers;

uint32_t wl_next_serial(void) { return ++serial; }
wl_client *wl_obj_client(wl_obj *o) { return o->client; }

void wl_register_handler(const struct wl_iface *iface, wl_handler h) {
    if (nhandlers < MAX_HANDLERS)
        handlers[nhandlers].iface = iface, handlers[nhandlers++].fn = h;
}

void wl_global(const struct wl_iface *iface, int version, wl_bind_fn bind) {
    if (nglobals < MAX_GLOBALS)
        globals[nglobals].iface = iface, globals[nglobals].version = version, globals[nglobals++].bind = bind;
}

/* ---- objects ---- */

wl_obj *wl_obj_get(wl_client *c, uint32_t id) { return id < c->nobjs ? c->objs[id] : NULL; }

wl_obj *wl_obj_new(wl_client *c, uint32_t id, const struct wl_iface *iface, int version, void *data) {
    if (id >= 0x1000000) /* server-side ids are not used by this compositor */
        return NULL;
    if (id >= c->nobjs) {
        uint32_t n = c->nobjs ? c->nobjs : 64;
        while (n <= id)
            n *= 2;
        c->objs = realloc(c->objs, sizeof *c->objs * n);
        memset(c->objs + c->nobjs, 0, sizeof *c->objs * (n - c->nobjs));
        c->nobjs = n;
    }
    wl_obj *o = calloc(1, sizeof *o);
    o->id = id, o->iface = iface, o->version = version, o->client = c, o->data = data;
    c->objs[id] = o;
    return o;
}

static void free_obj(wl_obj *o) {
    if (o->destroy)
        o->destroy(o);
    if (o->client && o->id < o->client->nobjs && o->client->objs[o->id] == o)
        o->client->objs[o->id] = NULL;
    free(o);
}

void wl_obj_destroy(wl_obj *o) {
    wl_client *c = o->client;
    uint32_t id = o->id;
    free_obj(o);
    wl_obj *display = wl_obj_get(c, 1);
    if (display && id != 1)
        wl_send(display, WL_DISPLAY_EV_DELETE_ID, id);
}

void wl_each(const struct wl_iface *iface, void (*fn)(wl_obj *o, void *ctx), void *ctx) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        wl_client *c = clients[i];
        if (!c || c->dead)
            continue;
        for (uint32_t id = 0; id < c->nobjs; id++)
            if (c->objs[id] && c->objs[id]->iface == iface)
                fn(c->objs[id], ctx);
    }
}

/* ---- sending ---- */

static void flush(wl_client *c) {
    while (c->outlen && !c->dead) {
        struct iovec iov = {c->out, c->outlen};
        char cbuf[CMSG_SPACE(sizeof(int) * 16)];
        struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
        if (c->noutfds) {
            mh.msg_control = cbuf;
            mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)c->noutfds);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)c->noutfds);
            memcpy(CMSG_DATA(cm), c->outfds, sizeof(int) * (size_t)c->noutfds);
        }
        ssize_t n = sendmsg(c->fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return;
            c->dead = true;
            return;
        }
        for (int i = 0; i < c->noutfds; i++)
            close(c->outfds[i]);
        c->noutfds = 0;
        memmove(c->out, c->out + n, c->outlen - (size_t)n);
        c->outlen -= (size_t)n;
    }
}

static void put32(wl_client *c, uint32_t v) {
    if (c->outlen + 4 <= OUTBUF) {
        memcpy(c->out + c->outlen, &v, 4);
        c->outlen += 4;
    }
}

static void put_bytes(wl_client *c, const void *d, uint32_t len, bool with_nul) {
    uint32_t total = len + (with_nul ? 1 : 0);
    put32(c, total);
    if (c->outlen + ((total + 3) & ~3u) > OUTBUF)
        return;
    memcpy(c->out + c->outlen, d, len);
    if (with_nul)
        c->out[c->outlen + len] = 0;
    uint32_t padded = (total + 3) & ~3u;
    memset(c->out + c->outlen + total, 0, padded - total);
    c->outlen += padded;
}

void wl_send(wl_obj *o, int opcode, ...) {
    wl_client *c = o->client;
    if (!c || c->dead || opcode >= o->iface->nev)
        return;
    const struct wl_msg *m = &o->iface->ev[opcode];
    if (m->since > o->version)
        return; /* the client bound an older version: it doesn't know this event */
    if (c->outlen > OUTBUF - 4096)
        flush(c);
    size_t start = c->outlen;
    put32(c, o->id);
    put32(c, 0); /* size and opcode, filled below */
    va_list ap;
    va_start(ap, opcode);
    for (const char *s = m->sig; *s; s++) {
        switch (*s) {
        case 'i': case 'u': case 'f': case 'n': put32(c, (uint32_t)va_arg(ap, int)); break;
        case 'o': {
            wl_obj *ob = va_arg(ap, wl_obj *);
            put32(c, ob ? ob->id : 0);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (str)
                put_bytes(c, str, (uint32_t)strlen(str), true);
            else
                put32(c, 0);
            break;
        }
        case 'a': {
            const void *d = va_arg(ap, const void *);
            uint32_t len = va_arg(ap, uint32_t);
            put_bytes(c, d, len, false);
            break;
        }
        case 'h': {
            int fd = va_arg(ap, int);
            if (c->noutfds < 16)
                c->outfds[c->noutfds++] = dup(fd);
            break;
        }
        default: break; /* '?' and version digits */
        }
    }
    va_end(ap);
    uint32_t size = (uint32_t)(c->outlen - start);
    uint32_t word = size << 16 | (uint32_t)opcode;
    memcpy(c->out + start + 4, &word, 4);
}

void wl_post_error(wl_obj *o, uint32_t code, const char *msg) {
    wl_client *c = o->client;
    wl_obj *display = wl_obj_get(c, 1);
    wl_host_log("wayland: error on %s@%u: %s", o->iface->name, o->id, msg);
    if (display)
        wl_send(display, WL_DISPLAY_EV_ERROR, o, code, msg);
    flush(c);
    c->dead = true;
}

/* ---- the display and registry, which every client starts with ---- */

static void registry_send_globals(wl_obj *reg) {
    for (int i = 0; i < nglobals; i++)
        wl_send(reg, WL_REGISTRY_EV_GLOBAL, i + 1, globals[i].iface->name, globals[i].version);
}

static void display_req(wl_obj *o, int op, union wl_arg *a) {
    if (op == WL_DISPLAY_SYNC) {
        wl_obj *cb = wl_obj_new(o->client, a[0].u, &wl_callback_iface, 1, NULL);
        if (cb) {
            wl_send(cb, WL_CALLBACK_EV_DONE, wl_next_serial());
            wl_obj_destroy(cb);
        }
    } else if (op == WL_DISPLAY_GET_REGISTRY) {
        wl_obj *reg = wl_obj_new(o->client, a[0].u, &wl_registry_iface, 1, NULL);
        if (reg)
            registry_send_globals(reg);
    }
}

static void registry_req(wl_obj *o, int op, union wl_arg *a) {
    if (op != WL_REGISTRY_BIND)
        return;
    uint32_t name = a[0].u;
    if (name < 1 || name > (uint32_t)nglobals) {
        wl_post_error(o, WL_DISPLAY_ERROR_INVALID_OBJECT, "no such global");
        return;
    }
    int v = (int)a[2].u;
    if (v > globals[name - 1].version)
        v = globals[name - 1].version;
    globals[name - 1].bind(o->client, a[3].u, v);
}

/* ---- receiving ---- */

static int take_fd(wl_client *c) {
    if (!c->nfds)
        return -1;
    int fd = c->fds[0];
    memmove(c->fds, c->fds + 1, sizeof(int) * (size_t)(--c->nfds));
    return fd;
}

static wl_handler handler_for(const struct wl_iface *iface) {
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i].iface == iface)
            return handlers[i].fn;
    return NULL;
}

/* Decodes and runs one message; returns its size, 0 if incomplete, -1 on a
 * protocol error. */
static int dispatch_one(wl_client *c, const uint8_t *p, size_t avail) {
    if (avail < 8)
        return 0;
    uint32_t id, word;
    memcpy(&id, p, 4);
    memcpy(&word, p + 4, 4);
    uint32_t size = word >> 16, op = word & 0xffff;
    if (size < 8 || (size & 3))
        return -1;
    if (avail < size)
        return 0;
    wl_obj *o = wl_obj_get(c, id);
    if (!o)
        return (int)size; /* a request to an object already gone: ignore, as libwayland does */
    if ((int)op >= o->iface->nreq) {
        wl_post_error(o, WL_DISPLAY_ERROR_INVALID_METHOD, "unknown request");
        return -1;
    }
    const struct wl_msg *m = &o->iface->req[op];
    union wl_arg args[WL_MAX_ARGS];
    int na = 0;
    const uint8_t *q = p + 8, *end = p + size;
    for (const char *s = m->sig; *s && na < WL_MAX_ARGS; s++) {
        if (*s == '?' || (*s >= '0' && *s <= '9'))
            continue;
        if (*s == 'h') {
            args[na++].h = take_fd(c);
            continue;
        }
        if (q + 4 > end)
            return -1;
        uint32_t v;
        memcpy(&v, q, 4);
        q += 4;
        if (*s == 's' || *s == 'a') {
            uint32_t padded = (v + 3) & ~3u;
            if (q + padded > end)
                return -1;
            if (*s == 's') {
                args[na++].s = v ? (const char *)q : NULL;
            } else {
                args[na].a.data = q;
                args[na++].a.len = v;
            }
            q += padded;
        } else {
            args[na++].u = v;
        }
    }
    if (o->iface == &wl_display_iface)
        display_req(o, (int)op, args);
    else if (o->iface == &wl_registry_iface)
        registry_req(o, (int)op, args);
    else {
        wl_handler h = handler_for(o->iface);
        if (h)
            h(o, (int)op, args);
    }
    return (int)size;
}

static void read_client(wl_client *c) {
    for (;;) {
        struct iovec iov = {c->in + c->inlen, INBUF - c->inlen};
        char cbuf[CMSG_SPACE(sizeof(int) * 28)];
        struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf};
        if (iov.iov_len == 0)
            break;
        ssize_t n = recvmsg(c->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            c->dead = true;
            return;
        }
        if (n < 0)
            break;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                int nf = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < nf; i++) {
                    int fd;
                    memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof fd);
                    if (c->nfds < MAX_FDS_IN)
                        c->fds[c->nfds++] = fd;
                    else
                        close(fd);
                }
            }
        c->inlen += (size_t)n;
        size_t off = 0;
        for (;;) {
            int used = dispatch_one(c, c->in + off, c->inlen - off);
            if (used < 0) {
                c->dead = true;
                return;
            }
            if (used == 0 || c->dead)
                break;
            off += (size_t)used;
        }
        memmove(c->in, c->in + off, c->inlen - off);
        c->inlen -= off;
    }
}

static void drop_client(int i) {
    wl_client *c = clients[i];
    wl_client_gone(c);
    for (uint32_t id = c->nobjs; id-- > 0;)
        if (c->objs[id])
            free_obj(c->objs[id]);
    free(c->objs);
    for (int k = 0; k < c->nfds; k++)
        close(c->fds[k]);
    for (int k = 0; k < c->noutfds; k++)
        close(c->outfds[k]);
    close(c->fd);
    free(c);
    clients[i] = NULL;
    wl_host_log("wayland: client %d gone", i);
}

bool wls_start(const char *runtime_dir) {
    mkdir(runtime_dir, 0700);
    snprintf(sock_path, sizeof sock_path, "%s/wayland-0", runtime_dir);
    unlink(sock_path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) {
        wl_host_log("wayland: no unix sockets (%s)", strerror(errno));
        return false;
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
    if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(listen_fd, 16) < 0) {
        wl_host_log("wayland: can't listen on %s (%s)", sock_path, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return false;
    }
    wl_comp_init();
    wl_host_log("wayland: listening on %s", sock_path);
    return true;
}

void wls_dispatch(void) {
    if (listen_fd < 0)
        return;
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
            break;
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (!clients[i]) {
                slot = i;
                break;
            }
        if (slot < 0) {
            close(fd);
            continue;
        }
        wl_client *c = calloc(1, sizeof *c);
        c->fd = fd;
        clients[slot] = c;
        wl_obj_new(c, 1, &wl_display_iface, 1, NULL);
        wl_host_log("wayland: client %d connected", slot);
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        wl_client *c = clients[i];
        if (!c)
            continue;
        if (!c->dead)
            read_client(c);
        if (!c->dead)
            flush(c);
        if (c->dead)
            drop_client(i);
    }
}

/* Events sent outside a dispatch (input, frame callbacks) go out now. */
void wl_flush_all(void) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] && !clients[i]->dead)
            flush(clients[i]);
}

int wls_fds(int *fds, int max) {
    int n = 0;
    if (listen_fd >= 0 && n < max)
        fds[n++] = listen_fd;
    for (int i = 0; i < MAX_CLIENTS && n < max; i++)
        if (clients[i])
            fds[n++] = clients[i]->fd;
    return n;
}
